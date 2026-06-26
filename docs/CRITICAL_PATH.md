# Mimas — critical-path model & headroom levers

> **Source.** Derived from the HW set `REC_BENCHMARKS.md §C.2` (2026-06-25/26, E1M1
> stripped, 2 spots, immobile) + a code-verification pass (2026-06-26, 4 grounded
> readers + 5 adversarial verifiers). Every claim here is anchored to `file:line`.
> When this doc and a plan doc disagree, **this doc's measured/verified facts win**;
> the plan docs (`VDP1_WORLD_PLAN.md`, `VDP2_FLOOR_CONSOLIDATION.md`) are re-gated on it.

## 0. The one-paragraph model

`fps = 1000 / MST` and the frame is **master-bound** (no per-frame `slSynch`; the loop
returns when the master finishes compute + blit — [dg_saturn.cxx:2734](../src/dg_saturn.cxx#L2734)).
Causal direction in code is `MST = 1000/fps` (fps is measured from `DG_DrawFrame`
completions vs real vblanks; MST is its reciprocal — [dg_saturn.cxx:998](../src/dg_saturn.cxx#L998)).
The master **never waits on VDP1**: the wall kick is fire-and-forget (`VDP1_PTMR=0x0002`,
[:2558](../src/dg_saturn.cxx#L2558)) and the shipped present is **1-cycle auto** (`VDP1_FBCR=0x0000`,
[:2402](../src/dg_saturn.cxx#L2402); `VDP1_MANUAL_CHANGE 0`, [:135](../src/dg_saturn.cxx#L135)).
⇒ **low `Dr` (VDP1 overrun) = wall-layer TEARING, never an fps stall.** This is the key
correction: VDP1 work can never cap fps, only degrade the VDP1 layer's image.

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

1. ~~**Pixel-balance / work-steal the floor split**~~ — **TRIED, REGRESSED** (`sat_plane_steal`,
   2026-06-26): per-plane uncached cursor sync + slave startup latency + crossing overlap > the tiny
   count-split imbalance at n~20 (`w` was ~0 already, only spiking on rare frames). Reverted to the
   static split (default `ws0`). See REC_BENCHMARKS §C.2 H.
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
  writes again (sequel to the shipped 8bpp pack), raises `Dr`, unblocks the floor plan.
- **W5 — blit only the 3D-view rows** when the HUD is static (~14 % fewer blit bytes).

**Quality tradeoffs (bigger but visible):**
- **V7 VDP1-world floors** — kills the dominant `P` bucket; cost = affine swim + VDP1 tear.
- **M6 global low-detail** (`detailshift`, FastDoom-style, [r_main.c:701](../core/r_main.c#L701))
  — ~halves wall+floor work; chunky 160-wide look. Currently gated to the 2p split only.
- **W1 RBG0 dominant flat** (perspective-exact, **HW-blocked** on the CYCxx commit) /
  **W4 back-screen gradient floor** (the ~0-bank, sky-preserving, no-minefield variant).

## 5. The two floor plans, re-gated on measured `Dr`

**`Dr` reality (verified):** pot0 `Dr` 25–42 % = **VDP1 already overruns its ~142 wall quads
~60–75 % of frames** = VDP1 is *tight* at pot0 (not idle). This **falsifies the headline
premise** of `VDP1_WORLD_PLAN.md` §0.3/§7.4 ("VDP1 idle, Dr 92–94 % measured").

**Can VDP1-world floors raise fps?** Two corrections vs the plan AND vs the workflow's first
read:
- Because the master never waits on VDP1 (§0), **moving floors→VDP1 DOES bank the fps gain**
  (master loses ~62 ms of floor compute). VDP1 does **not** "become the long pole that caps
  fps" — that read assumed a gated present (compiled out). **The cost is image quality: the
  VDP1 floor+wall layer tears more** (Dr drops further).
- The bus-relief counter-force is real and quantified: pot0→pot1 removes the master floor
  half and `Dr` rises **30 %→42 %** at constant VDP1 load → killing the software fill frees
  the bus and VDP1 speeds up. But from 30 %, even +~25 pts of bus credit lands ~55 % — below
  the plan's own 80 % comfort line, *before* charging the ~158 floor quads' overdraw. So at
  pot0 **floors-on-VDP1 = real fps win but visible tearing**; how bad depends on post-floor Dr.

**Why VDP2/RBG0 (or the skip+gradient) is structurally safest:** the dominant flat goes to a
**third HW unit** — loads neither master nor VDP1, adds **zero tearing**, perspective-exact.
It frees the master *and* the slave (both halves of `P`) for that flat, with no Dr risk.
Sized per-scene by **`d%`** (nukage 48 %, cour 95 % of floor pixels): win ≈ `d% × (floor P)`.

**Composition (the plans are partners, not rivals):** RBG0/skip takes the **dominant** flat;
VDP1-world (or software) takes the **residual** small visplanes (`Vs = vqtot − vdom`,
[VDP1_WORLD_PLAN.md §6]). RBG0 carrying the dominant is exactly what keeps the residual VDP1
floor list small enough for the §7.4 "1.6–2.5 ms async" claim to hold.

### Recommended sequencing
1. **Ship the dominant-flat SKIP + flat back-screen/line-colour gradient fill first.** The
   software skip already exists ([r_plane.c:1095](../core/r_plane.c#L1095)); the gradient is
   ~0 bank, keeps the HW sky, dodges the RBG0 CYCxx minefield entirely. **Lowest-risk,
   highest-confidence fps move** (captures most of the master+slave floor relief now).
2. **Then attempt the RBG0 commit** (CYCxx poke) for the perspective-exact quality upgrade on
   the dominant; gated on rows 13/14/15 + "snow gone". Falls back to step 1 if it won't stick.
3. **Then build VDP1-world for the residual only**, and **build the live floor A/B chord first**
   (`VDP1_FLOOR_TEST_AB`, §8.2 — does not exist yet) so you can measure floors-on `Dr` and the
   swim on the same still scene. Go iff post-floor `Dr` stays acceptable (tearing tolerable)
   AND row 5 `P` drops without row 4 `REC` rising.

### Go/no-go signals
- **RBG0/skip:** rows 13/14/15 (`CYa≠CYb`, banks `FFFFFFFF`) + snow gone; then `P`↓ and blit↓
  at pot0; `d%` sizes the win.
- **VDP1-world:** **row 2 `Dr%` is the gate** — toggle floors-on at a still spot, GO iff `Dr`
  holds (tearing acceptable); `Vp`/`tr` truncations 0; `P`↓ without `REC`↑.
