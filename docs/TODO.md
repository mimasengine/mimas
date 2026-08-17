# Mimas — TODO

Running list. Ordered: the governor first (the playability wall), then the visual
defects the owner reported 2026-08-16.

Every entry states **what is measured** vs **what is assumed**. Do not promote an
assumption to a cause without the subtraction ([[budget-before-mechanism]]).

---

## 0. THE GOVERNOR — make 5 fps not happen

### 0.1 The frame is not one thing: there are TWO regimes, measured

| regime | capture | fps | MST | `R` | dominant term | tic |
|---|---|---|---|---|---|---|
| **render-bound** | 2026-08-16 c1 | 5,5 | 181 | 150 | **`Bp` 110,8** (`P`44,6 `Bw`7,4 `M`3,3) | `T18 th16,0 x2,1` |
| **render-bound** | 2026-08-16 c2 | 4,3 | 232 | 210 | **`Bp` 91,0** (`P`43,9 `M`4,1) | `T19 th17,7 x2,2` |
| **tic-bound** | DOOM-TNT-latest t70 | 3,6 | 277 | 177 | `th` **111,4** | `T115 s2,2` |

The governor today only degrades the RENDER (axes `B`/`P`/`M`). It has **no axis at
all** for the tic-bound regime, and in the render-bound one it elects `B` and pulls
the wall-LOD rung — which flattens *texture on small distant walls* while the cost
is `Bp` = R_StoreWallRange over **`ds118` drawsegs**.

**Flattening a wall does not remove it from the seg list.** That is the gap.

### 0.2 What to give it — ranked by (measured size / cost to build)

1. **A drawseg budget.** `ds118` with `Bp110` is ~0,9 ms per drawseg. A rung that
   caps drawsegs (drop the farthest first, they are the cheapest to lose visually)
   attacks `Bp` directly instead of by proxy. ⚠ Must drop **far** first — the
   opposite mistake killed the VDP1 wall offload ([[wall-offload-vdp1-slave-dead]]).
2. **A things axis.** `M` has no knob. `sat_thing_role_cull` / `sat_thing_cult_dist`
   exists but is static at 1024. Make the distance a governed rung (1024/768/512).
   Small in these captures (`M3,3`) — build it only when a capture shows `M` electing.
3. **Feed the governor COUNTS, not only times.** `ds`, `vp`, `ss` are on screen and
   are leading indicators; `Bp` is the lagging one. Entering a big room is visible in
   `ds` one frame before it is visible in `Bp`.
4. **A tic axis** for the tic-bound regime — but see 0.3 first, which may dissolve it.

### 0.3 🔴 THE GAME IS RUNNING AT ~1/3 SPEED — read this before building a tic axis

`x` (tics per frame, shipped 2026-08-16) says so on its first capture:

| capture | fps | `x` measured | `x` expected (35 / fps) | ratio |
|---|---|---|---|---|
| c1 | 5,5 | **2,1** | 6,4 | 0,33 |
| c2 | 4,3 | **2,2** | 8,0 (capped) | 0,27 |

The game world advances at ~10-12 Hz instead of 35. This is the **ralenti invisible**
that [[gametic-slowmotion-tic-cap]] says was fixed by raising the maketic cap +2 → +8
("vrai 35Hz jusqu'à ~4fps"). At 4-5 fps it is **not** being achieved.

**Prime suspect, and it is already half-proven:** `maketic` is driven by `d_ms()` /
`DG_GetTicksMs`, the same clock caught SATURATING at 72-73 ms on hardware across three
different frame rates while the FRT said 106-110. A millisecond clock that
under-reports elapsed time hands `TryRunTics` too few tics — exactly this symptom, from
exactly the defect already measured for another reason.

**This is cheap to test and changes how the game FEELS at a given fps**, which is worth
more than the next few ms of render. Measure `DG_GetTicksMs` against the FRT over a
second before touching anything.

⚠ Open question for the owner once it is fixed: at 5 fps, true 35 Hz means ~7 tics of
monster movement between two displayed frames. Correct Doom, possibly worse to play.
The honest answer is to fix the clock first and let him judge, not to pre-decide.

### 0.4 Offline (WAD) vs live — both, and they do not overlap

They answer different questions and neither substitutes for the other:

- **Live** owns the *dynamic* peak: a horde spawns, you turn into a big room. No
  offline pass can see that coming.
- **Offline** owns the *structural* cost that is identical every time you enter that
  room, and it is FREE at runtime.

**The highest-value offline lever we have proven today is texture size.** Every
256x128 TNT patch is **35080 bytes and must land in ONE contiguous run**, against a
longest run measured at **20-38 KB depending on the scene**. That is a cliff the
engine cannot dodge — it is the bug that ate the sky, and at `lg20k` *no* 256x128
patch fits, walls included. `tools/strip_wad.py` already rewrites the WAD:
down-sizing the big patches (256x128 → 128x128 ≈ 9 KB) removes the whole class.

Other offline candidates, unmeasured: seg/detail reduction on the worst maps, flat
count per neighbourhood (sizes the `FLT` pool, see [[streaming-load-budget-and-flat-treadmill]]).

### 0.5 What is expensive on Saturn — the standing list

Ordered by how often it has actually bitten this project:

1. **Any contiguous zone allocation over ~32 KB.** The run is structurally 20-38 KB
   (~366 KB of small unpurgeable PU_STATIC texture blocks chop the middle).
   *Everything* above that size is a lottery, and losing the lottery is silent.
2. **Per-column software fill** (`Bw`/`Bp`/`P`/`M`) — one 28 MHz SH-2 on a shared bus.
3. **Pointer-chasing over the LWRAM (DRAM) zone heap** — thinkers, BSP, LOS.
   4 KB write-through cache, one system bus for two CPUs ([[saturn-memory-map]]).
