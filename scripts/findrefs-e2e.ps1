param(
    [string]$Exe = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [string]$FixtureDir = "",
    [switch]$SkipAccel
)
# findrefs-e2e.ps1 - Shift+F12 "Find All References" must list every real use of the
# symbol under the caret, and nothing that merely looks like one.
#
# Why this exists
# ---------------
# Batch 104 added F12 (jump to the declaration) and batch 105 the way back. Neither
# answers the question a user actually asks before changing a pin: "how many places
# use this?". Plain Find does, but it also matches `MCLK2` and the `// MCLK` in a
# comment, so the answer is wrong in a way that looks right.
#
# Batch 107 adds the command. The kernel halves are covered by unit tests:
#   - test_findrefs: whole-word matching, comments/strings excluded, and the
#     (line, col, len) -> byte-span conversion (LocateSymbolRef).
#   - test_shortcut:  the Shift+F12 binding itself.
# What only exists inside a live window, and is therefore what this probe asserts:
#   - the COMMAND being wired up at all (menu id -> FindAllReferences -> panel)
#   - the rows carrying the right file / line / line-text, which is the only place
#     where "the .dec hit was read, decoded and located correctly" can be observed.
#
# Why the shortcut is NOT exercised here (measured, not assumed)
# --------------------------------------------------------------
# The first version sent real Shift+F12 with SendInput and reported a failure. It
# was a probe artefact, not a product defect: `GetForegroundWindow()` here is
# `Chrome_WidgetWin_1` - another process's full-screen window covers the desktop and
# `SetForegroundWindow` cannot take it back (the same wall batch 106 hit for the
# mouse). SendInput reports 4 events delivered, but they go to that window.
# So the probe now refuses to judge that path unless the frame really is foreground,
# and the binding moved into test_shortcut where it runs on every build.
# `-SkipAccel` drives the same command through PostMessage(WM_COMMAND) instead; that
# proves the wiring but explicitly does NOT prove the accelerator.
#
# Why rows are read with a target-side LVITEM
# -------------------------------------------
# LVM_* are >= WM_USER, so USER32 does NOT marshal them: the LVITEM and its text
# buffer must be allocated INSIDE the target (VirtualAllocEx) or the address means
# nothing there - the hazard hard constraint 3 is about.
# NM_DBLCLK could NOT be used to activate rows: WM_NOTIFY is below WM_USER but the
# target silently ignores a cross-process notification (measured: the panel reacted
# fine to a posted WM_COMMAND/ID_CLOSE, and to nothing at all from WM_NOTIFY with a
# correct NMITEMACTIVATE in target memory). Cell text is therefore read instead.
#
# What is asserted
# ----------------
#   A. channel self-proofs:
#      - the panel is found by its REAL class name and its list by its REAL control
#        id, both parsed out of src/app/ResultsPanel.cpp - not by z-order or index
#      - SCI_GETLENGTH and WM_GETTEXT agree on the .pat, and it holds the fixture,
#        so every offset below is falsifiable
#      - the row count comes from LVM_GETITEMCOUNT (value-returning, and the same
#        value craft-e2e.ps1 / dblclick-test.ps1 already rely on)
#   B. POSITIVE (caret on MCLK): exactly 4 rows, and the (file, line, text) of each
#      is exactly {t.pat:3, t.pat:5, t.pat:7, pins.dec:4} with each row's text equal
#      to that line of the fixture - so the COMMENT on line 6 is not counted,
#      `MCLK2` on line 9 is not counted, and the .dec declaration IS
#   C. NEGATIVE (caret on NOSUCHPIN, a word that appears once and is declared
#      nowhere): exactly 1 row, t.pat:8 - no spurious .dec hit
#
# The fixture must be written BEFORE the app starts (batch 106: a fixture written
# after launch leaves the app with no document and every assertion reads as "empty").
#
# Switches
# --------
#   -FixtureOnly      write the fixture, print the .pat path, and stop.
#   -AttachPid N      drive the already-running instance N. With 0 (default) the
#                     script looks for a process whose Path is this repo's dev build,
#                     so a user instance is never touched.
#   -FixtureDir DIR   override the fixture location.
#   -SkipAccel        use PostMessage(WM_COMMAND) instead of real input.
#
# Launching is deliberately NOT done here: Start-Process is unusable in this host.
# Start the app from Bash as a background task with the fixture path as its argument,
# then attach.
#
# ASCII ONLY. PowerShell 5.1 reads a BOM-less .ps1 as ANSI; one multi-byte character
# anywhere (even inside a C# here-string) eats the following newline and the script
# dies with a wall of bogus CS10xx errors and an empty output file.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $root
if ($FixtureDir -eq "") { $FixtureDir = Join-Path ([IO.Path]::GetTempPath()) 'xfs_findrefs_e2e' }

