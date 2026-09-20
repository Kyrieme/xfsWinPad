param(
    [string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe",
    [string]$Stub = "",
    [int]$AttachPid = 0,
    [switch]$FixtureOnly,
    [switch]$KeepFixture
)
# craft-e2e.ps1 - the CRAFT compile-output panel must really jump on a double-click.
#
# Why this exists
# ---------------
# Batch 96 built the panel; batches 97/99 nailed the output parser down with 286
# assertions against six byte-exact samples taken from a real CRAFT install. What
# none of that can prove is the part the user actually touches: the rows reach the
# ListView, a double-click on a jumpable row resolves the compiler's path, opens
# the file and puts the caret on the reported line. That whole segment was only
# ever checked by eye - and v0.13.2 changed the jump target, which is exactly when
# an unverified segment is most dangerous.
#
# How a machine without CRAFT can still exercise the real path
# -----------------------------------------------------------
# The app resolves the toolchain from CRAFT_HOME when no tool directory is set by
# hand (CraftHost.cpp: settings dir -> CRAFT_HOME\bin -> PATH). So this script
# points CRAFT_HOME at a directory holding two copies of craft_stub.exe named
# plncmp.exe / patcmp.exe. The stub only replaces the *outermost compiler
# process*: LoadProject -> DetectToolchainFromEnv -> PlanBuild -> RunPlan ->
# RowsFromBuild all run for real. That is deliberate - CraftHost.h's header says a
# probe that uses its own copy of the code proves nothing ("both sides silently
# diverge, and the result is 'probe green, UI broken'"). CRAFT_HOME\bin is also
# one of the two layouts the vendor makefiles actually use, so this exercises a
# real configuration rather than an invented one.
#
# Nothing outside %TEMP% is written: CRAFT_HOME is an environment variable, so
# the user's settings.json is only ever read (see the guard in step 2).
#
# The samples are the batch-99 real-machine outputs, byte for byte (they are pure
# ASCII, so no code-page games are needed here). Three message syntaxes appear in
# one run:
#   step 1  plncmp     -> "<< File:[...] Line:[N] >>" + "Message:[Error ...]"
#   step 2  patcmp -c  -> ".\PAT\x.pat(17): error C1000: ..." (four of them)
#   step 3  patcmp -f  -> absolute ".pdt(49)" path, which the parser rewrites to .pat
#
# What is asserted
# ----------------
#   A. the panel really fills with the exact number of rows the parser implies
#   B. the summary line carries the right step/total/jumpable numbers
#   C. double-clicking a jumpable row moves the caret to the reported line *and*
#      that line is the one the fixture wrote there (checked against the file
#      content, not against the parser's own opinion)
#   D. double-clicking a row with no location (step header, record header,
#      statistics line, "Time used") leaves every open document untouched
#
# What is deliberately NOT asserted
# ---------------------------------
# The text inside the ListView cells is not read back: LVM_GETITEMTEXT takes a
# pointer to a caller-side buffer and is not marshalled across processes, so
# reading it would mean VirtualAllocEx-ing inside the target. The column text is
# covered in-process by test_craftproj's parser cases plus CompilePanel::Update's
# straight copy; this script's claim is the jump, not the rendering.
#
# Switches
# --------
#   -FixtureOnly   build the fixture, verify the sample byte counts, print the
#                  CRAFT_HOME / .pln paths, and stop. Needed when the app has to
#                  be started by the caller (see -AttachPid).
#   -AttachPid N   do not launch anything; drive the already-running instance N.
#                  For environments that forbid spawning processes, and for
#                  debugging a live window. The caller owns that process and must
#                  close it (so session.json is not touched here).
#   -KeepFixture   leave %TEMP%\xfs_craft_e2e in place for inspection.
#
# ASCII only. This is not a style preference. Without a BOM, PowerShell reads a
# .ps1 file using the ANSI code page, so a UTF-8 comment can shift byte parity,
# swallow a line break, and then produce a *phantom* syntax error reported at an
# unrelated line (observed while editing this file: "unexpected }" at a line that
# contained no brace at all). Keep every byte under 0x80.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Output "FAIL xfsWinPad.exe not found: $Exe"
    Write-Output "CRAFT-E2E-FAIL"
    exit 1
}
if ([string]::IsNullOrWhiteSpace($Stub)) {
    $Stub = Join-Path $repoRoot "build\bin\Release\craft_stub.exe"
}
if (-not (Test-Path -LiteralPath $Stub)) {
    Write-Output "FAIL craft_stub.exe not found: $Stub"
    Write-Output "Build first: cmake --build build --config Release"
    Write-Output "CRAFT-E2E-FAIL"
    exit 1
}

# ---------------------------------------------------------------------------
# Message ids are parsed from the source headers, never hardcoded - batch 73
# taught this the hard way (a renamed/reordered set silently invalidated a whole
# probe script).
# ---------------------------------------------------------------------------
function Get-SciIds([string]$headerPath) {
    $map = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($headerPath)) {
        if ($line -match '^#define\s+(SCI_[A-Z0-9_]+)\s+(\d+)\s*$') {
            $map[$Matches[1]] = [int]$Matches[2]
        }
    }
    foreach ($want in 'SCI_GETCURRENTPOS', 'SCI_LINEFROMPOSITION', 'SCI_GOTOLINE') {
        if (-not $map.ContainsKey($want)) { throw "Scintilla.h has no $want" }
    }
    return $map
}

$sci = Get-SciIds (Join-Path $repoRoot "third_party\scintilla\include\Scintilla.h")
Write-Output ("[setup] SCI_GETCURRENTPOS={0} SCI_LINEFROMPOSITION={1} SCI_GOTOLINE={2}" -f `
    $sci['SCI_GETCURRENTPOS'], $sci['SCI_LINEFROMPOSITION'], $sci['SCI_GOTOLINE'])

# Cmd::BuildCompile (src/core/CommandIds.h). Read it rather than trusting a
# comment, for the same reason.
$cmdIds = @{}
foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $repoRoot "src\core\CommandIds.h"))) {
    if ($line -match '^\s*(BuildCompile)\s*=\s*(\d+)\s*,') { $cmdIds[$Matches[1]] = [int]$Matches[2] }
}
if (-not $cmdIds.ContainsKey('BuildCompile')) { throw "CommandIds.h has no BuildCompile" }
Write-Output ("[setup] BuildCompile={0}" -f $cmdIds['BuildCompile'])

# CompilePanel control ids (src/app/CompilePanel.cpp: ID_LIST / ID_LABEL).
$ID_LIST  = 1310
$ID_LABEL = 1311

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class CP {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageStr(IntPtr h,uint m,IntPtr w,StringBuilder s);
    [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageLen(IntPtr h,uint m,IntPtr w,IntPtr l);

    public static uint pid;
    public static IntPtr frame = IntPtr.Zero;

    private static bool FrameScan(IntPtr h, IntPtr l){
        uint w; GetWindowThreadProcessId(h,out w);
        if(w==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadMainWindow"){ frame=h; return false; } }
        return true; }
    public static void LocateFrame(){ frame=IntPtr.Zero;
        EnumWindows(new EnumProc(FrameScan),IntPtr.Zero); }

    // CompilePanel is created lazily on the first compile, so this legitimately
    // returns zero until after WM_COMMAND BuildCompile.
    public static IntPtr FindPanel(){
        IntPtr found=IntPtr.Zero;
        if(frame==IntPtr.Zero) return found;
        EnumChildWindows(frame,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="xfsWinPadCompilePanel"){ found=h; return false; } return true; },
            IntPtr.Zero);
        return found; }

    // ALL Scintilla children, visible or not: the app keeps more than one editor
    // around and the one we want may be behind a tab. Callers identify documents
    // by their text, never by z-order.
    public static IntPtr[] Scintillas(){
        var list=new System.Collections.Generic.List<IntPtr>();
        if(frame==IntPtr.Zero) return list.ToArray();
        EnumChildWindows(frame,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
            if(c.ToString()=="Scintilla"){ list.Add(h); } return true; },IntPtr.Zero);
        return list.ToArray(); }

    // WM_GETTEXT/WM_GETTEXTLENGTH are marshalled by USER32, so unlike the
    // LVM_GETITEM* family they are safe to use across processes.
    public static string DocText(IntPtr h){
        int len=(int)SendMessageLen(h,0x000E,IntPtr.Zero,IntPtr.Zero);
        if(len<=0) return "";
        var sb=new StringBuilder(len+4);
        SendMessageStr(h,0x000D,(IntPtr)(len+1),sb);
        return sb.ToString(); }

    // ---------------------------------------------------------------------
    // Real mouse input. The ListView ignores synthetic WM_LBUTTONDOWN, so the
    // double-click has to be a genuine one -- which is also what the user does.
    // ---------------------------------------------------------------------
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x,y; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx,dy; public uint mouseData,dwFlags,time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);

    private static INPUT NewInput(){ var i=new INPUT(); i.type=0; return i; }

    // MOUSEEVENTF_MOVE|ABSOLUTE|VIRTUALDESK; absolute coords are normalised to
    // 0..65535 across the virtual screen, which is why the metrics are needed.
    public static void MoveTo(int px,int py){
        const int SM_XV=76, SM_YV=77, SM_CXV=78, SM_CYV=79;
        int vx=GetSystemMetrics(SM_XV), vy=GetSystemMetrics(SM_YV);
        int cx=GetSystemMetrics(SM_CXV), cy=GetSystemMetrics(SM_CYV);
        if(cx<=0) cx=1;
        if(cy<=0) cy=1;
        INPUT mv=NewInput();
        mv.u.mi.dwFlags=0x0001|0x8000|0x4000;
        mv.u.mi.dx=(int)((px-vx)*65536.0/cx);
        mv.u.mi.dy=(int)((py-vy)*65536.0/cy);
        SendInput(1,new INPUT[]{mv},Marshal.SizeOf(typeof(INPUT))); }

    public static void Click(){
        INPUT d=NewInput(); d.u.mi.dwFlags=0x0002;   // LEFTDOWN
        INPUT u=NewInput(); u.u.mi.dwFlags=0x0004;   // LEFTUP
        SendInput(1,new INPUT[]{d},Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(30);
        SendInput(1,new INPUT[]{u},Marshal.SizeOf(typeof(INPUT))); }

    public static void RealClick(int px,int py){
        MoveTo(px,py); System.Threading.Thread.Sleep(40); Click(); }

    // Two clicks inside the system double-click time (default 500ms) and at the
    // same point, which is what WM_LBUTTONDBLCLK requires.
    public static void RealDoubleClick(int px,int py){
        MoveTo(px,py); System.Threading.Thread.Sleep(120);
        Click(); System.Threading.Thread.Sleep(60); Click(); }
}
"@

# ListView message ids. LVM_FIRST = 0x1000 and the offsets come from commctrl.h.
# 0x1013 / 0x1027 are LVM_FIRST+19 and LVM_FIRST+39. Getting these wrong is
# silent and catastrophic: LVM_FIRST+15 is LVM_SETITEMPOSITION and LVM_FIRST+27 is
# LVM_INSERTCOLUMNA, whose NULL pszText fails and returns exactly -1 -- which is
# the "-1 topIndex" that once sent every click below the last row.
$LVM_FIRST          = 0x1000
$LVM_GETITEMCOUNT   = $LVM_FIRST + 4     # 0x1004
$LVM_GETNEXTITEM    = $LVM_FIRST + 12    # 0x100C
$LVM_ENSUREVISIBLE  = $LVM_FIRST + 19    # 0x1013
$LVM_GETTOPINDEX    = $LVM_FIRST + 39    # 0x1027
$LVNI_SELECTED      = 0x0002
$WM_COMMAND         = 0x0111
$WM_CLOSE           = 0x0010
# Scrolling. In a report-view ListView SB_LINEDOWN moves exactly one row.
$WM_VSCROLL         = 0x0115
$SB_LINEDOWN        = 1
$SB_TOP             = 6

# [why not one synthetic WM_LBUTTONDOWN / WM_LBUTTONDBLCLK is sent]
#   Measured: SendMessage-ing WM_LBUTTONDOWN straight to THIS ListView leaves the
#   selection completely unmoved (six dy values 0/10/30/60/100/150 all left
#   LVM_GETNEXTITEM(LVNI_SELECTED) on the same row), while one real SendInput click
#   selects immediately. So everything below uses real input.
#   This note was paid for: an earlier revision sent synthetic messages, which made
#   "the double-click jump does not work" look true - the real cause was wrong
#   coordinates (the two LVM offsets above).
#   Do NOT generalise this into "synthetic never works": batch 101 used the
#   find-results list as a control - in the SAME process, that ListView's synthetic
#   DBLCLK DOES trigger the jump (see dblclick-test.ps1). The accurate statement is
#   "synthetic is unreliable and control-dependent", not "synthetic is useless".
#   Real input passed on both controls, which is why it is the uniform tool here.

$script:fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Output "  PASS $what" }
    else     { Write-Output "  FAIL $what"; $script:fail++ }
}

# ===========================================================================
# 1. Fixture
# ===========================================================================
# The folders are resolved through the shell API rather than %TEMP% / %APPDATA%:
# those variables are absent in some hosts, and more importantly the app itself
# locates its settings with SHGetKnownFolderPath(FOLDERID_RoamingAppData), so the
# script should agree with the app rather than with an environment variable.
$fx        = Join-Path ([System.IO.Path]::GetTempPath()) "xfs_craft_e2e"
$projDir   = Join-Path $fx "proj"
$patDir    = Join-Path $projDir "PAT"
$craftHome = Join-Path $fx "crafthome"
$stubDir   = Join-Path $craftHome "bin"
$smpDir    = Join-Path $stubDir "samples"

# NOTE: only a run that OWNS the process may wipe this directory. Under -AttachPid
# the process under test was started by the caller *pointing at this directory*, so
# deleting it here - or at the end - is not ours to do. Measured: one attach run
# deleted the whole fixture, and the NEXT attach to the same live GUI died with
# "the fixture .pln never loaded into an editor" - which reads like the app failing
# to load a document, when really the script pulled the file out from under it.
# The files below are still rebuilt (identical content), so this only removes the
# delete-then-recreate window.
if (Test-Path -LiteralPath $fx) {
    if ($AttachPid -eq 0) {
        Remove-Item -LiteralPath $fx -Recurse -Force
    } else {
        Write-Output "[fixture] -AttachPid: reusing the existing fixture (the app points at it)"
    }
}
foreach ($d in @($projDir, $patDir, $stubDir, $smpDir)) {
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-Bytes([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, $utf8NoBom)
}

# ---- fixture sources -------------------------------------------------------
# Every line the samples point at carries a unique token, so the jump assertion
# can be checked against the file content instead of against the parser.
$plnPath = Join-Path $projDir "open_short.pln"
$patPath = Join-Path $patDir  "ls299_func.pat"
$decPath = Join-Path $patDir  "ls299_pin.dec"

$MARK_PLN = "// xfsWinPad craft-e2e fixture: open_short.pln"
$MARK_PAT = "// xfsWinPad craft-e2e fixture: ls299_func.pat"
$MARK_DEC = "// xfsWinPad craft-e2e fixture: ls299_pin.dec"

$patLines = @($MARK_PAT, "// The compiler sample reports errors on lines 17, 22, 35 and 49.")
for ($i = 3; $i -le 60; $i++) { $patLines += "; filler line $i" }
foreach ($n in 17, 22, 35, 49) { $patLines[$n - 1] = "@CRAFT-E2E-$n@" }

$decLines = @($MARK_DEC, "// The declaration-file sample reports an error on line 24.")
for ($i = 3; $i -le 30; $i++) { $decLines += "; filler line $i" }
$decLines[23] = "@CRAFT-E2E-DEC-24@"

$plnLines = @($MARK_PLN, "// PLAN_SOURCE for the fixture makefile.")
for ($i = 3; $i -le 12; $i++) { $plnLines += "; filler line $i" }

Write-Bytes $patPath (($patLines -join "`r`n") + "`r`n")
Write-Bytes $decPath (($decLines -join "`r`n") + "`r`n")
Write-Bytes $plnPath (($plnLines -join "`r`n") + "`r`n")

# ---- vendor makefile -------------------------------------------------------
# Shape copied from the vendor examples (see test_craftproj.cpp): bare command
# form, PATH_PAT0 indirection, one .pat source. PlanBuild turns this into exactly
# three steps: plncmp, patcmp -c -s, patcmp -f makefile_pdt0.lst.
$makefile = @'
# ---------------------User Definition Area Begin-----------------------------
PATH_PAT0     = .\PAT
PLN_SOURCE   = open_short.pln
PLN_TARGET   = open_short.pin
PLN_CFLAGS   =
PAT_SOURCE0   =                 $(PATH_PAT0)\ls299_func.pat

PAT_CFLAGS0   =
PAT_LFLAGS0   =
PAT_TARGET0   =.\PAT\ls299_func.ppo
# ---------------------User Definition Area End------------------------------

$(PLN_TARGET): $(PLN_SOURCE) .\PAT\ls299_pin.dec
  @plncmp $(PLN_CFLAGS) $(PLN_SOURCE)

$(PAT_TARGET0):: .\PAT\ls299_pin.dec
  @patcmp $(PAT_CFLAGS0) $(PAT_LFLAGS0) -f makefile_pdt0.lst

.pat.pdt :
  @patcmp -c -s $(PAT_CFLAGS0) $<
'@
Write-Bytes (Join-Path $projDir "makefile") ($makefile -replace "`r?`n", "`r`n")

# ---- the three real-machine samples, byte for byte -------------------------
# kPlncmpDecDupName (387 bytes): the third message syntax. Location on the record
# header line, severity on the NEXT line's Message:[...]. The relayed part uses
# \r\r\n while CRAFT's own lines use \r\n - that mix is real, not a typo.
$CRLF   = "`r`n"
$CRCRLF = "`r`r`n"
$s_plncmp = @(
    'Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA',
    'default linked library : ws2_32.lib ',
    'Parse Plan open_short.pln : ',
    'Make Declaration File ...Compile Failed !! Exit Code(59)',
    'Delaration file compiler for CRAFT_3380_2.50 Copyright (c) 2011 CHROMA',
    '',
    '<< File:[.\PAT\ls299_pin.dec] Line:[24] Last_Token:[;] >>',
    '    Message:[Error : Duplicate Declare Pin "QA"]',
    ''
)
$t = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $s_plncmp.Count; $i++) {
    [void]$t.Append($s_plncmp[$i]); [void]$t.Append($(if ($i -le 3) { $CRLF } else { $CRCRLF }))
}
Write-Bytes (Join-Path $smpDir "plncmp.txt") $t.ToString()

# kPatcmpMultiErr (515 bytes): patcmp does NOT stop at the first error - one file
# with four defects reports all four, on lines 17/17/22/35.
$s_patcmp = @(
    'Compile .\PAT\ls299_func.pat   ...',
    'Make declaration file ... OK',
    '.\PAT\ls299_func.pat(17): error C1000: pin:[DQA] Message:[There is no symbol data for IO pin ! ]',
    '.\PAT\ls299_func.pat(17): error C1000: pin:[DQH] Message:[There is no symbol data for IO pin ! ]',
    '.\PAT\ls299_func.pat(22): error C1000: symbol:[BOGUS_MICRO] Message:[The word cat''t be recognized ! ]',
    '.\PAT\ls299_func.pat(35): error C1000: symbol:[TS16] Message:[The word cat''t be recognized ! ]',
    '          Errors :  4                    Warning : 0'
)
Write-Bytes (Join-Path $smpDir "patcmp.txt") (($s_patcmp -join $CRLF) + $CRLF)

# kPatcmpLinkLabelErr (408 bytes): the link step fails on a label the compile step
# accepted, and the location points at the .pdt intermediate - the parser rewrites
# it to .pat. The absolute prefix is re-pointed at this fixture so the
# absolute-path branch of JumpToCompileLocation resolves for real.
$ORIG_PREFIX = 'Z:\AI_Work\codex\xfsPad\temp\_vmcheck\badpat_label'
$s_link = @(
    'Start time of compilation : Sun Sep 20 05:24:14 2026',
    '',
    'Link .\PAT\ls299_func.pdt   ...',
    'Link .\PAT\ls299_TMU.pdt   ...',
    ($projDir + '\PAT\ls299_func.pdt(49): error C1000: label:[no_such_label] Message:[Used label is not defined ! ]'),
    '          Errors :  1                    Warning : 0',
    '',
    'End  time  of compilation : Sun Sep 20 05:24:14 2026',
    '',
    'Time used :  0  seconds'
)
Write-Bytes (Join-Path $smpDir "patcmp_link.txt") (($s_link -join $CRLF) + $CRLF)

# The link step is the one that failed on the real machine; making it fail here
# too is what exercises the "N/M step failed" summary.
Write-Bytes (Join-Path $smpDir "patcmp_link.exit") "1`r`n"

# ---- self-check the transcription ------------------------------------------
# The byte counts are the ones the real probe self-reported and that
# test_craftproj.cpp pins with static_assert. If this script's copy drifts, the
# fixture is wrong and every downstream assertion would test the wrong text.
Write-Output "[fixture] verifying sample byte counts"
$lenPlncmp = (Get-Item (Join-Path $smpDir "plncmp.txt")).Length
$lenPatcmp = (Get-Item (Join-Path $smpDir "patcmp.txt")).Length
$lenLink   = (Get-Item (Join-Path $smpDir "patcmp_link.txt")).Length
Check ($lenPlncmp -eq 387) ("plncmp sample is 387 bytes (got $lenPlncmp)")
Check ($lenPatcmp -eq 515) ("patcmp sample is 515 bytes (got $lenPatcmp)")
$wantLink = 408 + ($projDir.Length - $ORIG_PREFIX.Length)
Check ($lenLink -eq $wantLink) ("link sample is $wantLink bytes (got $lenLink)")

# ---- stub toolchain --------------------------------------------------------
Copy-Item -LiteralPath $Stub -Destination (Join-Path $stubDir "plncmp.exe") -Force
Copy-Item -LiteralPath $Stub -Destination (Join-Path $stubDir "patcmp.exe") -Force

