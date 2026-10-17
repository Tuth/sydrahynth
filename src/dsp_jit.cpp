#include "dsp_jit.h"

#include <vector>

namespace hydra::dsp {

namespace {
constexpr std::size_t kCodeSize = 4u << 20;
constexpr std::size_t kBlockReserve = 64u << 10;  // more than the largest block can take
constexpr std::uint32_t kMaxBlock = 64;           // instructions

bool isControlFlow(Op op) {
    switch (op) {
    case Op::BranchGreater: case Op::BranchGreaterEqual: case Op::BranchEqual: case Op::BranchNotEqual:
    case Op::BranchHighEqualLow: case Op::BranchHighNotEqualLow: case Op::BranchLowEqual: case Op::BranchLowNotEqual:
    case Op::BranchBitSet: case Op::BranchBitClear:
    case Op::JumpIfZero: case Op::JumpIfNotZero: case Op::Loop: case Op::Call: case Op::JumpLink: case Op::Jump:
    case Op::Return: case Op::JumpRegister: case Op::ReturnFromInterrupt:
        return true;
    default:
        return false;
    }
}

template <typename T>
std::int32_t offsetIn(const DspCore& core, const T& member) {
    return static_cast<std::int32_t>(reinterpret_cast<const char*>(&member) - reinterpret_cast<const char*>(&core));
}
}  // namespace

// Host thunks use the platform ABI. The emitted code uses rdi/esi/edx
// internally; emitCall bridges these to rcx/edx/r8d on Windows.
struct JitThunks {
    static std::uint32_t HYDRA_JIT_HOST_ABI step(DspCore* core) { return static_cast<std::uint32_t>(core->interpret(2, 1)); }
    static std::uint32_t HYDRA_JIT_HOST_ABI idle(DspCore* core, std::uint32_t target) { return core->branchIdle(target) ? 1 : 0; }
    static std::uint32_t HYDRA_JIT_HOST_ABI dataRegRead(DspCore* core, std::uint32_t address) {
        return core->host_.dataRegRead(core->id_, address);
    }
    static void HYDRA_JIT_HOST_ABI dataRegWrite(DspCore* core, std::uint32_t address, std::uint32_t value) {
        core->host_.dataRegWrite(core->id_, address, value);
    }
    static std::uint32_t HYDRA_JIT_HOST_ABI ioRead(DspCore* core, std::uint32_t address) { return core->host_.ioRead(core->id_, address); }
    static void HYDRA_JIT_HOST_ABI ioWrite(DspCore* core, std::uint32_t address, std::uint32_t value) {
        core->host_.ioWrite(core->id_, address, value);
    }
};

DspJit::DspJit(DspCore& core) : Xbyak::CodeGenerator(kCodeSize), core_(core) {
    pcOffset_ = offsetIn(core, core.pc_);
    pendingOffset_ = offsetIn(core, core.pending_);
    writesOffset_ = offsetIn(core, core.writes_);
    spOffset_ = offsetIn(core, core.sp_);
    stackOffset_ = offsetIn(core, core.stack_);
    inInterruptOffset_ = offsetIn(core, core.inInterrupt_);
    regsOffset_ = offsetIn(core, core.r_);
    emitCommon();
}

void DspJit::flushAll() {
    reset();
    blocks_.fill(nullptr);
    emitCommon();
}

void DspJit::emitCall(const void* function) {
    mov(rax, reinterpret_cast<std::uint64_t>(function));
#if defined(_WIN32) || defined(HYDRA_TEST_WINDOWS_ABI)
    mov(r8d, edx);
    mov(edx, esi);
    mov(rcx, rdi);
    sub(rsp, 32);  // mandatory Windows x64 caller-owned shadow space
    call(rax);
    add(rsp, 32);
#else
    call(rax);
#endif
}

void DspJit::emitCommon() {
    // enter(core, code, &executed, budget)
    enter_ = getCurr<Enter>();
    push(rbp);
    push(rbx);
    push(r12);
    push(r13);
    push(r14);
    push(r15);
#if defined(_WIN32) || defined(HYDRA_TEST_WINDOWS_ABI)
    push(rdi);
    push(rsi);  // these are nonvolatile on Windows, scratch in the JIT
    push(r8);   // &executed; nine pushes keep rsp 16-byte aligned
    mov(rbx, rcx);
    mov(r12, qword[r8]);
    mov(r13, r9);
    lea(r14, ptr[rcx + regsOffset_]);
    mov(r15, reinterpret_cast<std::uint64_t>(core_.mem_.data));
    jmp(rdx);
#else
    push(rdx);  // seven pushes: rsp is 16-byte aligned for calls from here on
    mov(rbx, rdi);
    mov(r12, qword[rdx]);
    mov(r13, rcx);
    lea(r14, ptr[rdi + regsOffset_]);
    mov(r15, reinterpret_cast<std::uint64_t>(core_.mem_.data));
    jmp(rsi);
#endif

    exitCommon_ = getCurr();
    pop(rdx);
    mov(qword[rdx], r12);
#if defined(_WIN32) || defined(HYDRA_TEST_WINDOWS_ABI)
    pop(rsi);
    pop(rdi);
#endif
    pop(r15);
    pop(r14);
    pop(r13);
    pop(r12);
    pop(rbx);
    pop(rbp);
    ret();

    exitNormal_ = getCurr();
    mov(eax, kNormal);
    jmp(exitCommon_, T_NEAR);

    exitIdle_ = getCurr();
    mov(eax, kIdle);
    jmp(exitCommon_, T_NEAR);

    // eax = next pc: on to its block if it is compiled, nothing is pending and
    // the budget is not used up; else back to run().
    dispatch_ = getCurr();
    mov(dword[rbx + pcOffset_], eax);
    cmp(dword[rbx + pendingOffset_], 0);
    jne(exitNormal_);
    mov(rcx, reinterpret_cast<std::uint64_t>(&flush_));
    cmp(byte[rcx], 0);
    jne(exitNormal_);
    cmp(r12, r13);
    jae(exitNormal_);
    and_(eax, 0xFFF);
    mov(rcx, reinterpret_cast<std::uint64_t>(blocks_.data()));
    mov(rcx, qword[rcx + rax * 8]);
    test(rcx, rcx);
    jz(exitNormal_);
    jmp(rcx);
}

std::uint64_t DspJit::run(std::uint64_t budget) {
    std::uint64_t executed = 0;
    while (executed < budget) {
        if (core_.trapped_)
            return executed;
        if (flush_.exchange(false, std::memory_order_acq_rel))
            flushAll();
        if (core_.pending_ != 0) {
            core_.takeInterrupt();
            if (core_.pending_ != 0) {  // not deliverable now: step as the interpreter does
                executed += core_.interpret(budget - executed, 1);
                continue;
            }
        }
        const std::uint32_t pc = core_.pc_ & 0xFFF;
        const void* code = blocks_[pc] ? blocks_[pc] : compile(pc);
        if (!code) {
            executed += core_.interpret(budget - executed, 1);
            continue;
        }
        switch (enter_(&core_, code, &executed, budget)) {
        case kIdle: return budget;
        case kBudget: return executed + core_.interpret(budget - executed, budget - executed);
        default: break;
        }
    }
    return executed;
}

const void* DspJit::compile(std::uint32_t start) {
    if (getSize() + kBlockReserve > kCodeSize)
        flushAll();

    std::vector<Instruction> block;
    for (std::uint32_t pc = start; block.size() < kMaxBlock; pc = (pc + 1) & 0xFFF) {
        const Decoded d = decode(core_.mem_.program[pc]);
        if (d.op == Op::Invalid)
            break;
        block.push_back({pc, d});
        if (isControlFlow(d.op))
            break;
    }
    if (block.empty())
        return nullptr;

    const auto length = static_cast<std::uint32_t>(block.size());
    align(16);
    const void* entry = getCurr();
    Xbyak::Label overBudget;
    lea(rax, ptr[r12 + length]);
    cmp(rax, r13);
    ja(overBudget, T_NEAR);
    for (const Instruction& in : block)
        emitInstruction(in, length, entry, start);
    if (!isControlFlow(block.back().d.op))
        emitGoto((block.back().pc + 1) & 0xFFF, length, entry, start);
    L(overBudget);
    mov(dword[rbx + pcOffset_], start);
    mov(eax, kBudget);
    jmp(exitCommon_, T_NEAR);

    blocks_[start] = entry;
    return entry;
}

void DspJit::emitGoto(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    add(r12, length);
    mov(eax, target);
    const void* direct = target == blockStart ? blockEntry : target <= 0xFFF ? blocks_[target] : nullptr;
    if (direct) {
        // Straight on to a compiled block unless the host raised an interrupt
        // meanwhile (the dispatcher hands that over to run()).
        cmp(dword[rbx + pendingOffset_], 0);
        jne(dispatch_);
        jmp(direct, T_NEAR);
    } else {
        jmp(dispatch_, T_NEAR);
    }
}

void DspJit::emitGotoEax(std::uint32_t length) {
    add(r12, length);
    jmp(dispatch_, T_NEAR);
}

void DspJit::emitBackwardBranch(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    // As the interpreter: pc_ = target, then ask the idle-loop detection.
    mov(dword[rbx + pcOffset_], target);
    mov(rdi, rbx);
    mov(esi, target);
    emitCall(reinterpret_cast<const void*>(&JitThunks::idle));
    test(eax, eax);
    jnz(exitIdle_);
    emitGoto(target, length, blockEntry, blockStart);
}

void DspJit::emitFallback(const Instruction& in, bool controlFlow, std::uint32_t length) {
    mov(dword[rbx + pcOffset_], in.pc);
    mov(rdi, rbx);
    emitCall(reinterpret_cast<const void*>(&JitThunks::step));
    if (controlFlow) {
        cmp(eax, 1);
        jne(exitIdle_);
        mov(eax, dword[rbx + pcOffset_]);
        emitGotoEax(length);
    }
}

void DspJit::emitIncrementWrites() { add(qword[rbx + writesOffset_], 1); }

void DspJit::emitClampS32() {
    mov(rcx, 0x7FFFFFFF);
    cmp(rax, rcx);
    cmovg(rax, rcx);
    mov(rcx, -0x80000000LL);
    cmp(rax, rcx);
    cmovl(rax, rcx);
}

void DspJit::emitDataLoad() {
    const DspCore::Memory& m = core_.mem_;
    Xbyak::Label notRegister, host, done;
    and_(eax, m.dataMask);
    cmp(eax, 15);
    ja(notRegister, T_NEAR);
    mov(eax, dword[r14 + rax * 4]);
    jmp(done, T_NEAR);
    L(notRegister);
    if (m.dataLimit <= m.dataMask) {
        cmp(eax, m.dataLimit);
        jae(host, T_NEAR);
    }
    mov(eax, dword[r15 + rax * 4]);
    jmp(done, T_NEAR);
    L(host);
    if (m.dataLimit <= m.dataMask) {
        mov(esi, eax);
        mov(rdi, rbx);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegRead));
    }
    L(done);
}

