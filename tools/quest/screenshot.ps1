# Saves a screenshot of the Goemon window (or the whole primary screen) for desk tests of the PC build.
param([string]$Out = "shot.png", [int]$Scale = 2)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int L, T, R, B; } }
"@
$h = [W]::FindWindow($null, "Goemon 64: Recompiled")
$r = New-Object W+RECT
if ($h -ne [IntPtr]::Zero -and [W]::GetWindowRect($h, [ref]$r)) {
    $x = $r.L; $y = $r.T; $w = $r.R - $r.L; $hh = $r.B - $r.T
} else {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $x = $b.X; $y = $b.Y; $w = $b.Width; $hh = $b.Height
}
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / $Scale)), ([int]($hh / $Scale))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
