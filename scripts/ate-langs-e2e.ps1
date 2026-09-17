param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# ate-langs-e2e.ps1 - Batch 72 e2e: the self-built ATE ILexer5 lexers must really
# attach to the live editor and produce the designed styles.
#
# Why this exists: tests/test_atelexer.cpp drives Lex()/Fold() directly through a
# fake IDocument, so it can never prove the Editor wiring works - SCI_SETILEXER
# accepting our ILexer5, the theme-role tables actually being applied, styles
# surviving Scintilla's lazy styling. That is exactly the gap where a broken
# release could slip out, so it is checked here against the real process.
#
#   P1 .pat  -> SCI_GETLEXER == 7201, per-byte styles match SCE_ATEP_*
#   P2 .stil -> SCI_GETLEXER == 7202, per-byte styles match SCE_STIL_*
#   P3 .log  -> SCI_GETLEXER == 7203, per-byte styles match SCE_ATEL_* and the
#               conservative gate keeps a generic app log line uncolored
#   plus a check that drive/expect/mask resolve to three DISTINCT colors.
#
# 【为什么用 SCI_GETLEXER(4002) 而不是 SCI_GETLEXERLANGUAGE(4012) 判定词法器】
#   本脚本跨进程 SendMessageW 到 xfsWinPad.exe。Win32 只为 0..WM_USER(1024)
#   的消息封送参数，SCI_* 全在 1024 以上，指针原样传递——4012 让目标进程往
#   「调用方的地址」里写语言名，等于让 xfsWinPad 拿本进程的地址往自己的地址
#   空间里 memcpy。实测后果：那一次探针之后，连 SCI_GETTEXTLENGTH 都返回 0，
#   因为目标进程已经被写崩了。所以 4012 这类「缓冲区出参」的消息在本脚本里
#   **一律不许出现**；下面用到的消息全部通过返回值传值，跨进程安全。
#   4002 返回的 7201/7202/7203 正是 XfsCreateLexer() 里 名字->词法器 的一一映射，
#   等价证明了「自研词法器真的挂上去了」。名字本身由 test_atelexer 的工厂用例
#   在进程内断言（TestFactory）。
#
# The real %APPDATA%\xfsWinPad\session.json is backed up and restored.
# ASCII only.

$ErrorActionPreference = "Stop"

