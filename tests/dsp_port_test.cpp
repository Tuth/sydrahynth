#include "dsp_core.h"
#include "dsp_math.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

using namespace hydra::dsp;
using Word = std::uint32_t;
struct TestHost final : Host {
    std::map<std::pair<int, Word>, Word> words;
    Word read(int s, Word a) { auto i=words.find({s,a}); return i==words.end() ? a*2654435761u+s : i->second; }
    void write(int s, Word a, Word v) { words[{s,a}]=v; }
    Word ioRead(int, Word a) override { return read(0,a); }
    void ioWrite(int, Word a, Word v) override { write(0,a,v); }
    Word extRead(int, Word a) override { return read(1,a); }
    void extWrite(int, Word a, Word v) override { write(1,a,v); }
    Word portRead(int, Word a) override { return read(2,a); }
    void portWrite(int, Word a, Word v) override { write(2,a,v); }
    Word dataRegRead(int, Word a) override { return read(3,a); }
    void dataRegWrite(int, Word a, Word v) override { write(3,a,v); }
};
[[noreturn]] void fail(unsigned c, unsigned step) {
    std::fprintf(stderr,"DSP differential mismatch: seed case %u, step %u\n",c,step); std::exit(1);
}
int main() {
    std::mt19937 rng(0x57494e36);
#if defined(__SIZEOF_INT128__)
    // Independent exact arithmetic reference, including sums outside int64_t.
    for (unsigned c=0;c<20000;++c) {
        std::array<std::int64_t,5> p;
        __int128 sum=0;
        for(auto& v:p) { v=std::int64_t(std::int32_t(rng()))*std::int32_t(rng()); sum+=v; }
        if(c<128) { p.fill(c&1 ? -(std::int64_t{1}<<62) : (std::int64_t{1}<<62)); sum=__int128(p[0])*5; }
        for(unsigned shift=0;shift<128;++shift) {
            auto v=sum>>shift;
            Word expected=v>0x7fffffff ? 0x7fffffff : v< -__int128(0x80000000u) ? 0x80000000u : Word(v);
            if(biquadResult(p,shift)!=expected) fail(c,shift);
        }
    }
#endif
    // Every template is a valid instruction. Mix inline code, host-call thunks,
    // data-register accesses and interpreter fallbacks in the same JIT block.
    constexpr Word ops[]={0,0x30000012,0x30080013,0x30040014,0x300c0015,
        0x50000005,0x50080007,0x50010123,0x50090124,0x60040003,
        0x70000001,0x70020002,0x70060003,0x70801234,0x70825678,
        0x70861234,0x7088ffff,0x708a0055,0x70c61234,0x70c8aaaa,
        0x80100063,0x80140063,0xb0020005,0xb0030007,0xb0040009,
        0xb005000b,0xb00f0000,0xb00f8000,0xc0120003,0xc0130004,
        0xc0100003,0xc0110003,0xc0140003,0xc0150003,0xc0180003,
        0xc01a0003,0xc01c0003,0xc01d0003,0xd0000063,0xd0020063,
        0xd0060063,0xd0080001,0xd00a0002,0xe0120000,0xe0130000,
        0xe0140000,0xe0150000,0xe0160000,0xe0170000,0xe0180000,
        0xe0190000,0xe00003e0,0xf0140000,0xf0145000,0xf0146000,
        0xf0147000,0xf0020000,0xf0060003,0x10008001};
    for(auto op:ops) if(decode(op).op==Op::Invalid) {std::fprintf(stderr,"Invalid test opcode %08x\n",op);return 1;}
    for(unsigned c=0;c<160;++c) {
        std::array<Word,4096> p{};
        std::array<Word,512> a,b;
        std::vector<Word> sa(DspCore::kSharedWords),sb;
        for(auto& v:a) v=rng(); b=a;
        for(auto& v:sa) v=rng(); sb=sa;
        for(unsigned i=0;i<96;++i) p[i]=ops[rng()%std::size(ops)] | ((rng()%16)<<24);
        // Biquad and AddEightMem have fixed register fields.
        for(auto& op:p) if(decode(op).op==Op::Invalid) op=0;
        p[96]=0x901601ff; // conditional backward branch
        p[97]=0xa0080000; // jump to beginning
        if(c%8==0) {p[0]=0xa0062008;p[8]=0x7106002a;p[9]=0xa00a2000;} // call/return
        TestHost ha,hb;
        DspCore ca(0,ha,{p.data(),a.data(),511,sa.data(),480});
        DspCore cb(0,hb,{p.data(),b.data(),511,sb.data(),480});
        ca.setJit(false);cb.setJit(true);
        for(auto& v:ca.regs()) v=rng(); cb.regs()=ca.regs();
        for(unsigned step=0;step<12;++step) {
            if(step==4) { p[3]=0x72060077;ca.invalidate(3);cb.invalidate(3); }
            if(step==7 && c%4==0) { ca.raiseInterrupt(0);cb.raiseInterrupt(0); }
            const unsigned budget=std::array<unsigned,6>{1,7,31,64,127,193}[step%6];
            if(ca.run(budget)!=cb.run(budget) || ca.regs()!=cb.regs() || ca.pc()!=cb.pc() ||
               ca.pending()!=cb.pending() || ca.inInterrupt()!=cb.inInterrupt() || ca.trapped()!=cb.trapped() ||
               a!=b || sa!=sb || ha.words!=hb.words) fail(c,step);
        }
    }
    std::puts("PASS: exact biquad arithmetic and 160 seeded DSP JIT/interpreter scenarios");
}
