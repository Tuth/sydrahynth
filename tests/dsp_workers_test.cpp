#include "dsp_block.h"
#include <cstdio>

static void program(hydra::DspBlock& dsp) {
    for(int c=0;c<8;++c) {
        dsp.write(c*0x4000,0x70000001,4); // add immediate
        dsp.write(c*0x4000+4,0xd00a0000u+c,4); // private output port
        dsp.write(c*0x4000+8,0xa0080000,4); // jump to beginning
    }
    dsp.setRunMask(255);
}
int main() {
    hydra::DspBlock serial,workers;
    program(serial);program(workers);
    for(unsigned step=1;step<=80;++step) {
        workers.setThreads(1+(step%8)); // deliberately no startup sleep
        const auto sample=step*17;
        serial.advanceToSample(sample);workers.advanceToSample(sample);
        if(serial.instructionsExecuted()!=workers.instructionsExecuted()) return 1;
        for(int c=0;c<8;++c) {
            if(serial.core(c).regs()!=workers.core(c).regs() ||
               serial.privateData(c)!=workers.privateData(c) ||
               serial.ports()[c]!=workers.ports()[c] ||
               serial.core(c).pc()!=workers.core(c).pc()) return 2;
        }
    }
    if(!workers.portWrites_[0].empty()) return 3;
    workers.diagnostics=true;
    workers.advanceToSample(workers.sample()+1);
    if(workers.portWrites_[0].empty()) return 4;
    std::puts("PASS: 1..8 workers, immediate startup/restart, exact independent-core state, optional counters");
}
