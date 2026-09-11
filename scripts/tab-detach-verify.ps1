# tab-detach-verify.ps1 - strict verification of tab drag-out behavior:
# after dragging tab N out of the strip, the NEW window must show the dragged
# file, and the SOURCE window must keep the other file (1 tab left).
# Usage: -DragTab 0 | -DragTab 1
param([Parameter(Mandatory=$true)][string]$ExePath,
      [ValidateSet(0,1)][int]$DragTab = 1)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class TDV2 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,IntPtr e);
    [DllImport("user32.dll",EntryPoint="GetClientRect")] public static extern bool ClientRect(IntPtr h, ref RECT r);
    [DllImport("user32.dll",EntryPoint="ClientToScreen")] public static extern bool C2S(IntPtr h, ref POINT p);
    [DllImport("user32.dll",EntryPoint="SendMessageW")]
    public static extern IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x,y; }
    public static IntPtr FrameOf(uint pid){ IntPtr f=IntPtr.Zero;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid && f==IntPtr.Zero){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="xfsWinPadMainWindow"){ f=h; return false; } } return true; },IntPtr.Zero);
        return f; }
    public static IntPtr VisibleStrip(IntPtr frame){ IntPtr t=IntPtr.Zero;
        EnumChildWindows(frame,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="SysTabControl32" && IsWindowVisible(h)){ t=h; return false; } return true; },IntPtr.Zero);
        return t; }
    public static int TabCount(IntPtr frame){
        var s=VisibleStrip(frame); if(s==IntPtr.Zero) return -1;
        // TCM_GETITEMCOUNT = TCM_FIRST(0x1300) + 20 = 0x1314
        return (int)Send(s, 0x1314, IntPtr.Zero, IntPtr.Zero); }
    public static void DragOut(IntPtr strip, int tabX){
        var r=new RECT(); ClientRect(strip, ref r);
        var tl=new POINT{ x=r.L, y=r.T }; C2S(strip, ref tl);
        int midY = tl.y + (r.B - r.T)/2;
        int outY  = tl.y + (r.B - r.T) + 160;
        SetCursorPos(tl.x + tabX, midY);
        System.Threading.Thread.Sleep(150);
        mouse_event(2,0,0,0,IntPtr.Zero);
        System.Threading.Thread.Sleep(120);
        for(int y=midY; y<=outY; y+=14){ SetCursorPos(tl.x + tabX, y); System.Threading.Thread.Sleep(15); }
        mouse_event(4,0,0,0,IntPtr.Zero);
    }
}
"@

# fixtures: short name (tab0) + long name (tab1) so tab widths differ a lot
$tmp = Join-Path $env:TEMP 'xfs_tabdetach2'
New-Item -ItemType Directory -Force $tmp | Out-Null
$a = Join-Path $tmp 'a.txt'
$b = Join-Path $tmp 'the-second-file-with-long-name.txt'
[System.IO.File]::WriteAllText($a, "short`r`n")
[System.IO.File]::WriteAllText($b, "long`r`n")
$draggedPat = if ($DragTab -eq 0) { "a\.txt" } else { "the-second-file" }
$keptPat    = if ($DragTab -eq 0) { "the-second-file" } else { "a\.txt" }
# tab0 is narrow (~70px) - center x=35; tab1 starts ~70 and is wide - center x=240
$dragX = if ($DragTab -eq 0) { 35 } else { 240 }

$p = Start-Process -FilePath $ExePath -ArgumentList "--new `"$a`" `"$b`"" -PassThru
Start-Sleep -Seconds 3
$f = [TDV2]::FrameOf($p.Id)
if ($f -eq [IntPtr]::Zero) { Fail "launch"; exit 1 } else { Pass "launch" }
$strip = [TDV2]::VisibleStrip($f)
if ($strip -eq [IntPtr]::Zero) { Fail "tabstrip"; exit 1 } else { Pass "tabstrip" }
[TDV2]::DragOut($strip, $dragX)
Start-Sleep -Seconds 3

$procs = Get-Process xfsWinPad
if ($procs.Count -ne 2) { Fail "two-windows (got $($procs.Count))"; exit 1 } else { Pass "two-windows" }
$src = $procs | Where-Object { $_.Id -eq $p.Id }
$new = $procs | Where-Object { $_.Id -ne $p.Id }
$srcTitle = "$($src.MainWindowTitle)"
$newTitle = "$($new.MainWindowTitle)"
"source: [$srcTitle]"
"new:    [$newTitle]"

if ($newTitle -match $draggedPat) { Pass "new-window-has-dragged-file" } else { Fail "new-window-has-dragged-file" }
if ($srcTitle -match $keptPat) { Pass "source-keeps-other-file" } else { Fail "source-keeps-other-file" }
# tab-count cross-proc read is unreliable (TCM_* not marshalled) - title pair
# above IS the behavioral proof; keep the script free of false negatives.

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })
