#include "cpu.h"

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/terminal.h>
#include <dynarmic/frontend/A32/disassembler/disassembler.h>

namespace hydra {

using Dynarmic::A32::Exception;

CpuCore::CpuCore(Memory& memory, MmioBus& bus) : memory_(memory), bus_(bus), system_(*this) {
    cp15_ = std::make_shared<Cp15>(Cp15::Hooks{
        .mmuChanged = [](std::uint32_t, std::uint32_t) {},
        .instructionCacheInvalidated = [this] { jit_->ClearCache(); },
    });
    monitor_ = std::make_unique<Dynarmic::ExclusiveMonitor>(1);

    pageTable_ = std::make_unique<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    buildPageTable();

    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.page_table = pageTable_.get();
    config.global_monitor = monitor_.get();
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.coprocessors[15] = cp15_;
    config.hook_hint_instructions = true;
    config.define_unpredictable_behaviour = true;
    // For retryAccess(): linked blocks only check the cycle budget, so a halt
    // requested by a polled register would otherwise wait for the end of the slice.
    config.check_halt_on_memory_access = true;
    jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
}

CpuCore::~CpuCore() = default;

void CpuCore::buildPageTable() {
    pageTable_->fill(nullptr);
    for (Memory::Region& region : memory_.regions()) {
        if (!region.direct || region.readOnly)
            continue;
        for (std::uint32_t offset = 0; offset + 0x1000 <= region.bytes.size(); offset += 0x1000) {
            const std::uint32_t page = region.base + offset;
            if (page + 0x1000 > watchFrom_ && page < watchTo_)
                continue;  // watched: keep it on the callback path
            (*pageTable_)[page >> 12] = region.bytes.data() + offset;
        }
    }
}

void CpuCore::setWatch(std::uint32_t from, std::uint32_t to) {
    watchFrom_ = from;
    watchTo_ = to;
    buildPageTable();
    if (jit_)
        jit_->ClearCache();
}

void CpuCore::reset(std::uint32_t pc, std::uint32_t cpsr) {
    jit_->Reset();
    jit_->Regs().fill(0);
    jit_->Regs()[15] = pc;
    jit_->SetCpsr(cpsr);
    jit_->SetFpscr(0);
    fatal_.clear();
}

CpuCore::Stop CpuCore::run(std::uint64_t ticks) {
    ticksRemaining_ = ticks;
    running_ = true;
    Dynarmic::HaltReason reason = jit_->Run();
    while (Dynarmic::Has(reason, kHaltSample)) {  // a profiling sample: note the PC and go on
        ++guestSamples_[{regs()[15], regs()[14]}];
        reason = static_cast<Dynarmic::HaltReason>(static_cast<std::uint32_t>(reason) & ~static_cast<std::uint32_t>(kHaltSample));
        if (static_cast<std::uint32_t>(reason) != 0 || ticksRemaining_ == 0)
            break;
        reason = jit_->Run();
    }
    running_ = false;
    if (profileExits) [[unlikely]]
        ++exitReasons_[static_cast<std::uint32_t>(reason)];
    if (Dynarmic::Has(reason, kHaltFatal))
        return Stop::Fatal;
    if (Dynarmic::Has(reason, kHaltWfi))
        return Stop::WaitForInterrupt;
    return Stop::SliceDone;
}

void CpuCore::requestSample() { jit_->HaltExecution(kHaltSample); }

void CpuCore::retryAccess() {
    if (running_)
        jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);  // checked after each access
}

bool CpuCore::takeIrq() {
    if (cpsr() & arm::psr::I)
        return false;
    const std::uint64_t start = profileExits ? __rdtsc() : 0;
    // LR_irq is the next instruction + 4 so that SUBS PC, LR, #4 resumes it.
    system_.enterException(arm::ExceptionKind::Irq, regs()[15] + 4);
    if (profileExits)
        irqCycles += __rdtsc() - start;
    return true;
}

std::uint32_t CpuCore::vectorBase() {
    return cp15_->sctlr() & (1u << 13) ? 0xFFFF0000 : cp15_->vbar();
}

template <typename T>
T CpuCore::read(std::uint32_t addr) {
    if (const std::uint8_t* p = memory_.hostPtr(addr, sizeof(T))) {
        T value;
        std::memcpy(&value, p, sizeof(T));
        return value;
    }
    bus_.log().currentPc = regs()[15];
    if constexpr (sizeof(T) == 8) {
        return bus_.read(addr, 4) | std::uint64_t(bus_.read(addr + 4, 4)) << 32;
    } else {
        return static_cast<T>(bus_.read(addr, sizeof(T)));
    }
}

