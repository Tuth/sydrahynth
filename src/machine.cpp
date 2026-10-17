#include "machine.h"

#include "wave_rom.h"

#include <algorithm>
#include <numeric>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace hydra {

namespace {
// How far the panel MCU may lag behind the main CPU (its cycles, 72 MHz)
// between interactions: 50 us.
constexpr std::uint64_t kPanelLagCycles = 3600;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Adds the wall time of a scope to `bucket` when profiling is enabled.
class ScopedTime {
public:
    ScopedTime(bool enabled, std::uint64_t& bucket) : bucket_(enabled ? &bucket : nullptr), start_(enabled ? nowNs() : 0) {}
    ~ScopedTime() {
        if (bucket_)
            *bucket_ += nowNs() - start_;
    }

private:
    std::uint64_t* bucket_;
    std::uint64_t start_;
};
}  // namespace

// A device of the DSP side as the main CPU sees it: every access first waits for
// the DSP thread to reach the requested sample.
class Machine::DspSideDevice : public Device {
public:
    DspSideDevice(Machine& machine, Device& inner) : machine_(machine), inner_(inner) {}
    std::string_view name() const override { return inner_.name(); }
    std::uint32_t read(std::uint32_t offset, int size) override {
        machine_.waitDsp();
        return inner_.read(offset, size);
    }
    void write(std::uint32_t offset, std::uint32_t value, int size) override {
        machine_.waitDsp();
        inner_.write(offset, value, size);
    }

private:
    Machine& machine_;
    Device& inner_;
};

