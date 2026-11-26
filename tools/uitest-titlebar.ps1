# Prueft die Titelleiste am echten Fenster: Treffer (WM_NCHITTEST), die drei Fensterknoepfe mit echten Mausklicks
# (normal und maximiert), Doppelklick, Verschieben, Systemmenue; dazu Bildschirmfotos. -Native: mit der Titelleiste
# von Windows (Vergleich). Ergebnis: Protokoll und Bilder in -Out; Fehler werden gezaehlt und am Ende gemeldet.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Out,
    [switch]$Native
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
    public static int Hit(IntPtr h, int x, int y) {
        return (int)SendMessageW(h, 0x0084, IntPtr.Zero, (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)));
    }
}
"@
[W]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$tag = if ($Native) { 'native' } else { 'eigen' }
$script:fail = 0
function Log([string]$s) { Write-Host $s; Add-Content -Path (Join-Path $Out "$tag.log") -Value $s }
function Check([string]$what, [bool]$ok) {
    if ($ok) { Log "  OK    $what" } else { Log "  FEHLER $what"; $script:fail++ }
}
function Frame($h) {   # sichtbare Fensterflaeche (ohne Schatten)
    $r = New-Object W+RECT
    if ([W]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) -ne 0) { [W]::GetWindowRect($h, [ref]$r) | Out-Null }
    return $r
}
function Shot($h, [string]$name, [int]$extra = 0) {
    $r = Frame $h
    $x = [Math]::Max(0, $r.Left - $extra); $y = [Math]::Max(0, $r.Top - $extra)
    $w = $r.Right - $r.Left + 2 * $extra; $hh = $r.Bottom - $r.Top + 2 * $extra
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $g.Dispose()
    $bmp.Save((Join-Path $Out "$tag-$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function MouseTo([int]$x, [int]$y) { [W]::SetCursorPos($x, $y) | Out-Null; [W]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 250 }
function Click([int]$x, [int]$y, [switch]$Right, [switch]$Double) {
    MouseTo $x $y
    $down = if ($Right) { 8 } else { 2 }; $up = if ($Right) { 16 } else { 4 }
    [W]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 70
    [W]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
    if ($Double) {
        Start-Sleep -Milliseconds 90
        [W]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 70
        [W]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 900
}
function Key([byte]$vk) { [W]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 700 }
$names = @{ 0 = '.'; 1 = 'c'; 2 = 'T'; 3 = 'S'; 8 = 'm'; 9 = 'X'; 12 = '^'; 13 = '<'; 14 = '>'; 20 = 'Q'; 10 = '['; 11 = ']'; 15 = '_' }
function Probe($h, [string]$label) {
    # Zeichen: T Titel, S Systemmenue, m Minimieren, X Maximieren, Q Schliessen, ^ oberer Rand, < > Ecken, c Client
    $r = Frame $h
    $wr = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$wr) | Out-Null
    $cr = New-Object W+RECT; [W]::GetClientRect($h, [ref]$cr) | Out-Null
    $o = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o) | Out-Null
    $b = New-Object W+RECT; $hr = [W]::DwmGetWindowAttribute($h, 5, [ref]$b, 16)
    Log "$label : sichtbar $($r.Left),$($r.Top)-$($r.Right),$($r.Bottom)  Fenster $($wr.Left),$($wr.Top)-$($wr.Right),$($wr.Bottom)  Client ab $($o.X),$($o.Y) Groesse $($cr.Right)x$($cr.Bottom)  maximiert=$([W]::IsZoomed($h))"
    Log "  Knopfbereich laut DWM (hr=$hr, fensterbezogen): $($b.Left),$($b.Top)-$($b.Right),$($b.Bottom)"
    foreach ($dy in 0, 2, 5, 9, 14, 19, 24, 28, 31, 35, 44) {
        $y = [Math]::Max($r.Top, 0) + $dy
        $left = ''; foreach ($dx in 2, 10, 18, 24, 30, 40, 80, 200) { $c = [W]::Hit($h, $r.Left + $dx, $y); $left += $(if ($names.ContainsKey($c)) { $names[$c] } else { "($c)" }) }
        $right = ''; for ($dx = 168; $dx -ge 2; $dx -= 6) { $c = [W]::Hit($h, $r.Right - $dx, $y); $right += $(if ($names.ContainsKey($c)) { $names[$c] } else { "($c)" }) }
        Log ("  y+{0,2}: links {1}   rechts {2}" -f $dy, $left, $right)
    }
}
function Start-Peek([string[]]$arguments, [string]$theme = 'dark') {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:LOCALAPPDATA 'peek\einstellungen.dat')
    $all = $arguments + '--lang=de' + "--theme=$theme"
    if ($Native) { $all += '--native-frame' }
    $p = Start-Process -FilePath $Exe -ArgumentList $all -PassThru
    $deadline = (Get-Date).AddSeconds(25)
    while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300; $p.Refresh() }
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 'kein Fenster' }
    [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Seconds 5
    return $p
}
# Mitte der drei Knoepfe (46 breit) ab der rechten sichtbaren Kante, 15 Pixel unter der sichtbaren Oberkante
function Btn($h, [int]$i) { $r = Frame $h; return @(($r.Right - 23 - 46 * (2 - $i)), ([Math]::Max($r.Top, 0) + 15)) }

