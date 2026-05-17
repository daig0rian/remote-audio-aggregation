# gen_icons.ps1 - Generates placeholder tray icons (active / silent / error)
# Run once; replace the resulting .ico files with your own designs.
# Requires .NET (System.Drawing) — works on Windows PowerShell 5.1+

Add-Type -AssemblyName System.Drawing

function Draw-Speaker {
    param([string]$state, [int]$size)

    $bmp = New-Object System.Drawing.Bitmap($size, $size,
               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $col = switch ($state) {
        'active' { [System.Drawing.Color]::FromArgb(255, 60, 180, 60) }
        'silent' { [System.Drawing.Color]::FromArgb(255, 90, 140, 90) }
        'error'  { [System.Drawing.Color]::FromArgb(255, 220, 50, 50) }
    }

    $br  = New-Object System.Drawing.SolidBrush($col)
    $pen = New-Object System.Drawing.Pen($col, [float]([Math]::Max(1.0, $size / 14.0)))

    $s = $size / 16.0  # scale factor

    # Speaker body (rectangle)
    $g.FillRectangle($br,
        [float](1*$s), [float](5*$s),
        [float](4*$s), [float](6*$s))

    # Speaker cone (trapezoid / polygon)
    $pts = @(
        [System.Drawing.PointF]::new([float](5*$s),  [float](5*$s)),
        [System.Drawing.PointF]::new([float](10*$s), [float](2*$s)),
        [System.Drawing.PointF]::new([float](10*$s), [float](14*$s)),
        [System.Drawing.PointF]::new([float](5*$s),  [float](11*$s))
    )
    $g.FillPolygon($br, $pts)

    if ($state -eq 'active') {
        # Two arcs (sound waves)
        $g.DrawArc($pen, [float](10*$s), [float](4*$s),  [float](3*$s), [float](8*$s),  -65.0, 130.0)
        $g.DrawArc($pen, [float](11*$s), [float](2*$s),  [float](4*$s), [float](12*$s), -65.0, 130.0)
    }

    if ($state -eq 'error') {
        # X mark
        $xpen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 220, 50, 50),
                    [float]([Math]::Max(1.5, $size / 10.0)))
        $g.DrawLine($xpen, [float](11*$s), [float](5*$s), [float](14*$s), [float](9*$s))
        $g.DrawLine($xpen, [float](14*$s), [float](5*$s), [float](11*$s), [float](9*$s))
        $xpen.Dispose()
    }

    $br.Dispose(); $pen.Dispose(); $g.Dispose()
    return $bmp
}

function Save-IcoFile {
    param([System.Drawing.Bitmap[]]$bmps, [string]$path)

    # Encode each bitmap as PNG bytes
    $pngs = $bmps | ForEach-Object {
        $ms = New-Object System.IO.MemoryStream
        $_.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        ,$ms.ToArray()
        $ms.Dispose()
    }

    $fs = [System.IO.File]::OpenWrite($path)
    $w  = New-Object System.IO.BinaryWriter($fs)

    # GRPICONDIR header
    $w.Write([uint16]0)              # reserved
    $w.Write([uint16]1)              # type = ICO
    $w.Write([uint16]$bmps.Count)   # count

    # Directory entries (offset starts after header + all entries)
    [uint32]$offset = 6 + $bmps.Count * 16
    for ($i = 0; $i -lt $bmps.Count; $i++) {
        $bmp = $bmps[$i]
        # width/height: 0 encodes 256
        $bw = if ($bmp.Width  -eq 256) { [byte]0 } else { [byte]$bmp.Width }
        $bh = if ($bmp.Height -eq 256) { [byte]0 } else { [byte]$bmp.Height }
        $w.Write($bw)
        $w.Write($bh)
        $w.Write([byte]0)              # color count (0 = true-color)
        $w.Write([byte]0)              # reserved
        $w.Write([uint16]1)            # planes
        $w.Write([uint16]32)           # bit depth
        $w.Write([uint32]$pngs[$i].Length)
        $w.Write($offset)
        $offset += [uint32]$pngs[$i].Length
    }

    foreach ($png in $pngs) { $w.Write($png) }

    $w.Dispose(); $fs.Dispose()
}

$dir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$sizes  = @(16, 32, 48, 256)
$states = @('active', 'silent', 'error')

foreach ($state in $states) {
    $bmps = $sizes | ForEach-Object { Draw-Speaker $state $_ }
    $outPath = Join-Path $dir "$state.ico"
    Save-IcoFile $bmps $outPath
    $bmps | ForEach-Object { $_.Dispose() }
    Write-Host "Created: $outPath"
}

Write-Host "Done. Replace these .ico files with your own designs."
