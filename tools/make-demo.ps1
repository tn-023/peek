# Erzeugt einen Beispielordner mit Bildern, Unterordnern und sonstigen Dateien (fuer Screenshots und Tests).
param(
    [Parameter(Mandatory = $true)][string]$Root
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$rand = New-Object System.Random(42)
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function New-DemoImage([string]$path, [int]$w, [int]$h, [string]$label, [int]$seed) {
    $r = New-Object System.Random($seed)
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $c1 = [System.Drawing.Color]::FromArgb(255, $r.Next(20, 120), $r.Next(60, 160), $r.Next(120, 230))
    $c2 = [System.Drawing.Color]::FromArgb(255, $r.Next(180, 255), $r.Next(120, 220), $r.Next(60, 160))
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, [single]($r.Next(30, 120)))
    $g.FillRectangle($grad, $rect)
    # Sonne
    $sun = [System.Drawing.Color]::FromArgb(220, 255, 240, 200)
    $sr = [int]([Math]::Min($w, $h) * 0.12)
    $g.FillEllipse((New-Object System.Drawing.SolidBrush($sun)), [int]($w * 0.7), [int]($h * 0.15), $sr * 2, $sr * 2)
    # Berge
    for ($layer = 0; $layer -lt 3; $layer++) {
        $pts = New-Object System.Collections.Generic.List[System.Drawing.PointF]
        $base = $h * (0.55 + 0.12 * $layer)
        $pts.Add((New-Object System.Drawing.PointF(0, $h)))
        for ($x = 0; $x -le $w; $x += [Math]::Max(20, [int]($w / 8))) {
            $pts.Add((New-Object System.Drawing.PointF($x, [single]($base - $r.Next(0, [int]($h * 0.25))))))
        }
        $pts.Add((New-Object System.Drawing.PointF($w, $h)))
        $shade = 40 + 30 * $layer
        $col = [System.Drawing.Color]::FromArgb(235, [Math]::Min(255, $c1.R / 2 + $shade / 3), [Math]::Min(255, $c1.G / 2 + $shade / 2), [Math]::Min(255, $c1.B / 2 + $shade / 3))
        $g.FillPolygon((New-Object System.Drawing.SolidBrush($col)), $pts.ToArray())
    }
    # Beschriftung
    $font = New-Object System.Drawing.Font('Segoe UI Semibold', [single]([Math]::Max(14, $h * 0.045)))
    $g.DrawString($label, $font, (New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(230, 255, 255, 255))), [single]($w * 0.04), [single]($h * 0.05))
    $g.Dispose()
    $ext = [System.IO.Path]::GetExtension($path).ToLowerInvariant()
    $fmt = if ($ext -eq '.png') { [System.Drawing.Imaging.ImageFormat]::Png } else { [System.Drawing.Imaging.ImageFormat]::Jpeg }
    $bmp.Save($path, $fmt)
    $bmp.Dispose()
}

$i = 0
function Add([string]$dir, [string]$name, [int]$w, [int]$h, [string]$label, [int]$daysAgo) {
    $script:i++
    $p = Join-Path $dir $name
    New-DemoImage $p $w $h $label (1000 + $script:i)
    $t = (Get-Date).AddDays(-$daysAgo).AddMinutes(-$script:i * 7)
    (Get-Item $p).LastWriteTime = $t
    (Get-Item $p).CreationTime = $t
}

# Hauptordner
foreach ($n in 30..37) { Add $Root ("IMG_20240817_14{0}.jpg" -f $n) 1600 1200 ("IMG $n") (90 + $n) }
foreach ($n in 1..5) { Add $Root ("Urlaub Rom 2024 ({0}).jpg" -f $n) 1920 1080 ("Rom $n") (200 + $n) }
foreach ($n in 12..14) { Add $Root ("Screenshot 2024-08-17 1015{0}.png" -f $n) 1280 800 ("Screenshot") 5 }
foreach ($n in 4101..4104) { Add $Root ("DSC0{0}.jpg" -f $n) 1080 1440 ("DSC $n") (400 + $n % 10) }
Add $Root 'Familienfeier_Sommer_im_Garten_mit_einem_sehr_langen_Dateinamen_zum_Testen_des_Umbruchs.jpg' 1600 1067 'Familienfeier' 2
Add $Root 'Panorama Dolomiten.jpg' 3200 1000 'Panorama' 30
Add $Root 'Hochformat Portrait.jpg' 1000 1500 'Portrait' 1

# Unterordner (Ordner-Vorschaubilder)
foreach ($folder in @('Rom', 'Paris', 'Berge 2023')) {
    $d = Join-Path $Root $folder
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    foreach ($n in 1..6) { Add $d ("{0}_{1:00}.jpg" -f ($folder -replace ' ', '_'), $n) 1600 1067 "$folder $n" (60 + $n) }
}

# SVG-Grafiken (Text, Filter, Maske, CSS)
Set-Content -Path (Join-Path $Root 'Plakat Vektor.svg') -Encoding UTF8 -Value @'
<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="800" viewBox="0 0 1200 800">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#1e3c72"/><stop offset="1" stop-color="#2a5298"/>
    </linearGradient>
    <radialGradient id="sun" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#ffe9a8"/><stop offset="1" stop-color="#ffb347"/>
    </radialGradient>
    <filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="18"/></filter>
    <mask id="fade"><rect width="1200" height="800" fill="url(#fadeGrad)"/></mask>
    <linearGradient id="fadeGrad" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/>
    </linearGradient>
  </defs>
  <rect width="1200" height="800" fill="url(#bg)"/>
  <circle cx="860" cy="260" r="150" fill="#ffb347" filter="url(#soft)" opacity="0.8"/>
  <circle cx="860" cy="260" r="110" fill="url(#sun)"/>
  <path d="M0 620 L260 380 L420 520 L640 300 L900 560 L1200 420 L1200 800 L0 800 Z" fill="#0b1d3a" mask="url(#fade)"/>
  <text x="80" y="160" font-family="Segoe UI" font-size="96" font-weight="600" fill="white">Vektorgrafik</text>
  <text x="84" y="220" font-family="Segoe UI" font-size="36" fill="#cfe0ff">Text, Verlauf, Weichzeichner und Maske</text>
</svg>
'@
Set-Content -Path (Join-Path $Root 'Symbol Haken.svg') -Encoding UTF8 -Value @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">
  <style>.a{fill:none;stroke:#e8a33d;stroke-width:1.6;stroke-linecap:round}</style>
  <circle class="a" cx="12" cy="12" r="9"/><path class="a" d="M8 13l3 3 5-7"/>
</svg>
'@

# Sonstige Dateien
Set-Content -Path (Join-Path $Root 'Notizen.txt') -Value 'Beispieltext fuer peek' -Encoding UTF8
[System.IO.File]::WriteAllBytes((Join-Path $Root 'Rechnung_2024.pdf'), [byte[]](37, 80, 68, 70, 45, 49, 46, 52, 10))
[System.IO.File]::WriteAllBytes((Join-Path $Root 'Archiv.zip'), [byte[]](80, 75, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))

Write-Host "Beispielordner erzeugt: $Root"
