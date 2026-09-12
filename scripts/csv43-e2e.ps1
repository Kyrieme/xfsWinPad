param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 43 e2e: CSV panel go-to-row (ID_CTX_GOTO) + export selected rows
# (ID_CTX_EXPORT). ASCII only.
$ErrorActionPreference = "Stop"

# Orphan instances steal windows (batch 42 lesson) - kill first.
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$work = Join-Path $env:TEMP ("opencode\xfs-csv43-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$csvPath = Join-Path $work "test.csv"
$expPath = Join-Path $work "test_export.csv"
$lines = @('name,qty', 'alpha,1', 'beta,"x,y"', 'gamma,3')
[IO.File]::WriteAllText($csvPath, ($lines -join "`r`n") + "`r`n")
$log = Join-Path $env:LOCALAPPDATA "xfsWinPad\logs\xfsWinPad.log"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class C43 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessagePtr(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string win);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr FindByClass(IntPtr root, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(root, (h, l) => {
      StringBuilder sb = new StringBuilder(256);
      GetClassName(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  // FindWindowEx misses dialogs owned by a remote busy thread, and unfiltered
  // EnumWindows can hit another user instance's #32770; filter by pid.
  public static IntPtr FindTopByClassPid(string cls, uint target) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid != target) return true;
      StringBuilder sb = new StringBuilder(256);
      GetClassName(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
}
"@
function Fail($m) {
  Write-Output "E2E-FAIL: $m"
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  exit 1
}
function WaitWindow([string]$cls) {
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    $h = [C43]::FindTopByClassPid($cls, $appPid)
    if ($h -ne [IntPtr]::Zero) { return $h }
    Start-Sleep -Milliseconds 150
  }
  return [IntPtr]::Zero
}
function Goto($panel, [string]$num) {
  [C43]::PostMessage($panel, 0x0111, [IntPtr]1394, [IntPtr]::Zero) | Out-Null
  $dlg = WaitWindow "xfsWinPadInputBox"
  if ($dlg -eq [IntPtr]::Zero) { Fail "no input box for goto $num" }
  $edit = [C43]::GetDlgItem($dlg, 3001)
  if ($edit -eq [IntPtr]::Zero) { Fail "no edit in input box" }
  [C43]::SendMessageStr($edit, 0x000C, [IntPtr]::Zero, $num) | Out-Null   # WM_SETTEXT
  $ok = [C43]::GetDlgItem($dlg, 1)
  [C43]::SendMessagePtr($dlg, 0x0111, [IntPtr]1, $ok) | Out-Null   # WM_COMMAND IDOK (cross-proc BM_CLICK does not close dialog)
  $deadline = (Get-Date).AddSeconds(6)
  while ([C43]::FindTopByClassPid("xfsWinPadInputBox", $appPid) -ne [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
  }
}
$LVM_GETNEXTITEM = 0x100C; $LVNI_SELECTED = 0x0002; $LVM_GETSELECTEDCOUNT = 0x1032
function Selected($list) {
  $out = @()
  for ($i = -1; ($i = [C43]::SendMessagePtr($list, $LVM_GETNEXTITEM, [IntPtr]$i, [IntPtr]$LVNI_SELECTED).ToInt32()) -ge 0;) {
    $out += $i
    if ($out.Count -gt 50) { break }
  }
  return $out
}

$p = Start-Process -FilePath $Exe -ArgumentList "`"$csvPath`"" -PassThru
$appPid = [uint32]$p.Id
try {
  $deadline = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window" }
  Start-Sleep -Milliseconds 2500
  $main = $p.MainWindowHandle
  [C43]::PostMessage($main, 0x0111, [IntPtr]589, [IntPtr]::Zero) | Out-Null   # ViewCsvView
  Start-Sleep -Milliseconds 2000
  $panel = [C43]::FindByClass($main, "xfsWinPadCsvPanel")
  if ($panel -eq [IntPtr]::Zero) { Fail "no csv panel" }
  $list = [C43]::FindByClass($panel, "SysListView32")
  if ($list -eq [IntPtr]::Zero) { Fail "no listview" }
  $logMark = (Get-Item $log).Length   # only assert markers written by THIS run

  # C1: goto row 2 -> selected index 1 (0-based display row)
  Goto $panel "2"
  $sel = Selected $list
  if ($sel.Count -ne 1 -or $sel[0] -ne 1) { Fail "goto 2 selected=[$($sel -join ',')] expected [1]" }
  Write-Output "C1-OK goto row 2"

  # C2: goto row 3 -> row 2 also selected (multiselect, non-SINGLESEL)
  Goto $panel "3"
  $sel = Selected $list
  $selCount = [C43]::SendMessagePtr($list, $LVM_GETSELECTEDCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
  if ($sel -notcontains 2 -or $selCount -lt 2) { Fail "goto 3 selected=[$($sel -join ',')] count=$selCount" }
  Write-Output "C2-OK goto row 3, selected=$selCount"

  # C3: invalid input rejected (out of range) - selection unchanged
  $before = ($sel -join ',')
  Goto $panel "99"
  $after = ((Selected $list) -join ',')
  if ($after -ne $before) { Fail "out-of-range goto changed selection [$before]->[$after]" }
  Write-Output "C3-OK out-of-range rejected"

  # C4: export selected rows -> save dialog appears (pid-filtered); cancel via WM_CLOSE.
  # Cross-process automation cannot drive the modern #32770 Save button (DirectUI),
  # so disk-content fidelity is covered by ctest SerializeRows instead.
  [C43]::PostMessage($panel, 0x0111, [IntPtr]1395, [IntPtr]::Zero) | Out-Null
  $dlg = WaitWindow "#32770"
  if ($dlg -eq [IntPtr]::Zero) { Fail "no save dialog" }
  [C43]::PostMessage($dlg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE = cancel
  $deadline = (Get-Date).AddSeconds(6)
  while ([C43]::FindTopByClassPid("#32770", $appPid) -ne [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
  }
  if ([C43]::FindTopByClassPid("#32770", $appPid) -ne [IntPtr]::Zero) { Fail "save dialog not closed" }
  if (Test-Path $expPath) { Fail "export file created after cancel" }
  $cnt = [C43]::SendMessagePtr($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()   # LVM_GETITEMCOUNT
  if ($cnt -ne 3) { Fail "list count after cancel=$cnt expected 3" }
  Write-Output "C4-OK export dialog shown, cancelled cleanly"

  # C5: this-run log slice has goto + export markers, no completion after cancel
  $fs = [IO.File]::Open($log, "Open", "Read", "ReadWrite")
  [void]$fs.Seek($logMark, "Begin")
  $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
  $logTxt = $sr.ReadToEnd(); $sr.Close()
  if ($logTxt -notmatch "CsvPanel: goto row 3") { Fail "log missing goto marker" }
  if ($logTxt -notmatch "CsvPanel: export begin rows=2") { Fail "log missing export marker" }
  if ($logTxt -match "CsvPanel: exported") { Fail "log shows export completed despite cancel" }
  Write-Output "C5-OK log markers"

  # C6: app still alive, close cleanly
  if ($p.HasExited) { Fail "app exited early" }
  [C43]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
  $p.WaitForExit(15000) | Out-Null
  if (!$p.HasExited) { Fail "app did not exit" }
  Write-Output "CSV43-E2E-PASS"
  exit 0
} catch {
  Fail $_.Exception.Message
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
