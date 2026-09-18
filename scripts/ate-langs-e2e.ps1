param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# ate-langs-e2e.ps1 - the self-built ATE ILexer5 lexers must really attach to the
# live editor and produce the designed styles.
#
# Why this exists: tests/test_atelexer.cpp drives Lex()/Fold() directly through a
# fake IDocument, so it can never prove the Editor wiring works - SCI_SETILEXER
# accepting our ILexer5, the theme-role tables actually being applied, styles
# surviving Scintilla's lazy styling. That is exactly the gap where a broken
# release could slip out, so it is checked here against the real process.
#
#   P1 .pat  -> the five manual vector classes must be five DISTINCT colours, and
#               mask must not look like body text. (The full per-byte .pat style
#               matrix now lives in chroma-e2e.ps1's P1 - batch 73 renamed most of
#               that group, so a second full copy here would just rot again.)
#   P2 .stil -> stil lexer attaches, per-byte styles match SCE_STIL_*
#   P3 .log  -> ate_log lexer attaches, per-byte styles match SCE_ATEL_* and the
#               conservative gate keeps a generic app log line uncolored
#
# 【为什么用 SCI_GETLEXER(4002) 而不是 SCI_GETLEXERLANGUAGE(4012) 判定词法器】
#   本脚本跨进程 SendMessageW 到 xfsWinPad.exe。Win32 只为 0..WM_USER(1024)
#   的消息封送参数，SCI_* 全在 1024 以上，指针原样传递——4012 让目标进程往
#   「调用方的地址」里写语言名，等于让 xfsWinPad 拿本进程的地址往自己的地址
#   空间里 memcpy。实测后果：那一次探针之后，连 SCI_GETTEXTLENGTH 都返回 0，
#   因为目标进程已经被写崩了。所以 4012 这类「缓冲区出参」的消息在本脚本里
#   **一律不许出现**；下面用到的消息全部通过返回值传值，跨进程安全。
#   4002 返回的整数正是 XfsCreateLexer() 里 名字->词法器 的一一映射，等价证明了
#   「自研词法器真的挂上去了」。名字本身由 test_atelexer 的工厂用例在进程内断言
#   （TestFactory），唯一性也在那里钉死。
#
# 【数字全部从源码头文件解析，不硬编码】
#   批次 73 重排了 .pat 组的样式号并改掉了大半样式名，本脚本的批次 72 版本因此
#   静默失效（硬编码 66/80/88… 已经全是错的了）。现在样式号取自
#   src/language/XfsLexerStyles.h、词法器号取自 src/language/XfsLexer.h 的
#   kOwnLexers 表，名字对不上就直接抛错（见 scripts/_lexer-ids.ps1）。
#
# The real %APPDATA%\xfsWinPad\session.json is backed up and restored.
# ASCII only.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "_lexer-ids.ps1")
$styles = Get-StyleMap (Join-Path $repoRoot "src\language\XfsLexerStyles.h")
$lexers = Get-LexerMap  (Join-Path $repoRoot "src\language\XfsLexer.h")
Write-Output ("[setup] parsed {0} style ids, {1} own lexers" -f $styles.Count, $lexers.Count)

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
  // (see the visible-filter note). The caller picks by text length.
  public static IntPtr[] Editors(){
    var list = new System.Collections.Generic.List<IntPtr>();
    if (frame == IntPtr.Zero) return list.ToArray();
    EnumChildWindows(frame, (h,l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "Scintilla") { list.Add(h); } return true; }, IntPtr.Zero);
    return list.ToArray(); }
  public static int StyleAt(IntPtr ed, int pos){ return (int)SendMessageW(ed, 2010, (IntPtr)pos, IntPtr.Zero); }
  public static int LineEndPos(IntPtr ed, int line){ return (int)SendMessageW(ed, 2136, (IntPtr)line, IntPtr.Zero); }
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

function Expect-Lexer([string]$lexerName) {
  if (-not $lexers.ContainsKey($lexerName)) {
    Fail "unknown own lexer '$lexerName' - check kOwnLexers in XfsLexer.h" }
  $want = [int]$lexers[$lexerName]
  $len = [ATE]::TextLength($g_ed)
  $id  = [ATE]::GetLexerId($g_ed)
  Write-Output "  [diag] textLength=$len lexerId=$id"
  if ($len -le 0) { Fail "editor is empty (len=$len) - probed the wrong window?" }
  if ($id -ne $want) {
    Fail "lexer id = $id, want $want ('$lexerName') - self-built lexer not attached" }
  Write-Output "  OK lexer id=$id ($lexerName)"
}

