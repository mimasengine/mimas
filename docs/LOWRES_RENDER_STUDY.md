# Low-resolution 3D-view mode — feasibility study

*Investigation 2026-07-14 (5-agent render-pipeline map + user design refinement). Goal:
cut the per-frame FILL cost — the scene-independent floor that makes even empty big-WAD
scenes run at 5-7 fps — by rendering the software framebuffer at half horizontal
resolution and letting VDP2 hardware-enlarge it ×2 to full screen.*

## Why (the need)

Big-WAD scenes are **fill-bound + AI-bound**, not complexity-bound
([[big-wad-perf-compute-bound]]). Even a bare corridor pays ~75 ms of framebuffer fill
(`Bp`+`P`+`M`) + ~10 ms blit every frame because the software renderer paints **every
screen pixel every frame** regardless of what is visible. Halving the rendered pixels
halves that floor — and, via the tic-catch-up virtuous cycle (faster render → fewer 35 Hz
catch-up tics → lower `T`), buys a little more.

`detailshift` (FastDoom potato, already present) is **the wrong tool**: it halves
`viewwidth` to 160 logical columns but `R_DrawColumnLow` **duplicates** each column back
into the full 320-wide framebuffer ([core/r_draw.c:244-261](../core/r_draw.c#L244)). So the
framebuffer stays 320-wide, fill and blit are **not** saved — only per-column setup + texel
sampling. We want the framebuffer physically **packed to 160** + a hardware ×2 zoom.

## The design: HYBRID (not M0-gated) — the key insight

Naively this looks M0-only, because zooming NBG1 seems to desync the hybrid layers
(VDP1 walls/weapon/things + RBG0 floor + NBG0 sky draw in their own 1:1 320-space).
**But it aligns — because the software is rendered at 160 and zoomed back to 320, not
rendered at 320 and zoomed.**

- VDP1 wall: BSP computes screen-x in 160-space (`viewwidth=160`), emits `x << detailshift`
  = **320-space** ([src/dg_saturn.cxx:3535](../src/dg_saturn.cxx#L3535)).
- Software ceiling/floor: rendered at 160-space (edge at `x=100`), NBG1 zoomed ×2 → **200**.
- Both derive from the same world: `2·x_soft = x_vdp1`. **Aligned.** (Same proof as detailshift,
  which already works in M4 — render 160, display 320. We swap software-doubling for
  hardware-doubling to reclaim the fill/blit.)
- RBG0 floor: reader-4 confirmed the 1p transform (center `160,112`, default focal) **already
  matches** a ×2-horizontal / ×1-vertical zoom of a 160 render — the zoom cancels the
  half-width render and restores the full-320 FOV it is calibrated to. Likely **no RBG0
  rescale** (HW-verify half-texel jitter).

**So DON'T gate to M0. Run it in M4.** Result — the important elements stay full 320 HW,
only the secondary software leftovers go chunky-160:

| Full 320 (hardware, crisp) | Chunky 160 (software) |
|---|---|
| **Walls** (VDP1) | Ceilings |
| **Dominant floor** (RBG0) | Minor / non-dominant floors |
| **World sprites** (VDP1) | Near-wall CPU fallbacks |
| **Weapon** (VDP1, `SAT_WPN_VDP1=1` **default ON**) | — |

The fill it halves — the software plane fill `P` (ceilings + minor floors) — **is exactly the
SQ-invariant cost that dominates Plutonia map15** ([[big-wad-perf-compute-bound]]). Perfect target.

*(Correction: the weapon is on VDP1 by default in M4 — [dg_saturn.cxx:2855](../src/dg_saturn.cxx#L2855),
[:738](../src/dg_saturn.cxx#L738) "M4 = RBG0 + VDP1 walls + VDP1 weapon + VDP1 world things".
It stays crisp with no extra work. Earlier "weapon is software / gated off" was wrong.)*

## The mechanism exists and is already scaffolded

VDP2 enlarges NBG0/NBG1 (only) via the coordinate-increment registers **ZMXN1/ZMYN1**
(increment `0.5` ⇒ each source dot spans 2 screen dots = ×2). SGL: `slScrScaleNbg1(x,y)` /
`slZoomNbg1`. **A `VDP2_ZOOM_TEST` scaffold already exists**
([src/dg_saturn.cxx:70-80](../src/dg_saturn.cxx#L70), [:2803-2810](../src/dg_saturn.cxx#L2803)) —
the author planned this. Enlargement **lowers** the VDP2 fetch rate → bandwidth-friendly, no
snow ([[vdp2-floor-snows-on-hardware]] does not apply).

⚠️ **Verify on Ymir/HW first:** whether the SGL call wants the increment (`0.5`) or a
magnification (`2.0`). The scaffold passes `2.0`; the raw register is the increment. Flip
`VDP2_ZOOM_FACTOR` if the image shrinks. 5-minute check.

## The core is already parameterized

`R_ExecuteSetViewSize` derives `viewwidth`/`centerx`/`yslope`/`distscale`/`pspritescale`/
`ylookup` from runtime `setblocks`/`detailshift`; `R_SetViewWindow(wx,wy,w,h)` even renders an
arbitrary viewport. The fixed arrays (`columnofs[MAXWIDTH=1120]`, `ylookup[MAXHEIGHT=832]`)
are ≥ 320/224, so a 160-wide view uses a **prefix** — no resize.

**The one coupling to break:** `detailshift=1` gives us `viewwidth=160` (good) AND the VDP1
`x<<1` doubling (good) BUT swaps to the `*Low` drawers that re-duplicate into 320 (bad). We
want `viewwidth=160` + VDP1-double + **normal** (packed) drawers. So `sat_lowres` forces
`detailshift=1` but keeps the **normal** `colfunc`/`spanfunc` (`R_DrawColumn`/`R_DrawSpan`),
which write one byte per logical column 0..159 = packed-160 automatically. Small, platform-gated
core edit ([core/r_main.c:701-723](../core/r_main.c#L701)); DoomJo unaffected (flag default 0).

## The HUD wrinkle

The 32px status bar (rows 192-223) + `SRL::Debug` share NBG1; a whole-layer ×2 zoom stretches
them too, and the STBAR is a fixed 320px graphic — a ×2 zoom of full-320 HUD content would blow
to 640 and clip. So the HUD content must live in columns 0-159 to zoom to 0-319.

- **Phase 1 (measurement):** horizontally **decimate** the HUD rows (192-223) 2:1 into columns
  0-159 right before the blit (a ~32-row platform pass). Chunky HUD — which is exactly what lets
  us judge "la qualité, surtout la tête" and decide migration. Disable W5 (always blit HUD) in
  this mode for simplicity. **SHIPPED** (M7 lowres mode, pad-Z cycle).
- **Phase 2 (SHIPPED):** the software HUD is chunky under the zoom, so both HUD text layers moved
  to the **VDP1 prio-7 sprite layer** (immune to the NBG1 zoom — VDP1 has its own framebuffer):
  - **Status bar** (`vdp1_hud_capture`/`vdp1_hud_emit`): 8bpp normal sprite at `0x25C78000`
    (10KB), captured from framebuffer rows 192-223, displayed through CRAM bank 1 (= `colors[]`,
    the same palette NBG1 uses) → pixel-identical to the software bar but un-zoomed, and sits ON
    TOP of the weapon (fixes the bob poking over the HUD). **Always-on in 1p** (all modes), so
    non-lowres gains the weapon-poke fix too. **Additive**: the software bar is still blitted
    underneath as a never-vanish fallback, covered by the opaque sprite.
  - **HU message line** (`vdp1_hud_msg_emit`): SPD-OFF 8bpp sprite (index-0 transparent → only the
    glyphs draw over the view). The core (`hu_stuff.c` `HU_Drawer`) redirects `w_message` via
    `V_UseBuffer(sat_hu_msg_buf)` to draw its glyphs **directly into a VDP1 VRAM slot** (byte writes,
    proven by the 8bpp weapon bake; SH-2 cache is write-through). Drawing into VRAM instead of an
    HWRAM scratch keeps the **boot-loop pool** healthy — a 320×16 `.bss` scratch drove `_end` past
    the SGL work area (**negative pool**). **Double-buffered** by frame parity (two 320×8 slots at
    `0x25C7A800`/`0x25C7B200`) so the plotted slot is never the one being redrawn → tear-free. The
    core sets `sat_hu_msg_drawn` (= `message_on`) to gate the emit. **Lowres only** (non-lowres
    byte-identical). Automap title/chat stay software (rare in 1p).
  - Both are **1-frame latent**: the VDP1 list is closed by `sat_walls_kick` during
    `R_RenderPlayerView`, *before* `ST_Drawer`/`HU_Drawer` compose the HUD; so `DG_DrawFrame`
    captures frame N and the next frame's kick emits it. Imperceptible for slow-changing HUD.
  - **VRAM**: status bar (10KB) + two message slots (2×2.5KB) live in the `0x25C78000..0x25C7C000`
    gap, clear of the 28KB things pool (ends `0x25C78000`) and the M4/M7-off F-banks (`0x25C7C000`)
    → safe in every reachable mode. **`.bss`**: ~0 (both compose in VRAM), so the boot-loop pool
    stays at its M7 baseline (~4.6KB, ≥ the 4KB floor). Checked in `Mimas.map` (`_end..__heap_end`).
- HUD info affected (all software → all chunky in Phase 1): ammo/health/armor digits (survive —
  big), arms, keys, **the face (worst-degraded)**, top-of-screen messages (HU), automap. In
  Phase 2 the status bar (all of it, incl. the face) + the HU message line are crisp again.

## What we gain / lose (M4-hybrid, horizontal ×2)

**Gain:** software fill (`P` + wall-fallback) ~÷2; blit ÷2 (~10→5 ms); fewer catch-up tics →
`T` drops a little. Estimate (to be measured): corridor ~142→~95-100 ms (**7→~10-11 fps**);
worst open Plutonia (#6, 1.6 fps, ~490 ms fill) → ~375 ms (**1.6→~2.7 fps**), bigger where fill
dominates.

**Lose:** software leftovers (ceilings/minor-floors/fallbacks) horizontally chunky — but **walls,
main floor, sprites, weapon stay full 320**. HUD chunky until Phase 2. Vertical stays full (H-only
zoom). Possible ±1-2px seam at software/VDP1 boundaries + RBG0 half-texel jitter (HW-verify).

## Fit with the current config

Clean, **platform-gated** (default build + DoomJo byte-identical):
- `sat_apply_mode()` single writer → add a `sat_lowres` axis, **allowed in M4** (not gated to M0).
- Core: `sat_lowres` in `R_ExecuteSetViewSize` (detailshift=1 + normal drawers). Flag default 0.
- Platform: `slScrScaleNbg1` H×2 (existing scaffold), blit 160 B/row, HUD 2:1 decimate, live pad
  chord + a char on overlay row 7.

## Feasibility

- **Phase 1 (M4-hybrid, H-only 160):** MODERATE. Reuses the zoom scaffold + the existing
  detailshift projection/VDP1-doubling; the only new core code is the packed-drawer select; HUD
  accepted chunky.
- **Phase 2:** HUD → VDP1; HW-verify RBG0 jitter.
- **Phase 3 (vertical halving, 4× fill):** HARD — Doom has no native vertical decimation. Later, only if needed.

## Plan

1. **Phase 1 — measurement MVP** (this pass): M4 + `sat_lowres` (viewwidth 160 packed) + VDP2 H×2
   zoom + 160-byte blit + HUD 2:1 decimate + pad toggle + overlay char. **Measure** the fps win on
   the bad Plutonia/Doom2 map15 scenes and judge the chunky-HUD/face quality.
2. **Phase 2** (if the win is real): HUD on VDP1; RBG0 jitter HW-check; consider the ZMXN 0.5-vs-2.0
   convention finalized.
3. **Phase 3** (only if needed): vertical halving.

## Key references

- zoom scaffold: [dg_saturn.cxx:70-80](../src/dg_saturn.cxx#L70), [:2803-2810](../src/dg_saturn.cxx#L2803)
- framebuffer + blit: [:907](../src/dg_saturn.cxx#L907) (`framebuffer[320*224]`),
  [:6256-6297](../src/dg_saturn.cxx#L6256) (per-row 320B blit, W5, hud_top), [:6310-6327](../src/dg_saturn.cxx#L6310) (clear)
- VDP1 wall x-double: [dg_saturn.cxx:3535](../src/dg_saturn.cxx#L3535) (`x<<detailshift + vx`)
- RBG0 transform: [dg_saturn.cxx:2040](../src/dg_saturn.cxx#L2040) (center 160,112 + default focal)
- weapon VDP1 default-on: [dg_saturn.cxx:2854-2855](../src/dg_saturn.cxx#L2854), [:738](../src/dg_saturn.cxx#L738)
- mode/SQ single writer: [dg_saturn.cxx:823-883](../src/dg_saturn.cxx#L823)
- view sizing + drawer select: [core/r_main.c:678-769](../core/r_main.c#L678), [:701-723](../core/r_main.c#L701)
- SCREENWIDTH/HEIGHT compile-time: [core/i_video.h:27-28](../core/i_video.h#L27)
- VDP2 zoom reference: `saturn-refs/Azel/AzelLib/VDP2.h:89-105`, `VDP2.cpp:1120-1140`