Write-Output "[fixture] CRAFT_HOME = $craftHome"
Write-Output "[fixture] project    = $plnPath"

if ($FixtureOnly) {
    Write-Output "[fixture] -FixtureOnly: fixture ready, not launching the app"
    Write-Output ("CRAFT-E2E-FIXTURE-{0}" -f $(if ($script:fail -eq 0) { "READY" } else { "BROKEN" }))
    exit $(if ($script:fail -eq 0) { 0 } else { 1 })
}

# ===========================================================================
# 2. Guard: a hand-set tool directory outranks CRAFT_HOME
# ===========================================================================
# DetectToolchain tries the settings directory first, so a stale craftToolDir
# would silently send the build to a real CRAFT install and every assertion below
# would be about the wrong toolchain. Read-only: the file is never rewritten.
$settingsPath = Join-Path (Join-Path ([Environment]::GetFolderPath('ApplicationData')) "xfsWinPad") "settings.json"
if (Test-Path -LiteralPath $settingsPath) {
    $raw = [System.IO.File]::ReadAllText($settingsPath)
    if ($raw -match '"craftToolDir"\s*:\s*"([^"]*)"') {
        $handSet = $Matches[1]
        if (-not [string]::IsNullOrWhiteSpace($handSet)) {
            Write-Output "FAIL settings.json sets craftToolDir=$handSet, which takes priority"
            Write-Output "     over CRAFT_HOME - clear it in Tools > CRAFT tool directory, then rerun."
            Write-Output "CRAFT-E2E-FAIL"
            exit 1
        }
    }
}

$proc = $null
$exitCode = 1

# ---- session.json is rewritten on exit, so keep the user's copy -------------
# Only when we own the process: with -AttachPid the caller started it and is
# responsible for it, and we must not touch its session state.
$appDataDir    = Join-Path ([Environment]::GetFolderPath('ApplicationData')) "xfsWinPad"
$sessionPath   = Join-Path $appDataDir "session.json"
$sessionBackup = $null
$beforeFiles   = @{}
if ($AttachPid -eq 0 -and (Test-Path -LiteralPath $appDataDir)) {
    Get-ChildItem -LiteralPath $appDataDir -File -ErrorAction SilentlyContinue |
        ForEach-Object { $beforeFiles[$_.Name] = $true }
    if (Test-Path -LiteralPath $sessionPath) {
        $sessionBackup = Join-Path $fx "session.json.bak"
        Copy-Item -LiteralPath $sessionPath $sessionBackup -Force
    }
}

