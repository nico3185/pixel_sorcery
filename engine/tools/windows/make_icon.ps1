# make_icon.ps1 PNG ICO - the Windows icon from the PixelSorcery logo (img/sorpix.png), as make_icon.sh
# makes the Mac's .icns: the logo scaled to 16-256 px (high-quality bicubic), each size stored as a PNG
# inside the .ico (Windows Vista and later read them). make_zip.sh compiles it into PixelSorcery.exe.
param([Parameter(Mandatory)][string]$Png, [Parameter(Mandatory)][string]$Ico)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Png))
$images = foreach ($s in 16, 24, 32, 48, 64, 128, 256) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($src, 0, 0, $s, $s); $g.Dispose()
    $ms = New-Object System.IO.MemoryStream; $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    , @($s, $ms.ToArray())
}
$src.Dispose()
$out = New-Object System.IO.MemoryStream; $w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)          # ICONDIR: an icon
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {                                                         # ICONDIRENTRY each
    $s = $i[0]; $data = $i[1]; $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($i in $images) { $w.Write([byte[]]$i[1]) }
$w.Flush(); [System.IO.File]::WriteAllBytes($Ico, $out.ToArray())
"icon: $Ico ($($images.Count) sizes, $($out.Length) bytes)"
