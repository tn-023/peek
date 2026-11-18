# Erzeugt Testdateien fuer die Sandbox-Pruefung (peek.exe --sandbox-test): Bilder in allen Formaten, die
# System.Drawing schreiben kann, SVG, ein echtes PDF (mit peek erzeugt), ein Video (falls ffmpeg vorhanden ist)
# sowie absichtlich kaputte bzw. praeparierte Dateien.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Exe
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function New-Bitmap([int]$w, [int]$h, [int]$seed) {
    $r = New-Object System.Random($seed)
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $c1 = [System.Drawing.Color]::FromArgb(255, $r.Next(0, 255), $r.Next(0, 255), $r.Next(0, 255))
    $c2 = [System.Drawing.Color]::FromArgb(255, $r.Next(0, 255), $r.Next(0, 255), $r.Next(0, 255))
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45.0)), $rect)
    $g.DrawString("peek $seed", (New-Object System.Drawing.Font('Segoe UI', 24)), [System.Drawing.Brushes]::White, 10, 10)
    $g.Dispose()
    return $bmp
}

$fmt = [System.Drawing.Imaging.ImageFormat]
$b = New-Bitmap 1600 1200 1; $b.Save((Join-Path $Root 'foto.jpg'), $fmt::Jpeg); $b.Dispose()
$b = New-Bitmap 800 600 2; $b.Save((Join-Path $Root 'grafik.png'), $fmt::Png); $b.Dispose()
$b = New-Bitmap 640 480 3; $b.Save((Join-Path $Root 'animation.gif'), $fmt::Gif); $b.Dispose()
$b = New-Bitmap 300 200 4; $b.Save((Join-Path $Root 'bild.bmp'), $fmt::Bmp); $b.Dispose()
$b = New-Bitmap 1024 768 5; $b.Save((Join-Path $Root 'scan.tif'), $fmt::Tiff); $b.Dispose()
$b = New-Bitmap 1080 1920 6; $b.Save((Join-Path $Root 'hochformat.jpg'), $fmt::Jpeg); $b.Dispose()

# SVG: normal, mit externem Bild (wird entfernt) und mit verstecktem Verweis in einer Entity (wird abgelehnt)
Set-Content -Path (Join-Path $Root 'vektor.svg') -Encoding UTF8 -Value @'
<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300"><rect width="400" height="300" fill="#2a5298"/>
<text x="20" y="80" font-family="Segoe UI" font-size="48" fill="white">Sandbox</text></svg>
'@
Set-Content -Path (Join-Path $Root 'vektor-extern.svg') -Encoding UTF8 -Value @'
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="400" height="300">
<image href="foto.jpg" width="400" height="300"/><image xlink:href="\\server\freigabe\x.png" width="10" height="10"/></svg>
'@
Set-Content -Path (Join-Path $Root 'vektor-entity.svg') -Encoding UTF8 -Value @'
<!DOCTYPE svg [<!ENTITY e "&#60;image href='\\server\x.png' width='9' height='9'/&#62;">]>
<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30">&e;</svg>
'@

# Kaputte Dateien: abgeschnittenes JPEG, Zufallsdaten mit Bild-Endung
$jpg = [System.IO.File]::ReadAllBytes((Join-Path $Root 'foto.jpg'))
[System.IO.File]::WriteAllBytes((Join-Path $Root 'kaputt-abgeschnitten.jpg'), $jpg[0..([int]($jpg.Length / 3))])
$rnd = New-Object byte[] 50000; (New-Object System.Random(7)).NextBytes($rnd)
[System.IO.File]::WriteAllBytes((Join-Path $Root 'kaputt-zufall.png'), $rnd)

# TGA (Windows kann TGA nicht schreiben): 24 Bit unkomprimiert, 32 Bit RLE mit Alpha, Palette, kaputt
function New-TgaHeader([int]$type, [int]$w, [int]$h, [int]$bits, [int]$desc, [int]$mapType = 0, [int]$mapLen = 0, [int]$mapBits = 0) {
    [byte[]]$hd = New-Object byte[] 18
    $hd[1] = $mapType; $hd[2] = $type; $hd[5] = $mapLen -band 255; $hd[6] = $mapLen -shr 8; $hd[7] = $mapBits
    $hd[12] = $w -band 255; $hd[13] = $w -shr 8; $hd[14] = $h -band 255; $hd[15] = $h -shr 8; $hd[16] = $bits; $hd[17] = $desc
    return ,$hd
}
$l = New-Object System.Collections.Generic.List[byte]
$l.AddRange((New-TgaHeader 2 320 200 24 0))
for ($y = 0; $y -lt 200; $y++) { for ($x = 0; $x -lt 320; $x++) { $l.Add([byte]($x * 255 / 319)); $l.Add([byte]($y * 255 / 199)); $l.Add([byte]128) } }
[System.IO.File]::WriteAllBytes((Join-Path $Root 'textur-24bit.tga'), $l.ToArray())
$l = New-Object System.Collections.Generic.List[byte]
$l.AddRange((New-TgaHeader 10 256 128 32 0x28))
for ($y = 0; $y -lt 128; $y++) { for ($k = 0; $k -lt 2; $k++) { $l.Add(0xFF); $l.Add([byte](2 * $y)); $l.Add([byte]($k * 200)); $l.Add([byte]90); $l.Add([byte](255 - $y)) } }
[System.IO.File]::WriteAllBytes((Join-Path $Root 'sprite-rle-alpha.tga'), $l.ToArray())
$l = New-Object System.Collections.Generic.List[byte]
$l.AddRange((New-TgaHeader 1 64 64 8 0x20 1 4 24))
$l.AddRange([byte[]](0,0,0, 255,0,0, 0,255,0, 0,0,255))
for ($i = 0; $i -lt 4096; $i++) { $l.Add([byte](($i -shr 4) % 4)) }
[System.IO.File]::WriteAllBytes((Join-Path $Root 'palette.tga'), $l.ToArray())
$k = (New-TgaHeader 10 4000 4000 32 0x28) + [byte[]](0x85, 1, 2)
[System.IO.File]::WriteAllBytes((Join-Path $Root 'kaputt.tga'), $k)

# PDF mit peek selbst (Kontaktabzug aus den Testbildern)
$p = Start-Process -FilePath $Exe -ArgumentList @('--kontaktabzug-test', "`"$(Join-Path $Root 'dokument.pdf')`"", '0', '2', '2',
    "`"$(Join-Path $Root 'foto.jpg')`"", "`"$(Join-Path $Root 'grafik.png')`"") -Wait -PassThru
Write-Host "PDF erzeugt: $($p.ExitCode)"

# Video mit ffmpeg (auf dem CI-Runner bei Bedarf ueber Chocolatey installiert)
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -and (Get-Command choco -ErrorAction SilentlyContinue)) {
    choco install ffmpeg -y --no-progress | Out-Null
    $env:Path += ";$env:ProgramData\chocolatey\bin"
}
if (Get-Command ffmpeg -ErrorAction SilentlyContinue) {
    & ffmpeg -loglevel error -f lavfi -i testsrc=duration=3:size=640x360:rate=25 -pix_fmt yuv420p -c:v libx264 (Join-Path $Root 'clip.mp4')
    & ffmpeg -loglevel error -f lavfi -i testsrc=duration=2:size=320x240:rate=25 -c:v wmv2 (Join-Path $Root 'alt.wmv')
    Write-Host "Video erzeugt"
} else {
    Write-Host "ffmpeg nicht vorhanden: kein Testvideo"
}

Get-ChildItem $Root | Format-Table Name, Length
