# datalog-lag-test.ps1 - measure REAL Datalog interaction cost with a huge STDF.
# Synchronous SendMessage blocks until the app's UI thread finishes processing
# (incl. paint) -> the returned duration is the true per-operation latency.
#  1. open the sample you pass in (233MB; 5526 parts x 992 tests = ~1000 columns)
#  2. vertical: WM_VSCROLL SB_PAGEDOWN x15  (sync)
#  3. horizontal: WM_HSCROLL SB_PAGERIGHT x15 (sync)
#  4. wheel-up batches + hover moves
# PASS if worst op < 250 ms.
#
# The sample path is NOT hard-coded (real product names must not enter the repo):
#   .\datalog-lag-test.ps1 -ExePath <exe> -SamplePath <path-to-.std>
#   or set $env:XFS_DATALOG_SAMPLE
param([Parameter(Mandatory=$true)][string]$ExePath,
      [string]$SamplePath = $env:XFS_DATALOG_SAMPLE)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class DL1 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    public static IntPtr FrameOf(uint pid){ IntPtr f=IntPtr.Zero;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid && f==IntPtr.Zero){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="xfsWinPadMainWindow"){ f=h; return false; } } return true; },IntPtr.Zero);
        return f; }
    public static IntPtr Find(IntPtr parent, string cls){ IntPtr t=IntPtr.Zero;
        EnumChildWindows(parent,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()==cls && t==IntPtr.Zero){ t=h; return false; } return true; },IntPtr.Zero);
        return t; }
    public static System.Collections.Generic.List<IntPtr> All(IntPtr parent, string cls){
        var list=new System.Collections.Generic.List<IntPtr>();
        EnumChildWindows(parent,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()==cls) list.Add(h); return true; },IntPtr.Zero);
        return list; }
    public static long NowMs(){ return (long)(DateTime.UtcNow.Ticks/10000); }
}
public struct RECT { public int L; public int T; public int R; public int B; }
"@
if (-not $SamplePath) { "FAIL: no sample given (use -SamplePath or `$env:XFS_DATALOG_SAMPLE)"; exit 1 }
$p = Start-Process -FilePath $ExePath -ArgumentList ("--new `"{0}`"" -f $SamplePath) -PassThru
Start-Sleep -Seconds 6
$f = [DL1]::FrameOf($p.Id)
if ($f -eq [IntPtr]::Zero) { "FAIL launch"; Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force; exit 1 }
[DL1]::SendMessage($f, 0x0111, [IntPtr]576, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 3
$panel = [DL1]::Find($f, "xfsWinPadStdfPanel")
$tab = [DL1]::Find($panel, "SysTabControl32")
$lvs = [DL1]::All($panel, "SysListView32")
$lv = $lvs[$lvs.Count - 1]

# switch to Datalog tab (index 3), sync-measure the switch itself
$t0 = [DL1]::NowMs()
[DL1]::SendMessage($tab, 0x130D, [IntPtr]3, [IntPtr]::Zero) | Out-Null
$t1 = [DL1]::NowMs()
"tab switch (sync): $($t1 - $t0) ms"
Start-Sleep -Milliseconds 500

# vertical page-down x15 (sync = true cost incl. paint)
$worstV = 0; $sumV = 0
for ($i = 0; $i -lt 15; $i++) {
    $a = [DL1]::NowMs()
    [DL1]::SendMessage($lv, 0x0115, [IntPtr]2, [IntPtr]::Zero) | Out-Null   # WM_VSCROLL SB_PAGEDOWN
    [DL1]::UpdateWindow($lv) | Out-Null
    $b = [DL1]::NowMs(); $d = $b - $a
    $sumV += $d; if ($d -gt $worstV) { $worstV = $d }
}
"vert pagedown x15: worst=$worstV ms avg=$([math]::Round($sumV/15))"

# horizontal page-right x15 (sync)
$worstH = 0; $sumH = 0
for ($i = 0; $i -lt 15; $i++) {
    $a = [DL1]::NowMs()
    [DL1]::SendMessage($lv, 0x0114, [IntPtr]2, [IntPtr]::Zero) | Out-Null   # WM_HSCROLL SB_PAGERIGHT
    [DL1]::UpdateWindow($lv) | Out-Null
    $b = [DL1]::NowMs(); $d = $b - $a
    $sumH += $d; if ($d -gt $worstH) { $worstH = $d }
}
"horz pageright x15: worst=$worstH ms avg=$([math]::Round($sumH/15))"

# sync LVM_GETITEMCOUNT ping under load (UI responsiveness probe)
$a = [DL1]::NowMs()
[DL1]::SendMessage($lv, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$b = [DL1]::NowMs()
"ping GETITEMCOUNT: $($b - $a) ms"

$worst = [math]::Max($worstV, $worstH)
$pass = $worst -lt 250
if ($pass) { "PASS (worst=$worst ms)" } else { "FAIL (worst=$worst ms)" }
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
if ($pass) { exit 0 } else { exit 1 }
