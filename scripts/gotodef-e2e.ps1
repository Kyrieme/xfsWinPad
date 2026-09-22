param(
    [string]$Exe = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [string]$FixtureDir = ""
)
# gotodef-e2e.ps1 - "Go to Definition" (F12 / View menu) must really land on the
# .dec declaration of the symbol under the caret.
#
# Why this exists
# ---------------
# Batch 104 added the feature. The kernel half (finding the declaration line and
# column inside a .dec) is covered by test_chromadiag plus the real-corpus
# regression. What no unit test can reach is the half the user touches: take the
# word under the caret, decide which .dec to look in, open it, put the caret on
# the declaration and select it - and, when there is no declaration, do nothing
# visible and say so. That segment only exists inside a live window.
#
# Why the fixture is a .pat and not a .pln
# ----------------------------------------
# SET_DEC_FILE lives in the pattern file in every real project (checked against
# the vendor sample projects kept in the local, untracked corpus: the .pln files
# are GBK blobs, the .pat files carry `SET_DEC_FILE "./x_pin.dec"` on line 1).
# The feature accepts both Plan and Pattern, so .pat is the realistic carrier.
#
# What is asserted
# ----------------
#   A. channel self-proof: two independent messages agree (SCI_GETLENGTH equals the
#      length of the text read back with WM_GETTEXT), and the text really is the
#      fixture that was written to disk
#   B. NEGATIVE FIRST (the .pat is still the active document at that point): with
#      the caret on a word that is not declared anywhere, the command must not
#      open anything, must not move any caret, and must not create any selection
#   C. positive: with the caret on MCLK the command opens pins.dec, and the
#      selection is exactly the 4 bytes of "MCLK", on the same 0-based line AND at
#      the same 0-based column as in the file on disk
#   D. the document count grows by exactly one (the .dec was opened, not silently
#      focused on something else)
#
# Precondition: the app must be FRESH (only the .pat open, no .dec). The probe
# refuses to judge otherwise - see the comment above the negative case for the
# measured false failure that made this necessary.
#
# Order matters: the negative case has to run while the .pat is still active, so
# it runs before the positive one. Doing it the other way round would need a tab
# click to get back, and a probe that depends on tab geometry fails for reasons
# that have nothing to do with the feature.
#
# Switches
# --------
#   -FixtureOnly      write the fixture, print the .pat path, and stop.
#   -AttachPid N      drive the already-running instance N. With 0 (the default)
#                     the script looks for a process whose Path is this repo's
#                     dev build, so a user instance is never touched.
#   -FixtureDir DIR   override the fixture location.
#
# Launching the app is deliberately NOT done here: Start-Process is unusable in
# this host (it kills the session). Start it from Bash as a background task with
# the fixture path as its argument, then attach.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root 'build\bin\Release\xfsWinPad.exe' }
if ($FixtureDir -eq "") { $FixtureDir = Join-Path ([IO.Path]::GetTempPath()) 'xfs_gotodef_e2e' }
$out = Join-Path ([IO.Path]::GetTempPath()) 'gotodef_e2e_out.txt'

[IO.File]::WriteAllText($out, "")
function Say([string]$s) {
    Write-Output $s
    [IO.File]::AppendAllText($out, $s + "`r`n")
}
function Die([string]$code, [string]$why) {
    Say ("VERDICT: " + $code)
    Say ("  " + $why)
    Stop-ProfileGuard
    exit 1
}

Say ("=== gotodef-e2e " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===")

# ---- 1. ids, parsed out of the headers (never hardcoded) --------------------
$cmdH = Join-Path $root 'src\core\CommandIds.h'
$cmdText = [IO.File]::ReadAllText($cmdH)
if ($cmdText -notmatch 'GotoDefinition\s*=\s*(\d+)') {
    Die 'PRECONDITION-CMDID' "GotoDefinition is not an explicit value in $cmdH"
}
$CMD_GOTODEF = [int]$Matches[1]

$sciH = Join-Path $root 'third_party\scintilla\include\Scintilla.h'
$sciText = [IO.File]::ReadAllText($sciH)
$sci = @{}
foreach ($n in @('SCI_GETLENGTH','SCI_GETCURRENTPOS','SCI_GOTOPOS','SCI_GETSELECTIONSTART',
                 'SCI_GETSELECTIONEND','SCI_SETSEL','SCI_LINEFROMPOSITION')) {
    if ($sciText -notmatch ("#define\s+" + $n + "\s+(\d+)")) {
        Die 'PRECONDITION-SCIID' "$n not found in Scintilla.h"
    }
    $sci[$n] = [int]$Matches[1]
}
Say ("ids: GotoDefinition=" + $CMD_GOTODEF + " SCI_GOTOPOS=" + $sci['SCI_GOTOPOS'] +
     " SCI_GETSELECTIONEND=" + $sci['SCI_GETSELECTIONEND'])

# ---- 2. fixture --------------------------------------------------------------
# pins.dec: MCLK / WG0 / DRDY declared in a PIN_LIST, same shape as the real
# vendor files (name = ate-channel = dut# = type ;). MCLK is on 1-based line 4,
# 0-based column 2 - the probe recomputes both from this text, so the numbers
# below are the fixture's own truth and not a copy of the app's opinion.
$decLines = @(
    'PIN_LIST  (E2E_Site)',
    '{',
    '//=*name= ATE channel   = dut#= type  *=//',
    '  MCLK    =   40  =  1  = IO  ;  //CLK',
    '  WG0     =   39  =  2  = IO  ;  //IN',
    '  DRDY    =   38  =  3  = IO  ;  //OUT',
    '}',
    ''
)
$decText = ($decLines -join "`r`n")
$patLines = @(
    'SET_DEC_FILE "./pins.dec"',
    '',
    'HEADER MCLK,WG0,DRDY;',
    '',
    'NOSUCHPIN = 1;',
    ''
)
$patText = ($patLines -join "`r`n")

[void][IO.Directory]::CreateDirectory($FixtureDir)
$decPath = Join-Path $FixtureDir 'pins.dec'
$patPath = Join-Path $FixtureDir 't.pat'
$ascii = New-Object System.Text.ASCIIEncoding
[IO.File]::WriteAllText($decPath, $decText, $ascii)
[IO.File]::WriteAllText($patPath, $patText, $ascii)
Say ("fixture: " + $patPath + " (" + ([IO.File]::ReadAllBytes($patPath)).Length + " bytes)")
Say ("fixture: " + $decPath + " (" + ([IO.File]::ReadAllBytes($decPath)).Length + " bytes)")

# The fixture's own answer key, computed here and nowhere else.
function LineOf([string]$t, [int]$idx) { return ($t.Substring(0, $idx) -split "`n").Count - 1 }
function ColOf([string]$t, [int]$idx) {
    $ls = $t.LastIndexOf("`n", [Math]::Max($idx - 1, 0))
    return $idx - ($ls + 1)
}
$decNameIdx = $decText.IndexOf('MCLK')
$decLine = LineOf $decText $decNameIdx
$decCol = ColOf $decText $decNameIdx
Say ("answer key (from the file text): MCLK at 0-based line " + $decLine + ", col " + $decCol)
if ($decLine -ne 3 -or $decCol -ne 2) {
    Die 'PRECONDITION-ANSWERKEY' "fixture moved: expected line 3 col 2, got $decLine/$decCol"
}

if ($FixtureOnly) {
    Say "FIXTURE-ONLY"
    Say ("PAT=" + $patPath)
    exit 0
}
# ---- 2b. profile guard -------------------------------------------------------
# A probe must not leave session state in the real %APPDATA%. A surviving
# session-<pid>.json is picked up by the next launch, which spawns one extra
# window for it (--new --no-restore --restore <slot>); that child writes its own
# slot when it exits, so ONE stray slot is a PERMANENT phantom window.
# -NoKill: the caller already started the app and this script only attaches.
. (Join-Path $root 'scripts\_profile-guard.ps1')
Start-ProfileGuard -NoKill
trap { Say-PG ("PROFILE-GUARD-TRAP: " + $_.Exception.Message); Stop-ProfileGuard; exit 1 }


# ---- 3. Win32 helpers --------------------------------------------------------
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class G {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);

    public static uint pid;
    public static IntPtr frame = IntPtr.Zero;
    public static List<IntPtr> eds = new List<IntPtr>();

    private static bool FrameCb(IntPtr h, IntPtr l) {
        uint w; GetWindowThreadProcessId(h, out w);
        if (w == pid) { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
            if (c.ToString() == "xfsWinPadMainWindow") { frame = h; return false; } }
        return true;
    }
    public static void LocateFrame() { frame = IntPtr.Zero; EnumWindows(new EnumProc(FrameCb), IntPtr.Zero); }

    private static bool EdCb(IntPtr h, IntPtr l) {
        var c = new StringBuilder(64); GetClassNameW(h, c, 64);
        if (c.ToString() == "Scintilla") eds.Add(h);
        return true;
    }
    public static void ListEditors() {
        eds = new List<IntPtr>();
        if (frame != IntPtr.Zero) EnumChildWindows(frame, new EnumProc(EdCb), IntPtr.Zero);
    }
    // WM_GETTEXT is one of the few messages USER32 marshals across processes, so
    // reading a whole document this way is safe (unlike SCI_GETSELTEXT, which
    // takes an output buffer and would write into the caller's heap).
    public static string Doc(IntPtr h) {
        int len = (int)SW(h, 0x000E, IntPtr.Zero, null);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 4);
        SW(h, 0x000D, (IntPtr)(len + 1), sb);
        return sb.ToString();
    }
}
"@

# ---- 4. attach ---------------------------------------------------------------
if ($AttachPid -le 0) {
    $mine = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue |
              Where-Object { $_.Path -like (Join-Path $root 'build\bin\Release\*') })
    if ($mine.Count -eq 0) {
        Die 'PRECONDITION-NOPROC' "no dev-build xfsWinPad is running; start it from Bash with '$patPath' and pass -AttachPid"
    }
    if ($mine.Count -gt 1) {
        Die 'PRECONDITION-MULTIPROC' ("several dev-build instances: " + (($mine | ForEach-Object { $_.Id }) -join ','))
    }
    $AttachPid = $mine[0].Id
}
[G]::pid = [uint32]$AttachPid
[G]::LocateFrame()
if ([G]::frame -eq [IntPtr]::Zero) { Die 'PRECONDITION-FRAME' "no xfsWinPadMainWindow owned by pid $AttachPid" }
Say ("attached: pid=" + $AttachPid + " frame=0x" + ([G]::frame).ToString('X'))

# ---- 5. find the editors by their TEXT (never by z-order or visibility) ------
function Editors() { [G]::ListEditors(); return [G]::eds }
function FindEd([scriptblock]$pred) {
    foreach ($e in [G]::eds) { if (& $pred ([G]::Doc($e))) { return $e } }
    return [IntPtr]::Zero
}
$deadline = (Get-Date).AddSeconds(15)
$patEd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline -and $patEd -eq [IntPtr]::Zero) {
    [void](Editors)
    $patEd = FindEd { param($t) $t.Contains('SET_DEC_FILE') -and $t.Contains('NOSUCHPIN') }
    if ($patEd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 400 }
}
if ($patEd -eq [IntPtr]::Zero) { Die 'PRECONDITION-PATED' "no editor holds the .pat fixture (did the app get the file argument?)" }

$patLive = [G]::Doc($patEd)
$patFile = [IO.File]::ReadAllText($patPath)
# Channel self-proof: the length message and the text message must agree, and the
# text must be the fixture. Without this the offsets below are unfalsifiable.
$patSciLen = [int][G]::S($patEd, [uint32]$sci['SCI_GETLENGTH'], [IntPtr]::Zero, [IntPtr]::Zero)
Say ("channel: pat WM_GETTEXT chars=" + $patLive.Length + " SCI_GETLENGTH=" + $patSciLen +
     " file bytes=" + $patFile.Length)
if ($patSciLen -ne $patLive.Length) { Die 'PRECONDITION-CHANNEL' "SCI_GETLENGTH and WM_GETTEXT disagree" }
if (-not $patLive.Contains('HEADER MCLK,WG0,DRDY;')) { Die 'PRECONDITION-CHANNEL' "the .pat editor does not hold the fixture" }

