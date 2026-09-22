param(
    [string]$Exe = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [string]$FixtureDir = ""
)
# defhint-e2e.ps1 - the status bar's [7] "definition" hint must appear when the
# caret sits on a symbol declared in a referenced .dec, and clear when it does not.
#
# Why this exists
# ---------------
# Batch 104 added F12 "go to definition"; batch 105 added the way back (Alt+Left).
# Neither tells the user that the trip is possible at all. Batch 106 adds the
# missing half: a status-bar hint showing WHERE the symbol under the caret is
# declared. The kernel half (slicing the declaration line, UTF-8-safe truncation)
# is covered by test_chromadiag. What only exists inside a live window is the
# wiring: caret -> word -> cached .dec locations -> the right status-bar part.
#
# Why the status bar and not a hover tooltip
# ------------------------------------------
# The original plan was to hang the hint off batch 103's dwell tooltip. That was
# dropped after measuring, not guessing: in this sandbox the mouse cannot be put
# on the window at all (a full-screen foreign window covers every screen position;
# SendInput clicks never arrive; a synthetic WM_MOUSEMOVE is immediately followed
# by WM_MOUSELEAVE, which resets ptMouseLast and cancels the dwell ticker). A
# feature whose only observable is a tooltip would therefore ship unverifiable -
# which this project forbids. The status bar is a plain control whose parts can be
# read from another process, so the same idea becomes assertable.
#
# How the part text is read across processes (and why it is safe)
# ---------------------------------------------------------------
# SB_* messages are >= WM_USER, so USER32 does NOT marshal them: handing the
# control a pointer to a buffer in THIS process would make the target write
# through an address that means nothing in its own space - the hazard the
# project's hard constraint 3 exists for. So the buffer is allocated inside the
# target with VirtualAllocEx and read back with ReadProcessMemory.
#
# SB_GETTEXTW with lParam = 0 is a buffer-free length probe: it returns
# MAKELONG(cchText, type) and writes nothing. That is what makes the two negative
# cases cheap.
#
# What is asserted
# ----------------
#   A. channel self-proofs:
#      - the part count returned by the control equals the count src/app/StatusBar.cpp
#        actually sends (SB_SETPARTS), so a wrong message base cannot pass silently
#      - part[5] equals the ATE-pattern language label taken from src/app/MainWindow.cpp,
#        which pins the part INDEX mapping (reading the wrong part would show a
#        different string)
#      - for part[0], the length returned with lParam=0 equals the character count of
#        the text read back through the remote buffer (two independent paths agree)
#   B. NEGATIVE FIRST, with a controlled stimulus: with the caret on SET_DEC_FILE
#      (not a symbol) and then on NOSUCHPIN (a word that is declared nowhere), part[7]
#      must be EMPTY
#   C. positive: with the caret on MCLK (declared on line 4 of pins.dec) part[7] must
#      equal the app's own sb.defhint template rendered with the .dec file name, the
#      1-based declaration line and that line's verbatim text; and the same for WG0
#      (line 5) so it cannot be a one-off
#   D. the hint clears again when the caret leaves the symbol
#
# The expected string is built from the language files' own sb.defhint templates
# (all five are tried), so the assertion is exact AND language-agnostic: no Chinese
# literal appears in this script, which must stay pure ASCII.
#
# Switches
# --------
#   -FixtureOnly      write the fixture, print the .pat path, and stop.
#   -AttachPid N      drive the already-running instance N. With 0 (the default)
#                     the script looks for a process whose Path is this repo's dev
#                     build, so a user instance is never touched.
#   -FixtureDir DIR   override the fixture location.
#
# Launching is deliberately NOT done here: Start-Process is unusable in this host.
# Start the app from Bash as a background task with the fixture path as its
# argument, then attach.
#
# ASCII ONLY. PowerShell 5.1 reads a BOM-less .ps1 as ANSI; one multi-byte character
# anywhere (even in a comment inside a C# here-string) eats the following newline and
# the script dies with a wall of bogus CS10xx errors and an empty output file.

$ErrorActionPreference = 'Stop'

# ---- status bar message constants -------------------------------------------
# Taken from the real header (Windows Kits\10\Include\<ver>\um\CommCtrl.h), not from
# memory. Measured: SB_GETTEXTW is WM_USER+13 and SB_GETTEXTLENGTHW is WM_USER+12 -
# the reverse of the intuitive guess, and getting them the wrong way round is SILENT:
# SB_GETTEXTLENGTHW ignores lParam, so calling it with a buffer pointer simply leaves
# the buffer untouched and the text reads back empty.
$SB_SETPARTS = 0x0404   # WM_USER+4
$SB_GETPARTS = 0x0406   # WM_USER+6
$SB_GETTEXTW = 0x040D   # WM_USER+13  (lParam=0 -> MAKELONG(cchText,type), no write)

$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root 'build\bin\Release\xfsWinPad.exe' }
if ($FixtureDir -eq "") { $FixtureDir = Join-Path ([IO.Path]::GetTempPath()) 'xfs_defhint_e2e' }
$out = Join-Path ([IO.Path]::GetTempPath()) 'defhint_e2e_out.txt'

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

Say ("=== defhint-e2e " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===")

# ---- 1. ids and expected strings, parsed out of the sources (never hardcoded) ---
$sciH = Join-Path $root 'third_party\scintilla\include\Scintilla.h'
$sciText = [IO.File]::ReadAllText($sciH)
$sci = @{}
foreach ($n in @('SCI_GETLENGTH','SCI_GETCURRENTPOS','SCI_GOTOPOS')) {
    if ($sciText -notmatch ("#define\s+" + $n + "\s+(\d+)")) {
        Die 'PRECONDITION-SCIID' "$n not found in Scintilla.h"
    }
    $sci[$n] = [int]$Matches[1]
}

# The part count the app itself sends. Ties the message constants above to the
# product source: if the base or the SB_GETPARTS offset were wrong, the control
# would not answer with this number.
$sbCpp = [IO.File]::ReadAllText((Join-Path $root 'src\app\StatusBar.cpp'))
if ($sbCpp -notmatch 'SB_SETPARTS,\s*(\d+)') {
    Die 'PRECONDITION-PARTS' "could not read the SB_SETPARTS count from StatusBar.cpp"
}
$wantParts = [int]$Matches[1]
Say ("source: StatusBar.cpp sends SB_SETPARTS " + $wantParts)

# part[5] must hold the ATE-pattern label. It is a literal in MainWindow.cpp (not an
# i18n key), which makes it a good index fingerprint.
$mwCpp = [IO.File]::ReadAllText((Join-Path $root 'src\app\MainWindow.cpp'))
if ($mwCpp -notmatch '\{\s*kLexAtePattern\s*,\s*L"([^"]+)"\s*\}') {
    Die 'PRECONDITION-LANGLABEL' "could not read the kLexAtePattern label from MainWindow.cpp"
}
$wantPart5 = $Matches[1]
Say ("source: part[5] should read [" + $wantPart5 + "]")

# sb.defhint templates, one per language file. The hint text is asserted against
# ALL of them, so the check is exact without hardcoding any localized wording.
$templates = @()
foreach ($lf in (Get-ChildItem (Join-Path $root 'resources\lang') -Filter '*.json')) {
    $j = [IO.File]::ReadAllText($lf.FullName)
    if ($j -match '"sb\.defhint"\s*:\s*"((?:[^"\\]|\\.)*)"') {
        $t = $Matches[1] -replace '\\"', '"' -replace '\\\\', '\'
        $templates += ,@($lf.BaseName, $t)
    }
}
if ($templates.Count -eq 0) { Die 'PRECONDITION-TEMPLATE' "no sb.defhint key in resources/lang/*.json" }
Say ("source: sb.defhint present in " + $templates.Count + " language file(s)")
function Render([string]$tpl, [string]$file, [int]$line, [string]$text) {
    return ($tpl -replace '\{0\}', $file -replace '\{1\}', [string]$line -replace '\{2\}', $text)
}

# ---- 2. fixture --------------------------------------------------------------
# pins.dec: MCLK on 1-based line 4, WG0 on line 5. The probe recomputes both from
# this text, so those numbers are the fixture's own truth, not a copy of the app's.
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
$patLines = @(
    'SET_DEC_FILE "./pins.dec"',
    '',
    'HEADER MCLK,WG0,DRDY;',
    '',
    'NOSUCHPIN = 1;',
    ''
)
$decText = ($decLines -join "`r`n")
$patText = ($patLines -join "`r`n")

[void][IO.Directory]::CreateDirectory($FixtureDir)
$decPath = Join-Path $FixtureDir 'pins.dec'
$patPath = Join-Path $FixtureDir 't.pat'
$ascii = New-Object System.Text.ASCIIEncoding
[IO.File]::WriteAllText($decPath, $decText, $ascii)
[IO.File]::WriteAllText($patPath, $patText, $ascii)
Say ("fixture: " + $patPath + " (" + ([IO.File]::ReadAllBytes($patPath)).Length + " bytes)")
Say ("fixture: " + $decPath + " (" + ([IO.File]::ReadAllBytes($decPath)).Length + " bytes)")

function LineOf([string]$t, [int]$idx) { return ($t.Substring(0, $idx) -split "`n").Count - 1 }
# The answer key: the 1-based line of a symbol in the .dec, and that line verbatim
# with leading/trailing blanks stripped - exactly what the hint is supposed to show.
function DecLineOf([string]$t, [string]$name) {
    $i = $t.IndexOf($name)
    if ($i -lt 0) { return 0 }
    return (LineOf $t $i) + 1
}
function DecLineText([string]$t, [int]$oneBased) {
    $ls = ($t -split "`r?`n")
    if ($oneBased -lt 1 -or $oneBased -gt $ls.Count) { return "" }
    return $ls[$oneBased - 1].Trim()
}
$mclkLine = DecLineOf $decText 'MCLK'
$wg0Line  = DecLineOf $decText 'WG0'
$mclkText = DecLineText $decText $mclkLine
$wg0Text  = DecLineText $decText $wg0Line
Say ("answer key: MCLK line " + $mclkLine + " [" + $mclkText + "]")
Say ("answer key: WG0  line " + $wg0Line + " [" + $wg0Text + "]")
if ($mclkLine -ne 4 -or $wg0Line -ne 5) {
    Die 'PRECONDITION-ANSWERKEY' "fixture moved: expected lines 4/5, got $mclkLine/$wg0Line"
}
if ($mclkText -eq "" -or $wg0Text -eq "") {
    Die 'PRECONDITION-ANSWERKEY' "could not slice the declaration lines"
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
public class D {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);

    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, IntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, IntPtr size, uint type);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);

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
    // reading a whole document this way is safe.
    public static string Doc(IntPtr h) {
        int len = (int)SW(h, 0x000E, IntPtr.Zero, null);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 4);
        SW(h, 0x000D, (IntPtr)(len + 1), sb);
        return sb.ToString();
    }

    // SB_GETTEXTW with lParam = 0 returns MAKELONG(cchText, type) and writes nothing.
    public static int PartLen(IntPtr sb, int index) {
        return (int)((long)S(sb, 0x040D, (IntPtr)index, IntPtr.Zero) & 0xFFFF);
    }
    // Buffer allocated INSIDE the target: SB_* are >= WM_USER, so USER32 does not
    // marshal them and a local pointer would be meaningless in the target's space.
    public static string Part(IntPtr sb, int index) {
        int len = PartLen(sb, index);
        if (len <= 0) return "";
        int bytes = (len + 2) * 2;
        IntPtr h = OpenProcess(0x0008 | 0x0010 | 0x0020, false, pid);
        if (h == IntPtr.Zero) return "<OpenProcess failed>";
        IntPtr remote = VirtualAllocEx(h, IntPtr.Zero, (IntPtr)bytes, 0x1000 | 0x2000, 0x04);
        if (remote == IntPtr.Zero) { CloseHandle(h); return "<VirtualAllocEx failed>"; }
        try {
            S(sb, 0x040D, (IntPtr)index, remote);
            byte[] buf = new byte[bytes];
            IntPtr got;
            if (!ReadProcessMemory(h, remote, buf, (IntPtr)bytes, out got))
                return "<ReadProcessMemory failed>";
            int n = 0;
            while (n + 1 < (int)got && !(buf[n] == 0 && buf[n + 1] == 0)) n += 2;
            return Encoding.Unicode.GetString(buf, 0, n);
        } finally {
            VirtualFreeEx(h, remote, IntPtr.Zero, 0x8000);
            CloseHandle(h);
        }
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
[D]::pid = [uint32]$AttachPid
[D]::LocateFrame()
if ([D]::frame -eq [IntPtr]::Zero) { Die 'PRECONDITION-FRAME' "no xfsWinPadMainWindow owned by pid $AttachPid" }
Say ("attached: pid=" + $AttachPid + " frame=0x" + ([D]::frame).ToInt64().ToString("X"))

# ---- 5. the .pat editor, by its TEXT (never by z-order or visibility) --------
$patEd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and $patEd -eq [IntPtr]::Zero) {
    [D]::ListEditors()
    foreach ($e in [D]::eds) {
        $t = [D]::Doc($e)
        if ($t.Contains('SET_DEC_FILE') -and $t.Contains('NOSUCHPIN')) { $patEd = $e; break }
    }
    if ($patEd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 400 }
}
if ($patEd -eq [IntPtr]::Zero) { Die 'PRECONDITION-PATED' "no editor holds the .pat fixture (did the app get the file argument?)" }

$patLive = [D]::Doc($patEd)
$patFile = [IO.File]::ReadAllText($patPath)
# Channel self-proof: the length message and the text message must agree, and the
# text must be the fixture. Without this the caret offsets below are unfalsifiable.
$patSciLen = [int][D]::S($patEd, [uint32]$sci['SCI_GETLENGTH'], [IntPtr]::Zero, [IntPtr]::Zero)
Say ("channel: pat WM_GETTEXT chars=" + $patLive.Length + " SCI_GETLENGTH=" + $patSciLen +
     " file bytes=" + $patFile.Length)
if ($patSciLen -ne $patLive.Length) { Die 'PRECONDITION-CHANNEL' "SCI_GETLENGTH and WM_GETTEXT disagree" }
if (-not $patLive.Contains('HEADER MCLK,WG0,DRDY;')) { Die 'PRECONDITION-CHANNEL' "the .pat editor does not hold the fixture" }

function SciGet([IntPtr]$h, [string]$name) { return [int][D]::S($h, [uint32]$sci[$name], [IntPtr]::Zero, [IntPtr]::Zero) }

# ---- 6. the status bar, with self-proofs -------------------------------------
$sb = [D]::FindWindowExW([D]::frame, [IntPtr]::Zero, "msctls_statusbar32", $null)
if ($sb -eq [IntPtr]::Zero) { Die 'PRECONDITION-NOSB' "no status bar child of the frame" }
Say ("statusbar=0x" + $sb.ToInt64().ToString("X"))

$nParts = [int][D]::S($sb, [uint32]$SB_GETPARTS, [IntPtr]::Zero, [IntPtr]::Zero)
Say ("self-proof: SB_GETPARTS = " + $nParts + ", StatusBar.cpp sends " + $wantParts)
if ($nParts -ne $wantParts) {
    Die 'PRECONDITION-PARTCOUNT' ("the control reports " + $nParts + " parts but the source sends " + $wantParts +
                                  " - the status-bar message constants are wrong")
}
$HINT = 7   # [7] definition hint (batch 106); [8] is the static-check count
$LANG = 5   # [5] language name

# part[0] is never empty for an open document; compare the two independent read
# paths so a silently-failing buffer read cannot be mistaken for "no hint".
$p0len = [D]::PartLen($sb, 0)
$p0txt = [D]::Part($sb, 0)
Say ("self-proof: part[0] length=" + $p0len + " text=[" + $p0txt + "]")
if ($p0len -le 0) { Die 'PRECONDITION-PART0' "part[0] is empty - is a document open?" }
if ($p0txt.Length -ne $p0len) {
    Die 'PRECONDITION-CHANNEL' ("the buffer-free length (" + $p0len + ") and the text read back (" +
                                $p0txt.Length + " chars) disagree")
}

# Index fingerprint: reading the wrong part would show a different string.
$p5 = [D]::Part($sb, $LANG)
Say ("self-proof: part[" + $LANG + "] = [" + $p5 + "]")
if ($p5 -ne $wantPart5) {
    Die 'PRECONDITION-INDEX' ("part[" + $LANG + "] reads [" + $p5 + "], expected [" + $wantPart5 + "] - wrong part index")
}

function SetCaret([IntPtr]$h, [int]$pos) {
    [void][D]::S($h, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$pos, [IntPtr]::Zero)
    $now = SciGet $h 'SCI_GETCURRENTPOS'
    if ($now -ne $pos) { Die 'PRECONDITION-STIMULUS' ("caret did not land on " + $pos + " (got " + $now + ")") }
    Start-Sleep -Milliseconds 250
}
function HintLen() { return [D]::PartLen($sb, $HINT) }
function HintText() { return [D]::Part($sb, $HINT) }

$fail = 0
$before = HintLen
Say ("state: hint length before any stimulus = " + $before)

# ---- 7. NEGATIVE cases first, with a controlled stimulus ----------------------
$negWords = @('SET_DEC_FILE', 'NOSUCHPIN')
foreach ($w in $negWords) {
    $idx = $patLive.IndexOf($w)
    if ($idx -lt 0) { Die 'PRECONDITION-NEGWORD' "$w is not in the .pat text" }
    SetCaret $patEd $idx
    $l = HintLen
    $t = HintText
    Say ("negative: caret on " + $w + " at " + $idx + " -> hint len=" + $l + " [" + $t + "]")
    if ($l -ne 0) {
        Say ("  FAIL: a hint appeared for a word that is not a .dec symbol")
        $fail++
    }
}
if ($fail -eq 0) { Say "  negative cases OK: no hint for a non-symbol word" }

# ---- 8. POSITIVE cases --------------------------------------------------------
# Each symbol: caret on the first occurrence in the .pat, hint must be exactly the
# rendered template for its declaration line in the .dec.
$cases = @(
    @{ name = 'MCLK'; line = $mclkLine; text = $mclkText },
    @{ name = 'WG0';  line = $wg0Line;  text = $wg0Text }
)
foreach ($c in $cases) {
    $idx = $patLive.IndexOf($c.name)
    if ($idx -lt 0) { Die 'PRECONDITION-POSWORD' ($c.name + " is not in the .pat text") }
    SetCaret $patEd $idx
    $got = HintText
    $expected = @()
    foreach ($tpl in $templates) {
        $expected += (Render $tpl[1] 'pins.dec' $c.line $c.text)
    }
    Say ("positive: caret on " + $c.name + " -> hint [" + $got + "]")
    Say ("  expected (any language): [" + ($expected -join "] [") + "]")
    if ($expected -notcontains $got) {
        Say ("  FAIL: hint does not match the expected string for " + $c.name +
             " (line " + $c.line + " of pins.dec)")
        $fail++
    }
}

# ---- 9. the hint must clear again --------------------------------------------
SetCaret $patEd ($patLive.IndexOf('NOSUCHPIN'))
$after = HintLen
Say ("state: hint length after leaving the symbol = " + $after)
if ($after -ne 0) {
    Say "  FAIL: the hint did not clear when the caret left the symbol"
    $fail++
}

# ---- 10. verdict -------------------------------------------------------------
if ($fail -eq 0) {
    Say "VERDICT: DEFHINT-E2E-PASS"
    Stop-ProfileGuard
    exit 0
}
Say ("VERDICT: DEFHINT-E2E-FAIL (" + $fail + " failed assertion(s))")
Stop-ProfileGuard
exit 1
