// Mimas -- a Doom engine for the Sega Saturn.
// Copyright (C) 2025-2026 Romain Cicolini (N0rt0N85).
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.  Distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (the COPYING file at the repo root, or
// <https://www.gnu.org/licenses/>) for details.
//
/*
** Mimas -- doomgeneric platform layer for the Sega Saturn (SRL build).
**
** Hardware usage:
**   VDP2 NBG1   : 512x256 8bpp bitmap in VRAM bank B0 = Doom framebuffer
**   VDP2 NBG0   : 512x256 8bpp sky scroll layer in VRAM bank A0 (behind NBG1)
**   VDP2 NBG3   : SRL debug text overlay (SRL::Debug::Print)
**   CRAM bank 1 : Doom PLAYPAL (256 colours, RGB555)
**   4MB DRAM cart at 0x22400000: entire IWAD memory-mapped for zero-copy lumps
**   Low work RAM (0x00200000, 1MB): Doom zone heap
**   SMPC pad 1  : input
**   V-blank IRQ : millisecond clock (via SRL::Core::OnVblank)
**
** SRL is the platform SDK.  Direct SGL calls (slBitMapNbg1, slScrAutoDisp...)
** are still valid -- SRL links SGL internally.  SCU DMA, CRAM, FRT timer and
** cart probe are direct hardware register accesses as before.
*/
#include <srl.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"   /* struct color colors[256], palette_changed */
#include "doomtype.h"
#include "v_patch.h"   /* patch_t / post_t (for the VDP1 weapon sprite) */
#include "r_parallel.h"
}
#include "hud2p_panel.h"   /* generated 2-player compact HUD panel + field anchors */

/* Set by D_SetGameDescription() from the loaded IWAD's lumps; we surface it on
   the debug overlay (row 21) to confirm which WAD the binary actually detected.
   See D_FindIWAD/D_IdentifyVersion -- the *mission=none change makes this honest
   for Doom 1 and Doom 2 alike. */
extern "C" char *gamedescription;

#define SHOW_FPS 1

/* Set to 1 to ignore the RAM cart and always stream the WAD from CD (e.g. to
   run on a 4MB emulator the same way a no-cart / 1M-cart system would). */
#define FORCE_CD_STREAM 0

/* Framebuffer->VDP2 blit method.  1 = SCU DMA, 0 = plain CPU copy (~10ms/frame,
   the reliable fallback -- CURRENT).
   STEP 4a RESULT (2026-06-16): triggering on VBLANK-IN (D0MD start factor 0) does
   NOT fix the hang -- real Saturn still locks the bus at F00001.  And the
   "synchronous" wait is bogus for a deferred trigger: dma_wait_idle (DSTA & 0x30)
   returns BEFORE the vblank-deferred DMA fires, so it ran effectively async on a
   single buffer -> the next frame overwrote the framebuffer mid-transfer ->
   intermittent/torn frames on emulator (e.g. menu text present-or-not).
   Conclusion: the raw-register SCU DMA blit is a dead end (3rd failure).  Back to
   the CPU blit.  If the ~10ms is pursued again: dual-CPU blit (idle slave copies
   half) or SGL slDMACopy -- NOT this raw-register path. */
#define USE_SCU_DMA 0

/* SATURN PERF: dual-CPU framebuffer blit.  The framebuffer->VDP2 copy is the
   biggest FIXED frame cost (~12ms, constant across configs).  Since the walls
   moved to VDP1 and floors went flat, the slave SH-2 is ~80% IDLE by the time
   DG_DrawFrame runs (render+EX long finished) -- so split the 224-row copy: the
   slave copies the bottom rows [split,224) in parallel while the master copies the
   top [0,split).  Both purge their own cache before reading
   (SH7604 is write-through: the writes are in RAM, each CPU must purge to re-read
   the other's); DOOM_VRAM is uncached so its writes need no flush.  The master
   waits for the slave BEFORE clearing the framebuffer (else next frame's render
   overwrites it mid-copy = the SCU-DMA tearing bug).  This is a 2nd slSlaveFunc
   per frame -> we rewind the SGL slave work pointer before it (rp_sgl_workptr_reset)
   so the dispatch records can't creep (the ~2-min freeze).  1 = feature compiled in,
   0 = single-CPU copy only (trivial revert if the 2nd dispatch ever destabilises --
   freeze zone).  The actual split is chosen LIVE from blit_cfg[] via the pad L+R
   chord (one press = next config) so several ratios can be A/B'd on the same scene
   without a rebuild; row-2 'bl' shows the active config + ratio. */
#define DUAL_CPU_BLIT  1

/* Diagnostic slave-offload pad toggles (work-steal plane split = pad Y 'ws'; slave wall-prep =
   pad L+R 'wp').  Both are HW-CONFIRMED DEAD-ENDS (docs/RANK3_WALLPREP.md + REC_BENCHMARKS §C.2 H):
   the work-steal regresses at E1M1, and slave wall-prep is +5.8ms (cold cache the slave can't keep
   warm -- it's multiplexed across plane/masked).  Set to 0: the core implementations stay compiled
   but DORMANT (sat_plane_steal / sat_wallprep_slave default 0 = static split + inline wall-prep, the
   known-good), the pad bindings + 'ws'/'wp' overlay are compiled out, and Y + L+R are free.  Flip to
   1 to revive the live A/B (the modes are kept "sous la main"). */
#define SAT_DIAG_SLAVE_TOGGLES 0

/* SATURN PHASE-0 VDP2-ZOOM TEST.  Validate that an NBG1 *bitmap* can be hardware-
   scaled by VDP2 BEFORE building the real 160-wide render (Phase 1).  With the
   full 320-wide render unchanged, slScrScaleNbg1(2.0) enlarges NBG1 x2 horizontally
   -> on screen you see the LEFT 160 columns stretched across the whole width
   (chunky x2).  That proves (a) the scale register works on a bitmap NBG and (b)
   the scale-factor convention (2.0 = x2 bigger, per SRL's SetScale wrapper ->
   slScrScaleNbg1).  If the image instead SHRINKS to the left, the convention is
   inverse -> set VDP2_ZOOM_FACTOR to 0.5.  Vertical stays x1 (full vertical res,
   like the d32xr/FastDoom potato).  1 = on (this test), 0 = off (normal display). */
#define VDP2_ZOOM_TEST   0
#define VDP2_ZOOM_FACTOR 2.0

/* VDP1 ASYNC bring-up test (foundation for VDP1 rasterization).  slSynch is out
   (it waits a vblank = ~16% fps tax, which defeats using VDP1 to GAIN fps).  The
   async pattern (from Lobotomy's SlaveDriver, ../saturn-refs/SlaveDriver-Engine
   MEGAINIT.C/SPR.C): VDP1 in 1-cycle mode (TVMR=0, FBCR=0 => auto erase+draw+swap
   each frame), command list at VRAM 0x25C00000, PTMR to plot -- the SH-2 sets the
   trigger and RETURNS, VDP1 rasterizes in PARALLEL, hardware swaps at vblank.  This
   draws one white quad + reads the VDP1 status regs to the overlay (rows 22/23) to
   verify: (a) quad appears, (b) fps UNCHANGED (= parallel, no tax), (c) what state
   SGL/slInitSystem + its vblank ISR leave VDP1 in.  Now carries the player
   weapon (the async foundation is hardware-validated).  1 = on, 0 = off. */
#define VDP1_WEAPON 1

/* VDP1 WORLD-RENDERER Phase-0: draw ONE distorted textured quad (a grid texture on a
   trapezoid) to validate FUNC_DISTORSP and, above all, SEE the affine perspective
   WARP -- the #1 risk of the SlaveDriver-style wall renderer (walls = distorted
   textured sprites).  Grid lines that bow/skew = the affine error to judge (whether
   sub-quad subdivision is needed).  1 = on (this test), 0 = off. */
#define VDP1_WALL_TEST 1

/* VDP1 FLOOR inc-1: deport SECONDARY floors/ceilings (other heights + ceilings; NOT the RBG0
   dominant, NOT sky) to the VDP1 strip layer.  This increment is the index-0 SKIP ONLY -- with the
   stub hook below, the owned surfaces go BLACK (index 0), which (a) validates the skip targets the
   right surfaces and (b) shows the coverage the strip emitter (inc-3) will fill.  Runtime-gated by
   the core flag sat_vdp1_floor (default 0 = normal software floors); pad Y toggles it live when this
   compile flag is 1.  Set this 0 to remove the feature entirely (ship/DoomJo unaffected: hook NULL). */
#define VDP1_FLOOR_TEST 1

/* SAT_FLOOR_PERFSIM: pad-Y in-game PERF-SIMULATION toggle (4 modes) -- measures the floor-offload
   ceiling for ANY floor tech WITHOUT touching VDP2 (no RBG0, no RAMCTL/CYC) so the debug overlay
   stays readable.  The gain is identical for RBG0 / VDP1-strips / gradient floor (they all skip the
   same software floor span), so one measurement covers them all.  Modes (read REC/P rows 4/5 in each):
     0 NORMAL       draw everything (baseline)
     1 DOM-ABSENT   skip the dominant (player) floor          = VDP2/RBG0 floor perf ceiling
     2 ALL-BUT-DOM  skip secondary floors+ceilings, keep dom  = VDP1 affine-strip floor perf ceiling
     3 BOTH         skip dominant + secondary                 = VDP1+VDP2 combined ceiling
   Skipped surfaces show the backdrop (placeholder) -- only the perf numbers matter.  Set 0 to remove. */
#define SAT_FLOOR_PERFSIM 1

/* VDP1 command double-buffer.  0 = single bank, 1 = double bank.  Single-bank TESTED = BAD:
   the VDP1 reads a bank we're already overwriting (and a texture we're re-baking) the next
   frame -> "every other line shows sky" corruption.  Kept at 1 (double-buffer = correct). */
#define VDP1_DBLBANK 1

/* VDP1 framebuffer MANUAL-CHANGE (anti-tearing).  0 = 1-cycle auto (FBCR=0: VDP1 swaps its
   two framebuffers EVERY vblank), 1 = manual change.  The wall list spans several vblanks at
   our fps, so 1-cycle showed it half-drawn = the VDP1 "déchirures".  Manual change holds the
   last COMPLETE frame and only swaps when the draw finished (EDSR CEF), triggered from the
   existing OnVblank -- i.e. vsync the VDP1 presentation WITHOUT slSynch (no fps tax, no SCSP
   sound conflict, no latency: the CPU never waits).  1 = on (anti-tear), 0 = old auto swap. */
#define VDP1_MANUAL_CHANGE 0   /* BISECT: 0 = revert 3faaa95's manual-change present -> 1-cycle auto (test if the VDP1 walls come back, with tearing) */

extern "C" byte *I_VideoBuffer;
extern "C" int   gametic;
extern "C" int   r_visplane_peak;
extern "C" int   sat_floor_vq_cur, sat_floor_vq_peak;  /* VDP1-floor inc-0 estimate, shown on row 2 */
extern "C" unsigned int sat_sky_px, sat_floor_px;  /* sky-vs-floor coverage classifier (row 13) */
/* SATURN VALIDATION (Ymir-readable, deterministic): RAM-lever sizing telemetry. */
extern "C" int   r_visplane_coverage_peak;  /* #1: peak sum of live-plane spans (top-bytes) */
extern "C" int   r_visplane_pool_peak;      /* #1: peak bytes used in the span pool (0 if off) */
extern "C" int   r_visplane_pool_ovf;       /* #1: planes that overflowed VP_POOL_PLANES (0 = ok) */
extern "C" int   sat_texcache_active, sat_texcache_poolkb, sat_texcache_entries,
                 sat_texcache_builds, sat_texcache_evicts;  /* streaming texture cache (core/r_cache.c) */
extern "C" int   sat_texcache_carve_lf;     /* largest free block (KB) at the last carve attempt */
extern "C" int   sat_tex_numtex, sat_tex_sumwidth, sat_tex_dirbytes,
                 sat_tex_mptex, sat_tex_mpwidth;  /* Phase-0 texture-floor measurement (r_data.c) */
extern "C" int   Z_FreeMemory(void);          /* total reclaimable (free + purgeable) bytes */
extern "C" int   Z_LargestAllocatable(void);  /* largest contiguous run after purging */
extern "C" int   dg_heap_peak;              /* #4: peak newlib sbrk usage (bytes)             */
extern "C" int   dg_heap_size;              /* #4: newlib heap cap (bytes)                    */
/* split-screen perf breakdown (ms per piece of the 2p render block) -- diagnose the slowdown */
extern "C" unsigned int sat_spl_sw, sat_spl_v0, sat_spl_v1, sat_spl_v2, sat_spl_v3, sat_spl_kick;
/* VDP1 wall-texture bakes (cache misses) THIS flush -- diagnoses the `k` cost: if the 2 split
   views thrash the 22 shared slots, bk stays high every frame => re-bake is the kick cost. */
static int wtex_bakes = 0;

/* ------------------------------------------------------------------ */
/* Saturn memory map constants                                         */
/* ------------------------------------------------------------------ */

#define CART_RAM_UNCACHED   ((volatile unsigned char *)0x22400000)
#define CART_RAM_CACHED     ((unsigned char *)0x02400000)
#define CART_RAM_SIZE       0x400000
#define CART_ID_ADDR        ((volatile unsigned char *)0x24FFFFFF)
#define CART_ID_1MB         0x5a
#define CART_ID_4MB         0x5c

#define LOW_WORK_RAM        ((unsigned char *)0x00200000)
#define LOW_WORK_RAM_SIZE   0x100000

#define DOOM_VRAM           ((unsigned char *)0x25E40000)  /* VDP2 VRAM B0 */
#define DOOM_VRAM_STRIDE    512
/* Doom now renders NATIVE 320x224 (SCREENHEIGHT=224): the 192-line 3D view fills 0..191 and the
   32px status bar sits at 192..223 (the screen bottom), so no display offset is needed.  Kept at 0
   (the blit/VDP1-local-coord/sky-scroll terms become no-ops); only set non-zero to re-add a
   letterbox. */
#define VIEW_Y_OFFSET       0
#define CRAM_DOOM_PAL       ((volatile unsigned short *)(0x25F00000 + 256 * 2))

#define TVSTAT              (*(volatile unsigned short *)0x25F80004)

/* SCU DMA level 0 (indirect mode) */
#define SCU_D0W             (*(volatile unsigned int *)0x25FE0004)
#define SCU_D0AD            (*(volatile unsigned int *)0x25FE000C)
#define SCU_D0EN            (*(volatile unsigned int *)0x25FE0010)
#define SCU_D0MD            (*(volatile unsigned int *)0x25FE0014)
#define SCU_DSTA            (*(volatile unsigned int *)0x25FE007C)
#define DMA_END_FLAG        0x80000000u

extern "C" unsigned char  *sat_wad_base    = nullptr;
extern "C" unsigned int    sat_wad_size     = 0;
extern "C" int             sat_streaming_mode;   /* defined in core/p_setup.c; set to 1 below in CD-streaming mode */

/* Step 4b (STREAMING_ANALYSIS §7.9 "Cart load-once"): when a big WAD streams from
   CD (can't raw-load into the cart) but a 4MB cart is present, the .DRP loader
   stages each map's compressed blob into it once per level (CD then idle -> CDDA).
   sat_cart_usable = cart bytes free for that staging (0 = none / cart holds the
   raw WAD); sat_cart_cached_base = the cart's cached read alias (set in DG_Init).  */
extern "C" unsigned int    sat_cart_usable  = 0;
extern "C" unsigned char  *sat_cart_cached_base = nullptr;
extern "C" int W_SaturnCDInit(void);

/* ------------------------------------------------------------------ */
/* Sky -> VDP2 NBG0 scroll layer (SATURN sky offload)                   */
/* ------------------------------------------------------------------ */
/* Doom sky is 256x128, full-bright (palette indices are direct).  We blit it
   into VDP2 VRAM bank A0 as a 512x256 8bpp NBG0 bitmap (tiled 2x horizontally so
   the scroll wraps seamlessly) and scroll it by viewangle.  NBG1 (the game
   framebuffer) is at bank B0. */
#define SKY_VRAM         ((unsigned char *)0x25E00000)  /* VDP2 VRAM A0 */
#define SKY_VRAM_STRIDE  512
#define SKY_ANGLESHIFT   22   /* r_sky.h ANGLETOSKYSHIFT: 90deg (0x40000000)->256px */

/* SATURN RBG0 floor prototype (docs/RBG0_FLOOR_PLAN.md).  Phase-0 = bring-up +
   coexistence test: a cell-based rotation plane (tiled test flat, IDENTITY transform,
   no perspective yet) in the free VRAM bank A1, shown at priority 5 so it appears
   through the index-0 sky/ceiling region -- the goal is to confirm RBG0 displays
   WITHOUT breaking the raw-SGL NBG0/NBG1 cycle pattern.  Coefficient table + Mode-7
   perspective + dynamic flat selection are Phases 1-3.  Gated, throwaway. */
/* 0 = RBG0 hardware floor PAUSED -> known-good build: VDP2 hardware sky + software floor,
   no RBG0, no RAMCTL poke (set VDP2_HW_SKY=1 with this).  1 = RBG0 Mode-7 floor test
   (needs VDP2_HW_SKY=0; still snows on HW -- the cycle-pattern commit is unsolved, see
   docs/VDP2_ARCHITECTURE.md).  Code is kept under #if either way. */
#define VDP2_RBG0_TEST   1
/* DEBUG: force RBG0 above the game (priority 6, NBG1 dropped to 5) so its content is
   visible regardless of the index-0 window -- a definitive "does RBG0 render my grid?"
   check.  Set 0 for the real layering (RBG0 priority 5, shows only through index-0). */
#define RBG0_DEBUG_ONTOP 0
/* VDP2_HW_SKY: 1 = hardware sky bitmap on NBG0 in bank A0 (old config).  0 = SOFTWARE
   sky -> frees bank A0 so the textured RBG0 floor's K-table gets its OWN bank, giving
   the correct 4-bank layout (B0 framebuffer / A1 cells / B1 map / A0 K-table).  The
   textured floor REQUIRES 0: A0 cannot be both the sky bitmap and the K-table, and
   swapping it at runtime would need a mid-game RAMCTL/CYC re-commit (fragile -- slSynch
   makes it worse), so this is a BUILD choice, not a pad-mode toggle.  Software sky costs
   a little REC back, but it lands on the slave the floor offload frees (slave 46->0%
   busy).  See docs/VDP2_ARCHITECTURE.md.  PAUSED config = 1 (hardware sky, RBG0 off). */
#define VDP2_HW_SKY      0
/* RBG0 register-commit method (re-examining the "slSynch is poison" conviction, 2026-06-26):
   1 = ONE-SHOT slSynch at init -> SGL flushes its FULL VDP2 register shadow to the chip (commits
       every RBG0 register correctly), ZERO per-frame cost.  Tests whether slSynch one-shot is the
       simplest fix and whether the perf/SFX convictions were overstated.
   0 = manual block-flush of the shadow register image (rbg0_commit_cyc), no slSynch at all. */
#define RBG0_COMMIT_VIA_SLSYNCH  1
/* Layer-isolation test: 0 = do NOT display RBG0 (clear RBG0ON) while keeping all the setup/commit.
   If the "snow" DISAPPEARS with RBG0 off -> the rotation layer is the source.  If it PERSISTS ->
   it's NOT RBG0 (back-screen reading garbage, or another layer).  1 = normal (RBG0 shown). */
#define RBG0_DISPLAY     1
/* RBG0_NBG3: re-enable the NBG3 debug text overlay (B1) now that the bitmap floor freed B1 of
   its cell map.  NBG3's font/page/map live in B1 (SRL default) away from the RPT (B1+0x1ff00);
   when on, we let slScrAutoDisp schedule NBG3's B1 cycle (DON'T scrub CYCB1).  0 = off. */
#define RBG0_NBG3        1
/* RBG0_LINECOL_TEST: per-distance floor light via the VDP2 line-color screen + RBG0 color-calc
   (see rbg0_linecol_apply).  RUNG A (flat darken) only PROVED the plumbing; the effect is not
   convincing yet, so it is GATED OFF (0) -- the code stays for the RUNG C rework (per-line
   distance gradient, future session, see memory rbg0-floor-distance-light).  Footprint when on:
   0 VRAM banks / 0 CRAM / 0 cycles (rides the K-table + color-calc registers).  Set 1 to revisit. */
#define RBG0_LINECOL_TEST 0
/* RBG0 per-frame ROTATION-PARAMETER-TABLE transfer (the real root cause, proven by LIBSGL.A disasm,
   docs/RBG0_STRUCTURED_GARBAGE.md): slScrMatSet only fills SGL's CACHED RAM buffer (_RotScrParA) +
   sets a dirty flag; the RAM->VRAM DMA of the RPT is done ONLY by the _BlankIn ISR, armed ONLY by
   slSynch.  Without it the rotation reads the BOOT transform -> flat tiling (the "grid").
   0 = none (broken)  ;  1 = per-frame slSynch (Test A: confirms, but caps fps + mutes SCSP SFX)
   2 = manual RPT memcpy reproducing _BlankIn, NO slSynch (Test B: the real shipping fix). */
#define RBG0_RPT_TRANSFER 2
/* RBG0_BITMAP: 1 = floor is a 512x256 8bpp BITMAP (no pattern-name map).  Dropping the map
   removes the B1 rotation read -> B1 is FREED (NBG3 debug overlay coexists) and the floor
   drops from 3 banks to 2.  Bank layout (needs VDP2_HW_SKY=0): A0 bitmap / A1 K-table /
   B0 framebuffer / B1 free->NBG3.  The bitmap MUST be in A0, NOT A1: SGL hardcodes its
   rotation anchors in A1 (sl_def.h KTBL0_RAM=A1, RBG_PARA_ADR=A1+0x1ff00); writing the 128KB
   bitmap over them faults the rotation engine.  The coefficient table then sits in A1 =
   KTBL0_RAM, where SGL expects it.  0 = legacy cell+map floor (map in B1, 3 banks, evicts
   NBG3 -- the dead-end). */
#define RBG0_BITMAP      1
#define RBG0_CEL_VRAM    ((void *)0x25E20000)  /* VDP2 VRAM A1: cell (char) data (cell path)   */
#define RBG0_MAP_VRAM    ((void *)0x25E70000)  /* VDP2 VRAM B1: pattern name table (cell path) */
#if RBG0_BITMAP
/* SlaveDriver layout (PLAX.C initPlax, ships rotation bitmap on real HW): bitmap (char) in
   A1, rotation/coefficient table in A0 -- same banks as the BOOTING cell path (cells A1,
   K A0).  The earlier "bitmap must be in A0 / A1 anchors clobber it" was a WRONG conclusion. */
#define RBG0_BMP_VRAM    ((void *)0x25E20000)  /* VDP2 VRAM A1: 512x256 8bpp floor bitmap       */
#define RBG0_KTAB_VRAM   ((void *)0x25E00000)  /* VDP2 VRAM A0: coefficient/rotation table       */
#elif VDP2_HW_SKY
#define RBG0_KTAB_VRAM   ((void *)0x25E28000)  /* A1: collides w/ cells -- only safe if RBG0 floor off */
#else
#define RBG0_KTAB_VRAM   ((void *)0x25E00000)  /* VDP2 VRAM A0: freed by software sky -> K-table's own bank */
#endif
/* Pad Y toggles the RBG0 hardware floor.  ON = floor on RBG0 (NBG3 debug overlay evicted
   by the RBG0 map in B1).  OFF = software floor + the NBG3 overlay returns -> read REC/EX/
   P/FLAT to see what the floor offload saves.  Boot = on. */
/* Pad Y cycles 3 RBG0/debug modes (the RBG0 map in B1 and the NBG3 overlay are mutually
   exclusive, so the HW floor and the debug text can't show together -- hence the 3 states):
     0 = VDP2 floor, NO debug   (the ship look: sky+game+RBG0 floor, NBG3 evicted)
     1 = debug + SOFTWARE floor (NBG3 on, sat_vdp2_floor=0 -> floor drawn by CPU)
     2 = debug, NO software floor (NBG3 on, sat_vdp2_floor=1 -> floor skipped, RBG0 off)
   Modes 1 vs 2 (read REC/EX/P/FLAT in both) isolate the software-floor cost = the saving
   the VDP2 floor buys.  Boot = 0. */
static int rbg0_mode = 0;
static int nbg3_show = 0;   /* NBG3 debug overlay display, toggled live by the pad L+R chord (default OFF).
                               Its B1 cycle is reserved at init (RBG0_NBG3), so this only flips BGON. */
/* RBG0_TUNE_PAD gates the live floor-tuning pad toggles (orientation / texel offset / plane
   pitch+level) AND the d-pad-from-Doom gate.  0 = PARKED: the found values below are the baked
   defaults and player movement is normal.  Flip to 1 to re-tune on the pad. */
#define RBG0_TUNE_PAD 0
/* TEMP live floor tuning (no debug overlay -> the user counts pad presses).
   L + C      cycles the texture ORIENTATION over all 8 D4 symmetries of a square
              (0 id, 1 rot90, 2 rot180, 3 rot270, 4 mirrorH, 5 mirrorV, 6 transpose,
              7 anti-transpose) -- covers every rotation AND mirror.
   L + d-pad  shifts the texture +-1 texel on X/Y (re-shades, rbg0_tex_dirty).
   R + d-pad  nudges the PLANE in the transform: R+up/down = inclination (pitch),
              R+left/right = the plane's near level (Z).
   Defaults below = the values found on HW (2026-06-27): mirrorV, yoff 0, pitch +0x100.
   Toggles kept live for validation; then bake into #defines/upload + remove the pad. */