# ---- SCE_* numbers (src/language/XfsLexerStyles.h, base 64) -------------------
$S_ATEP_COMMENT   = 65
$S_ATEP_OPCODE    = 66
$S_ATEP_LABEL     = 67
$S_ATEP_PIN       = 68
$S_ATEP_VECTOR    = 69   # drive
$S_ATEP_EXPECT    = 70
$S_ATEP_MASK      = 71
$S_ATEP_NUMBER    = 73
$S_ATEP_STRING    = 74
$S_ATEP_OPERATOR  = 75
$S_ATEP_DIRECTIVE = 77
$S_STIL_BLOCK     = 80
$S_STIL_KEYWORD   = 81
$S_STIL_NAME      = 82
$S_STIL_UNIT      = 84
$S_ATEL_DEFAULT   = 88
$S_ATEL_SITE      = 91
$S_ATEL_TESTNAME  = 92
$S_ATEL_FAIL      = 96

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class ATE {
  public delegate bool EnumProc(IntPtr h,IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static uint pid;
  public static IntPtr frame = IntPtr.Zero;
  private static bool FS(IntPtr h, IntPtr l){
    uint w; GetWindowThreadProcessId(h, out w);
    if (w == pid) { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "xfsWinPadMainWindow") { frame = h; return false; } }
    return true; }
  public static void LocateFrame(){ frame = IntPtr.Zero; EnumWindows(new EnumProc(FS), IntPtr.Zero); }
  // Enumerate ALL Scintilla children, including hidden ones: the app keeps more
  // than one editor control around, and a hidden one is easy to grab by mistake
  // (see the visible-filter lesson in TODO.md). The caller picks by text length.
  public static IntPtr[] Editors(){
    var list = new System.Collections.Generic.List<IntPtr>();
    if (frame == IntPtr.Zero) return list.ToArray();
    EnumChildWindows(frame, (h,l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "Scintilla") { list.Add(h); } return true; }, IntPtr.Zero);
    return list.ToArray(); }
  public static int StyleAt(IntPtr ed, int pos){ return (int)SendMessageW(ed, 2010, (IntPtr)pos, IntPtr.Zero); }
  public static int LineStart(IntPtr ed, int line){ return (int)SendMessageW(ed, 2167, (IntPtr)line, IntPtr.Zero); }
  public static int StyleFore(IntPtr ed, int style){ return (int)SendMessageW(ed, 2481, (IntPtr)style, IntPtr.Zero); }
  // Scintilla hands colours back in 0x00BBGGRR (the old COLORREF order), so a raw
  // print reads backwards: theme RGB(0x09,0x86,0x58) comes out as 0x588609.
  // Always render through this so logs can be compared with Theme.cpp by eye.
  public static string HexRgb(int v){ return string.Format("0x{0:X2}{1:X2}{2:X2}",
      v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF); }
  // SCI_GETENDSTYLED (2028): how far Scintilla has actually styled. This is the
  // diagnostic that separates "lexer produced wrong styles" from "lexer was
  // never asked yet" - both look like style 0 at the probe position.
  public static int EndStyled(IntPtr ed){ return (int)SendMessageW(ed, 2028, IntPtr.Zero, IntPtr.Zero); }
  // SCI_COLOURISE (4003) start=0 end=-1 -> style the whole document now. Value
  // args only, so it survives the cross-process boundary. Needed because
  // Scintilla styles lazily on paint, and a freshly launched window may not have
  // painted the region we probe.
  public static void ColouriseAll(IntPtr ed){ SendMessageW(ed, 4003, IntPtr.Zero, new IntPtr(-1)); }
  // SCI_GETLEXER returns the ILexer5 identifier (XfsLexerBase ctor passes 720x)
  public static int GetLexerId(IntPtr ed){ return (int)SendMessageW(ed, 4002, IntPtr.Zero, IntPtr.Zero); }
  public static int TextLength(IntPtr ed){ return (int)SendMessageW(ed, 2183, IntPtr.Zero, IntPtr.Zero); }
  // DELIBERATELY ABSENT: SCI_GETLEXERLANGUAGE (4012). It makes Scintilla write
  // the language name into a caller-supplied buffer, and this harness talks to
  // xfsWinPad.exe from ANOTHER process. Win32 only marshals arguments for
  // messages in 0..WM_USER(1024); every SCI_* is above that, so the pointer goes
  // across raw and the target writes into *our* address as seen from its own
  // space - i.e. it scribbles on its own heap and dies. Measured: after that one
  // probe every later SendMessage (even SCI_GETTEXTLENGTH) returned 0 because
  // the app was already gone. Every message below returns through the call's
  // RETURN VALUE instead, which crosses processes safely.
}
"@

$sess = Join-Path $env:APPDATA "xfsWinPad\session.json"
$bak  = "$sess.e2eatebak"
$work = Join-Path $env:TEMP ("xfs-ate-" + (Get-Date).Ticks)
$g_p  = $null
$g_ed = [IntPtr]::Zero

