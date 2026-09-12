# _common-win32.ps1 - dot-source me: Win32 helpers for UI automation probes.
# Usage:  . $PSScriptRoot\_common-win32.ps1 ;  [WIN]::pid = <pid> ;  [WIN]::LocateFrame() ...
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class WIN {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x,y; }
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h,string s);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);

    public static bool AnyTooltip(){
        bool found=false;
        EnumWindows(new EnumProc((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid && IsWindowVisible(h)){
                var c=new StringBuilder(32); GetClassNameW(h,c,32);
                if(c.ToString()=="tooltips_class32"){ found=true; return false; } }
            return true; }), IntPtr.Zero);
        return found; }

    public static uint pid;
    public static IntPtr frame=IntPtr.Zero, dlg=IntPtr.Zero, list=IntPtr.Zero;

    private static bool FrameScan(IntPtr h, IntPtr lp){
        uint w; GetWindowThreadProcessId(h,out w);
        if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){ frame=h; return false; } }
        return true; }
    private static bool DlgScan(IntPtr h, IntPtr lp){
        uint w; GetWindowThreadProcessId(h,out w);
        if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadFindDlg"){ dlg=h; return false; } }
        return true; }
    private static bool ListScan(IntPtr h, IntPtr lp){
        var c=new StringBuilder(64); GetClassNameW(h,c,64);
        if(c.ToString()=="SysListView32"){ list=h; return false; }
        return true; }

    public static void LocateFrame(){ frame=IntPtr.Zero;
        EnumWindows(new EnumProc(FrameScan),IntPtr.Zero); }
    public static void LocateDlg(){ dlg=IntPtr.Zero;
        if(frame!=IntPtr.Zero) EnumWindows(new EnumProc(DlgScan),IntPtr.Zero); }
    public static void LocateList(){ list=IntPtr.Zero;
        if(frame!=IntPtr.Zero) EnumChildWindows(frame,new EnumProc(ListScan),IntPtr.Zero); }
    public static string ClassOf(IntPtr h){ var c=new StringBuilder(64); GetClassNameW(h,c,64); return c.ToString(); }
    public static IntPtr ActiveEditor(){
        IntPtr r=IntPtr.Zero;
        if(frame==IntPtr.Zero) return r;
        EnumChildWindows(frame,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="Scintilla" && IsWindowVisible(h)){ r=h; } return true; },IntPtr.Zero);
        return r; }
    public static IntPtr ComboEdit(IntPtr combo){
        IntPtr ed=IntPtr.Zero;
        if(combo==IntPtr.Zero) return ed;
        EnumChildWindows(combo,(h,l)=>{ var c=new StringBuilder(32); GetClassNameW(h,c,32);
            if(c.ToString()=="Edit"){ ed=h; return false; } return true; },IntPtr.Zero);
        return ed; }
    public static void TypeInto(IntPtr edit, string s){
        foreach(char ch in s) PostMessageW(edit,0x0102,(IntPtr)ch,IntPtr.Zero); }
    public static void ClearAndType(IntPtr edit, string s){
        SendMessageW(edit,0x00B1 /*EM_SETSEL*/,IntPtr.Zero,(IntPtr)(-1));
        foreach(char ch in s) PostMessageW(edit,0x0102,(IntPtr)ch,IntPtr.Zero); }
    public static void MoveTo(int px,int py){
        // MOUSEEVENTF_MOVE|ABSOLUTE|VIRTUALDESK; note ABSOLUTE is 0x8000.
        const int SM_XV=76, SM_YV=77, SM_CXV=78, SM_CYV=79;
        int vx=GetSystemMetrics(SM_XV), vy=GetSystemMetrics(SM_YV);
        int cx=GetSystemMetrics(SM_CXV), cy=GetSystemMetrics(SM_CYV);
        INPUT mv=NewInput();
        mv.u.mi.dwFlags=0x0001|0x8000|0x4000;
        mv.u.mi.dx=(int)((px-vx)*65536.0/cx); mv.u.mi.dy=(int)((py-vy)*65536.0/cy);
        INPUT[] a=new INPUT[]{mv}; SendInput(1,a,Marshal.SizeOf(typeof(INPUT))); }
    public static void Click(int px,int py){
        MoveTo(px,py);
        INPUT d=NewInput(); d.u.mi.dwFlags=0x0002;
        INPUT u=NewInput(); u.u.mi.dwFlags=0x0004;
        SendInput(1,new INPUT[]{d},Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(40);
        SendInput(1,new INPUT[]{u},Marshal.SizeOf(typeof(INPUT))); }
    public static void DoubleClick(int px,int py){
        MoveTo(px,py); System.Threading.Thread.Sleep(120);
        Click(px,py); System.Threading.Thread.Sleep(60); Click(px,py); }
    static INPUT NewInput(){
        var i=new INPUT(); i.type=0; i.u.mi.dx=0; i.u.mi.dy=0; i.u.mi.mouseData=0;
        i.u.mi.dwFlags=0; i.u.mi.time=0; i.u.mi.dwExtraInfo=IntPtr.Zero; return i; }
}
"@
