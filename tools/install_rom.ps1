# Normalize a WR64 ROM dump and deploy it for build + runtime.
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRom
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build-ninja"
$DestName = "wr64.us.revA.z64"
$StoredName = "wr64.us.revA.z64"
$DestRom = Join-Path $Root $DestName

python (Join-Path $Root "tools\fetch_ui_assets.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path -LiteralPath $SourceRom)) {
    Write-Error "ROM not found: $SourceRom"
}

$py = @"
import sys
from pathlib import Path
sys.path.insert(0, r'$($Root -replace '\\','\\')\tools')
from rom_util import load_rom
src = Path(r'$($SourceRom -replace '\\','\\')')
out = Path(r'$($DestRom -replace '\\','\\')')
normalized, hdr = load_rom(src)
out.write_bytes(normalized)
print(f'Normalized {src.name} -> {out.name}')
print(f'  title={hdr[\"title\"]!r} PC=0x{hdr[\"pc\"]:08X} format={hdr[\"source_format\"]}')
"@

python -c $py
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

python (Join-Path $Root "tools\rom_fingerprint.py") $DestRom
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path $Build) {
    Copy-Item -LiteralPath $DestRom -Destination (Join-Path $Build $StoredName) -Force
    Copy-Item -LiteralPath (Join-Path $Root "recompcontrollerdb.txt") -Destination $Build -Force -ErrorAction SilentlyContinue
    $assetsSrc = Join-Path $Root "assets"
    $assetsDst = Join-Path $Build "assets"
    if (Test-Path $assetsSrc) {
        if (Test-Path $assetsDst) { Remove-Item $assetsDst -Recurse -Force }
        Copy-Item $assetsSrc $assetsDst -Recurse -Force
    }
    New-Item -Path (Join-Path $Build "portable.txt") -ItemType File -Force | Out-Null
    Write-Host "Deployed ROM + assets -> $Build"
}
