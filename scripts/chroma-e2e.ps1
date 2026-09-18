param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# chroma-e2e.ps1 - Batch 73 end-to-end: the Chroma 3380 language pack and the
# signature hint, against a REAL xfsWinPad.exe.
#
# Why this exists (and why it is not a unit test):
#   tests/test_atelexer.cpp and tests/test_chromasig.cpp drive the lexers and the
#   position parser directly, through a fake IDocument and pure functions. They can
#   never prove that:
#     - the self-built ILexer5 actually attaches to a live Scintilla control,
#       per file extension, through LanguageMap + XfsCreateLexer;
#     - SCN_CHARADDED on a real keystroke really reaches Editor::HandleSignatureHint,
#       and that the calltip + dropdown really come up where we think;
#     - Tab really accepts the item (that path lives in ScintillaBase::KeyCommand,
#       NOT in our code, so no unit test of ours can cover it);
#     - the "not a CRAFT compilation result" note is scoped to Chroma files only.
#   Those are exactly the seams where a green test suite can still ship a broken
#   feature, so they are checked here against the real process.
#
#   P1 .pat     -> chroma/ate_pattern lexer attaches, per-byte styles match the
#                  manual's vector classification (drive / compare / drive+compare /
#                  mask / control), vector delimiters, timeset and comment.
#   P2 .pln     -> chroma_plan lexer attaches; block / test-statement / comment,
#                  plus a #include preprocessor line (batch-74 hang guard - the
#                  real Eagle/Merlin .pln headers are full of those and the lexer
#                  once spun forever on them)
#   P3 .pln     -> SIGNATURE HINT: typing inside FORCE_V_MLDPS arg 3 (v_range)
#                  raises the dropdown (NOT the calltip - Scintilla cancels one
#                  when the other shows) anchored at the argument we typed in;
#                  Tab accepts, and item 0 is the manual's first entry.
#   P3B .pln    -> a free parameter (arg 2, f_volt) raises the calltip instead.
#   P4 .pln     -> i_range really carries the manual's SEVEN current ranges. This
#                  is the end-to-end guard for the silent data bug found in 5.3
#                  (value-list truncation). Counting is done by saturating the
#                  listbox selection with Down and reading the index back.
#   P5 .txt     -> NO hint outside the Chroma family (regression guard for the
#                  36 pre-existing languages).
#   P6 .pln/.txt-> the status-bar note part is non-empty for Chroma files and
#                  empty for everything else.
#   P7 .pln     -> STATEMENT-NAME COMPLETION (batch 77): at a STATEMENT POSITION a
#                  four-character prefix raises the dropdown, Tab accepts, and what
#                  lands in the document is the manual's first match in CHAPTER
#                  order (FORCE_I_MLDPS, manual 4.3) - not the alphabetically first
#                  name. A one-character prefix must stay silent, and the same
#                  prefix in a .txt must stay silent too.
#   P8 .pln     -> CONTINUATION AFTER ACCEPTANCE (batch 78): Tab on a statement
#                  candidate must leave `NAME(` in the document and raise the
#                  signature hint (SCI_CALLTIPACTIVE), because the caret lands
#                  inside the argument list. P8B is the negative control: a
#                  block-header statement (`TEST_PRO {` in the manual) must be
#                  inserted bare - appending '(' there would be wrong code.
#   P9 .pln     -> SIGNATURE DATA COMPLETENESS (batch 79): RELAY_ON - the 2nd most
#                  frequent statement in a real .pln - completes, gets a '(' and
#                  raises the manual's signature. P9B is its negative control:
#                  RF_Initialize must be inserted BARE, because the signature it
#                  used to carry was lifted from the manual's Example section
#                  (`_IP,"192.168.1.2"`, `"CableLoss.ini"`) and batch 79 removed it.
#                  Showing no hint beats showing example literals as a parameter list.
#   P10 .pln    -> STATIC DIAGNOSTICS (batch 87): a rule-8 violation draws the
#                  error squiggle (indicator 10) on the statement name and fills
#                  status-bar part 7; P10B is the clean-file negative control.
#   P11 .pat    -> CROSS-FILE RULE 3 (batch 88): the referenced .dec declares
#                  DEC_MODE APAS, the .pat uses IMATCH -> warning squiggle
#                  (indicator 11) on the IMATCH token. The dec is resolved on
#                  disk relative to the pattern's directory. P11B: same .pat,
#                  .dec without DEC_MODE -> everything stays silent.
#   P12 .pln    -> CROSS-FILE DEC-SYMBOL COMPLETION (batch 89): the dec defines
#                  Vdps (manual 2.7.2's own example symbol); typing "Vdp" in the
#                  plan raises the word-completion dropdown from the dec symbol
#                  and Tab inserts "Vdps". P12B: a dec without a VDP* symbol
#                  keeps the dropdown silent (the hit really came from the dec).
#
# HARD RULE - the verdict is CHROMA-E2E-PASS / CHROMA-E2E-FAIL, and the exit code now agrees:
#   A passing run prints CHROMA-E2E-PASS and exits 0. Historically it exited 1 as
#   well, because Cleanup (which deletes the %APPDATA%\xfsWinPad\session.json
#   backup it made at startup) sits inside the main try block, and this sandbox's
#   safe-delete hook makes that Remove-Item THROW - the exception message is the
#   hook's own JSON, so the outer catch reported it through Fail() and the run
#   looked failed while every assertion had passed (batches 77 and 78 both lost
#   time to it). Cleanup now swallows per-step failures and re-checks the facts
#   instead; anything it could not do shows up as a CLEANUP-WARN line.
#
# HARD RULE - no buffer out-parameters, ever:
#   This harness talks to xfsWinPad.exe from ANOTHER process, and Win32 only
#   marshals arguments for messages below WM_USER. Every SCI_* is above it, so a
#   pointer argument crosses raw and the target writes into ITS OWN address space
#   at our address - i.e. it scribbles on its own heap. Measured in batch 72:
#   after one SCI_GETLEXERLANGUAGE probe every later SendMessage returned 0
#   because the app was already dead. So everything below is either a value-only
#   message or a WM_CHAR/WM_KEYDOWN, and every assertion reads the RETURN VALUE.
#   Consequence: we cannot read the autocomplete LIST text. We get the same
#   guarantee a different way - SCI_AUTOCGETCURRENT + the document-length delta
#   after Tab tells us exactly which item was accepted and how long it is.
#   ONE exception, and it is a Win32-sanctioned one: WM_GETTEXT (0x0D) is below
#   WM_USER, and USER32 marshals it as a text buffer even across processes, so
#   P7 uses it to read the document back and check the accepted name by text.
#
# HARD RULE - numbers are PARSED, never hardcoded:
#   Batch 73 renumbered the whole .pat style group, which silently rotted the
#   batch-72 copy of this script (it still asserted the old numbers). Style ids
#   now come from src/language/XfsLexerStyles.h and lexer ids from
#   src/language/XfsLexer.h (kOwnLexers). If a name is missing the script FAILS
#   instead of asserting against a stale constant.
#
# ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI unless there is a BOM).
# The real %APPDATA%\xfsWinPad\session.json is backed up and restored.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stylesH  = Join-Path $repoRoot "src\language\XfsLexerStyles.h"
$lexerH   = Join-Path $repoRoot "src\language\XfsLexer.h"

# ---- messages we use (all value-returning) -----------------------------------
$SCI_GETCURRENTPOS     = 2008
$SCI_GOTOPOS           = 2025
$SCI_GETLINEENDPOS     = 2136
$SCI_GETTEXTLENGTH     = 2183
$SCI_AUTOCACTIVE       = 2102
$SCI_AUTOCPOSSTART     = 2103
$SCI_AUTOCGETCURRENT   = 2445
$SCI_CALLTIPACTIVE     = 2202
$SCI_GETLEXER          = 4002
$SCI_STYLEGETFORE      = 2481
$SCI_STYLEGET           = 2010
$SCI_GETENDSTYLED      = 2028
$SCI_COLOURISE         = 4003
$SB_GETTEXTLENGTHW     = 1036      # WM_USER+12, takes only a part index -> safe
$WM_KEYDOWN            = 0x0100
$WM_KEYUP              = 0x0101
$WM_CHAR               = 0x0102
$VK_TAB                = 0x09
$VK_DOWN               = 0x28

# ---- style ids / lexer ids come from the headers (shared with the other
# ---- E2E harnesses; see scripts/_lexer-ids.ps1 for why they are parsed rather
# ---- than hardcoded).
. (Join-Path $PSScriptRoot "_lexer-ids.ps1")
$script:styles   = Get-StyleMap $stylesH
$script:lexerMap = Get-LexerMap $lexerH
Write-Output ("[setup] parsed {0} style ids, {1} own lexers" -f $script:styles.Count, $script:lexerMap.Count)
foreach ($k in ($script:lexerMap.Keys | Sort-Object)) {
  Write-Output ("  [diag] lexer {0} = {1}" -f $k, $script:lexerMap[$k])
}

# ---- statement data comes from the generated DB, not from this script --------
# P7 asserts "the accepted completion is the manual's first match in CHAPTER
# order", so it has to know what that is. Parsing Chroma3380Db.cpp (the same
# source the lexer and the completion module use) keeps the expectation honest:
# if the generator ever reorders or drops a statement, the probe follows the data
# instead of asserting a frozen string. The literal name is ALSO asserted, since
# FORCE_I_MLDPS (manual 4.3) is the requirement, not an implementation detail.
$script:dbH = Join-Path $repoRoot "src\language\Chroma3380Db.cpp"

function Get-FirstStatementFor([string]$lexerName, [string]$prefix) {
  $src = [IO.File]::ReadAllText($script:dbH, [Text.Encoding]::UTF8)
  # the word tables that decide which names a file type accepts
  $tables = switch ($lexerName) {
    "chroma_plan" { @("kPlanStmtWords", "kPlanBlockWords", "kPlanClibWords") }
    "ate_pattern" { @("kPatModuleWords", "kPatMicroWords") }
    "chroma_dec"  { @("kDecBlockWords", "kDecNameWords") }
    default       { Fail "Get-FirstStatementFor: unknown lexer $lexerName" }
  }
  $words = @{}
  foreach ($v in $tables) {
    $m = [regex]::Match($src, ('const char\* const ' + $v + ' =(?<b>.*?);'),
                        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $m.Success) { Fail "cannot parse $v out of Chroma3380Db.cpp" }
    foreach ($lit in [regex]::Matches($m.Groups['b'].Value, '"([^"]*)"')) {
      foreach ($w in $lit.Groups[1].Value.Split(' ')) { if ($w) { $words[$w] = $true } }
    }
  }
  # kStatements is in manual (chapter) order - the first match is the expectation
  foreach ($m in [regex]::Matches($src, '\{"([A-Za-z_0-9]+)", "[0-9.]+"')) {
    $n = $m.Groups[1].Value
    if ($n.StartsWith($prefix, 'OrdinalIgnoreCase') -and $words.ContainsKey($n)) { return $n }
  }
  return $null
}

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public static class CH {
  public delegate bool EnumProc(IntPtr h,IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EnumProc f,IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
  public static uint pid;
  public static IntPtr frame = IntPtr.Zero;
  private static bool FS(IntPtr h, IntPtr l){
    uint w; GetWindowThreadProcessId(h, out w);
    if (w == pid) { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "xfsWinPadMainWindow") { frame = h; return false; } }
    return true; }
  public static void LocateFrame(){ frame = IntPtr.Zero; EnumWindows(new EnumProc(FS), IntPtr.Zero); }
  // Enumerate ALL children by class name, hidden ones included: the app keeps
  // more than one Scintilla control around and a hidden one is easy to grab by
  // mistake, so callers pick by document length.
  public static IntPtr[] ChildrenByClass(string cls){
    var list = new System.Collections.Generic.List<IntPtr>();
    if (frame == IntPtr.Zero) return list.ToArray();
    EnumChildWindows(frame, (h,l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == cls) { list.Add(h); } return true; }, IntPtr.Zero);
    return list.ToArray(); }
  // Same, but rooted anywhere (a docked panel's own children, not the frame's).
  // EnumChildWindows walks the whole subtree, so this also finds nested controls.
  public static IntPtr[] ChildrenOf(IntPtr root, string cls){
    var list = new System.Collections.Generic.List<IntPtr>();
    if (root == IntPtr.Zero) return list.ToArray();
    EnumChildWindows(root, (h,l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == cls) { list.Add(h); } return true; }, IntPtr.Zero);
    return list.ToArray(); }
  public static IntPtr[] Editors(){ return ChildrenByClass("Scintilla"); }
  // value-only helpers
  public static int TextLength(IntPtr ed){ return (int)SendMessageW(ed, 2183, IntPtr.Zero, IntPtr.Zero); }
  public static int GetLexerId(IntPtr ed){ return (int)SendMessageW(ed, 4002, IntPtr.Zero, IntPtr.Zero); }
  public static int CurrentPos(IntPtr ed){ return (int)SendMessageW(ed, 2008, IntPtr.Zero, IntPtr.Zero); }
  public static int LineEndPos(IntPtr ed, int line){ return (int)SendMessageW(ed, 2136, (IntPtr)line, IntPtr.Zero); }
  public static int GotoPos(IntPtr ed, int pos){ return (int)SendMessageW(ed, 2025, (IntPtr)pos, IntPtr.Zero); }
  public static int StyleAt(IntPtr ed, int pos){ return (int)SendMessageW(ed, 2010, (IntPtr)pos, IntPtr.Zero); }
  public static int EndStyled(IntPtr ed){ return (int)SendMessageW(ed, 2028, IntPtr.Zero, IntPtr.Zero); }
  public static void ColouriseAll(IntPtr ed){ SendMessageW(ed, 4003, IntPtr.Zero, new IntPtr(-1)); }
  public static int AutoCActive(IntPtr ed){ return (int)SendMessageW(ed, 2102, IntPtr.Zero, IntPtr.Zero); }
  public static int AutoCPosStart(IntPtr ed){ return (int)SendMessageW(ed, 2103, IntPtr.Zero, IntPtr.Zero); }
  public static int AutoCCurrent(IntPtr ed){ return (int)SendMessageW(ed, 2445, IntPtr.Zero, IntPtr.Zero); }
  public static int CallTipActive(IntPtr ed){ return (int)SendMessageW(ed, 2202, IntPtr.Zero, IntPtr.Zero); }
  public static int StyleFore(IntPtr ed, int style){ return (int)SendMessageW(ed, 2481, (IntPtr)style, IntPtr.Zero); }
  public static string HexRgb(int v){ return string.Format("0x{0:X2}{1:X2}{2:X2}",
      v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF); }
  public static void TypeChar(IntPtr ed, int ch){ SendMessageW(ed, 0x0102, (IntPtr)ch, IntPtr.Zero); }
  public static void PressKey(IntPtr ed, int vk){
    SendMessageW(ed, 0x0100, (IntPtr)vk, IntPtr.Zero);
    SendMessageW(ed, 0x0101, (IntPtr)vk, IntPtr.Zero); }
  public static int StatusPartLen(IntPtr sb, int part){
    return (int)((SendMessageW(sb, 1036, (IntPtr)part, IntPtr.Zero).ToInt64()) & 0xFFFF); }
  // ---- batch 87: static-check probes ----------------------------------------
  // SCI_INDICATORVALUEAT(2507) takes two INTEGERS and returns 1 when that
  // indicator is set at that position; nothing crosses as a pointer, so it is
  // safe from another process (see the WM_USER note above).
  // ARGUMENT ORDER - this bit us once: Scintilla.iface says
  // `IndicatorValueAt(int indicator, position pos)`, i.e. indicator is WPARAM
  // and pos is LPARAM. Swapping them makes every probe read position
  // "indicator" (a 1-digit offset) and report "no squiggle anywhere", which
  // looks exactly like "the feature is not wired at all".
  public static int IndicatorAt(IntPtr ed, int pos, int ind){
    return (int)SendMessageW(ed, 2507, (IntPtr)ind, (IntPtr)pos); }
  // LVM_GETITEMCOUNT (LVM_FIRST+4) is a value-returning listview message.
  public static int ListCount(IntPtr lv){
    return (int)SendMessageW(lv, 0x1004, IntPtr.Zero, IntPtr.Zero); }
  // WM_COMMAND is below WM_USER, so USER32 marshals it for us - this is how the
  // harness drives a menu command in the running app.
  public static void Command(IntPtr frame, int id){
    SendMessageW(frame, 0x0111, (IntPtr)id, IntPtr.Zero); }
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  // WM_GETTEXT (0x0D) is below WM_USER, so USER32 marshals the buffer for us -
  // this is the ONLY way to read a Scintilla document from another process
  // (SCI_GETTEXT would hand the target our pointer; see the header).
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  public static string GetText(IntPtr ed){
    int n = TextLength(ed);
    var sb = new StringBuilder(n + 2);
    SendMessageW(ed, 0x000D, (IntPtr)(n + 1), sb);
    return sb.ToString(); }
}
"@

