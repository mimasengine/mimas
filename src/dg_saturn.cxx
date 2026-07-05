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
#include "m_fixed.h"   /* FixedDiv: SH-2 hardware DIV0U divide (~37cyc) -- see fxdiv below */
#include "v_patch.h"   /* patch_t / post_t (for the VDP1 weapon sprite) */
#include "r_parallel.h"
}
#include "hud2p_panel.h"   /* generated 2-player compact HUD panel + field anchors */
#include "hud4p_panel.h"   /* generated 3/4-player compact HUD band bevel column + anchors */

/* Set by D_SetGameDescription() from the loaded IWAD's lumps; we surface it on
   the debug overlay (row 21) to confirm which WAD the binary actually detected.
   See D_FindIWAD/D_IdentifyVersion -- the *mission=none change makes this honest
   for Doom 1 and Doom 2 alike. */
extern "C" char *gamedescription;

#define SHOW_FPS 1

/* Set to 1 to ignore the RAM cart and always stream the WAD from CD (e.g. to
   run on a 4MB emulator the same way a no-cart / 1M-cart system would). */
#define FORCE_CD_STREAM 0

/* Framebuffer->VDP2 blit: plain CPU copy (~10ms/frame).  SCU-DMA and dual-CPU-blit were both
   pursued and are DEAD: SCU->VDP2 hangs the B-bus (SEGA SCU manual -- no CPU A/B-bus access during
   an SCU-DMA B-bus transfer, SDRAM refresh stalls), and the dual-CPU split never beat single
   (bus-bound).  See docs/BLIT_DMA_PLAN.md.  The blit is permanently a single-CPU copy. */

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

/* Floor/ceiling TEXTURING on VDP1 (Option-1 step 2).  Pad Y (alone) cycles the mode LIVE (the
   in-session A/B law -- never judge build-vs-build):
     0 FLAT-FB   flat quads, full-bright potato colour     = the step-1 baseline (byte-identical)
     1 FLAT-LIT  + per-trapezoid CRAM distance-light bank  = restores Doom's light diminishing
     2 TEXTURED  + world-64x64-tile DISTORSP near-field    = real flat texels near, lit flat far
     3 TEX+CPU   textured near; planes reaching PAST the texture band render as the ORIGINAL
                 software spans (full quality, master cost) instead of the lit flat -- the A/B
                 for "flat far-field vs CPU far-field" (tall-room ceilings)
     4 SOFTWARE  reference picture: walls AND floors/ceilings software (RBG0 keeps the dominant
                 flat) -- the ground-truth image + the cost of the whole VDP1 world offload
     5 SW+WALLS  RBG0 dominant + SOFTWARE floors/ceilings + VDP1 walls + the TEXTURED wall-lag
                 catch-up band (core fclaim 3): the pre-Option-1 shipping config, with the
                 moving-junction black sliver covered by real plane texels
   When 1, pad Y is THIS cycle and the SAT_FLOOR_PERFSIM binding yields (set 0 to give Y back). */
#define SAT_FLOOR_TEX 1
#define FTEX_PLANE_CPU_PX 24000    /* per-PLANE cost threshold (owner's wall-budget idea applied to
                                      the claim): a plane bigger than this on screen goes WHOLE to
                                      the software spans (+ wall-lag band) instead of monopolising
                                      the shared tile budget. */
#define FTEX_PX_BUDGET 60000       /* shared tile-fill budget (clipped px): enforced at CLAIM time
                                      (planes past it render software+band instead of being punched
                                      then cut by an overrunning plot) AND at flush time (tiles are
                                      emitted nearest-first up to it). */
#define FTEX_CPU_NEARDIST 96       /* mode 3: rows NEARER than this many world units go to the CPU
                                      spans (magnified tiles = ms of VDP1 iteration for few px --
                                      the s-skip flat band the owner kept hitting; CPU spans are
                                      cheap exactly there).  The mode-3 punch is thus bounded BOTH
                                      sides: tiles serve [96..FTEX_MIPDIST] only. */
#define FTEX_MIPDIST  768          /* world units: textured nearer, lit-flat (or CPU, mode 3) farther.
                                      Extended 256->384->768 (owner: "affichons plus"): affordable
                                      because tiles past FTEX_MIP1/2DIST sample the 32x32 / 16x16
                                      DECIMATED mips (palette indices can't be averaged), so the
                                      minification fetch stays bounded. */
#define FTEX_NEAR_MIN_ROWS 12      /* ADAPTIVE m3 (owner 2026-07-03 "corriger la ou c'est visible"):
                                      the CPU near-band machinery (span trim + punch_nrow + the
                                      texcol residue) only pays where the band is TALL enough for
                                      the m2 flat patches to show (HW spot-5: s48).  A plane whose
                                      near band is under this many screen rows claims m2-style
                                      instead: VDP1 keeps the whole depth, the lit underlay covers
                                      the few magnified rows the iteration guard skips. */
#if SAT_FLOOR_TEX
static int sat_ftex_mode = 3;      /* boot default = mode 3 (owner 2026-07-03): textured tiles in
                                      the serviceable band + CPU spans everywhere VDP1 would be
                                      wasteful or unreliable.  Y cycles 0..5. */
/* SLAVE F-BUILD (owner GO 2026-07-03): the tile emission (inverse projections + one 64-bit divide
   per grid node) was the P-phase surcharge the HW mode-captures exposed (P(m2) ~2x P(m4)); it now
   runs on the slave SH-2 (idle 40-76% there) via the core aux-job API (r_parallel RP_AuxDispatch).
   Pad L+Y toggles it live (row 12 'sl') for the in-session A/B. */
extern "C" void RP_AuxDispatch(void (*fn)(void));
extern "C" void RP_AuxWait(void);
extern "C" void RP_AuxArm(void (*fn)(void));   /* piggyback: armed at the flush, taken by the
                                                  masked slave body right after MASK_DONE */
extern "C" int  RP_AuxKick(void);              /* DG-entry consumer (fallback dispatch)     */
static int sat_ftex_slave = 1;
/* BLIT<->F-BUILD DECOUPLE (pad R+Z, HW-A/B via composition-row 'b').  The captures showed 'b'
   (= RP_AuxWait + copy) inflated to ~17-23ms on heavy textured floors: the master IDLES in
   RP_AuxWait for the slave F-build (which does the present flip) before blitting.  OFF (default,
   0): current flip-before-blit, fully coherent, master idles.  ON (1): the slave emits tiles +
   the W->F chain but NOT the flip; the master single-CPU-blits CONCURRENTLY, joins, THEN flips
   (flip still after the blit => coherent) -- the blit hides in the wait, reclaiming ~6ms.  Needs
   sat_ftex_slave=1.  Present-coherence sensitive -> gated, default off, validate on HW. */
static int          sat_ftex_blit_overlap = 0;
static volatile int ftex_overlap_flip     = 0;   /* set when a slave build is ARMED to skip its flip
                                                    (so the master must flip in DG); read+cleared in DG */
static int ftexd_tiles, ftexd_skips, ftexd_trunc, ftexd_bakes, ftexd_px, ftexd_acc;   /* last flush */
extern "C" short *sat_floor_punch_edge;   /* core r_plane.c: mode-3 per-column VDP1/CPU split */
static short ftex_punch_edge_buf[320];    /* the platform buffer behind it (armed at init)    */
extern "C" void (*sat_floors_done_hook)(void);  /* core r_plane.c: fires at END of R_DrawPlanes */
extern "C" void sat_vdp1_floors_done(void);     /* fwd: builds+flips the F floor bank (1p)      */
extern "C" int sat_floor_punch_nrow;            /* core: mode-3 NEAR tile limit (screen row)    */
static int ftex_claim_px;                       /* projected tile fill CLAIMED so far this frame
                                                   (claim-time budget; reset by vdp1_ftex_flush) */
#endif

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
#define VDP1_MANUAL_CHANGE 0   /* PARKED (owner 2026-07-02): present-sync abandoned for good.  Re-tested a final time
                                  ON TOP of the plane-border decrochage fix -- still useless (PM/PAC unchanged verdict).
                                  0 compiles the machinery out: no L+Z / R+Z toggles, no row-2 P-mode chars, boot = the
                                  shipping 1-cycle-auto swap.  Code kept behind this guard for a possible future VDP1-walls
                                  revisit; flip to 1 to restore the knobs.  Verdict: seam is structural, docs/VDP1_PRESENT_SYNC_PLAN.md. */

extern "C" byte *I_VideoBuffer;
extern "C" int   gametic;
extern "C" int   r_visplane_peak;
extern "C" int   r_drawseg_peak;   /* core r_bsp.c: running high-water of drawsegs used (vs MAXDRAWSEGS 256) */
extern "C" int   sightcounts[2];   /* core p_sight.c: [0]=REJECT trivial-rejects, [1]=full BSP LOS walks */
extern "C" int   sat_floor_vq_cur, sat_floor_vq_peak;  /* VDP1-floor inc-0 estimate, shown on row 2 */
extern "C" unsigned int sat_sky_px, sat_floor_px;  /* sky-vs-floor coverage classifier (row 13) */
extern "C" int sat_plane_vscale;      /* deported-plane VERTICAL decrochage fill scale (pad R+Up/Down) */
extern "C" int sat_plane_border_max;  /* px cap on BOTH decrochage borders (core r_main.c, default 40 =
                                         legacy).  Set low (10) for SAT_FLOOR_TEX so a fast turn cannot
                                         blanket a textured plane in potato colour; pad R+Left/Right. */
extern "C" int sat_plane_fill_mode;   /* core r_plane.c: 0 = uniform-B perimeter (legacy), 1 = SWEPT
                                         per-column band from the claimed-region history (owner's
                                         red-band design) -- fills exactly the mask-vs-VDP1 gap. */
extern "C" int sat_plane_border_v;    /* live vertical fill-border px this frame (overlay readout) */
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
extern "C" int   sat_bsp_stage_used, sat_bsp_stage_want;  /* M5 BSP staging, row 1 st readout */
extern "C" int   sat_bsp_stage_on;                 /* M5 staging live A/B state (pad R+C) */
extern "C" void  P_BspStageApply(int on);          /* core/p_setup.c: swap LWRAM<->HWRAM sets */
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
/* VDP2_CELL_SKY: 1 = hardware sky on NBG0 as a 256-color CELL layer living in bank B1's free low
   half (cells SKY_CEL_VRAM, pattern-name map SKY_MAP_VRAM), coexisting with the RBG0 BITMAP floor
   -- A0 (K-table) / A1 (floor bitmap) / B0 (framebuffer) stay BYTE-IDENTICAL to the clean floor, so
   the floor cannot regress.  Distinct from VDP2_HW_SKY (the old A0 512x256 bitmap path, whose address
   IS the floor K-table -> collides).  The "floor XOR sky" law was lifted when the bitmap floor freed
   B1 (docs/VDP2_RBG0_CURRENT_STATE.md).  CRITICAL: the cell sky's B1 read slots (256-color cell =
   1 PN + 2 char/dot) are authored by slScrAutoDisp's allocator -- NEVER hand-pin CYCB1 (a 1-char
   table starves the 2nd 8bpp read = sky snow on HW only, invisible in Ymir).  Gated like the floor:
   potato-0 + 1-player.  See memory rbg0-hw-sky-feasible. */
#define VDP2_CELL_SKY    1
#define SKY_CEL_VRAM     ((void *)0x25E60000)  /* B1 low half: NBG0 sky 256-color cells (~32KB+filler) */
#define SKY_MAP_VRAM     ((void *)0x25E6A000)  /* B1: NBG0 sky pattern-name map (64x64 1-word = 8KB)    */
#define SKY_NB_CELL      512                    /* 32 cols x 16 rows of 8x8 cells (256x128 Doom sky)     */
/* VDP2_SKY_FORCE_CYC: experimental per-frame cycle override forcing NBG0's char read into B1 when
   RBG0 is on (sky_cell_force_cyc).  Did NOT change the "sky shows floor" HW bug -> gated OFF. */
#define VDP2_SKY_FORCE_CYC 0
/* VDP2_SKY_OCCL_DIAG: NBG0 sky ABOVE the RBG0 floor (sky=4 > floor=3).  The floor is a plane that is
   OPAQUE above the horizon on real HW (CONFIRMED by the priority swap), so with the ship order
   (sky 3 < floor 4) the floor's overspill occluded the sky; a VDP2 window to clip the floor would
   not commit without slSynch.  So instead we put the sky ON TOP and make it TRANSPARENT below the
   horizon (SKY_HORIZON_ROW): above the horizon the opaque sky covers the floor overspill; below it
   the transparent sky lets the floor show.  Walls/things (NBG1=6) and sprites(5) still sit above the
   sky.  This is the shipping config (1 = on).  0 = legacy sky(3) < floor(4). */
#define VDP2_SKY_OCCL_DIAG 1
/* SKY_HORIZON_ROW: screen scanline of the floor's horizon.  The sky cells are opaque ABOVE this row
   (covering the floor's above-horizon overspill) and transparent at/below it (the floor shows).
   Cell granularity is 8 px, so the effective boundary snaps to (SKY_HORIZON_ROW & ~7).  Tune to the
   floor's perspective horizon. */
#define SKY_HORIZON_ROW 96
#if VDP2_CELL_SKY
static void sky_cell_init(void);        /* forward decl: defined below, called from DG_Init */
static void sky_cell_build_map(void);   /* forward decl: rebuilds the sky map (live horizon tune) */
static int  sky_horizon_row = SKY_HORIZON_ROW;  /* live HW-sky horizon row (pad L + Up/Down); bake into SKY_HORIZON_ROW when tuned */
#endif
/* VDP2_SPLIT_HW_SKY (Part 5 -- docs/RBG0_SKY_SPLIT_ANALYSIS.md §5): 1 = in a co-op split give ONE
   ELECTED view the hardware NBG0 sky (the others keep the software sky), CONFINED to that view's band
   by VDP2 window W0 and SCROLLED by that view's viewangle.  NBG0 is a single scroll layer, so only one
   view can own it; the core (sat_sky_view/sat_vdp2_sky, r_plane.c + d_main.c) leaves that view's sky
   region index-0, and the W0 window keeps NBG0 from bleeding into the software views (incl. their VDP1
   torn wall gaps).  Layer priority already resolves the sky/floor overlap in the elected band
   (RBG0 floor 6 > NBG1 3D view 5 > NBG0 sky 4), so no extra window is needed for that.
   Static election: the elected view = P1 (view 0) -- couples with P1's HW floor in 2p; dynamic
   election by sat_sky_px_view[] + hysteresis is the documented next step (coverage already captured).
   W0 note: RBG0_LINECOL_TEST arms a per-line CCAL window on W0, but the fog is PARKED
   (rbg0_linecol_mode=0 -> ratio 0 -> no visible blend), so repurposing W0 as an NBG0 RECT window is
   visually free.  Requires VDP2_CELL_SKY.  This flag COMPILES the feature in; it is ON at runtime by
   default (hwsky_split_on = 1 -> the elected split view gets the HW sky) and toggled live with the pad
   chord L + C so it can be A/B'd against the software split sky on Ymir/HW without a rebuild.  Set this
   flag to 0 to remove the machinery entirely (e.g. a validated ship build that doesn't want
   it). */
#define VDP2_SPLIT_HW_SKY 1
#if VDP2_SPLIT_HW_SKY && !VDP2_CELL_SKY
#error "VDP2_SPLIT_HW_SKY needs VDP2_CELL_SKY (the NBG0 cell sky layer)"
#endif
#if VDP2_SPLIT_HW_SKY
static int hwsky_split_on = 1;   /* Part 5 LIVE toggle (pad L+C): 1 = HW sky for the elected split view (DEFAULT ON), 0 = software split sky.  Toggle OFF with L+C if it snows/misaligns on HW (cosmetic size/anchor still WIP -- docs §10). */
#endif
/* Frames a challenger must out-cover the leader (by margin) before the elected HW-sky view switches
   (dynamic election only; unused by the static default).  ~0.5s at 60fps avoids per-frame scroll jumps. */
#define SKY_ELECT_HYST 30
/* SATURN (Romain 2026-06-30): RBG0 floor improvements -- candidate defaults, flip to 0 to A/B-test.
   RBG0_FLOOR_DOMINANT -> drives core sat_vdp2_floor_dominant: the HW floor follows the DOMINANT
     visible flat (re-picked ONLY when the player changes sector) instead of the floor under the eye.
   RBG0_FLOOR_WINDOW   -> clips RBG0's DISPLAY to BELOW the horizon (VDP2 window W1) so a torn VDP1
     wall gap above the horizon shows the backdrop/sky, not the floor bleeding through. */
#define RBG0_FLOOR_DOMINANT 1
#define RBG0_FLOOR_WINDOW   1
/* RBG0_FLOOR_AUTO_HORIZON: drive BOTH the HW-sky transparent boundary AND the floor window from the
   ACTUAL rendered floor top (core sat_vdp2_floor_top_y), so the sky always comes down exactly to the
   floor window -> no sky/floor decalage at any vantage.  0 = static SKY_HORIZON_ROW (legacy). */
#define RBG0_FLOOR_AUTO_HORIZON 1
/* RBG0_SPLIT_P1HW: in 2-player split, drive the HW floor for P1 (left half) while P2 keeps its
   software floor.  0 = split stays fully software (legacy). */
#define RBG0_SPLIT_P1HW 1
#define RBG0_SPLIT_TUNE  0   /* 1 = live split-floor tuning knobs (R/L/C + d-pad) + VPW overlay + d-pad movement freeze; 0 = baked (ship) */
static int rbg0_floor_win_xend = 319;   /* RBG0 floor window X extent: 319 = full (1p), 159 = P1 left half (2p split) */
/* RBG0 split VIEWPORT PROJECTION (slWindow) -- only used when sat_split_p1hw (1-player never calls slWindow,
   so its slInitSystem-default projection is untouched).  centerX + window width reproject the floor onto
   P1's LEFT half (vanishing point x=80, FOV on 160px) so it aligns with P1's software walls.  centerY (the
   vanishing-point ROW) and the near-plane depthLimit come from slInitSystem's default which is NOT in the
   SGL sources (binary) -> TUNE these two on Ymir.  Full-screen restore uses the 1p defaults (160,112). */
/* Runtime so they can be cal'd LIVE in split (R + d-pad, see poll_pad) without a rebuild; bake the found
   values back here once tuned.  centerX/width reproject the floor onto P1's half; centerY + depthLimit are
   the slInitSystem defaults (not in the SGL sources) so they need dialing on Ymir. */
static int rbg0_win_cx    = 80;    /* P1 viewport centre X (vanishing point); 1p full-screen = 160 */
static int rbg0_win_cy    = 80;    /* P1 viewport centre Y (horizon row) -- TUNE live (R + Up/Down in split) */
static int rbg0_win_depth = 256;   /* slWindow near-plane depthLimit -- TUNE live (R + Left/Right in split); too small clips the floor */
static int rbg0_split_hz  = 80;    /* SPLIT floor horizon = the W1 clip top ("floor limit by height"), live R+Up/Down.  The 1p
                                      height-formula (96+(fhw+56)*3/23) is calibrated for the 224-tall 1p view; P1's split viewport
                                      is 160 tall (horizon ~80) so it over-clips -> in split we use THIS value instead.  Bake when tuned. */
static int rbg0_split_pitch = -1216; /* SPLIT plane pitch (inclination), live L+Up/Down.  Baked from live tuning.  = 1p's rbg0_pitch_adj (0x100):
                                       the plane tilt is viewport-independent, so P1 uses the SAME tilt as 1p once
                                       the projection (Cx/Cy/screen-dist) is derived right.  SPLIT-ONLY -> 1p keeps
                                       rbg0_pitch_adj untouched.  Likely compensates the off-centre projection; revisit
                                       once the rotation/centre is fixed (it should then match the 1p pitch). */
static int rbg0_split_cx = 80;      /* SPLIT rotation centre X (VDP2 RPT Cx, int16 @0x3C), via slDispCenterR.  Full
                                       screen is 160; 80 = centre of P1's left half -> vanishing point at P1's
                                       centre.  Live R+Left/Right. */
static int rbg0_split_sd = 7;       /* SPLIT screen-distance ratio, Q4 (16 = 1.0x default): baked from live tuning.  Halving
                                       MsScreenDist via slSetScreenDist widens the floor's FOV to P1's 160px
                                       viewport so texel ratio + rotation speed match the SW view.  Done NATIVELY
                                       through slScrMatConv (stable) -- post-scaling ΔX/ΔY in VRAM flickers at any
                                       !=1x, so this is the only clean lever.  Live L+Left/Right. */
static int rbg0_split_yaw = 0;      /* SPLIT floor YAW offset (ANGLE), live C+Left/Right.  0 once the projection is
                                       correct (the old 4864 compensated the restore-corruption bug, now fixed).  Aligns the
                                       forward-scroll direction (baked from live tuning).  NOTE: if this only holds
                                       at one facing (drifts as you turn), the floor rotation is scaled (see the x2
                                       rotation-speed issue) and this offset is not a true constant. */
static int rbg0_split_cy  = 80;     /* SPLIT rotation centre Y = centery of P1's 160-tall viewport (slDispCenterR
                                       sets Py=Cy too -> consistent).  Live C+Up/Down. */
static int rbg0_split_scroll = 16;  /* SPLIT scroll-rate scale, Q4 (16 = 1.0x): scales slTranslate X/Y so forward/back
                                       scrolls at the SW rate (sd changed the focal but not the K-table -> scroll too
                                       fast; the flat repeats, so scaling the world offset is invisible).  Live C+Up/Down. */
static int rbg0_split_yawsc = 16;   /* SPLIT yaw-RATE scale, Q4 (16 = 1.0x = correct per disasm; the x2 was from the
                                       old Cy=112+pitch band-aid, not the yaw).  Live C+Up/Down.  Rotation tracks
                                       viewangle at this rate.  Live C+Up/Down.  8 (halve) targets the "rotation x2
                                       too fast when turning" -- which is sd/FOV-INDEPENDENT, so it's the yaw rate. */
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
#define RBG0_LINECOL_TEST 1
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
static int nbg3_show = 1;   /* NBG3 debug overlay display; L+R cycles sat_dbg_overlay_mode (0 full / 1
                               fps-only / 2 off) and syncs this = (mode != 2).  Default ON = full perf
                               overlay visible at boot.  Its B1 cycle is reserved at init (RBG0_NBG3),
                               so this only flips BGON. */
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
extern "C" int            sat_sky_view;         /* core Part 5: elected split view for the HW sky (-1 = none => all software) */
extern "C" unsigned int   sat_sky_px_view[4];   /* core Part 5: per-view SKY pixel coverage (election metric) */
extern "C" unsigned int   sat_sky_view_angle;   /* core Part 5: elected view's viewangle (angle_t) for the NBG0 scroll */
extern "C" int            sat_vdp2_floor;   /* core: skip software floor (=> VDP2 RBG0) */
extern "C" int            sat_vdp2_floor_h; /* core: player's floor height (fixed_t) */
extern "C" int            sat_vdp2_floor_pic;/* core: player's floor flat (picnum) */
extern "C" unsigned char *sat_vdp2_floor_cmap;/* core: colormap for the floor's sector light (0=full bright) */
extern "C" int            sat_vdp2_floor_band;/* core: floor sector LIGHT BAND 0..15 (15=bright); drives the base level */
extern "C" int            sat_vdp2_floor_dominant;/* core: 1 = HW floor follows the DOMINANT visible flat (re-picked on sector change) vs the floor under the eye */
extern "C" int            sat_vdp2_floor_top_y;   /* core: TOP screen row of the floor punched this frame (its real horizon); 0x3FFF if none */
extern "C" int            sat_view_floor_h;       /* core: floorheight of the player's view sector (drives the player-height horizon, not the dominant) */
extern "C" int            sat_split_p1hw;          /* core: set here -> d_main punches the HW floor only for P1 in split */
extern "C" void           sat_setup_view_p1(void);/* core: re-anchor the view globals on P1 for the split RBG0 transform */
extern "C" int            sat_potato_floors;/* core: solid-colour floors/ceilings */
extern "C" int            sat_potato_walls; /* core: solid-colour walls (opaque, flat only) */
extern "C" int            sat_wall_nocpu;   /* core: banded/flat -> skip close-wall CPU fallback */
/* Phase-1 wall clamp ([[wall-clamp-world-anchored]], docs/WALL_SUBDIVISION_STUDY.md): 1 = tiers
   partially below floorclip / above a deported ceiling STAY on VDP1, cut at a WHOLE-TEXEL
   world-anchored line (straight on screen, exact at both ends -> no squish/swim; the platform's
   1px pad is inside the cut) + the residual WEDGE down/up to the true per-column clip stays
   software (core sat_wall_cut_floor/_ceil).  SPAN-close stays CPU (the v0 warp verdict) and
   magnified stays CPU/subdiv.  Live A/B: pad L+R+Y; row-6 FBK 'W<n><+/->' = kept tiers + state,
   'c' should melt where W rises. */
#define SAT_WALL_CLAMP 1
extern "C" int            sat_wall_clamp;   /* core r_segs.c global; set from SAT_WALL_CLAMP at init */

/* Option-1 FIRST CUT A/B (see-the-potential): 1 = deport SECONDARY floors/ceilings to VDP1 as flat
   quads (drains the software span phase P; the dominant flat stays on RBG0/software).  Emitter is
   defined near vdp1_walls_flush.  Defects deferred (1 quad/visplane, fixed bank, seam painter). */
#define SAT_VDP1_FLOOR 1
#if SAT_VDP1_FLOOR
extern "C" int            sat_vdp1_floor;
extern "C" int          (*sat_floor_vdp1_hook)(int, int, int, int, const unsigned char *, const unsigned char *, int);
extern "C" int            sat_floor_vdp1_emit(int, int, int, int, const unsigned char *, const unsigned char *, int);
#endif
extern "C" int            sat_local_players; /* core: LIVE local-coop player count (1 = single) */
extern "C" int            sat_split_vdp1;    /* core: split keeps walls on VDP1 (views 0/1); pad-X A/B */
extern "C" int            sat_plane_tas;     /* core: TAS.B plane work-steal A/B (pad-C 'pm1') */
extern "C" int            sat_plane_rowsplit;/* core: row-split plane balancer (pad-C 'pm2') */
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
extern int sat_psprite_early;          /* core r_things.c: platform draws psprites early (VDP1) */
extern int viewangleoffset;            /* core r_main.c: nonzero on side views (no psprites)    */
extern int viewwindowx, viewwindowy;   /* core r_draw.c: this view's framebuffer origin (0,0 in 1p) */
extern int scaledviewwidth, viewheight;/* core r_draw.c: this view's screen-space size            */
void R_DrawPlayerSprites(void);        /* core r_things.c: emit the weapon via sat_psprite_hook */
void sat_vdp1_wpn_clip(void);          /* sat_psprite_begin hook: clip the weapon to its view    */
void sat_vdp1_wpn_begin(void);
void sat_vdp1_wpn_draw(patch_t *patch, int lump, int sx, int sy, int flip,
                       const unsigned char *cmap);
/* world-things-on-VDP1 (SAT_WORLD_THINGS_VDP1 is #defined later, so -- like the weapon decls
   above -- these stay UNGUARDED; they are only referenced where the macro is on). */
extern int (*sat_thing_hook)(patch_t *patch, int lump, const unsigned char *cmap,
                             int x0, int y0, int x1, int y1,
                             int cx0, int cy0, int cx1, int cy1, int flip); /* core r_things.c */
void R_EmitWorldThingsVDP1(void);      /* core: emit world sprites to VDP1 at the post-BSP kick */
extern int sat_things_occ;             /* core: fully-occluded sprites skipped this frame (metric) */
extern int sat_thing_cap;              /* core: how many (nearest) things go to VDP1/frame (we set = slots) */
extern int sat_things_hw;              /* core: 1 = world sprites -> VDP1 (M4); 0 = software (M0/M6) */
int  sat_vdp1_thing_draw(patch_t *patch, int lump, const unsigned char *cmap,
                         int x0, int y0, int x1, int y1,
                         int cx0, int cy0, int cx1, int cy1, int flip);     /* our sat_thing_hook impl */
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

/* ============================================================================
   Render MODE (M) + per-zone Software Quality (SQ) -- the two orthogonal axes that
   replace the old potato_modes[] path-jumble + the sat_ftex_mode 0-5 pad cycle.
     M  = OFFLOAD strategy: WHERE each surface renders (software / VDP1 / VDP2).
     SQ = quality of whatever stays on the CPU, PER ZONE (wall / floor / ceiling).
   sat_apply_mode() is the SINGLE writer of every backend flag -> only coherent
   tuples exist, and each M activates ONLY the subsystems it needs (see the plan).
   Pad: Z cycles M (0..4); R+A/B/C cycle SQ wall/floor/ceil.  The MODE overlay row
   shows the resolved composition so a capture always pins the exact state.
   M1 reproduces the historical default (RBG0 dominant + VDP1 leftover + VDP1 walls
   + NBG0 sky) verbatim -- the regression anchor. */
enum { M0_SOFT, M1_FULL, M2_FLOORS, M3_CEILS, M4_RBG0, M5_CONVEX, M6_NOSPR, M_COUNT };
static int sat_m = M4_RBG0;                 /* boot = RBG0 dominant floor + VDP1 walls + VDP1 weapon + VDP1
                                               world things.  M6 = same but world things SOFTWARE (the
                                               sprite-only A/B).  Pad Z cycles ONLY {M0, M4, M6}; M1/M2/M3/M5
                                               are PARKED (kept for reference/A-B, off the cycle). */
static const char *const sat_m_name[M_COUNT] = { "soft", "full", "flr", "ceil", "rbg0", "cvx", "nospr" };
/* Pad-Z cycle: the live modes only (parked M1/M2/M3/M5 stay reachable only via code). */
static const int sat_m_cycle[] = { M0_SOFT, M4_RBG0, M6_NOSPR };
#define SAT_M_CYCLE_N ((int)(sizeof(sat_m_cycle) / sizeof(sat_m_cycle[0])))
enum { SQ_FULL, SQ_LD, SQ_BAND, SQ_FLAT };
static int sq_wall = SQ_FULL, sq_floor = SQ_LD, sq_ceil = SQ_LD;   /* floor+ceil ld by default (HW-tested "fll":
                                                                     ld is ~invisible on the ceiling and fine on the
                                                                     floor; the convex-exact deport (M5) puts the
                                                                     convex floors at FULL quality on VDP1 anyway) */
static const char *const sq_name[4] = { "full", "ld", "band", "flat" };
static int wall_potato_mode = 0;            /* VDP1 wall style: 0=tex 1=banded 2=flat (SQ_wall-derived) */
/* Platform VDP1-claim gates, read ONLY by sat_floor_vdp1_emit -- split leftover-floors vs
   ceilings independently so M2 (floors only) and M3 (ceilings only) isolate each offload. */
static int sat_vdp1_floor_claim = 1;        /* leftover floors -> VDP1 tiles (M1,M2) */
static int sat_vdp1_ceil_claim  = 1;        /* ceilings        -> VDP1 tiles (M1,M3) */

/* Framebuffer->VDP2 blit selector, cycled LIVE by the pad L+A chord (NOT L+R = the debug overlay).
   dma = 0 CPU memcpy, 1 = slDMACopy (on-chip DMAC; HW-confirmed no win, parked off the live ring).
   w5  = skip the static HUD band [hud_top,224) when it didn't change (core sat_hud_dirty / 2p sig);
   the 3D view always blits.  Row-1 'b<ms><c/d><-/5>' = ms + path + W5.  Async blit via SCU-DMA is
   DEAD + IMPOSSIBLE (SEGA SCU manual: no CPU A/B-bus access during an SCU-DMA B-bus transfer ->
   hang; every frame hits the B-bus) -> the blit is permanently synchronous.  docs/BLIT_DMA_PLAN.md. */
static const struct { int dma; int w5; } blit_cfg[] = {
    { 0, 0 },   /* 0: c-  CPU memcpy, full HUD */
    { 0, 1 },   /* 1: c5  CPU memcpy + W5 -- boot default */
    { 1, 0 },   /* 2: d-  slDMACopy (on-chip DMAC) sync -- parked (no win) */
    { 1, 1 },   /* 3: d5  slDMACopy sync + W5           -- parked */
};
#define BLIT_CFG_N ((int)(sizeof(blit_cfg) / sizeof(blit_cfg[0])))
/* Live pad-L+A A/B ring: c5 (CPU + W5, default) <-> c- (CPU, W5 off) -- the safe W5 on/off A/B.
   slDMACopy paths (2,3) parked off-ring (no win). */
static const int blit_cycle[] = { 1, 0 };
#define BLIT_CYCLE_N ((int)(sizeof(blit_cycle) / sizeof(blit_cycle[0])))
static int blit_cycle_i = 0;   /* index into blit_cycle; 0 -> blit_mode 1 (c5) at boot */
static int blit_mode = 1;      /* boot: c5 (CPU + W5) = blit_cycle[0] */
/* SATURN PERF: last frame's framebuffer->VDP2 blit wall-clock in ms*10 (master FRT delta
   around the copy, INCLUDING the dual-blit slave-join spin).  This is the number that
   decides dual-CPU blit GO/DROP -- fps/MST are too coarse (~12ms of a ~100ms frame).
   Read it on row 2 as 'b<ms.tenth>'; compare config 0 (single) vs 4 (75/25), same scene. */
static unsigned int sat_blit_ms10 = 0;
/* SATURN blit A/B precision: windowed accumulation of the per-frame sat_blit_ms10 (FRT tenths),
   reset on the L+A toggle (any config change) so at a standstill it builds a long, stable sample
   -> the MEAN (shown in the row-1 'b' field, tenths) resolves the ~1.5ms W5 / DMA deltas that the
   old integer 'b' rounded away.  Capped at 4096 samples (rock-stable by then, no overflow).
   NB: displayed by FOLDING into row 1 -- NO new overlay row (rows are saturated across
   dg_saturn.cxx AND core/r_parallel.c; see the debug-overlay-placement memory). */
static unsigned int blit10_sum = 0, blit10_cnt = 0;
#define BLIT10_CAP 4096
/* SATURN W5 (docs/BLIT_DMA_PLAN.md): blit the HUD rows [hud_top,224) only when the HUD
   framebuffer actually changed; the 3D-view rows [0,hud_top) always blit.  hud_top = the
   clear boundary (192 1p / 160 2p / 224 3-4p) so 3/4p (no bottom HUD band) blits all 224 = a
   no-op.  1p dirty comes from core sat_hud_dirty (the STlib widgets, now diff'd); 2p from the
   ST_SplitHudSig value signature below.  W5 is a RUNTIME axis of blit_cfg (the 'w5' field,
   pad L+A) -- always compiled, off at boot; the core diff/dirty writes are unconditional and
   harmless when w5=0.  Read blit_cfg[blit_mode].w5 at the blit. */
extern "C" int sat_hud_dirty;            /* core st_stuff.c: HUD region (re)drawn this frame */
extern "C" unsigned int ST_SplitHudSig(void);  /* core: 2p/4p compact-HUD value signature */
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
/* The single writer of every render backend flag.  Maps (sat_m, sq_*) -> a coherent
   per-surface tuple; each M leaves ON only the subsystems it needs.  Called at init, on any
   M/SQ change, and per-view in the split (the per-frame block downgrades RBG0 off P1). */
static void sat_apply_mode(void)
{
    extern int sat_split_lowdetail;            /* core: split detailshift (low-detail) */
    extern int sat_floor_ld;                   /* core r_plane.c: half-rate floor texel fetch */
    extern int sat_ceil_potato, sat_ceil_ld;   /* core r_plane.c: independent ceiling SQ (step 2) */
    int M = sat_m; if (M < 0 || M >= M_COUNT) { M = M1_FULL; sat_m = M; }

    /* ---- Axis A: offload targets ---- */
    int rbg0_want  = (M != M0_SOFT);           /* RBG0 dominant floor (M1..M4) */
    int vdp1_walls = (M != M0_SOFT);           /* VDP1 one-sided walls  (M1..M4) */
    sat_vdp1_floor_claim = (M == M1_FULL || M == M2_FLOORS || M == M5_CONVEX);   /* leftover floors -> VDP1 */
    sat_vdp1_ceil_claim  = (M == M1_FULL || M == M3_CEILS  || M == M5_CONVEX);   /* ceilings        -> VDP1 */
    int hook_consult = (sat_vdp1_floor_claim || sat_vdp1_ceil_claim);   /* any leftover -> VDP1 */

    sat_vdp2_sky            = rbg0_want ? 1 : 0;   /* M0 -> sat_vdp2_sky=0: core draws the software sky */
    sat_vdp2_floor          = rbg0_want ? 1 : 0;   /* per-frame block re-derives on split/1p (rbg0_on) */
    sat_vdp2_floor_dominant = RBG0_FLOOR_DOMINANT; /* every RBG0 mode uses the dominant-flat pick */
    sat_wall_skip           = vdp1_walls ? 1 : 0;  /* M0 -> core draws software one-sided walls */
    sat_things_hw           = (M != M0_SOFT && M != M6_NOSPR); /* world sprites -> VDP1 prio-7 (M4); M6 = SOFTWARE
                                                     sprites (same walls+floor as M4) = the sprite-only A/B */
    sat_vdp1_floor          = hook_consult ? 1 : 0;/* core: consult the emit hook (M1..M3) */
    /* sat_ftex_mode: 4 = M0 (legacy all-software path drops the VDP1 wall_acc); 6 = M5 CONVEX-EXACT
       (distant planes whose every run is ONE linear trapezoid -> 1 exact quad; non-convex or near ->
       software -- the cheap deport); 3 = the full tile deport (M1-M3).  M4's hook is not consulted
       (sat_vdp1_floor=0) so its value is inert there. */
    sat_ftex_mode           = (M == M0_SOFT) ? 4 : (M == M5_CONVEX) ? 6 : 3;

    /* ---- Axis B: per-zone software quality (bites only on software zones / fallback slivers;
       HW-owned zones ignore it -- no detail level on hardware) ---- */
    wall_potato_mode = (sq_wall == SQ_BAND) ? 1 : (sq_wall == SQ_FLAT) ? 2 : 0;  /* VDP1 wall style */
    sat_potato_walls = (sq_wall == SQ_FLAT);                     /* flat-shaded software walls */
    sat_wall_nocpu   = (sq_wall == SQ_BAND || sq_wall == SQ_FLAT);/* banded/flat -> skip close-wall CPU */
    sat_potato_floors = (sq_floor == SQ_FLAT);                   /* solid-colour software floors */
    sat_floor_ld      = (sq_floor == SQ_LD);                     /* half-rate floor texel fetch */
    sat_ceil_potato   = (sq_ceil == SQ_FLAT);                    /* solid-colour software ceilings */
    sat_ceil_ld       = (sq_ceil == SQ_LD);                      /* half-rate ceiling texel fetch */
    sat_split_lowdetail = (sq_wall == SQ_LD || sq_floor == SQ_LD || sq_ceil == SQ_LD); /* split detailshift */

    sat_wall_clamp = (M == M5_CONVEX) ? 0 : SAT_WALL_CLAMP;   /* off in M5: convex-exact planes are distant
                                                                 full-claims -> no near-band to clamp, and it
                                                                 drops the Bp wall-clamp cost (pad R+A re-arms) */
}
#define GS_LEVEL 0
#define SAT_CMAP_BYTES (34 * 256)           /* COLORMAP: 34 maps of 256 (r_data.c) */

/* Debug-overlay shim: core (d_main.c, r_*.c) calls dbg_print(x, y, str). */
extern "C" void dbg_print(int x, int y, char *str)
{
    SRL::Debug::Print((uint8_t)x, (uint8_t)y, str);
}

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

/* 3/4-player compact HUD: paint one quadrant's 160x16 band (each row a solid bevel
   colour -> one memset per row) at (ox, oy); the core then draws that player's
   widgets on top via ST_DrawQuadHud.  The band is OPAQUE (non-zero indices) so it
   occludes the VDP1 wall layer below NBG1 -- see hud4p_col in hud4p_panel.h. */
extern "C" void ST_DrawQuadHud(int pnum, int ox, int oy);   /* core: per-player compact widgets */
static void hud4p_blit_band(int ox, int oy)
{
    for (int y = 0; y < HUD4P_H; ++y)
        memset(framebuffer + (oy + y) * 320 + ox, hud4p_col[y], HUD4P_W);
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

/* 3/4-player flash: same per-viewport LUT wash, but per QUADRANT (160x112).  In 3p
   the 4th quadrant is the minimap (no player) -> pass n = player count so it is not
   remapped.  Reuses the 2p LUTs; only a damaged/flashing quadrant pays the remap. */
static void hud4p_apply_flash(int n)
{
    static const short qx[4] = { 0, 160, 0,   160 };
    static const short qy[4] = { 0, 0,   112, 112 };
    if (!hud2p_flash_built) hud2p_flash_build();
    for (int q = 0; q < n; ++q)
    {
        int lvl = ST_PlayerPaletteIndex(q);
        if (lvl <= 0 || lvl >= HUD2P_NPAL) continue;
        const unsigned char *lut = hud2p_flash_lut[lvl];
        for (int y = 0; y < HUD4P_QUAD_H; ++y)
        {
            unsigned char *row = framebuffer + (qy[q] + y) * 320 + qx[q];
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
#if VDP1_MANUAL_CHANGE
/* Corrected draw-gated present (docs/VDP1_PRESENT_SYNC_PLAN.md, brick A).  Declared up here (not by
   the driver below) so the row-2 overlay can show the mode.  Runtime A/B via pad L+Z:
   0 = 1-cycle auto (BOOT DEFAULT: tears, but Ymir-visible and == shipped behaviour);
   1 = gated manual present (tear-free; the VDP1 walls lag ~1 vblank = decrochage, the accepted trade).
   _pending/_wait drive the per-frame CEF-gated swap + the stuck watchdog. */
static volatile int vdp1_present_manual  = 0;
static volatile int vdp1_present_pending = 0;
static volatile int vdp1_present_wait    = 0;
static volatile int vdp1_couple_nbg1     = 0;   /* brick B: defer the NBG1 blit to land with the VDP1 present (separate toggle pad R+Z) */
#define VDP1_PRESENT_STUCK_MAX 16   /* vblanks pending w/o CEF -> force-swap (Ymir never latches manual-mode CEF) */
#define VDP1_COUPLE_MAX_VBL    4    /* couple: wait at most this many vblanks for CEF before blitting anyway */
#endif

/* SATURN world-things-on-VDP1: per-frame emitted / declined counters (overlay 'th').  Defined here
   (before the SHOW_FPS overlay block that prints them, and unconditionally so the emit path that
   increments them always links) -- the pool struct itself is defined later with the weapon cache. */
static int sat_things_n = 0, sat_things_decl = 0, thing_bake_n = 0;   /* 'th' emitted/declined, 'fb' baked (cache misses) */

#if SHOW_FPS
extern "C" int rp_timeout_count;
extern "C" unsigned int rp_master_ms;   /* master frame ms -> prefixes r_parallel.c's row-18 SLV line */
static unsigned int dg_frame_count = 0;
static int vdp1_last_cmds = 0;

/* ============================================================================
   SATURN world-things-on-VDP1: SESSION percentile metrics -- so ONE end-of-level capture tells
   the whole story instead of jittery instantaneous values.  Auto-RESET on a MODE change (sat_m /
   SQ), NOT on a level change (user 2026-07-05: "par session, tant que je ne change pas le mode")
   -> the numbers describe the whole run at the CURRENT mode.  A/B = capture M4, switch to M0
   (pad Z), capture again.  Histograms give p50/p90/p99 with no sort: frame time (8ms buckets),
   world-things emitted/frame and declined/frame (direct 0..63).  Plus the VDP1 plot done-rate (the
   headroom / flicker-inverse: the OnVblank Dr sampler feeds mh_vbl_*) and occluded-skip avg.
   ============================================================================ */
#define MH_MS_BUCKETS  40          /* 8ms buckets -> 0..320ms frame time */
#define MH_MS_SHIFT    3
#define MH_N_BUCKETS   64          /* things count 0..63 (direct index) */
static unsigned int mh_ms[MH_MS_BUCKETS];
static unsigned int mh_things[MH_N_BUCKETS];
static unsigned int mh_decl[MH_N_BUCKETS];
static unsigned int mh_frames;
static unsigned int mh_ms_mx, mh_things_mx, mh_decl_mx, mh_occ_sum;
static unsigned int mh_vbl_done, mh_vbl_tot;   /* VDP1 plot done-rate over the session */
static unsigned int mh_bake_sum, mh_emit_sum;  /* session totals: fresh bakes vs sprites emitted (cache reuse) */

static void mh_reset(void)
{
    memset(mh_ms, 0, sizeof mh_ms);
    memset(mh_things, 0, sizeof mh_things);
    memset(mh_decl, 0, sizeof mh_decl);
    mh_frames = mh_ms_mx = mh_things_mx = mh_decl_mx = mh_occ_sum = 0;
    mh_vbl_done = mh_vbl_tot = 0;
    mh_bake_sum = mh_emit_sum = 0;
}
static void mh_add(int ms, int things, int decl, int occ, int bake)
{
    int b;
    b = ms >> MH_MS_SHIFT; if (b < 0) b = 0; if (b >= MH_MS_BUCKETS) b = MH_MS_BUCKETS-1; mh_ms[b]++;
    b = things;            if (b < 0) b = 0; if (b >= MH_N_BUCKETS)  b = MH_N_BUCKETS-1;  mh_things[b]++;
    b = decl;              if (b < 0) b = 0; if (b >= MH_N_BUCKETS)  b = MH_N_BUCKETS-1;  mh_decl[b]++;
    if ((unsigned)ms     > mh_ms_mx)     mh_ms_mx     = (unsigned)ms;
    if ((unsigned)things > mh_things_mx) mh_things_mx = (unsigned)things;
    if ((unsigned)decl   > mh_decl_mx)   mh_decl_mx   = (unsigned)decl;
    mh_occ_sum  += (unsigned)occ;
    mh_bake_sum += (unsigned)bake;
    mh_emit_sum += (unsigned)things;
    mh_frames++;
}
/* percentile p(0..100): first bucket whose cumulative count reaches p% of mh_frames.  For the ms
   histogram the caller shifts the returned bucket back to ms (<<MH_MS_SHIFT = the lower edge). */
static int mh_pct(const unsigned int *h, int nb, int p)
{
    unsigned int target, acc = 0; int b;
    if (!mh_frames) return 0;
    target = (mh_frames * (unsigned)p + 99u) / 100u;
    for (b = 0; b < nb; b++) { acc += h[b]; if (acc >= target) return b; }
    return nb - 1;
}

/* RELIABLE VDP1 load (replaces the CEF/vblank-aliased Dr%): command-budget FILL % for the wall
   and floor banks, computed where WALL_CMD_CAP/FTEX_F_CAP are in scope (they are #defined below
   fps_update) and read by the overlay.  The CPU-fallback counts (fb_pk_starve = walls dumped
   when the bank fills, ftexd_trunc = floor tiles dropped) are the definitive "over budget" signal. */
static int vd1_wpct = 0, vd1_fpct = 0;
/* RBG0 floor view (pad Y): 0 = HW floor (sector-dimmed baked flat), 1 = software floor.  The HW floor
   ships BAKE-ONLY (no distance gradient) for now. */
static int rbg0_floor_view = 0;
static int rbg0_linecol_mode  = 0;   /* gradient OFF by default -- WIP, see below */
/* === SECTOR-DRIVEN BLACK VEIL gradient -- WORK IN PROGRESS, parked (rbg0_linecol_mode = 0) ===========
   The shipping HW floor is the per-sector DIMMED bake only.  This line-color "black veil" distance
   gradient is kept but DISABLED: over the FAR it lays a black veil (line-color at a high ratio so the
   floor is fully replaced -> black, no green residual), shape = short transition (base->black) then a
   long pure-black band, COVERAGE = [hz, bd] with bd = hz + (15-band)*zonek + zoneoff scaling with the
   room light band (band 15/outdoor -> NONE), gated by a VDP2 color-calc window so the near stays clean.
   It still didn't read right across rooms (black tint / coverage tuning), so it's parked -- flip
   rbg0_linecol_mode + re-add the pad toggles to resume.  See memory [[rbg0-floor-distance-light]]. */
static int rbg0_lc_far   = 7;        /* = the computed boundary bd (display only) */
static int rbg0_lc_trans = 24;       /* TRANSITION length in rows: base->black, then pure black (R+L/R) */
static int rbg0_zonek    = 7;        /* zone slope: bd = hz + (15-band)*zonek + zoneoff (C+L/R); b15 -> none */
static int rbg0_zoneoff  = 0;        /* zone offset rows, shifts the whole veil down/up (C+Up/Down) */
static int rbg0_linecol_ratio = 14;  /* veil DEPTH, default = max 14 (R+Up/Down, capped at 14 per user) */
/* Base floor brightness is now DRIVEN BY THE WAD per-sector light (sat_vdp2_floor_cmap = zlight[li][0]
   from the room's lightlevel, set in r_plane.c) -- the fixed-level bake is cancelled.  rbg0_floor_dim
   is now a manual signed OFFSET added on top of that sector level (0 = exactly the software room shade;
   <0 brighter, >0 darker).  L+Up/Down live; re-bakes when the sector light OR the offset changes. */
static int rbg0_floor_dim = 0;
/* SATURN 2026-06-29: baked CONTRAST of the RBG0 floor texels (0 = flat/uniform dim = old behaviour).
   >0 spreads each texel's colormap level around the flat's MEAN luma: bright texels get a lower level
   (brighter), dark texels a higher level (darker) -> the texture detail POPS.  Live via L + pad-C. */
static int rbg0_floor_contrast = 0;   /* base bake cancelled -> 0 (no texture-contrast pop); L+L/R re-enables */

/* SATURN PERF (2026-06-29): RELIABLE ms split of DG_DrawFrame (= I_FinishUpdate = overlay 'bl').
   The FRT-based sat_blit_ms wraps at ~73ms so it under-reads a stalled blit (the b6.x artifact);
   these use the 32-bit DG_GetTicksMs.  pre = sky/palette/fps_update before the blit; blit = the
   dual-CPU copy (incl. any VDP1-contention/present stall we're hunting on close walls); post =
   the index-0 view clear.  Summed over the fps 1s window, printed by fps_update on row 11. */
extern "C" uint32_t DG_GetTicksMs(void);
static unsigned int df_pre_sum, df_blit_sum, df_post_sum, df_frames;
/* SATURN PERF (2026-07-04): master-frame composition.  MST = REC(render) + T(game-tic) + S(sound)
   + blit + present(VDP1 kick) + other.  T/S come from the core (d_main.c, per tick); blit is the
   existing DG split; present is the VDP1 wall-kick FRT timed at its call sites.  Window sums,
   averaged once/sec by fps_update alongside df_pre/blit/post -> the decomposition on overlay row 1. */
static unsigned int df_tic_sum, df_snd_sum;      /* window sums of the core game-tic / sound ms  */
static unsigned int df_present_sum;              /* window sum of the VDP1 present-kick (tenths-ms) */
static unsigned int sat_present_frt = 0;         /* VDP1 kick FRT ticks THIS frame (reset in DG df block) */
extern "C" int sat_tic_ms, sat_snd_ms;           /* core d_main.c: game-tic / sound ms this tick   */
extern "C" int sat_dbg_overlay_mode;             /* 0 full / 1 fps-only / 2 off (core r_parallel.c) */
extern "C" int sat_prof_planepix;                /* arm the RP_PlanePixels floor sizer (core; def 0) */
/* SATURN PERF (2026-06-29): sub-split of the DF 'pre' phase to pin the facing-wall stall.
   sky = sky scroll + cmap + slScrAutoDisp; up = rbg0_upload_flat (131KB rebuild, normally guarded);
   xf = rbg0_set_transform (slScrMatConv/slScrMatSet matrix); rp = the RPT VRAM memcpy. 1s-window
   sums, printed by fps_update on row 12. Whichever dominates IS the ~110ms facing-wall stall. */
static unsigned int rbg_sky_sum, rbg_upl_sum, rbg_xfm_sum, rbg_rpt_sum;

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
extern "C" void RP_SprStats(int *proj10, int *fill10, int *nproj, int *ndraw); /* SATURN sprite-cost profiler (DSP study) */
extern "C" int gamemap;   /* core doomstat: drives the per-map window reset */

/* Phase-0 wall CPU-fallback profiler (core r_segs.c): per-frame tally by cause, folded to
   windowed peaks in vdp1_wpn_kick, shown on overlay row 24 (FBK).  clamp = SPAN/below-floor
   (the Phase-1 world-anchored VDP1 clamp target); mag = face-on magnified residue; starve =
   VDP1 bank full (Phase-1 worsens); px = clampable fill-work proxy (span*cols = the master
   software cost Phase-1 removes).  A big clamp/px => build the clamp; mostly mag/starve => reconsider. */
extern "C" int sat_fb_clamp_t, sat_fb_mag_t, sat_fb_starve_t, sat_fb_px;
extern "C" int sat_fb_wclamp_t;   /* Phase-1: tiers KEPT on VDP1 by the cut+wedge clamp */
static int fb_cur_clamp = 0, fb_cur_mag = 0, fb_cur_px = 0;             /* last rendered frame */
static int fb_cur_wclamp = 0;
static int fb_pk_clamp  = 0, fb_pk_mag  = 0, fb_pk_starve = 0, fb_pk_px = 0;  /* windowed peaks (reset on config change) */

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
           (the pad is already saturated: Y=SQ X=split Z=mode-M L+A=blit). */
        {
            static int l_map=-1, l_m=-1, l_sq=-1, l_blit=-1;
#if SAT_FLOOR_PERFSIM
            static int l_perfsim=-1;   /* reset the REC window on a pad-Y perf-sim toggle => clean per-mode numbers */
#endif
#if SAT_DIAG_SLAVE_TOGGLES
            static int l_steal=-1, l_wp=-1;
#endif
            if (gamemap != l_map || sat_m != l_m || (sq_wall<<4|sq_floor<<2|sq_ceil) != l_sq || blit_mode != l_blit
#if SAT_FLOOR_PERFSIM
                || floor_perfsim_mode != l_perfsim
#endif
#if SAT_DIAG_SLAVE_TOGGLES
                || sat_plane_steal != l_steal || sat_wallprep_slave != l_wp
#endif
               ) {
                RP_ProfReset();
                vd1_win_done = vd1_win_tot = 0;
                fb_pk_clamp = fb_pk_mag = fb_pk_starve = fb_pk_px = 0;   /* Phase-0: clean fallback A/B window */
                blit10_sum = blit10_cnt = 0;   /* row-1 'b' precise window: fresh sample on the L+A toggle */
                l_map=gamemap; l_m=sat_m; l_sq=(sq_wall<<4|sq_floor<<2|sq_ceil); l_blit=blit_mode;
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
        /* row 0 HEADLINE: inst fps, EMA(~4s) avg (the build-comparison number), MST (=1000/fps, the
           master frame ms), to = slave-timeout count (must stay 0), cd = CD read-retries.  Shown in
           every overlay mode except OFF(2); the fps-only mode(1) shows ONLY this row, so the
           mode0<->mode1 fps delta measures the overlay's own per-frame tax. */
        unsigned int mst = inst10 ? (10000u / inst10) : 0u;
        static char r0[45];
        sprintf(r0, "%u.%ufps a%u.%u MST%u to%d cd%d      ",
                inst10 / 10, inst10 % 10, avg10 / 10, avg10 % 10,
                mst, rp_timeout_count, sat_cd_read_retries);
        if (sat_dbg_overlay_mode != 2) SRL::Debug::Print(0, 0, r0);
        /* row 1: MASTER-FRAME COMPOSITION, window-AVERAGED over this 1s tick (ms) -- so a single
           heavy frame is never read as the general case (percentiles are on row 3).  Decomposes MST:
             R  = render (REC = B+P+M), DERIVED = MST - T - S - b - dg  (=> R+T+S+b+dg == MST)
             T  = game-tic     (core TryRunTics: thinkers / P_Ticker / P_CheckSights)
             S  = sound        (core S_UpdateSounds)
             b  = framebuffer->VDP2 blit
             dg = DG_DrawFrame pre+post (sky + present-kick + split-HUD + this overlay + index-0 clear)
             pr = VDP1 present-kick (tenths-ms) -- informational; a SUBSET of R (early kick runs in render). */
        unsigned int _f    = df_frames ? df_frames : 1u;
        unsigned int _tic  = df_tic_sum / _f;
        unsigned int _snd  = df_snd_sum / _f;
        unsigned int _blit = df_blit_sum / _f;
        unsigned int _dg   = (df_pre_sum + df_post_sum) / _f;
        unsigned int _pr10 = df_present_sum / _f;                        /* tenths-ms */
        unsigned int _used = _tic + _snd + _blit + _dg;
        unsigned int _rec  = (mst > _used) ? (mst - _used) : 0u;         /* derived render */
        static char r1[45];
        char blit_c = blit_cfg[blit_mode].dma ? 'd' : 'c';   /* blit path: c=CPU, d=slDMACopy (parked) */
        char blit_w = blit_cfg[blit_mode].w5  ? '5' : '-';   /* W5 HUD-skip: 5=on, -=off (L+A)  */
        /* 'b' = PRECISE blit mean in tenths-ms (FRT sat_blit_ms10, windowed since the last L+A
           toggle) -> resolves the ~1.5ms W5/DMA deltas the old integer rounded away.  Folded into
           THIS field (no new overlay row -- rows are saturated across dg_saturn + r_parallel). */
        unsigned int bmt = blit10_cnt ? (blit10_sum / blit10_cnt) : 0u;   /* tenths-ms */
        sprintf(r1, "R%u T%u S%u b%u.%u%c%c dg%u pr%u.%u    ",
                _rec, _tic, _snd, bmt / 10, bmt % 10, blit_c, blit_w, _dg, _pr10 / 10, _pr10 % 10);
        if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 1, r1);
        /* window reset -- read the row-1 composition ABOVE before this zeroes the sums.  The
           dead RAM/TXC/ZON sizer block (TEX/SPL/TXC/ZON, all display-off) was cut with the
           overlay clean-up; re-add from git if a memory-lever session needs it. */
        df_pre_sum = df_blit_sum = df_post_sum = 0;
        df_tic_sum = df_snd_sum = df_present_sum = df_frames = 0;
        rbg_sky_sum = rbg_upl_sum = rbg_xfm_sum = rbg_rpt_sum = 0;
        rp_master_ms = mst;   /* master frame ms, exposed for the shared core */
        {
            /* row 2: VDP1 load + done-rate + build stamp.  VD1 = cmds this frame + D/B
               (EDSR-CEF this frame) + Dr = % of plotted frames Done over the window.
               Post-8bpp the VDP1 CAN finish within a frame, so Dr is a live VDP1-floor
               headroom signal (high Dr => spare budget for floor strips).  b:__TIME__ =
               build stamp (build.ps1 touches this file so it refreshes). */
            unsigned int dr = vd1_win_tot ? (vd1_win_done * 100u / vd1_win_tot) : 0;
            static char r2v[52];
#if VDP1_MANUAL_CHANGE
            char pmode_c  = vdp1_present_manual ? 'M' : 'A';   /* P A=auto(tear) / M=gated(tear-free, ~1vbl lag) */
            char couple_c = vdp1_couple_nbg1 ? 'C' : '-';      /* C = NBG1 blit coupled to the VDP1 present */
            snprintf(r2v, sizeof r2v, "VD1 %d%c Dr%u%% P%c%c b:" __TIME__,
                    vdp1_last_cmds, vdp1_prev_done ? 'D' : 'B', dr, pmode_c, couple_c);
#else
            snprintf(r2v, sizeof r2v, "VD1 %d%c Dr%u%% b:" __TIME__,
                    vdp1_last_cmds, vdp1_prev_done ? 'D' : 'B', dr);
#endif
            (void)r2v;   /* VD1 row cut (perf overlay clean-up); dr reused on row 3 */
#if SAT_FLOOR_TEX
            /* row 12 (free): FTX = floor-texture state.  m = pad-Y mode (0 flat-FB / 1 flat-lit /
               2 textured), t = tiles drawn last frame, s = skipped (z-guard / blow-up / no cache
               slot), x = truncations (budget or grid caps -- flat underlay covered, never a hole),
               bk = flat cache bakes (misses; steady >0 = 6 slots thrash).  With Dr% (row 2) these
               are the GO/NO-GO counters of the textured-floor step. */
            static char r12[45];
            snprintf(r12, sizeof r12, "FTX m%d a%d t%d s%d x%d bk%d p%dK sl%d  ",
                     sat_ftex_mode, ftexd_acc, ftexd_tiles, ftexd_skips, ftexd_trunc,
                     ftexd_bakes, ftexd_px >> 10, sat_ftex_slave);
            (void)r12;   /* FTX row cut (floor-tex state, not perf/composition) */
#endif
            /* row 4: WINDOWED REC distribution p50/p95/max (tenths-ms) -- robust to the
               single-outlier max (a lone CD hitch) AND to an arbitrary threshold.  p50 =
               typical, p95 = sustained worst, mx = absolute worst (located on row 9).
               Window auto-resets on a config change (above).  ~29 cols. */
            static char r4[45];
            int p50 = RP_ProfPercentile(50), p95 = RP_ProfPercentile(95);
            snprintf(r4, sizeof r4, "REC 50:%d.%d 95:%d.%d mx%d.%d d%d      ",
                    p50/10, p50%10, p95/10, p95%10,
                    sat_prof_rec_max/10, sat_prof_rec_max%10, sat_prof_dropped);
            (void)dr;   /* Dr% dropped from the overlay: CEF/vblank-sampling aliasing makes it
                           unreliable (owner: "Dr% biaisé"); the Dr0-but-floor-complete captures
                           confirm it does NOT signal VDP1 saturation. */
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 3, r4);   /* row 3: REC percentiles + VDP1 Dr% */
            /* row 8 (free -- rows 3/4/5 belong to r_parallel's Bw/Bp critical-path overlay): deported-plane
               VERTICAL decrochage-fill knob (pad R+Up/Down).  vs = scale (sat_plane_vscale), bV = this frame's
               actual vertical fill-border px. */
            static char r8[32];
            snprintf(r8, sizeof r8, "PVfill vs %d  bV %d bM %d   ",
                     sat_plane_vscale, sat_plane_border_v, sat_plane_border_max);
            (void)r8;   /* PVfill row cut */
            /* row 10: per-PHASE INDEPENDENT peaks (each phase's own worst across the
               window, possibly different frames) -- the basis to size each offload
               (Bp -> slave wall-prep, P -> VDP1/RBG0 floor).  ~31 cols worst case. */
            static char r10[45];
            snprintf(r10, sizeof r10, "PK Bw%d.%d Bp%d.%d P%d.%d M%d.%d        ",
                    sat_prof_pk_bw/10, sat_prof_pk_bw%10, sat_prof_pk_bp/10, sat_prof_pk_bp%10,
                    sat_prof_pk_p/10,  sat_prof_pk_p%10,  sat_prof_pk_m/10,  sat_prof_pk_m%10);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 4, r10);   /* row 4: per-phase peaks */
            /* row 9: WHERE/WHEN the REC-max frame was (the locator), so the worst frame is
               reproducible.  m=map, x,y=player render pos (map units), a=angle 0-255,
               t=sec into the level.  ~31 cols worst case (6-digit coords). */
            static char r9[45];
            snprintf(r9, sizeof r9, "MX m%d %d,%d a%d t%ds        ",
                    sat_prof_mx_map, sat_prof_mx_x, sat_prof_mx_y, sat_prof_mx_ang,
                    sat_prof_mx_t/35);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 6, r9);   /* row 6: REC-max locator */
            /* row 7: ACTIVE render composition -- so an A/B is never read against the wrong state.
               M<n>:name = offload mode (pad Z).  RESOLVED per-surface targets: fl:<dom><leftover>
               cl:<ceil> sk:<sky> wl:<wall>, where R=RBG0 V=VDP1 N=NBG0 .=software(CPU).  SQ:<W><F><C>
               software quality per zone (pad R+Y/Y/L+Y): f=full l=ld b=band x=flat.  pm=plane split
               0stat/1TAS/2rowsplit (pad C). */
            static const char sqch[4] = { 'f', 'l', 'b', 'x' };   /* full / ld / band / flat */
            char m0 = (sat_m == M0_SOFT);
            char dm = m0 ? '.' : 'R';                                        /* dominant floor */
            char lf = m0 ? '.' : (sat_vdp1_floor_claim ? 'V' : '.');         /* leftover floors */
            char cl = m0 ? '.' : (sat_vdp1_ceil_claim  ? 'V' : '.');         /* ceilings        */
            char sk = m0 ? '.' : 'N';                                        /* sky             */
            char wl = m0 ? '.' : 'V';                                        /* walls           */
            static char r7[45];
            snprintf(r7, sizeof r7, "M%d %s fl:%c%c cl:%c sk:%c wl:%c SQ:%c%c%c pm%d ",
                     sat_m, sat_m_name[sat_m], dm, lf, cl, sk, wl,
                     sqch[sq_wall & 3], sqch[sq_floor & 3], sqch[sq_ceil & 3],
                     sat_plane_rowsplit ? 2 : sat_plane_tas);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 7, r7);
            /* row 8: RELIABLE VDP1 load (replaces the CEF-aliased Dr%).  w%/f% = wall/floor
               command-budget FILL % (100% = bank full, further surfaces spill to CPU); fbw = walls
               dumped to CPU because the bank filled (windowed peak); fbf = floor tiles dropped
               (trunc).  fbw|fbf > 0 = VDP1 genuinely over budget this window (the master pays). */
            static char rVD[45];
            snprintf(rVD, sizeof rVD, "VD1 w%d%% f%d%% fbw%d fbf%d   ",
                     vd1_wpct, vd1_fpct, fb_pk_starve, ftexd_trunc);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 8, rVD);
            /* row 5 (free): LINE-OF-SIGHT volume this ~1s window -- the game-tic (T, row 1) cost
               driver.  rej = P_CheckSight calls the REJECT table trivially rejected in O(1); walk =
               full BSP LOS traversals (P_CrossBSPNode, the expensive division-heavy path).  In the
               streaming-mode DEFAULT rejectmatrix is NULL so rej stays 0 and EVERY check full-walks
               -- a high walk with rej0 quantifies the REJECT lever (build -DSAT_KEEP_REJECT=1 to
               convert walks -> rej and read the T delta).  First window shows cumulative-since-boot,
               then per-window deltas. */
            static int l_sc0 = 0, l_sc1 = 0;
            int d_sc0 = sightcounts[0] - l_sc0;
            int d_sc1 = sightcounts[1] - l_sc1;
            l_sc0 = sightcounts[0]; l_sc1 = sightcounts[1];
            static char rLOS[45];
            snprintf(rLOS, sizeof rLOS, "LOS rej%d walk%d /win        ", d_sc0, d_sc1);
            /* row 13: was row 5, but r_parallel's SLVidle ('SLV') p3 row ALSO writes row 5 in
               the shipping (rp_disabled) config -> they collided.  Moved to the free row 13. */
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 13, rLOS);
            /* row 15 (SCU-DSP feasibility, deliverable #1): per-frame sprite cost split.
               pj = R_ProjectSprite time (the arithmetic a DSP could offload); fl = master
               R_DrawVisSprite fill (memory-bound, ~2x for total incl. slave right-half);
               n = things projected / vissprites filled.  Hold a monster-heavy scene still
               and read: pj<<fl => projection is not the sprite cost, so offloading it is
               pointless.  Tenths-ms; FRT-quantised (~0.02ms/tick) so pj jitters +-0.1ms. */
            int sp_pj = 0, sp_fl = 0, sp_np = 0, sp_nd = 0;
            RP_SprStats(&sp_pj, &sp_fl, &sp_np, &sp_nd);
            static char rSPR[48];
            /* th e/d = world-things emitted on VDP1 / declined; fb = baked THIS frame (instant);
               sb = SESSION bake% (mh_bake_sum/mh_emit_sum) = the cache read -- low = high reuse.
               On row 15 (bottom, clear of centre-screen monsters that hide the THp row). */
            unsigned int sbpc = mh_emit_sum ? (mh_bake_sum * 100u / mh_emit_sum) : 0;
            snprintf(rSPR, sizeof rSPR, "SPR pj%d.%d fl%d.%d n%d/%d th%d/%d fb%d sb%u%%  ",
                     sp_pj/10, sp_pj%10, sp_fl/10, sp_fl%10, sp_np, sp_nd,
                     sat_things_n, sat_things_decl, thing_bake_n, sbpc);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 15, rSPR);
            /* rows 9-10: SESSION percentiles (reset ONLY on a MODE change) -- read ONE end-of-level
               capture, no jitter.  THp = world-things emitted/frame p50/p99, declined/frame p99,
               occluded-skip avg/frame, f = sample count.  FMp = frame time (ms) p50/p90/p99 + max,
               D = VDP1 plot done-rate % (headroom; low D => things at the list tail get cut = flicker). */
            int f_p50 = mh_pct(mh_ms, MH_MS_BUCKETS, 50) << MH_MS_SHIFT;
            int f_p90 = mh_pct(mh_ms, MH_MS_BUCKETS, 90) << MH_MS_SHIFT;
            int f_p99 = mh_pct(mh_ms, MH_MS_BUCKETS, 99) << MH_MS_SHIFT;
            int t_p50 = mh_pct(mh_things, MH_N_BUCKETS, 50);
            int t_p99 = mh_pct(mh_things, MH_N_BUCKETS, 99);
            int d_p99 = mh_pct(mh_decl, MH_N_BUCKETS, 99);
            unsigned int occ10 = mh_frames ? (mh_occ_sum * 10u / mh_frames) : 0;
            unsigned int don    = mh_vbl_tot ? (mh_vbl_done * 100u / mh_vbl_tot) : 0;
            static char rTHp[45], rFMp[45];
            snprintf(rTHp, sizeof rTHp, "THp n%d/%d d%d o%u.%u f%u    ",
                     t_p50, t_p99, d_p99, occ10 / 10, occ10 % 10, mh_frames);
            snprintf(rFMp, sizeof rFMp, "FMp %d/%d/%d mx%u D%u%%    ",
                     f_p50, f_p90, f_p99, mh_ms_mx, don);
            if (sat_dbg_overlay_mode == 0) { SRL::Debug::Print(0, 9, rTHp); SRL::Debug::Print(0, 10, rFMp); }
            /* row 11 (ENDGAME limits high-water): how close this ~1s window got to the render
               HARD-HALT caps that I_Error-freeze a big WAD (docs/ENDGAME_ROADMAP.md Axis 2).
               vp = peak visplanes / MAXVISPLANES(256); ds = peak drawsegs / MAXDRAWSEGS(256);
               zf = zone free (KB); lg = largest contiguous purgeable run (KB) = the fragmentation-
               vs-exhaustion signal.  vp/ds are core running-maxes zeroed here each window; when
               either climbs toward 256 on Doom II MAP13/15 that is the cap that crashes next.
               (VP_POOL_PLANES=96 span-pool overflow -> r_visplane_pool_ovf is a GRACEFUL flat
               glitch, not a freeze -- tracked separately, not shown here.) */
            static char rLIM[48];
            snprintf(rLIM, sizeof rLIM, "LIM vp%d/256 ds%d/256 zf%dk lg%dk ",
                     r_visplane_peak, r_drawseg_peak,
                     Z_FreeMemory() >> 10, Z_LargestAllocatable() >> 10);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 11, rLIM);
            r_visplane_peak = 0;   /* zero the core running-maxes -> next window re-accumulates its own peak */
            r_drawseg_peak  = 0;
            /* row 17: FLOOR offload sizers.  Vs/Vp = VDP1-floor candidate quad count this
               frame / window peak (go/no-go: GO if Vp fits the VDP1 cmd budget).  d% = the
               dominant single-flat share of plane pixels, n = visplane count (RBG0 sweet
               spot = high d% + low n => one flat owns the floor).  ~24 cols. */
            static char r17[45];
            snprintf(r17, sizeof r17, "FLR Vs%d Vp%d d%d%% n%d        ",
                    sat_floor_vq_cur, sat_floor_vq_peak, sat_prof_dom_pct, sat_prof_plane_n);
            (void)r17;   /* FLR sizer row cut (arm sat_prof_planepix to size the floor) */
            /* row 20 (pari A sizing): "all floors+ceilings as VDP1 quads" (PowerSlave model).
               ss = visible subsectors, Q = geometry quad count this frame (fan pieces,
               UNtextured -> texture tiling would multiply), Qp = window peak, q4 = % of
               surfaces from <=4-sided (pure-quad) subsectors. */
            static char r20[45];
            snprintf(r20, sizeof r20, "PAR ss%d Q%d Qp%d q4%d%%      ",
                    sat_prof_ss_n, sat_prof_ss_q, sat_prof_ss_qpk, sat_prof_ss_q4pct);
            (void)r20;   /* PAR row cut */
            /* row 6: Phase-0 wall CPU-fallback sizer.  c = clampable tiers cur/pk (SPAN + below-floor
               = the Phase-1 world-anchored VDP1 clamp target); m = face-on magnified residue cur/pk;
               s = starved (VDP1 bank full) pk; K = clampable fill proxy cur/pk in kilo-pixels (the
               master software fill Phase-1 removes).  W = tiers Phase-1 KEPT on VDP1 (cut+wedge)
               this frame + the clamp state: c should melt where W rises (L+R+Y toggles). */
            static char rFB[45];
            snprintf(rFB, sizeof rFB, "FBK c%d/%d m%d/%d s%d K%d/%d W%d%c  ",
                     fb_cur_clamp, fb_pk_clamp, fb_cur_mag, fb_pk_mag, fb_pk_starve,
                     fb_cur_px >> 10, fb_pk_px >> 10,
                     fb_cur_wclamp, sat_wall_clamp ? '+' : '-');
            (void)rFB;   /* FBK wall-fallback sizer row cut */
            /* row 18: memory-latency calibration (one-shot cold 32 KB read per bank, FRT
               ticks).  rL = LWRAM/HWRAM ratio -- >1.0 means LWRAM (cmd buf + visplanes) is
               the slow bank, = the memory-bound penalty + the L2-relocate upside, measured
               on THIS hardware (Ymir will read ~1.0 -- it does not model the bank gap). */
            unsigned int rL10 = mem_hw_ticks ? (mem_lw_ticks * 10u / mem_hw_ticks) : 0;
            static char r18[45];
            snprintf(r18, sizeof r18, "MEM lw%u hw%u rL%u.%u        ",
                    mem_lw_ticks, mem_hw_ticks, rL10/10, rL10%10);
            /* [overlay lean] SRL::Debug::Print(0, 18, r18);  -- MEM rL (rL~2.1 noted), off for now */
#if SAT_FLOOR_PERFSIM
            /* row 19: pad-Y floor perf-sim mode.  Read REC (row 4) / P (row 5) in each mode;
               the delta vs mode 0 = the floor-offload saving (same for RBG0/VDP1/gradient). */
            static const char *const perfsim_name[4] = {
                "0 NORMAL", "1 DOM-ABSENT(vdp2)", "2 ALL-BUT-DOM(vdp1)", "3 BOTH" };
            static char r19[45];
            snprintf(r19, sizeof r19, "PERFSIM %s        ", perfsim_name[floor_perfsim_mode & 3]);
            (void)r19;   /* PERFSIM row cut */
#endif
            /* row 13: sky-vs-floor coverage classifier (Romain).  sky/flr = pixels the sky / the
               dominant floor cover this frame.  flr is non-zero only in a perf-sim floor-on mode
               (pad-Y 1/3).  Big sky% => the HW-sky bank earns its keep; tiny sky% => it's wasted,
               free it for a textured VDP2 floor.  W = which covers more (S=sky, F=floor). */
            static char r13[45];
            snprintf(r13, sizeof r13, "CLS sky%u flr%u %c       ", sat_sky_px, sat_floor_px,
                    (sat_sky_px >= sat_floor_px) ? 'S' : 'F');
            (void)r13;   /* CLS classifier row cut */
        }
#if VDP2_RBG0_TEST
        {
            /* row 14: RBG0 RAMCTL commit readback (visible in pad-Y debug modes 1/2).
               b = chip RAMCTL before our RDBS write, a = after.  Low byte should read
               0x8D (A0=coeff A1=char B0=fb B1=PN); bits 8-9 = 4-bank split.  If a != that
               or snow persists, the rotation banks' CYC pattern also needs writing. */
            static char rR[45];
            sprintf(rR, "RAMCTL b=%04X a=%04X", ramctl_before, ramctl_after);
            (void)rR;   /* RAMCTL row cut */
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
            (void)r21;   /* DRP streaming-status row cut (not perf/composition) */
        }
#endif
        /* fps-only mode: SRL::Debug tiles persist, so a row we stop writing would GHOST.  Blank
           rows 1-16 (every per-frame row: 1-8 the core B/P/M/REC/PK/SLV block, 11 LIM, 13 LOS,
           15 SPR -- all gated on mode==0 for both writers, so nothing re-fills them here) so only
           row 0 (fps + MST) remains -> the mode0<->mode1 fps delta is clean.  Mode 2 hides the
           whole NBG3 layer (nbg3_show=0), so no blanking is needed there. */
        if (sat_dbg_overlay_mode == 1) {
            static const char bl[] = "                                        ";
            for (int rr = 1; rr <= 16; ++rr) SRL::Debug::Print(0, rr, (char *)bl);
        }
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
    static FIXED rbg0_sd_default = 0;
    if (!rbg0_sd_default) rbg0_sd_default = MsScreenDist;   /* SGL default screen distance, captured ONCE (before any slSetScreenDist) */
    slPushMatrix();
    {
        slRotX((ANGLE)(0x4000 + RBG0_PITCH + (sat_split_p1hw ? rbg0_split_pitch : rbg0_pitch_adj))); /* 90deg + pitch */
        slRotZ((ANGLE)(-(sat_split_p1hw ? ((int)(viewangle >> 16) * rbg0_split_yawsc >> 4) : (int)(viewangle >> 16))
                       + RBG0_YAW_OFF + (sat_split_p1hw ? rbg0_split_yaw : 0))); /* yaw track (split: rate-scale C+U/D) + 90deg + offset (C+L/R) */
        slTranslate(sat_split_p1hw ? (FIXED)(((int64_t)(-viewx) * rbg0_split_scroll) >> 4) : (FIXED)(-viewx),
                    sat_split_p1hw ? (FIXED)(((int64_t)(-viewy) * rbg0_split_scroll) >> 4) : (FIXED)(-viewy),
                    -(viewz - sat_vdp2_floor_h) + rbg0_z_adj); /* X/Y scroll (split: rate-scaled C+U/D to match SW) + Z height, WORLD space */
        slCurRpara(RA);
#if RBG0_SPLIT_P1HW
        /* SATURN split: slDispCenterR sets the RBG0 ROTATION-SCROLL display centre (SGL SCROLL.TXT:480 "Sets
           rotation center coordinates of rotation scroll screen") -- the CORRECT API for the floor's vanishing
           point.  slWindow's CtX is the VDP1 *polygon* vanishing point, NOT RBG0 -> that's why cx via slWindow
           was inert; and patching Cx alone left XST computed for centre 160 -> inconsistent -> half-texel
           jitter.  slScrMatSet (below) then bakes XST/DX/matrix/Px/Cx ALL consistent for this centre, and the
           extended 0x54 RPT copy transfers them together.  cx=80 = centre of P1's left half; 1p uses 160.
           CtY=112 keeps the vertical identical (hz/pitch stay valid). */
        if (sat_split_p1hw) {
            slDispCenterR((FIXED)(rbg0_split_cx << 16), (FIXED)(rbg0_split_cy << 16));  /* rotation CENTRE: Cx=cx, Cy (Px/Py follow) */
            slSetScreenDist((FIXED)(((int64_t)rbg0_sd_default * rbg0_split_sd) >> 4));  /* focal for the 160px viewport */
            slScrMatConv();
            slScrMatSet();
            /* NO restore here!  Restoring Cx/dist BEFORE the DG_DrawFrame memcpy shipped a Frankenstein RPT
               (Xst baked for centre 80 but Cx/Px reset to 160) -> that broke the rotation (1p, which never
               restores, was clean).  1p sets its OWN centre/dist in the else branch instead, so the split's
               consistent centre-80 table reaches VRAM intact. */
        } else
#endif
        {
            slDispCenterR((FIXED)(160 << 16), (FIXED)(112 << 16));   /* 1p: explicit full-screen centre */
            slSetScreenDist(rbg0_sd_default);                        /* 1p: default screen distance */
            slScrMatConv();
            slScrMatSet();
        }
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
    static int loaded = -2, loaded_q = -2, loaded_c = -2;
    if (picnum < 0) return;
    /* SATURN 2026-06-29 (revert to BAKED dimming, QUANTIZED): the RBG0 bitmap palette-BANK switch does
       NOT work on HW -- slBMPaletteRbg0 only writes an SGL shadow that reaches the chip solely in
       _BlankIn under a slSynch gate we never open (no per-frame slSynch), and a 256-colour ROTATION
       bitmap's R0BMP page-select isn't honoured at runtime either (disasm-proven, workflow wf_4ff6c62b).
       So bake the sector light INTO the texels (the proven pre-3af4b6c path that dimmed on HW), but
       quantize to the 7 wall levels {0,5,10,16,21,26,31} and re-upload ONLY when the FLAT or the
       QUANTIZED level changes -> a glow/flicker sector re-uploads only on a band CROSSING (rare), not
       EVERY frame (the décrochage stall).  BMPNB stays at the init bank 1; the texels carry the shade. */
    /* FIXED uniform dim baked over the WHOLE floor (live-tunable via pad-C -> rbg0_floor_dim).
       Per-sector shading can't reach the chip (bank-switch dead; zlight[li][0]=nearest clamps to 0=full
       bright), so bake ONE dim colormap level into the texels.  0 = full bright .. 31 = near black. */
    /* Base flat dimmed PER SECTOR (lower lux in darker rooms) + the manual offset.  band 15 (bright) ->
       level 0, band 0 (dark) -> ~30.  Re-bakes (via the qlevel guard) on a band change = a brief blip
       when you cross into a differently-lit room; the BLACK VEIL (line-color) layers the far darkening on top. */
    int band = sat_vdp2_floor_band; if (band < 0) band = 0; else if (band > 15) band = 15;
    int qlevel = (15 - band) * 2 + rbg0_floor_dim;
    if (qlevel < 0) qlevel = 0; else if (qlevel > 31) qlevel = 31;
    if (picnum == loaded && qlevel == loaded_q && rbg0_floor_contrast == loaded_c && !rbg0_tex_dirty) return;
    const unsigned char *flat = sat_vdp2_floor_data();
    if (!flat) return;
    loaded = picnum; loaded_q = qlevel; loaded_c = rbg0_floor_contrast;
    rbg0_tex_dirty = 0;
    /* BRIGHTNESS (qlevel) + CONTRAST: build a per-original-index shade LUT, so the inner loop stays a
       single table lookup (cm_q[flat[..]]) -- same cost as the old straight colormap row.  mid = the
       flat's MEAN luma = the contrast pivot; level = qlevel spread by contrast around it (bright texels
       -> lower level/brighter, dark -> higher/darker).  contrast 0 -> cm_q[i]=colormaps[qlevel*256+i]
       (the old uniform-dim behaviour exactly). */
    int mid; { long s = 0; for (int i = 0; i < 64*64; ++i) { int o = flat[i]; s += colors[o].r + colors[o].g + colors[o].b; } mid = (int)(s >> 12); }
    unsigned char cm_q[256];
    for (int i = 0; i < 256; ++i) {
        int lum = colors[i].r + colors[i].g + colors[i].b;            /* 0..765 */
        int lvl = qlevel + rbg0_floor_contrast * (mid - lum) / 256;   /* spread the level around the mean */
        if (lvl < 0) lvl = 0; else if (lvl > 31) lvl = 31;
        cm_q[i] = colormaps[lvl * 256 + i];
    }
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
            row[x] = cm_q[flat[fy * 64 + fx]];   /* BAKE the quantized sector shade into the texel */
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
                    c[ry * 8 + rx] = cm_q[flat[(cy * 8 + ry) * 64 + (cx * 8 + rx)]];   /* baked quantized shade */
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
/* A0 spare (past the 2KB K-table); the 256-entry per-line line-colour table.  VDP2 VRAM at
   0x25Exxxxx is uncached, so direct halfword writes reach VRAM with no purge. */
#define LINECOL_TBL_VRAM ((void *)0x25E01000)   /* 512B: per-line line-colour table */
#define LINEWIN_TBL_VRAM ((void *)0x25E01400)   /* 1KB:  per-line color-calc WINDOW table (A0 spare past it) */
/* Veil params (rbg0_lc_trans/zonek/zoneoff/ratio) are declared early (near rbg0_linecol_mode) for the
   overlay.  See those decls + the pad handler for the live R/C+d-pad controls. */
/* rbg0_linecol_mode: 0 off / 1 uniform / 2 step / 3 wash (declared early for the overlay).
   Build a per-LINE line-colour table so the floor dims by DISTANCE (rows near the horizon are
   farther -> darker).  Each entry: MSB 0x8000 = "insert this line colour" (blended into RBG0 by
   CCRR); 0x0000 = no insert = full bright.  The horizon row = viewwindowy+centery, floor bottom =
   viewwindowy+viewheight (read live).  Composes with the palette-bank sector light.
   Set the table-mode SGL regs here too -- at init they're committed by the one-shot slSynch; at
   runtime only the VRAM table + CCRR (direct-poke) change, so no per-frame commit is needed. */
/* Rewrite the per-line line-colour table (uncached VRAM).  NO SGL calls -> runtime-safe (called
   on the pad-C toggle).  Calling SGL line-col/color-calc funcs at runtime caused the one-frame
   BLACK FLASH; they live in rbg0_linecol_apply (init only). */
static void rbg0_linecol_rebuild(void)
{
    int m = rbg0_linecol_mode;
    volatile unsigned short *t = (volatile unsigned short *)LINECOL_TBL_VRAM;
    /* Work in RASTER scanlines -- the line-colour table is raster-indexed and the framebuffer is shown
       1:1 (walls reach raster 223).  hz = the sky/floor horizon you can SEE and tune (sky_horizon_row),
       NOT Doom's centery: that screen-vs-raster mismatch is what FLIPPED the gradient before.  This row
       is valid at init too, so the boot table is correct (no more boot-flat). */
    int hz   = sky_horizon_row;          /* raster row of the horizon (top of the floor / far)  */
    int bot  = 224;                      /* screen bottom (raster) = nearest floor row          */
    int span = bot - hz; if (span < 1) span = 1;
    /* CRITICAL: the entry 0x0000 is NOT "no insert" -- it inserts CRAM index 0 = BLACK (0x8000 is the
       table-MODE bit, set once by slLineColTable, NOT a per-entry skip flag).  So every scanline blends
       RBG0 toward its table colour; there is no true pristine row.  The brightness ramp therefore runs
       bank nb (~the floor's own brightness, NEAR/bottom -> minimal change) .. bank 7 (near-black,
       FAR/horizon).  NEAR is kept as close to the base as the mechanism allows (bank nb, a grey at the
       floor's level -> small wash); FAR darkens, where the foreshortened floor hides the wash. */
    int G = 96, bestd = 1 << 30; const int target = 160;   /* sum ~= 53/channel: a MID neutral grey so the
                                                              insert ONSET (low banks) ~matches the dim base
                                                              -> seamless; bank 7 crushes it to near-black. */
    for (int i = 1; i < 256; ++i) {
        int r = colors[i].r, g = colors[i].g, b = colors[i].b;
        int mx = r>g ? (r>b?r:b) : (g>b?g:b);
        int mn = r<g ? (r<b?r:b) : (g<b?g:b);
        if (mx - mn <= 28) { int dd = (r + g + b) - target; if (dd < 0) dd = -dd; if (dd < bestd) { bestd = dd; G = i; } }
    }
    /* SECTOR-DRIVEN BLACK VEIL.  Zone = [hz, bd]; bd scales with the room light band (bright -> thin veil,
       band 15 -> bd=hz = NONE).  Shape: a short TRANSITION (base shade -> black, rbg0_lc_trans rows from
       the boundary) then a long pure-black band to the horizon.  At the high ratio the floor is fully
       replaced by these colours, so the veil reads BLACK (no green residual). */
    int band = sat_vdp2_floor_band; if (band < 0) band = 0; else if (band > 15) band = 15;
    int bd = hz + (15 - band) * rbg0_zonek + rbg0_zoneoff;
    if (bd < hz) bd = hz; else if (bd > bot) bd = bot;
    rbg0_lc_far = bd;                                    /* publish the computed boundary for the overlay */
    int seclvl = (15 - band) * 2 + rbg0_floor_dim; if (seclvl < 0) seclvl = 0; else if (seclvl > 31) seclvl = 31;
    int onset = 1 + seclvl * 6 / 31;                     /* veil onset bank ~ the dimmed base brightness (smooth) */
    if (onset < 1) onset = 1; else if (onset > 7) onset = 7;
    int T = rbg0_lc_trans; if (T < 1) T = 1;
    (void)span;
    for (int y = 0; y < 256; ++y)
    {
        unsigned short entry = 0x0000;                  /* clean zone (windowed) + off-floor */
        if (m && y >= hz && y < bd)
        {
            int dist = bd - y;                          /* 0 at the boundary .. (bd-hz) at the horizon */
            if (dist < T)                               /* short transition: base shade -> black */
            {
                int bank = onset + (8 - onset) * dist / T;
                if (bank < 1) bank = 1; else if (bank > 8) bank = 8;
                entry = (bank >= 8) ? (unsigned short)0x0000 : (unsigned short)(bank * 256 + G);
            }
            /* else dist >= T: long PURE-BLACK band -> entry stays 0x0000 */
        }
        t[y] = entry;
    }
}
/* Per-line COLOR-CALC WINDOW table: 2 u16/line {startX,endX}, 256 lines = 1KB in A0 spare just past the
   512B line-color table.  Rows ABOVE the boundary = full-width range (INSIDE -> color-calc ON = gradient);
   rows from the boundary DOWN = empty range start>end (OUTSIDE -> color-calc OFF = clean baked floor).
   Uncached VRAM the VDP2 reads itself -> runtime-writable with NO SGL call / NO flash (like the line-color
   table).  Armed once at init by slScrLineWindow0 + slScrWindowMode(scnCCAL, win0_IN). */
static void rbg0_ccwin_rebuild(void)
{
    volatile unsigned short *w = (volatile unsigned short *)LINEWIN_TBL_VRAM;
    int hz = sky_horizon_row;
    int band = sat_vdp2_floor_band; if (band < 0) band = 0; else if (band > 15) band = 15;
    int bd = hz + (15 - band) * rbg0_zonek + rbg0_zoneoff;   /* SAME boundary as the veil; b15 -> hz = no veil */
    if (bd < 0) bd = 0; else if (bd > 256) bd = 256;
    for (int y = 0; y < 256; ++y)
    {
        if (y < bd) { w[2*y] = 0x0000; w[2*y + 1] = 0x03FF; }   /* INSIDE: full width  -> color-calc ON  */
        else        { w[2*y] = 0x03FF; w[2*y + 1] = 0x0000; }   /* empty (start>end)   -> color-calc OFF */
    }
}
/* Set the RBG0 color-calc RATIO via the SGL SHADOW (slColRate), NOT a raw CCRR poke.  The SGL
   vblank IRQ re-pushes the ratio from its shadow EVERY vblank, so a direct 0x10C poke got reverted
   within the frame -> the flicker.  Updating the shadow makes the IRQ push OUR value -> it persists
   with NO per-frame poke.  Called on the pad-C toggle + at init. */
static inline void rbg0_linecol_ccrr(void)
{
    /* The blend depth lives on the LINE-COLOR (LNCL) layer, NOT RBG0 -- slColRateLNCL, per ReyeMe's
       working ScaryGame recipe.  slColRate writes the SGL shadow -> the vblank IRQ persists it. */
    slColRateLNCL((int16_t)(rbg0_linecol_mode ? rbg0_linecol_ratio : 0));
}
/* INIT ONLY: build the table + enable the line-color screen & color-calc on RBG0.  These SGL
   calls are committed by the init one-shot slSynch; doing them at runtime caused the black flash.
   Recipe verified against ReyeMe's ScaryGame LoadLineColorTable (per-line line-color over RBG0). */
static void rbg0_linecol_apply(void)
{
    rbg0_linecol_rebuild();
    rbg0_ccwin_rebuild();                                /* per-line color-calc WINDOW table         */
    slLineColTable(LINECOL_TBL_VRAM);                    /* per-LINE table mode (LCTA + LCCLMD)      */
    slLineColDisp(RBG0ON);                               /* RBG0 line-color INSERT bit (NOT LNCLON=back) */
    slColorCalc(CC_RATE | CC_2ND | RBG0ON);              /* line color = blended-in 2nd operand      */
    slColorCalcOn(RBG0ON);                               /* RBG0 ONLY -> NBG1/HUD untouched          */
    /* Gate the color-calc to the per-line WINDOW: rows inside window 0 (above the boundary) blend;
       rows outside (the near zone) show the clean baked floor.  ptr bit31 = LWE0 enable.  Window regs
       (WCTLD 0xD6, LWTA0 0xD8) ride the init block-flush; the per-vblank ISR (0x00..0x8E) never touches
       them, so it persists with NO slSynch.  win0_IN = raw WCTLD byte 0x03 (NOT the Window_In enum). */
    slScrLineWindow0((void *)(0x80000000u | (unsigned long)LINEWIN_TBL_VRAM));
    slScrWindowMode(scnCCAL, win0_IN);
    rbg0_linecol_ccrr();
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

    slPriorityRbg0(VDP2_SKY_OCCL_DIAG ? 3 : 4);   /* ship: sky(3) < RBG0 floor(4) < VDP1 walls(5) <
                                    NBG1 game(6); the walls occlude the infinite floor's overspill.
                                    DIAG: floor drops to 3 so NBG0 sky(4) sits ABOVE it. */
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
#if RBG0_NBG3 || VDP2_CELL_SKY
    /* NBG3 and/or the cell sky live in B1: leave CYCB1 EXACTLY as slScrAutoDisp's allocator authored it
       (NBG0 sky = 1 PN + 2 char, NBG3 = 1 PN + 1 char).  Do NOT scrub it -- a hand-pinned/scrubbed value
       would starve the sky's 2nd 8bpp char read = snow on HW (memory rbg0-hw-sky-feasible). */
#else
    VDP2_CYCB1L = 0xFEEE; VDP2_CYCB1U = 0xEEEE;   /* NBG3 off + no cell sky: scrub the stale NBG3 read SGL left */
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

#if RBG0_FLOOR_WINDOW
/* Apply / MOVE the RBG0 floor display window (clip RBG0 to BELOW row hz, via window W1).  At init the
   WPSx1/WCTLC shadow ride rbg0_commit_cyc's 0x0E..0xFE block-flush; to MOVE it at runtime (live
   horizon tune) the per-vblank ISR (0x00..0x8E) never re-pushes the window regs, so we copy W1's
   recomputed position + WCTLC straight from the SGL shadow to the chip (the runtime equivalent of the
   block-flush).  Called from sky_cell_build_map so the floor window tracks the SAME horizon as the
   HW sky.  Cheap: only runs when the horizon actually changes. */
static void rbg0_floor_window_apply(int hz)
{
    if (hz < 0) hz = 0; else if (hz > 223) hz = 223;
    slScrWindow1(0, (uint16_t)hz, (uint16_t)rbg0_floor_win_xend, 223);   /* W1 = [0,hz]..[xend,223]; xend=159 = P1's left half in split */
    slScrWindowModeRbg0(win1_IN);              /* RBG0 displayed INSIDE W1 (WCTLC) */
    volatile uint8_t *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    volatile uint8_t *const chip   = (volatile uint8_t *)0x25F80000;
    for (int off = 0xC8; off <= 0xCE; off += 2)                       /* WPSX1/WPSY1/WPEX1/WPEY1 */
        *(volatile uint16_t *)(chip + off) = *(volatile uint16_t *)(shadow + off);
    *(volatile uint16_t *)(chip + 0xD4) = *(volatile uint16_t *)(shadow + 0xD4);  /* WCTLC (RBG0 window ctrl) */
}
#endif

#if VDP2_SPLIT_HW_SKY
/* Part 5: confine the single NBG0 sky layer to the ELECTED split view's band via VDP2 window W0, so it
   never bleeds into the software views (their opaque sky, and their VDP1 torn wall gaps).  Band geometry
   MIRRORS d_main.c's split loop: 2p = vertical half (x 0..159 | 160..319, full height); 3/4p = quadrant
   (x vpx.., y vpy..).  W0 must be a RECT window here; RBG0_LINECOL_TEST armed it as a per-line CCAL
   window (LWE0=1), but the fog is parked (rbg0_linecol_mode=0 -> ratio 0 -> no visible blend), so
   switching W0 to rect is visually free.  The window regs (WPSx0 0xC0-0xC6, WCTLA 0xD0, LWTA0 0xD8-0xDA)
   ride the block-flush at init but the per-vblank ISR (0x00..0x8E) never re-pushes them, so a runtime
   MOVE needs the shadow->chip poke -- same recipe as rbg0_floor_window_apply for W1/WCTLC. */
static void nbg0_sky_window_apply(int view)
{
    int n = sat_local_players; if (n < 2) n = 2; else if (n > 4) n = 4;
    int twop = (n == 2);
    static const short vpx[4] = { 0, 160, 0, 160 };
    static const short vpy[4] = { 0, 0, 112, 112 };
    if (view < 0) view = 0; else if (view > 3) view = 3;
    int x0 = twop ? ((view & 1) ? 160 : 0) : vpx[view];
    int y0 = twop ? 0 : vpy[view];
    int x1 = x0 + 159;
    int y1 = twop ? 223 : (y0 + 111);
    slScrLineWindow0((void *)0);                              /* LWE0=0: W0 = RECT (drop the parked CCAL line window) */
    slScrWindow0((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
    slScrWindowModeNbg0(win0_IN);                            /* WCTLA: NBG0 displayed INSIDE W0 (its band) */
    volatile uint8_t *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    volatile uint8_t *const chip   = (volatile uint8_t *)0x25F80000;
    for (int off = 0xC0; off <= 0xC6; off += 2)              /* WPSX0/WPSY0/WPEX0/WPEY0 */
        *(volatile uint16_t *)(chip + off) = *(volatile uint16_t *)(shadow + off);
    *(volatile uint16_t *)(chip + 0xD0) = *(volatile uint16_t *)(shadow + 0xD0);  /* WCTLA (NBG0/NBG1 window ctrl) */
    *(volatile uint16_t *)(chip + 0xD8) = *(volatile uint16_t *)(shadow + 0xD8);  /* LWTA0U (LWE0 line-window enable) */
    *(volatile uint16_t *)(chip + 0xDA) = *(volatile uint16_t *)(shadow + 0xDA);  /* LWTA0L */
}
/* Part 5: drop the NBG0 window -> full-screen sky again (1-player, or split HW-sky disabled).  Leaves W0
   in rect mode (the parked CCAL fog does not care); only WCTLA is cleared so NBG0 is shown everywhere. */
static void nbg0_sky_window_clear(void)
{
    slScrWindowModeNbg0(0);                                  /* WCTLA = 0: NBG0 no window (full screen) */
    volatile uint8_t *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    volatile uint8_t *const chip   = (volatile uint8_t *)0x25F80000;
    *(volatile uint16_t *)(chip + 0xD0) = *(volatile uint16_t *)(shadow + 0xD0);  /* WCTLA */
}
#endif

#if VDP2_CELL_SKY
/* Force the VRAM cycle pattern so the NBG0 cell sky reads its cells from bank B1, EVERY frame.
   HW bug (Ymir-invisible): when RBG0 is ON, SGL's per-frame slScrAutoDisp `ape` allocator places
   NBG0's CHARACTER read in bank A1 (the RBG0 rotation char bank = the floor bitmap), so NBG0 shows
   the floor texture FLAT.  When RBG0 is OFF the allocator puts NBG0 in B1 and the sky is correct.
   The _BlankIn ISR re-pushes the allocator's (wrong) cycle every frame, so we must re-author it
   AFTER slScrAutoDisp.  Park A0/A1 (the rotation reads coeff/char via RDBS, NOT the cycle pattern --
   this is the same state the init rbg0_commit_cyc uses, so the floor is unaffected); NBG1 in B0;
   NBG0 (PN code 0 + two 8bpp char code 4) and NBG3 (PN code 3 + char code 7) in B1 -> NBG0 has no
   A1 slot and must read B1.  Nibble order per VDP2_CYCxxL: T0=hi..T3=lo (matches the 0xFEEE scrub).
   Written to the shadow (so the _BlankIn re-push stays coherent) AND straight to the chip. */
static void sky_cell_force_cyc(int sky_on, int nbg3_on)
{
    uint16_t b1l, b1u;
    if (sky_on && nbg3_on)  { b1l = 0x0443; b1u = 0x7EEE; }  /* NBG0 PN,char,char | NBG3 PN,char */
    else if (sky_on)        { b1l = 0x044E; b1u = 0xEEEE; }  /* NBG0 PN,char,char                */
    else if (nbg3_on)       { b1l = 0x37EE; b1u = 0xEEEE; }  /* NBG3 PN,char                     */
    else                    { b1l = 0xEEEE; b1u = 0xEEEE; }  /* nothing in B1                    */
    VDP2_CYCA0L = 0xEEEE; VDP2_CYCA0U = 0xEEEE;   /* park: rotation coeff via RDBS */
    VDP2_CYCA1L = 0xEEEE; VDP2_CYCA1U = 0xEEEE;   /* park: rotation char  via RDBS */
    VDP2_CYCB0L = 0x55EE; VDP2_CYCB0U = 0xEEEE;   /* NBG1 framebuffer (two 8bpp char) */
    VDP2_CYCB1L = b1l;    VDP2_CYCB1U = b1u;
    volatile uint16_t *const c = (volatile uint16_t *)0x25F80010;  /* chip CYCA0L..CYCB1U = 0x10..0x1E */
    c[0] = 0xEEEE; c[1] = 0xEEEE;  c[2] = 0xEEEE; c[3] = 0xEEEE;   /* CYCA0L/U, CYCA1L/U */
    c[4] = 0x55EE; c[5] = 0xEEEE;  c[6] = b1l;    c[7] = b1u;      /* CYCB0L/U, CYCB1L/U */
}
#endif

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
#if VDP2_CELL_SKY
    sky_cell_init();   /* NBG0 = 256-color cell sky in B1 (coexists with the RBG0 bitmap floor) */
#endif
#if SKY_DEBUG_SHOW
    slPriorityNbg0(6); slPriorityNbg1(5);   /* sky ON TOP to verify Stage A */
#else
    /* LAYER INVERSION: software (NBG1) ON TOP with Doom's correct occlusion; the VDP1
       walls render BELOW NBG1, filling the index-0 (transparent) wall gaps NBG1 leaves
       where the software wall draw is skipped.  NBG3 debug = 7 (top).
       NBG1 game = 6  >  every sprite priority = 5  >  NBG0 sky = 4 (3 with RBG0). */
#if VDP2_RBG0_TEST
    slPriorityNbg0(VDP2_SKY_OCCL_DIAG ? 4 : 3); slPriorityNbg1(6);   /* sky 3 below floor(4); DIAG: sky 4 above floor(3) */
#else
    slPriorityNbg0(4); slPriorityNbg1(6);
#endif
    /* SATURN sprites-on-VDP1 study (2026-07-05): FOUNDATION probe.  The whole "sprites can't
       go on VDP1" claim rested on VDP1 sitting BELOW NBG1 (prio 5 < 6).  But VDP1 sprite
       priority is PER-COMMAND (8 slPrioritySpr registers, selected by each sprite's priority
       bits), so it is NOT fixed.  Build with SAT_SPR_PRIO7_TEST=1 to raise the WHOLE VDP1
       sprite layer to 7 (ABOVE NBG1=6): the textured VDP1 walls/floors should then composite
       ON TOP of the software framebuffer.  If they do -> VDP1-above-NBG1 is real -> the
       weapon/things-at-prio-7 path is worth building (per-command split: things set their
       priority bits -> a reg pinned to 7, walls keep bits clear -> reg 0 = 5).  Deliberately
       "wrong-looking" (walls over everything) -- it is a yes/no mechanism proof, not the final
       layering.  Default 0 = shipping behaviour byte-identical. */
#ifndef SAT_SPR_PRIO7_TEST
#define SAT_SPR_PRIO7_TEST 0
#endif
#ifndef SAT_WPN_VDP1
#define SAT_WPN_VDP1 1   /* player weapon on the VDP1 prio-7 sprite layer (default ON).  Software-wall
                            split falls back to the software weapon (r_things.c R_DrawMasked gate). */
#endif
#ifndef SAT_WORLD_THINGS_VDP1
#define SAT_WORLD_THINGS_VDP1 1  /* DE-RISK PROBE (default ON): world sprites on the VDP1 prio-7 layer to
                                    offload the ~6-13ms masked FILL off the SH-2s.  Emitted at the post-BSP
                                    kick (1p only), non-occlusion-clipped (viewport/system-clip only) -- so
                                    a nearer wall does not yet hide a farther thing (FUNC_UserClip is the
                                    follow-up).  M0 keeps software sprites (sat_wall_skip gate) = A/B ref.
                                    Reads: overlay 'th e/d' (emitted/declined), Dr%/fps for the headroom
                                    question, and whether the boxes track the monsters + walls survive. */
#endif
#if SAT_WPN_VDP1
    /* PER-COMMAND SPLIT: register 0 = 5 (below NBG1) is what walls/floors select -- their
       framebuffer values are CRAM addresses <=2047, so the priority-select bits are CLEAR ->
       register 0.  registers 1..7 = 7 (above NBG1) -- anything that SETS a priority bit lands
       here.  So a command that ORs a priority bit into its CMDCOLR jumps above NBG1 while the
       walls/floors stay below.  This is the sprite-vs-world layer split. */
    slPrioritySpr0(5); slPrioritySpr1(7); slPrioritySpr2(7); slPrioritySpr3(7);
    slPrioritySpr4(7); slPrioritySpr5(7); slPrioritySpr6(7); slPrioritySpr7(7);
#elif SAT_SPR_PRIO7_TEST
    slPrioritySpr0(7); slPrioritySpr1(7); slPrioritySpr2(7); slPrioritySpr3(7);
    slPrioritySpr4(7); slPrioritySpr5(7); slPrioritySpr6(7); slPrioritySpr7(7);
#else
    slPrioritySpr0(5); slPrioritySpr1(5); slPrioritySpr2(5); slPrioritySpr3(5);
    slPrioritySpr4(5); slPrioritySpr5(5); slPrioritySpr6(5); slPrioritySpr7(5);
#endif
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
    slScrAutoDisp((VDP2_CELL_SKY ? NBG0ON : 0) | NBG1ON | (RBG0_DISPLAY ? RBG0ON : 0) | (RBG0_NBG3 ? NBG3ON : 0));  /* +cell sky(NBG0): SGL authors NBG0's B1 cycle into the shadow before the block-flush. floor(RBG0)+NBG3 */
#endif
#else
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON);
#endif

#if VDP2_RBG0_TEST
    /* Commit the RBG0 bank assignment (RDBS) straight to the chip -- the piece SGL would
       push inside slSynch.  After slScrAutoDisp so RBG0ON is already live; once is enough
       (the SGL vblank handler re-pushes BGON/scroll, not RAMCTL). */
#if RBG0_FLOOR_WINDOW
    /* Clip the RBG0 floor's DISPLAY to BELOW the horizon via VDP2 window W1, so a torn VDP1 wall gap
       above the horizon shows the backdrop/sky instead of the floor bleeding through.  W1 (not W0)
       leaves W0 free for the line-color CCAL window.  Set here -- WPSx1 (0xC8-0xCE) + WCTLC ride
       rbg0_commit_cyc's 0x0E..0xFE block-flush below, and the per-vblank ISR (0x00..0x8E) never
       touches them -> persists with NO slSynch (same proof as the CCAL line-window above).  MOVED at
       runtime by rbg0_floor_window_apply() (called from sky_cell_build_map) so the window tracks the
       live, pad-tunable sky horizon. */
    rbg0_floor_window_apply(SKY_HORIZON_ROW);
#endif
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
    sat_vdp2_sky = (VDP2_HW_SKY || VDP2_CELL_SKY);   /* 1 = HW sky: core leaves the sky region index-0 so NBG0 shows through NBG1 */
#if VDP2_RBG0_TEST
    /* Floor on RBG0 at boot (rbg0_mode 0); pad Y cycles the 3 RBG0/debug modes. */
    sat_vdp2_floor = 1;
    sat_vdp2_floor_dominant = RBG0_FLOOR_DOMINANT;   /* HW floor pick: dominant visible flat (sector-change) vs under-eye */
#endif
    sat_apply_mode();   /* boot the render mode M (owns sat_wall_skip/sat_vdp1_floor/sky/SQ); pad Z cycles it */

    /* LAYER INVERSION: the weapon + HUD now render in SOFTWARE (NBG1, on top) -- do NOT
       route them to VDP1.  VDP1 carries ONLY the walls, BELOW NBG1.  (The VDP1 weapon/
       HUD path is left in the file but unhooked.) */

#if VDP1_WALL_TEST
    /* Route one-sided (solid) walls to the VDP1 world renderer AND skip their
       software column draw -> see the VDP1 coverage + the perf it buys back. */
    sat_wall_hook = sat_wall_vdp1;
    /* sat_wall_skip is owned by sat_apply_mode (M-gated: 1 in M1..M4, 0 in M0 software walls). */
    /* kick VDP1 right after the BSP walk (parallel with the CPU floors/sprites) so the
       walls present the SAME frame as the framebuffer (no 1-frame lag / sky-at-the-seam). */
    sat_walls_done_hook = sat_walls_kick;
#if SAT_WPN_VDP1
    /* Route the player weapon to VDP1 at prio 7: the core R_DrawPSprite calls sat_psprite_hook
       (opaque case) instead of the software fill, and sat_psprite_early makes the platform draw
       it EARLY (in sat_walls_kick, before the end-of-planes present) so it lands this frame. */
    sat_psprite_hook  = sat_vdp1_wpn_draw;
    sat_psprite_begin = sat_vdp1_wpn_clip;   /* clip the weapon to its view (no HUD poke / no split spill) */
    sat_psprite_early = 1;
#endif
#if SAT_WORLD_THINGS_VDP1
    /* Route the world sprites to VDP1 prio 7 (offload the masked FILL).  core R_EmitWorldThingsVDP1
       (called from the post-BSP kick) computes each screen rect and calls this hook. */
    sat_thing_hook = sat_vdp1_thing_draw;   /* sat_thing_cap set in vdp1_wpn_init (THINGS_TEX_SLOTS in scope there) */
#endif
#endif

#if VDP1_FLOOR_TEST
    /* inc-1: register the floor-skip hook (claims secondary floors/ceilings).  Left OFF
       (sat_vdp1_floor = 0) so the boot render is normal software floors; pad Y toggles it
       live to A/B the index-0 coverage (owned surfaces go black until the strip emitter). */
#if SAT_VDP1_FLOOR
    sat_floor_vdp1_hook = sat_floor_vdp1_emit;   /* Option-1 first cut: secondary floors -> VDP1 flat quads */
    /* sat_vdp1_floor (consult the hook) is owned by sat_apply_mode (M-gated: 1 in M1..M3). */
#if SAT_FLOOR_TEX
    sat_floors_done_hook = sat_vdp1_floors_done;  /* build+flip the F floor bank right after
                                                     R_DrawPlanes (same frame as walls+mask) */
    sat_floor_punch_edge = ftex_punch_edge_buf;   /* arm the mode-3 partial-claim column edges */
    sat_plane_border_max = 10;   /* legacy-path cap, kept as the R+Left/Right knob + row 8 bM */
    sat_plane_fill_mode  = 2;    /* SWEPT decrochage fill (owner's red-band design): the CPU paints
                                    ONLY the per-column band between the plane's current span and
                                    its claimed region sat_plane_lag frames ago -- no more uniform
                                    perimeter blanketing small textured planes during a turn.
                                    2 = the band is drawn with the REAL flat texels (R_MapPlane
                                    mapping, per-row zlight) -> the cover itself is textured and
                                    essentially invisible; 1 = flat potato band (A/B); 0 legacy. */
#endif
#else
    sat_floor_vdp1_hook = sat_floor_vdp1_stub;
    sat_vdp1_floor      = 0;
#endif
#endif

    SRL::Debug::Print(0, 1, "INIT DOOM...");
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

#if VDP2_CELL_SKY
/* Hardware sky as a 256-color NBG0 CELL layer in bank B1 (coexists with the RBG0 bitmap floor;
   A0/A1/B0 untouched).  The 256x128 Doom sky becomes a 32x16 grid of 8x8 cells (SKY_NB_CELL),
   tiled 2x horizontally across the 512px page so the viewangle scroll wraps seamlessly; map rows
   below the sky reference a single near-black filler cell (index SKY_NB_CELL) -- never seen (NBG1
   is opaque there).  Cell index = col*16 + row (column-major, like Jo's __jo_create_map); char# =
   cellidx*2 (a 256-color 8x8 cell = 2 of the 32-byte char units); palette bank 1 (PLAYPAL) via the
   PN palette field (paloff 0x1000).  Cells start at bank B1's base -> char-base offset 0.  The B1
   VRAM alias (0x25E6xxxx) is uncached, so the cell writes reach VRAM with no slCashPurge. */
static void sky_cell_upload(void)
{
    unsigned char *cells = (unsigned char *)SKY_CEL_VRAM;
    unsigned char  nb    = (unsigned char)sat_near_black();
    for (int ccol = 0; ccol < 32; ++ccol)
        for (int rx = 0; rx < 8; ++rx)
        {
            const unsigned char *src = R_GetColumn(skytexture, ccol * 8 + rx);  /* 128-tall column */
            for (int crow = 0; crow < 16; ++crow)
                for (int ry = 0; ry < 8; ++ry)
                {
                    unsigned char p = src[crow * 8 + ry];
                    if (!p) p = nb;                /* keep the sky OPAQUE (0 = VDP2 transparent code) */
                    cells[(ccol * 16 + crow) * 64 + ry * 8 + rx] = p;
                }
        }
    memset(cells + SKY_NB_CELL * 64, 0, 64);        /* TRANSPARENT filler (index 0): floor shows below the horizon */
    sky_loaded_tex = skytexture;
}

/* Build the NBG0 sky PN map: sky cells ABOVE the horizon (sky_horizon_row), transparent filler at/
   below it.  Map (B1) is uncached -> writes land with no purge; cheap (4096 entries) so it can be
   rebuilt live when the pad nudges sky_horizon_row. */
static void sky_cell_build_map(void)
{
    unsigned short *map = (unsigned short *)SKY_MAP_VRAM;
    int thresh = sky_horizon_row >> 3;   /* cell-row boundary (8px cells) */
    for (int my = 0; my < 64; ++my)
        for (int mx = 0; mx < 64; ++mx)
        {
            int cellidx = (my < thresh) ? ((mx & 31) * 16 + my) : SKY_NB_CELL;  /* sky above the horizon; transparent filler at/below it */
            map[my * 64 + mx] = (unsigned short)((cellidx * 2) | 0x1000);       /* char#=idx*2, palette bank 1 */
        }
#if RBG0_FLOOR_WINDOW
    rbg0_floor_window_apply(sky_horizon_row & ~7);   /* window snapped to the sky's 8px cell boundary -> sky+floor meet exactly (no decalage) */
#endif
}

/* One-shot NBG0 cell config + PN map (cells change per level; map rebuilds on a live horizon tune). */
static void sky_cell_init(void)
{
    memset((void *)SKY_CEL_VRAM, 0, (SKY_NB_CELL + 1) * 64);
    memset((void *)SKY_MAP_VRAM, 0, 64 * 64 * 2);
    slPlaneNbg0(PL_SIZE_1x1);
    slCharNbg0(COL_TYPE_256, CHAR_SIZE_1x1);
    slMapNbg0(SKY_MAP_VRAM, SKY_MAP_VRAM, SKY_MAP_VRAM, SKY_MAP_VRAM);
    slPageNbg0(SKY_CEL_VRAM, 0, PNB_1WORD | CN_12BIT);
    sky_cell_build_map();
    slScrPosNbg0(toFIXED(0.0), toFIXED(0.0));
}
#endif

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
#if SAT_WPN_VDP1
/* SAT_WPN_VDP1: the weapon goes on VDP1 at prio 7 as an 8BPP palette sprite (half the RGB555
   size + carries the priority bit via CMDCOLR).  Its cache is RELOCATED off the LIVE wall pool
   (the old 0x25C45000 sat inside it -> would corrupt walls) to the last FOUR WTEX wide slots
   (WTEX_WIDE_N is cut 6->4 below) at 0x25C61000.
   Right-sized (measured): the LARGEST shareware weapon frame is SHTGD0 = 120x131 padded = 15720 B
   = 15.4KB at 8bpp, so a 16KB slot fits EVERY frame (a 32KB slot was 2x waste).  Slot COUNT is for
   tearing margin -- the textures are NOT double-buffered, so a MISS during the fast fire animation
   unpacks into a round-robin slot; need >= 2 x (textures/frame) so the displayed frame's slots
   survive the flip.  1p draws weapon+flash = 2/frame -> 4 slots.  So 4 x 16KB = 64KB (was 128KB):
   reclaims only 2 wide wall slots (WTEX_WIDE_N 6->4, not 6->2).  (2p needs 8 slots + half-res split
   weapon = follow-up.)  NOTE: the "weapon flickers/misses a frame" bug is NOT a size skip (every
   frame fits 16KB) -- it is the weapon at the TAIL of the VDP1 list being dropped on a HW plot
   overrun; fixed by emitting the weapon FIRST (see sat_walls_kick). */
#define WPN_TEX_BASE   0x25C61000u        /* reclaimed 2x32KB wide slots (see WTEX_WIDE_N) -> 4x16KB */
#define WPN_TEX_SLOTSZ 0x4000u            /* 16 KB @ 8bpp -> fits the 15.4KB max frame */
#define WPN_TEX_SLOTS  4
#define WPN_CMDCOLR    (0x2000u | 0x0100u)/* pr bit13 -> register 1 (=7, above NBG1) | CRAM bank 1
                                             (full-bright PLAYPAL; texel = the light-shaded index) */
#else
#define WPN_TEX_BASE   0x25C45000u
#define WPN_TEX_SLOTSZ 0xB000u            /* 44 KB -> up to ~160x140 padded */
#define WPN_TEX_SLOTS  4
#endif
static struct { int lump; const unsigned char *cmap; int padW; int H; }
                    wpn_cache[WPN_TEX_SLOTS];
static int          wpn_cache_rr;

static volatile int vd1_dr_live;   /* 1 = a world list is linked -> the vblank Dr sampler counts */
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

#if SAT_WORLD_THINGS_VDP1
/* World-things-on-VDP1: 8bpp texture pool in the free 28KB gap between the weapon cache (ends
   0x25C71000) and the HUD sprite (0x25C78000) -- touches neither the wall pool nor the weapon.
   TEARING FIX: the pool is DOUBLE-BUFFERED by frame parity (= vdp1_wbank).  The VDP1 coherent-pair
   present keeps replotting the OLD pair (parity ^1) every vblank until the new pair flips in; if a
   thing texture is re-baked in place while the old pair still references it, the sprite tears
   across its width.  So THIS frame bakes only into parity[vdp1_wbank]; the displayed pair's
   textures (the other parity) are never touched -> tear-free.
   TEAR-SAFE CACHE (keyed by lump+cmap; flip is geometric = quad corners, not baked): a slot in the
   write-bank parity keeps its baked texture ACROSS that parity's frames, so a sprite that stays on
   screen is baked ONCE (then reused) instead of re-baked every frame.  A stable set costs 0 bakes
   after the first 2 frames (one per parity); only NEW keys (a monster turns/animates, a fresh
   monster) bake, and only into the non-displayed parity -> still tear-free.  Two identical sprites
   (same lump+cmap) share one slot for free.  The pool is written ONLY here, so a key-match always
   means the VRAM holds that key's texture (survives M0/M6 excursions -> no invalidation needed).
   PER-FRAME GUARD: a slot claimed by an already-emitted command this frame must not be re-baked for
   a different key this frame (would clobber a texture the current list points at) -> `used` bit,
   reset per frame; if every slot is used, decline.  Full-patch bakes are distance-independent, so 4
   slots x 3584B fit any shareware monster.  The core GRANTS the 4 slots to the 4 distinct TEXTURES
   with the largest sprite (above a %-of-view floor) and offloads every sprite using a granted
   texture, so a same-type horde shares slots and offloads wholesale; the rest stay software.  4 x
   3584 x 2 parity = 28KB (the whole gap).  More distinct textures => cede wall-pool VRAM to raise
   THINGS_TEX_SLOTS.  'th' counts emitted/declined; 'fb' baked (misses), 'sb' session bake%. */
#define THINGS_TEX_BASE   0x25C71000u
#define THINGS_TEX_SLOTS  4                /* slots PER parity == max distinct things offloaded/frame (VRAM cap) */
#define THINGS_TEX_SLOTSZ 0x0E00u          /* 3584 B -> fits any shareware monster frame @ 8bpp */
static struct { int lump; const unsigned char *cmap; unsigned char used; unsigned int lru; }
                    thing_cache[2][THINGS_TEX_SLOTS];   /* [parity][slot]: key + per-frame used bit + LRU tick */
static unsigned int thing_lru_tick;        /* monotonic use counter -> evict the smallest (oldest) lru */
/* (sat_things_n / sat_things_decl / thing_bake_n are defined earlier, before the overlay block) */
#endif

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
#define WTEX_NARROW_N  15   /* 16 -> 15 (2026-07-03): one narrow slot (16KB) ceded to the floor-texture
                               tail (bigger F banks + mip storage).  Walls historically ran on 8 narrow
                               slots; watch the wtex_bakes (`bk`) counter if wall re-bakes climb. */
#define WTEX_NARROW_SZ 0x4000u                                      /* 16KB -> 128x128 @ 8bpp */
#if SAT_WPN_VDP1
#define WTEX_WIDE_N    4   /* SAT_WPN_VDP1: cede the last 2 wide slots (64KB) to the 4-slot 16KB VDP1
                              weapon cache (WPN_TEX_BASE=0x25C61000).  Watch the wtex_bakes 'bk'
                              counter: 4 wide-wall slots remain (was 6). */
#else
#define WTEX_WIDE_N    6
#endif
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
   walls instead (they vanish).  Stays <= the ~248 budget (a 480 would break that).  SET TO 160 (not
   240): on the current core 240->3520B / 248->3168B TLSF pool BOOT-LOOP, 160->~7KB BOOTS (the
   tlsf-create floor rose to ~3.5KB+ with the rbg0-rework SRL init allocs -- see boot-loop memory).
   160 also leaves headroom for the Tier-1 span-steal spanjobs[] .bss.  SOFT consequence: 1p never
   hits the cap (identical to 240); only the densest rooms / 4-way split bump a few far walls -- and
   per below those go to CPU SOFTWARE (graceful, never a missing wall; the old 128 "clipping" predates
   this fallback).  A per-view SOFT cap in the hook (below) reserves the upper half for the right view
   so a dense LEFT view -- accumulated first -- can't hog every VDP1 slot.  When the cap is hit the
   hook REJECTS the wall and the core renders it in SOFTWARE (no sky) -- so the cap is also the
   VDP1->CPU starvation handoff. */
#define WALL_ACC_MAX 128   /* restored to 128: lumpinfo moved to the LWRAM Doom zone + HEAP_SIZE trimmed 88->32KB (syscalls.c) freed ~56KB to the TLSF pool, so the 3p-minimap pool pressure is gone and full wall capacity is back. 1p peaks ~57 << 128. */
#define WALL_PX_BUDGET 200000 /* accumulated wall fill (screen px) beyond which further walls are
                                 REJECTED to the software fallback -- BSP visits near-first, so
                                 the sacrificed walls are the FARTHEST (owner: "les plus eloignes
                                 drop au CPU").  SAFETY NET ONLY: at 90k it starved corridor
                                 scenes to ~5 VDP1 walls and Bp exploded to 25-58ms (owner
                                 overlays 2026-07-03); 200k (~3x screen) only bites pathological
                                 overdraw.  FBK `s` counts the rejects. */
static int wall_px_acc;    /* fill claimed so far this frame (reset with wall_acc_n) */
/* vx/vxr = the view's framebuffer x-range [vx, vxr] this wall belongs to (split-screen: 0..159 for
   the left view, 160..319 for the right).  x1/x2 are stored ALREADY offset by viewwindowx, so the
   emit works in absolute framebuffer coords; vx/vxr drive the per-view user-clip window. */
static struct { short x1, yl1, yh1, x2, yl2, yh2, slot, v0, v1, vx, vxr, vyt, vyb; int texnum, u1, u2;
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
    extern int viewwindowx, viewwidth, viewwindowy, viewheight;   /* core: per-view origin + size (R_SetViewWindow) */
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
    {   /* fill budget: past it, the remaining (FARTHEST -- BSP is near-first) walls go to the
           software fallback instead of overloading the plot until it overruns the vblank */
        int aw = (x2 - x1 + 1) << detailshift;
        int h1 = yh1 - yl1 + 1; if (h1 < 0) h1 = 0;
        int h2 = yh2 - yl2 + 1; if (h2 < 0) h2 = 0;
        int a  = aw * ((h1 + h2) >> 1);
        if (wall_px_acc + a > WALL_PX_BUDGET) return 1;   /* -> CPU */
        wall_px_acc += a;
    }
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
    wall_acc[i].vyt = (short)vy;                        /* this view's framebuffer y-band [vy, vy+h-1] -- */
    wall_acc[i].vyb = (short)(vy + viewheight - 1);     /* clips the VDP1 walls vertically (3/4p quadrants) */
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

/* Flat quad screen-y clamp: a flat fill has NO texture, so clamping its geometry is FREE (no v ->
   no swim) and bounds the VDP1 fill.  The clamp band is now THIS VIEW's [vyt, vyb] (read per-wall
   from wall_acc), not the full screen -- see wall_emit_flat (fixes the 3/4p vertical bleed).
   (Too-close TEXTURED walls are handled upstream by the core CPU fallback, not here.) */

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
                           int vx, int vxr, int vyt, int vyb)
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
                cmd[6]  = (short)wx1;  cmd[7]  = (short)vyt;  /* upper-left  (XA,YA) = view band top   */
                cmd[10] = (short)wx2;  cmd[11] = (short)vyb;  /* lower-right (XC,YC) = view band bottom */
                vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
                winset = 1;
                if (vdp1_wnext >= WALL_CMD_CAP) break;
            }
            /* Phase-1 vertical [0,223] clamp REMOVED (owner's red diagnosis 2026-07-02).  With the
               SPAN routing reverted, a wall that projects off the TOP of the screen renders CORRECTLY
               as a FULL quad clipped by the VDP1 system clip: DISTORSP maps the texels across the whole
               (partly off-screen) projection, so the on-screen part shows the right texels -- no squish,
               no black.  The old clamp trimmed the texel with a SINGLE cut from one corner, which on a
               sloped-top tile (yls != yle) squished the texture AND left a black wedge under the clamped
               edge at the wall/screen-edge.  Full-quad + system-clip is the fix (overdraw = idle fill). */
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
            /* SATURN: this squish fallback has NO user-clip (CMDPMOD 0x00E0), so -- like the flat
               path -- clamp its y to THIS view's band, else a grazing near wall (sharp-angle
               pillar) bleeds vertically into the quadrant above/below (3/4p P1->P3, P2->P4). */
            if (cyls < vyt) cyls = vyt; else if (cyls > vyb) cyls = vyb;
            if (cyle < vyt) cyle = vyt; else if (cyle > vyb) cyle = vyb;
            if (chys < vyt) chys = vyt; else if (chys > vyb) chys = vyb;
            if (chye < vyt) chye = vyt; else if (chye > vyb) chye = vyb;
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
    int vyt = wall_acc[wi].vyt, vyb = wall_acc[wi].vyb; /* and y-band (split-screen vertical clip)      */
    int u1 = wall_acc[wi].u1, u2 = wall_acc[wi].u2;
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int v0 = wall_acc[wi].v0, v1 = wall_acc[wi].v1, vspan = v1 - v0;
    unsigned short colr = wall_light_colr(wall_acc[wi].cmap);  /* per-wall light = CRAM bank */

    if (H <= 0 || vspan <= 0)                          /* no valid v-range -> whole texture once */
    {
        int th = (H > 255) ? 255 : (H > 0 ? H : 1);
        unsigned short ca = (unsigned short)((base - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | th);
        wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
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
        wall_emit_band(x1, x2, yl1b, yh1b, yl2b, yh2b, u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
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
    int vyt = wall_acc[wi].vyt, vyb = wall_acc[wi].vyb;
    /* clamp the flat quad to THIS VIEW's y-band (was the full screen 0..223) -- FREE for a flat
       fill (no texture = no swim), bounds the VDP1 fill for a tall/near wall, AND stops a near
       wall in one quadrant from filling down into the view below it (the 3/4p vertical bleed,
       P1->P3 / P2->P4).  The layer inversion hides any silhouette overspill. */
    if (yl1 < vyt) yl1 = vyt; else if (yl1 > vyb) yl1 = vyb;
    if (yl2 < vyt) yl2 = vyt; else if (yl2 > vyb) yl2 = vyb;
    if (yh1 < vyt) yh1 = vyt; else if (yh1 > vyb) yh1 = vyb;
    if (yh2 < vyt) yh2 = vyt; else if (yh2 > vyb) yh2 = vyb;
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
    int vyt = wall_acc[wi].vyt, vyb = wall_acc[wi].vyb;
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
    wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
}

/* the VDP1 wall mode (0=textured 1=banded 2=flat).  Global per level (set in sat_apply_mode from
   SQ_wall); the flush forces flat for a wall with no texture slot, and forces textured for special walls. */
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
#if SAT_VDP1_FLOOR
/* ============================================================================================
   Option-1 FIRST CUT (see-the-potential): every SECONDARY (non-sky, non-dominant) visplane ->
   ONE flat-colour VDP1 quad, emitted BELOW the walls -> drains the software span phase P.  The
   R_DrawPlanes hook accumulates during the frame; vdp1_floors_flush emits before the walls. */
extern int firstflat; extern int *flattranslation; extern int detailshift; extern int viewz;
extern "C" int R_FlatPotatoColor(int lumpnum);
extern "C" int sat_vdp2_floor_pic;   /* dominant flat id (sat_vdp2_floor_h externed above) */
#if SAT_FLOOR_TEX
/* View state + light tables for the textured near-field + the distance-light bank.  All core C
   globals; still valid at flush time (sat_vdp1_wpn_begin runs after R_RenderPlayerView set them). */
extern "C" int            viewcos, viewsin;            /* fixed_t view basis (r_main.c) */
extern "C" int            centerxfrac, centeryfrac;    /* fixed_t screen centre */
extern "C" int            projection;                  /* fixed_t (= centerxfrac) */
extern "C" int            centerx, centery;            /* view cols/rows */
extern "C" int            viewwidth, viewheight;
extern "C" int            yslope[];                    /* fixed_t[SCREENHEIGHT] (r_plane.c) */
extern "C" int            extralight;
extern "C" unsigned char *fixedcolormap;               /* lighttable_t*: invuln/light-amp override */
extern "C" unsigned char *zlight[16][128];             /* [LIGHTLEVELS][MAXLIGHTZ] (r_main.h) */
extern "C" void          *W_CacheLumpNum(int lump, int tag);
#endif

#define MAX_FLOOR_ACC 80      /* floors emit first; cap leaves >=168 cmds for the walls (never starve a wall) */
#define SAT_CEIL_FILL 8       /* TOPIC B (owner's rotation-decrochage idea): px a CEILING flat is grown DOWN into
                                 the wall.  At rest the wall (painter-after, same VDP1 latency) covers it -> invisible.
                                 In rotation the wall (VDP1) lags the NBG1 mask, opening a sky gap at the junction; the
                                 grown ceiling colour reaches into that gap (revealed through the wall-skipped NBG1
                                 index-0) -> fills it with ceiling colour instead of sky.  No punch-dilate = no CPU-wall
                                 erase.  1 = off.  Caveat: on walls SHORTER than this it can bleed below the wall. */
#define SAT_FLOOR_HOVER 0     /* ABANDONED (owner 2026-07-02): growing the VDP1 quad past its silhouette put the
                                 fill on the VDP1 layer = the LAGGED latency, so it did NOT align with the mask
                                 (fixed nothing) and it leaked at rest (unmasked corners = two triangles).  The
                                 correct fill is on the SOFTWARE plane (NBG1 latency = aligned): see the plane-colour
                                 border in R_DrawPlanes, driven by sat_plane_border (r_main.c yaw history).  0 = off. */
#define FLOOR_STRIP_W 48      /* columns per strip: follow the silhouette (top/bottom) in vertical strips */
static struct floor_q { short x1, x2, ytl, ytr, ybl, ybr; unsigned short col;
#if SAT_FLOOR_TEX
                        short lump;              /* flat lump: texture cache key (step 2) */
                        int   hz;                /* height - viewz, signed fixed_t (light + projection) */
                        unsigned char ln, ceil;  /* zlight light row 0..15; 1 = ceiling */
                        unsigned char nsplit;    /* mode 3: 1 = near band tall enough for the CPU
                                                    split (FTEX_NEAR_MIN_ROWS); 0 = m2-style claim */
#endif
                      } floor_acc[MAX_FLOOR_ACC];
static int floor_acc_n = 0;
#if SAT_FLOOR_TEX
/* Slave F-build job header, FROZEN by the flush's master part (slot resolves + underlays) and
   consumed ONLY by ftex_slave_build on the slave -- so the kick/wjump/bank statics can move on
   with the next frame while a build is still in flight.  floor_acc entries themselves stay
   untouched until the next frame's claims, which JOIN the job first (RP_AuxWait). */
static struct { unsigned int base, wjump; unsigned short wroot; short n; short overlap;
                unsigned char idx[MAX_FLOOR_ACC]; short slot[MAX_FLOOR_ACC]; } ftex_job;
static int ftex_job_ready = 0;   /* master part done -> DG_DrawFrame dispatches the slave build */
static int ftex_build_mode = 0;  /* fmode snapshot for the emitters (the pad can move mid-frame) */
static void ftex_slave_build(void);   /* fwd: armed from the flush (RP_AuxArm) */

/* ADAPTIVE-m3 texture gate (owner 2026-07-03: "les lumieres rouges du plafond, trop
   visible"): the m2-style shallow-band claim shows the FLAT underlay wherever the
   iteration guard skips a magnified tile -- invisible on a CALM flat, glaring on a BUSY
   one (light grids, nukage).  Not a special-lump property: measured as the mean palette
   RGB distance of the texels to the flat's dominant colour, computed ONCE per flat
   (lazy; the lump set is WAD-constant, so the table survives level changes). */
#define FTEX_BUSY_THRESH 60           /* mean |dR|+|dG|+|dB| to dominant (0..765): above = busy */
static unsigned char ftex_busy_tab[256];   /* [lump-firstflat]: 0 unknown / 1 calm / 2 busy */
static int ftex_flat_busy(int lump)
{
    int idx = lump - firstflat;
    if (idx < 0 || idx >= (int)sizeof ftex_busy_tab) return 0;
    if (!ftex_busy_tab[idx])
    {
        const unsigned char *src = (const unsigned char *)W_CacheLumpNum(lump, 8 /* PU_CACHE */);
        if (!src) return 0;
        int dom = R_FlatPotatoColor(lump) & 0xFF;
        int dr = colors[dom].r, dg = colors[dom].g, db = colors[dom].b;
        long acc = 0;
        for (int i = 0; i < 64 * 64; i += 16)          /* 256 spread samples */
        {
            int c = src[i];
            int d = colors[c].r - dr; acc += d < 0 ? -d : d;
            d = colors[c].g - dg;     acc += d < 0 ? -d : d;
            d = colors[c].b - db;     acc += d < 0 ? -d : d;
        }
        ftex_busy_tab[idx] = (unsigned char)((acc >> 8) > FTEX_BUSY_THRESH ? 2 : 1);
    }
    return ftex_busy_tab[idx] == 2;
}
#endif
#if SAT_FLOOR_TEX
/* View-state SNAPSHOT taken when the frame's first trapezoid is accumulated (in R_DrawPlanes).
   The flush runs at the NEXT wall kick -- sat_walls_done_hook fires right after the BSP walk,
   BEFORE R_DrawPlanes fills floor_acc -- so floor_acc always holds the PREVIOUS frame's
   silhouettes.  Projecting tiles with the LIVE view globals mixed two frames' transforms
   (mis-clipped, shimmering tiles in motion -- audit wf_82497c03).  All tile math uses this
   snapshot so silhouettes, projection and light agree frame-wise; the remaining 1-frame lag
   vs the NBG1 mask is the same class as the walls' and is covered by the decrochage border. */
static int ftex_vx, ftex_vy, ftex_vcos, ftex_vsin;    /* viewx/y, viewcos/viewsin        */
static int ftex_cxf, ftex_cyf, ftex_proj;             /* centerxfrac/centeryfrac, projection */
static int ftex_ctx, ftex_cty;                        /* centerx, centery                */
static const unsigned char *ftex_fcmap;               /* fixedcolormap                   */

static inline int fxmul(int a, int b) { return (int)(((long long)a * b) >> 16); }
/* SATURN PERF: the ftex per-node perspective divide is the tile-fabrication bottleneck
   (HW mode-bench: P(textured) ~= 2x P(software) -- the cost is emitting tiles, not drawing
   them).  Route it through the core's SH-2 hardware DIV0U divide (m_fixed.h, ~37 cyc) instead
   of GCC's __divdi3 software 64/64 (~200 cyc).  BIT-IDENTICAL output: both ftex call sites are
   inside FixedDiv's non-saturating domain -- at scale=fxdiv(proj,tz) the tz<FTEX_ZGUARD(8<<16)
   guard above ensures tz>=524288 while abs(proj)>>14~=640; at the bbox site num>>14 (<=~704)
   << den(=proj).  So the saturation branch never trips and (a<<16)/b matches exactly. */
static inline int fxdiv(int a, int b) { return (int)FixedDiv(a, b); }

#define FTEX_CHUNK     40           /* max fb px per clip-rect chunk (see ftex_chunk_for) */
#define FTEX_WEDGE_MAX 6            /* px the sloped-chord wedge may reach within one chunk */

/* chunk width for this trapezoid, ADAPTIVE: narrow enough that the chord rise inside one
   chunk stays <= FTEX_WEDGE_MAX px (steep junction lines get narrow chunks -> the flat
   wedge triangles the owner flagged shrink everywhere).  Shared by the tile emitter and
   the mode-3 punch filler so the punched band and the tiles' UserClips agree BY
   CONSTRUCTION -- keep both call sites formula-identical. */
static int ftex_chunk_for(const struct floor_q *f)
{
    int w = f->x2 - f->x1; if (w < 1) return FTEX_CHUNK;
    int dT = f->ytr - f->ytl; if (dT < 0) dT = -dT;
    int dB = f->ybr - f->ybl; if (dB < 0) dB = -dB;
    int dm = dT > dB ? dT : dB;                  /* max chord rise over the full width */
    if (dm <= FTEX_WEDGE_MAX) return FTEX_CHUNK;
    {
        int c = FTEX_WEDGE_MAX * w / dm;
        if (c > FTEX_CHUNK) c = FTEX_CHUNK; else if (c < 8) c = 8;
        return c;
    }
}

/* mode 3 (partial claim): write this trapezoid's per-column punch edge = the SAME chunk
   interior the tile emitter will clip to (MUST mirror ftex_emit_trapezoid's math).  For a
   floor the edge is the chunk's interior TOP (punch [edge..bottom], software above); for a
   ceiling its interior BOTTOM (punch [top..edge], software below) -- so the far field AND
   the far-side wedge triangles render as real software texels. */
static void ftex_fill_punch(const struct floor_q *f)
{
    int ds = detailshift;
    int ph = f->hz < 0 ? -f->hz : f->hz;
    if (ph == 0) return;
    int cyTa = 0, cyBa = viewheight - 1;
    int W2 = (viewwidth << ds) >> 1;
    int mrows = (int)((((long long)ph * W2) / FTEX_MIPDIST) >> 16);
    if (mrows < 1) mrows = 1;
    if (!f->ceil) { int ymip = ftex_cty + mrows; if (cyTa < ymip) cyTa = ymip; }
    else          { int ymip = ftex_cty - mrows; if (cyBa > ymip) cyBa = ymip; }
    if (cyTa < 0) cyTa = 0;
    if (cyBa > viewheight - 1) cyBa = viewheight - 1;
    if (cyTa >= cyBa) return;                    /* fully beyond MIPDIST -> stays all-software */
    int w = f->x2 - f->x1;
    if (w < 1) return;
    int chunk = ftex_chunk_for(f);
    /* NEAR boundary (FTEX_CPU_NEARDIST): chunks lying entirely nearer go to the CPU spans --
       magnified tiles are pure VDP1 iteration waste there (and the per-column punch_nrow
       already routes those rows to the spans on straddling chunks). */
    int dyn = (int)(((long long)ph * W2 / FTEX_CPU_NEARDIST) >> 16); if (dyn < 1) dyn = 1;
    int prevT = f->ytl, prevB = f->ybl;
    for (int xa = f->x1; xa < f->x2; )
    {
        int xb = xa + chunk; if (xb > f->x2) xb = f->x2;
        int tb2 = f->ytl + (int)((long)(f->ytr - f->ytl) * (xb - f->x1) / w);
        int bb2 = f->ybl + (int)((long)(f->ybr - f->ybl) * (xb - f->x1) / w);
        int cT = prevT > tb2 ? prevT : tb2;
        int cB = prevB < bb2 ? prevB : bb2;
        if (cT < cyTa) cT = cyTa;
        if (cB > cyBa) cB = cyBa;
        if (cT < cB)
        {
            int nearskip = f->nsplit
                ? (f->ceil ? (cB <= ftex_cty - dyn) : (cT >= ftex_cty + dyn))
                : 0;   /* shallow band (nsplit 0): VDP1 serves the whole depth, punch it all */
            if (!nearskip)
            {
                short e = f->ceil ? (short)cB : (short)cT;
                for (int col = xa >> ds; col <= (xb >> ds) && col < 320; ++col)
                    ftex_punch_edge_buf[col] = e;
            }
        }
        prevT = tb2; prevB = bb2; xa = xb;
    }
}
#endif

extern "C" int sat_floor_vdp1_emit(int picnum, int height, int minx, int maxx,
                                   const unsigned char *top, const unsigned char *bottom, int lightlevel)
{
    if (height == sat_vdp2_floor_h && picnum == sat_vdp2_floor_pic) return 0;   /* dominant stays (RBG0/software) */
    if (maxx < minx) return 0;
    /* SATURN M: per-surface VDP1 claim gate.  M2 = leftover floors only (ceilings -> software);
       M3 = ceilings only (leftover floors -> software); M1 = both (gate is a no-op).  A rejected
       surface returns 0 so the core draws it with the software span path at its SQ quality. */
    if ((height > viewz) ? !sat_vdp1_ceil_claim : !sat_vdp1_floor_claim) return 0;
#if SAT_FLOOR_TEX
    int claim_add = 0;                   /* claim-budget charge, applied ONLY on a successful claim */
    int fdnear = 0;                      /* |row - centery| of the plane's nearest edge (mode-3 nsplit) */
    if (sat_ftex_mode == 4) return 0;    /* mode 4 SOFTWARE: every non-dominant plane -> CPU spans */
    if (sat_ftex_mode == 5) return 3;    /* mode 5: RBG0 dominant + SOFTWARE planes + VDP1 walls;
                                            the core paints the textured wall-lag catch-up band
                                            (rows the plane held sat_plane_lag frames ago, now
                                            wall-punched) -- no VDP1 floor list at all */
    if (sat_ftex_mode == 2 || sat_ftex_mode == 3 || sat_ftex_mode == 6)
    {
        /* SAFE ZONE (owner 2026-07-03): a plane hugging the viewer -- door threshold, shallow
           step (small planeheight) -- has a big on-screen share NEARER than FTEX_NEARCLIP,
           where world tiles cannot go (projection blow-up).  Claiming it freezes that share
           as flat underlay with no CPU fallback ("on affiche presque rien, meme en mode 3").
           If the too-near share exceeds ~30% of the plane's rows, hand the WHOLE plane to
           the software spans.  Mode 3 also bails when the plane lies ENTIRELY beyond the
           textured band (nothing to tile). */
        int ph3 = height - viewz; if (ph3 < 0) ph3 = -ph3;
        int isc = (height > viewz);
        int dnear = 0, dfar = 0x7FFF;        /* |row - centery| of the nearest/farthest edges */
        int parea = 0;                       /* the plane's on-screen area (view px) */
        for (int xx = minx; xx <= maxx; ++xx)
        {
            if (top[xx] > bottom[xx]) continue;              /* occluded gap column */
            int en = isc ? (int)top[xx]    : (int)bottom[xx];   /* NEAR (eye-side) edge */
            int ef = isc ? (int)bottom[xx] : (int)top[xx];      /* FAR (horizon-side) edge */
            int dn = en - centery; if (dn < 0) dn = -dn;
            int df = ef - centery; if (df < 0) df = -df;
            if (dn > dnear) dnear = dn;
            if (df < dfar)  dfar = df;
            parea += (int)bottom[xx] - (int)top[xx] + 1;
        }
        if (dnear <= 0) return 0;                            /* nothing below/above horizon */
        fdnear = dnear;
        /* COST threshold (owner): a plane whose VDP1 cost estimate -- its screen area, the
           same currency as the tile budget -- exceeds FTEX_PLANE_CPU_PX goes WHOLE to the
           software spans (+ the wall-lag band): one big plane must not eat the shared tile
           budget and leave everything else (or its own remainder) frozen flat. */
        if ((parea << detailshift) > FTEX_PLANE_CPU_PX) return 3;
        /* CLAIM-TIME shared budget (the structural fix for "punched but flat/cut"): a punch
           is a promise the plot must keep -- the overrun evidence (t large, x0/s0, planes
           still missing; Dr 42-70%) showed tiles being EMITTED then cut with the F-bank tail.
           Once the projected fill of already-claimed planes reaches the budget, later planes
           are not punched at all: they render as software spans (+ band) instead of being
           promised to a plot pass that cannot finish them. */
        if (ftex_claim_px + (parea << detailshift) > FTEX_PX_BUDGET) return 3;
        claim_add = parea << detailshift;   /* charged at the bottom, on the REAL claim only: the
                                               sliver/fully-far return-3 paths below and the
                                               decomposition return-0 rollbacks must not leave the
                                               shared budget inflated (planes went software but
                                               still starved later planes flat) */
        {
            int W2s = (viewwidth << detailshift) >> 1;
            int dyn = (int)(((long long)ph3 * W2s / 32) >> 16);   /* row offset of dist==NEARCLIP */
            if (dyn < 1) dyn = 1;
            if (dnear > dyn)                                 /* a sub-NEARCLIP sliver exists */
            {
                if (sat_ftex_mode == 6) return 3;            /* CONVEX-EXACT (M5): distant-only -- ANY near sliver -> software */
                int span   = dnear - (dfar == 0x7FFF ? 0 : dfar); if (span < 1) span = 1;
                int sliver = dnear - dyn;
                if (sliver * 10 > span * 3) return 3;        /* >30% too near -> whole plane CPU
                                                                (+ the textured wall-lag band) */
            }
        }
        if (sat_ftex_mode == 3)
        {
            int row = centery + (isc ? -dnear : dnear);
            if (row < 0) row = 0; else if (row > viewheight - 1) row = viewheight - 1;
            if (fxmul(ph3, yslope[row]) > (FTEX_MIPDIST << 16)) return 3;   /* fully far -> CPU
                                                                (+ the textured wall-lag band) */
        }
    }
#endif
    int ds = detailshift, lump = firstflat + flattranslation[picnum];
    unsigned short col = (unsigned short)(0x0100 | (R_FlatPotatoColor(lump) & 0xFF));   /* CRAM bank 1 | flat dominant */
    /* A1 step 1 (textured-floor foundation): emit the visplane as VERTICAL STRIPS following its
       per-column silhouette (top[x]/bottom[x]) instead of ONE bbox quad -> no more green/blue bleed.
       HOLE FIX (owner): count the strips first; if they won't FIT the accumulator, DON'T claim it
       (return 0) -> the CPU software renderer draws the whole visplane, so an over-budget floor is
       FILLED, never left as an index-0 hole.  Still FLAT colour; texture + swim bands are step 2. */
    /* Greedy TRAPEZOIDAL decomposition (owner's math): the visplane silhouette is piecewise-LINEAR
       (Doom projects wall edges as straight screen lines), so cover it with the MINIMUM trapezoids
       whose edges follow the real segments.  Grow each trapezoid while top[] AND bottom[] stay within
       ~1px of the linear chords (corridor of admissible slopes, x256); split at the breakpoint (a new
       wall edge).  A run that overflows the budget ROLLS BACK -> the CPU draws the whole visplane.
       Gaps (top>bottom, occluded) are skipped -- the core's index-0 fill skips them too (no hole). */
    int gtop = 1;                              /* +1 outward (masked by punch/wall): covers the residual ~1px corridor fit */
    int is_ceil = (height > viewz);
#if SAT_FLOOR_TEX
    /* zlight row = the same base pick R_DrawPlanes makes just before planezlight
       ((lightlevel >> LIGHTSEGSHIFT) + extralight, LIGHTSEGSHIFT=4, clamp LIGHTLEVELS-1);
       the distance index is resolved per trapezoid / per tile at flush time. */
    int zl_row = (lightlevel >> 4) + extralight;
    if (zl_row < 0) zl_row = 0; else if (zl_row > 15) zl_row = 15;
    if (floor_acc_n == 0)          /* first accumulation this frame -> snapshot the view state */
    {
        RP_AuxWait();              /* a straggling slave F-build still reads floor_acc + this
                                      snapshot -- join it before scribbling (normally long done:
                                      the build ends ~35ms into the previous frame's game logic) */
        ftex_vx  = viewx;       ftex_vy  = viewy;
        ftex_vcos = viewcos;    ftex_vsin = viewsin;
        ftex_cxf = centerxfrac; ftex_cyf = centeryfrac;  ftex_proj = projection;
        ftex_ctx = centerx;     ftex_cty = centery;      ftex_fcmap = fixedcolormap;
    }
#else
    (void)lightlevel;
#endif
    /* Topic B (SAT_CEIL_FILL: grow ceilings DOWN into the wall for the rotation gap) is RETIRED -- it bled the
       ceiling colour BELOW the wall into the sky wherever the upper wall was thinner than the grow (all window
       tops).  The plane-colour software border (sat_plane_border, r_main.c) now fills that gap at the correct
       (NBG1) latency, so the VDP1-side over-grow is no longer needed.  gbot = 1 = same tight fit as floors. */
    int gbot = 1;
    int save_n = floor_acc_n;
    for (int x = minx; x <= maxx; )
    {
        while (x <= maxx && top[x] > bottom[x]) ++x;                 /* skip gap columns (occluded) */
        if (x > maxx) break;
        int rs = x; while (x <= maxx && top[x] <= bottom[x]) ++x;
        int re = x - 1;                                             /* [rs, re] = one contiguous floor run */
        int x0 = rs;
        while (x0 < re)
        {
            /* Slope corridor, sampled EVERY column, DIVIDE-FREE (the SH-2 has no fast divide): extend
               while ONE straight edge stays within +-1px of BOTH top[] and bottom[] -- this bounds the
               chord deviation (no holes on perspective-curved spans, unlike a local 2nd-diff test) and
               breaks the instant it can't (a box-edge breakpoint -> no straddle -> no triangular sky
               gap).  Feasible top-edge slopes live in [tLo,tHi], each a fraction N/D (D = column
               distance); a new column narrows the range and fractions compare by CROSS-MULTIPLY
               (a/b<c/d <=> a*d<c*b, b,d>0).  Identical decisions to the *256 divide version, ~6x cheaper. */
            int tLoN = -0x4000, tLoD = 1, tHiN = 0x4000, tHiD = 1;   /* top slope    range [tLo, tHi] */
            int bLoN = -0x4000, bLoD = 1, bHiN = 0x4000, bHiD = 1;   /* bottom slope range [bLo, bHi] */
            /* SNAPSHOT of the corridor at the last FEASIBLE column (= x1).  The live tLoN.. keep being
               narrowed by the column that finally breaks the run, so after the loop they are INFEASIBLE
               (tLo>tHi) and unusable for the endpoint clamp below -- commit a copy each time x1 advances. */
            int sTLoN = tLoN, sTLoD = tLoD, sTHiN = tHiN, sTHiD = tHiD;
            int sBLoN = bLoN, sBLoD = bLoD, sBHiN = bHiN, sBHiD = bHiD;
            int x1 = x0 + 1, xcap = x0 + 96; if (xcap > re) xcap = re;
            for (int xx = x0 + 2; xx <= xcap; ++xx)
            {
                int dx = xx - x0;
                int tn = (int)top[xx]    - (int)top[x0];            /* top edge:    slope in [(tn-1)/dx, (tn+1)/dx] */
                int bn = (int)bottom[xx] - (int)bottom[x0];         /* bottom edge: slope in [(bn-1)/dx, (bn+1)/dx] */
                if ((long)(tn - 1) * tLoD > (long)tLoN * dx) { tLoN = tn - 1; tLoD = dx; }   /* raise top Lo   */
                if ((long)(tn + 1) * tHiD < (long)tHiN * dx) { tHiN = tn + 1; tHiD = dx; }   /* lower top Hi   */
                if ((long)(bn - 1) * bLoD > (long)bLoN * dx) { bLoN = bn - 1; bLoD = dx; }   /* raise bot Lo   */
                if ((long)(bn + 1) * bHiD < (long)bHiN * dx) { bHiN = bn + 1; bHiD = dx; }   /* lower bot Hi   */
                if ((long)tLoN * tHiD > (long)tHiN * tLoD) break;   /* top    Lo > Hi -> infeasible */
                if ((long)bLoN * bHiD > (long)bHiN * bLoD) break;   /* bottom Lo > Hi -> infeasible */
                x1 = xx;
                sTLoN = tLoN; sTLoD = tLoD; sTHiN = tHiN; sTHiD = tHiD;   /* commit feasible corridor at x1 */
                sBLoN = bLoN; sBLoD = bLoD; sBHiN = bHiN; sBHiD = bHiD;
            }
            if (floor_acc_n >= MAX_FLOOR_ACC) { floor_acc_n = save_n; return 0; }   /* overflow -> CPU whole visplane */
            struct floor_q *f = &floor_acc[floor_acc_n++];
            int xL = x0 << ds, xR = x1 << ds;
            /* WEDGE FIX (owner 2026-07-02): the corridor proves a line THROUGH x0 is within +-1px of every
               column, but the raw endpoint chord (x0->x1) can slope OUTSIDE [sTLo,sTHi] -> its middle overshoots
               the true silhouette by several px.  For a ceiling that is UP, into the sky (also index-0 in NBG1)
               -> a visible grey wedge.  Clamp the x1 endpoints into the committed feasible slope range so the
               drawn chord stays within +-1px of the real silhouette everywhere. */
            int dx1 = x1 - x0; if (dx1 < 1) dx1 = 1;
            int t1 = top[x1];
            { int tlo = top[x0] + (int)((long)sTLoN * dx1 / sTLoD);    /* top[x0] + sTLo*dx1 */
              int thi = top[x0] + (int)((long)sTHiN * dx1 / sTHiD);    /* top[x0] + sTHi*dx1 */
              if (t1 < tlo) t1 = tlo; else if (t1 > thi) t1 = thi; }
            int b1 = bottom[x1];
            { int blo = bottom[x0] + (int)((long)sBLoN * dx1 / sBLoD);
              int bhi = bottom[x0] + (int)((long)sBHiN * dx1 / sBHiD);
              if (b1 < blo) b1 = blo; else if (b1 > bhi) b1 = bhi; }
            int yTL = top[x0] - gtop,    yTR = t1 - gtop;
            int yBL = bottom[x0] + gbot, yBR = b1 + gbot;
#if SAT_FLOOR_HOVER
            /* grow SAT_FLOOR_HOVER px past each end ALONG the edge's own slope (extrapolate the trapezoid,
               staying a clean quad), clamped to a 45deg vertical cap so a near-vertical edge can't spike. */
            int w = xR - xL; if (w < 1) w = 1;
            int dT = (yTR - yTL) * SAT_FLOOR_HOVER / w;
            int dB = (yBR - yBL) * SAT_FLOOR_HOVER / w;
            if (dT >  SAT_FLOOR_HOVER) dT =  SAT_FLOOR_HOVER; else if (dT < -SAT_FLOOR_HOVER) dT = -SAT_FLOOR_HOVER;
            if (dB >  SAT_FLOOR_HOVER) dB =  SAT_FLOOR_HOVER; else if (dB < -SAT_FLOOR_HOVER) dB = -SAT_FLOOR_HOVER;
            xL -= SAT_FLOOR_HOVER; xR += SAT_FLOOR_HOVER;
            yTL -= dT; yTR += dT; yBL -= dB; yBR += dB;
#endif
            f->x1  = (short)xL;   f->x2  = (short)xR;
            f->ytl = (short)yTL;  f->ytr = (short)yTR;
            f->ybl = (short)yBL;  f->ybr = (short)yBR;
            f->col = col;
#if SAT_FLOOR_TEX
            f->lump = (short)lump;
            f->hz   = height - viewz;
            f->ln   = (unsigned char)zl_row;
            f->ceil = (unsigned char)is_ceil;
#endif
            x0 = x1;
            if (sat_ftex_mode == 6 && x0 < re) { floor_acc_n = save_n; return 3; }   /* CONVEX-EXACT: this run needs >1 trapezoid (non-convex) -> whole plane software */
        }
    }
    if (floor_acc_n == save_n) return 0;   /* nothing emitted -> software */
#if SAT_FLOOR_TEX
    ftex_claim_px += claim_add;            /* the plane really claims -> charge the shared budget */
    if (sat_ftex_mode == 3)
    {
        /* PARTIAL claim: per-column split edge = the tiles' chunk-clip interiors (identical
           math via ftex_chunk_for/ftex_fill_punch) -> the core punches only the tiled band;
           the far field AND the far-side wedge triangles render as software texels, and the
           MAGNIFIED near band (< FTEX_CPU_NEARDIST) goes to the CPU spans (punch_nrow). */
        int phn = height - viewz; if (phn < 0) phn = -phn;
        int W2n = (viewwidth << detailshift) >> 1;
        int dyn2 = (int)((((long long)phn * W2n) / FTEX_CPU_NEARDIST) >> 16);
        if (dyn2 < 1) dyn2 = 1;
        /* ADAPTIVE near split (FTEX_NEAR_MIN_ROWS): a shallow near band claims m2-style --
           punch_nrow 0 disarms the core's near trim (its pn>0 guards), nsplit disarms the
           emitters' near clamps, so VDP1 serves the whole depth there.  A BUSY flat (light
           grid -- high texel contrast, ftex_flat_busy) always keeps the CPU near band: the
           flat underlay patches the m2-style claim leaves there are glaring on it. */
        int nspl = (fdnear - dyn2 >= FTEX_NEAR_MIN_ROWS) || ftex_flat_busy(lump);
        sat_floor_punch_nrow = nspl ? (is_ceil ? (centery - dyn2) : (centery + dyn2)) : 0;
        for (int i = save_n; i < floor_acc_n; ++i)
            floor_acc[i].nsplit = (unsigned char)nspl;
        for (int xx = minx; xx <= maxx && xx < 320; ++xx)
            ftex_punch_edge_buf[xx] = is_ceil ? (short)-1 : (short)0x7FFF;   /* = no punch */
        for (int i = save_n; i < floor_acc_n; ++i)
            ftex_fill_punch(&floor_acc[i]);
        return 2;
    }
#endif
    return 1;   /* claimed -> core skips the software span + leaves index 0 (VDP1 shows through NBG1) */
}

#if SAT_FLOOR_TEX
/* ============================================================================================
   Option-1 STEP 2: textured floors/ceilings -- world-anchored 64x64 flat tiles on VDP1.
   VDP1 has NO per-vertex UV: a textured primitive is always the FULL char rect mapped onto a
   screen quad.  So the only faithful floor texturing is the SlaveDriver model: draw the
   PROJECTIONS of texture-space rectangles.  Flats are world-axis-aligned 64x64, so one world
   tile = one DISTORSP whose 4 screen corners are the projected tile corners -- view rotation
   lives in the quad's screen shape; anchoring is WORLD-anchored by construction (u = world x,
   v = -world y, the span drawer's ds_xfrac/ds_yfrac convention -- never screen-anchored, the
   SAT_YCLAMP lesson).
   Near/far: tiles only where view distance <= FTEX_MIPDIST (texture detail is visible, tile
   count and texel minification are bounded); farther keeps the step-1 lit-flat trapezoid
   (SlaveDriver mips there, we flat there).  Silhouette: tiles are hardware-clipped
   (FUNC_UserClip + Window_In) to the INTERIOR bbox of each silhouette trapezoid; the sloped-
   edge wedges outside the clip keep showing the flat UNDERLAY drawn in pass 1 -> never a hole,
   never a bleed past the silhouette (the ceiling-vs-sky wedge lesson).  Painter-safe: visplane
   regions are screen-disjoint and every tile is clipped to its own trapezoid.
   Split screen: one flush, one view transform -> tiles (and the distance light) are 1p-only. */
#define FTEX_ZGUARD    (8 << 16)    /* min view-axis z of a projected tile corner (den guard) */
#define FTEX_NEARCLIP  (32 << 16)   /* near clip of the textured band (SlaveDriver TILENEARCLIP=F(33)) */
#define FTEX_MIP1DIST  (256 << 16)  /* beyond: sample the 32x32 decimated mip (1/4 the fetch)  */
#define FTEX_MIP2DIST  (512 << 16)  /* beyond: the 16x16 mip, + HSS (SlaveDriver MIPDIST law)  */
#define FTEX_MAXU      12           /* tile-grid columns per clip rect -- beyond = truncate (underlay shows) */
#define FTEX_MAXV      16           /* tile-grid rows per clip rect */
#define FTEX_SLOTS     7            /* 7 x 6KB flat cache slots (64x64 + its two mips) */
#define FTEX_SLOT_SZ   0x1800u      /* +0x0000 = 64x64 (4KB), +0x1000 = 32x32 mip, +0x1400 = 16x16 mip */
#define FTEX_SLOT_BASE 0x25C71000u  /* the tail freed by the wtex 16->15 narrow shrink            */
#define FTEX_FBANK0    0x25C7C000u  /* F bank pair (256 cmds each): the WHOLE floor layer --      */
#define FTEX_FBANK1    0x25C7E000u  /* localcoord + fresh underlays + tiles + tail JUMP->walls -- */
#define FTEX_F_CAP     252          /* built AFTER R_DrawPlanes (same frame as walls+mask, kills
                                       the forward/backward slip); the previous frame's F bridges
                                       the pre-planes window (no flicker).  The old 126-cmd T cap
                                       silently starved coverage -> 252.  Bank F of
                                       VDP1_WORLD_PLAN §7.1, chained WALLS->FLOORS so an
                                       overrunning plot cuts floors, never walls. */
/* (FTEX_PX_BUDGET hoisted to the top define block -- it now also drives the CLAIM-TIME
   budget inside sat_floor_vdp1_emit.) */

/* VRAM tail ledger (wtex now ends 0x25C71000 after the 16->15 narrow shrink; VRAM ends
   0x25C80000): 7 flat slots x 0x1800 = 0x25C71000..0x25C7B800, 2KB slack, F banks 2 x 8KB at
   0x25C7C000..0x25C80000.  The old VDP1_HUD_TEX region (0x25C78000, dead code -- vdp1_hud_emit
   is never called) is inside the slot run; if the VDP1 HUD is ever revived it must move. */
static inline unsigned int ftex_slot_addr(int slot)
{ return FTEX_SLOT_BASE + (unsigned int)slot * FTEX_SLOT_SZ; }
static struct { int lump; unsigned int lru; unsigned char locked; } ftex_cache[FTEX_SLOTS];
static unsigned int ftex_tick;
static int ftex_tiles, ftex_skips, ftex_trunc, ftex_bakes;   /* this flush (snapshotted for row 12) */
static int ftex_px;                      /* tile fill spent this flush (bbox px, budget currency) */
static unsigned int ftex_tile_base;      /* F bank being written this frame (F0/F1)             */
static int          ftex_next;           /* write cursor in the F bank                          */
static unsigned int ftex_wjump_addr;     /* byte addr of THIS frame's wall-bank closing JUMP.
                                            WALLS FIRST in list order: when a heavy plot overruns
                                            the vblank, the FLOORS get cut, never the walls.  The
                                            root only flips at the flush, to the COMPLETE fresh
                                            walls+floors pair (no mixed-frame passes -- the old
                                            stale-floor bridge was the motion flick). */
static int          ftex_flushed;        /* this frame's F already built (hook ran; DG call no-ops) */

/* Mip decimation sample for ONE mip texel whose source block anchors at (x,y), step x step.
   CALM flats: plain point sample (skip-sampling keeps the hue family exact).  BUSY flats
   (owner 2026-07-03 "on les voit en software uniquement"): plain decimation DROPS small
   high-contrast features -- a 3-4-texel ceiling light falls BETWEEN the stride-4 samples,
   so the far mip bands lost the tech-room lights that every software region kept.  Instead
   take the block texel FARTHEST from the flat's dominant colour when that distance clears
   FTEX_MIP_FEAT (a defining feature -- lights stay lit); else the plain point sample.
   Bake-time only (once per slot upload); the per-frame fetch is unchanged. */
#define FTEX_MIP_FEAT 120   /* per-texel |dR|+|dG|+|dB| to dominant: above = feature, keep it */
static unsigned char ftex_mip_pick(const unsigned char *src, int x, int y,
                                   int step, int busy, int dr, int dg, int db)
{
    unsigned char pick = src[y * 64 + x];
    if (!busy) return pick;
    int best = FTEX_MIP_FEAT;
    for (int by = 0; by < step; ++by)
        for (int bx = 0; bx < step; ++bx)
        {
            int c = src[(y + by) * 64 + x + bx], a, d;
            d = colors[c].r - dr; a  = d < 0 ? -d : d;
            d = colors[c].g - dg; a += d < 0 ? -d : d;
            d = colors[c].b - db; a += d < 0 ? -d : d;
            if (a > best) { best = a; pick = (unsigned char)c; }
        }
    return pick;
}

/* flat lump -> VDP1 VRAM slot (LRU + per-frame lock, the wtex discipline: a slot referenced by
   an already-emitted command is never re-uploaded mid-frame).  Upload = raw 8bpp palette
   indices, 16-bit packed exactly like the wall bake (hi byte = even texel, big-endian SH-2);
   light is NOT baked -- it rides the CMDCOLR CRAM bank, so no re-upload on light/flash. */
static int ftex_resolve(int lump)
{
    for (int i = 0; i < FTEX_SLOTS; ++i)
        if (ftex_cache[i].lump == lump)
        { ftex_cache[i].locked = 1; ftex_cache[i].lru = ftex_tick; return i; }
    int victim = -1; unsigned int best = 0xFFFFFFFFu;
    for (int i = 0; i < FTEX_SLOTS; ++i)
    {
        if (ftex_cache[i].locked) continue;
        /* 2-GENERATION guard: the PREVIOUS frame's F bank is still chained (the anti-flicker
           bridge) and its tiles sample these slots -- re-uploading a slot it references put
           NUKAGE on a brown floor mid-bridge (owner capture 2026-07-03).  Only slots idle for
           BOTH generations are evictable. */
        if (ftex_cache[i].lru != 0 && ftex_cache[i].lru + 1 >= ftex_tick) continue;
        if (ftex_cache[i].lru <= best) { best = ftex_cache[i].lru; victim = i; }
    }
    if (victim < 0) return -1;                        /* nothing safely evictable -> stay flat */
    const unsigned char *src = (const unsigned char *)W_CacheLumpNum(lump, 8 /* PU_CACHE */);
    if (!src) return -1;
    {
        unsigned int base = ftex_slot_addr(victim);
        volatile unsigned short *t = (volatile unsigned short *)base;
        for (int i = 0; i < 64 * 64 / 2; ++i)
            t[i] = (unsigned short)(((unsigned int)src[2 * i] << 8) | src[2 * i + 1]);
        /* mips: 32x32 (+0x1000) and 16x16 (+0x1400), DECIMATED (palette indices cannot be
           averaged) via ftex_mip_pick -- point sample on calm flats, feature-max on busy
           ones.  Far tiles sample these instead of the 64x64 so the minification fetch
           stays bounded (MIP1/2DIST). */
        int busy = ftex_flat_busy(lump);
        int fdr = 0, fdg = 0, fdb = 0;
        if (busy)
        {
            int dom = R_FlatPotatoColor(lump) & 0xFF;
            fdr = colors[dom].r; fdg = colors[dom].g; fdb = colors[dom].b;
        }
        volatile unsigned short *m1 = (volatile unsigned short *)(base + 0x1000u);
        for (int y = 0; y < 32; ++y)
            for (int xw = 0; xw < 16; ++xw)
                m1[y * 16 + xw] = (unsigned short)
                    (((unsigned int)ftex_mip_pick(src, xw * 4,     y * 2, 2, busy, fdr, fdg, fdb) << 8)
                                  | ftex_mip_pick(src, xw * 4 + 2, y * 2, 2, busy, fdr, fdg, fdb));
        volatile unsigned short *m2 = (volatile unsigned short *)(base + 0x1400u);
        for (int y = 0; y < 16; ++y)
            for (int xw = 0; xw < 8; ++xw)
                m2[y * 8 + xw] = (unsigned short)
                    (((unsigned int)ftex_mip_pick(src, xw * 8,     y * 4, 4, busy, fdr, fdg, fdb) << 8)
                                  | ftex_mip_pick(src, xw * 8 + 4, y * 4, 4, busy, fdr, fdg, fdb));
    }
    ftex_cache[victim].lump = lump; ftex_cache[victim].locked = 1;
    ftex_cache[victim].lru = ftex_tick;
    ftex_bakes++;
    return victim;
}

/* CRAM light bank for a floor point at view-axis distance `dist` (fixed): the SAME pick the
   software span makes (r_plane.c: zlight[row][distance >> LIGHTZSHIFT], LIGHTZSHIFT=20,
   MAXLIGHTZ=128), quantized to the 7 CRAM banks by wall_light_colr -> VDP1 floors, VDP1 walls
   and the software residue agree on the band by construction. */
static inline unsigned short ftex_zcolr(int zrow, int dist)
{
    int zi = dist >> 20;
    if (zi > 127) zi = 127; else if (zi < 0) zi = 0;
    const unsigned char *cm = ftex_fcmap ? ftex_fcmap : zlight[zrow][zi];
    return wall_light_colr(cm);
}

/* Emit the tiles of ONE clip rect (a column chunk of a trapezoid): hardware-clip to it, then
   draw every 64x64 world tile of its [NEARCLIP..] footprint that projects into it, into the
   dedicated T tile bank.  Returns 0 when the tile bank is full (caller stops).  Anything
   skipped/truncated stays covered by the pass-1 flat underlay -- never a hole. */
static int ftex_emit_rect(const struct floor_q *f, int slot, int cx1, int cx2, int cyT, int cyB)
{
    unsigned short cmd[16];
    int ds = detailshift;
    int ph = f->hz < 0 ? -f->hz : f->hz;

    /* world footprint of the clip rect (convex: 2 constant-distance lines x 2 view rays) ->
       texel bbox.  Inverse projection: lateral = dist * (col - centerx)/projection. */
    int yfar = f->ceil ? cyB : cyT, ynear = f->ceil ? cyT : cyB;
    int dF = fxmul(ph, yslope[yfar]);
    int dN = fxmul(ph, yslope[ynear]);
    if (dN < FTEX_NEARCLIP) dN = FTEX_NEARCLIP;
    if (dF <= dN) return 1;
    int umin = 0, umax = 0, vmin = 0, vmax = 0;
    for (int k = 0; k < 4; ++k)
    {
        int dist = (k < 2) ? dF : dN;
        int cx   = (k & 1) ? cx2 : cx1;
        int txl  = fxmul(dist, fxdiv(((cx >> ds) - ftex_ctx) << 16, ftex_proj));
        int wx   = ftex_vx + fxmul(dist, ftex_vcos) + fxmul(txl, ftex_vsin);
        int wy   = ftex_vy + fxmul(dist, ftex_vsin) - fxmul(txl, ftex_vcos);
        int u = wx >> 16, v = (-wy) >> 16;
        if (k == 0) { umin = umax = u; vmin = vmax = v; }
        else { if (u < umin) umin = u; if (u > umax) umax = u;
               if (v < vmin) vmin = v; if (v > vmax) vmax = v; }
    }
    int tu0 = umin & ~63, tv0 = vmin & ~63;
    int nu = (umax - tu0) / 64 + 1, nv = (vmax - tv0) / 64 + 1;
    if (nu > FTEX_MAXU) { nu = FTEX_MAXU; ftex_trunc++; }
    if (nv > FTEX_MAXV) { nv = FTEX_MAXV; ftex_trunc++; }

    /* project the (nu+1)x(nv+1) tile-corner grid two rows at a time (each node ONCE) */
    struct fnode { short sx, sy; unsigned char ok; int tz; } rows[2][FTEX_MAXU + 1];
    int winset = 0;
    unsigned int slotbase = ftex_slot_addr(slot);
    for (int r = 0; r <= nv; ++r)
    {
        struct fnode *cur = rows[r & 1];
        int wy_ = -((tv0 + (r << 6)) << 16);         /* world y of this texel-grid row (v = -y) */
        for (int c = 0; c <= nu; ++c)
        {
            int wx_ = (tu0 + (c << 6)) << 16;
            int tx = wx_ - ftex_vx, ty = wy_ - ftex_vy;
            int tz = fxmul(tx, ftex_vcos) + fxmul(ty, ftex_vsin);
            cur[c].ok = 0; cur[c].tz = tz;
            if (tz < FTEX_ZGUARD) continue;          /* behind/too close -> tile falls to underlay */
            int scale = fxdiv(ftex_proj, tz);
            long long sxl = ((long long)ftex_cxf
                          + (((long long)(fxmul(tx, ftex_vsin) - fxmul(ty, ftex_vcos)) * scale) >> 16)) >> 16;
            long long syl = ((long long)ftex_cyf - (((long long)f->hz * scale) >> 16)) >> 16;
            int sx = (int)(sxl << ds), sy = (int)syl;
            if (sx < -1024 || sx > 1344 || sy < -512 || sy > 736) continue;  /* bound VDP1 iteration */
            cur[c].sx = (short)sx; cur[c].sy = (short)sy; cur[c].ok = 1;
        }
        if (r == 0) continue;
        struct fnode *top = rows[(r - 1) & 1];
        for (int c = 0; c < nu; ++c)
        {
            struct fnode *A = &top[c], *B = &top[c + 1], *C = &cur[c + 1], *D = &cur[c];
            if (!A->ok || !B->ok || !C->ok || !D->ok) { ftex_skips++; continue; }
            int mnx = A->sx, mxx = A->sx, mny = A->sy, mxy = A->sy;
            if (B->sx < mnx) mnx = B->sx; if (B->sx > mxx) mxx = B->sx;
            if (C->sx < mnx) mnx = C->sx; if (C->sx > mxx) mxx = C->sx;
            if (D->sx < mnx) mnx = D->sx; if (D->sx > mxx) mxx = D->sx;
            if (B->sy < mny) mny = B->sy; if (B->sy > mxy) mxy = B->sy;
            if (C->sy < mny) mny = C->sy; if (C->sy > mxy) mxy = C->sy;
            if (D->sy < mny) mny = D->sy; if (D->sy > mxy) mxy = D->sy;
            if (mxx < cx1 || mnx > cx2 || mxy < cyT || mny > cyB) continue;  /* outside the clip */
            int ax1 = mnx > cx1 ? mnx : cx1, ax2 = mxx < cx2 ? mxx : cx2;
            int ay1 = mny > cyT ? mny : cyT, ay2 = mxy < cyB ? mxy : cyB;
            int ca  = (ax2 - ax1) * (ay2 - ay1);     /* clipped (visible) area */
            {   /* iteration-waste guard: VDP1 walks the WHOLE quad even where clipped, so a
                   hugely magnified near tile can burn ms for a few visible px -- a prime
                   Dr%-drain (43-59% measured).  Skip when the full extent is large AND >8x
                   the visible part; the lit underlay covers the spot. */
                long long bb = (long long)(mxx - mnx) * (mxy - mny);
                if (bb > 32768 && bb > ((long long)ca << 3)) { ftex_skips++; continue; }
            }
            if (ftex_next >= FTEX_F_CAP || ftex_px > FTEX_PX_BUDGET) { ftex_trunc++; return 0; }
            if (!winset)
            {
                memset(cmd, 0, sizeof cmd);
                cmd[0]  = 0x0008;                    /* FUNC_UserClip = this chunk's interior rect */
                cmd[6]  = (unsigned short)cx1; cmd[7]  = (unsigned short)cyT;
                cmd[10] = (unsigned short)cx2; cmd[11] = (unsigned short)cyB;
                vdp1_cmd_at(ftex_tile_base, ftex_next++, cmd);
                winset = 1;
                if (ftex_next >= FTEX_F_CAP) { ftex_trunc++; return 0; }
            }
            int tzm = (int)((((long long)A->tz + B->tz) + ((long long)C->tz + D->tz)) >> 2);
            memset(cmd, 0, sizeof cmd);
            cmd[0] = 0x0002;                         /* DISTORSP | Window_In | 8bpp bank | SPD | ECD-off */
            if (tzm > FTEX_MIP2DIST)                 /* per-tile LOD: decimated mips bound the fetch */
            { cmd[2] = 0x14E0;                       /* + HSS on the farthest band */
              cmd[4] = (unsigned short)((slotbase + 0x1400u - VDP1_VRAM_BASE) >> 3); cmd[5] = 0x0210; }
            else if (tzm > FTEX_MIP1DIST)
            { cmd[2] = 0x04E0;
              cmd[4] = (unsigned short)((slotbase + 0x1000u - VDP1_VRAM_BASE) >> 3); cmd[5] = 0x0420; }
            else
            { cmd[2] = 0x04E0;
              cmd[4] = (unsigned short)((slotbase - VDP1_VRAM_BASE) >> 3);           cmd[5] = 0x0840; }
            cmd[3] = ftex_zcolr(f->ln, tzm);         /* per-tile CRAM distance-light bank */
            cmd[6]  = (unsigned short)A->sx; cmd[7]  = (unsigned short)A->sy;   /* A = tex (0,0)   */
            cmd[8]  = (unsigned short)B->sx; cmd[9]  = (unsigned short)B->sy;   /* B = tex (64,0)  */
            cmd[10] = (unsigned short)C->sx; cmd[11] = (unsigned short)C->sy;   /* C = tex (64,64) */
            cmd[12] = (unsigned short)D->sx; cmd[13] = (unsigned short)D->sy;   /* D = tex (0,64)  */
            vdp1_cmd_at(ftex_tile_base, ftex_next++, cmd);
            ftex_tiles++;
            ftex_px += ca;   /* budget currency = the CLIPPED area (an unclipped near bbox once
                                drained the whole budget alone -- owner capture 2026-07-03) */
        }
    }
    return 1;
}

/* Textured near-field of ONE silhouette trapezoid: clamp to the MIPDIST band, then walk it
   in FTEX_CHUNK-px column chunks, each with a CHORD-interpolated interior clip rect (the
   flat wedge at a sloped edge shrinks from slope*width to slope*chunk). */
static void ftex_emit_trapezoid(const struct floor_q *f, int slot)
{
    int ds = detailshift;
    int ph = f->hz < 0 ? -f->hz : f->hz;
    if (ph == 0) return;
    int cyTa = 0, cyBa = viewheight - 1;
    /* near/far split at FTEX_MIPDIST: rows closer to the horizon than ymip keep the lit flat */
    int W2 = (viewwidth << ds) >> 1;                 /* == yslope's (viewwidth<<detailshift)/2 */
    int mrows = (int)((((long long)ph * W2) / FTEX_MIPDIST) >> 16);
    if (mrows < 1) mrows = 1;
    if (!f->ceil) { int ymip = ftex_cty + mrows; if (cyTa < ymip) cyTa = ymip; }
    else          { int ymip = ftex_cty - mrows; if (cyBa > ymip) cyBa = ymip; }
    if (ftex_build_mode == 3 && f->nsplit)
    {   /* mode 3 WITH near split: the sub-FTEX_CPU_NEARDIST band belongs to the CPU spans
           (partial claim) -- don't spend tiles or fill there.  nsplit 0 (shallow band,
           FTEX_NEAR_MIN_ROWS) = m2-style: tiles serve the whole depth.  ftex_build_mode is
           the flush-time snapshot (the emitters may run on the SLAVE after the pad moved). */
        int dyn2 = (int)((((long long)ph * W2) / FTEX_CPU_NEARDIST) >> 16); if (dyn2 < 1) dyn2 = 1;
        if (!f->ceil) { int yn = ftex_cty + dyn2; if (cyBa > yn) cyBa = yn; }
        else          { int yn = ftex_cty - dyn2; if (cyTa < yn) cyTa = yn; }
    }
    if (cyTa < 0) cyTa = 0;
    if (cyBa > viewheight - 1) cyBa = viewheight - 1;
    if (cyTa >= cyBa) return;                        /* nothing serviceable -> spans/flat only */

    int w = f->x2 - f->x1;
    if (w < 1) return;
    int chunk = ftex_chunk_for(f);                   /* adaptive: wedge <= FTEX_WEDGE_MAX px */
    int prevT = f->ytl, prevB = f->ybl;              /* chord values at the chunk's left edge */
    for (int xa = f->x1; xa < f->x2; )
    {
        int xb = xa + chunk; if (xb > f->x2) xb = f->x2;
        int tb2 = f->ytl + (int)((long)(f->ytr - f->ytl) * (xb - f->x1) / w);   /* chordTop(xb) */
        int bb2 = f->ybl + (int)((long)(f->ybr - f->ybl) * (xb - f->x1) / w);   /* chordBot(xb) */
        int cT = prevT > tb2 ? prevT : tb2;          /* interior: max of the chord tops    */
        int cB = prevB < bb2 ? prevB : bb2;          /* interior: min of the chord bottoms */
        if (cT < cyTa) cT = cyTa;
        if (cB > cyBa) cB = cyBa;
        if (cT < cB && !ftex_emit_rect(f, slot, xa, xb, cT, cB)) return;   /* T bank full */
        prevT = tb2; prevB = bb2; xa = xb;
    }
}
#endif /* SAT_FLOOR_TEX */

#if !SAT_FLOOR_TEX
static void vdp1_floors_flush(void)   /* legacy step-1: flat quads into the wall bank */
{
    unsigned short cmd[16];
    for (int i = 0; i < floor_acc_n; ++i)
    {
        if (vdp1_wnext >= WALL_CMD_CAP) break;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0004; cmd[2] = 0x00C0; cmd[3] = floor_acc[i].col;   /* FUNC_Polygon flat | SPD | ECD-off */
        cmd[6]  = (unsigned short)floor_acc[i].x1; cmd[7]  = (unsigned short)floor_acc[i].ytl;   /* A far-left  */
        cmd[8]  = (unsigned short)floor_acc[i].x2; cmd[9]  = (unsigned short)floor_acc[i].ytr;   /* B far-right */
        cmd[10] = (unsigned short)floor_acc[i].x2; cmd[11] = (unsigned short)floor_acc[i].ybr;   /* C near-right*/
        cmd[12] = (unsigned short)floor_acc[i].x1; cmd[13] = (unsigned short)floor_acc[i].ybl;   /* D near-left */
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
    }
    floor_acc_n = 0;
}
#else
/* Build THIS frame's WHOLE floor layer in the F bank -- localcoord + fresh flat underlays +
   textured tiles + a tail JUMP into this frame's wall bank -- then make it live with ONE
   atomic root-link flip.  Runs from sat_floors_done_hook at the END of R_DrawPlanes, so the
   floors go live the SAME frame as the walls and the mask (kills the forward/backward
   wall-vs-plane slip: floors were previously one frame late).  Until it runs, the kick keeps
   the PREVIOUS frame's F bridged to the fresh walls (no flicker, no floor-less window);
   floors sit BEFORE the walls in list order, so the walls repaint any stale-bridge floor
   overhang at the junctions.  The DG_DrawFrame call is only a fallback (ftex_flushed). */
static void vdp1_ftex_flush(void)
{
    extern int sat_local_players;
    unsigned short cmd[16];
    if (ftex_flushed) return;
    ftex_flushed = 1;
    int fbk = vdp1_bank;                             /* this frame's parity (set by the kick) */
    int fmode = (sat_local_players <= 1) ? sat_ftex_mode : 0;
    {
        /* UNCONDITIONAL join + purge (review finding: gating these on the LIVE sat_ftex_slave
           let a pad L+Y toggle-off race a straggling build -- double root flip, stale row-12
           counters).  Both are near-free when no job is pending / the build is long done. */
        RP_AuxWait();
        volatile unsigned char *ccr = (volatile unsigned char *)0xFFFFFE92;
        *ccr = (unsigned char)(*ccr | 0x10);         /* the row-12 counters + ftex_next below
                                                        may have been written by the SLAVE */
    }
    ftexd_tiles = ftex_tiles; ftexd_skips = ftex_skips;      /* row-12 snapshot of last frame */
    ftexd_trunc = ftex_trunc; ftexd_bakes = ftex_bakes; ftexd_px = ftex_px;
    vd1_fpct = FTEX_F_CAP ? (ftexd_tiles * 100 / FTEX_F_CAP) : 0;   /* floor tile-budget fill % */
    ftexd_acc = floor_acc_n;                                 /* trapezoids claimed this frame */
    ftex_tiles = ftex_skips = ftex_trunc = ftex_bakes = 0; ftex_px = 0;
    ftex_claim_px = 0;                                       /* re-arm the claim-time budget */
    if (!ftex_wjump_addr) return;                    /* no wall list this frame (menu) */
    unsigned int wroot = (unsigned int)((VDP1_BANK[fbk] - VDP1_VRAM_BASE) >> 3);
    if (fmode == 4 || floor_acc_n == 0)
    {
        /* software mode / nothing claimed: the walls' JUMP already targets the empty bank
           (the kick wrote it) -- just present the fresh pair. */
        *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = (unsigned short)wroot;
        ftex_wjump_addr = 0;
        floor_acc_n = 0;
        return;
    }
    ftex_tick++;
    for (int i = 0; i < FTEX_SLOTS; ++i) ftex_cache[i].locked = 0;
    ftex_tile_base = fbk ? FTEX_FBANK1 : FTEX_FBANK0;
    ftex_next = 0;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A; cmd[7] = VIEW_Y_OFFSET;         /* slot 0: local coord (the list roots here) */
    vdp1_cmd_at(ftex_tile_base, ftex_next++, cmd);
    /* fresh flat underlays (exact silhouettes) -- full coverage under the tiles */
    for (int i = 0; i < floor_acc_n && ftex_next < FTEX_F_CAP; ++i)
    {
        unsigned short colr = floor_acc[i].col;
        if (fmode >= 1)                    /* distance-lit flat: bank from the trapezoid's mid row */
        {
            int ymid = (floor_acc[i].ytl + floor_acc[i].ytr + floor_acc[i].ybl + floor_acc[i].ybr) >> 2;
            if (ymid < 0) ymid = 0; else if (ymid > viewheight - 1) ymid = viewheight - 1;
            int ph = floor_acc[i].hz < 0 ? -floor_acc[i].hz : floor_acc[i].hz;
            colr = (unsigned short)(ftex_zcolr(floor_acc[i].ln, fxmul(ph, yslope[ymid]))
                                    | (floor_acc[i].col & 0xFFu));
        }
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0004; cmd[2] = 0x00C0; cmd[3] = colr;               /* FUNC_Polygon flat | SPD | ECD-off */
        cmd[6]  = (unsigned short)floor_acc[i].x1; cmd[7]  = (unsigned short)floor_acc[i].ytl;   /* A far-left  */
        cmd[8]  = (unsigned short)floor_acc[i].x2; cmd[9]  = (unsigned short)floor_acc[i].ytr;   /* B far-right */
        cmd[10] = (unsigned short)floor_acc[i].x2; cmd[11] = (unsigned short)floor_acc[i].ybr;   /* C near-right*/
        cmd[12] = (unsigned short)floor_acc[i].x1; cmd[13] = (unsigned short)floor_acc[i].ybl;   /* D near-left */
        vdp1_cmd_at(ftex_tile_base, ftex_next++, cmd);
    }
    /* textured tiles, NEAREST trapezoid first (spend the budget where texture is most visible) */
    ftex_build_mode = fmode;
    if (fmode >= 2)
    {
        unsigned char idx[MAX_FLOOR_ACC]; short key[MAX_FLOOR_ACC];
        for (int i = 0; i < floor_acc_n; ++i)
        {
            int ne = floor_acc[i].ceil ? (floor_acc[i].ytl < floor_acc[i].ytr ? floor_acc[i].ytl
                                                                              : floor_acc[i].ytr)
                                       : (floor_acc[i].ybl > floor_acc[i].ybr ? floor_acc[i].ybl
                                                                              : floor_acc[i].ybr);
            int d = ne - ftex_cty; if (d < 0) d = -d;
            idx[i] = (unsigned char)i; key[i] = (short)d;
        }
        for (int i = 1; i < floor_acc_n; ++i)        /* insertion sort, largest d (nearest) first */
        {
            unsigned char ii = idx[i]; short kk = key[i]; int j = i - 1;
            while (j >= 0 && key[j] < kk) { key[j + 1] = key[j]; idx[j + 1] = idx[j]; --j; }
            key[j + 1] = kk; idx[j + 1] = ii;
        }
        if (sat_ftex_slave)
        {
            /* SLAVE OFFLOAD: freeze the job header (sorted order + slots resolved HERE -- the
               zone allocator and the VRAM flat uploads stay master-only) and hand the tile
               emission to the slave SH-2.  DG_DrawFrame dispatches it (RP_AuxDispatch); the
               slave finishes with the END + the W->F chain + the SAME single atomic root flip
               (v7 pair semantics -- the dual-blit rewind joins the job, so the pair still goes
               live BEFORE the NBG1 mask blit). */
            for (int k = 0; k < floor_acc_n; ++k)
            {
                ftex_job.idx[k]  = idx[k];
                ftex_job.slot[k] = (short)ftex_resolve(floor_acc[idx[k]].lump);
            }
            ftex_job.n     = (short)floor_acc_n;
            ftex_job.base  = ftex_tile_base;
            ftex_job.wjump = ftex_wjump_addr;
            ftex_job.wroot = (unsigned short)wroot;
            ftex_job.overlap = (short)sat_ftex_blit_overlap;   /* frozen: the slave skips its flip iff set */
            ftex_overlap_flip = sat_ftex_blit_overlap ? 1 : 0; /* master flips in DG after the blit */
            ftex_wjump_addr = 0;
            ftex_job_ready  = 1;   /* floor_acc_n re-arms at the DG kick (entries stay frozen) */
            RP_AuxArm(ftex_slave_build);   /* PIGGYBACK: the masked slave body takes the build
                                              right after MASK_DONE (overlap = master's masked
                                              tail + DG pre); RP_AuxKick at DG entry is the
                                              fallback when masked never dispatched */
            return;
        }
        for (int k = 0; k < floor_acc_n; ++k)
        {
            if (ftex_next >= FTEX_F_CAP) { ftex_trunc++; break; }
            const struct floor_q *f = &floor_acc[idx[k]];
            int slot = ftex_resolve(f->lump);
            if (slot < 0) { ftex_skips++; continue; }
            ftex_emit_trapezoid(f, slot);
        }
    }
    /* tail: END -- the F bank is the last link of the chain (walls -> floors), so it can
       never loop and an overrunning plot cuts floors, not walls */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x8000;
    vdp1_cmd_at(ftex_tile_base, ftex_next, cmd);
    /* chain the fresh floors behind the fresh walls (the write bank is NOT displayed -- the
       root still shows last frame's pair), then present the WHOLE coherent pair with the one
       atomic root flip. */
    *((volatile unsigned short *)ftex_wjump_addr + 1) =
        (unsigned short)((ftex_tile_base - VDP1_VRAM_BASE) >> 3);
    *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = (unsigned short)wroot;
    ftex_wjump_addr = 0;
    floor_acc_n = 0;
}

/* SLAVE SH-2 body of the F build (dispatched by DG_DrawFrame via RP_AuxDispatch; the aux
   wrapper already purged the slave cache and switched to the dedicated 4KB stack).  Consumes
   ONLY the frozen job header + the floor_acc entries (guarded against next-frame scribbles by
   the RP_AuxWait joins at the first claim / the flush / every SGL rewind).  All VDP1 VRAM
   writes are cache-through (0x25C0xxxx has bit 29 set); the emitters' counter statics are
   slave-owned for the job's duration and re-read by the master after its flush-entry purge. */
static void ftex_slave_build(void)
{
    unsigned short cmd[16];
    for (int k = 0; k < ftex_job.n; ++k)
    {
        if (ftex_next >= FTEX_F_CAP) { ftex_trunc++; break; }
        int slot = ftex_job.slot[k];
        if (slot < 0) { ftex_skips++; continue; }
        ftex_emit_trapezoid(&floor_acc[ftex_job.idx[k]], slot);
    }
    memset(cmd, 0, sizeof cmd);                      /* tail END: F stays the chain's last link */
    cmd[0] = 0x8000;
    vdp1_cmd_at(ftex_job.base, ftex_next, cmd);
    *((volatile unsigned short *)ftex_job.wjump + 1) =         /* chain fresh W -> fresh F */
        (unsigned short)((ftex_job.base - VDP1_VRAM_BASE) >> 3);
    if (!ftex_job.overlap)                                     /* overlap mode: the MASTER flips after its blit */
        *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = ftex_job.wroot;   /* ONE atomic present */
}

/* sat_floors_done_hook target.  In SPLIT the hook fires once per view with a partial
   accumulator -- defer to the single end-of-frame DG_DrawFrame fallback there. */
extern "C" void sat_vdp1_floors_done(void)
{
    extern int sat_split_active;
    if (sat_split_active) return;
    vdp1_ftex_flush();
}
#endif /* SAT_FLOOR_TEX */
#endif  /* SAT_VDP1_FLOOR */

static void vdp1_walls_flush(void)
{
    wtex_bakes = 0;                      /* count this frame's texture re-bakes (the `k` driver) */
    if (wall_acc_n == 0) { wall_px_acc = 0; return; }   /* ALWAYS re-arm the fill budget: the
                             mode-4 wall_acc clear hit this early-return and the stale px_acc
                             then rejected EVERY wall forever (VD1=1, all walls CPU, 10fps --
                             owner overlay capture 2026-07-03) */
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

    /* paint FAR->NEAR (owner Ymir 2026-07-03, "les murs arriere devant"): VDP1 plots the bank
       head->tail and LAST-WRITTEN WINS, so the nearest wall must be emitted LAST to win every
       overlap.  The hook stores RAW yl/yh (pre per-column clip, dg_saturn:2904), so a far wall
       seen through a near opening -- or behind the pedestal/stair profile -- has a quad that
       extends BEHIND the near walls; near-first (the reverted 2026-07-03 order) let the far
       quad, written later, overpaint them.  The near-first order existed ONLY to make a
       plot-overrun drop the FARTHEST walls, but that famine is ALREADY handled at ACCUMULATE
       time and in the RIGHT direction: WALL_PX_BUDGET + the cmd cap route the farthest walls to
       the SOFTWARE fallback (dg_saturn:2891), which is per-column clipped (NBG1, but clipped to
       nothing behind a near wall -> no bleed).  So the two concerns are now separated: fill
       budget = overrun/famine protection (far walls -> CPU), emit order = painter correctness
       (far last... i.e. near last).  wall_acc is filled near-first by the BSP, so reverse it. */
    for (int i = wall_acc_n - 1; i >= 0; --i)
    {
        if      (wall_acc[i].mode == 1) wall_emit(i);
        else if (wall_acc[i].mode == 3) wall_emit_banded(i);
        else if (wall_acc[i].mode == 2) wall_emit_flat(i);
    }

    wall_acc_n = 0;
    wall_px_acc = 0;   /* the fill budget re-arms with the accumulator */
}
#endif

#if VDP1_MANUAL_CHANGE
/* OnVblank handler: present the finished VDP1 frame.  Corrected handshake (brick A): the kick
   drained the stale EDSR.CEF right after PTMR, so a SET CEF here belongs to THIS plot -> the swap
   is locked 1:1 to the kick's bank flip (kills the sticky-CEF "walls vanish").  FBCR = FCM|FCT
   (0x3) is a manual change: swap to the complete frame + erase the new back buffer; FCM is sticky
   so the first present also ENTERS manual mode (no auto-swap after).  Watchdog: after
   VDP1_PRESENT_STUCK_MAX vblanks with no CEF (Ymir never models manual-mode draw-end; or a
   pathological HW stall) force the swap so the walls never freeze.  Registered unconditionally at
   init; a no-op while AUTO (vdp1_present_manual == 0).  No fps/latency cost: the CPU never waits. */
static void vdp1_vblank_present(void)
{
    if (vdp1_couple_nbg1)
        return;                              /* coupled present is done in DG_DrawFrame, not here */
    if (!vdp1_present_manual || !vdp1_present_pending)
        return;
    if (VDP1_EDSR & 0x0002)                  /* this plot's draw is done */
    {
        VDP1_FBCR            = 0x0003;       /* swap + erase the new back buffer */
        vdp1_present_pending = 0;
        vdp1_present_wait    = 0;
    }
    else if (++vdp1_present_wait >= VDP1_PRESENT_STUCK_MAX)
    {
        VDP1_FBCR            = 0x0003;       /* watchdog force-swap (may tear once) -> never frozen */
        vdp1_present_pending = 0;
        vdp1_present_wait    = 0;
    }
}
#endif

#if SHOW_FPS
/* TRUE VDP1 done-rate: sample EDSR.CEF at EVERY VBLANK -- 1-cycle-auto restarts a plot pass
   per vblank, so the old per-KICK sample (once per ~5-10 vblank game frame) aliased with the
   frame rate and misread the rate at random phases (owner: "Dr% est biaisé, pb de fréquence").
   CEF at vblank = "did the pass started at the previous vblank finish in time" -- exactly the
   per-pass completion the budget work needs.  Gated on a linked world list (vd1_dr_live). */
static void vdp1_vblank_dr(void)
{
    if (!vd1_dr_live) return;
    int done = (VDP1_EDSR & 0x0002) ? 1 : 0;
    vd1_win_tot++;   if (done) vd1_win_done++;   /* ~1s window (row 2 Dr%) */
    mh_vbl_tot++;    if (done) mh_vbl_done++;     /* session (mode-scoped) done-rate -> metrics row */
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
#if SAT_WORLD_THINGS_VDP1
    thing_lru_tick = 0;
    for (int p = 0; p < 2; ++p)
        for (int i = 0; i < THINGS_TEX_SLOTS; ++i)
        { thing_cache[p][i].lump = -1; thing_cache[p][i].used = 0; thing_cache[p][i].lru = 0; }
    sat_thing_cap = THINGS_TEX_SLOTS;   /* granted distinct textures/frame (VRAM slot cap) */
#endif
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
    VDP1_FBCR = 0x0000;                              /* BOOT in 1-cycle auto (known-good, Ymir-safe, == ship) */
    VDP1_PTMR = 0x0002;
#if SHOW_FPS
    SRL::Core::OnVblank += vdp1_vblank_dr;           /* per-vblank Dr sampler (the honest done-rate) */
#endif
#if VDP1_MANUAL_CHANGE
    /* Register the gated-present handler; it is a NO-OP until pad L+Z sets vdp1_present_manual=1.
       The first gated present (FBCR=0x0003) then flips FCM=1 to ENTER manual mode at that point
       (no startup two-field erase needed: 1-cycle auto wipes both buffers via EWDR=0 anyway). */
    SRL::Core::OnVblank += vdp1_vblank_present;
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
#if SAT_WORLD_THINGS_VDP1
    sat_things_n = sat_things_decl = thing_bake_n = 0;   /* per-frame 'th'/'fb' overlay counters (reset at bank build) */
#endif
#if VDP1_DBLBANK
    vdp1_wbank = vdp1_bank ^ 1;                      /* the bank VDP1 isn't showing */
#else
    vdp1_wbank = vdp1_bank;                          /* TEST: single bank (no extra frame?) */
#endif
#if SAT_WORLD_THINGS_VDP1
    {   /* clear the write-bank parity's per-frame `used` bits (cache keys persist across frames) */
        int p = vdp1_wbank & 1;
        for (int i = 0; i < THINGS_TEX_SLOTS; ++i) thing_cache[p][i].used = 0;
    }
#endif
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A;                                 /* bank cmd0 = local coord */
    cmd[7] = VIEW_Y_OFFSET;                          /* local Y origin -> walls centred like NBG1 */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], 0, cmd);
    vdp1_wnext   = 1;
#if VDP1_MANUAL_CHANGE
    /* In-list full-screen colour-0 erase (manual-change only).  Without slSynch the VDP1 HW
       back-buffer erase (FBCR=0x0003 / EWLR-EWRR) does NOT fire reliably -> old walls accumulate
       on the sky/floor (VDP1 prio 5 sits over them).  A FUNC_Polygon writing index 0 over the whole
       framebuffer clears the back buffer at the start of each plot (~1 full-screen flat fill).
       AUTO mode keeps the free HW per-cycle erase (no polygon, no cost). */
    if (vdp1_present_manual)
    {
        memset(cmd, 0, sizeof cmd);
        cmd[0]  = 0x0004;                            /* FUNC_Polygon (flat)               */
        cmd[2]  = 0x00C0;                            /* SPD (write all, incl 0) | ECD-off */
        cmd[3]  = 0x0000;                            /* CMDCOLR 0 -> framebuffer index 0  */
        cmd[6]  = 0;   cmd[7]  = 0;                  /* A (0,0)   (VIEW_Y_OFFSET = 0)     */
        cmd[8]  = 319; cmd[9]  = 0;                  /* B (319,0)                         */
        cmd[10] = 319; cmd[11] = 223;                /* C (319,223)                       */
        cmd[12] = 0;   cmd[13] = 223;                /* D (0,223)                         */
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
    }
#endif
#if VDP1_WALL_TEST
#if SAT_VDP1_FLOOR && SAT_FLOOR_TEX
    if (sat_ftex_mode == 4)
    {
        wall_acc_n  = 0;  /* mode 4 SOFTWARE reference: the walls were drawn software this frame
                             (sat_wall_skip=0); drop the VDP1 duplicates so they cannot poke over
                             the HW sky through NBG1 index-0. */
        wall_px_acc = 0;  /* and re-arm the fill budget (its normal reset lives in walls_flush,
                             which early-returns on an empty accumulator) */
    }
#endif
#if SAT_VDP1_FLOOR && !SAT_FLOOR_TEX
    vdp1_floors_flush();  /* legacy step-1: flat floors into the wall bank (painter: under walls) */
#endif
    /* (SAT_FLOOR_TEX: the whole floor layer lives in the F bank, built fresh at the end of
       R_DrawPlanes and chained BEFORE this wall bank -- see vdp1_ftex_flush.) */
    /* WALLS moved: they are flushed in sat_walls_kick AFTER the weapon (weapon-first plot-overrun
       fix) so the weapon is the always-plotted prefix and the walls are the tail an overrun cuts.
       The DG_DrawFrame fallback caller (menu/intermission) has no walls, so nothing to flush here. */
#endif
    /* SAT_WPN_VDP1: the player weapon is emitted into THIS wall bank (before the closing JUMP)
       by the early R_DrawPlayerSprites() call in sat_walls_kick -> sat_psprite_hook ->
       sat_vdp1_wpn_draw, at priority 7 (above NBG1).  Nothing to emit here. */
    vdp1_wactive = 1;
}

#if SAT_WPN_VDP1
/* sat_psprite_begin hook: emitted ONCE at the top of R_DrawPlayerSprites (per view), before the
   weapon sprites.  A FUNC_UserClip that windows the following (Window_In) weapon quads to THIS
   view's screen rect -- so the weapon cannot poke over the status bar (1p) or spill into another
   quadrant (split).  Uses screen coords (bank local-coord origin VIEW_Y_OFFSET = 0). */
extern "C" void sat_vdp1_wpn_clip(void)
{
    if (vdp1_wnext >= VDP1_CMD_GUARD) return;
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0]  = 0x0008;                                          /* FUNC_UserClip                 */
    cmd[6]  = (short)viewwindowx;                             cmd[7]  = (short)viewwindowy;            /* upper-left  */
    cmd[10] = (short)(viewwindowx + scaledviewwidth - 1);     cmd[11] = (short)(viewwindowy + viewheight - 1); /* lower-right */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
}
#endif

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
        slot = wpn_cache_rr;
        wpn_cache_rr = (wpn_cache_rr + 1) % WPN_TEX_SLOTS;

        const unsigned int *colofs = (const unsigned int *)patch->columnofs;
#if SAT_WPN_VDP1
        /* 8BPP: 1 byte/texel = the LIGHT-SHADED Doom palette index (cmap[s[i]]); the CRAM bank 1
           (full-bright PLAYPAL) in WPN_CMDCOLR turns it back into the shaded colour.  Index 0 =
           transparent (SPD-off), so the padded gaps + true-black pixels show the scene through. */
        if ((unsigned int)(padW * H) > WPN_TEX_SLOTSZ) return;       /* too big to cache */
        volatile unsigned char *tex =
            (volatile unsigned char *)(WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ);
        for (int i = 0; i < padW * H; ++i) tex[i] = 0;               /* texel 0 = transparent gap */
        /* texel 0 is the HW transparent code, so a real black weapon pixel (shaded index 0) would
           punch a hole.  Remap 0 -> the darkest NON-zero palette index (looks black, stays opaque).
           Computed once per texture build from the live full-bright palette (bank 1 = colors[]). */
        int blk = 1, blkbest = 0x7fffffff;
        for (int p = 1; p < 256; ++p)
        {
            int lum = colors[p].r + colors[p].g + colors[p].b;
            if (lum < blkbest) { blkbest = lum; blk = p; }
        }
        for (int x = 0; x < W; ++x)
        {
            const post_t *post = (const post_t *)((const unsigned char *)patch + bswap32(colofs[x]));
            while (post->topdelta != 0xFF)
            {
                const unsigned char *s = (const unsigned char *)post + 3;
                int top = post->topdelta;
                for (int i = 0; i < post->length; ++i)
                {
                    int c = cmap[s[i]];
                    tex[(top + i) * padW + x] = (unsigned char)(c ? c : blk);   /* keep black opaque */
                }
                post = (const post_t *)((const unsigned char *)post + post->length + 4);
            }
        }
#else
        if ((unsigned int)(padW * H) * 2u > WPN_TEX_SLOTSZ) return;  /* RGB555: 2 bytes/texel */
        volatile unsigned short *tex =
            (volatile unsigned short *)(WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ);
        for (int i = 0; i < padW * H; ++i) tex[i] = 0;   /* clear to transparent */
        for (int x = 0; x < W; ++x)
        {
            const post_t *post = (const post_t *)((const unsigned char *)patch + bswap32(colofs[x]));
            while (post->topdelta != 0xFF)
            {
                const unsigned char *s = (const unsigned char *)post + 3;
                int top = post->topdelta;
                for (int i = 0; i < post->length; ++i)
                    tex[(top + i) * padW + x] = pal_rgb555(cmap[s[i]]);
                post = (const post_t *)((const unsigned char *)post + post->length + 4);
            }
        }
#endif
        wpn_cache[slot].lump = lump; wpn_cache[slot].cmap = cmap;
        wpn_cache[slot].padW = padW; wpn_cache[slot].H = H;
    }

    unsigned int texaddr = WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ;
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
#if SAT_WPN_VDP1
    /* Draw the weapon as a DISTORTED (scaled) VDP1 sprite so its on-screen size matches the
       software pspritescale.  A native-size sprite is wrong in a SPLIT view (viewwidth 160 ->
       pspritescale 0.5): it draws double-size and, placed at the half-scale x1, sits off-centre.
       scale = vis->scale = pspritescale<<detailshift; the internal x1 (sx) -> screen via
       <<detailshift + viewwindowx (0 in 1p, the quadrant origin in split); sy is already
       screen-vertical + viewwindowy.  1p: scale=FRACUNIT, offset=0 -> a native-size quad ==
       the old normal sprite. */
    {
        extern fixed_t pspritescale; extern int detailshift;
        unsigned int scale = (unsigned int)(pspritescale << detailshift);
        int x0 = (sx << detailshift) + viewwindowx;
        int y0 = sy + viewwindowy;
        int w  = (int)(((unsigned int)padW * scale) >> 16); if (w < 1) w = 1;
        int h  = (int)(((unsigned int)H    * scale) >> 16); if (h < 1) h = 1;
        int xl = flip ? (x0 + w - 1) : x0;             /* h-flip: texture A-corner -> screen right */
        int xr = flip ? x0 : (x0 + w - 1);
        cmd[0] = 0x0002;                               /* distorted sprite -> maps the texture into A,B,C,D */
        cmd[2] = 0x04A0;                               /* 256-bank | ECD-disable | Window_In (clip to the view);
                                                          SPD (bit6) CLEAR => index 0 transparent */
        cmd[3] = WPN_CMDCOLR;                          /* pr bit13 -> register 1 (prio 7, above NBG1) | bank 1 */
        cmd[4] = (unsigned short)((texaddr - VDP1_VRAM_BASE) >> 3);
        cmd[5] = (unsigned short)(((padW >> 3) << 8) | H);
        cmd[6]  = (short)xl; cmd[7]  = (short)y0;              /* A top-left  (of the texture)  */
        cmd[8]  = (short)xr; cmd[9]  = (short)y0;              /* B top-right                   */
        cmd[10] = (short)xr; cmd[11] = (short)(y0 + h - 1);    /* C bottom-right                */
        cmd[12] = (short)xl; cmd[13] = (short)(y0 + h - 1);    /* D bottom-left                 */
    }
#else
    cmd[0] = (unsigned short)(flip ? 0x0010 : 0x0000);  /* normal sprite, LR flip */
    cmd[2] = 0x00A8;                                    /* RGB (COLOR_5) | ECD off => SPD on */
    cmd[4] = (unsigned short)((texaddr - VDP1_VRAM_BASE) >> 3);  /* charAddr */
    cmd[5] = (unsigned short)(((padW >> 3) << 8) | H);          /* charSize */
    cmd[6] = (short)sx; cmd[7] = (short)sy;             /* point A = top-left */
#endif
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
}

#if SAT_WORLD_THINGS_VDP1
/* core sat_thing_hook: draw ONE world sprite as a VDP1 prio-7 distorted quad, offloading its
   masked FILL off the SH-2s.  Returns 1 if taken, 0 if declined (command budget / no free slot /
   oversize) -> the core then draws that one in software.  Same 8bpp palette recipe as the weapon
   (texel = light-shaded index cmap[s], CRAM bank 1 full-bright, index 0 transparent, black remap).
   TEAR-SAFE CACHE: reuses a slot in the write-bank parity whose (lump,cmap) still resides there (no
   re-bake); a MISS bakes into a slot NOT yet referenced by this frame's list (the `used` guard), so
   the displayed pair (other parity) is never touched.  Occlusion clip rect + quad corners by core. */
extern "C" int sat_vdp1_thing_draw(patch_t *patch, int lump, const unsigned char *cmap,
                                   int x0, int y0, int x1, int y1,
                                   int cx0, int cy0, int cx1, int cy1, int flip)
{
    int padW, H, W;

    if (vdp1_wnext >= WALL_CMD_CAP - 1) { sat_things_decl++; return 0; }  /* need 2 cmds (clip + quad) */

    W    = (int)bswap16((unsigned short)patch->width);
    H    = (int)bswap16((unsigned short)patch->height);
    padW = (W + 7) & ~7;
    if ((unsigned int)(padW * H) > THINGS_TEX_SLOTSZ) { sat_things_decl++; return 0; }  /* too big -> software */

    int p = vdp1_wbank & 1;
    int slot = -1;

    for (int i = 0; i < THINGS_TEX_SLOTS; ++i)                          /* cache lookup: (lump, cmap) */
        if (thing_cache[p][i].lump == lump && thing_cache[p][i].cmap == cmap) { slot = i; break; }

    int bake = (slot < 0);
    if (bake)
    {   /* MISS: evict the OLDEST (min lru) slot NOT feeding this frame's list.  An empty slot has
           lru 0 = always picked first (fill before evicting a resident texture). */
        int oldest = -1;
        for (int i = 0; i < THINGS_TEX_SLOTS; ++i)
            if (!thing_cache[p][i].used &&
                (oldest < 0 || thing_cache[p][i].lru < thing_cache[p][oldest].lru))
                oldest = i;
        if (oldest < 0) { sat_things_decl++; return 0; }   /* every slot feeds the current list -> out of textures this frame */
        slot = oldest;
    }
    thing_cache[p][slot].used = 1;
    thing_cache[p][slot].lru = ++thing_lru_tick;           /* most-recently-used (hit OR bake) */
    unsigned int texaddr = THINGS_TEX_BASE + (unsigned int)(p * THINGS_TEX_SLOTS + slot) * THINGS_TEX_SLOTSZ;

    if (bake)
    {   /* unpack the full patch into this slot (once per key per parity, then reused) */
        thing_cache[p][slot].lump = lump; thing_cache[p][slot].cmap = cmap; thing_bake_n++;
        const unsigned int *colofs = (const unsigned int *)patch->columnofs;
        volatile unsigned char *tex = (volatile unsigned char *)texaddr;
        for (int i = 0; i < padW * H; ++i) tex[i] = 0;                 /* texel 0 = transparent gap */
        int blk = 1, blkbest = 0x7fffffff;                            /* darkest non-zero index (keep black opaque) */
        for (int pi = 1; pi < 256; ++pi)
        { int lum = colors[pi].r + colors[pi].g + colors[pi].b; if (lum < blkbest) { blkbest = lum; blk = pi; } }
        for (int x = 0; x < W; ++x)
        {
            const post_t *post = (const post_t *)((const unsigned char *)patch + bswap32(colofs[x]));
            while (post->topdelta != 0xFF)
            {
                const unsigned char *s = (const unsigned char *)post + 3;
                int top = post->topdelta;
                for (int i = 0; i < post->length; ++i)
                { int c = cmap[s[i]]; tex[(top + i) * padW + x] = (unsigned char)(c ? c : blk); }
                post = (const post_t *)((const unsigned char *)post + post->length + 4);
            }
        }
    }

    unsigned short cmd[16];

    /* OCCLUSION: a FUNC_UserClip to the visible bounding box, then the quad clipped to it
       (Window_In).  A fully-occluded sprite never reaches here (core skips it); a partial cut
       (nearer wall/floor edge) is trimmed to the box -> the thing no longer floats over walls. */
    memset(cmd, 0, sizeof cmd);
    cmd[0]  = 0x0008;                              /* FUNC_UserClip */
    cmd[6]  = (short)cx0; cmd[7]  = (short)cy0;    /* upper-left  */
    cmd[10] = (short)cx1; cmd[11] = (short)cy1;    /* lower-right */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);

    memset(cmd, 0, sizeof cmd);
    int xl = flip ? x1 : x0;                       /* h-flip: texture A-corner -> screen right */
    int xr = flip ? x0 : x1;
    cmd[0] = 0x0002;                               /* distorted sprite */
    cmd[2] = 0x04A0;                               /* 256-bank | ECD-off | SPD CLEAR (idx0 transparent) | Window_In */
    cmd[3] = WPN_CMDCOLR;                          /* pr bit13 -> register 1 (prio 7, above NBG1) | CRAM bank 1 */
    cmd[4] = (unsigned short)((texaddr - VDP1_VRAM_BASE) >> 3);
    cmd[5] = (unsigned short)(((padW >> 3) << 8) | H);
    cmd[6]  = (short)xl; cmd[7]  = (short)y0;              /* A top-left  */
    cmd[8]  = (short)xr; cmd[9]  = (short)y0;              /* B top-right */
    cmd[10] = (short)xr; cmd[11] = (short)y1;              /* C bottom-right */
    cmd[12] = (short)xl; cmd[13] = (short)y1;              /* D bottom-left  */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
    sat_things_n++;
    return 1;
}
#endif

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
    vd1_wpct = WALL_CMD_CAP ? (vdp1_last_cmds * 100 / WALL_CMD_CAP) : 0;   /* wall command-budget fill % */
    /* Dr accumulation moved to the OnVblank sampler (vdp1_vblank_dr): per-KICK sampling ran
       once per GAME frame while 1-cycle-auto replots every VBLANK -> the rate aliased with
       the frame rate and under/over-read at random phases (owner: "Dr% est biaisé").  Here
       we only gate WHICH frames count (a world list is linked). */
    vd1_dr_live = vdp1_wactive ? 1 : 0;
    /* Phase-0 fallback profiler: snapshot the just-rendered frame's tally into cur + windowed peaks
       (r_segs.c accumulated the counters across this frame's segs), then reset below. */
    fb_cur_clamp = sat_fb_clamp_t; fb_cur_mag = sat_fb_mag_t; fb_cur_px = sat_fb_px;
    fb_cur_wclamp = sat_fb_wclamp_t;
    if (sat_fb_clamp_t  > fb_pk_clamp)  fb_pk_clamp  = sat_fb_clamp_t;
    if (sat_fb_mag_t    > fb_pk_mag)    fb_pk_mag    = sat_fb_mag_t;
    if (sat_fb_starve_t > fb_pk_starve) fb_pk_starve = sat_fb_starve_t;
    if (sat_fb_px       > fb_pk_px)     fb_pk_px     = sat_fb_px;
#endif
    sat_fb_clamp_t = sat_fb_mag_t = sat_fb_starve_t = sat_fb_px = 0;   /* reset each frame (also when SHOW_FPS off) */
    sat_fb_wclamp_t = 0;
    if (vdp1_wactive)
    {
        unsigned short end[16];
        memset(end, 0, sizeof end);
#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
        /* COHERENT-PAIR PRESENT (owner 2026-07-03: alternating perfect/destroyed frames in
           motion): the root is NOT flipped here.  VDP1 keeps replotting LAST frame's coherent
           walls+floors pair (its banks are the other parity, untouched) while this frame's W
           is closed with a JUMP to the empty bank; vdp1_ftex_flush then builds F, points this
           JUMP at it, and flips the root ONCE to the complete fresh pair.  No plot pass ever
           mixes frames (the old stale-floor bridge showed offset floors + fresh walls on the
           pre-flush vblanks = the flick).  Walls also land WITH the mask blit now instead of
           ~1 frame ahead.  In 1-cycle auto there is no head-start to lose: every vblank
           restarts the plot from the root. */
        end[0]  = 0x0009 | 0x1000;                   /* sysclip (non-drawing) + JUMP_ASSIGN */
        end[1]  = (unsigned short)((VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3);
        end[10] = 319; end[11] = 223;                /* keep the sysclip values (== root's) */
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext, end);
        ftex_wjump_addr = VDP1_BANK[vdp1_wbank] + (unsigned int)vdp1_wnext * 32u;
        ftex_flushed = 0;
        vdp1_bank = vdp1_wbank;                      /* the pair being BUILT (root flips at flush) */
        VDP1_PTMR = 0x0002;
        vdp1_wactive = 0;
        return;                                      /* root untouched: old coherent pair shows */
#else
        end[0] = 0x8000;
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext, end);
        link = (VDP1_BANK[vdp1_wbank] - VDP1_VRAM_BASE) >> 3;
        vdp1_bank = vdp1_wbank;
#endif
    }
    else
    {
        link = (VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3;
#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
        ftex_wjump_addr = 0;                         /* no wall list -> nothing to chain onto */
        ftex_flushed = 1;                            /* no level flush this frame */
#endif
    }
    /* atomic single-halfword flip of the root command's jump target */
    *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = (unsigned short)link;
    VDP1_PTMR = 0x0002;              /* start the draw (clears EDSR CEF until it finishes) */
#if VDP1_MANUAL_CHANGE
    if (vdp1_present_manual)
    {
        /* CEF-proven-this-frame: PTMR just cleared CEF; drain any stale latch (bounded) so the
           NEXT CEF=1 the vblank handler observes belongs to THIS plot, not the previous one. */
        for (int g = 0; (VDP1_EDSR & 0x0002) && g < 2000; ++g) { }
        vdp1_present_pending = 1;    /* arm the gated swap */
        vdp1_present_wait    = 0;    /* (re)start the stuck watchdog */
    }
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
#if VDP1_MANUAL_CHANGE
    /* Gated-present HOLD: while a plot is in flight (armed, not yet presented) do NOT rebuild the
       shared wall-texture VRAM cache (NOT double-buffered) that the live plot is still reading --
       drop this frame's accumulated walls and keep showing the last complete frame.  The vblank
       watchdog guarantees present_pending clears, so the walls never freeze.  (No-op while AUTO.) */
    if (vdp1_present_manual && vdp1_present_pending)
    {
#if VDP1_WALL_TEST
        wall_acc_n = 0;             /* discard this frame's BSP-accumulated walls */
#endif
        vdp1_kicked_this_frame = 1; /* suppress the DG_DrawFrame fallback kick */
        return;
    }
#endif
    {   /* SATURN PERF: time the VDP1 present (close bank + flip root LINK) -> overlay 'pr'. */
        unsigned short pk0 = frt_read();
        sat_vdp1_wpn_begin();       /* reset the bank + local-coord (walls flushed BETWEEN the copies) */
#if SAT_WPN_VDP1
        /* DOUBLE-EMIT the player weapon -- VDP1 has NO z-buffer: it rasterises every command into
           ONE framebuffer in painter order, so where the gun and a wall overlap the LAST-drawn
           command wins that pixel (its priority then only decides that pixel vs NBG1, it can't undo
           an overwrite).  So "on top of the walls" needs the weapon drawn AFTER them -- but "after"
           is the tail a HW 1-cycle-auto plot OVERRUN cuts (= the flicker: weapon drops, walls fine).
           On-top and no-flicker are in direct tension, so we draw the weapon TWICE:
             (1) FIRST, before the walls -> always in the plotted PREFIX, never fully flickers out;
                 the walls (drawn after) overwrite it only where they OVERLAP.
             (2) LAST, after the walls   -> redraws the overlap ON TOP.  If a plot overrun cuts (2),
                 copy (1) still shows the weapon -- gun-top just reverts to behind-the-walls that
                 frame.  Net: whole weapon ALWAYS visible; on top when the plot completes; only the
                 gun/wall overlap goes behind-walls in dense overrun frames.
           TODO (revisit): the clean fix is a VDP1 plot that always COMPLETES, so weapon-LAST alone
           is on top + flicker-free.  The manual / draw-gated present is a SETTLED DEAD END here
           (tried 4-5x, too many tradeoffs -- do NOT go back).  Find another route: shorten the wall
           list, a VDP1 command/time budget, or an L2-relocated cmd buffer.
           Emitted HERE (after the BSP walk, before the end-of-planes present -- the only pre-flip
           window); sat_psprite_early makes R_DrawMasked skip the late software draw.  SPLIT fans out
           per view (R_DrawSplitPlayerSprites).  sat_wall_skip gate -> M0 keeps the SOFTWARE weapon. */
        auto sat_emit_weapon = [](void)
        {
            if (sat_psprite_early && !viewangleoffset && sat_wall_skip)
            {
                extern int sat_split_active;
                extern void R_DrawSplitPlayerSprites(void);
                if (sat_split_active) R_DrawSplitPlayerSprites();   /* per-view weapons */
                else                  R_DrawPlayerSprites();
            }
        };
        sat_emit_weapon();          /* (1) FIRST -- guaranteed in the plotted prefix (anti-flicker) */
#endif
#if VDP1_WALL_TEST
        vdp1_walls_flush();         /* walls BETWEEN the two weapon copies */
#endif
#if SAT_WORLD_THINGS_VDP1
        /* World sprites to VDP1 prio 7, AFTER the walls (over them = clearly visible this probe;
           occlusion vs nearer walls is the FUNC_UserClip follow-up) and BEFORE weapon (2) so the
           gun stays on top (it is nearest).  Skips fuzz/translated + oversize -> those stay software. */
        R_EmitWorldThingsVDP1();
#endif
#if SAT_WPN_VDP1
        sat_emit_weapon();          /* (2) LAST  -- on top of the walls when the plot completes */
#endif
        vdp1_wpn_kick();
        sat_present_frt += (unsigned short)(frt_read() - pk0);
    }
    vdp1_kicked_this_frame = 1;
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
#if VDP1_WEAPON
        vdp1_wpn_init();
#endif
        SRL::Debug::Print(0, 1, "FRAME1 OK               ");
    }

#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
    if (ftex_job_ready)
    {
        /* Slave F-build: normally already TAKEN by the masked slave body (piggyback armed
           at the flush -- overlap = master's masked tail + DG pre); RP_AuxKick dispatches
           it as a plain aux job only when masked never ran this frame.  The pre-blit
           RP_AuxWait below joins either path, so the coherent pair still flips BEFORE the
           NBG1 mask blit (v7 ordering).  Only the COUNT re-arms here -- the entries stay
           frozen for the slave; next frame's first claim also joins before touching them. */
        ftex_job_ready = 0;
        RP_AuxKick();
        floor_acc_n = 0;
    }
#endif
    uint32_t df0 = DG_GetTicksMs();   /* SATURN PERF: DG_DrawFrame ms split (entry) */

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
#if VDP2_CELL_SKY
    if (skytexture > 0 && skytexture != sky_loaded_tex)
        sky_cell_upload();   /* re-pack the sky into B1 cells on level/episode change */
#if VDP2_SPLIT_HW_SKY
    /* Part 5 (docs/RBG0_SKY_SPLIT_ANALYSIS.md §5): in a co-op split, elect ONE view to receive the HW
       NBG0 sky (windowed to its band); the other views keep their software sky.  Static election = P1
       (view 0) -- couples with P1's HW floor in 2p; dynamic election (sat_sky_px_view[] + hysteresis,
       SKY_ELECT_HYST) is the documented next step, coverage already captured by d_main.  The core reads
       sat_sky_view in the NEXT frame's split loop (leaving that view's sky region index-0); the window
       + scroll below aim the single NBG0 layer at the SAME view THIS frame -- steady-state the choice is
       constant, so there is no phase skew.  Re-poke the W0 window only when the elected band changes. */
    int hwsky_split = (hwsky_split_on && sat_local_players >= 2 && gamestate == GS_LEVEL && !automapactive);
    sat_sky_view = hwsky_split ? 0 : -1;
    {
        static int sky_win_view = -2;   /* last NBG0-window state: -2 uninit, -1 full screen, 0..3 band */
        int want = hwsky_split ? sat_sky_view : -1;
        if (want != sky_win_view)
        {
            if (want < 0) nbg0_sky_window_clear();
            else          nbg0_sky_window_apply(want);
            sky_win_view = want;
        }
    }
#endif
#if SKY_FIXED
    slScrPosNbg0(toFIXED(0.0), toFIXED(-(double)VIEW_Y_OFFSET));   /* static backdrop */
#else
    {
        /* scroll the cell plane by viewangle; the 2x-tiled sky wraps seamlessly at the 512px page.  In a
           split with an elected HW-sky view, scroll by THAT view's angle (sat_sky_view_angle) rather
           than the global viewangle (which is the LAST-rendered view's). */
        unsigned int skyang = viewangle;
#if VDP2_SPLIT_HW_SKY
        if (hwsky_split) skyang = sat_sky_view_angle;
#endif
        int sx = -(int)(skyang >> (SKY_ANGLESHIFT + SKY_PARALLAX_SHIFT));
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
           cannot serve two split views -> the split renders the SOFTWARE sky instead
           (unless VDP2_SPLIT_HW_SKY elects one view for the HW sky -- see the block just below). */
        extern int sat_local_players;
        int show_sky = (gamestate == GS_LEVEL) && !automapactive && sat_frame_has_sky
                       && sat_local_players <= 1;
#if VDP2_SPLIT_HW_SKY
        /* Part 5: keep NBG0 on for the ELECTED split view whenever IT has visible sky this frame -- key
           on the elected view's OWN coverage (sat_sky_px_view[]), not the global sat_frame_has_sky which
           reflects only the last-rendered view.  The W0 window confines NBG0 to that view's band. */
        if (sat_local_players >= 2 && sat_sky_view >= 0 && sat_sky_view < 4
            && sat_sky_px_view[sat_sky_view] > 0)
            show_sky = 1;
#endif
#if VDP2_RBG0_TEST
        /* RBG0/debug 3-mode cycle (rbg0_mode, pad Y) -- see the rbg0_mode decl:
           0 = VDP2 floor, no dbg   (RBG0 on, NBG3 off, sw floor skipped)
           1 = dbg + software floor (RBG0 off, NBG3 on, sw floor drawn)
           2 = dbg, no software floor (RBG0 off, NBG3 on, sw floor skipped). */
        /* SATURN: the VDP2/RBG0 floor is ONE rotation plane -> use it ONLY at full detail
           (potato 0) and in 1-player; in any potato level OR split-screen it falls back to the
           SOFTWARE floor (sat_vdp2_floor=0 -> the sw floor draws; RBG0 display off). */
        int rbg0_active   = (sat_m != M0_SOFT) && (sat_local_players <= 1);
        int rbg0_split_p1 = RBG0_SPLIT_P1HW && (sat_m != M0_SOFT) && (sat_local_players == 2);   /* SATURN split: P1 floor in HW (left half), P2 software */
        int rbg0_on       = rbg0_active || rbg0_split_p1;
        { static int prev_split_p1 = -1;                        /* SATURN: force ONE flat re-upload on the 1p<->split */
          if (rbg0_split_p1 != prev_split_p1) {                 /* transition (the stale-pic root cause is fixed in core */
              rbg0_tex_dirty = 1; prev_split_p1 = rbg0_split_p1; } }  /* r_plane.c: sat_dom_last_sec dangled across level loads) */
        sat_split_p1hw    = rbg0_split_p1;                       /* core (d_main): punch the HW floor only for P1 in split */
        rbg0_floor_win_xend = rbg0_split_p1 ? 159 : 319;         /* window X: P1's left half in split, full screen in 1p */
        sat_vdp2_floor    = (rbg0_mode == 1 || !rbg0_on) ? 0 : 1;  /* 1p drives the punch; split is overridden per-view in d_main */
        uint16_t sky_bit  = (sat_vdp2_sky && show_sky) ? NBG0ON : 0;   /* HW sky bit gated on sat_vdp2_sky (M-owned): M0 => 0 => NBG0 off + core draws the software sky; M1..M4 => 1 => core leaves index-0, NBG0 shows through */
        uint16_t rbg0_bit = (RBG0_DISPLAY && rbg0_mode == 0 && rbg0_on) ? RBG0ON : 0;   /* HW floor: pot0 + (1p or 2p-split-P1) */
        uint16_t nbg3_bit = (RBG0_NBG3 && nbg3_show) ? NBG3ON : 0;  /* NBG3 overlay: display = pad L+R (default off); B1 cycle reserved at init */
        slScrAutoDisp((uint16_t)(sky_bit | NBG1ON | nbg3_bit | rbg0_bit));
#if VDP2_CELL_SKY && VDP2_SKY_FORCE_CYC
        sky_cell_force_cyc(sky_bit, nbg3_bit);   /* RBG0-on makes the allocator put NBG0's char in A1; force it back to B1 */
#endif
#else
        slScrAutoDisp((uint16_t)(show_sky ? (NBG0ON | NBG1ON | NBG3ON)
                                          : (NBG1ON | NBG3ON)));
#endif
        if (show_sky)
            for (int i = 192 * 320; i < 224 * 320; ++i)   /* status-bar rows (224: 192..223) */
                if (framebuffer[i] == 0) framebuffer[i] = nb;
    }

    rbg_sky_sum += DG_GetTicksMs() - df0;   /* SATURN PERF: 'sky' = sky scroll + cmap + slScrAutoDisp */

#if VDP2_RBG0_TEST
    /* When the floor toggle is on: upload the player's floor texture to RBG0 (only when the
       flat changes), then re-write its rotation params from the matrix each frame.
       NOTE: slScrMatSet only fills SGL's CACHED RAM buffer + a dirty flag; the RPT VRAM transfer is
       done by the _BlankIn ISR, armed ONLY by slSynch (disasm-proven, docs/RBG0_STRUCTURED_GARBAGE.md).
       So the transform never reaches VRAM without RBG0_RPT_TRANSFER below. */
    if (rbg0_mode == 0 && sat_vdp2_floor)   /* sat_vdp2_floor folds in the pot0 + 1p gate (set above) */
    {
        uint32_t rb_t0 = DG_GetTicksMs();
        rbg0_upload_flat(sat_vdp2_floor_pic);   /* per-sector light is now BAKED (quantized) into the texels */
        /* (The RBG0 bitmap palette-bank switch was removed: slBMPaletteRbg0/BMPNB never reach the chip
           on the no-slSynch path -- the floor stayed full-bright.  Dimming is baked in rbg0_upload_flat.) */
        uint32_t rb_t1 = DG_GetTicksMs();
#if RBG0_FLOOR_AUTO_HORIZON && VDP2_CELL_SKY
        /* FLOOR HORIZON -- pure geometry, NOTHING to do with colours (extracted out of the line-color
           block).  horizon = max(player_height_horizon, floor_top), in screen rows:
             - player_height_horizon = 96 + (fhw+56)*3/23, HW-calibrated on the PLAYER's view-sector floor
               height (sat_view_floor_h, NOT the dominant) -- the UPPER bound; the floor never goes above it;
             - floor_top (sat_vdp2_floor_top_y) only pulls it DOWN, and only when the real floor doesn't
               reach that horizon.
           One sky_horizon_row holds the result; sky_cell_build_map rebuilds the sky boundary AND the floor
           window from it TOGETHER -> the sky ALWAYS comes down exactly to the floor (no decalage), whoever
           limited last.  Rebuild only when the 8px sky-cell row changes. */
        { static int last_xend = -1;
          int ft  = sat_vdp2_floor_top_y;
          int hz;
#if RBG0_SPLIT_P1HW
          if (sat_split_p1hw)
              hz = rbg0_split_hz;   /* split: 1p height-formula mis-calibrated for P1's 160-tall viewport -> live split horizon (R+Up/Down) */
          else
#endif
          {
              int fhw = sat_view_floor_h >> 16;
              hz = 96 + ((fhw + 56) * 3) / 23;             /* 1p player-height horizon -- now only the NO-FLOOR fallback */
              if (hz < 8) hz = 8; else if (hz > 128) hz = 128;
              if (ft > 0 && ft < 0x3FFF) hz = ft;          /* owner 2026-07-02: the ACTUAL floor top is the SOLE reference (removed the "&& ft > hz" player-height clamp -> the plane length follows the real floor, not min(floor, player-height)) */
          }
          /* re-run sky_cell_build_map (sky boundary + floor window) when the horizon cell OR the window X
             extent changes (the latter on a 1p<->split toggle) so the window tracks P1's half. */
          if ((hz >> 3) != (sky_horizon_row >> 3) || rbg0_floor_win_xend != last_xend)
              { last_xend = rbg0_floor_win_xend; sky_horizon_row = hz; sky_cell_build_map(); } }
#endif
#if RBG0_LINECOL_TEST
        /* Line-color fog/veil tables ONLY (per-distance darkening -- PARKED, rbg0_linecol_mode=0).  This
           block is now PURELY colours; the horizon is owned by the floor-horizon block above.  Rebuild the
           tables when the (externally-set) horizon or the sector light band changes. */
        { static int lc_hz = -1, lc_band = -1;
          if (sky_horizon_row != lc_hz || sat_vdp2_floor_band != lc_band) {
              lc_hz = sky_horizon_row; lc_band = sat_vdp2_floor_band;
              rbg0_linecol_rebuild(); rbg0_ccwin_rebuild();
          } }
#endif
        if (sat_split_p1hw) sat_setup_view_p1();   /* split: re-anchor the view globals on P1 before the RBG0 transform */
        rbg0_set_transform();
        uint32_t rb_t2 = DG_GetTicksMs();
#if RBG0_RPT_TRANSFER == 1
        slSynch();   /* Test A: per-frame slSynch -> _BlankIn transfers the RPT.  Confirms the cause
                        (the floor should warp into perspective), but caps fps + mutes SCSP SFX. */
#elif RBG0_RPT_TRANSFER == 2
        /* Test B (the real fix): reproduce _BlankIn's RPT DMA, NO slSynch.  Source = SGL's RAM RPT
           buffer read via the UNCACHED 0x26 alias (so slScrMatSet's cached stores are seen); dest =
           the RPT VRAM at VDP2_VRAM_B1 + 0x1ff00.  0x30 bytes/plane (RA, then RB at +0x68). */
        /* RA RPT: copy the FULL parameter set slScrMatSet writes -- XST..KY (0x54 bytes) -- so the off-centre
           reprojection from slDispCenterR (consistent Xst/Px/Cx/matrix) reaches VRAM as ONE consistent block.
           The old 0x30 copy left Px/Cx/Mx/kx at their init values -> a "Frankenstein" table (start computed
           for centre 80 but Cx still 160) -> the half-texel per-frame jitter.  Stop BEFORE KAST (@0x54): the
           coefficient-table address is written once at init (slKtableRA), NOT by slScrMatSet -- copying it
           would clobber the K-table.  RB (unused) stays 0x30. */
        memcpy((void *)0x25E7FF00,          (const void *)0x260FFE1C, 0x54);
        memcpy((void *)(0x25E7FF00 + 0x68), (const void *)0x260FFE84, 0x30);
#endif
        uint32_t rb_t3 = DG_GetTicksMs();   /* SATURN PERF: split upl/xfm/rpt to pin the stall */
        rbg_upl_sum += rb_t1 - rb_t0;
        rbg_xfm_sum += rb_t2 - rb_t1;
        rbg_rpt_sum += rb_t3 - rb_t2;
    }
#endif

#if VDP1_WEAPON
    /* LAYER INVERSION: VDP1 carries ONLY the walls (below NBG1).  During a LEVEL render the
       early hook (sat_walls_kick, right after the BSP walk) already flushed+kicked so VDP1
       presents the same frame.  Only kick HERE when it did NOT fire (menu/intermission: no
       R_RenderPlayerView) -> the empty bank clears any stale walls.  Both before the
       palette_changed reset so the wall cache re-tints on a damage/pickup flash. */
    if (!vdp1_kicked_this_frame) {
        unsigned short pk0 = frt_read();   /* SATURN PERF: present-kick ms (menu/intermission path) */
        sat_vdp1_wpn_begin(); vdp1_wpn_kick();
        sat_present_frt += (unsigned short)(frt_read() - pk0);
    }
#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
    vdp1_ftex_flush();   /* FALLBACK only (ftex_flushed-guarded): the normal build+flip runs
                            from sat_floors_done_hook at the end of R_DrawPlanes; this covers
                            split screen (hook defers there) and any path that skipped it. */
#endif
    /* M0 = PURE SOFTWARE reference: everything is drawn in the framebuffer (NBG1), so VDP1 must show
       NOTHING.  Force the root to the EMPTY bank every M0 frame -> no stale VDP1 quads (walls /
       weapon / world things) from the last VDP1-mode frame stay frozen on screen when the user A/Bs
       to software (the coherent-pair flip was leaving the old pair rooted on the M4->M0 switch). */
    if (sat_m == M0_SOFT)
        *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) =
            (unsigned short)((VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3);
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

    /* CPU blit (the only viable path -- SCU-DMA to VDP2 hangs the bus).  Purge
       first so the master sees the slave's write-through framebuffer pixels. */
    /* Menus/title/intermission are 320x200 assets; on the 224 framebuffer rows 200..223 are
       uncovered.  Outside a level (no ST_Drawer), blacken that strip so it's not stale garbage. */
    if (gamestate != GS_LEVEL)
        memset(framebuffer + 200 * 320, 0, 24 * 320);
    {
        /* SATURN split-screen HUD, painted into the framebuffer before the blit.  Gate on
           the SAME predicate d_main uses to split-render (a real co-op game, in a level,
           not the full-screen automap) -- else a stale sat_local_players on the demo/attract
           loop, or an open automap, would get a split HUD painted over a view d_main did NOT
           split-render.  usergame is externed as int elsewhere in this file (matches core). */
        extern int sat_local_players, usergame;
        if (sat_local_players > 1 && usergame && gamestate == GS_LEVEL && !automapactive)
        {
            if (sat_local_players == 2)
            {
                /* 2p: two 160x64 compact-HUD panels in the bottom 64 rows (P1 left, P2 right),
                   each player's widgets on top, then a per-half damage/pickup flash.  W5 (runtime
                   blit_cfg[].w5): only repaint (and mark the band dirty) when a player's HUD
                   signature changed -- else skip the paint AND the blit skips [160,224).  w5=0:
                   always repaint (the flag then also always blits, below). */
                static unsigned int w5_2p_sig = ~0u;
                int w5_2p = blit_cfg[blit_mode].w5;
                unsigned int sig = ST_SplitHudSig();
                if (!w5_2p || sig != w5_2p_sig)
                {
                    w5_2p_sig = sig;
                    hud2p_blit_panels();
                    ST_DrawCompactWidgets(0, 0,   HUD2P_TOP);   /* P1 (left)  */
                    ST_DrawCompactWidgets(1, 160, HUD2P_TOP);   /* P2 (right) */
                    hud2p_apply_flash();
                    sat_hud_dirty = 1;   /* the HUD band changed -> blit it this frame */
                }
            }
            else
            {
                /* 3/4p: each 160x112 quadrant is a 160x96 view + a 16px compact HUD band at its
                   bottom (band top = quadrant top + 96).  Paint the opaque band + that player's
                   widgets, then the per-quadrant flash.  In 3p the 4th quadrant is the minimap
                   (no player) -> only views 0..n-1 get a band, and the flash is passed n so it
                   skips the minimap. */
                static const short qx[4] = { 0, 160, 0,   160 };
                static const short qy[4] = { 0, 0,   112, 112 };
                int n = sat_local_players; if (n > 4) n = 4;
                for (int q = 0; q < n; ++q)
                {
                    int oy = qy[q] + (HUD4P_QUAD_H - HUD4P_H);   /* = qy + 96: band at the view bottom */
                    hud4p_blit_band(qx[q], oy);
                    ST_DrawQuadHud(q, qx[q], oy);
                }
                hud4p_apply_flash(n);
            }
        }
    }
#if VDP1_MANUAL_CHANGE
    /* Brick B -- couple NBG1 to the VDP1 frame (separate toggle R+Z; layers over PA or PM).  Hold
       the just-rendered software framebuffer: wait (bounded) for THIS frame's wall plot to finish,
       latch the manual present, then fence so the blit lands on the swap vblank -> VDP1-N and
       NBG1-N go live on the SAME field (kills the decrochage).  The wait is the deterministic fps
       cost.  Bounded by VDP1_COUPLE_MAX_VBL so a never-finishing plot (or Ymir, no CEF) can't hang. */
    if (vdp1_couple_nbg1 && gamestate == GS_LEVEL)
    {
        unsigned int t0 = vbl_count;
        while (!(VDP1_EDSR & 0x0002) && (vbl_count - t0) < VDP1_COUPLE_MAX_VBL) { }
        if (vdp1_present_manual && vdp1_present_pending)
        {
            VDP1_FBCR            = 0x0003;   /* latch the manual change now (executes next vblank) */
            vdp1_present_pending = 0;
            vdp1_present_wait    = 0;
        }
        { unsigned int tf = vbl_count; while (vbl_count == tf) { } }   /* fence to the swap vblank */
    }
#endif
    uint32_t df1 = DG_GetTicksMs();        /* SATURN PERF: ms split -- end of pre, start of blit */
#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
    /* BLIT<->F-BUILD DECOUPLE (R+Z).  ftex_overlap_flip is set iff the slave build was ARMED to
       skip its flip -> the master owns the present flip and must do it AFTER the blit (acting on
       what was armed, not the live toggle, so a mid-frame toggle never strands the flip). */
    if (ftex_overlap_flip)
    {
        /* OVERLAP: single-CPU blit CONCURRENT with the slave F-build (tiles + W->F chain, NO flip),
           then join and flip -- the flip stays after the blit (coherent), the copy hides in the wait. */
        unsigned short bt0 = frt_read();
        cache_purge();
        for (int y = 0; y < 224; ++y)
            memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
        RP_AuxWait();                                                      /* slave done: tiles + chain */
        *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = ftex_job.wroot; /* master present, AFTER the blit */
        ftex_overlap_flip = 0;
        sat_blit_ms10 = ((unsigned int)(unsigned short)(frt_read() - bt0) * ns_per_frt) / 100000u;
    }
    else
#endif
    {
#if VDP1_WALL_TEST && SAT_VDP1_FLOOR && SAT_FLOOR_TEX
    /* JOIN the slave F-build BEFORE the NBG1 mask blit (review finding: without this the slave's
       root flip landed ~35ms into game logic, AFTER the mask went live = the v7 mask-vs-pair slip). */
    RP_AuxWait();
#endif
    unsigned short blit_t0 = frt_read();   /* SATURN PERF: time the blit (-> sat_blit_ms10) */
    /* W5: split the copy at hud_top (= the clear boundary).  [0,hud_top) is the re-rendered
       3D view -> always blit.  [hud_top,224) is the HUD band -> blit only when it changed
       (core sat_hud_dirty / 2p signature) or an overlay may have painted over it. */
    int hud_top = (sat_local_players >= 3) ? 224 : (sat_local_players == 2 ? 160 : 192);
    /* W5 is the runtime blit_cfg[].w5 axis (pad L+A).  Off -> blit all 224.  On -> blit the HUD
       band only when it changed, or an overlay/layout change may have painted over it. */
    static int w5_last_players = -1;
    int hud_force = (sat_local_players != w5_last_players)
                 || menuactive || (gamestate != GS_LEVEL);   /* overlays / layout change */
    w5_last_players = sat_local_players;
    int hud_blit = !blit_cfg[blit_mode].w5 || sat_hud_dirty || hud_force;
    if (blit_cfg[blit_mode].dma)
    {
        /* Inc 1 (docs/BLIT_DMA_PLAN.md) -- synchronous on-chip-DMAC blit via SGL slDMACopy, the
           wolf4sdl pattern: one slDMACopy per row (HWRAM framebuffer -> VDP2 VRAM, 320 B) then a
           single slDMAWait.  The framebuffer is write-through so RAM is already current -> NO
           cache_purge (unlike the CPU paths below).  slDMACopy = the SH-2 ON-CHIP DMAC (disasm-
           proven; spins-per-row -> bus-bound, HW-confirmed no win).  UNVALIDATED-async paths ->
           boot stays single-CPU (blit_mode 0); L+A opts in.  Watch for RBG0 snow (B-bus contention
           boot stays single-CPU (blit_mode 0); L+A opts in.  Watch for RBG0 snow (B-bus contention
           with the VDP2 fetch) + Dr (VDP1 shares the bus) when validating. */
        for (int y = 0; y < hud_top; ++y)                     /* 3D view: always */
            slDMACopy(framebuffer + y * 320,
                      DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, 320);
        if (hud_blit)
            for (int y = hud_top; y < 224; ++y)               /* HUD band: only when changed (W5) */
                slDMACopy(framebuffer + y * 320,
                          DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, 320);
        slDMAWait();
    }
    else
    {
        /* Single-CPU blit: master copies the picture.  W5: 3D-view
           rows always, HUD band only when changed. */
        cache_purge();
        for (int y = 0; y < hud_top; ++y)
            memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
        if (hud_blit)
            for (int y = hud_top; y < 224; ++y)
                memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
    }
    /* W5: the HUD band in VRAM now matches the framebuffer -> clear the dirty flag; the core
       (1p) / the 2p signature re-sets it next frame only if the HUD changes again. */
    if (hud_blit)
        sat_hud_dirty = 0;
    {
        /* SATURN PERF: blit wall-clock = master FRT delta across the copy (incl. slave join). */
        unsigned short blit_t1 = frt_read();
        sat_blit_ms10 = ((unsigned int)(unsigned short)(blit_t1 - blit_t0) * ns_per_frt) / 100000u;
        if (blit10_cnt < BLIT10_CAP) { blit10_sum += sat_blit_ms10; blit10_cnt++; }  /* row-1 `b` precise A/B */
    }
    }   /* end non-overlap blit path (the flip-before-blit default) */
    uint32_t df2 = DG_GetTicksMs();        /* SATURN PERF: ms split -- end of blit, start of clear */
    /* LAYER INVERSION: clear the 3D VIEW to index 0 so next frame the SKIPPED wall columns stay
       transparent -> the VDP1 walls (below NBG1) show through.  The HUD rows are left intact
       (1p: status bar 192..223 owned by ST_Drawer; 2p: panels 160..223 owned by hud2p).  3/4p:
       clear all 224 -- the compact HUD bands + minimap are repainted opaque each frame before
       the blit, so clearing their rows here (they're interleaved per quadrant) is harmless. */
    {
        extern int sat_local_players;
        int clear_rows = (sat_local_players >= 3) ? 224 : (sat_local_players == 2 ? 160 : 192);
        memset(framebuffer, 0, clear_rows * 320);
    }
    {   /* SATURN PERF: bank the master-frame composition (window-averaged once/sec by fps_update).
           df_pre/blit/post are DG_DrawFrame's own ms split; tic/snd come from the core this tick;
           present is the VDP1 kick FRT accumulated during render + this DG call.  sat_present_frt is
           reset AFTER banking so next frame's early kick (sat_walls_kick, during render) accumulates
           cleanly into it before the next bank. */
        uint32_t df3 = DG_GetTicksMs();
        df_pre_sum  += df1 - df0;
        df_blit_sum += df2 - df1;
        df_post_sum += df3 - df2;
        df_tic_sum  += (unsigned int)sat_tic_ms;
        df_snd_sum  += (unsigned int)sat_snd_ms;
        df_present_sum += ((unsigned int)sat_present_frt * ns_per_frt) / 100000u;  /* FRT -> tenths-ms */
        sat_present_frt = 0;
        df_frames++;

        /* SESSION percentile metrics: one sample per frame.  RESET on a MODE change (sat_m / SQ) so
           the histograms describe the whole run at the current mode (not the level).  Frame time =
           wall delta between DG_DrawFrame calls (== MST). */
        {
            static int mh_l_m = -999, mh_l_sq = -1;
            static uint32_t mh_last = 0;
            int cur_sq = (sq_wall << 4) | (sq_floor << 2) | sq_ceil;
            if (sat_m != mh_l_m || cur_sq != mh_l_sq) { mh_reset(); mh_l_m = sat_m; mh_l_sq = cur_sq; }
            uint32_t nowms = df3;                       /* end-of-frame timestamp (DG_GetTicksMs) */
            int fms = mh_last ? (int)(nowms - mh_last) : 0;
            mh_last = nowms;
            if (fms > 0) mh_add(fms, sat_things_n, sat_things_decl, sat_things_occ, thing_bake_n);
        }
    }
    return;
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
#if VDP1_MANUAL_CHANGE
        if (!(cur & PER_DGT_TL))             /* L+Z: A/B the VDP1 present (row 2 'P A'<->'P M') */
        {
            vdp1_present_manual = !vdp1_present_manual;
            if (!vdp1_present_manual)
            {
                VDP1_FBCR            = 0x0000;   /* back to 1-cycle auto swap */
                vdp1_present_pending = 0;
                vdp1_present_wait    = 0;
            }
        }
        else if (!(cur & PER_DGT_TR))        /* R+Z: A/B the NBG1<->VDP1 couple (brick B; row 2 2nd char 'C'/'-') */
        {
            vdp1_couple_nbg1     = !vdp1_couple_nbg1;
            vdp1_present_pending = 0;        /* drop any in-flight gated arm so the two paths don't fight */
            vdp1_present_wait    = 0;
        }
        else
#endif
        {   /* Z: cycle only the LIVE modes {M0, M4, M6}; M1/M2/M3/M5 are parked (off the cycle). */
            int ci = 0;
            for (int i = 0; i < SAT_M_CYCLE_N; ++i) if (sat_m_cycle[i] == sat_m) { ci = i; break; }
            sat_m = sat_m_cycle[(ci + 1) % SAT_M_CYCLE_N];
            sat_apply_mode();
        }
    }

    /* Pad L+A: live A/B of the framebuffer BLIT (docs/BLIT_DMA_PLAN.md) -- toggles W5 (HUD-skip)
       on/off across the safe ring c5 <-> c- (CPU blit; the ~1.3ms W5 A/B).  Row-1 'b<ms>c<-/5>'.
       The SCU-DMA paths (s/a) are OFF-RING: HW-DEAD, cycling to them FROZE the console (SCU->VDP2
       B-bus hang, 2026-07-05).  L held + A (edge); NOT L+R (the debug-overlay cycle).  The
       incidental fire taps to Doom are harmless (mirrors the R+A wall-clamp chord). */
    if (!(cur & PER_DGT_TL) && (changed & PER_DGT_TA) && !(cur & PER_DGT_TA))
    {
        blit_cycle_i = (blit_cycle_i + 1) % BLIT_CYCLE_N;
        blit_mode = blit_cycle[blit_cycle_i];
    }

    /* Pad R + Up/Down tunes the VERTICAL plane-decrochage fill scale (sat_plane_vscale, r_main.c): Up = MORE
       vertical fill (bob / stairs / lifts), Down = LESS.  Clamped [0,16].  Live value on overlay row 5.  R is
       the held modifier (R+C = M5 A/B, uses C -- no clash); the incidental Up/Down player-nudge during tuning
       is minor, same as the L+Up/Down sky-horizon knob. */
    if (!(cur & PER_DGT_TR))   /* R held */
    {
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU) && sat_plane_vscale < 16) sat_plane_vscale++;
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD) && sat_plane_vscale > 0)  sat_plane_vscale--;
        /* R + Left/Right tunes the decrochage border CAP (sat_plane_border_max, r_main.c): the px
           ceiling both borders saturate at during a fast turn.  With textured VDP1 planes the old
           40px ceiling blanketed any plane narrower than 80px in potato colour; the low default
           (10) keeps the texture visible at the price of a thin stale strip.  Row 8 `bM`. */
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR) && sat_plane_border_max < 40) sat_plane_border_max += 2;
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL) && sat_plane_border_max > 0)  sat_plane_border_max -= 2;
    }

    /* (R+C M5 BSP-staging A/B cut -- settled-negative; the staging mechanism stays inert in core.
       R+C is now the CEILING SQ knob below; C alone still cycles the plane-split pmode.) */

#if VDP2_RBG0_TEST
    /* Pad R + Y cycles the WALL software quality SQ (full/ld/band/flat), applied to the CPU wall
       fallback + the VDP1 wall style (band/flat).  (Was the RBG0 floor A/B -- folded into the M
       axis.)  R taps '.' to Doom -- the established diag-chord pattern. */
    if (!(cur & PER_DGT_TR) && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    { sq_wall = (sq_wall + 1) & 3; sat_apply_mode(); }
#if !RBG0_TUNE_PAD   /* was gated on !RBG0_LINECOL_TEST too -- a coherence bug: the parked line-color
                        fog (mode 0, never on C) has nothing to do with the plane-split toggle, and
                        RBG0_LINECOL_TEST=1 was silently killing pad C.  Only RBG0_TUNE_PAD really uses C. */
    /* Pad C cycles the plane-split mode (overlay row1 'pm'): 0 = static half-split, 1 = TAS plane
       work-steal, 2 = ROW-SPLIT (the universal balancer -- both SH-2 split the screen ROWS, so a
       dominant plane is balanced too, which pm0/pm1 cannot).  Watch w / SLVidle (row 3): pm2 should
       drop the master-wait in dominant-plane rooms.  (C also taps RSHIFT/run -- harmless.) */
    /* (L/R excluded: L+C = HW split sky, R+C = M5 staging A/B -- C alone cycles pmode.) */
    if ((cur & PER_DGT_TL) && (cur & PER_DGT_TR) && (changed & PER_DGT_TC) && !(cur & PER_DGT_TC))
    {
        static int pmode = 1;   /* boot default = TAS (sat_plane_tas=1); first press -> row-split */
        pmode = (pmode + 1) % 3;
        sat_plane_tas      = (pmode == 1);
        sat_plane_rowsplit = (pmode == 2);
    }
#endif
#if RBG0_NBG3
    /* Pad L+R (chord) toggles the NBG3 debug overlay (default OFF).  The B1 cycle is reserved at
       init (slScrAutoDisp(NBG3ON) + no scrub), so this only flips BGON.  (L/R also tap ','/'.' to
       Doom -- harmless; L+R is free since SAT_DIAG_SLAVE_TOGGLES=0.) */
    {
        const unsigned short lr = (unsigned short)(PER_DGT_TL | PER_DGT_TR);
        static int lr_was = 0;
        int lr_now = ((cur & lr) == 0);          /* both held (active-low) */
        if (lr_now && !lr_was) {
            /* Cycle the debug overlay: 0 full perf overlay -> 1 fps-only (measures the overlay's
               own per-frame tax via the mode0<->mode1 fps delta -- gates the r_parallel per-frame
               rows AND the RP_PlanePixels rescan) -> 2 off (layer hidden).  nbg3_show hides the
               whole NBG3 text layer only in mode 2. */
            sat_dbg_overlay_mode = (sat_dbg_overlay_mode + 1) % 3;
            nbg3_show = (sat_dbg_overlay_mode != 2);
        }
        lr_was = lr_now;
    }
#endif
#if VDP2_SPLIT_HW_SKY
    /* Pad L + C toggles the Part 5 HW split sky (docs/RBG0_SKY_SPLIT_ANALYSIS.md §5): OFF (default) =
       every split view draws the software sky; ON = the elected view (P1) gets the hardware NBG0 sky,
       windowed to its band.  Only meaningful in a co-op split (inert in 1p: hwsky_split needs
       sat_local_players>=2).  Edge-triggered on C while L is held (R NOT held, so it never collides
       with the L+R nbg3 chord); the incidental ',' (L) and run (C) taps to Doom are harmless.  This is
       the live A/B for the HW path, which is not yet validated on real Saturn. */
    if (!(cur & PER_DGT_TL) && (changed & PER_DGT_TC) && !(cur & PER_DGT_TC))
        hwsky_split_on = !hwsky_split_on;
#endif
#if VDP2_CELL_SKY && !RBG0_LINECOL_TEST
    /* Pad L + Up/Down nudges the HW-sky horizon (the row where the sky turns transparent so the
       floor shows below it), 8px (one cell row) per press.  Up raises the horizon up the screen,
       Down lowers it.  (L taps ',' and Up/Down also move the player -- minor during tuning.)
       Bake the found value into SKY_HORIZON_ROW and remove this block once tuned. */
    if (!(cur & PER_DGT_TL))   /* L held */
    {
        int adj = 0;
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) adj = -8;  /* Up: raise the horizon  */
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) adj = +8;  /* Down: lower the horizon */
        if (adj)
        {
            sky_horizon_row += adj;
            if (sky_horizon_row < 8)   sky_horizon_row = 8;
            if (sky_horizon_row > 128) sky_horizon_row = 128;
            sky_cell_build_map();
        }
    }
#endif
    /* (WIP: the L/R/C live floor-tuning toggles for the parked distance-gradient were removed for the
       bake-only ship.  rbg0_floor_dim/contrast + the veil params keep their baked defaults.  Re-add
       these handlers + the pad_map movement gate below to resume tuning the gradient.) */
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
#if RBG0_SPLIT_P1HW && !RBG0_TUNE_PAD && RBG0_SPLIT_TUNE
    /* SATURN split viewport TUNING (R + d-pad, split only): live-cal the RBG0 reprojection for P1.
       Up/Down = centre Y (horizon row, +-2); Left/Right = near-plane depth (+-16).  Values on the VPW
       overlay row.  Bake the found values into rbg0_win_cy / rbg0_win_depth once tuned.  (R also taps
       '.' to Doom + the d-pad moves P1 -- harmless during tuning.) */
    if (sat_split_p1hw && !(cur & PER_DGT_TR)) {
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) rbg0_split_hz  -= 8;   /* raise the split floor horizon (W1 clip) */
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) rbg0_split_hz  += 8;   /* lower it */
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL)) rbg0_split_cx -= 4;  /* move centre left  */
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR)) rbg0_split_cx += 4;  /* move centre right */
        if (rbg0_split_hz < 8) rbg0_split_hz = 8; else if (rbg0_split_hz > 200) rbg0_split_hz = 200;
        if (rbg0_split_cx < 0) rbg0_split_cx = 0; else if (rbg0_split_cx > 160) rbg0_split_cx = 160;
    }
    /* part 2 (L + d-pad, split only): Up/Down = PLANE PITCH = the inclination (proven matrix knob via
       slRotX; note it also tilts the 1p floor since rbg0_pitch_adj is shared -> bake into RBG0_PITCH).
       Left/Right = projection centre X (slWindow).  If PITCH moves the floor but cx/cy/depth do NOT,
       then slWindow is not reaching the RBG0 rotation -> switch to patching the RPT directly. */
    if (sat_split_p1hw && !(cur & PER_DGT_TL)) {
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) rbg0_split_pitch += 0x40;  /* steeper */
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) rbg0_split_pitch -= 0x40;  /* flatter */
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL)) rbg0_split_sd -= 1;   /* smaller dist => wider FOV (Q4) */
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR)) rbg0_split_sd += 1;   /* larger dist => narrower FOV   */
        if (rbg0_split_sd < 3) rbg0_split_sd = 3; else if (rbg0_split_sd > 24) rbg0_split_sd = 24;
    }
    /* part 3 (C + d-pad, split only): Left/Right = floor ORIENTATION (yaw offset -> "forward scrolls left");
       Up/Down = yaw-RATE scale (fixes rotation-x2-too-fast when turning; cy is baked). */
    if (sat_split_p1hw && !(cur & PER_DGT_TC)) {
        if ((changed & PER_DGT_KL) && !(cur & PER_DGT_KL)) rbg0_split_yaw -= 0x100;  /* rotate floor CCW */
        if ((changed & PER_DGT_KR) && !(cur & PER_DGT_KR)) rbg0_split_yaw += 0x100;  /* rotate floor CW  */
        if ((changed & PER_DGT_KU) && !(cur & PER_DGT_KU)) rbg0_split_scroll += 1;   /* faster scroll (Q4) */
        if ((changed & PER_DGT_KD) && !(cur & PER_DGT_KD)) rbg0_split_scroll -= 1;   /* slower scroll (Q4) */
        if (rbg0_split_scroll < 2) rbg0_split_scroll = 2; else if (rbg0_split_scroll > 48) rbg0_split_scroll = 48;
    }
#endif
#endif
#if SAT_FLOOR_TEX
    /* Pad Y (alone, L/R released) cycles the FLOOR software quality SQ, applied to the software
       floor spans + the VDP1-floor CPU-fallback slivers.  Flats cycle full/ld/flat -- band is
       skipped (meaningless for a flat: the potato span is already distance-shaded per row). */
    if ((cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    { sq_floor = (sq_floor == SQ_FULL) ? SQ_LD : (sq_floor == SQ_LD) ? SQ_FLAT : SQ_FULL; sat_apply_mode(); }
    /* Pad L+Y (R released) cycles the CEILING software quality SQ (full/ld/band/flat), independent
       of the floor (core sat_ceil_potato/sat_ceil_ld).  (Was the slave-F-build A/B -- sl1 kept as
       the compile default.)  Active-low: !(cur&TL) = L held. */
    if (!(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    { sq_ceil = (sq_ceil == SQ_FULL) ? SQ_LD : (sq_ceil == SQ_LD) ? SQ_FLAT : SQ_FULL; sat_apply_mode(); }
    /* Pad R+A: live A/B of the Phase-1 WALL CLAMP (partially-occluded tiers kept on VDP1 via the
       world-anchored cut + software wedge, core sat_wall_cut_floor/_ceil).  Row 6 W<n><+/-> = tiers
       kept + state.  (MOVED off L+R+Y -- the L+R chord fires the overlay toggle, so L+R+Y was
       unreachable.)  R held + A; the incidental fire tap to Doom is harmless. */
    if (!(cur & PER_DGT_TR) && (changed & PER_DGT_TA) && !(cur & PER_DGT_TA))
        sat_wall_clamp ^= 1;
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

#if SAT_DIAG_SLAVE_TOGGLES
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
            /* (WIP: the L/R/C tuning-movement freeze for the parked floor-gradient tuner was removed
               with the toggles -- the player moves normally now.) */
#if RBG0_SPLIT_P1HW && RBG0_SPLIT_TUNE
            /* SATURN split HW-floor tuning: while L or R is held the d-pad tunes the floor (above) ->
               do NOT also forward it to Doom, so the view stays put during calibration.  Split only
               (sat_split_p1hw) -> normal play keeps full d-pad movement. */
            if (sat_split_p1hw && (!(cur & PER_DGT_TL) || !(cur & PER_DGT_TR) || !(cur & PER_DGT_TC)) &&
                (pad_map[i].mask == PER_DGT_KU || pad_map[i].mask == PER_DGT_KD ||
                 pad_map[i].mask == PER_DGT_KL || pad_map[i].mask == PER_DGT_KR))
                continue;
#elif RBG0_TUNE_PAD
            /* PARKED (RBG0_TUNE_PAD): while L/R held the d-pad tunes the floor -- don't forward to Doom. */
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
