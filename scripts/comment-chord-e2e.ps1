param(
    [string]$Exe = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [string]$FixtureDir = ""
)
# comment-chord-e2e.ps1 - batch 154: toolbar line-comment button + VS-style
# Ctrl+K,C / Ctrl+K,U chord, asserted against a live window.
#
# Why this exists
# ---------------
# The kernel halves are covered by unit tests and do not need a window:
#   - test_lineops:  CommentLines only adds / UncommentLines only removes, and
#                    blank (all-whitespace) lines are skipped (the user's explicit
#                    requirement).
#   - test_chordkeys: DecideChord's four quadrants (Arm / Comment / Uncomment /
#                     None), and that a non-pure-Ctrl combination is NOT swallowed.
# What only exists inside a live window, and is therefore what this probe asserts:
#   - the two menu items carrying the "Ctrl+K, Ctrl+C" / "Ctrl+K, Ctrl+U" tail
#     annotation (the ChordText fallback for commands the shortcut table cannot hold)
#   - the toolbar button being installed for the toggle command
#   - the two command ids actually reaching the editor, in BOTH shapes the user
#     asked for: (a) no selection -> ONLY the caret line is commented; (b) a
#     multi-line selection -> every non-blank line is commented and the BLANK line
#     is left alone. Each direction round-trips and is idempotent.
#
# Why the chord is NOT exercised with real input here (measured, not assumed)
# --------------------------------------------------------------------------
# The chord interception reads the live modifier state with GetKeyState() and runs
# in the window message loop, so only REAL keyboard input can drive it. findrefs-e2e
# already measured that this host cannot deliver real input: GetForegroundWindow()
# is another process's full-screen window, and SetForegroundWindow cannot take it
# back. PostMessage(WM_KEYDOWN) would arrive with Ctrl UP (GetKeyState is unaffected
# by posted messages), so the chord would read as a plain key press and prove nothing.
# The keyboard-state logic is therefore asserted in test_chordkeys, and this probe
# proves the command wiring end to end instead. If a future host CAN take the
# foreground, add a SendInput chord leg here rather than weakening these assertions.
#
# How ids are obtained (never hardcoded)
# --------------------------------------
# The tail annotation after the TAB is language independent, so the probe finds
#   "Ctrl+K, Ctrl+C" -> EditCommentLine id
#   "Ctrl+K, Ctrl+U" -> EditUncommentLine id
#   "Ctrl+Q"         -> EditToggleComment id (the toolbar button's command)
# by walking the frame's whole menu tree and reading each item with
# GetMenuStringW + GetMenuItemID. Nothing in this file knows a numeric command id.
#
# Switches
# --------
#   -FixtureOnly      write the fixture, print the .cpp path, and stop.
#   -AttachPid N      drive the already-running instance N (0 = auto-find this
#                     repo's dev build, so a user instance is never touched).
#   -FixtureDir DIR   override the fixture location.
# Launching is deliberately NOT done here (Start-Process is unusable in this host):
# start the app from a shell with the fixture path as its argument, then attach.
#
# ASCII ONLY. PowerShell 5.1 reads a BOM-less .ps1 as ANSI; one multi-byte character
# anywhere (even inside a C# here-string) eats the following newline and the script
# dies with a wall of bogus CS10xx errors.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $root
if ($FixtureDir -eq "") { $FixtureDir = Join-Path ([IO.Path]::GetTempPath()) 'xfs_comment_chord_e2e' }

function Say([string]$s) { Write-Output $s }
function Die([string]$code, [string]$why) {
    Say ("VERDICT: COMMENT-CHORD-E2E-FAIL " + $code + ": " + $why)
    Stop-ProfileGuard
    exit 1
}

# ---- 1. fixture --------------------------------------------------------------
# A .cpp extension picks the "cpp" lexer, whose line-comment token is "//" (no
# trailing space), so the expected text stays readable. Three lines with the middle
# one BLANK is exactly the shape the user asked to skip.
$cppLines = @('alpha', '', 'beta', '')
$cppText = ($cppLines -join "`r`n")
$origNorm = "alpha`n`nbeta`n"          # fixture with CR stripped
$comNorm  = "//alpha`n`n//beta`n"      # after one Comment

[void][IO.Directory]::CreateDirectory($FixtureDir)
$cppPath = Join-Path $FixtureDir 't.cpp'
$ascii = New-Object System.Text.ASCIIEncoding
[IO.File]::WriteAllText($cppPath, $cppText, $ascii)
Say ("fixture: " + $cppPath + " (" + ([IO.File]::ReadAllBytes($cppPath)).Length + " bytes)")