// The main CPU's SPI device: each byte is exchanged with the panel MCU's SPI2.
// Before exchanging, the panel catches up and, if it has not read the previous
// byte yet, gets up to 2000 more cycles to do so (it stops when it reads it).
class Machine::PanelLink : public SpiDevice {
    static constexpr std::uint64_t kLinkWaitCycles = 2000, kLinkWaitStep = 50;
    static constexpr std::uint32_t kPanelSpi2Dr = 0x4000380C;

public:
    explicit PanelLink(Machine& machine) : machine_(machine) {}
    void select() override {}
    void deselect() override {}
    std::uint8_t transfer(std::uint8_t out) override {
        machine_.syncPanel();
        stm32::Spi& spi = machine_.panel_->spi(1);
        Profile& profile = machine_.profile_;
        static const std::uint64_t waitCycles = std::getenv("HYDRA_LINK_WAIT") ? std::strtoull(std::getenv("HYDRA_LINK_WAIT"), nullptr, 10) : kLinkWaitCycles;
        // While the panel's reply streams out by DMA, the master's bytes only
        // clock it and the panel never reads them, so there is nothing to wait
        // for; the reference waited the full 2000 cycles per byte there, which
        // ran the panel ahead of the main CPU's clock. Without the wait the link
        // carries ~300 instead of ~180 frames per second and the main display
        // never falls below ~17 redraws per second (see docs/hardware.md).
        // HYDRA_LINK_WAIT_REPLY=1 restores the reference's behaviour.
        static const bool waitWhileReplying = std::getenv("HYDRA_LINK_WAIT_REPLY") != nullptr;
        const bool replying = !waitWhileReplying && machine_.panel_->dma1().feeding(kPanelSpi2Dr);
        if (spi.rxPending() && !replying) {
            // Let the panel run on in steps of 50 cycles until it has read the
            // previous byte, at most 2000 cycles (as the reference does). Its DR
            // read stops the run at once, then the step is completed.
            const ScopedTime t(profile.enabled, profile.linkWait);
            CortexM3& core = machine_.panel_->core();
            const std::uint64_t start = core.now();
            machine_.linkWaiting_ = true;
            machine_.panel_->run(waitCycles);
            machine_.linkWaiting_ = false;
            const std::uint64_t ran = core.now() - start;
            if (const std::uint64_t rest = (kLinkWaitStep - ran % kLinkWaitStep) % kLinkWaitStep; rest && ran < waitCycles)
                machine_.panel_->run(rest);
            profile.linkWaitRuns += profile.enabled;
            if (spi.rxPending() && machine_.linkOverrunSites_.size() < 4096) {  // diagnostic: where the panel was when a byte was lost
                const std::uint32_t key = core.ipsr() << 24 | (core.interruptsMasked() ? 1u << 23 : 0) | (core.regs()[15] & 0x7FFFFF);
                ++machine_.linkOverrunSites_[key];
            }
        }
        profile.linkBytes += profile.enabled;
        ++machine_.panelLinkBytes_;
        // Diagnostic: frames by type (header 80 81 82, length, type), and how
        // often the screen frame (type 7) changes its content.
        frameHash_ = frameHash_ * 1099511628211ull ^ out;
        ++frameBytes_;
        if (headerSeen_ == 3) {
            if (++afterHeader_ == 2)
                ++machine_.linkFrames_[frameType_ = out];
        }
        if (out == 0x80 || (out == 0x81 && headerSeen_ == 1) || (out == 0x82 && headerSeen_ == 2)) {
            headerSeen_ = out == 0x80 ? 1 : headerSeen_ + 1;
            if (headerSeen_ == 1) {  // diagnostic: from the panel's ready (PB10 low) to the frame's first byte
                const std::uint64_t now = machine_.cpu_->ticksExecuted();
                if (machine_.pb10LowAt_ && now >= machine_.pb10LowAt_) {
                    const std::uint64_t us = (now - machine_.pb10LowAt_) / (Machine::kCpuHz / 1'000'000);
                    ++machine_.sendAfterReady_[std::min<std::uint64_t>(us / 100, machine_.sendAfterReady_.size() - 1)];
                    machine_.pb10LowAt_ = 0;
                }
            }
            if (headerSeen_ == 3) {  // the previous frame ended before this header
                {
                    const std::uint64_t now = machine_.cpu_->ticksExecuted();
                    const std::uint64_t gap = (now - lastFrameStart_) / (Machine::kCpuHz / 2000);  // 0.5 ms units
                    ++machine_.frameGapHistogram_[std::min<std::uint64_t>(gap, machine_.frameGapHistogram_.size() - 1)];
                    lastFrameStart_ = now;
                }
                if (frameBytes_ > 3 && frameType_ < 16) {
                    const std::uint64_t now = machine_.cpu_->ticksExecuted();
                    auto& gaps = machine_.frameChangeGaps_[static_cast<std::size_t>(frameType_)];
                    auto& last = lastFrame_[static_cast<std::size_t>(frameType_)];
                    if (frameHash_ != last.hash) {
                        const std::uint64_t gapMs = (now - last.changedAt) / (Machine::kCpuHz / 1000);
                        ++gaps[gapMs < 15 ? 0 : gapMs < 30 ? 1 : gapMs < 60 ? 2 : gapMs < 120 ? 3 : 4];
                        last.changedAt = now;
                        last.hash = frameHash_;
                    } else {
                        ++gaps[5];  // unchanged
                    }
                }
                frameType_ = 0;
                frameHash_ = 14695981039346656037ull;
                frameBytes_ = 0;
                afterHeader_ = 0;
            }
        } else if (headerSeen_ != 3) {
            headerSeen_ = 0;
        }
        machine_.frameEndAt_ = machine_.cpu_->ticksExecuted();  // diagnostic: time of the latest byte
        const std::uint8_t in = spi.exchange(out);
        if (in != 0xFF)
            ++machine_.panelLinkReplies_;
        return in;
    }

private:
    Machine& machine_;
    int headerSeen_ = 0, afterHeader_ = 0, frameType_ = 0;
    std::uint64_t frameHash_ = 14695981039346656037ull, frameBytes_ = 0;
    std::uint64_t lastFrameStart_ = 0;
    struct LastFrame { std::uint64_t hash = 0, changedAt = 0; };
    std::array<LastFrame, 16> lastFrame_{};
};

