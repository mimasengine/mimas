# Level Start/End Transitions (Fades) — Design Note

Status: **SHIPPED — software CRAM palette fade (Option 1). core 4f06d65 (`d_main.c` hooks) + port a693a4e (`DG_FadeOut`/`DG_FadeIn` in `dg_saturn.cxx`).** The fade is live in the ship build: `DG_FadeOut`/`DG_FadeIn` (`src/dg_saturn.cxx:1561-1570`) drive `dg_fade_bake` (`:1522`, `FADE_STEPS=16` at `:1520`), ramping `pending_cram` (+ `pending_wbank`) for the vblank handler to copy to CRAM; hooked in `core/d_main.c` (`DG_FadeOut` :265 leaving-level, `DG_FadeIn` :437-441 after `I_FinishUpdate`). The other options below are kept only as a historical rejected-alternatives note. (`slSynch` was abandoned project-wide; the direct-CRAM vblank path is the proven mechanism — see `docs/VDP1_PRESENT_SYNC_PLAN.md` for the present model.)
The vanilla screen-melt (`f_wipe`) is disabled in CD-streaming mode because it
crashes (see *Why the melt is gone*); it's now replaced by the **software CRAM
palette dip-to-black** (option 1): `DG_FadeOut`/`DG_FadeIn` in `src/dg_saturn.cxx`
(scale `colors[]` → `pending_cram` bank 1 + the dark wall light-banks 2..7 →
`pending_wbank`, flagged dirty, uploaded by the vblank handler — the proven
no-`slSynch` path), driven from the gamestate transition in `core/d_main.c`
(streaming only): fade the current frame out, draw the new gamestate while CRAM
is black, fade in after the blit. Works BOTH directions (fade-in needs no
captured screen, so entering a level transitions too — the melt never could).
FADE_STEPS=16 (~0.27 s each way). The wall-bank ramp inside `dg_fade_bake` is
gated `#if VDP1_WALL_TEST` (`src/dg_saturn.cxx:1533-1552`), so in the ship build
only `pending_cram` ramps.

---

## Why the melt is gone (the constraint we are designing around)

`core/f_wipe.c` allocates **two** full-screen scratch buffers —
`wipe_StartScreen`/`wipe_EndScreen`, each `SCREENWIDTH*SCREENHEIGHT` (320×224) =
71 680 B + 24 B header = **71 704 B**, plus a `shittyColMajorXform` temp — i.e.
~140–210 KB of `PU_STATIC` zone during the transition.

In big-WAD CD-streaming mode the 884 KB LWRAM zone carries a ~620 KB `PU_STATIC`
floor that **fragments** the free space. At the end-of-level transition the game
freed `PU_LEVEL` (so `lv0K` in the halt message) yet still died:

```
Z_Malloc fail 71704 (fr242K lg64K st621K lv0K)
```

242 KB free total but the largest **contiguous** run is only 64 KB < the 70 KB
the melt needs → fragmentation, not exhaustion. Fix shipped: skip the melt when
`sat_streaming_mode` (`core/d_main.c`, leaving-level branch; the entering-level
direction was already skipped because the new level's `PU_LEVEL` couldn't be
freed to make room). Shareware/cart and DoomJo (`sat_streaming_mode==0`) keep
the melt, byte-identical.

The shipped replacement (Option 1) satisfies the constraints that drove the design:

