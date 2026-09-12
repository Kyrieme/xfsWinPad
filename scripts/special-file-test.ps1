# special-file-test.ps1 - CLI dispatch: .xfm loads as macro, theme json gets
# imported (copied to themes\), plain .json stays a normal document.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

$log = Join-Path $env:LOCALAPPDATA 'xfsWinPad\logs\xfsWinPad.log'
$themesDir = Join-Path $env:APPDATA 'xfsWinPad\themes'

# --- fixture: a valid .xfm (chars 'h' 'i') and a theme json ---
$tmp = Join-Path $env:TEMP 'xfs_special'
New-Item -ItemType Directory -Force $tmp | Out-Null
$xfm = Join-Path $tmp 'hello.xfm'
"C 104`nC 105`n" | Out-File $xfm -Encoding ascii
$theme = Join-Path $tmp 'MyTheme.json'
@'
{
  "name": "MyTheme",
  "editorBg": "#201010",
  "editorFg": "#F0E0E0"
}
'@ | Out-File $theme -Encoding ascii
$plain = Join-Path $tmp 'plain.json'
'{ "hello": "world" }' | Out-File $plain -Encoding ascii

# clean themes dir target
Remove-Item (Join-Path $themesDir 'MyTheme.json') -Force -ErrorAction SilentlyContinue

# --- 1) .xfm: launched via CLI -> macro loaded, NOT opened as a document ---
$p = Start-Process -FilePath $ExePath -ArgumentList "--new `"$xfm`"" -PassThru
Start-Sleep -Seconds 3
$tail = Get-Content $log -Tail 20
$m = $tail | Select-String -Pattern "Macro loaded" | Select-Object -Last 1
"macro log: $m"
if ("$m" -match "Macro loaded: .*hello\.xfm events=2") { Pass "xfm-macro-loaded" }
else { Fail "xfm-macro-loaded" }
if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 400

# --- 2) theme json: launched via CLI -> imported into themes\, not a doc ---
$p2 = Start-Process -FilePath $ExePath -ArgumentList "--new `"$theme`"" -PassThru
Start-Sleep -Seconds 3
$dst = Join-Path $themesDir 'MyTheme.json'
if (Test-Path $dst) { Pass "theme-imported-to-dir" } else { Fail "theme-imported-to-dir" }
$tail2 = Get-Content $log -Tail 15
$t = $tail2 | Select-String -Pattern "Theme imported" | Select-Object -Last 1
"theme log: $t"
if ("$t" -match "Theme imported") { Pass "theme-import-log" } else { Fail "theme-import-log" }
# theme import pops a MessageBox - dismiss
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
Start-Sleep -Milliseconds 400
if (-not $p2.HasExited) { $p2.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p2.HasExited) { Stop-Process -Id $p2.Id -Force }
Start-Sleep -Milliseconds 400