# Scintilla ids parsed from the header, never hardcoded. SCI_SETSEL carries plain
# integers in wParam/lParam (no pointer), so unlike the LVM_* messages the findrefs
# probe had to VirtualAllocEx, it is safe to send cross-process.
$sciH = Join-Path $root 'third_party\scintilla\include\Scintilla.h'
$sciText = [IO.File]::ReadAllText($sciH)
$sci = @{}
foreach ($n in @('SCI_SETSEL','SCI_GETLENGTH')) {
    if ($sciText -notmatch ("#define\s+" + $n + "\s+(\d+)")) { Die 'PRECONDITION-SCIID' "$n not found in Scintilla.h" }
    $sci[$n] = [int]$Matches[1]
}
Say ("sci: SCI_SETSEL=" + $sci['SCI_SETSEL'] + " SCI_GETLENGTH=" + $sci['SCI_GETLENGTH'])

if ($FixtureOnly) {
    Say "FIXTURE-ONLY"
    Say ("CPP=" + $cppPath)
    exit 0
}

# ---- 2. profile guard --------------------------------------------------------
. (Join-Path $root 'scripts\_profile-guard.ps1')
Start-ProfileGuard -NoKill
trap { Say-PG ("PROFILE-GUARD-TRAP: " + $_.Exception.Message); Stop-ProfileGuard; exit 1 }

# ---- 3. Win32 helpers --------------------------------------------------------
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class C {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr h, int pos);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr h, int pos);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr h, uint item, [Out] StringBuilder sb, int max, uint flags);

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
    // Walk the whole menu tree; record every item whose text carries a TAB tail:
    // tail -> id. Tail text is language independent, so no caption is matched.
    public static void Walk(IntPtr hmenu, Dictionary<string, int> found) {
        int n = GetMenuItemCount(hmenu);
        for (int i = 0; i < n; ++i) {
            IntPtr sub = GetSubMenu(hmenu, i);
            if (sub != IntPtr.Zero) Walk(sub, found);
            var sb = new StringBuilder(256);
            GetMenuStringW(hmenu, (uint)i, sb, 256, 0x0400 /*MF_BYPOSITION*/);
            string t = sb.ToString();
            int tab = t.IndexOf('\t');
            if (tab < 0) continue;
            found[t.Substring(tab + 1)] = (int)GetMenuItemID(hmenu, i);
        }
    }
}
"@

# ---- 4. attach ---------------------------------------------------------------
if ($AttachPid -le 0) {
    $mine = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue |
              Where-Object { $_.Path -like (Join-Path $root 'build\bin\Release\*') })
    if ($mine.Count -eq 0) {
        Die 'PRECONDITION-NOPROC' "no dev-build xfsWinPad is running; start it with '$cppPath' and pass -AttachPid"
    }
    if ($mine.Count -gt 1) {
        Die 'PRECONDITION-MULTIPROC' ("several dev-build instances: " + (($mine | ForEach-Object { $_.Id }) -join ','))
    }
    $AttachPid = $mine[0].Id
}
[C]::pid = [uint32]$AttachPid
[C]::LocateFrame()
if ([C]::frame -eq [IntPtr]::Zero) { Die 'PRECONDITION-FRAME' "no xfsWinPadMainWindow owned by pid $AttachPid" }
Say ("attached: pid=" + $AttachPid + " frame=0x" + ([C]::frame).ToString('X'))

$fail = 0

# ---- 5. menu: the two chord items + the toggle command id --------------------
$found = New-Object 'System.Collections.Generic.Dictionary[string,int]'
[C]::Walk([C]::GetMenu([C]::frame), $found)
foreach ($k in $found.Keys) { Say ("  menu tail [" + $k + "] -> cmd " + $found[$k]) }

if (-not $found.ContainsKey('Ctrl+K, Ctrl+C')) { Say "  FAIL: no menu item annotated 'Ctrl+K, Ctrl+C'"; $fail++ }
if (-not $found.ContainsKey('Ctrl+K, Ctrl+U')) { Say "  FAIL: no menu item annotated 'Ctrl+K, Ctrl+U'"; $fail++ }
if (-not $found.ContainsKey('Ctrl+Q'))         { Say "  FAIL: no menu item annotated 'Ctrl+Q' (toggle)"; $fail++ }
if ($fail -gt 0) { Die 'MENU-TAILS' "the Convert menu items are not wired/annotated as expected" }
$commentId   = $found['Ctrl+K, Ctrl+C']
$uncommentId = $found['Ctrl+K, Ctrl+U']
$toggleId    = $found['Ctrl+Q']

