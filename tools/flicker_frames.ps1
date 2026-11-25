# Grab N quick frames of the CHDPS3Recomp window into OutDir and print, for each,
# how much a sample grid differs from the first frame. Frames that differ a lot
# from their neighbours are the flicker frames worth looking at.
# Usage: powershell -File tools\flicker_frames.ps1 -OutDir out\flk -Frames 40
param([string]$OutDir = "out\flk", [int]$Frames = 40, [int]$DelayMs = 25)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class FlfW { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct R { public int L, T, Rr, B; } }
'@
New-Item -ItemType Directory -Force $OutDir | Out-Null
$dir = (Resolve-Path $OutDir).Path
$p = Get-Process CHDPS3Recomp -ErrorAction Stop
[FlfW]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
$r = New-Object FlfW+R; [FlfW]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.Rr - $r.L; $h = $r.B - $r.T
$grids = @()
for ($f = 0; $f -lt $Frames; $f++) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save("$dir\f$('{0:D2}' -f $f).png")
    $list = New-Object System.Collections.Generic.List[int]
    for ($y = 40; $y -lt $h - 10; $y += 20) { for ($x = 10; $x -lt $w - 10; $x += 20) {
        $c = $bmp.GetPixel($x, $y); $list.Add($c.R + $c.G + $c.B) } }
    $grids += ,($list.ToArray()); $bmp.Dispose()
    Start-Sleep -Milliseconds $DelayMs
}
for ($f = 1; $f -lt $Frames; $f++) {
    $a = $grids[$f - 1]; $b = $grids[$f]; $n = 0
    for ($i = 0; $i -lt $a.Length; $i++) { if ([Math]::Abs($a[$i] - $b[$i]) -gt 60) { $n++ } }
    "f$('{0:D2}' -f $f): {0:N1}% changed vs previous" -f (100.0 * $n / $a.Length)
}
