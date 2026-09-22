param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 108 e2e: multi-window session slots, close disposition, fan-out, and the
# two tab-context-menu moves that share the window machinery.
#
# WHAT THE CLOSE-DISPOSITION RULE SAYS (added in batch 108)
#   A non-primary window leaves a session slot behind ONLY when it is the last
#   live window, i.e. when closing it means the app is exiting. Closing one
#   window while another is still open must leave NOTHING behind - otherwise the
#   next launch restores it as a phantom window, and because the restored child
#   claims the slot and writes a fresh one on its own exit, that phantom is
#   self-perpetuating (only the 30-day GC bounded it).
#   The primary window keeps writing session.json either way: it is the canonical
#   session and the window a later launch restores as "the main window".
#
# CASES
#   T1  close the SECONDARY first (primary still alive) -> NO slot may appear
#   T2  close the PRIMARY first, then the secondary (last) -> its slot MUST appear
#   T3  relaunch: primary restores session.json + fans out one child per slot
#   T4  ViewMoveToNewView (457)   - move the tab into a NEW window, item 1007
#   T5  ViewMoveToOtherView (456) - in-process split, tab menu item 1006
#   T6  File > New Window (112)   - one more blank process
#
# T4/T5 exist because the tab context menu wires ids 1006/1007 straight to
# Workspace::MoveActiveToOtherView / MainWindow::MoveCurrentToNewWindow, and the
# session change must not have disabled either. They are driven through the View
# menu commands (identical handlers) because a real right-click cannot be
# synthesised in this sandbox; the ids are parsed out of CommandIds.h, never
# hardcoded. T1/T2 pin the close ORDER on purpose: the rule is order-dependent
# by design, so the test must not leave it to Get-Process ordering.
#
# The real profile is snapshotted/restored by scripts\_profile-guard.ps1: the app
# resolves its data directory via SHGetKnownFolderPath, so pointing $env:APPDATA
# at a scratch dir does NOT isolate it. ASCII only.
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir '_profile-guard.ps1')

# The PowerShell host does not surface stdout, so every line goes to a file too.
# Truncate by writing without -Append on the first call - never by deleting the
# file first, which the safe-delete hook turns into a terminating error (that is
# how a previous probe ended up reading the run before last as a pass).
$out = Join-Path ([IO.Path]::GetTempPath()) 'xfs-multi-instance-e2e.txt'
$script:n = 0
$script:procs = @()
function Say($m) {
    $line = ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
    if ($script:n -eq 0) { $line | Out-File -FilePath $out -Encoding utf8 }
    else { $line | Out-File -FilePath $out -Encoding utf8 -Append }
    $script:n++
    Write-Output $line
}
# The guard reports through Say-PG, which writes to stdout only. Route it into
# the transcript too, or its "PROFILE-GUARD-DIRTY: <path>" verdict - the one line
# that tells you a phantom window was planted - is the first thing lost.
function Say-PG([string]$s) { Say $s }
function Kill-Launched {
    foreach ($p in $script:procs) {
        try { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } } catch { }
    }
}
function Cleanup {
    Kill-Launched
    # Windows the app spawned itself (fan-out children, moved-to-new-window) are
    # not in $script:procs. Kill by image name - the guard already established
    # that no foreign instance was running, so these are all ours. Killing (not
    # WM_CLOSE) is deliberate: nothing gets written to the profile.
    Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    Stop-ProfileGuard
}
function Die($m) {
    Say ("E2E-FAIL: " + $m)
    Cleanup
    exit 1
}

Say ("=== run {0} exe={1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Exe)
Start-ProfileGuard
trap { Say ("PROFILE-GUARD-TRAP: " + $_.Exception.Message); Cleanup; exit 1 }

$sessDir = Get-ProfileDir
if (-not (Test-Path $Exe)) { Die "exe not found: $Exe" }
if (-not $sessDir -or -not (Test-Path $sessDir)) { Die "profile dir not found: [$sessDir]" }
Say ("profile dir = " + $sessDir)

# ---- command ids: parsed, never hardcoded -----------------------------------
$ids = @{}
$idsSrc = Join-Path $root 'src\core\CommandIds.h'
if (-not (Test-Path $idsSrc)) { Die "CommandIds.h not found at $idsSrc" }
foreach ($m in [regex]::Matches((Get-Content $idsSrc -Raw), '(?m)^\s*([A-Za-z_]\w*)\s*=\s*(\d+)\s*,')) {
    $ids[$m.Groups[1].Value] = [int]$m.Groups[2].Value
}
foreach ($need in @('FileNewWindow', 'ViewMoveToOtherView', 'ViewMoveToNewView')) {
    if (-not $ids.ContainsKey($need)) { Die "CommandIds.h has no $need" }
}
Say ("ids: FileNewWindow={0} ViewMoveToOtherView={1} ViewMoveToNewView={2}" -f
     $ids['FileNewWindow'], $ids['ViewMoveToOtherView'], $ids['ViewMoveToNewView'])

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MI {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  public const uint WM_COMMAND = 0x0111, WM_CLOSE = 0x0010;
}
"@

# ---- fixtures ----------------------------------------------------------------
$work = Join-Path ([IO.Path]::GetTempPath()) ("xfs-multi-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$f1 = Join-Path $work "alpha1.txt"
$f2 = Join-Path $work "beta2.txt"
[IO.File]::WriteAllText($f1, "one`r`n")
[IO.File]::WriteAllText($f2, "two`r`n")

# ---- helpers -----------------------------------------------------------------
function Assert-NoStrays($where) {
    $alive = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue)
    if ($alive.Count -ne 0) {
        Die ("PRECONDITION-STRAY at " + $where + ": " + $alive.Count +
             " xfsWinPad process(es) alive; the probe's first launch would not be PRIMARY")
    }
}
function Wait-Main($p) {
    $dl = (Get-Date).AddSeconds(60)
    while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
        Start-Sleep -Milliseconds 300
        $p.Refresh()
    }
    if ($p.MainWindowHandle -eq 0) { Die "no main window for pid=$($p.Id)" }
    Start-Sleep -Milliseconds 1500
}
function Launch([string]$argLine) {
    if ($argLine -eq '') { $p = Start-Process -FilePath $Exe -PassThru }
    else { $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru }
    $script:procs += $p
    Wait-Main $p
    return $p
}
function Close-One($p) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq 0) { Die "pid=$($p.Id) has no main window to close" }
    # SendMessage (not Post) so the save path has finished before we look at disk
    [void][MI]::SendMessageW($h, [MI]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(30)
    while (-not $p.HasExited -and (Get-Date) -lt $dl) {
        Start-Sleep -Milliseconds 250
        $p.Refresh()
    }
    if (-not $p.HasExited) { Die "pid=$($p.Id) did not exit after WM_CLOSE" }
    Start-Sleep -Milliseconds 700
}
function Slot-Of($p) { Join-Path $sessDir ("session-" + $p.Id + ".json") }
function Session-Text {
    $f = Join-Path $sessDir 'session.json'
    if (-not (Test-Path $f)) { return "" }
    return (Get-Content $f -Raw)
}

