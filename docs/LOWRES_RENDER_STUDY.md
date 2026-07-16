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

---

## M7-MULTI — low-res in 2-player split (2026-07-15, mapped + adversarially reviewed)

**Status: 2p SHIPPED (uncommitted, HW-PENDING).** Builds clean SH-2, TLSF pool 4672 B ≥ 4096.
The "out of scope" note above (Phase 3) was wrong to dismiss split — the whole-layer zoom is
**geometrically compatible** with the split because the boundaries fall clean under ×2.

### The insight
The single whole-layer `ZMXN1=0.5` zoom maps source col `c → screen 2c` for the ENTIRE NBG1
layer. So if every viewport renders at HALF its width **packed into source cols [0,160)**, the
same one zoom restores all of them: 2p → P1 `fb[0,80)→scr[0,160)`, P2 `fb[80,160)→scr[160,320)`.
Boundaries (screen 160 ← fb 80) are even → exact seam, no half-pixel gap.

### Implementation (each viewport renders viewwidth=80 packed)
- **`R_SetViewWindow`** ([r_main.c](../core/r_main.c)): drawer-select **hoisted out of the
  (w,h,detailshift) size-cache** and keyed `!detailshift || sat_lowres` → packed `R_DrawColumn`;
  `columnofs` base = `sat_lowres ? (wx>>1) : wx`. **`viewwindowx` stays = wx** — the un-zoomed VDP1
  walls/weapon/sprites read `(x<<detailshift)+viewwindowx` and must land at full-320 screen coords.
  This **dual-origin split** (packed sw base `wx>>1` vs screen origin `wx`) is the crux.
- **`d_main.c`**: `detailshift = sat_lowres ? 1 : sat_split_lowdetail` (forces the 80-col projection).
- **`r_plane.c`** sky: `detailshift && !sat_lowres` routes lowres to the PACKED `R_DrawSkyColumn`.
- **Platform** ([dg_saturn.cxx](../src/dg_saturn.cxx)): `want_lr`/`sat_lr` gates `<=1 → <=2`; the
  2p HUD panels are drawn full-320 unchanged and **2:1-decimated by the existing blit**; the
  per-viewport damage flash (`hud2p_apply_flash`) is packed-aware (view rows washed at 80, HUD rows
  at 160). **No change** to: RBG0 P1 floor (separate un-zoomed layer, screen[0,160)), masked-split
  (stays OFF under lowres), weapon scale (`(pspritescale<<detailshift)<<1(split) = FRACUNIT` in both).

### Fixed during adversarial review (4-lens workflow)
- **Drop-in stale `sat_lowres`** (confirmed bug, 3/4 lenses): a live 2p→3p/4p drop-in changed
  `sat_local_players` without re-running `sat_apply_mode` (its only `R_SetLowRes` caller) → 3/4p
  packed but un-zoomed = garbled. Fix = a count-change reconciler in `poll_pad` re-runs
  `sat_apply_mode()` (the internal `want_lr != sat_lowres` guard absorbs the no-op).
- **Automap** squashed under M7 → added `&& !automapactive` to `sat_lr` (also fixes pre-existing 1p M7).
- **DMA blit** path had no lowres decimation → gated `dma && !sat_lr` (falls to the CPU path under M7).

### 2p CRISP HUD ON VDP1 — SHIPPED 2026-07-15 (fixes the "2p HUD moche" problem)
The 2p compact-HUD panels were 2:1-decimated (chunky) under the zoom. Now **captured + emitted as a
prio-7 VDP1 sprite** (un-zoomed → crisp), exactly like the 1p status bar. `vdp1_hud_capture(hy,hh)`
generalized: 1p bar = rows 192..223 (320×32=10KB); 2p M7 = rows 160..223 (320×64=**20KB**) at
`VDP1_HUD_TEX` (0x25C78000). VRAM safe in every reachable mode: things end exactly at 0x25C78000, and
the overlapped ftex flat-slots (0x25C78000+) + F-bank head (0x25C7C000+) are **never written** when
`sat_vdp1_floor=0` (all of {M0,M4,M6,M7}). The software decimated panels stay underneath as a
never-vanish fallback (opaque sprite covers them). Emit gate = `vdp1_hud_ready` (set by the capture
site for 1p / 2p-M7; cleared for 3/4p / non-M7 2p). Also fixed a **pre-existing W5 bug**: the 2p panel
skipped its repaint after a 1p↔2p↔3/4p count cycle (signature unchanged but framebuffer clobbered) →
now force-repaints on a count change.

### 3/4p M7 — SHIPPED 2026-07-15 (VDP1 HUD generalized to N regions)
`want_lr`/`sat_lr` relaxed to all counts (want_lr is now count-independent = `M==M7_LOWRES`, so the
2p drop-in stale-lowres bug is MOOT). View packing was already correct (R_SetViewWindow packs each
quadrant at `wx>>1`; y untouched). The VDP1 HUD was **generalized to N regions** (`vdp1_hud_begin/add/
emit`, `vdp1_hud_region[2]`): 1p bar = 1 region, 2p panels = 1, **3/4p bands = 2 strips** (rows 96-111
+ 208-223, 320×16, stacked in VDP1_HUD_TEX). `hud4p_apply_flash` made packed-aware (view rows packed
80, band rows full-320 for capture). The **3p minimap packs** into the bottom-right quad's left half
(`AM_DrawMiniMap(80,·,80,·)`; am_map.c allocs the scratch at max width + rebuilds on a `w` change).
RBG0 is off in 3/4p (software floor packs+zooms). Pool reclaimed via `HEAP_SIZE 24→20KB`.

**Adversarial review (3 lenses) — bugs caught + fixed:** (1) the capture predicates missed the
`usergame` guard → on the attract/demo loop after a co-op game they'd snapshot demo *view* rows as
garbage HUD → added `&& usergame`. (2) the 3/4p band strips were SPD-off → in 3p the empty minimap-slot
corner revealed x2-zoomed band garble → switched to **SPD-ON** (black covers it in 3p; no-op in 4p; also
closes any index-0-hole). Plus: reset HUD regions on palette change, hoisted the W5 count-tracker to
frame level (full 2p↔1p), gated the stale 1p HU-message on count==1, and **gated the split HW-sky off
under M7** (NBG0 un-zoomed vs NBG1 x2-zoomed → misalign; software sky packs+zooms instead).

**HW-verify (3/4p):** dense-4p VDP1 command budget (if exhausted, band sprites drop → the packed
software band garbles, not gracefully — graceful otherwise); band art has no unintended index-0 pixels
(SPD-ON now paints them near-black); `HEAP_SIZE 20KB` peak on a big-WAD run (row-22 `hp` < 20K).

### ⚠️ M7 IS A POOR 3/4p CANDIDATE — measured ~0 fps gain (2026-07-15)

HW captures, same scene, cycling M4↔M7 (pad Z): **3p 6.7→6.8 fps (MST 149→147), 4p 5.5→5.5 fps
(MST 181→181)** — essentially neutral, versus the real ~+8-18% M7 buys in 2p. **Keep the mode
(it renders correctly), but it is NOT the 3/4p perf lever.**

**CONFIRMED by the row-17 SPL probe (2026-07-15, controlled A/B, IDENTICAL position `m1 1104,-3600
a64`, 4p):** M4 `SPL 36 33 38 33 k13 =140`, R153, MST178 → M7 `SPL 34 33 37 33 k14 =137`, R153,
MST178. Halving each quad's `viewwidth` to 80 moves the per-view render by ~1ms (35→34). The phase
split shows work SHIFTS, not shrinks: `Bp` (walls) 16→13 but `P` (floors) **9→12** — net flat.
Mechanism: in the SHORT 96-row quadrants both dominant phases are **per-seg / per-row setup-bound,
not per-pixel**: `Bp` = wall-range calc + VDP1 quad emit (per-seg); `P` = `R_MapPlane` per-row
perspective+light (96 rows/quad, width-independent). Low-res halves only the per-pixel inner loops,
already a minority. The ONLY genuine M7 4p win is the blit `b 5.5→4.3` (160-byte copy) = 1.2ms of 178.
Render is **86% of the 4p frame** (140 views + 13 kick) and fixed-cost-per-view-bound; `T`(tic)=15ms
is not the problem.

