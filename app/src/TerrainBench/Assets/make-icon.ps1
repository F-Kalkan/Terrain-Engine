# Draws TerrainBench.ico: a ridge in teal on the app's navy, with a sight line from the observer
# (blue) to the target (amber) passing over it. Each size is drawn on its own rather than scaled
# down, so the small ones keep a readable line; the file holds PNG images at 16 to 256 px.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File make-icon.ps1

Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$output = Join-Path $PSScriptRoot 'TerrainBench.ico'

function New-Colour([string] $hex, [int] $alpha = 255) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb($alpha, $c.R, $c.G, $c.B)
}

function New-RoundedSquare([float] $size, [float] $radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($size - $d, 0, $d, $d, 270, 90)
    $path.AddArc($size - $d, $size - $d, $d, $d, 0, 90)
    $path.AddArc(0, $size - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Get-IconPng([int] $px) {
    # The design is laid out on a 160-unit square.
    $s = $px / 160.0
    $bitmap = New-Object System.Drawing.Bitmap $px, $px, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $tile = New-RoundedSquare ($px - 0.5) ([Math]::Max(3, 34 * $s))
    $g.FillPath((New-Object System.Drawing.SolidBrush (New-Colour '#0F2438')), $tile)
    $g.SetClip($tile)

    $ridge = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(0, 120 * $s), [System.Drawing.PointF]::new(38 * $s, 82 * $s),
        [System.Drawing.PointF]::new(60 * $s, 98 * $s), [System.Drawing.PointF]::new(92 * $s, 54 * $s),
        [System.Drawing.PointF]::new(122 * $s, 92 * $s), [System.Drawing.PointF]::new(160 * $s, 70 * $s),
        [System.Drawing.PointF]::new(160 * $s, 160 * $s), [System.Drawing.PointF]::new(0, 160 * $s))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush (New-Colour '#2F6B5E')), $ridge)

    $near = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(0, 138 * $s), [System.Drawing.PointF]::new(44 * $s, 108 * $s),
        [System.Drawing.PointF]::new(72 * $s, 120 * $s), [System.Drawing.PointF]::new(108 * $s, 88 * $s),
        [System.Drawing.PointF]::new(160 * $s, 110 * $s), [System.Drawing.PointF]::new(160 * $s, 160 * $s),
        [System.Drawing.PointF]::new(0, 160 * $s))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush (New-Colour '#86C9B4' 115)), $near)

    # Thicker, relatively, when small, so the line survives at taskbar size.
    $lineWidth = [Math]::Max(1.6, 7 * $s * $(if ($px -le 24) { 1.5 } else { 1 }))
    $pen = New-Object System.Drawing.Pen (New-Colour '#00B4D8'), $lineWidth
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, 22 * $s, 74 * $s, 144 * $s, 36 * $s)

    if ($px -ge 32) {
        foreach ($end in @(@(22, 74, '#0072B2'), @(144, 36, '#E69F00'))) {
            $ring = 11 * $s; $dot = 6 * $s
            $g.FillEllipse([System.Drawing.Brushes]::White, $end[0] * $s - $ring, $end[1] * $s - $ring, $ring * 2, $ring * 2)
            $g.FillEllipse((New-Object System.Drawing.SolidBrush (New-Colour $end[2])), $end[0] * $s - $dot, $end[1] * $s - $dot, $dot * 2, $dot * 2)
        }
    }

    $g.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return , $stream.ToArray()
}

# ICO: a 6-byte header, a 16-byte entry per image, then the PNG data.
$images = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($px in $sizes) { $images.Add([byte[]](Get-IconPng $px)) }
$file = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $file
$writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $px = $sizes[$i]
    $dimension = if ($px -ge 256) { 0 } else { $px }
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([UInt16]1); $writer.Write([UInt16]32)
    $writer.Write([UInt32]$images[$i].Length); $writer.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($output, $file.ToArray())
Write-Output "Wrote $output ($($file.Length) bytes, $($sizes.Count) sizes)"
