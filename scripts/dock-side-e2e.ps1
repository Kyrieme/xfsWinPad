# dock-side-e2e.ps1 - batch 122: side-dock containers in the REAL app.
#
# 1) deploys test_npp_dock.dll to %APPDATA%\xfsWinPad\plugins
# 2) launches the app; probes plugin menu command ids 10000..10030 to find
#    "Dock Panel" (bottom wrapper appears) and "Side Panels" (right + top
#    wrappers appear) -- both callbacks run in-host so DockedWidgetData
#    pointers marshal
# 3) asserts program-level geometry via GetWindowRect: right wrapper sits
#    right of the editor host, top wrapper between toolbar and editor,
#    bottom wrapper in the bottom chain.
#
# NOTE: keep this file pure ASCII (PS 5.1 reads BOM-less UTF-8 as ANSI).
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [Parameter(Mandatory=$true)][string]$PluginDir
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text;
public class DS {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
  public static uint pid; public static IntPtr frame = IntPtr.Zero;
  public static IntPtr editorHost = IntPtr.Zero;

  public static void Walk() {
    EnumWindows(new EnumProc((h, l) => {
      uint w; GetWindowThreadProcessId(h, out w); if (w != pid) return true;
      StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "xfsWinPadMainWindow") { frame = h; return false; }
      return true; }), IntPtr.Zero);
    if (frame != IntPtr.Zero)
      EnumChildWindows(frame, new EnumProc((h, l) => {
        StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
        if (c.ToString() == "xfsWinPadEditorHost") { editorHost = h; return false; }
        return true; }), IntPtr.Zero);
  }
  public static System.Collections.ArrayList Wrappers() {
    var list = new System.Collections.ArrayList();
    if (frame == IntPtr.Zero) return list;
    EnumChildWindows(frame, new EnumProc((h, l) => {
      StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "xfsWinPadPluginDock") { list.Add(h); }
      return true; }), IntPtr.Zero);
    return list;
  }
}
'@

# script-scope wrapper handles (filled by EnumChildWindows callbacks -- PS
# scriptblocks cannot capture function locals when invoked as Win32 delegates,
# so all callbacks write to $script: variables; see dock-side-diag.ps1 too)
$script:gBottom = [IntPtr]::Zero
$script:gRight  = [IntPtr]::Zero
$script:gTop    = [IntPtr]::Zero

function Get-WrapperTitle([IntPtr]$wrap) {
  $lbl = [DS]::FindWindowExW($wrap, [IntPtr]::Zero, 'STATIC', $null)
  if ($lbl -eq [IntPtr]::Zero) { return '' }
  $t = New-Object System.Text.StringBuilder 128
  [DS]::GetWindowTextW($lbl, $t, 128) | Out-Null
  return $t.ToString()
}
# enumerate wrappers once, filling script-scope handles by title (callbacks
# write here; do NOT rely on captured function locals inside Win32 callbacks)
function Find-AllWrappers {
  $script:gBottom = [IntPtr]::Zero
  $script:gRight  = [IntPtr]::Zero
  $script:gTop    = [IntPtr]::Zero
  # Pass 1 (callback): collect wrapper handles ONLY -- FindWindowExW returns 0
  # when called INSIDE an EnumChildWindows callback (empirical, see diag); keep
  # the callback body minimal like the counting function.
  $script:wrapHandles = New-Object System.Collections.ArrayList
  $cb = [DS+EnumProc]{ param($h, $l)
    $c = New-Object System.Text.StringBuilder 64
    [DS]::GetClassNameW($h, $c, 64) | Out-Null
    if ($c.ToString() -eq 'xfsWinPadPluginDock') { $script:wrapHandles.Add($h) | Out-Null }
    return $true }
  [DS]::EnumChildWindows([DS]::frame, $cb, [IntPtr]::Zero) | Out-Null
  # Pass 2 (outside callback): read labels -- user32 calls work normally here
  Write-Output ("  DBG wrapHandles=" + $script:wrapHandles.Count) | Out-Host
  foreach ($h in $script:wrapHandles) {
    $lbl = [DS]::FindWindowExW($h, [IntPtr]::Zero, 'STATIC', $null)
    $title = ''
    if ($lbl -ne [IntPtr]::Zero) {
      $t = New-Object System.Text.StringBuilder 128
      [DS]::GetWindowTextW($lbl, $t, 128) | Out-Null
      $title = $t.ToString()
    }
    if ($title -eq 'npp-dock')   { $script:gBottom = $h }
    if ($title -eq 'side-right') { $script:gRight  = $h }
    if ($title -eq 'side-top')   { $script:gTop    = $h }
    $cls = ''
    if ($lbl -ne [IntPtr]::Zero) {
      $lc = New-Object System.Text.StringBuilder 64
      [DS]::GetClassNameW($lbl, $lc, 64) | Out-Null
      $cls = $lc.ToString()
    }
    Write-Output ("  DBG pass2 title=[" + $title + "] lbl=" + $lbl + " cls=" + $cls) | Out-Host
  }
}

# ---- deploy + launch ----------------------------------------------------------
$dst = "$env:APPDATA\xfsWinPad\plugins\test_npp_dock.dll"
Copy-Item (Join-Path $PluginDir 'test_npp_dock.dll') $dst -Force
$exe = (Resolve-Path $ExePath).Path
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 2500
Start-Process -FilePath $exe | Out-Null
Start-Sleep -Seconds 7

$app = Get-Process xfsWinPad -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $app) { Write-Output 'FAIL app launched'; exit 1 }
[DS]::pid = [uint32]$app.Id
[DS]::Walk()
Write-Output ("frame=" + [DS]::frame)
if ([DS]::frame -eq [IntPtr]::Zero) { Write-Output 'FAIL frame'; exit 1 }

$pass = 0; $fail = 0
function Check([string]$n, [bool]$ok) {
  if ($ok) { $script:pass++; Write-Output "PASS $n" } else { $script:fail++; Write-Output "FAIL $n" }
}
function Send-Cmd([int]$id) { [DS]::PostMessageW([DS]::frame, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null }

# ---- probe plugin command ids --------------------------------------------------
$bottomFound = $false; $bottomId = 0
foreach ($id in 10000..10030) {
  Send-Cmd $id
  Start-Sleep -Milliseconds 350
  if (([DS]::Wrappers()).Count -ge 1) { $bottomFound = $true; $bottomId = $id; break }
}
Check 'bottom panel via FuncItem cmd' $bottomFound
if (-not $bottomFound) {
  Get-Process xfsWinPad | Stop-Process -Force
  Remove-Item $dst -Force
  exit 1
}

$sideFound = $false
foreach ($id in ($bottomId+1)..($bottomId+3)) {
  Send-Cmd $id
  Start-Sleep -Milliseconds 400
  if (([DS]::Wrappers()).Count -ge 3) { $sideFound = $true; break }
}
Check 'right+top panels via Side Panels cmd' $sideFound

# ---- geometry by classification (no title matching: FindWindowExW proved
# unreliable from this host process; the in-process test_dock asserts exact
# titles -- here we assert the BATCH GOAL: wrappers land in side containers)
Find-AllWrappers
if ($pass -ge 2) {
  function Get-WrapperRect([IntPtr]$wrap) {
    $r = New-Object DS+RECT
    [DS]::GetWindowRect($wrap, [ref]$r) | Out-Null
    return $r
  }
  function Get-EditorHostRect {
    $r = New-Object DS+RECT
    [DS]::GetWindowRect([DS]::editorHost, [ref]$r) | Out-Null
    return $r
  }
  Check 'three wrappers registered' ($script:wrapHandles.Count -eq 3)
  $eh = Get-EditorHostRect
  $nRight = 0; $nTop = 0; $nBottom = 0
  foreach ($h in $script:wrapHandles) {
    $r = Get-WrapperRect $h
    Write-Output ("  WRAP (" + $r.L + "," + $r.T + ")-(" + $r.R + "," + $r.B + ")") | Out-Host
    if ($r.L -ge $eh.R - 2) { $nRight++ }
    elseif ($r.B -le $eh.T + 2 -and ($r.R - $r.L) -gt ($r.B - $r.T)) { $nTop++ }
    elseif ($r.T -ge $eh.B - 2) { $nBottom++ }
  }
  Check 'one wrapper in right column' ($nRight -eq 1)
  Check 'one wrapper in top strip' ($nTop -eq 1)
  Check 'one wrapper in bottom chain' ($nBottom -eq 1)
}

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $dst -Force
Write-Output ("dock-side-e2e: $pass PASS / $fail FAIL")
if ($fail -gt 0) { exit 1 } else { exit 0 }