function Cleanup {
  Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 400
  if (Test-Path $bak) { Copy-Item $bak $sess -Force; Remove-Item $bak -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
function Fail($m) {
  Write-Output "E2E-FAIL: $m"
  Cleanup
  exit 1
}

function Start-App([string]$file) {
  # NOTE: $script: is mandatory here. A plain assignment inside a function
  # creates a *function-local* variable, so $g_ed/$g_p would stay null in the
  # script scope and every probe would end up messaging HWND 0.
  if ($script:g_p -ne $null) {
    Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
  }
  if (-not (Test-Path $file)) { Fail "sample file was not created: $file" }
  $info = Get-Item $file
  Write-Output ("  [diag] opening {0} ({1} bytes)" -f $info.FullName, $info.Length)

  # CLI files bypass session restore entirely (StartupSession: files first, then
  # return), so no --no-restore is needed; --new is avoided so single-instance
  # forwarding behaves normally.
  $script:g_p = Start-Process -FilePath $Exe -ArgumentList "`"$file`"" -PassThru
  $dl = (Get-Date).AddSeconds(60)
  while ($script:g_p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
    Start-Sleep -Milliseconds 300; $script:g_p.Refresh() }
  if ($script:g_p.MainWindowHandle -eq 0) { Fail "no main window for $file" }
  [ATE]::pid = [uint32]$script:g_p.Id

  # Wait for a Scintilla child that actually holds this document's text.
  $script:g_ed = [IntPtr]::Zero
  $dl = (Get-Date).AddSeconds(30)
  while ($script:g_ed -eq [IntPtr]::Zero -and (Get-Date) -lt $dl) {
    [ATE]::LocateFrame()
    foreach ($h in [ATE]::Editors()) {
      if ([ATE]::TextLength($h) -gt 0) { $script:g_ed = $h; break }
    }
    if ($script:g_ed -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 300 } }
  if ($script:g_ed -eq [IntPtr]::Zero) {
    foreach ($h in [ATE]::Editors()) {
      Write-Output ("  [diag] editor hwnd={0} len={1} visible={2}" -f `
                    $h, [ATE]::TextLength($h), [ATE]::IsWindowVisible($h)) }
    Fail "no Scintilla editor holding text for $file" }

  # Force a full colourise and wait until Scintilla reports the whole document
  # styled. Scintilla styles lazily on paint; without this the probe can race
  # that pass and read style 0 at every position, which looks exactly like "the
  # lexer attached but never ran" even though the wiring is fine.
  $dl = (Get-Date).AddSeconds(20)
  $styled = 0
  do {
    [ATE]::ColouriseAll($script:g_ed)
    Start-Sleep -Milliseconds 250
    $styled = [ATE]::EndStyled($script:g_ed)
    $need   = [ATE]::TextLength($script:g_ed)
  } while ($styled -lt $need -and (Get-Date) -lt $dl)
  Write-Output ("  [diag] endStyled={0} / {1}" -f $styled, $need)
  if ($styled -le 0) {
    Fail "Scintilla styled nothing (endStyled=$styled) - no lexer running?" }
  $script:g_dumped = $false   # allow one style-map dump per fixture
}

function Expect-Lexer([int]$wantId, [string]$wantName) {
  $len = [ATE]::TextLength($g_ed)
  $id  = [ATE]::GetLexerId($g_ed)
  Write-Output "  [diag] textLength=$len lexerId=$id"
  if ($len -le 0) { Fail "editor is empty (len=$len) - probed the wrong window?" }
  if ($id -ne $wantId) {
    Fail "lexer id = $id, want $wantId ('$wantName') - self-built lexer not attached" }
  Write-Output "  OK lexer id=$id ($wantName)"
}

function Expect-Style([int]$line, [int]$col, [int]$want, [string]$what) {
  # A vanished document means the probe is no longer measuring the lexer; say so
  # instead of reporting a bogus style mismatch.
  if ([ATE]::TextLength($g_ed) -le 0) {
    Fail "$what : editor went away (textLength=0) - did xfsWinPad crash?" }
  $pos = [ATE]::LineStart($g_ed, $line) + $col
  $got = [ATE]::StyleAt($g_ed, $pos)
  if ($got -ne $want) {
    Dump-Styles
    Fail "$what : line $line col $col (pos $pos) style = $got, want $want" }
  Write-Output ("  OK {0,-40} style = {1}" -f $what, $got)
}

