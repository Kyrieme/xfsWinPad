param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\bin\Release\xfsWinPad.exe"))
# Batch 109 e2e: the FEATURE that MainWindow's SCN_UPDATEUI branch exists for.
#
# WHY THIS EXISTS
#   Batch 109 narrowed that branch with a real focus test (ed.Send(SCI_GETFOCUS)),
#   because Scintilla hands every newly constructed Editor a pending Update::Content
#   (Editor.cxx:195) - so a blank document the workspace creates on the LEFT after a
#   ViewMoveToOtherView move emits one SCN_UPDATEUI on its first paint, having never
#   been focused. The handler read it as "the user focused the other split view" and
#   re-pointed Workspace::Active() at the blank document, which made the following
#   ViewMoveToNewView move the WRONG document. scripts\multi-instance-e2e.ps1 case
#   T7 pins that defect. THIS script pins the opposite direction, which T7 cannot:
#
#     with the split up, putting the keyboard focus in the other pane must STILL
#     make the frame title (and Workspace::Active()) follow that pane.
#
#   Narrowing a condition is exactly the change that silently deletes a feature, and
#   a guard that lives outside the repo rots. So this one is in the repo.
#
# WHAT IT DRIVES, AND WHY THAT WAY
#   * A REAL click (SendInput at absolute screen coordinates, via
#     scripts\_common-win32.ps1), never PostMessage(WM_LBUTTONDOWN): the branch is
#     gated on Scintilla's hasFocus, which only a genuine WM_SETFOCUS sets.
#   * WindowFromPoint is checked before clicking, so a click that would land on some
#     other window is reported INCONCLUSIVE instead of being counted as a miss.
#   * SCI_GETFOCUS (2381) is asserted after the click: the probe must prove its own
#     stimulus took effect before it is allowed to judge the title.
#   * SCI_GETCURRENTPOS (2008) is REPORTED, never gated on. An earlier draft made a
#     caret move a hard requirement, reasoning that Editor::SetSelection calls
#     InvalidateSelection (where ContainerNeedsUpdate runs) only when the caret
#     changes. Measurement refuted it: the plain-click path calls InvalidateSelection
#     UNCONDITIONALLY (Editor.cxx:4983), so every click queues an UPDATEUI. Both pane
#     shapes below pass with the caret not moving at all. Rule: follow the call sites,
#     not just the callee's internal condition.
#   * Both SCI_* are by-return-value messages - the only kind a cross-process probe may
#     use. Text is read with WM_GETTEXT (0x000D, marshalled by the OS), never with
#     SCI_GETTEXT (2182), which takes a buffer pointer into the caller's address space.
#
# BOTH PANE SHAPES ARE RUN, because a ViewMoveToOtherView move leaves the left pane as
# either of two different things and both are reachable by the user:
#   M1  one file open   -> the workspace creates a blank "new N" document on the left
#                          (this is the shape the batch 109 defect needed)
#   M2  two files open  -> the left pane keeps the other file
#   Same click/assert code for both; only the launch argument and the expected names
#   differ. A mode that cannot reach the target reports INCONCLUSIVE, never FAIL.
#
# EXIT CODE   0 = VIEWFOCUS-E2E-PASS  (both modes: the title followed the click in both
#                                      directions)
#             1 = VIEWFOCUS-E2E-FAIL  (focus moved, the title did not follow)
#             4 = VIEWFOCUS-E2E-INCONCLUSIVE (a click never reached / never focused -
#                                      NOT a verdict about the product)
#
# The real profile is snapshotted/restored by scripts\_profile-guard.ps1: the app
# resolves its data directory via SHGetKnownFolderPath, so $env:APPDATA cannot
# isolate it. ASCII only.

$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir '_profile-guard.ps1')
. (Join-Path $scriptDir '_common-win32.ps1')

