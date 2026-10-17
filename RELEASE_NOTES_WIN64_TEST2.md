# Sydrahynth Windows x64 Test2 (standalone + VST3)

Windows 10/11 x64 port of **systemfreund/sydrahynth**, the original Hydrasynth Desktop firmware emulator by systemfreund.

## Download and run

Extract the ZIP. Run Standalone/Sydrahynth.exe, or copy the whole VST3/Sydrahynth.vst3 directory into C:/Program Files/Common Files/VST3/. Choose your own Hydrasynth_Desktop_Firmware_2.2.0.dat in the GUI, then load your .hydra bank with Load bank.... Firmware and banks are not included.

## Included changes

- Windows x64 DSP JIT calling convention, register preservation and shadow space.
- Portable exact DSP arithmetic and interpreter dispatch.
- Pinned build dependencies, compatibility patches and reproducible build/package scripts.
- Separate standalone and VST3 settings, Unicode firmware paths.
- CPU/DSP synchronization at the existing 1 ms chunk boundary.
- Common Controls v6 manifests, fixing Test1's TaskDialogIndirect startup failure.

## Validation and limits

The Test2 standalone is confirmed working by the Windows tester on a Ryzen 9 5900X. VST3 compilation and binary checks passed; native DAW validation is still pending, so this is a prerelease. Standalone supports WASAPI/DirectSound; ASIO is not enabled in this build. VST3 can use the host's audio driver.

Linux DSP/reference and Windows-ABI tests passed, as did firmware boot, bank upload, MIDI audio and flash restore checks. See WINDOWS_VALIDATION.md. These checks are not a realtime performance guarantee.

This is the exact tested Test2 ZIP. BUILD_INFO.json records original local revision 71eb5902874bb6a72832bb7b7c2b3f6166b26fd9. The published source was restored from its Source directory; compiled source/build scripts match, with documentation added for publication.

SHA-256: `1627788589dd4eaa4991994cad8195b54635cbb3a8ddbda5907d36d2dc1c2395`

AGPL-3.0-only; third-party notices and source are included. No proprietary firmware or banks. Independent, unofficial project; not affiliated with or endorsed by ASM.
