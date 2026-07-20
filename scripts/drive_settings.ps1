# One-off: open the launcher Settings menu and walk the tab bar with arrow keys,
# screenshotting each step (verify the new Button config options render).
# ASCII only (PS 5.1 safe).
param(
    [string]$OutDir = "$PSScriptRoot\..\drive_settings",
    [string]$Exe = "$PSScriptRoot\..\build\WaveRace64Recomp.exe",
    [int]$RightTaps = 5,
    [string]$Window = '1920x800'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KeySend3 {
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
    $rect = New-Object KeySend3+RECT
    [KeySend3]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc(); [KeySend3]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $gfx.ReleaseHdc($hdc); $gfx.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "saved $path"
}

$env:WR64_WINDOW = $Window
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $Exe -WorkingDirectory $repoRoot -RedirectStandardError (Join-Path $OutDir 'stderr.log') -RedirectStandardOutput (Join-Path $OutDir 'stdout.log') -PassThru
Start-Sleep -Seconds 14
$proc.Refresh(); $hwnd = $proc.MainWindowHandle
[KeySend3]::SetCursorPos(0, 0) | Out-Null

$VK_ENTER = 0x0D; $VK_DOWN = 0x28; $VK_RIGHT = 0x27
# Launcher: Down x2 to Settings, Enter.
[KeySend3]::Tap($hwnd, [byte]$VK_DOWN, 150); Start-Sleep -Milliseconds 400
[KeySend3]::Tap($hwnd, [byte]$VK_DOWN, 150); Start-Sleep -Milliseconds 400
[KeySend3]::Tap($hwnd, [byte]$VK_ENTER, 200); Start-Sleep -Seconds 2
Save-Shot $hwnd (Join-Path $OutDir '00_settings.png')

for ($i = 1; $i -le $RightTaps; $i++) {
    [KeySend3]::Tap($hwnd, [byte]$VK_RIGHT, 150); Start-Sleep -Milliseconds 700
    Save-Shot $hwnd (Join-Path $OutDir ("{0:D2}_right.png" -f $i))
}

# Activate the highlighted tab, screenshot its contents (Enhancements at 4
# rights), then move once more and activate (Textures at 5).
[KeySend3]::Tap($hwnd, [byte]$VK_ENTER, 200); Start-Sleep -Milliseconds 900
Save-Shot $hwnd (Join-Path $OutDir '10_tab_a.png')
[KeySend3]::Tap($hwnd, [byte]$VK_RIGHT, 150); Start-Sleep -Milliseconds 500
[KeySend3]::Tap($hwnd, [byte]$VK_ENTER, 200); Start-Sleep -Milliseconds 900
Save-Shot $hwnd (Join-Path $OutDir '11_tab_b.png')

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. $OutDir"
