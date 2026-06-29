# Renderer Audit — Mimas vs d32xr vs FastDoom vs SlaveDriver

Code-level audit of four renderers to decide Mimas's next moves. Each finding
is tied to actual source. Action items at the end are split into **implement
directly** (high confidence) vs **A/B test** (measure first), continuing the
`SATURN PERF x.y` numbering from `core/r_parallel.c` / the perf notes.

> **STATUS — historical audit, partially settled.** This doc is the original
> four-renderer code audit; keep §2 (inner-loop comparison) and the still-valid
> levers (2.2, 2.7) as live reference. The hardware question (§4) is **settled**:
> VDP1 carries **walls only** (8bpp + CRAM light-banks), and the dominant flat
> (floor/ceiling) went to **RBG0 as a clean 512×256 8bpp bitmap**, NOT VDP1 — see
> [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md) for the floor reality
> and [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md) for VDP1↔NBG1 present
> sync. A from-scratch VDP1 world renderer remains an unshipped bet
> ([`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md)). The dual-SH2 plane split is also
> settled (see §5): work-steal "TAS" shipped default-on; wall-prep→slave is dead.

Repos audited (cloned in `../saturn-refs/`):
- **d32xr** — Doom 32X: Resurrection. Same architecture as Mimas (2× SH-2,
  software, no TMU). MIT/Jaguar-Doom license. The closest cousin.
- **FastDoom** — DOS 386/486 Doom, GPL. Pure algorithmic/asm optimization.
- **SlaveDriver-Engine** — Lobotomy (PowerSlave/Duke/Quake Saturn), GPL. The
  VDP1 hardware path.
- **Mimas** — us. `core/r_parallel.c` + `src/dg_saturn.cxx`.

> **Renderer status (settled).** The slave is **not** idle: it actively runs the
> **P plane-half** (`r_plane.c:1206`) AND the **M sprite-half** (`r_things.c:1185`),
> both shipped on by default (`main.cxx:52-53`). `rp_disabled=1` at `r_main.c:1181`
> is *intentional* — plane-parallel forces the legacy parity renderer off so there
> is no double-dispatch conflict. Residual serial work is the **B phase** (BSP/clip);
> only **Bp wall-prep** was offloadable, and that is **confirmed dead** (memory-bound,
> tried 3×). Current solo is ~5–12 fps depending on scene (hardware-measured).

---

## 1. The dual-SH2 split — the decisive difference

### Mimas (today)
- The slave is a **draw-only** consumer. `rp_slave_body` (`core/r_parallel.c`)
  loops over the command buffer and runs `rp_exec` on the **odd** columns
  (parity 1); the master draws **even** columns. Fixed 50/50 split by `dc_x & 1`.
- The slave is launched **every frame** via `slSlaveFunc(rp_slave_wrapper)`
  inside `rp_restart`, which is fragile: it needs the `rp_sgl_workptr_reset`
  GBR+72 hack to avoid the SGL leak/freeze, and a single timeout permanently
  disables it.
- The slave does **nothing** during the BSP walk except spin on `ready`.

> **Dual-CPU framebuffer blit — HW-REJECTED (do not re-investigate).** Measured on
> real hardware (2026-06-22): blit ~5.5ms, bus-bound S~1.3, the 50/50 split is the
> WORST (6.0ms vs 5.5ms single); `blit_mode` boot default = 0 (single-CPU). Kept
> gated only as the L+R-chord A/B harness; zero cost when off.

### d32xr (the model to copy)
- The secondary SH-2 runs a **persistent dispatch loop**, `Mars_Secondary()`
  (`marsnew.c:334`): it blocks on a hardware comm register
  (`while ((cmd = MARS_SYS_COMM4) == NONE);`) and executes whole **jobs**:
  - `R_WALL_PREP` → `Mars_Sec_R_Setup` + `Mars_Sec_R_WallPrep` +
    `R_SegCommands` + `R_PreDrawPlanes`
  - `R_DRAW_PLANES`, `R_DRAW_SPRITES` (split at mean-X via COMM6)
  - **and non-render jobs**: `P_CheckSights` (gameplay!), fire animation,
    screen-melt wipe, sound DMA init.
- **Producer/consumer overlap**: `Mars_Sec_R_WallPrep` (`r_phase2.c:346`) watches
  `addedsegs` (a comm register the master bumps as `R_BSP` emits each seg) and
  processes segs **as they are produced**, `-2` = "BSP done". The secondary does
  wall late-prep + the seg/clip loop + visplane setup **while the master is still
  walking the BSP**. This is real pipelining, not just a draw split.
- Result (author-reported): **2–4× faster** than the original 32X port.

**Takeaway:** d32xr's second CPU does *phases* (prep, planes, sprites, even
sights), driven by a robust register-mailbox loop. Mimas's slave does *half the
columns* via a fragile per-frame relaunch — and right now it isn't running at
all. **This is both the biggest perf gap and a reliability fix.**

### SlaveDriver
- True-3D engine, not Doom: transforms vertices by a matrix (`MthMatrix view`),
  walks sectors, draws walls (`WALLS.C`, 2687 lines) and sprites (`drawSprites`,
  `WALLS.C:2395`) as **VDP1 sprites** with per-sprite flags (NOSCALE, FOOTCLIP,
  shadow, COLORED). The SH-2 builds VDP1 command lists; VDP1 rasterizes. Lighting
  via colored light sources (`addLight`). Not a CPU rasterizer at all.

---

## 2. The column/span inner loops

### Mimas `rp_exec_col` (`core/r_parallel.c`)
- C, unrolled ×8. **Copies the source column into a 128-byte local first:**
  `memcpy(col_cache, src, 128)` *per column*, then indexes with `& 127`.
  → For a 10-pixel column that's 128 bytes copied to draw 10 — pure overhead, and
  it assumes 128-tall textures. **Already flagged as a suspect in the perf notes.**

### d32xr `_I_DrawColumnA` (`sh2_draw.s`)
- Hand-written SH-2 asm, **no source copy** — indexes `dc_source` directly with a
  `heightmask = texheight-1` AND (true power-of-2 wrap), plus a separate
  `_I_DrawColumnNPo2A` for non-power-of-2 heights.
- Colormap lives in **GBR-relative TLS** (`@(DOOMTLS_COLORMAP, gbr)`) — cheap to
  reach, not passed per-command.
- Stride×row by shifts (`shll8`+`shlr2` = ×320), no multiply.
- Compact loop unrolled ×2 with `dt` (decrement-and-test) + delayed branch +
  `.p2alignw` alignment. **Deliberately small to fit the 4KB I-cache.**

### FastDoom `linearp.asm` (Potato) / the `linear*` family
- **Fully unwound** scaling: a `scalecalls[]` jump table where each pixel-count
  has its own straight-line unrolled code (`vscaleN`), entered via
  `jmp [scalecalls+4+ebp*4]`. **Zero per-pixel loop overhead.** Spans use
  **self-modifying code** (patch a `RET` at the exit column).
- Separate routines per **detail level** (`linearh`/`linearl`/`linearp`) and per
  **CPU** (`KN`=K6, `PE`=Pentium suffixes) — "code that fits L1".

> ⚠️ **Caution for Saturn:** FastDoom's fully-unrolled + self-modifying approach
> is an *x86-with-big-cache* trick. A 200-tall unrolled column blows the SH-2's
> 4KB I-cache — which is exactly why **d32xr keeps a tight loop**, and matches
> Mimas's own finding that `-O3` *slowed* the slave via I-cache bloat. Do **not**
> copy the unrolled/SMC pattern wholesale; the *tight hand-asm loop* (d32xr) is the
> right reference for SH-2.

---

## 3. The "Potato" convergence — flat-shaded floors

Both software ports independently converged on the same big lever:
- d32xr: *"Potato mode renders floors and ceilings in solid color"* (README).
- FastDoom: `R_DrawPlanesFlatterPotato` (`r_plane.c:658`) + quarter-width
  (`viewwidth >>= detailshift`, potato = `>>2`), solid-color planes.

Floor/ceiling **spans are the most expensive fill** (full-width, perspective).
Flat-shading them (or dropping to quarter-width) is where both software projects
buy the most fps. **Mimas took a different route and shipped it:** the dominant
flat went to **VDP2 RBG0 as a clean 512×256 8bpp bitmap** (the rotation hardware
does the perspective), so the software span path is no longer the floor's bulk —
see [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md). The d32xr/FastDoom
flat-shade lever stands as a software reference, not an open Mimas item.

---

## 4. So — should we go hardware (VDP1)? — SETTLED

This section's original Track-A/Track-B "not yet, de-risk with a measurement"
staging is now **resolved**, and the answer was a split, not an either/or:

- **VDP1 carries walls only** — 8bpp textured wall quads with CRAM light-banks
  (the hybrid path). See [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md)
  for the VDP1↔NBG1 present sync; SlaveDriver's `WALLS.C` was the reference for
  feeding VDP1.
- **The dominant flat went to VDP2 RBG0**, not VDP1 — a clean 512×256 8bpp bitmap
  (the rotation hardware does the perspective):
  [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md).
- **A from-scratch VDP1 world renderer** (the full sector/sprite rewrite) remains
  an **unshipped bet**: [`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md).

The software-renderer findings in §1–§3 still informed the shipped dual-SH2 split
(see §5). The "facing-a-wall fps cliff" is a **VDP2 cost** (the per-frame RBG0
transform), analysed separately — not a VDP1 problem.

---

## 5. Action list

Measure everything on the row-19 profiler (`REC / EX / W / c`).

> **Planes settled.** The dual-SH2 plane split is decided: work-steal **TAS**
> shipped default-on (core `73f8cdc` / parent `4857f87`), row-split parked, and
> **wall-prep→slave is confirmed dead** (memory-bound, tried 3×). The old "make the
> slave a persistent mailbox worker / give it whole phases / rebalance the column
> split" items (2.1, 2.4, 2.5) are therefore closed and removed below.

### A. Implement directly (high confidence, low risk)

| # | Change | Source / rationale | Touches |
|---|--------|--------------------|---------|
| **2.2** | **Kill the per-column `memcpy(col_cache,src,128)`** in `rp_exec_col`; index `dc_source` directly with a height mask (add an NPo2 path if needed). | d32xr `sh2_draw.s` does zero source copy; already a suspect | `core/r_parallel.c` |
| **2.3** | **Real low-detail / Potato mode**: quarter-width render + solid-color floors/ceilings, **and keep the parallel renderer enabled** under `detailshift` (remove the `RP_BeginFrame` bail-out). | d32xr + FastDoom both ship this as their top fps lever | `core/r_parallel.c`, `core/r_plane.c`, `core/r_main.c` |

### B. A/B test (uncertain — measure, may not transfer)

| # | Hypothesis | Source / risk | How to measure |
|---|-----------|---------------|----------------|
| **2.6** | **Hand-asm the column loop** (tight SH-2 loop, GBR colormap, shift-stride, `dt`+delayed-branch), *tight, not unrolled*. | d32xr `sh2_draw.s`. Risk: I-cache; only worth it if EX is fillrate-bound. | EX per-command |
| **2.7** | **Overlap the SH-2 hardware divider** (`DIVU`): start a divide, do work, collect the quotient — for scale/perspective math. | d32xr `r_phase2.c:178` (`SH2_DIVU_DVSR/DVDNT`). Saturn SH-2 has the same unit. | REC |
| **2.8** | **Sky → VDP2 scroll layer** instead of software columns; frees fillrate + command count. | classic Saturn trick; reduces `c` | EX, `c` |
| **2.9** | **VDP1 wall prototype** (Track B) — throwaway, measure fillrate + affine warp on real hardware before any commitment. | SlaveDriver `WALLS.C`. Strategic, not incremental. | standalone fps |

### C. Explicitly do NOT copy
- FastDoom's **fully-unrolled `scalecalls[]`** and **self-modifying span** code:
  great on x86, but I-cache-hostile on the SH-2 (d32xr avoids it; our `-O3`
  result confirms). The lesson to take is *detail modes + tight loops*, not the
  unrolling.

---

## Sources
Code: `../saturn-refs/{d32xr,FastDoom,SlaveDriver-Engine}` ·
Mimas `core/r_parallel.c`, `src/dg_saturn.cxx`.
Background + links: `docs/PERF_REFERENCES.md`.
