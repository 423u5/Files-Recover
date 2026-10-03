# Checks the image test builders' files with Windows' own decoder (GDI+),
# which reads JPEG, PNG, GIF and BMP, RLE-compressed bitmaps included. The
# files come from ImageBuilderExport.WritesFiles (see docs/testing/testing.md).
# Companion of check_image_builder_files.sh (libjpeg-turbo, libpng, giflib, libwebp).
#
# Usage: powershell -File check_image_builder_files.ps1 <directory>
param([Parameter(Mandatory = $true)][string]$Directory)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$failed = 0
$checked = 0
foreach ($file in Get-ChildItem -Path $Directory -Include *.jpg, *.png, *.gif, *.bmp -Recurse) {
    try {
        $image = [System.Drawing.Image]::FromFile($file.FullName)
        # Touching a pixel forces GDI+ to decode, not just read the header.
        $bitmap = New-Object System.Drawing.Bitmap $image
        $null = $bitmap.GetPixel(0, 0)
        Write-Host ("  ok       {0} ({1}x{2}, {3})" -f $file.Name, $image.Width, $image.Height, $image.PixelFormat)
        $bitmap.Dispose()
        $image.Dispose()
        $checked++
    } catch {
        Write-Host ("  FAILED   {0}: {1}" -f $file.Name, $_.Exception.Message)
        $failed++
    }
}
Write-Host ("{0} files decoded, {1} refused" -f $checked, $failed)
if ($failed -gt 0) { exit 1 }
