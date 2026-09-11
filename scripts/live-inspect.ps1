# live-inspect.ps1 - capture and analyze the RUNNING user instance's dialog.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class LI {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    public struct RECT { public int Left,Top,Right,Bottom; }
    public static uint pid; public static IntPtr frame=IntPtr.Zero, dlg=IntPtr.Zero;
    private static bool Top(IntPtr h,IntPtr lp){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){frame=h;return false;}}
        return true;}
    private static bool Dl(IntPtr h,IntPtr lp){ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){var c=new StringBuilder(64);GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadFindDlg"){dlg=h;return false;}}
        return true;}
    public static void Locate(){ EnumWindows(new EnumProc(Top),IntPtr.Zero); }
    public static void LocateDlg(){ dlg=IntPtr.Zero;
        if(frame!=IntPtr.Zero) EnumWindows(new EnumProc(Dl),IntPtr.Zero); }
}
"@
[LI]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$p = Get-Process xfsWinPad -ErrorAction Stop | Select-Object -First 1
[LI]::pid = [uint32]$p.Id
[LI]::Locate()
[LI]::LocateDlg()
"frame=$([LI]::frame) dlg=$([LI]::dlg)"

if ([LI]::dlg -ne [IntPtr]::Zero) {
    # API visibility of key controls on their live window
    foreach ($id in 1120,1100,1,1113,1101,1140) {
        $c = [LI]::GetDlgItem([LI]::dlg, $id)
        $v = if ($c -ne [IntPtr]::Zero) { [LI]::IsWindowVisible($c) } else { "NULL" }
        "id=$id vis=$v"
    }
    # bring dialog forward & capture pixels
    [LI]::SetForegroundWindow([LI]::dlg) | Out-Null
    Start-Sleep -Milliseconds 400
    $r = New-Object LI+RECT
    [LI]::GetWindowRect([LI]::dlg,[ref]$r) | Out-Null
    $w=$r.Right-$r.Left; $h=$r.Bottom-$r.Top
    "rect=($($r.Left),$($r.Top)) ${w}x${h}"
    $bmp = New-Object System.Drawing.Bitmap($w,$h)
    $g=[System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left,$r.Top,0,0,(New-Object System.Drawing.Size($w,$h)))
    $bmp.Save("$PWD\out\live_dlg.png",[System.Drawing.Imaging.ImageFormat]::Png)
    function RegionColors([int]$x0,[int]$y0,[int]$x1,[int]$y1){
        $hist=@{}
        for($y=$y0;$y -lt $y1;++$y){for($x=$x0;$x -lt $x1;++$x){$c=$bmp.GetPixel($x,$y);$k="{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B;$hist[$k]=1+$hist[$k]}}
        ($hist.GetEnumerator()|Sort-Object Value -Descending|Select-Object -First 5|ForEach-Object{"{0} x{1}" -f $_.Key,$_.Value}) -join " | "
    }
    # @125%: client design px *1.302; button col x≈326..432,y≈36..61 design -> px*1.302
    $s=125/96.0
    "按钮区:"; RegionColors ([int](326*$s)) ([int](36*$s)) ([int]((326+106)*$s)) ([int]((36+25)*$s))
    "组合框区:"; RegionColors ([int](96*$s)) ([int](38*$s)) ([int]((96+216)*$s)) ([int]((38+20)*$s))
    "页签区:"; RegionColors ([int](6*$s)) ([int](6*$s)) ([int]((6+428)*$s)) ([int]((6+26)*$s))
    $g.Dispose();$bmp.Dispose()
} else {
    "NO DIALOG OPEN — 请按 Ctrl+F 后立刻再跑一次本脚本"
}
