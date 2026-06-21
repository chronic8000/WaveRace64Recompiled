# N64Recomp local patches (WR64 port)

Applied on top of upstream N64Recomp for Wave Race 64 symbol-less recompilation:

- `src/main.cpp`: entrypoint selection by `config.entrypoint` VRAM; skip missing overlay sections in `recomp_overlays.inl`; `use_lookup_for_all_function_calls` from TOML
- `src/analysis.cpp`: tolerate negative stack offsets; skip empty jump tables
- `src/recompilation.cpp`: runtime lookup for unresolved `jal`; relaxed branches, cache/TLB/cop0/cop1 edge cases

Regenerate code:

```powershell
cd tools
python rom_extract.py
python symbols_gen.py --split-functions --no-merge-inward-branches --boot-dma-slices --prog-overlays
python symbols_gen.py --overlays-list-only --boot-dma-slices --prog-overlays
powershell -File rebuild_recomp.ps1
```

`rebuild_recomp.ps1` deletes stale `funcs_*.c` before recompiling (required when function count changes) and re-runs `cmake` so new sources are picked up.

`wr64_usa.toml` sets `use_lookup_for_all_function_calls = true` for prog overlay jal resolution without `static_N_*` symbols.

Prog overlays use export anchors (`PROG_OVERLAY_EXPORT_VRAMS` in `symbols_gen.py`) — not full MIPS scans — so codeseg `func_802C*` calls resolve via `load_overlays()`.

`overlays.us.txt` must list every PI DMA relocatable section (from `wr64_rom_layout.json`).
Boot-critical PI DMA #1 is a MIPS slice at ROM `0xA95D0` → RDRAM `0x801DAFA0` (not the full `course6_2p` MIO0 blob).
DMA #2 (ROM `0xF6090`) is Gfx display-list data — raw DMA only, no recomp section.
Use `--boot-dma-slices` (not full `course6_2p`/`course2_com` sections; those crash N64Recomp).
Do **not** use `--layout-overlays` in syms gen — course MIO0 crashes N64Recomp. Bank/course blobs are raw PI DMA at runtime.
`prog*.o` spec entries are linker stubs — real program code lives in prog overlay slices + `bank*` / `course*` MIO0 blobs.

Boot regression: `powershell -File tools/boot_regress.ps1`

Full patch inventory: `docs/FUNCTION_AUDIT.md`

Output lands in `RecompiledFuncs/` including `lookup.cpp` and `recomp_overlays.inl`.