**Why (code-proven, not fill-bound in 3/4p):**
- **M7 halves ONLY the software pixel-fill + the blit.** `R_SetViewWindow` genuinely sets
  `viewwidth = w>>detailshift = 80` per quad (r_main.c:799), the packed drawers write 80 B/row,
  and the blit copies 160 B/row (half). Both proven from code.
- **RBG0 is OFF in 3/4p for BOTH M4 and M7** (`rbg0_on` needs `sat_local_players<=1` OR `==2`,
  dg_saturn.cxx:6137-6144). So M7 does *not* lose a hardware floor vs M4 — the only difference
  between the two in 3/4p is the lowres packing itself. That isolates the cause to the fill.
- **The 3/4p bottleneck is per-view FIXED cost paid 4×, which lowres cannot touch:** each of the 4
  sequential `R_RenderPlayerView` calls (d_main.c:406-420) pays BSP traversal + sprite/seg
  projection + **VDP1 command emission** (walls+things+weapon), none of which scales with
  `viewwidth`. The captures show `VD1 w102%` = the VDP1 command list is *over budget* in dense
  4p; the master waits on that full-res hardware fill, which M7 does not reduce.
- **Quadrants are SHORT (96 rows).** Pixel-fill ∝ area, so per-quad fill is already a *minority*
  of each `v_i`; halving a minority ≈ noise. 2p halves are 160 rows tall → fill dominates → M7 pays.
- The `Bw/Bp/P/M` "swap" some captures show (P *rose* under M7) is **not real**: those rows are
  *worst-frame snapshots that persist until beaten or a config change* (dg_saturn.cxx:1825), and a
  mode cycle IS a config change → M4 and M7 snapshot **different** worst-frames. Only **MST** (and
  now the row-17 `SPL` per-view times) is a valid cross-mode number.

**New probe (row 17 `SPL v0 v1 v2 v3 kN =S`, dg_saturn.cxx ~1732):** the per-view
`R_RenderPlayerView` ms (`sat_spl_v0..3`) + the single VDP1 kick — previously computed but not shown.
Cycle M4↔M7 on the same 4p scene: if `v_i` is ~flat → 3/4p is **emission/BSP-bound** (lowres is
structurally useless there); if `k` dominates → **VDP1-fill-bound**. Either way the 3/4p lever is
**cutting per-view VDP1 command/fill or geometry** (SQ-per-view, thing cull, fewer wall quads), NOT
lowres. **M7 config verdict: 1p ✅ / 2p ✅ / 3p ⚠️neutral / 4p ⚠️neutral.**

### THE ACTUAL 3/4p LEVERS — split-context SQ + VDP1 budget (2026-07-15, uncommitted)

Following the row-17 `SPL` finding (render = 86% of the 4p frame, fixed-cost-per-view-bound), two
levers were implemented to attack the per-view cost directly (NOT lowres):

**1. Split-context SQ per view** (`src/dg_saturn.cxx` + `core/d_main.c`). The co-op split gets its
OWN wall/floor/ceil software quality (`sq_wall_view[4]`/`sq_floor_view[4]`/`sq_ceil_view[4]`),
independent of 1p, applied per view via `sat_view_sq_apply(i)` before each `R_RenderPlayerView` and
restored after the kick. Defaults = the 1p defaults → byte-identical until toggled. Live: in a split,
R+Y (wall FULL→BAND→FLAT), Y (floor FULL→LD→FLAT), L+Y (ceil) drive the split set (uniformly across
views); in 1p they drive the globals as before. Overlay row 7 shows the split set when split.
- **Lets you brade the split walls/floors to cut `Bp`+`P` WITHOUT degrading 1p** — the config-per-count
  tool. Wall BAND/FLAT (`sat_wall_nocpu`/`wall_potato_mode`) cuts wall CPU + VDP1 tiles/bake; floor
  FLAT (`sat_potato_floors`) skips `R_MapPlane` per-row = the direct `P` cut.
