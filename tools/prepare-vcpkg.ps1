# Bereitet das vcpkg des GitHub-Runners vor:
# 1. auf den festen Stand aus vcpkg.json (builtin-baseline) setzen
# 2. Korrektur fuer dav1d einspielen (siehe unten)
# 3. vcpkg neu bootstrappen
#
# Aufruf in den Workflows: ./tools/prepare-vcpkg.ps1

$ErrorActionPreference = 'Stop'
$root = $env:VCPKG_INSTALLATION_ROOT
if (-not $root) { throw "VCPKG_INSTALLATION_ROOT ist nicht gesetzt" }

if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
    New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
}

$b = (Get-Content (Join-Path $PSScriptRoot '..\vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
git -C $root fetch -q origin $b
if ($LASTEXITCODE -ne 0) { throw "vcpkg-Stand $b nicht abrufbar" }
git -C $root -c advice.detachedHead=false checkout -q -f $b
if ($LASTEXITCODE -ne 0) { throw "vcpkg-Stand $b nicht auscheckbar" }

# dav1d: Die Kompatibilitaetsdatei include/compat/msvc/stdatomic.h nutzt __has_include_next,
# das MSVC nicht kennt. Bis MSVC 14.51 war das nur eine Warnung, ab 14.52 ist es ein Fehler
# (C7780). Die Abfrage betrifft nur clang-cl; fuer MSVC wird sie durch 0 ersetzt, also genau
# der Zweig, den MSVC bisher schon genommen hat. Ist die Stelle nicht (mehr) vorhanden, aendert
# die Korrektur nichts.
# Weil vcpkg.json eine feste Baseline hat, nimmt vcpkg die Ports aus seiner Versionsdatenbank und
# nicht aus ports\. Die Korrektur steckt deshalb in einem Overlay-Port (Kopie des Ports dieses
# Stands), den vcpkg ueber VCPKG_OVERLAY_PORTS bevorzugt.
$overlay = Join-Path ($(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() })) 'peek-vcpkg-overlay'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $overlay
New-Item -ItemType Directory -Force -Path $overlay | Out-Null
Copy-Item -Recurse -Path (Join-Path $root 'ports\dav1d') -Destination (Join-Path $overlay 'dav1d')
$portfile = Join-Path $overlay 'dav1d\portfile.cmake'
$lines = [System.Collections.Generic.List[string]](Get-Content $portfile)
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*vcpkg_from_\w+\s*\(') { $start = $i; break } }
$end = -1
if ($start -ge 0) {
    for ($i = $start; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\)\s*$') { $end = $i; break } }
}
if ($end -lt 0) { throw "dav1d-Korrektur: Einfuegestelle im Portfile nicht gefunden" }
$fix = @(
    '# peek: __has_include_next ist fuer MSVC unbekannt (ab MSVC 14.52 ein Fehler)'
    'set(_peek_sa "${SOURCE_PATH}/include/compat/msvc/stdatomic.h")'
    'if(EXISTS "${_peek_sa}")'
    '    file(READ "${_peek_sa}" _peek_sa_text)'
    '    string(REGEX REPLACE "__has_include_next\\([^)]*\\)" "0" _peek_sa_text "${_peek_sa_text}")'
    '    file(WRITE "${_peek_sa}" "${_peek_sa_text}")'
    '    message(STATUS "peek: dav1d stdatomic.h fuer MSVC angepasst")'
    'endif()'
)
$lines.InsertRange($end + 1, [string[]]$fix)
Set-Content -Path $portfile -Value $lines -Encoding utf8NoBOM
Write-Host "dav1d-Overlay mit Korrektur: $overlay (eingefuegt nach Zeile $($end + 1))"
$env:VCPKG_OVERLAY_PORTS = $overlay
if ($env:GITHUB_ENV) { "VCPKG_OVERLAY_PORTS=$overlay" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }

& (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics | Out-Null
if ($LASTEXITCODE -ne 0) { throw "vcpkg-Bootstrap fehlgeschlagen" }
