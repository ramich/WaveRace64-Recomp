# Drives WaveRace64Recomp from boot to the watercraft select menu with
# synthesized keyboard input (X = A button, Enter = Start), then captures
# stderr telemetry and a screenshot. ASCII only (PS 5.1 safe).
param(
    [int]$SettleSeconds = 14,
    [string]$OutDir = "$PSScriptRoot\..\drive_out"
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\WaveRace64Recomp.exe'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$errLog = Join-Path $OutDir 'stderr.log'
$outLog = Join-Path $OutDir 'stdout.log'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KeySend {
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public const uint KEYUP = 0x0002;
    // SDL reads scancodes: send the real scancode, not just the VK.
    public static void Tap(byte vk, int holdMs) {
        byte scan = (byte)MapVirtualKey(vk, 0);
        keybd_event(vk, scan, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(holdMs);
        keybd_event(vk, scan, KEYUP, UIntPtr.Zero);
    }
}
'@

function Save-WindowShot([IntPtr]$hwnd, [string]$path) {
    $rect = New-Object KeySend+RECT
    [KeySend]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { Write-Host 'bad window rect'; return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    [KeySend]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $gfx.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose(); $bmp.Dispose()
    Write-Host "saved $path"
}

$env:WR64_BORDERS = '0'
$env:WR64_SCENE_DEBUG = '1'
$env:WR64_FBP_DEBUG = '1'
$env:WR64_WINDOW = '1920x800'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $exe -WorkingDirectory $repoRoot -RedirectStandardError $errLog -RedirectStandardOutput $outLog -PassThru
Write-Host "Launched pid $($proc.Id), settling $SettleSeconds s..."
Start-Sleep -Seconds $SettleSeconds

if ($proc.HasExited) { Write-Host 'Process exited early!'; exit 1 }
[KeySend]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500

# VK codes: Enter=0x0D, X=0x58
$VK_ENTER = 0x0D; $VK_X = 0x58
$proc.Refresh()
$hwnd = $proc.MainWindowHandle

$steps = @(
    @{ key = $VK_ENTER; name = 'start1' ; wait = 3 },
    @{ key = $VK_ENTER; name = 'start2' ; wait = 3 },
    @{ key = $VK_X;     name = 'a1'     ; wait = 3 },
    @{ key = $VK_X;     name = 'a2'     ; wait = 3 },
    @{ key = $VK_X;     name = 'a3'     ; wait = 4 },
    @{ key = $VK_X;     name = 'a4'     ; wait = 4 }
)
foreach ($s in $steps) {
    Write-Host "Tap $($s.name)"
    [KeySend]::SetForegroundWindow($hwnd) | Out-Null
    [KeySend]::Tap([byte]$s.key, 250)
    Start-Sleep -Seconds $s.wait
    Save-WindowShot $hwnd (Join-Path $OutDir "$($s.name).png")
}

Start-Sleep -Seconds 2
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. Logs in $OutDir"