function Say([string]$s) { Write-Output $s }
function Die([string]$code, [string]$why) {
    Say ("VERDICT: FINDREFS-E2E-FAIL " + $code + ": " + $why)
    Stop-ProfileGuard
    exit 1
}

# ---- 1. ids, parsed out of the sources (never hardcoded) ---------------------
$cmdH = Join-Path $root 'src\core\CommandIds.h'
$cmdText = [IO.File]::ReadAllText($cmdH)
if ($cmdText -notmatch 'FindAllReferences\s*=\s*(\d+)') {
    Die 'PRECONDITION-CMDID' "FindAllReferences is not an explicit value in $cmdH"
}
$CMD_FINDREFS = [int]$Matches[1]

$rpCpp = Join-Path $root 'src\app\ResultsPanel.cpp'
$rpText = [IO.File]::ReadAllText($rpCpp)
if ($rpText -notmatch 'constexpr\s+wchar_t\s+kClass\[\]\s*=\s*L"([^"]+)"') {
    Die 'PRECONDITION-PANELCLASS' "results panel class name not found in $rpCpp"
}
$PANEL_CLASS = $Matches[1]
if ($rpText -notmatch 'constexpr\s+int\s+ID_LIST\s*=\s*(\d+)') {
    Die 'PRECONDITION-LISTID' "ID_LIST not found in $rpCpp"
}
$ID_LIST = [int]$Matches[1]

$sciH = Join-Path $root 'third_party\scintilla\include\Scintilla.h'
$sciText = [IO.File]::ReadAllText($sciH)
$sci = @{}
foreach ($n in @('SCI_GETLENGTH','SCI_GETCURRENTPOS','SCI_GOTOPOS','SCI_SETSEL',
                 'SCI_GETSELECTIONSTART','SCI_GETSELECTIONEND')) {
    if ($sciText -notmatch ("#define\s+" + $n + "\s+(\d+)")) {
        Die 'PRECONDITION-SCIID' "$n not found in Scintilla.h"
    }
    $sci[$n] = [int]$Matches[1]
}
$LVM_FIRST        = 0x1000
$LVM_GETITEMCOUNT = $LVM_FIRST + 4      # 0x1004, value-returning
$LVM_GETITEMTEXTW = $LVM_FIRST + 115    # 0x1073
$WM_COMMAND       = 0x0111

Say ("ids: FindAllReferences=" + $CMD_FINDREFS + " panel='" + $PANEL_CLASS +
     "' ID_LIST=" + $ID_LIST + " LVM_GETITEMTEXTW=0x" + $LVM_GETITEMTEXTW.ToString('X'))

# ---- 2. fixture --------------------------------------------------------------
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
    'WAIT(MCLK);',
    '// MCLK appears in this comment and must NOT be counted',
    'FORCE_V_PPMU(MCLK, 1.0, 0.1);',
    'NOSUCHPIN = 1;',
    'MCLK2 = 1;',
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

# The answer key is computed from the fixture text and nowhere else: (file, line) ->
# the verbatim content of that line.
$k3 = 3; $k5 = 5; $k7 = 7; $k8 = 8; $kDec = 4
$want = @{
    ("t.pat|" + $k3)    = $patLines[$k3 - 1]
    ("t.pat|" + $k5)    = $patLines[$k5 - 1]
    ("t.pat|" + $k7)    = $patLines[$k7 - 1]
    ("pins.dec|" + $kDec) = $decLines[$kDec - 1]
}
$wantNeg = @{ ("t.pat|" + $k8) = $patLines[$k8 - 1] }
# Cross-check the hardcoded line numbers against the text, so a fixture edit that
# moves a line cannot turn into a silently wrong expectation.
function LineNo([string]$t, [string]$needle) {
    $i = $t.IndexOf($needle)
    if ($i -lt 0) { return -1 }
    return (($t.Substring(0, $i) -split "`n").Count)
}
if ((LineNo $patText 'HEADER MCLK') -ne $k3) { Die 'PRECONDITION-ANSWERKEY' "HEADER MCLK moved" }
if ((LineNo $patText 'WAIT(MCLK)') -ne $k5) { Die 'PRECONDITION-ANSWERKEY' "WAIT(MCLK) moved" }
if ((LineNo $patText 'FORCE_V_PPMU') -ne $k7) { Die 'PRECONDITION-ANSWERKEY' "FORCE_V_PPMU moved" }
if ((LineNo $patText 'NOSUCHPIN') -ne $k8) { Die 'PRECONDITION-ANSWERKEY' "NOSUCHPIN moved" }
if ((LineNo $decText 'MCLK') -ne $kDec) { Die 'PRECONDITION-ANSWERKEY' "pins.dec MCLK moved" }
Say ("answer key (from the fixture text): " + (($want.Keys | Sort-Object) -join ' '))

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
public class F {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int n);

    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, IntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, IntPtr size, uint type);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int GetLastError();

    [StructLayout(LayoutKind.Sequential)] public struct KBD {
        public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSE {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
    }
    // The union must be declared at its FULL size (32 bytes on x64) or
    // Marshal.SizeOf(INPUT) is short of the 40 bytes Windows expects and SendInput
    // fails with ERROR_INVALID_PARAMETER instead of injecting anything.
    [StructLayout(LayoutKind.Explicit)] public struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public KBD ki;
        [FieldOffset(8)] public MOUSE mi;
    }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint n, INPUT[] p, int cb);

    [StructLayout(LayoutKind.Sequential)] public struct LVITEMW {
        public uint mask; public int iItem; public int iSubItem;
        public uint state; public uint stateMask;
        public IntPtr pszText; public int cchTextMax; public int iImage;
        public IntPtr lParam; public int iIndent; public int iGroupId;
        public uint cColumns; public IntPtr puColumns; public IntPtr piColFmt; public int iGroup;
    }

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
    public static uint ShiftF12() {
        INPUT[] a = new INPUT[4];
        for (int i = 0; i < 4; ++i) { a[i].type = 1; a[i].ki.dwExtraInfo = IntPtr.Zero; }
        a[0].ki.wVk = 0x10;                           // VK_SHIFT down
        a[1].ki.wVk = 0x7B;                           // VK_F12 down
        a[2].ki.wVk = 0x7B; a[2].ki.dwFlags = 0x0002; // VK_F12 up
        a[3].ki.wVk = 0x10; a[3].ki.dwFlags = 0x0002; // VK_SHIFT up
        return SendInput(4, a, Marshal.SizeOf(typeof(INPUT)));
    }
    // Read one ListView cell. LVM_GETITEMTEXTW is >= WM_USER, so USER32 does not
    // marshal it: both the LVITEM and its text buffer live INSIDE the target.
    // Returns "" and sets err when any step fails - a silent failure here would
    // look exactly like "the row is empty", so the caller must look at err.
    public static string Cell(IntPtr list, uint msg, int row, int sub, out string err) {
        err = "";
        IntPtr h = OpenProcess(0x0008 | 0x0010 | 0x0020, false, pid);
        if (h == IntPtr.Zero) { err = "OpenProcess gle=" + GetLastError(); return ""; }
        int cb = Marshal.SizeOf(typeof(LVITEMW));
        int tsz = 1024;
        IntPtr remote = VirtualAllocEx(h, IntPtr.Zero, (IntPtr)(cb + tsz), 0x1000 | 0x2000, 0x04);
        if (remote == IntPtr.Zero) { err = "VirtualAllocEx gle=" + GetLastError(); CloseHandle(h); return ""; }
        try {
            LVITEMW it = new LVITEMW();
            it.mask = 0x0001;                                  // LVIF_TEXT
            it.iItem = row; it.iSubItem = sub;
            it.pszText = (IntPtr)(remote.ToInt64() + cb);
            it.cchTextMax = tsz / 2;
            byte[] buf = new byte[cb];
            IntPtr p = Marshal.AllocHGlobal(cb);
            Marshal.StructureToPtr(it, p, false);
            Marshal.Copy(p, buf, 0, cb);
            Marshal.FreeHGlobal(p);
            IntPtr written;
            if (!WriteProcessMemory(h, remote, buf, (IntPtr)cb, out written)) {
                err = "WriteProcessMemory gle=" + GetLastError(); return "";
            }
            IntPtr r = S(list, msg, (IntPtr)row, remote);
            byte[] tb = new byte[tsz];
            IntPtr got;
            if (!ReadProcessMemory(h, (IntPtr)(remote.ToInt64() + cb), tb, (IntPtr)tsz, out got)) {
                err = "ReadProcessMemory gle=" + GetLastError(); return "";
            }
            int n = 0;
            while (n + 1 < tsz && !(tb[n] == 0 && tb[n + 1] == 0)) n += 2;
            err = "ret=" + r.ToInt64() + " chars=" + (n / 2);
            return Encoding.Unicode.GetString(tb, 0, n);
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
[F]::pid = [uint32]$AttachPid
[F]::LocateFrame()
if ([F]::frame -eq [IntPtr]::Zero) { Die 'PRECONDITION-FRAME' "no xfsWinPadMainWindow owned by pid $AttachPid" }
Say ("attached: pid=" + $AttachPid + " frame=0x" + ([F]::frame).ToString('X'))

# ---- 5. find the .pat editor by its TEXT -------------------------------------
function Editors() { [F]::ListEditors(); return [F]::eds }
function FindEd([scriptblock]$pred) {
    foreach ($e in [F]::eds) { if (& $pred ([F]::Doc($e))) { return $e } }
    return [IntPtr]::Zero
}
function SciGet([IntPtr]$h, [string]$name) { return [int][F]::S($h, [uint32]$sci[$name], [IntPtr]::Zero, [IntPtr]::Zero) }

$deadline = (Get-Date).AddSeconds(15)
$patEd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline -and $patEd -eq [IntPtr]::Zero) {
    [void](Editors)
    $patEd = FindEd { param($t) $t.Contains('SET_DEC_FILE') -and $t.Contains('NOSUCHPIN') }
    if ($patEd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 400 }
}
if ($patEd -eq [IntPtr]::Zero) { Die 'PRECONDITION-PATED' "no editor holds the .pat fixture (did the app get the file argument?)" }

$patLive = [F]::Doc($patEd)
$patSciLen = SciGet $patEd 'SCI_GETLENGTH'
Say ("channel: pat WM_GETTEXT chars=" + $patLive.Length + " SCI_GETLENGTH=" + $patSciLen)
if ($patSciLen -ne $patLive.Length) { Die 'PRECONDITION-CHANNEL' "SCI_GETLENGTH and WM_GETTEXT disagree" }

# The .dec hit is one of the expected rows, so the app must not have it open yet.
# Measured precedent (gotodef-e2e): a second run against the same instance reports
# failures that are purely leftover-state artefacts.
$decOpen = 0
[void](Editors)
foreach ($e in [F]::eds) { if ([F]::Doc($e).Contains('PIN_LIST  (E2E_Site)')) { $decOpen++ } }
if ($decOpen -ne 0) {
    Die 'PRECONDITION-NOTFRESH' ("pins.dec is already open in " + $decOpen + " editor(s); restart the app with only t.pat and re-run")
}

# ---- 6. the results panel, found by its REAL identity -------------------------
function PanelHwnd { return [F]::FindWindowExW([F]::frame, [IntPtr]::Zero, $PANEL_CLASS, $null) }
function ListHwnd([IntPtr]$panel) {
    if ($panel -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    return [F]::FindWindowExW($panel, [IntPtr]::Zero, 'SysListView32', $null)
}
function RowCount([IntPtr]$list) {
    if ($list -eq [IntPtr]::Zero) { return 0 }
    return [int][F]::S($list, [uint32]$LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Row([IntPtr]$list, [int]$i) {
    $e = ""
    $f = [F]::Cell($list, [uint32]$LVM_GETITEMTEXTW, $i, 0, [ref]$e)
    $n = [F]::Cell($list, [uint32]$LVM_GETITEMTEXTW, $i, 1, [ref]$e)
    $t = [F]::Cell($list, [uint32]$LVM_GETITEMTEXTW, $i, 2, [ref]$e)
    return @($f, $n, $t, $e)
}

$p0 = PanelHwnd
$l0 = [IntPtr]::Zero
if ($p0 -ne [IntPtr]::Zero) {
    $l0 = ListHwnd $p0
    if ($l0 -ne [IntPtr]::Zero) {
        $id = ([F]::GetWindowLongPtr($l0, -12)).ToInt32()
        Say ("channel: panel found, list id=" + $id + " (expected " + $ID_LIST + ")")
        if ($id -ne $ID_LIST) { Die 'PRECONDITION-CHANNEL' "the list control id does not match ID_LIST; the wrong ListView would be read" }
    }
}
$baseRows = RowCount $l0
Say ("state: rows before = " + $baseRows)
if ($baseRows -ne 0) { Die 'PRECONDITION-STALE' "the results list is not empty before the command" }

# ---- 7. POSITIVE: caret on MCLK ---------------------------------------------
$fail = 0
$goodIdx = $patLive.IndexOf('MCLK')
if ($goodIdx -lt 0) { Die 'PRECONDITION-POSWORD' "MCLK is not in the .pat text" }
[void][F]::S($patEd, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$goodIdx, [IntPtr]::Zero)
if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne $goodIdx) {
    Die 'PRECONDITION-STIMULUS' "the caret did not land on MCLK"
}
if ($SkipAccel) {
    Say ("positive: sending WM_COMMAND " + $CMD_FINDREFS + " (SkipAccel: the shortcut is NOT proven)")
    [void][F]::PostMessageW([F]::frame, [uint32]$WM_COMMAND, [IntPtr]$CMD_FINDREFS, [IntPtr]::Zero)
} else {
    [void][F]::SetForegroundWindow([F]::frame)
    Start-Sleep -Milliseconds 600
    if ([F]::GetForegroundWindow() -ne [F]::frame) {
        $c = New-Object System.Text.StringBuilder 64
        [void][F]::GetClassNameW([F]::GetForegroundWindow(), $c, 64)
        Die 'PRECONDITION-FOREGROUND' ("the frame cannot be made foreground here (foreground is '" + $c.ToString() +
            "'); real input would go to that window, so this run cannot judge the shortcut. Re-run with -SkipAccel - the binding itself is asserted in test_shortcut")
    }
    $sent = [F]::ShiftF12()
    Say ("positive: Shift+F12 via SendInput, " + $sent + " event(s) delivered")
    if ($sent -ne 4) { Die 'PRECONDITION-INPUT' "SendInput delivered $sent of 4 events" }
}
Start-Sleep -Milliseconds 1400

$panel = PanelHwnd
$list = ListHwnd $panel
if ($list -eq [IntPtr]::Zero) { Die 'PRECONDITION-CHANNEL' "the results list does not exist after the command" }
$rows = RowCount $list
Say ("positive: rows = " + $rows + " (expected " + $want.Count + ")")
if ($rows -ne $want.Count) {
    Say ("  FAIL: row count " + $rows + ", expected " + $want.Count)
    $fail++
}

$got = @{}
for ($i = 0; $i -lt [Math]::Min($rows, 16); ++$i) {
    $r = Row $list $i
    $key = ($r[0] + "|" + $r[1])
    Say ("  row " + $i + ": [" + $r[0] + "] line " + $r[1] + " text=[" + $r[2] + "]  (" + $r[3] + ")")
    if (-not $want.ContainsKey($key)) {
        Say ("    FAIL: unexpected row " + $key)
        $fail++
        continue
    }
    $got[$key] = $true
    if ($r[2] -ne $want[$key]) {
        Say ("    FAIL: text mismatch at " + $key)
        Say ("      got  [" + $r[2] + "]")
        Say ("      want [" + $want[$key] + "]")
        $fail++
    }
}
foreach ($k in $want.Keys) {
    if (-not $got.ContainsKey($k)) { Say ("  FAIL: missing row " + $k); $fail++ }
}
if ($fail -eq 0) {
    Say "  positive case OK: comment and MCLK2 excluded, .dec declaration included, every row's text is that line verbatim"
}

# ---- 8. NEGATIVE: caret on NOSUCHPIN (declared nowhere) ----------------------
$badIdx = $patLive.IndexOf('NOSUCHPIN')
if ($badIdx -lt 0) { Die 'PRECONDITION-NEGWORD' "NOSUCHPIN is not in the .pat text" }
[void][F]::S($patEd, [uint32]$sci['SCI_GOTOPOS'], [IntPtr]$badIdx, [IntPtr]::Zero)
if ((SciGet $patEd 'SCI_GETCURRENTPOS') -ne $badIdx) {
    Die 'PRECONDITION-STIMULUS' "the caret did not land on NOSUCHPIN"
}
if ($SkipAccel) {
    [void][F]::PostMessageW([F]::frame, [uint32]$WM_COMMAND, [IntPtr]$CMD_FINDREFS, [IntPtr]::Zero)
} else {
    [void][F]::SetForegroundWindow([F]::frame)
    Start-Sleep -Milliseconds 400
    $sent = [F]::ShiftF12()
    if ($sent -ne 4) { Die 'PRECONDITION-INPUT' "SendInput delivered $sent of 4 events on the negative case" }
}
Start-Sleep -Milliseconds 1400

$panel = PanelHwnd
$list = ListHwnd $panel
$negRows = RowCount $list
Say ("negative: rows for NOSUCHPIN = " + $negRows + " (expected 1)")
if ($negRows -ne 1) {
    Say ("  FAIL: row count " + $negRows + ", expected 1 (a word declared nowhere must not pick up a .dec hit)")
    $fail++
} else {
    $r = Row $list 0
    $key = ($r[0] + "|" + $r[1])
    Say ("  negative row: [" + $r[0] + "] line " + $r[1] + " text=[" + $r[2] + "]")
    if (-not $wantNeg.ContainsKey($key)) { Say ("    FAIL: unexpected row " + $key); $fail++ }
    elseif ($r[2] -ne $wantNeg[$key]) {
        Say ("    FAIL: text mismatch: got [" + $r[2] + "] want [" + $wantNeg[$key] + "]")
        $fail++
    }
}

# ---- 9. verdict --------------------------------------------------------------
if ($fail -eq 0) {
    Say "VERDICT: FINDREFS-E2E-PASS"
    Stop-ProfileGuard
    exit 0
}
Say ("VERDICT: FINDREFS-E2E-FAIL (" + $fail + " failed assertion(s))")
Stop-ProfileGuard
exit 1