# --- 3) plain json: stays a normal document (no import) ---
$p3 = Start-Process -FilePath $ExePath -ArgumentList "--new `"$plain`"" -PassThru
Start-Sleep -Seconds 3
$tail3 = Get-Content $log -Tail 12
$o = $tail3 | Select-String -Pattern "Opened .*plain\.json" | Select-Object -Last 1
"plain log: $o"
if ("$o" -match "Opened .*plain\.json") { Pass "plain-json-stays-document" }
else { Fail "plain-json-stays-document" }
if (-not $p3.HasExited) { $p3.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p3.HasExited) { Stop-Process -Id $p3.Id -Force }

# --- 4) explorer-panel double-click dispatch (OpenUserFile route) ---
# Project root from CLI + persisted explorerVisible forced true, then a real
# WM_LBUTTONDBLCLK on tree rows: .xfm row -> macro loaded (no document tab),
# plain.json row -> normal document open. Settings/session restored afterwards.
$proj = Join-Path $env:TEMP 'xfs_special_explorer'
New-Item -ItemType Directory -Force $proj | Out-Null
Copy-Item $xfm (Join-Path $proj 'double.xfm') -Force
'{ "hello": "world" }' | Set-Content (Join-Path $proj 'plain.json') -Encoding ascii

$settingsFile = Join-Path $env:APPDATA 'xfsWinPad\settings.json'
$sessionFile = Join-Path $env:APPDATA 'xfsWinPad\session.json'
$settingsBak = "$settingsFile.e2ebak"
Copy-Item $settingsFile $settingsBak -Force
$sessionBak = $null
if (Test-Path $sessionFile) { $sessionBak = "$sessionFile.e2ebak"; Copy-Item $sessionFile $sessionBak -Force }
$txt = [IO.File]::ReadAllText($settingsFile, [Text.Encoding]::UTF8)
if ($txt -match '"explorerVisible":\s*false') {
    $txt = $txt -replace '"explorerVisible":\s*false', '"explorerVisible": true'
    [IO.File]::WriteAllText($settingsFile, $txt, (New-Object Text.UTF8Encoding($false)))
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class E42 {
  public delegate bool EnumChildProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")]
  public static extern bool EnumChildWindows(IntPtr p, EnumChildProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
  [DllImport("user32.dll")]
  public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  public static IntPtr Found;
  public static EnumChildProc KeepAlive;   // pin the delegate
  public static IntPtr FindTree(IntPtr parent) {
    Found = IntPtr.Zero;
    KeepAlive = new EnumChildProc(Scan);
    EnumChildWindows(parent, KeepAlive, IntPtr.Zero);
    return Found;
  }
  static bool Scan(IntPtr h, IntPtr l) {
    var sb = new StringBuilder(64);
    GetClassNameW(h, sb, 64);
    if (sb.ToString() == "SysTreeView32") { Found = h; return false; }
    return true;
  }
}
"@
. (Join-Path $PSScriptRoot '_common-win32.ps1')
[WIN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
# Orphan instances from earlier sections would swallow clicks (wrong window).
Get-Process xfsWinPad -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }
Start-Sleep -Milliseconds 300

$p4 = Start-Process -FilePath $ExePath -ArgumentList "--new `"$proj`"" -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 500
    $p4.Refresh(); $hwnd = $p4.MainWindowHandle
}
$tree = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $tree -eq [IntPtr]::Zero; $i++) {
    $tree = [E42]::FindTree($hwnd)
    if ($tree -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($tree -ne [IntPtr]::Zero) { Pass "explorer-tree-found" } else { Fail "explorer-tree-found" }

function DbClickTree([IntPtr]$treeH, [IntPtr]$mainH, [int]$row, [int]$rh) {
    # Real mouse double-click (SendInput): synthesized WM_LBUTTONDBLCLK is not
    # enough for the common control to fire NM_DBLCLK. rh is physical pixels.
    $r = New-Object WIN+RECT
    [void][WIN]::GetWindowRect($treeH, [ref]$r)
    # x=60: label hit area starts after icon+gap; x<=42 falls in the gap and
    # the click lands on nothing (TVHT_ONITEM only covers icon/label).
    $px = $r.Left + 60
    $py = $r.Top + [int](($row + 0.5) * $rh)
    # Guard: the app may sit behind other windows; clicks would fall through.
    for ($t = 0; $t -lt 6; $t++) {
        $pt = New-Object WIN+POINT; $pt.x = $px; $pt.y = $py
        if ([WIN]::WindowFromPoint($pt) -eq $treeH) { [WIN]::DoubleClick($px, $py); return $true }
        [void][WIN]::SetForegroundWindow($mainH)
        Start-Sleep -Milliseconds 400
    }
    "dblclick guard: point $px,$py not on tree"
    return $false
}
# TVM_GETITEMCOUNT is unreliable cross-process here; poll TVM_GETNEXTITEM
# (TVGN_ROOT) for item existence instead. TV_FIRST=0x1100: NEXTITEM=+10,
# GETITEMHEIGHT=+28.
$root = [IntPtr]::Zero
for ($i = 0; $i -lt 12 -and $root -eq [IntPtr]::Zero; $i++) {
    $root = [E42]::SendMessageW($tree, 0x110A, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($root -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
$rh = [int][long][E42]::SendMessageW($tree, 0x111C, [IntPtr]1, [IntPtr]::Zero)
"tree root=$root rowH=$rh"
if ($root -ne [IntPtr]::Zero -and $rh -gt 0) { Pass "explorer-tree-populated" }
else { Fail "explorer-tree-populated" }

# row1 = double.xfm (dir rows would precede files; this root has only files)
[void][WIN]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400
[void](DbClickTree $tree $hwnd 1 $rh)
Start-Sleep -Seconds 2
$tail4 = Get-Content $log -Tail 30
$m4 = $tail4 | Select-String -Pattern "Macro loaded" | Select-Object -Last 1
"dblclick xfm log: $m4"
if ("$m4" -match "Macro loaded: .*double\.xfm events=2") { Pass "explorer-dblclick-xfm-macro" }
else { Fail "explorer-dblclick-xfm-macro" }
$doc4 = $tail4 | Select-String -Pattern "Opened .*double\.xfm" | Select-Object -Last 1
if (-not $doc4) { Pass "explorer-xfm-stays-non-document" } else { Fail "explorer-xfm-stays-non-document" }

# row2 = plain.json -> normal document
[void](DbClickTree $tree $hwnd 2 $rh)
Start-Sleep -Seconds 2
$tail5 = Get-Content $log -Tail 15
$o5 = $tail5 | Select-String -Pattern "Opened .*plain\.json" | Select-Object -Last 1
"dblclick json log: $o5"
if ("$o5" -match "Opened .*plain\.json") { Pass "explorer-dblclick-json-document" }
else { Fail "explorer-dblclick-json-document" }

if (-not $p4.HasExited) { $p4.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 800 }
if (-not $p4.HasExited) { Stop-Process -Id $p4.Id -Force }
Start-Sleep -Milliseconds 400
Copy-Item $settingsBak $settingsFile -Force; Remove-Item $settingsBak -Force -ErrorAction SilentlyContinue
if ($sessionBak) { Copy-Item $sessionBak $sessionFile -Force; Remove-Item $sessionBak -Force -ErrorAction SilentlyContinue }
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:TEMP 'xfs_probe') -Recurse -Force -ErrorAction SilentlyContinue

# cleanup
Remove-Item (Join-Path $themesDir 'MyTheme.json') -Force -ErrorAction SilentlyContinue
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })

