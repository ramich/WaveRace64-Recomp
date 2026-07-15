# Crash-driven bring-up loop: run the game, split the function at any
# "Failed to find function at 0x..." address, regenerate, rebuild, repeat.
# Stops when a run survives the probe window, the split is refused (needs
# manual analysis), or the iteration cap is reached.
param(
    [int]$MaxIterations = 10,
    [int]$ProbeSeconds = 60
)

$repo = "C:\dev\src\github\WaveRace64-Recomp"
$llvm = "C:\dev\src\github\Pilotwings64Recomp\portable-llvm\LLVM-19.1.3-Windows-X64\bin"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$env:PATH += ";$llvm"
Set-Location $repo

for ($i = 1; $i -le $MaxIterations; $i++) {
    Write-Output "=== iteration $i : running probe ($ProbeSeconds s) ==="
    $p = Start-Process -FilePath "$repo\build\WaveRace64Recomp.exe" -WorkingDirectory $repo `
        -RedirectStandardOutput "$repo\run_stdout.log" -RedirectStandardError "$repo\run_stderr.log" -PassThru
    Start-Sleep -Seconds $ProbeSeconds
    $exited = $p.HasExited
    if (-not $exited) { $p | Stop-Process -Force }

    $frames = (Select-String -Path "$repo\run_stderr.log" -Pattern 'send_dl: DONE').Count
    $fail = Select-String -Path "$repo\run_stderr.log" -Pattern 'Failed to find function at (0x[0-9A-Fa-f]+)' | Select-Object -First 1

    if (-not $exited) {
        Write-Output "SURVIVED the probe window ($frames frames) - no lookup failures. Stopping loop."
        break
    }
    if (-not $fail) {
        Write-Output "exited (code 0x$($p.ExitCode.ToString('X8'))) after $frames frames but NOT from a function lookup - needs manual analysis. Stopping."
        Get-Content "$repo\run_stderr.log" -Tail 10
        break
    }

    $addr = $fail.Matches[0].Groups[1].Value
    Write-Output "crashed after $frames frames at missing function $addr - splitting"
    python "$repo\scripts\find_indirect_targets.py" --split $addr
    if ($LASTEXITCODE -eq 2) { Write-Output "split refused - manual analysis needed. Stopping."; break }

    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d $repo && lib\N64Recomp\build\N64Recomp.exe recomp\waverace64.toml > recomp.log 2>&1 && cmake -S . -B build > configure.log 2>&1 && cmake --build build --config Release -j 12 > build.log 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Output "BUILD FAILED - stopping."
        Select-String -Path "$repo\build.log" -Pattern 'undefined symbol|\): (fatal )?error' | Select-Object -First 5 | ForEach-Object Line
        break
    }
    Write-Output "rebuilt ok"
}
Write-Output "=== bring-up loop finished ==="
