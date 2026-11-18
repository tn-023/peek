# Baut resvg (SVG-Renderer, Rust) als statische Bibliothek mit C-Schnittstelle fuer peek.
# Ergebnis: <Out>\include\resvg.h und <Out>\lib\resvg.lib (statische C-Laufzeit wie peek: +crt-static).
# Voraussetzung: Rust (rustup, Ziel x86_64-pc-windows-msvc) und git. Netzwerk wird nur zum Bauen gebraucht.
param(
    [string]$Out = "deps/resvg",
    [string]$Version = "0.49.0"
)
$ErrorActionPreference = 'Stop'

$Out = [System.IO.Path]::GetFullPath($Out)
if ((Test-Path "$Out/lib/resvg.lib") -and (Test-Path "$Out/include/resvg.h") -and (Test-Path "$Out/THIRD-PARTY-rust.txt") -and (Test-Path "$Out/THIRD-PARTY-rust.txt") -and
    ((Get-Content "$Out/version.txt" -ErrorAction SilentlyContinue) -eq $Version)) {
    Write-Host "resvg $Version bereits vorhanden: $Out"
    exit 0
}

$src = "$Out-src"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "v$Version" https://github.com/linebender/resvg $src
if ($LASTEXITCODE -ne 0) { throw "git clone fehlgeschlagen" }

rustup target add x86_64-pc-windows-msvc | Out-Null
# Statische C-Laufzeit; Build-Pfade (Benutzerordner des Build-Rechners, Quellordner) in Fehlermeldungen der
# Rust-Pakete durch neutrale Namen ersetzen, damit die EXE nichts ueber den Build-Rechner verraet.
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
$flags = @('-C', 'target-feature=+crt-static', "--remap-path-prefix=$cargoHome=cargo", "--remap-path-prefix=$src=resvg")
Remove-Item Env:RUSTFLAGS -ErrorAction SilentlyContinue
$env:CARGO_ENCODED_RUSTFLAGS = $flags -join [char]0x1f
# Funktionen: Text mit Systemschriften, eingebettete Rasterbilder; kein SVGZ (Verweise in komprimierten Dateien
# koennten nicht vorab auf lokale Pfade geprueft werden)
cargo build --locked --profile production --target x86_64-pc-windows-msvc `
    --manifest-path "$src/crates/c-api/Cargo.toml" `
    --no-default-features --features "text,system-fonts,memmap-fonts,raster-images"
if ($LASTEXITCODE -ne 0) { throw "cargo build fehlgeschlagen" }

# benoetigte Systembibliotheken anzeigen (nur zur Information)
cargo rustc --locked --profile production --target x86_64-pc-windows-msvc --manifest-path "$src/crates/c-api/Cargo.toml" `
    --no-default-features --features "text,system-fonts,memmap-fonts,raster-images" --crate-type staticlib `
    -- --print native-static-libs 2>&1 | Select-String "native-static-libs" | ForEach-Object { Write-Host $_ }

New-Item -ItemType Directory -Force -Path "$Out/lib", "$Out/include" | Out-Null

# Lizenzen aller mitgelinkten Rust-Pakete sammeln (fuer THIRD-PARTY.txt)
$meta = cargo metadata --locked --format-version 1 --manifest-path "$src/crates/c-api/Cargo.toml" `
    --filter-platform x86_64-pc-windows-msvc --no-default-features `
    --features "text,system-fonts,memmap-fonts,raster-images" | ConvertFrom-Json
$used = @{}
foreach ($node in $meta.resolve.nodes) { $used[$node.id] = $true }
$sb = New-Object System.Text.StringBuilder
$seen = @{}
$count = 0
foreach ($pkg in ($meta.packages | Where-Object { $used[$_.id] } | Sort-Object name)) {
    $dir = Split-Path $pkg.manifest_path -Parent
    $count++
    [void]$sb.AppendLine(('=' * 78))
    [void]$sb.AppendLine("$($pkg.name) $($pkg.version)  -  Lizenz: $($pkg.license)")
    if ($pkg.repository) { [void]$sb.AppendLine($pkg.repository) }
    [void]$sb.AppendLine(('=' * 78))
    $files = Get-ChildItem -Path $dir -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|COPYRIGHT|NOTICE|UNLICENSE)' }
    if (-not $files) {
        # Workspace-Pakete (resvg, usvg, ...): Lizenzdateien im Wurzelverzeichnis
        $files = Get-ChildItem -Path $src -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' }
    }
    foreach ($f in $files) {
        # gleiche Lizenztexte nur einmal ausschreiben (z. B. Apache-2.0 bei vielen Paketen)
        $text = (Get-Content $f.FullName -Raw)
        $norm = ($text -replace '\s+', ' ').Trim()
        if ($seen.ContainsKey($norm)) {
            [void]$sb.AppendLine("--- $($f.Name): Wortlaut wie bei $($seen[$norm]) ---")
        } else {
            $seen[$norm] = "$($pkg.name) ($($f.Name))"
            [void]$sb.AppendLine("--- $($f.Name) ---")
            [void]$sb.AppendLine($text)
        }
    }
    [void]$sb.AppendLine()
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Set-Content -Path "$Out/THIRD-PARTY-rust.txt" -Value $sb.ToString() -Encoding UTF8
Write-Host "Rust-Pakete mit Lizenzhinweis: $count"

Copy-Item "$src/target/x86_64-pc-windows-msvc/production/resvg.lib" "$Out/lib/resvg.lib" -Force
Copy-Item "$src/crates/c-api/resvg.h" "$Out/include/resvg.h" -Force
Copy-Item "$src/LICENSE-MIT" "$Out/LICENSE-MIT" -Force -ErrorAction SilentlyContinue
Set-Content -Path "$Out/version.txt" -Value $Version -NoNewline
Remove-Item -Recurse -Force $src
Write-Host "resvg $Version gebaut: $Out"
