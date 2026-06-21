# Boot regression: run recompiled WR64 briefly and assert log hygiene.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build-ninja"
$Exe = Join-Path $Build "WaveRace64Recompiled.exe"
$Log = Join-Path $Build "wr64_boot_regress.log"
$Seconds = 15

if (-not (Test-Path $Exe)) {
    Write-Error "Build first: $Exe"
}

Set-Location $Build
Remove-Item $Log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Exe -ArgumentList "--log-file", "wr64_boot_regress.log" -PassThru
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $Log)) {
    Write-Error "No log produced: $Log"
}

$bad = @(
    "HLE: boot fill rect",
    "stub #",
    "Failed to find function"
)
$text = Get-Content $Log -Raw
foreach ($pat in $bad) {
    if ($text -match [regex]::Escape($pat)) {
        Write-Error "FAIL: log contains '$pat'"
    }
}

# boot_mq_full only forbidden after early boot (heuristic: after Gfx Task #60)
$lines = Get-Content $Log
$afterGfx60 = $false
foreach ($line in $lines) {
    if ($line -match "Gfx Task #6[0-9]") { $afterGfx60 = $true }
    if ($afterGfx60 -and $line -match "boot_mq_full") {
        Write-Error "FAIL: boot_mq_full after frame 60"
    }
}

Write-Host "PASS: boot regression ($Seconds s) - see $Log"
