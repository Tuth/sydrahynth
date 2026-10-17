// Test driver for the DSP core, used to compare it against the reference
// implementation (see tools/oracle/). Binary little-endian I/O on stdin/stdout.
//
//   dsptest decode   u32 instructions in; per instruction out:
//                    u8 op, a, b, c, shift, pad[3]; u32 operand; i32 offset
//   dsptest run      test cases in (see readCase), results out (see writeResult)

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "dsp_core.h"

using namespace hydra::dsp;

namespace {

bool readWords(std::uint32_t* out, std::size_t count) {
    return std::fread(out, 4, count, stdin) == count;
}

void writeWords(const std::uint32_t* in, std::size_t count) {
    std::fwrite(in, 4, count, stdout);
}

// Deterministic contents for memory that is not explicitly initialised.
std::uint32_t initialValue(std::uint32_t space, std::uint32_t address) {
    return address * 2654435761u + space * 0x9E3779B9u;
}

// Host spaces start out with initialValue() and record every write.
class TestHost : public Host {
public:
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> writes;  // (space, address) -> value

    std::uint32_t ioRead(int, std::uint32_t a) override { return get(0, a); }
    void ioWrite(int, std::uint32_t a, std::uint32_t v) override { writes[{0, a}] = v; }
    std::uint32_t extRead(int, std::uint32_t a) override { return get(1, a); }
    void extWrite(int, std::uint32_t a, std::uint32_t v) override { writes[{1, a}] = v; }
    std::uint32_t portRead(int, std::uint32_t a) override { return get(2, a); }
    void portWrite(int, std::uint32_t a, std::uint32_t v) override { writes[{2, a}] = v; }

private:
    std::uint32_t get(std::uint32_t space, std::uint32_t a) {
        const auto it = writes.find({space, a});
        return it != writes.end() ? it->second : initialValue(space, a);
    }
};

int decodeMode() {
    std::uint32_t insn;
    while (readWords(&insn, 1)) {
        const Decoded d = decode(insn);
        const std::uint8_t head[8] = {static_cast<std::uint8_t>(d.op), d.a, d.b, d.c, d.shift, 0, 0, 0};
        const std::int32_t offset = d.offset;
        std::fwrite(head, 1, 8, stdout);
        writeWords(&d.operand, 1);
        std::fwrite(&offset, 4, 1, stdout);
    }
    return 0;
}

// Case layout (u32): budget, regs[16], pc, pending, program[4096], data[512].
// Shared memory starts as initialValue(3, index).
// Result layout (u32): executed, regs[16], pc, pending, inInterrupt, trapped,
// data[512], count of changed shared words then (index, value) pairs,
// count of host writes then (space, address, value) triples.
int runMode() {
    std::vector<std::uint32_t> program(DspCore::kProgramWords), data(512), shared(DspCore::kSharedWords);
    for (;;) {
        std::uint32_t budget, regs[16], pc, pending;
        if (!readWords(&budget, 1))
            return 0;
        if (!readWords(regs, 16) || !readWords(&pc, 1) || !readWords(&pending, 1) ||
            !readWords(program.data(), program.size()) || !readWords(data.data(), data.size()))
            return 1;
        for (std::uint32_t i = 0; i < shared.size(); ++i)
            shared[i] = initialValue(3, i);

        TestHost host;
        DspCore core(0, host, {program.data(), data.data(), 0x1FF, shared.data()});
        std::memcpy(core.regs().data(), regs, sizeof regs);
        core.setPc(pc);
        for (int line = 0; line < 32; ++line)
            if (pending >> line & 1)
                core.raiseInterrupt(line);

        const std::uint32_t executed = static_cast<std::uint32_t>(core.run(budget));
        const std::uint32_t head[4] = {executed};
        writeWords(head, 1);
        writeWords(core.regs().data(), 16);
        const std::uint32_t status[4] = {core.pc(), core.pending(), core.inInterrupt(), core.trapped()};
        writeWords(status, 4);
        writeWords(data.data(), data.size());

        std::vector<std::uint32_t> changed;
        for (std::uint32_t i = 0; i < shared.size(); ++i)
            if (shared[i] != initialValue(3, i)) {
                changed.push_back(i);
                changed.push_back(shared[i]);
            }
        const std::uint32_t changedCount = static_cast<std::uint32_t>(changed.size() / 2);
        writeWords(&changedCount, 1);
        writeWords(changed.data(), changed.size());

        const std::uint32_t writeCount = static_cast<std::uint32_t>(host.writes.size());
        writeWords(&writeCount, 1);
        for (const auto& [key, value] : host.writes) {
            const std::uint32_t triple[3] = {key.first, key.second, value};
            writeWords(triple, 3);
        }
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s decode|run\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "decode") == 0)
        return decodeMode();
    if (std::strcmp(argv[1], "run") == 0)
        return runMode();
    return 2;
}
