#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "a5_peripherals.h"
#include "a5pack.h"
#include "bus.h"
#include "cpu.h"
#include "dsp_block.h"
#include "fx_dsp.h"
#include "effects_rack.h"
#include "i2c.h"
#include "nor_flash.h"
#include "front_panel.h"
#include "panel_mcu.h"
#include "spi.h"
#include "tone_generator.h"

namespace hydra {

// The Hydrasynth main board around the A5 core. Address map (from the
// reference plugin):
//   0x00000000  DRAM (64 MiB), "A5 code" loaded at 0
//   0x40000000  NOR flash window (128 MiB); "A5 code" copy at +0x20000,
//               system settings at +0x7E00000, user flash at +0x7000000
//   0x90000000  DSP block (1 MiB)
//   0xA0000000  SRAM (1 MiB)
//   0xF1000000  timer      0xF3000000  SPI (16 MiB W25Q128 SPI flash behind it)
//   0xF4000000  GPIO/I2C   0xF5000000  UART0..2 (0x200 apart)
//   0xF6000000  tone generator           0xF7000000  ADC
//   0xF9000000  DSP control              0xFA000000  key scanner
//   0xFF000000  interrupt controller
//
// GPIO port 0 carries two bit-banged I2C buses, each with an effects DSP at 0x51:
//   bus A: SCL bit 25, SDA bit 23      bus B: SCL bit 27, SDA bit 26
// Modelled: timer, interrupt controller, UARTs, GPIO/I2C, NOR flash, tone
// generator registers, the main DSP block (8 cores) and the effects DSP
// memories. Everything else is a logging stub.
//
// The DSP block runs one sample per 1536 timer ticks (44.1 kHz); every 32
// samples it raises CPU interrupt line 15, and line 14 every 64.
//
// The panel MCU (see panel_mcu.h) is attached as on the board: the main SPI
// controller talks to its SPI2 (slave), UART1 is cross-connected with its
// USART1, and its GPIOB output register bits 10/11 (PB10/PB11) drive main
// GPIO port 0 inputs bits 4/5.
//
// Clocks: the CPU retires one instruction per tick at 400 MHz; the timer runs at
// 67.7376 MHz (timer = cpu * 0.169344), which is 1536 timer ticks per 44.1 kHz sample.
class Machine {
public:
    struct Config {
        std::uint32_t dramSize = 64u << 20;
        // Run the effects chain on its own thread (see EffectsRack).
        bool effectsThread = false;
        // Run the DSP side (DSP block, tone generator, effects feed) on its own
        // thread, concurrently with the main CPU: each slice requests the samples
        // due, and a CPU access to DSP control or the tone generator first waits
        // for them; the DSP block's RAM and mailboxes are accessed concurrently,
        // as on the hardware. The DSP block's interrupts are raised at the next
        // slice boundary. Implies effectsThread.
        bool dspThread = false;
        // Initial system settings for a blank settings table, as (page, entry) ->
        // value of the System Setup menu (firmware 2.2.0: page 0 entry 1 Tuning in Hz,
        // page 5 entry 1 Param RX, page 5 entry 7 PgmChgRX; see docs/hardware.md).
        // Entries not listed start at the firmware's menu defaults.
        std::map<std::pair<int, int>, std::int16_t> systemSettings;
        // Flash contents to restore before boot (e.g. saved settings and banks), as
        // byte offsets into the 128 MiB NOR flash.
        struct FlashBlock {
            std::uint32_t offset = 0;
            std::vector<std::uint8_t> bytes;
        };
        std::vector<FlashBlock> flash;
        // How long the main CPU's ADC takes per conversion. The firmware's main
        // loop waits for each one (sleeping, in the emulation), so this paces the
        // loop: pads and panel link per second, and the work they bring.
        double adcConversionMicros = 20.0;
    };

    Machine(const FirmwarePack& firmware, const Config& config);
    ~Machine();

    static constexpr std::uint64_t kCpuHz = 400'000'000;

    enum class Status { Running, Fatal };
    // Advances emulated time by `ticks` CPU cycles, including time the CPU spends
    // idle in WFI. Stops early on a fatal error. With `settle` the DSP side has
    // caught up when it returns (tests, captures); a host that only streams the
    // output can leave it running (its interrupts arrive at the next call).
    Status run(std::uint64_t ticks, bool settle = true);

    CpuCore& cpu() { return *cpu_; }
    MmioBus& bus() { return bus_; }
    A5Intc& intc() { return *intc_; }
    EffectsRack& effects() { return *effects_; }
    // Waits until the effects chain has caught up (threaded effects).
    void flushEffects() { effects_->flush(); }
    const std::array<I2cBus, 2>& i2cBuses() const { return i2c_; }
    const NorFlash& norFlash() const { return *nor_; }
    // The NOR flash contents (128 MiB); read it only while the machine is not running.
    const std::vector<std::uint8_t>& flashBytes() const { return norBytes_->bytes; }
    PanelMcu& panel() { return *panel_; }
    std::uint64_t panelLinkBytes() const { return panelLinkBytes_; }
    std::uint64_t panelLinkReplies() const { return panelLinkReplies_; }
    // Diagnostic: where the panel was (IPSR << 24 | PRIMASK << 23 | PC & 0x7FFFFF)
    // whenever a link byte overwrote one it had not read yet, with counts.
    const std::map<std::uint32_t, std::uint64_t>& linkOverrunSites() const { return linkOverrunSites_; }
    // Diagnostic: frames sent to the panel, by frame type.
    const std::map<std::uint8_t, std::uint64_t>& linkFrames() const { return linkFrames_; }
    // Diagnostic: per frame type (0-15), frames whose content changed, by the gap
    // since the previous change (<15, <30, <60, <120, >=120 ms), and [5] unchanged ones.
    // Diagnostic: time from one link frame's start to the next, in 0.5 ms buckets (last = 30 ms or more).
    const std::array<std::uint64_t, 61>& frameGapHistogram() const { return frameGapHistogram_; }
    // Diagnostic: main ADC conversions started (the firmware's main loop runs one scan of 24 per iteration).
    std::uint64_t adcConversions() const { return adcConversions_; }
    // Diagnostic: main GPIO input register reads per port (port 0 carries the panel's handshake).
    const std::array<std::uint64_t, 4>& gpioInputReads() const { return gpioInputReads_; }
    using FrameGaps = std::array<std::uint64_t, 6>;
    const std::array<FrameGaps, 16>& frameChangeGaps() const { return frameChangeGaps_; }
    // Runs the panel MCU up to the main CPU's current time (72 MHz = 400 MHz * 9 / 50).
    void syncPanel();
    // syncPanel() once the panel lags more than kPanelLagCycles.
    void syncPanelLazily();
    ToneGenerator& toneGenerator() { return *toneGenerator_; }
    std::uint64_t idleTicks() const { return idleTicks_; }
    A5Uart& uart(int index) { return *uarts_[index]; }
    // Queues MIDI bytes into the MIDI input (UART0).
    void sendMidi(std::initializer_list<std::uint8_t> bytes);
    void sendMidi(const std::vector<std::uint8_t>& bytes);
    // The host has enough buffered audio; let idle workers sleep promptly.
    void setHostIdle(bool idle) {
        hostIdle_.store(idle, std::memory_order_relaxed);
        dsp_->setHostIdle(idle); effects_->setHostIdle(idle);
    }
    DspBlock& dspBlock() { return *dsp_; }
    Memory& memory() { return memory_; }
    A5Gpio& gpio() { return *gpio_; }
    A5Adc& adc() { return *adc_; }
    // The front panel's controls and lights (see front_panel.h).
    FrontPanel& frontPanel() { return *frontPanel_; }

    // Audio: at the start of every 44.1 kHz sample the effects chain gets the DSP
    // block's previous outputs (ports 64-71), the tone generator renders this
    // sample's buses into ports 0-47, and onSample sees the port buffer. onAudio
    // receives the stereo output (24-bit range in 32-bit words) in sample order;
    // with Config::effectsThread it is called on the effects thread, behind
    // onSample (flushEffects() catches up).
    std::function<void(const std::uint32_t* ports)> onSample;
    std::function<void(std::int32_t left, std::int32_t right)> onAudio;
    void setEffectsEnabled(bool enabled) { effects_->enabled = enabled; }

    // Wall-clock nanoseconds per component while profiling is on (diagnostic).
    struct Profile {
        bool enabled = false;
        std::uint64_t cpu = 0, dspCores = 0, effects = 0, toneGenerator = 0, panel = 0;
        std::uint64_t dspWait = 0;  // main thread waiting for the DSP thread
        std::uint64_t dspSync = 0;  // DSP thread waiting for the DSP worker threads
        std::uint64_t slices = 0, idleSlices = 0;  // main loop iterations (counted while enabled)
        std::uint64_t delaySkips = 0;               // slices that fast-forwarded a delay loop
        std::uint64_t loop = 0;                     // wall ns in run() altogether
        std::uint64_t linkBytes = 0, linkWaitRuns = 0, linkWait = 0;  // panel link: bytes, waits for the panel, their ns
    };
    Profile& profile() { return profile_; }
    const Profile& profile() const { return profile_; }
    // Instruction-slice end PCs, as a cheap hotspot profile (with recordPcSamples).
    bool recordPcSamples = false;
    const std::map<std::uint32_t, std::uint64_t>& pcSamples() const { return pcSamples_; }

private:
    class PanelLink;
    class DspSideDevice;
    void stub(const std::string& name, std::uint32_t base, std::uint32_t size);
    void updateI2c();
    void processEffects(std::uint32_t* ports);
    // Brings the DSP side up to `sample` (asynchronously with Config::dspThread).
    void requestDsp(std::uint64_t sample);
    // Waits until the DSP side has reached the last requested sample.
    void waitDsp();
    void advanceDsp(std::uint64_t sample);
    void dspThreadLoop();
    void raiseDspInterrupts();

    Memory memory_;
    MmioBus bus_;
    std::vector<std::unique_ptr<Device>> devices_;
    A5Intc* intc_ = nullptr;
    A5Timer* timer_ = nullptr;
    A5Gpio* gpio_ = nullptr;
    A5Adc* adc_ = nullptr;
    std::array<A5Uart*, 3> uarts_{};
    DspBlock* dsp_ = nullptr;
    NorFlash* nor_ = nullptr;
    Memory::Region* norBytes_ = nullptr;
    std::unique_ptr<PanelMcu> panel_;
    std::unique_ptr<FrontPanel> frontPanel_;
    std::uint64_t nextFrontPanelTick_ = 0;
    bool linkWaiting_ = false;  // the panel link is waiting for the panel to read a byte
    std::uint64_t panelSyncDueAt_ = 0;  // CPU ticks at which the panel is due for a lazy sync
    std::unique_ptr<SpiDevice> panelLink_;
    std::uint64_t panelLinkBytes_ = 0, panelLinkReplies_ = 0;  // replies: bytes other than 0xFF
    std::map<std::uint32_t, std::uint64_t> linkOverrunSites_;
    std::map<std::uint8_t, std::uint64_t> linkFrames_;
    std::array<FrameGaps, 16> frameChangeGaps_{};
    std::array<std::uint64_t, 61> frameGapHistogram_{};
    std::array<std::uint64_t, 4> gpioInputReads_{};
public:
    // Diagnostic, 100 us buckets: from the panel's ready (PB10 low) to the main
    // CPU's next frame, and from the last link byte to the panel's next ready.
    std::array<std::uint64_t, 40> sendAfterReady_{}, readyAfterFrame_{};
    std::uint64_t pb10LowAt_ = 0, frameEndAt_ = 0;
private:
    std::uint64_t adcConversions_ = 0;
    ToneGenerator* toneGenerator_ = nullptr;
    std::array<I2cBus, 2> i2c_;
    std::unique_ptr<EffectsRack> effects_;
    Profile profile_;
    bool dspAsync_ = false;
    std::thread dspThread_;
    std::atomic<std::uint64_t> dspRequested_{0}, dspReached_{0};
    std::atomic<bool> hostIdle_{false};
    std::atomic<bool> dspQuit_{false};
    std::atomic<std::uint32_t> dspIrq15_{0}, dspIrq14_{0};  // raised by the DSP side, delivered by the CPU side
    std::unique_ptr<CpuCore> cpu_;
    bool idle_ = false;  // in WFI until an interrupt is pending
    std::uint64_t adcConversionTicks_ = 2000;
    std::uint64_t adcDoneAt_ = 0;  // CPU ticks when the running ADC conversion completes (0: none)
    bool adcWaiting_ = false;      // the CPU polled the busy ADC: idle until an interrupt
    std::uint64_t idleTicks_ = 0;
    std::map<std::uint32_t, std::uint64_t> pcSamples_;
};

}  // namespace hydra