$sess = Join-Path $env:APPDATA "xfsWinPad\session.json"
$bak  = "$sess.e2echromabak"
$work = Join-Path $env:TEMP ("xfs-chroma-" + (Get-Date).Ticks)
$g_p  = $null
$g_ed = [IntPtr]::Zero
$g_sb = [IntPtr]::Zero

function Cleanup {
  # 【为什么每一步都包 try】——批次 78 查清的坑，别再"以为是环境噪声"放过去：
  # Cleanup 是在主体那个 try 块里被调用的，而本沙箱的 safe-delete 钩子会让
  # 对 %APPDATA% 路径的 Remove-Item **直接抛异常**，异常消息就是钩子那段 JSON
  # （"[safe-delete][SAFE_DELETE_FAIL_CLOSED] {"target":…}"）。于是：
  #   抛异常 → 外层 catch → Fail($_.Exception.Message) → 打印一行 E2E-FAIL + exit 1
  # 结果是**所有断言都过、CHROMA-E2E-PASS 也打了，退出码却是 1**，
  # 而且那行失败文本和真失败长得一模一样（批次 77/78 两次都被它误导）。
  # 所以收尾不许改判定：单步失败就地吞掉，改成事后**核对事实**再报告。
  try { Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force } catch { }
  Start-Sleep -Milliseconds 400
  # 先 Copy 再 Remove：即使 Remove 被钩子拦下，session.json 也已经还原了。
  try { if (Test-Path $bak) { Copy-Item $bak $sess -Force; Remove-Item $bak -Force } } catch { }
  try { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue } catch { }
  # 只报告真正要紧的两件事（判定仍由 CHROMA-E2E-PASS / CHROMA-E2E-FAIL 决定）
  if (-not (Test-Path $sess)) { Write-Output "CLEANUP-WARN: session.json missing after cleanup" }
  if (Test-Path $bak) { Write-Output "CLEANUP-WARN: leftover backup (sandbox blocked its removal): $bak" }
}
function Fail($m) {
  # 前缀带脚本名（git64-e2e.ps1 也是这个惯例）：日志里一眼看出是哪条链红的。
  Write-Output "CHROMA-E2E-FAIL: $m"
  Cleanup
  exit 1
}
function Write-Fixture([string]$path, [string[]]$lines) {
  # -NoNewline style: write CRLF explicitly and do not append a trailing EOL, so
  # line-end positions are exactly what the fixture says.
  $text = ($lines -join "`r`n")
  [System.IO.File]::WriteAllText($path, $text, [System.Text.Encoding]::ASCII)
}

