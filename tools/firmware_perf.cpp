// Host-like benchmark with identical buffering for each worker configuration.
// Usage: firmware_perf firmware.dat bank.hydra threads seconds [paced=1]
#include "emulator.h"
#include "patch_bank.h"
#include "process_cpu.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <thread>
using namespace hydra;
using Clock=std::chrono::steady_clock;
int main(int argc,char** argv) {
    if(argc<5) return 2;
    try {
        Emulator emu;
        Emulator::Config cfg;
        cfg.firmwarePath=argv[1]; cfg.dspThreads=std::atoi(argv[3]);
        cfg.systemSettings={{{5,7},1}};
        cfg.profile=std::getenv("HYDRA_BENCH_PROFILE")!=nullptr;
        const auto deadline=Clock::now()+std::chrono::minutes(5);
        const auto check=[&] {
            if(Clock::now()>deadline || emu.state()==Emulator::State::Failed)
                throw std::runtime_error("firmware timeout/failure: "+emu.status());
        };
        emu.start(cfg);
        while(emu.state()==Emulator::State::Booting) {check(); std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        auto patches=loadPatches(argv[2]); if(patches.empty()) return 3;
        patches.resize(1);patches[0].slot=0;
        emu.uploadPatches(patches,0);
        while(emu.uploading()) {check();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        emu.selectPatch(0,0);
        std::array<float,256> l,r;
        const auto consume=[&](std::size_t count) {
            while(count) {
                check();const auto n=std::min({count,emu.bufferedFrames(),l.size()});
                if(!n) {std::this_thread::sleep_for(std::chrono::microseconds(100));continue;}
                emu.readAudio(l.data(),r.data(),n);count-=n;
            }
        };
        consume(88200);
        const std::uint8_t on[]={0x90,48,100,0x90,55,100,0x90,60,100};
        emu.sendMidi(on,sizeof(on));consume(44100); // exclude JIT warmup
        const bool paced=argc<6 || std::atoi(argv[5]);
        const auto total=std::size_t(std::atoi(argv[4]))*44100;
        const auto underruns=emu.underruns();emu.resetChunkStats();
        const auto p0=emu.profile();
        const auto start=Clock::now();const auto cpu=processCpuSeconds();
        std::size_t done=0;float peak=0;double energy=0;
        while(done<total) {
            check();const auto n=std::min(total-done,l.size());
            if(paced) std::this_thread::sleep_until(start+std::chrono::nanoseconds((done+n)*1000000000ull/44100));
            else if(emu.bufferedFrames()<n) {std::this_thread::sleep_for(std::chrono::microseconds(100));continue;}
            emu.readAudio(l.data(),r.data(),n);done+=n;
            for(std::size_t i=0;i<n;++i) {
                if(!std::isfinite(l[i]) || !std::isfinite(r[i])) return 4;
                peak=std::max({peak,std::abs(l[i]),std::abs(r[i])});energy+=double(l[i])*l[i]+double(r[i])*r[i];
            }
        }
        const double used=processCpuSeconds()-cpu;
        const double wall=std::chrono::duration<double>(Clock::now()-start).count();
        const auto p=emu.profile();
        std::printf("threads=%d paced=%d audio=%d wall=%.6f cpu=%.6f cores=%.3f underruns=%llu peak=%.8f rms=%.8f\n",cfg.dspThreads,paced,std::atoi(argv[4]),wall,used,used/wall,(unsigned long long)(emu.underruns()-underruns),peak,std::sqrt(energy/(total*2)));
        if(cfg.profile) std::printf("profile_ns cpu=%llu dsp=%llu dspWait=%llu workerWait=%llu panel=%llu fx=%llu\n",(unsigned long long)(p.cpu-p0.cpu),(unsigned long long)(p.dspCores-p0.dspCores),(unsigned long long)(p.dspWait-p0.dspWait),(unsigned long long)(p.dspSync-p0.dspSync),(unsigned long long)(p.panel-p0.panel),(unsigned long long)(p.effects-p0.effects));
        return peak>1e-4f ? 0:5;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