# The PowerShell host does not surface stdout, so every line goes to a file too.
# Truncate by writing without -Append on the first call - never by deleting the file
# first, which the safe-delete hook turns into a terminating error (that is how a
# previous probe ended up reading the run before last as a pass).
$out = Join-Path ([IO.Path]::GetTempPath()) 'xfs-viewfocus-e2e.txt'
$script:n = 0
$script:procs = @()
$script:fail = $false
$script:inconclusive = $false
function Say($m) {
    $line = ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
    if ($script:n -eq 0) { $line | Out-File -FilePath $out -Encoding utf8 }
    else { $line | Out-File -FilePath $out -Encoding utf8 -Append }
    $script:n++
    Write-Output $line
}
function Say-PG([string]$s) { Say $s }
function Kill-App {
    # Windows the app spawned itself are not in $script:procs. Kill by image name -
    # the guard established that no foreign instance was running, so these are ours.
    # Killing (not WM_CLOSE) is deliberate: nothing gets written to the profile.
    foreach ($p in $script:procs) {
        try { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } } catch { }
    }
    $script:procs = @()
    Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
function Cleanup {
    Kill-App
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

if (-not (Test-Path $Exe)) { Die "exe not found: $Exe" }
$sessDir = Get-ProfileDir
if (-not $sessDir) { Die "profile dir not resolved" }
Say ("profile dir = " + $sessDir)

# ---- command ids: parsed, never hardcoded -----------------------------------
$ids = @{}
$idsSrc = Join-Path $root 'src\core\CommandIds.h'
if (-not (Test-Path $idsSrc)) { Die "CommandIds.h not found at $idsSrc" }
foreach ($m in [regex]::Matches((Get-Content $idsSrc -Raw), '(?m)^\s*([A-Za-z_]\w*)\s*=\s*(\d+)\s*,')) {
    $ids[$m.Groups[1].Value] = [int]$m.Groups[2].Value
}
if (-not $ids.ContainsKey('ViewMoveToOtherView')) { Die "CommandIds.h has no ViewMoveToOtherView" }
Say ("ids: ViewMoveToOtherView={0}" -f $ids['ViewMoveToOtherView'])

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class VF {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  public const uint WM_COMMAND = 0x0111, WM_GETTEXT = 0x000D;
  public const uint SCI_GETCURRENTPOS = 2008, SCI_GETFOCUS = 2381;

  // WM_GETTEXT is one of the messages the OS marshals across processes, so the buffer
  // below is filled in the TARGET's space and copied back. This is why the probe must
  // not use SCI_GETTEXT (2182): that one takes a raw pointer and would have the target
  // write into its own address space.
  public static string TextOf(IntPtr h) {
    IntPtr buf = Marshal.AllocHGlobal(4096 * 2);
    try {
      for (int i = 0; i < 4096 * 2; i++) Marshal.WriteByte(buf, i, 0);
      SendMessageW(h, WM_GETTEXT, (IntPtr)4096, buf);
      return Marshal.PtrToStringUni(buf) ?? "";
    } finally { Marshal.FreeHGlobal(buf); }
  }
  public static int FocusOf(IntPtr h) { return (int)SendMessageW(h, SCI_GETFOCUS, IntPtr.Zero, IntPtr.Zero); }
  public static int CaretOf(IntPtr h) { return (int)SendMessageW(h, SCI_GETCURRENTPOS, IntPtr.Zero, IntPtr.Zero); }
  public static string TitleOf(IntPtr h) { var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512); return sb.ToString(); }
  public static IntPtr VisibleEditorIn(IntPtr host) {
    IntPtr after = IntPtr.Zero;
    while (true) {
      IntPtr e = FindWindowExW(host, after, "Scintilla", null);
      if (e == IntPtr.Zero) return IntPtr.Zero;
      if (IsWindowVisible(e)) return e;
      after = e;
    }
  }
}
"@

