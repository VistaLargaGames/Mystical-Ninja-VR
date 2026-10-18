param([string]$Out = "shot.png", [int]$X = 1759, [int]$Y = 238, [int]$W = 1600, [int]$H = 960, [int]$Scale = 2)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($W / $Scale)), ([int]($H / $Scale))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