# $requireStyled: only files that attach one of our own ILexer5 produce style
# bytes. A plain .txt has no lexer at all, so Scintilla never advances the
# endStyled watermark there - waiting for it would hang until the timeout and
# then fail for a reason that has nothing to do with what P5 is checking.
function Start-App([string]$file, [bool]$requireStyled = $true) {
  if ($script:g_p -ne $null) {
    Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
  }
  if (-not (Test-Path $file)) { Fail "fixture was not created: $file" }
  $script:g_p = Start-Process -FilePath $Exe -ArgumentList "`"$file`"" -PassThru
  $dl = (Get-Date).AddSeconds(60)
  while ($script:g_p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
    Start-Sleep -Milliseconds 300; $script:g_p.Refresh() }
  if ($script:g_p.MainWindowHandle -eq 0) { Fail "no main window for $file" }
  [CH]::pid = [uint32]$script:g_p.Id

  $script:g_ed = [IntPtr]::Zero
  $dl = (Get-Date).AddSeconds(30)
  while ($script:g_ed -eq [IntPtr]::Zero -and (Get-Date) -lt $dl) {
    [CH]::LocateFrame()
    foreach ($h in [CH]::Editors()) {
      if ([CH]::TextLength($h) -gt 0) { $script:g_ed = $h; break } }
    if ($script:g_ed -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 300 } }
  if ($script:g_ed -eq [IntPtr]::Zero) { Fail "no Scintilla editor holding text for $file" }

  [CH]::LocateFrame()
  $script:g_sb = [IntPtr]::Zero
  foreach ($h in [CH]::ChildrenByClass("msctls_statusbar32")) { $script:g_sb = $h; break }

  # Force a full colourise and wait for the watermark: Scintilla styles lazily on
  # paint, and probing before it has run reads style 0 everywhere, which looks
  # exactly like "the lexer attached but produced nothing".
  $dl = (Get-Date).AddSeconds(20)
  $styled = 0
  do {
    [CH]::ColouriseAll($script:g_ed)
    Start-Sleep -Milliseconds 250
    $styled = [CH]::EndStyled($script:g_ed)
    $need   = [CH]::TextLength($script:g_ed)
  } while ($styled -lt $need -and (Get-Date) -lt $dl)
  if ($requireStyled -and $styled -le 0) {
    Fail "Scintilla styled nothing (endStyled=$styled) - no lexer running?" }
  if (-not $requireStyled) {
    Write-Output ("  [diag] endStyled={0} (not required for this fixture)" -f $styled) }
}

function Expect-Lexer([string]$lexerName) {
  $want = [int]$script:lexerMap[$lexerName]
  $got  = [CH]::GetLexerId($G_ed)
  if ([CH]::TextLength($g_ed) -le 0) { Fail "editor is empty - probed the wrong window?" }
  if ($got -ne $want) {
    Fail "SCI_GETLEXER = $got, want $want ('$lexerName') - self-built lexer not attached" }
  Write-Output ("  OK lexer id=$got ($lexerName)")
}

function Expect-Style([int]$line, [int]$col, [string]$styleName, [string]$what) {
  if ([CH]::TextLength($g_ed) -le 0) { Fail "$what : editor went away - did xfsWinPad crash?" }
  $want = StyleOf $script:styles $styleName
  $pos  = [CH]::LineEndPos($g_ed, $line)
  # LineEndPos is a value-only message; derive the line start from the previous
  # line's end so we never need a buffer read.
  $ls = 0
  if ($line -gt 0) { $ls = [CH]::LineEndPos($g_ed, $line - 1) + 2 }
  $p = $ls + $col
  $got = [CH]::StyleAt($g_ed, $p)
  if ($got -ne $want) {
    Fail ("$what : line $line col $col (pos $p) style = $got, want $want ($styleName)") }
  Write-Output ("  OK {0,-46} style = {1} ({2})" -f $what, $got, $styleName)
}

function Expect-StatusNote([string]$mode, [int]$part) {
  # part 6 = the model-source note ("not a CRAFT compilation result").
  if ($g_sb -eq [IntPtr]::Zero) { Fail "status bar not found - cannot check the note" }
  $len = [CH]::StatusPartLen($g_sb, $part)
  if ($mode -eq "present" -and $len -le 0) {
    Fail "status-bar note part $part is EMPTY but this is a Chroma file" }
  if ($mode -eq "absent" -and $len -ne 0) {
    Fail "status-bar note part $part is non-empty ($len chars) for a non-Chroma file" }
  Write-Output ("  OK note part {0} {1} (len={2})" -f $part, $mode, $len)
}

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path $sess) { Copy-Item $sess $bak -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
  # ------------------------------------------------------------------ P1: .pat
  Write-Output "[P1] .pat - vector classification"
  $pat = Join-Path $work "p1.pat"
  Write-Fixture $pat @(
    'SET_DEC_FILE "./chip.dec"'
    'HEADER CLR,%SEL0,G1;'
    '*0 1 H L Z*  *R S T U X*  *V K 2*'
    'RPT 2 # tail comment'
  )
  Start-App $pat
  Expect-Lexer "ate_pattern"
  Expect-Style 0 0  "SCE_ATEP_MODULE"    "line0 SET_DEC_FILE module"
  Expect-Style 0 13 "SCE_ATEP_STRING"    "line0 quoted file name"
  Expect-Style 1 0  "SCE_ATEP_MODULE"    "line1 HEADER module"
  Expect-Style 1 7  "SCE_ATEP_PIN"       "line1 CLR pin"
  Expect-Style 1 11 "SCE_ATEP_OPERATOR"  "line1 % pin prefix is an operator"
  Expect-Style 1 12 "SCE_ATEP_PIN"       "line1 SEL0 pin after %"
  # The manual's five vector classes must be five different styles - the whole
  # point of splitting them instead of lumping them into one "vector" style.
  Expect-Style 2 1  "SCE_ATEP_VEC_DRIVE"   "line2 '0' drive"
  Expect-Style 2 3  "SCE_ATEP_VEC_DRIVE"   "line2 '1' drive"
  Expect-Style 2 5  "SCE_ATEP_VEC_CMP"     "line2 'H' compare only"
  Expect-Style 2 9  "SCE_ATEP_VEC_CMP"     "line2 'Z' compare only"
  Expect-Style 2 14 "SCE_ATEP_VEC_DRV_CMP" "line2 'R' drive+compare"
  Expect-Style 2 20 "SCE_ATEP_VEC_DRV_CMP" "line2 'U' drive+compare (was mask in b72)"
  Expect-Style 2 22 "SCE_ATEP_VEC_MASK"    "line2 'X' mask"
  Expect-Style 2 27 "SCE_ATEP_VEC_CTRL"    "line2 'V' control"
  Expect-Style 2 31 "SCE_ATEP_VEC_CTRL"    "line2 '2' control"
  Expect-Style 2 26 "SCE_ATEP_SEP"         "line2 '*' vector delimiter"
  Expect-Style 3 0  "SCE_ATEP_MICRO"     "line3 RPT micro instruction"
  Expect-Style 3 4  "SCE_ATEP_NUMBER"    "line3 counted number"
  Expect-Style 3 8  "SCE_ATEP_COMMENT"   "line3 '#' trailing comment"
  Expect-StatusNote "present" 6
  Write-Output "P1-OK"

  # ------------------------------------------------------------------ P2: .pln
  Write-Output "[P2] .pln - plan program"
  $pln = Join-Path $work "p2.pln"
  Write-Fixture $pln @(
    'TEST_PRO {'
    'FORCE_V_MLDPS(mldps1, f_volt, '
    'FORCE_V_MLDPS(mldps1, f_volt, @6V, '
    'FORCE_V_MLDPS(mldps1, f'
    '}'
    '#include "p2.h"'
  )
  Start-App $pln
  Expect-Lexer "chroma_plan"
  Expect-Style 0 0 "SCE_PLN_BLOCK"  "line0 TEST_PRO block statement"
  Expect-Style 1 0 "SCE_PLN_STMT"   "line1 FORCE_V_MLDPS test statement"
  Expect-Style 0 9 "SCE_PLN_OPERATOR" "line0 '{' operator"
  # 批次 74 hang guard：现场 .pln 头部全是 #include，而本 fixture 最初没有——
  # ChromaPlanLexer 预处理分支 j=i 的死循环因此从开发机 E2E 漏网（真机一打开就挂死）。
  # 这一行 + CheckFullyStyled 语义（Start-App 的 styled 等待必须走完全文）让回归在
  # 这里 20 秒内超时失败，而不是在用户机器上无限挂死。
  Expect-Style 5 0 "SCE_PLN_FLOW"   "line5 #include preprocessor (batch74 hang guard)"
  Expect-Style 5 9 "SCE_PLN_STRING" "line5 include path string"
  Expect-StatusNote "present" 6
  Write-Output "P2-OK"

  # --------------------------------------------------- P3: signature hint (v_range)
  # FORCE_V_MLDPS(MLDPS_name, f_volt, v_range, i_range, I_clamp, m_mode, ON/OFF,
  #               wait_time) - the caret goes at the end of line 1, i.e. inside
  #               argument 3 = v_range, which the manual gives two values: @6V @12V.
  Write-Output "[P3] signature hint - v_range dropdown + Tab"
  $caret   = [CH]::LineEndPos($g_ed, 1)
  $before  = [CH]::TextLength($g_ed)
  [CH]::GotoPos($g_ed, $caret) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'@')
  Start-Sleep -Milliseconds 400
  $after = [CH]::TextLength($g_ed)
  if ($after -ne $before + 1) { Fail "WM_CHAR '@' did not reach the document ($before -> $after)" }

  $tip = [CH]::CallTipActive($g_ed)
  $act = [CH]::AutoCActive($g_ed)
  $ps  = [CH]::AutoCPosStart($g_ed)
  Write-Output ("  [diag] callTip={0} autoC={1} posStart={2} caretWas={3} caretNow={4}" -f `
                $tip, $act, $ps, $caret, [CH]::CurrentPos($g_ed))
  if ($act -ne 1) { Fail "no dropdown for v_range (expected the manual's @6V / @12V)" }
  # Scintilla makes these two mutually exclusive: AutoCompleteStart() starts with
  # ct.CallTipCancel() and CallTipShow() starts with ac.Cancel(). So an enum
  # parameter must show the DROPDOWN and NOT the calltip - asserting calltip==1
  # here is what caught the original design being impossible. See P3B for the
  # calltip half of the rule.
  if ($tip -ne 0) {
    Fail "calltip and dropdown are both up - Scintilla cancels one when the other shows" }
  # SCI_AUTOCPOSSTART is NOT the prefix start: AutoComplete::Start stores
  # posStart = sel.MainCaret() and startLen = lenEntered separately (the prefix
  # range is posStart-startLen .. caret). So after typing one char at $caret it
  # must read $caret+1. Treating it as "prefix start" is a classic misread.
  if ($ps -ne $caret + 1) {
    Fail ("SCI_AUTOCPOSSTART = $ps, want $($caret + 1) (the caret when the list was " +
          "shown) - the hint did not fire at the argument we placed the caret in") }

  # Tab must ACCEPT. This goes through ScintillaBase::KeyCommand's Message::Tab
  # branch, which is Scintilla's code, not ours - and it is why the editor must
  # NOT intercept VK_TAB. (SCI_TAB would take a different path and would quietly
  # not exercise this branch at all, so we send a real key.)
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 300
  $accepted = [CH]::TextLength($g_ed) - $before
  Write-Output ("  [diag] after Tab: docDelta={0} autoC={1}" -f $accepted, [CH]::AutoCActive($g_ed))
  if ([CH]::AutoCActive($g_ed) -ne 0) { Fail "Tab did not close the dropdown" }
  # 1 typed char replaced by the accepted item, so docDelta-1 is the item length.
  # @6V -> 3 chars. @12V (4 chars) would mean the list got sorted alphabetically
  # and the manual's order was lost.
  if ($accepted -ne 3) {
    Fail ("Tab accepted an item of length {0}, want 3 ('@6V' minus the typed '@'); " +
          "4 would mean @12V came first, i.e. the list is not in manual order" -f ($accepted - 1)) }
  Write-Output "  OK calltip + dropdown on v_range; Tab accepted '@6V' (manual order kept)"
  Write-Output "P3-OK"

  # ------------------------------------------- P3B: free parameter -> calltip
  # f_volt (argument 2) has no candidate list, so the calltip is the whole
  # answer there - and that is the majority case (f_volt / pin_name / addresses).
  Write-Output "[P3B] signature hint - free parameter shows the calltip instead"
  $caretB = [CH]::LineEndPos($g_ed, 3)
  [CH]::GotoPos($g_ed, $caretB) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'x')
  Start-Sleep -Milliseconds 400
  $tipB = [CH]::CallTipActive($g_ed)
  $actB = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] callTip={0} autoC={1}" -f $tipB, $actB)
  if ($tipB -ne 1) { Fail "no calltip in a free parameter - the hint did not run at all" }
  if ($actB -ne 0) { Fail "dropdown shown for a parameter with no candidate values" }
  Write-Output "  OK free parameter raises the calltip and no dropdown"
  Write-Output "P3B-OK"

  # ------------------------------------------------- P4: i_range really has 7
  # This is the end-to-end guard for the value-pool bug in doc 5.3: the manual
  # gives i_range seven ranges (@5uA .. @1A) and the old generator truncated the
  # tail and dropped the '@' prefix. We cannot read the list text across
  # processes, so we count it: saturate the selection with Down (Scintilla clamps
  # at the last row) and read the index back. 7 items -> index 6.
  Write-Output "[P4] signature hint - i_range must carry the manual's 7 ranges"
  $caret2  = [CH]::LineEndPos($g_ed, 2)
  [CH]::GotoPos($g_ed, $caret2) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'@')
  Start-Sleep -Milliseconds 400
  if ([CH]::AutoCActive($g_ed) -ne 1) { Fail "no dropdown for i_range (argument 4)" }
  for ($i = 0; $i -lt 20; $i++) { [CH]::PressKey($g_ed, $VK_DOWN) }
  Start-Sleep -Milliseconds 300
  $last = [CH]::AutoCCurrent($g_ed)
  Write-Output ("  [diag] saturated selection index = {0}" -f $last)
  if ($last -ne 6) {
    Fail ("i_range dropdown holds {0} item(s), want 7 - the manual's current ranges " +
          "are @5uA @25uA @250uA @2.5mA @25mA @500mA @1A" -f ($last + 1)) }
  Write-Output "  OK i_range holds exactly 7 items (guards the 5.3 truncation bug)"
  [CH]::PressKey($g_ed, 0x1B)   # ESC: drop the list before moving on
  Start-Sleep -Milliseconds 200
  Write-Output "P4-OK"

  # --------------------------------------------- P5: no hint outside Chroma files
  Write-Output "[P5] plain .txt must not get the hint"
  $txt = Join-Path $work "p5.txt"
  Write-Fixture $txt @(
    'FORCE_V_MLDPS(mldps1, f_volt, '
    'x'
  )
  Start-App $txt $false
  Expect-StatusNote "absent" 6
  $c5 = [CH]::LineEndPos($g_ed, 0)
  [CH]::GotoPos($g_ed, $c5) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'@')
  Start-Sleep -Milliseconds 400
  $tip5 = [CH]::CallTipActive($g_ed)
  $act5 = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] .txt callTip={0} autoC={1}" -f $tip5, $act5)
  if ($tip5 -ne 0) { Fail "calltip appeared in a .txt file - the hint is not scoped to Chroma" }
  if ($act5 -ne 0) { Fail "dropdown appeared in a .txt file - the hint is not scoped to Chroma" }
  Write-Output "  OK no calltip and no dropdown in a non-Chroma file"
  Write-Output "P5-OK"

  # ------------------------------------ P7: statement-name completion (.pln)
  # Batch 77. The completion list is built from the lexer's OWN word tables (the
  # same names it highlights) and ordered by MANUAL CHAPTER, not alphabetically -
  # so accepting the first item at a statement position must drop the manual's
  # earliest matching statement into the document. That is the whole feature, and
  # it is measurable: the document text after Tab says which item won.
  Write-Output "[P7] statement completion - manual order, statement position only"
  $p7 = Join-Path $work "p7.pln"
  Write-Fixture $p7 @(
    'TEST_PRO {'
    ''
  )
  Start-App $p7
  Expect-Lexer "chroma_plan"
  $want = Get-FirstStatementFor "chroma_plan" "FORC"
  if ($want -ne "FORCE_I_MLDPS") {
    Fail (("the DB's first FORC* match is {0}, want FORCE_I_MLDPS (manual 4.3) - either " +
           "the generator reordered the table or the word tables changed") -f $want) }
  $nameLen = $want.Length

  $c7 = [CH]::LineEndPos($g_ed, 1)
  [CH]::GotoPos($g_ed, $c7) | Out-Null
  # (1) one character must stay silent: the prefix threshold is 2 (1 char matches
  #     too much of the 282-name table to be useful). This is the boundary the
  #     statement path deliberately runs at - the word-completion path wants 3.
  [CH]::TypeChar($g_ed, [int][char]'F')
  Start-Sleep -Milliseconds 400
  $act1 = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] prefix 'F' -> autoC={0}" -f $act1)
  if ($act1 -ne 0) { Fail "a one-character prefix raised a dropdown (threshold is 2)" }
  # (2) four characters at a statement position -> dropdown, and NOT the calltip
  [CH]::TypeChar($g_ed, [int][char]'O')
  [CH]::TypeChar($g_ed, [int][char]'R')
  [CH]::TypeChar($g_ed, [int][char]'C')
  Start-Sleep -Milliseconds 400
  $len7   = [CH]::TextLength($g_ed)
  $act7   = [CH]::AutoCActive($g_ed)
  $tip7   = [CH]::CallTipActive($g_ed)
  $ps7    = [CH]::AutoCPosStart($g_ed)
  Write-Output ("  [diag] prefix 'FORC' -> autoC={0} callTip={1} posStart={2} caret={3}" -f `
                $act7, $tip7, $ps7, [CH]::CurrentPos($g_ed))
  if ($act7 -ne 1) { Fail "no dropdown at a statement position for prefix FORC" }
  if ($tip7 -ne 0) { Fail "calltip and dropdown both up (Scintilla cancels one when the other shows)" }
  # SCI_AUTOCPOSSTART is the caret at the moment the list was SHOWN, and Scintilla
  # does NOT move it as the user keeps typing (later chars only re-filter the list).
  # The list is shown when the prefix reaches the 2-character threshold - i.e. on
  # the second character 'O' - so posStart must be $c7 + 2. This is the sharpest
  # available proof of WHERE the threshold sits: 1 char showed nothing (checked
  # above), 2 chars showed the list right there, 4 chars kept filtering it.
  if ($ps7 -ne ($c7 + 2)) {
    Fail (("SCI_AUTOCPOSSTART = {0}, want {1} (the caret when the list was shown, i.e. " +
           "after 2 prefix chars) - the list did not appear at the 2-char threshold") -f
          $ps7, ($c7 + 2)) }
  # (3) Tab accepts item 0. See the corrected notes below on what the delta can and
  #     cannot prove (batch 78: FORCE_CURRENT and FORCE_I_MLDPS are both 13 chars,
  #     so the ORDER proof lives in the text assertion).
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 400
  $delta = [CH]::TextLength($g_ed) - $len7
  # (4) the text itself (WM_GETTEXT is below WM_USER, so it is marshalled safely).
  #     Read BEFORE the delta assertion, because since batch 78 the delta also
  #     carries the continuation the acceptance triggers - see below.
  $text7 = [CH]::GetText($g_ed)
  Write-Output ("  [diag] after Tab: delta={0} autoC={1}" -f $delta, [CH]::AutoCActive($g_ed))
  if ([CH]::AutoCActive($g_ed) -ne 0) { Fail "Tab did not close the statement dropdown" }
  # (3) Delta pins the SIZE of what was accepted: nameLen - prefixLen, PLUS the
  #     batch-78 continuation (a '(' and, when the auto-close-brackets preference
  #     is on - the default - its paired ')'). Deriving the bracket count from the
  #     text instead of hardcoding it keeps this valid whether or not the local
  #     settings.json pairs brackets.
  #     NOTE, corrected in batch 78: this delta alone does NOT prove the manual
  #     order. Batch 77's comment claimed an alphabetically sorted list would have
  #     accepted FORCE_CURRENT "and the length would not match" - but FORCE_CURRENT
  #     (manual 4.8) and FORCE_I_MLDPS (manual 4.3) are BOTH 13 characters, so the
  #     sizes are identical. The ORDER proof is the text assertion in (4) below;
  #     this line only catches a wrong-length name (e.g. FORCE_I_DPS, 11).
  $extra7 = 1
  if ($text7.IndexOf($want + "()") -ge 0) { $extra7 = 2 }
  if ($delta -ne ($nameLen - 4 + $extra7)) {
    Fail (("Tab changed the document by {0} chars, want {1} ({2}-char name + {3} bracket " +
           "char(s)) - i.e. the accepted name is not '{4}'") -f
          $delta, ($nameLen - 4 + $extra7), $nameLen, $extra7, $want) }
  if ($text7.IndexOf($want) -lt 0) {
    Fail ("the accepted statement is not in the document. text=[" +
          ($text7 -replace "`r", '\r' -replace "`n", '\n') + "]") }
  Write-Output ("  OK 'FORC' + Tab inserted {0} (manual order, {1} chars)" -f $want, $nameLen)
  Write-Output "P7-OK"

  # ------------------------- P7B: the same prefix must stay silent in a .txt
  # The statement path is gated on the Chroma lexer family, exactly like the
  # signature hint - otherwise every plain text file would start offering TPG
  # statements. The fixture has no other FORC* word in it, so nothing (neither
  # our path nor the generic word completion) can legitimately fire here.
  Write-Output "[P7B] statement completion must not fire outside Chroma"
  $p7b = Join-Path $work "p7b.txt"
  Write-Fixture $p7b @('FORC')
  Start-App $p7b $false
  [CH]::GotoPos($g_ed, [CH]::LineEndPos($g_ed, 0)) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'E')
  Start-Sleep -Milliseconds 400
  $act7b = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] .txt 'FORCE' -> autoC={0}" -f $act7b)
  if ($act7b -ne 0) { Fail "the statement dropdown appeared in a .txt file" }
  Write-Output "  OK no statement completion in a non-Chroma file"
  Write-Output "P7B-OK"

  # ------------------- P8: accept a statement -> auto "(" + signature hint
  # Batch 78. The half that P7 could not reach: SCN_AUTOCSELECTION(2022) is sent
  # from inside NotifyParent BEFORE AutoCompleteInsert runs, so text inserted in
  # that callback is swallowed by the replacement that follows. SCN_AUTOCCOMPLETED
  # (2030) is sent AFTER the insertion, so the continuation hook is that one.
  # Everything asserted below is visible from outside: the paren shows up in the
  # document text, and the hint shows up as an active calltip.
  Write-Output "[P8] accepting a statement appends '(' and raises the signature hint"
  $p8 = Join-Path $work "p8.pln"
  Write-Fixture $p8 @(
    'TEST_PRO {'
    ''
  )
  Start-App $p8
  Expect-Lexer "chroma_plan"
  $want8 = Get-FirstStatementFor "chroma_plan" "FORC"
  if ($want8 -ne "FORCE_I_MLDPS") {
    Fail (("P8 wants FORCE_I_MLDPS from prefix FORC, got {0}") -f $want8) }

  $c8 = [CH]::LineEndPos($g_ed, 1)
  [CH]::GotoPos($g_ed, $c8) | Out-Null
  foreach ($ch8 in @('F','O','R','C')) { [CH]::TypeChar($g_ed, [int][char]$ch8) }
  Start-Sleep -Milliseconds 400
  if ([CH]::AutoCActive($g_ed) -ne 1) { Fail "no statement dropdown to accept" }
  $len8 = [CH]::TextLength($g_ed)

  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 500
  $delta8 = [CH]::TextLength($g_ed) - $len8
  $text8  = [CH]::GetText($g_ed)
  $tip8   = [CH]::CallTipActive($g_ed)
  Write-Output ("  [diag] after Tab: delta={0} callTip={1} autoC={2}" -f `
                $delta8, $tip8, [CH]::AutoCActive($g_ed))

  # (1) the left paren IS the feature. Read back with WM_GETTEXT.
  if ($text8.IndexOf($want8 + "(") -lt 0) {
    Fail ("accepted statement has no trailing '(' in the document. text=[" +
          ($text8 -replace "`r", '\r' -replace "`n", '\n') + "]") }
  # (2) delta pins the whole edit: 4-char prefix -> name, + '(' and, when the
  #     auto-close-brackets preference is on (the default), the paired ')'.
  #     Deriving it from the text instead of hardcoding keeps this assertion
  #     honest whether or not the local settings.json has the pairing off.
  $extra8 = 1
  if ($text8.IndexOf($want8 + "()") -ge 0) { $extra8 = 2 }
  if ($delta8 -ne ($want8.Length - 4 + $extra8)) {
    Fail (("Tab changed the document by {0} chars, want {1} ({2}-char name + {3} bracket " +
           "char(s)). The continuation hook did not run") -f
          $delta8, ($want8.Length - 4 + $extra8), $want8.Length, $extra8) }
  # (3) the caret now sits right after '(' - i.e. inside the argument list - so
  #     batch 73's hint must be up. FORCE_I_MLDPS' first parameter (MLDPS_name)
  #     has no candidate values, so the hint is the CALLTIP, not a dropdown.
  #     This is what makes the feature more than one saved keystroke: the user
  #     lands directly in the guided-argument flow, same as typing '(' by hand.
  if ($tip8 -ne 1) { Fail "no signature calltip after the auto-inserted '('" }
  if ([CH]::AutoCActive($g_ed) -ne 0) {
    Fail "a dropdown is up together with the calltip (Scintilla cancels one for the other)" }
  Write-Output ("  OK 'FORC' + Tab -> {0}( + signature hint ({1} bracket char(s) added)" -f `
                $want8, $extra8)
  Write-Output "P8-OK"

  # ---------------- P8B: a block statement must NOT be given a paren
  # The manual prints TEST_PRO as `TEST_PRO {` - a block header followed by an
  # indented body, with no parameter list at all. Appending '(' there would write
  # syntactically wrong code, which is the one thing this family must never do
  # (zero false positives). The gate is the manual's own signature shape, so this
  # case is the negative control for P8.
  Write-Output "[P8B] a block-header statement gets no paren"
  $p8b = Join-Path $work "p8b.pln"
  Write-Fixture $p8b @(
    'TEST_PRO {'
    '    '
  )
  Start-App $p8b
  Expect-Lexer "chroma_plan"
  $want8b = Get-FirstStatementFor "chroma_plan" "TEST_PR"
  if ($want8b -ne "TEST_PRO") {
    Fail (("P8B wants TEST_PRO from prefix TEST_PR, got {0}") -f $want8b) }
  $c8b = [CH]::LineEndPos($g_ed, 1)
  [CH]::GotoPos($g_ed, $c8b) | Out-Null
  foreach ($ch8b in @('T','E','S','T','_','P','R')) { [CH]::TypeChar($g_ed, [int][char]$ch8b) }
  Start-Sleep -Milliseconds 400
  if ([CH]::AutoCActive($g_ed) -ne 1) { Fail "no statement dropdown for prefix TEST_PR" }
  $len8b = [CH]::TextLength($g_ed)
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 500
  $delta8b = [CH]::TextLength($g_ed) - $len8b
  $text8b  = [CH]::GetText($g_ed)
  Write-Output ("  [diag] after Tab: delta={0} callTip={1}" -f $delta8b, [CH]::CallTipActive($g_ed))
  if ($text8b.IndexOf($want8b) -lt 0) {
    Fail ("the accepted block header is not in the document. text=[" +
          ($text8b -replace "`r", '\r' -replace "`n", '\n') + "]") }
  if ($text8b.IndexOf($want8b + "(") -ge 0) {
    Fail ("a '(' was appended after the block header {0} - the manual writes it as 'NAME {{'" -f $want8b) }
  if ($delta8b -ne ($want8b.Length - 7)) {
    Fail (("Tab changed the document by {0} chars, want {1} - only the name itself should " +
           "have been inserted") -f $delta8b, ($want8b.Length - 7)) }
  Write-Output ("  OK '{0}' + Tab inserted the name alone (no paren)" -f $want8b)
  Write-Output "P8B-OK"

  # ------------- P9: batch 79's data fix, end to end (RELAY_ON / RELAY_OFF)
  # Batch 78 measured coverage and found RELAY_ON/RELAY_OFF had no signature at
  # all: RELAY_ON is the 2nd most frequent statement in a real .pln (31 uses).
  # Batch 79 fixed the extractor (page window + heading lookahead); the whole
  # point of that work is this probe - the statement completes, gets its '(' and
  # raises the manual's signature. Asserting the NAME by text is what proves the
  # manual ORDER too: an alphabetically sorted list has RELAY_OFF before RELAY_ON
  # and both are 9 chars, so the delta alone cannot tell them apart.
  Write-Output "[P9] RELAY_ON (batch 79 data fix) completes, takes a paren, raises the hint"
  $p9 = Join-Path $work "p9.pln"
  Write-Fixture $p9 @(
    'TEST_PRO {'
    ''
  )
  Start-App $p9
  Expect-Lexer "chroma_plan"
  $want9 = Get-FirstStatementFor "chroma_plan" "RELAY"
  if ($want9 -ne "RELAY_ON") {
    Fail (("P9 wants RELAY_ON from prefix RELAY, got {0}") -f $want9) }
  $c9 = [CH]::LineEndPos($g_ed, 1)
  [CH]::GotoPos($g_ed, $c9) | Out-Null
  foreach ($ch9 in @('R','E','L','A','Y')) { [CH]::TypeChar($g_ed, [int][char]$ch9) }
  Start-Sleep -Milliseconds 400
  if ([CH]::AutoCActive($g_ed) -ne 1) { Fail "no statement dropdown for prefix RELAY" }
  $len9 = [CH]::TextLength($g_ed)
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 500
  $delta9 = [CH]::TextLength($g_ed) - $len9
  $text9  = [CH]::GetText($g_ed)
  $tip9   = [CH]::CallTipActive($g_ed)
  Write-Output ("  [diag] after Tab: delta={0} callTip={1}" -f $delta9, $tip9)
  if ($text9.IndexOf($want9) -lt 0) {
    Fail ("the accepted statement is not in the document. text=[" +
          ($text9 -replace "`r", '\r' -replace "`n", '\n') + "]") }
  if ($text9.IndexOf($want9 + "(") -lt 0) {
    Fail ("RELAY_ON got no '(' - the batch-79 signature is missing from the DB again") }
  $extra9 = 1
  if ($text9.IndexOf($want9 + "()") -ge 0) { $extra9 = 2 }
  if ($delta9 -ne ($want9.Length - 5 + $extra9)) {
    Fail (("Tab changed the document by {0} chars, want {1} ({2}-char name + {3} bracket " +
           "char(s))") -f $delta9, ($want9.Length - 5 + $extra9), $want9.Length, $extra9) }
  if ($tip9 -ne 1) { Fail "no signature calltip after RELAY_ON(" }
  Write-Output ("  OK 'RELAY' + Tab -> {0}( + signature hint" -f $want9)
  Write-Output "P9-OK"

  # ------------ P9B: the other half - a name whose old signature came from the
  # manual's EXAMPLE section must NOT get a paren. RF_Initialize used to carry
  # `RF_Initialize(KEYSIGHT,_IP,"192.168.1.2",_IniFilePath,"CableLoss.ini") ;`,
  # lifted from the example block; the real Format prints a differently named
  # function (CRAFT_RF_Initialize), so the honest answer is "no signature".
  # Zero false positives means showing nothing beats showing the example's
  # literal values as if they were the parameter list.
  Write-Output "[P9B] RF_Initialize must get no paren (its old signature was example text)"
  $p9b = Join-Path $work "p9b.pln"
  Write-Fixture $p9b @(
    'TEST_PRO {'
    ''
  )
  Start-App $p9b
  Expect-Lexer "chroma_plan"
  $want9b = Get-FirstStatementFor "chroma_plan" "RF_I"
  if ($want9b -ne "RF_Initialize") {
    Fail (("P9B wants RF_Initialize from prefix RF_I, got {0}") -f $want9b) }
  $c9b = [CH]::LineEndPos($g_ed, 1)
  [CH]::GotoPos($g_ed, $c9b) | Out-Null
  foreach ($ch9b in @('R','F','_','I')) { [CH]::TypeChar($g_ed, [int][char]$ch9b) }
  Start-Sleep -Milliseconds 400
  if ([CH]::AutoCActive($g_ed) -ne 1) { Fail "no statement dropdown for prefix RF_I" }
  $len9b = [CH]::TextLength($g_ed)
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 500
  $delta9b = [CH]::TextLength($g_ed) - $len9b
  $text9b  = [CH]::GetText($g_ed)
  $tip9b   = [CH]::CallTipActive($g_ed)
  Write-Output ("  [diag] after Tab: delta={0} callTip={1}" -f $delta9b, $tip9b)
  if ($text9b.IndexOf($want9b) -lt 0) {
    Fail ("the accepted statement is not in the document. text=[" +
          ($text9b -replace "`r", '\r' -replace "`n", '\n') + "]") }
  if ($text9b.IndexOf($want9b + "(") -ge 0) {
    Fail ("a '(' was appended after {0} - its signature is example text and must be gone" -f $want9b) }
  if ($delta9b -ne ($want9b.Length - 4)) {
    Fail (("Tab changed the document by {0} chars, want {1} - only the name itself should " +
           "have been inserted") -f $delta9b, ($want9b.Length - 4)) }
  if ($tip9b -ne 0) {
    Fail "a signature calltip was raised for a statement whose signature must be empty" }
  Write-Output ("  OK '{0}' + Tab inserted the name alone (no paren)" -f $want9b)
  Write-Output "P9B-OK"

  # ------------ P10: STATIC DIAGNOSTICS (batch 87) ------------------------------
  # Rule 8 (argument count, .pln) wired to the real UI: the status bar gains an
  # 8th part and the editor draws squiggles with indicator 10 (error) / 11
  # (warning). The minimal PLN-010 case below is lifted verbatim from
  # tests/test_chromadiag.cpp, so unit and e2e guard the same input.
  # The squiggle covers ONLY the statement name (12 chars at line offset 0),
  # which is exactly what the kernel reports.
  Write-Output "[P10] rule-8 violation draws the error squiggle + fills status part 7"
  $p10 = Join-Path $work "p10.pln"
  Write-Fixture $p10 @(
    'TEST_PRO {'
    'MEAS_I_MLDPS(PREF, 1mS, 10, AVE, 50uS, 3);'
    '}'
  )
  Start-App $p10
  Expect-Lexer "chroma_plan"
  # refresh runs synchronously on document open, but poll briefly so a slow
  # first-paint can never flake the probe
  $ls10 = [CH]::LineEndPos($g_ed, 0) + 2   # line 1 start (CRLF)
  $bad10 = 0
  $dl10 = (Get-Date).AddSeconds(5)
  while ((Get-Date) -lt $dl10) {
    if ([CH]::IndicatorAt($g_ed, $ls10, 10) -eq 1) { $bad10 = 1; break }
    Start-Sleep -Milliseconds 200
  }
  if ($bad10 -ne 1) {
    Fail ("indicator 10 is NOT set at the violating statement name (pos {0}) - " +
          "rule 8 is not wired to the editor" -f $ls10) }
  if ([CH]::IndicatorAt($g_ed, $ls10 + 5, 10) -ne 1) {
    Fail "indicator 10 must cover the whole 12-char statement name (probe at +5)" }
  if ([CH]::IndicatorAt($g_ed, $ls10 + 20, 10) -ne 0) {
    Fail "indicator 10 must NOT extend past the statement name (probe at +20)" }
  if ([CH]::IndicatorAt($g_ed, 0, 10) -ne 0) {
    Fail "indicator 10 on the clean TEST_PRO line - over-marking" }
  if ([CH]::IndicatorAt($g_ed, $ls10, 11) -ne 0) {
    Fail "indicator 11 (warning) set for an Error-severity finding" }
  if ($g_sb -eq [IntPtr]::Zero) { Fail "status bar not found" }
  $len7 = [CH]::StatusPartLen($g_sb, 7)
  if ($len7 -le 0) { Fail "status part 7 (diagnostics) is EMPTY with 1 finding" }
  Write-Output ("  OK squiggle on line 1 name + status part 7 len={0}" -f $len7)
  Write-Output "P10-OK"

  # P10B negative control: a syntactically clean call must leave indicator 10
  # and 11 silent everywhere while the status part still shows "no findings".
  Write-Output "[P10B] clean .pln: no squiggle anywhere, status part 7 says clean"
  $p10b = Join-Path $work "p10b.pln"
  Write-Fixture $p10b @(
    'TEST_PRO {'
    'MEAS_I_MLDPS(PREF, 1mS, 10, AVE, 50uS);'
    '}'
  )
  Start-App $p10b
  Expect-Lexer "chroma_plan"
  $ls10b = [CH]::LineEndPos($g_ed, 0) + 2
  Start-Sleep -Milliseconds 600   # allow the open-time refresh to settle
  $hits10b = 0
  for ($i = 0; $i -lt 40; $i++) {
    if ([CH]::IndicatorAt($g_ed, $ls10b + $i, 10) -ne 0) { $hits10b++ }
    if ([CH]::IndicatorAt($g_ed, $ls10b + $i, 11) -ne 0) { $hits10b++ }
  }
  if ($hits10b -ne 0) {
    Fail ("clean line 1 has {0} indicator hits - false positives in the UI" -f $hits10b) }
  $len7b = [CH]::StatusPartLen($g_sb, 7)
  if ($len7b -le 0) { Fail "status part 7 empty for a clean Chroma file (clean text missing)" }
  Write-Output ("  OK 0 squiggle hits on the clean call + status part 7 len={0}" -f $len7b)
  Write-Output "P10B-OK"

  # ------------ P11: CROSS-FILE RULE 3 (batch 88) -------------------------------
  # DEC_MODE APAS (in the referenced .dec) + IMATCH usage (in this .pat) draws
  # the WARNING squiggle (indicator 11). The dec is resolved on disk relative to
  # the pattern file's directory - this exercises the host's file resolution,
  # which unit tests cannot cover. Vector lines are lifted from the manual's
  # IMATCH worked example (LM p62), same as the unit fixtures.
  Write-Output "[P11] referenced .dec says DEC_MODE APAS -> IMATCH draws warning squiggle"
  $p11dec = Join-Path $work "p11.dec"
  Write-Fixture $p11dec @(
    'DEC_MODE  APAS;'
    ''
    'PIN_LIST (B) {'
    'A = 0 = 1 = IO;'
    'B = 1 = 2 = IO;'
    '}'
  )
  $p11 = Join-Path $work "p11.pat"
  Write-Fixture $p11 @(
    'SET_DEC_FILE "./p11.dec"'
    'HEADER CLR,%SEL0,G1;'
    '*1 01 00 1 X1 XXXXXXXX XH*TS2,IMATCH;'
    '*1 01 00 1 X0 XXXXXXXX XL*TS2,STOP;'
  )
  Start-App $p11
  Expect-Lexer "ate_pattern"
  $ls11 = [CH]::LineEndPos($g_ed, 1) + 2          # start of vector line (0-based line 2)
  $im11 = $ls11 + 30                              # IMATCH at col 30 of the manual vector
  Start-Sleep -Milliseconds 600                   # allow the open-time refresh to settle
  if ([CH]::IndicatorAt($g_ed, $im11, 11) -ne 1) {
    Fail "indicator 11 NOT set at IMATCH (pos $im11) - cross-file rule 3 not wired" }
  if ([CH]::IndicatorAt($g_ed, $im11 + 3, 11) -ne 1) {
    Fail "indicator 11 must cover the whole IMATCH token (probe at +3)" }
  if ([CH]::IndicatorAt($g_ed, $im11, 10) -ne 0) {
    Fail "indicator 10 (Error) set for a Warning-severity finding" }
  if ([CH]::IndicatorAt($g_ed, $ls11, 11) -ne 0) {
    Fail "indicator 11 before the IMATCH token - over-marking" }
  $ls11b = [CH]::LineEndPos($g_ed, 2) + 2         # control vector line (STOP, no IMATCH)
  if ([CH]::IndicatorAt($g_ed, $ls11b + 30, 11) -ne 0) {
    Fail "indicator 11 on the clean STOP vector line - false positive" }
  if ([CH]::IndicatorAt($g_ed, 0, 11) -ne 0) {
    Fail "indicator 11 on the SET_DEC_FILE line - over-marking" }
  $len7c = [CH]::StatusPartLen($g_sb, 7)
  if ($len7c -le 0) { Fail "status part 7 empty with 1 cross-file finding" }
  Write-Output ("  OK warning squiggle on IMATCH + status part 7 len={0}" -f $len7c)
  Write-Output "P11-OK"

  # P11B negative control: same .pat but the referenced .dec declares nothing ->
  # the cross-file gate stays silent everywhere (dec found on disk is mandatory).
  Write-Output "[P11B] referenced .dec without DEC_MODE: IMATCH stays clean"
  $p11bdec = Join-Path $work "p11b.dec"
  Write-Fixture $p11bdec @(
    'PIN_LIST (B) {'
    'A = 0 = 1 = IO;'
    'B = 1 = 2 = IO;'
    '}'
  )
  $p11b = Join-Path $work "p11b.pat"
  Write-Fixture $p11b @(
    'SET_DEC_FILE "./p11b.dec"'
    'HEADER CLR,%SEL0,G1;'
    '*1 01 00 1 X1 XXXXXXXX XH*TS2,IMATCH;'
  )
  Start-App $p11b
  Expect-Lexer "ate_pattern"
  $ls11c = [CH]::LineEndPos($g_ed, 1) + 2
  Start-Sleep -Milliseconds 600
  $hits11 = 0
  for ($i = 0; $i -lt 40; $i++) {
    if ([CH]::IndicatorAt($g_ed, $ls11c + $i, 10) -ne 0) { $hits11++ }
    if ([CH]::IndicatorAt($g_ed, $ls11c + $i, 11) -ne 0) { $hits11++ }
  }
  if ($hits11 -ne 0) {
    Fail ("dec without DEC_MODE: {0} indicator hits on the IMATCH line - false positives" -f $hits11) }
  Write-Output "P11B-OK"

  # ------------ P12: CROSS-FILE DEC-SYMBOL COMPLETION (batch 89) -----------------
  # The referenced .dec's symbols (pins / groups / time names) join the word-
  # completion word list. The dec is resolved on disk relative to the plan's
  # directory (same host resolution as rule 3). The symbol Vdps comes from the
  # manual's own worked example (LM 2.7.2: dec defines Vdps, the plan calls
  # FORCE_V_DPS(Vdps,...)) - the manual documents exactly this workflow.
  # Uniqueness: no statement name starts with "VDP", and the plan text contains
  # no VDP* word of its own, so any dropdown for prefix "Vdp" can only come from
  # the dec - and Tab-accepting it must leave "Vdps" in the document text.
  Write-Output "[P12] referenced .dec supplies the Vdps symbol for word completion"
  $p12dec = Join-Path $work "p12.dec"
  Write-Fixture $p12dec @(
    'PIN_LIST (B) {'
    'Vdps = 0 : 4 = 1 = DPS;'
    '}'
  )
  $p12 = Join-Path $work "p12.pln"
  Write-Fixture $p12 @(
    'TEST_PRO {'
    'SET_DEC_FILE "./p12.dec"'
    'FORCE_V_DPS('
    '}'
  )
  Start-App $p12
  Expect-Lexer "chroma_plan"
  $c12 = [CH]::LineEndPos($g_ed, 2)              # end of the FORCE_V_DPS( line
  [CH]::GotoPos($g_ed, $c12) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'V')
  [CH]::TypeChar($g_ed, [int][char]'d')
  [CH]::TypeChar($g_ed, [int][char]'p')
  Start-Sleep -Milliseconds 500
  $act12 = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] prefix 'Vdp' -> autoC={0}" -f $act12)
  if ($act12 -ne 1) {
    Fail "no dropdown for prefix Vdp - the dec's symbols are not reaching word completion" }
  [CH]::PressKey($g_ed, $VK_TAB)
  Start-Sleep -Milliseconds 500
  if ([CH]::AutoCActive($g_ed) -ne 0) { Fail "Tab did not close the dropdown" }
  $text12 = [CH]::GetText($g_ed)
  if (-not $text12.Contains("FORCE_V_DPS(Vdps")) {
    Fail "Tab acceptance did not insert the dec symbol Vdps (word list source wrong)" }
  Write-Output "  OK dropdown for Vdp came from the dec; Tab inserted Vdps"
  Write-Output "P12-OK"

  # P12B negative control: a .dec whose symbols cannot match the prefix keeps
  # the dropdown silent - proves the P12 hit came from the dec, not from noise.
  Write-Output "[P12B] .dec without a matching symbol: prefix Vdp stays silent"
  $p12bdec = Join-Path $work "p12b.dec"
  Write-Fixture $p12bdec @(
    'PIN_LIST (B) {'
    'QSYM = 0 : 4 = 1 = IO;'
    '}'
  )
  $p12b = Join-Path $work "p12b.pln"
  Write-Fixture $p12b @(
    'TEST_PRO {'
    'SET_DEC_FILE "./p12b.dec"'
    'FORCE_V_DPS('
    '}'
  )
  Start-App $p12b
  Expect-Lexer "chroma_plan"
  $c12b = [CH]::LineEndPos($g_ed, 2)
  [CH]::GotoPos($g_ed, $c12b) | Out-Null
  [CH]::TypeChar($g_ed, [int][char]'V')
  [CH]::TypeChar($g_ed, [int][char]'d')
  [CH]::TypeChar($g_ed, [int][char]'p')
  Start-Sleep -Milliseconds 500
  $act12b = [CH]::AutoCActive($g_ed)
  Write-Output ("  [diag] prefix 'Vdp' (no match in dec) -> autoC={0}" -f $act12b)
  if ($act12b -ne 0) {
    Fail "dropdown raised for Vdp although the dec has no VDP* symbol - false source" }
  Write-Output "P12B-OK"

  Write-Output "CHROMA-E2E-PASS"
  Cleanup
  exit 0
} catch {
  Fail $_.Exception.Message
}
