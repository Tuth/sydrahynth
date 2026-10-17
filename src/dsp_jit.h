#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <xbyak/xbyak.h>

#include "dsp_core.h"

#if defined(HYDRA_TEST_WINDOWS_ABI) && !defined(_WIN32)
#define HYDRA_JIT_HOST_ABI __attribute__((ms_abi))
#else
#define HYDRA_JIT_HOST_ABI
#endif

namespace hydra::dsp {

// Translates a DspCore's program to x86-64, a basic block at a time, with the
// interpreter's exact semantics (DspCore::interpret stays the reference):
//  - The core's registers and state stay in the DspCore; compiled code works on
//    them directly. rbx = core, r12 = instructions executed, r13 = budget,
//    r14 = registers, r15 = data memory.
//  - A block runs only if it fits into the remaining budget (else the
//    interpreter finishes the run), so runs end on the same instruction.
//  - Interrupts: run() enters compiled code only while none is pending, and
//    within a run only the host can raise one (from another thread, when the
//    DSP side runs concurrently); compiled code notices it at the next block
//    boundary. While one is pending, the interpreter steps single instructions
//    as before.
//  - Backward branches report to the idle-loop detection as in the interpreter.
//  - Rare or complex instructions call the interpreter for that one instruction.
//  - Any write to program memory (DspCore::invalidate) drops all translations
//    at the next block boundary.
class DspJit : private Xbyak::CodeGenerator {
public:
    explicit DspJit(DspCore& core);

    // DspCore::run with compiled code.
    std::uint64_t run(std::uint64_t budget);
    void invalidate() { flush_.store(true, std::memory_order_release); }

private:
    enum Exit : std::uint32_t { kNormal, kIdle, kBudget };
    using Enter = std::uint32_t (HYDRA_JIT_HOST_ABI *)(DspCore* core, const void* code, std::uint64_t* executed, std::uint64_t budget);

    struct Instruction {
        std::uint32_t pc;
        Decoded d;
    };

    void flushAll();
    void emitCommon();
    const void* compile(std::uint32_t pc);
    void emitInstruction(const Instruction& in, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    // Control flow out of a block: count the block's instructions and go to `target`.
    void emitGoto(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitGotoEax(std::uint32_t length);
    void emitBackwardBranch(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitFallback(const Instruction& in, bool controlFlow, std::uint32_t length);
    // Data memory with the address in eax (result in eax), and stores of edx.
    void emitDataLoad();
    void emitDataLoad(std::uint32_t address);
    void emitDataStore();
    void emitDataStore(std::uint32_t address);
    void emitIoRead();
    void emitIoWrite();
    void emitIncrementWrites();
    void emitClampS32();  // rax (signed 64-bit) -> eax saturated to 32 bits
    void emitCall(const void* function);

    Xbyak::Address reg(int index) { return dword[r14 + 4 * index]; }

    DspCore& core_;
    std::array<const void*, DspCore::kProgramWords> blocks_{};
    std::atomic<bool> flush_{false};
    Enter enter_ = nullptr;
    const void* dispatch_ = nullptr;  // eax = next pc
    const void* exitNormal_ = nullptr;
    const void* exitIdle_ = nullptr;
    const void* exitCommon_ = nullptr;

    // Offsets of the core's state from the DspCore pointer.
    std::int32_t pcOffset_, pendingOffset_, writesOffset_, spOffset_, stackOffset_, inInterruptOffset_, regsOffset_;
};

}  // namespace hydra::dsp