try {
    if ($AttachPid -ne 0) {
        Write-Output "[run] attaching to pid $AttachPid (caller owns the process)"
        [CP]::pid = [uint32]$AttachPid
    } else {
        # --new: never forward into a running editor. --no-restore: start blank, so
        # the "same file name already open" fallback cannot fire by accident.
        $env:CRAFT_HOME = $craftHome
        try {
            $proc = Start-Process -FilePath $Exe -PassThru `
                -ArgumentList @('--new', '--no-restore', "`"$plnPath`"")
        } finally {
            Remove-Item Env:\CRAFT_HOME -ErrorAction SilentlyContinue
        }
        [CP]::pid = [uint32]$proc.Id
        Write-Output "[run] launched pid $($proc.Id) with CRAFT_HOME=$craftHome"
    }

    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        [CP]::LocateFrame()
        if ([CP]::frame -ne [IntPtr]::Zero) { break }
    }
    if ([CP]::frame -eq [IntPtr]::Zero) { throw "main window never appeared (pid $([CP]::pid))" }
    Write-Output "[run] frame=$([CP]::frame)"

    # The document must be loaded before BuildCompile can find a project.
    $docReady = $false
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        foreach ($ed in [CP]::Scintillas()) {
            if ([CP]::DocText($ed).Contains($MARK_PLN)) { $docReady = $true; break }
        }
        if ($docReady) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $docReady) {
        # Under -AttachPid the script cannot open a document in the target - the
        # caller has to have launched it with the fixture .pln. Say so, because the
        # bare message reads like the app failed to load a file.
        throw ("the fixture .pln never loaded into an editor" + $(if ($AttachPid -ne 0) {
            " - under -AttachPid the GUI must be launched with this file on its command line: $plnPath"
        } else { "" }))
    }

    # =======================================================================
    # 3. Compile, and wait for the panel to fill
    # =======================================================================
    # Row layout implied by the parser (test_craftproj pins each sample's issue
    # count): per step one header row, then one row per non-blank output line.
    #   step 1: 1 + 7 = 8 rows    (rows 0..7)
    #   step 2: 1 + 7 = 8 rows    (rows 8..15)
    #   step 3: 1 + 7 = 8 rows    (rows 16..23)
    $EXPECTED_ROWS = 24
    $EXPECTED_JUMPABLE = 6

    Write-Output "[run] WM_COMMAND BuildCompile ($($cmdIds['BuildCompile']))"
    [CP]::PostMessageW([CP]::frame, $WM_COMMAND, [IntPtr]$cmdIds['BuildCompile'], [IntPtr]::Zero) | Out-Null

    $panel = [IntPtr]::Zero
    $list  = [IntPtr]::Zero
    $label = [IntPtr]::Zero
    $rows  = -1
    $stable = 0
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        if ($panel -eq [IntPtr]::Zero) {
            $panel = [CP]::FindPanel()
            if ($panel -eq [IntPtr]::Zero) { continue }
            $list  = [CP]::GetDlgItem($panel, $ID_LIST)
            $label = [CP]::GetDlgItem($panel, $ID_LABEL)
        }
        if ($list -eq [IntPtr]::Zero) { continue }
        $n = [int][CP]::SendMessageW($list, [uint32]$LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
        if ($n -eq $EXPECTED_ROWS) { $rows = $n; break }
        # A wrong-but-final count must be reported now, not after the timeout.
        if ($n -gt 0 -and $n -eq $rows) { $stable++; if ($stable -ge 3) { break } }
        else { $stable = 0 }
        $rows = $n
    }

    $summary = ""
    if ($label -ne [IntPtr]::Zero) {
        $len = [int][CP]::SendMessageLen($label, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)
        if ($len -gt 0) {
            $sb = New-Object System.Text.StringBuilder ($len + 4)
            [void][CP]::SendMessageStr($label, 0x000D, [IntPtr]($len + 1), $sb)
            $summary = $sb.ToString()
        }
    }

    Write-Output "[run] rows=$rows summary=[$summary]"
    if ($rows -ne $EXPECTED_ROWS) {
        # Do not let "the fixture is not wired up" masquerade as "the panel is
        # broken". Both look identical (wrong row count) but are fixed differently:
        #   1. under -AttachPid the script does NOT set CRAFT_HOME for the target
        #      process (only the self-launch path does), so the caller must pass it;
        #   2. a hand-set craftToolDir in settings.json outranks CRAFT_HOME.
        Write-Output "     note: expected $EXPECTED_ROWS rows; $rows means the target process is"
        Write-Output "     probably not using this fixture's toolchain. Under -AttachPid the"
        Write-Output "     caller must launch the GUI with CRAFT_HOME set to:"
        Write-Output "       $craftHome"
        Write-Output "     (a hand-set craftToolDir in settings.json outranks CRAFT_HOME)"
    }
    Check ($rows -eq $EXPECTED_ROWS) "panel holds $EXPECTED_ROWS rows (got $rows)"

    # A. summary numbers, matched language-independently: the panel text is
    # localised, so only the integers are compared. Expected "3/3" + jumpable 6.
    $nums = @([regex]::Matches($summary, '(?<!\d)\d+(?!\d)') | ForEach-Object { [int]$_.Value })
    Check (($nums.Count -eq 3) -and ($nums[0] -eq 3) -and ($nums[1] -eq 3) -and ($nums[2] -eq $EXPECTED_JUMPABLE)) `
        ("summary numbers are 3/3 + $EXPECTED_JUMPABLE (got [$($nums -join ',')])")

    if ($rows -ne $EXPECTED_ROWS) { throw "panel did not reach the expected shape - cannot continue" }

    # =======================================================================
    # 4. Calibrate the ListView geometry
    # =======================================================================
    # Row height and the top of the first row cannot be read back with a
    # value-returning message (LVM_GETITEMRECT wants a caller-side pointer), so
    # measure them: real-click a series of dy values and watch
    # LVM_GETNEXTITEM(LVNI_SELECTED) report which row each one selected.
    #
    # The scan deliberately stays clear of the bottom edge: clicking the partially
    # visible last row makes the control scroll itself, and that would move the
    # rows out from under the measurement.
    $cr = New-Object CP+RECT
    [CP]::GetClientRect($list, [ref]$cr) | Out-Null
    $listW = $cr.Right - $cr.Left
    $listH = $cr.Bottom - $cr.Top
    $probeX = [int]($listW * 0.3)
    Write-Output "[geometry] list=${listW}x${listH} probeX=$probeX"

    $script:listH  = $listH
    $script:probeX = $probeX

    function Get-SelectedRow {
        return [int][CP]::SendMessageW($list, [uint32]$LVM_GETNEXTITEM, [IntPtr](-1), [IntPtr]$LVNI_SELECTED)
    }
    function Get-TopIndex {
        return [int][CP]::SendMessageW($list, [uint32]$LVM_GETTOPINDEX, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    function Invoke-RealClickAt([int]$dy) {
        $pt = New-Object CP+POINT
        $pt.x = $script:probeX; $pt.y = $dy
        [CP]::ClientToScreen($list, [ref]$pt) | Out-Null
        [CP]::RealClick($pt.x, $pt.y)
        Start-Sleep -Milliseconds 60
    }

    # Real input goes to whatever window is under the cursor, so the frame must be
    # foreground or the clicks land on something else entirely.
    [CP]::SetForegroundWindow([CP]::frame) | Out-Null
    Start-Sleep -Milliseconds 400
    Check ([CP]::GetForegroundWindow() -eq [CP]::frame) "the frame is foreground (real input needs it)"
    # Hard stop, not just a failed Check: everything below moves the real cursor
    # and clicks. If the frame is not foreground those clicks would go to whatever
    # else happens to be on screen.
    if ([CP]::GetForegroundWindow() -ne [CP]::frame) { throw "frame is not foreground - refusing to send real clicks" }

    # Start from a known scroll position so the samples mean something.
    [CP]::SendMessageW($list, [uint32]$WM_VSCROLL, [IntPtr]$SB_TOP, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300

    $samples = @()
    $scanEnd = $listH - 24
    if ($scanEnd -lt 8) { $scanEnd = $listH - 1 }
    $lastSel = -999
    for ($dy = 2; $dy -le $scanEnd; $dy += 4) {
        Invoke-RealClickAt $dy
        $sel = Get-SelectedRow
        # -1 means the point was still inside the column-header band, which is not
        # a row at all; feeding those samples into the fit would inflate the pitch.
        if ($sel -ge 0) { $samples += ,@($dy, $sel) }
        if ($sel -ne $lastSel) { Write-Output "  [geometry] dy=$dy -> row $sel"; $lastSel = $sel }
    }

    # Two samples at least 3 rows apart give the row pitch; taking the widest pair
    # keeps the rounding error on $rowH well under a pixel.
    $rowH = 0.0
    $firstRowTop = -1.0
    for ($i = 0; $i -lt $samples.Count; $i++) {
        for ($j = $samples.Count - 1; $j -gt $i; $j--) {
            $dRow = $samples[$j][1] - $samples[$i][1]
            if ($dRow -ge 3) {
                $rowH = [double]($samples[$j][0] - $samples[$i][0]) / $dRow
                $firstRowTop = $samples[$i][0] - $samples[$i][1] * $rowH
                break
            }
        }
        if ($rowH -gt 0) { break }
    }
    if ($firstRowTop -lt 0) { $firstRowTop = 0 }
    Write-Output ("[geometry] firstRowTop={0:N1} rowH={1:N2}" -f $firstRowTop, $rowH)
    # A report-view row is one line of text plus padding. Anything outside this
    # band means the measurement itself is broken, and every assertion built on it
    # would be meaningless rather than merely wrong.
    Check (($rowH -ge 8) -and ($rowH -le 60)) ("row height is plausible (got {0:N2})" -f $rowH)
    if (($rowH -lt 8) -or ($rowH -gt 60)) { throw "cannot measure the ListView row height" }

    $script:SCI_GETCURRENTPOS    = $sci['SCI_GETCURRENTPOS']
    $script:SCI_LINEFROMPOSITION = $sci['SCI_LINEFROMPOSITION']
    $script:SCI_GOTOLINE         = $sci['SCI_GOTOLINE']
    $script:firstRowTop = $firstRowTop
    $script:rowH        = $rowH

    function Get-CaretLine([IntPtr]$ed) {
        $pos = [int][CP]::SendMessageW($ed, [uint32]$script:SCI_GETCURRENTPOS, [IntPtr]::Zero, [IntPtr]::Zero)
        return [int][CP]::SendMessageW($ed, [uint32]$script:SCI_LINEFROMPOSITION, [IntPtr]$pos, [IntPtr]::Zero)
    }
    function Get-DocLines([IntPtr]$ed) {
        return ([CP]::DocText($ed) -split "`r?`n")
    }
    function Get-EditorSnapshot {
        $parts = @()
        foreach ($ed in [CP]::Scintillas()) {
            $txt = [CP]::DocText($ed)
            if ([string]::IsNullOrEmpty($txt)) { continue }
            $first = ($txt -split "`r?`n", 2)[0]
            $parts += ("{0}@{1}" -f $first, (Get-CaretLine $ed))
        }
        return (($parts | Sort-Object) -join ' | ')
    }
    function Find-Editor([string]$marker) {
        foreach ($ed in [CP]::Scintillas()) {
            if ([CP]::DocText($ed).Contains($marker)) { return $ed }
        }
        return [IntPtr]::Zero
    }

    # Point at row `row`, prove the point really is on it, and only then send a
    # real double-click. The proof is what stops a wrong row height from turning
    # into a confident but meaningless assertion.
    function Invoke-RowDoubleClick([int]$row) {
        # Put the row near the TOP of the viewport: SB_TOP, then one SB_LINEDOWN
        # per row. That keeps the click well clear of the bottom edge, where
        # clicking the partially visible last row makes the control scroll itself
        # and move the row out from under the cursor.
        [CP]::SendMessageW($list, [uint32]$WM_VSCROLL, [IntPtr]$SB_TOP, [IntPtr]::Zero) | Out-Null
        for ($k = 0; $k -lt $row; $k++) {
            [CP]::SendMessageW($list, [uint32]$WM_VSCROLL, [IntPtr]$SB_LINEDOWN, [IntPtr]::Zero) | Out-Null
        }
        Start-Sleep -Milliseconds 200
        $top = Get-TopIndex
        if ($top -lt 0) { $top = 0 }

        $dy  = [int]($script:firstRowTop + ($row - $top) * $script:rowH + $script:rowH / 2)
        $sel = -1
        for ($i = 0; $i -lt 8; $i++) {
            if (($dy -lt 0) -or ($dy -ge $script:listH)) {
                Write-Output "  .. row ${row}: dy=$dy is outside the client area (top=$top)"
                return $false
            }
            Invoke-RealClickAt $dy
            $sel = Get-SelectedRow
            if ($sel -eq $row) { break }
            $delta = $row - $sel
            if ([Math]::Abs($delta) -gt 15) {
                Write-Output "  .. row ${row}: dy=$dy selected $sel, too far off to nudge"
                return $false
            }
            $dy += [int]($delta * $script:rowH)
        }
        if ($sel -ne $row) {
            Write-Output "  .. row ${row}: could not point at it (last dy=$dy selected $sel)"
            return $false
        }

        # Same point as the click that just selected the row, and nothing scrolls
        # in between, so the double-click still lands on `row`.
        $pt = New-Object CP+POINT
        $pt.x = $script:probeX; $pt.y = $dy
        [CP]::ClientToScreen($list, [ref]$pt) | Out-Null
        [CP]::SetForegroundWindow([CP]::frame) | Out-Null
        Start-Sleep -Milliseconds 150
        [CP]::RealDoubleClick($pt.x, $pt.y)
        return $true
    }

    # =======================================================================
    # 5. Jumpable rows must jump to the reported line - and that line must be
    #    the one the fixture wrote there.
    # =======================================================================
    # row -> (file marker, 1-based line, token that must be on that line)
    $cases = @(
        @{ row =  7; mark = $MARK_DEC; line = 24; token = '@CRAFT-E2E-DEC-24@'; why = 'dec record: Message line carries the location' },
        @{ row = 11; mark = $MARK_PAT; line = 17; token = '@CRAFT-E2E-17@';     why = 'patcmp multi-error #1 (line 17)' },
        @{ row = 12; mark = $MARK_PAT; line = 17; token = '@CRAFT-E2E-17@';     why = 'patcmp multi-error #2 (same line 17)' },
        @{ row = 13; mark = $MARK_PAT; line = 22; token = '@CRAFT-E2E-22@';     why = 'patcmp multi-error #3 (line 22)' },
        @{ row = 14; mark = $MARK_PAT; line = 35; token = '@CRAFT-E2E-35@';     why = 'patcmp multi-error #4 (line 35)' },
        @{ row = 20; mark = $MARK_PAT; line = 49; token = '@CRAFT-E2E-49@';     why = 'link error: absolute .pdt path rewritten to .pat' }
    )

    Write-Output "[run] double-clicking jumpable rows"
    foreach ($c in $cases) {
        $want = $c.line - 1     # SCI_LINEFROMPOSITION is 0-based

        # Two rows report the same line (17/17), and later rows target a document
        # an earlier row already opened. Without moving the caret away first, a
        # double-click that did nothing at all would still look like a pass.
        $ed = Find-Editor $c.mark
        if ($ed -ne [IntPtr]::Zero) {
            [CP]::SendMessageW($ed, [uint32]$script:SCI_GOTOLINE, [IntPtr]0, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 60
        }

        if (-not (Invoke-RowDoubleClick $c.row)) { Check $false "row $($c.row): $($c.why)"; continue }

        $got  = -1
        $deadline = (Get-Date).AddSeconds(3)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 150
            $ed = Find-Editor $c.mark
            if ($ed -eq [IntPtr]::Zero) { continue }
            $got = Get-CaretLine $ed
            if ($got -eq $want) { break }
        }
        Check ($got -eq $want) ("row {0}: caret on line {1} (got {2}) - {3}" -f $c.row, $c.line, ($got + 1), $c.why)

        $ed = Find-Editor $c.mark
        $okToken = $false
        if ($ed -ne [IntPtr]::Zero) {
            $lines = Get-DocLines $ed
            if ($want -lt $lines.Count) { $okToken = $lines[$want].Contains($c.token) }
        }
        Check $okToken ("row {0}: line {1} really holds {2}" -f $c.row, $c.line, $c.token)
    }

    # =======================================================================
    # 6. Rows without a location must do nothing at all
    # =======================================================================
    # Checked after the jumps, so the set of open documents has settled and an
    # unchanged snapshot is meaningful.
    $noJump = @(
        @{ row =  0; why = 'step header row (ours, not the compiler output)' },
        @{ row =  6; why = 'record header: has File:/Line: but the location belongs to the next line' },
        @{ row = 15; why = 'patcmp statistics line' },
        @{ row = 23; why = 'link "Time used" line' }
    )

    Write-Output "[run] double-clicking non-jumpable rows must be a no-op"
    foreach ($c in $noJump) {
        $before = Get-EditorSnapshot
        if (-not (Invoke-RowDoubleClick $c.row)) { Check $false "row $($c.row) no-op: $($c.why)"; continue }
        Start-Sleep -Milliseconds 700
        $after = Get-EditorSnapshot
        Check ($before -eq $after) ("row {0}: nothing moved - {1}" -f $c.row, $c.why)
    }

    Write-Output "[result] failures = $script:fail"
    $exitCode = if ($script:fail -eq 0) { 0 } else { 1 }
}
catch {
    Write-Output "FAIL exception: $_"
    $exitCode = 1
}
finally {
    # Only touch session state when we own the process: with -AttachPid the caller
    # started it and is responsible for it.
    if ($AttachPid -eq 0) {
        if ($proc -and -not $proc.HasExited) {
            [CP]::PostMessageW([CP]::frame, [uint32]$WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            if (-not $proc.WaitForExit(8000)) { Stop-Process -Id $proc.Id -Force }
        }
        if ($null -ne $sessionBackup) {
            Copy-Item -LiteralPath $sessionBackup -Destination $sessionPath -Force -ErrorAction SilentlyContinue
        }
        Get-ChildItem -LiteralPath $appDataDir -File -ErrorAction SilentlyContinue | ForEach-Object {
            if (-not $beforeFiles.ContainsKey($_.Name)) {
                Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
            }
        }
    }
    # Same rule as the wipe at the top: in -AttachPid mode the fixture is the
    # caller's, and they may want to attach again to the same live GUI. Deleting it
    # here is what made a second attach run fail.
    if (-not $KeepFixture -and $AttachPid -eq 0) {
        Remove-Item -LiteralPath $fx -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Output $(if ($exitCode -eq 0) { "CRAFT-E2E-PASS" } else { "CRAFT-E2E-FAIL" })
exit $exitCode