function SciGet([IntPtr]$h, [string]$name) { return [int][G]::S($h, [uint32]$sci[$name], [IntPtr]::Zero, [IntPtr]::Zero) }
function SciSetSel([IntPtr]$h, [int]$a, [int]$b) {
    [void][G]::S($h, [uint32]$sci['SCI_SETSEL'], [IntPtr]$a, [IntPtr]$b)
}
function Sel([IntPtr]$h) {
    return @((SciGet $h 'SCI_GETSELECTIONSTART'), (SciGet $h 'SCI_GETSELECTIONEND'))
}
function CountDecEditors {
    [void](Editors)
    $n = 0
    foreach ($e in [G]::eds) { if ([G]::Doc($e).Contains('PIN_LIST  (E2E_Site)')) { $n++ } }
    return $n
}
function FindDecEditor() {
    foreach ($e in [G]::eds) { if ([G]::Doc($e).Contains('PIN_LIST  (E2E_Site)')) { return $e } }
    return [IntPtr]::Zero
}

# ---- 6. NEGATIVE first: the .pat is still the active document ----------------
$fail = 0
$decBefore = CountDecEditors
Say ("state: .dec editors open before = " + $decBefore)

# This probe judges "did the command open the .dec", so it needs an app that has
# not opened it yet. Measured: run 1 passed, then a second run against the SAME
# instance reported a FAILURE - the .dec from run 1 was still open, so "count grew
# by one" could never hold and the leftover MCLK selection read as "the negative
# case jumped". That verdict was a probe artefact, not a product defect. Refusing
# to judge beats judging the wrong state.
if ($decBefore -ne 0) {
    Die 'PRECONDITION-NOTFRESH' ("the .dec is already open (" + $decBefore + " editor(s)); restart the app with only the .pat and re-run")
}

# Make "no selection anywhere" a well-founded claim instead of an assumption:
# clear every editor's selection first, then assert the reset took effect. Without
# this the negative case could pass simply because nothing was selected to begin
# with, for reasons that have nothing to do with the command.
[void](Editors)
foreach ($e in [G]::eds) {
    $cur = SciGet $e 'SCI_GETCURRENTPOS'
    SciSetSel $e $cur $cur
}
[void](Editors)
foreach ($e in [G]::eds) {
    $s = Sel $e
    if ($s[1] -gt $s[0]) {
        Die 'PRECONDITION-RESET' "a selection survived the reset; the negative case would be unfalsifiable"
    }
}

$badIdx = $patLive.IndexOf('NOSUCHPIN')
if ($badIdx -lt 0) { Die 'PRECONDITION-NEGWORD' "NOSUCHPIN is not in the .pat text" }
[void][G]::S($patEd, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$badIdx, [IntPtr]::Zero)
$nowPos = SciGet $patEd 'SCI_GETCURRENTPOS'
if ($nowPos -ne $badIdx) {
    Die 'PRECONDITION-STIMULUS' ("caret did not land on NOSUCHPIN: expected " + $badIdx + " got " + $nowPos)
}
[void][G]::PostMessageW([G]::frame, 0x0111, [IntPtr]$CMD_GOTODEF, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900

$decAfterNeg = CountDecEditors
if ($decAfterNeg -ne $decBefore) {
    Say ("  FAIL: the command opened " + ($decAfterNeg - $decBefore) + " document(s) for a non-symbol word")
    $fail++
}
$posAfterNeg = SciGet $patEd 'SCI_GETCURRENTPOS'
if ($posAfterNeg -ne $badIdx) {
    Say ("  FAIL: the caret moved on a non-symbol word: " + $badIdx + " -> " + $posAfterNeg)
    $fail++
}
[void](Editors)
foreach ($e in [G]::eds) {
    $s = Sel $e
    if ($s[1] -gt $s[0]) {
        $t = [G]::Doc($e)
        Say ("  FAIL: a selection appeared on a non-symbol word: [" +
             $t.Substring($s[0], [Math]::Min($s[1] - $s[0], 40)) + "]")
        $fail++
    }
}
if ($fail -eq 0) { Say "  negative case OK: nothing opened, no caret moved, no selection appeared" }

# ---- 7. POSITIVE: caret on MCLK ----------------------------------------------
# Clear any pre-existing .dec selection first, so the positive case cannot be
# satisfied by a stale one. Measured with the deliberate negative-control build
# (jump on ANY word): the .dec was already open AND already had MCLK selected, so
# a second, independent defect (return immediately for MCLK) went unnoticed - the
# positive case passed on the leftover selection. That is the "assert the expected
# target, not 'something changed'" trap, and clearing first removes it: after this
# the only thing that can produce a MCLK selection is the command under test.
[void](Editors)
$preDec = FindDecEditor
if ($preDec -ne [IntPtr]::Zero) {
    $cur = SciGet $preDec 'SCI_GETCURRENTPOS'
    SciSetSel $preDec $cur $cur
    $s = Sel $preDec
    if ($s[1] -gt $s[0]) { Die 'PRECONDITION-RESET' "could not clear the .dec selection before the positive case" }
}
$goodIdx = $patLive.IndexOf('MCLK')
if ($goodIdx -lt 0) { Die 'PRECONDITION-POSWORD' "MCLK is not in the .pat text" }
[void][G]::S($patEd, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$goodIdx, [IntPtr]::Zero)
$nowPos = SciGet $patEd 'SCI_GETCURRENTPOS'
if ($nowPos -ne $goodIdx) {
    Die 'PRECONDITION-STIMULUS' ("caret did not land on MCLK: expected " + $goodIdx + " got " + $nowPos)
}
Say ("positive case: caret at " + $goodIdx + " (MCLK), sending command " + $CMD_GOTODEF)
[void][G]::PostMessageW([G]::frame, 0x0111, [IntPtr]$CMD_GOTODEF, [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200

[void](Editors)
$decEd = FindDecEditor
if ($decEd -eq [IntPtr]::Zero) {
    Say "  FAIL: the .dec was never opened"
    $fail++
} else {
    $decLive = [G]::Doc($decEd)
    $decSciLen = [int][G]::S($decEd, [uint32]$sci['SCI_GETLENGTH'], [IntPtr]::Zero, [IntPtr]::Zero)
    Say ("  channel: dec WM_GETTEXT chars=" + $decLive.Length + " SCI_GETLENGTH=" + $decSciLen)
    if ($decSciLen -ne $decLive.Length) { Say "  FAIL: dec length channel disagrees"; $fail++ }

    $s = Sel $decEd
    $selLen = $s[1] - $s[0]
    if ($selLen -le 0) {
        Say "  FAIL: nothing is selected in the .dec"
        $fail++
    } else {
        $selText = $decLive.Substring($s[0], $selLen)
        $gotLine = LineOf $decLive $s[0]
        $gotCol = ColOf $decLive $s[0]
        Say ("  selection: [" + $selText + "] len=" + $selLen + " line=" + $gotLine + " col=" + $gotCol)
        if ($selText -ne 'MCLK')   { Say ("  FAIL: selected '" + $selText + "', expected MCLK"); $fail++ }
        if ($gotLine -ne $decLine) { Say ("  FAIL: line " + $gotLine + ", expected " + $decLine); $fail++ }
        if ($gotCol -ne $decCol)   { Say ("  FAIL: col " + $gotCol + ", expected " + $decCol); $fail++ }
    }
    if ($decSciLen -ne ([IO.File]::ReadAllBytes($decPath)).Length) {
        # Informational only: the app may normalise CRLF on load, so the byte
        # count can legitimately differ. The text checks above are the real ones.
        Say ("  note: dec editor bytes " + $decSciLen + " != file bytes " +
             ([IO.File]::ReadAllBytes($decPath)).Length)
    }
}
$decAfterPos = CountDecEditors
if ($decAfterPos -ne $decBefore + 1) {
    Say ("  FAIL: .dec editor count " + $decAfterPos + ", expected " + ($decBefore + 1))
    $fail++
}

# ---- 8. verdict --------------------------------------------------------------
if ($fail -eq 0) {
    Say "VERDICT: GOTODEF-E2E-PASS"
    Stop-ProfileGuard
    exit 0
}
Say ("VERDICT: GOTODEF-E2E-FAIL (" + $fail + " failed assertion(s))")
Stop-ProfileGuard
exit 1