function Expect-Style([int]$line, [int]$col, [string]$styleName, [string]$what) {
  # A vanished document means the probe is no longer measuring the lexer; say so
  # instead of reporting a bogus style mismatch.
  if ([ATE]::TextLength($g_ed) -le 0) {
    Fail "$what : editor went away (textLength=0) - did xfsWinPad crash?" }
  $want = StyleOf $styles $styleName
  # Line start from the previous line's end: SCI_GETLINEENDPOSITION is a
  # value-only message, so this stays cross-process safe (no buffer reads).
  $ls = 0
  if ($line -gt 0) { $ls = [ATE]::LineEndPos($g_ed, $line - 1) + 2 }   # CRLF = 2
  $pos = $ls + $col
  $got = [ATE]::StyleAt($g_ed, $pos)
  if ($got -ne $want) {
    Dump-Styles
    Fail "$what : line $line col $col (pos $pos) style = $got, want $want ($styleName)" }
  Write-Output ("  OK {0,-46} style = {1} ({2})" -f $what, $got, $styleName)
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
  # ------------------------------------------- P1: .pat colour separability
  # The per-byte style matrix for .pat is asserted in chroma-e2e.ps1 P1. What is
  # checked here is the thing batch 72 shipped broken and no style-id assertion
  # could catch: the FIVE vector classes must be five visually distinct colours,
  # and the mask colour must not equal the body-text colour. Batch 72 had mask
  # mapped to ROperator, which equals editorFg in both themes, so X/N/Z/U drew
  # exactly like an ordinary identifier - and every assertion of the form "the
  # three differ from each other" still passed.
  Write-Output "[P1] .pat - vector classes must be five distinct colours"
  $pat = Join-Path $work "t.pat"
  (@(
    '*0 1 H L Z*  *R S T U X*  *V K 2*'
  ) -join "`r`n") + "`r`n" | Set-Content -Path $pat -Encoding ASCII

  Start-App $pat
  Expect-Lexer "ate_pattern"
  $fDrive = [ATE]::StyleFore($g_ed, (StyleOf $styles "SCE_ATEP_VEC_DRIVE"))
  $fCmp   = [ATE]::StyleFore($g_ed, (StyleOf $styles "SCE_ATEP_VEC_CMP"))
  $fBoth  = [ATE]::StyleFore($g_ed, (StyleOf $styles "SCE_ATEP_VEC_DRV_CMP"))
  $fMask  = [ATE]::StyleFore($g_ed, (StyleOf $styles "SCE_ATEP_VEC_MASK"))
  $fCtrl  = [ATE]::StyleFore($g_ed, (StyleOf $styles "SCE_ATEP_VEC_CTRL"))
  # STYLE_DEFAULT (32) carries the body text colour, so this is the check with
  # teeth: mask must NOT look like ordinary text.
  $fBody  = [ATE]::StyleFore($g_ed, 32)
  Write-Output ("  [diag] drive={0} cmp={1} drive+cmp={2} mask={3} ctrl={4} body={5}" -f `
                [ATE]::HexRgb($fDrive), [ATE]::HexRgb($fCmp), [ATE]::HexRgb($fBoth),
                [ATE]::HexRgb($fMask), [ATE]::HexRgb($fCtrl), [ATE]::HexRgb($fBody))
  $palette = @{
    "drive"     = $fDrive
    "cmp"       = $fCmp
    "drive+cmp" = $fBoth
    "mask"      = $fMask
    "ctrl"      = $fCtrl
  }
  $names = @($palette.Keys)
  for ($i = 0; $i -lt $names.Count; $i++) {
    for ($j = $i + 1; $j -lt $names.Count; $j++) {
      if ($palette[$names[$i]] -eq $palette[$names[$j]]) {
        Fail ("vector classes {0} and {1} share colour {2} - the five manual classes " +
              "must be separable at a glance" -f $names[$i], $names[$j],
              [ATE]::HexRgb($palette[$names[$i]])) } } }
  if ($fMask -eq $fBody) {
    Fail ("mask colour {0} equals the body text colour - X would be " +
          "indistinguishable from an ordinary identifier" -f [ATE]::HexRgb($fMask)) }
  Write-Output "  OK five vector classes distinct, and mask is not body text"
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
  Expect-Lexer "stil"
  Expect-Style 0 0 "SCE_STIL_BLOCK"   "line0 'Signals' block keyword"
  Expect-Style 1 0 "SCE_STIL_NAME"    "line1 'CLK' quoted name"
  Expect-Style 1 6 "SCE_STIL_KEYWORD" "line1 'In' direction keyword"
  Expect-Style 2 0 "SCE_STIL_KEYWORD" "line2 'Period' keyword"
  Expect-Style 2 7 "SCE_STIL_UNIT"    "line2 '20ns' quantity with unit"
  Write-Output "P2-OK"

  # ---------------------------------------------------------------- P3: .log
  Write-Output "[P3] .log (conservative gate)"
  $log = Join-Path $work "t.log"
  (@(
    'SITE1 VDD 1.8 FAIL [1.0, 2.0]'
    '2026-09-17 08:12:20 INFO Service started successfully'
  ) -join "`r`n") + "`r`n" | Set-Content -Path $log -Encoding ASCII

  Start-App $log
  Expect-Lexer "ate_log"
  Expect-Style 0 0  "SCE_ATEL_SITE"     "line0 'SITE1' site marker"
  Expect-Style 0 6  "SCE_ATEL_TESTNAME" "line0 'VDD' test name"
  Expect-Style 0 10 "SCE_ATEL_FAIL"     "line0 measurement escalated (FAIL line)"
  Expect-Style 0 14 "SCE_ATEL_FAIL"     "line0 'FAIL' verdict"
  Expect-Style 0 19 "SCE_ATEL_FAIL"     "line0 limit escalated (FAIL line)"
  # The conservative gate: a PROPER subset of ATE evidence must not be enough.
  # Line1 is a timestamped app log with no SITE, no verdict and no limit range,
  # so every byte of it must stay SCE_ATEL_DEFAULT.
  Expect-Style 1 0  "SCE_ATEL_DEFAULT"  "line1 generic log, line start uncolored"
  Expect-Style 1 20 "SCE_ATEL_DEFAULT"  "line1 generic log, 'INFO' uncolored"
  Expect-Style 1 25 "SCE_ATEL_DEFAULT"  "line1 generic log, test name uncolored"
  Write-Output "P3-OK"

  Write-Output "ATE-LANGS-E2E-PASS"
  Cleanup
  exit 0
} catch {
  Fail $_.Exception.Message
}
