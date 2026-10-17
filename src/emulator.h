#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "machine.h"
#include "patch_bank.h"

namespace hydra {

// Runs a Machine on its own thread, ahead of real time, for a plugin or app:
// the stereo output (44.1 kHz, float) goes into a ring buffer that the audio
// thread drains with readAudio(), and MIDI from the audio thread is queued
// with sendMidi() and fed to the instrument's MIDI input between run chunks.
// Neither call blocks or allocates. Until the firmware has booted, the machine
// runs at full speed and its output is discarded.
class Emulator {
public:
    static constexpr double kSampleRate = 44100.0;

    struct Config {
        std::filesystem::path firmwarePath;           // the Hydrasynth Desktop .dat firmware pack
        std::vector<Machine::Config::FlashBlock> flash;  // e.g. from a saved state
        std::map<std::pair<int, int>, std::int16_t> systemSettings;
        int dspThreads = 4;
        bool profile = false;  // Machine::Profile (diagnostic)
        // Samples the emulator keeps buffered ahead of the audio thread (the
        // latency); see setTargetFill().
        std::size_t targetFill = 512;
        // Play a quiet note (MIDI channel 1) while booting, before the output
        // goes live, to warm up the JIT (see advanceBoot()).
        bool warmUp = true;
    };

    enum class State { Stopped, Booting, Running, Failed };

    Emulator();
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    // Loads the firmware, builds the machine and starts the thread. On failure
    // the state is Failed and status() says why.
    void start(const Config& config);
    void stop();

    State state() const { return state_.load(std::memory_order_acquire); }
    std::string status() const;

    // Audio thread: queues MIDI bytes for the instrument's MIDI input.
    void sendMidi(const std::uint8_t* bytes, std::size_t count);
    // Audio thread: takes up to `frames` stereo samples; returns how many were
    // available (the rest of the buffers is zeroed).
    std::size_t readAudio(float* left, float* right, std::size_t frames);
    std::size_t bufferedFrames() const;
    // Changes the buffered samples while running (any thread). It must exceed
    // the largest pull plus the emulator's jitter (a few milliseconds).
    void setTargetFill(std::size_t frames);
    std::size_t targetFill() const { return targetFill_.load(std::memory_order_relaxed); }
    std::uint64_t framesProduced() const { return audioWritten_.load(std::memory_order_relaxed); }

    // The non-blank 128 KiB sectors of the flash's user area (settings, banks),
    // as of the next pause between run chunks. Waits up to `timeoutMs`.
    std::vector<Machine::Config::FlashBlock> captureFlash(int timeoutMs = 2000);

    // Sends patches to a bank (0 = A ... 4 = E) over the instrument's MIDI input
    // as a librarian would, each into its own slot, and saves them. Runs on the
    // emulator thread, unpaced and muted (about 3 x real time; a full bank takes
    // about a minute and a half). Returns false if the emulator is not running or
    // an upload is already under way.
    bool uploadPatches(std::vector<Patch> patches, int bank);
    bool uploading() const { return uploading_.load(std::memory_order_acquire); }
    // Patches done and in total (approximate while uploading).
    std::pair<std::size_t, std::size_t> uploadProgress() const;

    // Front panel input, from any thread; applied between run chunks.
    struct PanelInput {
        enum class Kind { Button, DataEncoder, Encoder, Knob, ArpSwitch, Pad } kind;
        int index = 0;      // button id, encoder, knob, switch, pad row * 8 + column
        int amount = 0;     // button: 1 down / 0 up; encoders: detents; switch position
        float value = 0;    // knob position, pad pressure (0..1)
        int delayMs = 0;    // button: apply this much later (emulated time)
    };
    void panelInput(const PanelInput& input);
    // The panel's lights as of the last capture (every 10 ms of emulated time,
    // HYDRA_PANEL_CAPTURE_HZ overrides the rate).
    // Copies them and returns true if they changed since `generation`, which it
    // updates.
    bool readPanel(FrontPanel::Lights& out, std::uint64_t& generation) const;

    // The instrument's patches, from the firmware's bank cache in RAM (filled from
    // flash at boot and kept up to date by uploads and saves): the names of all
    // 640 slots (5 banks x 128, empty for a blank slot), and the current program
    // with its name as the instrument shows it. Captured every 100 ms of emulated
    // time; readPatches() copies and returns true if anything changed since
    // `generation`, which it updates.
    struct Patches {
        static constexpr int kBanks = 5, kSlots = 128;
        std::array<std::string, kBanks * kSlots> names;
        int current = -1;  // bank * 128 + slot, or -1 while no patch is loaded
        std::string currentName;
    };
    bool readPatches(Patches& out, std::uint64_t& generation) const;
    // Selects a stored patch (bank 0 = A ... 4 = E, slot 0-127) over the MIDI input
    // like a host would: bank select LSB, then a program change (channel 1). The
    // instrument must have program change reception on (the plugin's default).
    void selectPatch(int bank, int slot);
    // MIDI from other threads than the audio thread (the editor), delivered
    // between run chunks.
    void queueMidi(std::vector<std::uint8_t> bytes);

