# macro-smoke-test.ps1 - Macro record/playback smoke test (PostMessage-based)
param([string]$ExePath = (Join-Path $PSScriptRoot "..\build\bin\Release\xfsWinPad.exe"))
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MW {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, [Out] System.Text.StringBuilder t, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetFocus();
    [DllImport("user32.dll")] public static extern void SetActiveWindow(IntPtr h);
}
"@

$file = Join-Path $env:TEMP 'xfs_macro_test.txt'
[System.IO.File]::WriteAllText($file, "")

Write-Output ("exe=" + $ExePath + " exists=" + (Test-Path $ExePath))
$proc = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2200
$proc.Refresh()
$main = [IntPtr]$proc.MainWindowHandle

if ($main -eq [IntPtr]::Zero) { Write-Output ("FAIL no main window exited=" + $proc.HasExited); Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; exit 1 }
[MW]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 300

# find Scintilla child
function FindSci($parent) {
    $sci = [IntPtr]::Zero
    $cb = {
        param($h, $l)
        $buf = New-Object System.Text.StringBuilder 256
        [Win32]::GetClassNameW($h, $buf, 256) | Out-Null
        if ($buf.ToString() -eq 'Scintilla') { $script:sci = $h; return $false }
        return $true
    }
    # simpler: EnumChildWindows via inline C# helper below
    return [MacroHelper]::FindSci($parent)
}
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win32 {
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder t, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    public static IntPtr FindSci(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => {
            var b = new StringBuilder(256);
            GetClassNameW(h, b, 256);
            if (b.ToString() == "Scintilla") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$sci = [Win32]::FindSci($main)
if ($sci -eq [IntPtr]::Zero) { Write-Output "FAIL no scintilla"; Stop-Process -Id $proc.Id; exit 1 }

# WM_CHAR for typing: start recording Ctrl+Shift+R -> accelerator handled in main loop.
# Accelerators require the message loop; PostMessage WM_COMMAND works directly:
$WM_COMMAND = 0x0111
[MW]::PostMessageW($main, $WM_COMMAND, [IntPtr]850, [IntPtr]::Zero) | Out-Null   # MacroStart
Start-Sleep -Milliseconds 200

# type "abc"
foreach ($ch in @(97,98,99)) { [MW]::SendMessageW($sci, 0x0102, [IntPtr]$ch, [IntPtr]::Zero) | Out-Null }  # WM_CHAR
Start-Sleep -Milliseconds 150

# stop recording
[MW]::PostMessageW($main, $WM_COMMAND, [IntPtr]851, [IntPtr]::Zero) | Out-Null   # MacroStop
Start-Sleep -Milliseconds 150

# clear the document text so playback re-inserts it visibly
$SCI_SETTEXT = 2181; $SCI_GETTEXTLENGTH = 2006
[MW]::SendMessageW($sci, $SCI_SETTEXT, [IntPtr]::Zero, [IntPtr]"") | Out-Null
$lenBefore = [MW]::SendMessageW($sci, $SCI_GETTEXTLENGTH, [IntPtr]::Zero, [IntPtr]::Zero)

# playback
[MW]::PostMessageW($main, $WM_COMMAND, [IntPtr]852, [IntPtr]::Zero) | Out-Null   # MacroPlayback
Start-Sleep -Milliseconds 500
$lenAfter = [MW]::SendMessageW($sci, $SCI_GETTEXTLENGTH, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Output ("lenBefore=$lenBefore lenAfter=$lenAfter")
if ([int]$lenAfter -eq 3) { Write-Output "PASS macro playback inserted 3 chars" } else { Write-Output "FAIL expected 3 chars after playback" }

Stop-Process -Id $proc.Id -Force
