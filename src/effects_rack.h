#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fx_dsp.h"
#include "i2c.h"

namespace hydra {

// The two effects DSPs and their audio chain (board wiring as in the reference):
// per sample, DSP block ports 64/65 carry the dry stereo voice mix and 70/71 and
// 66/67 control words for effects DSPs A and B. A gets the dry signal scaled down
// by 4 in its ports 2/3 and its control words in 4/5; B gets A's ports 0/1 and
// its control words; B's ports 6/7 are the output. While either DSP is stopped,
// or with `enabled` false, the output is the dry signal.
//
// With `threaded`, the chain runs on its own thread, behind the caller: push()
// queues a sample, I2C accesses from the main CPU are queued with the sample
// count at that moment and applied before the next sample (only ever on the
// effects thread), and an I2C read waits until the thread has caught up. push()
// and the I2C accesses may come from different threads. onOutput sees every sample in order, on the
// effects thread when threaded.
class EffectsRack {
public:
    explicit EffectsRack(bool threaded = false);
    ~EffectsRack();
    EffectsRack(const EffectsRack&) = delete;
    EffectsRack& operator=(const EffectsRack&) = delete;

    // The I2C slave for DSP `index` (0 = A on bus A, 1 = B on bus B).
    I2cSlave& port(int index);
    const FxDsp& dsp(int index) const { return *dsps_[index]; }

    // One sample: `ports` is the DSP block's port buffer.
    void push(const std::uint32_t* ports);
    // Waits until every pushed sample has been processed and every queued I2C
    // access applied.
    void flush();
    void setHostIdle(bool idle) { hostIdle_.store(idle, std::memory_order_relaxed); }
    // Diagnostic: flushes so far, wall ns spent waiting in them, and samples
    // the worker was behind when they started (summed).
    std::uint64_t flushes = 0, flushNs = 0, flushBacklog = 0;

    std::function<void(std::int32_t left, std::int32_t right)> onOutput;
    std::atomic<bool> enabled{true};

private:
    std::atomic<bool> hostIdle_{false};
    struct Sample {
        std::uint32_t dryLeft, dryRight, a4, a5, b4, b5;
    };
    enum class OpKind : std::uint8_t { Start, Write, Stop };
    struct Op {
        std::uint64_t sample;  // apply before processing this sample
        int dsp;
        OpKind kind;
        std::uint8_t value;
    };
    class Port;

    void process(const Sample& sample);
    void queueOp(int dsp, OpKind kind, std::uint8_t value);
    void applyOps(std::uint64_t upTo);  // ops stamped <= upTo
    void apply(const Op& op);
    void workerLoop();

    std::array<std::unique_ptr<FxDsp>, 2> dsps_;
    std::array<std::unique_ptr<Port>, 2> ports_;
    const bool threaded_;

    // Single-producer, single-consumer sample ring (threaded mode).
    static constexpr std::size_t kRingSize = 8192;
    std::vector<Sample> ring_;
    std::atomic<std::uint64_t> pushed_{0}, processed_{0};

    std::mutex opsMutex_;
    std::deque<Op> ops_;
    std::atomic<std::size_t> pendingOps_{0};
    std::atomic<bool> flushRequested_{false};
    // The worker sleeps on wake_ when idle (sleeping_ set); producers bump it.
    std::atomic<bool> sleeping_{false};
    std::atomic<std::uint64_t> wake_{0};
    void wakeWorker();

    std::atomic<bool> quit_{false};
    std::thread worker_;
};

}  // namespace hydra
