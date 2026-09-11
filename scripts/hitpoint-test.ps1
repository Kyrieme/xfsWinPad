# hitpoint-test.ps1 - decisive: which window is TOPMOST at the button/combo centers?
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class HP {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct RECT { public int Left,Top,Right,Bottom; }
    public struct POINT { public int x,y; }
    public static uint pid; public static IntPtr frame=IntPtr.Zero, dlg=IntPtr.Zero;
    private static bool Top(IntPtr h,IntPtr l){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){frame=h;return false;}} return true;}
    private static bool Dl(IntPtr h,IntPtr l){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadFindDlg"){dlg=h;return false;}} return true;}
    public static void LocateFrame(){ frame=IntPtr.Zero; EnumWindows(new EnumProc(Top),IntPtr.Zero); }
    public static void LocateDlg(){ dlg=IntPtr.Zero;
        if(frame!=IntPtr.Zero) EnumWindows(new EnumProc(Dl),IntPtr.Zero); }
    public static string ClassOf(IntPtr h){ var c=new StringBuilder(64);GetClassNameW(h,c,64); return c.ToString(); }
    public static IntPtr TopAt(int x,int y){ POINT p; p.x=x; p.y=y; return WindowFromPoint(p); }
}
"@
$p = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 2200
[HP]::pid=[uint32]$p.Id
[HP]::LocateFrame()
[HP]::PostMessageW([HP]::frame,0x0111,[IntPtr]400,[IntPtr]::Zero)|Out-Null
Start-Sleep -Milliseconds 900
[HP]::LocateDlg()
"dlg=$([HP]::dlg) class=$([HP]::ClassOf([HP]::dlg))"

foreach ($id in 1,1100) {
    $c = [HP]::GetDlgItem([HP]::dlg, $id)
    $r = New-Object HP+RECT
    [HP]::GetWindowRect($c,[ref]$r)|Out-Null
    $cx = [int](($r.Left+$r.Right)/2); $cy=[int](($r.Top+$r.Bottom)/2)
    $top = [HP]::TopAt($cx,$cy)
    "id=$id rect=($($r.Left),$($r.Top))-($($r.Right),$($r.Bottom)) center=($cx,$cy)"
    "   topmost-at-center: $($top) class=$([HP]::ClassOf($top)) match=$($top -eq $c)"
}
Stop-Process -Id $p.Id -Force
