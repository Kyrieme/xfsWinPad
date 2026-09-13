param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 46 e2e: Git integration v1. Creates a throwaway git repo with a
# unique branch name, opens one tracked file through the CLI, and asserts:
#   A1 the main window title gains "[branch]" (async git rev-parse result)
#   A2 the app log records "git: branch <name>"
# Tree coloring + compare-with-HEAD menu are covered by unit tests (git
# parse) and manual verification; UIA cannot see owner-draw colors. ASCII only.
$ErrorActionPreference = "Stop"

# git must exist for this scenario to be meaningful.
try { $null = & git --version 2>&1 } catch { Write-Output "GIT46-E2E-SKIP no git"; exit 0 }
if ($LASTEXITCODE -ne 0) { Write-Output "GIT46-E2E-SKIP no git"; exit 0 }

# Orphan instances forward files to themselves + steal windows - kill first.
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$branch = "e2e46z" + (Get-Random -Maximum 99999)
$work = Join-Path $env:TEMP ("opencode\xfs-git46-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$repo = Join-Path $work "repo"
New-Item -ItemType Directory -Force -Path $repo | Out-Null
Set-Content -Path (Join-Path $repo "a.txt") -Value "one`r`ntwo" -NoNewline
$null = & git -C $repo init -q -b $branch 2>&1
$null = & git -C $repo add a.txt 2>&1
$null = & git -C $repo -c user.email=e2e@e2e.local -c user.name=e2e commit -qm init 2>&1
Set-Content -Path (Join-Path $repo "a.txt") -Value "one CHANGE`r`ntwo" -NoNewline
Set-Content -Path (Join-Path $repo "b.txt") -Value "untracked" -NoNewline   # green item
if ($LASTEXITCODE -ne 0) { Write-Output "GIT46-E2E-SKIP git init failed"; exit 0 }
$aPath = Join-Path $repo "a.txt"

$log = Join-Path $env:LOCALAPPDATA "xfsWinPad\logs\xfsWinPad.log"
$logOff = 0
if (Test-Path $log) { $logOff = (Get-Item $log).Length }

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class C46 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public static string Title(IntPtr h) {
    StringBuilder sb = new StringBuilder(1024); GetWindowTextW(h, sb, 1024);
    return sb.ToString();
  }
}
"@
function Fail($m) {
  Write-Output "E2E-FAIL: $m"
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  exit 1
}

$p = Start-Process -FilePath $Exe -ArgumentList "`"$aPath`"" -PassThru
try {
  $deadline = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window" }

  # A1: title grows " [branch]" once the async git refresh lands (unique
  # branch name => no false positive from a restored session's repo).
  $needle = "[" + $branch + "]"
  $title = ""
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    $title = [C46]::Title($p.MainWindowHandle)
    if ($title -like "*$needle*") { break }
    Start-Sleep -Milliseconds 400
  }
  if ($title -notlike "*$needle*") { Fail "title missing branch: got '$title'" }
  Write-Output "A1-OK title branch suffix"

  # A2: log line from the git client snapshot handler
  $newTail = ""
  if (Test-Path $log) {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
      $fs.Seek($logOff, 'Begin') | Out-Null
      $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
      $newTail = $sr.ReadToEnd()
    } finally { $fs.Dispose() }
  }
  if ($newTail -notlike "*git: branch $branch*") { Fail "log missing git branch line" }
  Write-Output "A2-OK log recorded branch"

  if ($p.HasExited) { Fail "app exited early" }
  Write-Output "GIT46-E2E-PASS"
  exit 0
} catch {
  Fail $_.Exception.Message
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
