# WR64 Function Audit

Cross-reference of runtime patches, overlay sections, and recompilation status (2026-06-16).

## Regeneration

```powershell
cd tools
python symbols_gen.py --split-functions --no-merge-inward-branches --boot-dma-slices --prog-overlays
python symbols_gen.py --overlays-list-only --boot-dma-slices --prog-overlays
powershell -File rebuild_recomp.ps1   # cleans stale funcs_*.c, recompiles, re-runs cmake
cmake --build ../build-ninja --config Release
```

`wr64_usa.toml` sets `use_lookup_for_all_function_calls = true` so prog overlays avoid `static_N_*` link collisions.

Prog overlays use **export anchors** in `tools/symbols_gen.py` (`PROG_OVERLAY_EXPORT_VRAMS`) — one recompiled function per codeseg entry point (`func_802C5BA4`, etc.).

## Overlay sections (`recomp_overlays.inl`)

| Section | ROM | VRAM | Status |
|---------|-----|------|--------|
| `codeSEG` | 0x1000 | 0x80046800 | RECOMPILED (static) |
| `..ovl_boot_dma1` | 0xA95D0 | 0x801DAFA0 | RECOMPILED (relocatable) |
| 19 prog overlays | 0x1B1FB0–0x1D11D0 | 0x802C5800 | RECOMPILED (export anchors) |
| 74 bank/course | layout | 0x80316800 / 0x802D6800 | RAW DMA only (`--layout-overlays` crashes N64Recomp on MIO0) |

## Runtime patches (`register_wr64_function_patches`)

### HLE-BRIDGE (libultra / host)

| Address | Symbol | Status |
|---------|--------|--------|
| 0x800C6420 | osCreateThread | HLE-BRIDGE |
| 0x800C6570 | osStartThread | HLE-BRIDGE |
| 0x800C5C60 | osRecvMesg | HLE-BRIDGE |
| 0x800C57A0 | osSendMesg | HLE-BRIDGE |
| 0x800C63B0 | osViSetEvent | HLE-BRIDGE |
| 0x800C6770 | osCreateViManager | HLE-BRIDGE |
| 0x800C6AD0 | osViBlack | HLE-BRIDGE |
| 0x800C59B0 | osViSwapBuffer | HLE-BRIDGE |
| 0x800C6B40 | osViGetCurrentFramebuffer | HLE-BRIDGE |
| 0x800C5E60 | osViSetSpecialFeatures | HLE-BRIDGE |
| 0x800CBF50 | osViSwapBuffer (alt) | HLE-BRIDGE |
| 0x800CB9C0 | osPiRawStartDma | HLE-BRIDGE |
| 0x800CA370 | osPiStartDma | HLE-BRIDGE |
| 0x800C7020–0x800C5AC4 | osCont* | HLE-BRIDGE |
| 0x800C615C / 0x800C62BC | osSpTaskLoad / Go | HLE-BRIDGE → RT64 |
| 0x800C6340 | osSetEventMesg | HLE-BRIDGE |
| 0x800C6DE0, 0x800CB7D0, 0x800C6310, 0x800C5DF0 | misc libultra | HLE-BRIDGE |

### HLE-FULL (COP1 / math)

| Address | Symbol | Status |
|---------|--------|--------|
| 0x800C7380 | guOrthoF | HLE-FULL |
| 0x800C74D4 | guOrtho | HLE-FULL |
| 0x80048854 | MatrixLookAt | HLE-FULL |
| 0x80047C38 | TaylorSeries | HLE-FULL |
| 0x80047EE0 | MtxFToMtx | HLE-FULL (audit) |
| 0x800474E4 | gu helper | HLE-FULL (audit) |
| 0x800B8CB0 | pi_get_access | HLE-FULL |

### HLE-FULL (DMA helper)

| Address | Symbol | Status |
|---------|--------|--------|
| 0x80097EC8 | game_dma memcpy | HLE-FULL |

### REMOVED — stock recompiled