4. **Synchronous CD reads inside the frame loop** — ~33 ms each.
5. **VDP1 command COUNT and transfer-over**, not fill ([[vdp1-transfer-over-lopr-probe]]).
6. **Composite rebuilds** (`cb`) — 8..32 KB column copies.

---

## 1. BUGS reported by the owner, 2026-08-16

### 1.1 ✅ CONFIRMED — the TNT sky shows only a quarter of itself
Owner: *"mimas-tnt a un ciel non continu. Je suppose que … il est en plusieurs parties
et qu'on en affiche une seule."* **Correct**, verified against the WAD:

```
TNT SKY1/SKY2/SKY3 = 1024x128, FOUR 256-wide patches
   originx = 0 / 256 / 512 / 768
```
`sky_cell_upload` (dg_saturn.cxx) hardcodes `ccol < 32` × `rx < 8` = **columns 0..255**,
i.e. patch 0 only, then tiles that quarter twice across the 512 px NBG0 page. DOOM1
shareware is the same 1024x128/4 — the bug is there too, just invisible on a uniform sky.

**The fix is a VRAM budget decision, not a loop bound.** A full 1024-wide 8bpp cell sky
is 128x16 cells = **128 KB of VDP2 VRAM**; today's quarter uses ~32 KB in bank B1's low
half (map at +0x A000). Options, cheapest first:
- **2:1 horizontal downscale** of the full 1024 into the existing 512 page (take every
  other column). Keeps VRAM, keeps the layer, sky is low-frequency — likely invisible.
- 4bpp cells → 64 KB for the full width (see [[rbg0-cell-floor-4bpp-snow-fix]] for the
  4bpp trap on hardware).
- A 1024-wide plane (`PL_SIZE_2x1`) at 128 KB — does not fit beside the RBG0 floor.

⚠ Also re-check the SCROLL divisor: with 1024 mapping to 360°, the current
`SKY_ANGLESHIFT` was calibrated against a 256-wide layer.

### 1.2 Transparent grates vs monsters are z-inverted
Owner: *"quand la grille est derrière le monstre, on la voit à travers le monstre.
Quand la grille est devant le monstre, le monstre cache la grille."*

**Structural, not a rounding bug:** world things are on **VDP1** (sprite priority 5)
while masked midtextures (grates) are drawn into the **NBG1 software framebuffer**
(priority 6). NBG1 sits above every sprite, so the grate wins *unconditionally* —
which is right in one of the two cases and wrong in the other, exactly as described.
Two surfaces on two layers with a fixed priority **cannot** z-sort against each other.

Fix directions: put masked midtextures on VDP1 too (they are already texture-mapped
quads), or route things overlapping a masked midtexture back to the software path.
See the layer-inversion contract in [[doomsrl-vdp1-capacity]].

### 1.3 1 px vertical gaps — LOCALISED by the owner: **between two VDP1 walls**
Owner, after investigating: *"Les bandes 1px verticales manquantes se produisent aux
jonctions entre deux murs vdp1."*

That kills the CPU↔VDP1 routing hypothesis and leaves **quad edge rounding**: two
adjacent quads each round their own x independently, so one screen column ends up
inside neither. The software path never had this — it walks columns, so column *n*
belongs to exactly one seg by construction; a quad rasteriser has to be *told* where
the shared edge is.

**Fix direction:** make the shared edge explicit rather than emergent — the left quad's
right edge must be *the same number* as the right quad's left edge, not a rounding of
the same world point computed twice. Failing that, the horizontal twin of `Wg`
(grow each quad 1 px right, since the neighbour will overwrite it) closes the seam at
the cost of a 1 px overdraw. **Grow is the cheap patch; the shared edge is the fix.**

⚠ Order this AFTER 1.4 — both are in the emission path and 1.4 may be the same rounding
producing a degenerate (zero-width) quad rather than a 1 px one.

### 1.4 A wall routed to VDP1 is never drawn — **the counter for this already exists**
Owner: *"le mur invisible devrait être vdp1 mais n'est pas affiché. je vois le
'rattrapage de mouvement' (fallback) cpu s'afficher pour ce mur, mais pas le mur
lui-même."*

This is diagnostic gold: the lead-fill spans ARE drawn, which proves the core routed
the seg to VDP1 (the software loop skipped it, and the lead fill only records quads
actually handed to VDP1). So the loss is **downstream of the routing, inside the emit
dispatch** — exactly what `N<orphan>/<drop>/<flip>` on row 13 was built to separate:

- **`drop` > 0** ⇒ `vdp1_wall_drop`: the core handed the wall over and the emit
  dispatch silently returned early. Measured at the command pointer (`vdp1_wnext` did
  not move across the emit call), so it catches every early return without auditing
  them: **the wall-cap guard, a texture slot that will not resolve, a degenerate quad.**
- **`orphan` > 0** ⇒ claimed by neither path (should be 0 here — the lead fill proves
  VDP1 claimed it).
- **`drop0` and `orphan0`** ⇒ the wall was emitted and VDP1 did not plot it: a
  different search entirely (bank, clip, or the wall-cap on the VDP1 side — watch
  `V1 ... W<n>/<n>` on row 19 and `B<n>` for a budget latch,
  [[vdp1-budget-latch-kills-sprites]]).

**Next capture must include row 13.** One photo picks the branch, and the three
candidates behind `drop` are each a few lines to check.

---

## 2. Branch state

On **`flicker-clean`**, 44 commits ahead of `master`, 0 behind. Nothing has been pushed.
Merging to master is an owner decision, not a prerequisite for any of the above.
