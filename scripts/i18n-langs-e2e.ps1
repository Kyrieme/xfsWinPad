# i18n-langs-e2e.ps1 - batch 40 e2e: verify ja / zh-TW / ko UI dictionaries.
#
# For each language: patch uiLang in %APPDATA%\xfsWinPad\settings.json,
# launch the exe with --new, read the top-level File menu title via Win32
# GetMenuStringW, compare against the expected localized string, close the app.
# settings.json + session.json are backed up and restored at the end.
# Script is pure ASCII; expected CJK strings are built from [char] codes.
param(
    [Parameter(Mandatory=$true)][string]$ExePath
)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class I18 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
[DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
[DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr m, uint item, StringBuilder s, int n, uint flags);
[DllImport("user32.dll")] public static extern bool SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
public static string SubItem(IntPtr menu, int i) {
  var sb = new StringBuilder(256);
  GetMenuStringW(menu, (uint)i, sb, 256, MF_BYPOSITION);
  return sb.ToString();
}
    public static IntPtr H = IntPtr.Zero;
    static bool Cb(IntPtr h, IntPtr l) {
        uint p; GetWindowThreadProcessId(h, out p);
        if (p == (uint)l) {
            var c = new StringBuilder(128); GetClassNameW(h, c, 128);
            if (c.ToString() == "xfsWinPadMainWindow") { H = h; return false; }
        }
        return true;
    }
    // FindWindowW proved unreliable for this window in this environment,
    // so locate the frame by enumerating top-level windows of the app PID.
    public static IntPtr FindByPid(int pid) { H = IntPtr.Zero; EnumWindows(Cb, (IntPtr)pid); return H; }
    public const uint MF_BYPOSITION = 0x400;
    public static string TopItem(IntPtr menu, int i) {
        var sb = new StringBuilder(256);
        GetMenuStringW(menu, (uint)i, sb, 256, MF_BYPOSITION);
        return sb.ToString();
    }
}
"@

function Fail([string]$m) {
    Write-Output "FAIL $m"
    Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Backups
    exit 1
}