static int rbg0_tex_orient = 5;     /* 0..7 D4 symmetry (pad L+C); 5 = mirrorV (found on HW)    */
static int rbg0_tex_xoff   = 0;     /* texel X offset (pad L + left/right), wraps mod 64        */
static int rbg0_tex_yoff   = 0;     /* texel Y offset (pad L + up/down); 0 found on HW          */
static int rbg0_tex_dirty  = 1;     /* force a re-upload after an orientation/offset change     */
static int rbg0_pitch_adj  = 0x100; /* ANGLE added to RBG0_PITCH (pad R+up/down); +0x100 found  */
static int rbg0_z_adj      = 0;     /* fixed_t added to the plane Z (pad R + left/right)        */
#if SAT_FLOOR_PERFSIM
/* Pad-Y floor perf-sim mode (0 normal / 1 dom-absent / 2 all-but-dom / 3 both).  See SAT_FLOOR_PERFSIM. */
static int floor_perfsim_mode = 0;
#endif
/* RBG0 RAMCTL-commit readback (direct chip write of the rotation bank-select RDBS; see
   rbg0_commit_ramctl).  Shown on overlay row 14 in pad-Y debug modes 1/2. */
static uint16_t ramctl_before = 0, ramctl_after = 0;
/* RBG0 cycle-pattern chip BEFORE-snapshot for the framebuffer debug readout (docs/RBG0_SNOW_FIX_PLAN.md). */
static uint32_t cyc_before[4] = {0,0,0,0};
/* SGL VDP2 register SHADOW (sglK01.o in LIBSGL.A; VDP2_RAMCTL already via srl_base.hpp).  We mirror
   the RBG0 cycle-pattern commit here too so it survives a possible per-vblank ISR re-push of the
   shadow -> chip.  extern "C" to match srl_base.hpp's VDP2_RAMCTL (global C symbols). */
extern "C" {
    extern uint16_t VDP2_CYCA0L, VDP2_CYCA0U, VDP2_CYCA1L, VDP2_CYCA1U,
                    VDP2_CYCB0L, VDP2_CYCB0U, VDP2_CYCB1L, VDP2_CYCB1U;
}
/* RBG0 plane geometry, tuned against the software floor 2026-06-18 (live X+d-pad tuning,
   since removed).  PITCH = +4.21deg off the 90deg ground tilt -> raises the plane's far end
   onto Doom's horizon; YAW = +90deg -> orients the flat to the world.  Texture scale came out
   1:1 (no slScale needed) once the pitch was right. */
#define RBG0_PITCH       0x300    /* ANGLE delta on slRotX (~4.21deg) */
#define RBG0_YAW_OFF     0x4000   /* ANGLE yaw offset (90deg) -- texture 180deg is done at upload */

/* SKY_FIXED 1 = the sky does NOT scroll with the view angle (Romain's choice:
   a static backdrop).  0 = scroll with viewangle, slowed by SKY_PARALLAX_SHIFT
   (0 = Doom-faithful 256px/90deg, 1 = half, 2 = quarter). */
#define SKY_FIXED          0
#define SKY_PARALLAX_SHIFT 0

/* SKY_DEBUG_SHOW: 1 = draw NBG0 ON TOP of the game (opaque) so we can verify the
   sky uploads/orients/scrolls before transparency is wired (Stage A).  0 = NBG0
   below NBG1, revealed only through index-0 transparency (Stage B/C). */
#define SKY_DEBUG_SHOW   0

extern "C" int            skytexture;
extern "C" unsigned int   viewangle;        /* angle_t (unsigned int) */
extern "C" int            viewx, viewy, viewz;  /* fixed_t camera pos (16.16 map units) */
extern "C" unsigned char *R_GetColumn(int tex, int col);
extern "C" unsigned char *colormaps;        /* lighttable_t* (byte*), saturn_cmap */
extern "C" int            gamestate;        /* gamestate_t: GS_LEVEL == 0 */
extern "C" int            menuactive;       /* boolean: menu overlay up */
extern "C" int            automapactive;    /* boolean: automap up */
extern "C" int            sat_vdp2_sky;     /* core: skip software sky (=> VDP2) */
extern "C" int            sat_frame_has_sky;/* core: a sky visplane was in view this frame */
extern "C" int            sat_vdp2_floor;   /* core: skip software floor (=> VDP2 RBG0) */
extern "C" int            sat_vdp2_floor_h; /* core: player's floor height (fixed_t) */
extern "C" int            sat_vdp2_floor_pic;/* core: player's floor flat (picnum) */
extern "C" unsigned char *sat_vdp2_floor_cmap;/* core: colormap for the floor's sector light (0=full bright) */
extern "C" int            sat_potato_floors;/* core: solid-colour floors/ceilings */
extern "C" int            sat_potato_walls; /* core: solid-colour walls (opaque, flat only) */
extern "C" int            sat_wall_nocpu;   /* core: banded/flat -> skip close-wall CPU fallback */
extern "C" int            sat_local_players; /* core: LIVE local-coop player count (1 = single) */
extern "C" int            sat_split_vdp1;    /* core: split keeps walls on VDP1 (views 0/1); pad-X A/B */
#if VDP1_FLOOR_TEST || SAT_FLOOR_PERFSIM
extern "C" int            sat_vdp1_floor;   /* core: skip secondary floors/ceilings (=> VDP1 strips) */
extern "C" int          (*sat_floor_vdp1_hook)(int picnum, int height, int minx, int maxx,
                                               const unsigned char *top, const unsigned char *bottom,
                                               int lightlevel);  /* core: 1 => platform owns this surface */
#endif

#if VDP1_WEAPON
/* VDP1 weapon: the core psprite hook pointers (defined in r_things.c) + our impls
   (defined below DG_DrawFrame).  Forward-declared here so DG_Init can register them. */
extern "C" {
extern void (*sat_psprite_begin)(void);
extern void (*sat_psprite_hook)(patch_t *patch, int lump, int sx, int sy, int flip,
                                const unsigned char *cmap);
void sat_vdp1_wpn_begin(void);
void sat_vdp1_wpn_draw(patch_t *patch, int lump, int sx, int sy, int flip,
                       const unsigned char *cmap);
}
#endif

#if VDP1_WALL_TEST
/* VDP1 world-renderer Step 3: textured one-sided walls.  The core hook (r_segs.c)
   hands each one-sided wall's 4 screen corners + texnum + texture-u at the two ends +
   light colormap; we build the texture (cached per texnum) and tile it across the wall
   as distorted sub-quads.  Forward-declared so DG_Init can register it.  texturewidthmask
   / textureheight are core globals (r_data.c, fixed_t = int). */
extern "C" {
extern int (*sat_wall_hook)(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                            int texnum, int u1, int u2, int v0, int v1,
                            const unsigned char *cmap);
int sat_wall_vdp1(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                  int texnum, int u1, int u2, int v0, int v1,
                  const unsigned char *cmap);
extern int *textureheight;       /* fixed_t: pixels = >>16 */
extern int *texturewidthmask;    /* width-1 */
extern int  sat_wall_skip;       /* 1 = skip the software one-sided wall draw (VDP1 owns it) */
extern int  R_WallPotatoColor(int tex);   /* dominant palette index of a wall texture */
extern void (*sat_walls_done_hook)(void); /* core: called after the BSP walk -> early VDP1 kick */
void sat_walls_kick(void);                /* platform: flush + kick the VDP1 walls */
}
#endif

/* Potato / quality levels cycled live with pad Z -- 6 modes for in-game A/B comparison.
   pot0 = textured; pot1 = flat floors; pot2 = flat-shaded walls (bd = banded texture stripes
   that scroll, fl = single flat colour).  +ld = low-detail (half horizontal res), meaningful
   ONLY in the 2-player split (it gates the d_main.c split block).  pot2+ld is omitted (low-detail
   of flat/banded walls is invisible).  Everything is platform-side EXCEPT sat_split_lowdetail
   (core flag, default 0 -> DoomJo/1p unaffected). */
#define POTATO_LEVEL 0
static int potato_level = POTATO_LEVEL;    /* 0..5, index into potato_modes */
static int wall_potato_mode = 0;           /* Mimas VDP1 wall mode: 0=textured 1=banded 2=flat */
/* the modes: floors flat?, wall mode (0=tex 1=banded 2=flat), low-detail?, floor-low-detail?,
   display name.  pot0.5 = textured floors drawn at HALF-rate fill (1 texel / 2 screen px) with
   FULL VDP1 walls -- the floor-only low-detail the global +ld can't give (global +ld halves the
   whole render geometry, walls included).  Inserted right after pot0 (a finer first step). */
static const struct { int floors, wmode, ld, fld; const char *name; } potato_modes[7] = {
    { 0, 0, 0, 0, "pot0"    },
    { 0, 0, 0, 1, "pot0.5" },
    { 0, 0, 1, 0, "pot0+ld" },
    { 1, 0, 0, 0, "pot1"    },
    { 1, 0, 1, 0, "pot1+ld" },
    { 1, 1, 0, 0, "pot2-bd" },
    { 1, 2, 0, 0, "pot2-fl" },
};

/* Dual-CPU blit configs, cycled LIVE by the pad L+R chord (one press = next, wraps).
   'mpct' = % of the 224 framebuffer rows the MASTER copies ([0,split)); the slave
   copies the rest [split,224).  The slave reads work-RAM less-cached than the master
   (which just generated it) so it is slower per row -> shifting rows toward the
   master (higher mpct) trims the master's spin-wait.  Index 0 = single-CPU baseline
   (for the A/B reference).  Sweep 50/50 -> 75/25 to bracket the balance point; if
   even 75/25 still leaves the master waiting, add higher mpct entries (or vice-versa).
   Row-2 'bl<idx> <mpct>/<spct>' shows the active config so a photo ties bl/MST/fps
   to its ratio. */
static const struct { int dual; int mpct; } blit_cfg[] = {
    { 0, 100 },   /* 0: single-CPU (master copies all 224) */
    { 1,  50 },   /* 1: 50/50 */
    { 1,  60 },   /* 2: 60/40 */
    { 1,  66 },   /* 3: 66/34 */
    { 1,  75 },   /* 4: 75/25 */
};
#define BLIT_CFG_N ((int)(sizeof(blit_cfg) / sizeof(blit_cfg[0])))
static int blit_mode = 0;   /* boot: single-CPU blit.  2026-06-22 HW (pot0 tech room):
   dual NEVER beats single -- blit is only ~5.5ms (not the assumed ~12ms) and is bus-bound
   (S~1.3); 50/50 was the WORST (6.0 vs 5.5).  Verdict DROP; L+R chord still A/Bs configs. */
/* Master row count for the current config: mpct% of 224. */
static inline int blit_split(void) { return (blit_cfg[blit_mode].mpct * 224) / 100; }
/* SATURN PERF: last frame's framebuffer->VDP2 blit wall-clock in ms*10 (master FRT delta
   around the copy, INCLUDING the dual-blit slave-join spin).  This is the number that
   decides dual-CPU blit GO/DROP -- fps/MST are too coarse (~12ms of a ~100ms frame).
   Read it on row 2 as 'b<ms.tenth>'; compare config 0 (single) vs 4 (75/25), same scene. */
static unsigned int sat_blit_ms10 = 0;
/* core/r_plane.c L1 toggle (1 = visplane hash, 0 = vanilla linear scan); frozen at 1 (HW: NULL
   at E1M1, REC §C.2) -- kept extern for the row-1 stamp only, no longer pad-toggled. */
extern "C" int sat_visplane_hash;
/* core/r_parallel.c visplane-split A/B (0 = static half-split [default, good], 1 = two-pointer
   work-steal); pad Y toggles it live -> read row-3 'w' (master wait at the barrier) + 'P' + fps.
   Row 1 shows ws<state>; the profiler window auto-resets on the flip. */
extern "C" int sat_plane_steal;
/* RANK 3 inc-1 (docs/RANK3_WALLPREP.md): run the deferred wall-prep flush on the SLAVE (1) vs the
   master (0).  Pad L+R toggles it live; ON also enables sat_wallprep_defer (walls queued, not
   inline).  inc-1 is NON-overlapped -> expect byte-identical render + Bp off the master + w up
   ~21ms + fps UNCHANGED (the win is inc-2).  Row 1 shows wp<state>. */
extern "C" int sat_wallprep_slave;
extern "C" int sat_wallprep_defer;
static void sat_apply_potato(void)
{
    extern int sat_split_lowdetail;            /* core flag (only acts in the split block) */
    extern int sat_floor_ld;                   /* core/r_plane.c -- pot0.5 half-rate textured floors */
    int L = potato_level; if (L < 0 || L >= 7) L = 0;
    sat_potato_floors   = potato_modes[L].floors;
    sat_potato_walls    = (potato_modes[L].wmode == 2);  /* DoomJo software-wall flat parity (flat only) */
    sat_wall_nocpu      = (potato_modes[L].wmode >= 1);  /* banded OR flat -> skip the close-wall CPU fallback */
    wall_potato_mode    = potato_modes[L].wmode;         /* Mimas VDP1 3-way (tex/banded/flat) */
    sat_split_lowdetail = potato_modes[L].ld;
    sat_floor_ld        = potato_modes[L].fld;           /* pot0.5: textured floors at half-rate fill */
}
#define GS_LEVEL 0
#define SAT_CMAP_BYTES (34 * 256)           /* COLORMAP: 34 maps of 256 (r_data.c) */

/* Debug-overlay shim: core (d_main.c, r_*.c) calls dbg_print(x, y, str). */
extern "C" void dbg_print(int x, int y, char *str)
{
    SRL::Debug::Print((uint8_t)x, (uint8_t)y, str);
}

#if SHOW_FPS
static unsigned short last_dma_ticks;
#endif

static unsigned short pending_cram[256];
static volatile int   palette_dirty = 0;

/* 8bpp wall lighting: 6 CRAM "dark" banks (2..7) hold the PLAYPAL pre-shaded by a colormap
   level; bank 1 is NBG1's live full-bright palette.  Built on a palette change (level load /
   damage flash) into pending_wbank[], then uploaded to CRAM in the vblank handler (avoids
   mid-display sparkle, like pending_cram).  -> per-wall light + flash re-tint with NO texture
   re-bake (the texture cache stores raw palette indices, never re-baked). */
#define WLIGHT_DARK_N 6                                   /* CRAM banks 2..7 */
static unsigned short pending_wbank[WLIGHT_DARK_N][256];
static volatile int   wbank_dirty = 0;
#define CRAM_BANK(b)  ((volatile unsigned short *)(0x25F00000 + (b) * 512))

static unsigned char framebuffer[320 * 224] __attribute__((aligned(4)));  /* = core I_VideoBuffer */
static unsigned int  dma_table[224][3] __attribute__((aligned(16)));

/* 2-player compact HUD: blit the two 160x64 panels (P1 left, P2 right) into the
   bottom 64 rows of the framebuffer; the core then draws each player's widgets
   (numbers/face/keys) on top via ST_DrawCompactWidgets. */
extern "C" void ST_DrawCompactWidgets(int pnum, int ox, int oy);  /* core: per-player HUD widgets */
#define HUD2P_TOP  (224 - HUD2P_H)            /* = 160 */
static void hud2p_blit_panels(void)
{
    for (int y = 0; y < HUD2P_H; ++y)
    {
        unsigned char *row = framebuffer + (HUD2P_TOP + y) * 320;
        memcpy(row,       hud2p_panel + y * HUD2P_W, HUD2P_W);   /* P1 (left)  */
        memcpy(row + 160, hud2p_panel + y * HUD2P_W, HUD2P_W);   /* P2 (right) */
    }
}

/* 2-player flash (per-half software wash): the hardware palette is shared by both
   viewports, so each player's damage/pickup flash is applied by remapping that
   half's framebuffer indices through a LUT = base-palette index nearest to
   PLAYPAL[level][i].  The 13 LUTs are built once (first 2p frame) from PLAYPAL. */
extern "C" int   ST_PlayerPaletteIndex(int pnum);   /* core: flash level for players[pnum] */
extern "C" void *W_CacheLumpName(char *name, int tag);
#define HUD2P_NPAL 14                               /* PLAYPAL sub-palettes (0 = base) */
static unsigned char hud2p_flash_lut[HUD2P_NPAL][256];
static int           hud2p_flash_built = 0;

static void hud2p_flash_build(void)
{
    const unsigned char *pp = (const unsigned char *)W_CacheLumpName((char *)"PLAYPAL", 1 /*PU_STATIC*/);
    if (!pp) return;
    for (int L = 1; L < HUD2P_NPAL; ++L)
    {
        const unsigned char *sub = pp + L * 768;
        /* SATURN: PLAYPAL 1..8 = red damage, 9..12 = gold pickup, 13 = green radsuit. */
        int damage = (L <= 8);
        for (int i = 0; i < 256; ++i)
        {
            int r = sub[i*3], g = sub[i*3+1], b = sub[i*3+2];
            /* The raw damage tint, matched on the base palette, reads too "pink"
               (light red) in 2p.  Cut green/blue on the RED damage palettes so the
               nearest match lands on a deeper BLOOD red (gold/green left intact). */
            if (damage) { g >>= 1; b >>= 1; }
            int best = 1 << 30, bj = 0;
            for (int j = 0; j < 256; ++j)
            {
                int dr = pp[j*3] - r, dg = pp[j*3+1] - g, db = pp[j*3+2] - b;
                int d = dr*dr + dg*dg + db*db;
                if (d < best) { best = d; bj = j; if (!d) break; }
            }
            hud2p_flash_lut[L][i] = (unsigned char)bj;
        }
        /* SATURN layer inversion: index 0 is the RESERVED transparent code in NBG1 --
           the VDP1 walls (and the VDP2 sky) show THROUGH it (see the darkest-index note
           ~:1282).  The nearest-colour search above maps index 0 (= PLAYPAL black tinted
           toward the flash) to a dark-RED index, which makes every transparent wall/sky
           hole OPAQUE during a flash -> the whole VDP1 wall layer is occluded ("all walls
           go black" in 2p).  Force index 0 to stay transparent so walls/sky remain visible
           through the flash.  (The VDP1 walls don't tint with the flash -- separate layer,
           shared CRAM -- but staying visible beats being blacked out.) */
        hud2p_flash_lut[L][0] = 0;
    }
    hud2p_flash_built = 1;
}

/* per-half flash: remap each flashing player's column-half (all 224 rows) in place. */
static void hud2p_apply_flash(void)
{
    if (!hud2p_flash_built) hud2p_flash_build();
    int l1 = ST_PlayerPaletteIndex(0);
    int l2 = ST_PlayerPaletteIndex(1);
    for (int half = 0; half < 2; ++half)
    {
        int lvl = half ? l2 : l1;
        if (lvl <= 0 || lvl >= HUD2P_NPAL) continue;
        const unsigned char *lut = hud2p_flash_lut[lvl];
        int x0 = half ? 160 : 0;
        for (int y = 0; y < 224; ++y)
        {
            unsigned char *row = framebuffer + y * 320 + x0;
            for (int x = 0; x < 160; ++x) row[x] = lut[row[x]];
        }
    }
}

extern "C" unsigned char *DG_FrameBuffer(void)
{
    return framebuffer;
}

/* ------------------------------------------------------------------ */
/* Console (NBG0 text overlay via SRL::Debug::Print)                   */
/* ------------------------------------------------------------------ */

#define CONSOLE_COLS 40
#define CONSOLE_ROWS 26
static char console_lines[CONSOLE_ROWS][CONSOLE_COLS + 1];
static int  console_row = 0;
static int  console_col = 0;
static int  console_enabled = 1;

static void console_redraw(void)
{
    if (!console_enabled)
        return;
    static char padded[CONSOLE_COLS + 1];
    for (int y = 0; y < CONSOLE_ROWS; ++y)
    {
        int len = (int)strlen(console_lines[y]);
        memcpy(padded, console_lines[y], len);
        memset(padded + len, ' ', CONSOLE_COLS - len);
        padded[CONSOLE_COLS] = '\0';
        SRL::Debug::Print(0, y + 2, padded);
    }
}

static void console_scroll(void)
{
    for (int y = 0; y < CONSOLE_ROWS - 1; ++y)
        memcpy(console_lines[y], console_lines[y + 1], CONSOLE_COLS + 1);
    memset(console_lines[CONSOLE_ROWS - 1], 0, CONSOLE_COLS + 1);
}

extern "C" void sat_console_putc(char c)
{
    if (c == '\r')
        return;
    if (c == '\n' || console_col >= CONSOLE_COLS)
    {
        if (console_row >= CONSOLE_ROWS - 1)
            console_scroll();
        else
            console_row++;
        console_col = 0;
        if (c == '\n')
        {
            console_redraw();
            return;
        }
    }
    console_lines[console_row][console_col++] = (c < 32) ? ' ' : c;
}

extern "C" void sat_console_clear(void)
{
    memset(console_lines, 0, sizeof(console_lines));
    console_row = 0;
    console_col = 0;
    SRL::Debug::PrintClearScreen();
}

extern "C" void sat_debug_row0(const char *s)
{
    SRL::Debug::Print(0, 1, s);
}

