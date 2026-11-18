# Prueft, dass peek.exe keine Netzwerk- oder Internetbibliotheken importiert.
# Bricht den Build ab, wenn eine verbotene DLL (auch verzoegert geladen) gefunden wird.
param(
    [Parameter(Mandatory = $true)][string]$Exe
)

$ErrorActionPreference = 'Stop'

$forbidden = @(
    'ws2_32.dll', 'wsock32.dll', 'mswsock.dll',
    'wininet.dll', 'winhttp.dll', 'urlmon.dll',
    'dnsapi.dll', 'iphlpapi.dll', 'webservices.dll',
    'webview2loader.dll', 'httpapi.dll', 'netapi32.dll',
    'wlanapi.dll', 'mpr.dll', 'rasapi32.dll'
)

if (-not (Test-Path $Exe)) { throw "Datei nicht gefunden: $Exe" }

$output = & dumpbin /nologo /dependents $Exe
if ($LASTEXITCODE -ne 0) { throw "dumpbin fehlgeschlagen" }

$dlls = $output | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -like '*.dll' }

Write-Host "Importierte DLLs:"
$dlls | Sort-Object -Unique | ForEach-Object { Write-Host "  $_" }

$hits = $dlls | Where-Object { $forbidden -contains $_ }
if ($hits) {
    Write-Error ("Verbotene Netzwerkbibliotheken gefunden: " + ($hits -join ', '))
    exit 1
}

Write-Host "OK: keine Netzwerkbibliotheken importiert."
