#!/usr/bin/env python3
"""Package a clean, built Windows tree with sources, licenses and provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, default=root/'build/win64')
parser.add_argument('--output', type=Path, default=root/'build/Sydrahynth-Win64-test2.zip')
args = parser.parse_args()
def git(*a):
    return subprocess.check_output(['git','-C',str(root),*a], text=True).strip()
if git('status','--porcelain'):
    raise SystemExit('Commit the source changes before packaging; the binary must match the source snapshot.')
product = args.build/'Sydrahynth_artefacts/Release'
exe = product/'Standalone/Sydrahynth.exe'
vst = product/'VST3/Sydrahynth.vst3'
if not exe.is_file() or not list(vst.rglob('*.vst3')):
    raise SystemExit('Build Sydrahynth_Standalone and Sydrahynth_VST3 first.')
files = { 'Standalone/Sydrahynth.exe': exe }
for p in vst.rglob('*'):
    if p.is_file(): files['VST3/Sydrahynth.vst3/'+p.relative_to(vst).as_posix()] = p
for name in ('emurun','dsp_port_test','firmware_smoke'):
    p=args.build/(name+'.exe')
    if p.exists(): files['Tools/'+p.name]=p
for name in git('ls-files').splitlines(): files['Source/'+name]=root/name
licenses={
    'Sydrahynth':root/'LICENSE',
    'JUCE':root/'external/JUCE/LICENSE.md',
    'Dynarmic':root/'external/dynarmic/LICENSE.txt',
    'Boost':root/'external/boost/LICENSE_1_0.txt',
    'Xbyak':root/'external/dynarmic/externals/xbyak/COPYRIGHT',
}
for name in ('fmt','mcl','robin-map','zydis','zycore'):
    licenses[name]=root/'external/dynarmic/externals'/name/'LICENSE'
# JUCE embeds codec, font and VST3 SDK notices; retain all of them.
for p in (root/'external/JUCE/modules').rglob('*'):
    if p.is_file() and (p.name.lower().startswith(('license','copying','copyright'))):
        files['Licenses/JUCE-components/'+p.relative_to(root/'external/JUCE/modules').as_posix()] = p
import os
if os.getenv('LLVM_MINGW_ROOT'):
    tc=Path(os.environ['LLVM_MINGW_ROOT'])
    licenses['LLVM-MinGW']=tc/'LICENSE.TXT'
    licenses['MinGW-runtime']=tc/'x86_64-w64-mingw32/share/mingw32/COPYING'
for name,p in licenses.items():
    if not p.is_file(): raise SystemExit(f'Missing license: {p}')
    files['Licenses/'+name+'.txt']=p
info={
    'source_revision':git('rev-parse','HEAD'),
    'upstream_base':'c23d935a30e21605299a9d8a79224d865193a447',
    'platform':'Windows x86-64 UCRT; LLVM-MinGW 20261006; Release; static C++ runtime',
    'firmware_included':False,
    'dependencies':{n:git('-C',str(root/'external'/n),'rev-parse','HEAD') for n in ('JUCE','dynarmic')},
    'sha256':{n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in files.items() if n.endswith(('.exe','.vst3'))},
}
readme='''Sydrahynth — Windows x64 test build 2

VST3 + standalone. Hydrasynth Desktop firmware and banks are NOT included.

1. Extract the ZIP. Run Standalone/Sydrahynth.exe, or copy the WHOLE
   VST3/Sydrahynth.vst3 directory into C:/Program Files/Common Files/VST3/.
2. Choose your Hydrasynth_Desktop_Firmware_2.2.0.dat in the firmware selector.
   Load your .hydra bank using Load bank... and select a patch.
3. In standalone, choose your audio/MIDI devices in the audio settings.
   Start with 44.1 kHz / 256 frames, then tune the buffer for your driver.
   The emulator latency selector is 6/12/23/46 ms (default 12 ms).

Standalone and VST3 firmware/preferences files are independent under
%APPDATA%/sydrahynth/. See Source/README.md for exact paths and build steps.

This is a Windows test build. Native Windows audio drivers, GUI and DAW
hosting require testing on an actual Windows machine. See VALIDATION.txt for
what was verified here. No performance measurement from Linux is a Windows
performance guarantee.

Tools/dsp_port_test.exe checks DSP JIT/interpreter equivalence without firmware.
Tools/firmware_smoke.exe firmware.dat bank.hydra checks boot, bank upload, MIDI
sound and flash restore by emulated time (also works on a slow test machine).
Tools/emurun.exe firmware.dat output.wav bank.hydra runs the headless firmware,
MIDI/audio and flash-restore exercise (writes a WAV; no sound device required).

Source/ contains the port source and pinned open-source dependency-fetch/build
scripts. All compatibility patches are included. Firmware/banks remain yours.
License: AGPL-3.0-only; see Licenses/ and Source/THIRD_PARTY.md.
'''
validation=root/'WINDOWS_VALIDATION.md'
if not validation.exists(): raise SystemExit('Record the actual validation in WINDOWS_VALIDATION.md first.')
args.output.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(args.output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for name,p in sorted(files.items()): z.write(p,name)
    z.writestr('README-FIRST.txt',readme)
    z.writestr('BUILD_INFO.json',json.dumps(info,indent=2)+'\n')
    z.writestr('VALIDATION.txt',validation.read_text())
print(args.output)
print(hashlib.sha256(args.output.read_bytes()).hexdigest())