try { Set-DisplayResolution -Width 1920 -Height 1080 -Force -ErrorAction Stop } catch { Write-Host "Aufloesung nicht aenderbar: $_" }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
Log "== $tag == Bildschirm $($screen.Bounds.Width)x$($screen.Bounds.Height), Arbeitsbereich $($screen.WorkingArea.Width)x$($screen.WorkingArea.Height)"

function ShotTop($h, [string]$name) {   # nur die oberen 60 Pixel rechts (600 breit)
    $r = Frame $h
    $bmp = New-Object System.Drawing.Bitmap(600, 60)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Right - 600, [Math]::Max($r.Top, 0), 0, 0, (New-Object System.Drawing.Size(600, 60)))
    $g.Dispose()
    $bmp.Save((Join-Path $Out "$tag-$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
# aktiv und im Hintergrund (inaktiv, Titel blasser), dunkel und hell
function ShotLeft($h, [string]$name) {
    $r = Frame $h
    $bmp = New-Object System.Drawing.Bitmap(400, 40); $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size(400, 40))); $g.Dispose()
    $bmp.Save((Join-Path $Out "$tag-$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
foreach ($theme in 'dark', 'light') {
    $p = Start-Peek @("`"$Root`"", '--view=grid', '--size=1000x680') $theme
    $h = $p.MainWindowHandle
    ShotLeft $h "aktiv-$theme-links"; ShotTop $h "aktiv-$theme-rechts"
    $c = Btn $h 1; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 900; ShotTop $h "ueber-max-$theme"
    $r = Frame $h; MouseTo ($r.Left + 300) ($r.Top + 300)
    $np = Start-Process -FilePath notepad.exe -PassThru
    Start-Sleep -Seconds 3
    ShotLeft $h "inaktiv-$theme-links"
    Stop-Process $np -Force -ErrorAction SilentlyContinue
    Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Stop-Process $p -Force
    Start-Sleep -Seconds 1
}

# ---- 1. Ordneransicht, nicht maximiert
$p = Start-Peek @("`"$Root`"", '--view=grid', '--size=1000x680')
$h = $p.MainWindowHandle
Shot $h 'normal' 12
Probe $h 'normal'
$r = Frame $h
$c = Btn $h 1; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 1200; Shot $h 'normal-ueber-max' 12
$c = Btn $h 2; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 600; Shot $h 'normal-ueber-schliessen' 12
$c = Btn $h 0; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 600; Shot $h 'normal-ueber-min' 12
MouseTo ($r.Left + 400) ($r.Top + 300)

Log 'Titelleiste, nicht maximiert:'
$c = Btn $h 1; Click $c[0] $c[1]
Check 'Klick auf Maximieren maximiert' ([W]::IsZoomed($h))
Shot $h 'maximiert'
Probe $h 'maximiert'
$c = Btn $h 1; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 1200; Shot $h 'maximiert-ueber-max'
$c = Btn $h 2; MouseTo $c[0] $c[1]; Start-Sleep -Milliseconds 600; Shot $h 'maximiert-ueber-schliessen'
$c = Btn $h 1; Click $c[0] $c[1]
Check 'Klick auf Wiederherstellen (maximiert) stellt wieder her' (-not [W]::IsZoomed($h))
if ([W]::IsZoomed($h)) { [W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Seconds 1 }
$r = Frame $h
Click ($r.Left + 500) ($r.Top + 15) -Double
Check 'Doppelklick auf die Titelleiste maximiert' ([W]::IsZoomed($h))
$r = Frame $h
Click ($r.Left + 500) ([Math]::Max($r.Top, 0) + 12) -Double
Check 'Doppelklick auf die Titelleiste (maximiert) stellt wieder her' (-not [W]::IsZoomed($h))
if ([W]::IsZoomed($h)) { [W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Seconds 1 }
$c = Btn $h 0; Click $c[0] $c[1]
Check 'Klick auf Minimieren minimiert' ([W]::IsIconic($h))
[W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Seconds 1; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Seconds 1
# verschieben
$r = Frame $h
MouseTo ($r.Left + 500) ($r.Top + 15)
[W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
for ($i = 1; $i -le 10; $i++) { [W]::SetCursorPos($r.Left + 500 + 8 * $i, $r.Top + 15 + 5 * $i) | Out-Null; [W]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
[W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 800
$r2 = Frame $h
Check "Ziehen an der Titelleiste verschiebt das Fenster (um $($r2.Left - $r.Left),$($r2.Top - $r.Top))" (($r2.Left - $r.Left) -ge 60 -and ($r2.Top - $r.Top) -ge 30)
# Systemmenue: Klick auf das Symbol, Rechtsklick auf die Titelleiste
$r = Frame $h
Click ($r.Left + 24) ($r.Top + 15); Shot $h 'systemmenue-symbol' 12; Key 0x1B
Click ($r.Left + 500) ($r.Top + 15) -Right; Shot $h 'systemmenue-rechtsklick' 12; Key 0x1B
# Klick in den Inhalt direkt unter der Titelleiste darf das Fenster nicht verschieben oder schliessen
Click ($r.Left + 600) ($r.Top + 200)
Check 'Fenster nach Klick in den Inhalt noch da' ([W]::IsWindow($h) -and -not $p.HasExited)
Shot $h 'normal-nachher' 12

Log 'Titelleiste, maximiert:'
$c = Btn $h 1; Click $c[0] $c[1]
Check 'maximiert' ([W]::IsZoomed($h))
# Vollbild (F11) aus dem maximierten Fenster und zurueck: der Inhalt bleibt an derselben Stelle
$o1 = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o1) | Out-Null
Shot $h 'maximiert-vor-vollbild'
Key 0x7A; Start-Sleep -Seconds 1
Probe $h 'Vollbild (F11) aus dem maximierten Fenster'
$o2 = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o2) | Out-Null
$bmp = New-Object System.Drawing.Bitmap($screen.Bounds.Width, $screen.Bounds.Height); $g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $g.Dispose(); $bmp.Save((Join-Path $Out "$tag-vollbild.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Key 0x7A; Start-Sleep -Seconds 1
Probe $h 'zurueck aus dem Vollbild'
$o3 = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o3) | Out-Null
Shot $h 'maximiert-nach-vollbild'
Log "  Client-Ursprung: maximiert $($o1.X),$($o1.Y)  Vollbild $($o2.X),$($o2.Y)  danach $($o3.X),$($o3.Y)"
Check 'nach dem Vollbild wieder maximiert' ([W]::IsZoomed($h))
if (-not $Native) { Check 'Client-Ursprung im Vollbild wie maximiert (kein Verrutschen)' ($o1.X -eq $o2.X -and $o1.Y -eq $o2.Y -and $o1.X -eq $o3.X -and $o1.Y -eq $o3.Y) }
$c = Btn $h 0; Click $c[0] $c[1]
Check 'Klick auf Minimieren (maximiert) minimiert' ([W]::IsIconic($h))
[W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Seconds 1; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Seconds 1
Check 'nach dem Wiederherstellen wieder maximiert' ([W]::IsZoomed($h))
# ganz oben rechts in der Ecke (Bildschirmrand): Schliessen muss auch dort treffen
$r = Frame $h
Click ($screen.Bounds.Right - 2) 1
$p.Refresh(); Start-Sleep -Seconds 2; $p.Refresh()
Check 'Klick in die Bildschirmecke oben rechts (maximiert) schliesst' ($p.HasExited)
if (-not $p.HasExited) { $c = Btn $h 2; Click $c[0] $c[1]; Start-Sleep -Seconds 2; $p.Refresh(); Check 'Klick auf Schliessen (maximiert) schliesst' ($p.HasExited) }
if (-not $p.HasExited) { Stop-Process $p -Force }

# ---- 2. Schliessen im nicht maximierten Fenster
$p = Start-Peek @("`"$Root`"", '--view=list', '--size=1000x680')
$h = $p.MainWindowHandle
Shot $h 'liste' 12
$c = Btn $h 2; Click $c[0] $c[1]; Start-Sleep -Seconds 2; $p.Refresh()
Check 'Klick auf Schliessen schliesst' ($p.HasExited)
if (-not $p.HasExited) { Stop-Process $p -Force }

# ---- 3. Einstellungen und Bildbetrachter: Titelleiste bzw. Bild bis oben, danach wieder Titelleiste
$p = Start-Peek @("`"$Root`"", '--settings', '--size=1000x680')
Shot $p.MainWindowHandle 'einstellungen' 12
Probe $p.MainWindowHandle 'einstellungen'
Stop-Process $p -Force
$img = Get-ChildItem -Path $Root -File | Where-Object { $_.Extension -in '.jpg', '.png' } | Select-Object -First 1
if ($img) {
    $p = Start-Peek @("`"--viewer=$($img.FullName)`"", '--size=1000x680')
    $h = $p.MainWindowHandle
    $before = Frame $h
    Shot $h 'betrachter' 12
    Probe $h 'betrachter'
    Key 0x1B
    Start-Sleep -Seconds 2
    $after = Frame $h
    Shot $h 'betrachter-geschlossen' 12
    Probe $h 'nach dem Betrachter'
    Check 'Fensterlage beim Schliessen des Betrachters unveraendert' ($before.Left -eq $after.Left -and $before.Top -eq $after.Top -and $before.Right -eq $after.Right -and $before.Bottom -eq $after.Bottom)
    # dasselbe maximiert: Bild oeffnen (Eingabetaste), Client-Flaeche darf sich nicht aendern
    [W]::ShowWindow($h, 3) | Out-Null; Start-Sleep -Seconds 2
    $c1 = New-Object W+RECT; [W]::GetClientRect($h, [ref]$c1) | Out-Null; $q1 = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$q1) | Out-Null
    Shot $h 'maximiert-ordner'
    Key 0x0D; Start-Sleep -Seconds 2
    $c2 = New-Object W+RECT; [W]::GetClientRect($h, [ref]$c2) | Out-Null; $q2 = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$q2) | Out-Null
    Shot $h 'maximiert-betrachter'
    Probe $h 'Betrachter maximiert'
    Key 0x1B; Start-Sleep -Seconds 2
    Shot $h 'maximiert-ordner-danach'
    Log "  Client maximiert: Ordner ab $($q1.X),$($q1.Y) $($c1.Right)x$($c1.Bottom)  Betrachter ab $($q2.X),$($q2.Y) $($c2.Right)x$($c2.Bottom)"
    if (-not $Native) { Check 'Client-Flaeche beim Oeffnen des Betrachters (maximiert) unveraendert' ($q1.X -eq $q2.X -and $q1.Y -eq $q2.Y -and $c1.Right -eq $c2.Right -and $c1.Bottom -eq $c2.Bottom) }
    $c = Btn $h 2; Click $c[0] $c[1]; Start-Sleep -Seconds 2; $p.Refresh()
    Check 'Klick auf Schliessen nach dem Betrachter schliesst' ($p.HasExited)
    if (-not $p.HasExited) { Stop-Process $p -Force }
}
Log "== $tag == Fehler: $script:fail"
if ($script:fail -gt 0) { exit 1 }
