#include "dsp_block.h"

#include <chrono>

#include <algorithm>

#if (defined(__x86_64__) || defined(_M_X64)) || defined(__i386__)
#include <immintrin.h>
#endif

namespace hydra {

DspBlock::DspBlock() : shared_(dsp::DspCore::kSharedWords), spareMailboxes_(0x300), ports_(kPortWords) {
    for (int i = 0; i < kCores; ++i) {
        Core& c = cores_[i];
        c.dsp = std::make_unique<dsp::DspCore>(
            i, *this, dsp::DspCore::Memory{c.program.data(), c.data.data(), 0x1FF, shared_.data()});
    }
}

DspBlock::~DspBlock() { stopWorkers(); }

namespace {
// Busy-waits briefly, then yields, so idle workers do not occupy a CPU for long.
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

void DspBlock::setThreads(int threads) {
    threads = std::clamp(threads, 1, kCores);
    if (threads == threads_)
        return;
    stopWorkers();
    threads_ = threads;
    quit_ = false;
    for (int t = 1; t < threads_; ++t)
        workers_.emplace_back([this, t] { workerLoop(t); });
}

void DspBlock::stopWorkers() {
    quit_ = true;
    generation_.fetch_add(1, std::memory_order_release);
    generation_.notify_all();
    for (std::thread& worker : workers_)
        worker.join();
    workers_.clear();
    threads_ = 1;
}

std::uint64_t DspBlock::runCores(int thread) {
    std::uint64_t executed = 0;
    for (int i = thread; i < kCores; i += threads_)
        if ((runMask_ >> i & 1) && !cores_[i].dsp->trapped())
            executed += cores_[i].dsp->run(kInstructionsPerSample);
    return executed;
}

void DspBlock::workerLoop(int thread) {
    std::uint64_t seen = generation_.load(std::memory_order_acquire);
    for (;;) {
        // Spin while samples come quickly; sleep when the DSP side pauses.
        for (int spins = 0; generation_.load(std::memory_order_acquire) == seen; ++spins) {
            if (spins < 8192) {
#if (defined(__x86_64__) || defined(_M_X64)) || defined(__i386__)
                _mm_pause();
#endif
            } else {
                generation_.wait(seen, std::memory_order_acquire);
            }
        }
        seen = generation_.load(std::memory_order_acquire);
        if (quit_)
            return;
        workerExecuted_.fetch_add(runCores(thread), std::memory_order_relaxed);
        finished_.fetch_add(1, std::memory_order_release);
    }
}

std::uint32_t* DspBlock::mailboxWord(std::uint32_t word) {
    const std::uint32_t core = (~word >> 5) & 0x1F;
    if (core < kCores)
        return &cores_[core].mailbox[word & 0x1F];
    return &spareMailboxes_[word - 0x1FC00];
}

std::uint32_t* DspBlock::cpuWord(std::uint32_t offset) {
    const std::uint32_t word = offset >> 2;
    if (offset < 0x20000)
        return &cores_[offset >> 14].program[word & 0xFFF];
    if (offset < 0x7F000)
        return &shared_[word - 0x8000];
    if (offset < 0x80000)
        return mailboxWord(word);
    return nullptr;
}

std::uint32_t DspBlock::read(std::uint32_t offset, int) {
    const std::uint32_t* w = cpuWord(offset & ~3u);
    return w ? *w : 0;
}

void DspBlock::write(std::uint32_t offset, std::uint32_t value, int) {
    if (std::uint32_t* w = cpuWord(offset & ~3u))
        *w = value;
    if (offset < 0x20000)
        cores_[offset >> 14].dsp->invalidate(offset >> 2);
    ++cpuWrites_[offset & ~0xFFu];
    if (offset >= 0x7FC00 && offset < 0x80000 && (offset & 0x7F) == 0 && value != 0 && value != 0x55AA)
        ++commands_[{static_cast<int>((~offset >> 7) & 0x1F), value}];
}

std::uint32_t* DspBlock::ioWord(int core, std::uint32_t a) {
    a &= 0x1FFFF;
    if (a & 0x10000) {
        if (a - 0x8000 < dsp::DspCore::kSharedWords)
            return &shared_[a - 0x8000];
        return mailboxWord(a);
    }
    if (a <= 0xFFF)
        return &cores_[core].program[a];
    if (a >= 0xFF00)
        return &cores_[(~a >> 5) & 7].mailbox[a & 0x1F];
    if (a <= 0x7FFF)
        return &cores_[a >> 12].program[a & 0xFFF];
    return &shared_[a - 0x8000];
}

std::uint32_t* DspBlock::extWord(int core, std::uint32_t a) {
    a &= 0xFFFF;
    if (a <= 0xFFF)
        return &cores_[core].program[a];
    if (a >= 0xFC00)
        return mailboxWord(0x10000 + a);
    return &shared_[a + 0x8000];
}

std::uint32_t DspBlock::ioRead(int core, std::uint32_t a) { return *ioWord(core, a); }
void DspBlock::ioWrite(int core, std::uint32_t a, std::uint32_t v) {
    *ioWord(core, a) = v;
    a &= 0x1FFFF;
    if (a <= 0xFFF)
        cores_[core].dsp->invalidate(a);
    else if (a <= 0x7FFF)
        cores_[a >> 12].dsp->invalidate(a);
}
std::uint32_t DspBlock::extRead(int core, std::uint32_t a) { return *extWord(core, a); }
void DspBlock::extWrite(int core, std::uint32_t a, std::uint32_t v) {
    *extWord(core, a) = v;
    if ((a & 0xFFFF) <= 0xFFF)
        cores_[core].dsp->invalidate(a);
}

void DspBlock::setRunMask(std::uint32_t mask) {
    mask &= 0xFF;
    for (int i = 0; i < kCores; ++i)
        if ((mask >> i & 1) && !(runMask_ >> i & 1))
            cores_[i].dsp->reset();
    runMask_ = mask;
}

void DspBlock::advanceToSample(std::uint64_t target) {
    while (sample_ < target) {
        if (onSampleStart)
            onSampleStart(ports_.data());
        for (int i = 0; i < kCores; ++i)
            if ((runMask_ >> i & 1) && !cores_[i].dsp->trapped())
                cores_[i].dsp->raiseInterrupt(2);
        if (threads_ == 1) {
            executed_ += runCores(0);
        } else {
            finished_.store(0, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
            generation_.notify_all();  // a syscall only if a worker sleeps
            executed_ += runCores(0);
            const auto waitStart = profileSync ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            waitUntil([&] { return finished_.load(std::memory_order_acquire) == threads_ - 1; });
            if (profileSync)
                syncNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
            executed_ += workerExecuted_.exchange(0, std::memory_order_relaxed);
        }
        ++sample_;
        if ((sample_ & 0x1F) == 0 && onBlock)
            onBlock(sample_);
    }
}

std::uint32_t DspControl::read(std::uint32_t offset, int) {
    if (offset == 0)
        return block_.runMask();
    return offset < 0x10 ? regs_[offset >> 2] : 0;
}

void DspControl::write(std::uint32_t offset, std::uint32_t value, int) {
    if (offset == 0) {
        block_.setRunMask(value);
        for (int core = 0; core < DspBlock::kCores; ++core)
            if (value >> (8 + core) & 1)
                block_.requestAttention(core);
    } else if (offset < 0x10) {
        regs_[offset >> 2] = value;
    }
}

}  // namespace hydra
