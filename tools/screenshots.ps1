# Startet peek in verschiedenen Zustaenden und speichert Bildschirmfotos (fuer die Dokumentation).
# -Lang de|en: Sprache der Oberflaeche; der Beispielordner muss mit make-demo.ps1 -Lang <gleich> erzeugt sein.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateSet('de', 'en')][string]$Lang = 'de'
)
$en = $Lang -eq 'en'
function N([string]$de, [string]$english) { if ($en) { $english } else { $de } }

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
}
"@

New-Item -ItemType Directory -Force -Path $Out | Out-Null

try { Set-DisplayResolution -Width 1920 -Height 1080 -Force -ErrorAction Stop } catch { Write-Host "Aufloesung nicht aenderbar: $_" }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
$wa = $screen.WorkingArea
Write-Host "Bildschirm: $($screen.Bounds.Width)x$($screen.Bounds.Height), Arbeitsbereich $($wa.Width)x$($wa.Height)"
$size = "--size=$($wa.Width)x$($wa.Height)"

function Shot([string]$name, [string[]]$arguments, [int]$wait = 8, [switch]$FullScreen) {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:LOCALAPPDATA 'peek\einstellungen.dat')
    $p = Start-Process -FilePath $Exe -ArgumentList ($arguments + "--lang=$Lang") -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300; $p.Refresh() }
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) { Write-Warning "$name : kein Fenster"; Stop-Process $p -Force; return }
    [Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Seconds $wait
    if ($FullScreen) {
        $b = $screen.Bounds
        $x = $b.Left; $y = $b.Top; $w = $b.Width; $h = $b.Height
    } else {
        $r = New-Object Win+RECT
        if ([Win]::DwmGetWindowAttribute($p.MainWindowHandle, 9, [ref]$r, 16) -ne 0) { [Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null }
        $x = $r.Left; $y = $r.Top; $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $file = Join-Path $Out $name
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Gespeichert: $file ($w x $h)"
    Stop-Process $p -Force
    Start-Sleep -Seconds 1
}

$sel = Join-Path $Root (N 'Urlaub Rom 2024 (2).jpg' 'Vacation Rome 2024 (2).jpg')
$svg = Join-Path $Root (N 'Plakat Vektor.svg' 'Poster Vector.svg')
$term = N 'rom' 'rome'
$dark = '--theme=dark'
Shot 'uebersicht.png' @("`"$sel`"", $dark, '--view=grid', '--tile=176', '--info', $size)
Shot 'gruppiert.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', '--group', $size)
Shot 'wolke.png' @("`"$Root`"", $dark, '--view=cloud', $size) -wait 10
Shot 'wolke-auswahl.png' @("`"$Root`"", $dark, '--view=cloud', "`"--cloud-select=$sel`"", $size) -wait 10
# viele Bilder fuer die Bilderwolke
$many = Join-Path $env:TEMP 'peek-Viele'
New-Item -ItemType Directory -Force -Path $many | Out-Null
$rnd = New-Object System.Random(7)
for ($i = 0; $i -lt 1500; $i++) {
    $w = 160; $h = if ($i % 3 -eq 0) { 200 } else { 110 }
    $b = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $c1 = [System.Drawing.Color]::FromArgb(255, $rnd.Next(40, 230), $rnd.Next(40, 230), $rnd.Next(40, 230))
    $c2 = [System.Drawing.Color]::FromArgb(255, $rnd.Next(20, 200), $rnd.Next(20, 200), $rnd.Next(20, 200))
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.Rectangle(0, 0, $w, $h)), $c1, $c2, 45.0)
    $g.FillRectangle($br, 0, 0, $w, $h)
    $g.FillEllipse([System.Drawing.Brushes]::White, [int]($w * 0.6), [int]($h * 0.15), 24, 24)
    $g.Dispose()
    $b.Save((Join-Path $many ('bild_{0:D4}.png' -f $i)), [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
}
Shot 'wolke-viele.png' @("`"$many`"", $dark, '--view=cloud', $size) -wait 25
Shot 'pillen.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', "--term=$term", $size)
Shot 'muster.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', '--open-chip=pattern', $size) -wait 9
Shot 'quelle.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', '--open-chip=source', $size) -wait 9
Shot 'suche.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', "`"--search=$(N 'Breite' 'Width'):>1500 $term`"", '--search-open', $size)
Shot 'svg.png' @("`"--viewer=$svg`"", $dark, $size) -FullScreen -wait 9
Shot 'svg-zoom.png' @("`"--viewer=$svg`"", $dark, '--viewer-zoom=6', $size) -FullScreen -wait 12
Shot 'umbenennen.png' @("`"$sel`"", $dark, '--view=grid', '--tile=144', '--rename', $size) -wait 9
Shot 'umbenennen-liste.png' @("`"$sel`"", $dark, '--view=list', '--rename', $size) -wait 9
$cmpA = Join-Path $Root (N 'Urlaub Rom 2024 (1).jpg' 'Vacation Rome 2024 (1).jpg'); $cmpB = $sel
Shot 'vergleich.png' @("`"--compare=$cmpA;$cmpB`"", $dark, $size) -FullScreen -wait 9
Shot 'vergleich-ueber.png' @("`"--compare=$cmpA;$cmpB`"", '--compare-mode=1', $dark, $size) -FullScreen -wait 9
# Kalender (Datumsfilter "Individuell") in Nahaufnahme
Shot 'kalender.png' @("`"$Root`"", $dark, '--view=grid', '--tile=144', '--date-range=2024-08-14..2024-08-20', '--open-datepicker', $size) -wait 9
# mittelgrosse Wolke, etwas hereingezoomt
$mid = Join-Path $env:TEMP 'peek-Wolke'
New-Item -ItemType Directory -Force -Path $mid | Out-Null
$rm = New-Object System.Random(11)
for ($i = 0; $i -lt 260; $i++) {
    $w = if ($i % 4 -eq 0) { 420 } else { 640 }; $h = if ($i % 4 -eq 0) { 560 } else { 420 }
    $b = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $c1 = [System.Drawing.Color]::FromArgb(255, $rm.Next(20, 140), $rm.Next(60, 170), $rm.Next(110, 235))
    $c2 = [System.Drawing.Color]::FromArgb(255, $rm.Next(170, 255), $rm.Next(110, 220), $rm.Next(60, 170))
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, [single]$rm.Next(30, 120))), $rect)
    $sr = [int]([Math]::Min($w, $h) * 0.11)
    $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(225, 255, 240, 205))), [int]($w * $rm.Next(15, 75) / 100), [int]($h * 0.14), $sr * 2, $sr * 2)
    for ($layer = 0; $layer -lt 3; $layer++) {
        $pts = New-Object System.Collections.Generic.List[System.Drawing.PointF]
        $base = $h * (0.55 + 0.12 * $layer)
        $pts.Add((New-Object System.Drawing.PointF(0, $h)))
        for ($x = 0; $x -le $w; $x += [int]($w / 7)) { $pts.Add((New-Object System.Drawing.PointF($x, [single]($base - $rm.Next(0, [int]($h * 0.22)))))) }
        $pts.Add((New-Object System.Drawing.PointF($w, $h)))
        $sh = 40 + 30 * $layer
        $col = [System.Drawing.Color]::FromArgb(235, [Math]::Min(255, $c1.R / 2 + $sh / 3), [Math]::Min(255, $c1.G / 2 + $sh / 2), [Math]::Min(255, $c1.B / 2 + $sh / 3))
        $g.FillPolygon((New-Object System.Drawing.SolidBrush($col)), $pts.ToArray())
    }
    $g.Dispose()
    $b.Save((Join-Path $mid ('Reise_{0:D3}.jpg' -f $i)), [System.Drawing.Imaging.ImageFormat]::Jpeg)
    $b.Dispose()
}
Shot 'wolke-nah.png' @("`"$mid`"", $dark, '--view=cloud', '--cloud-zoom=0.55', $size) -wait 14

# Kamerafahrt durch die Wolke als Video und GIF (README): peek startet die Fahrt selbst (--cloud-tour)
# und legt beim Start eine Markerdatei an; dann nimmt ffmpeg genau das Fenster auf.
function Record-Tour([string]$folder, [string]$name) {
    if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { Write-Warning 'ffmpeg fehlt: keine Aufnahme'; return }
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:LOCALAPPDATA 'peek\einstellungen.dat')
    $mark = Join-Path $env:TEMP 'peek-tour.start'
    Remove-Item -Force -ErrorAction SilentlyContinue $mark
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(($screen.Bounds.Right - 1), ($screen.Bounds.Bottom - 1))
    $p = Start-Process -FilePath $Exe -ArgumentList @("`"$folder`"", $dark, '--view=cloud', '--size=1280x760', "`"--cloud-tour=$mark`"", "--lang=$Lang") -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    [Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    $r = New-Object Win+RECT
    if ([Win]::DwmGetWindowAttribute($p.MainWindowHandle, 9, [ref]$r, 16) -ne 0) { [Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null }
    $w = ($r.Right - $r.Left) - (($r.Right - $r.Left) % 2); $h = ($r.Bottom - $r.Top) - (($r.Bottom - $r.Top) % 2)
    $deadline = (Get-Date).AddSeconds(40)
    while (-not (Test-Path $mark) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 40 }
    if (-not (Test-Path $mark)) { Write-Warning "$name : Fahrt nicht gestartet"; Stop-Process $p -Force; return }
    $raw = Join-Path $env:TEMP "$name-roh.mp4"
    & ffmpeg -loglevel error -y -f gdigrab -framerate 30 -draw_mouse 0 -offset_x $r.Left -offset_y $r.Top -video_size "${w}x${h}" -t 16.5 -i desktop `
        -c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p $raw
    Stop-Process $p -Force
    Start-Sleep -Seconds 1
    # Video fuer die Website (H.264) und GIF fuer das README; GIF moeglichst unter 5 MB
    & ffmpeg -loglevel error -y -i $raw -vf 'scale=1280:-2' -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -movflags +faststart (Join-Path $Out "$name.mp4")
    $gif = Join-Path $Out "$name.gif"
    foreach ($cfg in @(@(800, 15), @(720, 15), @(720, 12), @(640, 12))) {
        $vf = "fps=$($cfg[1]),scale=$($cfg[0]):-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle"
        & ffmpeg -loglevel error -y -i $raw -vf $vf -loop 0 $gif
        $mb = (Get-Item $gif).Length / 1MB
        Write-Host ("{0}: {1} px, {2} fps -> {3:N1} MB" -f $name, $cfg[0], $cfg[1], $mb)
        if ($mb -lt 5) { break }
    }
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -and (Get-Command choco -ErrorAction SilentlyContinue)) {
    choco install ffmpeg -y --no-progress | Out-Null
    $env:Path += ";$env:ProgramData\chocolatey\bin"
}
Record-Tour $mid 'wolke-fahrt'
Shot 'liste.png' @("`"$Root`"", $dark, '--view=list', $size)
Shot 'betrachter.png' @("`"--viewer=$sel`"", $dark, $size) -FullScreen
Shot 'zuschneiden.png' @("`"--viewer=$sel`"", '--crop', $dark, $size) -FullScreen
# PNG-Sequenz fuer die Kacheltextur: ein springender Ball mit Transparenz
$seq = Join-Path $env:TEMP 'peek-Sequenz\Ball'
New-Item -ItemType Directory -Force -Path $seq | Out-Null
for ($i = 0; $i -lt 24; $i++) {
    $b = New-Object System.Drawing.Bitmap(256, 256, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $t = $i / 24.0
    $y = 120 * [Math]::Abs([Math]::Sin([Math]::PI * $t))
    $squash = if ($y -lt 12) { 0.82 } else { 1.0 }
    $w = 76 / $squash; $h = 76 * $squash
    $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(60, 0, 0, 0))), 128 - 40, 226, 80, 14)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF((128 - $w / 2), (226 - $y - $h), $w, $h)),
        [System.Drawing.Color]::FromArgb(255, 255, 196, 64), [System.Drawing.Color]::FromArgb(255, 226, 80, 40), 60.0)
    $g.FillEllipse($br, [float](128 - $w / 2), [float](226 - $y - $h), [float]$w, [float]$h)
    $g.Dispose()
    $b.Save((Join-Path $seq ('ball_{0:D4}.png' -f $i)), [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
}
Shot 'kacheltextur.png' @("`"$seq`"", $dark, '--atlas', $size)
Shot 'kontaktabzug.png' @("`"$Root`"", $dark, '--sheet', $size)
Shot 'einstellungen.png' @("`"$Root`"", $dark, '--settings', $size)
