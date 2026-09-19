# Assembles dist\payload\ from build outputs, builds xtaclean.exe,
# generates dist\setup.rsp (relative paths) and compiles dist\setup.exe.
# Works on local machine (PS 5.1) and GitHub Actions runners (pwsh 7).
# Run from repo root:  powershell -File scripts\make-payload.ps1 [-SkipSetup]
param(
    [switch]$SkipSetup
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$bin  = Join-Path $root 'build\bin\Release'
$pay  = Join-Path $root 'dist\payload'
$csc  = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

if (-not (Test-Path "$bin\xfsWinPad.exe")) { throw "build output missing: $bin\xfsWinPad.exe (run cmake build first)" }
New-Item -ItemType Directory -Force -Path $pay | Out-Null

# --- core binaries ---
foreach ($f in 'xfsWinPad.exe','xfsWinPadPluginHost.exe','Scintilla.dll','Lexilla.dll') {
    Copy-Item "$bin\$f" $pay -Force
}

# --- VC runtime ---
# Locate the x64 Microsoft.VC*.CRT redist directory, two ways in order:
#   1) vswhere  -- the documented lookup; this is the path CI takes.
#   2) a fallback scan of the conventional "<drive>\Program Files\Microsoft
#      Visual Studio" roots.
# The fallback exists because vswhere can come back *empty* with the toolset
# fully installed: observed with vswhere 3.1.7 against VS 18 2026, where
# `-latest -products * -property installationPath` (and even `-help`) prints
# nothing, so step 1 alone aborts the whole payload step.
#
# Drive enumeration goes through Get-PSDrive (existing drives only) instead of
# probing letters directly: Get-ChildItem -Path on a nonexistent drive breaks
# FileSystem provider dynamic-parameter binding, so -Directory "cannot be
# found" there.
#
# ProgramFiles(x86) is guarded for emptiness -- if it is unset the naive
# "${env:ProgramFiles(x86)}\..." collapses to "\Microsoft Visual Studio\...".
$redistDir = $null

$vswhere = $null
foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
    if (-not $pf) { continue }
    $cand = Join-Path $pf 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $cand) { $vswhere = $cand; break }
}
if ($vswhere) {
    foreach ($vs in (& $vswhere -latest -products * -property installationPath)) {
        $glob = Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT'
        $cand = Get-ChildItem -Path $glob -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($cand) { $redistDir = $cand; break }
    }
}

if (-not $redistDir) {
    # Layout is <root>\Microsoft Visual Studio\<version>\<edition>\VC\Redist\...
    # so descend two levels (version, then edition) before globbing.
    $roots = @(Get-PSDrive -PSProvider FileSystem |
        ForEach-Object { Join-Path $_.Root 'Program Files\Microsoft Visual Studio' })
    $installs = Get-ChildItem -Path $roots -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue }
    $redistDir = $installs |
        ForEach-Object { Join-Path $_.FullName 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' } |
        ForEach-Object { Get-ChildItem -Path $_ -Directory -ErrorAction SilentlyContinue } |
        Sort-Object FullName -Descending | Select-Object -First 1
}

if (-not $redistDir) { throw 'VC redist (Microsoft.VC*.CRT) not found (vswhere + Program Files scan)' }
foreach ($f in 'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll') {
    Copy-Item (Join-Path $redistDir.FullName $f) $pay -Force
}

# --- language packs (payload root + lang subdir, mirroring installer layout) ---
$langSrc = "$bin\lang"
if (Test-Path $langSrc) {
    New-Item -ItemType Directory -Force -Path "$pay\lang" | Out-Null
    Copy-Item "$langSrc\*.json" $pay -Force
    Copy-Item "$langSrc\*.json" "$pay\lang" -Force
}

# --- docs + icon ---
Copy-Item "$root\resources\app.ico" $pay -Force
Copy-Item "$root\README.md" $pay -Force
Copy-Item "$root\THIRD_PARTY_NOTICES.md" $pay -Force

# --- xtaclean.exe (uninstall helper) ---
& $csc /nologo /target:exe /out:"$pay\xtaclean.exe" "$root\installer\XtaClean.cs"
if ($LASTEXITCODE -ne 0) { throw 'xtaclean build failed' }

# --- setup.rsp with relative paths ---
$langFiles = Get-ChildItem "$pay\*.json" | ForEach-Object { $_.Name }
$lines = @('/nologo', '/target:winexe', '/out:dist\setup.exe',
    '/win32icon:dist\payload\app.ico')
foreach ($r in @('app.ico') + $langFiles + @('Lexilla.dll','msvcp140.dll','README.md',
        'Scintilla.dll','THIRD_PARTY_NOTICES.md','vcruntime140.dll','vcruntime140_1.dll',
        'xfsWinPad.exe','xfsWinPadPluginHost.exe','xtaclean.exe')) {
    $lines += "/resource:`"dist\payload\$r`",$r"
}
$lines += @('/r:System.IO.Compression.FileSystem.dll', '/r:System.IO.Compression.dll',
    'installer\Setup.cs')
Set-Content -Path "$root\dist\setup.rsp" -Value $lines -Encoding ASCII

if (-not $SkipSetup) {
    Push-Location $root
    try {
        & $csc "@dist\setup.rsp"
        if ($LASTEXITCODE -ne 0) { throw 'setup.exe build failed' }
        Copy-Item "$root\dist\setup.exe" "$root\dist\xfsWinPad-setup.exe" -Force
        Compress-Archive -Path "$pay\*" -DestinationPath "$root\dist\xfsWinPad-portable.zip" -Force
        Write-Host 'dist\setup.exe + dist\xfsWinPad-setup.exe + portable zip rebuilt'
    } finally { Pop-Location }
} else {
    Write-Host 'payload assembled (setup.exe skipped)'
}
