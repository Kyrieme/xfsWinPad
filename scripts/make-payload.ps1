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
        # 打包 payload 时**剔掉** debug.log / xfsWinPad.log。
        #
        # 【为什么需要】程序按设计会在**自己 exe 同目录**写 debug.log（现场排查用，
        #   日志跟着工程走），而 dist\payload 里就放着 xfsWinPad.exe ⇒ 只要有人在这个
        #   目录里跑一次打包好的程序，payload 里就会多出 debug.log。原来这里是
        #   `Compress-Archive -Path "$pay\*"` 通配 ⇒ 它会被**静默**打进发布包。
        #   实测（批次 103 之后）：本地那份 zip 里就混进了 debug.log，而它含本机路径
        #   （用户目录、开发用的临时工作目录）—— 正是"注释/字符串不得指向本机路径"
        #   那条约束要防的泄露，只是走的是构建产物这条路，源码守卫看不见。
        #   CI 不受影响（干净工作区 + 从不跑 GUI），但本地手工打包会中招。
        #
        # 【为什么显式列名字而不是 `*.log`】只可能是这两个；用通配会把将来某个
        #   "其实应该发出去"的 .log 一并吞掉，属于误伤。
        # 【为什么不用 Remove-Item】沙箱的 safe-delete 钩子会把删除变成**终止性错误**，
        #   反而让整个打包失败（实测过），所以只从入包清单里排除、不动磁盘。
        $strayLogs = @('debug.log', 'xfsWinPad.log')
        $zipItems = @(Get-ChildItem -LiteralPath $pay |
                      Where-Object { $strayLogs -notcontains $_.Name })
        if ($zipItems.Count -eq 0) { throw "payload is empty: $pay" }
        Compress-Archive -Path ($zipItems | ForEach-Object { $_.FullName }) `
                         -DestinationPath "$root\dist\xfsWinPad-portable.zip" -Force
        Write-Host 'dist\setup.exe + dist\xfsWinPad-setup.exe + portable zip rebuilt'
    } finally { Pop-Location }
} else {
    Write-Host 'payload assembled (setup.exe skipped)'
}