- **CONSTRAINT:** split walls exclude SQ_LD (it drives the whole-frame `detailshift`, set once before
  the loop — cannot vary per view). Floor/ceil LD are per-view-safe.
- **Architecture note:** the arrays are per-VIEW (asymmetry-ready) but the live control writes them
  uniformly. True per-viewport asymmetry of the WALL STYLE needs `wall_potato()` made view-aware
  (it returns the global, read at flush — see the trap comment there); the per-view Bp/P flags
  (`sat_wall_nocpu` etc.) already differ per view correctly.

**2. VDP1 command-budget reservation** (`src/dg_saturn.cxx`, `sat_walls_kick`). `vdp1_wall_cap`
(runtime) caps the walls short of `VDP1_CMD_GUARD` by the overlays emitted after them — weapon copy-2
(≤3 cmds/view) + HUD strip(s) + HU msg + margin: `reserve = 3·nv + nhud + 3`, `nhud = (nv≥3)?2:1`.
1p unchanged (nominal `WALL_CMD_CAP`=248). **2p included** (cap 244) — its M7 VDP1 HUD needs 7 overlay
slots > the old 6-slot margin, so a dense 2p-both-firing frame dropped the panel (same bug class as
3/4p; caught by review). The farthest walls yield instead (already the overflow tail). This is what
**fixes the doubled/dropped HUD band** (`VD1 w102%`).

**Adversarial review (4 lenses, 2026-07-15):** no crash/corruption; timing verified (kick before
restore, uniform SQ). Fixed: the 2p-M7 HUD-drop worst case (reservation extended to 2p); the cap
floor made coherent (`WALL_ACC_MAX+16`, was 64 → far walls could vanish); the `wall_potato()`
asymmetry trap documented. **Deferred (flagged, not bugs):** (a) 2p SQ no longer inherits a 1p SQ
toggle — INTENDED (decoupled configs); (b) split `detailshift` still inherits 1p `sq_wall==LD` (set 1p
wall LD then split → low-detail projection, no in-split clear) — matches the "dedicated
split-detailshift control is a follow-up" note; (c) in the attract loop with leftover co-op count
(players>1 && !usergame) the row-7 SQ overlay/taps show the split set while 1p renders — cosmetic.

**HW-verify:** in 4p M7, cycle split walls→BAND + floors→FLAT and watch `Bp`/`P`/`SPL` fall; confirm
the HUD band no longer doubles (VDP1 budget); confirm 1p and default-split render unchanged.

### MORE 2/3/4p LEVERS (2026-07-15, uncommitted) — pistes from the SLV-idle / 86%-render analysis

Framing (from the row-17 `SPL` A/B): 4p is **86% render** (140ms views + 13ms kick), and the debug
shows **`SLV b3% id97%`** — the slave SH-2 is ~97% IDLE in split. So the game is cutting the 4
sequential per-view renders. Levers implemented as toggles:

- **Piste 3 — split thing cull** (`sat_split_thingcull`, pad **L+Left**, split-only, default off;
  core/r_things.c). In `R_ProjectSprite`, after `xscale`, drop sprites that project to near-nothing
  (cut the per-view vissprite alloc + sort + emit × N views). Two buckets: SHOOTABLE actors **AND
  MISSILES** use the conservative near-1px `ACTOR` floor (a split player must see incoming shots —
  review fix); decorations + pickups use the harsher `DECOR` floor (~5px of a 64px sprite). `xscale`
  is a depth proxy so big bosses subtend ~2× the quoted px. Overlay row 17 `tc`.
