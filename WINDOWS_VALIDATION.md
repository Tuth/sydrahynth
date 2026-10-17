# Windows x64 test build 2 — 2026-10-16

Upstream base: systemfreund/sydrahynth c23d935a30e21605299a9d8a79224d865193a447.
The source revision and binary SHA-256 values are recorded in BUILD_INFO.json
in the packaged ZIP.

## Completed

- Full Release cross-build, including standalone GUI EXE, VST3, plugin test host,
  firmware_smoke, DSP tests and all upstream command-line tools: successful.
- LLVM-MinGW 20261006 UCRT x86_64, clang 23.1.3, CMake 4.4, Ninja.
- Both GUI products are PE32+ AMD64. VST3 exports GetPluginFactory, InitDll and
  ExitDll. Imports were inspected: Windows system/UCRT DLLs only; no external
  libc++, libunwind or MinGW runtime DLL is required.
- Linux GCC 13.3 tests: dsp_port_test and dsp_windows_abi_test both passed. Each
  compares the JIT and interpreter in 160 seeded scenarios, including host I/O,
  fallback calls, program invalidation, interrupts and partial budgets. The
  second executes the generated Windows x64 calling convention and portable
  interpreter dispatch on Linux. Portable biquad arithmetic was compared with
  an independent __int128 reference across 20,000 five-product sums and 128
  shift values (2,560,000 comparisons per executable).
- Headless firmware integration, on Linux with default four DSP worker threads
  and no HYDRA_DSP_SETTLE override:

```
Firmware boot: PASS
Patch 'DeltaStringer RA': MIDI note peak 0.08040905
Flash restore: 6 sectors, MIDI note peak 0.07663989
PASS: firmware, bank upload, MIDI audio, flash restore (not a realtime benchmark)
```

The user-supplied Hydrasynth Desktop 2.2.0 firmware and SE bank were used
privately. The first patch was uploaded to A001. A fresh emulator received the
captured flash, the stored patch name was checked, and A001 was selected before
playing the restored note. Both takes were checked for finite, non-silent audio.

## Timing correction

The old unsynchronized Emulator path could let the ARM CPU advance ahead of
DSP sample generation. The functional test sometimes produced only a near-
silent 0.00000286 peak. With synchronization at the end of the existing 1 ms
run chunk, the supplied patch produced audible output before and after flash
restore. The corrected behavior is now the default. Parallel work within each
chunk is retained; the audio ring size/latency choices were not increased.
HYDRA_DSP_SETTLE=0 retains the old behavior only for diagnosis.

## Limits

These Windows binaries have not been executed on native Windows. Wine runtime
verification was blocked because this environment does not permit Wine's
server socket. Windows GUI rendering, physical MIDI/audio drivers, realtime
underruns and VST3 hosting in Cubase/VSTHost remain to be validated on Windows.
This is a test build, not a claim of native Windows performance or exhaustive
hardware accuracy. Standalone audio backends are WASAPI and DirectSound; ASIO
is not enabled in this build. A VST3 host may use its own ASIO driver as usual.

The optional VST3 moduleinfo.json cache is omitted in the cross-build; hosts
can enumerate the exported factory. MSVC builds have not been validated.

## Reproduction

See README.md and scripts/build-win64.ps1. Use scripts/fetch-deps.py to fetch
exact dependency revisions and apply all compatibility patches; a repeated
fetch was checked to be idempotent. Firmware/banks are never fetched or bundled.
On Windows, run Tools/dsp_port_test.exe, then optionally:

```
Tools/firmware_smoke.exe firmware.dat bank.hydra
```

The firmware test advances by emulated frames; it is not an audio-driver or
realtime performance benchmark.

## Test build 2: Windows loader correction

The first native Windows standalone launch failed before opening the GUI:
TaskDialogIndirect entry point not found. Its PE resources contained version
information but no Win32 activation manifest. JUCE's MSVC manifestdependency
pragma is ignored by the LLVM-MinGW GNU-style linker, leaving Common Controls
v5 selected, which does not export this function.

Build 2 embeds a Common Controls v6 dependency as RT_MANIFEST/1 in the standalone
and plugin-test EXEs and RT_MANIFEST/2 in the VST3 DLL. The EXE manifest also
requests asInvoker and declares Windows 10 compatibility. It adds no DLL files
and changes no firmware/audio processing code. Binary resource inspection
verifies the embedded XML, resource IDs and v6 identity. Actual Windows startup
of this correction still needs confirmation on the user's machine.

Microsoft API reference:
https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect
https://learn.microsoft.com/windows/win32/controls/cookbook-overview


## Native Windows confirmation and publication provenance

The tester confirmed that the Test2 standalone starts and plays on Windows (Ryzen 9 5900X). Native VST3/DAW validation has not yet been reported.

The release ZIP is the original tested Test2 package, SHA-256 `1627788589dd4eaa4991994cad8195b54635cbb3a8ddbda5907d36d2dc1c2395`. Its BUILD_INFO.json records original local source revision `71eb5902874bb6a72832bb7b7c2b3f6166b26fd9`. Publication source was restored from that package after the temporary build environment expired. Compiled source and build scripts are identical; publication adds documentation only. No new binary build is claimed.
