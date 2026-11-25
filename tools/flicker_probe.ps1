# Grab N quick frames of the CHDPS3Recomp window and report how much a sample
# grid changes between consecutive frames. A steady screen gives ~0 everywhere;
# flicker shows up as frames with large differences.
# Usage: powershell -File tools\flicker_probe.ps1 [-Frames 20] [-DelayMs 70]
param([int]$Frames = 20, [int]$DelayMs = 70, [string]$SaveFirst = "")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class FlkW { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct R { public int L, T, Rr, B; } }
'@
$p = Get-Process CHDPS3Recomp -ErrorAction Stop
[FlkW]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
$r = New-Object FlkW+R; [FlkW]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.Rr - $r.L; $h = $r.B - $r.T
$prev = $null; $diffs = @()
for ($f = 0; $f -lt $Frames; $f++) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    if ($f -eq 0 -and $SaveFirst) { $bmp.Save($SaveFirst) }
    $cur = New-Object 'int[]' 0
    $list = New-Object System.Collections.Generic.List[int]
    for ($y = 40; $y -lt $h - 10; $y += 24) { for ($x = 10; $x -lt $w - 10; $x += 24) {
        $c = $bmp.GetPixel($x, $y); $list.Add($c.R + $c.G + $c.B) } }
    $cur = $list.ToArray(); $bmp.Dispose()
    if ($prev) {
        $changed = 0; for ($i = 0; $i -lt $cur.Length; $i++) { if ([Math]::Abs($cur[$i] - $prev[$i]) -gt 60) { $changed++ } }
        $diffs += [Math]::Round(100.0 * $changed / $cur.Length, 1)
    }
    $prev = $cur; Start-Sleep -Milliseconds $DelayMs
}
"changed% between frames: " + ($diffs -join ' ')
"max: " + ($diffs | Measure-Object -Maximum).Maximum
