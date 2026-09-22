param(
    [string]$Exe = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [string]$FixtureDir = "",
    [switch]$SkipAccel
)
# navhist-e2e.ps1 - navigation history (Back / Forward, Alt+Left / Alt+Right) must
# really take the caret back to where it came from, in the document it came from.
#
# Why this exists
# ---------------
# Batch 105 added the feature on top of batch 104's Go to Definition. The pure
# cursor arithmetic is covered by test_navhistory; the guards cover the id space
# and the language files. What no unit test can reach is the half the user
# touches: is the *right document* activated, and does the caret land on the
# *exact byte* it was at before the jump?
#
# That second half is exactly where the position-measurement trap of batch 104
# lives (raw file bytes vs decoded editor text), so this probe recomputes its
# answer key from the fixture text and never copies the app's opinion.
#
# What is asserted
# ----------------
#   A. channel self-proof: SCI_GETLENGTH and WM_GETTEXT agree on both documents,
#      and the text really is the fixture written to disk
#   B. NEGATIVE FIRST, while the history is still EMPTY: Back and Forward must
#      move nothing, open nothing and select nothing
#   C. positive: after Go to Definition put us in the .dec, Back must return to
#      the .pat at the exact byte the caret was at before the jump, and Forward
#      must return to the .dec at the exact byte the caret had there
#   D. the ACTIVE DOCUMENT follows: the frame's title (which is
#      "<document name> - xfsWinPad", see UpdateTitleBar) must name the document
#      we just navigated to. A caret move in a document that never got activated
#      is invisible to the user, so this has to be asserted, not assumed.
#      Deliberately NOT asserted through GetGUIThreadInfo: that call returns
#      hwndFocus = NULL whenever the app is not the foreground thread, which made
#      three assertions fail on a build that was in fact correct (measured) - the
#      "verification method failed" and "the thing under test failed" trap.
#   E. the Alt+Left accelerator must do the same thing as the menu command - the
#      id/ACCEL wiring is a separate failure mode from the command body
#
# Discriminating, not just "something changed"
# --------------------------------------------
# Every assertion below is arranged so that a stale value cannot satisfy it: the
# .pat caret is deliberately parked at 0 before Back, and the .dec caret is
# deliberately parked at 0 (with the selection cleared) before Forward. Without
# that, "the caret is where it should be" would be true simply because nothing
# had moved it - the trap that the batch 104 negative control exposed.
#
# Precondition: a FRESH instance (only the .pat open). The probe refuses to judge
# otherwise: with a warmed-up history the "nothing to go back to" case is not
# testable at all.
#
# Switches
# --------
#   -FixtureOnly      write the fixture, print the .pat path, and stop.
#   -AttachPid N      drive the already-running instance N (default: find the dev
#                     build so a user instance is never touched).
#   -FixtureDir DIR   override the fixture location.
#   -SkipAccel        skip phase E (the real keyboard input).
#
# Launching the app is deliberately NOT done here: Start-Process is unusable in
# this host. Start it from Bash as a background task with the fixture path as its
# argument, then attach.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root 'build\bin\Release\xfsWinPad.exe' }
if ($FixtureDir -eq "") { $FixtureDir = Join-Path ([IO.Path]::GetTempPath()) 'xfs_navhist_e2e' }
$out = Join-Path ([IO.Path]::GetTempPath()) 'navhist_e2e_out.txt'

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

Say ("=== navhist-e2e " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===")

# ---- 1. ids, parsed out of the headers (never hardcoded) --------------------
$cmdH = Join-Path $root 'src\core\CommandIds.h'
$cmdText = [IO.File]::ReadAllText($cmdH)
$cmds = @{}
foreach ($n in @('NavBack','NavForward','GotoDefinition')) {
    if ($cmdText -notmatch ($n + '\s*=\s*(\d+)')) {
        Die 'PRECONDITION-CMDID' "$n is not an explicit value in $cmdH"
    }
    $cmds[$n] = [int]$Matches[1]
}
if ($cmds['NavBack'] -eq $cmds['NavForward'] -or
    $cmds['NavBack'] -eq $cmds['GotoDefinition'] -or
    $cmds['NavForward'] -eq $cmds['GotoDefinition']) {
    Die 'PRECONDITION-CMDID' "the three command ids are not distinct"
}

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
Say ("ids: NavBack=" + $cmds['NavBack'] + " NavForward=" + $cmds['NavForward'] +
     " GotoDefinition=" + $cmds['GotoDefinition'] +
     " SCI_GETCURRENTPOS=" + $sci['SCI_GETCURRENTPOS'])

# ---- 2. fixture --------------------------------------------------------------
# Same shape as the batch 104 probe: a real-looking .dec whose PIN_LIST declares
# MCLK on 1-based line 4 / 0-based column 2, and a .pat that references it.
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
$patNameIdx = $patText.IndexOf('MCLK')
Say ("answer key (from the file text): .dec MCLK at 0-based line " + $decLine + ", col " + $decCol +
     "; .pat MCLK at byte " + $patNameIdx)
if ($decLine -ne 3 -or $decCol -ne 2) {
    Die 'PRECONDITION-ANSWERKEY' "fixture moved: expected line 3 col 2, got $decLine/$decCol"
}
if ($patNameIdx -lt 0) { Die 'PRECONDITION-ANSWERKEY' "MCLK is not in the .pat fixture" }

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
public class N {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();

    [StructLayout(LayoutKind.Sequential)] public struct KBD {
        public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSE {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
    }
    // The union must be declared at its FULL size (32 bytes on x64), otherwise
    // Marshal.SizeOf(INPUT) is short of the 40 bytes Windows expects and
    // SendInput fails with ERROR_INVALID_PARAMETER instead of injecting anything.
    [StructLayout(LayoutKind.Explicit)] public struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public KBD ki;
        [FieldOffset(8)] public MOUSE mi;
    }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint n, INPUT[] p, int cb);

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
    // WM_GETTEXT is one of the few messages USER32 marshals across processes.
    public static string Doc(IntPtr h) {
        int len = (int)SW(h, 0x000E, IntPtr.Zero, null);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 4);
        SW(h, 0x000D, (IntPtr)(len + 1), sb);
        return sb.ToString();
    }
    // Real keyboard input: a synthetic PostMessage(WM_KEYDOWN) never reaches the
    // accelerator table, so the Alt+Left path can only be exercised this way.
    public static uint AltArrow(ushort vk) {
        INPUT[] a = new INPUT[4];
        for (int i = 0; i < 4; ++i) { a[i].type = 1; a[i].ki.dwExtraInfo = IntPtr.Zero; }
        a[0].ki.wVk = 0x12;                       // VK_MENU down
        a[1].ki.wVk = vk;                         // arrow down
        a[2].ki.wVk = vk; a[2].ki.dwFlags = 0x0002;   // arrow up
        a[3].ki.wVk = 0x12; a[3].ki.dwFlags = 0x0002; // VK_MENU up
        return SendInput(4, a, Marshal.SizeOf(typeof(INPUT)));
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
[N]::pid = [uint32]$AttachPid
[N]::LocateFrame()
if ([N]::frame -eq [IntPtr]::Zero) { Die 'PRECONDITION-FRAME' "no xfsWinPadMainWindow owned by pid $AttachPid" }
Say ("attached: pid=" + $AttachPid + " frame=0x" + ([N]::frame).ToString('X'))

# ---- 5. find the editors by their TEXT (never by z-order or visibility) ------
function Editors() { [N]::ListEditors(); return [N]::eds }
function FindEd([scriptblock]$pred) {
    foreach ($e in [N]::eds) { if (& $pred ([N]::Doc($e))) { return $e } }
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

function SciGet([IntPtr]$h, [string]$name) { return [int][N]::S($h, [uint32]$sci[$name], [IntPtr]::Zero, [IntPtr]::Zero) }
function SciSetSel([IntPtr]$h, [int]$a, [int]$b) { [void][N]::S($h, [uint32]$sci['SCI_SETSEL'], [IntPtr]$a, [IntPtr]$b) }
function SciGoto([IntPtr]$h, [int]$p) { [void][N]::S($h, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$p, [IntPtr]::Zero) }
function Sel([IntPtr]$h) { return @((SciGet $h 'SCI_GETSELECTIONSTART'), (SciGet $h 'SCI_GETSELECTIONEND')) }
function CountDecEditors {
    [void](Editors)
    $n = 0
    foreach ($e in [N]::eds) { if ([N]::Doc($e).Contains('PIN_LIST  (E2E_Site)')) { $n++ } }
    return $n
}
function FindDecEditor {
    foreach ($e in [N]::eds) { if ([N]::Doc($e).Contains('PIN_LIST  (E2E_Site)')) { return $e } }
    return [IntPtr]::Zero
}

$patLive = [N]::Doc($patEd)
$patFile = [IO.File]::ReadAllText($patPath)
# Channel self-proof: the length message and the text message must agree, and the
# text must be the fixture. Without this every offset below is unfalsifiable.
$patSciLen = SciGet $patEd 'SCI_GETLENGTH'
Say ("channel: pat WM_GETTEXT chars=" + $patLive.Length + " SCI_GETLENGTH=" + $patSciLen +
     " file bytes=" + ([IO.File]::ReadAllBytes($patPath)).Length)
if ($patSciLen -ne $patLive.Length) { Die 'PRECONDITION-CHANNEL' "SCI_GETLENGTH and WM_GETTEXT disagree on the .pat" }
if (-not $patLive.Contains('HEADER MCLK,WG0,DRDY;')) { Die 'PRECONDITION-CHANNEL' "the .pat editor does not hold the fixture" }
if ($patLive.IndexOf('MCLK') -ne $patNameIdx) {
    Die 'PRECONDITION-CHANNEL' ("the live .pat text and the file disagree about where MCLK is (" +
                                $patLive.IndexOf('MCLK') + " vs " + $patNameIdx + ")")
}

# ---- 6. freshness: a warmed-up history cannot test the empty case ------------
$decBefore = CountDecEditors
Say ("state: .dec editors open before = " + $decBefore)
if ($decBefore -ne 0) {
    Die 'PRECONDITION-NOTFRESH' ("the .dec is already open (" + $decBefore + " editor(s)); restart the app with only the .pat and re-run")
}

$fail = 0

# ---- 7. NEGATIVE FIRST: the history is still empty ---------------------------
# Park the caret somewhere unmistakable so "nothing moved" is a real claim.
$park = $patLive.Length - 1
SciGoto $patEd $park
if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne $park) {
    Die 'PRECONDITION-STIMULUS' "could not park the .pat caret at the end"
}
Say ("negative case: empty history, .pat caret parked at " + $park)
foreach ($phase in @(@('NavBack', $cmds['NavBack']), @('NavForward', $cmds['NavForward']))) {
    [void][N]::PostMessageW([N]::frame, 0x0111, [IntPtr][int]$phase[1], [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
    $n = CountDecEditors
    if ($n -ne $decBefore) {
        Say ("  FAIL: " + $phase[0] + " on an empty history opened " + ($n - $decBefore) + " document(s)")
        $fail++
    }
    $p = SciGet $patEd 'SCI_GETCURRENTPOS'
    if ($p -ne $park) {
        Say ("  FAIL: " + $phase[0] + " on an empty history moved the caret: " + $park + " -> " + $p)
        $fail++
    }
    $s = Sel $patEd
    if ($s[1] -gt $s[0]) { Say ("  FAIL: " + $phase[0] + " on an empty history created a selection"); $fail++ }
}
if ($fail -eq 0) { Say "  negative case OK: nothing opened, no caret moved, no selection appeared" }

# ---- 8. POSITIVE: Go to Definition, then Back, then Forward ------------------
# Step 8a: jump into the .dec (this is also the batch 104 assertion, reused as
# the setup for the navigation assertions below).
SciGoto $patEd $patNameIdx
if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne $patNameIdx) {
    Die 'PRECONDITION-STIMULUS' "caret did not land on MCLK in the .pat"
}
Say ("positive case: .pat caret at " + $patNameIdx + " (MCLK), sending GotoDefinition " + $cmds['GotoDefinition'])
[void][N]::PostMessageW([N]::frame, 0x0111, [IntPtr]$cmds['GotoDefinition'], [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200

[void](Editors)
$decEd = FindDecEditor
if ($decEd -eq [IntPtr]::Zero) {
    Die 'PRECONDITION-DECED' "Go to Definition did not open the .dec; nothing to navigate back from"
}
$decLive = [N]::Doc($decEd)
$decSciLen = SciGet $decEd 'SCI_GETLENGTH'
Say ("  channel: dec WM_GETTEXT chars=" + $decLive.Length + " SCI_GETLENGTH=" + $decSciLen)
if ($decSciLen -ne $decLive.Length) { Die 'PRECONDITION-CHANNEL' "SCI_GETLENGTH and WM_GETTEXT disagree on the .dec" }
$s = Sel $decEd
if (($s[1] - $s[0]) -le 0 -or $decLive.Substring($s[0], $s[1] - $s[0]) -ne 'MCLK') {
    Die 'PRECONDITION-DECED' "Go to Definition did not select MCLK in the .dec; the setup is not valid"
}
$decPosAfterJump = SciGet $decEd 'SCI_GETCURRENTPOS'
Say ("  setup OK: .dec open, MCLK selected, .dec caret at " + $decPosAfterJump)

# Step 8b: Back. Park both carets somewhere unmistakable first, so that "the
# caret is where it should be" cannot be satisfied by a value that was already
# there (see the header note on discriminating assertions).
SciGoto $patEd 0
SciSetSel $decEd 0 0
if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne 0) { Die 'PRECONDITION-PARK' "could not park the .pat caret at 0" }
$s = Sel $decEd
if ($s[1] -gt $s[0] -or (SciGet $decEd 'SCI_GETCURRENTPOS') -ne 0) {
    Die 'PRECONDITION-PARK' "could not park the .dec caret at 0 with no selection"
}
Say ("  back: .pat caret parked at 0, .dec caret parked at 0, sending NavBack " + $cmds['NavBack'])
[void][N]::PostMessageW([N]::frame, 0x0111, [IntPtr]$cmds['NavBack'], [IntPtr]::Zero)
Start-Sleep -Milliseconds 1000

$p = SciGet $patEd 'SCI_GETCURRENTPOS'
Say ("    .pat caret now " + $p + " (expected " + $patNameIdx + ")")
if ($p -ne $patNameIdx) { Say "  FAIL: Back did not restore the .pat caret"; $fail++ }
$s = Sel $patEd
if ($s[1] -gt $s[0]) { Say "  FAIL: Back left a selection in the .pat"; $fail++ }
$t = [N]::Doc([N]::frame)
Say ("    title=" + $t)
if (-not $t.Contains('t.pat')) { Say "  FAIL: Back did not activate the .pat (title does not name it)"; $fail++ }

# Step 8c: Forward must return to the .dec at the byte the caret had there.
Say ("  forward: sending NavForward " + $cmds['NavForward'])
[void][N]::PostMessageW([N]::frame, 0x0111, [IntPtr]$cmds['NavForward'], [IntPtr]::Zero)
Start-Sleep -Milliseconds 1000

$d = SciGet $decEd 'SCI_GETCURRENTPOS'
Say ("    .dec caret now " + $d + " (expected " + $decPosAfterJump + ")")
if ($d -ne $decPosAfterJump) { Say "  FAIL: Forward did not restore the .dec caret"; $fail++ }
$s = Sel $decEd
if ($s[1] -gt $s[0]) { Say "  FAIL: Forward left a selection behind (a plain caret was recorded)"; $fail++ }
$t = [N]::Doc([N]::frame)
Say ("    title=" + $t)
if (-not $t.Contains('pins.dec')) { Say "  FAIL: Forward did not activate the .dec (title does not name it)"; $fail++ }
$p = SciGet $patEd 'SCI_GETCURRENTPOS'
if ($p -ne $patNameIdx) { Say ("  FAIL: Forward disturbed the .pat caret (" + $p + ")"); $fail++ }

# ---- 9. the Alt+Left accelerator --------------------------------------------
# A separate failure mode: the command body can be perfect while the id never
# reaches it because the ACCEL table entry is missing or the menu loop eats the
# keystroke. PostMessage cannot exercise that path, so this uses real input.
if (-not $SkipAccel) {
    SciGoto $patEd 0
    if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne 0) { Die 'PRECONDITION-PARK' "could not park the .pat caret at 0" }
    [void][N]::SetForegroundWindow([N]::frame)
    Start-Sleep -Milliseconds 400
    $sent = [N]::AltArrow(0x25)   # VK_LEFT
    Say ("  accel: Alt+Left, SendInput returned " + $sent + " event(s)")
    if ($sent -ne 4) { Say "  FAIL: SendInput did not deliver 4 events (blocked?)"; $fail++ }
    Start-Sleep -Milliseconds 1000
    $p = SciGet $patEd 'SCI_GETCURRENTPOS'
    Say ("    .pat caret now " + $p + " (expected " + $patNameIdx + ")")
    if ($p -ne $patNameIdx) { Say "  FAIL: Alt+Left did not reach the NavBack command"; $fail++ }
    $t = [N]::Doc([N]::frame)
    if (-not $t.Contains('t.pat')) { Say "  FAIL: Alt+Left did not activate the .pat"; $fail++ }
}

# ---- 10. verdict -------------------------------------------------------------
if ($fail -eq 0) {
    Say "VERDICT: NAVHIST-E2E-PASS"
    Stop-ProfileGuard
    exit 0
}
Say ("VERDICT: NAVHIST-E2E-FAIL (" + $fail + " failed assertion(s))")
Stop-ProfileGuard
exit 1