| Address | Symbol | Notes |
|---------|--------|-------|
| 0x80091F50 | Gfx pool/matrix init | Stock |
| 0x80092CF0 | Per-state Gfx builder | Stock → `segment_1B1FB0_802C5BA4` etc. |
| 0x800468E0 | Segment + DL bootstrap | Stock |
| 0x80046D2C | Gfx init frame hook | Stock |
| 0x800922E4 | Per-frame game logic | Stock |
| 0x80046BF4 / 46C30 / 46CF8 | GfxFullSync helpers | Stock |
| 0x80098208 | GameLoad_LoadOverlay | Stock |
| 0x80095050 | unk_game_load | Stock |
| 0x80095A28 | overlay segment loader | Stock |
| 0x80097E68 | DMA queue drain | Stock |
| 0x8004A130 / 0x8004A2B4 | SysUtils Cont* | Stock |
| 0x80047B00 / 0x800BF370 | Audio init/update | Stock (audio thread deferred at boot) |

### REMOVED stubs

| Address | Was | Now |
|---------|-----|-----|
| 0x802C5800 | `wr64_stub_802C5800` no-op | `load_overlays()` on PI DMA |
| 0x801E5470 | `wr64_stub_801E5470` | boot_dma1 recompiled |

### `add_loaded_function` overrides

| Address | Handler | Status |
|---------|---------|--------|
| 0x800980C8 | wr64_static_0_800980C8 | HLE data |
| 0x800C6D00 | wr64_codeSEG_800C6D00 | HLE |
| 0x800CAB50 | codeSEG_800CAB50 | bridge |
| 0x800CC370 | wr64_pi_manager_dma_handler | HLE PI completion |
| 0x801EAFB4 | wr64_static_1_801EAFB4 | static override if needed |

## `func_80092CF0` dispatch matrix (gGameState → overlay export)

| gGameState | Export VRAM | Overlay section |
|------------|-------------|-----------------|
| 0x5–6 | 0x802C5BA4 | segment_1B1FB0 |
| 0x2, 0x3C–3D, 0x46–49 | 0x802C5800 | seg_1C3D00 |
| 0x3 | 0x802C5A7C | ovl_i4 |
| 0x4 | 0x802C6944 | ovl_i5 |
| 0x7–8 | 0x802C913C | ovl_i7 |
| 0x28–2C | 0x802C5AE4 | ovl_i2 |
| 0x2D | 0x802C7D00 | ovl_i6 |
| 0x32–33 | 0x802C5B74 | ovl_i8 |
| 0x50–51 | 0x802C5924 | ovl_i9 |
| 0xA–B | 0x802C5B4C | ovl_i10 |
| 0x14–15 | 0x802C5B78 | ovl_i11 |
| 0x1E–1F | 0x802C5B40 | ovl_i12 |
| 0x34–35 | 0x802C5C1C | ovl_i13 |
| 0x3E–3F | 0x802C5D3C | ovl_i14 |
| 0x42–43 | 0x802C5D24 | ovl_i15 |
| 0x44–45 | 0x802C5968 | ovl_i1 |
| 0x40–41 | 0x802C5F6C | ovl_i0 |
| 0x5A–5B | 0x802C7484 | segment_1B1FB0 |
| 0x66–67 | 0x802C5F50 | ovl_i3 |
| 0x38 | 0x802C583C | segment_1B1FB0 |

Boot_dma1 states (0, 0x64–65, 0x39, pause): `func_801E*` in `..ovl_boot_dma1`.

## Boot regression assertions

Run `tools/boot_regress.ps1`. Log must **not** contain:

- `HLE: boot fill rect`
- `stub #`
- `boot_mq_full` (after frame 60)
- `Failed to find function`

## Known gaps

- Bank/course MIO0 overlays: DMA + endian swap only; no per-function recomp (N64Recomp crash).
- Boot may hit `invalid MEM_B` in stock Main path until full Gfx init chain completes — investigate `gGameState` / overlay table timing.
- RT64 F3DWAVE: audit `segment_F6090` opcodes vs `rt64_gbi_f3dwave.cpp` (see `docs/N64RECOMP_PATCHES.md`).
