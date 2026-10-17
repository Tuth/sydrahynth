# Sydrahynth — Windows x64 fork

Windows port of [systemfreund/sydrahynth](https://github.com/systemfreund/sydrahynth), the original emulator by systemfreund. Test2 standalone is confirmed working on Windows (Ryzen 9 5900X); native VST3 DAW validation is pending.

Sydrahynth is a Linux and Windows x64 plugin that emulates ASM's Hydrasynth Desktop. It runs ASM's own
Hydrasynth Desktop firmware on an emulated main board. No firmware is included:
you supply `Hydrasynth_Desktop_Firmware_2.2.0.dat` from ASM's firmware pack.

## Experimental Test3 CPU work

The `perf/win64-test3` branch keeps four DSP workers and the existing audio buffering.
It parks waiting workers promptly only at the existing host-buffer wait, makes costly
DSP map diagnostics opt-in, and fixes worker startup synchronization. Native Windows
Test3 performance and audio validation remain pending. See [PERFORMANCE.md](PERFORMANCE.md).

## Build

    scripts/fetch-deps.sh
    cmake -S . -B build -G Ninja
    cmake --build build

This builds the tools and, with JUCE fetched, the plugin as VST3, LV2 and a
standalone app in `build/Sydrahynth_artefacts/<config>/`. Install by
copying `VST3/Sydrahynth.vst3` to `~/.vst3/` and `LV2/Sydrahynth.lv2` to
`~/.lv2/`.

## Windows x64 build and installation

Windows 10/11 x64 port: VST3 and standalone, LLVM-MinGW UCRT x86_64 toolchain. Use Python
3.12+, Git, CMake 3.25+ and Ninja. With LLVM-MinGW extracted and its `bin` on PATH:

```powershell
$env:LLVM_MINGW_ROOT = 'C:/tools/llvm-mingw'
powershell -ExecutionPolicy Bypass -File scripts/build-win64.ps1
```

The dependency script fetches exact Dynarmic and JUCE commits and verifies the
Boost 1.86.0 archive SHA-256. It preserves unexpected existing checkouts instead
of resetting them. No synthesizer firmware or banks are downloaded or bundled.
The toolchain used for this port is LLVM-MinGW **20261006 UCRT x86_64**; MSVC compilation
has not been validated. A Linux-to-Windows cross-build uses the same commands:

```sh
python3 scripts/fetch-deps.py
export LLVM_MINGW_ROOT=/path/to/llvm-mingw
cmake -S . -B build/win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
cmake --build build/win64 --parallel 4
```

A cross-build also needs native JUCE helper build dependencies (X11 development
headers, freetype/fontconfig and ALSA on Linux). Copy the **whole**
`build/win64/Sydrahynth_artefacts/Release/VST3/Sydrahynth.vst3` directory to
`C:/Program Files/Common Files/VST3/`. The standalone `.exe` is in the adjacent
`Standalone` directory. The static runtime build needs no LLVM-MinGW DLLs.

Choose your own `Hydrasynth_Desktop_Firmware_2.2.0.dat` with the GUI firmware
button. Alternatively place it under `%APPDATA%/sydrahynth/Firmware/` or set
`HYDRA_FIRMWARE` to its full path. Load your `.hydra` bank with **Load bank...**.
Firmware contains no factory patches. Select the audio/MIDI device from the
standalone audio settings; begin with 44.1 kHz and a 256-frame device buffer,
then lower it as your audio driver permits. The existing 6/12/23/46 ms emulator
latency selector remains available.

Standalone preferences use `%APPDATA%/sydrahynth/sydrahynth-standalone.settings`;
plugin preferences use `%APPDATA%/sydrahynth/sydrahynth-vst3.settings`. Firmware paths
and preferences are therefore independent. Instrument flash/banks remain in the
saved plugin state. JUCE's standalone audio settings and saved instrument state
use `%APPDATA%/Sydrahynth/Sydrahynth.settings`, separately from both preference
files. The standalone includes WASAPI and DirectSound audio backends. Paths with non-ASCII
characters are passed to the firmware loader as UTF-8 filesystem paths.

`ctest --test-dir build/win64 --output-on-failure` exercises exact filter
arithmetic and seeded JIT/interpreter comparisons on Windows. On Linux,
`dsp_windows_abi_test` additionally exercises Windows x64 calling conventions
and the portable interpreter dispatch. The Linux signal sampler is disabled on
Windows; use a native profiler there.

`firmware_smoke firmware.dat bank.hydra` exercises boot, an uploaded patch,
finite nonzero MIDI audio, and flash restore. It waits for emulated frames and
uses the production upload queue, so it also works on a slow test machine.
This checks functionality and is not a realtime or audio-driver benchmark.

CPU and DSP now synchronize at the end of the existing 1 ms emulator chunk.
They still overlap within the chunk. This prevents host scheduling from letting
CPU time run arbitrarily ahead of DSP time; the supplied bank could otherwise
produce a near-silent MIDI note in the headless test. No audio buffering is
added. `HYDRA_DSP_SETTLE=0` restores the old unsynchronized behavior for diagnosis;
leave it unset for normal use.

## License

Sydrahynth is free software, licensed under the GNU Affero General Public
License version 3 (SPDX: `AGPL-3.0-only`); see `LICENSE`. It builds on JUCE
(used here under its AGPLv3 option), Dynarmic and Xbyak, each under its own
licence.

The firmware is not part of this project: it is ASM's, and you supply it from
ASM's firmware pack. Hydrasynth is a trademark of Ashun Sound Machines (ASM);
this project is not affiliated with or endorsed by ASM.

## Plugin

- Firmware: choose the `.dat` file in the plugin window (stored in
  `~/.config/sydrahynth/sydrahynth.settings`), or put it into
  `~/.local/share/sydrahynth/`, or set `HYDRA_FIRMWARE`.
- The emulator runs on its own threads, ahead of the host by the latency chosen
  in the editor (6, 12, 23 or 46 ms; default 12 ms, and always at least a host
  block plus about 3 ms); MIDI goes to the instrument's MIDI input, the output
  is resampled from 44.1 kHz to the host rate. The emulator runs at about three
  times real time. What can still make it crackle, once, is firmware code running
  for the first time (its translation by the JIT takes a  few milliseconds), e.g.
  the first note after a patch was sent.
- Host parameters: the instrument's 111 MIDI CCs and an output gain.
- The plugin state holds those parameters and the instrument's flash user
  area (settings and banks), so stored patches survive saving a project.
- Panel view: the emulated displays and lights are captured every 10 ms of
  emulated time and drawn at 60 Hz; only what changed is repainted. SHIFT
  latches on a click and applies to the next button, or is released by another
  click; the computer's Shift key, while the mouse is over the panel, holds SHIFT.
- Patches: the editor's patch browser (shown or hidden with "Patches" in the
  top bar; the window grows or shrinks by its width) lists the five banks (A-E, 128 slots
  each) as the instrument stores them, marks the current patch and follows
  changes made on the panel; clicking a slot selects it (bank select LSB and a
  program change on the MIDI input; reception is switched on for a fresh
  instrument). "Load bank..." sends a `.hydra` bank (or a `.patch`) into the
  shown bank. The patches live in the instrument's flash, which the plugin state
  saves and restores, as does the standalone app after quitting. The firmware
  file holds no factory patches; get the banks from ASM as `.hydra` files. Or
  send a bank or patch as SysEx (e.g. from a librarian).
