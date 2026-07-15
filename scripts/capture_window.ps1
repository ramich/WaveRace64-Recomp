# Capture a screenshot of the WaveRace64Recomp window to a PNG.
param([string]$OutFile = "window_capture.png", [int]$WaitSeconds = 12)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Capture {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$repo = "C:\dev\src\github\WaveRace64-Recomp"
$p = Start-Process -FilePath "$repo\build\WaveRace64Recomp.exe" -WorkingDirectory $repo -PassThru
Start-Sleep -Seconds $WaitSeconds

$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    # Fallback: find the SDL window by title prefix.
    $found = Get-Process | Where-Object { $_.MainWindowTitle -like 'Wave Race 64*' } | Select-Object -First 1
    if ($found) { $hwnd = $found.MainWindowHandle }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "no window handle"; $p | Stop-Process -Force; exit 1 }

[Win32Capture]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

$rect = New-Object Win32Capture+RECT
[Win32Capture]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
Write-Output "window rect: $($rect.Left),$($rect.Top) ${w}x${h}"

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
# PW_RENDERFULLCONTENT (2): captures DX swapchain content even when occluded.
[Win32Capture]::PrintWindow($hwnd, $hdc, 2) | Out-Null
$gfx.ReleaseHdc($hdc)
$bmp.Save("$repo\$OutFile", [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()

$p | Stop-Process -Force
Write-Output "saved $repo\$OutFile"
