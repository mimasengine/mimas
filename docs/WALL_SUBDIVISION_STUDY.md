# Wall sub-quad subdivision & the CPU-fallback clamp — study + roadmap

> **STATUS: Phase 0 SHIPPED; Phase 1 IMPLEMENTED 2026-07-03 (occlusion cuts, HW validation
> pending).** The shipped Phase 1 pivoted from the SPAN clamp (§5.A — v0 rejected, "warp
> affine = moche") to the **below-floor / above-ceiling occlusion cuts**: cut the tier at a
> WHOLE-TEXEL world-anchored line (straight on screen, exact at both ends) kept clear of
> min(floorclip)/max(ceilingclip), emit through the unchanged hook (corners `e∓1` absorb the
> platform's 1px pad, `v0/v1 = vcut`), and leave the residual per-column WEDGE to the software
> column loop (`sat_wall_cut_floor/_ceil` + `sat_wcl_*` in core/r_segs.c). Gate
> `sat_wall_clamp` (default ON), live A/B pad L+R+Y, row-6 FBK `W<n><+/->`.
> Deep study of SlaveDriver-style
> sub-quad subdivision applied to Mimas's walls: kill the affine squish and **replace/limit the
> CPU software fallback** for near / partially-below-floor walls. Feeds the roadmap decision
> (§6). Companion to [`VDP1_CAPACITY_STUDY.md`](VDP1_CAPACITY_STUDY.md),
> [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) (overdraw model),
> [`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md) (the floor-quad bet Phase 2 revisits).

Tags: `[HW]`/`[Ymir]`/`[src]`/`[est]` as elsewhere.

---

## 1. Mimas already subdivides (partially)
A textured wall is **already a sub-quad grid** ([dg_saturn.cxx `wall_emit`/`wall_emit_band`](../src/dg_saturn.cxx)) `[src]`:
- **u-tiling** (horizontal): 1 DISTORSP per `texw`(64)-texel tile + 1 `FUNC_UserClip`/band, cap `MAXWALLTILES=12`.
- **v-banding** (vertical): bands of texture-height H (for correct v-wrap), cap `MAXVBANDS=4`.
- Cmds/wall: **typ 2, peak 52** (4 bands × 13), flat fallback **1**, banded **2**.
- Like SlaveDriver, tile **corners** are perspective-correct; the **interior** of each tile is affine.

What is missing is subdivision that **bounds the overdraw/squish** instead of demoting the wall to CPU.

## 2. Why the CPU fallback fires — 4 causes ([r_segs.c:312-488](../core/r_segs.c)) `[src]`
| Cause | Test | Axis | Nature |
|---|---|---|---|
| **SPAN** | `s > SAT_WALL_CPU_SPAN(480)` (hyst. V1=576) | vertical | overdraw + VDP1 coord range (tall/near wall) |
| **MAGNIFICATION** | `sx > mdu*SAT_WALL_CPU_MAG(3)` px/texel | horizontal | squish/"écrasement" (face-on doors) |
| **below RBG0 floor line** | `yh ≥ floorclip` | vertical | quad can't clip to the floor line |
| **starve** | `wall_acc_n ≥ WALL_ACC_MAX` | budget | command bank full |

**Horizontal partial occlusion is already resolved before emit** (`R_ClipSolidWallSegment`
splits a mid-occluded wall into visible x-fragments → DISTORSP spans only visible columns; no
horizontal overdraw). **The only overdraw is the off-screen VERTICAL trapezoid of near walls**
(projects to y≈±2000, VDP1 iterates ~4000 rows for ~200 visible = ~20×; clip suppresses the
*write*, not the *iteration*).

## 3. The insight — walls do NOT vertically swim
A wall column has ~**constant z** → screen-y is **linear in texture-v** → the affine DISTORSP is
**exact vertically**. So a **world-anchored vertical clamp** — clamp the quad to [0,223] **and
re-derive the texels at the clamp point** — is **exact, swim-free** (unlike floors, which have
1/z curvature and genuinely swim).

**`SAT_YCLAMP` was never real code** — a doc-only cautionary law from a reverted experiment
([VDP1_ARCHITECTURE.md:136](../src/dg_saturn.cxx), [VDP1_WORLD_PLAN.md:405](VDP1_WORLD_PLAN.md)).
It was **screen-anchored** (clamp y to a fixed edge + keep v → "texels slide every frame").
The project generalised "textured near walls must go to CPU" from that broken variant. The
**world-anchored** clamp (the docs' own prescribed alternative, used for floors via `yslope`)
was **never tried for walls** — and is *simpler* there (linear, no 1/z). **This is the gap.**
The flat fallback already clamps ([dg_saturn.cxx:2838](../src/dg_saturn.cxx), "no texture = no
swim"); the linear mapping means a *textured* clamp is equally swim-free.

## 4. SlaveDriver ground truth (saturn-refs/SlaveDriver-Engine) `[src]`
- Walls = floors = one primitive: baked `tileLength×tileHeight` grid of `FUNC_DISTORSP`, one cmd/cell.
- **Anti-swim = subdivision, and only that** — lattice vertices get a per-vertex 1/z divide
  (`rectTransform`); interior is affine; smaller cells = less swim, **1:1 command cost**.
- Guards: `NEARCLIP F(32)` (whole-wall near clip before subdivision), `TILENEARCLIP F(33)`
  (per-vertex z floor), **±F(2000) coord clamp**.
- **`MIPDIST F(256)`**: grid ÷2 (**¼ commands**) + half-res box-filtered art beyond the distance.
- **100% VDP1, NO CPU rasterizer**: partial occlusion = per-sector `EZ_userClip` (HW scissor) +
  per-tile bbox cull (off-window cells = 0 cmd); overflow policy = **drop the whole surface**, never spill CPU.
- Bank 1540 cmds/frame; batched DMA staging. **COLOR_5 16bpp** → gouraud/mesh work there;
  the **geometry transfers to 8bpp**, the **gouraud does not** (breaks on palette banks).

## 5. The design (hybrid/partial clamp)
- **A. Vertical world-anchored clamp + cull (cause SPAN + below-floor) — THE win.** Per u-tile,
  clamp screen-y to the view, trim the texel range to visible, cull fully-off-screen bands.
  Linear → exact, swim-free. **≈0 extra commands (cull can *reduce* them)**, and it **removes the
  software column draw of near walls** (the most expensive software walls) = **net-master-positive**.
- **B. Horizontal u-trim (cause MAGNIFICATION) — the hard residue.** Face-on doors: trim u to the
  visible texels + ±coord clamp. ~exact (face-on = low horizontal perspective) but fiddlier; may
  not beat keeping flat/banded/CPU.
- **C. Borrow from SlaveDriver:** extend the per-tile cull to the vertical axis; add **MIP
  distance-LOD** to *reclaim* command budget on far walls.
- **D. Do NOT copy:** 16bpp/gouraud (broken on 8bpp), drop-whole-surface (Mimas's degrade-to-flat
  is better), HW-divider (reciprocal-mul `SAT_WALL_RMUL` amortises better).

**The two tensions:** subdivision costs **command slots** (scarce, HWRAM-gated) **and master
build-`k`** (11-28 ms, the bottleneck). So *finer subdivision everywhere is anti-productive*
(loads the master). The right move is **clamp/trim/cull** (same/fewer cmds, **less** software `k`);
MIP *recovers* budget rather than spending it. The SPAN clamp is net-positive; the MAG-trim marginal.

## 6. Roadmap — highest ROI first

**Option 2 (wall partial quads) > Option 1 (floors→VDP1).** Option 2 wins on effort, risk, command
cost, and is **net-master-positive**; Option 1 has a bigger *potential* fps gain but is risky
(floor swim), command-heavy (~158 quads bust the 248 bank), and **may raise REC** (world-plan's own
open unknown). Both also improve **quality** by consolidating software surfaces onto VDP1 (the
misaligned H/V planes today are the *software* floors + fallback walls; keep the dominant floor on RBG0).

| Phase | What | Gate |
|---|---|---|
| **0 — measure** ✅ SHIPPED | FBK fallback profiler + PERFSIM floor-sim (§7) | — |
| **1 — wall clamp** | vertical world-anchored clamp+cull for SPAN (+ below-floor), behind a toggle; A/B HW (swim? Δmaster) | Phase-0 shows big `clamp`/`K` |
| **1b — wall MIP** (opt) | distance grid-halving → reclaim command budget | budget pressure |
| **2 — non-dominant floors→VDP1** | reuse Phase-1's world-anchored machinery; **dominant stays RBG0** | Phase-0 `ΔP(0→2)` big + budget freed + swim HW-validated |

Per mode: 1j has command room (do Phase 1); 2j workable; 4j (30/view) too tight for finer
subdivision — there the **banded/flat** modes (already swim-tolerant, fallback-off) are the lever,
though the SPAN clamp (constant cmds) still applies.

## 7. Phase 0 — SHIPPED instrumentation (read protocol)

**Counter 1 — `FBK` (overlay row 6)** `[src]` — `core/r_segs.c` counts fallback tiers by cause
(8 sites, gated `!sat_v1_mid`/`sat_v1_up/lo` so no double-count; inert on DoomJo), folded per-frame
in `vdp1_wpn_kick`:
```
FBK c<cur>/<pk>  m<cur>/<pk>  s<pk>  K<cur>/<pk>
```
- **c** = clampable tiers (SPAN + below-floor) = **the Phase-1 target**
- **m** = magnified residue (face-on doors, clamp can't fix)
- **s** = starved (bank full — Phase 1 *worsens*)
- **K** = clampable fill proxy (kilo-pixels = the master software fill Phase 1 removes)

Read in corridors / facing walls / combat. **Big c + K, small m/s → build the clamp.** Counts are
deterministic (valid on Ymir); K→ms is HW-only.

**Counter 2 — floor residual `P`** (already built): pad-Y cycles `SAT_FLOOR_PERFSIM` (row 19),
read REC row 4 / P per mode. **Mode 2 "ALL-BUT-DOM"** skips the non-dominant floors →
**`ΔP(mode0 − mode2)` = the residual non-dominant P = Option-1's ceiling.** Small (high `dom%`,
row 17) → Option 1 not worth it; big (low `dom%`) → worth the risk. `PAR Q/Qp` (row 20) = the
full-VDP1-floor quad count (Option-1 command budget).

Both HW-only for the ms verdict (Ymir understates memory-bound). Counters active in 1j (and 2j VDP1-split).

> **Note:** `core/r_segs.c` (+ `core/d_main.c` overlay trim) are **shared core** — commit to the
> `core/` submodule and pull into DoomJo per `CLAUDE.md`. DoomJo is behaviourally unchanged.
