param(
    [Parameter(Mandatory=$true)][ValidateSet('normal', 'throw', 'nc')][string]$Case
)
# _profile-guard-selftest.ps1 - prove scripts/_profile-guard.ps1 actually restores.
#
# WHY THIS EXISTS
#   A guard that prints "restored 3 file(s)" while doing nothing looks EXACTLY like a
#   guard that worked. The only way to tell them apart is to run it against a profile
#   you control, mutate that profile the way a probe does, and assert the mutation is
#   gone - plus a negative control where the restore is deliberately disabled.
#
# HOW TO RUN (one invocation per case - `exit` inside a called .ps1 ends the CALLER's
# session, so they cannot be chained with `;` in a single command)
#   & scripts\_profile-guard-selftest.ps1 -Case normal   # restore on the normal path
#   & scripts\_profile-guard-selftest.ps1 -Case throw    # restore via the script-scope trap
#   & scripts\_profile-guard-selftest.ps1 -Case nc       # NEGATIVE CONTROL
#
#   -Case nc rewrites the guard with its restore line removed and asserts the mutation
#   SURVIVES. If it does not survive, the other two cases are not measuring the restore
#   at all - they would pass even with the restore gone.
#
# The fake profile lives under %TEMP%. The real %APPDATA% profile is never part of any
# case - that is the whole point of the guard, so testing it must not need it.
#
# Transcript goes to %TEMP% as xfs-profile-guard-selftest-<case>.txt (the PowerShell
# tool does not return stdout; the scratch dir is used rather than a repo path so that
# this file's comments stay free of any private file name). Do NOT pre-delete it: this
# sandbox's safe-delete hook turns that Remove-Item into a terminating error, the script
# would die before writing, and the PREVIOUS run's transcript would read as a pass.
# Truncate by writing without -Append on the first call instead.
#
# ASCII only (PowerShell 5.1 reads a BOM-less .ps1 as ANSI).

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path ([IO.Path]::GetTempPath()) ("xfs-profile-guard-selftest-" + $Case + ".txt")
$script:n = 0
function T([string]$s) {
    if ($script:n -eq 0) { $s | Out-File -FilePath $out -Encoding UTF8 }
    else { $s | Out-File -FilePath $out -Append -Encoding UTF8 }
    $script:n++
    Write-Output $s
}
$fail = 0
function Check([string]$what, [bool]$ok) {
    if ($ok) { T ("  ok   " + $what) } else { T ("  FAIL " + $what); $script:fail++ }
}

# Assertions live in a function because the -Case throw path reaches them from inside
# the trap: a script-scope trap on a `throw` runs its body and then still ends the
# script, so the code after the throwing statement is never reached.
function Assert-All {
    $session = (Get-Content (Join-Path $p 'session.json') -Raw).Trim()
    $slot999 = (Get-Content (Join-Path $p 'session-999.json') -Raw).Trim()
    $recent  = (Get-Content (Join-Path $p 'recent.txt') -Raw).Trim()
    $phantom = Test-Path (Join-Path $p 'session-1234.json')
    T ("after:  session.json = " + $session)
    if ($Case -eq 'nc') {
        Check 'NEGATIVE CONTROL: the mutation SURVIVES (restore really is disabled)' ($session -eq '{"entries":[{"path":"PROBE-MUTATED"}]}')
        Check 'NEGATIVE CONTROL: nothing else was touched either' ($slot999 -eq '{"entries":[{"path":"REAL-SLOT"}]}')
    } else {
        Check 'session.json was restored'          ($session -eq '{"entries":[{"path":"REAL-ORIGINAL"}]}')
        Check 'the pre-existing slot was restored' ($slot999 -eq '{"entries":[{"path":"REAL-SLOT"}]}')
        Check 'recent.txt was restored'            ($recent  -eq 'REAL-RECENT')
        Check 'the probe-created slot is gone'     (-not $phantom)
    }
    if ($fail -eq 0) {
        T ("VERDICT: PROFILE-GUARD-SELFTEST-PASS (" + $Case + ")")
        exit 0
    }
    T ("VERDICT: PROFILE-GUARD-SELFTEST-FAIL (" + $Case + ", " + $fail + " check(s))")
    exit 1
}

# ---- fake profile ------------------------------------------------------------
$p = Join-Path ([IO.Path]::GetTempPath()) ("xfs-guard-selftest-" + $Case)
if (Test-Path $p) { Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $p | Out-Null
Set-Content -Path (Join-Path $p 'session.json')     -Value '{"entries":[{"path":"REAL-ORIGINAL"}]}'
Set-Content -Path (Join-Path $p 'session-999.json') -Value '{"entries":[{"path":"REAL-SLOT"}]}'
Set-Content -Path (Join-Path $p 'recent.txt')       -Value 'REAL-RECENT'

$guard = Join-Path $PSScriptRoot '_profile-guard.ps1'
if ($Case -eq 'nc') {
    $src = [IO.File]::ReadAllText($guard)
    $needle = 'Copy-Item $src $dst -Force -ErrorAction Stop'
    if (($src -split [regex]::Escape($needle)).Count -ne 2) {
        T 'SELFTEST-BROKEN: the restore line is not found exactly once in the guard'
        T ("  looked for: " + $needle)
        exit 2
    }
    $guard = Join-Path $p '_profile-guard-nc.ps1'
    # Replace the copy with a no-op that keeps the braces balanced. Do NOT paste a
    # trailing "# comment" here: the guard writes `try { Copy-Item ... }` on ONE line,
    # so a comment would swallow the closing brace and the neutered guard would fail to
    # parse - which the trap then reports as a mysterious error, not as "NC is broken".
    [IO.File]::WriteAllText($guard, $src.Replace($needle, '[void]0'))
}

T ("case       = " + $Case)
T ("guard      = " + $guard)
T ("fake profile = " + $p)
T ("before: session.json = " + (Get-Content (Join-Path $p 'session.json') -Raw).Trim())

# ---- run ---------------------------------------------------------------------
. $guard
(Start-ProfileGuard -ProfileDir $p -NoKill) | ForEach-Object { T ("guard: " + $_) }
trap {
    T ("TRAP-CAUGHT: " + $_.Exception.Message)
    (Stop-ProfileGuard) | ForEach-Object { T ("guard: " + $_) }
    Assert-All
}

# What a probe does to the profile: rewrite the session, and add a slot of its own.
Set-Content -Path (Join-Path $p 'session.json')      -Value '{"entries":[{"path":"PROBE-MUTATED"}]}'
Set-Content -Path (Join-Path $p 'session-1234.json') -Value '{"entries":[{"path":"PHANTOM"}]}'
T 'probe body ran'

if ($Case -eq 'throw') { throw 'deliberate-boom' }
(Stop-ProfileGuard) | ForEach-Object { T ("guard: " + $_) }

# ---- assert ------------------------------------------------------------------
Assert-All
