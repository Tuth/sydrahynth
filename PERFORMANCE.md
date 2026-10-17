# Windows x64 Test3: experimental CPU optimization

2026-10-17. Stable fallback: release `win64-test2`. Native Windows Test3 audio and performance validation is pending.

## Final changes

- When the existing audio ring is full and the emulator already waits for the host, tell DSP/effects workers to use their existing atomic waits promptly. Queued work always executes, even when the hint is set. Clear the hint before the next emulation chunk. No extra audio buffer, guest-time skipping, reduced sample rate or reduced DSP instruction budget.
- Keep the original busy-wait budgets during active processing. Four DSP workers remain the default.
- DSP port-write, CPU-write and command map counters are opt-in through `HYDRA_DSP_DIAGNOSTICS=1`. The hydraboot diagnostic CLI explicitly enables them, preserving its reports.
- Capture a worker's starting generation before launching it, preventing a late-starting worker from mistaking its first job for an already-observed generation.

## Measured firmware comparison

Linux x86-64 container on AMD EPYC 9V74, GCC 13.3.0, Release `-O3`; these are not Windows/5900X measurements.
Both use the production emulator, four DSP workers, default 512-frame audio target, 256-frame host pulls at 44.1 kHz, the supplied bank's first patch (DeltaStringer RA), and a three-note held chord after warmup. Each measured interval is 12 seconds at realtime pace; process CPU time sums all threads. Runs were sequential with no build running concurrently.

| Run | Test2 CPU seconds | Test3 CPU seconds | Reduction | Test2 underruns | Test3 underruns |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 41.258861 | 38.384653 | 6.97% | 36 | 32 |
| 2 | 42.467335 | 38.653269 | 8.98% | 67 | 32 |

Combined CPU reduction: about **8.0%**. Test2 used about 3.49 logical CPU equivalents during the measured intervals, Test3 about 3.21. Audio remained finite and nonzero. This shared container had underruns even with the original Test2, so these runs do not certify realtime audio reliability or a particular minimum buffer size. Native Windows A/B testing is necessary.

## Validation

- Exact DSP JIT/interpreter arithmetic test: PASS.
- Windows x64 ABI path exercised on Linux: PASS.
- 1..8 worker startup/restart, exact independent-core register/data/output state, and host-idle hint with pending work: PASS.
- Windows x64 standalone and VST3 Release cross-build: PASS.
- AMD64 binaries, embedded Common Controls manifests, VST3 exports and static-runtime imports: PASS.
- Firmware boot, bank upload, MIDI audio, and flash restore integration: PASS. Note peak 0.08047652; restored note peak 0.07715487, six flash sectors restored.
- Native Windows Test3 GUI/audio/DAW and performance: NOT YET TESTED. Test2's previous user confirmation does not apply automatically to Test3.

## Rejected experiments

An initial worker-counter/cache-line rearrangement plus generic pause changes increased firmware CPU cost. Shortening all worker spin budgets to 128 iterations caused severe underruns. Both were reverted. The final version only changes wait behavior at the existing host-buffer pause. Lowering the default DSP worker count was also rejected: the one-worker configuration could not sustain realtime in this environment.

## Native Windows A/B

Run from the extracted package (no firmware or bank is included):

```powershell
powershell -ExecutionPolicy Bypass -File Scripts/Test-Performance.ps1 -Firmware "C:/your/firmware.dat" -Bank "C:/your/bank.hydra"
```

This runs Test2/Test3 benchmark EXEs at 1/2/4 workers and writes performance-results.txt. Windows CPU time comes from GetProcessTimes (kernel + user across all threads), not std::clock, which has different semantics on Windows. Compare CPU, underruns and listening results together. The GUI products keep four DSP workers; the other counts are diagnostic benchmark options only.

The baseline benchmark uses the unmodified Source/ tree from the exact Test2 ZIP (recorded source 71eb5902874bb6a72832bb7b7c2b3f6166b26fd9; published equivalent tag win64-test2), with the same new firmware_perf.cpp/process_cpu.h host harness linked to that baseline core. To reproduce it, build hydra_core at win64-test2 and link the current firmware_perf target against that library.

Build the current GUI and tests with scripts/build-win64.ps1. Proprietary firmware and banks are never fetched or packaged.
