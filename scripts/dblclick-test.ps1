# dblclick-test.ps1 - REAL mouse double-click on a find-results row must locate
# and select the match in the editor (full notification chain).
#
# [why this now uses real mouse input - batch 101, measured on two controls]
#   This script used to send a SYNTHETIC WM_LBUTTONDBLCLK. The measured conclusion
#   is not "synthetic never works" but "unreliable, and it varies by control":
#     - find-results list (the target here): a synthetic DBLCLK DOES trigger the
#       jump (measured: dy=28 succeeded on the first try);
#     - the compile-output panel's list: in the SAME process, a synthetic DOWN/UP
#       does not even move the selection, while real SendInput works at once.
#   Both are SysListView32, so the difference is not that the message was posted
#   wrongly - you simply cannot rely on synthetic input. Real SendInput passed on
#   BOTH controls, and it is what the user actually does anyway.
#   _common-win32.ps1 already ships [WIN]::DoubleClick (a SendInput implementation);
#   this file used to bypass it and post its own synthetic message - it now uses it.
#
# Also corrects a stale record: an earlier local note blamed this script's failure on
#   "SendInput is swallowed by the automation host". That attribution does not hold -
#   real SendInput works on this machine (verified by craft-e2e.ps1 and by the
#   control comparison above).
#
# ASCII only. Not a style preference: without a BOM, PowerShell reads a .ps1 using the
# ANSI code page, so a UTF-8 comment can shift byte parity, swallow a line break, and
# then produce a *phantom* syntax error reported at an unrelated line. Keep bytes
# below 0x80. (This bit craft-e2e.ps1 during batch 101: "unexpected }" on a line that
# contained no brace at all.)
#
# Usage:
#   .\scripts\dblclick-test.ps1 -ExePath <xfsWinPad.exe>   # launch it ourselves
#   .\scripts\dblclick-test.ps1 -AttachPid <pid>           # attach to a running instance
# -AttachPid exists so this can run where Start-Process is unavailable (same as
# craft-e2e.ps1). Note that it does NOT open the fixture file for you (only the
# -ExePath path does), so the caller must have the target open it - the precondition
# check below exists precisely to catch that.
param(
    [string]$ExePath,
    [int]$AttachPid = 0
)
$ErrorActionPreference = 'Stop'
. $PSScriptRoot\_common-win32.ps1
[WIN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

# Control ids come from src/app/FindDialog.cpp rather than being hardcoded:
#   IDC_FIND_TEXT  = 1100  the find page's edit box
#   IDC_BTN_ALLCUR = 1114  find all in the CURRENT document
#   IDC_BTN_ALLOPEN= 1115  find all in all OPEN documents
# This case has a single document open, so 1114 is the right one (an earlier comment
# described it as 1115's meaning, which was wrong).
$IDC_FIND_TEXT  = 1100
$IDC_BTN_ALLCUR = 1114
$WM_COMMAND     = 0x0111
$CMD_FIND       = 400
# LVM_FIRST(0x1000) + 4. A value-returning message, so USER32 marshals it across
# processes and it is safe to read.
$LVM_GETITEMCOUNT = 0x1004

# Resolve the temp directory via GetTempPath() rather than $env:TEMP: the env var can
# be empty in some hosts, and then Join-Path throws while the error message never
# mentions TEMP, so it reads like a bug in this script. (Measured on this host:
# APPDATA is empty, TEMP is set - but there is no reason to bet on the next one.)
$file = Join-Path ([System.IO.Path]::GetTempPath()) 'xfs_fr.txt'
[System.IO.File]::WriteAllText($file, "alpha beta`r`ngamma alpha`r`ndelta`r`n")

$proc = $null
if ($AttachPid -gt 0) {
    [WIN]::pid = [uint32]$AttachPid
    Write-Output "[run] attaching to pid $AttachPid (caller owns the process)"
} else {
    if (-not $ExePath) { throw "-ExePath is required unless -AttachPid is given" }
    $proc = Start-Process -FilePath $ExePath -ArgumentList "`"$file`"" -PassThru
    Start-Sleep -Milliseconds 2200
    [WIN]::pid = [uint32]$proc.Id
}
[WIN]::LocateFrame()
if ([WIN]::frame -eq [IntPtr]::Zero) { throw "main window not found" }

$overallFail = $false
try {
    # Open the find panel -> type the needle -> find all in the current document.
    [WIN]::PostMessageW([WIN]::frame, [uint32]$WM_COMMAND, [IntPtr]$CMD_FIND, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 700
    [WIN]::LocateDlg()
    if ([WIN]::dlg -eq [IntPtr]::Zero) { throw "find dialog not found" }
    $ce = [WIN]::ComboEdit([WIN]::GetDlgItem([WIN]::dlg, $IDC_FIND_TEXT))
    [WIN]::ClearAndType($ce, 'gamma')
    Start-Sleep -Milliseconds 150
    [WIN]::PostMessageW([WIN]::dlg, [uint32]$WM_COMMAND, [IntPtr]$IDC_BTN_ALLCUR, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800

    [WIN]::LocateList()
    if ([WIN]::list -eq [IntPtr]::Zero) { throw "results list not found" }
    Write-Output "[run] list=$([WIN]::list)"

    # Precondition: the results list must actually hold a match. Zero means the active
    # document is not the text this case is about (typically: under -AttachPid the
    # caller forgot to open the fixture file). This must report "wrong document" and
    # never fall through to "FAIL dblclick-locate" - that looks like the product is
    # broken, when the truth is that the probe pointed at the wrong place.
    $rows = [int][WIN]::SendMessageW([WIN]::list, [uint32]$LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Output "[run] result rows=$rows"
    if ($rows -lt 1) {
        # Write-Output + exit rather than throw: a throw's text is lost when the output
        # is redirected through a pipeline (measured - only the lines written before it
        # survive), and this is the one line that most needs to be seen.
        Write-Output "FAIL precondition: the find-results list is empty - the active document"
        Write-Output "     does not contain the needle, so this run would measure the wrong text."
        Write-Output "     Under -AttachPid the caller must open the fixture file first:"
        Write-Output "       $file"
        Write-Output "DBLCLICK-TEST-PRECONDITION"
        exit 2
    }

    $r = New-Object WIN+RECT
    [WIN]::GetWindowRect([WIN]::list, [ref]$r) | Out-Null
    $cx = $r.Left + [int](($r.Right - $r.Left) * 0.25)

    # A real click lands on whatever is under the cursor, so the frame must be
    # foreground. Hard gate: refuse rather than click into someone else's window.
    [WIN]::SetForegroundWindow([WIN]::frame) | Out-Null
    Start-Sleep -Milliseconds 400
    if ([WIN]::GetForegroundWindow() -ne [WIN]::frame) {
        throw "frame is not foreground - refusing to send real clicks"
    }

    # Row height cannot be read back with a read-only message (LVM_GETITEMRECT wants a
    # pointer), so try a series of dy values: for each, confirm the point really lands
    # on the list, real-double-click it, then see whether the editor's selection became
    # "gamma" (0-based 12..17).
    $hitOk = $false
    foreach ($dy in 28,34,40,46,52,58,64,70,76,82) {
        $pt = New-Object WIN+POINT
        $pt.x = $cx; $pt.y = $r.Top + $dy
        if ([WIN]::WindowFromPoint($pt) -ne [WIN]::list) { continue }
        [WIN]::DoubleClick($pt.x, $pt.y)
        Start-Sleep -Milliseconds 500
        $ed = [WIN]::ActiveEditor()
        $ss = [int][WIN]::SendMessageW($ed, 2143, [IntPtr]::Zero, [IntPtr]::Zero)   # SCI_GETSELECTIONSTART
        $se = [int][WIN]::SendMessageW($ed, 2145, [IntPtr]::Zero, [IntPtr]::Zero)   # SCI_GETSELECTIONEND
        Write-Output "  dy=$dy sel=[$ss,$se]"
        if ($ss -eq 12 -and $se -eq 17) { $hitOk = $true; break }
    }
    if ($hitOk) { Write-Output "PASS dblclick-locate" }
    else { Write-Output "FAIL dblclick-locate"; $overallFail = $true }
}
finally {
    # Only clean up when we launched the process; -AttachPid leaves it to the caller.
    if ($AttachPid -eq 0) {
        if ($proc -and -not $proc.HasExited) {
            $proc.CloseMainWindow() | Out-Null
            Start-Sleep -Milliseconds 600
            Add-Type -AssemblyName System.Windows.Forms
            [System.Windows.Forms.SendKeys]::SendWait("n")
            if (-not $proc.WaitForExit(4000)) { Stop-Process -Id $proc.Id -Force }
        }
        Remove-Item $file -Force -ErrorAction SilentlyContinue
    }
}

Write-Output $(if ($overallFail) { "DBLCLICK-TEST-FAIL" } else { "DBLCLICK-TEST-PASS" })
exit $(if ($overallFail) { 1 } else { 0 })
