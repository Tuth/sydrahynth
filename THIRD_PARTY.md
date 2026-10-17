# Open-source components

Sydrahynth is AGPL-3.0-only (see LICENSE). JUCE is used under its AGPLv3 option.
Hydrasynth firmware and patch banks are user-supplied and are not distributed.

The dependency fetch script pins all of the following. Local compatibility
patches are included in patches/ and applied idempotently.

| Component | Source / revision | License |
| --- | --- | --- |
| Dynarmic (Azahar fork) | https://github.com/azahar-emu/dynarmic/tree/a46601580d5512d324104f985b5f0209dc980ddc | 0BSD |
| JUCE 8.0.15 | https://github.com/juce-framework/JUCE/tree/91ad83ae34a81e0833b1a2b0866f54846370ae53 | AGPLv3 option; embedded components retain their notices |
| Boost 1.86.0 headers | https://archives.boost.io/release/1.86.0/source/ | Boost Software License 1.0 |
| fmt | Dynarmic pinned submodule 7b273fbb541c7c9058800bc613f4291274b2d26e | MIT |
| mcl | Dynarmic pinned submodule 5fc4beaf331037649b10625736b41365defb4f50 | MIT |
| robin-map | Dynarmic pinned submodule 054ec5ad67440fcd65e0497e5a27ef31f53fcc7f | MIT |
| Xbyak | Dynarmic pinned submodule 8c0098b69f95d02dea47efcdd42ab5aa2d03c72d | BSD 3-Clause |
| Zydis | Dynarmic pinned submodule bffbb610cfea643b98e87658b9058382f7522807 | MIT |
| Zycore | Dynarmic pinned submodule 0b2432ced0884fd152b471d97ecf0258ff4d859f | MIT |

Windows binaries are built with LLVM-MinGW UCRT, with libc++/libunwind and the
compiler support runtime linked statically. Their notices accompany the binary
package. Windows system DLLs are provided by Windows.

The Windows port changes DSP generated-code entry/calls for Windows x64 ABI,
provides exact portable biquad accumulation and a portable interpreter dispatch,
bounds CPU/DSP skew at the existing 1 ms chunk boundary, adds Windows build scripts/settings paths, and records dependency compatibility
patches. The DSP fast path remains enabled on x86-64; no extra audio buffer was
introduced by the port.