try {
    # ---- T1: close the secondary first -> it must NOT be remembered ---------
    Assert-NoStrays "T1"
    Say "T1: two windows (primary=f1, secondary=f2); close the SECONDARY first"
    $p1 = Launch ("`"$f1`"")
    $p2 = Launch ("--new `"$f2`"")
    $slot2 = Slot-Of $p2
    Close-One $p2
    if (Test-Path $slot2) {
        Die ("T1: closing a window while another was alive left " + $slot2 +
             " behind - the next launch would restore it as a phantom window")
    }
    Say "T1-OK window closed while the primary was alive left no slot"
    Close-One $p1
    if ((Session-Text) -notlike "*alpha1.txt*") {
        Die "T1: primary did not leave session.json holding f1"
    }
    Say "T1-OK primary wrote session.json on its way out"

    # ---- T2: close the primary first, the secondary last -> slot appears ----
    Assert-NoStrays "T2"
    Say "T2: two windows; close the PRIMARY first, then the secondary (now last)"
    $p3 = Launch ("`"$f1`"")
    $p4 = Launch ("--new `"$f2`"")
    $slot4 = Slot-Of $p4
    Close-One $p3
    if ((Session-Text) -notlike "*alpha1.txt*") {
        Die "T2: session.json lacks f1 after the primary closed"
    }
    Close-One $p4
    if (-not (Test-Path $slot4)) {
        Die ("T2: the LAST window left no slot (" + $slot4 +
             ") - a single-window-session would be lost on exit")
    }
    if ((Get-Content $slot4 -Raw) -notlike "*beta2.txt*") {
        Die "T2: the last window's slot lacks f2"
    }
    Say "T2-OK the last window left its own slot holding f2"

    # ---- T3: relaunch -> restore session.json + fan out one child per slot --
    Assert-NoStrays "T3"
    Say "T3: relaunch restores session.json and fans the slot out to its own window"
    $p5 = Launch ''
    $dl = (Get-Date).AddSeconds(40)
    $titles = @()
    while ((Get-Date) -lt $dl) {
        $titles = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue |
                    ForEach-Object { $_.MainWindowTitle })
        if ($titles.Count -ge 2) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($titles.Count -ne 2) { Die ("T3: fan-out produced " + $titles.Count + " window(s), expected 2") }
    if (-not (($titles -join '|') -match 'alpha1.txt')) { Die "T3: no window restored f1" }
    if (-not (($titles -join '|') -match 'beta2.txt')) { Die "T3: no window restored f2" }
    if (Test-Path $slot4) { Die "T3: the child did not claim (delete) its slot" }
    Say "T3-OK primary restored f1 and the child window restored f2"
    $p5.Refresh()
    $frame = $p5.MainWindowHandle
    if ($frame -eq 0) { Die "T3: primary lost its window" }

    # ---- T4: ViewMoveToNewView (457) - move the tab into a NEW window ------
    # Tab context menu item 1007 -> Activate(idx) + host_.MoveCurrentToNewWindow().
    # Driven here through the View menu command, which reaches the same handler.
    # Observable: a new xfsWinPad process whose main window title names the file
    # that was moved out (T3 already proved that technique on this same app).
    # MUST run while the document sits in the view that is current - after a 456
    # move the workspace keeps a fresh blank doc on the left and the title (and
    # currentView_) follow THAT doc, so a following 457 would move the blank one.
    # That is pre-existing behaviour in Workspace, unrelated to the session work.
    $before = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue).Count
    [void][MI]::PostMessageW($frame, [MI]::WM_COMMAND, [IntPtr]$ids['ViewMoveToNewView'], [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(40)
    $moved = $false
    while (-not $moved -and (Get-Date) -lt $dl) {
        $now = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue)
        if (@($now | Where-Object { $_.MainWindowTitle -match 'alpha1.txt' }).Count -gt 0) { $moved = $true }
        if (-not $moved) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $moved) {
        # A modal "save changes?" dialog DISABLES the owner frame. Log it, so
        # "the probe was blocked" can never be reported as "the feature is dead".
        $dump = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue |
                  ForEach-Object { "pid=$($_.Id) title=[$($_.MainWindowTitle)]" }) -join ' ; '
        Say ("T4 dump: before=$before frameEnabled=" + [MI]::IsWindowEnabled($frame) + " | " + $dump)
        Die "T4: ViewMoveToNewView did not open a new window holding f1 (tab menu 1007 is dead)"
    }
    Say "T4-OK MoveCurrentToNewWindow still opens the document in a new window"

    # ---- T5: ViewMoveToOtherView (456) - in-process split ------------------
    # Tab context menu item 1006 -> Activate(idx) + MoveActiveToOtherView().
    # Fresh window so the split state is known; observable is the split divider
    # child window becoming visible.
    Say "T5: fresh window; ViewMoveToOtherView must open the split"
    $p6 = Launch ("--new `"$f2`"")
    $frame6 = $p6.MainWindowHandle
    $div = [MI]::FindWindowExW($frame6, [IntPtr]::Zero, "xfsWinPadSplitViewDivider", $null)
    if ($div -eq [IntPtr]::Zero) { Die "T5: no split-view divider child found" }
    if ([MI]::IsWindowVisible($div)) { Die "T5: PRE-EXISTING split - cannot judge the move" }
    Say "T5: divider hidden before the move (precondition OK)"
    [void][MI]::PostMessageW($frame6, [MI]::WM_COMMAND, [IntPtr]$ids['ViewMoveToOtherView'], [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(10)
    while (-not [MI]::IsWindowVisible($div) -and (Get-Date) -lt $dl) { Start-Sleep -Milliseconds 200 }
    if (-not [MI]::IsWindowVisible($div)) {
        Die "T5: ViewMoveToOtherView did not open the split (tab menu 1006 is dead)"
    }
    Say "T5-OK MoveActiveToOtherView still splits the view"

    # ---- T6: File > New Window (112) ---------------------------------------
    $before = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue).Count
    [void][MI]::PostMessageW($frame, [MI]::WM_COMMAND, [IntPtr]$ids['FileNewWindow'], [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(40)
    $n = $before
    while ($n -le $before -and (Get-Date) -lt $dl) {
        Start-Sleep -Milliseconds 500
        $n = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue).Count
    }
    if ($n -le $before) { Die ("T6: New Window spawned nothing (n=$n, was $before)") }
    Say "T6-OK File > New Window spawned a blank extra process"

    Say "MULTIINSTANCE-E2E-PASS"
    Cleanup
    exit 0
} catch {
    Die $_.Exception.Message
}