Machine::Machine(const FirmwarePack& firmware, const Config& config) {
    const FirmwareSection* code = firmware.find("A5 code");
    if (!code)
        throw std::runtime_error("firmware has no 'A5 code' section");

    memory_.addRam("dram", 0x00000000, config.dramSize);
    memory_.addRam("sram", 0xA0000000, 1u << 20);
    memory_.load(0, code->data.data(), code->data.size());

    Memory::Region& nor = memory_.addRam("nor", 0x40000000, 128u << 20, 0xFF);
    nor.readOnly = true;
    std::copy(code->data.begin(), code->data.end(), nor.bytes.begin() + 0x20000);
    const std::vector<std::uint8_t> waveRom = buildWaveRom(firmware);
    std::copy(waveRom.begin(), waveRom.end(), nor.bytes.begin() + kWaveRomFlashOffset);
    for (const Config::FlashBlock& block : config.flash) {
        if (block.offset > nor.bytes.size() || block.bytes.size() > nor.bytes.size() - block.offset)
            throw std::invalid_argument("flash block outside the NOR flash");
        std::copy(block.bytes.begin(), block.bytes.end(), nor.bytes.begin() + block.offset);
    }
    norBytes_ = &nor;
    // The settings block also holds the index of the last patch (offset 0xDA), which
    // the firmware loads from the bank at boot without checking it. Erased (0xFFFF)
    // it points outside the bank and every patch parameter comes out 0 (no envelope
    // triggers, so silence); a factory device has a valid index there: A001.
    constexpr std::size_t kLastPatch = 0x7E000DA;
    if (nor.bytes[kLastPatch] == 0xFF && nor.bytes[kLastPatch + 1] == 0xFF)
        nor.bytes[kLastPatch] = nor.bytes[kLastPatch + 1] = 0x00;
    // Factory state of the system settings: the firmware loads a table of 11 x 8
    // menu values (int16) from the start of the settings sector and takes 0x00FF
    // as "unset, use the menu default". Erased flash (0xFFFF) would be taken as -1,
    // e.g. a master tune of 65535 Hz. Only applies while the table is blank.
    constexpr int kSettingsPages = 11, kSettingsEntries = 8;
    constexpr std::size_t kSettingsTable = 0x7E00000, kSettingsTableBytes = kSettingsPages * kSettingsEntries * 2;
    const auto table = nor.bytes.begin() + kSettingsTable;
    if (std::all_of(table, table + kSettingsTableBytes, [](std::uint8_t b) { return b == 0xFF; })) {
        for (std::size_t i = 0; i < kSettingsTableBytes; i += 2)
            table[static_cast<std::ptrdiff_t>(i) + 1] = 0x00;
        for (const auto& [where, value] : config.systemSettings) {
            const auto [page, entry] = where;
            if (page < 0 || page >= kSettingsPages || entry < 0 || entry >= kSettingsEntries)
                throw std::invalid_argument(std::format("no system setting {}.{}", page, entry));
            const auto at = table + 2 * (page * kSettingsEntries + entry);
            at[0] = static_cast<std::uint8_t>(value);
            at[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
        }
    }
    auto norFlash = std::make_unique<NorFlash>(nor);
    nor_ = norFlash.get();
    bus_.map(0x40000000, 128u << 20, nor_);
    devices_.push_back(std::move(norFlash));

    auto intc = std::make_unique<A5Intc>();
    auto timer = std::make_unique<A5Timer>(*intc);
    intc_ = intc.get();
    timer_ = timer.get();
    bus_.map(0xFF000000, 0x1000, intc_);
    bus_.map(0xF1000000, 0x1000, timer_);
    devices_.push_back(std::move(intc));
    devices_.push_back(std::move(timer));

    const FirmwareSection* panelCode = firmware.find("stm32 code");
    if (!panelCode)
        throw std::runtime_error("firmware has no 'stm32 code' section");
    panel_ = std::make_unique<PanelMcu>(*panelCode);
    panelLink_ = std::make_unique<PanelLink>(*this);
    panel_->spi(1).onDataRead = [this] {
        if (linkWaiting_)
            panel_->core().stopAfterAccess();
    };
    auto spi = std::make_unique<A5Spi>(*panelLink_);
    bus_.map(0xF3000000, 0x1000, spi.get());
    devices_.push_back(std::move(spi));
    for (int i = 0; i < 3; ++i) {
        auto uart = std::make_unique<A5Uart>(*intc_, i);
        uarts_[i] = uart.get();
        bus_.map(0xF5000000 + 0x200 * i, 0x200, uart.get());
        devices_.push_back(std::move(uart));
    }
    auto gpio = std::make_unique<A5Gpio>();
    gpio_ = gpio.get();
    gpio_->onOutput = [this](int port) {
        if (port == 0)
            updateI2c();
    };
    gpio_->onInputRead = [this](int port) {
        static const bool noSync = std::getenv("HYDRA_NO_GPIO_SYNC") != nullptr;  // experiment
        if (noSync)
            return;
        gpioInputReads_[static_cast<std::size_t>(port & 3)]++;
        if (port == 0)
            syncPanel();  // the panel's handshake lines
    };
    bus_.map(0xF4000000, 0x1000, gpio_);
    devices_.push_back(std::move(gpio));
    effects_ = std::make_unique<EffectsRack>(config.effectsThread || config.dspThread);
    effects_->onOutput = [this](std::int32_t left, std::int32_t right) {
        if (onAudio)
            onAudio(left, right);
    };
    for (int i = 0; i < 2; ++i)
        i2c_[i].attach(0x51, &effects_->port(i));
    auto adc = std::make_unique<A5Adc>(*intc_, [this] { return gpio_->output(1) >> 23 & 7; });
    adc_ = adc.get();
    bus_.map(0xF7000000, 0x1000, adc.get());
    devices_.push_back(std::move(adc));
    auto toneGenerator = std::make_unique<ToneGenerator>(nor.bytes);
    toneGenerator_ = toneGenerator.get();
    dspAsync_ = config.dspThread;
    const auto dspSide = [this](Device& device) -> Device* {
        if (!dspAsync_)
            return &device;
        auto wrapper = std::make_unique<DspSideDevice>(*this, device);
        Device* raw = wrapper.get();
        devices_.push_back(std::move(wrapper));
        return raw;
    };
    bus_.map(0xF6000000, 0x1000, dspSide(*toneGenerator));
    devices_.push_back(std::move(toneGenerator));
    auto dsp = std::make_unique<DspBlock>();
    dsp_ = dsp.get();
    auto dspControl = std::make_unique<DspControl>(*dsp_);
    dsp_->onSampleStart = [this](std::uint32_t* ports) {
        {
            const ScopedTime t(profile_.enabled, profile_.effects);
            processEffects(ports);
        }
        {
            const ScopedTime t(profile_.enabled, profile_.toneGenerator);
            toneGenerator_->render(ports);
        }
        if (onSample)
            onSample(ports);
    };
    dsp_->onBlock = [this](std::uint64_t sample) {
        dspIrq15_.fetch_add(1, std::memory_order_relaxed);
        if ((sample & 0x3F) == 0)
            dspIrq14_.fetch_add(1, std::memory_order_relaxed);
        if (!dspAsync_)
            raiseDspInterrupts();
    };
    // The DSP block's RAM and mailboxes are shared with the running cores, as on the
    // hardware: no waiting (word accesses race like the real buses do).
    bus_.map(0x90000000, 1u << 20, dsp_);
    bus_.map(0xF9000000, 0x1000, dspSide(*dspControl));
    devices_.push_back(std::move(dsp));
    devices_.push_back(std::move(dspControl));
    frontPanel_ = std::make_unique<FrontPanel>(*this);  // maps the keyscan block at 0xFA000000

    // Blocks the reference plugin deliberately leaves unmodelled.
    stub("sys80", 0x80000000, 0x10000);
    stub("blk82", 0x82000000, 0x10000);
    stub("blk85", 0x85000000, 0x10000);
    stub("blk86", 0x86000000, 0x80000);
    stub("blk8c", 0x8C000000, 0x10000);
    for (std::uint32_t block = 0xF0; block < 0xFF; ++block) {
        const bool modelled = (block & 0xFD) == 0xF1 || (block >= 0xF4 && block <= 0xFA && block != 0xF8);
        if (!modelled)
            stub(std::format("blk{:02x}", block), block << 24, 0x10000);
    }

    uarts_[1]->onTransmit = [this](std::uint8_t byte) {
        syncPanel();
        panel_->usart1().receive(byte);
    };
    panel_->usart1().onTransmit = [this](std::uint8_t byte) { uarts_[1]->receive(byte); };
    const auto handshake = [this](std::uint16_t before, std::uint16_t after) {
        if (cpu_ && ((before ^ after) & 0x400)) {  // diagnostic: PB10 edges (the panel ready / busy for a frame)
            const std::uint64_t now = cpu_->ticksExecuted();
            if (!(after & 0x400)) {
                pb10LowAt_ = now;
                if (frameEndAt_) {
                    const std::uint64_t us = (now - frameEndAt_) / (kCpuHz / 1'000'000);
                    ++readyAfterFrame_[std::min<std::uint64_t>(us / 100, readyAfterFrame_.size() - 1)];
                }
            }
        }
        static const bool trace = std::getenv("HYDRA_HANDSHAKE_TRACE") != nullptr;
        static int traced = 0;
        if (trace && ((before ^ after) & 0xC00) && traced++ < 12)
            std::fprintf(stderr, "handshake PB10/11 %u%u at %.3f s (panel %.3f s, pc %08x)\n", after >> 10 & 1, after >> 11 & 1,
                         cpu_ ? static_cast<double>(cpu_->ticksExecuted()) / kCpuHz : 0.0, static_cast<double>(panel_->core().now()) / 72e6,
                         panel_->core().regs()[15]);
        gpio_->setInput(0, 0x10, after >> 10 & 1);
        gpio_->setInput(0, 0x20, after >> 11 & 1);
    };
    panel_->gpio(1).onOutputChange = handshake;
    handshake(0, panel_->gpio(1).output());

    cpu_ = std::make_unique<CpuCore>(memory_, bus_);
    // ADC conversions take adcConversionTicks_. The firmware starts one and polls
    // the busy bit until the interrupt handler has fetched the result; while the
    // conversion runs, that is idle time, so the CPU sleeps as in WFI (the read is
    // retried after the interrupt). Once the interrupt is pending, polling ends
    // the slice so it is taken at once.
    adcConversionTicks_ = static_cast<std::uint64_t>(config.adcConversionMicros * (kCpuHz / 1'000'000));
    if (const char* micros = std::getenv("HYDRA_ADC_US"))  // experiments
        adcConversionTicks_ = static_cast<std::uint64_t>(std::atof(micros) * (kCpuHz / 1'000'000));
    adc_->onStart = [this] {
        ++adcConversions_; adcDoneAt_ = cpu_->ticksExecuted() + adcConversionTicks_; };
    adc_->onBusyPoll = [this] {
        if (cpu_->cpsr() & arm::psr::I)
            return;  // the interrupt cannot be taken: let the firmware spin
        if (adcDoneAt_) {
            cpu_->retryAccess();
            adcWaiting_ = true;
        } else if (intc_->irqPending()) {
            cpu_->retryAccess();
        }
    };
    cpu_->reset(0, 0x1D3);  // SVC mode, IRQ and FIQ masked, ARM state
}

void Machine::stub(const std::string& name, std::uint32_t base, std::uint32_t size) {
    auto& device = devices_.emplace_back(std::make_unique<StubDevice>(name, bus_.log(), base));
    bus_.map(base, size, device.get());
}

void Machine::syncPanel() {
    // ticks * kHz / kCpuHz without the 64-bit overflow that ticks * 72 MHz hits
    // after 640 s (which froze the panel, and with it the displays and
    // encoders, after 10 minutes and 40 seconds of running).
    constexpr std::uint64_t kGcd = std::gcd(PanelMcu::kHz, kCpuHz), kNum = PanelMcu::kHz / kGcd, kDen = kCpuHz / kGcd;
    const std::uint64_t ticks = cpu_->ticksExecuted();
    const std::uint64_t target = ticks / kDen * kNum + ticks % kDen * kNum / kDen;
    panelSyncDueAt_ = cpu_->ticksExecuted() + kPanelLagCycles * (kCpuHz / PanelMcu::kHz);
    if (target > panel_->core().now()) {
        const ScopedTime t(profile_.enabled, profile_.panel);
        panel_->run(target - panel_->core().now());
    }
}

void Machine::syncPanelLazily() {
    // Between its interactions with the main CPU (the SPI link, UART1 and the
    // handshake pins, which sync it themselves) the panel may lag a little, so
    // that it runs in fewer, longer stretches.
    static const bool eager = std::getenv("HYDRA_PANEL_EAGER") != nullptr;  // experiment
    if (eager || cpu_->ticksExecuted() >= panelSyncDueAt_)
        syncPanel();
}

void Machine::sendMidi(std::initializer_list<std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes)
        uarts_[0]->receive(byte);
}

void Machine::sendMidi(const std::vector<std::uint8_t>& bytes) {
    for (const std::uint8_t byte : bytes)
        uarts_[0]->receive(byte);
}

// Ports 72/73 receive the dry signal for the panel's oscilloscope.
void Machine::processEffects(std::uint32_t* ports) {
    effects_->push(ports);
    ports[72] = ports[64];
    ports[73] = ports[65];
}

void Machine::updateI2c() {
    constexpr std::uint32_t kSclA = 1u << 25, kSdaA = 1u << 23, kSclB = 1u << 27, kSdaB = 1u << 26;
    const bool sdaA = i2c_[0].update(gpio_->level(0, kSclA), gpio_->level(0, kSdaA));
    const bool sdaB = i2c_[1].update(gpio_->level(0, kSclB), gpio_->level(0, kSdaB));
    gpio_->setInput(0, kSdaA, sdaA);
    gpio_->setInput(0, kSdaB, sdaB);
}

namespace {

// Instructions between checks for timer events (which usually come first) and
// panel catch-ups; a polled ADC ends a slice early (see onBusyPoll).
constexpr std::uint64_t kMaxSlice = 40000;
constexpr std::uint64_t kTimerTicksPerSample = 1536;  // 67.7376 MHz / 44.1 kHz

// 67.7376 MHz / 400 MHz = 2646 / 15625; 64-bit arithmetic lasts 100 days of
// emulated time and divides by constants (a 128-bit division cost ~1% of the
// main thread).
constexpr std::uint64_t kTimerRatioNum = 2646, kTimerRatioDen = 15625;
static_assert(A5Timer::kHz * kTimerRatioDen == Machine::kCpuHz * kTimerRatioNum);

std::uint64_t timerTicksAt(std::uint64_t cpuTicks) { return cpuTicks * kTimerRatioNum / kTimerRatioDen; }

std::uint64_t cpuTicksFor(std::uint64_t timerTicks) {
    return (timerTicks * kTimerRatioDen + kTimerRatioNum - 1) / kTimerRatioNum;
}

}  // namespace

Machine::~Machine() {
    dspQuit_ = true;
    dspRequested_.fetch_add(1, std::memory_order_release);  // wake it up to see dspQuit_
    dspRequested_.notify_one();
    if (dspThread_.joinable())
        dspThread_.join();
}

void Machine::advanceDsp(std::uint64_t sample) {
    dsp_->profileSync = profile_.enabled;
    // Includes the per-sample effects and tone generator work, subtracted.
    const std::uint64_t effects = profile_.effects, toneGenerator = profile_.toneGenerator;
    const std::uint64_t start = profile_.enabled ? nowNs() : 0;
    dsp_->advanceToSample(sample);
    profile_.dspSync = dsp_->syncNs;
    if (profile_.enabled)
        profile_.dspCores += nowNs() - start - (profile_.effects - effects) - (profile_.toneGenerator - toneGenerator);
}

void Machine::requestDsp(std::uint64_t sample) {
    if (!dspAsync_) {
        advanceDsp(sample);
        return;
    }
    if (sample <= dspRequested_.load(std::memory_order_relaxed))
        return;
    if (!dspThread_.joinable())
        dspThread_ = std::thread([this] { dspThreadLoop(); });
    dspRequested_.store(sample, std::memory_order_release);
    dspRequested_.notify_one();  // a syscall only if the DSP thread sleeps
}

void Machine::waitDsp() {
    if (!dspAsync_)
        return;
    const std::uint64_t target = dspRequested_.load(std::memory_order_relaxed);
    if (dspReached_.load(std::memory_order_acquire) >= target)
        return;
    const ScopedTime t(profile_.enabled, profile_.dspWait);
    unsigned spins = 0;
    while (dspReached_.load(std::memory_order_acquire) < target) {
        if (spins < 4096) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
            ++spins;
        } else {
            std::this_thread::yield();
        }
    }
}

void Machine::dspThreadLoop() {
    std::uint64_t reached = 0;
    for (int spins = 0; !dspQuit_.load(std::memory_order_relaxed);) {
        const std::uint64_t target = dspRequested_.load(std::memory_order_acquire);
        if (target <= reached) {
            // Spin briefly (requests come every few microseconds while running),
            // then sleep until the next request, e.g. while the emulator pauses.
            if (spins < 4096) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
                ++spins;
            } else {
                dspRequested_.wait(target, std::memory_order_acquire);
            }
            continue;
        }
        spins = 0;
        advanceDsp(target);
        reached = target;
        dspReached_.store(reached, std::memory_order_release);
    }
}

void Machine::raiseDspInterrupts() {
    if (dspIrq15_.load(std::memory_order_relaxed) == 0 && dspIrq14_.load(std::memory_order_relaxed) == 0)
        return;
    for (std::uint32_t n = dspIrq15_.exchange(0, std::memory_order_relaxed); n > 0; --n)
        intc_->raise(15);
    for (std::uint32_t n = dspIrq14_.exchange(0, std::memory_order_relaxed); n > 0; --n)
        intc_->raise(14);
}