extern "C" void DG_Fatal(const char *msg)
{
    /* Row 0: full message (44 chars max, no format args so % is literal) */
    {
        static char tmp[45];
        int i;
        for (i = 0; i < 44 && msg[i]; i++) tmp[i] = msg[i];
        tmp[i] = '\0';
        SRL::Debug::Print(0, 1, tmp);
    }
    console_enabled = 1;
    sat_console_putc('\n');
    while (*msg)
        sat_console_putc(*msg++);
    sat_console_putc('\n');
    console_redraw();
    /* 5s pause so user can read the error before the counter starts (300 vblanks) */
    for (int _w = 0; _w < 300; _w++) SRL::Core::Synchronize();
    {
        unsigned int n = 0;
        for (;;)
        {
            SRL::Debug::Print(0, 1, "FATAL loop n=%u", n++);
            SRL::Core::Synchronize();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zone memory                                                          */
/* ------------------------------------------------------------------ */

extern "C" unsigned char *DG_ZoneBase(int *size)
{
    *size = LOW_WORK_RAM_SIZE - RP_CMD_BUF_SIZE;
    return LOW_WORK_RAM;
}

/* ------------------------------------------------------------------ */
/* RAM cartridge setup + WAD loading                                   */
/* ------------------------------------------------------------------ */

static void cart_enable(void)
{
    *(volatile unsigned int *)0x25FE00B0  = 0x23301FF0;
    *(volatile unsigned int *)0x25FE00B8  = 0x00000013;
    *(volatile unsigned short *)0x257EFFFE = 1;
}

/* SATURN: probe the *usable* cart size in bytes (0x400000 / 0x200000 /
   0x100000), or 0 if absent/broken.  An Action Replay stuck in 1M mode (or
   any sub-4MB cart) mirrors its banks: writing four distinct sentinels one
   per MB and reading bank 0 back tells us the real size from the aliasing
   pattern.  The 3.94MB IWAD needs the full 4MB; a smaller cart must fall
   back to CD streaming, otherwise the truncated/aliased WAD is the black
   screen seen on real hardware. */
static unsigned int cart_probe_size(void)
{
    volatile unsigned int *b = (volatile unsigned int *)CART_RAM_UNCACHED;
    unsigned char id = *CART_ID_ADDR;
    const unsigned int MBW = 0x100000u / 4u;   /* 32-bit words per MB */

    printf("cart id: 0x%02x\n", id);
    if (id != CART_ID_4MB && id != CART_ID_1MB)
        return 0;

    b[0]       = 0xA5A50000u;
    b[1 * MBW] = 0xA5A50001u;
    b[2 * MBW] = 0xA5A50002u;
    b[3 * MBW] = 0xA5A50003u;

    /* liveness: bank 0 must read back one of the values we wrote */
    if (b[0] != 0xA5A50000u && b[0] != 0xA5A50001u &&
        b[0] != 0xA5A50002u && b[0] != 0xA5A50003u)
    {
        printf("cart probe: no writable RAM\n");
        return 0;
    }
    if (b[0] == 0xA5A50000u && b[1 * MBW] == 0xA5A50001u &&
        b[2 * MBW] == 0xA5A50002u && b[3 * MBW] == 0xA5A50003u)
        return 0x400000u;                      /* 4 independent MB banks */
    if (b[0] == 0xA5A50002u && b[1 * MBW] == 0xA5A50003u)
        return 0x200000u;                      /* mirrors every 2 MB     */
    return 0x100000u;                          /* mirrors every 1 MB     */
}

static void cache_purge(void)
{
    volatile unsigned char *ccr = (volatile unsigned char *)0xFFFFFE92;
    *ccr = (unsigned char)(*ccr | 0x10);
}

/* Step 4a (STREAMING_ANALYSIS §7.9): copy `len` bytes starting at CD `sector` of
   the open file `f` into cart RAM at byte offset `cart_ofs`, via the uncached
   write window, then purge the cache so the cached alias sees the new bytes.
   Returns bytes copied, 0 on failure / out-of-range.  Factored from load_wad's
   whole-file copy and reused by the per-map .DRP blob staging (w_drp_saturn.cxx).
   C++ linkage (both callers compile as C++); declared `extern` where used. */
int sat_cart_load_region(SRL::Cd::File &f, size_t sector, int len, unsigned int cart_ofs)
{
    if (len <= 0 || cart_ofs >= (unsigned int)CART_RAM_SIZE ||
        (unsigned int)len > (unsigned int)CART_RAM_SIZE - cart_ofs)
        return 0;
    int got = f.LoadBytes(sector, len, (void *)(CART_RAM_UNCACHED + cart_ofs));
    if (got <= 0) return 0;
    cache_purge();
    return got;
}

/* Load DOOM1.WAD from CD into cart RAM using SRL::Cd::File. */
static int load_wad(void)
{
    printf("loading DOOM1.WAD from CD...\n");
    SRL::Cd::File wad("DOOM1.WAD");
    if (!wad.Exists())
    {
        printf("DOOM1.WAD not found on CD\n");
        return 0;
    }
    wad.Open();

    /* Peek the 12-byte header to learn the WAD's TRUE size before committing
       to the cart.  The lump directory sits at the END of the WAD
       (infotableofs + numlumps*16); a WAD bigger than the 4MB cart would load
       directory-less (truncated) -> black screen.  Refuse here so the caller
       falls back to CD streaming (same guard already used for 1M/2M carts).
       The true size also drives an accurate load percentage below. */
    unsigned int total = (unsigned int)CART_RAM_SIZE;
    {
        unsigned char hdr[12];
        if (wad.LoadBytes(0, 12, hdr) >= 12)
        {
            int32_t numlumps     = (int32_t)(hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | (hdr[7]<<24));
            int32_t infotableofs = (int32_t)(hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | (hdr[11]<<24));
            unsigned int true_sz = (unsigned int)(infotableofs + numlumps * 16);
            if (true_sz > (unsigned int)CART_RAM_SIZE)
            {
                printf("WAD %u bytes > %u cart -- CD streaming\n",
                       true_sz, (unsigned int)CART_RAM_SIZE);
                wad.Close();
                return 0;
            }
            if (true_sz >= 12)
                total = true_sz;   /* exact size -> accurate %, and no over-read past EOF */
        }
    }

    /* Load in chunks so a live percentage shows on the boot screen -- the multi-MB
       CD read is the long wait before the menu.  Each sat_cart_load_region purges
       the cache for its range; the per-chunk purge over ~16 chunks is negligible
       next to the CD transfer itself.

       NB: sat_cart_load_region's first arg is a CD SECTOR (2048 bytes), not a byte
       offset.  CHUNK must stay a multiple of 2048 so `done` is always sector-aligned
       at the start of each read (we also break on any short read), letting done/2048
       give the exact start sector. */
    const unsigned int SECTOR = 2048u;
    int len = 0;
    {
        const unsigned int CHUNK = 128u * SECTOR;   /* 256 KB, sector-aligned */
        unsigned int done = 0;
        SRL::Debug::Print(0, 2, "LOADING WAD:   0%");
        while (done < total)
        {
            unsigned int want = total - done;
            if (want > CHUNK) want = CHUNK;
            int got = sat_cart_load_region(wad, (size_t)(done / SECTOR), (int)want, done);
            if (got <= 0) break;
            done += (unsigned int)got;
            SRL::Debug::Print(0, 2, "LOADING WAD: %3d%%", (int)(done * 100u / total));
            if ((unsigned int)got < want) break;   /* short read = EOF (WAD < cart) */
        }
        len = (int)done;
    }
    wad.Close();

    if (len <= 12)
    {
        printf("CD read failed (len=%d)\n", len);
        return 0;
    }
    /* cache already purged by sat_cart_load_region */
    sat_wad_base = CART_RAM_CACHED;
    sat_wad_size = (unsigned int)len;
    printf("WAD: %d bytes [%c%c%c%c]\n", len,
           sat_wad_base[0], sat_wad_base[1], sat_wad_base[2], sat_wad_base[3]);
    return sat_wad_base[0] == 'I' && sat_wad_base[1] == 'W';
}

/* ------------------------------------------------------------------ */
/* Clock: V-blank count + master FRT for sub-frame resolution          */
/* ------------------------------------------------------------------ */

#define FRT_TCR  (*(volatile unsigned char *)0xFFFFFE16)
#define FRT_FRCH (*(volatile unsigned char *)0xFFFFFE12)
#define FRT_FRCL (*(volatile unsigned char *)0xFFFFFE13)

static volatile unsigned int       vbl_count    = 0;
static volatile unsigned long long us_acc        = 0;
static volatile unsigned short     frt_at_vbl   = 0;
static unsigned int                us_per_frame  = 16683;
static unsigned int                ns_per_frt    = 4469;

static unsigned short frt_read(void)
{
    unsigned int sr, sr_masked;
    unsigned char h, l;
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    sr_masked = sr | 0xF0;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr_masked) : "memory");
    h = FRT_FRCH;
    l = FRT_FRCL;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
    return (unsigned short)((h << 8) | l);
}

extern "C" unsigned short sat_frt(void)
{
    return frt_read();
}

static void vblank_handler(void)
{
    vbl_count++;
    frt_at_vbl = frt_read();
    us_acc += us_per_frame;
    if (palette_dirty)
    {
        for (int i = 0; i < 256; i++)
            CRAM_DOOM_PAL[i] = pending_cram[i];
        palette_dirty = 0;
    }
    if (wbank_dirty)
    {
        for (int b = 0; b < WLIGHT_DARK_N; ++b)        /* dark light-banks 2..7 */
        {
            volatile unsigned short *c = CRAM_BANK(b + 2);
            const unsigned short    *s = pending_wbank[b];
            for (int i = 0; i < 256; ++i) c[i] = s[i];
        }
        wbank_dirty = 0;
    }
}

/* ------------------------------------------------------------------ */
/* doomgeneric interface                                               */
/* ------------------------------------------------------------------ */

extern "C" volatile int game_phase;
volatile int game_phase = 0;

/* VDP1 completion signal (set in the kick from EDSR CEF): did the previous frame's plot
   FINISH before this kick?  'D'one = VDP1 had headroom, 'B'usy = it overran the frame.
   2026-06-24: the old "always B" note was PRE-8bpp -- since the 8bpp wall pack (half the
   VDP1 writes) the list CAN finish within a frame, so D/B is a live, meaningful headroom
   signal again.  Row 2 shows D/B + Dr = the % of frames Done over the measurement window
   (vd1_win_done/vd1_win_tot, reset with the REC window) -- the go/no-go input for the
   VDP1-floor offload (high Dr => spare VDP1 budget for floor strips). */
static int vdp1_prev_done = 1;
static unsigned int vd1_win_done = 0, vd1_win_tot = 0;   /* VDP1 done-rate over the window */

#if SHOW_FPS
extern "C" int rp_timeout_count;
extern "C" unsigned int rp_master_ms;   /* master frame ms -> prefixes r_parallel.c's row-18 SLV line */
static unsigned int dg_frame_count = 0;
static int vdp1_last_cmds = 0;

/* SATURN PERF (2026-06-24): windowed REC stats exported by core/r_parallel.c (set on the
   ship path's rp_p3_prof_show), surfaced on the 1/s overlay tick + reset by RP_ProfReset
   when the config under test changes.  Defined unconditionally in r_parallel.c so they
   link with RP_PROF off (then 0). */
extern "C" int sat_prof_rec_max;                                 /* window max (= p100), tenths-ms */
extern "C" int sat_prof_pk_bw, sat_prof_pk_bp, sat_prof_pk_p, sat_prof_pk_m;  /* per-phase peaks */
extern "C" int sat_prof_mx_map, sat_prof_mx_x, sat_prof_mx_y, sat_prof_mx_ang, sat_prof_mx_t;
extern "C" int sat_prof_dom_pct, sat_prof_plane_n;               /* RBG0-floor sizer */
extern "C" int sat_prof_ss_n, sat_prof_ss_q, sat_prof_ss_qpk, sat_prof_ss_q4pct;  /* pari A sizing */
extern "C" int sat_prof_dropped;                                 /* glitch frames excluded from the window */
extern "C" int RP_ProfPercentile(int pct);                       /* windowed REC percentile, tenths-ms */
extern "C" void RP_ProfReset(void);
extern "C" int gamemap;   /* core doomstat: drives the per-map window reset */

/* SATURN PERF (2026-06-24): one-shot memory-latency calibration.  The memory-bound
   ceiling is the root cause of REC cost but is unmeasurable directly (no SH7604 PMU),
   so cold-read a 32 KB block from each work-RAM bank and FRT-time it.  rL = LWRAM/HWRAM
   ratio (>1.0 => LWRAM -- where the cmd buffer + visplanes live -- is the slow bank;
   this is the size of the L2-relocate / placement upside, quantified on THIS hardware).
   Read-only (non-destructive); 32 KB >> the 4 KB cache so it measures real RAM latency. */
static unsigned int mem_lw_ticks = 0, mem_hw_ticks = 0;
static inline unsigned int dg_mem_frt(void)
{
    unsigned char h = *(volatile unsigned char *)0xFFFFFE12;
    unsigned char l = *(volatile unsigned char *)0xFFFFFE13;
    return (unsigned short)((h << 8) | l);
}
static unsigned int dg_mem_bench(volatile unsigned int *base)
{
    /* MIN-of-N: a VBlank IRQ (every ~16ms) landing inside a ~2.5ms read inflates it --
       that made the Ymir ratio swing 0.7<->1.2 (lw/hw swapped, total ~constant = the IRQ
       in one bench or the other).  The MIN over N reads = the IRQ-free run = true latency. */
    unsigned int best = 0xffffffffu;
    for (int rep = 0; rep < 8; rep++) {
        volatile unsigned int sink = 0;
        unsigned int t0 = dg_mem_frt();
        for (int i = 0; i < 8192; i++) sink += base[i];   /* 32 KB read (>> the 4 KB cache) */
        (void)sink;
        unsigned int dt = (unsigned short)(dg_mem_frt() - t0);   /* < 65536 -> no wrap */
        if (dt < best) best = dt;
    }
    return best;
}
static void dg_mem_calibrate(void)
{
    mem_lw_ticks = dg_mem_bench((volatile unsigned int *)0x00200000);  /* LWRAM (slow DRAM) */
    mem_hw_ticks = dg_mem_bench((volatile unsigned int *)0x06000000);  /* HWRAM (fast SDRAM) */
}

static void fps_update(void)
{
    static unsigned int t0     = 0;
    static unsigned int frames = 0;
    unsigned int now = vbl_count;
    unsigned int hz  = (us_per_frame == 20000) ? 50 : 60;

    frames++;
    if (now - t0 >= hz)
    {
        unsigned int elapsed = now - t0;
        /* tenths of an fps, for resolution at 5-10 fps; EMA (~4s) for a stable
           average to compare builds with. */
        unsigned int inst10 = (frames * 10u * hz + elapsed / 2) / elapsed;
        static unsigned int avg10 = 0;
        avg10 = avg10 ? (avg10 * 3 + inst10) / 4 : inst10;
        /* one-shot memory-latency calibration on the first 1/s tick (a single ~30ms
           hitch at startup, off the render path) -> row 18. */
        static int mem_done = 0;
        if (!mem_done) { dg_mem_calibrate(); mem_done = 1; }
        /* SATURN PERF (2026-06-24): auto-reset the windowed stats (REC histogram p50/p95,
           per-phase peaks, floor sizers, VDP1 done-rate) whenever the variable under test
           changes (new map / potato / visplane-hash / blit config),
           so each A/B run starts a clean min/avg/max window -- no manual button needed
           (the pad is already saturated: Y=hash X=split Z=potato L+R=blit). */
        {
            static int l_map=-1, l_pot=-1, l_blit=-1;
#if SAT_FLOOR_PERFSIM
            static int l_perfsim=-1;   /* reset the REC window on a pad-Y perf-sim toggle => clean per-mode numbers */
#endif
#if SAT_DIAG_SLAVE_TOGGLES
            static int l_steal=-1, l_wp=-1;
#endif
            if (gamemap != l_map || (int)potato_level != l_pot || blit_mode != l_blit
#if SAT_FLOOR_PERFSIM
                || floor_perfsim_mode != l_perfsim
#endif
#if SAT_DIAG_SLAVE_TOGGLES
                || sat_plane_steal != l_steal || sat_wallprep_slave != l_wp
#endif
               ) {
                RP_ProfReset();
                vd1_win_done = vd1_win_tot = 0;
                l_map=gamemap; l_pot=(int)potato_level; l_blit=blit_mode;
#if SAT_FLOOR_PERFSIM
                l_perfsim=floor_perfsim_mode;
#endif
#if SAT_DIAG_SLAVE_TOGGLES
                l_steal=sat_plane_steal; l_wp=sat_wallprep_slave;
#endif
            }
        }
        /* OVERLAY 2026-06-24 (audited): useful values packed onto the top rows; cut the
           WAD line, the F/ph/vbl/gt heartbeats, and the VD2~/SCU/68K nominal labels.
           Row 0 = HEADLINE: inst fps, EMA(~4s) avg (THE build-comparison number), and
           to = slave-timeout count (must stay 0 -- the RP_CMD_BUF-shrink safety);
           cd = CD read-retries (whackCD): 0 = clean disc, climbing = flaky reads. */
        extern int sat_cd_read_retries;   /* w_file_saturn.cxx */
        static char r0[45];
        sprintf(r0, "%u.%u fps avg%u.%u to%d cd%d",
                inst10 / 10, inst10 % 10, avg10 / 10, avg10 % 10,
                rp_timeout_count, sat_cd_read_retries);
        SRL::Debug::Print(0, 0, r0);
        /* row 1: vp = visplane peak (MAXVISPLANES sizing) + the live pad toggles:
           potato mode (pad Z), blit config bl + measured blit ms b (pad L+R, dual-CPU
           GO/DROP), visplane-hash h (pad Y). */
        static char r1[45];
        /* cc = SH-2 master CCR @0xFFFFFE92 low nibble (CP bit4 reads 0): bit0 CE=cache enable,
           bit1 ID, bit2 OD, bit3 TW (0=4-way / 1=2-way+2KB-RAM).  Expect 01 = enabled 4-way; if
           it's 00 (disabled) or 09 (2-way) the warm-cache work is moot / a separate free win. */
        unsigned ccr = (unsigned)(*(volatile unsigned char *)0xFFFFFE92) & 0x0Fu;
#if SAT_DIAG_SLAVE_TOGGLES
        sprintf(r1, "vp%3d %-7s bl%d b%u.%u ws%d wp%d cc%02x", r_visplane_peak,
                potato_modes[potato_level].name, blit_mode,
                sat_blit_ms10 / 10, sat_blit_ms10 % 10, sat_plane_steal, sat_wallprep_slave, ccr);
#else
        sprintf(r1, "vp%3d %-7s bl%d b%u.%u cc%02x", r_visplane_peak,
                potato_modes[potato_level].name, blit_mode,
                sat_blit_ms10 / 10, sat_blit_ms10 % 10, ccr);
#endif
        SRL::Debug::Print(0, 1, r1);
        {
            /* SATURN VALIDATION row: RAM-lever sizing (all high-water, Ymir-honest).
               hp = newlib heap peak/cap (#4: trim HEAP_SIZE to peak+margin).
               cov = peak sum of live-plane spans (#1: a tight pooled top-arena needs
                     ~cov bytes, +cov for bottom => arena ~= 2*cov; compare to today's
                     332KB pool to see #1's ceiling).  pool = #1 span-pool peak bytes
                     (0 unless SAT_VISPLANE_POOL=1).  vp (row 2) sizes #2 MAXVISPLANES. */
            /* TEX = Phase-0 texturecolumnlump floor measurement (one-shot at load,
               docs/TEXTURECOLUMNLUMP_PLAN.md): nt=numtextures, w=total columns (Sum
               width), d=the columnlump+columnofs PU_STATIC floor in KB (=4*w), mp=#
               multi-patch textures.  Confirms the ~400-600K floor on real hardware.
               (Replaced the VAL visplane-sizing telemetry -- that lever shipped.) */
            static char rV[45];
            sprintf(rV, "TEX nt%d w%d d%dK mp%d",
                    sat_tex_numtex, sat_tex_sumwidth, sat_tex_dirbytes >> 10, sat_tex_mptex);
            SRL::Debug::Print(0, 8, rV);     /* memory levers grouped on rows 6-8 */
            /* split-block breakdown + LIVE mode: P = player count (sat_local_players), W = wall
               mode V/S/- (V=VDP1 walls all views, S=all software, -=1p inert), v0..v3 = each
               R_RenderPlayerView ms, k = the VDP1 kick, bk = wall-texture re-bakes (last frame;
               0 in 1p).  Surfaces the pad-X VDP1<->software A/B + which path actually ran. */
            static char rS[45];
            char wmode = (sat_local_players <= 1) ? '-' : (sat_split_vdp1 ? 'V' : 'S');
            snprintf(rS, sizeof rS, "SPL P%d %c v0%u v1%u v2%u v3%u k%u bk%u",
                    sat_local_players, wmode, sat_spl_v0, sat_spl_v1,
                    sat_spl_v2, sat_spl_v3, sat_spl_kick, wtex_bakes);
            SRL::Debug::Print(0, 16, rS);   /* split only -> kept lower */
            /* streaming texture cache (core/r_cache.c): a=active (0 in split/non-stream),
               <pool>K size, e=live composites, b=builds this level, x=evictions.  a1 + b>0
               = the LRU is doing work; a0 = inactive (2p split / shareware / cart mode). */
            /* TXC = streaming texture cache (core/r_cache.c): a=active, <pool>K size,
               e=live composites, b=builds, x=evicts.  lf = largest free block (KB) the
               carve saw -- a0 with lf below ~88K means the zone was too tight to slice
               the slab (64K margin + 24K min), so the cache is BLOCKED ON THE FLOOR
               (the TEX-row cut is what frees it), not a measurement bug. */
            static char rTC[45];
            sprintf(rTC, "TXC a%d %dK e%d b%d x%d lf%dK",
                    sat_texcache_active, sat_texcache_poolkb, sat_texcache_entries,
                    sat_texcache_builds, sat_texcache_evicts, sat_texcache_carve_lf);
            SRL::Debug::Print(0, 6, rTC);
            /* LWRAM zone pressure (the Z_Malloc OOM lever): fr = total reclaimable
               (free+purgeable); mx = largest CONTIGUOUS run after a purge.  At an
               'Z_Malloc: failed on N' crash: mx < N while fr >> N => FRAGMENTATION;
               fr < N => true EXHAUSTION. */
            static char rZ[45];
            sprintf(rZ, "ZON fr%dK mx%dK", Z_FreeMemory() >> 10, Z_LargestAllocatable() >> 10);
            SRL::Debug::Print(0, 7, rZ);
        }
        /* master frame ms (the synchronous bottleneck) -> prefixes r_parallel.c's row-18
           SLV line; set the shared value here (the AVG row is gone, fps is on row 0). */
        rp_master_ms = inst10 ? (10000u / inst10) : 0;   /* frame ms */
        {
            /* row 2: VDP1 load + done-rate + build stamp.  VD1 = cmds this frame + D/B
               (EDSR-CEF this frame) + Dr = % of plotted frames Done over the window.
               Post-8bpp the VDP1 CAN finish within a frame, so Dr is a live VDP1-floor
               headroom signal (high Dr => spare budget for floor strips).  b:__TIME__ =
               build stamp (build.ps1 touches this file so it refreshes). */
            unsigned int dr = vd1_win_tot ? (vd1_win_done * 100u / vd1_win_tot) : 0;
            static char r2v[45];
            snprintf(r2v, sizeof r2v, "VD1 %d%c Dr%u%% b:" __TIME__,
                    vdp1_last_cmds, vdp1_prev_done ? 'D' : 'B', dr);
            SRL::Debug::Print(0, 2, r2v);
            /* row 4: WINDOWED REC distribution p50/p95/max (tenths-ms) -- robust to the
               single-outlier max (a lone CD hitch) AND to an arbitrary threshold.  p50 =
               typical, p95 = sustained worst, mx = absolute worst (located on row 9).
               Window auto-resets on a config change (above).  ~29 cols. */
            static char r4[45];
            int p50 = RP_ProfPercentile(50), p95 = RP_ProfPercentile(95);
            snprintf(r4, sizeof r4, "REC p50 %d.%d p95 %d.%d mx%d.%d d%d   ",
                    p50/10, p50%10, p95/10, p95%10,
                    sat_prof_rec_max/10, sat_prof_rec_max%10, sat_prof_dropped);
            SRL::Debug::Print(0, 4, r4);
            /* row 10: per-PHASE INDEPENDENT peaks (each phase's own worst across the
               window, possibly different frames) -- the basis to size each offload
               (Bp -> slave wall-prep, P -> VDP1/RBG0 floor).  ~31 cols worst case. */
            static char r10[45];
            snprintf(r10, sizeof r10, "PK Bw%d.%d Bp%d.%d P%d.%d M%d.%d        ",
                    sat_prof_pk_bw/10, sat_prof_pk_bw%10, sat_prof_pk_bp/10, sat_prof_pk_bp%10,
                    sat_prof_pk_p/10,  sat_prof_pk_p%10,  sat_prof_pk_m/10,  sat_prof_pk_m%10);
            SRL::Debug::Print(0, 10, r10);
            /* row 9: WHERE/WHEN the REC-max frame was (the locator), so the worst frame is
               reproducible.  m=map, x,y=player render pos (map units), a=angle 0-255,
               t=sec into the level.  ~31 cols worst case (6-digit coords). */
            static char r9[45];
            snprintf(r9, sizeof r9, "MX m%d %d,%d a%d t%ds        ",
                    sat_prof_mx_map, sat_prof_mx_x, sat_prof_mx_y, sat_prof_mx_ang,
                    sat_prof_mx_t/35);
            SRL::Debug::Print(0, 9, r9);
            /* row 17: FLOOR offload sizers.  Vs/Vp = VDP1-floor candidate quad count this
               frame / window peak (go/no-go: GO if Vp fits the VDP1 cmd budget).  d% = the
               dominant single-flat share of plane pixels, n = visplane count (RBG0 sweet
               spot = high d% + low n => one flat owns the floor).  ~24 cols. */
            static char r17[45];
            snprintf(r17, sizeof r17, "FLR Vs%d Vp%d d%d%% n%d        ",
                    sat_floor_vq_cur, sat_floor_vq_peak, sat_prof_dom_pct, sat_prof_plane_n);
            SRL::Debug::Print(0, 17, r17);
            /* row 20 (pari A sizing): "all floors+ceilings as VDP1 quads" (PowerSlave model).
               ss = visible subsectors, Q = geometry quad count this frame (fan pieces,
               UNtextured -> texture tiling would multiply), Qp = window peak, q4 = % of
               surfaces from <=4-sided (pure-quad) subsectors. */
            static char r20[45];
            snprintf(r20, sizeof r20, "PAR ss%d Q%d Qp%d q4%d%%      ",
                    sat_prof_ss_n, sat_prof_ss_q, sat_prof_ss_qpk, sat_prof_ss_q4pct);
            SRL::Debug::Print(0, 20, r20);
            /* row 18: memory-latency calibration (one-shot cold 32 KB read per bank, FRT
               ticks).  rL = LWRAM/HWRAM ratio -- >1.0 means LWRAM (cmd buf + visplanes) is
               the slow bank, = the memory-bound penalty + the L2-relocate upside, measured
               on THIS hardware (Ymir will read ~1.0 -- it does not model the bank gap). */
            unsigned int rL10 = mem_hw_ticks ? (mem_lw_ticks * 10u / mem_hw_ticks) : 0;
            static char r18[45];
            snprintf(r18, sizeof r18, "MEM lw%u hw%u rL%u.%u        ",
                    mem_lw_ticks, mem_hw_ticks, rL10/10, rL10%10);
            SRL::Debug::Print(0, 18, r18);
#if SAT_FLOOR_PERFSIM
            /* row 19: pad-Y floor perf-sim mode.  Read REC (row 4) / P (row 5) in each mode;
               the delta vs mode 0 = the floor-offload saving (same for RBG0/VDP1/gradient). */
            static const char *const perfsim_name[4] = {
                "0 NORMAL", "1 DOM-ABSENT(vdp2)", "2 ALL-BUT-DOM(vdp1)", "3 BOTH" };
            static char r19[45];
            snprintf(r19, sizeof r19, "PERFSIM %s        ", perfsim_name[floor_perfsim_mode & 3]);
            SRL::Debug::Print(0, 19, r19);
#endif
            /* row 13: sky-vs-floor coverage classifier (Romain).  sky/flr = pixels the sky / the
               dominant floor cover this frame.  flr is non-zero only in a perf-sim floor-on mode
               (pad-Y 1/3).  Big sky% => the HW-sky bank earns its keep; tiny sky% => it's wasted,
               free it for a textured VDP2 floor.  W = which covers more (S=sky, F=floor). */
            static char r13[45];
            snprintf(r13, sizeof r13, "CLS sky%u flr%u %c       ", sat_sky_px, sat_floor_px,
                    (sat_sky_px >= sat_floor_px) ? 'S' : 'F');
            SRL::Debug::Print(0, 13, r13);
        }
#if VDP2_RBG0_TEST
        {
            /* row 14: RBG0 RAMCTL commit readback (visible in pad-Y debug modes 1/2).
               b = chip RAMCTL before our RDBS write, a = after.  Low byte should read
               0x8D (A0=coeff A1=char B0=fb B1=PN); bits 8-9 = 4-bank split.  If a != that
               or snow persists, the rotation banks' CYC pattern also needs writing. */
            static char rR[45];
            sprintf(rR, "RAMCTL b=%04X a=%04X", ramctl_before, ramctl_after);
            SRL::Debug::Print(0, 14, rR);
        }
#endif
#ifdef SAT_REPACK
        {
            /* row 21: per-level repack (.DRP) status (STREAMING_ANALYSIS.md §7.9-7.11).
               ON => DOOMRP.DRP validated for streaming; s = lumps served from the blob
               this session (grows as you play => the loader is working); r = .DRP read
               retries.  Step 4b: "CART<kb>k" = this map's blob staged in cart RAM (CD
               idle -> CDDA); "cd" = served from CD (no 4MB cart / blob too big / map not
               in .DRP).  off code: -1 cart/not-stream, -2 no file, -3 hdr, -4 CRC, -5 tbl. */
            extern int sat_drp_state, sat_drp_n_maps, sat_drp_served, sat_drp_read_retries;
            extern int sat_drp_cart, sat_drp_cart_kb;   /* Step 4b: cart-staging status */
            static char r21[45];
            if (sat_drp_state == 1)
                snprintf(r21, sizeof r21, "DRP ON m%d s%d r%d %s%dk",
                         sat_drp_n_maps, sat_drp_served, sat_drp_read_retries,
                         sat_drp_cart ? "CART" : "cd", sat_drp_cart_kb);
            else
                snprintf(r21, sizeof r21, "DRP off (%d)         ", sat_drp_state);
            SRL::Debug::Print(0, 21, r21);
        }
#endif
        /* (cut: the "WAD:" identity line -- served its purpose, owner flagged it useless.) */
        t0     = now;
        frames = 0;
    }
}
#endif

#if VDP2_RBG0_TEST
/* RBG0 floor prototype -- Phase-0 bring-up (docs/RBG0_FLOOR_PLAN.md).  Cell-based
   rotation plane: one recognizable 8x8 tile, repeated across a 1-page plane, with an
   IDENTITY ROTSCROLL (flat 1:1 map, no perspective, no coefficient table).  Lives in
   the free VRAM bank A1.  Shown at priority 5 (> sky 4, < game NBG1 6) so it appears
   through the index-0 sky/ceiling region while the game still draws on top -- the test
   is purely "does RBG0 light up without disturbing the raw-SGL NBG0/NBG1 cycle
   pattern?".  Must be set up AFTER slBitMapNbg0/1 in DG_Init. */
/* Drive RBG0's rotation parameters from the SGL matrix stack into SRL's FIXED VRAM
   rotation-parameter table (Core::Initialize set it at VDP2_VRAM_B1+0x1ff00 via
   slRparaInitSet -- srl_vdp2.hpp:1529).  slScrMatSet writes the rpara straight to VRAM,
   so this needs no slSynch.  Phase-0 = OneAxis (flat, NO coefficient table): a plain
   translate places the plane; perspective (slRotX + a TwoAxis K-table) is Phase-1.
   Mirrors the working SRL sample's SetCurrentTransform (Samples/VDP2 - RBG0 Rotation). */
static void rbg0_set_transform(void)
{
    /* Mode-7 GROUND matrix: rotate the plane 90deg about X so it
       lies flat = the floor, then translate to the camera height/position.  slScrMatConv
       folds the perspective in; slScrMatSet writes the rpara to VRAM (no slSynch needed).
       Values are a first test -- tune the height (z) + position once it's on screen. */
    slPushMatrix();
    {
        slRotX((ANGLE)(0x4000 + RBG0_PITCH + rbg0_pitch_adj)); /* 90deg + pitch (+ live R+up/down nudge) */
        slRotZ((ANGLE)(-(int)(viewangle >> 16) + RBG0_YAW_OFF)); /* yaw track + baked 90deg flat orientation */
        /* viewx/viewy are fixed_t (16.16) in map units; slTranslate's FIXED is also 16.16,
           so passing them directly scrolls the floor by the player's map position (1 unit ->
           1 texel for a 64-unit flat).  Z = eye height above the player's floor (viewz -
           floor height) so the plane sits EXACTLY on the floor the player stands on, and
           follows it up/down stairs.  Signs/scale tune with the real texture. */
        slTranslate(-viewx, -viewy, -(viewz - sat_vdp2_floor_h) + rbg0_z_adj); /* +live R+left/right level */
        slCurRpara(RA);
        slScrMatConv();
        slScrMatSet();
    }
    slPopMatrix();
}

extern "C" unsigned char *sat_vdp2_floor_data(void);

/* Swizzle the player's-floor Doom flat (64x64, from sat_vdp2_floor_data) into the RBG0
   8x8 cells, shaded through the floor sector's colormap (sat_vdp2_floor_cmap) so the
   hardware floor dims with the room.  Re-uploads only when the flat OR the light level
   changes (~0.6ms = 4096 VDP2-VRAM bytes; cheap even in flicker sectors).  Cell (cx,cy)
   at index cy*8+cx, pixels row-major; the map (rbg0_proto_init) references char# = idx*2. */
static void rbg0_upload_flat(int picnum)
{
    static int loaded = -2;
    static const unsigned char *loaded_cmap = (const unsigned char *)1;
    const unsigned char *cmap = sat_vdp2_floor_cmap;
    if (picnum < 0) return;
    if (picnum == loaded && cmap == loaded_cmap && !rbg0_tex_dirty) return;
    const unsigned char *flat = sat_vdp2_floor_data();
    if (!flat) return;
    loaded = picnum;
    loaded_cmap = cmap;
    rbg0_tex_dirty = 0;
#if RBG0_BITMAP
    /* Build each 512-wide bitmap row in a STACK buffer (cached), then bulk-memcpy to the
       uncached VRAM (A1).  Avoids 131072 slow per-byte uncached writes (= the slowdown) and
       uses no static .bss (a 4KB static here starved the TLSF pool = the old boot-loop).
       The flat is re-oriented HERE (rbg0_tex_orient = one of the 8 D4 symmetries, pad Y) plus
       a live texel offset (rbg0_tex_xoff/yoff, pad L+d-pad), so the texture aligns -- rotation
       AND mirror -- WITHOUT a yaw offset (which would invert the scroll direction).  A MIRROR
       (det -1) is needed, not a rotation, when the lit corner reads flipped.  slOverRA tiles. */
    unsigned char row[512];
    unsigned char *bmp = (unsigned char *)RBG0_BMP_VRAM;
    for (int y = 0; y < 256; ++y)
    {
        int by = (y & 63);
        for (int x = 0; x < 64; ++x)
        {
            int u = (x  + rbg0_tex_xoff) & 63;     /* texel offset (pad L + d-pad) */
            int v = (by + rbg0_tex_yoff) & 63;
            int fx, fy;                            /* D4 orientation (pad Y)       */
            switch (rbg0_tex_orient & 7) {
                default:
                case 0: fx = u;      fy = v;      break;  /* identity       */
                case 1: fx = v;      fy = 63 - u; break;  /* rot 90         */
                case 2: fx = 63 - u; fy = 63 - v; break;  /* rot 180        */
                case 3: fx = 63 - v; fy = u;      break;  /* rot 270        */
                case 4: fx = 63 - u; fy = v;      break;  /* mirror H       */
                case 5: fx = u;      fy = 63 - v; break;  /* mirror V       */
                case 6: fx = v;      fy = u;      break;  /* transpose      */
                case 7: fx = 63 - v; fy = 63 - u; break;  /* anti-transpose */
            }
            unsigned char px = flat[fy * 64 + fx];
            row[x] = cmap ? cmap[px] : px;
        }
        for (int t = 1; t < 8; ++t) memcpy(row + t * 64, row, 64); /* tile 64 -> 512          */
        memcpy(bmp + y * 512, row, 512);                           /* bulk write to VRAM      */
    }
#else
    unsigned char *cel = (unsigned char *)RBG0_CEL_VRAM;
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx)
        {
            unsigned char *c = cel + (cy * 8 + cx) * 64;
            for (int ry = 0; ry < 8; ++ry)
                for (int rx = 0; rx < 8; ++rx)
                {
                    unsigned char px = flat[(cy * 8 + ry) * 64 + (cx * 8 + rx)];
                    c[ry * 8 + rx] = cmap ? cmap[px] : px;
                }
        }
#endif
}

