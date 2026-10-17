// Firmware integration test paced by EMULATED frames, suitable for slow CI.
// It is deliberately not an audio-driver or realtime-performance benchmark.
#include "emulator.h"
#include "patch_bank.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using Clock=std::chrono::steady_clock;
using namespace hydra;
void waitFor(Emulator& emu, auto done, int seconds=120) {
    const auto deadline=Clock::now()+std::chrono::seconds(seconds);
    while(!done()) {
        if(emu.state()==Emulator::State::Failed || Clock::now()>deadline)
            throw std::runtime_error("emulator failed or test timed out: "+emu.status());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
float frames(Emulator& emu, std::size_t count) {
    std::array<float,256> left,right;
    float peak=0;
    const auto deadline=Clock::now()+std::chrono::seconds(120);
    while(count) {
        if(Clock::now()>deadline || emu.state()!=Emulator::State::Running)
            throw std::runtime_error("audio generation timed out: "+emu.status());
        if(!emu.bufferedFrames()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        const auto n=emu.readAudio(left.data(),right.data(),std::min({count,emu.bufferedFrames(),left.size()}));
        for(std::size_t i=0;i<n;++i) {
            if(!std::isfinite(left[i]) || !std::isfinite(right[i])) throw std::runtime_error("nonfinite audio");
            peak=std::max({peak,std::abs(left[i]),std::abs(right[i])});
        }
        count-=n;
    }
    return peak;
}
int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"usage: firmware_smoke firmware.dat bank.hydra\n");return 2; }
    try {
        Emulator emu;
        Emulator::Config config;
        config.firmwarePath=argv[1];
        if(const char* n=std::getenv("HYDRA_DSP_THREADS")) config.dspThreads=std::clamp(std::atoi(n),1,8);
        config.systemSettings={{{5,7},1}};
        emu.start(config);
        waitFor(emu,[&]{return emu.state()==Emulator::State::Running;});
        std::puts("Firmware boot: PASS");std::fflush(stdout);
        auto patches=loadPatches(argv[2]);
        if(patches.empty()) throw std::runtime_error("empty bank");
        patches.resize(1);patches[0].slot=0;
        if(!emu.uploadPatches(patches,0)) throw std::runtime_error("patch upload rejected");
        waitFor(emu,[&]{return !emu.uploading();});
        emu.selectPatch(0,0);
        frames(emu,88200);
        const std::uint8_t on[]={0x90,60,100},off[]={0x80,60,0};
        emu.sendMidi(on,3);const auto peak=frames(emu,44100);
        emu.sendMidi(off,3);frames(emu,22050);
        std::printf("Patch '%s': MIDI note peak %.8f\n",patches[0].name.c_str(),peak);std::fflush(stdout);
        if(peak<1e-4f) throw std::runtime_error("silent first note");
        config.flash=emu.captureFlash(10000);
        if(config.flash.empty()) throw std::runtime_error("empty flash capture");
        emu.stop();
        // The processor restores into a fresh emulator, as a new host instance does.
        Emulator restoredEmu;
        restoredEmu.start(config);
        waitFor(restoredEmu,[&]{return restoredEmu.state()==Emulator::State::Running;});
        frames(restoredEmu,44100);
        Emulator::Patches stored;
        std::uint64_t generation=0;
        if(!restoredEmu.readPatches(stored,generation) || stored.names[0]!=patches[0].name)
            throw std::runtime_error("uploaded patch not found in restored bank");
        restoredEmu.selectPatch(0,0);
        frames(restoredEmu,88200);
        restoredEmu.sendMidi(on,3);const auto restored=frames(restoredEmu,88200);
        restoredEmu.sendMidi(off,3);
        std::printf("Flash restore: %zu sectors, MIDI note peak %.8f\n",config.flash.size(),restored);
        if(restored<std::max(1e-4f,peak*0.01f)) throw std::runtime_error("silent restored note");
        std::puts("PASS: firmware, bank upload, MIDI audio, flash restore (not a realtime benchmark)");
    } catch(const std::exception& ex) {std::fprintf(stderr,"FAIL: %s\n",ex.what());return 1;}
}
