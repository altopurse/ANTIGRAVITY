# Generates the Antigravity brand artwork from code (single source of truth):
#   installer/app.ico          - exe/window/taskbar/installer icon
#   docs/logo.png              - 1024px master logo tile
#   docs/logo-instagram.png    - 1080x1080 Instagram post (logo + wordmark)
# Re-run only if you want to change the artwork:
#   powershell -ExecutionPolicy Bypass -File installer/make-icon.ps1
#
# Design: dark rounded-square tile, upward chevron "A" in a violet->cyan
# gradient, a levitating dot above the apex (antigravity), equalizer bars
# underneath (voice/soundboard). Matches the v2.1 in-app theme.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$icoPath   = Join-Path $root "installer\app.ico"
$logoPath  = Join-Path $root "docs\logo.png"
$instaPath = Join-Path $root "docs\logo-instagram.png"

# Brand palette (mirrors src/ui/Theme.h)
$colBgTop   = [System.Drawing.Color]::FromArgb(255, 26, 30, 51)
$colBgBot   = [System.Drawing.Color]::FromArgb(255, 10, 12, 22)
$colViolet  = [System.Drawing.Color]::FromArgb(255, 138, 116, 255)
$colCyan    = [System.Drawing.Color]::FromArgb(255, 66, 196, 245)
$colBar     = [System.Drawing.Color]::FromArgb(230, 122, 100, 240)
$colBorder  = [System.Drawing.Color]::FromArgb(70, 138, 116, 255)

function New-RoundedRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Draw the logo mark onto graphics $g inside the square (ox, oy, s).
# $withTile: draw the rounded-square background tile.
function Draw-Mark($g, [single]$ox, [single]$oy, [single]$s, [bool]$withTile) {
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    if ($withTile) {
        $pad = $s * 0.01
        $tile = New-RoundedRectPath ($ox + $pad) ($oy + $pad) ($s - 2 * $pad) ($s - 2 * $pad) ($s * 0.22)
        $bgRect = New-Object System.Drawing.RectangleF($ox, $oy, $s, $s)
        $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            $bgRect, $colBgTop, $colBgBot,
            [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
        $g.FillPath($bgBrush, $tile)
        if ($s -ge 48) {
            $bPen = New-Object System.Drawing.Pen($colBorder, [Math]::Max(1.0, $s * 0.012))
            $g.DrawPath($bPen, $tile)
            $bPen.Dispose()
        }
        $bgBrush.Dispose(); $tile.Dispose()
    }

    # Gradient for the chevron stroke (violet top-left -> cyan bottom-right)
    $gradRect = New-Object System.Drawing.RectangleF($ox, $oy, $s, $s)
    $strokeBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $gradRect, $colViolet, $colCyan,
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)

    # Upward chevron "A" (no crossbar)
    $penW = $s * 0.085
    $pen = New-Object System.Drawing.Pen($strokeBrush, $penW)
    $pen.StartCap  = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap    = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin  = [System.Drawing.Drawing2D.LineJoin]::Round
    $apex  = New-Object System.Drawing.PointF(($ox + 0.50 * $s), ($oy + 0.26 * $s))
    $left  = New-Object System.Drawing.PointF(($ox + 0.26 * $s), ($oy + 0.70 * $s))
    $right = New-Object System.Drawing.PointF(($ox + 0.74 * $s), ($oy + 0.70 * $s))
    $g.DrawLines($pen, [System.Drawing.PointF[]]@($left, $apex, $right))
    $pen.Dispose(); $strokeBrush.Dispose()

    # Levitating dot above the apex
    $dotR = $s * 0.052
    $dotBrush = New-Object System.Drawing.SolidBrush($colCyan)
    $g.FillEllipse($dotBrush, ($ox + 0.50 * $s - $dotR), ($oy + 0.115 * $s - $dotR), $dotR * 2, $dotR * 2)
    $dotBrush.Dispose()

    # Equalizer bars under the chevron (skip at tiny sizes - unreadable)
    if ($s -ge 48) {
        $barBrush = New-Object System.Drawing.SolidBrush($colBar)
        $xs      = @(0.335, 0.4175, 0.50, 0.5825, 0.665)
        $heights = @(0.055, 0.095, 0.135, 0.095, 0.055)
        $barW = $s * 0.045
        $cy = 0.855
        for ($i = 0; $i -lt $xs.Count; $i++) {
            $bh = $heights[$i] * $s
            $bx = $ox + $xs[$i] * $s - $barW / 2
            $by = $oy + $cy * $s - $bh / 2
            $bp = New-RoundedRectPath $bx $by $barW $bh ($barW / 2)
            $g.FillPath($barBrush, $bp)
            $bp.Dispose()
        }
        $barBrush.Dispose()
    }
}

