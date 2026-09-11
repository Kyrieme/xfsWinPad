# theme-test.ps1 - verify dark/light theme switching, persistence, and
# syntax coloring presence on a Python file in dark mode.
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [string]$OutDir = "out"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sp = "$env:APPDATA\xfsWinPad\settings.json"

Add-Type @"
using System;using System.Runtime.InteropServices;
public static class TW {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[TW]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Shot([IntPtr]$hwnd, [string]$png) {
    Start-Sleep -Milliseconds 400
    [TW]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 250
    $r = New-Object TW+RECT
    [TW]::GetWindowRect($hwnd,[ref]$r) | Out-Null
    $w=$r.Right-$r.Left; $h=$r.Bottom-$r.Top
    $bmp = New-Object System.Drawing.Bitmap($w,$h)
    $g=[System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left,$r.Top,0,0,(New-Object System.Drawing.Size($w,$h)))
    $bmp.Save($png,[System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

function EditorMean([string]$png) {
    $bmp = New-Object System.Drawing.Bitmap($png)
    $n=0;$sum=0
    for($y=[int]($bmp.Height*0.35); $y -lt [int]($bmp.Height*0.65); $y+=3){
        for($x=[int]($bmp.Width*0.3); $x -lt [int]($bmp.Width*0.7); $x+=3){
            $c=$bmp.GetPixel($x,$y); $sum += [int](0.299*$c.R+0.587*$c.G+0.114*$c.B); $n++
        }
    }
    $bmp.Dispose()
    return [math]::Round($sum/[math]::Max(1,$n),1)
}

$results=@()
function Pass([string]$m){$script:results+="PASS $m";Write-Output "PASS $m"}
function Fail([string]$m){$script:results+="FAIL $m";Write-Output "FAIL $m"}

# --- python sample with comments/strings/keywords ------------------------------
$py = Join-Path $env:TEMP 'xfs_theme_sample.py'
@'
# comment line one
# another comment
def compute(items):
    total = 0
    for it in items:
        total += it * 2
    return total

print("hello string", compute([1,2,3]))
'@ | Set-Content $py -Encoding UTF8

'{"theme":"dark","hasWindow":false}' | Set-Content $sp -Encoding UTF8

$p = Start-Process -FilePath $ExePath -ArgumentList "`"$py`"" -PassThru
Start-Sleep -Milliseconds 2200
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Fail "launch"; exit 1 }

Shot $hwnd (Join-Path (Get-Location) "$OutDir\theme_dark.png")
$meanDark = EditorMean (Join-Path (Get-Location) "$OutDir\theme_dark.png")
"editor mean dark = $meanDark"
if ($meanDark -lt 90) { Pass "dark-editor-bg" } else { Fail "dark-editor-bg" }

# syntax colors via hue classes (ClearType tints exact RGBs):
#   greenish = comment #6A9955, bluish = keyword #569CD6, orange = string #CE9178
$bmp = New-Object System.Drawing.Bitmap((Join-Path (Get-Location) "$OutDir\theme_dark.png"))
$commentPx=0; $kwBlue=0; $strOrange=0
for($y=110; $y -lt [int]($bmp.Height*0.85); ++$y){
    for($x=70; $x -lt [int]($bmp.Width*0.75); ++$x){
        $c=$bmp.GetPixel($x,$y)
        $R=[int]$c.R; $G=[int]$c.G; $B=[int]$c.B
        $lum=[int](0.299*$R+0.587*$G+0.114*$B)
        if($lum -lt 60) { continue }          # skip background
        if(($G -gt ($R+12)) -and ($G -gt ($B+12)) -and ($G -gt 90)){$commentPx++}
        elseif(($B -gt ($R+35)) -and ($B -gt ($G+15)) -and ($B -gt 120)){$kwBlue++}
        elseif(($R -gt ($G+35)) -and ($G -gt ($B+5)) -and ($R -gt 130)){$strOrange++}
    }
}
$bmp.Dispose()
"syntax px: comment(green)=$commentPx keyword(blue)=$kwBlue string(orange)=$strOrange"
if ($commentPx -ge 40) { Pass "syntax-comment-colored" } else { Fail "syntax-comment-colored" }
if ($kwBlue -ge 20) { Pass "syntax-keyword-colored" } else { Fail "syntax-keyword-colored" }
if ($strOrange -ge 20) { Pass "syntax-string-colored" } else { Fail "syntax-string-colored" }

# switch to light via menu command
$WM_COMMAND=0x0111
[TW]::PostMessageW($hwnd,$WM_COMMAND,[IntPtr]460,[IntPtr]::Zero)|Out-Null   # ThemeLight
Shot $hwnd (Join-Path (Get-Location) "$OutDir\theme_light.png")
$meanLight = EditorMean (Join-Path (Get-Location) "$OutDir\theme_light.png")
"editor mean light = $meanLight"
if ($meanLight -gt 200) { Pass "light-switch-live" } else { Fail "light-switch-live" }

[TW]::PostMessageW($hwnd,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force; Fail "clean-exit" } else { Pass "clean-exit" }

$saved = Get-Content $sp -Raw
if ($saved -match '"theme":\s*"light"') { Pass "theme-persisted" } else { Fail "theme-persisted ($($saved.Substring(0,[Math]::Min(160,$saved.Length))))" }
Remove-Item $sp -Force -ErrorAction SilentlyContinue
Remove-Item $py -Force -ErrorAction SilentlyContinue

$fails = ($results|Where-Object {$_ -like 'FAIL*'}).Count
Write-Output ("SUMMARY pass={0} fail={1}" -f (($results|Where-Object {$_ -like 'PASS*'}).Count), $fails)
exit ($(if($fails -gt 0){1}else{0}))
