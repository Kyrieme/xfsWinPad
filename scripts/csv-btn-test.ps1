# csv-btn-test.ps1 - verify the Datalog "Save as CSV" button works:
# 1. rows = parts + 3 header rows (Unit/Low/High present in the grid)
# 2. clicking the button (WM_COMMAND to the tab, its real parent) opens the
#    GetSaveFileName dialog; then we cancel it. The dialog is proven present
#    by a #32770 window owned by our process that was NOT there before.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class CB1 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
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
    public static int CountBoxes(uint pid){ int n=0;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="#32770") ++n; } return true; },IntPtr.Zero);
        return n; }
    public static void CloseBoxes(uint pid){
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="#32770") SendMessage(h,0x0010,IntPtr.Zero,IntPtr.Zero); } return true; },IntPtr.Zero);
    }
}
"@
$p = Start-Process -FilePath $ExePath -ArgumentList '--new "D:\AI_Work\codex\xfsPad\temp\SAMPLE-B-01_20230411171340_Dlog.std"' -PassThru
Start-Sleep -Seconds 4
$f = [CB1]::FrameOf($p.Id)
if ($f -eq [IntPtr]::Zero) { "FAIL launch"; exit 1 }
[CB1]::SendMessage($f, 0x0111, [IntPtr]576, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$panel = [CB1]::Find($f, "xfsWinPadStdfPanel")
$tab = [CB1]::Find($panel, "SysTabControl32")
[CB1]::SendMessage($tab, 0x130C, [IntPtr]3, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500

# rows: LVM_GETITEMCOUNT on the datalog listview (last one)
$lvs = [CB1]::All($panel, "SysListView32")
$lv = $lvs[$lvs.Count - 1]
$rows = [int][CB1]::SendMessage($lv, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
"rows=$rows (expect 103)"
if ($rows -ne 103) { "FAIL rows"; Get-Process xfsWinPad | Stop-Process -Force; exit 1 }

# click the button: POST WM_COMMAND to tab_ (its parent) - GetSaveFileNameW runs
# a MODAL loop, so a synchronous SendMessage would block the test forever.
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class CB2 { [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); }
"@
$boxesBefore = [CB1]::CountBoxes($p.Id)
[CB2]::PostMessage($tab, 0x0111, [IntPtr]1357, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$boxesAfter = [CB1]::CountBoxes($p.Id)
"dialog boxes before=$boxesBefore after=$boxesAfter"
if ($boxesAfter -gt $boxesBefore) { "PASS: save dialog opened" } else { "FAIL: no dialog" }

[CB1]::CloseBoxes($p.Id)
Start-Sleep -Milliseconds 500
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
if ($boxesAfter -gt $boxesBefore) { exit 0 } else { exit 1 }