# ---- 6. toolbar: the toggle button is installed ------------------------------
$tb = [C]::GetDlgItem([C]::frame, 1103)
if ($tb -eq [IntPtr]::Zero) { Die 'PRECONDITION-TOOLBAR' "toolbar (id 1103) not found on the frame" }
# TB_COMMANDTOINDEX (WM_USER+25) is value-returning and needs no cross-process buffer.
$idx = [C]::S($tb, 0x0419, [IntPtr]$toggleId, [IntPtr]::Zero).ToInt64()
Say ("toolbar: TB_COMMANDTOINDEX(cmd " + $toggleId + ") = " + $idx)
if ($idx -lt 0) { Say "  FAIL: the toggle button is not on the toolbar"; $fail++ }

# ---- 7. find the fixture editor and its baseline text ------------------------
function Normalize([string]$s) { return $s.Replace("`r", "") }
$deadline = (Get-Date).AddSeconds(15)
$ed = [IntPtr]::Zero
$base = ""
while ((Get-Date) -lt $deadline -and $ed -eq [IntPtr]::Zero) {
    [void]([C]::ListEditors())
    foreach ($e in [C]::eds) {
        $t = [C]::Doc($e)
        if ((Normalize $t) -eq $origNorm) { $ed = $e; $base = $t; break }
    }
    if ($ed -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 400 }
}
if ($ed -eq [IntPtr]::Zero) { Die 'PRECONDITION-ED' "no editor holds the fixture text (did the app get the file argument?)" }
Say ("channel: editor=0x" + $ed.ToString('X') + " chars=" + $base.Length)
if ($base.Length -eq 0) { Die 'PRECONDITION-CHANNEL' "the editor reads empty; text channel is not proven" }

function EditorText { return [C]::Doc($ed) }
function Cmd([int]$id) {
    # 0x0111 = WM_COMMAND
    [void][C]::PostMessageW([C]::frame, 0x0111, [IntPtr]$id, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 350
}
function Sel([int]$a, [int]$b) { [void][C]::S($ed, $sci['SCI_SETSEL'], [IntPtr]$a, [IntPtr]$b) }
function SelectAllDoc() {
    $l = [C]::S($ed, $sci['SCI_GETLENGTH'], [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    Sel 0 $l
}
function Show([string]$t) { return ($t.Replace("`n", "\n")) }

# ---- 8a. current line only (no selection) -----------------------------------
# Shape (a) of the user's request: "the line the caret is on". With NO selection the
# command must touch ONLY the caret line - not the blank line, not the line below it.
Sel 0 0
Cmd $commentId
$a1 = Normalize (EditorText)
Say ("A. comment(caret on line 1) -> [" + (Show $a1) + "]")
if ($a1 -ne "//alpha`n`nbeta`n") { Say ("  FAIL: expected [//alpha\n\nbeta\n] (only the caret line), got [" + (Show $a1) + "]"); $fail++ }

Cmd $uncommentId
$a2 = Normalize (EditorText)
Say ("A. uncomment -> [" + (Show $a2) + "]")
if ($a2 -ne $origNorm) { Say ("  FAIL: current-line comment must round-trip to the original, got [" + (Show $a2) + "]"); $fail++ }

# ---- 8b. multi-line selection skips the blank line ---------------------------
# Shape (b): a multi-line selection where the blank line must be left untouched.
SelectAllDoc
Cmd $commentId
$c1 = Normalize (EditorText)
Say ("B. comment(select all) -> [" + (Show $c1) + "]")
if ($c1 -ne $comNorm) { Say ("  FAIL: expected [" + (Show $comNorm) + "], got [" + (Show $c1) + "]"); $fail++ }

# idempotent: commenting already-commented lines changes nothing
Cmd $commentId
$c2 = Normalize (EditorText)
Say ("B. comment #2 -> [" + (Show $c2) + "]")
if ($c2 -ne $comNorm) { Say "  FAIL: a second Comment must be a no-op"; $fail++ }

Cmd $uncommentId
$u1 = Normalize (EditorText)
Say ("B. uncomment -> [" + (Show $u1) + "]")
if ($u1 -ne $origNorm) { Say "  FAIL: uncomment must round-trip to the original"; $fail++ }

# strict: uncommenting lines that carry no prefix changes nothing
Cmd $uncommentId
$u2 = Normalize (EditorText)
Say ("B. uncomment #2 -> [" + (Show $u2) + "]")
if ($u2 -ne $origNorm) { Say "  FAIL: a second Uncomment must be a no-op"; $fail++ }

# ---- 9. verdict --------------------------------------------------------------
if ($fail -eq 0) {
    Say "VERDICT: COMMENT-CHORD-E2E-PASS (menu tails, toolbar button, comment/uncomment wiring; caret-line and multi-line-selection shapes, blank line skipped)"
    Stop-ProfileGuard
    exit 0
}
Say ("VERDICT: COMMENT-CHORD-E2E-FAIL (" + $fail + " failed assertion(s))")
Stop-ProfileGuard
exit 1
