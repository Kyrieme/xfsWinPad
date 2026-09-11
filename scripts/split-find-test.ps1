# split-find-test.ps1 - split-view "all open documents" find/replace scope test:
# alpha.txt stays in the LEFT view (needle x1), beta.txt moved to the RIGHT view
# (needle x2). Then:
#   1. FindAllOpen must list 3 hits (old bug: only left view was scanned = 1 hit)
#   2. ReplaceAllOpen must rewrite BOTH editors (3 replacements)
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class SFT {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    public static uint pid;
    public static IntPtr frameW, panelListW, dlgW;
    public static void Locate(){
        frameW=IntPtr.Zero; panelListW=IntPtr.Zero; dlgW=IntPtr.Zero;
        EnumWindows((h,l)=>{
            uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid){
                var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="xfsWinPadMainWindow" && frameW==IntPtr.Zero) frameW=h;
                if(c.ToString()=="xfsWinPadFindDlg" && dlgW==IntPtr.Zero) dlgW=h;
            }
            return true; },IntPtr.Zero);
        if(frameW!=IntPtr.Zero)
            EnumChildWindows(frameW,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="SysListView32"){ panelListW=h; return false; } return true; },IntPtr.Zero);
    }
    public static System.Collections.Generic.List<IntPtr> Editors(){
        var list = new System.Collections.Generic.List<IntPtr>();
        if(frameW==IntPtr.Zero) return list;
        EnumChildWindows(frameW,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="Scintilla") list.Add(h); return true; },IntPtr.Zero);
        return list;
    }
    public static IntPtr ComboEdit(IntPtr combo){
        IntPtr edit=IntPtr.Zero;
        EnumChildWindows(combo,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="Edit"){ edit=h; return false; } return true; },IntPtr.Zero);
        return edit; }
}
"@
$WM_COMMAND = 0x0111
$WM_SETTEXT = 0x000C
$WM_GETTEXT = 0x000D
$LVM_GETITEMCOUNT = 0x1004
function SetTxt($h, $s) {
    $p = [Runtime.InteropServices.Marshal]::StringToHGlobalUni($s)
    [SFT]::SendMessageW($h, $WM_SETTEXT, [IntPtr]0, $p) | Out-Null
    [Runtime.InteropServices.Marshal]::FreeHGlobal($p)
}

# --- fixture files ---
$tmp = Join-Path $env:TEMP 'xfs_splitfind'
New-Item -ItemType Directory -Force $tmp | Out-Null
$alpha = Join-Path $tmp 'alpha.txt'
$beta  = Join-Path $tmp 'beta.txt'
[System.IO.File]::WriteAllText($alpha, "alpha line1`r`nneedle alpha`r`nalpha line3`r`n")
[System.IO.File]::WriteAllText($beta,  "beta line1`r`nneedle beta`r`nneedle beta2`r`nbeta line4`r`n")

# --- launch both files, move the 2nd doc to the right view ---
$p = Start-Process -FilePath $ExePath -ArgumentList "`"$alpha`" `"$beta`"" -PassThru
Start-Sleep -Seconds 3
[SFT]::pid = $p.Id
[SFT]::Locate()
if ([SFT]::frameW -eq [IntPtr]::Zero) { Fail "launch"; exit 1 } else { Pass "launch" }

# activate doc index 1 (WindowDocFirst+1 = 7201), then move to other view (456)
[SFT]::PostMessageW([SFT]::frameW, $WM_COMMAND, [IntPtr]7201, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
[SFT]::PostMessageW([SFT]::frameW, $WM_COMMAND, [IntPtr]456, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
$edCount = [SFT]::Editors().Count
"editor windows: $edCount (expect >=2 after split)"
if ($edCount -ge 2) { Pass "split-created" } else { Fail "split-created" }

# --- open find dialog (SearchFind=400), fill "needle", fire FindAllOpen (1115) ---
[SFT]::PostMessageW([SFT]::frameW, $WM_COMMAND, [IntPtr]400, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
[SFT]::Locate()
$dlg = [SFT]::dlgW
if ($dlg -eq [IntPtr]::Zero) { Fail "find-dialog-open" } else { Pass "find-dialog-open" }

$combo = [SFT]::GetDlgItem($dlg, 1100)
$edit = [SFT]::ComboEdit($combo)
if ($edit -eq [IntPtr]::Zero) { Fail "find-combo-edit" } else {
    SetTxt $edit "needle"
    Start-Sleep -Milliseconds 250
    [SFT]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]1115, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1300
    [SFT]::Locate()
    if ([SFT]::panelListW -eq [IntPtr]::Zero) {
        Fail "results-panel"
    } else {
        $rows = [int][SFT]::SendMessageW([SFT]::panelListW, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
        "FindAllOpen rows=$rows (expect 3 = alpha x1 + beta x2)"
        if ($rows -eq 3) { Pass "findall-open-both-views" } else { Fail "findall-open-both-views" }
    }
}

# --- ReplaceAllOpen across both views: switch to replace page, replace needle->found ---
$WM_APP_SETPAGE = 0x800C
[SFT]::PostMessageW($dlg, $WM_APP_SETPAGE, [IntPtr]1, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
$comboR = [SFT]::GetDlgItem($dlg, 1130)
$editR = [SFT]::ComboEdit($comboR)
$replEdit = [SFT]::GetDlgItem($dlg, 1110)
if ($editR -eq [IntPtr]::Zero -or $replEdit -eq [IntPtr]::Zero) {
    Fail "replace-page-controls"
} else {
    SetTxt $editR "needle"
    SetTxt $replEdit "found"
    Start-Sleep -Milliseconds 250
    # IDC_BTN_REPLOPEN = 1116
    [SFT]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]1116, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1500
    # dismiss the modal summary MessageBox: find #32770 owned by our pid and click IDOK
    Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class SFT4 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    public static IntPtr FindBox(uint pid){
        IntPtr f=IntPtr.Zero;
        EnumWindows((h,l)=>{ uint w; GetWindowThreadProcessId(h,out w);
            if(w==pid && f==IntPtr.Zero){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
                if(c.ToString()=="#32770"){ f=h; return false; } } return true; },IntPtr.Zero);
        return f; }
}
"@
    $box = [SFT4]::FindBox($p.Id)
    if ($box -ne [IntPtr]::Zero) {
        [SFT4]::SendMessageW($box, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null  # WM_COMMAND IDOK
        Start-Sleep -Milliseconds 500
    }
    # verify by re-running FindAllOpen: with every "needle" replaced the panel must show 0 rows
    [SFT]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]1115, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1300
    [SFT]::Locate()
    $rows2 = -1
    if ([SFT]::panelListW -ne [IntPtr]::Zero) {
        $rows2 = [int][SFT]::SendMessageW([SFT]::panelListW, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    "post-replace FindAllOpen rows=$rows2 (expect 0: both views rewritten)"
    if ($rows2 -eq 0) { Pass "replace-all-open-both-views" } else { Fail "replace-all-open-both-views" }
}

# --- cleanup ---
if (-not $p.HasExited) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 600
    if (-not $p.WaitForExit(3000)) { Stop-Process -Id $p.Id -Force }
}
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })

