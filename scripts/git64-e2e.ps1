# git64-e2e.ps1 - batch 64 regression: opening a folder through the CLI
# shows the file-explorer panel (window-level proof of the explorerVisible
# fix) and the git title suffix still works for file opens.
#
# Assertions (no UIA, no mouse - only window state + log + title):
#   A1 folder arg  -> FileExplorer panel window exists, visible, sized
#   A2 folder arg  -> SysTreeView32 exists inside the panel
#   A3 folder arg  -> log records "Project root: <fixture>"
#   A4 file arg    -> main window title grows " [branch]" (unique branch)
#   A5 file arg    -> log records "git: branch <branch>"
#   A6 app stays alive (no crash on either run)
param(
  [string]$Exe = "",
  [string]$WorkRoot = ""
)
$ErrorActionPreference = "Stop"

$native = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace W64n {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static class U32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public static string Class(IntPtr h) {
      var sb = new StringBuilder(256);
      GetClassNameW(h, sb, 256);
      return sb.ToString();
    }
    public static string Title(IntPtr h) {
      var sb = new StringBuilder(1024);
      GetWindowTextW(h, sb, 1024);
      return sb.ToString();
    }
    // visible top-level main window of the target process
    public static IntPtr FindMain(uint target) {
      IntPtr found = IntPtr.Zero;
      EnumWindows((h, l) => {
        uint pid;
        GetWindowThreadProcessId(h, out pid);
        if (pid == target && IsWindowVisible(h) && Class(h) == "xfsWinPadMainWindow")
        { found = h; return false; }
        return true;
      }, IntPtr.Zero);
      return found;
    }
    // flattened child search under a parent (panel is a child of main)
    public static IntPtr FindChild(IntPtr parent, string cls) {
      IntPtr found = IntPtr.Zero;
      EnumChildWindows(parent, (h, l) => {
        if (Class(h) == cls) { found = h; return false; }
        return true;
      }, IntPtr.Zero);
      return found;
    }
  }
}
'@
Add-Type -TypeDefinition $native

if ($Exe -eq "") {
  $Exe = Join-Path (Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent) "build\bin\Release\xfsWinPad.exe"
}
if (-not (Test-Path $Exe)) { Write-Output "GIT64-E2E-SKIP exe not found"; exit 0 }
if ($WorkRoot -eq "") {
  $WorkRoot = Join-Path $env:TEMP ("opencode\xfs-git64-" + (Get-Date).Ticks)
}

$settings = Join-Path $env:APPDATA "xfsWinPad\settings.json"
$settingsBak = "$WorkRoot.settings.bak"
$log = Join-Path $env:LOCALAPPDATA "xfsWinPad\logs\xfsWinPad.log"

function KillApp {
  Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
  $deadline = (Get-Date).AddSeconds(8)
  while ((Get-Process xfsWinPad -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
  }
}

function Wait-Main($p) {
  $deadline = (Get-Date).AddSeconds(60)
  $main = [IntPtr]::Zero
  while ((Get-Date) -lt $deadline) {
    $main = [W64n.U32]::FindMain([uint32]$p.Id)
    if ($main -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 300
    $p.Refresh()
    if ($p.HasExited) { Fail "app exited before window" }
  }
  if ($main -eq [IntPtr]::Zero) { Fail "no main window" }
  Start-Sleep -Milliseconds 800
  return $main
}

function Read-LogTail($off) {
  if (-not (Test-Path $log)) { return "" }
  $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
  try {
    $fs.Seek($off, 'Begin') | Out-Null
    $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
    return $sr.ReadToEnd()
  } finally { $fs.Dispose() }
}

function Fail($m) {
  Write-Output "GIT64-E2E-FAIL: $m"
  KillApp
  if (Test-Path $settingsBak) {
    New-Item -ItemType Directory -Force -Path (Split-Path $settings -Parent) | Out-Null
    Copy-Item $settingsBak $settings -Force
    Remove-Item $settingsBak -Force -ErrorAction SilentlyContinue
  }
  Remove-Item -Recurse -Force $WorkRoot -ErrorAction SilentlyContinue
  exit 1
}

try {
  New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

  # fixture repo: one commit, tracked file, unique branch name
  $repo = Join-Path $WorkRoot "repo"
  New-Item -ItemType Directory -Force -Path $repo | Out-Null
  $file = Join-Path $repo "h64.txt"
  Set-Content -LiteralPath $file -Value "v1" -NoNewline
  & git -C $repo init -q -b main 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Output "GIT64-E2E-SKIP git init failed"; exit 0 }
  & git -C $repo add h64.txt 2>&1 | Out-Null
  $branch = "g64-" + (Get-Date).Ticks
  & git -C $repo -c user.email=e2e@e2e -c user.name=e2e commit -qm r1 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Output "GIT64-E2E-SKIP git commit failed"; exit 0 }
  & git -C $repo checkout -q -b $branch 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Fail "git checkout -b failed" }

  # keep user settings untouched
  if (Test-Path $settings) { Copy-Item $settings $settingsBak -Force }

  KillApp

  # ---- run 1: open the folder -> explorer panel must be visible ----
  $logOff = 0
  if (Test-Path $log) { $logOff = (Get-Item $log).Length }
  $p = Start-Process -FilePath $Exe -ArgumentList "`"$repo`"" -PassThru
  $main = Wait-Main $p

  $fe = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    $fe = [W64n.U32]::FindChild($main, "xfsWinPadFileExplorer")
    if ($fe -ne [IntPtr]::Zero -and [W64n.U32]::IsWindowVisible($fe)) { break }
    Start-Sleep -Milliseconds 300
  }
  if ($fe -eq [IntPtr]::Zero) { Fail "A1 no FileExplorer panel window" }
  if (-not [W64n.U32]::IsWindowVisible($fe)) { Fail "A1 FileExplorer panel hidden" }
  $r = New-Object W64n.RECT
  [void][W64n.U32]::GetWindowRect($fe, [ref]$r)
  if (($r.Right - $r.Left) -lt 20 -or ($r.Bottom - $r.Top) -lt 20) {
    Fail ("A1 FileExplorer panel degenerate rect {0}x{1}" -f ($r.Right - $r.Left), ($r.Bottom - $r.Top))
  }
  Write-Output "A1-OK explorer panel visible"

  $tv = [W64n.U32]::FindChild($fe, "SysTreeView32")
  if ($tv -eq [IntPtr]::Zero) { Fail "A2 no SysTreeView32 in panel" }
  Write-Output "A2-OK tree control present"

  $tail = Read-LogTail $logOff
  $needleRoot = "Project root: " + $repo
  if ($tail -notlike ("*" + $needleRoot + "*")) { Fail "A3 log missing Project root line" }
  Write-Output "A3-OK log recorded project root"

  if ($p.HasExited) { Fail "A6 app exited on folder open" }
  KillApp

  # ---- run 2: open the file -> title grows the branch suffix ----
  $logOff = 0
  if (Test-Path $log) { $logOff = (Get-Item $log).Length }
  $p = Start-Process -FilePath $Exe -ArgumentList "`"$file`"" -PassThru
  $main = Wait-Main $p

  $needle = "[" + $branch + "]"
  $title = ""
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    $title = [W64n.U32]::Title($main)
    if ($title -like ("*" + $needle + "*")) { break }
    Start-Sleep -Milliseconds 400
  }
  if ($title -notlike ("*" + $needle + "*")) { Fail "A4 title missing branch: got '$title'" }
  Write-Output "A4-OK title branch suffix"

  $tail = Read-LogTail $logOff
  $needleLog = "git: branch $branch"
  if ($tail -notlike ("*" + $needleLog + "*")) { Fail "A5 log missing git branch line" }
  Write-Output "A5-OK log recorded branch"

  if ($p.HasExited) { Fail "A6 app exited on file open" }
  Write-Output "GIT64-E2E-PASS"
} catch {
  Fail $_.Exception.Message
} finally {
  KillApp
  if (Test-Path $settingsBak) {
    New-Item -ItemType Directory -Force -Path (Split-Path $settings -Parent) | Out-Null
    Copy-Item $settingsBak $settings -Force
    Remove-Item $settingsBak -Force -ErrorAction SilentlyContinue
  }
  Remove-Item -Recurse -Force $WorkRoot -ErrorAction SilentlyContinue
}
exit 0
