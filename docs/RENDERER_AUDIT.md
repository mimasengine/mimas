# Renderer Audit — DoomSRL vs d32xr vs FastDoom vs SlaveDriver

Code-level audit of four renderers to decide DoomSRL's next moves. Each finding
is tied to actual source. Action items at the end are split into **implement
directly** (high confidence) vs **A/B test** (measure first), continuing the
`SATURN PERF x.y` numbering from `core/r_parallel.c` / the perf notes.

> **⚠️ §4 ("should we go hardware?") is DECIDED as of 2026-06-19.** DoomSRL took
> **Track B**: VDP1 now renders all walls (hybrid — VDP1 walls below software NBG1).
> The current VDP1 hardware model, costs, and convictions are in
> [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md). The §1–§3 *software-renderer*
> findings (slave-as-worker, kill the per-column memcpy, Potato) remain valid and
> still relevant — VDP1 offloads EX/fill, not REC, which is still the frame's bulk.

Repos audited (cloned in `../saturn-refs/`):
- **d32xr** — Doom 32X: Resurrection. Same architecture as DoomSRL (2× SH-2,
  software, no TMU). MIT/Jaguar-Doom license. The closest cousin.
- **FastDoom** — DOS 386/486 Doom, GPL. Pure algorithmic/asm optimization.
- **SlaveDriver-Engine** — Lobotomy (PowerSlave/Duke/Quake Saturn), GPL. The
  VDP1 hardware path.
- **DoomSRL** — us. `core/r_parallel.c` + `src/dg_saturn.cxx`.

> **Reality check (from the perf notes):** DoomSRL's parallel renderer has been
> **disabling itself near frame 1** (slave timeout → `rp_disabled` permanent), so
> the ~8.6 fps baseline is **serial master-only** rendering. We are nowhere near
> the software ceiling — the second SH-2 is effectively idle. This reframes the
> whole "should we go hardware?" question (see §4).

---

## 1. The dual-SH2 split — the decisive difference

### DoomSRL (today)
- The slave is a **draw-only** consumer. `rp_slave_body` (`core/r_parallel.c`)
  loops over the command buffer and runs `rp_exec` on the **odd** columns
  (parity 1); the master draws **even** columns. Fixed 50/50 split by `dc_x & 1`.
- The slave is launched **every frame** via `slSlaveFunc(rp_slave_wrapper)`
  inside `rp_restart`, which is fragile: it needs the `rp_sgl_workptr_reset`
  GBR+72 hack to avoid the SGL leak/freeze, and a single timeout permanently
  disables it.
- The slave does **nothing** during the BSP walk except spin on `ready`.

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
sights), driven by a robust register-mailbox loop. DoomSRL's slave does *half the
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

### DoomSRL `rp_exec_col` (`core/r_parallel.c`)
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
> DoomSRL's own finding that `-O3` *slowed* the slave via I-cache bloat. Do **not**
> copy the unrolled/SMC pattern wholesale; the *tight hand-asm loop* (d32xr) is the
> right reference for SH-2.

---

## 3. The "Potato" convergence — flat-shaded floors

Both software ports independently converged on the same big lever:
- d32xr: *"Potato mode renders floors and ceilings in solid color"* (README).
- FastDoom: `R_DrawPlanesFlatterPotato` (`r_plane.c:658`) + quarter-width
  (`viewwidth >>= detailshift`, potato = `>>2`), solid-color planes.

Floor/ceiling **spans are the most expensive fill** (full-width, perspective).
Flat-shading them (or dropping to quarter-width) is where both projects buy the
most fps. DoomSRL currently renders full spans (`rp_exec_span`) **and disables the
parallel renderer entirely when `detailshift != 0`** (`RP_BeginFrame`) — so it has
no working low-detail mode at all. Big, cheap, untapped.

---

## 4. So — should we go hardware (VDP1)?

**Short answer: not yet, and not as "an optimization."** Reasoning from the audit:

1. **You're not at the software ceiling — you're far below it.** The parallel
   renderer is self-disabled; you're running serial on one SH-2. d32xr proves the
   *same hardware* (2× SH-2, software) reaches 2–4× by (a) a robust secondary-CPU
   job loop and (b) phase-level parallelism. That headroom is real, lower-risk,
   and **keeps the shared `core/` with DoomJo** (doomgeneric stays intact).
2. **VDP1 is a different project, not a patch.** SlaveDriver is a from-scratch
   sector/sprite engine; it is *not* Doom's renderer with VDP1 bolted on. Going
   VDP1 means rewriting r_segs/r_plane/r_things into VDP1 command-list generation,
   abandoning doomgeneric's column renderer, and inheriting the **affine texture
   warp** (the exact thing Carmack rejected on Saturn Doom) plus the hard
   floor/ceiling problem. It breaks the DoomJo code-sharing.
3. **But it's the only path to a *big* leap** (full framerate, smooth). PowerSlave
   proves it. So it shouldn't be dismissed — it should be **de-risked with a
   measurement**, not entered on faith.

**Recommended staging:**
- **Track A (now): make the software renderer do what it already promises.** Fix
  the slave reliability, copy d32xr's secondary-as-worker model, rebalance, kill
  the per-column memcpy, add a real Potato mode. Target d32xr-level (~2–3×). High
  ROI, keeps the shared core.
- **Track B (parallel, cheap): a throwaway VDP1 wall prototype.** Draw a handful
  of walls as VDP1 distorted sprites on *your* Saturn and measure real fillrate
  and warp. Decide the hardware bet with numbers from your hardware — not from
  forum lore. SlaveDriver's `WALLS.C` is the reference for how to feed VDP1.

---

## 5. Action list

Measure everything on the row-19 profiler (`REC / EX / W / c`) **after** the
parallel renderer is confirmed live again — today's numbers are serial.

### A. Implement directly (high confidence, low risk)

| # | Change | Source / rationale | Touches |
|---|--------|--------------------|---------|
| **2.1** | **Make the slave a persistent job worker** driven by a comm-register mailbox loop (à la `Mars_Secondary`), instead of relaunching `slSlaveFunc` every frame. Removes the GBR+72 leak hack *and* the permanent-disable fragility. | d32xr `marsnew.c:334`; fixes the self-disable in the perf notes | `core/r_parallel.c`, `src/main.cxx` |
| **2.2** | **Kill the per-column `memcpy(col_cache,src,128)`** in `rp_exec_col`; index `dc_source` directly with a height mask (add an NPo2 path if needed). | d32xr `sh2_draw.s` does zero source copy; already a suspect | `core/r_parallel.c` |
| **2.3** | **Real low-detail / Potato mode**: quarter-width render + solid-color floors/ceilings, **and keep the parallel renderer enabled** under `detailshift` (remove the `RP_BeginFrame` bail-out). | d32xr + FastDoom both ship this as their top fps lever | `core/r_parallel.c`, `core/r_plane.c`, `core/r_main.c` |

### B. A/B test (uncertain — measure, may not transfer)

| # | Hypothesis | Source / risk | How to measure |
|---|-----------|---------------|----------------|
| **2.4** | **Give the slave whole phases, not half the columns**: move wall-prep / visplane setup / sprite-half onto the slave (producer/consumer with the master's BSP). | d32xr `r_phase2.c:346`. Bigger change to the sync core (freeze history) → careful. | REC should drop (master does less); W tells if balanced |
| **2.5** | **Rebalance the column split** by the `W` measurement: W≈0 ⇒ master-bound, give slave >50%; W≈EX ⇒ slave-bound, give master more. | perf-notes 2.1 "two pointers meet in the middle" | row-19 `W` before/after |
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
DoomSRL `core/r_parallel.c`, `src/dg_saturn.cxx`.
Background + links: `docs/PERF_REFERENCES.md`.
