# Launches the demo map, captures the window, then closes it.
#   .\tools\screenshot.ps1
#   .\tools\screenshot.ps1 -Out docs\images\screenshot.png -Seconds 6
param(
    [string]$Out = 'screenshot.png',
    [int]$Seconds = 6,
    [string]$Exe = 'build\release\tripmap.exe'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
}
'@
[Win]::SetProcessDPIAware() | Out-Null
# Keep the pointer off the stop list so opening the window does not activate a row.
[Win]::SetCursorPos(2000, 400) | Out-Null
$root = Split-Path $PSScriptRoot -Parent
$proc = Start-Process (Join-Path $root $Exe) -ArgumentList '--demo' -WorkingDirectory $root -PassThru `
    -RedirectStandardOutput (Join-Path $env:TEMP 'tripmap_stdout.txt') `
    -RedirectStandardError (Join-Path $env:TEMP 'tripmap_stderr.txt')
Start-Sleep -Seconds $Seconds
$proc.Refresh()
if ($proc.HasExited) {
    Write-Host "App exited with code $($proc.ExitCode)"
    Get-Content (Join-Path $env:TEMP 'tripmap_stderr.txt')
    exit 1
}
$h = $proc.MainWindowHandle
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
# Escape clears a stop the focus click may have selected; F fits every picture in view.
foreach ($vk in 0x1B, 0x46) {
    [Win]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [Win]::keybd_event([byte]$vk, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
}
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$dir = Split-Path $Out -Parent
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$bmp.Save((Join-Path $root $Out), [System.Drawing.Imaging.ImageFormat]::Png)
Stop-Process -Id $proc.Id
Get-Content (Join-Path $env:TEMP 'tripmap_stdout.txt')
Write-Host "Saved $Out"