Machine::Status Machine::run(std::uint64_t ticks, bool settle) {
    const ScopedTime total(profile_.enabled, profile_.loop);
    bus_.profiling = panel_->bus().profiling = cpu_->profileExits = profile_.enabled;
    const std::uint64_t end = cpu_->ticksExecuted() + ticks;
    while (cpu_->ticksExecuted() < end) {
        const std::uint64_t now = cpu_->ticksExecuted();
        const std::uint64_t timerNow = timerTicksAt(now);
        timer_->advanceTo(timerNow);
        requestDsp(timerNow / kTimerTicksPerSample);
        raiseDspInterrupts();
        if (now >= nextFrontPanelTick_) {
            frontPanel_->advance(now / (kCpuHz / 1'000'000));
            nextFrontPanelTick_ = now + kCpuHz / 4000;  // every 250 us
        }

        if (adcDoneAt_ && now >= adcDoneAt_) {
            adcDoneAt_ = 0;
            adc_->complete();
        }
        if (intc_->irqPending()) {
            idle_ = false;  // a pending interrupt wakes WFI even while masked
            cpu_->takeIrq();
        }

        std::uint64_t slice = std::min(end - now, kMaxSlice);
        if (const auto next = timer_->ticksToNextEvent())
            slice = std::min(slice, cpuTicksFor(*next) + 1);
        if (adcDoneAt_)
            slice = std::min(slice, adcDoneAt_ - now);
        slice = std::max<std::uint64_t>(slice, 1);

        if (profile_.enabled) {
            ++profile_.slices;
            profile_.idleSlices += idle_;
        }
        if (idle_) {
            cpu_->AddTicks(slice);
            idleTicks_ += slice;
            syncPanelLazily();
            continue;
        }

        if (cpu_->skipDelayLoop(slice) != 0) {
            if (profile_.enabled)
                ++profile_.delaySkips;
            syncPanelLazily();
            continue;
        }

        CpuCore::Stop stop;
        {
            const ScopedTime t(profile_.enabled, profile_.cpu);
            stop = cpu_->run(slice);
        }
        if (recordPcSamples)
            ++pcSamples_[cpu_->regs()[15]];
        syncPanelLazily();
        if (stop == CpuCore::Stop::Fatal) {
            waitDsp();
            return Status::Fatal;
        }
        if (stop == CpuCore::Stop::WaitForInterrupt || std::exchange(adcWaiting_, false))
            idle_ = true;
    }
    if (settle) {
        waitDsp();
        raiseDspInterrupts();
    }
    return Status::Running;
}

}  // namespace hydra
