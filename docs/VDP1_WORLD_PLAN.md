# VDP1 world floors/ceilings — the per-subsector inverted hybrid (bet A)

Written 2026-06-25 after a multi-agent investigation (per-subsector classifier, the
emitter, occlusion/painter order, the command/VRAM budget + RBG0 integration, the
measurement protocol, and SlaveDriver/PowerSlave ground-truth), then revised the same day
against a critic pass that flagged the command-bank story, the VRAM arithmetic, the wall
"verbatim reuse" overclaim, the swim-band re-derivation, the painter-order granularity, the
missing A/B toggle, the ladder ordering, and the colormap-availability proof. Companion to
[`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) (the chip/cost reference),
[`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md) (the earlier per-visplane dominant-flat strip
plan this **subsumes**), [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md) and
[`VDP2_FLOOR_CONSOLIDATION.md`](VDP2_FLOOR_CONSOLIDATION.md) (the RBG0 commit status).

This plan is the PowerSlave/SlaveDriver model adapted to Doom's BSP: **the whole world's
floors and ceilings become per-subsector VDP1 distorted-sprite quads, emitted during the
BSP walk like the walls, with only the non-quad-able residue left to the CPU and the
dominant flat to RBG0.** It supersedes the per-visplane affine-strip plan
([`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md)): the floor is known at `R_Subsector`, not
deferred to `R_DrawPlanes`, so it rides the **existing wall kick** with no visplane
deferral — the structural advantage that makes the bet cheap.

---

## 0. TL;DR — the bet in five lines, and the data that justifies it

1. **Deport floors+ceilings to VDP1 as per-subsector quads, emitted in the BSP walk** (the
   inverted hybrid): the floor is final at `R_Subsector`, like the walls, so it rides the
   already-shipping wall kick — no new `slSlaveFunc`, no visplane deferral.
2. **Doom geometry is quad-clean (measured, HW, DOOM2).** Row 20 `PAR`: `Qp ≈ 169`
   geometry quads for the *whole-world* floors+ceilings; `q4 ≈ 80–93 %` of surfaces come
   from `≤4`-side (pure-quad) subsectors. Row 17 `FLR`: `Vp ≈ 158` textured-secondary
   quads at `FLOOR_HBAND=16` (dominant excluded → RBG0). Walls `VD1 ≈ 20–227`.
3. **The CPU is the bottleneck, VDP1 is the idle resource.** REC `≈ 40–60 ms` (up to
   105 ms), all master, memory-bound; `P` (visplane/span **generation**) is 12–55 ms at
   pot0. VDP1 finishes ~92–94 % of frames (`Dr%`). Deporting kills the **generation (P)**,
   the **fill (EX)**, *and* the **blit** of those pixels — not just the fill.
4. **It fits the budget — but ONLY after a mandatory VRAM cull (§7.2).** The floor needs a
   **new dedicated command bank F** (a *separate* double-buffered pair from the wall bank,
   not a reuse of the wall buffers — §7.1) plus a 32 KB flat cache. Today VDP1 VRAM is
   512 KB-tight with zero headroom; the cull of the dead weapon/HUD VDP1 region frees the
   space. The closed address ledger is in §7.1–7.2.
5. **The risk is swim + REC, not fill.** Affine swim near the horizon (HW-judged) and the
   chance the per-band CPU build *raises* REC instead of lowering `P` are the two live
   unknowns; both are hardware-only verdicts. The whole ladder is gated by overlay
   counters with explicit GO/NO-GO and a one-button A/B toggle (built in **inc-A0**, §9).

> **What is real today.** The inc-0 profilers (`RP_Subsector` row 20, `RP_PlanePixels`
> row 17, `RP_PROF=1`) are BUILT and live. The inc-1 floor-skip hook
> (`sat_vdp1_floor`/`sat_floor_vdp1_hook`, [r_plane.c:892](../core/r_plane.c)) exists with
> an own-everything stub (`sat_floor_vdp1_stub` returns 1, emits zero strips). **No floor
> quad is emitted yet, no second command bank exists yet, the A/B toggle does not exist yet
> (the pad is saturated, §8.2).** This plan is the build-out from there.

---

## 1. Target architecture — the inverted hybrid

Today: VDP1 = **walls only**, drawn *below* a software NBG1 that does floors/ceilings/
sprites/HUD with Doom's occlusion (the layer inversion). Floors/ceilings are software
visplanes + spans on master+slave (the P3 path). RBG0 is parked (commit gap on HW).

Target: VDP1 carries **walls + floors + ceilings** as per-subsector quads; RBG0 carries
the single dominant flat; the CPU keeps only the non-quad-able residue. The layer/priority
stack (verified [dg_saturn.cxx:1448-1457](../src/dg_saturn.cxx); RBG0-on path
[:1239/:1453](../src/dg_saturn.cxx)):

| Priority | Layer | Role (target) | vs today |
|---|---|---|---|
| 7 | NBG3 | debug overlay text | unchanged |
| 6 | NBG1 (software) | occlusion master + sprites + HUD + **CPU residue** floors/ceilings | was: + all floors/ceilings |
| 5 | VDP1 sprites | **walls + floor quads + ceiling quads** | was: walls only |
| 4 | RBG0 (Mode-7) | the **dominant flat** (perspective-exact), gated on HW commit | was: parked |
| 3 | NBG0 | sky (drops 4→3 when RBG0 is on; software sky if RBG0 needs the bank) | was: sky at 4 |

So `sky(3) < RBG0(4) < VDP1(5) < NBG1(6) < debug(7)`. Two ordering problems live in this
stack and are answered by **two different mechanisms** (§4): VDP1-internal overlap by
command-list position (painter's, reverse-BSP), and VDP1-vs-NBG1 overlap by hardware
priority mediated by the **index-0 hole** contract. The whole feature is NULL-default
hooked so DoomJo links unchanged (§11) and the shipping VDP1 walls are untouched.

**Why it beats the per-visplane plan.** The earlier strip plan
([`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md)) emits at `R_DrawPlanes` (end of frame) because
a *visplane's* silhouette is only complete after the full BSP walk + seg clipping. The
per-subsector emitter takes the floor at `R_Subsector` ([r_bsp.c:556](../core/r_bsp.c)),
the instant its sector data (height/picnum/light) and seg fan are final — identical to how
walls leave `R_AddLine`. That removes the deferral, the second kick, and (when the visplane
is never built at all, §3) the *generation* cost `P`, not just the fill.

---

## 2. Static classification of subsectors

A **static load-time byte per subsector** lets the emitter decide, with zero per-frame
branching, *how* each floor/ceiling is drawn, plus a **per-frame dominant bit** that cannot
be precomputed. The piece formula is byte-identical to the live `RP_Subsector`
([r_parallel.c:1147-1158](../core/r_parallel.c)), so the classifier and the row-17/row-20
profiler **agree by construction** — the overlay *is* the validation oracle.

### 2.1 The classes

- **SIMPLE** — `numlines ≤ 4`: the convex leaf projects to one `FUNC_DISTORSP` quad per
  surface. The `q4 ≈ 80–93 %` common case.
- **COMPLEX** — `numlines > 4`: fanned into `(numlines-1)/2` quad pieces. Taken on a small
  map; left to CPU residue on a large map.
- **DOMINANT-FLAT candidate** — the floor matches the stable view-sector dominant flat
  (`sat_vdp2_floor_h/_pic/_band`). The emitter **skips** it (RBG0 owns it; emitting VDP1
  quads there would clobber the RBG0 layer below). This is a *runtime* `==` test, never a
  static bit.

Plus a **per-level world-size verdict** `sat_ss_world_small`: small map ⇒ deport COMPLEX
too (VDP1-everything); large map ⇒ VDP1 takes SIMPLE, CPU keeps COMPLEX.

### 2.2 Storage and memory cost

One `unsigned char` array sized `numsubsectors`, `PU_LEVEL` (freed with the map), mirroring
the shipping `sat_seg_cpu[SAT_SEG_MAX=4096]` ([r_segs.c:265](../core/r_segs.c)):

```c
/* core/r_bsp.c (def) + r_state.h (extern) — SATURN, DoomJo-safe.
   NULL until the platform arms sat_ss_classify (default 0 => never built on DoomJo). */
extern unsigned char *sat_ss_class;   /* [numsubsectors], PU_LEVEL, or NULL */

#define SSC_SIMPLE      0x01u   /* numlines<=4 -> single VDP1 quad/surface          */
#define SSC_FLOOR_OK    0x02u   /* this subsector owns a floor surface (geometry)    */
#define SSC_CEIL_OK     0x04u   /* this subsector owns a NON-SKY ceiling (geometry)  */
#define SSC_PIECES_MASK 0xF8u   /* (numlines<=4)?1:((numlines-1)/2), clamped 0..31   */
#define SSC_PIECES_SHIFT 3
```

`SSC_FLOOR_OK`/`SSC_CEIL_OK` are *geometry capability* (could this surface ever be a quad —
a sky-only ceiling is permanently `SSC_CEIL_OK=0`), **not** per-frame visibility. Visibility
(`floorheight < viewz` etc.) stays the runtime `floorplane`/`ceilingplane` test in
`R_Subsector`, exactly as today.

| Item | Cost | Notes |
|---|---|---|
| `sat_ss_class[numsubsectors]` | **~0.5–4 KB/map** | 1 byte/ss; DOOM2 = a few hundred to low-thousands of subsectors |
| `sat_ss_world_small` | 1 int | the per-level small/large verdict |
| dominant bit | 0 (not stored) | computed inline at `R_Subsector`, never written back |

Negligible against the 884 KB streaming zone; the array stays read-only after load
(cache-friendly, no per-frame writes).

### 2.3 When + how it is computed

Once, in `P_SetupLevel` right after `P_GroupLines()` ([p_setup.c:850](../core/p_setup.c)),
where `subsectors[]`, `segs[]` and the `sub->sector` back-pointers all exist. A single
linear `R_ClassifySubsectors()` pass in `r_bsp.c` (pure C, beside `R_Subsector`):

```c
void R_ClassifySubsectors(void)            /* pure C, no C++isms, DoomJo never calls it */
{
    int i; unsigned int qsum = 0;
    sat_ss_class = (unsigned char*)Z_Malloc(numsubsectors, PU_LEVEL, 0);
    for (i = 0; i < numsubsectors; i++) {
        subsector_t *ss = &subsectors[i];
        sector_t    *sec = ss->sector;
        int nl = ss->numlines;
        int pieces = (nl <= 4) ? 1 : ((nl - 1) / 2);   /* == RP_Subsector's formula */
        unsigned char t = SSC_FLOOR_OK;
        if (nl <= 4) t |= SSC_SIMPLE;
        if (sec->ceilingpic != skyflatnum) t |= SSC_CEIL_OK;
        if (pieces > 31) pieces = 31;
        t |= (unsigned char)(pieces << SSC_PIECES_SHIFT);
        sat_ss_class[i] = t;
        { int nsurf = 1 + ((sec->ceilingpic != skyflatnum) ? 1 : 0);
          qsum += (unsigned int)(nsurf * pieces); }     /* the static Qp analogue */
    }
    sat_ss_world_small = (qsum <= SSC_WORLD_QMAX);       /* tunable compile constant */
}
```

`SSC_WORLD_QMAX` is the static whole-level worst case (every subsector deported,
untextured). It is the static analogue of the live row-20 `Qp` and is pinned to the
measured `Qp ≈ 169` / two-bank `Vp ≤ 120` budget — a **bench-tunable compile constant**, not
a magic literal. The static sum is an upper bound: a map whose *visible* subset is small but
whose whole-level total is large is conservatively forced to hybrid (safe for bank budget,
may leave VDP1 headroom unused — accepted).

The dominant-flat refinement (the only per-frame work) is one integer compare already paid
by the existing RBG0 skip — see §3.

---

## 3. The per-subsector floor/ceiling emitter

The core of the renderer: at `R_Subsector` (during the BSP walk, beside the `RP_Subsector`
profiler at [r_bsp.c:556](../core/r_bsp.c)), project each visible subsector's floor and
non-sky ceiling to screen space and emit them as `FUNC_DISTORSP` quads. **The wall pipeline
is reused — but it is NOT reused verbatim: the shipping wall emitter hardcodes the wall bank
and the wall cap (§3.5), so the parameterization to retarget it to bank F is a real,
specified code change, not a no-op.**

### 3.1 The new hook (core, pure C, DoomJo-safe)

The existing `sat_floor_vdp1_hook` ([r_plane.c:893](../core/r_plane.c)) fires **per-visplane,
end-of-frame** — the wrong kick point. The per-subsector emitter needs a **second, distinct
hook** fired from `R_Subsector`, following the `sat_wall_hook` template
([r_segs.c:219](../core/r_segs.c)):

```c
/* core/r_bsp.c — new globals; NULL/0 default => DoomJo + normal build link unchanged. */
int sat_ss_floor = 0;                      /* runtime gate, pad-toggled on the platform */
int (*sat_ss_floor_hook)(
        const subsector_t *sub,            /* fan: &segs[sub->firstline], sub->numlines */
        int   is_ceiling,                  /* 0=floor 1=ceiling (called once per surface) */
        fixed_t height,                    /* frontsector->floor/ceilingheight (16.16)   */
        int   picnum,                      /* floor/ceilingpic (flat lump index)         */
        int   lightlevel,                  /* frontsector->lightlevel                    */
        const lighttable_t *cmap           /* resolved colormap (light + extralight)     */
    ) = NULL;                              /* returns 1 = platform claimed it             */
```

#### 3.1.1 Resolving `cmap` in scope at `R_Subsector` (the colormap-availability proof)

The critic correctly flagged that the old draft *referenced* `ss_cmap_floor`/`ss_cmap_ceil`
without showing they are computable at BSP-walk time. The software floor path computes its
colormap in `R_MapPlane`/`R_DrawPlanes` ([r_plane.c:~1128](../core/r_plane.c)), *not* at
`R_Subsector`. But the value it computes is a pure function of data already in scope at
`R_Subsector`, so we resolve it there directly, with the identical arithmetic:

```c
/* at R_Subsector, frontsector is in scope (set by R_Subsector itself). */
static const lighttable_t *ss_floor_cmap(sector_t *sec)
{
    int li = (sec->lightlevel >> LIGHTSEGSHIFT) + extralight;   /* == R_MapPlane's index */
    if (li < 0)              li = 0;
    if (li >= LIGHTLEVELS-1) li = LIGHTLEVELS-1;
    /* software floors index zlight[]/scalelight[] by DISTANCE per scanline; but the FLAT
       colormap pick for a VDP1 quad is the per-SECTOR base light, NOT per-distance (VDP1
       gets ONE CRAM bank per surface, §3.5).  The base index here is byte-identical to the
       light the dominant-flat path already feeds RBG0 (sat_vdp2_floor_cmap), so the VDP1
       floor's CRAM bank matches the software/RBG0 layers by construction. */
    return colormaps + li * 256;          /* the unshaded base; wall_light_colr maps it -> CRAM bank */
}
```

The key facts that make this *available* and *bit-matching*:

- `frontsector->lightlevel` and `extralight` are both in scope at `R_Subsector`
  (`extralight` is a renderer global, set once per frame in `R_SetupFrame`).
- `LIGHTSEGSHIFT`/`LIGHTLEVELS` are the same constants the software floor uses, so
  `(lightlevel>>LIGHTSEGSHIFT)+extralight` produces the *same light band index* the
  software path lands on for that sector.
- The VDP1 path does **not** need the per-distance `zlight` table the software span uses,
  because a VDP1 quad carries exactly **one** CRAM light-bank (CMDCOLR), chosen by
  `wall_light_colr` from this base index (§3.5). The dominant-flat RBG0 path already makes
  the same per-sector (not per-distance) light choice (`sat_vdp2_floor_cmap`), so the three
  layers (software residue, VDP1 floor, RBG0 dominant) agree on the band by construction.
- The match is the **byte-identity GO of inc-A2/inc-C**: floor-OFF must stay
  byte-identical, and floor-ON must put the VDP1 quad in the CRAM bank the software floor
  would have used for that sector's base light. Verified on HW by the overlay, not asserted.

Wire it in `R_Subsector`, reusing the `floorplane`/`ceilingplane` visibility already
computed (NULL = not visible, so the gates match Doom exactly):

```c
if (sat_ss_floor && sat_ss_floor_hook) {
    if (floorplane && sat_ss_floor_hook(sub, 0, frontsector->floorheight,
            frontsector->floorpic, frontsector->lightlevel, ss_floor_cmap(frontsector)))
        floorplane = NULL;                 /* CLAIMED -> suppress the visplane entirely */
    if (ceilingplane && frontsector->ceilingpic != skyflatnum
        && sat_ss_floor_hook(sub, 1, frontsector->ceilingheight,
            frontsector->ceilingpic, frontsector->lightlevel, ss_ceil_cmap(frontsector)))
        ceilingplane = NULL;
}
```

**The key difference from inc-1 — and the real REC win.** The inc-1
`sat_floor_vdp1_hook` leaves a visplane's `top[]/bottom[]` at index-0 *after* the full
silhouette is known. Here we instead null `floorplane`/`ceilingplane` *before* `R_AddLine`
runs, so the subsector's segs never `R_CheckPlane`-merge into a visplane. There is **no
visplane to skip, no `top[]/bottom[]` to write, no `W_CacheLumpNum`/span generation**.
That eliminates `P` (generation), not merely the `EX` fill — the analogue of the measured
wall offload (`Bp 16→3 ms`, [VDP1_ARCHITECTURE.md:198](VDP1_ARCHITECTURE.md)).

> **Wiring caveat (occlusion/index-0).** Nulling the visplane at `R_Subsector` (before walls
> clip it) is correct **for suppressing generation**, but the *index-0 hole* it would
> imply is computed against an un-clipped polygon. §4 resolves this: the quad moves to
> BSP-walk time, but the index-0 hole stays at visplane time (Wiring A). Where the static
> tag routes a surface to the CPU residue (or budget demotes it), the hook returns 0 and the
> visplane is built as normal.

### 3.2 Projection — subsector polygon to screen quad(s)

Inputs in scope at the hook: the seg fan `line = &segs[sub->firstline]`, `count =
sub->numlines`, the flat `height` (16.16). Reuse Doom's projection constants verbatim
([r_main.c:704-708](../core/r_main.c)): `centerx`, `centery`, `centeryfrac`,
`projection = centerxfrac`, plus `viewx/viewy/viewz/viewsin/viewcos/viewangle` and the
`viewangletox[]` table. **No new trig** — `FixedMul`/`FixedDiv` (SH-2 `dmuls.l`/`div1`,
[m_fixed.h:37](../core/m_fixed.h)) and `finesine`/`finecosine` only.

Per-vertex transform (identical in form to `sat_wall_hook`,
[r_segs.c:405-413](../core/r_segs.c)):

```c
/* world vertex (vx,vy) at flat world-Z = height: */
ang   = R_PointToAngle(vx, vy) - viewangle;
sx    = viewangletox[(ang + ANG90) >> ANGLETOFINESHIFT];      /* screen X, 0..viewwidth */
tx    = vx - viewx;  ty = vy - viewy;
z     = FixedMul(tx, viewcos) - FixedMul(ty, viewsin);        /* forward depth (gxt-gyt) */
if (z < MINZ) z = MINZ;                                       /* near-clamp             */
scale = FixedDiv(projection, z);
sy    = centery - (FixedMul(height - viewz, scale) >> FRACBITS);
```

Apply `+viewwindowx` and `<<detailshift` to `sx` exactly as the wall hook does so the
coords land in the same framebuffer geometry.

**Closing the open fan.** Doom subsector seg lists are **open**: BSP partition-line edges
carry no seg, so `&segs[firstline..]` is an open chain, not a closed ring. But the leaf is
**convex** (guaranteed, [r_defs.h:217](../core/r_defs.h)). Collect the ordered vertex chain
`v[0]=seg0->v1, v[1]=seg0->v2(=seg1->v1), … v[n]=segLast->v2` and **fan from `v[0]`**:
triangles `(v0, vi, vi+1)`. The missing implicit edge is exactly the fan's closing chord;
for a convex leaf the fan over the seg endpoints covers the visible interior — no need to
reconstruct the partition edge (vanilla nodes carry no GL ring).

Quad-fan emission, tied to the row-17/row-20 counters:
- **`numlines ≤ 4` (`q4`, ~80–93 %):** one DISTORSP with the 4 projected corners A,B,C,D
  clockwise (degenerate 3-vertex fans repeat D=C). The dominant, cheapest path.
- **`numlines > 4`:** `(numlines-1)/2` quads, each `(v0, v[2k+1], v[2k+2], v[2k+3])` → one
  DISTORSP. `Qp ≈ 169` is the world peak of these pieces — the bank-budget input.

(u,v) per corner = world XY relative to the flat origin, masked into 64-texel flat space
(`u = (vx - origin_x) & (64*FRACUNIT-1)`), consistent with Doom's flat addressing. Flats are
axis-aligned, so SlaveDriver's `pattern[]` winding-rotation trick is unnecessary.

### 3.3 Affine / swim — the two distinct banding mechanisms (do NOT conflate)

DISTORSP is affine, not perspective-correct: it interpolates (u,v) linearly in screen
space, exact only at constant z. A floor spans feet→horizon, so the 1/z curvature is huge
and one quad swims badly — error ∝ `(1/z curvature) × band_height²`, concentrated at the
horizon. There are **two separate banding mechanisms** in this renderer, and the critic
correctly noted they are NOT the same code:

