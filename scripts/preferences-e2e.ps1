# preferences-e2e.ps1 - end-to-end smoke for the Preferences dialog (stage 1).
#
# 1) launch app, 2) WM_COMMAND(Cmd::Preferences=880) to open 首选项,
# 3) assert the dialog window exists, 4) WM_COMMAND(IDC_CANCEL=3203) to close,
# 5) assert it closed and the app is still alive. Exit 1 on any failure.
param(
    [Parameter(Mandatory=$true)][string]$ExePath
)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class PF {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }

    // Regression: OK/Cancel buttons must be visible and inside the dialog
    // screen rect. They were once double-DPI-scaled out of the client area
    // (invisible at 125% scaling), leaving no way to commit changes.
    public static bool ButtonVisibleOnDialog(IntPtr dlg, int id) {
        IntPtr btn = GetDlgItem(dlg, id);
        if (btn == IntPtr.Zero || !IsWindowVisible(btn)) return false;
        RECT rb; GetWindowRect(btn, out rb);
        if (rb.R - rb.L <= 0 || rb.B - rb.T <= 0) return false;
        RECT rd; GetWindowRect(dlg, out rd);
        return rb.L >= rd.L && rb.T >= rd.T && rb.R <= rd.R && rb.B <= rd.B;
    }

    public static uint pid;
    public static IntPtr frame = IntPtr.Zero;
    public static IntPtr prefs = IntPtr.Zero;

    static bool FrameScan(IntPtr h, IntPtr lp) {
        uint w; GetWindowThreadProcessId(h, out w);
        if (w == pid) { var c = new StringBuilder(64); GetClassNameW(h, c, 64);
            if (c.ToString().StartsWith("xfsWinPad")) { frame = h; return false; } }
        return true;
    }
    static bool PrefsScan(IntPtr h, IntPtr lp) {
        uint w; GetWindowThreadProcessId(h, out w);
        if (w == pid) {
            var c = new StringBuilder(64); GetClassNameW(h, c, 64);
            // 按 ASCII 类名查找，避免 .ps1 无 BOM 时中文字面量乱码
            if (c.ToString() == "xfsWinPadPreferences") { prefs = h; return false; }
        }
        return true;
    }
    public static void Locate() {
        frame = IntPtr.Zero; prefs = IntPtr.Zero;
        EnumWindows(new EnumProc(FrameScan), IntPtr.Zero);
        if (frame != IntPtr.Zero) EnumWindows(new EnumProc(PrefsScan), IntPtr.Zero);
    }
}
"@

function Fail([string]$m) { Write-Output "FAIL $m"; if ($app) { Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force }; exit 1 }

function Wait-True([scriptblock]$cond, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

$exe = (Resolve-Path $ExePath).Path
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
Start-Process -FilePath $exe | Out-Null
if (-not (Wait-True { (Get-Process xfsWinPad -ErrorAction SilentlyContinue) -ne $null } 15000)) { Fail 'app launched' }
$app = Get-Process xfsWinPad -ErrorAction SilentlyContinue
[PF]::pid = [uint32]$app.Id
if (-not (Wait-True { [PF]::Locate(); [PF]::frame -ne [IntPtr]::Zero } 15000)) { Fail 'main frame found' }

# open Preferences (plugin load may delay message-loop start; poll generously)
[PF]::PostMessageW([PF]::frame, 0x0111, [IntPtr]880, [IntPtr]::Zero) | Out-Null
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -ne [IntPtr]::Zero } 20000)) { Fail 'Preferences dialog opened' }
Write-Output 'PASS Preferences dialog opened'

# OK/Cancel buttons visible inside the dialog (double-DPI-scaling regression)
if (-not [PF]::ButtonVisibleOnDialog([PF]::prefs, 3202)) { Fail 'OK button visible inside dialog' }
if (-not [PF]::ButtonVisibleOnDialog([PF]::prefs, 3203)) { Fail 'Cancel button visible inside dialog' }
Write-Output 'PASS OK/Cancel buttons visible inside dialog'

# cancel it (IDC_CANCEL = 3203)
[PF]::PostMessageW([PF]::prefs, 0x0111, [IntPtr]3203, [IntPtr]::Zero) | Out-Null
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -eq [IntPtr]::Zero } 8000)) { Fail 'Preferences dialog closed' }
Write-Output 'PASS Preferences dialog closed'

# app still alive
if (-not (Get-Process xfsWinPad -ErrorAction SilentlyContinue)) { Fail 'app alive after dialog round-trip' }
Write-Output 'PASS app alive after dialog round-trip'

# reopen and OK it (IDC_OK = 3202) to exercise the save path
[PF]::PostMessageW([PF]::frame, 0x0111, [IntPtr]880, [IntPtr]::Zero) | Out-Null
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -ne [IntPtr]::Zero } 8000)) { Fail 'Preferences reopened' }
[PF]::PostMessageW([PF]::prefs, 0x0111, [IntPtr]3202, [IntPtr]::Zero) | Out-Null
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -eq [IntPtr]::Zero } 8000)) { Fail 'Preferences closed via OK' }
Write-Output 'PASS Preferences OK path (apply+save)'

# X-button path: WM_CLOSE must restore the (disabled) owner — regression for
# the 2026-08-28 freeze where closing via X left the frame WS_DISABLED forever.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class EN {
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
}
"@
[PF]::PostMessageW([PF]::frame, 0x0111, [IntPtr]880, [IntPtr]::Zero) | Out-Null
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -ne [IntPtr]::Zero } 8000)) { Fail 'Preferences reopened for X test' }
if (-not (Wait-True { -not [EN]::IsWindowEnabled([PF]::frame) } 3000)) { Fail 'frame disabled while dialog open (modal)' }
[PF]::PostMessageW([PF]::prefs, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
if (-not (Wait-True { [PF]::Locate(); [PF]::prefs -eq [IntPtr]::Zero } 8000)) { Fail 'Preferences closed via X' }
if (-not (Wait-True { [EN]::IsWindowEnabled([PF]::frame) } 8000)) { Fail 'frame re-enabled after X close' }
if (-not (Get-Process xfsWinPad -ErrorAction SilentlyContinue)) { Fail 'app alive after X close' }
Write-Output 'PASS X close re-enables owner (freeze regression)'

Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output 'preferences-e2e: ALL PASS'
exit 0
