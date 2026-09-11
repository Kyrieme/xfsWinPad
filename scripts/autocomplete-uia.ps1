$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class E {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [Out] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr S(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SW(IntPtr h, uint m, IntPtr w, [Out] StringBuilder sb);
    public static IntPtr Sci = IntPtr.Zero;
    public static bool Cb(IntPtr h, IntPtr l) {
        var cn = new StringBuilder(64);
        GetClassNameW(h, cn, 64);
        if (cn.ToString() == "Scintilla" && Sci == IntPtr.Zero) Sci = h;
        return true;
    }
    public static string Doc(IntPtr h) {
        int len = (int)SW(h, 0x000E, IntPtr.Zero, null);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 4);
        SW(h, 0x000D, (IntPtr)(len + 1), sb);
        return sb.ToString();
    }
}
"@
$pad = "D:\AI_Work\codex\xfsPad\build\bin\Release\xfsWinPad.exe"
$testFile = "$env:TEMP\autoc_probe.txt"
Set-Content -Path $testFile -Value "" -Encoding ASCII
Get-Process xfsWinPad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Start-Process $pad -ArgumentList "`"$testFile`""
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $p = Get-Process xfsWinPad -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) { break }
}
$p = Get-Process xfsWinPad
Start-Sleep -Milliseconds 1200
[E]::EnumChildWindows([IntPtr]$p.MainWindowHandle, [E+EnumProc]{ param($h,$l) [E]::Cb($h,$l) }, [IntPtr]::Zero) | Out-Null
$ed = [E]::Sci
[E]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
[E]::SetFocus($ed) | Out-Null
Start-Sleep -Milliseconds 300
# type "printf_internals" then REAL Enter (WM_KEYDOWN VK_RETURN, no WM_CHAR)
foreach ($c in "printf_internals".ToCharArray()) {
    [E]::PostMessageW($ed, 0x0102, [IntPtr][int]$c, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 15
}
[E]::PostMessageW($ed, 0x0100, [IntPtr]13, [IntPtr]::Zero) | Out-Null   # WM_KEYDOWN VK_RETURN
Start-Sleep -Milliseconds 300
foreach ($c in "pri".ToCharArray()) {
    [E]::PostMessageW($ed, 0x0102, [IntPtr][int]$c, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 700
Write-Host ("active: " + [E]::S($ed, 2102, [IntPtr]::Zero, [IntPtr]::Zero))
Write-Host ("doc before accept: [" + [E]::Doc($ed) + "]")
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
$bmp.Save("D:\AI_Work\codex\xfsPad\out\autoc_popup.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "screenshot saved"
# accept with REAL Enter (WM_KEYDOWN)
[E]::PostMessageW($ed, 0x0100, [IntPtr]13, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 400
Write-Host ("doc after accept: [" + [E]::Doc($ed) + "]")
Write-Host ("alive: " + ($null -ne (Get-Process xfsWinPad -ErrorAction SilentlyContinue)))