- **Piste 5 — rotating SQ balance** (`sat_split_balance`, pad **L+Right**, cycle 0/1/2, split-only;
  sat_view_sq_apply). Spread the split's degraded SQ over views+frames so no view stays permanently
  ugly. `1` = 1 view degraded/frame (rotating; 2p = each half every other frame) → min fps hit / best
  quality; `2` = 2 views/frame. "Degraded" = the cheap split SQ (`sq_*_view`, which you set); "full"
  = FULL/LD preset. Each view degraded a fair fraction (rotation verified; no starvation). Overlay
  row 17 `bal`.
- **Per-view wall STYLE capture** (`wall_acc[].pot`) — required by piste 5: the VDP1 wall style is now
  captured per view at accumulate (was read globally at flush = only the last view's). Removes the
  documented asymmetry trap; byte-identical for uniform/1p.

**Piste 1 — segloop-micro: INVESTIGATED, NOT implemented.** `R_RenderSegLoop` is already SATURN-
optimized (texture/light/`dc_iscale` divide skipped when VDP1 owns every tier); the residual per-
column cost is inherent `byte` writes to visplane spans + clip arrays. A base-pointer hoist is <2% on
byte arrays and touches the core clip logic — poor risk/reward. (The "only profiled" note in the
slave study overstated the opportunity.)

**Piste 2 — wake the idle slave: NOT a toggle.** The slave is 97% idle in split *because* M4/M7
already offloaded walls→VDP1, floors→RBG0/small-software, sprites→VDP1 — little parallelizable
*software* work remains. The dominant master cost (BSP + seg processing + VDP1 command build) needs
renderer state; a 2nd copy overflows 2MB (dead ×3, memory-bound — [[wall-offload-vdp1-slave-dead]],
[[slave-second-renderer-bp-study]]). The one faint angle (view-pipelining with quadrant-sized state)
is walled by the statically SCREENWIDTH-sized visplane pools — a large re-architecture, not a lever.
**Recommend: realize pistes 3+5 first; revisit slave only as a dedicated project.**

**Adversarial review (3 lenses):** no corruption; balance rotation verified fair. Fixed: piste-3
missile bucket (→ ACTOR threshold), overlay-row comment (17 not 7). HW-verify: 4p thingcull on →
`M`/`SPR n` (row 15) fall, no visible monster/missile pops; balance 1/2 → `SPL`/fps trade vs quality.

### KNOWN LIMITATIONS / Phase 2 (deferred)
1. **HU pickup-message in 2p M7** — the VDP1 message-redirect is still 1p-only; in 2p the software
   message draws full-320 into the packed view and is x2-stretched. Transient/cosmetic (on pickup).
   Fix = extend the message-on-VDP1 redirect to 2p (the HUD-sprite infra now exists to build on).
3. **`r_parallel.c` parity + slave-sprite `if(detailshift)` guards** lack `&& !sat_lowres`
   (rp_exec:496, rp_exec_fuzz:361, RP_Record* fallbacks 1715/1730/1740/1748; slave drawers
   r_things 1261/1286/1308/1429). **DEAD in the ship config** (`sat_plane_parallel=1` forces
   `rp_disabled=1`; masked-split gated `!sat_lowres`). Coherence trap only if a dev re-enables the
   parity slave under M7 — would also break 1p M7. Left as documented constraint, not edited (dead code).
4. **Menu / PAUSE over 2p M7** cosmetically squished behind the overlay (pre-existing 1p M7 tradeoff).
5. **RBG0 P1 seam** (2p M7): sound by reasoning (screen-space-baked focal is FOV-invariant to the
   packing) but **HW-verify** the P1 floor/wall registration at the split boundary.

### HW validation checklist (Ymir / real Saturn)
- 2p M7: both halves fill the screen (not squished into the left); P1|P2 seam at screen 160 clean.
- P1 RBG0 floor aligns with P1's zoomed walls at the seam (limitation #5).
- Damage flash tints the correct player's half (view + HUD).
- Drop a 3rd player in during 2p M7 → 3/4p goes full-res correctly (reconciler), then cycle back.
- Measure the fps delta on a fill-bound 2p scene (expected ~+40-70% where fill dominates).