void DspJit::emitDataLoad(std::uint32_t address) {
    const DspCore::Memory& m = core_.mem_;
    address &= m.dataMask;
    if (address <= 15) {
        mov(eax, reg(static_cast<int>(address)));
    } else if (address >= m.dataLimit) {
        mov(esi, address);
        mov(rdi, rbx);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegRead));
    } else {
        mov(eax, dword[r15 + address * 4]);
    }
}

void DspJit::emitDataStore() {
    const DspCore::Memory& m = core_.mem_;
    Xbyak::Label notRegister, host, done;
    and_(eax, m.dataMask);
    cmp(eax, 15);
    ja(notRegister, T_NEAR);
    mov(dword[r14 + rax * 4], edx);
    jmp(done, T_NEAR);
    L(notRegister);
    if (m.dataLimit <= m.dataMask) {
        cmp(eax, m.dataLimit);
        jae(host, T_NEAR);
    }
    mov(dword[r15 + rax * 4], edx);
    jmp(done, T_NEAR);
    L(host);
    if (m.dataLimit <= m.dataMask) {
        mov(esi, eax);
        mov(rdi, rbx);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegWrite));
    }
    L(done);
}

void DspJit::emitDataStore(std::uint32_t address) {
    const DspCore::Memory& m = core_.mem_;
    address &= m.dataMask;
    if (address <= 15) {
        mov(reg(static_cast<int>(address)), edx);
    } else if (address >= m.dataLimit) {
        mov(esi, address);
        mov(rdi, rbx);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegWrite));
    } else {
        mov(dword[r15 + address * 4], edx);
    }
}

