# gen_icons.ps1 - Generates tray icons for raa-player (playing / idle / stopped / offline)
# Headphone glyph, so it is distinguishable from raa-client's speaker icon in the tray.
# Requires .NET (System.Drawing) - works on Windows PowerShell 5.1+

Add-Type -AssemblyName System.Drawing

function Draw-Headphones {
    param([string]$state, [int]$size)

    $bmp = New-Object System.Drawing.Bitmap($size, $size,
               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $col = switch ($state) {
        'playing' { [System.Drawing.Color]::FromArgb(255,  30, 144, 255) }
        'idle'    { [System.Drawing.Color]::FromArgb(255,  95, 125, 155) }
        'stopped' { [System.Drawing.Color]::FromArgb(255, 150, 150, 150) }
        'offline' { [System.Drawing.Color]::FromArgb(255, 220,  50,  50) }
    }

    $s   = $size / 16.0  # scale factor (designed on a 16px grid)
    $br  = New-Object System.Drawing.SolidBrush($col)
    $pen = New-Object System.Drawing.Pen($col, [float]([Math]::Max(1.0, 1.8 * $s)))

    # Head band (upper half of an ellipse)
    $g.DrawArc($pen, [float](2.5*$s), [float](2*$s), [float](11*$s), [float](12*$s), 180.0, 180.0)

    # Ear cups
    $g.FillRectangle($br, [float](1*$s),  [float](8*$s), [float](4*$s), [float](6.5*$s))
    $g.FillRectangle($br, [float](11*$s), [float](8*$s), [float](4*$s), [float](6.5*$s))

    if ($state -eq 'stopped') {
        # Slash: playback switched off by the user
        $spen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 70, 70, 70),
                    [float]([Math]::Max(1.5, 1.8 * $s)))
        $g.DrawLine($spen, [float](2*$s), [float](14.5*$s), [float](14.5*$s), [float](1.5*$s))
        $spen.Dispose()
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

    $fs = [System.IO.File]::Create($path)
    $w  = New-Object System.IO.BinaryWriter($fs)

    # ICONDIR header
    $w.Write([uint16]0)              # reserved
    $w.Write([uint16]1)              # type = ICO
    $w.Write([uint16]$bmps.Count)    # count

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
$sizes  = @(16, 20, 24, 32, 48, 256)
$states = @('playing', 'idle', 'stopped', 'offline')

foreach ($state in $states) {
    $bmps = $sizes | ForEach-Object { Draw-Headphones $state $_ }
    $outPath = Join-Path $dir "$state.ico"
    Save-IcoFile $bmps $outPath
    $bmps | ForEach-Object { $_.Dispose() }
    Write-Host "Created: $outPath"
}