# Print the whole style byte map as runs, plus the endStyled watermark. This is
# what turns "style 0 at the probe position" from a guess into a fact: it shows
# whether the document is uniformly default (lexer never ran / pushed nothing)
# or shifted by one (offset bug), which need opposite fixes.
$script:g_dumped = $false
function Dump-Styles {
  if ($script:g_dumped) { return }
  $script:g_dumped = $true
  $n = [ATE]::TextLength($g_ed)
  $runs = New-Object System.Collections.ArrayList
  $prev = -1; $cnt = 0
  for ($i = 0; $i -lt $n; $i++) {
    $s = [ATE]::StyleAt($g_ed, $i)
    if ($s -ne $prev) {
      if ($cnt -gt 0) { [void]$runs.Add(("{0}x{1}" -f $prev, $cnt)) }
      $prev = $s; $cnt = 1
    } else { $cnt++ }
  }
  if ($cnt -gt 0) { [void]$runs.Add(("{0}x{1}" -f $prev, $cnt)) }
  Write-Output ("  [diag] style runs by byte: {0}" -f ($runs -join " "))
  Write-Output ("  [diag] endStyled={0} textLength={1}" -f `
                [ATE]::EndStyled($g_ed), $n)
}

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path $sess) { Copy-Item $sess $bak -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
  # ---------------------------------------------------------------- P1: .pat
  Write-Output "[P1] .pat"
  $pat = Join-Path $work "t.pat"
  (@(
    '# comment line'
    'RPT 4'
    '( VDD RESET ) 0 1'
    'LBL_A:'
    '    JMP LBL_A'
    '.INCLUDE "x"'
    '( VDD RESET ) X N Z'
  ) -join "`r`n") + "`r`n" | Set-Content -Path $pat -Encoding ASCII

  Start-App $pat
  Expect-Lexer 7201 "ate_pattern"
  Expect-Style 0 0  $S_ATEP_COMMENT   "line0 '#' comment"
  Expect-Style 1 0  $S_ATEP_OPCODE    "line1 'RPT' opcode"
  Expect-Style 1 4  $S_ATEP_NUMBER    "line1 '4' counter number"
  Expect-Style 2 0  $S_ATEP_OPERATOR  "line2 '(' operator"
  Expect-Style 2 2  $S_ATEP_PIN       "line2 'VDD' pin in group"
  Expect-Style 2 6  $S_ATEP_PIN       "line2 'RESET' pin (also an opcode)"
  Expect-Style 2 14 $S_ATEP_VECTOR    "line2 '0' drive"
  Expect-Style 2 16 $S_ATEP_VECTOR    "line2 '1' drive"
  Expect-Style 3 0  $S_ATEP_LABEL     "line3 'LBL_A:' label def"
  Expect-Style 4 4  $S_ATEP_OPCODE    "line4 'JMP' opcode"
  Expect-Style 4 8  $S_ATEP_LABEL     "line4 'LBL_A' jump target"
  Expect-Style 5 0  $S_ATEP_DIRECTIVE "line5 '.INCLUDE' directive"
  Expect-Style 5 9  $S_ATEP_STRING    "line5 quoted string"
  # line6 exists purely so SCE_ATEP_MASK is really produced by the live lexer -
  # a colour check on a style that never occurs would be vacuous.
  Expect-Style 6 14 $S_ATEP_MASK      "line6 'X' mask"
  Expect-Style 6 16 $S_ATEP_MASK      "line6 'N' mask"
  Expect-Style 6 18 $S_ATEP_MASK      "line6 'Z' mask"

  # drive / expect / mask must resolve to three DISTINCT colors - the whole
  # point of splitting them instead of lumping them into one number style.
  $fDrive  = [ATE]::StyleFore($g_ed, $S_ATEP_VECTOR)
  $fExpect = [ATE]::StyleFore($g_ed, $S_ATEP_EXPECT)
  $fMask   = [ATE]::StyleFore($g_ed, $S_ATEP_MASK)
  # STYLE_DEFAULT (32) carries the body text colour, so this is the check with
  # teeth: mask must NOT look like ordinary text. Asserting only "the three
  # differ from each other" passes happily while mask == editorFg, which is
  # exactly the defect this batch shipped and then fixed (RDim role).
  $fBody   = [ATE]::StyleFore($g_ed, 32)
  Write-Output ("  [diag] drive={0} expect={1} mask={2} body(STYLE_DEFAULT)={3}" -f `
                [ATE]::HexRgb($fDrive), [ATE]::HexRgb($fExpect),
                [ATE]::HexRgb($fMask), [ATE]::HexRgb($fBody))
  if ($fDrive -eq $fExpect -or $fExpect -eq $fMask -or $fDrive -eq $fMask) {
    Fail ("vector styles not visually distinct: drive={0} expect={1} mask={2}" -f `
          [ATE]::HexRgb($fDrive), [ATE]::HexRgb($fExpect), [ATE]::HexRgb($fMask)) }
  if ($fMask -eq $fBody) {
    Fail ("mask colour {0} equals the body text colour - X/N/Z/U would be " +
          "indistinguishable from an ordinary identifier" -f [ATE]::HexRgb($fMask)) }
  Write-Output "  OK drive/expect/mask distinct, and mask is not body text"
  Write-Output "P1-OK"

  # --------------------------------------------------------------- P2: .stil
  Write-Output "[P2] .stil"
  $stil = Join-Path $work "t.stil"
  (@(
    'Signals {'
    "'CLK' In;"
    "Period '20ns';"
    '}'
  ) -join "`r`n") + "`r`n" | Set-Content -Path $stil -Encoding ASCII

  Start-App $stil
  Expect-Lexer 7202 "stil"
  Expect-Style 0 0 $S_STIL_BLOCK   "line0 'Signals' block keyword"
  Expect-Style 1 0 $S_STIL_NAME    "line1 'CLK' quoted name"
  Expect-Style 1 6 $S_STIL_KEYWORD "line1 'In' direction keyword"
  Expect-Style 2 0 $S_STIL_KEYWORD "line2 'Period' keyword"
  Expect-Style 2 7 $S_STIL_UNIT    "line2 '20ns' quantity with unit"
  Write-Output "P2-OK"

  # ---------------------------------------------------------------- P3: .log
  Write-Output "[P3] .log (conservative gate)"
  $log = Join-Path $work "t.log"
  (@(
    'SITE1 VDD 1.8 FAIL [1.0, 2.0]'
    '2026-09-17 08:12:20 INFO Service started successfully'
  ) -join "`r`n") + "`r`n" | Set-Content -Path $log -Encoding ASCII

  Start-App $log
  Expect-Lexer 7203 "ate_log"
  Expect-Style 0 0  $S_ATEL_SITE    "line0 'SITE1' site marker"
  Expect-Style 0 6  $S_ATEL_TESTNAME "line0 'VDD' test name"
  Expect-Style 0 10 $S_ATEL_FAIL    "line0 measurement escalated (FAIL line)"
  Expect-Style 0 14 $S_ATEL_FAIL    "line0 'FAIL' verdict"
  Expect-Style 0 19 $S_ATEL_FAIL    "line0 limit escalated (FAIL line)"
  # The conservative gate: a PROPER subset of ATE evidence must not be enough.
  # Line1 is a timestamped app log with no SITE, no verdict and no limit range,
  # so every byte of it must stay SCE_ATEL_DEFAULT.
  Expect-Style 1 0  $S_ATEL_DEFAULT "line1 generic log, line start uncolored"
  Expect-Style 1 20 $S_ATEL_DEFAULT "line1 generic log, 'INFO' uncolored"
  Expect-Style 1 25 $S_ATEL_DEFAULT "line1 generic log, test name uncolored"
  Write-Output "P3-OK"

  Write-Output "ATE-LANGS-E2E-PASS"
  Cleanup
  exit 0
} catch {
  Fail $_.Exception.Message
}
