# One-off: functional test of the new config Button type. Opens Settings ->
# Enhancements, bumps the FOV slider, presses "Reset to 45" and screenshots
# before/after. ASCII only (PS 5.1 safe).
param(
    [string]$OutDir = "$PSScriptRoot\..\drive_btn",
    [string]$Exe = "$PSScriptRoot\..\build\WaveRace64Recomp.exe"
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KeySend4 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    const uint WM_KEYDOWN = 0x0100; const uint WM_KEYUP = 0x0101; const uint EXT = 0x01000000;
    static bool IsExt(byte vk) { return vk >= 0x21 && vk <= 0x2E; }
    public static void Tap(IntPtr hwnd, byte vk, int holdMs) {
        byte scan = (byte)MapVirtualKey(vk, 0);
        uint lp = 1u | ((uint)scan << 16); if (IsExt(vk)) lp |= EXT;
        PostMessage(hwnd, WM_KEYDOWN, (IntPtr)vk, (IntPtr)(long)lp);
        System.Threading.Thread.Sleep(holdMs);
        uint lpu = lp | (1u << 30) | (1u << 31);
        PostMessage(hwnd, WM_KEYUP, (IntPtr)vk, (IntPtr)(long)lpu);
    }
}
'@
function Save-Shot([IntPtr]$hwnd, [string]$path) {
    $rect = New-Object KeySend4+RECT
    [KeySend4]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc(); [KeySend4]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $gfx.ReleaseHdc($hdc); $gfx.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "saved $path"
}

$env:WR64_WINDOW = '1920x800'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $Exe -WorkingDirectory $repoRoot -RedirectStandardError (Join-Path $OutDir 'stderr.log') -RedirectStandardOutput (Join-Path $OutDir 'stdout.log') -PassThru
Start-Sleep -Seconds 14
$proc.Refresh(); $hwnd = $proc.MainWindowHandle
[KeySend4]::SetCursorPos(0, 0) | Out-Null

$E = 0x0D; $D = 0x28; $R = 0x27
function T([int]$vk, [int]$after = 500) { [KeySend4]::Tap($hwnd, [byte]$vk, 150); Start-Sleep -Milliseconds $after }

# Launcher -> Settings
T $D; T $D; T $E 2000
# Tab bar: right x4 to Enhancements, Enter to activate.
T $R; T $R; T $R; T $R; T $E 900
Save-Shot $hwnd (Join-Path $OutDir '00_enh.png')
# Into the options: Down (show_borders), Down (fov slider), bump +4 steps.
T $D; T $D
T $R 250; T $R 250; T $R 250; T $R 250
Save-Shot $hwnd (Join-Path $OutDir '01_fov_bumped.png')
# Down to the Reset button, press it.
T $D
T $E 900
Save-Shot $hwnd (Join-Path $OutDir '02_after_reset.png')

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. $OutDir"