template <typename T>
void CpuCore::write(std::uint32_t addr, T value) {
    if (addr < watchTo_ && addr + sizeof(T) > watchFrom_ && onWatchedWrite) [[unlikely]]
        onWatchedWrite(regs()[15], regs()[14], addr, value);
    if (Memory::Region* region = memory_.regionFor(addr, sizeof(T)); region && !region->readOnly) {
        std::memcpy(region->bytes.data() + (addr - region->base), &value, sizeof(T));
        return;
    }
    bus_.log().currentPc = regs()[15];
    if constexpr (sizeof(T) == 8) {
        bus_.write(addr, static_cast<std::uint32_t>(value), 4);
        bus_.write(addr + 4, static_cast<std::uint32_t>(value >> 32), 4);
    } else {
        bus_.write(addr, value, sizeof(T));
    }
}

template std::uint32_t CpuCore::read<std::uint32_t>(std::uint32_t);
template void CpuCore::write<std::uint32_t>(std::uint32_t, std::uint32_t);

template <typename T>
bool CpuCore::writeExclusive(std::uint32_t addr, T value, T expected) {
    if (read<T>(addr) != expected)
        return false;
    write<T>(addr, value);
    return true;
}

bool CpuCore::MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) { return writeExclusive(a, v, e); }
bool CpuCore::MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) { return writeExclusive(a, v, e); }
bool CpuCore::MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) { return writeExclusive(a, v, e); }
bool CpuCore::MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) { return writeExclusive(a, v, e); }

std::optional<std::uint32_t> CpuCore::MemoryReadCode(std::uint32_t vaddr) {
    if (const std::uint8_t* p = memory_.hostPtr(vaddr, 4)) {
        std::uint32_t value;
        std::memcpy(&value, p, 4);
        return value;
    }
    return std::nullopt;
}

bool CpuCore::IsSystemInstruction(bool is_thumb, std::uint32_t instruction) {
    return !is_thumb && arm::SystemState::isSystemInstruction(instruction);
}

void CpuCore::fail(std::string message) {
    if (fatal_.empty())
        fatal_ = std::move(message);
    jit_->HaltExecution(kHaltFatal);
}

namespace {
constexpr auto kHaltDelayLoop = Dynarmic::HaltReason::UserDefined3;

// Recognises the head of a count-down delay loop (see skipDelayLoop).
bool isDelayLoop(const std::uint8_t* code) {
    std::uint32_t w[3];
    std::memcpy(w, code, sizeof w);
    const std::uint32_t t = w[0] >> 12 & 0xF, c = w[0] & 0xF;
    return (w[0] & 0xFFFF0FF0) == 0xE1B00000 && (w[1] & 0xFFF00FFF) == 0xE2400001 && (w[1] >> 16 & 0xF) == c &&
           (w[1] >> 12 & 0xF) == c && w[2] == 0x1AFFFFFC && t != c && t != 15 && c != 15;
}
}  // namespace

// When code runs into a delay loop, the block ends just before the loop head with
// a halt, so Run() returns there and skipDelayLoop can take over. A block that
// starts at the head (the loop itself, or its last iterations) is translated
// normally.
bool CpuCore::PreCodeReadHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) {
    const std::uint32_t start = Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC();
    if (start == pc) {
        ++blocksTranslated;
        // Diagnostic: HYDRA_TRACE_TRANSLATE=lo-hi (hex) prints when blocks in that
        // range are first translated, i.e. first run.
        static const std::pair<std::uint32_t, std::uint32_t> traced = [] {
            const char* range = std::getenv("HYDRA_TRACE_TRANSLATE");
            if (!range)
                return std::pair<std::uint32_t, std::uint32_t>{1, 0};
            char* end;
            const auto lo = static_cast<std::uint32_t>(std::strtoul(range, &end, 16));
            const auto hi = static_cast<std::uint32_t>(std::strtoul(*end ? end + 1 : end, nullptr, 16));
            return std::pair<std::uint32_t, std::uint32_t>{lo, hi};
        }();
        if (pc >= traced.first && pc < traced.second)
            std::fprintf(stderr, "translate %08x at %.3f s\n", pc, static_cast<double>(ticksExecuted_) / 400e6);
    }
    if (is_thumb)
        return true;
    const std::uint8_t* code = start == pc ? nullptr : memory_.hostPtr(pc, 12);
    if (!code || !isDelayLoop(code)) {
        if (profileBlocks_ && start / 4 < kProfiledCodeWords) {
            // The counting call must not precede a conditional first instruction
            // (Dynarmic would end the block before it, empty); such blocks count
            // at their second instruction, inside the condition.
            const std::uint8_t* first = memory_.hostPtr(start, 4);
            const bool conditionalStart = first && (first[3] >> 4) < 0xE;
            if (pc == (conditionalStart ? start + 4 : start))
                ir.CallHostFunction(&CpuCore::blockThunk, ir.Imm64(reinterpret_cast<std::uint64_t>(this)), ir.Imm64(start));
            blockLengths_[start / 4] = (pc + 4 - start) / 4;
        }
        return true;
    }
    ir.CallHostFunction(&CpuCore::delayLoopThunk, ir.Imm64(reinterpret_cast<std::uint64_t>(this)));
    ir.BranchWritePC(ir.Imm32(pc));
    ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
    return false;
}

void CpuCore::blockThunk(std::uint64_t core, std::uint64_t pc) {
    ++reinterpret_cast<CpuCore*>(core)->blockEntries_[pc / 4];
}

