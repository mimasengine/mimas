# Level Start/End Transitions (Fades) — Design Note

Status: **Option 1 IMPLEMENTED 2026-06-23 (pending Ymir/HW validation), uncommitted.**
The vanilla screen-melt (`f_wipe`) is disabled in CD-streaming mode because it
crashes (see *Why the melt is gone*); it's now replaced by the **software CRAM
palette dip-to-black** (option 1): `DG_FadeOut`/`DG_FadeIn` in `src/dg_saturn.cxx`
(scale `colors[]` → `pending_cram` bank 1 + the dark wall light-banks 2..7 →
`pending_wbank`, flagged dirty, uploaded by the vblank handler — the proven
no-`slSynch` path), driven from the gamestate transition in `core/d_main.c`
(streaming only): fade the current frame out, draw the new gamestate while CRAM
is black, fade in after the blit. Works BOTH directions (fade-in needs no
captured screen, so entering a level transitions too — the melt never could).
FADE_STEPS=16 (~0.27 s each way). Open: HW-validate the VDP1 wall layer behaves
during the blocking fade (it should keep redrawing the last command list, dimming
via CRAM); tune FADE_STEPS to taste.

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

So any replacement transition must:

1. **Use NO large contiguous zone buffer** (the 70 KB run isn't reliably there).
2. **Commit WITHOUT `slSynch`.** DoomSRL runs a no-`slSynch` VDP2 architecture;
   the RBG0-floor experiment proved RAMCTL/cycle-pattern register changes are
   *never committed* without `slSynch`, and a one-shot `slSynch` made HW worse
   (see `docs/VDP2_ARCHITECTURE.md` / the rbg0-floor memory). The CRAM palette,
   by contrast, **is** written directly every vblank and works fine.
3. Be **cheap** (perf is the project target) and work in **2-player** split.
4. Ideally handle **both** directions — the melt never managed a start-of-level
   transition here at all.

---

## The seam that already works (reuse it)

Doom's `I_SetPalette` writes the 256-entry `colors[]` and sets `palette_changed`.
On the next frame `dg_saturn.cxx` bakes `colors[]` → `pending_cram[256]` (RGB555,
MSB=1) and sets `palette_dirty` (`src/dg_saturn.cxx:2372-2381`); the **vblank
handler copies `pending_cram` straight into CRAM** (`src/dg_saturn.cxx:729-734`)
— a direct CRAM write, **no `slSynch`, proven every frame**. A fade is nothing
more than driving this exact path with a toward-black (or from-black) ramp over
N frames.

---

## Options (ranked)

### 1. Software palette fade (CRAM lerp) — RECOMMENDED

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

### 2. VDP2 hardware colour-offset ramp — cheaper, needs HW proof

SRL exposes `VDP2::SetColorOffsetA/B(ColorOffset&)` and
`ScrollScreen::UseColorOffset(channel)` (`SaturnRingLib/.../srl_vdp2.hpp:1538,
1626, 684`; see the `VDP2 - ColorCalc` sample). Assign NBG1 (the Doom
framebuffer) to offset channel A and ramp the signed RGB offset 0 → −255 (fade to
black) and back.

- **Memory:** zero. **CPU:** one register write per step (cheaper than the CRAM
  copy — the palette is untouched).
- **RISK — must validate on HW first:** does SRL's colour-offset path commit
  **without `slSynch`**? If the SGL wrapper defers the register the way RAMCTL
  did, the offset won't take on real hardware. Mitigation: write the VDP2
  colour-offset registers **directly from the vblank handler**, exactly as the
  CRAM bank copy already does, bypassing any `slSynch`-gated SGL path.
- Leaves the palette/light banks alone, so no wall-bank resync needed — but it
  also fades NBG3 (debug) / sprite layers only if they're routed to the same
  offset channel; pick channels so the whole composited Doom image dims together.

### 3. Keep the melt with a boot-reserved static buffer — REJECTED

Reserve 140 KB permanently outside the zone for the two wipe screens. Removes the
fragmentation failure but **costs 140 KB of permanent RAM** in a build that is
already memory-starved, and the melt still needs *both* old and new screens, so
it can't do a start-of-level fade-in (no room alongside the freshly loaded
level). Uneconomical; listed only to document the why-not.

### 4. Software framebuffer darken through the colormap — FALLBACK only

Blit the existing framebuffer through progressively darker colormap rows
(colormaps are resident, no alloc). Works without `slSynch` and without a buffer,
but costs a **full-screen LUT pass per fade frame** (CPU) — strictly worse than
option 1, which gets the same dimming for free at the CRAM lookup. Keep as a
fallback if a CRAM/offset fade proves unworkable.

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

## Open questions / validation

- HW-confirm option 2 commits without `slSynch`; if not, fall to the
  direct-register or option-1 path.
- Confirm the level/light-bank palette is fully restored after a fade-in (no
  residual dimming on walls or HUD).
- Decide whether the intermission (`WI_Drawer`) and finale (`F_Drawer`) want
  their own fades or just the level boundary.
- 2-player: both halves dip together (acceptable for a level boundary) — confirm
  it reads well and that the compact HUD doesn't flicker oddly mid-ramp.

## References

- Disabled melt + the crash anatomy: `docs/STREAMING_ANALYSIS.md` §3 S3,
  `core/d_main.c` (leaving-level branch), `core/f_wipe.c:237,249`.
- Proven no-`slSynch` CRAM path: `src/dg_saturn.cxx:729-734` (vblank copy),
  `:2372-2381` (bake from `colors[]`).
- SRL colour-offset/calc: `SaturnRingLib/saturnringlib/srl_vdp2.hpp`
  (`ColorOffset` :1538, `SetColorOffsetA` :1626, `UseColorOffset` :684);
  sample `SaturnRingLib/Samples/VDP2 - ColorCalc/`.
- Saturn idiom (vblank-driven colour ramps for transitions): SlaveDriver
  `saturn-refs/SlaveDriver-Engine/{V_BLANK.C,SCL_FUNC.C}` (and the `FLASH/`
  variant).
- no-`slSynch` VDP2 trap: `docs/VDP2_ARCHITECTURE.md`, the rbg0-floor memory.
