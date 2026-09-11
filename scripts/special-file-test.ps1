# special-file-test.ps1 - CLI dispatch: .xfm loads as macro, theme json gets
# imported (copied to themes\), plain .json stays a normal document.
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
$failures = 0
function Pass($n) { Write-Host "PASS $n" -ForegroundColor Green }
function Fail($n) { Write-Host "FAIL $n" -ForegroundColor Red; $script:failures++ }

$log = Join-Path $env:LOCALAPPDATA 'xfsWinPad\logs\xfsWinPad.log'
$themesDir = Join-Path $env:APPDATA 'xfsWinPad\themes'

# --- fixture: a valid .xfm (chars 'h' 'i') and a theme json ---
$tmp = Join-Path $env:TEMP 'xfs_special'
New-Item -ItemType Directory -Force $tmp | Out-Null
$xfm = Join-Path $tmp 'hello.xfm'
"C 104`nC 105`n" | Out-File $xfm -Encoding ascii
$theme = Join-Path $tmp 'MyTheme.json'
@'
{
  "name": "MyTheme",
  "editorBg": "#201010",
  "editorFg": "#F0E0E0"
}
'@ | Out-File $theme -Encoding ascii
$plain = Join-Path $tmp 'plain.json'
'{ "hello": "world" }' | Out-File $plain -Encoding ascii

# clean themes dir target
Remove-Item (Join-Path $themesDir 'MyTheme.json') -Force -ErrorAction SilentlyContinue

# --- 1) .xfm: launched via CLI -> macro loaded, NOT opened as a document ---
$p = Start-Process -FilePath $ExePath -ArgumentList "--new `"$xfm`"" -PassThru
Start-Sleep -Seconds 3
$tail = Get-Content $log -Tail 20
$m = $tail | Select-String -Pattern "Macro loaded" | Select-Object -Last 1
"macro log: $m"
if ("$m" -match "Macro loaded: .*hello\.xfm events=2") { Pass "xfm-macro-loaded" }
else { Fail "xfm-macro-loaded" }
if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 400

# --- 2) theme json: launched via CLI -> imported into themes\, not a doc ---
$p2 = Start-Process -FilePath $ExePath -ArgumentList "--new `"$theme`"" -PassThru
Start-Sleep -Seconds 3
$dst = Join-Path $themesDir 'MyTheme.json'
if (Test-Path $dst) { Pass "theme-imported-to-dir" } else { Fail "theme-imported-to-dir" }
$tail2 = Get-Content $log -Tail 15
$t = $tail2 | Select-String -Pattern "Theme imported" | Select-Object -Last 1
"theme log: $t"
if ("$t" -match "Theme imported") { Pass "theme-import-log" } else { Fail "theme-import-log" }
# theme import pops a MessageBox - dismiss
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
Start-Sleep -Milliseconds 400
if (-not $p2.HasExited) { $p2.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p2.HasExited) { Stop-Process -Id $p2.Id -Force }
Start-Sleep -Milliseconds 400

# --- 3) plain json: stays a normal document (no import) ---
$p3 = Start-Process -FilePath $ExePath -ArgumentList "--new `"$plain`"" -PassThru
Start-Sleep -Seconds 3
$tail3 = Get-Content $log -Tail 12
$o = $tail3 | Select-String -Pattern "Opened .*plain\.json" | Select-Object -Last 1
"plain log: $o"
if ("$o" -match "Opened .*plain\.json") { Pass "plain-json-stays-document" }
else { Fail "plain-json-stays-document" }
if (-not $p3.HasExited) { $p3.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 600 }
if (-not $p3.HasExited) { Stop-Process -Id $p3.Id -Force }

# cleanup
Remove-Item (Join-Path $themesDir 'MyTheme.json') -Force -ErrorAction SilentlyContinue
"SUMMARY failures=$failures"
exit $(if ($failures -gt 0) { 1 } else { 0 })
