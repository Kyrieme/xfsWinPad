# diag-visibility.ps1 - dump every FindDialog child: id, class, visible flag, rect.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class DV {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h,int i);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    public struct RECT { public int Left,Top,Right,Bottom; }
    public static uint pid; public static IntPtr frame=IntPtr.Zero, dlg=IntPtr.Zero;
    private static bool TopScan(IntPtr h, IntPtr lp){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){frame=h;return false;}}
        return true;}
    private static bool DlgScan(IntPtr h, IntPtr lp){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadFindDlg"){dlg=h;return false;}}
        return true;}
    public static void LocateFrame(){ frame=IntPtr.Zero;
        EnumWindows(new EnumProc(TopScan),IntPtr.Zero); }
    public static void LocateDlg(){ dlg=IntPtr.Zero;
        if(frame!=IntPtr.Zero) EnumWindows(new EnumProc(DlgScan),IntPtr.Zero); }
    public static void Dump(){
        RECT dr; GetWindowRect(dlg,out dr);
        Console.WriteLine("DLG vis="+IsWindowVisible(dlg)+" rect=("+dr.Left+","+dr.Top+")-("+dr.Right+","+dr.Bottom+")");
        int i=0;
        EnumChildWindows(dlg,(h,l)=>{
            var c=new StringBuilder(64);GetClassNameW(h,c,64);
            var t=new StringBuilder(96);GetWindowTextW(h,t,96);
            int id=GetWindowLongW(h,-12);
            RECT r;GetWindowRect(h,out r);
            Console.WriteLine(i+" id="+id+" "+c.ToString()+" vis="+IsWindowVisible(h)+
                " rect=("+r.Left+","+r.Top+","+r.Right+","+r.Bottom+") ["+t.ToString()+"]");
            i++; return true;},IntPtr.Zero);}
}
"@
$p = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 2200
[DV]::pid=[uint32]$p.Id
[DV]::LocateFrame()
[DV]::PostMessageW([DV]::frame,0x0111,[IntPtr]400,[IntPtr]::Zero)|Out-Null
Start-Sleep -Milliseconds 900
[DV]::LocateDlg()
"dlg=$([DV]::dlg)"
if ([DV]::dlg -ne [IntPtr]::Zero) { [DV]::Dump() }
Stop-Process -Id $p.Id -Force
