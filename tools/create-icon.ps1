param(
    [string]$OutputPath = (Join-Path $PSScriptRoot "..\NetworkPinger.ico")
)

Add-Type -AssemblyName System.Drawing

$size = 256
$bitmap = [System.Drawing.Bitmap]::new($size, $size)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$graphics.Clear([System.Drawing.Color]::FromArgb(4, 9, 6))

$gridPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(20, 55, 35), 1)
for ($position = 0; $position -lt $size; $position += 16) {
    $graphics.DrawLine($gridPen, $position, 0, $position, $size)
    $graphics.DrawLine($gridPen, 0, $position, $size, $position)
}

$random = [System.Random]::new(1977)
$characters = "0123456789ABCDEF+-<>[]{}"
$font = [System.Drawing.Font]::new([System.Drawing.FontFamily]::GenericMonospace, 16, [System.Drawing.FontStyle]::Bold)

for ($column = 8; $column -lt $size - 8; $column += 16) {
    $startY = $random.Next(-110, 40)
    $length = $random.Next(7, 16)
    for ($row = 0; $row -lt $length; $row++) {
        $y = $startY + ($row * 17)
        if ($y -lt -16 -or $y -gt $size) {
            continue
        }

        $brightness = [Math]::Max(45, 240 - (($length - $row) * 14))
        $color = [System.Drawing.Color]::FromArgb($brightness, 0, $brightness, 45)
        $brush = [System.Drawing.SolidBrush]::new($color)
        $character = $characters[$random.Next($characters.Length)]
        $graphics.DrawString($character, $font, $brush, $column, $y)
        $brush.Dispose()
    }
}

$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
$icon = [System.Drawing.Icon]::FromHandle($bitmap.GetHicon())
$icon.Save($stream)
$stream.Dispose()
$icon.Dispose()
$font.Dispose()
$gridPen.Dispose()
$graphics.Dispose()
$bitmap.Dispose()