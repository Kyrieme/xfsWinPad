# crash-repro-menu.ps1 - reproduce: edit text, then open the File menu.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'

# per-user WER local dumps so we get a minidump if it crashes
$dumpKey = 'HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\xfsWinPad.exe'
New-Item -Path $dumpKey -Force | Out-Null
Set-ItemProperty $dumpKey -Name DumpFolder -Value "$env:LOCALAPPDATA\CrashDumps" -Type ExpandString
Set-ItemProperty $dumpKey -Name DumpType -Value 2 -Type DWord
Set-ItemProperty $dumpKey -Name DumpCount -Value 5 -Type DWord

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class CR {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public static IntPtr FindChildByClass(IntPtr parent, string cls){
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent,(h,l)=>{var b=new StringBuilder(64);GetClassNameW(h,b,64);
            if(b.ToString()==cls){found=h;return false;}return true;},IntPtr.Zero);
        return found;}
}
"@
[CR]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms

$file = Join-Path $env:TEMP 'xfs_crash.txt'
[System.IO.File]::WriteAllText($file, "line one`r`nline two`r`n")

$p = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2000
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { "FAIL no-window"; exit 1 }

function Alive { -not ([System.Diagnostics.Process]::GetProcessById($p.Id)).HasExited }

[CR]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300

# 1) type text -> document becomes modified
[System.Windows.Forms.SendKeys]::SendWait("abc")
Start-Sleep -Milliseconds 500
"after-typing alive=" + (Alive)

# 2) open the File menu (Alt+F enters the modal menu loop)
[System.Windows.Forms.SendKeys]::SendWait("%f")
Start-Sleep -Milliseconds 1200
"after-altF alive=" + (Alive)

# 3) escape out and also try Search menu
[System.Windows.Forms.SendKeys]::SendWait("{ESC}")
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("%(ESC)") | Out-Null
[System.Windows.Forms.SendKeys]::SendWait("{ESC}")
[System.Windows.Forms.SendKeys]::SendWait("%v")
Start-Sleep -Milliseconds 1000
"after-viewmenu alive=" + (Alive)
[System.Windows.Forms.SendKeys]::SendWait("{ESC}")
Start-Sleep -Milliseconds 400

if (Alive) {
    "RESULT NO_CRASH"
    $p.CloseMainWindow() | Out-Null
    if (-not $p.WaitForExit(4000)) { Stop-Process -Id $p.Id -Force }
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    exit 0
} else {
    "RESULT CRASHED exitcode=" + $p.ExitCode
    Get-ChildItem "$env:LOCALAPPDATA\CrashDumps" -Filter *.dmp -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    exit 2
}
