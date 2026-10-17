#include "emulator.h"
#include <string_view>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>

#include "a5pack.h"
#include "sysex.h"

namespace hydra {

namespace {
// The flash's user area (settings, banks); the rest is code and the wave ROM,
// which are rebuilt from the firmware.
constexpr std::uint32_t kUserAreaStart = 0x7000000, kUserAreaEnd = 0x8000000;
constexpr std::uint32_t kSector = 0x20000;
// The boot (with the warm-up) lasts at least this long, in CPU ticks.
// Diagnostic (HYDRA_BOOT_TRACE): emulated and wall time of the boot phases.
void traceBoot(const char* phase, std::uint64_t ticks) {
    static const bool enabled = std::getenv("HYDRA_BOOT_TRACE") != nullptr;
    static const auto start = std::chrono::steady_clock::now();
    if (enabled)
        std::fprintf(stderr, "boot: %-28s emulated %6.2f s, wall %5.2f s\n", phase, static_cast<double>(ticks) / Machine::kCpuHz,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}
// Pauses after each patch upload message, in emulated milliseconds.
constexpr std::uint64_t kUploadChunkGapMs = 10, kUploadControlGapMs = 60;

// Firmware 2.2.0's bank cache in DRAM (all 640 slots as 2786-byte 2.x bodies,
// filled from flash at boot) and its current patch (the same layout; zeros
// while no patch is loaded, as after selecting a blank slot). A body starts with
// the version (u16), the slot index bank * 128 + slot (u16), the category and
// the 16-character name. See docs/hardware.md, "Patches and SysEx".
constexpr std::uint32_t kBankCache = 0x6C50E2, kCurrentPatch = 0xB498E2, kPatchBytes = 2786;
constexpr std::uint32_t kPatchName = 5, kPatchNameLength = 16;

// How often the panel's lights are captured for the UI (HYDRA_PANEL_CAPTURE_HZ
// overrides it, for measuring how fast the firmware redraws).
std::uint64_t panelCaptureHz() {
    if (const char* hz = std::getenv("HYDRA_PANEL_CAPTURE_HZ"))
        if (const long v = std::atol(hz); v >= 10 && v <= 1000)
            return static_cast<std::uint64_t>(v);
    return 100;
}

std::string patchName(const std::uint8_t* body) {
    const std::uint8_t* name = body + kPatchName;
    if (name[0] < 0x20 || name[0] > 0x7E)  // blank (0xFF or zeros), as the firmware checks it
        return {};
    std::string s;
    for (std::uint32_t i = 0; i < kPatchNameLength && name[i] >= 0x20 && name[i] <= 0x7E; ++i)
        s.push_back(static_cast<char>(name[i]));
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}
// Emulated time per run chunk: 1 ms.
constexpr std::uint64_t kChunkTicks = Machine::kCpuHz / 1000;
// 24-bit output range to float.
constexpr float kOutputScale = 1.0f / 8388608.0f;
}  // namespace

Emulator::Emulator() : audio_(2 * kAudioFrames), midi_(kMidiBytes) {}

Emulator::~Emulator() { stop(); }

void Emulator::start(const Config& config) {
    panelCaptureHz_ = panelCaptureHz();
    oledTrace_ = std::getenv("HYDRA_OLED_TRACE") != nullptr;
    bootTrace_ = std::getenv("HYDRA_BOOT_TRACE") != nullptr;
    // Bound CPU/DSP skew to the existing 1 ms run chunk. Without the barrier,
    // a fast CPU can queue DSP time far ahead and firmware MIDI/voice setup
    // becomes dependent on host thread scheduling. Both still run in parallel
    // inside the chunk; this adds no audio buffering.
    const char* settle = std::getenv("HYDRA_DSP_SETTLE");
    settleDsp_ = !settle || std::string_view(settle) != "0";
    stop();
    setTargetFill(config.targetFill);
    audioWritten_ = audioRead_ = 0;
    midiWritten_ = midiRead_ = 0;
    underruns_ = overruns_ = 0;
    try {
        firmware_ = std::make_unique<FirmwarePack>(FirmwarePack::load(config.firmwarePath));
        Machine::Config machineConfig;
        machineConfig.dspThread = true;
        machineConfig.systemSettings = config.systemSettings;
        machineConfig.flash = config.flash;
        machine_ = std::make_unique<Machine>(*firmware_, machineConfig);
        machine_->dspBlock().setThreads(config.dspThreads);
        machine_->profile().enabled = config.profile;
        machine_->onAudio = [this](std::int32_t left, std::int32_t right) { pushAudio(left, right); };
    } catch (const std::exception& e) {
        machine_.reset();
        firmware_.reset();
        {
            const std::lock_guard lock(statusMutex_);
            status_ = std::format("cannot start: {}", e.what());
        }
        state_ = State::Failed;
        return;
    }
    {
        const std::lock_guard lock(statusMutex_);
        status_ = "booting the firmware";
    }
    quit_ = false;
    warmUpEnabled_ = config.warmUp;
    warmUp_ = WarmUp::Waiting;
    state_ = State::Booting;
    traceBoot("machine built", 0);
    thread_ = std::thread([this] { threadMain(); });
}

void Emulator::stop() {
    if (oledTrace_ && !oledRuns_.empty()) {
        std::string line = "main display write runs (chunks of 1 ms; +active/-idle), from the end:";
        const std::size_t n = std::min<std::size_t>(oledRuns_.size(), 160);
        for (std::size_t k = oledRuns_.size() - n; k < oledRuns_.size(); ++k)
            line += (oledRuns_[k] > 0 ? " +" : " ") + std::to_string(oledRuns_[k]);
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    quit_ = true;
    if (thread_.joinable())
        thread_.join();
    machine_.reset();  // stops the machine's DSP and effects threads
    firmware_.reset();
    if (state_ != State::Failed)
        state_ = State::Stopped;
}

std::string Emulator::status() const {
    const std::lock_guard lock(statusMutex_);
    return status_;
}

void Emulator::sendMidi(const std::uint8_t* bytes, std::size_t count) {
    const std::uint64_t written = midiWritten_.load(std::memory_order_relaxed);
    if (written + count - midiRead_.load(std::memory_order_acquire) > kMidiBytes)
        return;  // full: drop the message rather than block the audio thread
    for (std::size_t i = 0; i < count; ++i)
        midi_[(written + i) % kMidiBytes] = bytes[i];
    midiWritten_.store(written + count, std::memory_order_release);
}

void Emulator::deliverMidi() {
    const std::uint64_t written = midiWritten_.load(std::memory_order_acquire);
    std::uint64_t read = midiRead_.load(std::memory_order_relaxed);
    if (read == written)
        return;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(written - read);
    for (; read < written; ++read)
        bytes.push_back(midi_[read % kMidiBytes]);
    midiRead_.store(read, std::memory_order_release);
    machine_->sendMidi(bytes);
}

bool Emulator::uploadPatches(std::vector<Patch> patches, int bank) {
    if (state() != State::Running || patches.empty() || bank < 0 || bank > 7)
        return false;
    std::vector<std::vector<std::uint8_t>> messages;
    for (const Patch& patch : patches) {
        if (patch.slot < 0 || patch.slot > 127 || (patch.body.size() != 1762 && patch.body.size() != 2786))
            continue;
        for (auto& message : sysex::patchUpload(patch.body, bank, patch.slot, true))
            messages.push_back(std::move(message));
    }
    if (messages.empty())
        return false;
    const std::lock_guard lock(uploadMutex_);
    if (uploading_.load())
        return false;
    uploadPatches_ = patches.size();
    uploadPatchCount_ = patches.size();
    uploadTotal_ = messages.size();
    uploadDone_ = 0;
    uploadMessages_ = std::move(messages);
    uploadNext_ = 0;
    uploadNextAt_ = 0;
    uploading_.store(true, std::memory_order_release);
    return true;
}

std::pair<std::size_t, std::size_t> Emulator::uploadProgress() const {
    const std::size_t total = uploadTotal_.load(), patches = uploadPatchCount_.load();
    return {total ? uploadDone_.load() * patches / total : 0, patches};
}

void Emulator::stepUpload() {
    if (!uploading_.load(std::memory_order_acquire))
        return;
    const std::uint64_t now = machine_->cpu().ticksExecuted();
    if (now < uploadNextAt_)
        return;
    const std::lock_guard lock(uploadMutex_);
    if (uploadNext_ >= uploadMessages_.size()) {
        uploadMessages_.clear();
        uploading_.store(false, std::memory_order_release);
        return;
    }
    const std::vector<std::uint8_t>& message = uploadMessages_[uploadNext_++];
    machine_->sendMidi(message);
    uploadDone_.store(uploadNext_, std::memory_order_relaxed);
    // MIDI speed (320 us a byte), then time for the firmware to answer; the
    // messages that start, save and end a patch take longer.
    const bool chunk = message.size() > 20;
    const std::uint64_t byteTicks = Machine::kCpuHz * 320 / 1'000'000, gapMs = chunk ? kUploadChunkGapMs : kUploadControlGapMs;
    uploadNextAt_ = now + message.size() * byteTicks + Machine::kCpuHz * gapMs / 1000;
}

void Emulator::pushAudio(std::int32_t left, std::int32_t right) {
    if (state_.load(std::memory_order_relaxed) != State::Running || uploading_.load(std::memory_order_relaxed))
        return;  // booting or uploading: discarded, so that it runs at full speed
    const std::uint64_t written = audioWritten_.load(std::memory_order_relaxed);
    if (written - audioRead_.load(std::memory_order_acquire) >= kAudioFrames) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t at = 2 * (written % kAudioFrames);
    audio_[at] = static_cast<float>(left) * kOutputScale;
    audio_[at + 1] = static_cast<float>(right) * kOutputScale;
    audioWritten_.store(written + 1, std::memory_order_release);
}

std::size_t Emulator::bufferedFrames() const {
    return static_cast<std::size_t>(audioWritten_.load(std::memory_order_acquire) - audioRead_.load(std::memory_order_relaxed));
}

std::size_t Emulator::readAudio(float* left, float* right, std::size_t frames) {
    const std::uint64_t read = audioRead_.load(std::memory_order_relaxed);
    const std::size_t available =
        static_cast<std::size_t>(audioWritten_.load(std::memory_order_acquire) - read);
    const std::size_t n = std::min(frames, available);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = 2 * ((read + i) % kAudioFrames);
        left[i] = audio_[at];
        right[i] = audio_[at + 1];
    }
    std::fill(left + n, left + frames, 0.0f);
    std::fill(right + n, right + frames, 0.0f);
    audioRead_.store(read + n, std::memory_order_release);
    if (n < frames && state_.load(std::memory_order_relaxed) == State::Running && !uploading_.load(std::memory_order_relaxed))
        underruns_.fetch_add(1, std::memory_order_relaxed);
    return n;
}

std::vector<Machine::Config::FlashBlock> Emulator::captureFlash(int timeoutMs) {
    if (!thread_.joinable())
        return {};
    flashReady_ = false;
    flashRequested_ = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flashReady_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline || !thread_.joinable())
            return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::lock_guard lock(flashMutex_);
    return std::move(flashCapture_);
}

void Emulator::setTargetFill(std::size_t frames) {
    targetFill_.store(std::clamp<std::size_t>(frames, 128, kAudioFrames / 2), std::memory_order_relaxed);
}

void Emulator::panelInput(const PanelInput& input) {
    const std::lock_guard lock(panelInputMutex_);
    panelInput_.push_back(input);
}

bool Emulator::readPanel(FrontPanel::Lights& out, std::uint64_t& generation) const {
    const std::lock_guard lock(lightsMutex_);
    if (!lights_ || generation == lightsGeneration_)
        return false;
    out = *lights_;
    generation = lightsGeneration_;
    return true;
}

bool Emulator::readPatches(Patches& out, std::uint64_t& generation) const {
    const std::lock_guard lock(patchesMutex_);
    if (!patches_ || generation == patchesGeneration_)
        return false;
    out = *patches_;
    generation = patchesGeneration_;
    return true;
}

void Emulator::selectPatch(int bank, int slot) {
    bank = std::clamp(bank, 0, Patches::kBanks - 1);
    slot = std::clamp(slot, 0, Patches::kSlots - 1);
    queueMidi({0xB0, 0x20, static_cast<std::uint8_t>(bank), 0xC0, static_cast<std::uint8_t>(slot)});
}

void Emulator::queueMidi(std::vector<std::uint8_t> bytes) {
    const std::lock_guard lock(controlMidiMutex_);
    controlMidi_.insert(controlMidi_.end(), bytes.begin(), bytes.end());
}

void Emulator::applyPanelInput() {
    std::vector<PanelInput> inputs;
    {
        const std::lock_guard lock(panelInputMutex_);
        inputs.swap(panelInput_);
    }
    FrontPanel& panel = machine_->frontPanel();
    for (const PanelInput& in : inputs) {
        switch (in.kind) {
        case PanelInput::Kind::Button:
            panel.setButton(in.index, in.amount != 0, static_cast<std::uint64_t>(std::max(0, in.delayMs)) * 1000);
            break;
        case PanelInput::Kind::DataEncoder: panel.turnDataEncoder(in.amount); break;
        case PanelInput::Kind::Encoder: panel.turnEncoder(in.index, in.amount); break;
        case PanelInput::Kind::Knob: panel.setKnob(static_cast<FrontPanel::Knob>(in.index), in.value); break;
        case PanelInput::Kind::ArpSwitch: panel.setArpSwitch(static_cast<FrontPanel::ArpSwitch>(in.index), in.amount); break;
        case PanelInput::Kind::Pad:
            panel.setPad(in.index / FrontPanel::kPadColumns, in.index % FrontPanel::kPadColumns, in.value);
            break;
        }
    }
}

void Emulator::capturePanel() {
    const std::uint64_t now = machine_->cpu().ticksExecuted();
    if (now < nextLightsTick_)
        return;
    nextLightsTick_ = now + Machine::kCpuHz / panelCaptureHz_;
    if (!lightsScratch_)
        lightsScratch_ = std::make_unique<FrontPanel::Lights>();
    machine_->frontPanel().readLights(*lightsScratch_);
    const std::lock_guard lock(lightsMutex_);
    if (lights_ && std::memcmp(lights_.get(), lightsScratch_.get(), sizeof(FrontPanel::Lights)) == 0)
        return;
    std::swap(lights_, lightsScratch_);
    ++lightsGeneration_;
}

void Emulator::capturePatches() {
    const std::uint64_t now = machine_->cpu().ticksExecuted();
    if (now < nextPatchesTick_)
        return;
    nextPatchesTick_ = now + Machine::kCpuHz / 10;
    const std::uint8_t* cache = machine_->memory().hostPtr(kBankCache, kPatchBytes * Patches::kBanks * Patches::kSlots);
    const std::uint8_t* current = machine_->memory().hostPtr(kCurrentPatch, kPatchBytes);
    if (!cache || !current)
        return;
    if (!patchesScratch_)
        patchesScratch_ = std::make_unique<Patches>();
    Patches& p = *patchesScratch_;
    for (std::size_t i = 0; i < p.names.size(); ++i)
        p.names[i] = patchName(cache + i * kPatchBytes);
    p.currentName = patchName(current);
    const int index = current[2] | current[3] << 8;
    p.current = p.currentName.empty() || index >= Patches::kBanks * Patches::kSlots ? -1 : index;
    const std::lock_guard lock(patchesMutex_);
    if (patches_ && patches_->current == p.current && patches_->currentName == p.currentName && patches_->names == p.names)
        return;
    std::swap(patches_, patchesScratch_);
    ++patchesGeneration_;
}


void Emulator::advanceBoot() {
    // Booted once both effects DSPs run (the firmware starts them last). Then a
    // warm-up: a quiet note, so that the code paths of playing and releasing a
    // voice are translated by the JIT now rather than on the first real note
    // (which would otherwise take long enough to underrun a short buffer).
    const std::uint64_t now = machine_->cpu().ticksExecuted();
    switch (warmUp_) {
    case WarmUp::Waiting:
        if (!machine_->effects().dsp(0).running() || !machine_->effects().dsp(1).running())
            return;
        if (!warmUpEnabled_) {
            warmUp_ = WarmUp::Done;
            break;
        }
        traceBoot("effects DSPs running", now);
        warmUp_ = WarmUp::Settling;  // the firmware takes a moment before it plays notes
        warmUpAt_ = now + Machine::kCpuHz;
        return;
    case WarmUp::Settling:
        if (now < warmUpAt_)
            return;
        machine_->sendMidi({0x90, kWarmUpNote, 1});
        warmUp_ = WarmUp::NoteOn;
        warmUpAt_ = now + Machine::kCpuHz * 12 / 10;  // held long enough for the envelopes to move on
        return;
    case WarmUp::NoteOn:
        if (now < warmUpAt_)
            return;
        machine_->sendMidi({0x80, kWarmUpNote, 0});
        traceBoot("warm-up note released", now);
        warmUp_ = WarmUp::Release;
        // The release and its effects tail.
        warmUpAt_ = now + Machine::kCpuHz * 3 / 2;
        return;
    case WarmUp::Release:
        // Also until the panel's start screen has ended (6.8 s of emulated time;
        // until then the panel does not talk to the main CPU) and the first
        // screens have been drawn, so that the code of that step is translated
        // before the output goes live.
        if (now < warmUpAt_)
            return;
        if (!panelTalking_) {
            if (machine_->panelLinkBytes() == 0)
                return;
            panelTalking_ = true;
            warmUpAt_ = now + Machine::kCpuHz * 3 / 10;
            traceBoot("panel start screen done", now);
            return;
        }
        warmUp_ = WarmUp::Done;
        traceBoot("live", now);
        break;
    case WarmUp::Done:
        break;
    }
    {
        const std::lock_guard lock(statusMutex_);
        status_ = "running";
    }
    state_ = State::Running;
}

void Emulator::threadMain() {
    while (!quit_.load(std::memory_order_relaxed)) {
        if (flashRequested_.exchange(false)) {
            std::vector<Machine::Config::FlashBlock> blocks;
            const std::vector<std::uint8_t>& flash = machine_->flashBytes();
            for (std::uint32_t at = kUserAreaStart; at < kUserAreaEnd; at += kSector) {
                const auto begin = flash.begin() + at, end = begin + kSector;
                if (std::all_of(begin, end, [](std::uint8_t b) { return b == 0xFF; }))
                    continue;
                blocks.push_back({at, std::vector<std::uint8_t>(begin, end)});
            }
            {
                const std::lock_guard lock(flashMutex_);
                flashCapture_ = std::move(blocks);
            }
            flashReady_.store(true, std::memory_order_release);
        }

        if (state_.load(std::memory_order_relaxed) == State::Running && !uploading_.load(std::memory_order_relaxed) &&
            bufferedFrames() >= targetFill_.load(std::memory_order_relaxed)) {
            machine_->setHostIdle(true);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        {
            std::vector<std::uint8_t> control;
            {
                const std::lock_guard lock(controlMidiMutex_);
                control.swap(controlMidi_);
            }
            if (!control.empty())
                machine_->sendMidi(control);
        }
        machine_->setHostIdle(false);
        deliverMidi();
        stepUpload();
        applyPanelInput();
        capturePanel();
        capturePatches();
        const auto chunkStart = std::chrono::steady_clock::now();
        const Machine::Status status = machine_->run(kChunkTicks, settleDsp_);
        if (oledTrace_ && oledRuns_.size() < 100000) {
            const std::uint64_t g = machine_->panel().displays().display(0).generation();
            const bool active = g != oledGeneration_;
            oledGeneration_ = g;
            if (!oledRuns_.empty() && (oledRuns_.back() > 0) == active)
                oledRuns_.back() += active ? 1 : -1;
            else
                oledRuns_.push_back(active ? 1 : -1);
        }
        const auto chunkMicros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - chunkStart).count());
        if (chunkMicros > slowestChunk_.load(std::memory_order_relaxed))
            slowestChunk_.store(chunkMicros, std::memory_order_relaxed);
        if (chunkMicros > 2000)
            slowChunks_.fetch_add(1, std::memory_order_relaxed);
        if (bootTrace_) {
            const auto& frames = machine_->linkFrames();
            for (const auto& [type, count] : frames)
                if (!seenFrameTypes_[type]) {
                    seenFrameTypes_[type] = true;
                    traceBoot(std::format("first link frame type {:02x}", type).c_str(), machine_->cpu().ticksExecuted());
                }
            if (chunkMicros > 2000)
                traceBoot(std::format("slow chunk {} us", chunkMicros).c_str(), machine_->cpu().ticksExecuted());
        }
        if (status == Machine::Status::Fatal) {
            {
                const std::lock_guard lock(statusMutex_);
                status_ = std::format("the firmware stopped: {}", machine_->cpu().fatalMessage());
            }
            state_ = State::Failed;
            return;
        }
        if (state_.load(std::memory_order_relaxed) == State::Booting)
            advanceBoot();
    }
}

}  // namespace hydra