function Render-LogoPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    Draw-Mark $g 0 0 ([single]$size) $true
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return ,$ms.ToArray()
}

# ---- installer/app.ico (PNG-compressed entries; supported since Vista) ----
$sizes = @(256, 64, 48, 32, 16)
$images = @()
foreach ($s in $sizes) { $images += ,(Render-LogoPng $s) }

$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
try {
    $bw.Write([uint16]0)               # reserved
    $bw.Write([uint16]1)               # type: icon
    $bw.Write([uint16]$sizes.Count)    # image count

    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $data = $images[$i]
        $dim = 0; if ($s -lt 256) { $dim = $s }   # 0 means 256
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
Write-Output "Icon written: $icoPath"

# ---- docs/logo.png (1024 master) -------------------------------------------
[System.IO.File]::WriteAllBytes($logoPath, (Render-LogoPng 1024))
Write-Output "Logo written: $logoPath"

# ---- docs/logo-instagram.png (1080x1080 post) -------------------------------
$W = 1080
$bmp = New-Object System.Drawing.Bitmap($W, $W)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$g.Clear($colBgBot)

# Soft violet glow behind the mark
$glowPath = New-Object System.Drawing.Drawing2D.GraphicsPath
$glowPath.AddEllipse(140, 40, 800, 800)
$glow = New-Object System.Drawing.Drawing2D.PathGradientBrush($glowPath)
$glow.CenterColor = [System.Drawing.Color]::FromArgb(60, 138, 116, 255)
$glow.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 138, 116, 255))
$g.FillEllipse($glow, 140, 40, 800, 800)
$glow.Dispose(); $glowPath.Dispose()

# Logo tile
$markSize = 520
Draw-Mark $g (($W - $markSize) / 2) 130 $markSize $true

# Wordmark with manual letter-spacing
function Draw-Spaced($g, [string]$text, $font, $brush, [single]$cx, [single]$y, [single]$tracking) {
    $widths = @()
    $total = 0.0
    foreach ($ch in $text.ToCharArray()) {
        $w = $g.MeasureString([string]$ch, $font).Width
        $widths += $w
        $total += $w + $tracking
    }
    $total -= $tracking
    $x = $cx - $total / 2
    for ($i = 0; $i -lt $text.Length; $i++) {
        $g.DrawString([string]$text[$i], $font, $brush, $x, $y)
        $x += $widths[$i] + $tracking
    }
}

$fTitle = New-Object System.Drawing.Font("Segoe UI", 92, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$fSub   = New-Object System.Drawing.Font("Segoe UI", 40, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$fTag   = New-Object System.Drawing.Font("Segoe UI", 30, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$white  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 236, 238, 245))
$violet = New-Object System.Drawing.SolidBrush($colViolet)
$dim    = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 130, 138, 160))

Draw-Spaced $g "ANTIGRAVITY" $fTitle $white  ($W / 2) 705 4
Draw-Spaced $g "VOICE ENGINE" $fSub  $violet ($W / 2) 830 14
Draw-Spaced $g "Real-time voice changer & soundboard" $fTag $dim ($W / 2) 935 0

$fTitle.Dispose(); $fSub.Dispose(); $fTag.Dispose()
$white.Dispose(); $violet.Dispose(); $dim.Dispose()
$g.Dispose()
$bmp.Save($instaPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "Instagram post written: $instaPath"
