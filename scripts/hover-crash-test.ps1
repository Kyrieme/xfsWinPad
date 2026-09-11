# hover-crash-test.ps1 - real mouse gesture: edit text, then move the cursor
# from the editor body up across the tab strip onto the File menu (and open it).
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class HV {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[HV]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms

function MoveAbs([int]$x,[int]$y){
    $i = New-Object HV+INPUT
    $i.type = 0            # INPUT_MOUSE
    $i.u.mi.dx = $x; $i.u.mi.dy = $y
    $i.u.mi.dwFlags = 0x0001 -bor 0x4000   # MOVE | ABSOLUTE
    $i.u.mi.mouseData = 0; $i.u.mi.time = 0; $i.u.mi.dwExtraInfo = [IntPtr]::Zero
    [HV]::SendInput(1, @($i), [System.Runtime.InteropServices.Marshal]::SizeOf([type][HV+INPUT])) | Out-Null
}

$file = Join-Path $env:TEMP 'xfs_hover.txt'
[System.IO.File]::WriteAllText($file, "alpha`r`nbeta`r`ngamma`r`n")

$p = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2200
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { "FAIL no-window"; exit 1 }
function Alive { -not ([System.Diagnostics.Process]::GetProcessById($p.Id)).HasExited }

[HV]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300

# 1) type -> document modified
[System.Windows.Forms.SendKeys]::SendWait("hello")
Start-Sleep -Milliseconds 500

# 2) glide the real cursor from editor center straight up to the File menu,
#    crossing the tab strip, then click File and hover inside the popup.
$r = New-Object HV+RECT
[HV]::GetWindowRect($hwnd,[ref]$r) | Out-Null
$cx = [int](($r.Left + $r.Right) / 2)
$fileY = $r.Top + 45          # menu bar row
MoveAbs $cx ($fileY + 260); Start-Sleep -Milliseconds 120
for ($y = $fileY + 240; $y -ge ($fileY - 6); $y -= 8) {
    MoveAbs $cx $y
    Start-Sleep -Milliseconds 40
    if (-not (Alive)) { break }
}
Start-Sleep -Milliseconds 300
"after-glide alive=" + (Alive)

if (Alive) {
    MoveAbs ($r.Left + 35) $fileY; Start-Sleep -Milliseconds 250
    # click to open File popup
    $down = New-Object HV+INPUT; $down.type=0; $down.u.mi.dwFlags=0x0002; $down.u.mi.mouseData=0; $down.u.mi.time=0; $down.u.mi.dwExtraInfo=[IntPtr]::Zero
    $up   = New-Object HV+INPUT; $up.type=0;   $up.u.mi.dwFlags=0x0004;   $up.u.mi.mouseData=0;   $up.u.mi.time=0;   $up.u.mi.dwExtraInfo=[IntPtr]::Zero
    [HV]::SendInput(1,@($down),72)|Out-Null; Start-Sleep -Milliseconds 120
    [HV]::SendInput(1,@($up),72)|Out-Null
    Start-Sleep -Milliseconds 900
    "after-fileclick alive=" + (Alive)
    # wiggle over the opened popup area
    MoveAbs ($r.Left+70) ($fileY+90); Start-Sleep -Milliseconds 200
    MoveAbs ($r.Left+80) ($fileY+140); Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 300
}

$crashed = -not (Alive)
if (-not $crashed) {
    "RESULT NO_CRASH"
    $p.CloseMainWindow() | Out-Null
    if (-not $p.WaitForExit(4000)) { Stop-Process -Id $p.Id -Force }
} else {
    "RESULT CRASHED exit=" + $p.ExitCode
}
Remove-Item $file -Force -ErrorAction SilentlyContinue
exit ($(if ($crashed) { 2 } else { 0 }))
