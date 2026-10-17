// Synthetic DSP scheduling benchmark, independent of proprietary firmware.
// Reports process CPU time separately from wall time; no realtime claim.
#include "dsp_block.h"
#include "process_cpu.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

int main(int argc, char** argv) {
    const int threads = argc > 1 ? std::atoi(argv[1]) : 4;
    const bool active = argc > 2 && std::atoi(argv[2]);
    hydra::DspBlock dsp;
    for (int c=0;c<8;++c) {
        // Independent cores: arithmetic + a private output port, no RAM races.
        for (unsigned i=0;i<63;++i)
            dsp.write(c*0x4000+i*4, active ? 0x70000001 : 0, 4);
        dsp.write(c*0x4000+63*4, active ? 0xd00a0000u+c : 0, 4);
        dsp.write(c*0x4000+64*4, 0xa0080000, 4);
    }
    dsp.setRunMask(255);
    dsp.setThreads(threads);
    // Baseline workers initialize their observed generation on startup.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    dsp.advanceToSample(256); // warm JIT
    const auto start=std::chrono::steady_clock::now();
    const auto cpu=processCpuSeconds();
    dsp.advanceToSample(256+44100);
    const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const double work=processCpuSeconds()-cpu;
    std::printf("threads=%d active=%d wall=%.6f cpu=%.6f instructions=%llu out=%u\n",
        threads,active,wall,work,(unsigned long long)dsp.instructionsExecuted(),dsp.ports()[0]);
}
