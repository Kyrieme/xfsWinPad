# verify-run.ps1 - launch xfsWinPad, capture a window screenshot, then close it.
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [string]$Arguments = "",
    [Parameter(Mandatory=$true)][string]$OutPng,
    [int]$WaitMs = 2500
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

if ([string]::IsNullOrWhiteSpace($Arguments)) {
    $proc = Start-Process -FilePath $ExePath -PassThru
} else {
    $proc = Start-Process -FilePath $ExePath -ArgumentList $Arguments -PassThru
}
Start-Sleep -Milliseconds $WaitMs

if ($proc.HasExited) {
    Write-Output "RESULT FAILED_EXIT code=$($proc.ExitCode)"
    exit 1
}

$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    # force refresh
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output "RESULT NO_WINDOW pid=$($proc.Id)"
    Stop-Process -Id $proc.Id -Force
    exit 1
}

[Win32]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object Win32+RECT
[Win32]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { Write-Output "RESULT BAD_RECT"; Stop-Process -Id $proc.Id -Force; exit 1 }

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Write-Output ("RESULT OK pid={0} rect={1},{2},{3}x{4} png={5}" -f $proc.Id, $r.Left, $r.Top, $w, $h, $OutPng)

# graceful close
$proc.CloseMainWindow() | Out-Null
if (-not $proc.WaitForExit(5000)) {
    Stop-Process -Id $proc.Id -Force
    Write-Output "CLOSE FORCE_KILLED"
} else {
    Write-Output "CLOSE GRACEFUL"
}