| Mechanism | Axis | Source | Reused from walls? |
|---|---|---|---|
| **(H) horizontal u-tiling** | screen-X | a 64-texel flat tiles across the floor's width | YES — the `wall_emit_band` u-loop (§3.4) maps onto this directly |
| **(V) vertical screen-Y swim banding** | screen-Y | the floor's 1/z curvature from feet to horizon | **NO — this is NEW code.** Walls have ~constant z per column; they have no screen-Y swim analogue |

Mechanism (H) is §3.4. Mechanism (V) is below, and it is the genuinely new emitter logic.

#### 3.3.1 (V) The screen-Y swim band — the new re-derivation

This is the same model `RP_PlanePixels` profiles at `FLOOR_HBAND=16` / `FLOOR_MAXTILES=16`
([r_parallel.c:997-998](../core/r_parallel.c)); the `Vp ≈ 158` row-17 number is measured
there. **There is no wall analogue** — a wall column has one near-constant z, so a wall needs
no per-row z re-derivation; a floor's z sweeps from `MINZ` at the feet to ∞ at the horizon,
so each screen-Y band needs its own world footprint re-projected. Concrete spec:

For a subsector floor at world height `height`, with `planeheight = abs(height - viewz)`,
split the subsector's screen-Y extent `[ytop, ybot]` into bands of `FLOOR_HBAND` rows. For
each band `[ym0, ym1)`:

1. **Invert screen-Y → world distance**, per Doom's own `yslope[]`
   ([r_plane.c:319](../core/r_plane.c), the table `R_DrawPlanes` uses to map a row to a
   span distance):

   ```c
   /* distance from the eye to where this screen row's ray meets the flat plane: */
   fixed_t dist0 = FixedMul(planeheight, yslope[ym0]);   /* world distance at band top    */
   fixed_t dist1 = FixedMul(planeheight, yslope[ym1]);   /* world distance at band bottom */
   ```

   `yslope[ym]` is `FixedDiv(planerelativeheight_unit, abs(ym-centery))` precomputed once
   in `R_InitTables` — exactly the inversion the software floor already trusts, so the band
   edges land on the same world distances the software span path would walk.

2. **Map each band edge to world XY at the subsector's LEFT and RIGHT screen-X.** For each
   band edge distance `dist`, and for the two view-ray angles at the subsector's left
   (`xl`) and right (`xr`) screen-X columns:

   ```c
   /* the view ray through screen column x has angle xtoviewangle[x]+viewangle: */
   angle_t a_l = (viewangle + xtoviewangle[xl]) >> ANGLETOFINESHIFT;
   angle_t a_r = (viewangle + xtoviewangle[xr]) >> ANGLETOFINESHIFT;
   /* world XY where that ray meets the plane at distance dist: */
   fixed_t wx_l = viewx + FixedMul(dist, finecosine[a_l]);
   fixed_t wy_l = viewy + FixedMul(dist, finesine  [a_l]);
   /* ...and (wx_r, wy_r) for a_r. */
   ```

   This yields the band's true **world footprint quad** (4 world XY corners: left/right ×
   top/bottom). The band's (u,v) come from these world XY (masked into 64-texel space, §3.4)
   — **world-anchored, never screen-anchored** (§3.3.2).

3. **Re-project that world footprint to one DISTORSP per band** via §3.2's per-vertex
   transform. Within the band z is ~constant so the affine map is near-exact (Mode-7 /
   piecewise-linear-1/z). One band = one (or, after u-tiling, a few) DISTORSP commands.

> **Anti-swim law (HW-tested, do NOT violate).** Each band's texel coords are
> **WORLD-anchored, never SCREEN-anchored.** The reverted `SAT_YCLAMP` proved that clamping
> y to a fixed screen edge and remapping v makes texels slide every frame
> ([VDP1_ARCHITECTURE.md:137](VDP1_ARCHITECTURE.md)). Step 2 above pins each band's (u,v) to
> its true world footprint via `yslope[]` inversion + ray-march, *then* re-projects — so as
> the player moves, the same world texel stays under the same world point. This is the same
> discipline `wall_emit_band` uses for u, applied to the new screen-Y axis.

Mitigations for the worst (near-horizon) bands, riskiest-first:
- **Non-uniform bands:** thin near the horizon (peak `yslope` curvature), fat near the feet
  (large z, affine already accurate). E.g. `HBAND ∈ {4,8,16,32}` from horizon downward —
  concentrates command budget where swim hurts.
- **Flat-clamp the top 1–2 bands:** the bands touching the horizon (where `yslope[ym]`
  explodes as `ym→centery`) get a single `FUNC_Polygon` flat-color fill (dominant flat
  index, CRAM-lit) instead of a textured strip — kills the worst swim, costs 1 cmd. Same
  philosophy as `wall_emit_flat` ([dg_saturn.cxx:2209](../src/dg_saturn.cxx)).
- **Near-clip / distance LOD (SlaveDriver `TILENEARCLIP=F(33)`, `MIPDIST=F(256)`):** clamp
  `dist` UP before re-projecting to bound projected coords; if the whole subsector is beyond
  a distance threshold, halve the band subdivision (¼ the quads) — a binary per-surface LOD.
- **Flat-color fallback for over-budget/grazing subsectors:** never a hole — the coverage
  guarantee, exactly like the wall path.

A guard against the near-floor blow-up: as `ym→centery` (`yslope[ym]→∞`), `dist→∞` and the
world footprint runs off to infinity. Clamp `ym` to never reach `centery±SAT_YCLAMP_GUARD`
(a small constant); the bands beyond it are the flat-clamped horizon bands above. This is
the den→0 guard the wall projection already carries, on the screen-Y axis.

Perspective tile-placement was tried for walls and **reverted** (vanishing-point blow-up);
do not repeat it for floors.

### 3.4 (H) 64-texel wrap / horizontal tiling

Flats are 64×64; floors tile horizontally and DISTORSP does **not** wrap a 64×64 char.

| Option | VRAM cost | Verdict |
|---|---|---|
| (A) one DISTORSP per 64-texel u-tile (retarget `wall_emit_band`'s u-loop, §3.5) | 0 extra (one 64×64 flat in cache) | **RECOMMEND** |
| (B) pre-tile a larger texture (e.g. 256×256 = 4×4 tiled) | 64 KB/flat (8bpp) — busts the freed tail with >1 flat | NO |
| (C) rely on VDP1 UV-wrap | — DISTORSP has no UV-wrap mode | impossible |

Recommend **(A):** cache each flat once as a 64×64 8bpp char (4 KB, COLOR_4) and split each
band's u-span into 64-texel tiles, one DISTORSP per tile with corners extrapolated to the
tile's true screen extent — the identical machinery in `wall_emit_band` (the
`for (ub…; ub += texw)` loop at [dg_saturn.cxx:2071](../src/dg_saturn.cxx), here `texw=64`,
clamp `FLOOR_MAXTILES=16`). This is exactly what `RP_PlanePixels` already counts
(`tiles = u-span/64 + 1`). It is the per-band u-loop nested inside each screen-Y band of
§3.3.1 — so a textured subsector floor = `(#screen-Y bands) × (#u-tiles per band)` DISTORSPs.

### 3.5 Reuse of the existing wall pipeline — and the parameterization it requires

Most of the wall machinery ships and is reused. **But it is NOT a verbatim drop-in:**
`wall_emit_band` hardcodes the wall bank `VDP1_BANK[vdp1_wbank]` (verified
[dg_saturn.cxx:2055, :2098, :2112](../src/dg_saturn.cxx)) and the wall cap `WALL_CMD_CAP`
(verified [dg_saturn.cxx:2046, :2073, :2100](../src/dg_saturn.cxx)). To draw into bank F
under `FLOOR_CMD_CAP`, these must be **parameterized**. Two equivalent options:

- **(P1) Parameterize the existing emitter.** Add `(unsigned int bank_base, int *cursor,
  int cmd_cap)` parameters to `wall_emit_band` (and the `vdp1_cmd_at` calls inside it),
  threading the wall path's existing `VDP1_BANK[vdp1_wbank]/&vdp1_wnext/WALL_CMD_CAP` so the
  wall behavior is byte-identical, and passing `F_BANK[f_wbank]/&f_wnext/FLOOR_CMD_CAP` for
  floors. The risk: touching the shipping wall emitter — gated by the inc-A1 byte-identity GO.
- **(P2) A floor-specific emitter `floor_emit_band`** that copies the `wall_emit_band`
  band/u-tile structure but writes `F_BANK[f_wbank]` / `FLOOR_CMD_CAP` and adds the §3.3.1
  screen-Y band loop on top. No risk to the wall path; some duplicated u-loop code.

**Recommend (P2)** for the screen-Y band logic (which has no wall analogue anyway, §3.3) and
keep the wall emitter untouched, with (P1)'s parameterization applied only to the genuinely
shared inner u-tile helper if it can be factored cleanly. Either way, **"verbatim reuse" is
retired**: the bank and cap are explicit parameters of the floor path.

The exact pieces reused:

- **Command writer:** `vdp1_cmd_at(base, idx, cmd)`
  ([dg_saturn.cxx:1654](../src/dg_saturn.cxx)) — the 32-byte writer is already
  `base`-parameterized; pass `F_BANK[f_wbank]`. The shared *cursor* is NOT reused: floors get
  their own `f_wnext`.
- **DISTORSP template:** the textured-quad command from `wall_emit_band`
  ([dg_saturn.cxx:2104-2112](../src/dg_saturn.cxx)): `cmd[0]=0x0002`, `cmd[2]=0x04E0`
  (Window_In|COLOR_4 8bpp|SPD|ECD-off) or `0x00E0` (no window), `cmd[3]=colr`,
  `cmd[4]=charAddr=(addr-VDP1_VRAM_BASE)>>3`, `cmd[5]=charSize=((padW>>3)<<8)|rows`, A/B/C/D
  corners. Add `HSS_ENABLE` for minified far-floor tiles (SlaveDriver always sets it).
- **Flat fallback:** `wall_emit_flat`'s `FUNC_Polygon` (`cmd[0]=0x0004`, `cmd[2]=0x00C0`,
  `cmd[3]=bank<<8|dominant_idx`) for horizon-clamp / over-budget — same parameterization
  (bank+cap) as the textured emitter.
