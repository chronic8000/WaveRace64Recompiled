# Rebuild N64Recomp and run WR64 recompilation.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "N64Recomp\build"

Remove-Item (Join-Path $Build "N64Recomp.dir") -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Build "Release\N64Recomp.lib") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Build "Release\N64Recomp.exe") -Force -ErrorAction SilentlyContinue

cmake --build $Build --config Release --target N64RecompCLI
Push-Location $Root
try {
    Get-ChildItem (Join-Path $Root "RecompiledFuncs\funcs_*.c") -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item (Join-Path $Root "RecompiledFuncs\funcs.h") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $Root "RecompiledFuncs\recomp_overlays.inl") -Force -ErrorAction SilentlyContinue
    & (Join-Path $Build "Release\N64Recomp.exe") wr64_usa.toml
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake -S $Root -B (Join-Path $Root "build-ninja") -DCMAKE_BUILD_TYPE=Release | Out-Null
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
