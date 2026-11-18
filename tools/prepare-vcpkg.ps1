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
$portfile = Join-Path $root 'ports\dav1d\portfile.cmake'
$lines = [System.Collections.Generic.List[string]](Get-Content $portfile)
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*vcpkg_from_\w+\s*\(') { $start = $i; break } }
$end = -1
if ($start -ge 0) {
    for ($i = $start; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\)\s*$') { $end = $i; break } }
}
if ($end -lt 0) {
    Write-Host "::warning::dav1d-Korrektur: Einfuegestelle im Portfile nicht gefunden, nicht angewendet"
} else {
    $fix = @(
        '# peek: __has_include_next ist fuer MSVC unbekannt (ab MSVC 14.52 ein Fehler)'
        'set(_peek_sa "${SOURCE_PATH}/include/compat/msvc/stdatomic.h")'
        'if(EXISTS "${_peek_sa}")'
        '    file(READ "${_peek_sa}" _peek_sa_text)'
        '    string(REGEX REPLACE "__has_include_next\\([^)]*\\)" "0" _peek_sa_text "${_peek_sa_text}")'
        '    file(WRITE "${_peek_sa}" "${_peek_sa_text}")'
        'endif()'
    )
    $lines.InsertRange($end + 1, [string[]]$fix)
    Set-Content -Path $portfile -Value $lines -Encoding utf8NoBOM
    Write-Host "dav1d-Korrektur eingefuegt nach Zeile $($end + 1) von $portfile"
}

& (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics | Out-Null
if ($LASTEXITCODE -ne 0) { throw "vcpkg-Bootstrap fehlgeschlagen" }
