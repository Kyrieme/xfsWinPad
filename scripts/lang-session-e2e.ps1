param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 66+ e2e: manual Language-menu pick must survive a restart.
#   A1 close after WM_COMMAND LangFirst+1 -> session.json entry gains "lang": 1
#   A2 relaunch (session restore) -> Language popup item 1 has MF_CHECKED
# The real %APPDATA%\xfsWinPad\session.json is backed up and restored.
# ASCII only.
$ErrorActionPreference = "Stop"

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$sess = Join-Path $env:APPDATA "xfsWinPad\session.json"
$bak = "$sess.e2elangbak"
if (Test-Path $sess) { Copy-Item $sess $bak -Force } else { Remove-Item $bak -ErrorAction SilentlyContinue }

$work = Join-Path $env:TEMP ("opencode\xfs-langsess-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$cpp = Join-Path $work "sample.cpp"
Set-Content -Path $cpp -Value "int main(){return 0;}`r`n" -Encoding ASCII

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class LS {
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr h, int pos);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr h, int pos);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr h, int id, uint flags);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  public const uint WM_COMMAND = 0x0111, WM_CLOSE = 0x0010, MF_CHECKED = 0x0008;
  public static int LangPopupPos(IntPtr main) {
    IntPtr menu = GetMenu(main);
    if (menu == IntPtr.Zero) return -1;
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
      IntPtr pop = GetSubMenu(menu, i);
      if (pop != IntPtr.Zero && GetMenuItemID(pop, 0) == 2600) return i;
    }
    return -1;
  }
}
"@

function Fail($m) {
  Write-Output "E2E-FAIL: $m"
  Cleanup
  exit 1
}
function Cleanup {
  Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 300
  if (Test-Path $bak) { Copy-Item $bak $sess -Force; Remove-Item $bak -Force }
  else { Remove-Item $sess -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
function Wait-Main([diagnostics.process]$p) {
  $dl = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window" }
  Start-Sleep -Milliseconds 1500   # let file open + idle init finish
}

$p = Start-Process -FilePath $Exe -ArgumentList "`"$cpp`"" -PassThru
try {
  Wait-Main $p

  # pick Language -> C/C++ (catalog index 1)
  [void][LS]::SendMessageW($p.MainWindowHandle, [LS]::WM_COMMAND,
                           [IntPtr]2601, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 800

  # graceful close -> SessionSave runs (document is clean, no prompt)
  [void][LS]::SendMessageW($p.MainWindowHandle, [LS]::WM_CLOSE,
                           [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $p.WaitForExit(30000)) { Fail "app did not exit after WM_CLOSE" }
  Start-Sleep -Milliseconds 500

  if (-not (Test-Path $sess)) { Fail "session.json missing after close" }
  $json = [IO.File]::ReadAllText($sess)
  if ($json -notlike "*sample.cpp*") { Fail "session.json lacks the cpp entry" }
  if ($json -notlike "*`"lang`": 1*") { Fail "session.json lacks lang:1 -> '$json'" }
  Write-Output "A1-OK session.json persisted lang pick"

  # relaunch with no args -> session restore + applyLang
  $p = Start-Process -FilePath $Exe -PassThru
  Wait-Main $p
  $dl = (Get-Date).AddSeconds(25)
  $checked = $false
  while ((Get-Date) -lt $dl -and -not $checked) {
    $pos = [LS]::LangPopupPos($p.MainWindowHandle)
    if ($pos -ge 0) {
      $pop = [LS]::GetSubMenu([LS]::GetMenu($p.MainWindowHandle), $pos)
      $st = [LS]::GetMenuState($pop, 1, 0)   # MF_BYPOSITION = 0
      if (($st -band [LS]::MF_CHECKED) -ne 0) { $checked = $true }
    }
    if (-not $checked) { Start-Sleep -Milliseconds 400; $p.Refresh() }
  }
  if (-not $checked) { Fail "restored Language menu has no C/C++ radio check" }
  Write-Output "A2-OK restart re-applied manual language"

  Write-Output "LANGSESSION-E2E-PASS"
  Cleanup
  exit 0
} catch {
  Fail $_.Exception.Message
}
