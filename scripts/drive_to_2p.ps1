# Drives WaveRace64Recomp from boot into a 2P VS split-screen race, capturing a
# screenshot at every step + the [SCENE]/[FBP] stderr telemetry, so the
# split-screen widescreen work can be iterated without manual testing.
#
# Menu path (main menu order: CHAMPIONSHIP / TIME TRIALS / STUNT MODE / 2P VS. /
# OPTIONS): title -(X,X)-> main menu -(Down x3, X)-> 2P VS -> course select
# -(X)-> watercraft select (BOTH players confirm) -> race.
# Input is posted straight to the game's HWND queue (no focus steal), same as
# drive_to_race.ps1. ASCII only (PS 5.1 safe).
param(
    [int]$SettleSeconds = 14,
    [int]$AccelerateSeconds = 6,
    [int]$MenuDowns = 3,           # main-menu Down taps: 0=Championship,1=Time Trials,3=2P VS
    [int]$CraftXtaps = 9,          # X taps course->craft->race (single A drives both players)
    [string]$OutDir = "$PSScriptRoot\..\drive_2p",
    [string]$Exe = "$PSScriptRoot\..\build\WaveRace64Recomp.exe"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$errLog = Join-Path $OutDir 'stderr.log'
$outLog = Join-Path $OutDir 'stdout.log'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KeySend2 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    const uint WM_KEYDOWN = 0x0100;
    const uint WM_KEYUP = 0x0101;
    const uint EXTENDED_FLAG = 0x01000000;
    static bool IsExtended(byte vk) {
        return vk == 0x21 || vk == 0x22 || vk == 0x23 || vk == 0x24
            || vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28
            || vk == 0x2D || vk == 0x2E;
    }
    public static void KeyDown(IntPtr hwnd, byte vk) {
        byte scan = (byte)MapVirtualKey(vk, 0);
        uint lParam = 1u | ((uint)scan << 16);
        if (IsExtended(vk)) lParam |= EXTENDED_FLAG;
        PostMessage(hwnd, WM_KEYDOWN, (IntPtr)vk, (IntPtr)(long)lParam);
    }
    public static void KeyUp(IntPtr hwnd, byte vk) {
        byte scan = (byte)MapVirtualKey(vk, 0);
        uint lParam = 1u | ((uint)scan << 16) | (1u << 30) | (1u << 31);
        if (IsExtended(vk)) lParam |= EXTENDED_FLAG;
        PostMessage(hwnd, WM_KEYUP, (IntPtr)vk, (IntPtr)(long)lParam);
    }
    public static void Tap(IntPtr hwnd, byte vk, int holdMs) {
        KeyDown(hwnd, vk);
        System.Threading.Thread.Sleep(holdMs);
        KeyUp(hwnd, vk);
    }
}
'@

function Get-WindowBitmap([IntPtr]$hwnd) {
    $rect = New-Object KeySend2+RECT
    [KeySend2]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    [KeySend2]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $gfx.ReleaseHdc($hdc)
    $gfx.Dispose()
    return $bmp
}

function Save-WindowShot([IntPtr]$hwnd, [string]$path) {
    $bmp = Get-WindowBitmap $hwnd
    if ($null -eq $bmp) { Write-Host 'bad window rect'; return }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "saved $path"
}

$env:WR64_BORDERS = '0'
$env:WR64_SCENE_DEBUG = '1'
$env:WR64_FBP_DEBUG = '1'
$env:WR64_WINDOW = '1920x800'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $Exe -WorkingDirectory $repoRoot -RedirectStandardError $errLog -RedirectStandardOutput $outLog -PassThru
Write-Host "Launched pid $($proc.Id), settling $SettleSeconds s..."
Start-Sleep -Seconds $SettleSeconds
if ($proc.HasExited) { Write-Host 'Process exited early!'; exit 1 }

$proc.Refresh()
$hwnd = $proc.MainWindowHandle

# The active control config maps the A button to SPACE and the stick to WASD
# (captured from a manual playthrough via scripts/log_keys.ps1). X is unbound,
# which is why the earlier X-based driver never advanced past the title.
$VK_A = 0x20      # Space = A button (confirm / accelerate)
$VK_DOWN = 0x53   # S = stick down (menu navigation)
$VK_ENTER = 0x0D  # launcher only
$VK_X = $VK_A     # alias so the rest of the script reads naturally

# Park the mouse cursor in the top-left corner, away from the centered launcher
# menu: RecompFrontend highlights menu items on HOVER, so a cursor left over the
# window overrides the keyboard highlight and ENTER selects the wrong item
# (the flaky "ended up in Settings / Championship" runs). Keyboard nav only.
[KeySend2]::SetCursorPos(0, 0) | Out-Null

# The RecompFrontend launcher is up first, with the first item (Start Game)
# pre-highlighted, so a SINGLE Enter selects it and boots the ROM. The mouse is
# parked in the corner above so hover can't override the keyboard highlight.
Write-Host 'Launcher: ENTER to Start Game'
[KeySend2]::Tap($hwnd, [byte]$VK_ENTER, 250)
# The ROM boots now (Start Game -> recomp::start_game); wait for the interactive
# title before sending input, but not so long the idle-timeout rolls the demo.
Start-Sleep -Seconds 8
Save-WindowShot $hwnd (Join-Path $OutDir '00b_after_enter.png')

Write-Host 'Game title -> main menu (X, X)'
[KeySend2]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
[KeySend2]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
Save-WindowShot $hwnd (Join-Path $OutDir '01_main_menu.png')

Write-Host "Main menu: Down x$MenuDowns then X"
for ($d = 1; $d -le $MenuDowns; $d++) {
    [KeySend2]::Tap($hwnd, [byte]$VK_DOWN, 200); Start-Sleep -Milliseconds 400
    Save-WindowShot $hwnd (Join-Path $OutDir ("02_down{0}.png" -f $d))
}
[KeySend2]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 2
Save-WindowShot $hwnd (Join-Path $OutDir '05_after_2pvs.png')

# From here it's a run of A presses (single A drives BOTH players in 2P VS):
# course select -> both players' watercraft select -> race. Tap X repeatedly,
# screenshotting each, until the [SCENE] log shows we're in the wide race.
Write-Host "Advancing with X taps ($CraftXtaps) course->craft->race"
for ($i = 1; $i -le $CraftXtaps; $i++) {
    [KeySend2]::Tap($hwnd, [byte]$VK_X, 250)
    Start-Sleep -Seconds 1
    Save-WindowShot $hwnd (Join-Path $OutDir ("06_x{0}.png" -f $i))
}
Start-Sleep -Seconds 2
Save-WindowShot $hwnd (Join-Path $OutDir '08_maybe_race.png')

# Burst-capture the pre-race FLYBY window (intro camera pan before the
# countdown), where a lower-half ghosting artifact was reported.
for ($f = 0; $f -lt 16; $f++) {
    Save-WindowShot $hwnd (Join-Path $OutDir ("flyby_{0:D2}.png" -f $f))
    Start-Sleep -Milliseconds 140
}

Write-Host "Holding accelerator for $AccelerateSeconds s..."
[KeySend2]::KeyDown($hwnd, [byte]$VK_X)
# Burst-capture the accelerate window so flickering artifacts (present only on
# some frames, e.g. interpolated ones) can't be missed by a single shot.
$burst = [int]([math]::Max(1, $AccelerateSeconds)) * 6
for ($b = 0; $b -lt $burst; $b++) {
    Save-WindowShot $hwnd (Join-Path $OutDir ("burst_{0:D2}.png" -f $b))
    Start-Sleep -Milliseconds 160
}
[KeySend2]::KeyUp($hwnd, [byte]$VK_X)
Save-WindowShot $hwnd (Join-Path $OutDir '09_racing.png')

Start-Sleep -Seconds 1
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. Screens + logs in $OutDir"
