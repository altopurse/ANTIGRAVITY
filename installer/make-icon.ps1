# Generates installer/app.ico (dark circle, cyan "A") used as the exe and
# installer icon. Re-run only if you want to change the artwork:
#   powershell -ExecutionPolicy Bypass -File installer/make-icon.ps1

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$outPath = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "app.ico"

function Render-Png([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # Dark navy disc with a subtle vertical gradient
    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 22, 26, 38),
        [System.Drawing.Color]::FromArgb(255, 10, 12, 20),
        [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillEllipse($brush, 0, 0, $size - 1, $size - 1)

    # Cyan ring
    if ($size -ge 32) {
        $penW = [Math]::Max(1, [int]($size * 0.05))
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 56, 176, 248), $penW)
        $half = [int]($penW / 2)
        $g.DrawEllipse($pen, $half, $half, $size - 1 - $penW, $size - 1 - $penW)
        $pen.Dispose()
    }

    # Bold "A"
    $fontSize = [Math]::Max(6, [single]($size * 0.52))
    $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 56, 176, 248))
    $g.DrawString("A", $font, $textBrush, (New-Object System.Drawing.RectangleF(0, 0, $size, $size)), $fmt)

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $font.Dispose(); $textBrush.Dispose(); $brush.Dispose()
    return ,$ms.ToArray()
}

$sizes = @(256, 64, 48, 32, 16)
$images = @()
foreach ($s in $sizes) { $images += ,(Render-Png $s) }

# Pack as ICO (PNG-compressed entries; supported since Windows Vista)
$fs = [System.IO.File]::Create($outPath)
$bw = New-Object System.IO.BinaryWriter($fs)
try {
    $bw.Write([uint16]0)               # reserved
    $bw.Write([uint16]1)               # type: icon
    $bw.Write([uint16]$sizes.Count)    # image count

    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $data = $images[$i]
        $dim = if ($s -ge 256) { 0 } else { $s }   # 0 means 256
        $bw.Write([byte]$dim)          # width
        $bw.Write([byte]$dim)          # height
        $bw.Write([byte]0)             # colors in palette
        $bw.Write([byte]0)             # reserved
        $bw.Write([uint16]1)           # color planes
        $bw.Write([uint16]32)          # bits per pixel
        $bw.Write([uint32]$data.Length)
        $bw.Write([uint32]$offset)
        $offset += $data.Length
    }
    foreach ($d in $images) { $bw.Write($d) }
}
finally {
    $bw.Close()
}

Write-Output "Icon written: $outPath"