    // Diagnostic (tools): the machine itself, read racily while the thread runs.
    Machine* machine() { return machine_.get(); }
    // Diagnostic: the machine's profile counters (Config::profile), read racily.
    Machine::Profile profile() const { return machine_ ? machine_->profile() : Machine::Profile{}; }
    std::uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    // Diagnostic: wall time of the slowest run chunk so far (microseconds), and
    // how many chunks took longer than 2 ms. resetChunkStats() clears both.
    std::uint64_t slowestChunkMicros() const { return slowestChunk_.load(std::memory_order_relaxed); }
    std::uint64_t slowChunks() const { return slowChunks_.load(std::memory_order_relaxed); }
    void resetChunkStats() { slowestChunk_ = 0; slowChunks_ = 0; }

private:
    void threadMain();
    void pushAudio(std::int32_t left, std::int32_t right);
    void deliverMidi();
    void stepUpload();
    void applyPanelInput();
    void advanceBoot();
    void capturePanel();
    void capturePatches();

    std::unique_ptr<FirmwarePack> firmware_;
    std::unique_ptr<Machine> machine_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<State> state_{State::Stopped};
    mutable std::mutex statusMutex_;
    std::string status_;
    std::atomic<std::size_t> targetFill_{512};
    enum class WarmUp { Waiting, Settling, NoteOn, Release, Done };
    static constexpr std::uint8_t kWarmUpNote = 60;
    bool warmUpEnabled_ = true;
    WarmUp warmUp_ = WarmUp::Waiting;
    std::uint64_t warmUpAt_ = 0;

    // Audio ring (single producer: the effects thread; single consumer: audio thread).
    static constexpr std::size_t kAudioFrames = 1 << 15;
    std::vector<float> audio_;  // interleaved stereo
    std::atomic<std::uint64_t> audioWritten_{0}, audioRead_{0};
    std::atomic<std::uint64_t> underruns_{0}, overruns_{0};
    std::atomic<std::uint64_t> slowestChunk_{0}, slowChunks_{0};

    // MIDI byte ring (single producer: audio thread; single consumer: emulator thread).
    static constexpr std::size_t kMidiBytes = 1 << 16;
    std::vector<std::uint8_t> midi_;
    std::atomic<std::uint64_t> midiWritten_{0}, midiRead_{0};

    // Patch upload: the SysEx messages, sent one by one by the emulator thread.
    std::mutex uploadMutex_;
    std::vector<std::vector<std::uint8_t>> uploadMessages_;
    std::size_t uploadNext_ = 0, uploadPatches_ = 0;
    std::uint64_t uploadNextAt_ = 0;  // CPU ticks
    std::atomic<std::size_t> uploadDone_{0}, uploadTotal_{0}, uploadPatchCount_{0};
    std::atomic<bool> uploading_{false};

    // Front panel: queued inputs and the latest lights.
    std::mutex panelInputMutex_;
    std::vector<PanelInput> panelInput_;
    mutable std::mutex lightsMutex_;
    std::unique_ptr<FrontPanel::Lights> lights_, lightsScratch_;
    std::uint64_t lightsGeneration_ = 0, nextLightsTick_ = 0, panelCaptureHz_ = 100;

    // Patch list capture, and MIDI queued by other threads.
    mutable std::mutex patchesMutex_;
    std::unique_ptr<Patches> patches_, patchesScratch_;
    std::uint64_t patchesGeneration_ = 0, nextPatchesTick_ = 0;
    std::mutex controlMidiMutex_;
    std::vector<std::uint8_t> controlMidi_;

    // Diagnostic (HYDRA_OLED_TRACE): per run chunk, whether the main display
    // was written, run-length encoded (count > 0 active chunks, < 0 idle).
    bool oledTrace_ = false;
    bool settleDsp_ = true;
    bool bootTrace_ = false;  // HYDRA_BOOT_TRACE
    bool panelTalking_ = false;  // the panel's start screen has ended
    std::array<bool, 256> seenFrameTypes_{};  // HYDRA_DSP_SETTLE: wait for the DSP side at every run chunk (experiment)
    std::uint64_t oledGeneration_ = 0;
    std::vector<int> oledRuns_;

    // Flash capture handshake.
    std::atomic<bool> flashRequested_{false};
    std::mutex flashMutex_;
    std::vector<Machine::Config::FlashBlock> flashCapture_;
    std::atomic<bool> flashReady_{false};
};

}  // namespace hydra