void CpuCore::setProfileBlocks(bool enabled) {
    if (enabled == profileBlocks_)
        return;
    profileBlocks_ = enabled;
    if (enabled && blockEntries_.empty()) {
        blockEntries_.assign(kProfiledCodeWords, 0);
        blockLengths_.assign(kProfiledCodeWords, 0);
    }
    jit_->ClearCache();
}

std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> CpuCore::blockCounts() const {
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> counts;
    for (std::uint32_t i = 0; i < blockEntries_.size(); ++i)
        if (blockEntries_[i])
            counts[i * 4] = {blockEntries_[i], blockLengths_[i]};
    return counts;
}

void CpuCore::delayLoopThunk(std::uint64_t core) {
    CpuCore& self = *reinterpret_cast<CpuCore*>(core);
    ++self.delayLoopEntries_;
    self.jit_->HaltExecution(kHaltDelayLoop);
}

// A count-down delay loop at the current PC, ARM state:
//   head:   MOVS Rt, Rc        ; flags from the counter
//           SUB  Rc, Rc, #1
//           BNE  head
// With Rc = n at the head the loop runs n + 1 more times, 3 instructions each.
// Fast-forwarding k <= n iterations leaves it at the head with Rc = n - k,
// Rt = n - k + 1, N from Rt and Z clear, exactly as running them would.
std::uint64_t CpuCore::skipDelayLoop(std::uint64_t budget) {
    if (!delayLoopSkip || (cpsr() & (1u << 5)))
        return 0;  // Thumb
    auto& r = regs();
    const std::uint32_t pc = r[15];
    const std::uint8_t* p = memory_.hostPtr(pc, 12);
    if (!p)
        return 0;
    if (!isDelayLoop(p))
        return 0;
    std::uint32_t w0;
    std::memcpy(&w0, p, 4);
    const std::uint32_t t = w0 >> 12 & 0xF, c = w0 & 0xF;
    const std::uint32_t n = r[c];
    if (delayCallers)
        ++(*delayCallers)[static_cast<std::uint64_t>(r[14]) << 32 | std::min<std::uint32_t>(n, 0xFFFFFFFF)];
    const std::uint32_t k = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, budget / 3));
    if (k == 0)
        return 0;
    r[c] = n - k;
    r[t] = n - k + 1;
    setCpsr((cpsr() & ~0xC0000000u) | (r[t] & 0x80000000u));  // N from Rt, Z clear
    ticksExecuted_ += 3ull * k;
    ticksSkipped_ += 3ull * k;
    return 3ull * k;
}

void CpuCore::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    fail(std::format("interpreter fallback at {:08x} ({} instructions): {}", pc, num_instructions,
                     Dynarmic::A32::DisassembleArm(read<std::uint32_t>(pc))));
}

void CpuCore::CallSVC(std::uint32_t) {
    ++svcCount;
    const std::uint64_t start = profileExits ? __rdtsc() : 0;
    // PC already points past the SVC, which is the preferred return address.
    system_.enterException(arm::ExceptionKind::SupervisorCall, regs()[15]);
    if (profileExits)
        svcCycles += __rdtsc() - start;
}

void CpuCore::ExceptionRaised(std::uint32_t pc, Exception exception) {
    const auto instruction = MemoryReadCode(pc);
    const auto describe = [&] {
        return instruction ? std::format("{:08x} '{}' at {:08x}", *instruction, Dynarmic::A32::DisassembleArm(*instruction), pc)
                           : std::format("<no code> at {:08x}", pc);
    };

    switch (exception) {
    case Exception::SystemInstruction: {
        ++systemInstructions;
        const std::uint64_t start = profileExits ? __rdtsc() : 0;
        if (!instruction || system_.execute(*instruction, pc) == arm::SystemState::Result::Unsupported)
            fail("unsupported system instruction " + describe());
        if (profileExits)
            systemInstructionCycles += __rdtsc() - start;
        return;
    }
    case Exception::WaitForInterrupt:
        jit_->HaltExecution(kHaltWfi);
        return;
    case Exception::WaitForEvent:
    case Exception::SendEvent:
    case Exception::SendEventLocal:
    case Exception::Yield:
    case Exception::PreloadData:
    case Exception::PreloadDataWithIntentToWrite:
    case Exception::PreloadInstruction:
        return;
    case Exception::Breakpoint:
        fail(std::format("BKPT {} (lr={:08x})", describe(), regs()[14]));
        return;
    case Exception::NoExecuteFault:
        fail(std::format("instruction fetch from unmapped address {:08x} (lr={:08x})", pc, regs()[14]));
        return;
    default:
        fail(std::format("exception {} for {}", static_cast<int>(exception), describe()));
        return;
    }
}

void CpuCore::AddTicks(std::uint64_t ticks) {
    ticksExecuted_ += ticks;
    ticksRemaining_ = ticks >= ticksRemaining_ ? 0 : ticksRemaining_ - ticks;
}

}  // namespace hydra
