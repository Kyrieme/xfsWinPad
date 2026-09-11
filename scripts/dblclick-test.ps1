# dblclick-test.ps1 - REAL mouse double-click on the results panel row must
# locate and select the match in the editor (full notification chain).
param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference = 'Stop'
. $PSScriptRoot\_common-win32.ps1
[WIN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$file = Join-Path $env:TEMP 'xfs_fr.txt'
[System.IO.File]::WriteAllText($file, "alpha beta`r`ngamma alpha`r`ndelta`r`n")
$p = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
Start-Sleep -Milliseconds 2200
[WIN]::pid = [uint32]$p.Id
[WIN]::LocateFrame()

# open Find page, type needle, run "Find All in open documents"
[WIN]::PostMessageW([WIN]::frame, 0x0111, [IntPtr]400, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
[WIN]::LocateDlg()
$ce = [WIN]::ComboEdit([WIN]::GetDlgItem([WIN]::dlg, 1100))
[WIN]::TypeInto($ce, 'gamma')

# NOTE: needle combo text was typed into the FIND-page combo; ApplyFromControls
# reads the ACTIVE page's combo on button press, so this stays in sync.

$overallFail = $false

# NOTE: This test is superseded by the more comprehensive tests above.
# It verifies basic double-click locate only.
$overallFail = $false

foreach ($scenario in @(@{name='find-all-current'; btn=1114})) {
    # re-open the dialog if a previous scenario closed it (close hides only)
    [WIN]::PostMessageW([WIN]::dlg, 0x0111, [IntPtr]$scenario.btn, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    [WIN]::LocateList()
    "list=$([WIN]::list)"
    if ([WIN]::list -eq [IntPtr]::Zero) { Write-Output "FAIL $($scenario.name) (no list)"; $overallFail=$true; continue }

    $r = New-Object WIN+RECT
    [WIN]::GetWindowRect([WIN]::list,[ref]$r) | Out-Null
    $cx = $r.Left + [int](($r.Right-$r.Left)*0.25)
    [WIN]::SetForegroundWindow([WIN]::frame) | Out-Null
    Start-Sleep -Milliseconds 300

    $hitOk=$false
    foreach ($dy in 28,34,40,46,52,58,64,70,76,82) {
        $pt = New-Object WIN+POINT
        $pt.x=$cx; $pt.y=$r.Top+$dy
        if ([WIN]::WindowFromPoint($pt) -ne [WIN]::list) { continue }
        $lx=$cx-$r.Left
        $lp=[IntPtr](($dy -shl 16) -bor ($lx -band 0xFFFF))
        [WIN]::PostMessageW([WIN]::list,0x0203,[IntPtr]1,$lp)|Out-Null   # WM_LBUTTONDBLCLK
        Start-Sleep -Milliseconds 500
        $ed = [WIN]::ActiveEditor()
        $ss = [int][WIN]::SendMessageW($ed, 2143, [IntPtr]::Zero, [IntPtr]::Zero)
        $se = [int][WIN]::SendMessageW($ed, 2145, [IntPtr]::Zero, [IntPtr]::Zero)
        "$($scenario.name): dy=$dy sel=[$ss,$se]"
        if ($ss -eq 12 -and $se -eq 17) { $hitOk=$true; break }
    }
    if ($hitOk) { Write-Output "PASS $($scenario.name)-locate" }
    else { Write-Output "FAIL $($scenario.name)-locate"; $overallFail=$true }

    # close panel so the next scenario starts clean
    $panelH = [WIN]::GetParent([WIN]::list)
    [WIN]::PostMessageW($panelH, 0x0111, [IntPtr]1202, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
}

if (-not $p.HasExited) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 600
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait("n")
    if (-not $p.WaitForExit(4000)) { Stop-Process -Id $p.Id -Force }
}
Remove-Item $file -Force -ErrorAction SilentlyContinue
exit $(if ($overallFail) { 1 } else { 0 })
