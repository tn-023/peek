# Writes THIRD-PARTY.txt: license notices for all third-party libraries statically linked into peek.exe.
# Sources: the copyright files of the vcpkg ports and the Rust license data collected by build-resvg.ps1.
# Compact form: every component lists its own copyright lines and refers to its license texts by number;
# identical license texts (compared without their copyright lines) are written only once at the end.
param(
    [string]$VcpkgShare = "build/vcpkg_installed/x64-windows-static/share",
    [string]$Rust = "deps/resvg/rust-licenses.json",
    [string]$Out = "THIRD-PARTY.txt"
)
$ErrorActionPreference = 'Stop'

$texts = New-Object System.Collections.Generic.List[string]   # unique license texts, numbered from 1
$index = @{}                                                    # normalized text -> number
# A copyright line: "Copyright (c) 2016 ...", "Copyright 2018 ...", "Copyright © ...", "(c) 2020 ...".
# Template lines such as "Copyright [yyyy] [name of copyright owner]" stay in the license text.
$copyRe = '(?i)^\s*(copyright\s*(\(c\)|©|\d{4})|©\s*\d{4}|\(c\)\s*\d{4})'

function Split-License([string]$text) {
    $copy = New-Object System.Collections.Generic.List[string]
    $body = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($text -split "`r?`n")) {
        if ($l -match $copyRe -and $l -notmatch '\[yyyy\]|\{yyyy\}|<year>') { $copy.Add($l.Trim()) } else { $body.Add($l) }
    }
    return @{ copy = $copy; body = (($body -join "`n").Trim()) }
}

function Add-Text([string]$body) {
    $norm = ($body -replace '\s+', ' ').Trim().ToLowerInvariant()
    if ($norm.Length -eq 0) { return 0 }
    if (-not $index.ContainsKey($norm)) {
        $texts.Add($body)
        $index[$norm] = $texts.Count
    }
    return $index[$norm]
}

$components = New-Object System.Text.StringBuilder
function Add-Component([string]$title, [string]$license, [string]$repo, $files) {
    $copies = New-Object System.Collections.Generic.List[string]
    $refs = New-Object System.Collections.Generic.List[string]
    foreach ($f in $files) {
        $r = Split-License $f.text
        foreach ($c in $r.copy) { if (-not $copies.Contains($c)) { $copies.Add($c) } }
        $n = Add-Text $r.body
        if ($n -gt 0) { $refs.Add("[$n] $($f.name)") }
    }
    [void]$components.AppendLine($title)
    if ($license) { [void]$components.AppendLine("  License: $license") }
    if ($repo) { [void]$components.AppendLine("  Source: $repo") }
    foreach ($c in $copies) { [void]$components.AppendLine("  $c") }
    if ($refs.Count -gt 0) { [void]$components.AppendLine("  License text: " + ($refs -join ', ')) }
    [void]$components.AppendLine()
}

# vcpkg ports (without helper ports that are only used for building)
$skip = @('vcpkg-cmake', 'vcpkg-cmake-config', 'vcpkg-cmake-get-vars', 'vcpkg-tool-meson', 'vcpkg-pkgconfig-get-modules')
$ports = Get-ChildItem -Path $VcpkgShare -Directory |
    Where-Object { $skip -notcontains $_.Name -and (Test-Path (Join-Path $_.FullName 'copyright')) } | Sort-Object Name
foreach ($p in $ports) {
    $text = Get-Content (Join-Path $p.FullName 'copyright') -Raw
    Add-Component $p.Name '' '' @([pscustomobject]@{ name = 'copyright'; text = $text })
}

# Rust packages of resvg
if (-not (Test-Path $Rust)) { throw "Rust license data missing: $Rust" }
# parsed with System.Text.Json (PowerShell's ConvertFrom-Json treats a top-level array inconsistently)
$kind = [System.Text.Json.JsonValueKind]
$doc = [System.Text.Json.JsonDocument]::Parse((Get-Content $Rust -Raw))
$root = $doc.RootElement
if ($root.ValueKind -eq $kind::Object) {   # {"value": [...], "Count": n} from older PowerShell
    foreach ($prop in $root.EnumerateObject()) { if ($prop.Name -eq 'value') { $root = $prop.Value } }
}
function Json-Props($el) {
    $h = @{}
    if ($el.ValueKind -eq $kind::Object) { foreach ($prop in $el.EnumerateObject()) { $h[$prop.Name] = $prop.Value } }
    return $h
}
function Json-Str($h, [string]$name) {
    if ($h.ContainsKey($name) -and $h[$name].ValueKind -eq $kind::String) { return $h[$name].GetString() }
    return ''
}
$crates = New-Object System.Collections.Generic.List[object]
foreach ($el in $root.EnumerateArray()) {
    $h = Json-Props $el
    $files = New-Object System.Collections.Generic.List[object]
    if ($h.ContainsKey('files')) {
        $fv = $h['files']
        $items = if ($fv.ValueKind -eq $kind::Array) { @($fv.EnumerateArray()) } else { @($fv) }
        foreach ($f in $items) {
            $fh = Json-Props $f
            if ($fh.Count -gt 0) { $files.Add([pscustomobject]@{ name = (Json-Str $fh 'name'); text = (Json-Str $fh 'text') }) }
        }
    }
    $crates.Add([pscustomobject]@{
        name = (Json-Str $h 'name'); version = (Json-Str $h 'version'); license = (Json-Str $h 'license')
        repository = (Json-Str $h 'repository'); files = $files.ToArray() })
}
if ($crates.Count -lt 2) { throw "Rust license data looks wrong ($($crates.Count) packages): $Rust" }
foreach ($p in $crates) {
    Add-Component "$($p.name) $($p.version)" $p.license $p.repository $p.files
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("peek - third-party notices")
[void]$sb.AppendLine()
[void]$sb.AppendLine("peek.exe statically links the libraries listed below. Each entry shows its copyright notices and")
[void]$sb.AppendLine("refers to its license texts by number; the full texts follow at the end. Identical license texts")
[void]$sb.AppendLine("are included only once.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("C/C++ libraries (vcpkg): " + (($ports | ForEach-Object Name) -join ', '))
[void]$sb.AppendLine("SVG rendering: resvg and the $($crates.Count) Rust packages it uses")
[void]$sb.AppendLine()
[void]$sb.AppendLine(('=' * 78))
[void]$sb.AppendLine("COMPONENTS")
[void]$sb.AppendLine(('=' * 78))
[void]$sb.AppendLine()
[void]$sb.Append($components.ToString())
[void]$sb.AppendLine(('=' * 78))
[void]$sb.AppendLine("LICENSE TEXTS")
[void]$sb.AppendLine(('=' * 78))
for ($i = 0; $i -lt $texts.Count; $i++) {
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("[$($i + 1)]")
    [void]$sb.AppendLine(('-' * 78))
    [void]$sb.AppendLine($texts[$i])
}
Set-Content -Path $Out -Value $sb.ToString() -Encoding UTF8
Write-Host ("vcpkg ports: " + (($ports | ForEach-Object Name) -join ', '))
Write-Host ("THIRD-PARTY.txt written ({0:N0} characters, {1} vcpkg ports, {2} Rust packages, {3} distinct license texts)" -f
    $sb.Length, $ports.Count, $crates.Count, $texts.Count)
