# VDP1 floor — the dominant-flat affine-strip plan

Written 2026-06-23 after a 7-agent investigation (audit of the current VDP1 + software
floor code, SlaveDriver floor technique, hardware feasibility, and a 3-way design panel).
Companion to [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) (the chip/cost reference),
[`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md) and [`RBG0_FLOOR_PLAN.md`](RBG0_FLOOR_PLAN.md)
(the VDP2 alternative that broke on hardware).

**TL;DR conviction (multi-tier, revised 2026-06-23).** Floors/ceilings split across
chips **by strength**, not RBG0 **xor** VDP1 — they **compose**:

| Surface | Tier | Why |
|---|---|---|
| **Dominant floor** (biggest `(picnum,height)`, ~49–93 % / avg ~64 % of floor fill) | **RBG0** | one big perspective-correct plane = RBG0's whole job; it is also VDP1's **worst** swim case (hugest 1/z range), so giving it to RBG0 fixes VDP1's weakness |
| **Other-height floors + non-sky ceilings** | **VDP1** affine strips | multi-surface (RBG0 can't — one plane = one height); smaller/nearer = **less swim**; rides the **hardware-validated async driver** |
| Tiny / over-budget surfaces | **NBG1 software** | the long tail |
| Sky | **NBG0** | done |

Each chip covers the other's weakness (RBG0 = VDP1's worst swim plane; VDP1 = RBG0's
one-height limit). The priority stack already encodes it: `sky NBG0(3) < RBG0(4) <
VDP1(5) < NBG1(6)`. **Full** per-visplane VDP1 floors stay a hard **NO-GO** (busts the
command/CPU budget); the VDP1 tier is the **bounded secondary set**, capped, with a
software fallback. This plan covers the **VDP1 tier**; RBG0 is the parallel
[`RBG0_FLOOR_PLAN.md`](RBG0_FLOOR_PLAN.md) (commit on HW in ~2 days). The earlier
"dominant flat on VDP1" framing is **superseded**: the dominant goes to RBG0, VDP1 takes
what RBG0 cannot.

---

## 1. Why this, and why not the obvious alternatives

Three independent design agents, each assigned a different lens (true-3D per-subsector /
visplane-driven strips / economical dominant-flat), **all converged** on the same answer.
The rejected paths and why:

| Path | Verdict | Why |
|---|---|---|
| **Full per-subsector tessellation** (project every BSP subsector floor polygon, tessellate to affine-safe quads — the SlaveDriver model) | **NO-GO** | Doom floors are **runtime arbitrary spans**, not PowerSlave's **design-time tile grids**. Tessellating 256 visplanes × 8–32 sub-quads = **2 000–8 000 quads/frame** — busts the 256-cmd bank *and* the whole-Saturn practical ceiling (~1 300–2 000 quads/30 fps, where the **SH-2 transform+build is the limiter**, not VDP1). Collides head-on with the wall list. |
| **VDP2 RBG0 Mode-7 floor** (the other plan) | **PARKED** | Perspective-correct & zero-CPU, but **single-height** and **broke on real HW**: the cycle-pattern/RAMCTL never commit without `slSynch`; the one-shot `slSynch` fix was HW-tested *worse* and reverted ([`RBG0_FLOOR_PLAN.md`](RBG0_FLOOR_PLAN.md), [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md)). |
| **Dominant-flat VDP1 affine strips** | **GO-PARTIAL** | Bounded (~30–72 cmds, 4 KB VRAM, ~2.5 ms async fill, sub-ms CPU). Rides the **validated** async driver. Multi-surface-*extensible* later (unlike RBG0's one plane). Recovers the dominant flat's ~64 % of floor fill. **Swim** is the price (RBG0 has none). |

**Single biggest economy:** the hard part is **already built**. The
`sat_vdp2_floor` hook (added for RBG0) already does the **stable** dominant-flat
selection *and* the **index-0 skip** (leaves the matching visplane transparent in NBG1
for a lower layer to show through). We **reuse it verbatim** and route the skipped
surface to **VDP1 strips** instead of RBG0 — no new selection logic, no flicker.

---

## 2. How it works

```
R_DrawPlanes: for the ONE dominant (picnum,height) visplane
  → leave it as index-0 in NBG1            (reuse sat_vdp2_floor skip)
  → hand its silhouette+height+light to the platform (new NULL-default hook)

Platform (dg_saturn.cxx): emit N≈16–32 horizontal AFFINE STRIPS
  per strip [ya,yb):
    ymid → distance = FixedMul(planeheight, yslope[ymid])     (R_MapPlane math, ONCE/band)
         → u/v basis = distscale[x]·viewangle (xfrac/yfrac/xstep/ystep)
    split [ya,yb) into x-runs inside the visplane silhouette  (the spans, regrouped)
    each run, each 64-texel u-tile → one FUNC_DISTORSP 8bpp quad
       CMDCOLR = CRAM light bank from distance  (reuse wall_light_colr)
       texture = the flat's 64×64×8bpp lump     (reuse wall_tex_resolve, 4 KB)
       FUNC_UserClip window + 1px overlap        (reuse wall_emit_band's seam fix)
  → present below NBG1 (priority 5, same as VDP1 walls)
```

**The affine trick (Mode-7-on-VDP1).** VDP1 interpolates texture (u,v) **linearly in
screen space** — correct only where depth is constant. A floor's 1/z range (feet →
horizon) is enormous, so **one** quad swims badly. Slicing into **thin horizontal bands**
where z is ~constant makes each band's affine map near-exact: this is the classic
constant-z-scanline / piecewise-linear-1/z approximation (SNES Mode-7, software floor
casters). Error ∝ `(1/z curvature) × band_height²`, **concentrated at the horizon**.

**The CPU asymmetry that is the whole win.** The per-band u/v basis is the *same*
`FixedMul` trig `R_MapPlane` already runs — but **once per band (~24×)** instead of
**once per scanline (~112×)**. So a VDP1 strip floor is *cheaper* on the CPU than drawing
the same surface in software, and the **fill runs async on VDP1** in parallel with the
remaining software floors/sprites/walls.

**Layer-inversion fit.** Identical to the walls: software skips the surface (index-0),
VDP1 fills it *below* NBG1, software occlusion (clip arrays) keeps sprites/walls correct.
Because walls and the floor occupy **disjoint** screen pixels (they tile without overlap
in software, and each only shows through *its own* index-0 gap), **paint order between
walls and floor is moot** — they never touch a shared pixel.

---

## 3. Capacity — do we have the room? (corrected ledger)

**Yes, with margin — the binding constraint is command count, not fill or VRAM.**

| Resource | Budget | Floor (dominant, partial) | Fits? |
|---|---|---|---|
| **Fill** | full 320×224 textured ≈ 5 ms; frame ≈ 100 ms @10 fps | ~half-screen × 64 % ≈ **1.6–2.5 ms async** (~2 % of frame) | ✅ trivially |
| **VRAM** | **512 KB** total (`0x25C00000–0x25C7FFFF`); walls use ~480 KB; **free tail ≈ 44 KB** (`0x25C75000–0x25C7FFFF`) | one 64×64×8bpp flat = **4 KB** (reuse a wall cache slot) + optional 2nd cmd bank (2×8 KB) | ✅ (≈20 KB of the 44 KB) |
| **Commands** | bank = 256 cmds; walls use **~150 → ~100 free** | **~30–72 cmds** (N strips × x-run fragments × 64-texel u-tiles) | ✅ *if fragmentation stays bounded* — **the one number to measure first** |
| **CPU build** | the SH-2 is the frame bottleneck (REC×N) | **sub-ms** (band-amortised trig) | ✅ cheaper than the software draw it replaces |

> **VRAM correction.** The investigation's "1 MB VRAM / 576 KB free tail" was **wrong** —
> it folded the **framebuffer** region (`0x25C80000+`, 2×256 KB, *separate*) into VRAM.
> VDP1 VRAM is **512 KB**; the real free tail is **~44 KB**. Still enough for the flat
> texture (4 KB) **and** a second double-buffered command bank (16 KB) — but it is tight,
> so removing the **dead weapon/HUD VRAM reservations** (§5) first is worth it.

**The make-or-break number = floor command count.** Two things inflate it: (a) **x-run
fragmentation** — foreground columns/pillars shatter a band into multiple runs; (b)
**u-tiling** — VDP1 DISTORSP does **not** wrap a 64×64 flat, so a strip wider than 64
world-texels needs sub-quads (like `wall_emit_band`'s u-loop). A near strip can need
4–12 sub-quads. **Increment 0 measures this offline before any emitter is written.**

---

## 4. Build increments (riskiest unknown first; core stays DoomJo-safe)

Every `core/` change is a **NULL-default function-pointer / flag hook** (DoomJo never sets
it → byte-identical, pure C). Mirrors the proven `sat_vdp2_floor` / `sat_wall_*` pattern.

> **RECONCILED 2026-06-24 — status of the increments below:**
> - **inc-0 (profiler): BUILT** (`RP_PlanePixels`, `RP_PROF`), but its GO/NO-GO peak `Vp`
>   has **NOT been read** on HW/Ymir — the single number that decides the approach is still
>   unmeasured.
> - **inc-1: STUB ONLY.** What exists is an own-everything skip stub (`sat_floor_vdp1_stub`
>   returns 1 for *every* candidate, `sat_vdp1_floor=0` boot default, pad-Y toggle) that
>   validates coverage but has **no per-dominant skip logic and emits zero strips**.
> - **inc-2..6: UNBUILT.** No kick plumbing, no affine strip emitter, no silhouette clip,
>   no lighting, no gate. **NO floor strip is emitted today.**
> Net: floor offload is **PARTIAL / STILL-TODO**, gated on reading `Vp` first.

0. **MEASURE — BUILT (2026-06-23, the GO/NO-GO gate).** The profiler lives in
   `RP_PlanePixels` (`core/r_parallel.c`, under `RP_PROF`): for every non-sky **regular
   flat** — which is exactly the VDP1 candidate set (other-height floors + ceilings; the
   RBG0 view-sector floor is already `continue`d *before* this call, so it is excluded) —
   it estimates the would-be VDP1 quad count as **bbox-clamped affine strips**
   (`FLOOR_HBAND=8` rows/band, tiles = `64-texel u-span / 64`, capped `FLOOR_MAXTILES=16`).
   Read **overlay row 13**: `FLAT d{dom%} n{visplanes} Vt{total} Vs{secondary} Vp{peak}`.
   `Vs` = the VDP1 candidate cost (= `Vt` when `sat_vdp2_floor=1` since RBG0's plane is
   already excluded; else `Vt − dominant-group`); **`Vp` = monotonic peak across the
   session = the go/no-go number.** Walk E1M1 outdoor (93 %), MAP01, big halls. **GO if
   `Vp` ≲ 64** (fits the wall bank's ~100-cmd free headroom) **or ≲ 120** (justifies a 2nd
   bank); if it explodes → coarser bands (raise `FLOOR_HBAND`) / near-floor-only / abandon.
   Pure C, **zero DoomJo impact** (shared core, `sat_vdp2_floor=0` there). Compiles clean.
   *This single number decides the whole approach — measure it on HW/Ymir first.*

1. **Index-0 skip via the existing hook.** [STUB-ONLY 2026-06-24] Extend `sat_vdp2_floor`'s
   skip to a `sat_vdp1_floor` gate: when set, leave the dominant visplane index-0 (the
   **exact** existing loop) and stash the silhouette+height+cmap for the platform. Off by
   default. *Check:* the dominant floor becomes a transparent hole — confirms selection+skip;
   byte-identical OFF.
   > **As-built:** only `sat_floor_vdp1_stub` exists, which claims **every** candidate
   > (returns 1), not the per-dominant skip described here — it validates coverage but does
   > not stash silhouette/height/cmap and emits nothing. The real inc-1 is unbuilt.

2. **Kick/paint-order plumbing (the riskiest plumbing).** Floors are known only *in*
   `R_DrawPlanes`, but the wall kick fires *after BSP, before planes*. Two options:
   - **(a) Second VDP1 bank** (2×8 KB in the free tail) kicked by a new
     `sat_floors_done_hook` after `R_DrawPlanes` — **isolates** the shipped wall kick
     (safest; +16 KB VRAM, a second `PTMR`).
   - **(b) One bank, move the kick** to after `R_DrawPlanes` (walls still accumulate
     during BSP) — minimal, but touches the shipped wall timing (regression risk).
   Prefer **(a)** unless the BSP→planes gap proves negligible on HW.

3. **One coarse strip, near-field, no clip.** Emit full-width DISTORSP strips for the near
   half, per-band 1/z from `yslope`, no lighting. **Validate swim** at `N=16/24/32` on HW;
   pick the knee. (Riskiest *visual* unknown, on a simple surface before clip complexity.)

4. **Silhouette clip + seams.** Split strips into x-runs inside the silhouette + per-run
   `FUNC_UserClip` window + 1px overlap (reuse `wall_emit_band`). Confirm the floor shows
   **only** through its index-0 gap (walls/sprites occlude correctly via NBG1).

5. **Lighting + horizon anti-swim + budget reserve.** CRAM-bank distance light
   (`wall_light_colr`); **non-uniform bands** (thin near horizon) or flat-clamp the top 1–2
   bands; a **floor sub-cap** so strips don't starve wall textured upgrades. HW A/B the
   recovered ms.

6. **Gate + decide.** Wire to a potato/pad toggle (pot0 = software, byte-identical; new
   mode = VDP1 strip floor). Ship dominant-flat-only **or** stop. **Do NOT generalize** to
   all visplanes (busts the budget; violates perf-economy).

---

## 5. Risks & limits

- **Swim** (the quality cap) — worse than walls (bigger 1/z range). Mitigate: non-uniform
  bands / horizon flat-clamp. RBG0 would have none; this is the trade for shipping on HW.
- **Command-count explosion** (fragmentation + u-tiling) — the GO/NO-GO gate (inc-0).
- **Multi-height is NOT solved** — only the **one** dominant flat moves to VDP1; all other
  heights/ceilings stay software (same coverage as the RBG0 plan). *Unlike* RBG0, it can be
  extended to a 2nd/3rd flat later by emitting more quads — but only if the budget allows.
- **Dominant-flat flicker** — use the **stable view-sector** pick (`sat_vdp2_floor_*`), not
  a per-frame histogram, to avoid the surface popping between two near-equal flats.

## 6. Multiplayer note

Split-screen *helps*: each viewport is smaller → the dominant flat per view is smaller →
**fewer strips, lower fragmentation** → cheaper per view. Command banks scale ×N (cheap
VRAM). Consistent with [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) §7 — the N-player
ceiling stays **REC×N on the CPU**, which the floor offload actually *relieves*.

## 7. Sources

7-agent workflow (2026-06-23): current `src/dg_saturn.cxx` + `core/r_plane.c` /
`r_segs.c` audit; `saturn-refs/SlaveDriver-Engine` `WALLS.C` (design-time tile grids,
`MAXNMSLAVEPOLYS=1300`); `RBG0_FLOOR_PLAN.md` (dom% 49–93 %); Copetti (VDP1 affine / VDP2
perspective floors); SegaXtreme/Beyond3D (~1 300–2 000 quads/30 fps Saturn ceiling, CPU is
the limiter); Heckbert/Stanford piecewise-linear texture-map error theory. See
[[doomsrl-vdp1-capacity]].