#if RBG0_LINECOL_TEST
/* Per-distance floor light, RUNG A (FLAT darken proof) -- PARKED default-OFF, toggle pad C.
   The VDP2 LINE-COLOR SCREEN blended into RBG0 only (color-calc) darkens the whole floor with a
   single near-black line color.  GOTCHA (cost a build): do NOT add LNCLON to the slScrAutoDisp
   (BGON) mask -- it broke NBG1 (the whole software framebuffer vanished); the line-color DISPLAY
   is enabled by slLineColDisp(LNCLON) alone.  Enable regs (LCTAU 0xA8/LCTAL 0xAA, LNCLEN 0xE8,
   CCCTL 0xEC) are INSIDE the 0x0E..0xFE block-flush; the RATIO reg CCRR (0x10C) is OUTSIDE it ->
   direct-poke.  Pad C toggles the ratio (off<->dark).  NEXT (future session): RUNG C = a per-line
   line-color table (in spare VRAM) for the real DISTANCE gradient instead of this flat darken. */
static int rbg0_linecol_on = 0;     /* default OFF: the committed floor is unchanged; C opts in */
static void rbg0_linecol_apply(void)
{
    slLine1ColSet((void *)RBG0_KTAB_VRAM, (unsigned short)0x8000);  /* one near-black line color (MSB=insert) */
    slLineColDisp(LNCLON);                                          /* enable the line-color screen           */
    slColorCalc(0);                                                 /* CC_RATE | CC_TOP: ratio mode, top pixel */
    slColorCalcOn(RBG0ON);                                          /* RBG0 ONLY -> NBG1/HUD untouched         */
    *(volatile unsigned short *)0x25F8010C =                        /* CCRR: RBG0 color-calc ratio (outside flush) */
        (unsigned short)(rbg0_linecol_on ? 24 : 0);
}
#endif

static void rbg0_proto_init(void)
{
#if RBG0_BITMAP
    /* SlaveDriver-inspired BITMAP RBG0 (PLAX.C initPlax): bitmap (char) 512x256 in A1, the
       coefficient/rotation table in A0, OVER_0 (repeat), perspective via the coefficient
       table.  NO pattern-name map.  Same banks as the BOOTING cell path (A1 char / A0 K).
       NBG3 debug is fully off (B1 holds only the RPT now). */
    memset((void *)RBG0_BMP_VRAM, 0, 512 * 256);          /* zero the whole bitmap (A1)         */
    slRparaMode(K_CHANGE);                                 /* rpara mode FIRST (SRL/SlaveDriver order) */
    slOverRA(0);                                            /* OVER_0: repeat (wrap the bitmap)   */
    slBitMapRbg0(COL_TYPE_256, BM_512x256, RBG0_BMP_VRAM); /* A1 bitmap; CHCTLB BMPMD=1          */
    slBMPaletteRbg0(1);
    slMakeKtable(RBG0_KTAB_VRAM);                          /* A0 coefficient table               */
    slKtableRA(RBG0_KTAB_VRAM, K_FIX | K_LINE | K_2WORD | K_ON);
#if RBG0_LINECOL_TEST
    rbg0_linecol_apply();   /* RUNG A: line-color screen + RBG0 color-calc (flat); no K_LINECOL yet */
#endif
    /* NO slPageRbg0/slPlaneRA (cell-only).  CRITICAL (root cause, disasm): slBitMapRbg0 does
       NOT call rbank_set -> the A1 bitmap bank is never registered in the RAMCTL RDBS shadow.
       We reserve RDBS (A1=char/A0=coeff -> 0x0D) AND park the A0/A1 rotation cycle slots BY
       HAND below (rbg0_commit_ramctl + rbg0_commit_cyc), and do NOT use slSynch -- it would
       recompute the cycle pattern from the inconsistent shadow (= the boot-loop).  Mirrors
       SlaveDriver's explicit vramA0=K/vramA1=CHAR + parked-0xEEEE cycle table. */
#else
    /* 1) cells filled per-flat by rbg0_upload_flat(); zero them for the pre-first-flat frame. */
    memset((void *)RBG0_CEL_VRAM, 0, 64 * 64);
    /* 2) pattern-name table (1-WORD) tiling the flat's 8x8 cell grid, palette 1, map in B1. */
    {
        unsigned short *map = (unsigned short *)RBG0_MAP_VRAM;
        for (int my = 0; my < 64; ++my)
            for (int mx = 0; mx < 64; ++mx)
            {
                int cellidx = (my & 7) * 8 + (mx & 7);
                map[my * 64 + mx] = (unsigned short)((cellidx * 2) | 0x1000);
            }
    }
    /* 3) RBG0 cell config + per-line coefficient table (Mode-7 perspective). */
    slOverRA(0);
    slCharRbg0(COL_TYPE_256, CHAR_SIZE_1x1);
    slPageRbg0(RBG0_CEL_VRAM, 0, PNB_1WORD | CN_12BIT);
    slPlaneRA(PL_SIZE_1x1);
    sl1MapRA(RBG0_MAP_VRAM);
    slMakeKtable(RBG0_KTAB_VRAM);
    slKtableRA(RBG0_KTAB_VRAM, K_FIX | K_LINE | K_2WORD | K_ON);
    slRparaMode(K_CHANGE);
    slBMPaletteRbg0(1);
#endif

    /* 4) DRIVE THE ROTATION FROM THE MATRIX, NOT BY HAND.  We do NOT call slRparaInitSet:
       SRL::Core::Initialize already pointed the rotation-param table at VRAM (B1+0x1ff00).
       Pointing it at a RAM struct (the old code) was THE black bug -- VDP2 read the
       rotation from a RAM address -> garbage -> the plane collapsed to a single point ->
       uniform opaque black.  Set an initial transform so it shows before frame 1;
       DG_DrawFrame re-sets it each frame (rbg0_set_transform). */
    rbg0_set_transform();

    slPriorityRbg0(4);           /* sky(3) < RBG0 floor(4) < VDP1 walls(5) < NBG1 game(6):
                                    the walls cleanly occlude the infinite floor's overspill
                                    (no priority tie), so it shows ONLY on the player's floor */
}
#endif

#if VDP2_RBG0_TEST
/* Direct-to-chip RAMCTL commit -- the cycle-pattern piece SGL would push inside slSynch,
   which we cannot call (it corrupts our no-slSynch VDP2/sound setup, HW-tested worse).
   Sets the rotation-data-bank-select (RDBS) so the VDP2 rotation engine reads RBG0 from
   the right banks: A0=coefficient(K), A1=character(cells), B1=pattern-name(map),
   B0=framebuffer(normal).  Without it, RBG0ON in BGON (re-pushed each vblank by SGL's IRQ
   handler) makes the rotation engine read unassigned banks -> whole-screen "snow".
   RDBS encoding decoded from Jo's NOSGL RAMCTL=0x1327: 1=coeff, 2=pattern-name,
   3=character.  Byte = (B1=2)<<6 | (B0=0)<<4 | (A1=3)<<2 | (A0=1) = 0x8D.  Bits 8-9 =
   VRAMD|VRBMD (4-bank split); bits 10-15 (CRMD/CRKTE) preserved. */
static void rbg0_commit_ramctl(void)
{
    volatile uint16_t *const RAMCTL = (volatile uint16_t *)0x25F8000E;
    ramctl_before = *RAMCTL;
    /* RDBS low byte = (B1)<<6 | (B0)<<4 | (A1)<<2 | (A0).  1=coeff, 2=pattern-name, 3=char/bitmap. */
#if RBG0_BITMAP
    uint16_t v = (uint16_t)((ramctl_before & 0xFC00u) | 0x0300u | 0x000Du);  /* A1=char/bitmap(3), A0=coeff(1), B1=0 */
#else
    uint16_t v = (uint16_t)((ramctl_before & 0xFC00u) | 0x0300u | 0x008Du);  /* cell: + B1=pattern-name(2) = 0x8D */
#endif
    *RAMCTL = v;
    VDP2_RAMCTL = v;   /* shadow-coherent: survive a possible per-vblank ISR re-push (RBG0 snow fix) */
    ramctl_after = *RAMCTL;
    printf("RAMCTL before=%04x after=%04x (rbg0 RDBS commit)\n",
           ramctl_before, ramctl_after);
}

/* Commit the FULL RBG0 register set, not just RAMCTL/CYC (docs/RBG0_SNOW_FIX_PLAN.md + HW 2026-06-26).
   The cycle pattern alone did NOT kill the snow: with CYC correct (fb reads + clean rotation banks)
   and RDBS committed, RBG0 still snows.  Reason: RBG0 also needs its rotation-parameter-table address
   (RPTAU/L @0xB8), coefficient table (KTCTL/KTAOF @0xB0), map registers (0x40-0x5E), CHCTLB, plane
   size, priorities -- SGL set them ALL in its shadow but they're never flushed to the chip (no
   slSynch).  So the rotation engine reads its transform/coeff from garbage chip addresses -> snow.
   Fix = what slSynch does, minus slSynch: BLOCK-FLUSH the shadow VDP2 register image -> the chip.
   The shadow is a contiguous register image; base = &VDP2_RAMCTL - 0x0E (RAMCTL is chip offset 0x0E).
   We flush 0x0E..0xFE (skip the display/status regs 0x00-0x0C). */
static void rbg0_commit_cyc(void)
{
#if RBG0_BITMAP
    /* THE FIX (disasm + SlaveDriver): slBitMapRbg0 never reserved the A1 bitmap bank, so SGL's
       auto-cycle leaves A0/A1 inconsistent -> the rotation engine/ISR walks a bad bank map ->
       address error -> boot-loop.  Park BOTH rotation banks (A0=coeff, A1=bitmap) at 0xEEEE,
       exactly like SlaveDriver's cycle[] table; B1 = normal (RPT). */
    VDP2_CYCA0L = 0xEEEE; VDP2_CYCA0U = 0xEEEE;
    VDP2_CYCA1L = 0xEEEE; VDP2_CYCA1U = 0xEEEE;
#if RBG0_NBG3
    /* NBG3 on: leave CYCB1 as slScrAutoDisp(NBG3ON) built it (NBG3 font+map reads live in B1). */
#else
    VDP2_CYCB1L = 0xFEEE; VDP2_CYCB1U = 0xEEEE;   /* NBG3 off: scrub the stale NBG3 read SGL left */
#endif
#else
    /* first correct the stale NBG3 reads SGL left in CYCB1's shadow (B1 is now the RBG0 map) */
    VDP2_CYCB1L = 0xFEEE; VDP2_CYCB1U = 0xEEEE;
#endif
    volatile uint8_t *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    volatile uint8_t *const chip   = (volatile uint8_t *)0x25F80000;
    for (int off = 0x0E; off <= 0xFE; off += 2)
        *(volatile uint16_t *)(chip + off) = *(volatile uint16_t *)(shadow + off);
    for (int b = 0; b < 4; ++b) {                                          /* snapshot CYC for readout */
        volatile uint16_t *s = (volatile uint16_t *)(shadow + 0x10 + b * 4);
        cyc_before[b] = ((uint32_t)s[0] << 16) | (uint32_t)s[1];
    }
}

/* 8x8 hex font (0-F), 1 byte/row, MSB = leftmost pixel. */
static const unsigned char rbg0_hexfont[16][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
};
static void rbg0_puthex(int px, int py, uint32_t val, int ndig)
{
    for (int i = 0; i < ndig; ++i) {
        const unsigned char *g = rbg0_hexfont[(val >> ((ndig - 1 - i) * 4)) & 0xF];
        int gx = px + i * 8;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                if ((g[r] >> (7 - c)) & 1)
                    DOOM_VRAM[(py + r) * DOOM_VRAM_STRIDE + gx + c] = 254;
    }
}
/* Hex dump of the chip RAMCTL/CYC registers straight into the framebuffer (NBG1, bank B0 -- the one
   bank RBG0 never touches, so it survives when the NBG3 overlay dies on B1).  5 lines, before|after.
   Readable even WITH the RBG0 snow behind it (NBG1 prio 6 > RBG0 prio 4).  Holds ~3s for the photo.
   Line order top->bottom: RAMCTL / CYCA0 / CYCA1 / CYCB0 / CYCB1.  CYCB0 (4th) must read 55EEEEEE. */
static void rbg0_draw_debug_readout(void)
{
    volatile uint16_t *const cram1 = (volatile uint16_t *)0x25F00200;  /* CRAM bank-1 (NBG1 palette) */
    cram1[1]   = 0x8000;                                          /* opaque black (box bg)   */
    cram1[254] = (uint16_t)(0x8000 | (31 << 10) | (31 << 5) | 31);/* opaque white (glyphs)   */
    for (int y = 0; y < 72; ++y)                                  /* black box rows 0..71    */
        for (int x = 0; x < 320; ++x)
            DOOM_VRAM[y * DOOM_VRAM_STRIDE + x] = 1;
    volatile uint16_t *const C = (volatile uint16_t *)0x25F80010;
    volatile uint8_t  *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    uint32_t cyc_sh[4], cyc_chip[4];                /* read fresh -> independent of the commit method */
    for (int b = 0; b < 4; ++b) {
        volatile uint16_t *s = (volatile uint16_t *)(shadow + 0x10 + b * 4);
        cyc_sh[b]   = ((uint32_t)s[0] << 16) | (uint32_t)s[1];          /* SGL shadow (the real values) */
        cyc_chip[b] = ((uint32_t)C[b*2] << 16) | (uint32_t)C[b*2 + 1];  /* chip (write-only -> 0)       */
    }
    rbg0_puthex(4, 4, ramctl_before, 4);  rbg0_puthex(84, 4, ramctl_after, 4);   /* line 0 RAMCTL b|a    */
    for (int b = 0; b < 4; ++b) {                                                 /* lines 1..4 CYC sh|chip */
        int py = 16 + b * 12;
        rbg0_puthex(4, py, cyc_sh[b], 8);  rbg0_puthex(84, py, cyc_chip[b], 8);
    }
    { unsigned int t = vbl_count; while (vbl_count - t < 180) ; }                 /* hold ~3s      */
}
#endif

#if VDP1_FLOOR_TEST
/* VDP1 floor inc-1 stub: own EVERY candidate visplane (return 1) -> all secondary
   floors/ceilings go index 0 (black) while sat_vdp1_floor is on (pad Y).  Validates that
   the core skip targets exactly the non-sky / non-RBG0-dominant surfaces, and shows the
   coverage the affine-strip emitter (inc-3) will have to fill.  No strips emitted yet. */
extern "C" int sat_floor_vdp1_stub(int picnum, int height, int minx, int maxx,
                                   const unsigned char *top, const unsigned char *bottom,
                                   int lightlevel)
{
    (void)picnum; (void)height; (void)minx; (void)maxx;
    (void)top; (void)bottom; (void)lightlevel;
    return 1;
}
#endif

#if SAT_FLOOR_PERFSIM
/* Perf-sim hook for pad-Y mode 2/3: claim (=> skip to index 0) every secondary floor/ceiling, but
   NOT the player's dominant floor (kept drawn) -- so mode 2 ("all but dominant") measures exactly the
   VDP1-strip floor's offload.  The core computes sat_vdp2_floor_h/pic whenever sat_vdp1_floor is on
   (r_plane.c guard).  In mode 3 the dominant is already skipped by sat_vdp2_floor before the hook. */
extern "C" int sat_floor_perfsim_hook(int picnum, int height, int minx, int maxx,
                                      const unsigned char *top, const unsigned char *bottom,
                                      int lightlevel)
{
    (void)minx; (void)maxx; (void)top; (void)bottom; (void)lightlevel;
    if (height == sat_vdp2_floor_h && picnum == sat_vdp2_floor_pic)
        return 0;   /* keep the dominant floor drawn */
    return 1;       /* skip everything else (secondary floors + ceilings) */
}
#endif

extern "C" void DG_Init(void)
{
    if (TVSTAT & 1)
    {
        us_per_frame = 20000;
        ns_per_frt   = 4501;
    }

    FRT_TCR = (unsigned char)((FRT_TCR & ~3) | 2);  /* sysclk/128, ~4.47us/tick */

    /* Register our VBlank handler via SRL event system */
    SRL::Core::OnVblank += vblank_handler;

    /* IRQ-cost probe removed: it answered perf question 1.1 (IRQ steals ~2.4% CPU
       -- not a bottleneck, crossed off) and was a one-shot boot reading that never
       updated.  Its build-identity stamp (b:__TIME__) now lives on the live row-17
       fps line.  Removing the 60-vblank busy-loop also shaves ~1s off boot. */

    printf("build: " __DATE__ " " __TIME__ "\n");
    printf("Mimas platform init\n");
    printf("video: %s\n", (TVSTAT & 1) ? "PAL" : "NTSC");

    SRL::Debug::Print(0, 1, "INIT CD...");

    /* CD filesystem init (SRL wraps GFS) */
    SRL::Cd::Initialize();

    SRL::Debug::Print(0, 1, "INIT CART...");
    cart_enable();
    sat_cart_cached_base = CART_RAM_CACHED;       /* Step 4b: cart read alias for blob staging */
    unsigned int cart_sz = cart_probe_size();
    {
        static char cid[45];
        unsigned char id = *CART_ID_ADDR;
        sprintf(cid, "CART id=0x%02x usable=%uKB", (unsigned int)id,
                cart_sz / 1024u);
        SRL::Debug::Print(0, 1, cid);
        printf("cart usable size: %u bytes (%u KB)\n", cart_sz, cart_sz / 1024u);
        /* 3s readability pause shown as a live countdown on row 2, so this
           pre-menu screen isn't a frozen still (boot-screen feedback 2026-06-26).
           Row 2 is reused by the WAD-load % right after, so clear it on the way out. */
        for (int s = 3; s > 0; --s)
        {
            SRL::Debug::Print(0, 2, "STARTING IN %d...", s);
            unsigned int t = vbl_count;
            while (vbl_count - t < 60) ;
        }
        SRL::Debug::Print(0, 2, "                 ");
    }
#if FORCE_CD_STREAM
    cart_sz = 0;   /* test override: ignore the cart, force CD streaming */
#endif
    /* The cart path only works if the WAD actually FITS in the 4MB cart.  A
       1M/2M (or 1M-mode AR) cart, OR a WAD bigger than 4MB (e.g. the full
       Ultimate Doom / Doom II IWADs), cannot be cart-loaded -- load_wad()
       refuses oversized WADs and returns 0 -- so we stream from CD instead of
       loading a truncated, aliased WAD that renders as a black screen. */
    int cart_loaded = 0;
    if (cart_sz >= 0x400000u)
    {
        SRL::Debug::Print(0, 1, "INIT WAD(cart 4MB)...");
        cart_loaded = load_wad();
        if (cart_loaded)
        {
            static char ws[45];
            sprintf(ws, "WAD OK sz=%u", sat_wad_size);
            SRL::Debug::Print(0, 1, ws);
            unsigned int t = vbl_count; while (vbl_count - t < 120) ;
        }
    }
    if (!cart_loaded)
    {
        if (cart_sz >= 0x400000u)
            printf("4MB cart but WAD too big/load failed -- CD streaming\n");
        else if (cart_sz)
            printf("cart only %uKB (<4MB) -- IWAD too big, CD streaming\n",
                   cart_sz / 1024u);
        else
            printf("No usable RAM cart -- CD streaming mode\n");
        sat_streaming_mode = 1;
        /* Step 4b: a >=4MB cart that couldn't raw-load the (too-big) WAD is still
           usable as a per-map compressed-blob store -- the .DRP loader stages this
           map's blob into it at level start (worst blob ~3.5MB fits 4MB).  Smaller
           carts can't hold the worst case, so only the full 4MB enables staging. */
        sat_cart_usable = (cart_sz >= 0x400000u) ? cart_sz : 0;
        SRL::Debug::Print(0, 1, cart_sz ? "CART -> CD STREAM..."
                                        : "NO CART -> CD STREAM...");
        if (!W_SaturnCDInit())
            DG_Fatal("DOOM1.WAD not found on CD");
        {
            static char ws[45];
            sprintf(ws, "WAD CD sz=%u", sat_wad_size);
            SRL::Debug::Print(0, 1, ws);
            unsigned int t = vbl_count; while (vbl_count - t < 120) ;
        }
    }

    SRL::Debug::Print(0, 1, "INIT VDP2...");

#if VDP2_RBG0_TEST
    /* RBG0 floor prototype Phase-0: set up the rotation plane FIRST, BEFORE the NBG0/
       NBG1 bitmaps, so it reserves its VRAM access cycles (cell + pattern-name reads)
       before the NBGs grab them.  SRL's own note (srl_vdp2.hpp): "allocate RBG0 before
       NBG0-3".  RBG0-after-NBG read all-zero cells = opaque black (docs/RBG0_FLOOR_PLAN.md). */
    rbg0_proto_init();
#endif

    /* VDP2: NBG1 as 512x256 8bpp bitmap, palette bank 1, below console.
       These are direct SGL calls -- still valid under SRL (SGL is linked). */
    for (int y = 0; y < 256; ++y)
        memset(DOOM_VRAM + y * DOOM_VRAM_STRIDE, 0, DOOM_VRAM_STRIDE);

    slBitMapNbg1(COL_TYPE_256, BM_512x256, (void *)DOOM_VRAM);
    slBMPaletteNbg1(1);
    slScrPosNbg1(toFIXED(0.0), toFIXED(0.0));
#if VDP2_ZOOM_TEST
    /* Phase-0: enlarge NBG1 x2 horizontally (see the VDP2_ZOOM_TEST note up top).
       Set once at init like slBitMapNbg1/slBMPaletteNbg1 -- the SGL vblank handler
       re-pushes the scroll-screen registers each frame (same path that makes the
       NBG0 sky scroll work without slSynch).  If the scale doesn't stick, move this
       call into DG_DrawFrame next to slScrPosNbg0. */
    slScrScaleNbg1(toFIXED(VDP2_ZOOM_FACTOR), toFIXED(1.0));
#endif

    /* SATURN sky: NBG0 = 512x256 8bpp bitmap (VRAM A0, palette bank 1, shared
       with NBG1).  Bitmap content is uploaded per-level by sky_upload() once
       skytexture is known.  NBG3 = SRL debug text (priority 7). */
    /* VDP2 hardware sky stays (A0); the RBG0 floor map is in B1, no conflict.  Sky A0,
       RBG0 cells+ktable A1, framebuffer B0, RBG0 map B1.  The only casualty is NBG3 debug
       text (shares B1 with the map) -> dropped while RBG0 is on; the pad toggle flips RBG0
       off to read the overlay. */
#if VDP2_HW_SKY
    for (int y = 0; y < 256; ++y)
        memset(SKY_VRAM + y * SKY_VRAM_STRIDE, 0, SKY_VRAM_STRIDE);
    slBitMapNbg0(COL_TYPE_256, BM_512x256, (void *)SKY_VRAM);
    slBMPaletteNbg0(1);
    slScrPosNbg0(toFIXED(0.0), toFIXED(0.0));
#endif
#if SKY_DEBUG_SHOW
    slPriorityNbg0(6); slPriorityNbg1(5);   /* sky ON TOP to verify Stage A */
#else
    /* LAYER INVERSION: software (NBG1) ON TOP with Doom's correct occlusion; the VDP1
       walls render BELOW NBG1, filling the index-0 (transparent) wall gaps NBG1 leaves
       where the software wall draw is skipped.  NBG3 debug = 7 (top).
       NBG1 game = 6  >  every sprite priority = 5  >  NBG0 sky = 4 (3 with RBG0). */
#if VDP2_RBG0_TEST
    slPriorityNbg0(3); slPriorityNbg1(6);   /* sky drops to 3 to sit below the RBG0 floor(4) */
#else
    slPriorityNbg0(4); slPriorityNbg1(6);
#endif
    slPrioritySpr0(5); slPrioritySpr1(5); slPrioritySpr2(5); slPrioritySpr3(5);
    slPrioritySpr4(5); slPrioritySpr5(5); slPrioritySpr6(5); slPrioritySpr7(5);
#endif
#if VDP2_RBG0_TEST
    /* rbg0_proto_init() was called above, before the NBG bitmaps (cycle-pattern order). */
#if RBG0_DEBUG_ONTOP
    /* DEBUG: RBG0 above the game so its content is visible regardless of the index-0
       window.  RBG0=6 > NBG1=5 (overrides the slPriorityNbg1(6) just set); NBG3 text=7
       stays on top.  Confirms "does RBG0 render my grid?". */
    slPriorityRbg0(6);
    slPriorityNbg1(5);
#endif
#if VDP2_HW_SKY
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON | RBG0ON);   /* sky(NBG0) + floor(RBG0) both on */
#else
    slScrAutoDisp(NBG1ON | (RBG0_DISPLAY ? RBG0ON : 0) | (RBG0_NBG3 ? NBG3ON : 0));  /* sw sky; floor(RBG0) + NBG3 (line-color display via slLineColDisp, NOT BGON) */
#endif
#else
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON);
#endif

#if VDP2_RBG0_TEST
    /* Commit the RBG0 bank assignment (RDBS) straight to the chip -- the piece SGL would
       push inside slSynch.  After slScrAutoDisp so RBG0ON is already live; once is enough
       (the SGL vblank handler re-pushes BGON/scroll, not RAMCTL). */
#if RBG0_BITMAP
    /* BITMAP fix (disasm + SlaveDriver): reserve the RDBS (A1=char/A0=coeff) + park the A0/A1
       rotation cycle slots BY HAND -- slBitMapRbg0 never did (no rbank_set).  Then flush the
       shadow.  Do NOT slSynch: it would recompute the cycle pattern from the inconsistent
       shadow = the boot-loop. */
    rbg0_commit_ramctl();         /* RDBS = 0x0D */
    rbg0_commit_cyc();            /* park A0/A1 cycles + block-flush shadow -> chip */
#else
    rbg0_commit_ramctl();         /* cell path: poke RDBS (B1=pattern-name) */
#if RBG0_COMMIT_VIA_SLSYNCH
    slSynch();                    /* one-shot full-register commit via SGL's own flush */
#else
    rbg0_commit_cyc();            /* manual block-flush of the shadow register image */
#endif
#endif
    slCashPurge();               /* TEST (cache hypothesis): flush the SH-2 cache so the RBG0 cells/map/
                                    K-table SGL wrote via CACHED addresses actually reach VRAM.  Ymir has
                                    no cache model (renders); HW reads stale VRAM -> snow.  Known trap. */
#endif

    /* Enable the core sky-skip: R_DrawPlanes leaves the sky region as index 0
       (transparent) so the VDP2 NBG0 sky shows through. */
    sat_vdp2_sky = VDP2_HW_SKY;   /* 0 = software sky (frees A0 for the RBG0 K-table) */
#if VDP2_RBG0_TEST
    /* Floor on RBG0 at boot (rbg0_mode 0); pad Y cycles the 3 RBG0/debug modes. */
    sat_vdp2_floor = 1;
#endif
    sat_apply_potato();   /* boot Potato level; pad Z cycles it live */

    /* LAYER INVERSION: the weapon + HUD now render in SOFTWARE (NBG1, on top) -- do NOT
       route them to VDP1.  VDP1 carries ONLY the walls, BELOW NBG1.  (The VDP1 weapon/
       HUD path is left in the file but unhooked.) */

#if VDP1_WALL_TEST
    /* Route one-sided (solid) walls to the VDP1 world renderer AND skip their
       software column draw -> see the VDP1 coverage + the perf it buys back. */
    sat_wall_hook = sat_wall_vdp1;
    sat_wall_skip = 1;
    /* kick VDP1 right after the BSP walk (parallel with the CPU floors/sprites) so the
       walls present the SAME frame as the framebuffer (no 1-frame lag / sky-at-the-seam). */
    sat_walls_done_hook = sat_walls_kick;
#endif

#if VDP1_FLOOR_TEST
    /* inc-1: register the floor-skip hook (claims secondary floors/ceilings).  Left OFF
       (sat_vdp1_floor = 0) so the boot render is normal software floors; pad Y toggles it
       live to A/B the index-0 coverage (owned surfaces go black until the strip emitter). */
    sat_floor_vdp1_hook = sat_floor_vdp1_stub;
    sat_vdp1_floor      = 0;
#endif

    SRL::Debug::Print(0, 1, "INIT DOOM...");
}

static void dma_table_build(void)
{
    /* SATURN: build the SCU indirect descriptor list through the cache-through
       mirror (| 0x20000000) so the descriptors are guaranteed to be in
       physical RAM.  The SCU is a bus master with no cache: if the table were
       written copy-back and not flushed, the SCU would read stale/garbage
       descriptors and DMA to wild addresses -- a classic cause of the bus
       hang seen on real hardware (works on emulators, which model the cache
       leniently).  SCU_D0W still gets the normal 0x06 address; the SCU reads
       the same physical RAM we just wrote uncached. */
    unsigned int (*t)[3] =
        (unsigned int (*)[3])((unsigned int)dma_table | 0x20000000u);
    for (int y = 0; y < 224; ++y)
    {
        t[y][0] = 320;
        t[y][1] = (unsigned int)DOOM_VRAM + y * DOOM_VRAM_STRIDE;
        t[y][2] = (unsigned int)framebuffer + y * 320;
    }
    t[223][2] |= DMA_END_FLAG;
}

static void dma_wait_idle(void)
{
    int guard = 2000000;
    while ((SCU_DSTA & 0x30) && guard--)
        ;
}

/* SATURN sky: upload the current sky texture (256x128, full-bright) into the NBG0
   bitmap, tiled 2x across the 512-wide plane so horizontal scroll wraps cleanly.
   Called when skytexture changes (per level/episode).  VDP2 VRAM is uncached I/O
   space, so direct writes are fine. */
/* Darkest non-zero palette index (cached).  Index 0 is the VDP2 transparent code,
   so we must keep it out of any layer that should be opaque -- both the sky here
   and (Stage B) the scene colormap.  colors[] is the live PLAYPAL. */
static int sat_near_black(void)
{
    static int idx = -1;
    if (idx < 0)
    {
        int best = 0x7fffffff;
        idx = 1;
        for (int i = 1; i < 256; ++i)
        {
            int lum = (int)colors[i].r + colors[i].g + colors[i].b;
            if (lum < best) { best = lum; idx = i; }
        }
    }
    return idx;
}

static int sky_loaded_tex = -1;
static void sky_upload(void)
{
    unsigned char *vram = SKY_VRAM;
    unsigned char  nb   = (unsigned char)sat_near_black();
    for (int col = 0; col < 256; ++col)
    {
        const unsigned char *src = R_GetColumn(skytexture, col);  /* 128-tall */
        for (int y = 0; y < 128; ++y)
        {
            unsigned char p = src[y];
            if (!p) p = nb;     /* keep the sky OPAQUE: 0 is the transparent code */
            vram[y * SKY_VRAM_STRIDE + col]       = p;
            vram[y * SKY_VRAM_STRIDE + col + 256] = p;   /* 2nd tile */
        }
    }
    sky_loaded_tex = skytexture;
}

/* ------------------------------------------------------------------ */
/* VDP1 weapon sprite -- player gun on the hardware sprite layer        */
/* ------------------------------------------------------------------ */
/* The async VDP1 driver (command list @0x25C00000, 1-cycle auto, PTMR plot, no
   wait -- see the VDP1 note up top) now carries the player weapon.  R_DrawPSprite
   (core) hands us the cached patch + screen top-left + flip + light colormap via
   the sat_psprite_* hooks; we unpack it to an RGB555 VDP1 texture (masked gaps
   transparent) and append a normal sprite command.  VDP1 rasterises it in PARALLEL
   over the VDP2 game layer, freeing the software masked-column path. */
#if VDP1_WEAPON
#define VDP1_TVMR  (*(volatile unsigned short *)0x25D00000)
#define VDP1_FBCR  (*(volatile unsigned short *)0x25D00002)
#define VDP1_PTMR  (*(volatile unsigned short *)0x25D00004)
#define VDP1_EWDR  (*(volatile unsigned short *)0x25D00006)
#define VDP1_EWLR  (*(volatile unsigned short *)0x25D00008)
#define VDP1_EWRR  (*(volatile unsigned short *)0x25D0000A)
#define VDP1_EDSR  (*(volatile unsigned short *)0x25D00010)   /* status: bit1 CEF = draw done */
#define VDP1_VRAM_BASE 0x25C00000u

/* DOUBLE-BUFFERED command list (kills the tearing: VDP1 in 1-cycle mode plots every
   vblank, so rewriting the list in place lets it read a half-written frame -> black
   square / missing parts).  A fixed root command @VRAM 0 (sysclip + JUMP, CTRL
   constant) whose 1-halfword LINK is the ONLY per-frame write -> atomic, race-free
   buffer flip.  Layout: root@+0, empty@+0x40, bank0@+0x100, bank1@+0x2100 (256 cmds
   each).  Textures are NOT double-buffered -- they live in a STABLE per-lump cache
   (below), so VDP1 never reads a texture mid-rebuild. */
#define VDP1_ROOT_ADDR  0x25C00000u
#define VDP1_BANKE_ADDR 0x25C00040u
static const unsigned int VDP1_BANK[2] = { 0x25C00100u, 0x25C02100u };
#define VDP1_BANK_CMDS  256               /* commands per bank (0x2000 VRAM / 32B each) */
#define VDP1_CMD_GUARD  (VDP1_BANK_CMDS - 2)   /* weapon/HUD stop here (leave end + 1) */

/* Texture cache: each weapon frame's texture lives in a STABLE VRAM slot keyed by
   (lump, colormap) -> unpacked only on a frame/light change, not every frame (most
   frames are just the cheap, double-buffered command).  4 slots x 44KB @0x25C45000
   (shrunk from 8 to free VRAM for the wall cache); round-robin eviction (4 slots =
   enough margin that a slot referenced by the displayed bank survives the 1-frame
   flip -- the weapon draws only 1-2 sprites/frame). */
#define WPN_TEX_BASE   0x25C45000u
#define WPN_TEX_SLOTSZ 0xB000u            /* 44 KB -> up to ~160x140 padded */
#define WPN_TEX_SLOTS  4
static struct { int lump; const unsigned char *cmap; int padW; int H; }
                    wpn_cache[WPN_TEX_SLOTS];
static int          wpn_cache_rr;

static int          vdp1_bank;     /* weapon bank VDP1 is currently displaying */
static int          vdp1_wbank;    /* bank being written this frame */
static int          vdp1_wnext;    /* next command slot in the write bank */
static int          vdp1_wactive;  /* 1 = R_DrawPlayerSprites ran this frame */
static unsigned int vdp1_hud_csum = 0xFFFFFFFFu;  /* status-bar change detector */

/* HUD on VDP1: the status bar (framebuffer rows 168-199, drawn by ST_Drawer) is
   re-drawn as a VDP1 sprite -- appended AFTER the weapon so it sits ON TOP of it
   (the weapon's bob/recoil no longer pokes over the HUD).  Texture rebuilt only when
   the bar changes.  HUD_Y is parameterised for future per-viewport multiplayer HUDs. */
#define HUD_W        320
#define HUD_H        32
#define HUD_Y        168
#define VDP1_HUD_TEX 0x25C78000u   /* 320*32*2 = 20KB, just past the weapon cache */

/* Write one 32-byte VDP1 command (16 halfwords) at command index `idx` of `base`. */
static void vdp1_cmd_at(unsigned int base, int idx, const unsigned short *c)
{
    volatile unsigned short *p = (volatile unsigned short *)base + idx * 16;
    for (int k = 0; k < 16; ++k)
        p[k] = c[k];
}

static inline unsigned short bswap16(unsigned short v)
{ return (unsigned short)((v >> 8) | (v << 8)); }
static inline unsigned int bswap32(unsigned int v)
{ return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

/* Doom palette index -> VDP1 RGB555 (MSB=1 = opaque), via the live PLAYPAL. */
static inline unsigned short pal_rgb555(int idx)
{
    return (unsigned short)(0x8000
        | ((colors[idx].b >> 3) << 10)
        | ((colors[idx].g >> 3) << 5)
        |  (colors[idx].r >> 3));
}

#if VDP1_WALL_TEST
/* ---- VDP1 world-renderer Step 3: textured one-sided walls -------------------- */
/* Each one-sided wall (captured by the core hook during the BSP walk) is accumulated,
   then drained in vdp1_wpn_begin -- after the bank's local-coord and BEFORE the weapon
   so walls sit behind it.  Its texture (cached per texnum, light baked) is tiled across
   the wall as FUNC_DISTORSP sub-quads.  The software one-sided wall draw is SKIPPED
   (sat_wall_skip=1) so this REPLACES them -> walls VDP1 can't cache (wide textures /
   >cap) leave a hole = exactly the VDP1 coverage gap to see.  v1 approximations (TODO,
   hw-tune): only ~3
   textures cached (VRAM gap -- reorganise/shrink the weapon cache for more), light baked
   per texnum (no per-distance Gouraud), no u-offset / vertical tiling for tall walls. */
/* Wall texture cache.  The weapon+HUD are now SOFTWARE (layer inversion), so their old
   VDP1 VRAM is RECLAIMED for walls.  TWO POOLS keyed by texture size: 8 x 32KB (narrow,
   <=128x128) + 3 x 64KB (wide, up to 256x128) -> wide tech textures (a 32KB slot can't
   hold them) stop leaving sky-through-wall holes.  PERSISTENT LRU (built once per texnum)
   + per-frame `locked` (never evicted mid-frame -> no mid-frame clobber).  Narrow textures
   prefer narrow slots, fall back to wide. */
/* 8BPP: 1 byte/texel (was 2) -> HALF the VRAM per texture -> DOUBLE the slots in the same budget.
   Two pools keyed by size: 16 x 16KB (narrow, <=128x128) + 6 x 32KB (wide, up to 256x128) = 22
   slots (was 11).  Same VRAM region 0x25C05000..0x25C75000 (16*16KB + 6*32KB = 448KB, unchanged). */
#define WTEX_BASE      0x25C05000u
#define WTEX_NARROW_N  16
#define WTEX_NARROW_SZ 0x4000u                                      /* 16KB -> 128x128 @ 8bpp */
#define WTEX_WIDE_N    6
#define WTEX_WIDE_SZ   0x8000u                                      /* 32KB -> 256x128 @ 8bpp */
#define WTEX_WIDE_BASE (WTEX_BASE + WTEX_NARROW_N * WTEX_NARROW_SZ) /* 0x25C45000 */
#define WTEX_SLOTS     (WTEX_NARROW_N + WTEX_WIDE_N)                /* 22; ends 0x25C75000 */
#define WALL_CMD_CAP   (VDP1_BANK_CMDS - 8)   /* walls stop here -> room for end + margin */

/* 8BPP PALETTE LIGHTING (replaces gouraud, which can't light a 256-colour BANK: VDP1 applies
   gouraud to the palette CODE before the CRAM lookup -> it shifts the index, not the RGB).
   Each texel = the RAW Doom palette index (1 byte, NEVER re-baked -- not for light, not for
   flash).  Per-wall light = a CRAM 256-colour BANK chosen by CMDCOLR (bank<<8): bank 1 = NBG1's
   live full-bright PLAYPAL, banks 2..7 = the PLAYPAL pre-shaded by 6 colormap levels.  So a wall
   texel idx -> CRAM[bank*256+idx] = the EXACT (multiplicative) colormap colour, matching the
   software floors/sprites; flash re-tints the banks in CRAM (see wtex_rebuild_banks). */
static struct { int texnum; unsigned int addr, cap; short padW, H;
                unsigned int lru; unsigned char locked; }
                wtex_cache[WTEX_SLOTS];
static unsigned int wtex_tick;     /* per-frame monotonic clock for LRU */

/* Per-wall light = a CRAM bank.  Bank 1 = full bright (= NBG1 PLAYPAL); the 6 dark banks 2..7
   hold the PLAYPAL shaded by these colormap levels (0=brightest..31=darkest).  wlight_bank_lut
   maps a wall's Doom colormap level (0..33) to the nearest of the 7 banks. */
static const unsigned char wlight_darklevel[WLIGHT_DARK_N] = { 5, 10, 16, 21, 26, 31 };
static unsigned char wlight_bank_lut[34];

/* one-time: assign each slot its fixed VRAM address + capacity (narrow then wide pool),
   and build the colormap-level -> CRAM-bank lookup. */
static void wtex_setup(void)
{
    for (int i = 0; i < WTEX_NARROW_N; ++i)
    { wtex_cache[i].addr = WTEX_BASE + (unsigned int)i * WTEX_NARROW_SZ;
      wtex_cache[i].cap = WTEX_NARROW_SZ; }
    for (int i = 0; i < WTEX_WIDE_N; ++i)
    { wtex_cache[WTEX_NARROW_N + i].addr = WTEX_WIDE_BASE + (unsigned int)i * WTEX_WIDE_SZ;
      wtex_cache[WTEX_NARROW_N + i].cap = WTEX_WIDE_SZ; }
    for (int L = 0; L < 34; ++L)
    {
        int Lc = L > 31 ? 31 : L;
        int best = 1, bestd = Lc;                        /* bank 1 = level 0 (full bright) */
        for (int k = 0; k < WLIGHT_DARK_N; ++k)
        {
            int d = Lc - (int)wlight_darklevel[k]; if (d < 0) d = -d;
            if (d < bestd) { bestd = d; best = k + 2; }
        }
        wlight_bank_lut[L] = (unsigned char)best;
    }
}

/* CMDCOLR (= CRAM 256-colour bank base, bank<<8) for a wall's colormap = its light level. */
static inline unsigned short wall_light_colr(const unsigned char *cmap)
{
    int L = (int)((cmap - colormaps) >> 8);              /* colormap level 0..33 */
    if (L < 0) L = 0; else if (L > 33) L = 33;
    return (unsigned short)((unsigned int)wlight_bank_lut[L] << 8);
}

/* (Re)shade the 6 dark CRAM light-banks from the LIVE palette (colors[] -- already flashed when
   called on palette_changed) + the colormap.  CRAM-only: NO texture re-bake -> the damage-flash
   spike is gone, and the dark walls flash together with bank 1 / the software layers.  Uploaded
   to CRAM in the vblank handler (pending_wbank + wbank_dirty). */
static void wtex_rebuild_banks(void)
{
    if (!colormaps) return;
    for (int k = 0; k < WLIGHT_DARK_N; ++k)
    {
        const unsigned char *cm = colormaps + (unsigned int)wlight_darklevel[k] * 256u;
        unsigned short *dst = pending_wbank[k];
        for (int idx = 0; idx < 256; ++idx)
        {
            int ci = cm[idx];                            /* light-mapped palette index */
            dst[idx] = (unsigned short)(0x8000
                | ((colors[ci].b >> 3) << 10)
                | ((colors[ci].g >> 3) << 5)
                |  (colors[ci].r >> 3));
        }
    }
    wbank_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* SATURN level-transition fade (docs/TRANSITIONS_PLAN.md option 1):  */
/* a buffer-FREE CRAM palette dip-to-black, replacing the f_wipe melt */
/* (which OOM'd the fragmented streaming zone).  Reuses the proven    */
/* no-slSynch vblank palette path: write pending_cram (full-bright    */
/* bank 1) + pending_wbank (dark wall light-banks 2..7) scaled toward */
/* black, flag dirty, and the vblank handler copies them to CRAM.     */
/* The framebuffer + VDP1 command list are UNTOUCHED -- only CRAM     */
/* ramps -- so the currently-displayed frame dims out / the freshly-  */
/* drawn frame rises in.  Blocking (~FADE_STEPS frames); called from  */
/* d_main.c's gamestate transition (streaming mode only).             */
#define FADE_STEPS 16

static void dg_fade_bake(int num)   /* brightness num/FADE_STEPS: 0 = black .. FADE_STEPS = full */
{
    int i;
    for (i = 0; i < 256; ++i)
    {
        int r = colors[i].r * num / FADE_STEPS;
        int g = colors[i].g * num / FADE_STEPS;
        int b = colors[i].b * num / FADE_STEPS;
        pending_cram[i] = (unsigned short)(0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    }
    palette_dirty = 1;
#if VDP1_WALL_TEST
    if (colormaps)   /* fade the dark wall light-banks in step, else VDP1 walls wouldn't dim */
    {
        int k;
        for (k = 0; k < WLIGHT_DARK_N; ++k)
        {
            const unsigned char *cm = colormaps + (unsigned int)wlight_darklevel[k] * 256u;
            unsigned short *dst = pending_wbank[k];
            for (i = 0; i < 256; ++i)
            {
                int ci = cm[i];
                int r = colors[ci].r * num / FADE_STEPS;
                int g = colors[ci].g * num / FADE_STEPS;
                int b = colors[ci].b * num / FADE_STEPS;
                dst[i] = (unsigned short)(0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
            }
        }
        wbank_dirty = 1;
    }
#endif
}

static void dg_fade_wait(void)      /* one frame: the vblank uploads pending_* to CRAM + clears dirty */
{
    unsigned int t = vbl_count;
    while (vbl_count - t < 1) ;
}

extern "C" void DG_FadeOut(void)    /* dip the current frame to black */
{
    for (int s = FADE_STEPS - 1; s >= 0; --s) { dg_fade_bake(s); dg_fade_wait(); }
}

extern "C" void DG_FadeIn(void)     /* rise the freshly-drawn frame from black */
{
    for (int s = 1; s <= FADE_STEPS; ++s) { dg_fade_bake(s); dg_fade_wait(); }
    palette_changed = true;         /* re-assert the true palette + light banks next normal frame */
}

/* one-sided mid + two-sided upper/lower quads.  Must stay <= the command budget (WALL_CMD_CAP
   ~248) so the zero-clipping flush's all-flat baseline always fits -> no wall is ever dropped to
   sky.  Was 128 -> dense rooms (tech room) overflowed it and the surplus far walls weren't even
   accumulated = "clipping".
   SPLIT-SCREEN shares this ONE command bank/budget across BOTH half-views (accumulated together,
   kicked once).  The cap MUST stay <= the budget: the flush guarantees >=1 flat per wall only while
   wall_acc_n <= budget; a larger accumulator makes `used+1+remaining > budget` fire for the NEAREST
   walls instead (they vanish).  So it stays 240 (a 480 would break that); a per-view SOFT cap in the
   hook (below) reserves the upper half for the right view so a dense LEFT view -- accumulated first
   -- can't hog every VDP1 slot.  When the cap is hit the hook REJECTS the wall and the core renders
   it in SOFTWARE (no sky) -- so the cap is also the VDP1->CPU starvation handoff. */
#define WALL_ACC_MAX 240
/* vx/vxr = the view's framebuffer x-range [vx, vxr] this wall belongs to (split-screen: 0..159 for
   the left view, 160..319 for the right).  x1/x2 are stored ALREADY offset by viewwindowx, so the
   emit works in absolute framebuffer coords; vx/vxr drive the per-view user-clip window. */
static struct { short x1, yl1, yh1, x2, yl2, yh2, slot, v0, v1, vx, vxr; int texnum, u1, u2;
                unsigned char mode, special, view; const unsigned char *cmap; } wall_acc[WALL_ACC_MAX];
static int wall_acc_n;

/* core hook (per one-sided seg, during the BSP walk): stash the wall.  x1/x2 arrive VIEW-relative
   (0..viewwidth-1); add viewwindowx so the VDP1 quad lands in this view's framebuffer x-range
   (0 for 1p / left half; 160 for the right half).  Single-view viewwindowx==0 => byte-identical.
   RETURNS 0 = queued for VDP1; 1 = REJECTED (the accumulator is full -> VDP1 starved): the core
   then draws this wall in SOFTWARE instead of dropping it to sky (Romain: "fallback CPU, pas skip"). */
extern "C" int sat_wall_vdp1(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                             int texnum, int u1, int u2, int v0, int v1,
                             const unsigned char *cmap)
{
    extern int viewwindowx, viewwidth, viewwindowy;   /* core: per-view origin + width (R_SetViewWindow) */
    extern int sat_split_active;         /* core: 1 while rendering the split half-views */
    extern int sat_split_view, sat_local_players;     /* core: current view index + live player count */
    extern int sat_wall_textured;        /* core: this seg's linedef is a special (door/switch) */
    extern int detailshift;              /* core: 1 = low-detail (half-res, x is the halved column) */
    /* Split-screen shares the single command bank across both half-views.  Reserve the upper half of
       the accumulator for the right view so a dense LEFT view (accumulated first) cannot starve the
       right view out of VDP1 slots (its overflow falls back to CPU, below).  1p = the full cap.
       When the cap is hit the wall is REJECTED -> the core renders it in software (no sky). */
    /* 4-way per-view command split: each split view gets a contiguous WALL_ACC_MAX/nv slice of the
       shared accumulator -- views render in index order, so view i is capped at (i+1)*share, and a
       light earlier view leaves its slack to later views.  nv=2 reproduces the old halves exactly. */
    int nv = sat_local_players; if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
    int cap = sat_split_active ? ((sat_split_view + 1) * (WALL_ACC_MAX / nv)) : WALL_ACC_MAX;
    if (wall_acc_n >= cap) return 1;     /* VDP1 list full -> caller draws this wall in SOFTWARE */
    int vx = viewwindowx, vy = viewwindowy;
    int i = wall_acc_n++;
    /* low-detail: x arrives as the HALVED column (0..viewwidth-1); the framebuffer is full width,
       so screen x = vx + (x<<detailshift).  detailshift==0 (1p / hi-detail) => byte-identical. */
    wall_acc[i].x1 = (short)((x1 << detailshift) + vx); wall_acc[i].yl1 = (short)(yl1 + vy); wall_acc[i].yh1 = (short)(yh1 + vy);
    wall_acc[i].x2 = (short)((x2 << detailshift) + vx); wall_acc[i].yl2 = (short)(yl2 + vy); wall_acc[i].yh2 = (short)(yh2 + vy);
    wall_acc[i].texnum = texnum; wall_acc[i].u1 = u1; wall_acc[i].u2 = u2;
    wall_acc[i].v0 = (short)v0; wall_acc[i].v1 = (short)v1; wall_acc[i].cmap = cmap;
    wall_acc[i].vx  = (short)vx;
    wall_acc[i].vxr = (short)(vx + (viewwidth << detailshift) - 1);
    wall_acc[i].view = (unsigned char)sat_split_view;   /* 4-way budget bin (0..3) */
    wall_acc[i].special = (unsigned char)(sat_wall_textured ? 1 : 0);   /* force textured in pot2 */
    return 0;                            /* queued for VDP1 */
}

/* best victim in [lo,hi): an empty slot, else the least-recently-used UNLOCKED slot */
static int wtex_find_victim(int lo, int hi)
{
    int victim = -1; unsigned int oldest = 0xFFFFFFFFu;
    for (int i = lo; i < hi; ++i)
    {
        if (wtex_cache[i].locked) continue;
        if (wtex_cache[i].texnum < 0) return i;
        if (wtex_cache[i].lru < oldest) { oldest = wtex_cache[i].lru; victim = i; }
    }
    return victim;
}

/* resolve texnum -> cache slot, building the texture (light baked from cmap) ONLY on a
   miss.  LRU-persistent + per-frame locked so a visible texture keeps its slot and is
   never overwritten while another wall's command still points at it.  -> slot or -1. */
static int wall_tex_resolve(int texnum, const unsigned char *cmap)
{
    (void)cmap;
    for (int i = 0; i < WTEX_SLOTS; ++i)
        if (wtex_cache[i].texnum == texnum)             /* hit: reuse, touch, lock */
        {
            wtex_cache[i].locked = 1;
            wtex_cache[i].lru = wtex_tick;
            return i;
        }
    int W = texturewidthmask[texnum] + 1;
    int H = textureheight[texnum] >> 16;                /* fixed_t -> pixels */
    int padW = (W + 7) & ~7;
    if (W <= 0 || H <= 0) return -1;
    unsigned int size = (unsigned int)(padW * H) * 1u;  /* 8bpp: 1 byte/texel */
    if (size > WTEX_WIDE_SZ) return -1;                 /* too big even for a wide slot */

    int victim;
    if (size <= WTEX_NARROW_SZ)                         /* narrow: prefer narrow pool */
    {
        victim = wtex_find_victim(0, WTEX_NARROW_N);
        if (victim < 0) victim = wtex_find_victim(WTEX_NARROW_N, WTEX_SLOTS);
    }
    else                                                /* wide: only the wide pool fits */
        victim = wtex_find_victim(WTEX_NARROW_N, WTEX_SLOTS);
    if (victim < 0) return -1;                          /* all fitting slots used -> flat in flush */
    wtex_bakes++;                                       /* cache miss -> a real bake follows (the `k` cost) */

    /* bake the RAW palette index (1 byte/texel) full-bright; light is applied at draw time via
       the CMDCOLR CRAM bank.  No re-bake ever (light or flash) -> max cache stability.
       Write 16-bit packed (two adjacent columns per halfword: hi byte = even col, lo = odd, on the
       big-endian SH-2) -- VDP VRAM is 16-bit and the SGL/SRL references upload by DMA, never byte
       writes, so 8-bit stores to VDP1 VRAM are not relied on.  The byte layout is identical. */
    volatile unsigned short *t = (volatile unsigned short *)wtex_cache[victim].addr;
    int halfW = padW >> 1;                                    /* 16-bit words per texture row */
    for (int x = 0; x < W; x += 2)
    {
        const unsigned char *c0 = R_GetColumn(texnum, x);        /* even column (high byte) */
        const unsigned char *c1 = (x + 1 < W) ? R_GetColumn(texnum, x + 1) : c0;  /* odd (low) */
        int wx = x >> 1;
        for (int y = 0; y < H; ++y)
            t[y * halfW + wx] = (unsigned short)(((unsigned int)c0[y] << 8) | c1[y]);
    }
    wtex_cache[victim].texnum = texnum;
    wtex_cache[victim].padW = (short)padW; wtex_cache[victim].H = (short)H;
    wtex_cache[victim].locked = 1;
    wtex_cache[victim].lru = wtex_tick;
    return victim;
}

/* Emit one wall, WORLD-U-ANCHORED + WINDOW-CLIPPED.
   The texture repeats every `texw` in u.  Each tile is one DISTORSP sprite mapping the
   FULL texture, with corners EXTRAPOLATED to the tile's true screen extent (linear u->x)
   so the tiling is pinned to the world (no sliding/resizing as you move).  The sprite is
   VDP1 user-clipped (window) to the wall's visible x-range -> a wall that only shows a
   sub-range of the texture shows the CORRECT sub-range, not the whole texture squished
   onto the visible quad, and the extrapolated overhang never spills onto neighbours.
   Grazing tiles whose extent flies too far off-screen fall back to a clamped squish quad
   (bounds VDP1 fill + coordinate range). */
#define MAXWALLTILES 12   /* horizontal tiles per wall (more = fewer long-wall sky gaps) */
#define MAXVBANDS    4    /* vertical texture-height bands per wall (wrap / tall-wall tiling) */
static int wall_ext = 96;  /* extend past a screen edge before the squish fallback.  MAGNIFIED
                              (close/face-on) walls -- which squish badly at the edge and CAN'T be
                              fixed on VDP1 (DISTORSP can't address a texture column-subrange) -- are
                              rendered in SOFTWARE upstream (core r_segs.c magnification fallback), so
                              they never reach here; this only handles normal/grazing walls (low mag,
                              small extrapolation -> squish is rare/mild). */

/* Flat quad screen-y clamp (low-detail / Z mode): a flat fill has NO texture, so clamping its
   geometry to the screen is FREE (no v -> no swim) and bounds the VDP1 fill; the layer
   inversion hides any silhouette overspill.  (Too-close TEXTURED walls are handled upstream
   by the core CPU fallback, not here.)  3D view is rows 0..191 (320x224). */
#define WALL_FLAT_YLO  (-8)
#define WALL_FLAT_YHI  223   /* SATURN: screen bottom -- wall y is now ABSOLUTE (viewwindowy added), so bottom-row split views reach 223 */

static int wall_vbands(int wi)   /* number of vertical texture-height bands this wall needs */
{
    int H = (wall_acc[wi].slot >= 0) ? wtex_cache[wall_acc[wi].slot].H : 128;
    int v0 = wall_acc[wi].v0, vspan = wall_acc[wi].v1 - v0;
    /* EXACT count matching wall_emit's band loop (starts at vmod0 = v0%H, then H-aligned steps).
       The old (vspan+H-1)/H + 1 over-counted ~2x for normal walls -> the budget estimate saturated
       at ~half the real command count -> far walls dropped to sky.  Tight = more textured. */
    int b = 1;
    if (H > 0 && vspan > 0) { int vmod0 = ((v0 % H) + H) % H; b = (vmod0 + vspan + H - 1) / H; }
    if (b > MAXVBANDS) b = MAXVBANDS;
    return b < 1 ? 1 : b;
}

static int wall_tilecount(int wi)  /* est. command cost (bands x (tiles + 1 user-clip)) for budget */
{
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int du = wall_acc[wi].u2 - wall_acc[wi].u1; if (du < 0) du = -du;
    int n = (texw > 0) ? du / texw + 1 : 1;
    if (n > MAXWALLTILES) n = MAXWALLTILES;
    return wall_vbands(wi) * (n + 1);
}

static int wall_banded_cost(int wi)  /* est. cmd cost of a banded wall = ONE band's u-tiles + clip */
{
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int du = wall_acc[wi].u2 - wall_acc[wi].u1; if (du < 0) du = -du;
    int n = (texw > 0) ? du / texw + 1 : 1;
    if (n > MAXWALLTILES) n = MAXWALLTILES;
    return n + 1;
}

/* Emit the horizontal u-tiles of ONE vertical band: the texture rows at [charAddr, +charSize.h]
   mapped across the wall's u-range, window-clipped to [x1,x2].  The yl/yh args are THIS band's
   screen y at the two seg ends. */
/* SATURN: reciprocal-multiply the wall perspective math.  The SH-2 has NO hardware divide,
   and wall_emit/wall_emit_band did ~6 int + ~4 int64 software divisions PER TILE/BAND by the
   PER-WALL constants du / xspan / vspan -> the `k` (VDP1 flush) cost (11-28ms, measured).
   Precompute round(2^S / den) once per band/wall, then multiply (hardware) -> ~5-10x.  S=22
   keeps the error sub-pixel; round-to-nearest + sign-fold.  Pixel-validate the seams on Ymir.
   Flip SAT_WALL_RMUL to 0 for the original divisions (A/B). */
#define SAT_WALL_RMUL 1
#if SAT_WALL_RMUL
#define WRMUL_S 22
static inline int wrecip(int den)            /* den > 0 */
{ return (int)(((1u << WRMUL_S) + ((unsigned)den >> 1)) / (unsigned)den); }
static inline int wrmul_(long long num, int recip)   /* ~= num/den, rounded, sign-correct */
{ return (num >= 0) ?  (int)(( num * recip + (1 << (WRMUL_S - 1))) >> WRMUL_S)
                    : -(int)((-num * recip + (1 << (WRMUL_S - 1))) >> WRMUL_S); }
#define WDIV(numP, denP, recip)  wrmul_((long long)(numP), (recip))
#else
#define wrecip(den)              (0)
#define WDIV(numP, denP, recip)  ((int)((long long)(numP) / (denP)))
#endif

static void wall_emit_band(int x1, int x2, int yl1, int yh1, int yl2, int yh2,
                           int u1, int u2, int texw,
                           unsigned short charAddr, unsigned short charSize, unsigned short colr,
                           int vx, int vxr)
{
    int xspan = x2 - x1, du = u2 - u1;
    unsigned short cmd[16];

    if (xspan <= 0 || du == 0 || texw <= 0)            /* degenerate -> single quad */
    {
        if (vdp1_wnext >= WALL_CMD_CAP) return;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0002; cmd[2] = 0x00E0;                 /* DISTORSP | COLOR_4 8bpp | SPD | ECD-off */
        cmd[3] = colr;                                    /* CMDCOLR = CRAM light-bank base */
        cmd[4] = charAddr; cmd[5] = charSize;
        cmd[6]  = (short)x1; cmd[7]  = (short)yl1;
        cmd[8]  = (short)x2; cmd[9]  = (short)yl2;
        cmd[10] = (short)x2; cmd[11] = (short)yh2;
        cmd[12] = (short)x1; cmd[13] = (short)yh1;
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
        return;
    }

    /* per-band constants: reciprocals of du (signed) and xspan (>0 here) -> multiply per tile */
    int adu = (du < 0) ? -du : du, sdu = (du < 0) ? -1 : 1;
    int inv_du = wrecip(adu);
    int inv_xspan = wrecip(xspan);

    /* normalise u (tiling is periodic in texw) so the arithmetic stays small */
    int ubase = u1 & ~(texw - 1);
    u1 -= ubase; u2 -= ubase;
    int umin = (u1 < u2) ? u1 : u2;
    int umax = (u1 < u2) ? u2 : u1;

    int winset = 0, ntiles = 0;
    for (int ub = umin & ~(texw - 1); ub < umax && ntiles < MAXWALLTILES; ub += texw, ++ntiles)
    {
        if (vdp1_wnext >= WALL_CMD_CAP) break;
        int xs = x1 + WDIV((long long)(ub        - u1) * xspan * sdu, adu, inv_du);  /* /du */
        int xe = x1 + WDIV((long long)(ub + texw - u1) * xspan * sdu, adu, inv_du);  /* /du */
        int lo = (xs < xe) ? xs : xe, hi = (xs < xe) ? xe : xs;
        if (hi < x1 || lo > x2) continue;                /* tile outside the visible range */

        int yls = yl1 + WDIV((long long)(yl2 - yl1) * (xs - x1), xspan, inv_xspan);
        int yhs = yh1 + WDIV((long long)(yh2 - yh1) * (xs - x1), xspan, inv_xspan);
        int yle = yl1 + WDIV((long long)(yl2 - yl1) * (xe - x1), xspan, inv_xspan);
        int yhe = yh1 + WDIV((long long)(yh2 - yh1) * (xe - x1), xspan, inv_xspan);

        if (lo >= -wall_ext && hi <= 320 + wall_ext)     /* extend + window-clip (correct) */
        {
            if (!winset)
            {
                /* extend the window 1px each side so adjacent walls OVERLAP -> no seam
                   (the gap that, in motion, let the NBG0 sky show between quads).  CLAMP to the
                   view's x-range [vx, vxr] (full-screen 0..319 for 1p; the left/right half in
                   split) so the overlap never bleeds across the split seam into the other view. */
                int wx1 = x1 > vx  ? x1 - 1 : vx;
                int wx2 = x2 < vxr ? x2 + 1 : vxr;
                memset(cmd, 0, sizeof cmd);
                cmd[0] = 0x0008;                         /* FUNC_UserClip = wall window */
                cmd[6]  = (short)wx1; cmd[7]  = 0;       /* upper-left  (XA,YA) */
                cmd[10] = (short)wx2; cmd[11] = 223;     /* lower-right (XC,YC) */
                vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
                winset = 1;
                if (vdp1_wnext >= WALL_CMD_CAP) break;
            }
            /* grow the quad 1px top+bottom -> close vertical seams; the overspill into the
               (software NBG1) floor/ceiling is hidden by the layer inversion. */
            memset(cmd, 0, sizeof cmd);
            cmd[0] = 0x0002; cmd[2] = 0x04E0;  /* DISTORSP | Window_In | COLOR_4 8bpp | SPD | ECD-off */
            cmd[3] = colr;                                 /* CMDCOLR = CRAM light-bank base */
            cmd[4] = charAddr; cmd[5] = charSize;
            cmd[6]  = (short)xs; cmd[7]  = (short)(yls - 1);   /* A col0  top */
            cmd[8]  = (short)xe; cmd[9]  = (short)(yle - 1);   /* B colW  top */
            cmd[10] = (short)xe; cmd[11] = (short)(yhe + 1);   /* C colW  bot */
            cmd[12] = (short)xs; cmd[13] = (short)(yhs + 1);   /* D col0  bot */
            vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
        }
        else                                             /* grazing -> clamp + squish */
        {
            int cxs = xs < x1 ? x1 : (xs > x2 ? x2 : xs);
            int cxe = xe < x1 ? x1 : (xe > x2 ? x2 : xe);
            int cyls = yl1 + WDIV((long long)(yl2 - yl1) * (cxs - x1), xspan, inv_xspan);
            int chys = yh1 + WDIV((long long)(yh2 - yh1) * (cxs - x1), xspan, inv_xspan);
            int cyle = yl1 + WDIV((long long)(yl2 - yl1) * (cxe - x1), xspan, inv_xspan);
            int chye = yh1 + WDIV((long long)(yh2 - yh1) * (cxe - x1), xspan, inv_xspan);
            memset(cmd, 0, sizeof cmd);
            cmd[0] = 0x0002; cmd[2] = 0x00E0;                 /* DISTORSP | COLOR_4 8bpp | SPD | ECD-off */
            cmd[3] = colr;                                    /* CMDCOLR = CRAM light-bank base */
            cmd[4] = charAddr; cmd[5] = charSize;
            cmd[6]  = (short)cxs; cmd[7]  = (short)cyls;
            cmd[8]  = (short)cxe; cmd[9]  = (short)cyle;
            cmd[10] = (short)cxe; cmd[11] = (short)chye;
            cmd[12] = (short)cxs; cmd[13] = (short)chys;
            vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
        }
    }
}

/* Emit a wall: split the visible texel range [v0,v1) into VERTICAL bands aligned to the texture
   height H (the v analogue of the horizontal u-tiling), so the texture WRAPS (v mod H) exactly
   like Doom's software renderer.  This fixes textures whose [v0,v1) leaves [0,H] -- rowoffset,
   unpegged walls, two-sided upper/lower, walls taller than the texture -- which used to fall back
   to "full texture squished onto the band" (details at the wrong height / broken across segs).
   Each band is one full-texture-height slice mapped to its true screen-y sub-range. */
static void wall_emit(int wi)
{
    int slot = wall_acc[wi].slot;
    int padW = wtex_cache[slot].padW, H = wtex_cache[slot].H;
    unsigned int base = wtex_cache[slot].addr;
    int x1 = wall_acc[wi].x1, x2 = wall_acc[wi].x2;
    int yl1 = wall_acc[wi].yl1, yh1 = wall_acc[wi].yh1;
    int yl2 = wall_acc[wi].yl2, yh2 = wall_acc[wi].yh2;
    int vx = wall_acc[wi].vx, vxr = wall_acc[wi].vxr;   /* this wall's viewport x-range (split-screen) */
    int u1 = wall_acc[wi].u1, u2 = wall_acc[wi].u2;
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int v0 = wall_acc[wi].v0, v1 = wall_acc[wi].v1, vspan = v1 - v0;
    unsigned short colr = wall_light_colr(wall_acc[wi].cmap);  /* per-wall light = CRAM bank */

    if (H <= 0 || vspan <= 0)                          /* no valid v-range -> whole texture once */
    {
        int th = (H > 255) ? 255 : (H > 0 ? H : 1);
        unsigned short ca = (unsigned short)((base - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | th);
        wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr);
        return;
    }

    int inv_vspan = wrecip(vspan);   /* vspan > 0 here; reciprocal -> multiply per band */
    int v = v0, nb = 0;
    while (v < v1 && nb < MAXVBANDS)
    {
        if (vdp1_wnext >= WALL_CMD_CAP) break;
        int vmod = ((v % H) + H) % H;                  /* texel within the texture (wraps) */
        int rows = H - vmod;                           /* down to the next tile seam */
        if (v + rows > v1) rows = v1 - v;              /* last (partial) band */
        if (rows > 255) rows = 255;                    /* VDP1 charSize height is 8-bit */
        if (rows <= 0) break;
        int vb = v + rows;
        /* this band's screen y at the two seg ends (linear v->y over the whole [v0,v1] range) */
        int yl1b = yl1 + WDIV((long long)(v  - v0) * (yh1 - yl1), vspan, inv_vspan);
        int yh1b = yl1 + WDIV((long long)(vb - v0) * (yh1 - yl1), vspan, inv_vspan);
        int yl2b = yl2 + WDIV((long long)(v  - v0) * (yh2 - yl2), vspan, inv_vspan);
        int yh2b = yl2 + WDIV((long long)(vb - v0) * (yh2 - yl2), vspan, inv_vspan);
        unsigned int taddr = base + (unsigned int)vmod * (unsigned int)padW * 1u;  /* 8bpp */
        unsigned short ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | rows);
        wall_emit_band(x1, x2, yl1b, yh1b, yl2b, yh2b, u1, u2, texw, ca, cs, colr, vx, vxr);
        v = vb; ++nb;
    }
}

/* Fallback FLAT-colour quad for a wall drawn without its texture (low-detail Z mode / cache
   miss).  A palette polygon: CMDCOLR = light-bank<<8 | the texture's dominant index, so it is
   lit by the SAME CRAM bank as the textured walls and flashes via CRAM too.  1px generous. */
static void wall_emit_flat(int wi)
{
    if (vdp1_wnext >= WALL_CMD_CAP) return;
    int x1 = wall_acc[wi].x1, x2 = wall_acc[wi].x2;
    int yl1 = wall_acc[wi].yl1, yh1 = wall_acc[wi].yh1;
    int yl2 = wall_acc[wi].yl2, yh2 = wall_acc[wi].yh2;
    /* clamp the flat quad to the screen -- FREE for a flat fill (no texture = no swim) and it
       bounds the VDP1 fill for a tall/near wall.  The layer inversion hides any silhouette
       overspill (software ceiling/floor draw on top). */
    if (yl1 < WALL_FLAT_YLO) yl1 = WALL_FLAT_YLO; else if (yl1 > WALL_FLAT_YHI) yl1 = WALL_FLAT_YHI;
    if (yl2 < WALL_FLAT_YLO) yl2 = WALL_FLAT_YLO; else if (yl2 > WALL_FLAT_YHI) yl2 = WALL_FLAT_YHI;
    if (yh1 < WALL_FLAT_YLO) yh1 = WALL_FLAT_YLO; else if (yh1 > WALL_FLAT_YHI) yh1 = WALL_FLAT_YHI;
    if (yh2 < WALL_FLAT_YLO) yh2 = WALL_FLAT_YLO; else if (yh2 > WALL_FLAT_YHI) yh2 = WALL_FLAT_YHI;
    /* palette polygon: CMDCOLR is written directly to the framebuffer (MSB=0 -> palette pixel),
       so (light-bank<<8 | dominant index) goes through CRAM = lit by the bank + flashes via CRAM. */
    unsigned short colr = wall_light_colr(wall_acc[wi].cmap);
    unsigned short col  = (unsigned short)(colr |
                          (unsigned int)(R_WallPotatoColor(wall_acc[wi].texnum) & 0xFF));
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x0004;                                  /* FUNC_Polygon (flat) */
    cmd[2] = 0x00C0;                                  /* SPD (opaque) | ECD-off */
    cmd[3] = col;                                     /* CMDCOLR = bank<<8 | index -> CRAM */
    cmd[6]  = (short)x1; cmd[7]  = (short)(yl1 - 1);
    cmd[8]  = (short)x2; cmd[9]  = (short)(yl2 - 1);
    cmd[10] = (short)x2; cmd[11] = (short)(yh2 + 1);
    cmd[12] = (short)x1; cmd[13] = (short)(yh1 + 1);
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
}

/* Banded wall (pot2-bd): emit ONE narrow band (BAND_ROWS texels at a fixed source row) and let
   VDP1's DISTORSP magnify it over the WHOLE wall height -> per-column horizontal texel variation =
   vertical stripes that SCROLL with u (player movement) and track vertical movement via v0, keeping
   the texture's hue/pattern (unlike the flat quad) but ~as cheap (1 band, not N).  Distance-shaded
   via the CRAM light bank (colr).  Does NOT call R_WallPotatoColor. */
#define BAND_ROWS 4
static void wall_emit_banded(int wi)
{
    int slot = wall_acc[wi].slot;
    int padW = wtex_cache[slot].padW, H = wtex_cache[slot].H;
    unsigned int base = wtex_cache[slot].addr;
    int x1 = wall_acc[wi].x1, x2 = wall_acc[wi].x2;
    int yl1 = wall_acc[wi].yl1, yh1 = wall_acc[wi].yh1;
    int yl2 = wall_acc[wi].yl2, yh2 = wall_acc[wi].yh2;
    int vx = wall_acc[wi].vx, vxr = wall_acc[wi].vxr;
    int u1 = wall_acc[wi].u1, u2 = wall_acc[wi].u2;
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int v0 = wall_acc[wi].v0;
    unsigned short colr = wall_light_colr(wall_acc[wi].cmap);
    int vmod, rows;
    unsigned int taddr;
    unsigned short ca, cs;

    if (H <= 0) H = 1;
    vmod = ((v0 % H) + H) % H;                       /* one source row set, tracks vertical movement */
    /* clamp to the baked tile [vmod, H): the tile is only padW*H bytes, so reading past row H-1
       would sample the NEXT slot's texture (corruption).  Mirrors wall_emit's rows = H - vmod. */
    rows = BAND_ROWS; if (rows > H - vmod) rows = H - vmod; if (rows > 255) rows = 255; if (rows < 1) rows = 1;
    taddr = base + (unsigned int)vmod * (unsigned int)padW;
    ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
    cs = (unsigned short)(((padW >> 3) << 8) | rows);
    wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr);
}

/* the VDP1 wall mode (0=textured 1=banded 2=flat).  Global per level (set in sat_apply_potato);
   the flush forces flat for a wall with no texture slot, and forces textured for special walls. */
static int wall_potato(int wi)
{
    (void)wi;
    return wall_potato_mode;
}

/* drain accumulated walls into the current bank (from vdp1_wpn_begin, behind the weapon).
   ZERO CLIPPING: EVERY accumulated wall draws AT LEAST a 1-command FLAT (never dropped to sky);
   the nearest are UPGRADED to textured tiles while the budget allows -- but each upgrade RESERVES
   1 command for every wall still to come, so a far wall can never be starved out of its flat.
   (The previous "greedy" flush charged the worst-case tile estimate without that reservation, so an
   over-estimate -- VD1 finished at ~147/248 yet far walls vanished -- dropped them to mode 0 = sky.)
   Painted far->near (painter's algorithm). */
static void vdp1_walls_flush(void)
{
    wtex_bakes = 0;                      /* count this frame's texture re-bakes (the `k` driver) */
    if (wall_acc_n == 0) return;
    wtex_tick++;
    for (int i = 0; i < WTEX_SLOTS; ++i) wtex_cache[i].locked = 0;

    for (int i = 0; i < wall_acc_n; ++i)                 /* resolve textures (near-first) */
        wall_acc[i].slot = (short)wall_tex_resolve(wall_acc[i].texnum, wall_acc[i].cmap);

    /* mode: 1 = textured, 2 = flat, 0 = skip.  Every wall gets at least a 1-cmd FLAT (never sky
       while wall_acc_n <= budget, which WALL_ACC_MAX guarantees); the leftover budget (the SURPLUS
       beyond the all-flat baseline) upgrades the nearest walls to textured tiles.
       SPLIT-SCREEN shares this one command bank, so the textured surplus is divided EQUALLY between
       the two half-views -- else the left view (accumulated first, painted near-first) spends the
       whole surplus and the right view stays all-flat.  Each textured upgrade beyond a wall's own
       flat costs (tiles-1) extra cmds, charged to that view's surplus share.  For 1p (nviews==1)
       this is ALGEBRAICALLY identical to the old single-budget reservation
       (extra_used + (c-1) <= budget-n  <=>  used + c + (n-i-1) <= budget). */
    extern int sat_split_active, sat_local_players;     /* core: split flag + live player count */
    int budget = WALL_CMD_CAP - vdp1_wnext;
    int nv = sat_local_players; if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
    int nviews = sat_split_active ? nv : 1;             /* d_main renders nv views in split (2..4) */
    int surplus = budget - wall_acc_n;                 /* cmds available beyond the all-flat baseline */
    if (surplus < 0) surplus = 0;
    int surplus_per_view = surplus / nviews;
    int extra_used[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < wall_acc_n; ++i)
    {
        if (i >= budget) { wall_acc[i].mode = 0; continue; }   /* n > budget (cap makes this unreachable) */
        int v = (nviews > 1) ? (int)wall_acc[i].view : 0;     /* per-view surplus bin (0..nviews-1) */
        if (v >= nviews) v = nviews - 1;
        /* 3-way: 0=textured 1=banded 2=flat; a wall with no texture slot must be flat.  A SPECIAL
           wall (door/switch, wall_acc[i].special) is forced TEXTURED for readability even in pot2. */
        int wmode = (wall_acc[i].slot < 0) ? 2
                  : (wall_acc[i].special)  ? 0
                  : wall_potato(i);
        if (wmode != 2)                                        /* textured/banded: charge extra to surplus */
        {
            int extra = ((wmode == 1) ? wall_banded_cost(i) : wall_tilecount(i)) - 1;
            if (extra < 0) extra = 0;
            if (extra_used[v] + extra <= surplus_per_view)
            {
                extra_used[v] += extra;
                wall_acc[i].mode = (wmode == 1) ? 3 : 1;       /* banded=3, textured=1 */
                continue;
            }
        }
        wall_acc[i].mode = 2;                                  /* flat baseline (guaranteed to fit) */
    }

    for (int i = wall_acc_n - 1; i >= 0; --i)            /* paint far->near */
    {
        if      (wall_acc[i].mode == 1) wall_emit(i);
        else if (wall_acc[i].mode == 3) wall_emit_banded(i);
        else if (wall_acc[i].mode == 2) wall_emit_flat(i);
    }

    wall_acc_n = 0;
}
#endif

#if VDP1_MANUAL_CHANGE
/* Set by the kick after a plot is triggered: "a new frame is being drawn; present it (swap
   the VDP1 framebuffers) once its draw completes."  Read/cleared by the vblank handler. */
static volatile int vdp1_present_pending = 0;

/* Wait one field (init only): used to space the two startup erases a frame apart so each
   manual change actually executes (FBCR latches but runs at the next field). */
static void vdp1_wait_field(void)
{
    while (TVSTAT & 0x0008) ;        /* leave the current vblank   */
    while (!(TVSTAT & 0x0008)) ;     /* wait for the next vblank-in */
}

/* OnVblank: present the VDP1 frame ONLY when its draw has finished (EDSR CEF = bit1).  In
   1-cycle mode VDP1 swapped its framebuffers every vblank, even mid-draw -> the multi-vblank
   wall list was shown half-rasterised = tearing.  FBCR = FCM|FCT (0x3) is a manual change:
   swap the buffers (show the completed frame) and erase the new back buffer.  FCM (mode) is
   sticky, FCT (trigger) self-clears -> between presents nothing swaps = the last complete
   frame is held.  No fps/latency cost: we don't wait, the swap is just deferred to draw-done. */
static void vdp1_vblank_present(void)
{
    if (vdp1_present_pending && (VDP1_EDSR & 0x0002))
    {
        VDP1_FBCR = 0x0003;          /* manual change: swap + erase the new back buffer */
        vdp1_present_pending = 0;
    }
}
#endif

/* One-time: build the fixed root (sysclip + JUMP, link -> empty bank) and the empty
   bank, then put VDP1 in 1-cycle auto (or manual-change) mode. */
static void vdp1_wpn_init(void)
{
    unsigned short cmd[16];

    memset(cmd, 0, sizeof cmd);
    cmd[0] = (unsigned short)(0x0009 | 0x1000);      /* system clip + JUMP_ASSIGN */
    cmd[1] = (unsigned short)((VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3);  /* link */
    cmd[10] = 319; cmd[11] = 223;
    vdp1_cmd_at(VDP1_ROOT_ADDR, 0, cmd);

    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A;                                 /* empty bank: local coord */
    vdp1_cmd_at(VDP1_BANKE_ADDR, 0, cmd);
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x8000;                                 /*             + end */
    vdp1_cmd_at(VDP1_BANKE_ADDR, 1, cmd);

    vdp1_bank = 0; vdp1_wactive = 0;
    for (int i = 0; i < WPN_TEX_SLOTS; ++i) wpn_cache[i].lump = -1;
    wpn_cache_rr = 0;
#if VDP1_WALL_TEST
    wtex_setup();                                    /* fixed per-slot VRAM addr + capacity */
    for (int i = 0; i < WTEX_SLOTS; ++i) { wtex_cache[i].texnum = -1; wtex_cache[i].lru = 0;
                                           wtex_cache[i].locked = 0; }
    wtex_tick = 0; wall_acc_n = 0;
#endif

    VDP1_TVMR = 0x0000;                              /* 16bpp, VBE=0 (erase in display: full-screen safe) */
    VDP1_EWDR = 0x0000;                              /* erase to 0 = transparent */
    VDP1_EWLR = 0x0000;
    VDP1_EWRR = (unsigned short)(((320 >> 3) << 9) | 223);
#if VDP1_MANUAL_CHANGE
    /* Manual-change double-buffer (anti-tear).  Clear BOTH framebuffers first: each change
       erases the buffer that becomes the new back buffer, so two changes (a field apart) wipe
       both -> no boot garbage in the index-0 (sky/ceiling) gaps.  Then stay in manual mode;
       the per-frame swap is driven by vdp1_vblank_present on draw-complete. */
    VDP1_PTMR = 0x0000;                              /* no draw; first plot kicked per-frame */
    VDP1_FBCR = 0x0003; vdp1_wait_field();           /* change + erase back buffer #1 */
    VDP1_FBCR = 0x0003; vdp1_wait_field();           /* change + erase back buffer #2 (both clear) */
    SRL::Core::OnVblank += vdp1_vblank_present;
#else
    VDP1_FBCR = 0x0000;                              /* 1-cycle auto erase+draw+swap (tears) */
    VDP1_PTMR = 0x0002;
#endif
}

/* core hook: begin this frame's player-sprite list in the OFF-screen bank. */
extern "C" void sat_vdp1_wpn_begin(void)
{
    unsigned short cmd[16];
    /* NO RE-BAKE ON FLASH: the wall cache stores raw palette indices and is NOT dropped on
       palette_changed (that re-baked every visible texture each flash frame -> the damage/pickup
       SLOWDOWN).  The flash re-tints the walls' CRAM light-banks instead (wtex_rebuild_banks, in
       DG_DrawFrame).  The weapon/HUD caches below are dead (software now) -- left harmless. */
    if (palette_changed)
    {
        for (int i = 0; i < WPN_TEX_SLOTS; ++i) wpn_cache[i].lump = -1;
        vdp1_hud_csum = 0xFFFFFFFFu;
    }
#if VDP1_DBLBANK
    vdp1_wbank = vdp1_bank ^ 1;                      /* the bank VDP1 isn't showing */
#else
    vdp1_wbank = vdp1_bank;                          /* TEST: single bank (no extra frame?) */
#endif
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A;                                 /* bank cmd0 = local coord */
    cmd[7] = VIEW_Y_OFFSET;                          /* local Y origin -> walls centred like NBG1 */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], 0, cmd);
    vdp1_wnext   = 1;
#if VDP1_WALL_TEST
    vdp1_walls_flush();   /* textured walls -- behind the weapon, in front of NBG1 */
#endif
    vdp1_wactive = 1;
}

/* core hook (per psprite): draw the weapon frame as a VDP1 sprite at the screen
   position.  The texture is CACHED by (lump, colormap) in stable VRAM, so it is
   unpacked only when the weapon frame OR the light changes -- not every frame; most
   frames only rewrite the (double-buffered) command.  Patch fields are little-endian
   (WAD) on the big-endian SH-2 -> byte-swap width/height/columnofs. */
extern "C" void sat_vdp1_wpn_draw(patch_t *patch, int lump, int sx, int sy, int flip,
                                  const unsigned char *cmap)
{
    int slot = -1, padW, H;

    if (vdp1_wnext >= VDP1_CMD_GUARD) return;         /* command-bank slot guard */

    for (int i = 0; i < WPN_TEX_SLOTS; ++i)          /* cache lookup: (lump, cmap) */
        if (wpn_cache[i].lump == lump && wpn_cache[i].cmap == cmap) { slot = i; break; }

    if (slot >= 0)
    {
        padW = wpn_cache[slot].padW;
        H    = wpn_cache[slot].H;
    }
    else
    {
        /* miss: unpack the patch into the next round-robin slot */
        int W = (int)bswap16((unsigned short)patch->width);
        H     = (int)bswap16((unsigned short)patch->height);
        padW  = (W + 7) & ~7;
        if ((unsigned int)(padW * H) * 2u > WPN_TEX_SLOTSZ) return;  /* too big to cache */

        slot = wpn_cache_rr;
        wpn_cache_rr = (wpn_cache_rr + 1) % WPN_TEX_SLOTS;

        volatile unsigned short *tex =
            (volatile unsigned short *)(WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ);
        for (int i = 0; i < padW * H; ++i) tex[i] = 0;   /* clear to transparent */

        const unsigned int *colofs = (const unsigned int *)patch->columnofs;
        for (int x = 0; x < W; ++x)
        {
            const post_t *post =
                (const post_t *)((const unsigned char *)patch + bswap32(colofs[x]));
            while (post->topdelta != 0xFF)
            {
                const unsigned char *s = (const unsigned char *)post + 3;
                int top = post->topdelta;
                for (int i = 0; i < post->length; ++i)
                    tex[(top + i) * padW + x] = pal_rgb555(cmap[s[i]]);
                post = (const post_t *)((const unsigned char *)post + post->length + 4);
            }
        }
        wpn_cache[slot].lump = lump; wpn_cache[slot].cmap = cmap;
        wpn_cache[slot].padW = padW; wpn_cache[slot].H = H;
    }

    unsigned int texaddr = WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ;
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = (unsigned short)(flip ? 0x0010 : 0x0000);  /* normal sprite, LR flip */
    cmd[2] = 0x00A8;                                    /* RGB (COLOR_5) | ECD off => SPD on */
    cmd[4] = (unsigned short)((texaddr - VDP1_VRAM_BASE) >> 3);  /* charAddr */
    cmd[5] = (unsigned short)(((padW >> 3) << 8) | H);          /* charSize */
    cmd[6] = (short)sx; cmd[7] = (short)sy;             /* point A = top-left */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
}

/* DG_DrawFrame, after the render + before the kick: append the status bar as a VDP1
   sprite ON TOP of the weapon, so the weapon no longer pokes over the HUD.  The
   texture (framebuffer rows 168-199 -> RGB555, opaque) is rebuilt only when the bar
   actually changes (cheap FNV checksum of the 8bpp region). */
static void vdp1_hud_emit(void)
{
    if (!vdp1_wactive || vdp1_wnext >= VDP1_CMD_GUARD) return;  /* only over a rendered level */

    const unsigned int *s32 = (const unsigned int *)(framebuffer + HUD_Y * 320);
    unsigned int csum = 2166136261u;
    for (int i = 0; i < HUD_W * HUD_H / 4; ++i) csum = (csum ^ s32[i]) * 16777619u;
    if (csum != vdp1_hud_csum)
    {
        vdp1_hud_csum = csum;
        const unsigned char *s = framebuffer + HUD_Y * 320;
        volatile unsigned short *tex = (volatile unsigned short *)VDP1_HUD_TEX;
        for (int i = 0; i < HUD_W * HUD_H; ++i) tex[i] = pal_rgb555(s[i]);
    }

    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x0000;                                   /* normal sprite */
    cmd[2] = 0x00A8;                                   /* RGB; every texel MSB=1 -> opaque */
    cmd[4] = (unsigned short)((VDP1_HUD_TEX - VDP1_VRAM_BASE) >> 3);
    cmd[5] = (unsigned short)(((HUD_W >> 3) << 8) | HUD_H);   /* charSize 0x2820 */
    cmd[6] = 0; cmd[7] = HUD_Y;                         /* top-left (0,168) */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
}


/* End of frame (DG_DrawFrame): close the off-screen bank, then flip the root LINK
   to it with a single atomic halfword write (race-free).  If the level view wasn't
   rendered (title/intermission), point the root at the empty bank instead. */
static void vdp1_wpn_kick(void)
{
    unsigned int link;
    vdp1_prev_done = (VDP1_EDSR & 0x0002) ? 1 : 0;   /* did the previous frame's plot finish? */
#if SHOW_FPS
    vdp1_last_cmds = vdp1_wactive ? vdp1_wnext : 0;
    /* accumulate the done-rate only on frames that actually plotted a world list
       (skip title/intermission empty banks, which always read 'D' and would inflate Dr). */
    if (vdp1_wactive) { vd1_win_tot++; if (vdp1_prev_done) vd1_win_done++; }
#endif
    if (vdp1_wactive)
    {
        unsigned short end[16];
        memset(end, 0, sizeof end);
        end[0] = 0x8000;
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext, end);
        link = (VDP1_BANK[vdp1_wbank] - VDP1_VRAM_BASE) >> 3;
        vdp1_bank = vdp1_wbank;
    }
    else
    {
        link = (VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3;
    }
    /* atomic single-halfword flip of the root command's jump target */
    *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = (unsigned short)link;
    VDP1_PTMR = 0x0002;              /* start the draw (clears EDSR CEF until it finishes) */
#if VDP1_MANUAL_CHANGE
    vdp1_present_pending = 1;        /* arm: vdp1_vblank_present swaps once this draw completes */
#endif
    vdp1_wactive = 0;
}

/* core sat_walls_done_hook: flush + kick the VDP1 walls right after the BSP walk so VDP1 draws
   in PARALLEL with the CPU floors/sprites and presents the SAME frame (no 1-frame lag = no sky
   at the CPU/VDP1 wall seam).  Sets a flag so DG_DrawFrame only kicks (empty bank) when NO level
   was rendered this frame (menu/intermission -> the hook didn't fire). */
static int vdp1_kicked_this_frame = 0;
extern "C" void sat_walls_kick(void)
{
    sat_vdp1_wpn_begin();
    vdp1_wpn_kick();
    vdp1_kicked_this_frame = 1;
}
#endif

#if DUAL_CPU_BLIT
/* Slave-done flag for the dual-CPU blit.  SH7604 is write-through so the slave's
   store reaches RAM, but the master must READ it uncached to see it (its cache
   would hold the stale 0) -- so go through the cache-through mirror (0x20000000),
   same trick as r_parallel.c's SYNC.  Plain static storage, accessed only via the
   mirror macro. */
static volatile int blit_slave_done_storage;
#define BLIT_SLAVE_DONE (*(volatile int *)((unsigned int)&blit_slave_done_storage | 0x20000000u))

/* Runs on the SLAVE SH-2 (dispatched from DG_DrawFrame): copy the BOTTOM rows of
   the framebuffer to VDP2 VRAM while the master copies the top.  'arg' carries the
   split row (passed by value in the dispatch record -> no cache-coherency concern).
   Purge first so the slave sees the master's (and its own earlier) write-through
   framebuffer pixels; DOOM_VRAM is uncached VDP2 VRAM so the writes need no flush. */
static void blit_slave_body(void *arg)
{
    int split = (int)(unsigned int)arg;
    volatile unsigned char *ccr = (volatile unsigned char *)0xFFFFFE92;
    *ccr = (unsigned char)(*ccr | 0x10);   /* cache purge on THIS (slave) CPU */
    for (int y = split; y < 224; ++y)
        memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE,
               framebuffer + y * 320, 320);
    BLIT_SLAVE_DONE = 1;
}
#endif

extern "C" void DG_DrawFrame(void)
{
    static int first_frame = 1;

    if (first_frame)
    {
        first_frame      = 0;
        console_enabled  = 0;
        sat_console_clear();
        dma_table_build();
#if VDP1_WEAPON
        vdp1_wpn_init();
#endif
        SRL::Debug::Print(0, 1, "FRAME1 OK               ");
    }

    /* SATURN sky -> VDP2: (re)upload on level/episode change; position the layer.
       SKY_FIXED keeps it static; otherwise scroll by viewangle (90deg = 256 sky
       px via SKY_ANGLESHIFT, slowed by SKY_PARALLAX_SHIFT; VDP2 wraps the plane). */
#if VDP2_HW_SKY
    if (skytexture > 0 && skytexture != sky_loaded_tex)
        sky_upload();
#if SKY_FIXED
    slScrPosNbg0(toFIXED(0.0), toFIXED(-(double)VIEW_Y_OFFSET));   /* centred like NBG1/VDP1 */
#else
    {
        /* Negated: invert the scroll direction (Romain -- the un-negated way felt
           wrong-way round; this was the real issue, not the speed). */
        int sx = -(int)(viewangle >> (SKY_ANGLESHIFT + SKY_PARALLAX_SHIFT));
        slScrPosNbg0((FIXED)(sx << 16), toFIXED(-(double)VIEW_Y_OFFSET));
    }
#endif
#endif

    /* SATURN sky index-0 reservation (index 0 = VDP2 transparent code; where NBG1
       has 0 the NBG0 sky behind shows through):
       (1) remap the scene colormap ONCE so the 3D view (walls/floors/sprites/fuzz,
           all via colormaps[]) and the software sky never emit 0;
       (2) gate the sky by NBG0's DISPLAY (slScrAutoDisp), NOT slScrTransparent --
           slScrTransparent rewrites the whole transparent-mask and clobbered SRL's
           text/back setup (black screen).  When an overlay owns the screen (menu/
           automap) or we're out of a level, drop NBG0 so UI index-0 shows the
           black back-screen instead of sky.  NBG1 keeps SRL's default transparency.
       (3) scrub the status-bar rows' 0 -> near-black (direct-palette UI that
           bypasses the colormap) while the sky is shown. */
    {
        static int cmap_done = 0;
        unsigned char nb = (unsigned char)sat_near_black();
        if (!cmap_done && colormaps && skytexture > 0)
        {
            for (int i = 0; i < SAT_CMAP_BYTES; ++i)
                if (colormaps[i] == 0) colormaps[i] = nb;
            cmap_done = 1;
        }
        /* Keep the sky shown while the pause menu is up (menuactive): the menu
           draws opaque patches over the frozen game frame, so the sky belongs
           behind it.  Drop the sky only for the automap (its index-0 background
           would otherwise show sky) and outside a level. */
        (void)menuactive;
        /* SATURN: drop the hardware sky (NBG0) when there is NO sky visplane in view this
           frame (fully-enclosed room) -> the VDP1 walls' torn index-0 gaps then show the dark
           backdrop instead of the bright sky, so the tearing is far less visible.
           Also drop it in 2-player: NBG0 is a single layer scrolled by one viewangle and
           cannot serve two split views -> the split renders the SOFTWARE sky instead. */
        extern int sat_local_players;
        int show_sky = (gamestate == GS_LEVEL) && !automapactive && sat_frame_has_sky
                       && sat_local_players <= 1;
#if VDP2_RBG0_TEST
        /* RBG0/debug 3-mode cycle (rbg0_mode, pad Y) -- see the rbg0_mode decl:
           0 = VDP2 floor, no dbg   (RBG0 on, NBG3 off, sw floor skipped)
           1 = dbg + software floor (RBG0 off, NBG3 on, sw floor drawn)
           2 = dbg, no software floor (RBG0 off, NBG3 on, sw floor skipped). */
        /* SATURN: the VDP2/RBG0 floor is ONE rotation plane -> use it ONLY at full detail
           (potato 0) and in 1-player; in any potato level OR split-screen it falls back to the
           SOFTWARE floor (sat_vdp2_floor=0 -> the sw floor draws; RBG0 display off). */
        int rbg0_active   = (potato_level == 0) && (sat_local_players <= 1);
        sat_vdp2_floor    = (rbg0_mode == 1 || !rbg0_active) ? 0 : 1;  /* skip the sw floor only when the HW floor shows */
        uint16_t sky_bit  = (VDP2_HW_SKY && show_sky) ? NBG0ON : 0;   /* no NBG0 when sky is software */
        uint16_t rbg0_bit = (RBG0_DISPLAY && rbg0_mode == 0 && rbg0_active) ? RBG0ON : 0;   /* HW floor: pot0 + 1p only */
        uint16_t nbg3_bit = (RBG0_NBG3 && nbg3_show) ? NBG3ON : 0;  /* NBG3 overlay: display = pad L+R (default off); B1 cycle reserved at init */
        slScrAutoDisp((uint16_t)(sky_bit | NBG1ON | nbg3_bit | rbg0_bit));
#else
        slScrAutoDisp((uint16_t)(show_sky ? (NBG0ON | NBG1ON | NBG3ON)
                                          : (NBG1ON | NBG3ON)));
#endif
        if (show_sky)
            for (int i = 192 * 320; i < 224 * 320; ++i)   /* status-bar rows (224: 192..223) */
                if (framebuffer[i] == 0) framebuffer[i] = nb;
    }

#if VDP2_RBG0_TEST
    /* When the floor toggle is on: upload the player's floor texture to RBG0 (only when the
       flat changes), then re-write its rotation params from the matrix each frame.
       NOTE: slScrMatSet only fills SGL's CACHED RAM buffer + a dirty flag; the RPT VRAM transfer is
       done by the _BlankIn ISR, armed ONLY by slSynch (disasm-proven, docs/RBG0_STRUCTURED_GARBAGE.md).
       So the transform never reaches VRAM without RBG0_RPT_TRANSFER below. */
    if (rbg0_mode == 0 && sat_vdp2_floor)   /* sat_vdp2_floor folds in the pot0 + 1p gate (set above) */
    {
        rbg0_upload_flat(sat_vdp2_floor_pic);
        rbg0_set_transform();
#if RBG0_RPT_TRANSFER == 1
        slSynch();   /* Test A: per-frame slSynch -> _BlankIn transfers the RPT.  Confirms the cause
                        (the floor should warp into perspective), but caps fps + mutes SCSP SFX. */
#elif RBG0_RPT_TRANSFER == 2
        /* Test B (the real fix): reproduce _BlankIn's RPT DMA, NO slSynch.  Source = SGL's RAM RPT
           buffer read via the UNCACHED 0x26 alias (so slScrMatSet's cached stores are seen); dest =
           the RPT VRAM at VDP2_VRAM_B1 + 0x1ff00.  0x30 bytes/plane (RA, then RB at +0x68). */
        memcpy((void *)0x25E7FF00,          (const void *)0x260FFE1C, 0x30);
        memcpy((void *)(0x25E7FF00 + 0x68), (const void *)0x260FFE84, 0x30);
#endif
    }
#endif

#if VDP1_WEAPON
    /* LAYER INVERSION: VDP1 carries ONLY the walls (below NBG1).  During a LEVEL render the
       early hook (sat_walls_kick, right after the BSP walk) already flushed+kicked so VDP1
       presents the same frame.  Only kick HERE when it did NOT fire (menu/intermission: no
       R_RenderPlayerView) -> the empty bank clears any stale walls.  Both before the
       palette_changed reset so the wall cache re-tints on a damage/pickup flash. */
    if (!vdp1_kicked_this_frame) { sat_vdp1_wpn_begin(); vdp1_wpn_kick(); }
    vdp1_kicked_this_frame = 0;
#endif

    if (palette_changed)
    {
        palette_changed = false;
        for (int x = 0; x < 256; ++x)
            pending_cram[x] = (unsigned short)
                (0x8000 |
                 ((colors[x].b >> 3) << 10) |
                 ((colors[x].g >> 3) << 5)  |
                 (colors[x].r >> 3));
        palette_dirty = 1;
#if VDP1_WALL_TEST
        /* 8bpp walls: re-shade the dark CRAM light-banks from the (possibly flashed) palette.
           CRAM-only, NO texture re-bake -> the damage-flash spike stays gone AND the dark walls
           flash in sync with bank 1 / the software floors+sprites.  Uploaded next vblank. */
        wtex_rebuild_banks();
#endif
    }

#if SHOW_FPS
    dg_frame_count++;
    fps_update();
#endif

    /* SATURN: no per-frame slSynch / SRL::Core::Synchronize here -- the freeze is
       handled by rp_sgl_workptr_reset() (core/r_parallel.c) resetting BOTH the
       slave write (GBR+72) and read (GBR+68) pointers each frame.  That avoids
       slSynch's vblank-cap (~7-12fps) and its SCSP-sound conflict (silent SFX),
       so we keep the full parallel speed and working sound. */

#if !USE_SCU_DMA
    /* CPU blit fallback (no SCU DMA).  The raw SCU DMA blit below hangs the
       SH-2 bus on real hardware; until that is fixed properly this plain CPU
       copy keeps the game runnable on hardware (slow).  Purge first so the
       master sees the slave's write-through framebuffer pixels. */
    /* Menus/title/intermission are 320x200 assets; on the 224 framebuffer rows 200..223 are
       uncovered.  Outside a level (no ST_Drawer), blacken that strip so it's not stale garbage. */
    if (gamestate != GS_LEVEL)
        memset(framebuffer + 200 * 320, 0, 24 * 320);
    {
        /* SATURN 2p: paint the two compact-HUD panels into the bottom 64 rows
           (rows 160..223), draw each player's widgets on top (P1 left, P2 right),
           then apply each player's damage/pickup flash as a per-half wash -- all
           into the framebuffer before the blit.  (2-player only; the 3/4-player
           quadrant HUD is a later iteration -> those use full-screen views.) */
        extern int sat_local_players;
        if (sat_local_players == 2 && gamestate == GS_LEVEL)
        {
            hud2p_blit_panels();
            ST_DrawCompactWidgets(0, 0,   HUD2P_TOP);   /* P1 (left)  */
            ST_DrawCompactWidgets(1, 160, HUD2P_TOP);   /* P2 (right) */
            hud2p_apply_flash();                        /* per-half damage/pickup flash */
        }
    }
    unsigned short blit_t0 = frt_read();   /* SATURN PERF: time the blit (-> sat_blit_ms10) */
#if DUAL_CPU_BLIT
    if (blit_cfg[blit_mode].dual)
    {
        /* Dual-CPU blit: dispatch the bottom rows [split,224) to the idle slave, copy
           the top [0,split) on the master, then WAIT for the slave before clearing/
           returning (else next frame's render overwrites the framebuffer mid-copy =
           tearing).  rp_sgl_workptr_reset rewinds the SGL slave work pointer so this
           2nd dispatch per frame can't creep into the freeze -- and covers the
           rp_disabled serial frame (where rp_restart did not reset it this frame). */
        int split = blit_split();
        rp_sgl_workptr_reset();
        BLIT_SLAVE_DONE = 0;            /* uncached: visible to the slave before it sets 1 */
        cache_purge();                 /* master purges before reading its half */
        slSlaveFunc(blit_slave_body, (void *)(unsigned int)split);
        for (int y = 0; y < split; ++y)
            memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
        {
            /* Bounded wait: hang-safe in the freeze zone.  If the slave never signals
               (wedge), the master copies the bottom rows itself -> a slow frame, not a
               hard freeze.  Guard is generous (slave half ~6-8ms; master is already
               past its half here so it normally spins only briefly). */
            int guard = 30000000;
            while (!BLIT_SLAVE_DONE && --guard) ;
            if (!guard)
                for (int y = split; y < 224; ++y)
                    memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE,
                           framebuffer + y * 320, 320);
        }
    }
    else
