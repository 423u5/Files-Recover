# Makes small image files with Windows' own encoders (GDI+), for the samples
# embedded in tests/support/image_samples.cpp and for the reference corpus of
# ImageReferenceTest. Companion of make_image_samples.sh (libjpeg-turbo,
# libwebp, giflib).
#
# Usage: powershell -File make_image_samples.ps1 <output directory> [large]
#   Without "large" the images are 16x16; with "large", 320x240 (GDI+ is
#   driven pixel by pixel here, which is slow for larger pictures).
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Size = "small"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if ($Size -eq "large") { $w = 320; $h = 240 } else { $w = 16; $h = 16 }

# The same gradient as make_image_samples.sh, with an alpha ramp.
$argb = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $r = [int](255 * $x / [Math]::Max(1, $w - 1))
        $g = [int](255 * $y / [Math]::Max(1, $h - 1))
        $b = [int](127 + 127 * [Math]::Sin(($x + $y) / 3.0))
        $a = [int](255 * ($x + $y) / [Math]::Max(1, $w + $h - 2))
        $argb.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($a, $r, $g, $b))
    }
}
$rgb = $argb.Clone((New-Object System.Drawing.Rectangle 0, 0, $w, $h),
                   [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)

$formats = [System.Drawing.Imaging.ImageFormat]
$jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$quality = New-Object System.Drawing.Imaging.EncoderParameters 1
$quality.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 85L

$rgb.Save((Join-Path $Out "gdiplus.jpg"), $jpegCodec, $quality)
$rgb.Save((Join-Path $Out "gdiplus_24.png"), $formats::Png)
$argb.Save((Join-Path $Out "gdiplus_alpha.png"), $formats::Png)
$rgb.Save((Join-Path $Out "gdiplus.gif"), $formats::Gif)
$rgb.Save((Join-Path $Out "gdiplus_24.bmp"), $formats::Bmp)
$argb.Save((Join-Path $Out "gdiplus_32.bmp"), $formats::Bmp)

$argb.Dispose()
$rgb.Dispose()
Get-ChildItem $Out -Filter "gdiplus*" | Format-Table Name, Length