- The editor shows the instrument's front panel, driven by the emulated
  hardware: the five displays, button LEDs, encoder rings and pad colours.
  Click buttons and pads; drag or scroll encoders, the data encoder and knobs
  (the volume knob is the plugin's output gain).

Code: `src/emulator.*` (`hydra::Emulator`) runs a `hydra::Machine` on its own
threads (audio ring, MIDI queue, panel input and capture, patch upload, flash
capture for the state); `src/front_panel.*` (`hydra::FrontPanel`) turns panel
input into what the boards read and reads the lights back; `plugin/` holds the
JUCE side: `HydraProcessor` (parameters, state, resampling), `HydraEditor`
(status bar), `PanelView` (the front panel, SHIFT handling) and `PatchBrowser`.

`build/emurun firmware.dat out.wav [bank.hydra [seconds]]` drives the same
emulator like a host (real-time pulls, a note, a state restore) and reports
throughput and buffer underruns (`HYDRA_TARGET_FILL` and `HYDRA_BLOCK` set the
buffering and pull size); `build/plugintest_artefacts/*/plugintest
<plugin.vst3> <bank.hydra> <prefix>` loads the built VST3 headless at 48 kHz
and checks that a saved state restores the stored patch (`HYDRA_HOST_BLOCK` sets
its block size).
`HYDRA_SOAK=<seconds>` turns emurun into a panel soak test instead: it runs that
long (unpaced; `HYDRA_SOAK_PACED=1` at real-time pace; `HYDRA_SOAK_HOLD=1` holds
a note), turns an encoder every 5 s, plays notes, and reports panel captures,
display redraws, link frames by type and lost link bytes per 10 s (this found
the panel freeze after 10 min 40 s and the display's frame budget).

## Status

The main ARM firmware boots to its steady-state main loop.

    build/hydraboot Hydrasynth_Desktop_Firmware_2.2.0.dat [cpu ticks] [--note] [--wav file] [--bank file.hydra [--bank-first i] [--bank-slot s]] [--program n] [--midi "hex bytes"] [--setting page.entry=value]

The firmware boots to its main loop with the main DSP block running for real
(8 cores of the custom DSP, verified against the reference), the wave ROM
generated from the firmware, tone generator voices, NOR/SPI flash, ADC, UARTs,
MIDI and the panel MCU (STM32, with its own firmware) linked over SPI. Patches can be uploaded over SysEx (`--bank`), and with a
patch loaded a MIDI note produces a voice at the right pitch at the DSP output
(ports 64/65).

Patches go to any slot of bank A and, with `--setting 5.7=1` (PgmChgRX on), are
selected by program change (`--program`).

Both effects DSPs run, so the stereo output carries the patch's effects
(`--wav file` writes it as a 24-bit WAV).

Speed: with `HYDRA_DSP_ASYNC=1 HYDRA_DSP_THREADS=4` (DSP side and effects on their
own threads, DSP cores on four, as in the plugin) the emulator runs at about
3.2 x real time on a Ryzen 7 5800X (`emurun` measures it, with the buffer target raised for the measurement: at the default 512 frames its 5 ms pulls would cap the reading at ~2.3 x). The limit is the
main thread (main CPU and panel MCU); the DSP thread is about a quarter busy;
`HYDRA_HOST_PROFILE=1` breaks the main thread's time down (JIT, panel, link,
device accesses, JIT exits, interrupts, exception handling), `HYDRA_BLOCK_PROFILE=1`
and `HYDRA_PANEL_BLOCK_PROFILE=1` give exact instruction profiles of the two
firmwares by translated block (slow).
What makes it fast: RAM accessed directly by the JIT (page table), count-down
delay loops in the firmware fast-forwarded (they take over 90% of the main CPU's
time at times), the CPU sleeping while it polls a running ADC conversion
(otherwise about 40% of its time) with conversions taking 20 us so that the
firmware's free-running main loop, which rebuilds the panel's LED and screen
frames every iteration, runs at a hardware-like pace rather than as fast as the
host allows, the panel MCU sleeping while it polls a DMA flag or SysTick and
stopping exactly when it has read a byte from the main CPU (instead of running
in 50-cycle steps), the panel lagging up to 50 us between interactions so that
it runs in longer stretches, the DSP cores compiled to x86-64
(`src/dsp_jit.*`, bit-exact with the interpreter; `HYDRA_DSP_JIT=0` switches it
off) and run in parallel, and the DSP side overlapped with the CPU.
`HYDRA_PROFILE=1` prints
the time per component; `HYDRA_SAMPLE=file` samples the main thread (all threads
with `HYDRA_SAMPLE_ALL=1`) for `tools/symbolize_samples.py`;
`HYDRA_GUEST_PROFILE=1` lists the firmware's hottest code; `build/armdis image
start end` disassembles it.

`--panel "b45 d2 e0:3 w500 s/tmp/screen.pgm"` operates the front panel after
boot (press button 45, turn the data encoder 2 detents and encoder 1 three, wait,
write the displays as an image); `HYDRA_SCREEN=file.pgm` writes the displays at
the end of a run.

Other tools: `panelboot` boots the panel MCU alone, `waverom` writes the generated wave ROM, `dsptest` and
`tools/oracle/` compare components with the reference plugin, `tools/dspdis.py`
disassembles DSP programs.
