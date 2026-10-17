#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "bus.h"
#include "dsp_core.h"

namespace hydra {

// The main DSP block at 0x90000000: 8 DSP cores, shared RAM and mailboxes.
//
// CPU view (byte offsets):
//   0x00000-0x1FFFF  program memory, 0x4000 bytes per core (core = offset >> 14)
//   0x20000-0x7EFFF  shared RAM
//   0x7F000-0x7FFFF  mailboxes, 32 words per core counting down from the top:
//                    core = (~offset >> 7) & 0x1F, word = (offset >> 2) & 0x1F.
//                    Slots for "cores" 8-31 (0x7F000-0x7FBFF) are plain RAM.
// Each core also has 512 words of private data memory.
//
// DSP view, "io" space (17-bit word addresses), as seen by core n:
//   0x00000-0x00FFF  own program memory     0x01000-0x07FFF  other cores' program memory
//   0x08000-0x0FEFF  shared RAM [0x0000-]   0x0FF00-0x0FFFF  mailboxes
//   0x10000-0x1FBFF  shared RAM [0x8000-]   0x1FC00-0x1FFFF  mailboxes (like the CPU's)
// "ext" space (16 bit): 0x0000-0x0FFF own program memory, 0x1000-0xFBFF shared
// RAM [0x9000-], 0xFC00-0xFFFF mailboxes. "port" space: the per-sample I/O
// buffer that the tone generator fills and the effects rack reads.
//
// Per 44.1 kHz sample: onSampleStart(ports) runs (effects push, tone generator
// render), every running core gets the sample interrupt (line 2) and 3072
// instructions; every 32 samples onBlock(sample) runs.
//
// Host command protocol (firmware 2.2.0), per core: mailbox 0 command, 1 target
// address, 2 word count (max 16); data staged at the start of the core's program
// memory. 0x55AA is the core's "ready" reply at start and after attention.
class DspBlock : public Device, public dsp::Host {
public:
    static constexpr int kCores = 8;
    static constexpr std::uint32_t kInstructionsPerSample = 0xC00;
    static constexpr std::uint32_t kPortWords = 0x10000;

    DspBlock();
    ~DspBlock() override;

    // Device (CPU access)
    std::string_view name() const override { return "dsp"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    // dsp::Host
    std::uint32_t ioRead(int core, std::uint32_t address) override;
    void ioWrite(int core, std::uint32_t address, std::uint32_t value) override;
    std::uint32_t extRead(int core, std::uint32_t address) override;
    void extWrite(int core, std::uint32_t address, std::uint32_t value) override;
    std::uint32_t portRead(int, std::uint32_t port) override { return ports_[port % kPortWords]; }
    void portWrite(int core, std::uint32_t port, std::uint32_t value) override {
        ports_[port % kPortWords] = value;
        if (diagnostics) ++portWrites_[core][port];
    }
    bool diagnostics = false;  // opt-in counters, not needed for audio
    std::array<std::map<std::uint32_t, std::uint64_t>, 8> portWrites_;  // diagnostic: per core, port -> writes
    std::map<std::pair<int, std::uint32_t>, std::uint64_t> commands_;    // diagnostic: (core, command) from the CPU
    std::map<std::uint32_t, std::uint64_t> cpuWrites_;                   // diagnostic: CPU writes per 256-byte block

    // Control (DSP control registers)
    std::uint32_t runMask() const { return runMask_; }
    void setRunMask(std::uint32_t mask);
    void requestAttention(int core) { cores_[core].dsp->raiseInterrupt(1); }

    // Runs all samples before `sample`.
    void advanceToSample(std::uint64_t sample);
    // Runs the cores on `threads` threads (1-8; 1 = no worker threads). Core i runs
    // on thread i % threads; the calling thread is thread 0. All cores of a sample
    // finish before the next sample starts, as on the hardware, where they run
    // concurrently; with more than one thread the order of their accesses to
    // shared RAM within a sample is not fixed.
    void setThreads(int threads);
    // Diagnostic: wall ns the calling thread spent waiting for the workers (with profileSync).
    bool profileSync = false;
    std::uint64_t syncNs = 0;
    int threads() const { return threads_; }
    std::uint64_t sample() const { return sample_; }

    std::function<void(std::uint32_t* ports)> onSampleStart;
    std::function<void(std::uint64_t sample)> onBlock;

    std::uint32_t* ports() { return ports_.data(); }
    dsp::DspCore& core(int index) { return *cores_[index].dsp; }
    const std::array<std::uint32_t, 512>& privateData(int index) const { return cores_[index].data; }
    std::uint64_t instructionsExecuted() const { return executed_; }

private:
    struct Core {
        std::array<std::uint32_t, dsp::DspCore::kProgramWords> program{};
        std::array<std::uint32_t, 512> data{};
        std::array<std::uint32_t, 32> mailbox{};
        std::unique_ptr<dsp::DspCore> dsp;
    };

    // Word in the 17-bit io space as seen by `core`.
    std::uint32_t* ioWord(int core, std::uint32_t address);
    std::uint32_t* extWord(int core, std::uint32_t address);
    std::uint32_t* mailboxWord(std::uint32_t wordAddress);  // 0x1FC00-0x1FFFF
    std::uint32_t* cpuWord(std::uint32_t offset);

    // Runs the cores assigned to `thread` for one sample; returns instructions.
    std::uint64_t runCores(int thread);
    void workerLoop(int thread, std::uint64_t seen);
    void stopWorkers();

    std::array<Core, kCores> cores_;
    int threads_ = 1;
    std::vector<std::thread> workers_;
    alignas(64) std::atomic<std::uint64_t> generation_{0};  // bumped to release the workers for a sample
    alignas(64) std::atomic<int> finished_{0};              // workers done with the current sample
    struct alignas(64) WorkerResult { std::uint64_t executed = 0; };
    std::array<WorkerResult, kCores> workerResults_{};
    std::atomic<bool> quit_{false};
    std::vector<std::uint32_t> shared_;
    std::vector<std::uint32_t> spareMailboxes_;
    std::vector<std::uint32_t> ports_;
    std::uint32_t runMask_ = 0;
    std::uint64_t sample_ = 0;
    std::uint64_t executed_ = 0;
};

// DSP control registers at 0xF9000000:
//   +0  write: bits 0-7 per-core run enable (a core is reset when enabled),
//       bits 8-15 raise the attention interrupt (line 1) on that core;
//       read: the run mask
//   +4, +8, +0xC  plain registers (firmware 2.2.0 writes 0x88888888 to +4 and +8)
class DspControl : public Device {
public:
    explicit DspControl(DspBlock& block) : block_(block) {}
    std::string_view name() const override { return "dspctl"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

private:
    DspBlock& block_;
    std::array<std::uint32_t, 4> regs_{};
};

}  // namespace hydra
