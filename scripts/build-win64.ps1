# Run from PowerShell with Python 3.12+, Git, CMake 3.25+ and LLVM-MinGW installed.
# LLVM-MinGW release used for the verified cross-build: 20261006, UCRT x86_64.
param([string]$ToolchainRoot = $env:LLVM_MINGW_ROOT, [int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
if (-not $ToolchainRoot) { throw 'Set LLVM_MINGW_ROOT to the extracted LLVM-MinGW UCRT toolchain.' }
$env:LLVM_MINGW_ROOT = (Resolve-Path $ToolchainRoot).Path
$env:PATH = "$env:LLVM_MINGW_ROOT/bin;$env:PATH"
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    python scripts/fetch-deps.py
    if ($LASTEXITCODE) { throw 'Dependency fetch failed' }
    cmake -S . -B build/win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    cmake --build build/win64 --parallel $Jobs --target Sydrahynth_VST3 Sydrahynth_Standalone dsp_port_test firmware_smoke
    if ($LASTEXITCODE) { throw 'Build failed' }
    ctest --test-dir build/win64 --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed' }
} finally { Pop-Location }
