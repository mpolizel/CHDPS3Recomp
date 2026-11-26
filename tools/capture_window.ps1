# Capture a window (the game by default, or -Process CHDLauncher) to a PNG.
# Usage: powershell -File tools\capture_window.ps1 out\shot.png [-Process CHDLauncher]
param([string]$Out = "out\shot.png", [string]$Process = "CHDPS3Recomp")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class CapW { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct R { public int L, T, Rr, B; } }
'@
$p = Get-Process $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[CapW]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
$r = New-Object CapW+R; [CapW]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.Rr - $r.L), ($r.B - $r.T)
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save((Resolve-Path -LiteralPath (Split-Path $Out -Parent)).Path + "\" + (Split-Path $Out -Leaf))
$p.MainWindowTitle
