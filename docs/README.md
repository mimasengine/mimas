# Mimas — docs index

Index of the design/analysis docs for the Mimas Saturn Doom port. Each line tags the doc's
**status**: `LIVE` (authoritative current reference) · `REFERENCE` (durable HW/API reference) ·
`PLAN` (active, partly unshipped) · `RECORD` (shipped feature / settled result) ·
`HISTORICAL` (kept only for unique measured data; framing superseded).

Reorg 2026-06-29: four fully-obsolete cell-era RBG0 plans were **deleted**
(`RBG0_SNOW_FIX_PLAN.md`, `RBG0_STRUCTURED_GARBAGE.md`, `VDP2_FLOOR_CONSOLIDATION.md`,
`VDP1_FLOOR_PLAN.md`); their useful content lives in the LIVE docs below.

## Authoritative live references — read these first
- [VDP2_RBG0_CURRENT_STATE.md](VDP2_RBG0_CURRENT_STATE.md) — **LIVE.** The shipped RBG0 hardware floor (512×256 8bpp bitmap, 2 banks, no snow, no slSynch). Wins any floor disagreement.
- [VDP1_PRESENT_SYNC_PLAN.md](VDP1_PRESENT_SYNC_PLAN.md) — **LIVE.** The VDP1↔NBG1 present-sync plan: why the décrochage is intrinsic, and the bug-fixed gated present + coupled deterministic present + MVOL freebie.
- [SRL_API.md](SRL_API.md) — **REFERENCE.** Saturn Ring Library v0.9.2 C++ SDK API surface (the vendor library the platform layer uses).

