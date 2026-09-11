# feature-test.ps1 - end-to-end verification: encoding convert+save, reload,
# bookmark rendering, find/replace dialog presence.
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [string]$OutDir = "out"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FW {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

function Shot([IntPtr]$hwnd, [string]$png) {
    Start-Sleep -Milliseconds 350
    [FW]::SetForegroundWindow($hwnd) | Out-Null
    $r = New-Object FW+RECT
    [FW]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Right-$r.Left; $h = $r.Bottom-$r.Top
    $bmp = New-Object System.Drawing.Bitmap($w,$h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left,$r.Top,0,0,(New-Object System.Drawing.Size($w,$h)))
    $bmp.Save($png,[System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$results = @()
function Pass([string]$n){ $script:results += "PASS $n"; Write-Output "PASS $n" }
function Fail([string]$n){ $script:results += "FAIL $n"; Write-Output "FAIL $n" }

# --- setup -----------------------------------------------------------------
$file = Join-Path $env:TEMP 'xfs_feat.txt'
[System.IO.File]::WriteAllText($file, "alpha beta gamma`r`nsecond line here`r`nthird`r`n")
$logPath = "$env:LOCALAPPDATA\xfsWinPad\logs\xfsWinPad.log"

$p = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2200
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Fail "window-created"; exit 1 }
Pass "window-created"

$WM_COMMAND = 0x0111; $WM_CLOSE = 0x0010
function Cmd([IntPtr]$h,[int]$id){ [FW]::PostMessageW($h,$WM_COMMAND,[IntPtr]$id,[IntPtr]::Zero)|Out-Null }

# --- 1. convert-to UTF-8 BOM then save -> file must gain EF BB BF ----------
Cmd $hwnd 700 # EncConvertFirst + UTF8BOM(=1)... placeholder replaced below

# NOTE: ids computed from CommandIds.h
$EncConvertFirst = 700
$EncReloadAsFirst = 710
$FileSave = 102; $FileReload = 108   # matches CommandIds.h after FilePrint insertion
$BookmarkToggle = 800; $SearchReplace = 403   # opens Search dialog on Replace page

[FW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]($EncConvertFirst+1),[IntPtr]::Zero)|Out-Null  # UTF8 BOM
Start-Sleep -Milliseconds 250
[FW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]$FileSave,[IntPtr]::Zero)|Out-Null
Start-Sleep -Milliseconds 600
$bytes = [System.IO.File]::ReadAllBytes($file)
if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    Pass "convert-bom-save"
} else { Fail "convert-bom-save" }

# --- 2. external change + reload from disk ----------------------------------
[System.IO.File]::WriteAllText($file, "RELOADED-MARKER`r`nline two`r`n", (New-Object System.Text.UTF8Encoding($false)))
[FW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]$FileReload,[IntPtr]::Zero)|Out-Null
Start-Sleep -Milliseconds 600
$tail = Get-Content $logPath -Tail 6
if (($tail -join "`n") -match 'Reloaded') { Pass "reload-from-disk" } else { Fail "reload-from-disk" }

# --- 3. bookmark toggle -> blue glyph rendered in symbol margin -------------
[FW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]$BookmarkToggle,[IntPtr]::Zero)|Out-Null
Shot $hwnd (Join-Path (Get-Location) "$OutDir\feat_bookmark.png")

$bmp = New-Object System.Drawing.Bitmap((Join-Path (Get-Location) "$OutDir\feat_bookmark.png"))
$blueFound = $false
for ($y=90; $y -lt [Math]::Min(320,$bmp.Height); ++$y) {
    for ($x=6; $x -lt 90 -and $x -lt $bmp.Width; ++$x) {
        $c = $bmp.GetPixel($x,$y)
        if ($c.B -gt 150 -and $c.R -lt 90 -and $c.G -lt 130) { $blueFound = $true; break }
    }
    if ($blueFound) { break }
}
$bmp.Dispose()
if ($blueFound) { Pass "bookmark-rendered" } else { Fail "bookmark-rendered" }

# --- 4. find/replace dialog opens -------------------------------------------
[FW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]$SearchReplace,[IntPtr]::Zero)|Out-Null
Start-Sleep -Milliseconds 700

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class EW2 {
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static IntPtr FindByTitle(uint pid, string title){
        IntPtr found = IntPtr.Zero;
        EnumWindows((h,l)=>{uint wpid;GetWindowThreadProcessId(h,out wpid);
            if(wpid==pid && IsWindowVisible(h)){var t=new StringBuilder(256);GetWindowTextW(h,t,256);
                if(t.ToString().Contains(title)){found=h;return false;}}return true;},IntPtr.Zero);
        return found;
    }
}
"@
$dlgHwnd = [EW2]::FindByTitle([uint32]$p.Id, [string][char]0x67E5 + [string][char]0x627E)
if ($dlgHwnd -ne [IntPtr]::Zero) { Pass "replace-dialog-open" } else { Fail "replace-dialog-open" }
Shot $hwnd (Join-Path (Get-Location) "$OutDir\feat_replace.png")
if ($dlgHwnd -ne [IntPtr]::Zero) {
    [FW]::PostMessageW($dlgHwnd,$WM_CLOSE,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
}
Start-Sleep -Milliseconds 400

# --- cleanup -----------------------------------------------------------------
[FW]::PostMessageW($hwnd,$WM_CLOSE,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force; Fail "clean-exit" } else { Pass "clean-exit" }

Remove-Item $file -Force -ErrorAction SilentlyContinue
$failures = ($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Output ("SUMMARY pass={0} fail={1}" -f (($results|Where-Object {$_ -like 'PASS*'}).Count), $failures)
exit ($(if ($failures -gt 0) { 1 } else { 0 }))
