# Drives WaveRace64Recomp from boot into a live Time Trials race, then holds
# the accelerator for a while so the in-race camera settles (for FOV-site
# classification probes that need real gameplay, not just the attract demo).
# Input is posted directly to the game's HWND message queue (no
# SetForegroundWindow / keybd_event), so the window never needs focus and can
# sit behind other windows while this drives it.
#
# Menu path (confirmed by the user walking it manually 2026-07-16), Time
# Trials instead of Championship because it skips craft-select entirely and
# goes straight from course pick to difficulty to the race:
#   title -(Enter,Enter)-> main menu [CHAMPIONSHIP / TIME TRIALS / STUNT MODE /
#   2P VS. / OPTIONS] -(Down, X)-> Course Select [only Sunny Beach unlocked;
#   the rest need Championship progression] -(X)-> difficulty select
#   [NORMAL/HARD/EXPERT] -(X)-> live race.
# The final confirm is verified against the game's own [SCENE] classifier
# (WR64_SCENE_DEBUG=1), which prints "-> wide" once the live per-frame
# projection is active vs "-> menu" for any parked-world menu screen, so a
# dropped tap gets retried instead of silently stalling.
# ASCII only (PS 5.1 safe).
param(
    [int]$SettleSeconds = 14,
    [int]$AccelerateSeconds = 20,
    [int]$MaxRetries = 10,
    [string]$OutDir = "$PSScriptRoot\..\drive_out",
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
public static class KeySend {
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    const uint WM_KEYDOWN = 0x0100;
    const uint WM_KEYUP = 0x0101;
    const uint EXTENDED_FLAG = 0x01000000;
    // Arrow/Home/End/Insert/Delete/PageUp/PageDown share their VK code with a
    // Numpad key when NumLock is off; Windows disambiguates them only via the
    // lParam "extended key" bit (24). Omitting it (as an earlier version of
    // this script did) meant every Down-arrow tap could resolve to Numpad-2
    // instead, which the game doesn't bind to anything -- explained why X
    // taps always worked but Down silently never did (observed 2026-07-16).
    static bool IsExtended(byte vk) {
        return vk == 0x21 || vk == 0x22 || vk == 0x23 || vk == 0x24 // PgUp/PgDn/End/Home
            || vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 // arrows
            || vk == 0x2D || vk == 0x2E;                            // Insert/Delete
    }
    // Posted directly to the game's HWND message queue (no SetForegroundWindow,
    // no keybd_event) so the window never needs focus and can sit behind other
    // windows while this drives it. SDL2's WndProc processes WM_KEYDOWN/UP from
    // any source the same way; keybd_event instead injects into the real
    // hardware queue, which Windows only routes to the foreground window.
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
    $rect = New-Object KeySend+RECT
    [KeySend]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    [KeySend]::PrintWindow($hwnd, $hdc, 2) | Out-Null
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

function Test-SceneIsWide([string]$errLogPath) {
    if (-not (Test-Path $errLogPath)) { return $false }
    $tail = Get-Content $errLogPath -Tail 40 -ErrorAction SilentlyContinue
    foreach ($line in $tail) {
        if ($line -match '\[SCENE\].*-> wide') { return $true }
    }
    return $false
}

# True once the CHAMPIONSHIP bar's dark-green background is showing at its
# known position (fractional coords, verified against 1920x800 captures
# 2026-07-16). The title screen shows bright sky/logo colors at the same
# spot, so this reliably distinguishes "still on title" from "menu is up" —
# a fixed wait was NOT reliable here (the "Press START to begin." prompt can
# still be showing well past the point a wait assumed it wouldn't be).
function Test-MainMenuShowing([IntPtr]$hwnd) {
    $bmp = Get-WindowBitmap $hwnd
    if ($null -eq $bmp) { return $false }
    $x = [int]($bmp.Width * 0.498)
    $y = [int]($bmp.Height * 0.357)
    $c = $bmp.GetPixel($x, $y)
    $bmp.Dispose()
    return ($c.R -lt 90 -and $c.G -gt 40 -and $c.G -lt 140 -and $c.B -lt 90)
}

$env:WR64_BORDERS = '0'
$env:WR64_SCENE_DEBUG = '1'
$env:WR64_FBP_DEBUG = '1'
$env:WR64_WINDOW = '1920x800'
if (-not $env:WR64_FB_DUMP) { $env:WR64_FB_DUMP = '1' }

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$proc = Start-Process -FilePath $Exe -WorkingDirectory $repoRoot -RedirectStandardError $errLog -RedirectStandardOutput $outLog -PassThru
Write-Host "Launched pid $($proc.Id), settling $SettleSeconds s..."
Start-Sleep -Seconds $SettleSeconds
if ($proc.HasExited) { Write-Host 'Process exited early!'; exit 1 }

$proc.Refresh()
$hwnd = $proc.MainWindowHandle

# VK codes.
$VK_ENTER = 0x0D; $VK_X = 0x58; $VK_DOWN = 0x28

# Exact recipe confirmed by the user manually walking it live (2026-07-16):
# wait 5s, X, wait 1s, X, wait 1s, Down+X, wait 2s, X, wait 1s, X, wait 1s, X
# -> in game. All taps are X (the A button), not Enter/Start.
Write-Host 'Title -> main menu (X, X)'
Start-Sleep -Seconds 5
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
Save-WindowShot $hwnd (Join-Path $OutDir 'main_menu.png')

Write-Host 'Main menu: Down to Time Trials, then X'
[KeySend]::Tap($hwnd, [byte]$VK_DOWN, 250)
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 2
Save-WindowShot $hwnd (Join-Path $OutDir 'course_select.png')

Write-Host 'Course select -> difficulty -> warm up -> race (X, X, X)'
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
Save-WindowShot $hwnd (Join-Path $OutDir 'confirm_0.png')
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Start-Sleep -Seconds 1
Save-WindowShot $hwnd (Join-Path $OutDir 'confirm_1.png')
[KeySend]::Tap($hwnd, [byte]$VK_X, 250)
Save-WindowShot $hwnd (Join-Path $OutDir 'entered_race.png')

# Hold the accelerator (A / X) so the world camera actually moves and settles
# on the live in-race projection instead of sitting parked at the start line.
Write-Host "Holding accelerator for $AccelerateSeconds s..."
[KeySend]::KeyDown($hwnd, [byte]$VK_X)
Start-Sleep -Seconds $AccelerateSeconds
[KeySend]::KeyUp($hwnd, [byte]$VK_X)
Save-WindowShot $hwnd (Join-Path $OutDir 'racing.png')

Start-Sleep -Seconds 1
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done. Logs in $OutDir"
