# Iteratively discover the audio microcode's indirect branch targets:
# run the game, harvest "Unhandled jump target 0x..." from the log, add it to
# extra_indirect_branch_targets in the RSPRecomp toml, regenerate, rebuild, repeat.
# ASCII only - Windows PowerShell 5.1 misparses UTF-8 punctuation without BOM.
param(
    [int]$MaxIterations = 12,
    [int]$ProbeSeconds = 25
)

$repo = "C:\dev\src\github\WaveRace64-Recomp"
$llvm = "C:\dev\src\github\Pilotwings64Recomp\portable-llvm\LLVM-19.1.3-Windows-X64\bin"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$toml = "$repo\recomp\aspMain.us.rev1.toml"
$env:PATH += ";$llvm"
Set-Location $repo

for ($i = 1; $i -le $MaxIterations; $i++) {
    Write-Output "=== iteration $i : probing ($ProbeSeconds s) ==="
    $p = Start-Process -FilePath "$repo\build\WaveRace64Recomp.exe" -WorkingDirectory $repo `
        -RedirectStandardOutput "$repo\run_stdout.log" -RedirectStandardError "$repo\run_stderr.log" -PassThru
    Start-Sleep -Seconds $ProbeSeconds
    if (-not $p.HasExited) { $p | Stop-Process -Force }

    # NOTE: the generated ucode prints this diagnostic via printf -> STDOUT.
    $hit = Select-String -Path "$repo\run_stdout.log", "$repo\run_stderr.log" -Pattern 'Unhandled jump target (0x[0-9A-Fa-f]+) in microcode aspMain' | Select-Object -First 1
    if (-not $hit) {
        $fail = Select-String -Path "$repo\run_stderr.log" -Pattern 'Failed to find function at (0x[0-9A-Fa-f]+)' | Select-Object -First 1
        if ($fail) {
            $addr = $fail.Matches[0].Groups[1].Value
            Write-Output "no ucode issue, but game hit missing function $addr - splitting via CPU loop tooling"
            python "$repo\scripts\find_indirect_targets.py" --split $addr
            if ($LASTEXITCODE -eq 2) { Write-Output "split refused - stopping."; break }
            cmd /c "`"$vcvars`" >nul 2>&1 && cd /d $repo && lib\N64Recomp\build\N64Recomp.exe recomp\waverace64.toml > recomp.log 2>&1 && cmake -S . -B build > configure.log 2>&1 && cmake --build build --config Release -j 12 > build.log 2>&1"
            if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED - stopping."; break }
            Write-Output "rebuilt ok (CPU split)"
            continue
        }
        Write-Output "clean probe - no unhandled jump targets, no missing functions. Done."
        break
    }

    $target = $hit.Matches[0].Groups[1].Value
    Write-Output "adding indirect branch target $target"
    $content = Get-Content $toml -Raw
    if ($content -match [regex]::Escape("$target,")) {
        Write-Output "target already present but still failing - stopping for manual analysis."
        break
    }
    $content = $content -replace '(extra_indirect_branch_targets = \[)', "`$1`n    $target,"
    [IO.File]::WriteAllText($toml, $content)

    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d $repo && lib\N64Recomp\build\RSPRecomp.exe recomp\aspMain.us.rev1.toml > rsprecomp.log 2>&1 && cmake --build build --config Release -j 12 > build.log 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Output "BUILD FAILED - stopping."
        Get-Content "$repo\rsprecomp.log" -Tail 3
        Select-String -Path "$repo\build.log" -Pattern '\): (fatal )?error' | Select-Object -First 3 | ForEach-Object Line
        break
    }
    Write-Output "regenerated + rebuilt ok"
}
Write-Output "=== audio ucode loop finished ==="