// Shared RAM at io 0x8000-0xFEFF and 0x10000-0x1FBFF (word = address - 0x8000);
// the rest goes to the host. Address in eax (17 bits).
void DspJit::emitIoRead() {
    Xbyak::Label shared, done;
    if (core_.mem_.shared) {
        lea(ecx, ptr[rax - 0x8000]);
        cmp(ecx, 0xFF00 - 0x8000);
        jb(shared, T_NEAR);
        lea(edx, ptr[rcx - 0x8000]);
        cmp(edx, 0x1FC00 - 0x10000);
        jb(shared, T_NEAR);
    }
    mov(esi, eax);
    mov(rdi, rbx);
    emitCall(reinterpret_cast<const void*>(&JitThunks::ioRead));
    if (core_.mem_.shared) {
        jmp(done, T_NEAR);
        L(shared);
        mov(rdx, reinterpret_cast<std::uint64_t>(core_.mem_.shared));
        mov(eax, dword[rdx + rcx * 4]);
    }
    L(done);
}

// Address in eax, value in edx.
void DspJit::emitIoWrite() {
    emitIncrementWrites();
    Xbyak::Label shared, done;
    if (core_.mem_.shared) {
        lea(ecx, ptr[rax - 0x8000]);
        cmp(ecx, 0xFF00 - 0x8000);
        jb(shared, T_NEAR);
        lea(esi, ptr[rcx - 0x8000]);
        cmp(esi, 0x1FC00 - 0x10000);
        jb(shared, T_NEAR);
    }
    mov(esi, eax);
    mov(rdi, rbx);
    emitCall(reinterpret_cast<const void*>(&JitThunks::ioWrite));
    if (core_.mem_.shared) {
        jmp(done, T_NEAR);
        L(shared);
        mov(rsi, reinterpret_cast<std::uint64_t>(core_.mem_.shared));
        mov(dword[rsi + rcx * 4], edx);
    }
    L(done);
}

void DspJit::emitInstruction(const Instruction& in, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    const Decoded& d = in.d;
    const std::uint32_t pc = in.pc, next = (pc + 1) & 0xFFF;
    const Xbyak::Address ra = reg(d.a), rb = reg(d.b), rc = reg(d.c);
    const bool ioHook = static_cast<bool>(core_.onIoWrite);

    // Conditional relative branch: taken -> pc + offset + 1, backward ones with
    // idle-loop detection.
    const auto branch = [&](auto emitJumpIfTaken) {
        Xbyak::Label taken;
        emitJumpIfTaken(taken);
        emitGoto(next, length, blockEntry, blockStart);
        L(taken);
        const std::uint32_t target = (pc + static_cast<std::uint32_t>(d.offset) + 1) & 0xFFF;
        if (d.offset < 0)
            emitBackwardBranch(target, length, blockEntry, blockStart);
        else
            emitGoto(target, length, blockEntry, blockStart);
    };
    // Absolute jump; idle detection when enabled and backward.
    const auto jumpTo = [&](std::uint32_t target) {
        if (core_.jumpIdleDetection_ && target <= pc)
            emitBackwardBranch(target & 0xFFF, length, blockEntry, blockStart);
        else
            emitGoto(target & 0xFFF, length, blockEntry, blockStart);
    };

    switch (d.op) {
    case Op::Nop: break;

    case Op::Load:
        emitDataLoad(d.operand);
        mov(ra, eax);
        break;
    case Op::Store:
        emitIncrementWrites();
        mov(edx, ra);
        emitDataStore(d.operand);
        break;
    case Op::LoadIndexed:
        mov(eax, rb);
        add(eax, d.operand);
        emitDataLoad();
        mov(ra, eax);
        break;
    case Op::StoreIndexed:
        emitIncrementWrites();
        mov(eax, rb);
        add(eax, d.operand);
        mov(edx, ra);
        emitDataStore();
        break;
    case Op::LoadReg:
        mov(eax, rb);
        emitDataLoad();
        mov(ra, eax);
        break;
    case Op::LoadRegInc:
        mov(eax, rb);
        lea(ecx, ptr[rax + 1]);
        mov(rb, ecx);
        emitDataLoad();
        mov(ra, eax);
        break;
    case Op::StoreReg:
        emitIncrementWrites();
        mov(eax, ra);
        mov(edx, rb);
        emitDataStore();
        break;
    case Op::StoreRegInc:
        mov(eax, ra);
        lea(ecx, ptr[rax + 1]);
        mov(ra, ecx);
        emitIncrementWrites();
        mov(edx, rb);
        emitDataStore();
        break;

    case Op::IoLoad:
        mov(eax, d.operand & 0x1FFFF);
        emitIoRead();
        mov(ra, eax);
        break;
    case Op::IoLoadReg:
        mov(eax, rb);
        and_(eax, 0x1FFFF);
        emitIoRead();
        mov(ra, eax);
        break;
    case Op::IoLoadRegInc:
        mov(eax, rb);
        lea(ecx, ptr[rax + 1]);
        mov(rb, ecx);
        and_(eax, 0x1FFFF);
        emitIoRead();
        mov(ra, eax);
        break;
    case Op::IoStore:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        mov(eax, d.operand & 0x1FFFF);
        mov(edx, ra);
        emitIoWrite();
        break;
    case Op::IoStoreReg:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        mov(eax, ra);
        and_(eax, 0x1FFFF);
        mov(edx, rb);
        emitIoWrite();
        break;
    case Op::IoStoreRegInc:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        mov(eax, ra);
        lea(ecx, ptr[rax + 1]);
        mov(ra, ecx);
        and_(eax, 0x1FFFF);
        mov(edx, rb);
        emitIoWrite();
        break;

    case Op::AddImm: add(ra, d.operand); break;
    case Op::SubImm: sub(ra, d.operand); break;
    case Op::MovImm: mov(ra, d.operand); break;
    case Op::AddImmLow:
    case Op::SubImmLow:
        mov(eax, ra);
        mov(ecx, eax);
        if (d.op == Op::AddImmLow)
            add(ecx, d.operand);
        else
            sub(ecx, d.operand);
        and_(eax, 0xFFFF0000);
        movzx(ecx, cx);
        or_(eax, ecx);
        mov(ra, eax);
        break;
    case Op::MovImmLow:
        mov(eax, ra);
        and_(eax, 0xFFFF0000);
        or_(eax, d.operand);
        mov(ra, eax);
        break;
    case Op::AndImmLow: and_(ra, d.operand | 0xFFFF0000); break;
    case Op::OrImmLow: or_(ra, d.operand); break;
    case Op::MovImmHigh:
        mov(eax, ra);
        and_(eax, 0xFFFF);
        or_(eax, d.operand << 16);
        mov(ra, eax);
        break;
    case Op::AndImmHigh: and_(ra, d.operand << 16 | 0xFFFF); break;

    case Op::Mul:
    case Op::MulAcc:
        movsxd(rax, ra);
        movsxd(rcx, rb);
        imul(rax, rcx);
        if (d.shift)
            sar(rax, d.shift);
        if (d.op == Op::Mul)
            mov(rc, eax);
        else
            add(rc, eax);
        break;
    case Op::MulMem:
        emitDataLoad(d.operand);
        movsxd(rcx, eax);
        movsxd(rax, ra);
        imul(rax, rcx);
        if (d.shift)
            sar(rax, d.shift);
        mov(ra, eax);
        break;
    case Op::AddMem:
        emitDataLoad(d.operand);
        add(ra, eax);
        break;
    case Op::SubMem:
        emitDataLoad(d.operand);
        sub(ra, eax);
        break;
    case Op::AddMemSat:
    case Op::AddMemSatStore:
        if (d.op == Op::AddMemSatStore)
            emitIncrementWrites();
        emitDataLoad(d.operand);
        movsxd(rdx, eax);
        movsxd(rax, ra);
        add(rax, rdx);
        emitClampS32();
        if (d.op == Op::AddMemSat) {
            mov(ra, eax);
        } else {
            mov(edx, eax);
            emitDataStore(d.operand);
        }
        break;

    case Op::Add:
        mov(eax, ra);
        add(eax, rb);
        mov(rc, eax);
        break;
    case Op::Sub:
        mov(eax, ra);
        sub(eax, rb);
        mov(rc, eax);
        break;
    case Op::AddSat:
    case Op::SubSat:
        movsxd(rax, ra);
        movsxd(rdx, rb);
        if (d.op == Op::AddSat)
            add(rax, rdx);
        else
            sub(rax, rdx);
        emitClampS32();
        mov(rc, eax);
        break;
    case Op::And:
        mov(eax, ra);
        and_(eax, rb);
        mov(rc, eax);
        break;
    case Op::Or:
        mov(eax, ra);
        or_(eax, rb);
        mov(rc, eax);
        break;
    case Op::Shl:
    case Op::Sar:
        mov(eax, ra);
        mov(ecx, rb);
        and_(ecx, 31);
        if (d.op == Op::Shl)
            shl(eax, cl);
        else
            sar(eax, cl);
        mov(rc, eax);
        break;
    case Op::Max:
    case Op::Min:
        mov(eax, ra);
        mov(ecx, rb);
        cmp(eax, ecx);
        if (d.op == Op::Max)
            cmovle(eax, ecx);
        else
            cmovge(eax, ecx);
        mov(rc, eax);
        break;
    case Op::SarImm:
    case Op::ShlImm:
    case Op::ShrImm:
        mov(eax, ra);
        mov(ecx, d.shift);  // by cl, as the interpreter's variable shift
        if (d.op == Op::SarImm)
            sar(eax, cl);
        else if (d.op == Op::ShlImm)
            shl(eax, cl);
        else
            shr(eax, cl);
        mov(rc, eax);
        break;

    case Op::Mov:
        mov(eax, rb);
        mov(ra, eax);
        break;
    case Op::MovHighToLow:
        mov(eax, rb);
        shr(eax, 16);
        mov(ra, eax);
        break;
    case Op::MovLowToHigh:
        mov(eax, rb);
        shl(eax, 16);
        mov(ra, eax);
        break;
    case Op::MovHigh:
        mov(eax, rb);
        and_(eax, 0xFFFF0000);
        mov(ra, eax);
        break;
    case Op::NegSat:
        movsxd(rax, ra);
        neg(rax);
        mov(rcx, 0x7FFFFFFF);
        cmp(rax, rcx);
        cmovg(rax, rcx);
        mov(ra, eax);
        break;
    case Op::AbsSat:
        movsxd(rax, ra);
        mov(rdx, rax);
        neg(rdx);
        cmovs(rdx, rax);  // -x negative: x was positive
        mov(rcx, 0x7FFFFFFF);
        cmp(rdx, rcx);
        cmovg(rdx, rcx);
        mov(rc, edx);
        break;

    case Op::BranchGreater:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); cmp(eax, rb); jg(taken, T_NEAR); });
        break;
    case Op::BranchGreaterEqual:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); cmp(eax, rb); jge(taken, T_NEAR); });
        break;
    case Op::BranchEqual:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); cmp(eax, rb); je(taken, T_NEAR); });
        break;
    case Op::BranchNotEqual:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); cmp(eax, rb); jne(taken, T_NEAR); });
        break;
    case Op::BranchBitSet:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); bt(eax, d.c & 31); jc(taken, T_NEAR); });
        break;
    case Op::BranchBitClear:
        branch([&](Xbyak::Label& taken) { mov(eax, ra); bt(eax, d.c & 31); jnc(taken, T_NEAR); });
        break;
    case Op::JumpIfZero:
    case Op::JumpIfNotZero: {
        Xbyak::Label taken;
        cmp(ra, 0);
        if (d.op == Op::JumpIfZero)
            je(taken, T_NEAR);
        else
            jne(taken, T_NEAR);
        emitGoto(next, length, blockEntry, blockStart);
        L(taken);
        jumpTo(d.operand);
        break;
    }
    case Op::Jump: jumpTo(d.operand); break;
    case Op::Loop: {
        // if (ra-- != 1) jump to the operand
        Xbyak::Label done;
        mov(eax, ra);
        lea(ecx, ptr[rax - 1]);
        mov(ra, ecx);
        cmp(eax, 1);
        je(done, T_NEAR);
        emitGoto(d.operand, length, blockEntry, blockStart);
        L(done);
        emitGoto(next, length, blockEntry, blockStart);
        break;
    }
    case Op::Call:
        mov(eax, dword[rbx + spOffset_]);
        lea(ecx, ptr[rax + 1]);
        mov(dword[rbx + spOffset_], ecx);
        and_(eax, 0xF);
        mov(dword[rbx + rax * 4 + stackOffset_], next);
        emitGoto(d.operand, length, blockEntry, blockStart);
        break;
    case Op::Return:
        mov(eax, dword[rbx + spOffset_]);
        sub(eax, 1);
        mov(dword[rbx + spOffset_], eax);
        and_(eax, 0xF);
        mov(eax, dword[rbx + rax * 4 + stackOffset_]);
        and_(eax, 0xFFF);
        emitGotoEax(length);
        break;
    case Op::JumpLink:
        mov(ra, next);
        emitGoto(d.operand, length, blockEntry, blockStart);
        break;
    case Op::JumpRegister:
        mov(eax, ra);
        and_(eax, 0xFFF);
        emitGotoEax(length);
        break;
    case Op::ReturnFromInterrupt:
        mov(eax, reg(15));
        shr(eax, 16);
        and_(eax, 0xFFF);
        mov(byte[rbx + inInterruptOffset_], 0);
        or_(reg(15), 1);
        emitGotoEax(length);
        break;

    default:  // rare or complex: one step of the interpreter
        emitFallback(in, isControlFlow(d.op), length);
        break;
    }
}

}  // namespace hydra::dsp
