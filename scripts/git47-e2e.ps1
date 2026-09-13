# Batch 47 smoke: directory state aggregation must not crash custom draw.
$ErrorActionPreference = 'Stop'
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$exe = 'D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe'
$rand = Get-Random -Minimum 100 -Maximum 999
$branch = "e2e47z$rand"
$repo = Join-Path $env:TEMP "xfsGitTest47"
if (Test-Path $repo) { Remove-Item -Recurse -Force $repo }
New-Item -ItemType Directory -Path (Join-Path $repo 'sub1\sub2') | Out-Null
Set-Content -Path (Join-Path $repo 'sub1\sub2\file.txt') -Value "v1`n" -NoNewline
Set-Content -Path (Join-Path $repo 'top.txt') -Value "t1`n" -NoNewline
& git -C $repo init -q
& git -c user.email=t@t -c user.name=t -C $repo add -A
& git -c user.email=t@t -c user.name=t -C $repo commit -qm init
if ($LASTEXITCODE -ne 0) { throw 'git init/commit failed' }
& git -C $repo checkout -qb $branch
# dirty deep file + deleted tracked file + untracked collapsed dir
Set-Content -Path (Join-Path $repo 'sub1\sub2\file.txt') -Value "v2`n" -NoNewline
Remove-Item -Force (Join-Path $repo 'top.txt')
New-Item -ItemType Directory -Path (Join-Path $repo 'sub3\nested') | Out-Null
Set-Content -Path (Join-Path $repo 'sub3\nested\u.txt') -Value "u`n" -NoNewline

$settings = Join-Path $env:APPDATA 'xfsWinPad\settings.json'
$bak = "$settings.bak47"
Copy-Item $settings $bak -Force
$txt = [IO.File]::ReadAllText($settings)
$txt = $txt -replace '("projectRoot"\s*:\s*)"[^"]*"', ('$1"' + ($repo -replace '\\','\\') + '"')
$txt = $txt -replace '("explorerVisible"\s*:\s*)false', '${1}true'
[IO.File]::WriteAllText($settings, $txt)

$log = Join-Path $env:LOCALAPPDATA 'xfsWinPad\logs\xfsWinPad.log'
$off = 0
if (Test-Path $log) { $off = (Get-Item $log).Length }

Start-Process -FilePath $exe
Start-Sleep -Seconds 10
$procs = @(Get-Process xfsWinPad -ErrorAction SilentlyContinue)
$alive = $procs.Count -gt 0

$tiny = $false
if ($alive) {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    $fs.Position = $off
    $sr = New-Object IO.StreamReader($fs)
    $tail = $sr.ReadToEnd()
    $sr.Close()
    $tiny = $tail -match "git: branch $branch"
    $procs | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

Copy-Item $bak $settings -Force
Remove-Item $bak -Force
Remove-Item -Recurse -Force $repo -ErrorAction SilentlyContinue

if (-not $alive) { Write-Output 'FAIL: process died'; exit 1 }
if (-not $tiny) { Write-Output 'FAIL: no git branch line in log'; exit 1 }
Write-Output 'PASS: git47 dir-aggregation smoke'
exit 0
