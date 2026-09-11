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

# --- VC runtime (search all common VS install drives) ---
$redistDirs = @()
foreach ($drive in 'C:\','D:\','E:\') {
    $redistDirs += Get-ChildItem -Path "$drive`Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue
}
$redistDir = $redistDirs | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redistDir) { throw 'VC redist (Microsoft.VC143.CRT) not found' }
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