function Wait-True([scriptblock]$cond, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

$exe = (Resolve-Path $ExePath).Path
$appData = Join-Path $env:APPDATA 'xfsWinPad'
$settings = Join-Path $appData 'settings.json'
$session  = Join-Path $appData 'session.json'
$sbak = Join-Path $env:TEMP 'xfs_i18n_settings.bak'
$ybak = Join-Path $env:TEMP 'xfs_i18n_session.bak'

function Restore-Backups {
    if (Test-Path $sbak) { Copy-Item $sbak $settings -Force }
    if (Test-Path $ybak) { Copy-Item $ybak $session -Force }
    elseif (Test-Path $session) { Remove-Item $session -Force }
}

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if (-not (Test-Path $settings)) { Fail "settings.json not found at $settings" }
Copy-Item $settings $sbak -Force
if (Test-Path $session) { Copy-Item $session $ybak -Force }

# expected File-menu titles (top-level item): File(&F)
$expect = @{
    'ja'    = ([string][char]0x30D5) + ([char]0x30A1) + ([char]0x30A4) + ([char]0x30EB) + '(&F)'
    'zh-TW' = ([string][char]0x6A94) + ([char]0x6848) + '(&F)'
    'ko'    = ([string][char]0xD30C) + ([char]0xC77C) + '(&F)'
}
$baseline = ([string][char]0x6587) + ([char]0x4EF6) + '(&F)'   # zh-CN File(&F)

$enc = [Text.Encoding]::UTF8
$allPass = $true

foreach ($lang in @('ja','zh-TW','ko')) {
    $raw = [IO.File]::ReadAllText($settings, $enc)
    $new = [regex]::Replace($raw, '"uiLang"\s*:\s*"[^"]*"', ('"uiLang": "' + $lang + '"'))
    $m = [regex]::Match($new, '"uiLang"\s*:\s*"([^"]*)"')
    if (-not $m.Success -or $m.Groups[1].Value -ne $lang) { Fail "uiLang not patched for $lang" }
    [IO.File]::WriteAllText($settings, $new, $enc)

    Start-Process -FilePath $exe -ArgumentList '--new'
    $script:hwnd = [IntPtr]::Zero
    if (-not (Wait-True {
            $p = Get-Process xfsWinPad -ErrorAction SilentlyContinue
            if ($p) { $script:hwnd = [I18]::FindByPid($p[0].Id) }
            $script:hwnd -ne [IntPtr]::Zero
        } 10000)) {
        Fail "main window not found for $lang"
    }
    $hwnd = $script:hwnd
    Start-Sleep -Milliseconds 800   # menu built after window creation

    $menu = [I18]::GetMenu($hwnd)
    if ($menu -eq [IntPtr]::Zero) { Fail "no menu handle for $lang" }
    $count = [I18]::GetMenuItemCount($menu)
    $found = $false
    $items = @()
    for ($i = 0; $i -lt $count; $i++) {
        $t = [I18]::TopItem($menu, $i)
        $items += $t
        if ($t -ceq $expect[$lang]) { $found = $true }
    }
    if (-not $found) {
        Write-Output ("LANG {0} menu titles: {1}" -f $lang, ($items -join ' | '))
        Write-Output ("LANG {0} expected: {1}" -f $lang, $expect[$lang])
        Fail "menu title mismatch for $lang"
    }
    if ($items -contains $baseline) { Fail "zh-CN baseline still present for $lang" }

    # batch 65: Encoding > Convert to / Reload as must be fully localized
    # and match resources\lang\<lang>.json (enc.* keys, 11 items each).
    $langPath = Join-Path (Split-Path $PSScriptRoot -Parent) "resources\lang\$lang.json"
    $langJson = [IO.File]::ReadAllText($langPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $encTitle = $langJson.'menu.encoding'
    $encIdx = -1
    for ($i = 0; $i -lt $count; $i++) {
        if (([I18]::TopItem($menu, $i)) -ceq $encTitle) { $encIdx = $i; break }
    }
    if ($encIdx -lt 0) { Fail "no Encoding top menu for $lang" }
    $encMenu = [I18]::GetSubMenu($menu, $encIdx)
    if ($encMenu -eq [IntPtr]::Zero) { Fail "no Encoding popup for $lang" }
    $convPop = [I18]::GetSubMenu($encMenu, 0)
    $relasPop = [I18]::GetSubMenu($encMenu, 1)
    if ([I18]::GetMenuItemCount($convPop) -ne 11) { Fail "conv item count for $lang" }
    if ([I18]::GetMenuItemCount($relasPop) -ne 11) { Fail "relas item count for $lang" }
    $encKeys = @('enc.utf8','enc.utf8bom','enc.utf16le','enc.utf16be','enc.ansi',
                 'enc.utf32le','enc.utf32be','enc.big5','enc.shiftjis','enc.koi8r','enc.iso88591')
    for ($k = 0; $k -lt 11; $k++) {
        $want = [string]$langJson.($encKeys[$k])
        $t1 = [I18]::SubItem($convPop, $k)
        $t2 = [I18]::SubItem($relasPop, $k)
        if ($t1 -cne $want) { Fail ("conv[{0}] '{1}' != '{2}' for $lang" -f $k, $t1, $want) }
        if ($t2 -cne $want) { Fail ("relas[{0}] '{1}' != '{2}' for $lang" -f $k, $t2, $want) }
    }
    Write-Output "ENC-OK $lang encoding menus localized"
    Write-Output "PASS $lang File menu = $($expect[$lang])"

    [I18]::SendMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    if (-not (Wait-True { -not (Get-Process xfsWinPad -ErrorAction SilentlyContinue) } 10000)) {
        Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Milliseconds 300   # let the app finish rewriting config files
}

Restore-Backups
# the user's original uiLang was zh-CN; make sure we leave it that way
$raw = [IO.File]::ReadAllText($settings, $enc)
$new = [regex]::Replace($raw, '"uiLang"\s*:\s*"[^"]*"', '"uiLang": "zh-CN"')
[IO.File]::WriteAllText($settings, $new, $enc)
Write-Output 'ALL LANGS PASS'
exit 0
