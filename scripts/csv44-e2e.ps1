param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 44 e2e: CSV panel print table (ID_CTX_PRINT=1396). On Win11 24H2 the
# legacy PrintDlgW is redirected to the modern print flyout hosted in ANOTHER
# process as an ApplicationFrameWindow (title = "Printing from a Win32 app"),
# so detection must be class+title based, not pid+#32770 based. Page-layout
# fidelity is covered by ctest BuildPrintPages. ASCII only (CJK via \uXXXX).
$ErrorActionPreference = "Stop"

# ---- profile guard -----------------------------------------------------------
# Uniform rule: every e2e probe declares its profile handling. This one launches
# the app itself, so it may kill stray instances first; a surviving
# session-<pid>.json would otherwise be resurrected as a phantom window.
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
. (Join-Path $scriptDir '_profile-guard.ps1')
Start-ProfileGuard
trap { Say-PG ("PROFILE-GUARD-TRAP: " + $_.Exception.Message); Stop-ProfileGuard; exit 1 }

# Orphan instances steal windows (batch 42 lesson) - kill first.
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$work = Join-Path $env:TEMP ("opencode\xfs-csv44-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$csvPath = Join-Path $work "test.csv"
$lines = @('name,qty', 'alpha,1', 'beta,"x,y"', 'gamma,3')
[IO.File]::WriteAllText($csvPath, ($lines -join "`r`n") + "`r`n")

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class C44 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr res);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr FindByClass(IntPtr root, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(root, (h, l) => {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  // print-related title keywords across our UI languages + en
  const string CN = "\u6253\u5370";          // da-yin (zh)
  const string JP = "\u5237";                // satsu (ja)
  const string KR = "\uC778\uD480";          // insole (ko)
  const string TW = "\u5217\u5370";          // lie-yin (zh-TW)
  public static IntPtr FindPrintDialog() {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() != "ApplicationFrameWindow") return true;
      StringBuilder tb = new StringBuilder(512); GetWindowTextW(h, tb, 512);
      string t = tb.ToString();
      if (t.IndexOf(CN) >= 0 || t.IndexOf(JP) >= 0 || t.IndexOf(KR) >= 0
          || t.IndexOf(TW) >= 0 || t.IndexOf("Print", StringComparison.OrdinalIgnoreCase) >= 0) {
        found = h; return false;
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@
function Fail($m) {
  Write-Output "E2E-FAIL: $m"
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  exit 1
  Stop-ProfileGuard
}

$p = Start-Process -FilePath $Exe -ArgumentList "`"$csvPath`"" -PassThru
try {
  $deadline = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window" }
  Start-Sleep -Milliseconds 2500
  $main = $p.MainWindowHandle
  [C44]::PostMessage($main, 0x0111, [IntPtr]589, [IntPtr]::Zero) | Out-Null   # ViewCsvView
  Start-Sleep -Milliseconds 2000
  $panel = [C44]::FindByClass($main, "xfsWinPadCsvPanel")
  if ($panel -eq [IntPtr]::Zero) { Fail "no csv panel" }
  $list = [C44]::FindByClass($panel, "SysListView32")
  if ($list -eq [IntPtr]::Zero) { Fail "no listview" }

  # C1: print command -> modern print flyout appears (out-of-process frame)
  [C44]::PostMessage($panel, 0x0111, [IntPtr]1396, [IntPtr]::Zero) | Out-Null
  $dlg = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    $dlg = [C44]::FindPrintDialog()
    if ($dlg -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
  }
  if ($dlg -eq [IntPtr]::Zero) { Fail "no print dialog (frame window)" }
  Write-Output "C1-OK print dialog shown"

  # Win11 24H2 hosts the print flyout out-of-process; its buttons are not
  # drivable by cross-process messages, so cancel path is covered by manual
  # test (2026-09-12) and page layout by ctest BuildPrintPages. Cleanup here
  # just kills the app; the flyout dies with the PrintDlgW session.
  if ($p.HasExited) { Fail "app exited early" }
  Write-Output "CSV44-E2E-PASS"
  exit 0
} catch {
  Fail $_.Exception.Message
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
Stop-ProfileGuard
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
