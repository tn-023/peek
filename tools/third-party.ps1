# Erzeugt THIRD-PARTY.txt: Lizenzhinweise aller in peek.exe eingebundenen Fremdbibliotheken.
# Quellen: Copyright-Dateien der vcpkg-Pakete und die von build-resvg.ps1 gesammelten Rust-Lizenzen.
param(
    [string]$VcpkgShare = "build/vcpkg_installed/x64-windows-static/share",
    [string]$Rust = "deps/resvg/THIRD-PARTY-rust.txt",
    [string]$Out = "THIRD-PARTY.txt"
)
$ErrorActionPreference = 'Stop'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("peek - Hinweise zu Fremdbibliotheken")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("peek.exe enthaelt die folgenden Bibliotheken (statisch eingebunden). Ihre Lizenztexte stehen unten.")
[void]$sb.AppendLine("")

# vcpkg-Pakete (ohne reine Hilfsports fuer das Bauen)
$skip = @('vcpkg-cmake', 'vcpkg-cmake-config', 'vcpkg-cmake-get-vars', 'vcpkg-tool-meson', 'vcpkg-pkgconfig-get-modules')
$ports = Get-ChildItem -Path $VcpkgShare -Directory | Where-Object { $skip -notcontains $_.Name -and (Test-Path (Join-Path $_.FullName 'copyright')) } | Sort-Object Name
[void]$sb.AppendLine("C/C++-Bibliotheken (vcpkg): " + (($ports | ForEach-Object Name) -join ', '))
[void]$sb.AppendLine("SVG-Darstellung: resvg und zugehoerige Rust-Pakete")
[void]$sb.AppendLine("")
foreach ($p in $ports) {
    [void]$sb.AppendLine(('=' * 78))
    [void]$sb.AppendLine($p.Name)
    [void]$sb.AppendLine(('=' * 78))
    [void]$sb.AppendLine((Get-Content (Join-Path $p.FullName 'copyright') -Raw))
    [void]$sb.AppendLine()
}
if (Test-Path $Rust) { [void]$sb.Append((Get-Content $Rust -Raw)) } else { throw "Rust-Lizenzen fehlen: $Rust" }
Set-Content -Path $Out -Value $sb.ToString() -Encoding UTF8
Write-Host ("vcpkg-Pakete: " + (($ports | ForEach-Object Name) -join ', '))
Write-Host ("THIRD-PARTY.txt geschrieben ({0:N0} Zeichen, {1} vcpkg-Pakete)" -f $sb.Length, $ports.Count)
