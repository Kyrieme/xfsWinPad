param([string]$Exe = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe")
# Batch 67 e2e: multi-window session slots + fan-out + New Window command.
#   B1 secondary instance saves session-<pid>.json (not the legacy file)
#   B2 relaunch: primary restores session.json AND spawns one child per slot;
#      the child claims (deletes) its slot file
#   B3 File>New Window (WM_COMMAND 112, Cmd::FileNewWindow) adds a third blank process
# The real %APPDATA%\xfsWinPad\session*.json are backed up and restored.
# ASCII only.
$ErrorActionPreference = "Stop"

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$sessDir = Join-Path $env:APPDATA "xfsWinPad"
$bakDir = Join-Path $env:TEMP "xfs-multi-e2e-bak"
if (Test-Path $bakDir) { Remove-Item -Recurse -Force $bakDir }
New-Item -ItemType Directory -Force -Path $bakDir | Out-Null
foreach ($f in Get-ChildItem (Join-Path $sessDir "session*.json") -ErrorAction SilentlyContinue) {
  Copy-Item $f.FullName (Join-Path $bakDir $f.Name) -Force
  Remove-Item $f.FullName -Force
}

$work = Join-Path $env:TEMP ("opencode\xfs-multi-" + (Get-Date).Ticks)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$f1 = Join-Path $work "alpha1.txt"
$f2 = Join-Path $work "beta2.txt"
Set-Content -Path $f1 -Value "one`r`n" -Encoding ASCII
Set-Content -Path $f2 -Value "two`r`n" -Encoding ASCII

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MI {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  public const uint WM_COMMAND = 0x0111, WM_CLOSE = 0x0010;
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
  Get-ChildItem $bakDir -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $sessDir $_.Name) -Force
  }
  Remove-Item -Recurse -Force $bakDir -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
function Wait-Main([diagnostics.process]$p) {
  $dl = (Get-Date).AddSeconds(60)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $dl) {
    Start-Sleep -Milliseconds 300; $p.Refresh()
  }
  if ($p.MainWindowHandle -eq 0) { Fail "no main window pid=$($p.Id)" }
  Start-Sleep -Milliseconds 1500
}
function Close-All-Windows() {
  # graceful WM_CLOSE for every live main window (session save path runs)
  $deadline = (Get-Date).AddSeconds(40)
  while ((Get-Process xfsWinPad -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    foreach ($p in Get-Process xfsWinPad) {
      if ($p.MainWindowHandle -ne 0) {
        try { [void][MI]::SendMessageW($p.MainWindowHandle, [MI]::WM_CLOSE,
                                       [IntPtr]::Zero, [IntPtr]::Zero) } catch {}
      }
    }
    Start-Sleep -Milliseconds 1200
    $alive = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue)
    if ($alive.Count -eq 0) { break }
  }
  if (Get-Process xfsWinPad -ErrorAction SilentlyContinue) { Fail "windows did not close" }
}

try {
  # ---- T1: two live windows (primary + explicit secondary) ---------------
  $p1 = Start-Process -FilePath $Exe -ArgumentList "`"$f1`"" -PassThru
  Wait-Main $p1
  $p2 = Start-Process -FilePath $Exe -ArgumentList "--new `"$f2`"" -PassThru
  Wait-Main $p2

  Close-All-Windows
  Start-Sleep -Milliseconds 500

  $sess = Join-Path $sessDir "session.json"
  if (-not (Test-Path $sess)) { Fail "legacy session.json missing" }
  if ((Get-Content $sess -Raw) -notlike "*alpha1.txt*") { Fail "session.json lacks f1" }
  $slots = @(Get-ChildItem (Join-Path $sessDir "session-*.json") -ErrorAction SilentlyContinue)
  if ($slots.Count -ne 1) { Fail "expected 1 slot file, got $($slots.Count)" }
  if ((Get-Content $slots[0].FullName -Raw) -notlike "*beta2.txt*") { Fail "slot lacks f2" }
  Write-Output "B1-OK secondary wrote its own session slot"

  # ---- T2: relaunch -> primary restore + fan-out --------------------------
  $p3 = Start-Process -FilePath $Exe -PassThru
  Wait-Main $p3
  $dl = (Get-Date).AddSeconds(25)
  $titles = @()
  while ((Get-Date) -lt $dl) {
    $titles = @(Get-Process xfsWinPad | ForEach-Object { $_.MainWindowTitle })
    if ($titles.Count -ge 2) { break }
    Start-Sleep -Milliseconds 500
  }
  if ($titles.Count -ne 2) { Fail "fan-out windows: got $($titles.Count)" }
  if (-not ($titles -join "|" -match "alpha1.txt")) { Fail "primary window lacks f1" }
  if (-not ($titles -join "|" -match "beta2.txt"))  { Fail "child window lacks f2" }
  if (@(Get-ChildItem (Join-Path $sessDir "session-*.json") -ErrorAction SilentlyContinue).Count -ne 0) {
    Fail "slot was not claimed by the child"
  }
  Write-Output "B2-OK primary restored and spawned the slot window"

  # ---- T3: File > New Window (Cmd::FileNewWindow = 112) --------------------
  $p3.Refresh()
  $h3 = $p3.MainWindowHandle
  if ($h3 -eq 0) { $dl = (Get-Date).AddSeconds(20); while ($h3 -eq 0 -and (Get-Date) -lt $dl) { Start-Sleep -Milliseconds 300; $p3.Refresh(); $h3 = $p3.MainWindowHandle } }
  if ($h3 -eq 0) { Fail "primary lost its window before T3" }
  [void][MI]::PostMessageW($h3, [MI]::WM_COMMAND, [IntPtr]112, [IntPtr]::Zero)
  $dl = (Get-Date).AddSeconds(25)
  $n = 0
  while ((Get-Date) -lt $dl) {
    $n = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue).Count
    if ($n -ge 3) { break }
    Start-Sleep -Milliseconds 500
  }
  if ($n -ne 3) { Fail "New Window did not spawn a third process (n=$n)" }
  Write-Output "B3-OK New Window spawned a blank third window"

  Write-Output "MULTIINSTANCE-E2E-PASS"
  Cleanup
  exit 0
} catch {
  Fail $_.Exception.Message
}
