# Launches WaveRace64Recomp with the scene/FBP telemetry on and records every
# key press/release (with a timestamp relative to launch) while YOU drive the
# game manually. Purpose: capture the exact human recipe (which keys, how long
# between them) to reach a 2P VS split-screen race, plus the [SCENE]/[FBP]
# stderr telemetry during that race, so the split-screen widescreen work can be
# retraced and automated.
#
# Usage: run it, play into a 2P race, hold a few seconds, then just close the
# game window. The script stops when the game exits and writes:
#   drive_2p\keylog.txt       - timestamped key transitions
#   drive_2p\manual_stderr.log - game [SCENE]/[FBP]/... telemetry
# ASCII only (PS 5.1 safe).
param(
    [string]$OutDir = "$PSScriptRoot\..\drive_2p",
    [string]$Exe = "$PSScriptRoot\..\build\WaveRace64Recomp.exe",
    [int]$MaxMinutes = 8
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$errLog = Join-Path $OutDir 'manual_stderr.log'
$outLog = Join-Path $OutDir 'manual_stdout.log'
$keyLog = Join-Path $OutDir 'keylog.txt'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KeyState {
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
}
'@

# VK -> friendly name for the keys the game/launcher use.
$keys = @{
    0x0D='Enter'; 0x1B='Esc'; 0x20='Space'; 0x10='Shift'; 0x11='Ctrl';
    0x25='Left'; 0x26='Up'; 0x27='Right'; 0x28='Down';
    0x70='F1'; 0x7A='F11';
}
# letters A-Z
for ($v = 0x41; $v -le 0x5A; $v++) { $keys[$v] = [char]$v }
# digits 0-9
for ($v = 0x30; $v -le 0x39; $v++) { $keys[$v] = [string]([char]$v) }

$env:WR64_BORDERS = '0'
$env:WR64_SCENE_DEBUG = '1'
$env:WR64_FBP_DEBUG = '1'
$env:WR64_WINDOW = '1920x800'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $Exe -WorkingDirectory $repoRoot -RedirectStandardError $errLog -RedirectStandardOutput $outLog -PassThru
Write-Host "Launched pid $($proc.Id). Play into a 2P VS race, then close the window."

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$down = @{}
$sb = New-Object System.Text.StringBuilder
"# t(s)  event  key   (launch=0)" | Out-File -FilePath $keyLog -Encoding ascii
$deadline = (Get-Date).AddMinutes($MaxMinutes)

while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    foreach ($vk in $keys.Keys) {
        $isDown = ([KeyState]::GetAsyncKeyState($vk) -band 0x8000) -ne 0
        $was = $down[$vk] -eq $true
        if ($isDown -and -not $was) {
            $down[$vk] = $true
            $line = ('{0,9:F3}  DOWN  {1}' -f ($sw.Elapsed.TotalSeconds), $keys[$vk])
            Add-Content -Path $keyLog -Value $line
        } elseif (-not $isDown -and $was) {
            $down[$vk] = $false
            $line = ('{0,9:F3}  UP    {1}' -f ($sw.Elapsed.TotalSeconds), $keys[$vk])
            Add-Content -Path $keyLog -Value $line
        }
    }
    Start-Sleep -Milliseconds 8
}

if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
Write-Host "Done. keylog: $keyLog ; telemetry: $errLog"
