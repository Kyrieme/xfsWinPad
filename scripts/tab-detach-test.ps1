# tab-detach-test.ps1 - drag a tab OUT of the tab strip => a new xfsWinPad
# window must appear (SpawnWindow --new) with that file open, and the source
# window keeps running.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class TDT {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags,int dx,int dy,uint data,IntPtr extra);
    public static uint pid;
    public static IntPtr frameW, tabW;
    public static void Locate(){
        frameW=IntPtr.Zero; tabW=IntPtr.Zero;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="xfsWinPadMainWindow" && frameW==IntPtr.Zero) frameW=h; }
            return true; },IntPtr.Zero);
        if(frameW!=IntPtr.Zero)
            EnumChildWindows(frameW,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="SysTabControl32" && tabW==IntPtr.Zero){ tabW=h; return false; } return true; },IntPtr.Zero);
    }
    public static System.Collections.Generic.List<uint> MainPids(){
        var list = new System.Collections.Generic.List<uint>();
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow") list.Add(w);
            return true; },IntPtr.Zero);
        return list; }
}
"@
$MOUSE_LEFTDOWN = 0x0002
$MOUSE_LEFTUP = 0x0004

# fixture: two files
$tmp = Join-Path $env:TEMP 'xfs_tabdetach'
New-Item -ItemType Directory -Force $tmp | Out-Null
$a = Join-Path $tmp 'one.txt'
$b = Join-Path $tmp 'two.txt'
[System.IO.File]::WriteAllText($a, "file one`r`n")
[System.IO.File]::WriteAllText($b, "file two`r`n")

$p = Start-Process -FilePath $ExePath -ArgumentList "`"$a`" `"$b`"" -PassThru
Start-Sleep -Seconds 3
[TDT]::pid = $p.Id
[TDT]::Locate()
if ([TDT]::frameW -eq [IntPtr]::Zero) { Fail "launch"; exit 1 } else { Pass "launch" }
if ([TDT]::tabW -eq [IntPtr]::Zero) { Fail "tabstrip-found"; exit 1 } else { Pass "tabstrip-found" }

$pidsBefore = [TDT]::MainPids()
"main-window processes before: $($pidsBefore.Count)"

# drag the FIRST tab (one.txt) straight DOWN out of the strip and release
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class TDT2 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x,y; }
}
'@
$r = New-Object TDT2+RECT
[TDT2]::GetWindowRect([TDT]::tabW, [ref]$r) | Out-Null
$stripMidY = ($r.T + $r.B) / 2
$tabX = $r.L + 60            # inside the first tab
$outY  = $r.B + 120          # well below the strip (inside the editor area)
"strip rect: L=$($r.L) T=$($r.T) R=$($r.R) B=$($r.B); drag ($tabX,$stripMidY) -> ($tabX,$outY)"

[TDT]::SetCursorPos($tabX, $stripMidY)
Start-Sleep -Milliseconds 150
[TDT]::mouse_event($MOUSE_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
# move gradually so WM_MOUSEMOVE reorder logic runs and the detach flag trips
for ($y = $stripMidY; $y -le $outY; $y += 15) {
    [TDT]::SetCursorPos($tabX, $y)
    Start-Sleep -Milliseconds 15
}
[TDT]::mouse_event($MOUSE_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 3

# a new xfsWinPad main window process must exist now
$pidsAfter = [TDT]::MainPids()
"main-window processes after: $($pidsAfter.Count) (expect 2)"
if ($pidsAfter.Count -eq 2) { Pass "detach-spawned-new-window" } else { Fail "detach-spawned-new-window" }

# source window must still be alive
if ($p.HasExited) { Fail "source-window-alive" } else { Pass "source-window-alive" }

# cleanup: kill every xfsWinPad
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })
