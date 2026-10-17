#include "dsp_core.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string_view>

#include "dsp_jit.h"
#include "dsp_math.h"

namespace hydra::dsp {

namespace {

std::int32_t s32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

std::uint32_t saturatingAdd(std::uint32_t a, std::uint32_t b) {
    const auto result = static_cast<std::int64_t>(s32(a)) + s32(b);
    return static_cast<std::uint32_t>(std::clamp(result, std::int64_t{-0x80000000LL}, std::int64_t{0x7FFFFFFF}));
}

std::uint32_t saturatingSub(std::uint32_t a, std::uint32_t b) {
    const auto result = static_cast<std::int64_t>(s32(a)) - s32(b);
    return static_cast<std::uint32_t>(std::clamp(result, std::int64_t{-0x80000000LL}, std::int64_t{0x7FFFFFFF}));
}

std::uint32_t clampToS32(std::int64_t v) {
    return static_cast<std::uint32_t>(v > 0x7FFFFFFF ? 0x7FFFFFFF : v);
}

}  // namespace

Decoded decode(std::uint32_t insn) {
    Decoded d;
    const std::uint32_t major = insn >> 28, minor = insn >> 16 & 0xF, lo = insn & 0xFFFF;
    const std::uint32_t bm = insn >> 16 & 0xFF;  // B and minor together
    d.a = insn >> 24 & 0xF;
    d.b = insn >> 20 & 0xF;
    d.c = insn & 0xF;
    d.operand = lo;
    const auto shift5 = [&] { d.shift = insn >> 5 & 0x1F; };

    switch (major) {
    case 0x0:
        d.op = insn == 0 ? Op::Nop : Op::Invalid;
        break;
    case 0x1:
        if ((insn >> 16) == 0x1000 && (lo & 0x8000)) {
            d.op = Op::AddEightMem;
            d.operand = lo & 0x7FFF;
        }
        break;
    case 0x3:
        if (bm == 0x00) d.op = Op::IoLoad;
        else if (bm == 0x08) d.op = Op::IoStore;
        else if (bm == 0x04) d.op = Op::ExtLoad;
        else if (bm == 0x0C) d.op = Op::ExtStore;
        else if (minor == 0x1) d.op = Op::IoLoadIndexed;
        else if (minor == 0x9) d.op = Op::IoStoreIndexed;
        if (d.op >= Op::IoLoad && d.op <= Op::ExtStore)
            d.b = 0;
        break;
    case 0x5:
        if ((minor & 7) == 0 && d.b != 0)
            break;
        switch (minor) {
        case 0x0: d.op = Op::Load; break;
        case 0x8: d.op = Op::Store; break;
        case 0x1: d.op = Op::LoadIndexed; break;
        case 0x9: d.op = Op::StoreIndexed; break;
        }
        break;
    case 0x6:
        if (bm == 0x04) {
            d.op = Op::MulMem;
            d.operand = lo & 0xFFF;
            d.shift = static_cast<std::uint8_t>(lo >> 12);
            d.b = 0;
        }
        break;
    case 0x7: {
        const auto sext = static_cast<std::uint32_t>(static_cast<std::int16_t>(lo));
        switch (bm) {
        case 0x00: d.op = Op::AddImm; d.operand = sext; break;
        case 0x02: d.op = Op::SubImm; d.operand = sext; break;
        case 0x06: d.op = Op::MovImm; d.operand = sext; break;
        case 0x80: d.op = Op::AddImmLow; break;
        case 0x82: d.op = Op::SubImmLow; break;
        case 0x86: d.op = Op::MovImmLow; break;
        case 0x88: d.op = Op::AndImmLow; break;
        case 0x8A: d.op = Op::OrImmLow; break;
        case 0xC6: d.op = Op::MovImmHigh; break;
        case 0xC8: d.op = Op::AndImmHigh; break;
        }
        break;
    }
    case 0x8:
        if ((minor & 0xB) == 0 && (insn & 0xFC10) == 0) {
            d.op = minor == 4 ? Op::MulAcc : Op::Mul;
            shift5();
        }
        break;
    case 0x9:
        d.offset = static_cast<std::int16_t>(static_cast<std::int32_t>(insn << 23) >> 23);
        switch (minor) {
        case 0x0: d.op = Op::BranchGreater; break;
        case 0x2: d.op = Op::BranchGreaterEqual; break;
        case 0x4:
        case 0x6: {
            const bool equal = minor == 4;
            if (lo <= 0xFFF)
                d.op = equal ? Op::BranchEqual : Op::BranchNotEqual;
            else if (lo >> 12 == 4)
                d.op = equal ? Op::BranchLowEqual : Op::BranchLowNotEqual;
            else if (lo >> 12 == 5)
                d.op = equal ? Op::BranchHighEqualLow : Op::BranchHighNotEqualLow;
            break;
        }
        case 0x8:
        case 0xA:
            if (d.b == 0 && (insn & 0xC000) == 0) {
                d.op = minor == 8 ? Op::BranchBitSet : Op::BranchBitClear;
                d.c = insn >> 9 & 0x1F;
            }
            break;
        }
        break;
    case 0xA:
        d.operand = lo & 0xFFF;
        if (d.b != 0)
            break;
        switch (minor) {
        case 0x0: d.op = Op::JumpIfZero; break;
        case 0x2: d.op = Op::JumpIfNotZero; break;
        case 0x4: d.op = Op::Loop; break;
        case 0x6: d.op = (lo & 0x2000) ? Op::Call : Op::JumpLink; break;
        case 0x8: if (d.a == 0) d.op = Op::Jump; break;
        case 0xA: d.op = (lo & 0x2000) ? Op::Return : Op::JumpRegister; break;
        case 0xC: if (d.a == 0) d.op = Op::ReturnFromInterrupt; break;
        }
        break;
    case 0xB:
        if (d.b != 0)
            break;
        switch (minor) {
        case 0x2: d.op = Op::AddMem; break;
        case 0x3: d.op = Op::AddMemSatStore; break;
        case 0x4: d.op = Op::AddMemSat; break;
        case 0x5: d.op = Op::SubMem; break;
        case 0xF:
            d.op = (lo & 0x8000) ? Op::DelayWrite : Op::DelayRead;
            d.operand = lo & 0x7FFF;
            break;
        }
        break;
    case 0xC:
        if (lo & 0xFFF0)
            break;
        switch (minor) {
        case 0x0: d.op = Op::Add; break;
        case 0x1: d.op = Op::AddSat; break;
        case 0x2: d.op = Op::Sub; break;
        case 0x3: d.op = Op::SubSat; break;
        case 0x4: d.op = Op::And; break;
        case 0x5: d.op = Op::Or; break;
        case 0x8: d.op = Op::Shl; break;
        case 0xA: d.op = Op::Sar; break;
        case 0xD: d.op = Op::Max; break;
        case 0xC: d.op = Op::Min; break;
        }
        break;
    case 0xD:
        if (d.b != 0)
            break;
        switch (minor) {
        case 0x0:
        case 0x2:
        case 0x6:
            if ((insn & 0xFC10) == 0) {
                d.op = minor == 0 ? Op::SarImm : minor == 2 ? Op::ShlImm : Op::ShrImm;
                shift5();
            }
            break;
        case 0x8: d.op = Op::PortRead; break;
        case 0xA: d.op = Op::PortWrite; break;
        }
        break;
    case 0xE:
        if (lo != 0) {
            if ((insn >> 16) == 0xE000 && (insn & 0xFC1F) == 0) {
                d.op = Op::Biquad;
                d.b = 0;
                shift5();
            }
            break;
        }
        d.operand = 0;
        switch (minor) {
        case 0x2: d.op = Op::LoadReg; break;
        case 0x3: d.op = Op::LoadRegInc; break;
        case 0x4: d.op = Op::StoreReg; break;
        case 0x5: d.op = Op::StoreRegInc; break;
        case 0x6: d.op = Op::IoLoadReg; break;
        case 0x7: d.op = Op::IoLoadRegInc; break;
        case 0x8: d.op = Op::IoStoreReg; break;
        case 0x9: d.op = Op::IoStoreRegInc; break;
        }
        break;
    case 0xF:
        if (minor == 4 && (lo == 0 || lo == 0x5000 || lo == 0x6000 || lo == 0x7000)) {
            d.op = lo == 0 ? Op::Mov : lo == 0x5000 ? Op::MovHighToLow : lo == 0x6000 ? Op::MovLowToHigh : Op::MovHigh;
        } else if (d.b == 0 && minor == 2 && lo == d.a) {
            d.op = Op::NegSat;
            d.b = 0;
        } else if (d.b == 0 && minor == 6 && (lo & 0xFFF0) == 0) {
            d.op = Op::AbsSat;
        }
        break;
    default:
        break;
    }
    return d;
}

DspCore::DspCore(int id, Host& host, Memory memory) : id_(id), host_(host), mem_(memory) {
    reset();
    setJit(jitByDefault());
}

DspCore::~DspCore() = default;

bool DspCore::jitByDefault() {
#if (defined(__x86_64__) || defined(_M_X64))
    static const bool enabled = [] {
        const char* setting = std::getenv("HYDRA_DSP_JIT");
        return !setting || std::string_view(setting) != "0";
    }();
    return enabled;
#else
    return false;
#endif
}

void DspCore::setJit(bool enabled) {
#if (defined(__x86_64__) || defined(_M_X64))
    if (enabled && !jit_)
        jit_ = std::make_unique<DspJit>(*this);
    else if (!enabled)
        jit_.reset();
#else
    (void)enabled;
#endif
}

void DspCore::invalidate(std::uint32_t pc) {
    cache_[pc & (kProgramWords - 1)].valid = false;
    if (jit_)
        jit_->invalidate();
}

void DspCore::reset() {
    r_.fill(0);
    pc_ = 0;
    pending_ = 0;
    inInterrupt_ = false;
    trapped_ = false;
    trapMessage_.clear();
    sp_ = 0;
    idleTarget_ = ~0u;
    idleWrites_ = ~0ull;
}

const Decoded& DspCore::fetch(std::uint32_t pc) {
    CacheEntry& entry = cache_[pc];
    if (!entry.valid) [[unlikely]] {
        entry.decoded = decode(mem_.program[pc]);
        entry.valid = true;
    }
    return entry.decoded;
}

bool DspCore::takeInterrupt() {
    if (pending_ == 0 || inInterrupt_ || !(r_[15] & 1))
        return false;
    int line;
    std::uint32_t flag;
    if (pending_ & 2) {
        line = 1;
        flag = 0x100;
    } else if ((pending_ & 4) && (r_[15] & 2)) {
        line = 2;
        flag = 0x20;
    } else {
        return false;
    }
    pending_ &= ~(1u << line);
    ++interruptsTaken_[line];
    inInterrupt_ = true;
    r_[15] = pc_ << 16 | flag | (r_[15] & 0xFFFE);
    pc_ = static_cast<std::uint32_t>(line);
    return true;
}

std::uint32_t DspCore::dataLoad(std::uint32_t address) {
    address &= mem_.dataMask;
    if (address <= 15)
        return r_[address];
    if (address >= mem_.dataLimit) [[unlikely]]
        return host_.dataRegRead(id_, address);
    return mem_.data[address];
}

void DspCore::dataStore(std::uint32_t address, std::uint32_t value) {
    address &= mem_.dataMask;
    if (address <= 15)
        r_[address] = value;
    else if (address >= mem_.dataLimit) [[unlikely]]
        host_.dataRegWrite(id_, address, value);
    else
        mem_.data[address] = value;
}

std::uint32_t DspCore::ioRead(std::uint32_t a) {
    if (mem_.shared && ((a >= 0x8000 && a < 0xFF00) || (a >= 0x10000 && a < 0x1FC00)))
        return mem_.shared[a - 0x8000];
    return host_.ioRead(id_, a);
}

void DspCore::ioWrite(std::uint32_t a, std::uint32_t value) {
    ++writes_;
    if (onIoWrite) [[unlikely]]
        onIoWrite(a, value);
    if (mem_.shared && ((a >= 0x8000 && a < 0xFF00) || (a >= 0x10000 && a < 0x1FC00)))
        mem_.shared[a - 0x8000] = value;
    else
        host_.ioWrite(id_, a, value);
}

bool DspCore::branchIdle(std::uint32_t target) {
    const bool interruptDeliverable = pending_ != 0 && !inInterrupt_ && (r_[15] & 1) &&
                                      ((pending_ & 2) || ((pending_ & 4) && (r_[15] & 2)));
    if (interruptDeliverable)
        return false;
    if (target == idleTarget_ && writes_ == idleWrites_ && r_ == idleRegs_) {
        idleTarget_ = ~0u;
        return true;
    }
    idleTarget_ = target;
    idleWrites_ = writes_;
    idleRegs_ = r_;
    return false;
}

std::uint64_t DspCore::run(std::uint64_t budget) {
    if (budget == 0 || trapped_)
        return 0;
    if (jit_)
        return jit_->run(budget);
    return interpret(budget, budget);
}

std::uint64_t DspCore::interpret(std::uint64_t budget, std::uint64_t limit) {
    if (trapped_)
        return 0;

    // Threaded dispatch: every handler fetches the next instruction and jumps
    // to its handler itself (one indirect branch per handler predicts far
    // better than a shared switch).
#if !defined(HYDRA_PORTABLE_DISPATCH) && (!defined(_MSC_VER) || defined(__clang__))
    static const void* const kHandlers[] = {
        &&op_Nop,
        &&op_Invalid,
        &&op_AddEightMem,
        &&op_IoLoad,
        &&op_IoStore,
        &&op_ExtLoad,
        &&op_ExtStore,
        &&op_IoLoadIndexed,
        &&op_IoStoreIndexed,
        &&op_Load,
        &&op_Store,
        &&op_LoadIndexed,
        &&op_StoreIndexed,
        &&op_AddImm,
        &&op_SubImm,
        &&op_MovImm,
        &&op_AddImmLow,
        &&op_SubImmLow,
        &&op_MovImmLow,
        &&op_AndImmLow,
        &&op_OrImmLow,
        &&op_MovImmHigh,
        &&op_AndImmHigh,
        &&op_BranchGreater,
        &&op_BranchGreaterEqual,
        &&op_BranchEqual,
        &&op_BranchNotEqual,
        &&op_BranchHighEqualLow,
        &&op_BranchHighNotEqualLow,
        &&op_BranchLowEqual,
        &&op_BranchLowNotEqual,
        &&op_BranchBitSet,
        &&op_BranchBitClear,
        &&op_JumpIfZero,
        &&op_JumpIfNotZero,
        &&op_Loop,
        &&op_Call,
        &&op_JumpLink,
        &&op_Jump,
        &&op_Return,
        &&op_JumpRegister,
        &&op_ReturnFromInterrupt,
        &&op_Mul,
        &&op_MulAcc,
        &&op_MulMem,
        &&op_AddMem,
        &&op_AddMemSatStore,
        &&op_AddMemSat,
        &&op_SubMem,
        &&op_DelayRead,
        &&op_DelayWrite,
        &&op_Add,
        &&op_AddSat,
        &&op_Sub,
        &&op_SubSat,
        &&op_And,
        &&op_Or,
        &&op_Shl,
        &&op_Sar,
        &&op_Max,
        &&op_Min,
        &&op_LoadReg,
        &&op_LoadRegInc,
        &&op_StoreReg,
        &&op_StoreRegInc,
        &&op_IoLoadReg,
        &&op_IoLoadRegInc,
        &&op_IoStoreReg,
        &&op_IoStoreRegInc,
        &&op_Biquad,
        &&op_SarImm,
        &&op_ShlImm,
        &&op_ShrImm,
        &&op_PortRead,
        &&op_PortWrite,
        &&op_Mov,
        &&op_MovHighToLow,
        &&op_MovLowToHigh,
        &&op_MovHigh,
        &&op_NegSat,
        &&op_AbsSat,
    };
#define HYDRA_DISPATCH_JUMP() goto *kHandlers[static_cast<int>(dp->op)]
#else
#define HYDRA_DISPATCH_JUMP() goto portable_dispatch
#endif
#if !defined(HYDRA_PORTABLE_DISPATCH) && (!defined(_MSC_VER) || defined(__clang__))
    static_assert(std::size(kHandlers) == static_cast<std::size_t>(Op::AbsSat) + 1);
#endif

    std::uint64_t executed = 0;
    std::uint32_t pc = 0, next = 0, rb = 0;
    std::uint32_t* pa = nullptr;
    const Decoded* dp = nullptr;

    // Conditional relative branch with idle-loop detection on backward targets.
    const auto branch = [&](bool taken) {
        if (!taken)
            return false;
        next = (pc + static_cast<std::uint32_t>(dp->offset) + 1) & 0xFFF;
        pc_ = next;
        return dp->offset < 0 && branchIdle(next);
    };
    // Absolute jump; idle detection only when enabled and backward.
    const auto jump = [&](std::uint32_t target) {
        next = target & 0xFFF;
        pc_ = next;
        return jumpIdleDetection_ && target <= pc && branchIdle(next);
    };

    // Interrupts are taken between instructions, but not after the last
    // instruction of a run: the next run() takes them first.
#define DISPATCH()                                   \
    do {                                             \
        if (executed >= limit)                       \
            return executed;                         \
        if (pending_ != 0) [[unlikely]]              \
            takeInterrupt();                         \
        pc = pc_;                                    \
        dp = &fetch(pc);                             \
        next = (pc + 1) & 0xFFF;                     \
        pa = &r_[dp->a];                             \
        rb = r_[dp->b];                              \
        ++executed;                                  \
        HYDRA_DISPATCH_JUMP();                      \
    } while (0)
#define NEXT()      \
    do {            \
        pc_ = next; \
        DISPATCH(); \
    } while (0)

    DISPATCH();
#if defined(HYDRA_PORTABLE_DISPATCH) || (defined(_MSC_VER) && !defined(__clang__))
portable_dispatch:
    switch(dp->op) {
    case Op::Nop: goto op_Nop;
    case Op::Invalid: goto op_Invalid;
    case Op::AddEightMem: goto op_AddEightMem;
    case Op::IoLoad: goto op_IoLoad;
    case Op::IoStore: goto op_IoStore;
    case Op::ExtLoad: goto op_ExtLoad;
    case Op::ExtStore: goto op_ExtStore;
    case Op::IoLoadIndexed: goto op_IoLoadIndexed;
    case Op::IoStoreIndexed: goto op_IoStoreIndexed;
    case Op::Load: goto op_Load;
    case Op::Store: goto op_Store;
    case Op::LoadIndexed: goto op_LoadIndexed;
    case Op::StoreIndexed: goto op_StoreIndexed;
    case Op::AddImm: goto op_AddImm;
    case Op::SubImm: goto op_SubImm;
    case Op::MovImm: goto op_MovImm;
    case Op::AddImmLow: goto op_AddImmLow;
    case Op::SubImmLow: goto op_SubImmLow;
    case Op::MovImmLow: goto op_MovImmLow;
    case Op::AndImmLow: goto op_AndImmLow;
    case Op::OrImmLow: goto op_OrImmLow;
    case Op::MovImmHigh: goto op_MovImmHigh;
    case Op::AndImmHigh: goto op_AndImmHigh;
    case Op::BranchGreater: goto op_BranchGreater;
    case Op::BranchGreaterEqual: goto op_BranchGreaterEqual;
    case Op::BranchEqual: goto op_BranchEqual;
    case Op::BranchNotEqual: goto op_BranchNotEqual;
    case Op::BranchHighEqualLow: goto op_BranchHighEqualLow;
    case Op::BranchHighNotEqualLow: goto op_BranchHighNotEqualLow;
    case Op::BranchLowEqual: goto op_BranchLowEqual;
    case Op::BranchLowNotEqual: goto op_BranchLowNotEqual;
    case Op::BranchBitSet: goto op_BranchBitSet;
    case Op::BranchBitClear: goto op_BranchBitClear;
    case Op::JumpIfZero: goto op_JumpIfZero;
    case Op::JumpIfNotZero: goto op_JumpIfNotZero;
    case Op::Loop: goto op_Loop;
    case Op::Call: goto op_Call;
    case Op::JumpLink: goto op_JumpLink;
    case Op::Jump: goto op_Jump;
    case Op::Return: goto op_Return;
    case Op::JumpRegister: goto op_JumpRegister;
    case Op::ReturnFromInterrupt: goto op_ReturnFromInterrupt;
    case Op::Mul: goto op_Mul;
    case Op::MulAcc: goto op_MulAcc;
    case Op::MulMem: goto op_MulMem;
    case Op::AddMem: goto op_AddMem;
    case Op::AddMemSatStore: goto op_AddMemSatStore;
    case Op::AddMemSat: goto op_AddMemSat;
    case Op::SubMem: goto op_SubMem;
    case Op::DelayRead: goto op_DelayRead;
    case Op::DelayWrite: goto op_DelayWrite;
    case Op::Add: goto op_Add;
    case Op::AddSat: goto op_AddSat;
    case Op::Sub: goto op_Sub;
    case Op::SubSat: goto op_SubSat;
    case Op::And: goto op_And;
    case Op::Or: goto op_Or;
    case Op::Shl: goto op_Shl;
    case Op::Sar: goto op_Sar;
    case Op::Max: goto op_Max;
    case Op::Min: goto op_Min;
    case Op::LoadReg: goto op_LoadReg;
    case Op::LoadRegInc: goto op_LoadRegInc;
    case Op::StoreReg: goto op_StoreReg;
    case Op::StoreRegInc: goto op_StoreRegInc;
    case Op::IoLoadReg: goto op_IoLoadReg;
    case Op::IoLoadRegInc: goto op_IoLoadRegInc;
    case Op::IoStoreReg: goto op_IoStoreReg;
    case Op::IoStoreRegInc: goto op_IoStoreRegInc;
    case Op::Biquad: goto op_Biquad;
    case Op::SarImm: goto op_SarImm;
    case Op::ShlImm: goto op_ShlImm;
    case Op::ShrImm: goto op_ShrImm;
    case Op::PortRead: goto op_PortRead;
    case Op::PortWrite: goto op_PortWrite;
    case Op::Mov: goto op_Mov;
    case Op::MovHighToLow: goto op_MovHighToLow;
    case Op::MovLowToHigh: goto op_MovLowToHigh;
    case Op::MovHigh: goto op_MovHigh;
    case Op::NegSat: goto op_NegSat;
    case Op::AbsSat: goto op_AbsSat;
    default: goto op_Invalid;
    }
#endif
        op_Nop: NEXT();
        op_Invalid:
            trapped_ = true;
            trapMessage_ = std::format("unknown {:04x} {:04x} at {:03x}", mem_.program[pc] >> 16, mem_.program[pc] & 0xFFFF, pc);
            return executed;
        op_AddEightMem:
            ++writes_;
            dataStore(dp->operand, dataLoad(dp->operand) + 8);
            NEXT();
        op_IoLoad: (*pa) = ioRead(dp->operand & 0x1FFFF); NEXT();
        op_IoStore: ioWrite(dp->operand & 0x1FFFF, (*pa)); NEXT();
        op_ExtLoad: (*pa) = host_.extRead(id_, dp->operand); NEXT();
        op_ExtStore:
            ++writes_;
            host_.extWrite(id_, dp->operand, (*pa));
            NEXT();
        op_IoLoadIndexed: (*pa) = ioRead((rb + dp->operand) & 0xFFFF); NEXT();
        op_IoStoreIndexed: ioWrite((rb + dp->operand) & 0xFFFF, (*pa)); NEXT();
        op_Load: (*pa) = dataLoad(dp->operand); NEXT();
        op_Store:
            ++writes_;
            dataStore(dp->operand, (*pa));
            NEXT();
        op_LoadIndexed: (*pa) = dataLoad(rb + dp->operand); NEXT();
        op_StoreIndexed:
            ++writes_;
            dataStore(rb + dp->operand, (*pa));
            NEXT();
        op_AddImm: (*pa) += dp->operand; NEXT();
        op_SubImm: (*pa) -= dp->operand; NEXT();
        op_MovImm: (*pa) = dp->operand; NEXT();
        op_AddImmLow: (*pa) = ((*pa) & 0xFFFF0000) | (((*pa) + dp->operand) & 0xFFFF); NEXT();
        op_SubImmLow: (*pa) = ((*pa) & 0xFFFF0000) | (((*pa) - dp->operand) & 0xFFFF); NEXT();
        op_MovImmLow: (*pa) = ((*pa) & 0xFFFF0000) | dp->operand; NEXT();
        op_AndImmLow: (*pa) &= dp->operand | 0xFFFF0000; NEXT();
        op_OrImmLow: (*pa) |= dp->operand; NEXT();
        op_MovImmHigh: (*pa) = ((*pa) & 0xFFFF) | dp->operand << 16; NEXT();
        op_AndImmHigh: (*pa) &= dp->operand << 16 | 0xFFFF; NEXT();
        op_BranchGreater: if (branch(s32((*pa)) > s32(rb))) return budget; NEXT();
        op_BranchGreaterEqual: if (branch(s32((*pa)) >= s32(rb))) return budget; NEXT();
        op_BranchEqual: if (branch((*pa) == rb)) return budget; NEXT();
        op_BranchNotEqual: if (branch((*pa) != rb)) return budget; NEXT();
        op_BranchHighEqualLow: if (branch(((*pa) >> 16) == (rb & 0xFFFF))) return budget; NEXT();
        op_BranchHighNotEqualLow: if (branch(((*pa) >> 16) != (rb & 0xFFFF))) return budget; NEXT();
        op_BranchLowEqual: if (branch(((*pa) & 0xFFFF) == (rb & 0xFFFF))) return budget; NEXT();
        op_BranchLowNotEqual: if (branch(((*pa) & 0xFFFF) != (rb & 0xFFFF))) return budget; NEXT();
        op_BranchBitSet: if (branch((*pa) >> dp->c & 1)) return budget; NEXT();
        op_BranchBitClear: if (branch(!((*pa) >> dp->c & 1))) return budget; NEXT();
        op_JumpIfZero:
            if ((*pa) == 0 && jump(dp->operand))
                return budget;
            NEXT();
        op_JumpIfNotZero:
            if ((*pa) != 0 && jump(dp->operand))
                return budget;
            NEXT();
        op_Loop:
            if ((*pa)-- != 1)
                next = dp->operand;
            NEXT();
        op_Call:
            stack_[sp_++ & 0xF] = next;
            next = dp->operand;
            NEXT();
        op_JumpLink:
            (*pa) = next;
            next = dp->operand;
            NEXT();
        op_Jump:
            if (jump(dp->operand))
                return budget;
            NEXT();
        op_Return:
            --sp_;
            next = stack_[sp_ & 0xF] & 0xFFF;
            NEXT();
        op_JumpRegister: next = (*pa) & 0xFFF; NEXT();
        op_ReturnFromInterrupt:
            next = r_[15] >> 16 & 0xFFF;
            inInterrupt_ = false;
            r_[15] |= 1;
            NEXT();
        op_Mul:
            r_[dp->c] = static_cast<std::uint32_t>((static_cast<std::int64_t>(s32((*pa))) * s32(rb)) >> dp->shift);
            NEXT();
        op_MulAcc:
            r_[dp->c] += static_cast<std::uint32_t>((static_cast<std::int64_t>(s32((*pa))) * s32(rb)) >> dp->shift);
            NEXT();
        op_MulMem:
            (*pa) = static_cast<std::uint32_t>((static_cast<std::int64_t>(s32((*pa))) * s32(dataLoad(dp->operand))) >> dp->shift);
            NEXT();
        op_AddMem: (*pa) += dataLoad(dp->operand); NEXT();
        op_AddMemSatStore:
            ++writes_;
            dataStore(dp->operand, saturatingAdd((*pa), dataLoad(dp->operand)));
            NEXT();
        op_AddMemSat: (*pa) = saturatingAdd((*pa), dataLoad(dp->operand)); NEXT();
        op_SubMem: (*pa) -= dataLoad(dp->operand); NEXT();
        op_DelayRead:
        op_DelayWrite: {
            const std::uint32_t pointer = dataLoad(dp->operand);
            if (dp->op == Op::DelayRead)
                (*pa) = ioRead(pointer & 0x1FFFF);
            else
                ioWrite(pointer & 0x1FFFF, (*pa));
            const std::uint32_t advanced = pointer < dataLoad(dp->operand + 1) ? pointer + 1 : dataLoad(dp->operand + 2);
            ++writes_;
            dataStore(dp->operand, advanced);
            NEXT();
        }
        op_Add: r_[dp->c] = (*pa) + rb; NEXT();
        op_AddSat: r_[dp->c] = saturatingAdd((*pa), rb); NEXT();
        op_Sub: r_[dp->c] = (*pa) - rb; NEXT();
        op_SubSat: r_[dp->c] = saturatingSub((*pa), rb); NEXT();
        op_And: r_[dp->c] = (*pa) & rb; NEXT();
        op_Or: r_[dp->c] = (*pa) | rb; NEXT();
        op_Shl: r_[dp->c] = (*pa) << (rb & 31); NEXT();
        op_Sar: r_[dp->c] = static_cast<std::uint32_t>(s32((*pa)) >> (rb & 31)); NEXT();
        op_Max: r_[dp->c] = s32((*pa)) > s32(rb) ? (*pa) : rb; NEXT();
        op_Min: r_[dp->c] = s32((*pa)) < s32(rb) ? (*pa) : rb; NEXT();
        op_LoadReg: (*pa) = dataLoad(rb); NEXT();
        op_LoadRegInc: {
            const std::uint32_t address = r_[dp->b]++;
            r_[dp->a] = dataLoad(address);
            NEXT();
        }
        op_StoreReg:
            ++writes_;
            dataStore((*pa), rb);
            NEXT();
        op_StoreRegInc: {
            const std::uint32_t address = (*pa)++;
            ++writes_;
            dataStore(address, r_[dp->b]);
            NEXT();
        }
        op_IoLoadReg: (*pa) = ioRead(rb & 0x1FFFF); NEXT();
        op_IoLoadRegInc: {
            const std::uint32_t address = r_[dp->b]++;
            r_[dp->a] = ioRead(address & 0x1FFFF);
            NEXT();
        }
        op_IoStoreReg: ioWrite((*pa) & 0x1FFFF, rb); NEXT();
        op_IoStoreRegInc: {
            const std::uint32_t address = (*pa)++;
            ioWrite(address & 0x1FFFF, r_[dp->b]);
            NEXT();
        }
        op_Biquad:
            // Direct-form-I biquad over two consecutive samples. r10, r9, r8:
            // feed-forward coefficients; r7, r6: feedback; r5, r4: x[n-1], x[n-2];
            // r3, r2: y[n-1], y[n-2]; r11: sample pointer (read, filtered, written back).
            for (int i = 0; i < 2; ++i) {
                const std::uint32_t x = dataLoad(r_[11]);
                const auto y = biquadResult({
                    static_cast<std::int64_t>(s32(x)) * s32(r_[10]),
                    static_cast<std::int64_t>(s32(r_[9])) * s32(r_[5]),
                    static_cast<std::int64_t>(s32(r_[8])) * s32(r_[4]),
                    static_cast<std::int64_t>(s32(r_[7])) * s32(r_[3]),
                    static_cast<std::int64_t>(s32(r_[6])) * s32(r_[2])}, dp->shift);
                r_[4] = r_[5];
                r_[5] = x;
                r_[2] = r_[3];
                r_[3] = y;
                const std::uint32_t address = r_[11]++;
                ++writes_;
                dataStore(address, r_[3]);
            }
            NEXT();
        op_SarImm: r_[dp->c] = static_cast<std::uint32_t>(s32((*pa)) >> dp->shift); NEXT();
        op_ShlImm: r_[dp->c] = (*pa) << dp->shift; NEXT();
        op_ShrImm: r_[dp->c] = (*pa) >> dp->shift; NEXT();
        op_PortRead: (*pa) = host_.portRead(id_, dp->operand); NEXT();
        op_PortWrite:
            ++writes_;
            host_.portWrite(id_, dp->operand, (*pa));
            NEXT();
        op_Mov: (*pa) = rb; NEXT();
        op_MovHighToLow: (*pa) = rb >> 16; NEXT();
        op_MovLowToHigh: (*pa) = rb << 16; NEXT();
        op_MovHigh: (*pa) = rb & 0xFFFF0000; NEXT();
        op_NegSat: (*pa) = clampToS32(-static_cast<std::int64_t>(s32((*pa)))); NEXT();
        op_AbsSat: {
            const std::int64_t v = s32((*pa));
            r_[dp->c] = clampToS32(v < 0 ? -v : v);
            NEXT();
        }
#undef HYDRA_DISPATCH_JUMP
#undef NEXT
#undef DISPATCH
}

}  // namespace hydra::dsp