# ---- fixtures ----------------------------------------------------------------
$work = Join-Path ([IO.Path]::GetTempPath()) ("xfs-viewfocus-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$f1 = Join-Path $work "alpha1.txt"
$f2 = Join-Path $work "beta2.txt"
[IO.File]::WriteAllText($f1, "one`r`n")
[IO.File]::WriteAllText($f2, "two`r`n")

# ---- helpers -----------------------------------------------------------------
function Rect-Of($h) {
    # [Activator] rather than New-Object -TypeName: the struct is a NESTED type and
    # the "VF+RECT" string form is not resolved by New-Object on PS 5.1.
    $r = [Activator]::CreateInstance([VF+RECT])
    [void][VF]::GetWindowRect($h, [ref]$r)
    return $r
}
function Hosts-Of($frame) {
    # both editor hosts share one window class; the split makes a second one visible.
    # Left/right is decided by the screen x of their rects, which is what "the other
    # pane" means to the user.
    $list = @()
    $after = [IntPtr]::Zero
    while ($true) {
        $h = [VF]::FindWindowExW($frame, $after, "xfsWinPadEditorHost", $null)
        if ($h -eq [IntPtr]::Zero) { break }
        if ([VF]::IsWindowVisible($h)) { $list += $h }
        $after = $h
    }
    return ,@($list)
}
function Click-Into($ed, [int]$dx, [int]$dy) {
    # Report whether the click can even reach the target: WindowFromPoint is the same
    # query the OS uses to route it.
    $r = Rect-Of $ed
    $px = $r.Left + $dx
    $py = $r.Top + $dy
    $pt = [Activator]::CreateInstance([WIN+POINT])
    $pt.x = $px; $pt.y = $py
    $hit = [WIN]::WindowFromPoint($pt)
    if ($hit -ne $ed) {
        Say ("  click target check FAILED: WindowFromPoint(" + $px + "," + $py + ") = " + $hit +
             " (class " + [WIN]::ClassOf($hit) + "), expected " + $ed)
        return $false
    }
    [WIN]::Click($px, $py)
    Start-Sleep -Milliseconds 700
    return $true
}
function Focus-Click($ed, [string]$label) {
    # Returns $true only when the stimulus is proven to have taken effect (the editor
    # really owns the focus). The caret is reported, not required - see the header.
    $caret0 = [VF]::CaretOf($ed)
    if (-not (Click-Into $ed 110 30)) { return $false }
    $foc = [VF]::FocusOf($ed)
    $caret1 = [VF]::CaretOf($ed)
    Say ("  " + $label + ": focus=" + $foc + " caret " + $caret0 + " -> " + $caret1)
    if ($foc -ne 1) {
        Say ("  " + $label + ": the synthetic click did not give the editor the focus")
        return $false
    }
    return $true
}

# One full mode: launch -> split -> click the left pane -> click it back.
# Sets $script:fail / $script:inconclusive instead of exiting, so both modes run.
function Invoke-Mode([bool]$blankLeft) {
    $tag = if ($blankLeft) { "M1 blank-left" } else { "M2 two-files" }
    Say ("---- " + $tag + " ----")
    if (@(Get-Process xfsWinPad -ErrorAction SilentlyContinue).Count -ne 0) {
        Say ("  PRECONDITION-STRAY: an xfsWinPad process is already running")
        $script:inconclusive = $true
        return
    }

    $argLine = if ($blankLeft) { "`"$f1`"" } else { "`"$f1`" `"$f2`"" }
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru
    $script:procs += $p
    $dl = (Get-Date).AddSeconds(60)
    while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
        Start-Sleep -Milliseconds 300; $p.Refresh()
    }
    if ($p.MainWindowHandle -eq 0) { Say "  no main window appeared"; $script:inconclusive = $true; return }
    Start-Sleep -Milliseconds 1800

    [WIN]::pid = [uint32]$p.Id
    [WIN]::LocateFrame()
    $frame = [WIN]::frame
    if ($frame -eq [IntPtr]::Zero) { Say "  main window not found by class scan"; $script:inconclusive = $true; return }
    Say ("  frame = " + $frame + " title = [" + [VF]::TitleOf($frame) + "]")

    # ---- bring the split up ------------------------------------------------
    [void][VF]::PostMessageW($frame, [VF]::WM_COMMAND, [IntPtr]$ids['ViewMoveToOtherView'], [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(15)
    $hosts = @()
    while ((Get-Date) -lt $dl) {
        $hosts = Hosts-Of $frame
        if ($hosts.Count -ge 2) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($hosts.Count -lt 2) { Say ("  after 456 only " + $hosts.Count + " visible editor host(s)"); $script:inconclusive = $true; return }
    Start-Sleep -Milliseconds 1200

    $sorted = @($hosts | Sort-Object { (Rect-Of $_).Left })
    $leftEd = [VF]::VisibleEditorIn($sorted[0])
    $rightEd = [VF]::VisibleEditorIn($sorted[$sorted.Count - 1])
    if ($leftEd -eq [IntPtr]::Zero -or $rightEd -eq [IntPtr]::Zero) {
        Say "  an editor host has no visible Scintilla child"; $script:inconclusive = $true; return
    }
    $leftText = [VF]::TextOf($leftEd)
    $rightText = [VF]::TextOf($rightEd)
    Say ("  left  ed=" + $leftEd + " text=[" + $leftText.Trim() + "]")
    Say ("  right ed=" + $rightEd + " text=[" + $rightText.Trim() + "]")

    # What the split must look like. The moved tab is the one that was ACTIVE in the
    # left view: with two files the left pane keeps the other one, with one file the
    # workspace creates a blank document there instead.
    $movedName  = if ($blankLeft) { 'alpha1' } else { 'beta2' }
    $leftExpect = if ($blankLeft) { 'new ' }  else { 'alpha1' }
    if ($blankLeft) {
        if ($leftText.Trim() -ne '') { Say "  expected a BLANK left pane"; $script:inconclusive = $true; return }
        if ($rightText -notmatch 'one') { Say "  the moved alpha1.txt is not in the right pane"; $script:inconclusive = $true; return }
    } else {
        if ($leftText -notmatch 'one' -or $rightText -notmatch 'two') {
            Say ("  unexpected split state (left=[" + $leftText.Trim() + "] right=[" + $rightText.Trim() + "])")
            $script:inconclusive = $true
            return
        }
    }

    $title0 = [VF]::TitleOf($frame)
    Say ("  title after 456 = [" + $title0 + "]")
    if ($title0 -notmatch $movedName) {
        # This is the batch 109 defect itself (T7 in multi-instance-e2e.ps1 also pins it).
        Say ("  after 456 the title must name the moved document " + $movedName + ".txt")
        $script:fail = $true
        return
    }

    # ---- focus + click into the LEFT pane; the title must follow it ---------
    [void][WIN]::SetForegroundWindow($frame)
    Start-Sleep -Milliseconds 500
    if (-not (Focus-Click $leftEd 'click LEFT')) {
        Say ("  " + $tag + ": could not put the focus in the left pane")
        $script:inconclusive = $true
        return
    }
    Start-Sleep -Milliseconds 600
    $title1 = [VF]::TitleOf($frame)
    Say ("  after clicking LEFT: title=[" + $title1 + "]")
    if ($title1 -notmatch $leftExpect) {
        Say ("  the title does not name the left pane's document (" + $leftExpect + ")")
        $script:fail = $true
        return
    }

    # ---- and back into the RIGHT pane --------------------------------------
    if (-not (Focus-Click $rightEd 'click RIGHT')) {
        Say ("  " + $tag + ": could not put the focus back in the right pane")
        $script:inconclusive = $true
        return
    }
    Start-Sleep -Milliseconds 600
    $title2 = [VF]::TitleOf($frame)
    Say ("  after clicking RIGHT: title=[" + $title2 + "]")
    if ($title2 -notmatch $movedName) {
        Say ("  the title does not name the right pane's document (" + $movedName + ")")
        $script:fail = $true
        return
    }
    Say ("  " + $tag + "-OK the title followed the focus in both directions")
}

try {
    Invoke-Mode $true
    Kill-App
    Invoke-Mode $false

    if ($script:fail) {
        Say "VIEWFOCUS-E2E-FAIL"
        Cleanup
        exit 1
    }
    if ($script:inconclusive) {
        Say "VIEWFOCUS-E2E-INCONCLUSIVE"
        Cleanup
        exit 4
    }
    Say "VIEWFOCUS-E2E-PASS"
    Cleanup
    exit 0
} catch {
    Die $_.Exception.Message
}
