#include "effects_rack.h"

#include <chrono>

#if (defined(__x86_64__) || defined(_M_X64)) || defined(__i386__)
#include <immintrin.h>
#endif

namespace hydra {

namespace {
// Busy-waits briefly, then yields.
template <typename Done>
void waitUntil(Done done) {
    for (int spins = 0; !done(); ++spins) {
        if (spins < 4096) {
#if (defined(__x86_64__) || defined(_M_X64)) || defined(__i386__)
            _mm_pause();
#endif
        } else {
            std::this_thread::yield();
        }
    }
}
}  // namespace

// Forwards the I2C bus's calls for one DSP: queued in threaded mode, direct
// otherwise; a read first lets the effects thread catch up.
class EffectsRack::Port : public I2cSlave {
public:
    Port(EffectsRack& rack, int dsp) : rack_(rack), dsp_(dsp) {}
    void start(bool read) override { rack_.queueOp(dsp_, OpKind::Start, read ? 1 : 0); }
    bool write(std::uint8_t byte) override {
        rack_.queueOp(dsp_, OpKind::Write, byte);
        return true;
    }
    std::uint8_t read() override {
        rack_.flush();
        return rack_.dsps_[dsp_]->read();
    }
    void stop() override { rack_.queueOp(dsp_, OpKind::Stop, 0); }

private:
    EffectsRack& rack_;
    int dsp_;
};

EffectsRack::EffectsRack(bool threaded) : threaded_(threaded) {
    dsps_[0] = std::make_unique<FxDsp>("fxA");
    dsps_[1] = std::make_unique<FxDsp>("fxB");
    for (int i = 0; i < 2; ++i)
        ports_[i] = std::make_unique<Port>(*this, i);
    if (threaded_) {
        ring_.resize(kRingSize);
        worker_ = std::thread([this] { workerLoop(); });
    }
}

I2cSlave& EffectsRack::port(int index) { return *ports_[index]; }

EffectsRack::~EffectsRack() {
    quit_.store(true, std::memory_order_seq_cst);
    wakeWorker();
    if (worker_.joinable())
        worker_.join();
}

void EffectsRack::push(const std::uint32_t* ports) {
    const Sample sample{ports[64], ports[65], ports[70], ports[71], ports[66], ports[67]};
    if (!threaded_) {
        process(sample);
        return;
    }
    const std::uint64_t index = pushed_.load(std::memory_order_relaxed);
    waitUntil([&] { return index - processed_.load(std::memory_order_acquire) < kRingSize; });
    ring_[index % kRingSize] = sample;
    pushed_.store(index + 1, std::memory_order_seq_cst);
    wakeWorker();
}

void EffectsRack::wakeWorker() {
    if (sleeping_.load(std::memory_order_seq_cst)) {
        wake_.fetch_add(1, std::memory_order_seq_cst);
        wake_.notify_one();
    }
}

void EffectsRack::flush() {
    if (!threaded_)
        return;
    // Only the effects thread applies queued accesses; when it runs out of samples
    // with a flush pending, it applies the ones stamped for future samples too.
    const std::uint64_t target = pushed_.load(std::memory_order_acquire);
    ++flushes;
    flushBacklog += target - processed_.load(std::memory_order_acquire);
    const auto start = std::chrono::steady_clock::now();
    flushRequested_.store(true, std::memory_order_seq_cst);
    wakeWorker();
    waitUntil([&] {
        return processed_.load(std::memory_order_acquire) >= target && pendingOps_.load(std::memory_order_acquire) == 0;
    });
    flushNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

void EffectsRack::queueOp(int dsp, OpKind kind, std::uint8_t value) {
    const Op op{pushed_.load(std::memory_order_relaxed), dsp, kind, value};
    if (!threaded_) {
        apply(op);
        return;
    }
    const std::lock_guard lock(opsMutex_);
    ops_.push_back(op);
    pendingOps_.fetch_add(1, std::memory_order_release);
}

void EffectsRack::applyOps(std::uint64_t upTo) {
    if (pendingOps_.load(std::memory_order_acquire) == 0)
        return;
    const std::lock_guard lock(opsMutex_);
    while (!ops_.empty() && ops_.front().sample <= upTo) {
        apply(ops_.front());
        ops_.pop_front();
        pendingOps_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void EffectsRack::apply(const Op& op) {
    FxDsp& dsp = *dsps_[op.dsp];
    switch (op.kind) {
    case OpKind::Start: dsp.start(op.value != 0); break;
    case OpKind::Write: dsp.write(op.value); break;
    case OpKind::Stop: dsp.stop(); break;
    }
}

void EffectsRack::process(const Sample& s) {
    std::uint32_t left = s.dryLeft, right = s.dryRight;
    FxDsp& a = *dsps_[0];
    FxDsp& b = *dsps_[1];
    if (enabled.load(std::memory_order_relaxed) && a.running() && b.running()) {
        const auto quarter = [](std::uint32_t v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 2); };
        a.process(quarter(s.dryLeft), quarter(s.dryRight), s.a4, s.a5);
        b.process(a.port(0), a.port(1), s.b4, s.b5);
        left = b.port(6);
        right = b.port(7);
    }
    if (onOutput)
        onOutput(static_cast<std::int32_t>(left), static_cast<std::int32_t>(right));
}

void EffectsRack::workerLoop() {
    std::uint64_t next = 0;
    while (!quit_.load(std::memory_order_relaxed)) {
        if (pushed_.load(std::memory_order_acquire) == next) {
            if (flushRequested_.exchange(false, std::memory_order_acq_rel))
                applyOps(UINT64_MAX);
            // Out of samples: spin briefly (they come every ~20 us), then sleep
            // until push() or flush() wakes us.
            const auto ready = [&] {
                return pushed_.load(std::memory_order_acquire) != next || quit_.load(std::memory_order_relaxed) ||
                       flushRequested_.load(std::memory_order_acquire);
            };
            for (int spins = 0; spins < 1024 && !hostIdle_.load(std::memory_order_relaxed) && !ready(); ++spins) {
#if (defined(__x86_64__) || defined(_M_X64)) || defined(__i386__)
                _mm_pause();
#endif
            }
            if (!ready()) {
                const std::uint64_t seen = wake_.load(std::memory_order_seq_cst);
                sleeping_.store(true, std::memory_order_seq_cst);
                if (!ready())  // either this sees the producer's store or the producer sees sleeping_
                    wake_.wait(seen, std::memory_order_seq_cst);
                sleeping_.store(false, std::memory_order_seq_cst);
            }
            continue;
        }
        applyOps(next);
        process(ring_[next % kRingSize]);
        ++next;
        processed_.store(next, std::memory_order_release);
    }
}

}  // namespace hydra
