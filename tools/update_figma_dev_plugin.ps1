param(
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist')
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'figma-slugui'
$destination = Join-Path $OutputDirectory 'share\slugia\figma-slugui-v6'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @('code.js','ui.html')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $destination $name) -Force
}
$manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw -Encoding UTF8 |
    ConvertFrom-Json
$manifest.name = 'SlugUI Exporter - Stroke v6'
# A fresh DEVELOPMENT plugin registration avoids running a cached copy of the
# previous plugin, which produced byte-identical and visibly incorrect .slugui.
$manifest.id = 'slugia-slugui-exporter-stroke-v6'
$contents = $manifest | ConvertTo-Json -Depth 10
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $destination 'manifest.json'), $contents, $utf8)
& node --check (Join-Path $destination 'code.js')
if ($LASTEXITCODE -ne 0) { throw 'Generated developer plugin syntax check failed.' }
$sourceHash = (Get-FileHash (Join-Path $source 'code.js') -Algorithm SHA256).Hash
$destinationHash = (Get-FileHash (Join-Path $destination 'code.js') -Algorithm SHA256).Hash
if ($sourceHash -ne $destinationHash) { throw 'Developer plugin has stale code.js.' }
Write-Host 'Figma Stroke v6 is staged. IMPORT THIS NEW MANIFEST:' -ForegroundColor Green
Write-Host (Join-Path $destination 'manifest.json')
Write-Host 'After importing: close the old Figma plugin window, then launch'
Write-Host 'Plugins > Development > SlugUI Exporter - Stroke v6.'
