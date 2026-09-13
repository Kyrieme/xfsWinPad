param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 49 e2e: tracking ahead/behind warning in the title bar. Builds a
# bare origin, a pushed work clone and a second clone that is 1 commit
# ahead AND 1 commit behind, opens a file there and asserts:
#   A1 title shows "[main <up>1<down>1]" (async status -b result)
#   A2 log records "git: tracking ahead=1 behind=1"
# ASCII only; the arrows are built from char codes.
$ErrorActionPreference = "Stop"

try { $null = & git --version 2>&1 } catch { Write-Output "GIT49-E2E-SKIP no git"; exit 0 }
if ($LASTEXITCODE -ne 0) { Write-Output "GIT49-E2E-SKIP no git"; exit 0 }

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$work = Join-Path $env:TEMP ("opencode\xfs-git49-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$origin = Join-Path $work "origin.git"
$wdir = Join-Path $work "w"
$c2 = Join-Path $work "c2"

$null = & git init --bare -q -b main $origin 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "GIT49-E2E-SKIP git too old (init -b)"; exit 0 }
New-Item -ItemType Directory -Force -Path $wdir | Out-Null
Set-Content -Path (Join-Path $wdir "seed.txt") -Value "s" -NoNewline
$null = & git -C $wdir init -q -b main 2>&1
$null = & git -C $wdir add seed.txt 2>&1
$null = & git -C $wdir -c user.email=e2e@e2e.local -c user.name=e2e commit -qm seed 2>&1
$null = & git -C $wdir remote add origin $origin 2>&1
$null = & git -C $wdir push -q -u origin main 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "GIT49-E2E-SKIP push failed"; exit 0 }

$null = & git clone -q $origin $c2 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "GIT49-E2E-SKIP clone failed"; exit 0 }

# origin/main advances (w pushes a second commit)...
Set-Content -Path (Join-Path $wdir "p2.txt") -Value "p2" -NoNewline
$null = & git -C $wdir add p2.txt 2>&1
$null = & git -C $wdir -c user.email=e2e@e2e.local -c user.name=e2e commit -qm p2 2>&1
$null = & git -C $wdir push -q origin main 2>&1
# ...and c2 gains its own local commit => c2 is ahead 1.
Set-Content -Path (Join-Path $c2 "local.txt") -Value "l" -NoNewline
$null = & git -C $c2 add local.txt 2>&1
$null = & git -C $c2 -c user.email=e2e@e2e.local -c user.name=e2e commit -qm local 2>&1
$wPath = Join-Path $wdir "seed.txt"
$cPath = Join-Path $c2 "seed.txt"
$null = $wPath
# status -b compares against the cached remote-tracking ref: fetch c2 so the
# behind-1 counter becomes visible (this app does not fetch on its own).
$null = & git -C $c2 fetch -q origin 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "GIT49-E2E-SKIP fetch failed"; exit 0 }

$log = Join-Path $env:LOCALAPPDATA "xfsWinPad\logs\xfsWinPad.log"
$logOff = 0
if (Test-Path $log) { $logOff = (Get-Item $log).Length }

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class C49 {
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

$up = [string][char]0x2191
$down = [string][char]0x2193
$needle = "[main " + $up + "1 " + $down + "1]"

$p = Start-Process -FilePath $Exe -ArgumentList "`"$cPath`"" -PassThru
try {
  $deadline = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window" }

  $title = ""
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    $title = [C49]::Title($p.MainWindowHandle)
    if ($title.Contains($needle)) { break }
    Start-Sleep -Milliseconds 400
  }
  if (-not $title.Contains($needle)) { Fail "title missing ahead/behind: got '$title'" }
  Write-Output "A1-OK title ahead/behind suffix"

  $newTail = ""
  if (Test-Path $log) {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
      $fs.Seek($logOff, 'Begin') | Out-Null
      $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
      $newTail = $sr.ReadToEnd()
    } finally { $fs.Dispose() }
  }
  if ($newTail -notlike "*git: tracking ahead=1 behind=1*") { Fail "log missing tracking line" }
  Write-Output "A2-OK log recorded tracking"

  if ($p.HasExited) { Fail "app exited early" }
  Write-Output "GIT49-E2E-PASS"
  exit 0
} catch {
  Fail $_.Exception.Message
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
