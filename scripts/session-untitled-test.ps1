# session-untitled-test.ps1 - untitled dirty tabs survive a restart.
# Cross-process SCI_GETTEXT cannot marshal its buffer, so editor content is
# verified through session.json (what the saver wrote) instead of reading the
# Scintilla control directly.
#   1. launch fresh, type "scratch123" into the untitled tab, close window
#   2. session.json must contain the text snapshot + tab label
#   3. relaunch -> session restore recreates the tab (verified via
#      "Session restored" log line + window title shows the tab label)
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class SUT2 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    public static IntPtr Frame(uint pid){ IntPtr f=IntPtr.Zero;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid && f==IntPtr.Zero){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="xfsWinPadMainWindow"){ f=h; return false; } } return true; },IntPtr.Zero);
        return f; }
    public static String Title(IntPtr h){
        var t=new StringBuilder(256); GetWindowTextW(h,t,256); return t.ToString(); }
}
"@
$sess = Join-Path $env:APPDATA 'xfsWinPad\session.json'
Remove-Item $sess -Force -ErrorAction SilentlyContinue   # isolate from prior sessions
$log  = Join-Path $env:LOCALAPPDATA 'xfsWinPad\logs\xfsWinPad.log'

# --- round 1: type into the untitled tab, close -> snapshot must land ---
$p = Start-Process -FilePath $ExePath -ArgumentList '--new' -PassThru
Start-Sleep -Seconds 3
$f = [SUT2]::Frame($p.Id)
if ($f -eq [IntPtr]::Zero) { Fail "launch"; exit 1 } else { Pass "launch" }

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class FW1 { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
[FW1]::SetForegroundWindow($f) | Out-Null
Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("scratch1239")
Start-Sleep -Milliseconds 500

if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null }
if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 600

if (Test-Path $sess) {
    $j = Get-Content $sess -Raw -Encoding UTF8
    if ($j -match "scratch") { Pass "session-has-snapshot" }
    else { Fail "session-has-snapshot"; }
    "session.json size: $((Get-Item $sess).Length) bytes"
} else { Fail "session-file-missing" }

# --- round 2: relaunch -> restore must recreate the tab (log + title) ---
$p2 = Start-Process -FilePath $ExePath -ArgumentList '--new' -PassThru
Start-Sleep -Seconds 3
$f2 = [SUT2]::Frame($p2.Id)
if ($f2 -eq [IntPtr]::Zero) { Fail "relaunch"; exit 1 } else { Pass "relaunch" }

$tail = Get-Content $log -Tail 40
$restoredLine = $tail | Select-String -Pattern "Session restored" | Select-Object -Last 1
"restore log: $restoredLine"
if ("$restoredLine" -match "Session restored: 1 \+ 0") { Pass "untitled-restored-count" }
else { Fail "untitled-restored-count" }

# tab label survives? title bar shows the active document name
$title = [SUT2]::Title($f2)
"window title: $title"
if ($title -match "new 1|new1 1") { Pass "tab-label-restored" } else { Fail "tab-label-restored" }

# cleanup
if (-not $p2.HasExited) { $p2.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 800 }
if (-not $p2.HasExited) { Stop-Process -Id $p2.Id -Force }
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })
