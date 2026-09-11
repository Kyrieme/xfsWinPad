# find-replace-test.ps1 - functional verification of the NP++-style search suite:
# tabbed Find/Replace dialog, find-next, find-all-in-open-files results panel,
# double-click locate path, and replace-all-in-open-files.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class FRT {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h,string s);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    public static uint targetPid;
    public static IntPtr frameW, editorW, dialogW, panelListW;

    private static bool FrameScan(IntPtr h, IntPtr lp){
        uint wpid; GetWindowThreadProcessId(h,out wpid);
        if(wpid==targetPid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){ frameW=h; return false; } }
        return true; }
    private static bool EditorScan(IntPtr h, IntPtr lp){
        uint wpid; GetWindowThreadProcessId(h,out wpid);
        if(wpid==targetPid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){
                EnumChildWindows(h,(ch,l2)=>{ var cc=new StringBuilder(64); GetClassNameW(ch,cc,64);
                    if(cc.ToString()=="Scintilla"){ editorW=ch; return false; } return true; },IntPtr.Zero);
                if(editorW!=IntPtr.Zero) return false; } }
        return true; }
    private static bool DialogScan(IntPtr h, IntPtr lp){
        uint wpid; GetWindowThreadProcessId(h,out wpid);
        if(wpid==targetPid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadFindDlg"){ dialogW=h; return false; } }
        return true; }

    public static void Locate(){
        frameW=IntPtr.Zero; editorW=IntPtr.Zero; dialogW=IntPtr.Zero; panelListW=IntPtr.Zero;
        EnumWindows(new EnumProc(FrameScan),IntPtr.Zero);
        EnumWindows(new EnumProc(EditorScan),IntPtr.Zero); }
    public static void LocateDialog(){ dialogW=IntPtr.Zero; EnumWindows(new EnumProc(DialogScan),IntPtr.Zero); }
    public static void LocatePanelList(){
        panelListW=IntPtr.Zero;
        if(frameW==IntPtr.Zero) return;
        EnumChildWindows(frameW,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="SysListView32"){ panelListW=h; return false; } return true; },IntPtr.Zero); }
    public static int ChildCount(){ int n=0;
        if(dialogW!=IntPtr.Zero) EnumChildWindows(dialogW,(h,l)=>{n++;return true;},IntPtr.Zero);
        return n; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    public static int KidsOutside(IntPtr dlg){ int bad=0;
        RECT dr; GetWindowRect(dlg,out dr);
        EnumChildWindows(dlg,(h,l)=>{ RECT r; GetWindowRect(h,out r);
            if(r.Left<dr.Left||r.Top<dr.Top||r.Right>dr.Right-6||r.Bottom>dr.Bottom-6) bad++;
            return true; },IntPtr.Zero);
        return bad; }
    public static IntPtr ComboEdit(IntPtr combo){
        IntPtr ed=IntPtr.Zero;
        EnumChildWindows(combo,(h,l)=>{ var c=new StringBuilder(32); GetClassNameW(h,c,32);
            if(c.ToString()=="Edit"){ ed=h; return false; } return true; },IntPtr.Zero);
        return ed; }
    public static void TypeInto(IntPtr edit, string s){
        foreach(char ch in s){ PostMessageW(edit,0x0102,(IntPtr)ch,IntPtr.Zero); } }
    public static void ClearAndType(IntPtr edit, string s){
        SendMessageW(edit,0x00B1 /*EM_SETSEL*/,IntPtr.Zero,(IntPtr)(-1));
        foreach(char ch in s){ PostMessageW(edit,0x0102,(IntPtr)ch,IntPtr.Zero); } }
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static IntPtr ActiveEditor(IntPtr frame){
        IntPtr r=IntPtr.Zero;
        EnumChildWindows(frame,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="Scintilla" && IsWindowVisible(h)){ r=h; } return true; },IntPtr.Zero);
        return r; }
}
"@

$file = Join-Path $env:TEMP 'xfs_fr.txt'
[System.IO.File]::WriteAllText($file, "alpha beta`r`ngamma alpha`r`ndelta`r`n")
$p = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2200
[FRT]::targetPid = [uint32]$p.Id
[FRT]::Locate()
$WM_COMMAND = 0x0111
$SCI_GETLENGTH = 2006
$SCI_GETSELECTIONSTART = 2143
$SCI_GETSELECTIONEND = 2145
$SCI_SETSELECTION = 2160
$LVM_GETITEMCOUNT = 0x1004
$WM_APP_SEARCHACT = 0x8009
$WM_APP_GOTOHIT   = 0x800B

$failures = 0
function Pass([string]$m){ Write-Output "PASS $m" }
function Fail([string]$m){ Write-Output "FAIL $m"; $script:failures++ }

# open search dialog on the FIND page (Cmd::SearchFind = 400)
[FRT]::PostMessageW([FRT]::frameW, $WM_COMMAND, [IntPtr]400, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
[FRT]::LocateDialog()
"kids=$([FRT]::ChildCount())"
if ([FRT]::ChildCount() -ge 14) { Pass "dialog-controls" } else { Fail "dialog-controls" }

$outside = [FRT]::KidsOutside([FRT]::dialogW)
if ($outside -eq 0) { Pass "controls-fit" } else { Fail "controls-fit ($outside outside)" }

# --- visibility: tab strip + active-page controls visible, inactive hidden ---
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class VIS {
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
}
"@
$tabsVis  = [VIS]::IsWindowVisible([FRT]::GetDlgItem([FRT]::dialogW, 1120))
$findVis  = [VIS]::IsWindowVisible([FRT]::GetDlgItem([FRT]::dialogW, 1100))
$replVis  = [VIS]::IsWindowVisible([FRT]::GetDlgItem([FRT]::dialogW, 1130))
"visible: tabs=$tabsVis findCombo=$findVis replCombo(hidden expected False)=$replVis"
if ($tabsVis -and $findVis -and -not $replVis) { Pass "page-visibility" } else { Fail "page-visibility (tabs=$tabsVis find=$findVis replVisible=$replVis)" }

# diagnostics: locate find combo by ID vs by class
$cById = [FRT]::GetDlgItem([FRT]::dialogW, 1100)
"diag getdlg1100=$cById"

# --- find next in current document ---
$cById = [FRT]::GetDlgItem([FRT]::dialogW, 1100)
$comboEdit = [FRT]::ComboEdit($cById)
[FRT]::TypeInto($comboEdit, 'gamma')
Start-Sleep -Milliseconds 400
$ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(64)
[FRT]::SendMessageW($comboEdit, 0x000D, [IntPtr]64, $ptr) | Out-Null
"diag edit text=[" + [System.Runtime.InteropServices.Marshal]::PtrToStringUni($ptr) + "]"
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 200
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
$selEnd = [int][FRT]::SendMessageW([FRT]::editorW, $SCI_GETSELECTIONEND, [IntPtr]::Zero, [IntPtr]::Zero)
"find-next selEnd=$selEnd (expect 17)"
if ($selEnd -eq 17) { Pass "find-next" } else { Fail "find-next" }

# --- find all in all open documents -> results panel ---
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1115, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
[FRT]::LocatePanelList()
$rows = [int][FRT]::SendMessageW([FRT]::panelListW, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
"panel rows=$rows (expect 1)"
if ([FRT]::panelListW -ne [IntPtr]::Zero -and $rows -eq 1) { Pass "findall-open-panel" } else { Fail "findall-open-panel" }

# --- activate row 0 (same path as double-click) -> selection at match ---
if ([FRT]::panelListW -ne [IntPtr]::Zero -and $rows -ge 1) {
    [FRT]::PostMessageW([FRT]::frameW, $WM_APP_GOTOHIT, [IntPtr]0, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    $s = [int][FRT]::SendMessageW([FRT]::editorW, $SCI_GETSELECTIONSTART, [IntPtr]::Zero, [IntPtr]::Zero)
    $e = [int][FRT]::SendMessageW([FRT]::editorW, $SCI_GETSELECTIONEND, [IntPtr]::Zero, [IntPtr]::Zero)
    "locate sel=[$s,$e] (expect [12,17])"
    if ($s -eq 12 -and $e -eq 17) { Pass "result-locate" } else { Fail "result-locate" }
}

# --- replace all in all open documents ('gamma' -> 'GG') ---
[FRT]::ClearAndType($comboEdit, 'gamma')
$replEdit = [FRT]::ComboEdit([FRT]::GetDlgItem([FRT]::dialogW, 1110))
[FRT]::ClearAndType($replEdit, 'GG')
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1116, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900

# dismiss the summary MessageBox (owned #32770 titled xfsWinPad)
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class MBX {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    public static uint pid;
    public static void CloseInfoBox(){ EnumWindows((h,l)=>{ uint w;GetWindowThreadProcessId(h,out w);
        if(w==pid){ var c=new StringBuilder(32);GetClassNameW(h,c,32);
            var t=new StringBuilder(64);GetWindowTextW(h,t,64);
            if(c.ToString()=="#32770" && t.ToString()=="xfsWinPad"){ PostMessageW(h,0x0010,IntPtr.Zero,IntPtr.Zero); return false; } }
        return true; },IntPtr.Zero); }
}
"@
[MBX]::pid=[uint32]$p.Id
[MBX]::CloseInfoBox()
Start-Sleep -Milliseconds 500
$len = [int][FRT]::SendMessageW([FRT]::editorW, $SCI_GETLENGTH, [IntPtr]::Zero, [IntPtr]::Zero)
"replace-all-open len=$len (expect 29)"
if ($len -eq 29) { Pass "replace-all-open" } else { Fail "replace-all-open" }

# --- five tabs present ---
$tabsH = [IntPtr]::Zero
[FRT]::EnumChildWindows([FRT]::dialogW, { param($h,$l)
    $sb = New-Object System.Text.StringBuilder 64
    [FRT]::GetClassNameW($h,$sb,64) | Out-Null
    if ($sb.ToString() -eq 'SysTabControl32') { $script:tabsH = $h; return $false }
    return $true }, [IntPtr]::Zero) | Out-Null
$tabCount = [int][FRT]::SendMessageW($tabsH, 0x1304, [IntPtr]::Zero, [IntPtr]::Zero)
"tab pages=$tabCount (expect 5)"
if ($tabCount -eq 5) { Pass "five-pages" } else { Fail "five-pages" }

$WM_APP_SETPAGE = 0x800C

# --- hit-test: tab strip must NOT cover page widgets ---
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class HIT {
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
    public struct RECT { public int Left,Top,Right,Bottom; }
    public struct POINT { public int x,y; }
}
"@
foreach ($id in 1100,1) {
    $c = [FRT]::GetDlgItem([FRT]::dialogW, $id)
    $r = New-Object HIT+RECT
    [HIT]::GetWindowRect($c,[ref]$r)|Out-Null
    $pt = New-Object HIT+POINT
    $pt.x=[int](($r.Left+$r.Right)/2); $pt.y=[int](($r.Top+$r.Bottom)/2)
    $top = [HIT]::WindowFromPoint($pt)
    $sb = New-Object System.Text.StringBuilder 64
    [HIT]::GetClassNameW($top,$sb,64)|Out-Null
    $cls = $sb.ToString()
    $ok = if ($id -eq 1100) { $cls -eq 'ComboBox' -or $cls -eq 'Edit' } else { $cls -eq 'Button' }
    "hit id=$id top=$cls"
    if ($ok) { Pass "hit-test-$id" } else { Fail "hit-test-$id (tab covers controls)" }
}

# --- 文件中查找 (page 2): dedicated temp folder with one matching file ---
$fifDir = Join-Path $env:TEMP 'xfs_fif'
New-Item -ItemType Directory -Force $fifDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $fifDir 'fif_sample.txt'), "hello`r`ngamma world`r`n")
Remove-Item (Join-Path $fifDir 'fif_sample.txt') -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText((Join-Path $fifDir 'fif_sample.txt'), "hello`r`ngamma world`r`n")
[FRT]::PostMessageW([FRT]::dialogW, $WM_APP_SETPAGE, [IntPtr]2, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$dirEdit = [FRT]::GetDlgItem([FRT]::dialogW, 1140)
"diag dirEdit=$dirEdit"
[FRT]::ClearAndType($dirEdit, $fifDir)
$fifEdit = [FRT]::ComboEdit([FRT]::GetDlgItem([FRT]::dialogW, 1144))
[FRT]::ClearAndType($fifEdit, 'gamma')
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1145, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
[FRT]::LocatePanelList()
$rows = [int][FRT]::SendMessageW([FRT]::panelListW, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
"fif rows=$rows (expect 1)"
if ($rows -eq 1) { Pass "find-in-files-panel" } else { Fail "find-in-files-panel" }

# locate row 0 -> opens the closed file at line 2 AND selects the keyword
if ($rows -ge 1) {
    [FRT]::PostMessageW([FRT]::frameW, $WM_APP_GOTOHIT, [IntPtr]0, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    $ed2 = [FRT]::ActiveEditor([FRT]::frameW)
    $pos = [int][FRT]::SendMessageW($ed2, 2008, [IntPtr]::Zero, [IntPtr]::Zero)
    $lineNo = [int][FRT]::SendMessageW($ed2, 2166, [IntPtr]$pos, [IntPtr]::Zero)
    $ss = [int][FRT]::SendMessageW($ed2, $SCI_GETSELECTIONSTART, [IntPtr]::Zero, [IntPtr]::Zero)
    $se = [int][FRT]::SendMessageW($ed2, $SCI_GETSELECTIONEND, [IntPtr]::Zero, [IntPtr]::Zero)
    "fif locate line=$lineNo sel=[$ss,$se] (expect line=1 sel=[7,12])"
    if ($lineNo -eq 1 -and $ss -eq 7 -and $se -eq 12) { Pass "find-in-files-locate" } else { Fail "find-in-files-locate" }
}

# --- panel close button hides the dock ---
$panelH = [IntPtr]::Zero
if ([FRT]::panelListW -ne [IntPtr]::Zero) {
    $panelH = [FRT]::GetParent([FRT]::panelListW)
    [FRT]::PostMessageW($panelH, $WM_COMMAND, [IntPtr]1202, [IntPtr]::Zero) | Out-Null   # ✕ 关闭
    Start-Sleep -Milliseconds 400
    $stillVis = [FRT]::IsWindowVisible($panelH)
    "panel visible after close=$stillVis (expect False)"
    if (-not $stillVis) { Pass "panel-close" } else { Fail "panel-close" }
} else { Fail "panel-close (no panel)" }

# --- 标记 (page 4): bookmark lines containing 'gamma', then clear ---
[FRT]::PostMessageW([FRT]::dialogW, $WM_APP_SETPAGE, [IntPtr]4, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$markEdit = [FRT]::ComboEdit([FRT]::GetDlgItem([FRT]::dialogW, 1160))
[FRT]::ClearAndType($markEdit, 'gamma')
$edNow = [FRT]::ActiveEditor([FRT]::frameW)
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1164, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
[MBX]::CloseInfoBox()
Start-Sleep -Milliseconds 400
$mask = [int][FRT]::SendMessageW($edNow, 2046, [IntPtr]1, [IntPtr]::Zero)   # SCI_MARKERGET line2(0b)
"mark mask=$mask (expect bit3=8)"
if (($mask -band 8) -eq 8) { Pass "mark-all" } else { Fail "mark-all" }
[FRT]::PostMessageW([FRT]::dialogW, $WM_COMMAND, [IntPtr]1165, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 400
$mask2 = [int][FRT]::SendMessageW($edNow, 2046, [IntPtr]1, [IntPtr]::Zero)
"after clear mask=$mask2"
if ($mask2 -eq 0) { Pass "mark-clear" } else { Fail "mark-clear" }

# --- cleanup ---
if (-not $p.HasExited) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 600
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait("n")
    if (-not $p.WaitForExit(4000)) { Stop-Process -Id $p.Id -Force }
}
Remove-Item $file -Force -ErrorAction SilentlyContinue
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })
