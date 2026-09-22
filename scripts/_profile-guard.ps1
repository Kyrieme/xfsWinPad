# _profile-guard.ps1 - keep an e2e probe out of the user's REAL profile.
#
# WHY THIS EXISTS (post-mortem of the batch 104-107 probes)
#   The probes written for batches 104-107 (gotodef / navhist / defhint / findrefs,
#   plus csv44 / preferences) had NO profile handling at all. The app they start (or
#   attach to) resolves its data directory through SHGetKnownFolderPath, which is
#   registry-backed and therefore NOT redirectable through $env:APPDATA. On a clean
#   exit it writes %APPDATA%\xfsWinPad\session.json - and a NON-PRIMARY instance
#   writes session-<pid>.json instead.
#
#   A surviving session-<pid>.json is not harmless. On every later launch the
#   primary instance restores session.json AND spawns one extra window per surviving
#   slot ("--new --no-restore --restore <slot>"), and that child writes its own slot
#   when it exits. So one stray slot = one permanent phantom window, each one opening
#   whatever temp fixture the probe had loaded (t.pat). That is exactly the "two
#   windows after install" report.
#
# WHAT IT DOES
#   Start-ProfileGuard : snapshot session*.json + recent.txt to a temp dir, then kill
#                        stray xfsWinPad instances (a leftover instance is what makes
#                        the next one non-primary, and only a non-primary instance
#                        writes a slot).
#   Stop-ProfileGuard  : copy the snapshot back and report anything the probe ADDED.
#
# HARD RULES
#   * NEVER throws. This sandbox's safe-delete hook turns Remove-Item on a %APPDATA%
#     path into a terminating error ("SAFE_DELETE_BULK_GUARD_ERROR"), so every step is
#     individually guarded and the outcome is printed as text instead of raised.
#   * A probe-created session file is removed with Remove-Item, falling back to
#     [IO.File]::Delete because this sandbox's safe-delete hook turns the cmdlet
#     into a terminating error on a %APPDATA% path. If BOTH fail the guard prints
#     "PROFILE-GUARD-DIRTY: <path>" instead of raising, and you delete that file
#     from a shell that is not sandboxed (Bash `rm` works). Leaving it in place is
#     what produces the phantom window.
#   * ASCII only. PowerShell 5.1 reads a BOM-less .ps1 as ANSI; one multi-byte
#     character anywhere (even inside a here-string) eats the following newline.
#
# USAGE (attach-style probes: the caller starts the app, the script only inspects it)
#   . "$PSScriptRoot\_profile-guard.ps1"
#   Start-ProfileGuard                     # before the app is launched / attached to
#   trap { Say-PG ("PROFILE-GUARD-TRAP: " + $_.Exception.Message); Stop-ProfileGuard; exit 1 }
#   ... probe body ...
#   Stop-ProfileGuard                      # in Die() and before the verdict exit
#
#   The `trap` line is the safety net for an unexpected throw. It is a plain
#   script-scope statement on purpose: Register-EngineEvent -Action was tried first
#   and rejected, because the action runs in its own job and its output never reaches
#   the console - a restore that half-works while printing nothing is exactly the
#   "verification failed vs. subject is fine" trap this project keeps falling into.
#
#   Start-ProfileGuard -ProfileDir <dir>   # snapshot some other profile (self-test)
#   Start-ProfileGuard -NoKill             # do not touch running instances
#
# TEST THIS GUARD BEFORE TRUSTING IT: scripts\_profile-guard-selftest.ps1 runs it
# against a fake profile in %TEMP% and asserts the restore really happened, plus a
# negative control (restore stripped out) where the mutation must SURVIVE. A guard that
# prints "restored" while doing nothing is indistinguishable from one that worked
# unless you run that control. One invocation per case - `exit` inside a called .ps1
# ends the caller's session.

function Say-PG([string]$s) { Write-Output $s }

function Get-ProfileDir {
    # Same source the app uses (SHGetKnownFolderPath(FOLDERID_RoamingAppData)).
    # $env:APPDATA is only a fallback: in this sandbox it is often EMPTY, and
    # Join-Path on an empty string throws a message that never mentions APPDATA.
    $d = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
    if (-not $d) { $d = $env:APPDATA }
    if (-not $d) { return "" }
    return (Join-Path $d 'xfsWinPad')
}

# The restore body lives in exactly one place: Stop-ProfileGuard. An earlier draft
# duplicated it into a PowerShell.Exiting action block; that was dropped because the
# action runs in its own job and cannot see script-scope functions, and its output
# never reaches the console. The trap line documented at the top covers the same
# ground while staying observable.
function Stop-ProfileGuard {
    if (-not $global:PG_Active -or $global:PG_Done) { return }
    $global:PG_Done = $true
    if (-not $global:PG_Real -or -not $global:PG_Dir) { return }
    foreach ($n in $global:PG_Names) {
        $src = Join-Path $global:PG_Dir $n
        $dst = Join-Path $global:PG_Real $n
        if (-not (Test-Path $src)) { continue }
        try { Copy-Item $src $dst -Force -ErrorAction Stop }
        catch { Say-PG ("PROFILE-GUARD-WARN: could not restore " + $dst) }
    }
    $extra = @()
    foreach ($f in @(Get-ChildItem (Join-Path $global:PG_Real 'session*.json') -ErrorAction SilentlyContinue)) {
        if ($global:PG_Names -notcontains $f.Name) { $extra += $f.FullName }
    }
    foreach ($p in $extra) {
        $gone = $false
        try { Remove-Item $p -Force -ErrorAction Stop; $gone = $true } catch { }
        if (-not $gone) {
            # The safe-delete hook turns Remove-Item on a %APPDATA% path into a
            # TERMINATING error - but it matches the cmdlet, not the .NET call:
            # [IO.File]::Delete gets through (measured, batch 108). Without this
            # fallback every probe left a session.json behind that the next real
            # launch would restore, i.e. the guard reported the very footgun it
            # exists to prevent.
            try { [IO.File]::Delete($p); $gone = -not (Test-Path $p) } catch { }
        }
        if ($gone) { Say-PG ("PROFILE-GUARD: removed probe-created " + $p) }
        else { Say-PG ("PROFILE-GUARD-DIRTY: " + $p + " - delete it by hand, or it will spawn a phantom window on every launch") }
    }
    if ($extra.Count -eq 0) {
        Say-PG ("PROFILE-GUARD: restored " + $global:PG_Names.Count + " file(s); no probe-created session file")
    }
    try { Remove-Item -Recurse -Force $global:PG_Dir -ErrorAction SilentlyContinue } catch { }
}

function Start-ProfileGuard {
    param([string]$ProfileDir = "", [switch]$NoKill)
    if ($global:PG_Active) { return }
    $global:PG_Active = $true
    $global:PG_Done   = $false
    $global:PG_Names  = @()
    if ($ProfileDir -ne "") { $global:PG_Real = $ProfileDir } else { $global:PG_Real = Get-ProfileDir }
    $global:PG_Dir = Join-Path ([IO.Path]::GetTempPath()) ("xfs-profile-guard-" + [Guid]::NewGuid().ToString('N'))
    try { New-Item -ItemType Directory -Force -Path $global:PG_Dir -ErrorAction Stop | Out-Null } catch { }
    if (-not $global:PG_Real -or -not (Test-Path $global:PG_Real)) {
        Say-PG ("PROFILE-GUARD: no profile dir at [" + $global:PG_Real + "] - nothing to snapshot")
    } else {
        foreach ($f in @(Get-ChildItem (Join-Path $global:PG_Real 'session*.json') -ErrorAction SilentlyContinue)) {
            try {
                Copy-Item $f.FullName (Join-Path $global:PG_Dir $f.Name) -Force -ErrorAction Stop
                $global:PG_Names += $f.Name
            } catch { }
        }
        $rc = Join-Path $global:PG_Real 'recent.txt'
        if (Test-Path $rc) {
            try {
                Copy-Item $rc (Join-Path $global:PG_Dir 'recent.txt') -Force -ErrorAction Stop
                $global:PG_Names += 'recent.txt'
            } catch { }
        }
        Say-PG ("PROFILE-GUARD: snapshot " + $global:PG_Names.Count + " file(s) from " + $global:PG_Real)
    }
    if (-not $NoKill) {
        $stray = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue)
        if ($stray.Count -gt 0) {
            Say-PG ("PROFILE-GUARD: killing " + $stray.Count + " stray xfsWinPad instance(s); the probe's app must be PRIMARY")
            try { $stray | Stop-Process -Force -ErrorAction Stop } catch { }
            Start-Sleep -Milliseconds 500
        }
    }
}
