param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\bin\Release\xfsWinPad.exe"),
    [int]$ExpectExtraDocs = 0   # negative control: revert the fix and pass 1
)
# Batch 132 e2e: reopening a file that is ALREADY OPEN IN THE RIGHT SPLIT VIEW.
#
# THE BUG (fixed in batch 132)
#   A file open in the right view was reopened, and a SECOND copy appeared in the
#   LEFT view. Root cause was in Workspace::OpenPath: the "is this file already
#   open?" probe walked docs_ (left view) only, so a right-view document looked
#   brand new. The function then parsed the file again and ended in InsertDoc,
#   which always lands on the left (doc->view = 0) - hence the duplicate, and
#   hence the second reopen being a no-op (by then the left copy exists).
#
# WHAT THIS PROBE DRIVES
#   The recurrence point is Workspace::OpenPath, which is shared by every open
#   route (explorer double-click -> OpenUserFile -> OpenPath, File>Recent ->
#   OpenUserFile -> OpenPath). Reading a SysTreeView32 node by coordinates is
#   flaky in this sandbox, so the reopen is driven through File>Recent, i.e. the
#   very same OpenUserFile entry point the explorer click uses.
#
# SIGNAL
#   Every open document owns exactly one Scintilla control under the frame, so
#   "number of Scintilla children" == "number of open documents" whether or not
#   the pane is visible. Buggy build: +1 after the reopen. Fixed build: 0.
#   Second, independent signal: OpenPath logs "Opened <path> (x ms)" once per real
#   load, so the app log must show exactly one load of bbb.txt.
#
# SETUP VALIDITY IS ASSERTED, NOT ASSUMED
#   The probe refuses to call itself a pass unless the split really happened:
#   visible editors must go 1 -> 2 after ViewMoveToOtherView. Otherwise the test
#   would pass vacuously on a build where the move silently did nothing.
#
# NEGATIVE CONTROL (run this before trusting a pass)
#   Make OpenPath's already-open probe left-view-only again, rebuild, and run with
#   -ExpectExtraDocs 1: the probe must print DUALVIEW-REOPEN-E2E-FAIL with
#   total 2 -> 3 and two "Opened ...bbb.txt" log lines. A guard that only ever
#   prints PASS is indistinguishable from one that checks nothing.
#
# The real profile is snapshotted/restored by scripts\_profile-guard.ps1: the app
# resolves its data directory via SHGetKnownFolderPath, so pointing $env:APPDATA
# at a scratch dir does NOT isolate it. ASCII only.
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir '_common-win32.ps1')
. (Join-Path $scriptDir '_profile-guard.ps1')

# The PowerShell host does not surface stdout, so every line goes to a file too.
# Truncate by writing without -Append on the first call - never by deleting the
# file first, which the safe-delete hook turns into a terminating error.
$out = Join-Path ([IO.Path]::GetTempPath()) 'xfs-dualview-reopen-e2e.txt'
$script:n = 0
function Say($m) {
    $line = ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
    if ($script:n -eq 0) { $line | Out-File -FilePath $out -Encoding ascii }
    else { $line | Out-File -FilePath $out -Encoding ascii -Append }
    $script:n++
    Write-Output $line
}
function Say-PG([string]$s) { Say $s }

# Command ids come from the header, never hardcoded.
function Get-CmdId([string]$name) {
    $txt = [IO.File]::ReadAllText((Join-Path $repo 'src\core\CommandIds.h'))
    $m = [regex]::Match($txt, "\b" + $name + "\s*=\s*(\d+)")
    if (-not $m.Success) { throw "command id not found: $name" }
    return [int]$m.Groups[1].Value
}
$CMD_MOVE_OTHER = Get-CmdId 'ViewMoveToOtherView'
$CMD_RECENT0    = Get-CmdId 'FileRecentFirst'

function Count-Sci([bool]$VisibleOnly) {
    $script:sciN = 0
    if ([WIN]::frame -eq [IntPtr]::Zero) { return 0 }
    [WIN]::EnumChildWindows([WIN]::frame, [WIN+EnumProc]{
        param($h, $l)
        if ([WIN]::ClassOf($h) -eq 'Scintilla') {
            if ((-not $VisibleOnly) -or [WIN]::IsWindowVisible($h)) { $script:sciN++ }
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    return $script:sciN
}
function Send-Cmd([int]$id) {
    [WIN]::PostMessageW([WIN]::frame, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
}

# --- fixture -----------------------------------------------------------------
$fx = Join-Path ([IO.Path]::GetTempPath()) 'xfs-dualview-fixture'
[IO.Directory]::CreateDirectory($fx) | Out-Null
$fa = Join-Path $fx 'aaa.txt'
$fb = Join-Path $fx 'bbb.txt'
[IO.File]::WriteAllText($fa, "AAA dualview fixture`r`n")
[IO.File]::WriteAllText($fb, "BBB dualview fixture`r`n")

$log = Join-Path (Split-Path -Parent $Exe) 'debug.log'
$logBase = 0
if (Test-Path $log) { $logBase = ([IO.File]::ReadAllLines($log)).Count }

$proc = $null
trap { Say ("TRAP: " + $_.Exception.Message); Stop-ProfileGuard; exit 1 }
Start-ProfileGuard

# Two file arguments: StartupSession skips session restore entirely and opens
# both into the left view, so the probe starts from a known document set (2).
$proc = Start-Process -FilePath $Exe -ArgumentList @("`"$fa`"", "`"$fb`"") -PassThru
Say ("launched pid=" + $proc.Id + " exe=" + $Exe)
[WIN]::pid = [uint32]$proc.Id
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    [WIN]::LocateFrame()
    if ([WIN]::frame -ne [IntPtr]::Zero) { break }
}
if ([WIN]::frame -eq [IntPtr]::Zero) {
    Say "FAIL: no frame window appeared"
    Stop-ProfileGuard
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Start-Sleep -Milliseconds 1500

$n0 = Count-Sci $false
$v0 = Count-Sci $true
Say ("after load: total=$n0 visible=$v0 (expect total=2 visible=1)")

# move the active document (bbb.txt) into the right split view
Send-Cmd $CMD_MOVE_OTHER
Start-Sleep -Milliseconds 900
$n1 = Count-Sci $false
$v1 = Count-Sci $true
Say ("after move-to-other-view($CMD_MOVE_OTHER): total=$n1 visible=$v1 (expect total=2 visible=2)")

# reopen bbb.txt (most recent = FileRecentFirst) - the reported flow
Send-Cmd $CMD_RECENT0
Start-Sleep -Milliseconds 1600
$n2 = Count-Sci $false
Say ("after reopen #1 (recent $CMD_RECENT0): total=$n2 (delta=" + ($n2 - $n1) + ", expect " + $ExpectExtraDocs + ")")

Send-Cmd $CMD_RECENT0
Start-Sleep -Milliseconds 1600
$n3 = Count-Sci $false
Say ("after reopen #2: total=$n3 (delta=" + ($n3 - $n2) + ", expect " + $ExpectExtraDocs + ")")

# log evidence: OpenPath logs "Opened <path> (x ms)" once per real load
if (Test-Path $log) {
    $lines = [IO.File]::ReadAllLines($log)
    $tail = @()
    if ($lines.Count -gt $logBase) { $tail = $lines[$logBase..($lines.Count - 1)] }
    $loads = @($tail | Where-Object { $_ -match 'Opened .*bbb\.txt' }).Count
    Say ("log: 'Opened ...bbb.txt' lines = $loads (fixed build: 1; buggy: 2)")
}

$setupOk = ($v0 -eq 1) -and ($v1 -eq 2)
$verdict = "FAIL"
if ($setupOk -and ($n2 - $n1) -eq $ExpectExtraDocs -and ($n3 - $n2) -eq $ExpectExtraDocs) {
    $verdict = "PASS"
}
Say ("setup-valid=" + $(if ($setupOk) { 'yes' } else { 'NO - split never happened, the run proves nothing' }))
Say ("DUALVIEW-REOPEN-E2E-$verdict")

Stop-ProfileGuard
try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
exit $(if ($verdict -eq "PASS") { 0 } else { 1 })
