# Sprites on VDP1 / SCU-DSP — feasibility study

**Question.** Can Mimas offload sprite/thing rendering off the software masked-column
path — move the *fill* to VDP1 hardware quads and the *projection* arithmetic to the
idle SCU-DSP — for a net win?

## Status (2026-07-05): player weapon SHIPPED on VDP1

The first draft said **NO-GO** ("the VDP1 weapon was reverted, so all sprites-on-VDP1 is
dead"). **That was a premature armchair kill** — the same mistake made before on the RBG0
floor and the VDP1 walls, both called infeasible, both now shipping. Retracted, and then
**the player weapon was put back on VDP1 at priority 7 above NBG1 and validated on Ymir** —
the exact feature that was declared dead. See "Working implementation" below for the recipe.

- **Player weapon on VDP1 → DONE** (gated `SAT_WPN_VDP1`, default off).
- **World things on VDP1 → next increment** (the real ~6 ms prize; needs present-after-masked
  + `FUNC_UserClip` occlusion + software fallback + a headroom measurement).
- **DSP for sprite projection → NO** (robust; billboarded sprites need scalar divides, the
  DSP's weakness — see below. Never the prize).

### Working implementation — the recipe (validated on Ymir)

The blocker was the layer inversion (VDP1 prio 5 *below* NBG1 prio 6). The fix: VDP1 sprite
priority is **per-command**, so raise *only* the weapon above NBG1, walls/floors stay below.

1. **Per-command priority — sprite TYPE 3.** Priority is a 2-bit field at **framebuffer bits
   13–14** (`pr:2`, selects registers 0–3). Found by an on-screen 5-quad bit sweep (bits
   11–15; only 13 and 14 rose). Bit 15 is RGB/shadow, **not** priority — so RGB-mode sprites
   can never be raised, which is why the old RGB weapon was invisible-or-below.
2. **Priority split.** `slPrioritySpr0(5)` (walls/floors: their CRAM addresses ≤ 2047 leave
   bits 13–14 clear → register 0 → below NBG1) + `slPrioritySpr1..7(7)` (anything that sets a
   priority bit → above NBG1). ([dg_saturn.cxx](../src/dg_saturn.cxx), `SAT_WPN_VDP1` block.)
3. **8bpp palette, not RGB.** The weapon patch is unpacked to 8bpp palette indices (the
   light-shaded index) so its CMDCOLR can carry the priority bit: `CMDCOLR = 0x2000 | 0x0100`
   (pr bit 13 → register 1 = prio 7 | CRAM bank 1 = full-bright PLAYPAL; the texel is the
   shaded index, so bank 1 turns it back into the correct colour). CMDPMOD `0x00A0` =
   256-colour bank (bit 5) | ECD-disable (bit 7), **SPD (bit 6) clear** → texel 0 transparent.
4. **Texel 0 is the HW transparent code**, so a genuinely-black weapon pixel (shaded index 0)
   would punch a hole — remapped to the darkest non-zero palette index (opaque, reads black).
5. **Early emit.** The VDP1 present (root flip) fires at **end of R_DrawPlanes**, before the
   masked phase draws sprites. So the weapon is emitted in `sat_walls_kick` (right after the
   BSP walk, before the present) by calling the core `R_DrawPlayerSprites` early — reusing the
   engine's positioning/light; a one-line core gate `sat_psprite_early` skips the late software
   draw. The weapon can do this because it doesn't need the BSP walk; **world things do**, so
   they will need the present moved after the masked phase.
6. **VRAM.** The old weapon cache (`0x25C45000`) overlapped the live wall pool → corrupted
   walls; relocated to two reclaimed WTEX wide slots (`WTEX_WIDE_N` 6→4, cache at `0x25C61000`,
   8bpp halves the size). Watch the `bk` (wall re-bake) counter.

Result: the weapon renders on the VDP1 hardware layer, off the SH-2 software masked-column
fill path, with correct light, transparent gaps, and opaque blacks.

---

### Original analysis — two independent questions

- **DSP for sprite projection → NO (robust).** A billboarded Doom sprite needs *scalar*
  projection (screen x1/x2 + a depth-divide), **not** a matrix×vector transform — the quad
  corners fall straight out of `R_ProjectSprite`. The SCU-DSP's strength is MAC/matrix; its
  weakness is division (no hardware divide), and sprite projection is division-dominated.
  Add that projection is <0.4 ms total, the round-trip (stop/DMA/purge) exceeds the saving,
  and the D0 bus contends with CD-WAD streaming. The DSP is the wrong tool for *this*
  math — and it was never the prize anyway.
- **Sprites as VDP1 quads (FILL offload) → RE-OPENED, plausibly feasible.** The prize is
  moving the ~6 ms memory-bound masked FILL off the two SH-2 CPUs onto VDP1. My "dead"
  hinged on the layer inversion (VDP1 prio 5 *below* NBG1 prio 6). But **VDP1 sprite
  priority is per-command** (verified: Saturn sprite types 0–7 carry 2–3 priority bits that
  index the 8 `slPrioritySpr` registers, all currently pinned to 5). So the layer ordering
  can be challenged *without a global flip*: emit **things at priority 7 (above NBG1)**
  while **walls/floors stay at 5 (below)**. The reverted weapon only proves *prio-5-below*
  fails — the untried config is *prio-7-above*, and the infra to build it (texture cache,
  CRAM light banks, `FUNC_UserClip`, slave-emit) exists now and did not when the weapon was
  reverted. See "Deliverable 2 — corrected" below and `[[docs plan]]`.

---

## Deliverable 1 — Where does the sprite cost actually live? (measure first)

### The pipeline (core/r_things.c, shared verbatim with DoomJo)

Per frame the master SH-2 does three things:

1. **PROJECTION** — `R_ProjectSprite` ([r_things.c:479](../core/r_things.c#L479)) called
   per-thing from `R_AddSprites` ([:640](../core/r_things.c#L640)), which `R_Subsector`
   invokes **during the BSP walk** ([r_bsp.c:563](../core/r_bsp.c#L563)), interleaved with
   wall/seg processing. So sprite projection is folded into the serial **Bw** (BSP-walk)
   phase today — it has **no separate timer**.
2. **SORT** — `R_SortVisSprites` ([:833](../core/r_things.c#L833)), an O(n²) linked-list
   selection sort by ascending scale (back-to-front). `MAXVISSPRITES = 128`, overflow
   silently dropped.
3. **FILL** — `R_DrawMasked` ([:1208](../core/r_things.c#L1208)) walks the sorted list:
   `R_DrawSprite` (clip against every drawseg into `clipbot`/`cliptop`) →
   `R_DrawVisSprite` → `R_DrawMaskedColumn` → per-column `colfunc`. This is the **M**
   phase, and it is **already half-offloaded to the slave SH-2** ("masked-by-half",
   `sat_masked_parallel=1`, [main.cxx:73](../src/main.cxx#L73)): the slave draws the
   right-half vissprite columns while the master draws the left half.

### Projection arithmetic inventory (what a DSP could touch)

`R_ProjectSprite` per accepted sprite:

| Op | Count | SH-2 cost |
|----|------:|-----------|
| `FixedMul` (inline `DMULS.L`) | 6 | ~4 cyc each |
| `FixedDiv` (`DIV0U` HW, [m_fixed.h:56](../core/m_fixed.h#L56)) | 2 | ~37 cyc each |
| `R_PointToAngle` (SlopeDiv + table), **only if rotated** | 0–1 | ~50–100 cyc |
| pointer-chasing lookups (`sprites→spriteframes→lump`, `spriteoffset/width/topoffset[lump]`, `spritelights[index]`) | several | memory-bound |
| `vissprite_t` field stores | ~15 | memory-bound |

Roughly **5–12 µs per accepted sprite** (rejected sprites early-out much cheaper). The
transform half (tr_x/tr_y rotate → tz → xscale=projection/tz → tx → x1/x2 → iscale →
light index) is pure fixed-point a coprocessor *could* reproduce; the lump/flip/colormap
resolution is pointer-chasing it *cannot*.

**Crucially, the two divides (`xscale`, `iscale`) are the expensive part — and the DSP has
no hardware divide.** On the SH-2 each is ~37 cyc via `DIV0U`; on the DSP each is a ~30-
instruction software shift-subtract (proven by SlideHop's `hmap2.asm`, which ships exactly
such a routine). The DSP's MAC strength buys the cheap `FixedMul`s; the costly divides are
where the DSP is **worse** than the SH-2.

### Measurement build (ready to read)

Instrumentation added (FRT clock, 224 ticks/ms, DoomJo-safe via `RP_PROF` gating):

- **core/r_parallel.c** — `RP_SprProjEnter/Leave`, `RP_SprFillEnter/Leave`, `RP_SprReset`,
  `RP_SprStats` accumulators (after `RP_MakeSpansLeave`).
- **core/r_things.c** — reset in `R_ClearSprites`; projection bracket around the
  `R_AddSprites` thing-loop; fill bracket in `R_DrawVisSprite`.
- **src/dg_saturn.cxx** — overlay **row 15**: `SPR pj<ms> fl<ms> n<proj>/<draw>`.

**To read:** boot the `-Mus` build, overlay mode 0 (full), stand still in a monster-heavy
scene, read row 15. `pj` = projection ms (master, complete). `fl` = master-half fill ms
(≈½ of total; slave draws the other half untimed — or read the existing **M** row for the
full dual-split wall-clock). FRT-quantised, so `pj` jitters ±~0.1 ms.

### Expected result (from CRITICAL_PATH.md + the arithmetic above)

- **M (sprite fill) ≈ 6 ms**, already dual-SH2 split, and it does *not* collapse with
  potato mode (it's memory-bound pixel writes).
- **Projection ≈ 0.1–0.4 ms** for a monster scene (~20–40 things, many rejected).

So projection is **~2–7 % of the sprite fill, ~0.3 % of the ~130 ms frame.** Fill dominates
by 1–2 orders of magnitude — exactly the user's caveat. **Offloading projection alone
saves a fraction of a millisecond.** The live row-15 read pins the exact number; the
decision does not depend on it.

---

## Deliverable 2 — Sprites as VDP1 quads: **RE-OPENED** (was wrongly "dead")

> The material below documents the *layer-inversion* constraint accurately, but the
> original conclusion ("dead") over-generalised from *prio-5-below-NBG1* to *all* VDP1
> sprites. The corrected path — **things at prio 7 above NBG1** — is at the end of this
> section, followed by the prototype plan. Read the constraint, then the way around it.

### Corrected recipe candidate — things ABOVE NBG1

The blocker was: a VDP1 sprite at prio 5 (below NBG1) is hidden by the opaque software
scene, and there is no natural index-0 hole behind a mid-scene thing. **Fix: put things
*above* NBG1.**

- **Per-command priority.** Saturn sprite types 0–7 carry 2–3 priority-select bits (verified
  against libyaul `vdp2/sprite.h`: type 0 = `pr:2`, `cc:3`, `dc:11`). Those bits index the 8
  `slPrioritySpr` registers (Mimas pins all to 5 today, [dg_saturn.cxx:2451](../src/dg_saturn.cxx#L2451)).
  Mimas's wall/floor CRAM addresses (banks 0–7 × 256 = ≤ 2047 = **11 bits**) fit `dc:11`
  with the priority bits **clear** → register 0 → prio 5 (matches today). **Things set those
  top bits (via CMDCOLR) → a register pinned to 7 → above NBG1.** Same VDP1 layer, no global
  flip, walls/floors untouched.
- **Transparent gaps show the scene.** A thing above NBG1 with SPD-off (masked posts →
  transparent) lets the software floor/scene show through its gaps automatically — **no
  hole-punch**, which was the fatal cost of the prio-5 path.
- **Occlusion by nearer walls = clip, not depth.** `R_DrawSprite` already computes the
  per-column visible range (`clipbot`/`cliptop`). The common case (a vertical wall edge, or
  a floor/ceiling cut) is a **rectangle** → one `FUNC_UserClip` window. That mechanism
  **already ships** for walls ([dg_saturn.cxx:3224](../src/dg_saturn.cxx#L3224)) and floor
  tiles ([:4081](../src/dg_saturn.cxx#L4081)). The rare jagged case (thing behind a
  staircase silhouette) → a few UserClip strips, or route that one thing to the existing
  software fill — exactly the wall-clamp famine-routing pattern that already ships.
- **Painter order among things** = the existing back-to-front `R_SortVisSprites`.
- **Light** = the CRAM light-bank scheme (`wall_light_colr`) at ~0/quad; 7 levels may band
  vs the software 48 — refine with more banks if needed.
- **Texture** = reuse `sat_vdp1_wpn_draw`'s patch→cache→emit path, keyed by (lump, colormap),
  in a small LRU reclaimed from wall/floor VRAM.

**The prize:** the ~6 ms memory-bound masked FILL leaves the two SH-2 CPUs for VDP1 hardware,
freeing SH-2 critical-path time.

**Genuinely-open questions (prototype-and-measure, NOT armchair-kill):**
1. Exact SPCTL sprite-type + the priority-bit recipe for Mimas's 8bpp bank scheme (the
   "2-bank not 3-bank" moment — find it live on Ymir, like the wall/floor recipes).
2. **VDP1 rasterisation headroom** — VDP1 already ~overruns (Dr 25–42%). Do thing quads fit
   the end-of-frame window (walls already rasterised), or push walls to software fallback?
   The strongest risk; measurable via the existing Dr%/done-rate.
3. Jagged-clip fallback frequency (instrument it).
4. VRAM budget for the thing cache (~6 KB free; reclaim from wall/floor slots).

### Prototype plan

- **Increment 0 (decisive, cheap):** re-wire the existing dead weapon path
  (`sat_psprite_begin`/`sat_psprite_hook` → `sat_vdp1_wpn_draw`) and pin one priority
  register to 7 with a **live pad toggle sweeping which register**. On Ymir, sweep until the
  weapon renders *above* the floor without lifting the walls → proves the prio-7-above layer
  challenge on-screen, reusing 100% existing emit code. (The weapon is the ideal probe:
  always-on-top, 1–2/frame, the exact thing that was reverted.)
- **Increment 1:** route world things through the emit at prio 7, clipped by `FUNC_UserClip`
  to the `R_DrawSprite` rect; software fallback for jagged clips; measure VDP1 headroom + fps.
- **Increment 2:** thing-texture LRU cache, light-bank tuning, balance vs the walls/floors
  budget.

---

### (original constraint analysis — accurate, but see the corrected path above)

**Verdict was: "MORTE" — RETRACTED.** The layer-inversion facts below are correct; the
over-generalisation to all VDP1 sprites was the error.

### The layer inversion (the #1 blocker)

Mimas composites by pure VDP2 priority with **no per-pixel depth**:

```
NBG1 software framebuffer (256-col bitmap, index 0 = transparent)  priority 6   <- ON TOP
VDP1 (walls + textured floor tiles)                                priority 5
NBG0 sky                                                           priority 4 (3 w/ RBG0)
RBG0 rotated floor                                                 priority 3
```
([dg_saturn.cxx:2436](../src/dg_saturn.cxx#L2436), sprite priorities `:2438`.)

**Any opaque (non-index-0) NBG1 pixel unconditionally covers VDP1. VDP1 shows ONLY through
NBG1 index-0 holes.** This inversion is load-bearing: it is *why* VDP1 walls can coexist
with software sprites — the software wall draw is skipped (`sat_wall_skip=1`), leaving
index-0 holes for the VDP1 walls to show through, while the software framebuffer keeps
Doom's correct per-pixel occlusion for everything else.

A VDP1 sprite inherits an impossible bind:

- **At priority ≤ 5 (below NBG1):** occluded by every opaque software pixel — floors,
  ceilings, other walls, HUD. Mid-scene the region behind a thing is opaque floor/wall
  drawn by software, so there is **no natural index-0 hole** for the sprite to show
  through. To create one you must draw the sprite's clipped silhouette in software per
  column — i.e. do the exact memory-bound fill you were trying to eliminate. Self-defeating.
- **At priority ≥ 6 (above NBG1):** the sprite paints over the software scene with no depth
  test → a thing standing *behind* a wall shows *over* the wall. Wrong occlusion.

There is no priority that yields correct sprite-vs-world occlusion, because VDP1 has no
z-buffer and the software/VDP1 split has only two priority planes with nothing per-pixel
between them.

### No per-column depth clip

The software masked-column path exists *precisely because* sprites need per-column
clipping: `R_DrawSprite` computes `mfloorclip`/`mceilingclip` per screen-x from the
drawsegs' silhouettes ([:946](../core/r_things.c#L946)). A VDP1 quad is a rectangle; the
only per-command clip is `FUNC_UserClip`, a single screen-*rectangle*. It cannot follow the
jagged per-column occlusion boundary of a wall edge cutting across a sprite. So even with
priority tricks, a sprite straddling a wall edge cannot be occluded correctly.

Walls & floors are two separate **painter** passes (far→near wall emit, then floors chained
after), not a global depth sort ([dg_saturn.cxx:4396](../src/dg_saturn.cxx#L4396)). Adding
sprites = interleave by depth into the wall pass or a 3rd chained pass — both painter-only,
so a sprite partly behind a wall still can't be per-pixel occluded.

### The reverted weapon = proof of attempt

The project already built a VDP1 path for the **player weapon** — the *easiest* possible
sprite: always on top, 1–2 per frame, needs no wall occlusion. `sat_vdp1_wpn_draw` unpacks
the patch to an RGB555 VDP1 texture and emits one sprite command
([dg_saturn.cxx:4578](../src/dg_saturn.cxx#L4578)). It is **dead code today**:
`sat_psprite_hook`/`sat_psprite_begin` are **never assigned** anywhere in `src/` (they stay
NULL → the weapon draws in software), and the comment at
[dg_saturn.cxx:4517](../src/dg_saturn.cxx#L4517) states *"The weapon/HUD caches below are
dead (software now) — left harmless."* Its VRAM was reclaimed for the wall cache. Reason:
once VDP1 walls sat *below* NBG1 (the inversion), the weapon — which must sit *on top of
everything* — could no longer be a priority-5 VDP1 primitive. If VDP1 can't host the
always-on-top weapon, it certainly can't host world sprites that must be occluded by walls.

### VDP1 VRAM is full

512 KB VDP1 VRAM (`0x25C00000`–`0x25C80000`) is essentially full: command banks + 22 wall
texture slots (15×16 KB + 6×32 KB) + 7 floor slots + 2 F-banks. **Total free ≈ 5.9 KB.** A
sprite texture cache would have to evict live wall or floor slots.

**Verdict: MORTE**, for correctness (no per-column depth clip) compounded by the layer
inversion and full VRAM. Not "partial" — dead. The only architecture in which sprites-on-
VDP1 works is an all-VDP1 renderer (PowerSlave/Duke3D-Saturn style) that drops the software
framebuffer entirely — a different project, and one the existing "pre-tessellation levers
all DEAD" memory already rules out (residual cost is memory-bound generation, not the BSP
walk that a VDP1-world would replace).

---

## Deliverable 3 — DSP batch corner-projection design: **moot / net-loss**

Deliverable 3 presupposes deliverable 2 (VDP1 sprite quads to project corners for). Since #2
is dead, there are **no 4-corners-per-sprite to transform** — the design has no target.

For completeness, applying the DSP to the *software* projection instead is still a net loss:

- **No "in advance" window.** Projection needs *this* frame's `viewx/viewy/viewangle/
  viewcos/viewsin` (set at frame start in `R_SetupFrame`) **and** the visible set (which
  things are in visible subsectors — only known *during* the BSP walk). Both are per-frame,
  in-frame. There is no tic-boundary at which you can pre-project ahead of need; you'd have
  to serialize the currently-interleaved walk-and-project into walk-all-then-batch-project,
  and the SH-2 would then *wait* for the DSP mid-frame.
- **DSP round-trip > the saving.** Per SlideHop's SEGA driver, `DSP_WriteData`/`ReadData`
  both **stop the DSP first**; the program self-DMAs its data over the D0 bus, with cache
  purge required after (`0xFFFFFE92 |= 0x10`). For a ~0.1–0.4 ms projection budget, the
  stop/start + DMA-setup + cache-purge latency plausibly exceeds the work itself.
- **DSP has no hardware divide.** The two per-sprite divides become ~30-instruction software
  routines on the DSP — the SH-2 does them in ~37 cyc. The DSP loses on the expensive half.
- **D0-bus contention with CD.** DSP-DMA shares D0 arbitration with SCU-DMA and CD reads —
  and **Mimas streams the WAD from CD**. BLIT_DMA_PLAN also eyes the SCU-DMA level-0 channel
  for async blit. A DSP sprite batch would contend on the exact bus the streaming build
  needs.
- **Lookups stay on the SH-2.** Lump/flip/colormap-pointer resolution is pointer-chasing the
  DSP can't do; results must return to the SH-2 for the fill anyway.

---

## Deliverable 4 — Go / no-go, quantified

| Condition (user's gate) | Result |
|---|---|
| Projection is a *real* cost worth offloading | **NO** — ~0.1–0.4 ms (~0.3 % of frame), 1–2 orders below the ~6 ms fill. Measurement build ready (row 15) to confirm. |
| Sprite quads clip correctly on VDP1 | **NO** — no per-column depth clip; layer inversion; VRAM full; weapon already reverted. |
| DSP beats the SH-2 at the projection math | **NO** — no HW divide (the expensive half), round-trip latency > saving, D0-bus/CD contention. |

Both halves of the "projection is real **AND** quads clip" gate fail. **NO-GO.** Do not
build the DSP pipeline; do not move sprites to VDP1.

### What *is* the real sprite lever

The M phase (~6 ms) is already dual-SH2 split at a static `viewwidth/2` boundary. Its
documented open lever (`CRITICAL_PATH.md:99`) is **pixel-balance the masked split** — divide
the columns so each CPU draws equal *fill work* rather than equal *width* — for ~1–3 ms.
That is CPU-side, needs no DSP, no VDP1, and respects the existing per-column clip. This is
the sprite optimization to pursue if/when the sprite phase becomes the bottleneck (it is not
today — `P` floor fill and `Bp` wall-prep are the surviving levers).

---

## Appendix — reference material (for any future use)

### SCU-DSP plumbing (clean license)

- **libyaul `scu/scu_dsp.c` + `scu/dsp.h` — MIT** (Israel Jacquez). Portable minimal loader;
  lift this, *not* SlideHop's `DSP/DSP.C` (Copyright(c) 1994 SEGA, proprietary SGL sample).
- Ports: `PPAF=0x25FE0080` (ctrl/status), `PPD=0x25FE0084` (program), `PDA=0x25FE0088`
  (data-RAM addr), `PDD=0x25FE008C` (data). Control bits: LOAD=15, EX=16, STEP=17, END=18,
  OVERFLOW=19, DMA_BUSY(T0)=23. Program RAM 256 words; data RAM 4 banks × 64 words.
- **No hardware divide** (software shift-subtract, ~30 instr). VLIW: 4 slots (ALU | X | Y |
  D1); `mov mul,p` + `ad2` for MAC. `jmp COND,label` has a mandatory delay slot; subroutine
  = `mvi 1,LOP` + `mvi LABEL,PC`, return `BTM`.
- Errata: CPU touches data RAM only while stopped; multiply pipeline latency; D1-bus→RX/PL
  constants "sometimes non-functional on real HW"; UNCACHE + cache-purge for DSP-DMA'd data;
  end flag self-clears on read.
- Only SlideHop (GPLv3) does vertex-domain DSP work: `winder.asm` (245 words, per-vertex 2D
  chirality/portal clip — cross products, MUL, **no divide**) is the closest analogue, and
  it is a *clip test*, not a projection. Azel's decomp carried SCU-DSP infra but left it
  unused.

### Confirmed context

- SCU-DSP is genuinely idle every frame (SGL does all matrix math on the SH-2).
- The slave SH-2 idles the whole B phase, but that idle = the memory-bound BSP walk a
  cache-cold bus-shared slave can't accelerate — not a projection-offload window.
- This is the "sprites parked to dedicated session" follow-up from the SCU-DSP sight/tic
  verdict. Result: **settled dead.**