1. **Uses NO large contiguous zone buffer** (the 70 KB run isn't reliably there).
2. **Commits WITHOUT `slSynch`.** `slSynch` was abandoned project-wide (it costs
   ~16% fps and mutes SCSP SFX); the CRAM palette is written directly every
   vblank and works fine, so the fade piggybacks on that proven path.
3. Is **cheap** (perf is the project target) and works in **2-player** split.
4. Handles **both** directions — the melt never managed a start-of-level
   transition here at all.

---

## The seam that already works (reuse it)

Doom's `I_SetPalette` writes the 256-entry `colors[]` and sets `palette_changed`.
On the next frame `dg_saturn.cxx` bakes `colors[]` → `pending_cram[256]` (RGB555,
MSB=1) and sets `palette_dirty` (`src/dg_saturn.cxx:2445-2450`); the **vblank
handler copies `pending_cram` straight into CRAM** (`src/dg_saturn.cxx:753-757`)
— a direct CRAM write, **no `slSynch`, proven every frame**. A fade is nothing
more than driving this exact path with a toward-black (or from-black) ramp over
N frames.

---

## The shipped approach: software palette fade (CRAM lerp)

Ramp the live palette toward black for fade-out, and from black up to the level
palette for fade-in, over ~8–16 tics. Implementation: scale `colors[]` (or
`pending_cram[]`) by `k/STEPS` each step and force `palette_dirty=1`; the
existing vblank CRAM copy does the rest.

- **Memory:** zero. No wipe buffer → the fragmentation crash cannot recur.
- **slSynch:** none — piggybacks on the already-working direct-CRAM path.
- **CPU:** a 256-entry scale per fade frame (negligible; the bake loop already
  runs on a palette change).
- **Both directions:** fade-IN needs no old screen — it just brings the new
  level's first frame up from black, which is precisely the case the melt could
  never do here. Fade-OUT darkens the current framebuffer in place.
- **2-player:** CRAM is shared by both halves (that's why the damage/pickup
  *flash* is a software per-half LUT — `HUD2P_NPAL`), so a CRAM fade dips **both
  views together**. For a level boundary both players transition together, so
  this is correct; do NOT use this mechanism for any per-player effect.
- **Light banks / walls:** the 8bpp wall path shades dark CRAM light-banks from
  the live palette (`wtex_rebuild_banks`). The fade must ramp those banks too (or
  just re-bake them from the ramped `colors[]`) so walls fade in sync with
  sprites/floors — the flash path already does exactly this, so reuse it.
- **Restore:** at fade-in end, re-assert the true level palette
  (`palette_changed = true`) so nothing is left dimmed.

### Rejected / fallback alternatives (historical)

These were considered before Option 1 shipped and are kept only for the record:

- **VDP2 hardware colour-offset ramp** (`VDP2::SetColorOffsetA/B`,
  `ScrollScreen::UseColorOffset`, `SaturnRingLib/.../srl_vdp2.hpp:1538, 1626,
  684`): cheaper in CPU but risked the SGL wrapper deferring the register behind
  `slSynch`; not needed once the direct-CRAM fade shipped.
- **Keep the melt with a boot-reserved static buffer:** rejected — costs 140 KB
  of permanent RAM in a memory-starved build, and still can't do a start-of-level
  fade-in.
- **Software framebuffer darken through the colormap:** a full-screen LUT pass
  per fade frame, strictly worse than the CRAM lerp which gets the same dimming
  for free at the CRAM lookup; fallback only.

---

## Hook points

- **Trigger:** the same `core/d_main.c` transition site where the melt is now
  gated (gamestate change to/from `GS_LEVEL`). Fade-out runs on leaving a level
  (before/over the intermission), fade-in on the first frames of `GS_LEVEL`.
- **Drive:** replace the old `wipe_ScreenWipe` loop with a fade loop that steps
  the palette ramp and calls `I_FinishUpdate` each tic; reuse the existing
  `wipestart`/`I_GetTime` timing so duration is frame-rate independent.
- **Duration:** target ~0.25–0.5 s (8–16 tics) — long enough to read as a
  transition, short enough not to stall play at the boundary.

## Remaining polish (optional)

- Decide whether the intermission (`WI_Drawer`) and finale (`F_Drawer`) want
  their own fades or just the level boundary.
- 2-player: both halves dip together (acceptable for a level boundary) — confirm
  it reads well and that the compact HUD doesn't flicker oddly mid-ramp.

## References

- Disabled melt + the crash anatomy: `docs/STREAMING_ANALYSIS.md` §3 S3,
  `core/d_main.c` (leaving-level branch), `core/f_wipe.c:237,249`.
- Proven no-`slSynch` CRAM path: `src/dg_saturn.cxx:753-757` (vblank copy),
  `:2445-2450` (bake from `colors[]`). Fade impl: `DG_FadeOut`/`DG_FadeIn` `:1561-1570`,
  `dg_fade_bake` `:1522`, `FADE_STEPS=16` `:1520` (wall-bank ramp gated `#if VDP1_WALL_TEST` `:1533-1552`).
- Saturn idiom (vblank-driven colour ramps for transitions): SlaveDriver
  `saturn-refs/SlaveDriver-Engine/{V_BLANK.C,SCL_FUNC.C}` (and the `FLASH/`
  variant).
- VDP2 RBG0-floor reality / no-`slSynch` register-commit lesson:
  `docs/VDP2_RBG0_CURRENT_STATE.md`. VDP1↔NBG1 present model:
  `docs/VDP1_PRESENT_SYNC_PLAN.md`.