- **Lighting:** `wall_light_colr(cmap)` ([dg_saturn.cxx:1746](../src/dg_saturn.cxx))
  verbatim — pass the floor's resolved `cmap` (§3.1.1); CMDCOLR = `wlight_bank_lut[L]<<8`
  selects one of the 7 pre-shaded CRAM banks (bank 1 = live PLAYPAL ⇒ VDP1 floor color
  bit-matches the software NBG1 layers). Texel = RAW index, never re-baked. Gouraud stays OFF
  (additive, can't light an 8bpp bank). Flash via `wtex_rebuild_banks` (CRAM-only). This
  helper is genuinely bank-agnostic, so it is reused as-is.
- **Texture cache:** a **dedicated flat cache** modeled on `wall_tex_resolve`/`wtex_cache`
  ([dg_saturn.cxx:1907](../src/dg_saturn.cxx)) — see §7.2 for why it is separate, not the
  shared 22-slot wall pool. A flat = 64×64×1B = 4 KB; same 8bpp COLOR_4 16-bit-packed
  upload (charAddr `(addr-base)>>3`, charSize `((64>>3)<<8)|64`).
- **The kick:** the floor quads share the **existing wall kick** (`sat_walls_done_hook` →
  `sat_walls_kick` → `sat_vdp1_wpn_begin(); vdp1_wpn_kick();`,
  [dg_saturn.cxx:2565](../src/dg_saturn.cxx)). Because the floor is emitted *during* the BSP
  walk it is already in bank F when the kick fires — no second PTMR, no second
  `slSlaveFunc`, no new freeze-zone exposure. Bank F chains off the same root LINK and rides
  the same kick (§7.1).

### 3.6 When to emit — during the BSP walk

The hook fires at [r_bsp.c:556](../core/r_bsp.c), in the front-to-back BSP traversal. The
floor's sector data and polygon are final the instant `R_Subsector` runs — identical to
walls emitted from `R_AddLine`. No visplane deferral (visplanes complete only at
`R_DrawPlanes`). The quads accumulate into the VDP1 bank F and ride the wall kick that
presents the same frame. Occlusion is free (BSP order + the layer inversion) — see §4.

> **Inherited tearing.** The floor shares the early wall kick, so it inherits the
> `VDP1_MANUAL_CHANGE=0` tearing ([dg_saturn.cxx:135](../src/dg_saturn.cxx)): the early kick
> restarts the draw before EDSR CEF. Proper anti-tear is an open issue affecting both walls
> and floors; do **not** add a mirror buffer / 1-frame delay (HW-tested "largement pire").

---

## 4. Occlusion & painter order

Walls, floors and ceilings coexist in **one VDP1 command list** with no Z-buffer, sharing
the **index-0 contract** with NBG1. Grounded in the shipping wall path
([dg_saturn.cxx:2270-2327](../src/dg_saturn.cxx)), the index-0 skip
([r_plane.c:1152-1178](../core/r_plane.c)), and the per-subsector anchor
([r_bsp.c:556](../core/r_bsp.c)).

### 4.1 Two ordering problems, two mechanisms (do not conflate)

1. **VDP1-internal overlap** (quad vs quad, no Z-buffer) → resolved by **command-list
   position** (painter's, later command overpaints). BSP order matters here.
2. **VDP1-layer vs NBG1-layer overlap** (a VDP1 floor vs a software sprite/HUD/CPU-residue
   floor) → resolved by **hardware priority** (NBG1=6 always beats VDP1=5) **mediated by the
   index-0 hole**. BSP order is irrelevant here.

The simplifying invariant (inherited from the shipping walls, confirmed by SlaveDriver's
per-sector model): **within a convex subsector, its floor, ceiling and walls occupy
mutually-disjoint screen pixels** (floor below the horizon span, ceiling above, walls in the
vertical band between). So *intra-subsector* painter order is a non-issue; only
*inter-subsector* order matters.

### 4.2 Reverse-BSP, per-subsector-contiguous emission (a NEW grouping)

Doom visits subsectors **front-to-back** (its O(1) solidseg occlusion); VDP1 painter needs
**back-to-front**. The shipping walls already reverse — **but the critic is right that the
shipping reverse is at *accumulator-entry* granularity, not subsector granularity.** The
wall flush walks `wall_acc[]` in reverse ([dg_saturn.cxx:2319](../src/dg_saturn.cxx), "paint
far→near"), where each entry is one accumulated wall, in BSP-visit order. There is **no
subsector grouping today**. The unified emitter introduces one — this is **new code**, and
its principal regression risk (inc-A1's whole risk) is that re-ordering the wall accumulator
must not perturb the existing far→near wall paint.

**What is new:** a `subsector_seq` ordinal assigned to every accumulated surface at
`R_Subsector` (a monotonic per-frame counter, bumped once per subsector), plus a `kind` tag
(WALL/FLOOR/CEIL) on each accumulator entry. The flush then walks **subsectors in reverse
ordinal**, and within each subsector emits its `{ceiling, floor, walls}` **contiguously**.

**How the wall accumulator and `floor_acc` co-order without regressing the wall reverse
loop.** The two accumulators share the **same `subsector_seq` ordinal** (assigned at
`R_Subsector`, the single point both the wall hook and the floor hook see). At flush:

```c
/* both accumulators are already in BSP-visit (front-to-back) order, because both were
   appended during the same front-to-back walk.  Walk the subsector ordinals in REVERSE; for
   each, emit its surfaces.  This is a STRICT GENERALISATION of the shipping wall reverse:
   if there were one surface per subsector and no floors, it reduces to the existing
   `for (i = wall_acc_n-1; i>=0; --i)` loop byte-for-byte. */
for (int s = max_subsector_seq; s >= 0; --s) {
    emit_ceilings_of(s);     /* from floor_acc, kind==CEIL */
    emit_floors_of(s);       /* from floor_acc, kind==FLOOR */
    emit_walls_of(s);        /* from wall_acc, walls LAST so a 1px seam lands on top of the flat */
}
```

The inc-A1 GO is exactly **byte-identical wall render with floors OFF**: because the wall
entries within a subsector retain their relative order and the subsectors are still visited
in the same reverse order, the wall paint sequence is unchanged. The only way it regresses
is if `subsector_seq` is mis-assigned (e.g. bumped per-surface instead of per-subsector) —
caught by the inc-A1 byte-identity test before any floor quad exists.

This is provably correct: BSP order is a *total* front-to-back order on subsectors that is
correct for **any pair that overlaps on screen** (the defining property of the BSP). Within
the disjoint-surfaces-per-subsector invariant, the only cross-subsector overlaps are between
*different* subsectors, all resolved by subsector ordinal. The hazard "near floor vs far
wall" only bites if surfaces are interleaved **by type across subsectors**; keeping each
subsector's surfaces contiguous and reversing at subsector granularity tames it — exactly
SlaveDriver's per-sector sublist
([WALLS.C:2257](../../saturn-refs/SlaveDriver-Engine/WALLS.C)).

### 4.3 The index-0 hole (the layer inversion)

**The wall contract today:** `sat_wall_skip=1` leaves skipped wall columns at framebuffer
index 0; `DG_DrawFrame` re-clears the 3D view to 0 each frame
([dg_saturn.cxx:2807-2815](../src/dg_saturn.cxx)); NBG1 (6) sits on top; where NBG1 reads 0,
VDP1 (5) shows through; the colormap is remapped so real content never emits a true 0
(`sat_near_black`).

**The floor hole already exists and is the right primitive**
([r_plane.c:1152-1178](../core/r_plane.c)): when `sat_floor_vdp1_hook` claims a visplane, the
core writes index 0 across every `[top[x],bottom[x]]` run (detailshift-aware) and
`continue`s. The per-subsector emitter reuses this contract but **splits the timing**:

- **Wiring A (recommended) — quad at subsector time, hole at visplane time.** The emitter
  produces the *VDP1 quad* during the BSP walk; the *index-0 fill* stays at `R_DrawPlanes`
  via the visplane skip, keyed to the surface the emitter drew. The visplane silhouette
  (`top[]/bottom[]`) stays authoritative for the hole, while the quad uses the cheaper
  per-subsector fan. **Risk: the quad's screen coverage must be a SUPERSET of the visplane
  hole** (else a 1px ring of index-0 shows as black) — mitigated by emitting the quad to the
  subsector's full projected polygon + a 1px near-edge grow.
- **Wiring B (rejected) — per-subsector index-0 fill.** The subsector polygon's *final*
  on-screen silhouette is not known during the BSP walk (later walls clip it). Writing
  index 0 to the un-clipped polygon would zero pixels a nearer wall/sprite later wants.
  Wiring A defers the hole to after clipping — correct.

> **Reconciling §3.1 with Wiring A.** §3.1 nulls `floorplane`/`ceilingplane` at
> `R_Subsector` to suppress generation. Where the surface is genuinely VDP1-owned, the
> *quad* is emitted then, and the visplane is never built — so the "hole at visplane time"
> is realized by a **per-frame claimed-surface stamp** rather than the old skip path. The
> cleanest implementation is to stamp the visplane *before* nulling it: when the emitter
> accumulates a quad, set `vp->sat_vdp1_owned = 1` on the `floorplane`/`ceilingplane` it was
> given (both in scope at `r_bsp.c:556`), then either keep the visplane alive purely for its
> silhouette+hole (Wiring A proper) **or**, if the visplane is nulled, fall back to the inc-1
> visplane-skip path for surfaces the budget demoted. The invariant that must hold:
> **the index-0 hole and the sprite vissprite-clip use the identical silhouette.** Wiring A
> guarantees this; it is why it wins.

**Walls and floors do not fight over index-0:** within a subsector the wall skip owns the
wall band columns, the floor skip the rows below the floor edge, the ceiling skip the rows
above — **disjoint (x,[yl,yh]) runs writing the same value 0 to non-overlapping pixels.**
The one boundary to watch is the floor/wall seam (a wall's bottom row == the floor's top
row): both VDP1 quads must reach it, so grow the floor's near edge by 1px (as the wall
emitter already grows its user-clip window) — over-claim is harmless (both write 0), a gap is
the failure mode (a pixel neither claims stays real content and occludes VDP1).

### 4.4 Sprites

**Sprites stay in NBG1 (software, priority 6) — unchanged and correct.** Where a sprite
pixel is opaque, NBG1 occludes the VDP1 floor (priority 5); where the sprite is
transparent/outside its bbox, NBG1 is index 0 and the floor shows through. So sprites occlude
VDP1 floors for free, by the same layer-priority + index-0 mechanism as walls — no masking
change.

Two implications:
1. **Sprite-vs-floor depth is Doom's software clip, not VDP1.** A monster in a pit occluded
   by the near floor lip relies on `mfloorclip`/`mceilingclip` to *not draw* the occluded
   sprite rows; those rows stay index 0 and the VDP1 floor fills them. This works **only if
   the index-0 floor hole and the sprite clip use the identical silhouette** — which Wiring A
   guarantees (both read the visplane), Wiring B would not.
2. **Index-0 preservation in any NBG1 transform (the 2p-flash bug class).** Any LUT/remap on
   the framebuffer must map index 0 → 0 (`hud2p_flash_lut[L][0]=0`,
   [dg_saturn.cxx:516](../src/dg_saturn.cxx)), or every transparent hole turns opaque and
   blacks out the whole VDP1 world. The floor skip *enlarges* the index-0 region, so this
   invariant becomes **more** load-bearing — audit every post-pass (flash, `dg_fade_bake`)
   for `lut[0]==0`.

### 4.5 Command-list assembly (the concrete contract)

```
root cmd (sysclip + JUMP)   -- LINK halfword = the atomic per-frame flip
[ W bank cmd0: local-coord ]   walls (existing, unchanged, double-buffered pair W0/W1)
   ... walls accumulated, reverse-subsector ...
[ JUMP -> F bank ]             (root-LINK chain edge; one atomic flip, one PTMR)
[ F bank cmd0: local-coord ]   floors/ceilings (NEW, double-buffered pair F0/F1)
-- accumulator emitted REVERSE subsector order (far -> near): --
  for each subsector S, far to near:
    [ FUNC_UserClip = S's screen x-span, grown 1px, clamped to view ]
    ceiling quad(s) of S   (DISTORSP COLOR_4 8bpp + CRAM bank via CMDCOLR)
    floor   quad(s) of S
[ end cmd 0x8000 ]
```

- Painter order = list position = reverse-subsector-BSP = back-to-front. No Z-buffer.
- W and F are **two independent double-buffered bank pairs** (§7.1), chained root→W→F via
  the root LINK: one atomic flip, one PTMR, one kick.
- Each subsector's quads are hardware-clipped to its own x-span (`FUNC_UserClip` +
  `Window_In`), so a near subsector cannot bleed into a far one (SlaveDriver per-sector clip
  adapted to per-subsector — the x-span is free during the walk).
- Index-0 holes keyed to the `sat_vdp1_owned` stamp guarantee one-and-only-one drawer/pixel.
- Budget cap: when bank F fills (`FLOOR_CMD_CAP`), demote the tail to software (truncate,
  never overrun) — the surface's hook returns 0 and it draws as a normal visplane (§7.3).

---

## 5. CPU residue path

The complex/oversized ~10 % of surfaces (`>4`-side, or over-budget) stay in software
(visplanes → spans → NBG1), the simple ~90 % go to VDP1. Both run in the **same frame**.

**Per-surface routing.** The static tag (§2) is read at `R_Subsector`: VDP1-eligible →
emit quad + arrange the index-0 skip; residue → do nothing special, let the surface flow into
the normal visplane/span path. A *dynamic* override handles the command-budget cap: if bank F
is full, the surface is **demoted to software this frame** (like `sat_wall_vdp1` returning
1 = rejected → CPU fallback, [dg_saturn.cxx:1875](../src/dg_saturn.cxx)). So
`route = static_tag AND budget_available`, decided per surface per frame.

**The no-double-draw contract (load-bearing).** A surface's pixels are drawn by **exactly
one** of {VDP1, NBG1-software}, never both, because the two branches are driven by the **same
per-surface decision**:

- VDP1-routed: the hook claims it → the software span draw is skipped → those pixels stay
  index 0 → VDP1 fills them. Software drew nothing.
- Software-routed (residue or budget-demoted): the hook returns 0 → the software span draws
  real indices → NBG1 occludes VDP1 there → and **the emitter produced no quad** for it.
  VDP1 drew nothing.

This is the exact shape of the wall CPU-fallback (`sat_seg_cpu[]`, one byte: a surface is
*either* a VDP1 wall quad *or* a software column, never both). The subtle requirement:
emit-time (`R_Subsector`) and skip-time (`R_DrawPlanes`) routing must **agree for every
surface**. They do, because bank F is filled entirely during the BSP walk and **frozen at the
kick before `R_DrawPlanes`** — so at skip time the VDP1-owned set is final.

**Recommended: stamp the visplane.** When the emitter accumulates a quad, set
`vp->sat_vdp1_owned=1` on the `floorplane`/`ceilingplane`; `sat_floor_vdp1_hook` returns
`pl->sat_vdp1_owned` — emit-time and skip-time agree by construction, zero recomputation.
One byte added to `visplane_t`, reset in `R_FindPlane`, behind `#if` (DoomJo-safe).

**Race note (master/slave).** VDP1 quads are built by the master into bank F during REC; the
software residue spans are filled by master+slave into the framebuffer (the P3 split).
Because VDP1 and software pixels are **disjoint** (no-double-draw), the command build and the
span fill touch non-overlapping memory → race-free, exactly as the shipping wall split relies
on. Master write-through framebuffer writes are cache-purged before the blit
([dg_saturn.cxx:2798](../src/dg_saturn.cxx)); VDP1 reads its own VRAM bank, untouched.

---

## 6. RBG0 dominant flat

The dominant flat is **excluded from VDP1 entirely** (not emitted as quads) and owned by
RBG0 (Mode-7, priority 4), selected by the stable view-sector pick
(`sat_vdp2_floor_h/_pic/_band/_cmap`, [r_plane.c:875/901-919](../core/r_plane.c) — height +
picnum + lightband of the sector under the camera, **not** per-frame-biggest-pixels, which
flickered). The core skip leaves the dominant visplane index-0 for RBG0 to show through,
*before* both `RP_PlanePixels` and the per-subsector hook see it, so the row-17
`Vs = vqtot - vdom` already subtracts it.

**The dominant-flat candidate test** (the per-frame bit of §2) is the same `==` compare the
RBG0 skip already does, computed inline at `R_Subsector`:

```c
int dom_floor = (sat_vdp2_floor && floorplane
    && frontsector->floorheight == sat_vdp2_floor_h
    && frontsector->floorpic    == sat_vdp2_floor_pic
    && sat_floor_band(frontsector) == sat_vdp2_floor_band);
/* dom_floor => the emitter SKIPS this floor (RBG0 owns it; leave index-0). */
```

**The slot, and why it is gated.** RBG0 requires the direct-register cycle-pattern commit
(`rbg0_commit_cyc()` @0x25F80010 + `rbg0_commit_ramctl()` @0x25F8000E, **no slSynch** —
[dg_saturn.cxx:1276/:1255](../src/dg_saturn.cxx)) which is **wired but unverified on HW**
(snow + dead sky if it fails). Per [`VDP2_FLOOR_CONSOLIDATION.md`](VDP2_FLOOR_CONSOLIDATION.md)
(2026-06-25) the CYCxx poke now exists (correcting the older docs that say it is missing),
but its HW verdict is pending (readback rows 13/14/15: A0/A1/B1 must read `FFFFFFFF`,
`CYa≠CYb`).

> **The commit gap is the open HW blocker.** The plan defines RBG0 as the dominant slot but
> **gates it behind HW verification**. The VDP1 floor ladder (§9 inc-A0..D) **ships without
> RBG0** — the strips ride the validated async driver. RBG0 and VDP1-floors **compose by
> strength**, not mutual exclusion: if the commit cannot be made to work, the fallback is
> **the dominant flat ALSO goes to VDP1 bank F** (`Vs` becomes `vqtot`, `Vp` rises toward
> ~227 — still inside the 2-bank budget, re-checked against the §7.2 ledger). Two hardware
> laws constrain RBG0 regardless: **HW sky XOR RBG0 floor** (cell RBG0 + VRAM K-table =
> 3 banks + framebuffer = 4; enabling RBG0 means dropping HW sky to software to free the
> K-table bank), and **MP = RBG0 off** (one affine matrix can't serve two split views — MP
> floor stays software potato).

---

## 7. Budget & capacity

### 7.1 Command banks — the wall double-buffer pair vs the NEW floor pair (corrected)

**Correcting the earlier draft (and the critic's CRITICAL flag).** The shipping
`VDP1_BANK[2] = {0x25C00100, 0x25C02100}` (verified
[dg_saturn.cxx:1621](../src/dg_saturn.cxx)) is **NOT** two independent W/F banks. It is the
**double-buffer pair of the SAME wall bank** — verified
[dg_saturn.cxx:1612-1639](../src/dg_saturn.cxx): `vdp1_bank` is the buffer VDP1 is
*displaying*, `vdp1_wbank` is the buffer being *written* this frame; they swap each frame so
VDP1 never reads a half-written list (the tearing fix). Both halves hold walls. There is
today **no floor bank at all.**

So the floor needs a **brand-new, independent double-buffered pair F0/F1** — itself two
buffers, for the same anti-tear reason — *in addition to* the wall pair W0/W1. Measured
inputs (DOOM2, HW — re-verify each before locking a cap): walls `VD1` 20–227 (peak 227,
typical ~150); `Qp ≈ 169` untextured geometry; `Vp ≈ 158` textured-secondary after the
64-texel u-tiling at `FLOOR_HBAND=16`, dominant excluded.

A single 256-cmd bank cannot hold 227 walls + 158 floors = **385 cmds**, so walls and floors
need **separate bank pairs**:

- **Bank pair W = walls** (existing W0/W1 at `0x25C00100`/`0x25C02100`, `WALL_CMD_CAP=248`),
  kicked by the existing `sat_walls_kick` (unchanged — do not regress the shipping wall path).
- **Bank pair F = floors/ceilings** (NEW F0/F1), accumulated into a `floor_acc[]` during the
  same BSP walk, drained before `vdp1_wpn_kick` under `FLOOR_CMD_CAP=248`. W and F chain
  root→W-end→F-start via the root LINK (one atomic flip, one PTMR), mirroring SlaveDriver's
  `JUMP_ASSIGN` chaining ([SPR.C:393](../../saturn-refs/SlaveDriver-Engine/SPR.C)). Because
  walls and floors occupy **disjoint index-0 holes**, W↔F draw order is moot (no overdraw).

**Why bank F cannot live at `0x25C04100`/`0x25C06100` (the old draft's error).** After the
wall pair (W0 `0x25C00100`..`0x25C02100`, W1 `0x25C02100`..`0x25C04100`), the gap to
`WTEX_BASE=0x25C05000` (verified [dg_saturn.cxx:1695](../src/dg_saturn.cxx)) is only
`0x25C05000 − 0x25C04100 = 0xF00` = **3.8 KB** — it cannot hold even one 8 KB F buffer, let
alone two (16 KB). The old "F at 0x25C04100/0x25C06100" claim does **not** close. Bank F must
be carved from VRAM freed by the §7.2 cull, **with `WTEX_BASE` shifted up** to make room.
Concretely, F0/F1 go **immediately after the wall pair**, and the wall texture pool starts
16 KB higher:

| Region | Addr (revised) | Size | vs today |
|---|---|---|---|
| root + empty | `0x25C00000` | 256 B | unchanged |
| W bank 0 | `0x25C00100` | 8 KB | wall double-buffer half 0 (unchanged) |
| W bank 1 | `0x25C02100` | 8 KB | wall double-buffer half 1 (unchanged) |
| **F bank 0** | `0x25C04100` → `0x25C06100` | **8 KB** | **NEW** floor double-buffer half 0 |
| **F bank 1** | `0x25C06100` → `0x25C08100` | **8 KB** | **NEW** floor double-buffer half 1 |
| **flat cache (8×4 KB)** | `0x25C08100` → `0x25C10100` | **32 KB** | **NEW** dedicated flat LRU |
| **wtex pool (shrunk)** | `0x25C10100` → … | **see §7.2** | **WTEX_BASE shifted up 0xB100 from `0x25C05000`** |

`WTEX_BASE` moves from `0x25C05000` to `0x25C10100` (up by `0xB100` ≈ 44 KB: the 16 KB F
pair + 32 KB flat cache − the 4 KB it gains by no longer needing the old W↔wtex gap). The
wtex pool is **shrunk** to fit (§7.2 proves the bytes), so the F pair + flat cache do not
push past 512 KB. Cost vs today: **+16 KB (F pair) + 32 KB (flat cache) = 48 KB**, all
reclaimed from the dead WPN/HUD region by the §7.2 cull.

### 7.2 VRAM — the closed, post-cull byte ledger

**The cull is a hard gate, and it CANNOT both reclaim WPN's tail AND keep wtex at 448 KB.**
The critic is right: the old "448 KB wtex + 16 W + 16 F + 32 flat = 512 KB, then ~44 KB free"
was internally contradictory. Here is the closed map.

**Today's VDP1 VRAM (512 KB, `0x25C00000`..`0x25C80000`), verified:**

| Region | Addr | Size | Verified |
|---|---|---|---|
| root + empty | `0x25C00000` | 256 B | [:1619](../src/dg_saturn.cxx) |
| W bank pair (double-buffered) | `0x25C00100` | 16 KB | [:1621](../src/dg_saturn.cxx) |
| (gap) | `0x25C04100` | 3.8 KB | the `0xF00` gap |
| wtex pool (16 narrow×16 KB + 6 wide×32 KB) | `0x25C05000` | **448 KB** | [:1695-1701](../src/dg_saturn.cxx), ends `0x25C75000` |
| **WPN cache (4×44 KB)** — **DEAD, aliases wtex wide** | `0x25C45000` | 176 KB | [:1631](../src/dg_saturn.cxx); `WTEX_WIDE_BASE=0x25C45000` [:1700](../src/dg_saturn.cxx) — **same address** |
| **HUD tex** — **DEAD** | `0x25C78000` | 20 KB | [:1651](../src/dg_saturn.cxx) |

Two facts the ledger must honor:
1. **WPN aliases wtex.** `WPN_TEX_BASE = 0x25C45000 == WTEX_WIDE_BASE` (verified). The WPN
   cache is *not* extra VRAM on top of wtex — it overlaps the wtex wide pool. It is harmless
   today only because the weapon path is software (layer inversion) and never writes there.
   **Freeing WPN reclaims ZERO bytes** (those bytes are already accounted as wtex wide).
2. **HUD tex is genuine dead VRAM** at `0x25C78000` (past wtex's `0x25C75000` end), 20 KB.
   It is the only region freeing it actually reclaims.

So the cull's real reclaim is: **HUD 20 KB (real) + whatever we shrink wtex by**. Since
freeing WPN reclaims nothing, the 48 KB the floor needs (16 KB F pair + 32 KB flat cache)
**must come from shrinking wtex.** The shrink: **wtex 16 narrow + 6 wide → 14 narrow + 6
wide** frees `2 × 16 KB = 32 KB`; combined with the 20 KB dead HUD = **52 KB reclaimed**,
covering the 48 KB floor cost with **~4 KB margin**.

**Post-cull VDP1 VRAM (512 KB, closed):**

| Region | Addr | Size | Notes |
|---|---|---|---|
| root + empty | `0x25C00000` | 256 B | unchanged |
| W bank pair | `0x25C00100` | 16 KB | walls (unchanged) |
| **F bank pair** | `0x25C04100` | **16 KB** | **NEW** floor double-buffer |
| **flat cache (8×4 KB)** | `0x25C08100` | **32 KB** | **NEW** |
| wtex pool (**14** narrow×16 KB + 6 wide×32 KB) | `0x25C10100` | **416 KB** | WTEX_BASE shifted; **2 narrow slots dropped** |
| (HUD freed) | — | (20 KB reclaimed) | dead `VDP1_HUD_TEX` def removed |
| **split-PTMR reserve** | tail | **≥8 KB** | for the 2nd PTMR in split-screen |
| **Total used** | | **≤ 504 KB** | 16 + 16 + 32 + 416 + 16(root+gap rounding) = within 512 KB, ≥8 KB tail free |

Arithmetic: `0.25 + 16 + 16 + 32 + 416 = 480.25 KB`; plus the gap/rounding ≈ 488 KB; leaving
**~24 KB** for the ≥8 KB split-PTMR reserve and headroom. **The cull frees the floor's 48 KB;
wtex shrinks from 448→416 KB (it does NOT stay at 448); the dead HUD def is removed; WPN's
"freeing" is acknowledged as a no-op (it aliases wtex).** The map closes ≤512 KB with the
≥8 KB PTMR reserve intact.

Flats use a **dedicated cache, separate from the 22→20-slot wall `wtex` pool**: (1) flats are
fixed 64×64 (no narrow/wide split, no `padW`), so the two-pool LRU is over-engineered for
them; (2) sharing would let a wall texture evict a flat mid-frame (and vice-versa), doubling
LRU thrash on the memory-bound path; (3) the 64-wrap invariant (`FLOOR_MAXTILES`) stays
local. An **8-slot × 4 KB = 32 KB** flat LRU, same 8bpp COLOR_4 16-bit-packed upload +
`wall_light_colr` CMDCOLR lighting. 8 distinct flats/frame is generous (DOOM2 rooms rarely
show >4–5 at once; the dominant goes to RBG0, removing the biggest from this cache).

> **Cost of the wtex shrink (16→14 narrow).** Two fewer narrow slots = slightly more wall
> texture bakes (watch `bk`, row 16). The wall cache has historically run on fewer slots
> (8 narrow / 3 wide pre-8bpp); 14 narrow + 6 wide = 20 is still nearly double that.
> Accepted, monitored. If `bk` spikes on HW, the fallback is to drop the flat cache to
> 6 slots (24 KB) and restore one narrow wtex slot.

### 7.3 Truncation (no silent caps)

Bank F drains under `FLOOR_CMD_CAP = 248` (`VDP1_BANK_CMDS - 8`), mirroring `vdp1_walls_flush`
([dg_saturn.cxx:2270](../src/dg_saturn.cxx)). Drop order on overflow (drop-first → keep-last,
SlaveDriver reverse-FIFO [SPR.C:142](../../saturn-refs/SlaveDriver-Engine/SPR.C)):

1. **Farthest first.** `floor_acc[]` is accumulated BSP front-to-back, so far = highest
   index → truncate from the tail. Far surfaces are smallest screen-area, most-occluded.
2. **Among equidistant, smallest screen-area first** (bbox `(maxx-minx)*(ymax-ymin)` — reuse
   the `RP_PlanePixels` `pix` estimate as the proxy).
3. **Non-dominant always before dominant** — automatic (the dominant is on RBG0, never in F).

**Never drop to a hole.** Every accepted surface gets ≥1 flat baseline command; an
over-budget surface is **dropped entirely to the CPU software span** (hook returns 0 →
`R_DrawPlanes` draws it) — the graceful residue handoff, not corruption.

**Emit a counter.** Add a truncation count `tr` to overlay **row 17**
(`"FLR Vs%d Vp%d d%d%% tr%d"`): surfaces the floor emitter refused this frame. A persistent
non-zero `tr` on HW = over budget → coarsen `FLOOR_HBAND`, or accept CPU residue. Ship with
`tr` visible, never a silent drop.

### 7.4 VDP1 fill margin

Removed from CPU (the win): `P` (visplane/span generation, 12–55 ms HW pot0), `EX` (executor
span fill, pot0 12.5–30.7 ms almost all floor), and the **blit** of those pixels (~5.5 ms
total; the dominant alone is a large fraction). The dominant (~49–93 %, avg ~64 %) goes to
RBG0 (zero VDP1 cost); the secondary set (`Vp ≈ 158`) goes to bank F.

Added to VDP1 (the cost): the secondary strips fill ≈ **1.6–2.5 ms async** (~2 % of a 100 ms
frame) at the textured rate (~14.3 Mpx/s) + ~16 cyc/command × ~158 cmds (~0.1 ms fetch). Small
**because the dominant — largest-area, worst-swim — is on RBG0, not VDP1.**

> **Correction — there is no "state D / 90-cmd headroom".** The "VDP1 finishes early" framing
> is **DISPROVEN on HW**: EDSR reads `B` always
> ([dg_saturn.cxx:780](../src/dg_saturn.cxx)); the fill-finish metric was removed
> ([VDP1_ARCHITECTURE.md:115-121](VDP1_ARCHITECTURE.md)). The live headroom signal is **row 2
> `Dr%`** (~92–94 % measured). The limiter is **overdraw (iteration), not fill** — VDP1
> iterates the whole projected trapezoid even where clipped, so the real risk is a **near
> floor projecting to a huge off-screen span** (the `y≈±2000` blow-up the walls fixed with
> world-anchored bands, and §3.3.1's `SAT_YCLAMP_GUARD` re-applies on the screen-Y axis).
> NO-GO signals: `Dr%` collapse below ~80 % (fill/overdraw became the bottleneck — the kill
> switch); persistent `tr` (world needs >248 floor cmds). The fill budget is comfortable; the
> realistic bottleneck is **near-floor overdraw**, addressed by bands + z-clamp, monitored by
> `Dr%`.

---

## 8. Expected gain + measurement protocol

### 8.1 What moves (HW, DOOM2)

| Cost | Direction | Mechanism | Magnitude |
|---|---|---|---|
| **P** (visplane/span generation) | **DOWN** | deported surface never builds spans; a 4-corner quad replaces per-column build | 12–55 ms pot0; 22 ms/view in 2p pot0 |
| **EX** (executor span fill) | **DOWN** | recorded spans removed from the drain | 12.5–30.7 ms pot0 (almost all floor) |
| **blit** (FB→VDP2 copy) | **DOWN** | deported pixels leave NBG1 (index-0 holes) | ~5.5 ms; floor is a large fraction of 320×224 |
| VDP1 strip fill (async) | up, ~free | runs in parallel on the idle VDP1 | ~1.6–2.5 ms async |
| per-band CPU build | up, sub-ms | per-band trig (~24×/surface) vs per-scanline (~112×) — *cheaper* than the fill it replaces | sub-ms expected |

**Dominant win = P (generation) at pot0**, where the headroom lives, mirroring the measured
wall offload (REC 110→5–23 ms, Bp 16→3 ms,
[VDP1_ARCHITECTURE.md:196-199](VDP1_ARCHITECTURE.md)). EX/blit reductions are real but
secondary (EX is only ~2.7 ms at pot1/2; blit is bus-bound). At pot1/2 the value is mostly
**quality** (perspective-tiled floors) plus a modest P/EX trim — so the **headline fps gate is
read at pot0 and 2p pot0**, where the per-view ×N REC term is the ceiling. The bet gets
*cheaper* per view in split (smaller dominant flat, less fragmentation), so it improves, not
degrades, in 2p.

### 8.2 The exact overlay rows = GO/NO-GO, and the A/B toggle (a hard prerequisite)

Current layout (verified [dg_saturn.cxx](../src/dg_saturn.cxx); the overlay was reorged
2026-06-24/25 — benchmark docs use the **stale** numbering):

| Row | Content | Use |
|---|---|---|
| 0 | `{fps} avg{} to{} cd{}` | fps; `to` (timeout) must stay 0 |
| 1 | `vp{} {pot} bl{} b{ms} h{}` | `b` = blit ms (watch ↓ at pot0) |
| 2 | `VD1 {n}{D/B} Dr{}%` | VDP1 cmds + EDSR + **`Dr%` headroom** (collapse <80 % = NO-GO) |
| 4 | `REC p50 p95 mx d{}` | windowed REC distribution |
| 5 | `Bw Bp P M` | per-phase REC split — watch **P↓**, Bp not rising |
| 10 | `PK Bw Bp P M` | per-phase independent peaks — the basis to size the P offload |
| 16 | `SPL sw v0 v1 k bk` | 2p per-view costs — `v0/v1`↓ without regressing `k`; **`bk`** = wall bakes (watch after the wtex shrink, §7.2) |
| **17** | `FLR Vs Vp d% tr` | **textured GO/NO-GO: `Vp ≲ 64` one-bank, `≲ 120` two-bank; `tr` = truncations (must be 0)** |
| **20** | `PAR ss Q Qp q4%` | **geometry GO/NO-GO: `Qp ≈ 169`, `q4 ≈ 80–93 %`** |

**The one-button A/B toggle — does NOT exist yet, and is a PREREQUISITE for every A/B
GO/NO-GO, so it is built FIRST (inc-A0, §9).** Pad Y is consumed (RBG0 cycle under
`VDP2_RBG0_TEST`, else the hash toggle; the old VDP1-floor Y-flip was retired/parked,
[dg_saturn.cxx:3026](../src/dg_saturn.cxx)). Because the pad is saturated, the toggle is a
**deliverable rung, not a footnote**:

- Add compile flag `VDP1_FLOOR_TEST_AB`.
- Bind `sat_ss_floor` (the runtime gate) to a **free chord** (e.g. **X+Y held**, verified
  unused while held together) so one press flips the whole feature live on the same scene.
- The toggle's own **GO (inc-A0):** holding the chord flips `sat_ss_floor` 0↔1 with no other
  side effect, the overlay rows 5 `P` / 1 `b` / 4 `REC` / 0 `fps` re-baseline cleanly on the
  flip, and the flip is glitch-free (no pad bounce, no double-toggle). Without this GO, no
  later rung's A/B comparison is executable.

**Baseline capture.** The window auto-resets on map/potato/hash/blit change
([dg_saturn.cxx:923-931](../src/dg_saturn.cxx)). For peaks (`Vp/Qp/PK`), WALK the level then
photograph; for instantaneous A/B, stand STILL at a plane-heavy spot and toggle the chord.
Capture the 6 reference HW spots (tech/eau, sombre/esc, brun/eau × pot0/1/2 + a damage-flash
frame), DOOM2, plus 2p split, emitter OFF (baseline) then ON.

### 8.3 HW-only verdicts (the Ymir law, HARD)

Ymir understates memory-bound cost **3–10×** (Bw 2.2–3 ms Ymir vs 7–26 ms HW). Ymir validates
**only**: geometry sizers (`Vp`, `Qp`, `q4` — deterministic), mechanics (`VD1`, `Dr%`,
command count, the A/B toggle's logic), byte-identity (floor-OFF must stay byte-identical),
and visual correctness (swim/seams/tear, sprite-clip, index-0). **Every REC/EX/blit/fps GO
verdict is read on real Saturn.** A green Ymir reading is NOT a GO for any ms claim.

---

## 9. Increments — correctness-scaffolding-first, then HW-only risk

**On the ordering principle (the critic's §9 flag).** The genuinely new *existential* risk is
the per-subsector projection + **affine swim** (HW-judged). A literal "riskiest-first" ladder
would retire swim on rung 0. We do **two** things so the ladder is honest:

1. **inc-A0.5 is an early, throwaway swim spike-test** on one plane-heavy subsector, run on
   **HW before** the full classifier/painter/cull scaffolding — it retires the existential
   "does a world-anchored screen-Y banded floor quad even look acceptable on the real chip?"
   question first, cheaply, with no merge.
2. The remaining ladder then follows **"correctness-scaffolding-first, then HW-only risk"**:
   build the inert, byte-identity-checkable scaffolding (classifier, painter order, index-0
   stamp) before spending HW budget on the full textured painter. This is stated as the
   ordering principle, so the ladder no longer reads as a contradiction.

Each rung is independently mergeable, DoomJo-safe (NULL/zero-default hook, pure C), and gated
by an explicit GO/NO-GO.

**inc-A0 — The A/B toggle + static classifier + world verdict, NO emit.** Build
`VDP1_FLOOR_TEST_AB`, bind `sat_ss_floor` to the free chord (§8.2); build
`R_ClassifySubsectors`, allocate `sat_ss_class`, compute `sat_ss_world_small`. Surface static
`Qstat` (load-time `qsum`) and `simple%` (static `SSC_SIMPLE` share). *GO (toggle):* the chord
flips the gate live with no side effect, overlay re-baselines on the flip (§8.2). *GO (Ymir,
deterministic):* `Qstat ≈` live row-20 `Qp` peak (~169) and `simple% ≈ q4%` (80–93 %) at the
reference spots. *NO-GO:* toggle bounces/double-fires (fix the chord debounce) or classifier
divergence >few % ⇒ static side-count differs from the BSP-walk view (firstline/numlines
mismatch) — fix before any emitter.

**inc-A0.5 — Swim spike-test (HW, throwaway, retires the existential risk).** On a single
hand-picked plane-heavy subsector, emit a world-anchored screen-Y banded floor quad (§3.3.1,
hard-coded flat, no cache, no painter integration, behind `VDP1_FLOOR_SPIKE`). Toggle it with
the inc-A0 chord. *GO (HW visual):* the quad sits on the real floor, tracks the player, lines
stay straight, swim acceptable at `HBAND=16` (or a tuned non-uniform set). *NO-GO:*
unacceptable swim even with non-uniform bands + horizon flat-clamp ⇒ **the bet's core is
unsound; stop and reconsider** before building the scaffolding. This is the one rung that can
kill the whole plan; it is run early and cheaply. **Not merged** (spike only).

**inc-A1 — Painter-order plumbing, walls re-routed, NO floor quads.** Extend the accumulator
with `kind`+`subsector_seq` (the NEW grouping, §4.2); reverse-emit the *existing walls*
through the per-subsector loop. *GO (Ymir):* **byte-identical** wall render, fps unchanged,
`to`=0 (the strict-generalisation check of §4.2). *NO-GO:* any wall reorder artifact → the
seq assignment or reverse loop is wrong (must reverse at **subsector** granularity, bumping
`subsector_seq` once per subsector, not per surface).

**inc-A2 — Index-0 ownership stamp + Wiring-A agreement, claim-all stub (no quads).** Add
`visplane_t.sat_vdp1_owned`, stamp it at the per-subsector hook, make `sat_floor_vdp1_hook`
return it; keep emitting no quads (claim-all blacks the floor). *GO (Ymir, visual):* exactly
the tagged surfaces go black; sprites in pits clip correctly against the hole (silhouette
shared); index-0 never leaks as black on real content; flash/fade map 0→0. *NO-GO:* black
rings at floor/wall seams (silhouette/quad mismatch) or sprite bleed (clip/skip diverged).

**inc-B — VRAM cull + second command bank F + JUMP chain + flat quad per simple floor (HW).**
**Do the §7.2 cull first:** remove the dead `VDP1_HUD_TEX` def (20 KB real), shrink wtex
16→14 narrow (32 KB), shift `WTEX_BASE` up `0xB100`, place the F pair `0x25C04100`/`0x25C06100`
and the 32 KB flat cache `0x25C08100` (the §7.2 ledger). Add bank F, the root-LINK chain,
`sat_floors_kick` (parameterized cap/bank per §3.5); emit one `FUNC_Polygon` per `numlines≤4`
floor (dominant flat index, CRAM-lit), reverse-order, 1px-grown near edge, null the visplane.
*GO (HW):* floors fill their holes with no index-0/black ring; far floor never paints over a
near wall/floor; `VD1`+`Vp` under the bank cap (`Dr%` ≥80 %); row 5 `P`↓, row 1 `b`↓ at pot0;
walls unregressed (`VD1`/`Dr%`/fps identical with floor OFF; `bk` not spiking after the wtex
shrink). *NO-GO:* black seams (grow more / fix Wiring-A superset), painter inversions
(grouping broken), bank overflow (`Dr%` collapse / `tr`>0 → tighten tag → truncate),
`Bp`/REC rises instead of `P` falling (emitter too expensive — must be sub-ms), or `bk` spike
(restore a wtex slot, shrink flat cache to 6 — §7.2 fallback).

**inc-C — Textured simple floors: HBAND banding + 64-tile u-loop + flat cache (HW, the
riskiest *integrated* visual rung).** The full §3.3.1 (V) screen-Y bands + §3.4 (H) u-tiles +
the flat cache; CRAM light via `wall_light_colr`; `FUNC_UserClip` per-subsector; clamp the
re-derived `dist` UP before re-projecting (`SAT_YCLAMP_GUARD`). Validate swim at
`N∈{16,24,32}`, pick the knee (informed by inc-A0.5). *GO (HW visual):* the quad sits on the
real floor, tracks the player, lines stay straight, swim acceptable at the chosen band height;
row 17 `Vp` ≤ budget, `tr`=0; `P`/blit↓ and fps↑ at pot0; color bit-matches NBG1 (bank 1 =
live PLAYPAL, §3.1.1). *NO-GO:* swim unacceptable → non-uniform bands / horizon flat-clamp;
vanishing-point holes (`dist`/den→0 guard missing) → revert (do NOT ship a swimming floor);
bank overflow → truncate.

**inc-D — Ceilings + the >4-side fan + world-size switch + horizon mitigations + cap
hardening (HW).** Add ceilings (`height>viewz` discriminant, exclude sky); fan emission for the
~10 % residue (or leave software per `sat_ss_world_small`); non-uniform bands; reverse-FIFO
truncation with the `tr` counter. *GO (HW):* full floors+ceilings, no double-draw shimmer at
residue/VDP1 boundaries, demoted-on-overflow surfaces fall back cleanly; net fps↑ at the 6
spots AND in 2p split (row 16 `v0/v1`↓ per view, `k` not regressed). *NO-GO:* REC rises
(per-band build > P saved — the bet fails, revert) or 2p regression (`k`/MST up).

**inc-E (optional, HW-blocked) — RBG0 dominant flat.** Enable `sat_vdp2_floor`, drop HW sky to
software, verify `rbg0_commit_cyc` lands (rows 13/14/15: A0/A1/B1 = `FFFFFFFF`, `CYa≠CYb`, no
snow). Independent of inc-A0..D (VDP1 takes whatever RBG0 cannot). *GO:* dominant renders
perspective-correct under VDP1, no snow, software sky still draws. *NO-GO:* snow persists →
RBG0 parked, dominant falls back to VDP1 bank F (`Vs`→`vqtot`, recheck the §7.2 budget); ship
inc-A0..D regardless.

---

## 10. Risks & mitigations

| Risk | Detection signal | Mitigation / fallback |
|---|---|---|
| **Swim** (floors have a bigger 1/z range than walls; horizon worst; the screen-Y banding is NEW code with no wall analogue) | inc-A0.5 spike + inc-C HW visual across N=16/24/32; swim concentrates at the horizon | world-anchored bands only (§3.3.1, NEVER screen-clamp — HW-reverted); `yslope[]`-inverted footprint re-projection; non-uniform thin-near-horizon bands; flat-clamp top 1–2 bands; `SAT_YCLAMP_GUARD`; near-field-only or software for that surface |
| **Command explosion** (pillars fragment x-runs; >64-texel strips u-tile; screen-Y bands multiply per surface) | row 17 `Vp` / row 2 `VD1` climbing toward `FLOOR_CMD_CAP`; `tr`>0 persistently | raise `FLOOR_HBAND`/`FLOOR_MAXTILES`; reverse-FIFO truncation (inc-D); classify to route only simple subsectors; hard fallback = software (≥1 baseline cmd, never a hole) |
| **Bank-address arithmetic wrong / VRAM map does not close** (the §7.1 contradiction the old draft shipped) | the §7.1/§7.2 closed ledger; build-time static-assert that F pair + flat cache + shrunk wtex + ≥8 KB reserve ≤ 512 KB | F pair is a NEW independent double-buffer (not the wall pair); WTEX_BASE shifts up `0xB100`; wtex shrinks 448→416 KB; dead HUD removed; WPN "free" acknowledged as a no-op (aliases wtex) — static-assert the total |
| **CPU/VDP1 desync (décrochage)** (floors-on-VDP1 vs CPU sprites/HUD on NBG1; inherited `VDP1_MANUAL_CHANGE=0` tearing) | HW visual tear/lag under fast motion | floor rides the SAME early kick as walls (proven phase); do NOT add a mirror / 1-frame delay (HW-tested worse); tearing accepted until the proper anti-tear fix |
| **Regressing the shipping VDP1 walls** (parameterizing `wall_emit_band`, re-ordering the accumulator, shrinking wtex) | with floor OFF, row 2 `VD1`/`Dr%`/row 0 fps must be number-identical to baseline; inc-A1 byte-identity; `bk` row 16 after wtex shrink | separate isolated bank pair F (inc-B); NULL/zero-default hook; never share the wall cap/cursor; prefer a floor-specific emitter (P2, §3.5) over touching the wall emitter; revert wtex shrink (flat cache 6 slots) if `bk` spikes |
| **Index-0 reservation fragility** (any per-pixel NBG1 transform turning index-0 opaque blacks out the VDP1 world — the 2p-flash class; the floor skip *enlarges* the index-0 region) | inc-A2 visual (opaque holes, walls/sky/floor disappearing) | core skip writes true index-0; audit every framebuffer post-pass (flash, `dg_fade_bake`) for `lut[0]==0`; this is exactly the inc-A2 gate |
| **No-double-draw breaks** (emit-time vs skip-time routing disagree → double-bright shimmer) | inc-D HW (shimmer at residue/VDP1 boundaries) | accumulator frozen at the kick before `R_DrawPlanes`; stamp `visplane_t.sat_vdp1_owned` as the single source of truth (emit-time and skip-time agree by construction) |
| **Colormap mismatch** (VDP1 floor lands in a different CRAM bank than the software/RBG0 layers) | inc-C HW: floor-ON color differs from floor-OFF software at the same sector | resolve `(lightlevel>>LIGHTSEGSHIFT)+extralight` at `R_Subsector` (§3.1.1), identical to `R_MapPlane`; same base index as `sat_vdp2_floor_cmap`; bank 1 = live PLAYPAL; byte-identity GO |
| **REC rises instead of falling** (per-band build > P saved) | row 5 `P` does not drop / `Bp` rises on HW | the emitter must be sub-ms; if not, the bet fails for that surface class — revert (HW-only verdict; Ymir understates P 3–10×) |
| **RBG0 commit gap** (cycle-pattern never reaches the chip → snow + dead sky; Ymir hides it) | rows 13/14/15 HW readback — A0/A1/B1 must read `FFFFFFFF`; `CYa==CYb` ⇒ SGL ISR clobbered the poke | keep RBG0 (inc-E) fully independent of the VDP1 ladder; if the poke fails, dominant → VDP1 strips or CPU; HW sky XOR RBG0 floor remains a hard bank-budget law; MP = RBG0 off |
| **VRAM cull frees less than expected** | the post-cull byte map (§7.2) | the cull frees HUD (20 KB real) + wtex shrink (32 KB); WPN frees nothing (aliases wtex); if short, shrink the flat cache to 6 slots / drop a wide wtex slot; reserve ≥8 KB headroom |

---

## 11. DoomJo-safety appendix

`core/` is compiled **verbatim** by DoomJo (GCC 9.3, pure C). Every addition here follows the
established NULL-default discipline so DoomJo links unchanged and the shipping VDP1 walls do
not regress:

1. **Pure C, no C++isms.** `R_ClassifySubsectors`, the `R_Subsector` emit block, `ss_floor_cmap`,
   and the new hook are plain C — named params, no references, no unnamed params (which GCC 9.3
   errors on, GCC 14 only warns). They sit in `r_bsp.c` beside the existing pure-C
   `RP_Subsector` caller; `r_parallel.c` stays pure C.
2. **A global flag, default 0.** `sat_ss_floor = 0` — DoomJo never sets it, so it is inert.
   Same as `sat_vdp1_floor`/`sat_vdp2_floor`/`sat_plane_parallel`.
3. **A function pointer, default NULL.** `sat_ss_floor_hook = NULL`, guarded
   `if (sat_ss_floor && sat_ss_floor_hook && sat_ss_class)` so a NULL hook short-circuits with
   zero cost — identical to `sat_floor_vdp1_hook` ([r_plane.c:1152](../core/r_plane.c)) and
   `sat_wall_hook` ([r_segs.c:219](../core/r_segs.c)).
4. **No new mandatory core data.** `sat_ss_class` is NULL until the Mimas platform arms
   `sat_ss_classify` (default 0). On DoomJo: `R_ClassifySubsectors` is never called, the array
   never allocated, the `R_Subsector` block short-circuits on `sat_ss_class==NULL` /
   `sat_ss_floor_hook==NULL`. **Zero bytes, zero cycles.**
5. **Visplane stamp behind `#if`.** The one-byte `visplane_t.sat_vdp1_owned` is compiled out
   on DoomJo (`#if SAT_*`), so the struct layout DoomJo sees is unchanged.
6. **The new `subsector_seq`/`kind` accumulator fields are platform-side.** They live in the
   Mimas `wall_acc`/`floor_acc` structs in `src/dg_saturn.cxx`, not in any `core/` struct —
   the core only passes a `const subsector_t *` and the surface fields. DoomJo's accumulator
   is unaffected.
7. **Profiler unchanged.** `RP_Subsector`/`RP_PlanePixels` still run (the validation oracle);
   the classifier is additive and touches no profiler counter. Their bodies stay `#if RP_PROF`
   with `(void)`-cast `#else` so the shared core links with RP_PROF off.
8. **No new kick, no new freeze-zone.** The floor quads ride the existing `sat_walls_done_hook`
   kick (floor known during the BSP walk), so no new `slSlaveFunc` is dispatched — no new
   `rp_sgl_workptr_reset` (GBR+68/+72) exposure.

DoomJo keeps linking unchanged because every touch-point is inert by default and the only
non-default behavior is armed exclusively by the Mimas platform (`src/dg_saturn.cxx`,
`extern "C"`), never by `core/`.

---

## 12. Sources

Multi-agent investigation (2026-06-25) + critic revision (same day): current
`src/dg_saturn.cxx` (VDP1 driver — verified `VDP1_BANK[2]` is the wall double-buffer pair
[:1612-1639], `WTEX_BASE=0x25C05000` [:1695], `WPN_TEX_BASE==WTEX_WIDE_BASE==0x25C45000`
[:1631/:1700] aliasing, `VDP1_HUD_TEX=0x25C78000` [:1651], `wall_emit_band` hardcoded bank/cap
[:2046/:2055/:2073], reverse-paint at accumulator-entry granularity [:2319]), `core/r_bsp.c` /
`r_plane.c` / `r_segs.c` / `r_parallel.c` (hooks, profilers, `yslope[]` [r_plane.c:319], the
P3 path), `core/p_setup.c` / `r_defs.h` / `r_main.c` / `m_fixed.h` (geometry + projection
ground-truth); `saturn-refs/SlaveDriver-Engine` `WALLS.C`/`WALLASM.S`/`SPR.C` (the whole-world
VDP1-quad reference: `rectTransform` lattice, `TILENEARCLIP F(33)`, `MIPDIST F(256)`, `EZ_`
command system, reverse-FIFO truncation, per-sector JUMP_CALL chaining, per-sector user-clip
occlusion); companions [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md),
[`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md), [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md),
[`VDP2_FLOOR_CONSOLIDATION.md`](VDP2_FLOOR_CONSOLIDATION.md); the HW perf record
(`REC_REDUCTION.md`, `REC_BENCHMARKS.md`, `SPLIT_BENCHMARKS.md` — the Ymir-vs-HW law and the
`Qp/q4/Vp/Dr%` measurements); memories [[doomsrl-vdp1-capacity]],
[[doomsrl-vdp1-world-renderer]], [[doomsrl-vdp1-async]], [[romain-perf-economy]].
