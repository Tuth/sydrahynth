param(
    [Parameter(Mandatory=$true)][string]$Firmware,
    [Parameter(Mandatory=$true)][string]$Bank,
    [ValidateRange(5,120)][int]$Seconds = 10,
    [ValidateRange(1,5)][int]$Repeats = 1
)
$ErrorActionPreference = 'Stop'
$Firmware = (Resolve-Path $Firmware).Path
$Bank = (Resolve-Path $Bank).Path
$Tools = Join-Path $PSScriptRoot '../Tools'
$Output = Join-Path $PSScriptRoot '../performance-results.txt'
"Sydrahynth Test2/Test3 native Windows comparison; process CPU = all threads; identical audio buffering" | Set-Content $Output
for ($repeat=1; $repeat -le $Repeats; $repeat++) {
    foreach ($threads in 1,2,4) {
        foreach ($version in 'Test2','Test3') {
            $exe = Join-Path $Tools "$version/firmware_perf.exe"
            "repeat=$repeat version=$version threads=$threads" | Tee-Object -FilePath $Output -Append
            & $exe $Firmware $Bank $threads $Seconds 1 2>&1 | Tee-Object -FilePath $Output -Append
            if ($LASTEXITCODE) { throw "$version benchmark failed: $LASTEXITCODE" }
        }
    }
}
Write-Host "Results saved to $Output"
