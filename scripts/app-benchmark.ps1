# app-benchmark.ps1 - app-level benchmarks (xu-qiu doc section 60):
#   Startup / Open(10MB,120MB) / Save / Memory.
# Results: printed as a table and written to out\app-benchmark-last.csv.
#
# Lessons baked in (TODO batches 10/38):
#   - pure ASCII only (PS 5.1 reads BOM-less UTF-8 as ANSI)
#   - kill old instance + wait 2.5s before each launch (file-lock release)
#   - delete autosave .asb + session.json before runs (deterministic startup,
#     no recovery dialog, no session-restore noise)
#   - one warmup launch before measured startup runs
#   - cross-process text reads via WM_GETTEXTLENGTH (SCI_GETTEXT pointer would
#     crash the app -- batch 10 red line)
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [int]$StartupRuns = 3,
    [switch]$SkipLargeFile
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text;
public class BM {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr MainWndFor(uint pid) {
    IntPtr r = IntPtr.Zero;
    EnumWindows(new EnumProc((h, l) => {
      uint w; GetWindowThreadProcessId(h, out w); if (w != pid) return true;
      StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "xfsWinPadMainWindow") { r = h; return false; }
      return true; }), IntPtr.Zero);
    return r;
  }
  public static IntPtr SciFor(IntPtr frame) {
    IntPtr r = IntPtr.Zero;
    EnumChildWindows(frame, new EnumProc((h, l) => {
      StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (c.ToString() == "Scintilla") { r = h; return false; }
      return true; }), IntPtr.Zero);
    return r;
  }
  public static long TextLength(IntPtr sci) {
    return SendMessageW(sci, 0x000E, IntPtr.Zero, IntPtr.Zero).ToInt64(); // WM_GETTEXTLENGTH
  }
  public static string TitleOf(IntPtr frame) {
    StringBuilder t = new StringBuilder(256); GetWindowTextW(frame, t, 256);
    return t.ToString();
  }
}
'@

$exe = (Resolve-Path $ExePath).Path
$appData = $env:APPDATA + '\xfsWinPad'
$corpus = $env:TEMP + '\xfsbench-app'

function Kill-App {
  Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 2500
}
function Clear-TransientState {
  Get-ChildItem "$appData\autosave" -Filter *.asb -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
  Remove-Item "$appData\session.json" -Force -ErrorAction SilentlyContinue
}
function Start-Measured([string]$argLine) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  if ($argLine) { Start-Process -FilePath $exe -ArgumentList $argLine | Out-Null }
  else { Start-Process -FilePath $exe | Out-Null }
  $p = $null
  while ($sw.ElapsedMilliseconds -lt 60000) {
    $p = Get-Process xfsWinPad -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p -and [BM]::MainWndFor([uint32]$p.Id) -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 10
  }
  return @{ Proc = $p; Ms = $sw.ElapsedMilliseconds }
}
function Wait-SciLength($p, [long]$want) {
  if (-not $p) { return -1 }
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 60000) {
    $frame = [BM]::MainWndFor([uint32]$p.Id)
    if ($frame -ne [IntPtr]::Zero) {
      $sci = [BM]::SciFor($frame)
      if ($sci -ne [IntPtr]::Zero -and [BM]::TextLength($sci) -eq $want) {
        return $sw.ElapsedMilliseconds
      }
    }
    Start-Sleep -Milliseconds 10
  }
  return -1
}
function Save-And-Measure($p) {
  $frame = [BM]::MainWndFor([uint32]$p.Id)
  $sci = [BM]::SciFor($frame)
  [BM]::PostMessageW($sci, 0x0102, [IntPtr]88, [IntPtr]0) | Out-Null   # 'X' -> dirty
  Start-Sleep -Milliseconds 300
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  [BM]::PostMessageW($frame, 0x0111, [IntPtr]102, [IntPtr]::Zero) | Out-Null  # Cmd::FileSave
  while ($sw.ElapsedMilliseconds -lt 30000) {
    if (-not ([BM]::TitleOf($frame)).StartsWith('* ')) { break }
    Start-Sleep -Milliseconds 10
  }
  return $sw.ElapsedMilliseconds
}

$results = New-Object System.Collections.ArrayList
function Record([string]$name, [long]$ms, [string]$note) {
  [void]$results.Add(("$name,$ms,$note"))
  Write-Output ("{0,-30} {1,8} ms   {2}" -f $name, $ms, $note)
}

# ---- corpus --------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $corpus | Out-Null
$f10  = "$corpus\text_10mb.txt"
$f120 = "$corpus\text_120mb.txt"
$line = "benchmark line {0} padding padding padding padding padding padding`r`n"
if (-not (Test-Path $f10) -or (Get-Item $f10).Length -ne 10485760) {
  $w = [System.IO.StreamWriter]::new($f10, $false, [Text.Encoding]::UTF8, 1MB)
  $n = 0; $written = 0
  while ($written -lt 10485760) { $s = $line -f $n; $w.Write($s); $written += $s.Length; $n++ }
  $w.Close()
}
if (-not $SkipLargeFile -and
    (-not (Test-Path $f120) -or (Get-Item $f120).Length -ne 125829120)) {
  $w = [System.IO.StreamWriter]::new($f120, $false, [Text.Encoding]::UTF8, 1MB)
  $n = 0; $written = 0L
  while ($written -lt 125829120) { $s = $line -f $n; $w.Write($s); $written += $s.Length; $n++ }
  $w.Close()
}
Write-Output "corpus ready"

# ---- warmup (state convergence; not recorded) ----------------------------------
Clear-TransientState
Kill-App
$w = Start-Measured ''
Start-Sleep -Milliseconds 1500
Kill-App

# ---- 1. startup (median of N) ----------------------------------------------------
$times = @()
for ($i = 0; $i -lt $StartupRuns; $i++) {
  Clear-TransientState
  $r = Start-Measured ''
  $times += $r.Ms
  Start-Sleep -Milliseconds 1200
  Kill-App
}
$sorted = $times | Sort-Object
$median = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
Record 'startup (median)' $median "runs=$StartupRuns all=$($times -join '/')"

# ---- 2. open 10MB + save ----------------------------------------------------------
# UTF8 StreamWriter writes a 3-byte BOM -> document length = actual size - 3
# (generation loop counts chars; formatting digits make size drift, so use the
# ACTUAL file size, not the nominal target)
Clear-TransientState
$r = Start-Measured ('"' + $f10 + '"')
$want10 = (Get-Item $f10).Length - 3
$ms = Wait-SciLength $r.Proc $want10
$wsMB = if ($r.Proc) { [math]::Round($r.Proc.WorkingSet64 / 1MB, 1) } else { -1 }
Record 'open 10MB' $ms "ws=${wsMB}MB"
if ($ms -ge 0) {
  $sms = Save-And-Measure $r.Proc
  Record 'save 10MB (dirty->disk)' $sms ''
}
Kill-App

# ---- 3. open 120MB (large-file path) ----------------------------------------------
if (-not $SkipLargeFile) {
  Clear-TransientState
  $r = Start-Measured ('"' + $f120 + '"')
  $want120 = (Get-Item $f120).Length - 3
  $ms = Wait-SciLength $r.Proc $want120
  $wsMB = if ($r.Proc) { [math]::Round($r.Proc.WorkingSet64 / 1MB, 1) } else { -1 }
  Record 'open 120MB' $ms "ws=${wsMB}MB"
  Kill-App
}

# ---- emit CSV -----------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path out | Out-Null
'scenario,ms,note' | Set-Content -Path 'out\app-benchmark-last.csv' -Encoding ascii
foreach ($row in $results) { Add-Content -Path 'out\app-benchmark-last.csv' -Value $row -Encoding ascii }
Write-Output 'csv -> out\app-benchmark-last.csv'