#endif
    {
        /* Single-CPU blit: master copies the whole picture (also the compile-out path). */
        cache_purge();
        for (int y = 0; y < 224; ++y)   /* native 224: blit the full picture (VIEW_Y_OFFSET = 0) */
            memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
    }
    {
        /* SATURN PERF: blit wall-clock = master FRT delta across the copy (incl. slave join). */
        unsigned short blit_t1 = frt_read();
        sat_blit_ms10 = ((unsigned int)(unsigned short)(blit_t1 - blit_t0) * ns_per_frt) / 100000u;
    }
    /* LAYER INVERSION: clear the 3D VIEW to index 0 so next frame the SKIPPED wall columns stay
       transparent -> the VDP1 walls (below NBG1) show through.  The HUD rows are left intact
       (1p: status bar 192..223 owned by ST_Drawer; 2p: panels 160..223 owned by hud2p; 3/4p:
       no HUD -> clear the whole frame). */
    {
        extern int sat_local_players;
        int clear_rows = (sat_local_players >= 3) ? 224 : (sat_local_players == 2 ? 160 : 192);
        memset(framebuffer, 0, clear_rows * 320);
    }
    return;
#endif

    /* SCU DMA blit -- STEP 4a reliability probe.  D0MD start factor = 0
       (VBLANK-IN) instead of 7 (immediate): the transfer is deferred to vblank
       so it no longer fights the VDP for the B-bus during active display (the old
       hang).  Still SYNCHRONOUS (wait for previous, kick, wait for this one) -- so
       this is NOT a speed win yet (we pay the vblank latency + transfer); it only
       answers "does VBLANK-IN stop the hang and render correctly?".  If stable,
       step 4b drops the wait-after and adds a pre-render wait to reclaim the time. */
#if SHOW_FPS
    {
        unsigned short t0, t1;
        unsigned char  h, l;

        h  = *(volatile unsigned char *)0xFFFFFE12;
        l  = *(volatile unsigned char *)0xFFFFFE13;
        t0 = (unsigned short)((h << 8) | l);

        dma_wait_idle();

        cache_purge();
        SCU_D0W  = (unsigned int)dma_table;
        SCU_D0AD = 0x101;
        SCU_D0MD = 0x01000000;   /* indirect + start factor 0 = VBLANK-IN */
        SCU_D0EN = 0x101;
        dma_wait_idle();

        h  = *(volatile unsigned char *)0xFFFFFE12;
        l  = *(volatile unsigned char *)0xFFFFFE13;
        t1 = (unsigned short)((h << 8) | l);
        last_dma_ticks = (unsigned short)(t1 - t0);
    }
#else
    dma_wait_idle();
    cache_purge();
    SCU_D0W  = (unsigned int)dma_table;
    SCU_D0AD = 0x101;
    SCU_D0MD = 0x01000000;   /* indirect + start factor 0 = VBLANK-IN */
    SCU_D0EN = 0x101;
    dma_wait_idle();
#endif
}

extern "C" uint32_t DG_GetTicksMs(void)
{
    /* safe_ms: last known-good ms value.
       last_fv:  frt_at_vbl snapshot at last safe call â€” used as a real-vblank
                 discriminator when us_acc looks corrupted.  frt_at_vbl is set
                 by the ISR once per vblank; it changes at most once per real
                 frame regardless of how many times DG_GetTicksMs is called.
                 When us_acc is stomped by a rogue slave write, safe_ms advances
                 by exactly A = us_per_frame/1000 ms per real vblank detected
                 via frt_at_vbl change â€” preventing both the prev_ms+17-per-call
                 runaway and the single-spike-then-reset failure mode. */
    static uint32_t       safe_ms  = 0;
    static unsigned short last_fv  = 0;
    unsigned long long us_snap;
    unsigned short fv, f;
    unsigned int sr, sr_masked;
    uint32_t result;

    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    sr_masked = sr | 0xF0;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr_masked) : "memory");
    us_snap = us_acc;
    fv      = frt_at_vbl;
    f       = (unsigned short)(((unsigned short)FRT_FRCH << 8) | (unsigned short)FRT_FRCL);
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");

    result = (uint32_t)((us_snap + ((unsigned short)(f - fv) * ns_per_frt) / 1000) / 1000);

    if (result > 7200000U ||
        (safe_ms > 0U && result > safe_ms + 5000U))
    {
        /* Corruption detected: advance safe_ms by A ms only if a real vblank
           occurred (frt_at_vbl changed).  All calls within the same vblank
           see the same fv â†’ same safe_ms, unlike prev_ms+17 per call. */
        uint32_t A = us_per_frame / 1000U;
        if (fv != last_fv)
        {
            safe_ms += A;
            last_fv  = fv;
        }
        return safe_ms;
    }

    safe_ms = result;
    last_fv = fv;
    return result;
}

extern "C" void DG_SleepMs(uint32_t ms)
{
    uint32_t start = DG_GetTicksMs();
    while (DG_GetTicksMs() - start < ms)
        ;
}

/* ------------------------------------------------------------------ */
/* Input: SMPC digital pad -> Doom key events                          */
/* ------------------------------------------------------------------ */

typedef struct { unsigned short mask; unsigned char key; } pad_map_t;

static const pad_map_t pad_map[] =
{
    { PER_DGT_KU, KEY_UPARROW   },
    { PER_DGT_KD, KEY_DOWNARROW },
    { PER_DGT_KL, KEY_LEFTARROW },
    { PER_DGT_KR, KEY_RIGHTARROW},
    { PER_DGT_ST, KEY_ESCAPE    },
    { PER_DGT_TA, KEY_FIRE      },
    { PER_DGT_TB, KEY_USE       },
    { PER_DGT_TC, KEY_RSHIFT    },
    { PER_DGT_TX, KEY_TAB       },
    { PER_DGT_TY, 'y'           },
    { PER_DGT_TL, ','           },
    { PER_DGT_TR, '.'           },
};
#define PAD_MAP_LEN (sizeof(pad_map) / sizeof(pad_map[0]))

#define KEYQ_LEN 32
static unsigned char keyq[KEYQ_LEN];
static int keyq_head = 0, keyq_tail = 0;

static void keyq_push(int pressed, unsigned char key)
{
    int next = (keyq_tail + 1) % KEYQ_LEN;
    if (next == keyq_head) return;
    keyq[keyq_tail] = (unsigned char)(key & 0x7f) | (pressed ? 0x80 : 0);
    keyq_tail = next;
}

static unsigned char keyq_decode(unsigned char k)
{
    switch (k)
    {
        case 1: return KEY_UPARROW;
        case 2: return KEY_DOWNARROW;
        case 3: return KEY_LEFTARROW;
        case 4: return KEY_RIGHTARROW;
        case 5: return KEY_FIRE;
        case 6: return KEY_USE;
        case 7: return KEY_RSHIFT;
        default: return k;
    }
}

static unsigned char keyq_encode(unsigned char key)
{
    switch (key)
    {
        case KEY_UPARROW:   return 1;
        case KEY_DOWNARROW: return 2;
        case KEY_LEFTARROW: return 3;
        case KEY_RIGHTARROW:return 4;
        case KEY_FIRE:      return 5;
        case KEY_USE:       return 6;
        case KEY_RSHIFT:    return 7;
        default: return key;
    }
}

/* Local-multiplayer opt-in (docs/MULTIPLAYER_PLAN.md, Iter 1): the platform owns the title-screen
   gesture.  The title arms sat_armed_players (applied to the live sat_local_players by G_DoNewGame);
   in-game, START on pad 2 cycles the live sat_local_players directly via the sat_dropin_want hook. */
extern "C" {
    extern int sat_local_players;       /* core: LIVE count (1 = single player) */
    extern int sat_armed_players;       /* core: title-armed count for the NEXT new game */
    extern int sat_dropin_want;         /* core: in-game drop-in request (G_SatDropInService) */
    extern int sat_split_vdp1;          /* core: split-screen keeps walls on VDP1 per-view (vs software) */
    extern int usergame;                /* core: true only during a real player-started game */
    int sat_count_local_pads(void);     /* mp_input.cxx: connected local pads, 1..4 */
    int sat_mp_pad2_a(void);            /* mp_input.cxx: 1 while pad-2 holds A */
    int sat_mp_pad2_start(void);        /* mp_input.cxx: 1 while pad-2 holds START */
}

static void poll_pad(void)
{
    static unsigned short prev = 0xffff;
    static unsigned int   last_poll_frame = 0;

    if (vbl_count == last_poll_frame) return;
    last_poll_frame = vbl_count;

    if (Smpc_Peripheral[0].id == PER_ID_NotConnect) return;

    unsigned short cur     = Smpc_Peripheral[0].data;
    unsigned short changed = cur ^ prev;
    prev = cur;

    /* Pad Z (unmapped in pad_map) cycles the Potato level live (0 off -> 1 floors flat
       -> 2 + VDP1 walls flat / low-detail), for A/B testing quality vs fps without a rebuild. */
    if ((changed & PER_DGT_TZ) && !(cur & PER_DGT_TZ))
    {
        potato_level = (potato_level + 1) % 7;
        sat_apply_potato();
    }

#if VDP2_RBG0_TEST
    /* Pad Y = the floor A/B comparator: 0 = VDP2 hardware floor (RBG0) <-> 1 = software floor.
       Mode 2 (no floor) is dropped from the cycle during tuning (user) -- only the two floors
       that matter for the side-by-side.  (Y also taps 'y' to Doom -- harmless.) */
    if ((changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
        rbg0_mode = (rbg0_mode + 1) % 2;
#if RBG0_NBG3
    /* Pad L+R (chord) toggles the NBG3 debug overlay (default OFF).  The B1 cycle is reserved at
       init (slScrAutoDisp(NBG3ON) + no scrub), so this only flips BGON.  (L/R also tap ','/'.' to
       Doom -- harmless; L+R is free since SAT_DIAG_SLAVE_TOGGLES=0.) */
    {
        const unsigned short lr = (unsigned short)(PER_DGT_TL | PER_DGT_TR);
        static int lr_was = 0;
        int lr_now = ((cur & lr) == 0);          /* both held (active-low) */
        if (lr_now && !lr_was) nbg3_show = !nbg3_show;
        lr_was = lr_now;
    }
#endif
#if RBG0_LINECOL_TEST
    /* Pad C toggles the floor line-color darken A/B (RBG0_LINECOL_TEST).  Direct-poke CCRR
       (0x10C, outside the block-flush) -- the only register that changes at runtime; the SGL
       enables are already committed at init.  (C also taps run to Doom -- harmless.) */
    if ((changed & PER_DGT_TC) && !(cur & PER_DGT_TC)) {
        rbg0_linecol_on = !rbg0_linecol_on;
        *(volatile unsigned short *)0x25F8010C = (unsigned short)(rbg0_linecol_on ? 24 : 0);
    }
#endif
#if RBG0_TUNE_PAD
    /* PARKED live floor tuning (RBG0_TUNE_PAD) -- the found values are baked as defaults:
       L + C     = cycle the TEXTURE orientation over the 8 D4 symmetries (rotation + mirror).
       L + d-pad = shift the TEXTURE +-1 texel on X/Y (re-shades the bitmap).
       R + d-pad = nudge the PLANE (transform, live): up/down = inclination (pitch),
                   left/right = the near level (Z).
       (L taps ',' and R taps '.' to Doom -- harmless; the d-pad is gated from Doom while held.) */
    if (!(cur & PER_DGT_TL)) {                                       /* L held: texture */
        if ((changed & PER_DGT_TC) && !(cur & PER_DGT_TC)) { rbg0_tex_orient = (rbg0_tex_orient + 1) & 7; rbg0_tex_dirty = 1; }  /* L+C: orientation */
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) { rbg0_tex_yoff = (rbg0_tex_yoff + 1) & 63; rbg0_tex_dirty = 1; }
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) { rbg0_tex_yoff = (rbg0_tex_yoff - 1) & 63; rbg0_tex_dirty = 1; }
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL)) { rbg0_tex_xoff = (rbg0_tex_xoff + 1) & 63; rbg0_tex_dirty = 1; }
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR)) { rbg0_tex_xoff = (rbg0_tex_xoff - 1) & 63; rbg0_tex_dirty = 1; }
    }
    if (!(cur & PER_DGT_TR)) {                                       /* R held: plane geometry */
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) rbg0_pitch_adj += 0x80;        /* +pitch (~0.7deg) */
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) rbg0_pitch_adj -= 0x80;        /* -pitch           */
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL)) rbg0_z_adj += (4 << 16);       /* +level (4 units) */
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR)) rbg0_z_adj -= (4 << 16);       /* -level           */
    }
#endif
#elif SAT_FLOOR_PERFSIM
    /* Pad Y cycles the 4 floor PERF-SIM modes (read REC/P rows 4/5 in each = the floor-offload
       ceiling, valid for RBG0 / VDP1-strips / gradient alike).  No RBG0/RAMCTL -> overlay stays
       readable; skipped surfaces show the backdrop.  See SAT_FLOOR_PERFSIM. */
    if ((changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    {
        floor_perfsim_mode  = (floor_perfsim_mode + 1) % 4;
        sat_vdp2_floor      = (floor_perfsim_mode == 1 || floor_perfsim_mode == 3) ? 1 : 0;  /* skip dominant */
        sat_vdp1_floor      = (floor_perfsim_mode == 2 || floor_perfsim_mode == 3) ? 1 : 0;  /* skip secondary */
        sat_floor_vdp1_hook = sat_floor_perfsim_hook;  /* claims all-but-dominant (only consulted when flag on) */
    }
#elif SAT_DIAG_SLAVE_TOGGLES
    /* Pad Y: diagnostic A/B of the visplane split.  ws0 = static half-split (default, good);
       ws1 = two-pointer work-steal.  DEAD-END on HW (the steal regresses at E1M1; REC_BENCHMARKS
       §C.2 H) -- kept revivable behind SAT_DIAG_SLAVE_TOGGLES.  Row 1 shows ws<state>; the window
       auto-resets on the flip. */
    if ((changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
        sat_plane_steal = !sat_plane_steal;
#else
    /* Pad Y free (the ws diagnostic is parked, SAT_DIAG_SLAVE_TOGGLES=0) -- only taps 'y' to Doom. */
#endif

#if DUAL_CPU_BLIT && SAT_DIAG_SLAVE_TOGGLES
    /* Pad L+R (chord): diagnostic A/B of the deferred wall-prep flush onto the SLAVE (RANK3_WALLPREP).
       wp 0 master inline / 1 slave+purge / 2 slave+warm.  ON ties sat_wallprep_defer (walls queued).
       DEAD-END on HW (slave +5.8ms, cold cache it can't keep warm) -- kept revivable behind
       SAT_DIAG_SLAVE_TOGGLES.  Both held = active-low = (cur & (L|R)) == 0; fire once on the edge. */
    {
        const unsigned short lr = (unsigned short)(PER_DGT_TL | PER_DGT_TR);
        static int lr_was = 0;
        int lr_now = ((cur & lr) == 0);
        if (lr_now && !lr_was) {
            sat_wallprep_slave = (sat_wallprep_slave + 1) % 3;  /* 0 master / 1 slave+purge / 2 slave+WARM */
            sat_wallprep_defer = (sat_wallprep_slave != 0);     /* queue walls iff the slave will flush them */
        }
        lr_was = lr_now;
    }
    /* (SAT_DIAG_SLAVE_TOGGLES=0: L+R free -- only tap ','/'.' to Doom.) */
#endif

    /* Split-screen wall-path A/B (live, mid-game): in local multiplayer, pad-1 X toggles the
       half-views' walls between VDP1 (sat_split_vdp1=1, the new mode) and pure software
       (sat_split_vdp1=0, the baseline) so both can be compared on the same scene on hardware.
       Gated to sat_local_players>1 so X is inert in single-player.  In split the X->KEY_TAB
       (automap) forward is suppressed below so X is ONLY this toggle; 1p keeps X = automap. */
    if (sat_local_players > 1 && (changed & PER_DGT_TX) && !(cur & PER_DGT_TX))
        sat_split_vdp1 = !sat_split_vdp1;

    /* Local co-op opt-in: outside a level (title/menu/demo), the 2nd pad's A toggles local
       multiplayer on (= the detected pad count, 2..4) / off (1p).  G_DoNewGame reads
       sat_local_players lazily at skill-confirm, so arming it at the title is enough; gating to
       non-level means it can never change mid-game (the split render + ticcmd build read it live).
       The gesture IS the A/B toggle -- don't arm = 1p (VDP1 hybrid), arm = Np split, same disc. */
    {
        static int p2s_was = 0, shown = -2;
        int p2s_now = sat_mp_pad2_start();
        if (p2s_now && !p2s_was)
        {
            if (!usergame)
                /* Attract loop: ARM the count for the next New Game (separate from the live
                   in-game count, so a prior drop-in never leaks here).  Cycle 1 -> 2 -> 3 -> 4 -> 1
                   so 3/4-player can be forced for testing even when the emulator only exposes 2 pads
                   (J3/J4 mirror J1/J2 -- see mp_input.cxx). */
                sat_armed_players = (sat_armed_players >= 4) ? 1 : sat_armed_players + 1;
            else if (gamestate == GS_LEVEL)
                /* In-game drop-in: cycle the LIVE count 1 -> 2 -> 3 -> 4 -> 1.  G_SatDropInService
                   (top of G_Ticker) spawns the new marines at their co-op starts, or despawns them
                   on the 4 -> 1 wrap (refs cleared first).  Serviced next tic. */
                sat_dropin_want = (sat_local_players >= 4) ? 1 : sat_local_players + 1;
        }
        p2s_was = p2s_now;

        /* Attract-loop feedback (the armed count) so the gesture is visibly confirmed before
           starting a game; cleared once a real game is running so it never lingers over the view. */
        if (!usergame)
        {
            int n = sat_armed_players;
            if (n != shown) { SRL::Debug::Print(0, 23, "PLAYERS: %d  (START on pad 2 cycles)", n); shown = n; }
        }
        else if (shown != -2) { SRL::Debug::Print(0, 23, "                                        "); shown = -2; }  /* 40 spaces: clear the FULL row (msg is 35 chars, an old 32-space clear left "es)") */
    }

    for (unsigned int i = 0; i < PAD_MAP_LEN; ++i)
    {
        if (changed & pad_map[i].mask)
        {
            int pressed = !(cur & pad_map[i].mask);
#if RBG0_TUNE_PAD
            /* PARKED (RBG0_TUNE_PAD): while L (texture) or R (plane) is held the d-pad tunes the
               floor (above) -- do NOT also forward it to Doom, so the player stays put.  (Press
               L/R FIRST, then tap the d-pad, to avoid a held direction sticking in Doom.) */
            if ((!(cur & PER_DGT_TL) || !(cur & PER_DGT_TR)) &&
                (pad_map[i].mask == PER_DGT_KU || pad_map[i].mask == PER_DGT_KD ||
                 pad_map[i].mask == PER_DGT_KL || pad_map[i].mask == PER_DGT_KR))
                continue;
#endif
            /* SATURN: in split, pad-X is the live sat_split_vdp1 A/B toggle (above),
               so DON'T also forward its KEY_TAB (= automap) -- the minimap would open
               over the 3D view and ruin the comparison.  1p keeps X = automap. */
            if (pad_map[i].mask == PER_DGT_TX && sat_local_players > 1)
                continue;
            keyq_push(pressed, keyq_encode(pad_map[i].key));
            if (pad_map[i].mask == PER_DGT_TA)
                keyq_push(pressed, KEY_ENTER);
        }
    }
}

extern "C" int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    poll_pad();
    if (keyq_head == keyq_tail) return 0;
    *pressed = (keyq[keyq_head] & 0x80) != 0;
    *doomKey = keyq_decode(keyq[keyq_head] & 0x7f);
    keyq_head = (keyq_head + 1) % KEYQ_LEN;
    return 1;
}

extern "C" void DG_SetWindowTitle(const char *title) { (void)title; }

/* ------------------------------------------------------------------ */
/* Main Doom entry (called from run_on_doom_stack in main.cxx)         */
/* ------------------------------------------------------------------ */

extern "C" void doomgeneric_Create(int argc, char **argv);
extern "C" void doomgeneric_Tick(void);

extern "C" void doom_start(void)
{
#ifdef SAT_WARP_MAP
    /* SATURN: benchmark warp -- boot straight into a map, skipping the menu.
       SAT_WARP_MAP is the core's -warp argument string: "15" (Doom II MAPxx)
       or "4 2" (Doom 1 ExMy, two single digits).  SAT_WARP_SKILL = skill 1-5
       (4 = Ultra-Violence).  Set via the Makefile, e.g. `make SAT_WARP_MAP=15`.
       Undefined (default) keeps the normal menu boot below. */
#  ifndef SAT_WARP_SKILL
#    define SAT_WARP_SKILL "4"
#  endif
    static char  warpbuf[32] = SAT_WARP_MAP;     /* mutable copy for strtok */
    static char *argv[12];                       /* static: core keeps myargv */
    int argc = 0;
    argv[argc++] = (char *)"doom";
    argv[argc++] = (char *)"-warp";
    for (char *tok = strtok(warpbuf, " "); tok && argc < 9; tok = strtok(NULL, " "))
        argv[argc++] = tok;
    argv[argc++] = (char *)"-skill";
    argv[argc++] = (char *)SAT_WARP_SKILL;
    argv[argc]   = 0;
    doomgeneric_Create(argc, argv);
#else
    static char *argv[] = { (char *)"doom", 0 };
    doomgeneric_Create(1, argv);
#endif
    for (;;)
        doomgeneric_Tick();
}
