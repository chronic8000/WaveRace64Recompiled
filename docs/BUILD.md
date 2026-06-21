# Building Wave Race 64: Recompiled

## Prerequisites

- Windows 10/11, Visual Studio 2022
- CMake 3.20+
- Python 3.10+ (`pip install rabbitizer xxhash`)
- User-supplied USA Rev A ROM (see [ROM.md](ROM.md))

## Toolchain

```powershell
git submodule update --init --recursive
cmake -S N64Recomp -B N64Recomp/build
cmake --build N64Recomp/build --config Release --target N64RecompCLI

cd tools
python symbols_gen.py --split-functions --no-merge-inward-branches
powershell -File rebuild_recomp.ps1
```

## Game executable

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Place `wr64.us.revA.z64` next to the executable or select it in the launcher.

## Release packaging

Ship the binary, launcher assets, and docs only. **Do not** redistribute ROM or extracted game assets. The launcher validates the ROM XXH3-64 hash against the USA Rev A entry in `supported_games`.
