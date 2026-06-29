# Mimas — critical-path model & headroom levers

> **Status (2026-06-29).** LIVE cost model — the master frame breakdown, slave-reuse
> ledger, and HW levers below are the current ground truth. The floor question is
> **settled**: the dominant flat shipped to **RBG0 as a clean 512×256 8bpp bitmap** —
> see [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md). VDP1 carries **walls
> only**; its present sync is owned by [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md).
> The old "two floor plans, re-gated on Dr" sequencing (§5) is kept only as a trimmed
> historical note.
>
> **Source.** Derived from the HW set `REC_BENCHMARKS.md §C.2` (2026-06-25/26, E1M1
> stripped, 2 spots, immobile) + a code-verification pass (2026-06-26, 4 grounded
> readers + 5 adversarial verifiers). Every measured/`file:line`-anchored claim here
> still holds.

## 0. The one-paragraph model

`fps = 1000 / MST` and the frame is **master-bound** (the loop returns when the master
finishes compute + blit — [dg_saturn.cxx:2734](../src/dg_saturn.cxx#L2734)).
Causal direction in code is `MST = 1000/fps` (fps is measured from `DG_DrawFrame`
completions vs real vblanks; MST is its reciprocal — [dg_saturn.cxx:998](../src/dg_saturn.cxx#L998)).
The VDP1 present model (fire-and-forget wall kick, FBCR, manual-change, tearing vs.
fps) is **out of scope for this doc** — owned by [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md).
For the cost model here it suffices that **VDP1 work never caps fps** (it only affects
the wall layer's image); the "facing-a-wall fps cliff" is a **VDP2 cost** (the per-frame
RBG0 transform), analysed separately.

## 1. Master frame breakdown at pot0 (~130 ms / ~7.6 fps, nukage)

Ship config: `sat_plane_parallel=1`, `sat_masked_parallel=1` ([main.cxx:62](../src/main.cxx#L62)),
which forces `rp_disabled=1` ([r_main.c:1181](../core/r_main.c#L1181)) — **the command/parity
column-renderer is OFF**; the renderer runs the direct serial path with two explicit
slave half-splits (planes, masked). The overlay row `Bw Bp P M` = master-FRT phase deltas
([r_parallel.c:1487](../core/r_parallel.c#L1487)).

| # | Phase | REC | ~ms pot0 | CPU | Memory bus | Notes |
|---|-------|-----|----------|-----|-----------|-------|
| 1 | **BSP walk** | `Bw` | ~7–8 | master | **cart-DRAM WAD** (0x22400000, slowest) + LWRAM | `R_RenderBSPNode` recursion; **inherently serial** (front-to-back occlusion) |
| 2 | **Wall-prep** | `Bp` | **~21** | master | LWRAM zone + cart-DRAM | `R_StoreWallRange`+`R_RenderSegLoop`: per-column visplane mark + clip. **Does NOT collapse with potato.** Slave-offloadable (scaffold gated off) |
| 3 | VDP1 wall kick | — | ~0 | →VDP1 async | VDP1 VRAM | walls rasterise in parallel; never blocks master |
| 4 | **Plane fill** | `P` | **~62** | **master+slave** | HWRAM framebuffer (write) + LWRAM (flat/spanstart) | `R_DrawPlanes`, 50/50 split + barrier. **Dominant bucket.** Collapses 62→17(pot1)→11(pot2) |
| 5 | Masked/sprites | `M` | ~6 | master+slave | HWRAM fb + LWRAM | half-split; does not collapse with potato |
| 6 | Blit | — | ~5.5 | master | HWRAM→VDP2 VRAM (bus) | single-CPU wins (bus-bound, not compute) |

`Bw+Bp+P+M ≈ 96 ms`; remainder = blit + HUD + game tick. **`P` ≈ 48 % of the render is
the whole game.**

### ⚠️ `P` is NOT "the master's half of the floor"
`P = p3_t_planes − p3_t_bsp` is the **wall-clock of the entire plane phase including the
master's spin-wait on the slave** (`RP_WaitPlanes` barrier) — the code says so verbatim:
*"P = plane phase (master half + the wait for the slave half)"* ([r_parallel.c:1483](../core/r_parallel.c#L1483)).
The wait is separately timed as `w` (overlay row 3). Since `w ≈ 0` (slave balanced), the
floor is **already split across both SH-2s**: each CPU spends ~62 ms, so the floor is
~124 ms-equiv of compute parallelised into 62 ms wall-clock. **Offloading floors (→VDP2/VDP1)
frees ~62 ms on the master AND ~62 ms on the slave.**

## 2. What's serial / parallel / offloadable

- **Serial floor (un-offloadable):** the **BSP walk `Bw` ~7–8 ms** ([r_bsp.c:580](../core/r_bsp.c#L580)) —
  depth-first occlusion recursion, true data dependency. Hard lower bound on any single
  master frame regardless of potato.
- **Already parallel (master+slave):** `P` (50/50, [r_plane.c:1224](../core/r_plane.c#L1224)),
  `M` (half-split, [r_things.c:1187](../core/r_things.c#L1187)).
- **Parallel-able but NOT yet offloaded (the headroom):** **`Bp` wall-prep ~21 ms.** The
  d32xr-style producer/consumer scaffold exists (`RP_QueueWall`/`RP_FlushWalls`,
  `sat_wallprep_defer`, [r_segs.c:728](../core/r_segs.c#L728)) but is gated **off**. This is
  the **only big lever that survives full potato** (Bp doesn't collapse).

`SLVidle` (29 %→62 %) is **real, reclaimable** spare slave capacity, but it is a *derived*
metric = `(B+M)/(B+P+M)` — the slave has no task dispatched during the BSP walk and (when
`sat_masked_parallel` off) masked. It is grounded in real dispatch structure, not a
measurement artifact. The "slave finished early, master waited" quantity is the separate
`w` (small = balanced).

## 3. Slave-reuse, ranked — **MOSTLY EXHAUSTED (HW-measured 2026-06-26)**

> **Meta-conclusion.** The slave's ~30 % idle is the **B phase (BSP walk + wall-prep), which is
> MEMORY-bound** (cold reads of geometry/textures/visplanes from LWRAM + the cart-DRAM WAD). A
> cache-cold, bus-sharing slave **cannot do memory-bound work faster than the master** (it pays the
> full `rL=2.1`), so offloading B-phase work to it does not pay. The 2nd SH-2 only helps for
> **compute-bound, cache-warm** work — which is the floor span fill (`P`), and that is **already
> split + balanced** (`w`≈0). So there is little left to reclaim on the slave; attack the *costs*
> (P via VDP2/VDP1 floor, memory latency via CCR/HWRAM — §4) instead of the slave.

Hard caps for all: slave reads LWRAM at rL=2.1 and **shares the bus**; new slave entry must use the
4 KB `rp_plane_slave_stack` + `rp_run_on_stack` trampoline ([r_parallel.c:824](../core/r_parallel.c#L824)).

1. **Work-steal the floor split** — **SHIPPED DEFAULT-ON** (dual-SH2 plane "TAS" work-steal,
   core 73f8cdc / parent 4857f87). The earlier per-plane cursor-sync variant regressed; the TAS
   form (atomic test-and-set span hand-off) is the shipping default, with the static split and the
   parked row-split selectable via pad-C. This lever is **settled** — see the dual-SH2 span-steal
   notes; row-split is parked.
2. **Asymmetric P split toward the slave** — only compute-bound lever left, but the split is already
   ~balanced (`w`≈0) so the headroom is small. Untested; low priority.
3. ~~**Slave wall-prep consumer**~~ — **BUILT (inc-1) + MEASURED DEAD-END** (`sat_wallprep_slave`,
   2026-06-26). Render byte-identical, but the slave flush `Bp` 17.4→23.2 ms (+5.8 ms, cold LWRAM) →
   inc-2 overlap win = `Bw − (Bp_slave − Bp_master)` ≈ **+0.9 ms → NO-GO**. See
   [`RANK3_WALLPREP.md`](RANK3_WALLPREP.md) §6.
4. **Pixel-balance the masked split** ([r_things.c:1187](../core/r_things.c#L1187)) — ~1–3 ms, LOW
   risk; small (M doesn't collapse with potato so it's a larger *share* there). Untested.
5. **BSP-side** — skip (bus-bound + serial).

## 4. Hardware-capacity levers (8bpp-class = free; QT = quality tradeoff)

**Free game-changers to chase first:**
- **M1 — verify/fix SH-2 cache mode (CCR).** Code only ever ORs the purge bit into
  `CCR@0xFFFFFE92` ([dg_saturn.cxx:700](../src/dg_saturn.cxx#L700)); it **never sets the
  2-way / cache-enable bits**. If SGL left it sub-optimal, forcing CE+2-way could speed
  *every* LWRAM read. **Measure CCR on HW first** — potentially the broadest free win.
- **M5 — stage hot BSP geometry (nodes/segs/subsectors) out of cart-DRAM into work RAM** at
  load. The column-cache precedent already proved ~25× A-Bus relief ([r_draw.c:138](../core/r_draw.c#L138)).
  Attacks `Bw`+`Bp` directly.
- **M3/M4 — DMA the blit + the framebuffer clear** (`slDMACopy`; raw SCU DMA failed 3×).
  ~5.5 ms + a few ms of pure master time, same pixels.
- **M2 — promote a hot LWRAM struct (visplane top/bottom, spanstart) to HWRAM** (2.1× per
  read; the framebuffer already lives in HWRAM bss).
- **V1 — 4bpp/COLOR_BANK walls** for textures whose ramp fits ≤16 entries: halves VDP1
  writes again (sequel to the shipped 8bpp pack), raises `Dr`.
- **W5 — blit only the 3D-view rows** when the HUD is static (~14 % fewer blit bytes).

**Quality tradeoffs (bigger but visible):**
- **W1 RBG0 dominant flat** — **SHIPPED** (clean 512×256 8bpp bitmap floor, commits
  19768ca/41dd895; gated potato-0 + 1-player). This is the lever that drained the dominant `P`
  bucket to a third HW unit (see §5). Details: [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md).
- **V7 VDP1-world floors** — an UNSHIPPED bet for the *residual* small visplanes only
  ([`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md)); cost = affine swim + VDP1 tear.
- **M6 global low-detail** (`detailshift`, FastDoom-style, [r_main.c:701](../core/r_main.c#L701))
  — ~halves wall+floor work; chunky 160-wide look. Currently gated to the 2p split only.

## 5. The floor question — SETTLED (dominant flat → RBG0)

The dominant flat shipped to **RBG0 as a clean 512×256 8bpp bitmap** (commits 19768ca/41dd895,
gated potato-0 + 1-player). It went to a **third HW unit** — loads neither master nor VDP1, adds
**zero tearing**, perspective-exact — and frees the master *and* the slave (both halves of `P`)
for that flat, with no Dr risk. The authoritative doc is
[`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md). VDP1 carries **walls only**; a
VDP1-world floor for the *residual* small visplanes remains an unshipped bet
([`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md)).

### `Dr` reality (verified — kept for the cost model)
pot0 `Dr` 25–42 % = **VDP1 already overruns its ~142 wall quads ~60–75 % of frames** = VDP1 is
*tight* at pot0 (not idle). The bus-relief counter-force is real and quantified: pot0→pot1 removes
the master floor half and `Dr` rises **30 %→42 %** at constant VDP1 load → killing the software
fill frees the bus and VDP1 speeds up. (This is why draining `P` to RBG0 also helps the wall
layer; it does **not** make VDP1 the fps long pole, since the master never waits on VDP1 — §0.)

> **Historical.** Earlier revisions of this section laid out "two floor plans re-gated on Dr"
> with a recommended sequencing (skip+gradient → RBG0 commit → VDP1-world residual) and go/no-go
> signals built around a *cell* RBG0 floor that "snowed" and an uncommitted CYCxx poke. That path
> is obsolete: the snow was cycle-pattern **starvation**, solved by the **bitmap** floor
> (RBG0_BITMAP=1, RDBS=0x0D, parked A0/A1 cycles) — not by slSynch and not by any CYCxx
> minefield. See [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md) for the shipped reality.