## VDP1 — walls, present, capacity
- [VDP1_ARCHITECTURE.md](VDP1_ARCHITECTURE.md) — **REFERENCE.** The VDP1 chip cost model (fill/overdraw), VRAM ledger, 8bpp+CRAM-light-bank doctrine, IN/OUT routing, multiplayer budget.
- [VDP1_4BPP_STUDY.md](VDP1_4BPP_STUDY.md) — **REFERENCE.** Decision study: keep walls 8bpp (CRAM-bank lighting); 4bpp is storage-only. CL16Bnk vs CL16Look, measured WAD quantization.
- [VDP1_WORLD_PLAN.md](VDP1_WORLD_PLAN.md) — **PLAN (archived bet).** The unshipped per-subsector VDP1 floors/ceilings design-of-record + VDP1 VRAM ledger + anti-swim derivation.
- [VDP1_CAPACITY_STUDY.md](VDP1_CAPACITY_STUDY.md) — **REFERENCE (2026-07-02).** Consolidated CPU+VDP1 capacity ledger (1j/2j/4j), the sprite-deport question, and the leftover-capacity effects catalog. Holds the two owner corrections (Dr% is present-noise not a fill gauge; sprite priority is config).
- [WALL_SUBDIVISION_STUDY.md](WALL_SUBDIVISION_STUDY.md) — **PLAN (Phase 0 shipped).** SlaveDriver-style sub-quad subdivision for walls: the world-anchored vertical clamp that replaces the CPU near-wall fallback (walls don't vertically swim — linear), the command/build-`k` tension, and the Option-2→Option-1 roadmap + the shipped `FBK`/PERFSIM profilers.

## VDP2 — floor, layers, configs
- [VDP2_ARCHITECTURE.md](VDP2_ARCHITECTURE.md) — **REFERENCE.** VDP2 chip + VRAM access-cycle ("snow") model + per-layer bandwidth + bank-free register features (colour offset, line-colour, colour-calc).
- [VDP2_LAYER_BUDGET.md](VDP2_LAYER_BUDGET.md) — **REFERENCE.** 4-bank VRAM/cycle budget, per-dot read-slot cost table, real-game cycle-pattern examples.
- [VDP2_CONFIG_CATALOG.md](VDP2_CONFIG_CATALOG.md) — **PLAN.** Catalog of VDP2/VDP1 layout configs (live unshipped bets: gradient floor, NBG2 clouds, VDP1-floor, world-on-VDP1) + measured-HW anchors.
- [RBG0_FLOOR_PLAN.md](RBG0_FLOOR_PLAN.md) — **HISTORICAL.** Trimmed to its FLAT-profiler data (dom% 49–93%) + cost model; the floor shipped per VDP2_RBG0_CURRENT_STATE.md.

## Performance — critical path, REC, parallelism
- [CRITICAL_PATH.md](CRITICAL_PATH.md) — **REFERENCE.** HW-measured master frame breakdown (Bw/Bp/P/M/blit at pot0), serial/parallel/offloadable taxonomy, slave-reuse ledger, capacity levers.
- [REC_BENCHMARKS.md](REC_BENCHMARKS.md) — **REFERENCE.** Living cross-session HW/Ymir benchmark table (sections B/C captures, Dr semantics, measurement law) + the WAD-témoins stress bench.
- [REC_REDUCTION.md](REC_REDUCTION.md) — **PLAN.** Catalogue + gated experiments to cut REC (the mono-master command-generation phase); lever status tracker.
- [REC_L2_SPEC.md](REC_L2_SPEC.md) — **PLAN.** Master→slave command buffer: SHRINK (shipped) + RELOCATE-to-HWRAM (ready-to-code spec).
- [PARALLEL_REC_AUDIT.md](PARALLEL_REC_AUDIT.md) — **RECORD.** Why putting the slave on render *generation* (wall-prep) doesn't fit 2MB / is dead; d32xr phase-split model + duplication memory cost.
- [RANK3_WALLPREP.md](RANK3_WALLPREP.md) — **RECORD.** The settled-negative result: wall-prep→slave is memory-bound and does not pay (HW-measured, tried 3×).
- [RENDERER_AUDIT.md](RENDERER_AUDIT.md) — **REFERENCE.** Cross-engine inner-loop audit (Mimas vs d32xr/FastDoom/SlaveDriver) + the SH-2 I-cache "tight loop, no SMC/unroll" lesson + transferable levers.
- [PERF_REFERENCES.md](PERF_REFERENCES.md) — **REFERENCE.** External Saturn/dual-SH2/optimized-Doom project catalog with source URLs and transferable techniques.
- [REC_HW_CAPTURES_2026-06-19.csv](REC_HW_CAPTURES_2026-06-19.csv) — **RECORD.** Raw real-Saturn capture set (floor on/off × potato × blit).

## Features — shipped plans & records
- [MULTIPLAYER_PLAN.md](MULTIPLAYER_PLAN.md) — **RECORD/PLAN.** Local split-screen (2p shipped + compact HUD); perf cost model; open 4p/DM/potato TODOs.
- [SPLIT_BENCHMARKS.md](SPLIT_BENCHMARKS.md) — **REFERENCE.** 2-player split benchmark table + the RMUL kick fix.
- [STREAMING_ANALYSIS.md](STREAMING_ANALYSIS.md) — **PLAN/RECORD.** Big-WAD CD streaming on the 2MB cartless Saturn: byte budgets, .DRP repack/LZSS, music-vs-streaming model.
- [TEXTURECOLUMNLUMP_PLAN.md](TEXTURECOLUMNLUMP_PLAN.md) — **RECORD.** Measurement + DEFER gate for cutting the texturecolumnlump/composite PU_STATIC floor (per-WAD floor table).
- [TRANSITIONS_PLAN.md](TRANSITIONS_PLAN.md) — **RECORD.** Shipped level start/end CRAM dip-to-black fades (replaced the crashing screen-melt under streaming).
- [RENDER_CORRUPTION_ANALYSIS.md](RENDER_CORRUPTION_ANALYSIS.md) — **RECORD.** Resolved Doom II first-frame master-stack corruption (spanstart/spanstop[256] overrun) + the .map forensics.
