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
#include "hud4p_panel.h"   /* generated 3/4-player compact HUD band (brushed-metal bg) + anchors */

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



/* Aux slave-job API (core r_parallel).  Used by the framebuffer clear-on-slave and joined
   before every SGL rewind.  RP_AuxArm/RP_AuxKick went with the ftex F-build (2026-08-02). */
extern "C" void RP_AuxDispatch(void (*fn)(void));
extern "C" void RP_AuxWait(void);
/* SATURN VDP1 layer routing + ISOLATION modes (core r_things.c / r_segs.c).  The real VDP1 limiter
   is command count + OVERDRAW (near walls iterate off-screen spans -> "transfer-over", SEGA VDP1 UM
   p.53), NOT pixel fill (fill is abundant, ~2.5ms/screen).  So the old distance-prioritized FILL
   budget was REMOVED 2026-07-25 -- the HW test proved the weapon-fill lever inert.  What stays are
   the LAYER gates the isolation cycle uses to find WHAT overruns the list: sat_things_hw (core,
   world sprites off VDP1) and sat_iso_flat (every VDP1 wall FLAT).  See docs/VDP1_LIMITS_SOURCED.md.
   sat_wpn_soft (core r_things.c) is now PERMANENTLY 0: the L+X chord that set it was CUT 2026-08-02
   -- its ON state left the walls un-re-cleared (HW-confirmed) and the weapon-fill lever measured
   inert, so it was a glitch with no upside.  Still declared because core owns and reads it. */
extern "C" int sat_wpn_soft;
extern "C" int sat_thing_vdp1_fill, sat_thing_vdp1_kept, sat_thing_vdp1_spill;
/* VDP1 isolation (pad L+Z, 1p): 0 all / 1 no-things / 2 flat-walls.  Applied by sat_apply_iso()
   (drives sat_things_hw + sat_iso_flat); re-applied at the end of sat_apply_mode so a mode cycle
   preserves it.  0 = no change (byte-identical to ship). */
static int sat_iso_mode = 0;
static int sat_iso_flat = 0;     /* mode 2: every VDP1 wall -> FLAT (overdraw-vs-fill discriminator) */
static void sat_apply_iso(void); /* fwd: defined near the pad handler */

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
extern "C" int   r_solidseg_peak;  /* core r_bsp.c: solidsegs high-water (vs MAXSEGS 32); ==32 => guard fired = M7 freeze root-cause */
extern "C" int   r_solidseg_ovf;   /* core r_bsp.c: latched '!' when a solidsegs post was dropped (over-budget view) */
extern "C" int   r_opening_ovf;    /* core r_plane.c: openings-pool overflow redirects THIS frame (0 = fine; >0 = garde-OPENINGS sinking) */
extern "C" int   r_composite_ovf;  /* core r_data.c: # textures stubbed by garde-COMPOSITE (0 = fine; >0 = a composite OOM was crash-proofed) */
extern "C" int   r_readlump_short; /* core w_wad.c: # streaming reads sunk by garde-W_ReadLump (0 = fine; >0 = a short CD read was zero-filled not I_Error-frozen) */
extern "C" int   r_nopatch_col;    /* core r_data.c: textures with a patchless column -- the ex-printf site, row 22 `np` */
/* SATURN 2026-08-16 -- the shared placeholder column every garde in r_data.c returns when the zone
   cannot serve a texture (zero-init, 256 B).  The sky uploader MUST compare against it: a sky built
   from stub columns is uniformly near-black, and `sky_loaded_tex` used to latch anyway -> a single
   tight moment at level load painted the sky black FOR THE WHOLE LEVEL.  See sky_cell_upload. */
/* 🔴 OVERLAY HOUSEKEEPING 2026-08-17, at the owner's request ("on en a tellement, on peut faire le
   ménage").  Criterion, the one already applied to `T`, `hc`, `nr` and `sc`: A PROBE WHOSE QUESTION
   IS ANSWERED GOES.  These three read IDENTICAL values on all four of the 2026-08-16 captures --
   they are latched historical peaks that stopped moving, so they cost three lines of a 25-line
   screen and tell nothing.  Flip to 1 to get them back; the accumulators behind them still run.
     row 4  PK   per-phase peaks    (frozen at Bw9.1 Bp206.2 P77.9 M33.5)
     row 14 MXd  worst-frame decomp (frozen at Bw6.4 Bp202.4 P31.0 M2.0)
     row 16 W72  SGL work-pointer   (frozen at c808 d+0)
   EXPLICITLY KEPT, per the owner: the whole VDP1<->CPU synchronisation train -- row 13 `N`/`L`,
   `Fl` the field lock, row 19 `V1 c/B/LP/ec/ws/tx`.  Those are the ones a sync bug will need. */
#define OVL_RETIRED 0

extern "C" unsigned int prof_seg_cols, prof_seg_fill, prof_seg_px, prof_lead_px;  /* row 14 `SEG` */
extern "C" unsigned int prof_gc_st[4], prof_gc_sn[4];                             /* row 16 `GCS` */
extern "C" unsigned int prof_wallprep, prof_segloop, prof_segrout;  /* core: the Bp split, FRT ticks */
extern "C" unsigned char r_column_stub[256];
extern "C" int  *texturewidthmask;   /* core r_data.c: width-1 per texture -- the sky uploader's only
                                        way to learn the sky is 1024 wide, not the 256 it assumed */
extern "C" int   sat_lowres;       /* core r_main.c: 1 = half-h-res (160) packed software render + VDP2 x2 NBG1 zoom (docs/LOWRES_RENDER_STUDY.md) */
extern "C" void  R_SetLowRes(int); /* core r_main.c: set sat_lowres + setsizeneeded (recompute viewwidth next D_Display) */
extern "C" int   sightcounts[2];   /* core p_sight.c: [0]=REJECT trivial-rejects, [1]=full BSP LOS walks */
extern "C" int   sat_sight_cachehit;  /* core p_sight.c: temporal sight-cache hits (row 24 `hc`)      */
extern "C" int   sat_floor_vq_cur, sat_floor_vq_peak;  /* VDP1-floor inc-0 estimate, shown on row 2 */
extern "C" unsigned int sat_sky_px, sat_floor_px;  /* sky-vs-floor coverage classifier (row 13) */
extern "C" int sat_plane_vscale;      /* deported-plane VERTICAL decrochage fill scale (baked at 4; live pad knob cut 2026-07-07) */
/* INERT since 2026-08-02: both of these only ever fed the deported-plane decrochage fill, which core
   reaches solely under `fclaim` (r_plane.c ~1531) -- and fclaim needs sat_floor_vdp1_hook, which no
   longer exists.  The platform no longer sets them, so core keeps its own defaults (border_max 40,
   fill_mode 0).  They stay declared because r_main.c still RECOMPUTES sat_plane_border from
   border_max every frame -- cost paid, benefit nil.  Cutting that computation is a core commit
   (shared with DoomJo), tracked separately.  border_max is still printed on overlay row 11. */
extern "C" int sat_plane_border_max;
extern "C" int sat_plane_fill_mode;
extern "C" int sat_plane_border_v;    /* live vertical fill-border px this frame (overlay readout) */
/* SATURN VALIDATION (Ymir-readable, deterministic): RAM-lever sizing telemetry. */
extern "C" int   r_visplane_coverage_peak;  /* #1: peak sum of live-plane spans (top-bytes) */
extern "C" int   r_visplane_pool_peak;      /* #1: peak bytes used in the span pool (0 if off) */
extern "C" int   r_visplane_pool_ovf;       /* #1: planes that overflowed VP_POOL_PLANES (0 = ok) */
extern "C" int   r_visplane_pool_ovf_pk;    /* ...and its ~1 s HIGH-WATER: the per-view reset made the
                                               raw counter read 0 on the overlay almost always.
                                               Printed as the 2nd digit of row 11 `vp<peak>.<ovf>` */
/* (the seven sat_texcache_* externs went with core/r_cache.c on 2026-08-17.  Their `xc` field had
   already been dropped from row 22 -- it printed the constant `xc0/0/60`, i.e. "the 96 KB contiguous
   slab was never carved", every capture of the summer.  Deleting the file is what paid for the two
   THK probes on this build: dead code costs the TLSF pool 1:1.) */
extern "C" int   sat_tex_numtex, sat_tex_sumwidth, sat_tex_dirbytes,
                 sat_tex_mptex, sat_tex_mpwidth;  /* Phase-0 texture-floor measurement (r_data.c) */
extern "C" int   Z_FreeMemory(void);          /* total reclaimable (free + purgeable) bytes */
extern "C" int   Z_LargestAllocatable(void);  /* largest contiguous run after purging */
extern "C" int   dg_heap_peak;              /* #4: peak newlib sbrk usage (bytes)             */
extern "C" int   dg_heap_size;              /* #4: newlib heap cap (bytes)                    */
extern "C" int   dg_heap_fail;              /* #4: sbrk REFUSALS -- any non-zero = raise HEAP_SIZE */
extern "C" int   z_block_count;             /* core z_zone.c: zone blocks walked by the last zone walk */
extern "C" unsigned int sat_bp_zw;          /* core r_parallel.c: zone blocks walked on the PK-Bp frame */
/* split-screen perf breakdown (ms per piece of the 2p render block) -- diagnose the slowdown */
extern "C" unsigned int sat_spl_sw, sat_spl_v0, sat_spl_v1, sat_spl_v2, sat_spl_v3, sat_spl_kick;
extern "C" int   sat_bsp_stage_used, sat_bsp_stage_want;  /* M5 BSP staging, row 1 st readout */
extern "C" int   sat_bsp_stage_on;                 /* M5 staging live A/B state (pad R+C) */
extern "C" void  P_BspStageApply(int on);          /* core/p_setup.c: swap LWRAM<->HWRAM sets */
/* VDP1 wall-texture bakes (cache misses) THIS flush -- diagnoses the `k` cost: if the split
   views thrash the 19 shared slots, bk stays high every frame => re-bake is the kick cost. */
static int wtex_bakes = 0;
/* SATURN VRAM-PRESSURE ROW (row 18 `VRM`, 2026-08-05).  BEFORE that row existed, wtex_bakes was
   incremented and reset every flush and printed NOWHERE, and every thing decline was folded into one
   `sat_things_decl` -- so neither "is the wall cache thrashing" nor "WHY was that sprite refused"
   could be read at all.  (Both are fixed: `bk`/`q` print on row 18 and `td` on row 15 since
   2026-08-09.  The past tense matters -- as written in the present it read as an open defect.)
   Both are the prerequisites for re-cutting the VDP1 VRAM map (cede WTEX slots -> bigger command
   banks; top 16KB -> more thing slots): without them a cut is measured only by a fps drop with no
   cause attached.  All four are ~1 s WINDOW sums, reset at print. */
static int wtex_bakes_win = 0;    /* wall-texture re-bakes / window  = WTEX thrash                 */
static int wtex_qrefuse   = 0;    /* evictions REFUSED / window: victim still on screen (3-state lock) */
static int thd_size   = 0;        /* thing declined: patch > THINGS_TEX_SLOTSZ -> slot SIZE limit  */
static int thd_slot   = 0;        /* thing declined: every slot already feeds this frame's list    */
static int thd_budget = 0;        /* thing declined: command bank / split queue full               */
static int vdp1_tx_total = 0;     /* WTEX_SLOTS (defined far below this row's printer)             */

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
/* 🔴 2026-08-16: the layer is now the FULL PAGE WIDTH, 64x16 cells.  It used to be 32x16 = the
   first 256 columns of the sky, tiled twice -- and a Doom sky is 1024 wide (TNT SKY1/2/3 and even
   DOOM1's are 1024x128, four 256-wide patches), so we showed ONE QUARTER of it and wrapped in the
   MIDDLE of the image: the owner's "ciel non continu".  Now the whole 1024 is sampled 2:1 into 512
   unique columns, so the wrap falls on the sky's own seam, which Doom art is drawn to hide.
   VRAM: 1025 cells x 64 B = 64,06 KB at B1+0, map moved to B1+64 KB.  B1 is 128 KB -> 56 KB still
   free.  A full-resolution 1024-wide layer would need 128 KB of cells + 16 KB of map and does NOT
   fit; A0/A1/B0 are the K-table, the floor bitmap and the framebuffer. */
/* 🔴 B1 BUDGET, WRITTEN OUT SO IT CANNOT ROT AGAIN.  B1 spans 0x25E60000..0x25E7FFFF (128 KB), and
   it is SHARED: **0x25E70000 is RBG0_MAP_VRAM** (the floor's pattern-name table, handed to the chip
   by sl1MapRA) and 0x25E7FF00 is the live RPT.  The sky therefore owns exactly the low 64 KB --
   CELLS **AND** MAP.  Writing the sky map at 0x25E70000 overwrote the floor's map and painted the
   whole floor in regular vertical stripes (owner's capture, 2026-08-16).  Arithmetic, checked:
       cells  64 cols x 13 rows + 1 filler = 833 cells x 64 B = 53312 B  [0x00000..0x0D03F]
       map    64x64 1-word                                    =  8192 B  [0x0E000..0x0FFFF]  (8 KB aligned)
       total                                                    61504 B  <= 65536  ✔ 0x25E70000 untouched
   13 rows = 104 px of sky, against a horizon of 96 (thresh = 12) -- one spare row, and
   sky_cell_write_map CLAMPS thresh to the row count so a live horizon tune can never index past
   the last stored row.  16 rows would need 65600 B and overflow into the floor by 64 bytes. */
#define SKY_CEL_VRAM     ((void *)0x25E60000)  /* B1+0:     NBG0 sky 256-color cells                  */
#define SKY_MAP_VRAM     ((void *)0x25E6E000)  /* B1+56K:   NBG0 sky pattern-name map (64x64 = 8KB)   */
#define SKY_CELL_ROWS    13                     /* 104 px of sky kept; horizon 96 needs 12            */
#define SKY_NB_CELL      (64 * SKY_CELL_ROWS)   /* 832 = index of the transparent filler cell         */
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
extern "C" void sat_sky_precache(void);              /* level-load sky upload (defined below)   */
extern "C" void (*sat_sky_precache_hook)(void);      /* core p_setup.c: fired inside P_SetupLevel */
static void sky_cell_build_map(void);   /* forward decl: rebuilds the sky map (live horizon tune) */
static void sky_cell_write_map(void);   /* forward decl: the VRAM half alone, deferred past the fence */
static int  sky_map_pending = 0;        /* a horizon change owes a deferred map write (sky_mode >= 1) */
static int  sky_horizon_row = SKY_HORIZON_ROW;  /* HW-sky horizon row (baked at SKY_HORIZON_ROW=96; live pad knob cut 2026-07-07). Still re-derived live by the horizon auto-track (~5669). */
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
/* K_OFF PROBE (temporary, 2026-07-07) -- the cell floor's 3rd rotation read (pattern-name) makes
   slScrAutoDisp return NG (unschedulable) alongside the 8bpp framebuffer; HW+diag confirmed (ad=0,
   rotation banks got zero cycle slots).  Set 1 to DROP the coefficient read (K_OFF -> flat/affine
   floor, NO perspective, ugly -- IGNORE the look) purely to test whether 2 rotation reads (char+map)
   DO schedule.  Read `ad` in the NBG1 diag box (works in Ymir -- slScrAutoDisp is deterministic SGL):
     ad = 00000001 -> 2 reads schedule -> CRKTE (coeff->CRAM, keeps perspective) is the validated fix.
     ad = 00000000 -> even 2 reads don't fit the framebuffer -> cell floor is dead, drop it.
   Set back to 0 to restore the real K_ON perspective coefficient. */
#define RBG0_CELL_KOFF_PROBE 0
/* SNOW FIX option 1: 16-colour (4bpp) floor cells.  256c char read = 2 VRAM accesses/dot, 16c = 1 ->
   cell rotation drops from 3 reads/dot (PN+char2) to 2 (PN+char1) = the bitmap floor's budget that
   coexists with the 8bpp framebuffer.  Cost: floor quantized to <=16 colours/flat.  The 16 (shaded)
   colours live in a free CRAM window (bank 0 idx 16..31; palette_number 1 = the map's 0x1000 bit). */
#define RBG0_CELL_4BPP   1
#define CRAM_CEL16       ((volatile unsigned short *)(0x25F00000 + 16 * 2))  /* 16-entry cell palette */
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
/* RBG0 floor KIND -- runtime-selectable (per-map), NOT a compile flag, so the shipping BITMAP floor
   and the new CELL / dual-param (RPA+RPB) floor coexist in ONE binary and switch at level load (or
   via the test chord).  Default = the shipping bitmap floor -> its path stays byte-identical.  The
   cell path revives the old #else branch but with the MATURE bitmap-era commit (RDBS=0x8D + A0/A1/B1
   parked + block-flush + slCashPurge) that fixed the snow class; docs/VDP2_RBG0_CURRENT_STATE.md. */
enum { RBG0_KIND_BITMAP = 0, RBG0_KIND_CELL = 1 };
static int rbg0_kind      = RBG0_KIND_BITMAP;   /* live kind (what is currently committed to the chip) */
static int rbg0_kind_want = RBG0_KIND_BITMAP;   /* requested kind; a mismatch triggers rbg0_reinit()   */
/* Rotation-parameter-table (RPT) location.  BITMAP keeps it at SRL's default B1+0x1ff00 (shipping,
   byte-identical -- B1 is otherwise free there).  CELL needs B1 for the pattern-name MAP, and having
   the RPT + the per-dot map READ in the SAME bank B1 starves the rotation (whole-plane snow, HW 2026-
   07-07).  So CELL moves the RPT into A0 (high), joining the K-table -- exactly SEGA's S_8_9_2 layout
   (RPT+K together, cells alone, map alone).  A0=K+RPT is SEGA-proven (there it's A1).  We only move it
   on a real switch (rbg0_rpt_moved) so the boot bitmap path never calls slRparaInitSet = unchanged. */
#define RBG0_RPT_B1  ((void *)0x25E7FF00)   /* B1+0x1ff00: bitmap RPT (SRL default)                    */
#define RBG0_RPT_A0  ((void *)0x25E1FF00)   /* A0+0x1ff00: cell RPT (joins K-table -> B1 = map alone)  */
static void *rbg0_rpt_vram  = RBG0_RPT_B1;  /* live RPT base; RA at +0, RB (dual-param) at +0x80       */
static int   rbg0_rpt_moved = 0;            /* 1 once we've re-pointed off the SRL B1 default           */
static int   rbg0_autodisp_ret = -1;        /* diag: slScrAutoDisp() return -- NG (0) for BOTH kinds (it is
                                               NOT the schedulability oracle; the hand-park is what works) */
static int   rbg0_cell_koff    = RBG0_CELL_KOFF_PROBE; /* runtime: cell coeff K_OFF (2 rotation reads char+map)
                                               vs K_ON (3 reads +coeff) -- R+Down A/Bs it to test a 2-bank wall */
static int   rbg0_reinit_force = 0;          /* R+Down sets this to re-run rbg0_reinit() with no kind change */
static int   rbg0_cell_nofb    = 0;          /* R+Left test: drop NBG1 (framebuffer) in cell mode -> if the
                                               floor renders CLEAN without it, the 8bpp framebuffer is the
                                               NG cause (proves the 'free a bank' path).  Loses HUD/weapon. */
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
extern "C" int            sat_wall_paint;   /* core r_data.c: DEBUG PAINT, bit0 VDP1 green / bit1 CPU red */
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

extern "C" int            sat_local_players; /* core: LIVE local-coop player count (1 = single) */
extern "C" void           SAT_CycleCheat(void);  /* core p_tick.c: cycle test cheat off->god->god+noclip */
extern "C" int            sat_split_vdp1;    /* core: split keeps walls on VDP1 (views 0/1); pad-X A/B */
extern "C" int            sat_plane_tas;     /* core: TAS.B plane work-steal (shipped default; A/B removed 2026-07-16) */

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
                             const unsigned char *xlat,
                             int x0, int y0, int x1, int y1,
                             int cx0, int cy0, int cx1, int cy1, int flip); /* core r_things.c */
void R_EmitWorldThingsVDP1(void);      /* core: emit world sprites to VDP1 at the post-BSP kick */
extern int sat_things_occ;             /* core: fully-occluded sprites skipped this frame (metric) */
extern int sat_thing_cap;              /* core: granted distinct textures/frame (we set = VRAM slots) */
extern int sat_thing_emit_cap;         /* core: max things emitted/frame -- we AIMD-adapt it (raster budget) */
extern int sat_wall_cpu_span;          /* core r_segs.c: near-wall->software CPU-entry span -- LOD-driven */
extern int sat_wall_cpu_v1;            /* core r_segs.c: VDP1-exit span (kept = span + prewarm band) -- LOD-driven */

/* SATURN live A/B toggles (2026-07-09) -- three perf levers, each a one-HW-session flip (see poll_pad):
   sat_clear_slave (R+C): dispatch the end-of-frame fb clear to the idle slave SH-2 (docs/BLIT_DMA_PLAN
     Inc3).  It writes HWRAM, not the B-bus, so the SCU-DMA hang law does not apply; the core joins it
     (RP_AuxWait) at the top of R_RenderPlayerView.  Watch dg/MST (rows 1/0).
   sat_near_sprites: FastDoom nearSprites cull of far decorations (defined in core/r_things.c).
     ⚠ NOT A TOGGLE and NOT OBSERVABLE.  It is baked ON, its R+X chord was reclaimed for the texture
     load budget (verified 2026-08-06), and row 7's `ns` -- a constant printed as if it were a knob --
     was cut 2026-08-09.  Listed here only so it is not re-discovered as a live A/B: there is no way
     to turn it off in a shipping build and no field that shows its effect.
   (things-AIMD is no longer a toggle: wbudget is baked for 1p -- see the emit-budget block.) */
extern "C" int sat_clear_slave = 1;    /* default ON: HW-validated -2..-3ms dg (R+C to A/B off) */
extern int     sat_near_sprites;       /* defined in core/r_things.c; default ON there.  (Its old R+X
                                          chord is GONE -- verified 2026-08-06 on PER_DGT_TX: no site
                                          binds it.  R+X now cycles sat_tex_load_budget.) */
extern "C" int sat_tex_load_budget;    /* core r_segs.c: textures faulted in per frame, 0 = off (R+X) */
extern "C" int sat_wall_flat_io;       /* core r_segs.c: tiers drawn flat for want of residency        */
extern "C" int sat_wall_flat_nocol;    /* core r_segs.c: ...and with no cached dominant colour either  */
extern "C" int sat_plane_flat_io;      /* core r_plane.c: visplanes drawn potato for want of residency */
extern "C" int sat_plane_flat_nocol;   /* core r_plane.c: ...with no cached flat colour either         */
extern "C" int sat_spr_flat_io;        /* core r_things.c: sprites skipped for want of residency       */
extern "C" int r_composite_builds;     /* core r_data.c: composites REBUILT (CPU copy, no disc I/O)    */
extern "C" int r_composite_distinct;   /* core r_data.c: DISTINCT textures behind them (thrash vs churn)*/
extern "C" int r_composite_pf;         /* core r_data.c: worst useful fraction of a patch decode (%)   */
/* (the composite-pin externs went with the pin, 2026-08-17 -- it was gated on a flag no code could
   set, and its `pn` field had already been dropped from row 22 for printing a constant zero.) */
extern "C" int sat_lpin_on, r_lpin_kb, r_lpin_yield;   /* PATCH-LUMP pin (2026-08-17): row 16 `P`      */
extern "C" int r_lpin_evict;                           /* ...ring-full evictions, PER WINDOW           */
extern "C" void R_LumpPinFlush(void);                  /* release the ring on the pad L+Left A/B       */
extern "C" int sat_wall_lod_hits;                      /* core r_segs.c: size LOD, row 22 `Lo`        */
extern "C" int sat_lod_mindist, sat_wall_lod_near;     /* core r_segs.c: LOD distance floor + rescues */
extern "C" int sat_lod_eff, sat_lod_auto_step, sat_gov_debt;   /* governor, row 21                    */
extern "C" int sat_gov_axis, sat_gov_p_step, sat_gov_p_dirty;   /* multi-axis governor: which knob    */
extern "C" int sat_thing_role_cull, sat_thing_cull_dist, sat_thing_role_cut;   /* role cull, row 21   */
extern "C" void R_CompositeWindowReset (void);   /* one writer for both + the 16-slot distinct set     */
/* SATURN RESIDENT FLAT POOL (core/r_flatcache.c) -- the fix for the "flat treadmill": before it,
   W_ReleaseLumpNum demoted every visible plane's flat to PU_CACHE after EVERY plane of EVERY frame,
   so Z_Malloc's address-ordered rover purged the floor under the player's feet and it cost a fresh
   ~42 ms disc read next frame (measured 80..221 non-resident flat fetches PER SECOND on TNT MAP11).
   Read row 19 `FLT`: `ld` must PLATEAU -- a flat disc-read count that keeps climbing means the pool
   is bypassed (A-), too small (`f`>0, `ev` climbing), or never carved (`p0`). */
extern "C" int sat_flatcache_on;       /* live A/B bypass (pad R+Z); slab stays carved either way    */
extern "C" int sat_flatcache_slots;    /* slots carved this level (0 = zone too tight -> pool-less)  */
extern "C" int sat_flatcache_live;     /* slots currently holding a flat                             */
extern "C" int sat_flatcache_load;     /* cumulative slot fills = the REAL flat disc reads           */
extern "C" int sat_flatcache_evict;    /* cumulative LRU evictions                                   */
extern "C" int sat_flatcache_full;     /* views where every slot was busy -> classic zone path       */
/* SATURN: row-2 `P` split into its parts (core/r_parallel.c).  Row 20 `PSP`; k+n+d+j == `P`. */
extern "C" unsigned int sat_p_kick10;  /* VDP1 wall kick + R_DrawPlayerSprites (weapon), tenths-ms  */
/* (sat_p_net10 / _draw10 / _join10 removed with the row that printed them -- settled at ~0.) */
extern "C" int R_TextureIOFree(int tex);  /* core r_data.c: 1 = resolving this texture hits no disc */
extern "C" int sat_tex_load_spent;     /* core r_segs.c: tenths of a ms of disc spent this frame    */
extern "C" int R_LoadBudgetLeft(void); /* core r_segs.c: 1 = the frame can still afford a fault     */
extern "C" int sat_budget_refused;     /* core r_segs.c: 1 once the budget has refused something    */
extern "C" int R_WallPotatoColorPeek(int tex);  /* core r_data.c: cached dominant colour, -1 = none,
                                                   NEVER loads (R_WallPotatoColor faults the texture
                                                   in through R_GetColumn -- see wall_emit_flat)    */
#define SAT_WALL_FLAT_UNKNOWN 100      /* neutral palette index, same as the software path uses      */
static unsigned int sat_p_emit10 = 0;  /* ...in the wall EMIT loop (flush minus the resolve pass)      */
static unsigned int sat_p_thg10 = 0;   /* ...in the WORLD-THINGS emit (R_EmitWorldThingsVDP1)          */
/* (sat_p_thgcd10 / _thgcdn -- the CD half of that bracket -- removed with the PSP row 2026-08-07,
   after they proved c ~= e in 13/13 captures.  sat_p_thg10 is kept: it is the one live number.) */
/* Window MIN/MAX.  `P` is BIMODAL at a FIXED viewpoint -- the owner's six same-spot captures read
   P = 11.6 / 74.2 / 167.1 / 10.3 / 87.2 / 8.2 with Bw, Bp and M all constant -- so a single sample,
   which is all the once-per-second overlay block could give, is worthless here.  Track the extremes
   over the window instead: one capture then shows BOTH modes and which sub-term carries them. */
/* (pk_kmin/pk_kmax/pk_emax/pk_cmax/pk_nmax removed with the PSP row, 2026-08-07.) */
/* LEVEL-LOAD disc cost, isolated from boot and from play.  Owner 2026-08-07: *"pourquoi le jeu est
   si lent a charger ?  Il ne me semble pas que c'etait le cas avant"* -- and cumulative `t` cannot
   answer that, it mixes boot + load + the in-play thrash.  Detector: more than 2 s of CD time
   BETWEEN TWO CONSECUTIVE DG_DrawFrame calls cannot be gameplay -- nothing in a frame blocks that
   long -- so it is a P_SetupLevel (or the boot read).  Latch that gap verbatim.  No core change,
   no guess about where the load starts, and it catches any blocking stall of that size.
   REMOVED 2026-08-07 once it had answered (L270s/4704 -> the BOOT, not P_SetupLevel).  To revive:
   four statics (cd_prev_ms10/cd_prev_n/lvl_load_ms10/lvl_load_n), a delta+latch in the per-frame
   fold below, and the L%us/%u pair on the CD row. */
/* 🔴 AIMD-DAMP STATE CUT 2026-08-09 -- thing_emit_floor / thing_overrun_run / thing_cap_clean.
   The comment that stood here promised "a learned floor never lets ec collapse to 0" and "2-frame
   hysteresis against the noisy HW EDSR-CEF".  NEITHER EXISTED: all three were only ever ASSIGNED
   ZERO (five statements between them, two of which run every frame in each WBUDGET branch) and READ
   nowhere except the row-15 `ef` field, which therefore printed a constant 0.
   That is worse than dead code: the reader -- me, this morning -- believes a damper is protecting
   `ec` and looks elsewhere for why it snapped to 0. The policy is, and always was, pure
   feed-forward: see the ramp at the two `budget_cap` sites. If a damper is wanted, write one. */
extern int sat_things_hw;              /* core: 1 = world sprites -> VDP1 (M4); 0 = software (M0/M6) */
extern int sat_split_active;           /* core r_main.c: split emits per view PRE-kick -> queue path */
int  sat_vdp1_thing_draw(patch_t *patch, int lump, const unsigned char *cmap,
                         const unsigned char *xlat,
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
   HISTORY: the parked M1/M2/M3 (full/floors-only/ceilings-only VDP1 tile deport)
   were REMOVED 2026-07-16 -- HW-settled net-negative (memory-bound F-build, not
   fill; M4 +44% vs M1) and structurally dead for ceilings (layer order inverted).
   M5_CONVEX (convex-exact floor deport) followed them on 2026-08-02: four staircase
   HW captures settled it negative (the wall is NON-CONVEXITY, not the near clip), and
   it was the SOLE remaining consumer of the whole SAT_FLOOR_TEX machinery -- ~9KB of
   .text/.bss that no reachable mode executed.  Cutting M5 cut all of it and roughly
   TRIPLED the TLSF pool.  Do not re-propose a VDP1 floor/ceiling tile deport without
   new HW evidence: see docs/M7_FEATURE_AUDIT.md and the m5-convex-deport-preview note. */
/* M1_FULL/M2_FLOORS/M3_CEILS removed 2026-07-16, M5_CONVEX removed 2026-08-02 (see above).
   sat_m is only ever compared to these tokens (never a literal int), so the renumber is safe. */
enum { M0_SOFT, M4_RBG0, M6_NOSPR, M7_LOWRES, M_COUNT };
static int sat_m = M7_LOWRES;               /* boot = M4 (RBG0 dominant floor + VDP1 walls + VDP1 weapon +
                                               VDP1 world things) + LOW-RES: the shipped default in EVERY
                                               player-count (1p/2p/3-4p all pack into fb[0,160) and x2-zoom).
                                               Pad Z still cycles down to M4/M6/M0 for A/B.  M6 = same but
                                               world things SOFTWARE (the sprite-only A/B).  M7 = M4 + LOW-RES
                                               (docs/LOWRES_RENDER_STUDY.md):
                                               software render at viewwidth 160 + VDP2 x2 NBG1 zoom -- the
                                               HW-owned elements (VDP1 walls/weapon/sprites, RBG0 floor) stay
                                               full-res, only the SOFTWARE leftovers (ceilings/minor-floors/
                                               fallbacks) go half-res.  Lowres is WHOLE-VIEW (shared projection +
                                               whole-layer zoom) so it is a MODE, not a per-zone SQ.  Pad Z cycles
                                               {M4, M6, M7}; M0+M5 are PARKED (off the cycle). */
static const char *const sat_m_name[M_COUNT] = { "soft", "rbg0", "nospr", "lowr" };
/* SATURN (2026-07-19, user-requested): ALL modes except M7_LOWRES are PARKED.  sat_m boots at M7
   and never changes -- so none of the mode-SWITCH machinery (RBG0/VDP1/viewwidth re-init, coherent-
   pair wash, slave re-dispatch) ever runs.  That machinery is where every switch corruption / stale-
   wall / M0-crash lived; with one mode there is nothing to break.  M0/M4/M5/M6 remain valid modes in
   the code (sat_apply_mode still maps them) reachable only by editing THIS ring; pad-Z is now a no-op
   (cycles M7->M7).  Re-add a mode here ONLY once switching has been made atomic. */
static const int sat_m_cycle[] = { M7_LOWRES };
#define SAT_M_CYCLE_N ((int)(sizeof(sat_m_cycle) / sizeof(sat_m_cycle[0])))
enum { SQ_FULL, SQ_LD, SQ_BAND, SQ_FLAT };
static int sq_wall = SQ_FULL, sq_floor = SQ_LD, sq_ceil = SQ_LD;   /* floor+ceil ld by default (HW-tested "fll":
                                                                     ld is ~invisible on the ceiling and fine on the
                                                                     floor; the convex-exact deport (M5) puts the
                                                                     convex floors at FULL quality on VDP1 anyway) */
/* SATURN sprite-SQ (the 4th SQ zone, pad R+B): full/ld only.  Software world sprites are drawn at
   half horizontal resolution when SQ_LD -- an INDEPENDENT axis from the wall/floor/ceil detailshift
   (sat_sprite_ld, core r_things.c), so the sprite quality no longer rides the global detailshift the
   floor/ceil LD used to force.  Default FULL -> inert (byte-identical to today); the LD path only
   bites the software sprite fill (split/M0/M6), never the VDP1 world-things (1p M4). */
static int sq_sprite = SQ_FULL;
static const char *const sq_name[4] = { "full", "ld", "band", "flat" };
static int wall_potato_mode = 0;            /* VDP1 wall style: 0=tex 1=banded 2=flat (SQ_wall-derived) */
/* SATURN split-context SQ (2026-07-15, docs/LOWRES_RENDER_STUDY.md §SQ-PER-VIEW): the co-op split
   viewports get their OWN wall/floor/ceil software quality, INDEPENDENT of the 1p sq_* above -- so a
   split can run BAND walls + FLAT floors to cut the per-view Bp+P (the measured 3/4p bottleneck; M7
   low-res proved USELESS there -- fill is a minority) WITHOUT degrading 1p.  Per-VIEW arrays: the
   live toggles write all four uniformly (the common case), but sat_view_sq_apply(i) reads per view,
   so per-viewport asymmetry (e.g. the RBG0-elected view crisp, others cheap) is a 1-combo follow-up.
   Default = the 1p defaults -> byte-identical until toggled in a split.  CONSTRAINT: SQ_LD is NOT
   offered for split WALLS -- it drives the GLOBAL detailshift (a whole-frame projection lever set
   once before the split loop, cannot vary per view); the split wall cycle is FULL->BAND->FLAT.
   Floor/ceil LD ARE per-view-safe (half-rate texel fetch, read inside the plane fill). */
static int sq_wall_view[4]  = { SQ_FULL, SQ_FULL, SQ_FULL, SQ_FULL };
static int sq_floor_view[4] = { SQ_LD,   SQ_LD,   SQ_LD,   SQ_LD   };
static int sq_ceil_view[4]  = { SQ_LD,   SQ_LD,   SQ_LD,   SQ_LD   };

/* SATURN: cycle a PLANE (floor/ceil) software quality full->ld->flat->full, but SKIP ld when
   detailshift is on (M7 / split-lowdetail) where floor/ceil ld is a NO-OP (the half-rate texel
   path R_TexturedSpan is high-detail only, r_plane.c) -> never land on a dead 'l'.  Walls keep their
   own cycle (wall ld is what DRIVES detailshift, so it is meaningful). */
static inline int sq_plane_cycle(int cur)
{
    extern int detailshift;
    if (cur == SQ_FULL) return detailshift ? SQ_FLAT : SQ_LD;
    if (cur == SQ_LD)   return SQ_FLAT;
    return SQ_FULL;
}

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
/* Blit config: c5 = CPU memcpy + W5 (skip the unchanged static-HUD band).  The old pad-L+A A/B
   ring (c5<->c-, plus the HW-dead slDMACopy paths) was cut 2026-07-07 -- W5 is a real idle-time
   win so it stays ON permanently; blit_mode is fixed at 1.  See [[blit-dma-lever]] / BLIT_DMA_PLAN. */
static int blit_mode = 1;      /* c5 (CPU + W5); was the L+A ring index, now fixed */
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
/* core/r_plane.c: RBG0 mark-suppress -- keep the never-drawn dominant floor as ONE visplane
   (no R_CheckPlane split memsets).  Live A/B via pad L+B; window auto-resets on the flip.
   Read Bp (row 2/4) + vp (row 11 LIM) with it on vs off, same scene. */
extern "C" int sat_mark_suppress;
/* RANK 3 inc-1 (docs/RANK3_WALLPREP.md): run the deferred wall-prep flush on the SLAVE (1) vs the
   master (0).  Pad L+R toggles it live; ON also enables sat_wallprep_defer (walls queued, not
   inline).  inc-1 is NON-overlapped -> expect byte-identical render + Bp off the master + w up
   ~21ms + fps UNCHANGED (the win is inc-2).  Row 1 shows wp<state>. */
extern "C" int sat_wallprep_slave;
extern "C" int sat_wallprep_defer;
/* SATURN (2026-07-18): frames of forced VDP1 erase after a render-MODE change.  Consumed by
   vdp1_walls_flush (takes the empty-bank present path instead of the coherent-pair HOLD) so the
   new mode does not keep showing the PREVIOUS mode's walls until its own pair completes.  M0 used
   to clear this by side effect (all-software = no VDP1 walls); dropping M0 from the pad-Z ring
   exposed the stale-wall-on-switch, so we now clear it explicitly, independent of M0. */
static int sat_vdp1_switch_clear = 0;

/* The single writer of every render backend flag.  Maps (sat_m, sq_*) -> a coherent
   per-surface tuple; each M leaves ON only the subsystems it needs.  Called at init, on any
   M/SQ change, and per-view in the split (the per-frame block downgrades RBG0 off P1). */
static void sat_apply_mode(void)
{
    extern int sat_split_lowdetail;            /* core: split detailshift (low-detail) */
    extern int sat_floor_ld;                   /* core r_plane.c: half-rate floor texel fetch */
    extern int sat_ceil_potato, sat_ceil_ld;   /* core r_plane.c: independent ceiling SQ (step 2) */
    extern int sat_sprite_ld;                  /* core r_things.c: half-res software sprite fill (indep. of detailshift) */
    int M = sat_m; if (M < 0 || M >= M_COUNT) { M = M4_RBG0; sat_m = M; }

    /* On a REAL render-mode change, force 2 VDP1-erase frames so the coherent-pair present does not
       keep showing the previous mode's walls (see sat_vdp1_switch_clear).  Guarded on an actual sat_m
       change -- sat_apply_mode is also called at init and per-view in split with sat_m unchanged. */
    {
        static int prev_applied_m = -1;
        if (sat_m != prev_applied_m) { sat_vdp1_switch_clear = 2; prev_applied_m = sat_m; }
    }

    /* ---- Axis A: offload targets ---- */
    int rbg0_want  = (M != M0_SOFT);           /* RBG0 dominant floor (every mode but M0) */
    int vdp1_walls = (M != M0_SOFT);           /* VDP1 one-sided walls  (every mode but M0) */
    sat_vdp2_sky            = rbg0_want ? 1 : 0;   /* M0 -> sat_vdp2_sky=0: core draws the software sky */
    sat_vdp2_floor          = rbg0_want ? 1 : 0;   /* per-frame block re-derives on split/1p (rbg0_on) */
    sat_vdp2_floor_dominant = RBG0_FLOOR_DOMINANT; /* every RBG0 mode uses the dominant-flat pick */
    sat_wall_skip           = vdp1_walls ? 1 : 0;  /* M0 -> core draws software one-sided walls */
    sat_things_hw           = (M != M0_SOFT && M != M6_NOSPR); /* world sprites -> VDP1 prio-7 (M4); M6 = SOFTWARE
                                                     sprites (same walls+floor as M4) = the sprite-only A/B */
    /* (The VDP1 floor/ceiling tile deport and its VRAM interlock against the world-things pool
       both went with M5 on 2026-08-02.  core's sat_floor_vdp1_hook is left NULL, so R_DrawPlanes
       never consults it and the 44KB VDP1 tail at 0x25C71000 belongs to THINGS alone -- the pool
       collision that forced the interlock cannot recur.) */

    /* ---- Axis B: per-zone software quality (bites only on software zones / fallback slivers;
       HW-owned zones ignore it -- no detail level on hardware) ---- */
    wall_potato_mode = (sq_wall == SQ_BAND) ? 1 : (sq_wall == SQ_FLAT) ? 2 : 0;  /* VDP1 wall style */
    sat_potato_walls = (sq_wall == SQ_FLAT);                     /* flat-shaded software walls */
    sat_wall_nocpu   = (sq_wall == SQ_BAND || sq_wall == SQ_FLAT);/* banded/flat -> skip close-wall CPU */
    /* SATURN 2026-08-15: the LOD governor's PLANE axis rides ON TOP of the owner's own SQ setting,
       as a FLOOR -- max(), never assignment.  It can degrade what he chose and never silently
       improve past it, and when it releases (`sat_gov_p_step` back to 0) his setting is exactly
       what it was.  Steps skip BAND: band is documented meaningless for a plane (the potato span is
       already distance-shaded per row), so the useful ladder is FULL -> LD -> FLAT. */
    {
        static const int gov_sq[3] = { SQ_FULL, SQ_LD, SQ_FLAT };
        int gp = gov_sq[(sat_gov_p_step < 0 ? 0 : sat_gov_p_step > 2 ? 2 : sat_gov_p_step)];
        int ef = sq_floor > gp ? sq_floor : gp;
        int ec = sq_ceil  > gp ? sq_ceil  : gp;
        sat_potato_floors = (ef == SQ_FLAT);                     /* solid-colour software floors */
        sat_floor_ld      = (ef == SQ_LD);                       /* half-rate floor texel fetch */
        sat_ceil_potato   = (ec == SQ_FLAT);                     /* solid-colour software ceilings */
        sat_ceil_ld       = (ec == SQ_LD);                       /* half-rate ceiling texel fetch */
    }
    /* SATURN M/SQ: detailshift is a GLOBAL half-viewwidth lever -- it hits walls' software
       fallback AND sprites AND spans alike, so it must NOT be driven by the floor/ceil SQ.
       Floor/ceil LD already have their OWN per-plane mechanism (sat_floor_ld / sat_ceil_ld =
       half-rate texel fetch on the worklist), independent of detailshift.  Deriving the global
       detailshift from floor/ceil LD dragged sprites + the wall CPU-fallback to half-res in split
       even at sq_wall=FULL.  Gate it on the WALL SQ only -> "fll" (wall full, floor+ceil LD) now
       keeps sprites + walls full-res.  (A dedicated split-detailshift control, decoupled from SQ
       entirely, is the follow-up for the solo/multi hd/ld combos.) */
    sat_split_lowdetail = (sq_wall == SQ_LD); /* split detailshift: wall SQ only */
    sat_sprite_ld       = (sq_sprite == SQ_LD); /* independent sprite axis (pad R+B); only bites when detailshift==0 */

    sat_wall_clamp = SAT_WALL_CLAMP;   /* pad R+A re-arms it live */

    /* SATURN M7 = M4 + LOW-RES (docs/LOWRES_RENDER_STUDY.md).  sat_lowres drives the core (packed
       half-width render) + the platform (VDP2 x2 NBG1 zoom + 160-byte blit).  1p: viewwidth 160
       packed into fb[0,160).  M7-MULTI 2p: each half renders viewwidth 80 packed (P1 fb[0,80),
       P2 fb[80,160), quadrants likewise via R_SetViewWindow's packed columnofs base) and the SAME
       whole-layer x2 zoom restores them all -- split boundaries fall clean under x2.  The compact
       split HUD (2p panels / 3-4p bands) is captured onto un-zoomed VDP1 prio-7 sprites so it stays
       crisp; RBG0 is off in 3/4p (software floor, packs+zooms fine).  R_SetLowRes sets setsizeneeded
       -> a ~74ms R_ExecuteSetViewSize recompute, so flip ONLY on a real state change. */
    {
        extern int sat_local_players;
        int want_lr = (M == M7_LOWRES);   /* M7-multi: 1p + 2p + 3/4p (all pack into fb[0,160)) */
        if (want_lr != sat_lowres)
            R_SetLowRes(want_lr);
    }

    /* Re-apply the VDP1 isolation mode (sat_things_hw / sat_iso_flat) so a mode cycle preserves it.
       sat_iso_mode==0 (ship default) restores the all-layers-on-VDP1 state -- and since M0/M6 are
       parked off the pad-Z ring, that write is byte-identical to the sat_things_hw set above. */
    sat_apply_iso();
}

/* VDP1 isolation modes (pad L+Z, 1p): find what overruns the command list ("transfer-over" = the
   flicker) by varying which layers ride VDP1.  The player WEAPON stays ON VDP1 in every mode: the
   L+X chord that routed it to software was CUT 2026-08-02 (it left the walls un-re-cleared -- a
   PRE-EXISTING transition glitch, HW-confirmed -- and the weapon-fill lever measured inert, so it
   was a glitch with no upside).  So isolation varies THINGS + FLAT only.  Read LP% per mode:
     0 all       things + weapon + TEXTURED walls (ship default -- no change)
     1 no-things weapon + TEXTURED walls          (LP delta vs 0 = the THINGS' share of the overrun)
     2 flat      weapon + FLAT walls (1 cyc/px vs 2): if LP recovers vs mode 1 it was per-pixel texel
                 FILL; if not, it is command/overdraw GEOMETRY (the SEGA near-wall off-screen span). */
static void sat_apply_iso(void)
{
    extern int sat_things_hw;   /* core r_things.c: world sprites on VDP1 (1) / software (0) */
    switch (sat_iso_mode) {
        default: sat_things_hw = 1; sat_iso_flat = 0; break;  /* 0 all */
        case 1:  sat_things_hw = 0; sat_iso_flat = 0; break;  /* no things */
        case 2:  sat_things_hw = 0; sat_iso_flat = 1; break;  /* flat walls */
    }
}

/* SATURN piste-5 (rotating split-SQ balance, pad R+Right): spread the split's degraded SQ over
   views+frames so no single viewport stays permanently ugly, at a chosen cost fraction.  0 = off
   (every view uses its split SQ, as set).  1 = ONE view degraded per frame (rotating) -> 3/4 views
   full = minimal fps hit / best quality; 2p = each half degraded every other frame.  2 = TWO views
   degraded per frame (rotating) -> bigger cut.  A view is "degraded" = it uses the cheap split SQ
   (sq_*_view, which YOU set via R+Y/Y/L+Y); a "full" view uses the FULL/LD preset.  Needs the
   per-wall wall_acc.pot capture so each view's VDP1 walls take its own style (else all walls would
   flicker to the last view's style).  Tick advances once per split frame (on v==0). */
static int          sat_split_balance = 0;
static unsigned int sat_split_bal_tick = 0;

/* SATURN split-context SQ: apply viewport v's software quality to the SAME core render flags
   sat_apply_mode maps from the 1p sq_* (lines above), MINUS detailshift/sat_split_lowdetail (a
   whole-frame projection lever -- excluded, so split walls never offer SQ_LD).  Called PER VIEW in
   d_main's split loop before each R_RenderPlayerView; sat_view_sq_restore() re-applies the 1p set
   after the loop so the HUD/menu/1p paths see the normal globals.  No sat_apply_mode() call: only
   the per-surface software-quality flags move, not the M-axis offload tuple. */
extern "C" void sat_view_sq_apply(int v)
{
    extern int sat_floor_ld, sat_ceil_potato, sat_ceil_ld;
    extern int sat_local_players;
    if (v < 0 || v > 3) v = 0;
    /* piste-5 balance: pick whether THIS view runs the cheap split SQ or the full preset this frame */
    int use_split = 1;
    if (sat_split_balance) {
        if (v == 0) sat_split_bal_tick++;                       /* once per split frame */
        int nv = sat_local_players; if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
        int ndeg = (sat_split_balance >= 2) ? 2 : 1; if (ndeg > nv) ndeg = nv;
        int start = (int)(sat_split_bal_tick % (unsigned)nv);   /* rotating window origin */
        int rel = v - start; rel %= nv; if (rel < 0) rel += nv;
        use_split = (rel < ndeg);                               /* in the degraded window this frame? */
    }
    int w, f, c;
    if (use_split) {
        w = sq_wall_view[v]; f = sq_floor_view[v]; c = sq_ceil_view[v];
        /* BALANCE flicker guard (user 2026-07-15): a per-frame LD<->FLAT alternation on floor/ceil is
           too jarring (esp. at low split fps = slow strobe).  Under balance, clamp the degraded
           floor/ceil to LD -- the alternation on those planes becomes FULL/LD<->LD (subtle), while the
           WALLS still alternate (band/flat wall shimmer is far less visible than a floor going
           textured<->solid).  No clamp when balance is off (uniform, no alternation -> FLAT is fine). */
        if (sat_split_balance) { if (f == SQ_FLAT) f = SQ_LD; if (c == SQ_FLAT) c = SQ_LD; }
    }
    else           { w = SQ_FULL;         f = SQ_LD;            c = SQ_LD; }   /* FULL-quality preset */
    wall_potato_mode  = (w == SQ_BAND) ? 1 : (w == SQ_FLAT) ? 2 : 0;
    sat_potato_walls  = (w == SQ_FLAT);
    sat_wall_nocpu    = (w == SQ_BAND || w == SQ_FLAT);
    sat_potato_floors = (f == SQ_FLAT);
    sat_floor_ld      = (f == SQ_LD);
    sat_ceil_potato   = (c == SQ_FLAT);
    sat_ceil_ld       = (c == SQ_LD);
}
extern "C" void sat_view_sq_restore(void)
{
    extern int sat_floor_ld, sat_ceil_potato, sat_ceil_ld;
    wall_potato_mode  = (sq_wall == SQ_BAND) ? 1 : (sq_wall == SQ_FLAT) ? 2 : 0;
    sat_potato_walls  = (sq_wall == SQ_FLAT);
    sat_wall_nocpu    = (sq_wall == SQ_BAND || sq_wall == SQ_FLAT);
    sat_potato_floors = (sq_floor == SQ_FLAT);
    sat_floor_ld      = (sq_floor == SQ_LD);
    sat_ceil_potato   = (sq_ceil == SQ_FLAT);
    sat_ceil_ld       = (sq_ceil == SQ_LD);
}
#define GS_LEVEL 0
#define GS_INTERMISSION 1                   /* gamestate_t: WI owns the 200..223 band (meta line + grain-extended art) */
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

/* 3/4-player compact HUD: blit one quadrant's 160x16 brushed-metal band (real STBAR
   pixels, hud4p_panel) at (ox, oy); the core then draws that player's widgets on top
   via ST_DrawQuadHud.  The band is OPAQUE (every index non-zero) so it occludes the
   VDP1 wall layer below NBG1 -- see hud4p_panel.h. */
extern "C" void ST_DrawQuadHud(int pnum, int ox, int oy);   /* core: per-player compact widgets */
static void hud4p_blit_band(int ox, int oy)
{
    for (int y = 0; y < HUD4P_H; ++y)
        memcpy(framebuffer + (oy + y) * 320 + ox, hud4p_panel + y * HUD4P_W, HUD4P_W);
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
    /* M7-multi (sat_lowres in 2p): the caller (D_DrawFrame HUD composite) already guarantees
       GS_LEVEL && !menuactive && 2p, so sat_lowres alone means the framebuffer is PACKED.  The
       3D-view rows [0,160) hold P1 in fb[0,80) and P2 in fb[80,160) (the x2 NBG1 zoom restores
       them); the HUD panels below (rows 160-223) are still drawn full-320 and 2:1-decimated at
       blit time.  So wash the VIEW rows in packed half-width (80) and the HUD rows in full
       screen-width (160) -- matching the blit's split at hud_top=160.  Non-lowres: one screen-x
       [x0,x0+160) wash over all 224 rows, exactly as before. */
    int packed = sat_lowres;
    for (int half = 0; half < 2; ++half)
    {
        int lvl = half ? l2 : l1;
        if (lvl <= 0 || lvl >= HUD2P_NPAL) continue;
        const unsigned char *lut = hud2p_flash_lut[lvl];
        if (packed)
        {
            int vx0 = half ? 80 : 0;                    /* packed 3D-view half (fb cols) */
            for (int y = 0; y < 160; ++y)
            {
                unsigned char *row = framebuffer + y * 320 + vx0;
                for (int x = 0; x < 80; ++x) row[x] = lut[row[x]];
            }
            int hx0 = half ? 160 : 0;                   /* full-320 HUD panel (decimated at blit) */
            for (int y = 160; y < 224; ++y)
            {
                unsigned char *row = framebuffer + y * 320 + hx0;
                for (int x = 0; x < 160; ++x) row[x] = lut[row[x]];
            }
        }
        else
        {
            int x0 = half ? 160 : 0;
            for (int y = 0; y < 224; ++y)
            {
                unsigned char *row = framebuffer + y * 320 + x0;
                for (int x = 0; x < 160; ++x) row[x] = lut[row[x]];
            }
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
        /* M7 3/4p: the quad's VIEW rows are packed to half-width (fb x qx>>1, 80 wide); its BAND
           rows [+96,+112) stay full-320 (captured onto the un-zoomed VDP1 band sprite).  Wash the
           view portion packed + the band portion full-width.  Non-lowres: one full-320 quad wash. */
        if (sat_lowres)
        {
            int vh  = HUD4P_QUAD_H - HUD4P_H;             /* view rows = 112-16 = 96 */
            int vx0 = qx[q] >> 1;                         /* packed view half (0 or 80) */
            for (int y = 0; y < vh; ++y)
            {
                unsigned char *row = framebuffer + (qy[q] + y) * 320 + vx0;
                for (int x = 0; x < 80; ++x) row[x] = lut[row[x]];
            }
            for (int y = vh; y < HUD4P_QUAD_H; ++y)       /* band rows: full-320 (VDP1 capture) */
            {
                unsigned char *row = framebuffer + (qy[q] + y) * 320 + qx[q];
                for (int x = 0; x < 160; ++x) row[x] = lut[row[x]];
            }
        }
        else
        {
            for (int y = 0; y < HUD4P_QUAD_H; ++y)
            {
                unsigned char *row = framebuffer + (qy[q] + y) * 320 + qx[q];
                for (int x = 0; x < 160; ++x) row[x] = lut[row[x]];
            }
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

/* WPROBE/CPROBE death-screen reporter (defined with the probe block below):
   every fatal photo then carries the CD-path CRC verdict (WP row 2) and the
   cart-copy directory CRC boot vs death (CP row 3). */
static void wprobe_fatal_rows(void);

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
    wprobe_fatal_rows();
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

/* R0.2 k-meter companion: the vblank count disambiguates FRT wraps for CD commands
** longer than ~293ms (w_file_saturn.cxx sat_cd_clock_add). */
extern "C" unsigned int sat_vbl(void)
{
    return vbl_count;
}

/* 🔴 SATURN 2026-08-16 -- THE MILLISECOND CLOCK MEASURES VBLANKS IT ACTUALLY SAW.
   `us_acc += us_per_frame` asserted that this handler runs on EVERY field.  Nothing guaranteed
   that, and row 24 `x` (tics per frame) says it does not: 2,1 tics/frame at 5,5 fps where 35 Hz
   over a 181 ms frame owes 6,4.  A handler that misses fields makes DG_GetTicksMs run slow by
   exactly the miss ratio, `GetAdjustedTime` hands `NetUpdate` too few tics, and NetUpdate has
   already advanced `lasttime` past them -- so they are LOST, not deferred.  That is the whole
   slow-motion mechanism, and it is the same clock caught saturating at 72-73 ms on hardware
   while the FRT read 106-110.
   Fix: add the MEASURED FRT delta since the previous handler run instead of a constant.  If the
   handler is healthy every delta is one field and the result is byte-identical to before; if it
   misses fields the delta covers the gap and the clock stays true.  Self-correcting either way,
   and it needs no knowledge of WHY a field was missed.  (The delta can only wrap past 293 ms,
   which would mean the handler is dead, not late.)  Row 24 `v` = fields seen per frame reports
   the miss rate directly: it must read 60/fps. */
static void vblank_handler(void)
{
    static int vbl_primed = 0;
    unsigned char  h = FRT_FRCH, l = FRT_FRCL;          /* read H then L: latches the pair */
    unsigned short f = (unsigned short)((h << 8) | l);
    vbl_count++;
    if (vbl_primed)
        us_acc += ((unsigned int)(unsigned short)(f - frt_at_vbl) * ns_per_frt) / 1000u;
    else
        vbl_primed = 1;
    frt_at_vbl = f;
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
   signal again.
   ⚠ 2026-08-10, comment corrected: this narration described a row that no longer exists.  The VD1
   row was deleted 2026-08-06 and row 2 is now r_parallel's Bw/Bp/P/M line; `Dr` and its two
   accumulators went with the 08-09 cleanup (see the note below).  `vdp1_prev_done` itself is still
   live -- but the trustworthy VDP1 headroom signal to read on a photo is row 17 `LP%`, not any
   done-rate: EDSR.CEF latches 30-60% on real hardware ([[vdp1-cef-latches-on-hw]]). */
static int vdp1_prev_done = 1;
/* (vd1_win_done / vd1_win_tot CUT 2026-08-09: their only consumer computed `dr` and then
   `(void)`-discarded it, with a note saying the number is discredited -- CEF/vblank aliasing.
   Same defect as the row-10 `D%` cut in the same pass.  See [[vdp1-cef-latches-on-hw]].) */
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

/* ============================================================================
   SATURN MANUAL PRESENT (2026-08-19) -- the owner's theory, rebuilt from the Kronos-corrected
   ST-013 contract (docs/VDP1_MANUAL_PRESENT_VERDICT.md).  THE present since 2026-08-19: the
   owner's Ymir A/B validated v2 (20-24 fps vs 18.4-19.4 auto+leadfill, `w0`, fence 13-15 ms,
   motion holes GONE) -> default and unconditional, the L+B toggle removed, the AUTO 1-cycle
   present retired, the lead-fill PARKED (core r_segs.c boot default 0).  Not yet re-validated
   on console at that date; SlaveDriver precedent (same VBE sequence shipped) covers the HW
   side on paper.  NOT the parked VDP1_MANUAL_CHANGE machinery above: that one gated on
   EDSR.CEF (30-60% HW latch, never on Ymir) with an unfenced blit -- the two poisons every
   past attempt carried.

   AUTO (retired): FBCR=0 1-cycle.  VDP1 erases+swaps EVERY field and replots the rooted list;
   a list rooted at kick K is visible from K+2 while the blit lands wherever the free loop puts
   it -> the screen pairs picture N with list N-1 for 0-1 field per frame, beating with the
   fractional field count.  That stale-pair sliver is the motion hole the CPU lead-fill
   repainted each frame -- both mechanisms now retired together (owner 2026-08-19: "corriger
   le decalage pour desactiver le lead-fill").

   MANUAL: the kick plots ONCE (PTMR=1) into the clean back buffer; the erase is the in-list
   colour-0 polygon (first bank command), so the HW erase choreography is never RELIED on.
   The frame-end fence grants the swap with the VBE erase & change sequence (ST-013 p.40;
   the exact sequence SlaveDriver ships as SCL frame interval 0xfffe): TVMR.VBE=1 +
   FBCR=0x0003 at a fresh vblank-IN, VBE cleared after the OUT edge (red step 7).  That is
   the ONE mode both machines time identically -- swap at the END of that same vblank: Ymir
   evaluates it at its LastLine-end (vdp.cpp BeginHPhaseLeftBorder), the manual says "at the
   end of V-blank ... the frame is changed".  (v1 used the p.38 OUT-window 0x0003 pulse and
   rode one field -- correct per Table 4.3(a), swap at the NEXT boundary, but Ymir executes
   a write landing in its one-line OUT window ~63 us later, a FIELD EARLY: owner captures
   2026-08-19 showed the walls one field AHEAD of the picture for one field per frame = the
   residual holes + the double-door frame.  One hardcoded ordering cannot satisfy both
   contracts; VBE mode sidesteps the divergence.)  The blit starts right after the fence's
   OUT edge: walls and picture commit on the same field, every frame, no beat -> the
   lead-fill (and with it the entry-cover class) becomes belt-and-braces.
   Completion gate = COPR, never CEF: done = COPR parked at the empty-bank END (0xC) or COPR
   unmoved since the kick (Ymir: register not modelled = frozen = pass-through; HW: a re-read
   of a *parked* register).  A moving-but-stalled plot times out on SAT_MP_WD_VBL and is
   force-swapped -- sat_mp_wd counts it and is THE overrun metric here (LP% is tautological
   once the swap is gated on completion).  Revert to AUTO = the legal two-pulse sequence
   (erase 0x0002 in the prior field, then 0x0000; Table 4.3(a) note 5).  SGL cannot clobber
   the pulses: in the SRL_FRAMERATE=0 config _BlankOut's FBCR write is dead code (DMASetFlag's
   single writer is the never-run slSynch pipeline -- LIBSGL.A disasm, 2026-08-18 audit).
   Known transition artifact: the first manual frame runs PTM=1 under the still-auto swap, so
   its plot straddles both buffers -> ~1 frame of wall garbage on toggle-ON.  Accepted for an
   A/B knob; entering via a pre-change would cost more machinery than it removes. */
static int sat_mp_active       = 0;   /* FCM entered (first change pulse issued since boot) */
static int sat_mp_pending      = 0;   /* a kick armed this frame; present due at the fence */
static int sat_mp_wd           = 0;   /* cumulative timed-out plots (force-swapped, may tear) */
static int sat_mp_wait_ms      = 0;   /* fence wait last frame, ms = the measured cap cost */
static unsigned short sat_mp_copr_kick = 0;  /* COPR sampled right after PTMR=1 (Ymir clause) */
static unsigned short sat_mp_end_ca    = 0;  /* staged end-command addr of the list just kicked
                                                (>>3 units): wall path = the empty-bank END; the
                                                menu erase mini-list = its own END.  Staged per
                                                kick, never hardcoded -- the one COPR trap the
                                                2026-08-18 verify pass flagged. */
#define SAT_MP_WD_VBL 4               /* fields before a running plot is force-swapped */

/* SATURN world-things-on-VDP1: per-frame emitted / declined counters (overlay 'th').  Defined here
   (before the SHOW_FPS overlay block that prints them, and unconditionally so the emit path that
   increments them always links) -- the pool struct itself is defined later with the weapon cache. */
static int sat_things_n = 0, sat_things_decl = 0, thing_bake_n = 0;   /* 'th' emitted/declined, 'fb' baked (cache misses) */

#if SHOW_FPS
extern "C" int rp_timeout_count;
extern "C" int rp_to_site[4];            /* core r_parallel.c: per-wait-site timeout split A/P/M/W */
/* SATURN L5 near-wall edge split: the core-side hook pointer + this file's implementation of it. */
extern "C" int (*sat_wall_edge_hook)(int, int, int, int, int, int, int, int, int,
                                     int *, int *, int *, int *, int *);
extern "C" int sat_wall_edge_split(int, int, int, int, int, int, int, int, int,
                                   int *, int *, int *, int *, int *);
extern "C" unsigned int rp_master_ms;   /* master frame ms -> prefixes r_parallel.c's row-18 SLV line */
extern "C" int sat_opt;                  /* core r_segs.c: cumulative perf-lever level L1..L4 (pad L+C) */
static unsigned int dg_frame_count = 0;
static int vdp1_last_cmds = 0;
/* VDP1 transfer-over meter (SEGA VDP1 UM p.52-53).  The real flicker signal: did the plot finish the
   command list in the frame?  LOPR/COPR are cmd addrs in (VRAM byte offset)>>3 units. */
static unsigned short vdp1_lopr = 0, vdp1_copr = 0;   /* raw LOPR (last op, prev frame) / COPR (current op) -- both help the HW unit-check */
static unsigned short vdp1_endca = 0;                 /* computed end-of-list cmd addr (same /8 unit) = LP reference */
static int vdp1_lp_pct  = 100;                        /* % of the list LOPR reached (100 = finished; <100 = transfer-over) */
static int vdp1_tx_used = 0;                          /* wtex cache slots occupied (VRAM limiter) */
/* MEASURED VDP1 budget (2026-07-26) = the heart of the budget-driven LOD.  vdp1_budget_cmds = how
   many VDP1 commands the frame's plot TIME actually paid for, READ from LOPR (not guessed): on an
   overrun it IS the exact reach; on a clean frame it drifts up +1 / THING_LP_CLEAN to re-probe
   headroom.  The things AIMD and the wall-span LOD both allocate against THIS number instead of the
   static slot cap -- so the "default" cap is the real measured per-frame budget, not a guess. */
/* 🔴 RECOVERY RE-TUNED 2026-08-09 -- the controller's clock was the FRAME, and our frame is 10-25x
   longer than whatever rate 24 was chosen for.  Decrease is one-shot multiplicative (`/4`), increase
   was +1 every 24 frames: from a latched 45 back to 248 is 203 steps x 24 frames = 487 SECONDS at
   10 fps, twenty minutes at 4.  So a single transient overrun removed VDP1 sprites for the rest of
   the session -- observed live as `B45` with `ec0` while the SAME spot seconds later read `B0 ec16`.
   "Never reset on level load" was the symptom; this is the defect.
   Now: gate 4 clean frames, and the step is 1/16 of the REMAINING GAP (min 1, cap 8).  Recovery
   stops scaling with how deep the crash was -- ~10 s from 45 at 10 fps -- while the step shrinks to
   +1 near the ceiling, so the fine probing where it matters is unchanged.  The sawtooth this
   produces IS the intended AIMD behaviour; what was broken was only the length of the climb. */
#define THING_LP_CLEAN     4                          /* clean (LP=100) frames between drift-up probes */
#define THING_BUDGET_STEP  8                          /* max commands per probe (gap>>4, floored at 1) */
static int vdp1_budget_cmds  = 0;                     /* measured budget in commands (0 until first sample; WALL_CMD_CAP-bounded) */
static int vdp1_budget_clean = 0;                     /* consecutive clean frames (drift-up counter) */
static int vdp1_budget_map   = -1;                    /* gamemap the current measurement belongs to */
/* WEAPON-ALWAYS-VISIBLE reserve (2026-07-31, replaces the double-emit hedge).  The gun is the LAST
   world command (it must be: VDP1 is a painter, so "above the walls AND above the monsters" = drawn
   after both), which makes "was it displayed?" DIRECTLY observable instead of inferred -- LOPR >=
   its slot.  The reserve is in COMMAND-EQUIVALENTS, not slots: the gun occupies ~3 command slots but
   costs many slots' worth of plot TIME (large sprite), and that ratio is scene- and weapon-dependent,
   so it is LEARNED by AIMD on the direct observation rather than guessed.  Grows fast on a cut (the
   gun must not blink twice for the same cause), decays slowly when the plot keeps reaching it. */
#define WPN_RESERVE_MIN   6                           /* floor = the weapon's own slot cost (clip+gun+flash+slack) */
#define WPN_RESERVE_MAX  64                           /* ceiling: past this we would starve the world to save the gun */
#define WPN_RESERVE_UP    8                           /* AIMD grow step on a cut                                     */
#define WPN_SAFE_DECAY   48                           /* consecutive reached-frames before giving one back           */
static int vdp1_wpn_slot_end  = 0;                    /* slot just past the weapon in the list being BUILT           */
static int vdp1_wpn_slot_disp = 0;                    /* ... in the list currently PLOTTED (what LOPR refers to)     */
static int vdp1_wpn_reserve   = WPN_RESERVE_MIN;      /* command-equivalents withheld from things/walls              */
static int vdp1_wpn_cut       = 0;                    /* frames whose plot did NOT reach the weapon (window count)   */
static int vdp1_wpn_safe      = 0;                    /* consecutive reached-frames (decay counter)                  */
/* Two-engine LOD tuning knobs (HW-tune these -- read rp_master_ms on the row-18 SLV line to calibrate
   SOFT_BUDGET_MS).  The wall LOD only engages when things are already shed and the walls ALONE still
   overrun the VDP1 budget, AND the master (software) has room to take them (else it would cause the
   decrochage we are avoiding).  Relaxes back to the core default when VDP1 fits or the master fills. */
#define SOFT_BUDGET_MS        50                      /* rp_master_ms above this = software saturated -> stop offloading walls */
#define WALL_LOD_TRIGGER       4                      /* VDP1 commands left for things <= this = walls are eating the budget */
#define SAT_WALL_CPU_SPAN_DEF 480                     /* == core r_segs.c default; span relaxes back up to here */
#define WALL_SPAN_MIN         200                     /* floor: cap how many near walls we push to software */
#define WALL_SPAN_STEP         40                     /* per-frame span adjust (AIMD ramp; < BAND so a wall crosses over >=2 frames) */
#define WALL_PREWARM_BAND      96                     /* = core (V1 576 - span 480); kept constant so the CPU/VDP1 handoff band survives the shift */

/* ============================================================================
   SATURN world-things-on-VDP1: SESSION percentile metrics -- so ONE end-of-level capture tells
   the whole story instead of jittery instantaneous values.  Auto-RESET on a MODE change (sat_m /
   SQ), NOT on a level change (user 2026-07-05: "par session, tant que je ne change pas le mode")
   -> the numbers describe the whole run at the CURRENT mode.  A/B = capture M4, switch to M0
   (pad Z), capture again.  Histograms give p50/p90/p99 with no sort: frame time (8ms buckets),
   world-things emitted/frame and declined/frame (direct 0..63).  Plus the occluded-skip avg.
   (The VDP1 plot done-rate that used to be described here went with row 10 `D%` on 2026-08-09 and
   its vblank sampler on 08-10 -- CEF latches 30-60% on real HW, so the rate was never trustworthy.
   Read row 17 `LP%` instead.)
   ============================================================================ */
#define MH_MS_BUCKETS  40          /* 8ms buckets -> 0..320ms frame time */
#define MH_MS_SHIFT    3
#define MH_N_BUCKETS   64          /* things count 0..63 (direct index) */
static unsigned int mh_ms[MH_MS_BUCKETS];
static unsigned int mh_things[MH_N_BUCKETS];
static unsigned int mh_decl[MH_N_BUCKETS];
static unsigned int mh_frames;
static unsigned int mh_ms_mx, mh_things_mx, mh_decl_mx, mh_occ_sum;
/* (mh_bake_sum / mh_emit_sum CUT 2026-08-09: two accumulators, two adds per frame and a divide per
   second, whose only consumer computed `sbpc` and then `(void)`-discarded it.  Row 15 `fb` answers
   the same question live.)
   (mh_vbl_done / mh_vbl_tot CUT 2026-08-10: the 08-09 pass cut their ONLY consumer -- row 10 `D%` --
   and left the pair being incremented once per vblank for nobody, i.e. it created exactly the class
   of dead symbol it was cutting.  The trustworthy VDP1 done signal is row 17 `LP%`; EDSR.CEF latches
   30-60% on real hardware, which is why `D%` went.) */

static void mh_reset(void)
{
    memset(mh_ms, 0, sizeof mh_ms);
    memset(mh_things, 0, sizeof mh_things);
    memset(mh_decl, 0, sizeof mh_decl);
    mh_frames = mh_ms_mx = mh_things_mx = mh_decl_mx = mh_occ_sum = 0;
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
    (void)bake;                      /* consumer cut 2026-08-09; param kept to spare the call sites */
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
   bank, computed where WALL_CMD_CAP is in scope (#defined below fps_update) and read by the
   overlay.  fb_pk_starve (walls dumped when the bank fills) is the definitive "over budget"
   signal.  The floor-bank twin (vd1_fpct / ftexd_trunc) went with the ftex F-build 2026-08-02. */
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
/* SATURN 2026-08-16 -- THE VEIL WAS EXTENDED TO THE SOFTWARE VIEW AND REJECTED ON SIGHT.  The
   line-colour insert CAN be routed to NBG1 (`N1LCEN`, VDP2 manual 1800E8H bit 1) with a mirrored
   ramp above the horizon, at 0 VRAM banks / 0 CRAM / 0 cycles.  It was built, and the owner's
   verdict was *"L1 c'est très moche et inutile. Je n'en veux pas."*  Removed entirely -- the floor-
   only veil below is untouched and still parked at rbg0_linecol_mode = 0.
   Keep the fact, not the code: routing to NBG1 is possible and cheap; it just does not look good. */
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
/* worst-REC frame FULL detail, snapshotted at each new peak (row 14) -- phase split + slave b/Pb */
extern "C" int sat_prof_mx_bw, sat_prof_mx_bp, sat_prof_mx_p, sat_prof_mx_m, sat_prof_mx_b, sat_prof_mx_pb;
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
extern "C" int sat_fb_edge_t;     /* L5: tiers saved by the CPU-borders/VDP1-core near-wall split */
extern "C" int sat_fb_edge_w;     /* L5: tiers that ASKED for the split (the denominator)          */
extern "C" int sat_fb_edge_b[4];  /* L5 bail causes: lateral / magnitude / too-thin / refused      */
/* The L5 RATE instrumentation (row-8 `e<r>/<r> m<r> b<L><M><T><R>`) is REMOVED -- it delivered its
   verdict and the pool needed the bytes back for the wall-lag band.  What it established, so nobody
   rebuilds it: over ~5700 frames the split was REQUESTED 0.1x per frame and taken ~0 times, and the
   alternative population (subdiv squish-guard software dumps, `m`) was ~0 too.  L5 therefore cannot
   move the average by construction; it only ever targeted the nose-to-wall Bp spike, and that A/B
   (/o4 vs /o5 with the nose on a wall, reading row-2 Bp) is still the only thing that can decide its
   fate.  The core counters stay -- they cost one increment and are the hook to re-measure from. */
/* The LAG vblank probe (r/v/@site/px) is REMOVED -- it had done its job and the TLSF pool went under
   its 4 KB floor.  What it established, so nobody rebuilds it: all three display commits land in the
   same field (`ovbuf v0@1`), so the residual trail is NOT a commit-timing or frame-vs-field-beat
   problem; the vblank fence that followed from that theory changed nothing but the frame rate and is
   removed too.  The remaining lag is VDP1's own plot latency, which is irreducible.  See
   [[m7-vdp1-latency-coherent-pair-hold]]. */
/* VDP1 YAW ANTICIPATION IS REMOVED -- four HW rejections, so nobody rebuilds it.  Pre-shifting the
   VDP1 world layer by a predicted frame of yaw was tried as (a) a whole-layer LocalCoord translation,
   (b) the same applied to wall_acc so the per-wall UserClip window followed, (c) a per-corner
   re-projection through xtoviewangle[]/viewangletox[] so the quad STRETCHED instead of sliding.
   The owner's verdict on the last one: "tout a l'air d'empirer par rapport a g0", and on the
   symmetric gains before it: g-4/g+4 indistinguishable, g-8/g+8 both clearly worse.  A displacement
   model that is right about the sign has to improve one direction; symmetric worsening at every
   amplitude means the walls are not DISPLACED.  What replaces it is core/r_segs.c's entry coverage:
   a wall that just came into view is drawn by the CPU for its first frames, because on that frame
   the VDP1 quad is not in the wrong place -- it is not on screen at all. */
extern "C" int sat_wall_entry;    /* core r_segs.c: CPU frames covering a newly-visible VDP1 wall */
extern "C" int sat_seg_frame;     /* core r_segs.c: per-seg visit tag, advanced once per frame here */
/* FIELD LOCK -- PARKED 2026-08-03 (owner: *"supprime wm1, 2, et park wm3"*), SUPERSEDED
   2026-08-19: the manual-present fence (sat_mp_fence) now edge-locks EVERY frame and its
   call site no longer consults this flag -- the variable and sat_field_fence are dead code
   the compiler elides.  Kept as documentation of the Fl1/Fl2 era (see the long note at
   sat_field_fence); reviving it would mean disabling the manual present, which the 2026-08-19
   validation (holes gone, +fps) argues against ever doing. */
static int sat_field_lock = 0;
static int rbg0_rpt_late  = 2;    /* RBG0 rotation-table copy timing, 0/1/2 -- pad R+Left, row-13
                                     `F<m><rp>` 2nd digit.  Declared up here only so the row-13
                                     overlay can read it; the derivation lives at its old home
                                     (search rbg0_rpt_pending). */
/* SKY/FLOOR BOUNDARY MODE (pad L+Down, 1p; row-13 `F<s><rp>` 1st digit).  The owner's artifact:
   *"la limite entre RBG0 et le ciel hardware ne correspond pas à la hauteur max du plan sol, donc on
   voit le ciel au lieu de la texture du sol sur les lignes les plus hautes du sol"* -- and forcing
   the sky OFF entirely (mode 3) removes it, so the SKY BOUNDARY is the limiter.  (That also kills
   the other branch: RBG0 does reach those rows, so the rotation calibration is NOT the target.)
     0 = legacy: the sky MAP is rewritten inside the horizon block, mid-frame.
     1 = DEFAULT, the timing fix: the map write is deferred past the field fence.  The map is VRAM
         and VDP2 reads it DURING DISPLAY, so a mid-frame rewrite takes effect immediately -- while
         the software picture it must agree with only commits at the blit, one fence later.  Between
         the two, the display shows the NEW sky boundary over the OLD picture: move so the floor top
         descends and the sky spreads down over floor rows that are still on screen.  Deferring the
         write to just after the fence puts both at the top of the same field.  (The WINDOW half
         stays before the fence -- it is registers, latched at vblank, so that is where it belongs.)
     2 = 1 + lift the boundary one whole 8 px cell: the sky stops ABOVE the floor top instead of
         meeting it exactly.  The geometric fix, if 1 is not enough.  Costs a cell of sky at the
         horizon (those rows show RBG0 instead).
   (A 4th mode forced the sky OFF entirely.  Removed once it had answered -- see the horizon block.) */
static int sky_mode       = 1;
static int sat_field_n    = 0;    /* fields the last locked frame occupied -- row-13 readback.  MUST
                                     be STEADY: a value flipping N/N+1 means the frame sits on a
                                     field boundary and the beat is back, coarser (judder). */
static void sat_field_fence(void);   /* defined next to the long note in DG_DrawFrame */
/* SATURN 2026-08-08: VDP1 flat quads that had to use the NEUTRAL GREY index because the dominant
   colour was neither primed nor affordable.  Cumulative; printed as `gy` on row 12.  It should be
   ~0 now that the flat path computes the colour when looking costs no disc -- a climbing `gy` is
   the owner's grey walls coming back, and means the budget is genuinely out on those frames. */
int vdp1_wall_nocol = 0;
static int vdp1_wall_drop = 0;   /* walls the core handed to VDP1 that the emit silently dropped --
                                    row 13 `N<orphan>/<drop>/<flip>`, summed over the window.  Watched
                                    at the command pointer, so it catches every early return in
                                    wall_emit/_flat/_banded without auditing them one by one. */
extern "C" int sat_wall_lead_x;    /* core r_segs.c: LEAD-FILL depth in frames -- PARKED 2026-08-19,
                                      boot 0, no chord: the manual present removed the stale-pair
                                      offset it repainted.  Set > 0 to revive for an A/B. */
extern "C" int sat_gov_inert;      /* governor: bitmask of axes PROVEN not to buy time (w/p/lead) */
extern "C" int sat_gov_lead_step;  /* governor rung: 0 full / 1 flat spans / 2 lead-fill off */
extern "C" int sat_lead_mode;      /* core r_segs.c: 0 master-tex / 1 SLAVE-tex / 2 master-flat
                                      (only read when sat_wall_lead_x > 0 -- parked with it) */
extern "C" int sat_wall_dwell;     /* core r_segs.c: frames a flipped seg stays CPU-covered (pad R+Up) */
extern "C" int sat_lead_span_drop; /* core r_segs.c: spans the slave list could not hold             */
extern "C" int sat_lead_cols;      /* core r_segs.c: extra software column-spans drawn by the fill    */
/* px the VDP1 wall quad is grown top/bottom.  0 = texture-EXACT (default): DISTORSP maps the WHOLE
   character corner to corner, so moving the vertices without changing the character stretches the
   texture -- grow*rows/span texels of error at the band edges, worst on FAR walls.  1/2 = the legacy
   grow, live on pad L+Up.  DEFAULT 2 (owner 2026-08-02: "wg2 ferme les trous existants a l'arret"):
   since the MATELAS the character grows with the quad, so the mapping is exact wherever the pad
   covers it and degrades to the old stretch only where it cannot -- the seam is worth more than the
   residual.  Flat/untextured quads (wall_emit_flat) keep their own grow -- no texels to shift. */
static int sat_wall_grow = 2;

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
    /* SATURN 2026-08-06: ONE shared scratch for every overlay row.  Each row was a private
       `static char rXX[45]` -- ~21 of them, ~950 B of HWRAM .bss, i.e. of TLSF pool -- while every
       one is written and Printed back-to-back, so exactly one is ever live.  SRL::Debug::Print
       renders synchronously into NBG3, so sharing is safe.  (rTHp/rFMp were the one pair that
       overlapped; their fill/print order was interleaved just above to make them disjoint too.) */
    static char ovbuf[56];
    static unsigned int frames = 0;
    unsigned int now = vbl_count;
    unsigned int hz  = (us_per_frame == 20000) ? 50 : 60;

    frames++;
    /* SATURN 2026-08-06: fold the `P` sub-brackets into WINDOW MIN/MAX, every frame.  The overlay
       block below only prints once per second, so it would otherwise show whichever single frame
       happened to be last -- useless on a BIMODAL term, and it is why row 20 showed `k131.9` next
       to a row-2 `P11.6` (different frames).  The owner's six same-spot captures read P = 11.6 /
       74.2 / 167.1 / 10.3 / 87.2 / 8.2 with Bw, Bp and M constant: the extremes ARE the signal. */
    {
        /* (the pk_* min/max fold went with the PSP row on 2026-08-07 -- see there) */
    }
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
            static int l_map=-1, l_m=-1, l_sq=-1, l_blit=-1, l_ms=-1, l_cls=-1, l_ns=-1, l_opt=-1, l_wen=-1, l_wb=-1;
            static int l_lx=-1, l_lm=-1;   /* lead-fill chord (pad R+Right): depth + mode */
            static int l_lp=-1;            /* PATCH-LUMP pin (pad L+Left) -- see below */
#if SAT_DIAG_SLAVE_TOGGLES
            static int l_steal=-1, l_wp=-1;
#endif
            if (gamemap != l_map || sat_m != l_m || (sq_wall<<6|sq_sprite<<4|sq_floor<<2|sq_ceil) != l_sq || blit_mode != l_blit
                || sat_mark_suppress != l_ms
                || sat_clear_slave != l_cls || sat_near_sprites != l_ns
                || sat_opt != l_opt          /* pad L+C: the perf-lever ladder IS an A/B -> flush the
                                                profiler + every windowed peak, else /o4 vs /o5 is read
                                                through numbers latched before the switch */
                || sat_wall_entry != l_wen   /* pad L+Left/Right: entry coverage costs software columns,
                                                so En0 vs En1 must be read on a clean Bp window too */
                || sat_wall_grow != l_wb    /* pad L+Up: the grow adds VDP1 fill -> read its cost on
                                               a clean window as well */
                || sat_wall_lead_x != l_lx || sat_lead_mode != l_lm
                                            /* 🔴 2026-08-12: pad R+Right was MISSING from this key,
                                               and it is an A/B like every other entry.  Consequence
                                               caught on a real capture: PK/MXd and the row-20 g/b latch
                                               are cleared only here, so after a chord press the latched
                                               frame could have been drawn under a DIFFERENT lead-fill
                                               configuration than the one row 13 prints on the same
                                               photo -- a 275-second window spanning up to four
                                               configurations.  The A/B was unattributable and nothing
                                               said so. */
                || sat_lpin_on != l_lp      /* 🔴 2026-08-17: pad L+Left was missing here for exactly
                                               the same reason R+Right was, and it is THE toggle of
                                               this build.  Row-20 `c` and row-4 `w` are WINDOW peaks:
                                               without this flush, turning the lump pin off keeps the
                                               fat `c` latched from the pin-ON frames (and vice
                                               versa), so the one measurement the hardware run exists
                                               to make would have read the wrong side of its own A/B. */
#if SAT_DIAG_SLAVE_TOGGLES
                || sat_plane_steal != l_steal || sat_wallprep_slave != l_wp
#endif
               ) {
                RP_ProfReset();
                fb_pk_clamp = fb_pk_mag = fb_pk_starve = fb_pk_px = 0;   /* Phase-0: clean fallback A/B window */
                blit10_sum = blit10_cnt = 0;   /* row-1 'b' precise window: fresh sample on the L+A toggle */
                l_map=gamemap; l_m=sat_m; l_sq=(sq_wall<<6|sq_sprite<<4|sq_floor<<2|sq_ceil); l_blit=blit_mode; l_ms=sat_mark_suppress;
                l_cls=sat_clear_slave; l_ns=sat_near_sprites; l_opt=sat_opt; l_wen=sat_wall_entry; l_wb=sat_wall_grow;
                l_lx=sat_wall_lead_x; l_lm=sat_lead_mode; l_lp=sat_lpin_on;
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
        extern int sat_cd_loads;          /* R1: cumulative GFS_Load chunk commands (watch the warp jump) */
        extern int sat_cd_persector;      /* R1: what the old per-sector path would have issued (baseline) */
        /* row 0 HEADLINE: inst fps, EMA(~4s) avg (the build-comparison number), MST (=1000/fps, the
           master frame ms), to = slave-timeout count (must stay 0), cd = CD read-retries.  Shown in
           every overlay mode except OFF(2); the fps-only mode(1) shows ONLY this row, so the
           mode0<->mode1 fps delta measures the overlay's own per-frame tax. */
        unsigned int mst = inst10 ? (10000u / inst10) : 0u;
        /* SATURN 2026-07-31: `to` used to be ONE never-reset aggregate over five wait sites, so it
           could not distinguish a level-load burst from a steady leak, nor say WHICH wait failed --
           it could not support any conclusion.  Now `to<rate>:<A><P><M><W>`: rate = timeouts in THIS
           ~1s window (the number that actually matters), then the cumulative per-site split --
           A=aux/clear-on-slave, P=plane-split, M=masked-split, W=wall-prep(+1p-dead REC).  Digits are
           clamped to 9 to hold the ~40-column line (a 3-digit MST may clip the trailing ld field). */
        static int to_prev = 0;
        int to_rate = rp_timeout_count - to_prev;
        to_prev = rp_timeout_count;
        if (to_rate > 9) to_rate = 9;
        sprintf(ovbuf, "%u.%ufps a%u.%u MST%u to%d:%d%d%d%d cd%d ld%d/%d ",
                inst10 / 10, inst10 % 10, avg10 / 10, avg10 % 10,
                mst, to_rate,
                rp_to_site[0] > 9 ? 9 : rp_to_site[0], rp_to_site[1] > 9 ? 9 : rp_to_site[1],
                rp_to_site[2] > 9 ? 9 : rp_to_site[2], rp_to_site[3] > 9 ? 9 : rp_to_site[3],
                sat_cd_read_retries, sat_cd_loads, sat_cd_persector);
        if (sat_dbg_overlay_mode != 2) SRL::Debug::Print(0, 0, ovbuf);
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
        char blit_c = 'c';   /* blit path baked to CPU memcpy 2026-07-16 */
        char blit_w = '5';   /* W5 HUD-skip baked permanently ON */
        /* 'b' = PRECISE blit mean in tenths-ms (FRT sat_blit_ms10, windowed since the last L+A
           toggle) -> resolves the ~1.5ms W5/DMA deltas the old integer rounded away.  Folded into
           THIS field (no new overlay row -- rows are saturated across dg_saturn + r_parallel). */
        unsigned int bmt = blit10_cnt ? (blit10_sum / blit10_cnt) : 0u;   /* tenths-ms */
        /* 🔴 `rs` (2026-08-18) = R_RenderPlayerView's PRE-BSP setup -- the slave-clear join,
           R_SetupFrame, R_PostFlatCacheFrame.  `R` here is DERIVED (MST - T - S - b - dg) while
           row 2's Bw/Bp/P/M are MEASURED, and the two differ by ~11,6 ms.  `rs` is the only
           candidate phase in that gap: if it reads ~11 the frame is fully accounted for, if it
           reads ~0 the gap is the slop of a derived number and should be treated as noise. */
        unsigned int rs10 = _f ? (sat_r_setup_frt * 10u / 224u) / _f : 0u;
        sat_r_setup_frt = 0;
        sprintf(ovbuf, "R%u T%u S%u b%u.%u%c%c dg%u pr%u.%u rs%u.%u  ",
                _rec, _tic, _snd, bmt / 10, bmt % 10, blit_c, blit_w, _dg, _pr10 / 10, _pr10 % 10,
                rs10 / 10, rs10 % 10);
        ovbuf[40] = ' ';
        if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 1, ovbuf);
        /* row 17: SPLIT per-view render times, ms (the CLEAN probe for "does M7 lowres actually
           save time in 3/4p?").  v0..v3 = each R_RenderPlayerView (d_ms-bracketed in d_main's split
           loop), k = the single VDP1 flush/kick, =S = sum of the per-view renders.  M7 lowres halves
           ONLY the software pixel-fill INSIDE each v_i (planes + per-column clip + sprite fill),
           NOT the per-view BSP traversal / sprite+seg projection / VDP1 command emission -- so in
           the SHORT 96-row 3/4p quadrants (fill is a MINORITY of v_i) each v_i barely drops M4->M7,
           while in the TALL 160-row 2p halves it drops hard.  Cycle M4<->M7 (pad Z) on the SAME 4p
           scene and compare v0..v3 + k: if v_i is ~flat, 3/4p is emission/BSP-bound (lowres can't
           help); if k dominates, it's VDP1-fill-bound.  Split-only (values are stale in 1p). */
        if (sat_dbg_overlay_mode == 0 && sat_local_players > 1) {
            extern int sat_split_thingcull;   /* core piste-3 */
            unsigned int vsum = sat_spl_v0 + sat_spl_v1 + sat_spl_v2 + sat_spl_v3;
            snprintf(ovbuf, sizeof ovbuf, "SPL %u %u %u %u k%u =%u tc%d bal%d ",
                     sat_spl_v0, sat_spl_v1, sat_spl_v2, sat_spl_v3, sat_spl_kick, vsum,
                     sat_split_thingcull, sat_split_balance);
            SRL::Debug::Print(0, 17, ovbuf);
        }
        /* row 12 (1p): VDP1 REAL-limiter probe (docs/VDP1_LIMITS_SOURCED.md).  Moved off row 17
           (2026-07-25) where the VDP1 weapon sprite covered part of it -- row 12 is empty above LOS
           in the 1p cart/MUS build (ovbuf/CD-streaming only fills it in a CD build; split owns row 17).
           The flicker = "transfer-over" (SEGA VDP1 UM p.53): the plot does not finish the command
           list within the frame.  This row shows the budget-driven LOD ALLOCATOR working.  Fields:
             c  = commands emitted last frame (cap WALL_CMD_CAP=248; the raw COUNT)
             B  = MEASURED VDP1 budget in commands (read from LOPR) -- what the plot TIME actually paid
                  for; when c > B the tail overran.  This is the number we allocate against (0 = not yet
                  measured -> unlimited).
             LP = % of the list LOPR reached last frame -- 100 = finished, <100 = OVERRAN (the flicker)
             ec = things emitted to VDP1 (allocator output; the rest shed to the SOFTWARE fill)
             ws = near-wall->software span (LOD lowers it when the walls alone overrun + master has room)
             tx = wtex cache slots occupied of WTEX_SLOTS=26 (16 small 8448B + 6 narrow 16KB +
                  4 wide 32KB; was 19 in the two-pool map) -- see row 18 `VRM` for tx/bk together
             i  = isolation mode (L+Z: 0 all / 1 no-things / 2 flat-walls; weapon always on VDP1)
             W  = weapon guarantee, reserve/cuts.  The gun is emitted ONCE, as the LAST world command
                  (above walls AND monsters), so "did it display" is read straight from LOPR instead
                  of hedged with a second copy.  reserve = command-equivalents currently withheld
                  from things/walls so the plot always reaches it (AIMD-learned, WPN_RESERVE_MIN..MAX);
                  cuts = frames the plot did NOT reach it since the window reset.  **cuts must stay 0**
                  -- that is the whole acceptance test.  A rising reserve with cuts 0 means the loop
                  is holding the line and paying for it in shed sprites; cuts>0 with reserve at MAX
                  means the scene cannot fit the gun even after shedding everything sheddable.
           HW-VERIFIED 2026-07-26: LOPR tracks on real HW -- a scene that OVERRUNS reads a mid-bank LOPR
           (LP<100 = the flicker; e.g. L6c0/6e8 = 94% -> B = 0.94*c), one that FINISHES reads Lc (LP=100).
           Ymir doesn't model LOPR (never overruns anyway), so LP=100 / B unmeasured there is correct.
           (rp_master_ms -- the B_s software budget the wall LOD gates on -- is on the row-18 SLV line.) */
        if (sat_dbg_overlay_mode == 0 && sat_local_players <= 1) {
            static char ovbuf[56];   /* trailing spaces clear the tail when a field narrows (else "i3"->"i33" ghost) */
            snprintf(ovbuf, sizeof ovbuf, "V1 c%d B%d LP%d%% ec%d ws%d tx%d i%d W%d/%d   ",
                     vdp1_last_cmds, vdp1_budget_cmds, vdp1_lp_pct,
                     sat_thing_emit_cap, sat_wall_cpu_span, vdp1_tx_used, sat_iso_mode,
                     vdp1_wpn_reserve, vdp1_wpn_cut);
            /* row 17, NOT 12: the CD row also prints to 12 and runs LATER, so V1 was being
               overwritten -- the owner's captures show the CD row with V1's tail (`tx13 i0 W6/9`)
               still hanging off the right.  Row 17 is blank in every capture. */
            SRL::Debug::Print(0, 17, ovbuf);
        }
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
            /* (VD1 row DELETED 2026-08-06 -- cut long ago but still formatting cmds + D/B + Dr% +
               a __TIME__ build stamp into a dead 52-byte buffer every frame.  `Dr%` itself is
               DISCREDITED (CEF/vblank aliasing -- see the (void)dr note below).) */
            /* row 4: WINDOWED REC distribution p50/p95/max (tenths-ms) -- robust to the
               single-outlier max (a lone CD hitch) AND to an arbitrary threshold.  p50 =
               typical, p95 = sustained worst, mx = absolute worst (located on row 9).
               Window auto-resets on a config change (above).  ~29 cols. */
            int p50 = RP_ProfPercentile(50), p95 = RP_ProfPercentile(95);
            snprintf(ovbuf, sizeof ovbuf, "REC 50:%d.%d 95:%d.%d mx%d.%d d%d      ",
                    p50/10, p50%10, p95/10, p95%10,
                    sat_prof_rec_max/10, sat_prof_rec_max%10, sat_prof_dropped);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 3, ovbuf);   /* row 3: REC percentiles + VDP1 Dr% */
            /* (PVfill row DELETED 2026-08-06 -- its R+Up/Down knob was cut 2026-07-07 and the values
               baked (sat_plane_vscale=4, sat_plane_border_max=10), yet it still formatted them.) */
            /* row 10: per-PHASE INDEPENDENT peaks (each phase's own worst across the
               window, possibly different frames) -- the basis to size each offload
               (Bp -> slave wall-prep, P -> VDP1/RBG0 floor).  ~31 cols worst case. */
            /* 🔴 ROW 4 REBORN 2026-08-17 -- THE Bp SPLIT.  The owner's open-scene captures killed the
               premise the whole `B` axis was built on: most of the picture was VDP1 walls, the HW
               floor and the HW sky, and `Bp` was still 106 ms.  It cannot be fill.  `Bp` is
               R_StoreWallRange IN FULL, and that runs for EVERY visible seg whatever draws it --
               clipping, visplane marking, silhouettes.  So the question is no longer "how much
               fill" but WHICH HALF:
                 pr = per-seg SETUP  (R_StoreWallRange minus the column loop: trig, scale, texture
                      resolution, the VDP1 routing + lead-fill history search)
                 lp = the PER-COLUMN loop (R_RenderSegLoop proper)
                 wp = prof_wallprep, the whole of it -- `Bw` - wp is the pure BSP traversal
               DECISION RULE, and it is an arithmetic one: a 320-wide screen has ~320-600 wall
               columns TOTAL, so if `lp` carried 100 ms that would be ~200 us PER COLUMN, which no
               handful of adds can cost.  Expect `pr` to dominate => the lever is PER-SEG work, and
               the seg COUNT (ds104) is the multiplier -- not the pixels. */
            {
                unsigned int pr10 = prof_segrout  * 10u / 224u;
                unsigned int lp10 = prof_segloop  * 10u / 224u;
                unsigned int wp10 = prof_wallprep * 10u / 224u;
                snprintf(ovbuf, sizeof ovbuf, "BPS pr%u.%u lp%u.%u wp%u.%u        ",
                         pr10 / 10u, pr10 % 10u, lp10 / 10u, lp10 % 10u, wp10 / 10u, wp10 % 10u);
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 4, ovbuf);
            }
            /* row 16 (moved off row 5 -- row 5 belongs to r_parallel's per-frame SLVi%/w slave-
               occupancy readout, the WORK-DISTRIBUTION meter; the 1/s W72 stamp was stomping it
               once a second): SGL slave work-pointer creep watch (the idle menu/intermission freeze).
               W72 = *(GBR+72) low 16 bits + delta vs the previous ~1s window.  slSetScreenDist
               (per-frame RBG0 transform) bump-allocates +8B here; render frames rewind it at the
               dispatch sites, no-render frames via the DG_DrawFrame fallback reset.  HEALTHY =
               d stays ~0 everywhere (menu, intermission, automap, in-game).  Pre-fix the menu
               crept d~+0x118/s up to the GBR+20 vblank callback = the freeze.  Remove this row
               once HW-validated. */
            {
                unsigned int _gbr; __asm__ volatile ("stc gbr,%0" : "=r"(_gbr));
                unsigned int _w72 = *(volatile unsigned int *)(_gbr + 72);
                static unsigned int _w72_prev = 0;
#if OVL_RETIRED   /* SATURN 2026-08-17: the FORMATTING goes too.  Gating only the Print
                   reclaimed almost nothing, and the TLSF pool is now the binding
                   constraint on this branch (10,19 -> 7,61 KB across the day). */
                snprintf(ovbuf, sizeof ovbuf, "W72 %04x d%+d      ",
                         _w72 & 0xffff, (int)(_w72 - _w72_prev));
                _w72_prev = _w72;
                if (OVL_RETIRED && sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 16, ovbuf);
#endif
                /* 🔴 ROW 16 REBORN 2026-08-17 -- WHO CALLS R_GetColumn.  A capture read `SEG c320 f0`
                   beside `BP g98 n402`: 402 resolutions for zero fills, 98 ms, and every bracketed
                   sub-site of the worst call at ~0.  The owner authorised cutting the useless
                   resolutions; this says WHICH ones they are before anything is cut.
                     w = the seg loop's own tier resolution -- the calls a VDP1-routed wall should
                         not need, i.e. exactly what "option B" would remove
                     p = R_StoreWallRange's routing preamble (R_WallPotatoColor walks a texture)
                     m = R_RenderMaskedSegRange (grates).  ⚠ INVISIBLE in row 20 `g`, which is gated
                         on prof_in_wp -- this is the first time that path is on screen at all
                     o = everything else (r_plane.c's sky column, ...)
                   ms/calls each, per frame, same window as rows 4 and 14. */
                {
                    unsigned int w10 = prof_gc_st[1] * 10u / 224u, p10g = prof_gc_st[2] * 10u / 224u;
                    unsigned int m10 = prof_gc_st[3] * 10u / 224u, o10 = prof_gc_st[0] * 10u / 224u;
                    /* `o` gave up its columns to the LUMP PIN readout (2026-08-17): it read 0 on
                       every capture but one, and `P` is the witness of the fix `c8..c12` called
                       for -- `P0/n` means the pin is yielding and holding nothing, exactly the
                       `pn0/25` failure the composite pin lived through all summer.
                       🔴 AND NOW `p` GOES THE SAME WAY (2026-08-17, 5th HW run).  It has printed
                       `p0.0/0` on every capture it ever appeared on -- the routing preamble makes no
                       R_GetColumn call, `R_WallPotatoColor` is memoised.  Its columns buy the field
                       that would have decided the previous build: `P<rung><kb>/<yields>.<EVICTIONS>`.
                       `yields` only ever counted the floor guard, so `P2122/0` was read as "no
                       pressure" when the ring may have been shedding an entry on every add. */
                    snprintf(ovbuf, sizeof ovbuf, "GCS w%u.%u/%u m%u.%u/%u P%c%d/%d.%d      ",
                             w10/10, w10%10, prof_gc_sn[1] > 9999u ? 9999u : prof_gc_sn[1],
                             m10/10, m10%10, prof_gc_sn[3] > 9999u ? 9999u : prof_gc_sn[3],
                             "-12"[sat_lpin_on % 3], r_lpin_kb,   /* rung: - off / 1 64K / 2 128K */
                             r_lpin_yield > 999 ? 999 : r_lpin_yield,
                             r_lpin_evict > 999 ? 999 : r_lpin_evict);
                    (void)o10; (void)p10g;
                    ovbuf[40] = '\0';   /* five fields: pad, then cut, like row 23 */
                    if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 16, ovbuf);
                    r_lpin_evict = 0;   /* a RATE: evictions per window, unlike the cumulative yields */
                }
            }
            /* row 9: WHERE/WHEN the REC-max frame was (the locator), so the worst frame is
               reproducible.  m=map, x,y=player render pos (map units), a=angle 0-255,
               t=sec into the level.  ~31 cols worst case (6-digit coords). */
            snprintf(ovbuf, sizeof ovbuf, "MX m%d %d,%d a%d t%ds        ",
                    sat_prof_mx_map, sat_prof_mx_x, sat_prof_mx_y, sat_prof_mx_ang,
                    sat_prof_mx_t/35);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 6, ovbuf);   /* row 6: REC-max locator */
            /* row 14: WORST-frame DETAIL, snapshotted at the same peak as the MX locator (row 6) and
               the REC mx (row 3).  Updates ONLY when a new all-time-worst REC frame occurs (persists
               until beaten or a config change) -> a fresh peak is a capture opportunity.  Bw/Bp/P/M =
               that frame's phase split (which phase spiked); b/Pb = the slave's busy%/plane-share AT
               that frame (was the idle slave able to help, or was it a master-serial Bp spike?). */
#if OVL_RETIRED   /* SATURN 2026-08-17: the FORMATTING goes too.  Gating only the Print
                   reclaimed almost nothing, and the TLSF pool is now the binding
                   constraint on this branch (10,19 -> 7,61 KB across the day). */
            snprintf(ovbuf, sizeof ovbuf, "MXd Bw%d.%d Bp%d.%d P%d.%d M%d.%d b%d Pb%d ",
                     sat_prof_mx_bw/10, sat_prof_mx_bw%10, sat_prof_mx_bp/10, sat_prof_mx_bp%10,
                     sat_prof_mx_p/10,  sat_prof_mx_p%10,  sat_prof_mx_m/10,  sat_prof_mx_m%10,
                     sat_prof_mx_b, sat_prof_mx_pb);
            if (OVL_RETIRED && sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 14, ovbuf);
#endif
            /* 🔴 ROW 14 REBORN 2026-08-17 -- SIZE `lp` INSTEAD OF ARGUING ABOUT IT.  Row 4's own
               decision rule said "expect pr to dominate"; four hardware captures said the opposite
               (`pr` 9,9-12,4 against `lp` 85,1-105,0), and the same arithmetic that predicted `pr`
               now says `lp` is impossible: a few hundred wall columns cannot spend 100 ms on adds.
               So count what the loop actually does, with increments only -- no timer, so the probe
               cannot inflate what it measures:
                 c  = column iterations of R_RenderSegLoop
                 f  = colfunc() calls made from inside it (3 wall tiers + the lead-fill's own)
                 k  = pixels those calls wrote, in THOUSANDS
                 lk = the LEAD-FILL's share of those pixels, in thousands (counted on both the
                      master and the slave path, so `L1s` and `L1-` stay comparable)
               THE SUBTRACTION TO READ: R_DrawColumn is ~7 cycles/pixel = ~0,25 us at 28,6 MHz, so
               a fill-bound `lp` of 100 ms needs ~400 k pixels = `k400` -- twelve whole 160x200 M7
               screens.  `k` far below that PROVES `lp` is not fill, and kills the `w` axis on
               arithmetic rather than on a 24-frame probe.  `lk` vs `k` prices the lead-fill in the
               one unit that decides whether the governor needs a finer rung than on/off. */
            snprintf(ovbuf, sizeof ovbuf, "SEG c%u f%u k%u lk%u        ",
                     prof_seg_cols > 99999u ? 99999u : prof_seg_cols,
                     prof_seg_fill > 99999u ? 99999u : prof_seg_fill,
                     prof_seg_px / 1000u > 9999u ? 9999u : prof_seg_px / 1000u,
                     prof_lead_px / 1000u > 9999u ? 9999u : prof_lead_px / 1000u);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 14, ovbuf);
            /* row 7: ACTIVE A/B state -- so a photo is never read against the wrong config.  Kept
               SHORT (<=~40 visible cols; see the debug-overlay-line-width memory): the changing
               knobs FIRST so nothing clips.  M<n>:name = offload mode (pad Z).  ms = mark-suppress
               (pad L+B).  pm = plane split 0stat/1TAS/2rowsplit (pad C).  SQ:<W><F><C><S> software
               quality per zone (f=full l=ld b=band x=flat).  The old fl/cl/sk/wl routing chars were
               dropped -- they are all-on in M4/M6 and all-off in M0, i.e. redundant with M. */
            static const char sqch[4] = { 'f', 'l', 'b', 'x' };   /* full / ld / band / flat */
            /* In a co-op split the SQ toggles drive the SPLIT set (sq_*_view) -- show THAT so the
               photo matches the active config; 1p shows the global sq_*.  Sprite stays global. */
            int sqw = (sat_local_players > 1) ? sq_wall_view[0]  : sq_wall;
            int sqf = (sat_local_players > 1) ? sq_floor_view[0] : sq_floor;
            int sqc = (sat_local_players > 1) ? sq_ceil_view[0]  : sq_ceil;
            int sqs = sq_sprite;
            /* SATURN honesty: floor/ceil/sprite LD is a NO-OP under detailshift (M7 / split-lowdetail;
               the half-rate path is high-detail only, r_plane.c) -> show 'f', not a misleading 'l'.
               Walls untouched (wall LD is what DRIVES detailshift). */
            { extern int detailshift;
              if (detailshift) { if (sqf==SQ_LD) sqf=SQ_FULL; if (sqc==SQ_LD) sqc=SQ_FULL; if (sqs==SQ_LD) sqs=SQ_FULL; } }
            /* `ns` CUT 2026-08-09: sat_near_sprites has no chord (the R+X it was documented for was
               never bound -- see the note at the R+X budget chord) and no other writer, so it read
               a constant 1 forever while occupying a column and reading as a live knob. */
            snprintf(ovbuf, sizeof ovbuf, "M%d %s ms%d pm%d SQ:%c%c%c%c cs%d lr%d/o%d",
                     sat_m, sat_m_name[sat_m], sat_mark_suppress,
                     sat_plane_tas,
                     sqch[sqw & 3], sqch[sqf & 3], sqch[sqc & 3], sqch[sqs & 3],
                     sat_clear_slave, sat_lowres, sat_opt);   /* /o = perf-lever level 0-4 (pad L+C) */
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 7, ovbuf);
            /* row 8: RELIABLE VDP1 load (replaces the CEF-aliased Dr%).
               ⚠ 2026-08-10, legend corrected: THE FORMAT PRINTS ONLY `fbw` AND `fbm`.  Everything
               else described below -- `w%`, `f%`, `fbf`, `e<got>/<want>`, `m`, `b<L><M><T><R>` -- is
               either CUT (w%, 08-09; fbf, 08-02) or was NEVER in the snprintf (the whole L5 edge
               block: ATLAS U8, and r_segs.c:362's own rule says never judge a lever through an
               instrument that cannot show why it did nothing).  The text is kept because it is the
               only surviving specification of those counters, which ARE still maintained -- but do
               not go looking for them on a photo.  fbw = walls dumped to CPU because the command
               bank filled (windowed peak); fbm = tiers refused for the VDP1 +/-1024 coordinate
               limit.  fbw|fbm > 0 = VDP1 genuinely over budget this window (the master pays).
               ---- SPECIFICATION OF THE UNPRINTED FIELDS (do not read these off a capture) ----
               L5 near-wall split as PER-FRAME RATES (one decimal), not peaks: e<got>/<want> = tiers
               split / tiers that ASKED, m = tiers the subdiv squish guard dumped to SOFTWARE (the
               OTHER, much larger population -- what L5 does NOT reach).  Rates because a peak only
               says "it happened once in this window" and forces the reader to photograph a rare
               moment; a rate is stationary, so rarity reads straight off the number: e0.0 with
               w>0 = never fires, e~=w = full coverage, and e<<m = L5 is aimed at the small pile.
               b<L><M><T><R> (still peaks, 1 digit) = why the rest bailed: L lateral (outside the
               view window +/- wall_ext, RESCUABLE by widening), M magnitude (tile leaves the +/-1024
               frame-buffer plane -- HARDWARE, VDP1 UM p.21; no screen split can EVER fix it, only a
               narrower BAKED sub-texture), T interior thinner than SAT_WALL_EDGE_MIN, R refused
               (floor-clearance proof / bank full).  The rate window resets with the profiler, so
               changing /o (pad L+C) starts a clean count for the A/B. */
            /* `w%` CUT 2026-08-09: it was EXACTLY vdp1_last_cmds*100/WALL_CMD_CAP, i.e. row 17 `c`
               expressed as a percentage -- a pure duplicate, and one whose letter (`w` for wall)
               actively misled: the numerator is the WHOLE command list, not the walls. */
            /* MP<act> w<wd> <ms>ms = MANUAL PRESENT (sat_mp_*, THE present since 2026-08-19;
               the L+B toggle and its `on` digit left with the validation).  act = FCM entered
               (first change granted since boot; 0 only before the first fence).  w =
               CUMULATIVE force-swapped plots (the overrun metric -- LP% is tautological once
               the swap is gated on completion; a RISING w across two photos = plots not
               finishing).  <ms>ms = last frame's fence wait = the measured quantisation cost
               of the cap: align-to-vblank + the vblank itself, expect ~2-18 avg ~10, owner
               captures 13-15 (a reading of 14-26+ sustained = a missed change sample -- the
               v1 armed-field cost). */
            snprintf(ovbuf, sizeof ovbuf, "VD1 fbw%d fbm%d MP%d w%d %dms ",
                     fb_pk_starve, fb_pk_mag,
                     sat_mp_active, (sat_mp_wd > 999 ? 999 : sat_mp_wd),
                     (sat_mp_wait_ms > 99 ? 99 : sat_mp_wait_ms));
            /* (fbf -- floor tiles truncated -- dropped 2026-08-02 with the VDP1 floor deport: it
               was structurally 0 in every reachable mode.  fbw is now the sole over-budget bit.) */
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 8, ovbuf);
            /* (The sight-volume deltas that lived here are GONE 2026-08-16.  Their consumers -- `r`
               and `w` on the LOS row -- were dropped for column space in August 2026 and the two
               subtractions kept running every window for nobody, which is exactly the discarded work
               this session has been removing ([[cut-all-useless-work-always]]).  Row 24 `sc` now
               OWNS sightcounts[] and resets it; keeping a cumulative-delta reader alongside a row
               that zeroes the source would have gone negative anyway -- one owner per counter
               ([[debug-overlay-legend]]).) */
            /* C<+/-> = the PHASE-1 WALL CLAMP (core sat_wall_clamp, pad R+A): + = a wall crossing
               the floor line is SPLIT, the occluded part going to the software, the rest staying on
               VDP1; - = no split.  It used to print on the (now cut) FBK row, i.e. NOWHERE -- and
               R+A was silently shared with the lead-fill X cycle until 2026-08-05, so a whole round
               of staircase-hole captures was taken with this bit in an unknown state.  It is the
               first thing to read on any missing-wall-band report.
               En/<d> = CPU frames covering a newly-visible VDP1 wall (core sat_wall_entry, pad
               L+Left/L+Right, 1p).  En0 = off = the pre-2026-08-02 behaviour, where a wall entering
               the view is drawn by nobody on its first frame and shows sky.  Raising it trades a few
               software columns on the walls that just appeared for that hole.
               <d> = DWELL (core sat_wall_dwell, pad R+Up: 0/4/8): once a seg flips CPU<->VDP1 it is
               PINNED to the CPU for <d> frames, so a seg oscillating on the routing threshold cannot
               strobe.  Pins to the CPU only, never to VDP1 -- forcing VDP1 could hit a tier with no
               VDP1 claim and leave it drawn by nobody.  Costs software columns -> watch row-2 `Bp`. */
            /* Wg = VDP1 WALL GROW px (pad L+Up) -- the quad grows by Wg screen px and its character
               by the matching texels (the matelas), so the seam closes without the texture slipping.
               P = DEBUG WALL PAINT state (pad L+X): 1 = every VDP1 wall flat GREEN, 2 = every
               CPU wall flat RED, 3 = both.  This is the "show me what is on VDP1" toggle.
               (`Wm` wall mode and `Fl`/`A` field-lock + wall-age left the row on 2026-08-03 with the
               modes and probes they belonged to.  The wall-age verdict, so nobody re-measures it:
               pinned A2/2 in Fl2 on 4 captures INCLUDING the ones showing the offset -- the walls
               are NOT late by a field and no presentation-timing experiment is worth running.)
               F<s><rp> = the HARDWARE FLOOR pair, both digits about the RBG0 / HW-sky boundary
               (owner's symptom: the sky comes down OVER the floor's topmost rows).
               s (pad L+Down) = SKY/FLOOR BOUNDARY MODE: 0 legacy (map rewritten mid-frame) / 1
               DEFAULT, the map write deferred past the field fence so the VRAM map and the software
               picture commit in the same field / 2 = 1 plus the boundary lifted one 8px cell.
               A removed 3rd mode forced the sky OFF and is what proved the SKY BOUNDARY is the
               limiter -- and that RBG0 does reach those rows, so the rotation calibration is not
               the target.  Three candidates died before it: the floor-window MARGIN (0/4/8/16 px, no
               change -> the WINDOW is not the limiter; note it never moved the SKY boundary), the
               no-floor horizon fallback (artifact visible with its counter at ZERO), and the whole
               VDP1 display-latency family (`A2/2` with the holes unchanged).
               rp (pad R+Left) = RBG0 rotation-table copy timing: 0 early / 1 after the blit (ONE
               FIELD LATE -- VDP2 latches the table at the vblank the blit starts on) / 2 = default,
               before the fence so that same vblank latches it.  Judge it walking straight.
               t = the LARGEST number of game tics a single frame advanced in this window.  Doom's
               logic is 35 Hz fixed; after a long frame (see FMp `mx`) the loop catches up several
               tics at once, so ONE frame moves the camera several tics' worth -- and any residual
               one-field lag is multiplied by exactly that.  t1 = no catch-up happening.
               L<X><m>/<spans> = VDP1 LEAD-FILL.  Pad R+Right cycles the WHOLE state in 5 steps:
               `1s` `2s` `3s` = depth X drawn on the SLAVE, `1-` = X1 drawn by the MASTER (the
               offload A/B), `0-` = off = the reference.  `!` in place of <m> = the span list
               overflowed this window.  X = how many
               RENDERED FRAMES back the "old wall" is taken from -- i.e. how late you believe VDP1
               is.  The software then draws, per column, the new tier's rows MINUS the rows the old
               quad already covered (0, 1 or 2 spans).  spans = how many such spans were drawn this
               window: 0 means the fill did nothing (off, or old == new = VDP1 in phase), a big
               number means the view moved a lot and the fill is carrying it.  Sweep X and keep the
               smallest one that closes the hole -- that value IS the VDP1 lag, measured by eye.
               (`r` = the old d_sc0 sight counter, dropped for the room 2026-08-03; `w` = d_sc1, the
               full BSP LOS walk count, followed it 2026-08-05 when `C` arrived -- adding C clipped
               `<spans>` off the 40-col edge on the owner's first capture, and this row is the WALL
               row, not the LOS row.  Both live in `sightcounts[]` if ever needed again.) */
            {   extern int sat_wall_nodraw, sat_wall_flip;
            snprintf(ovbuf, sizeof ovbuf, "LOS C%c En%d/%d Wg%d P%d N%d/%d/%d F%d%d L%d%c/%d ",
                     sat_wall_clamp ? '+' : '-',
                     sat_wall_entry, sat_wall_dwell, sat_wall_grow, sat_wall_paint,
                     (sat_wall_nodraw > 999 ? 999 : sat_wall_nodraw),
                     (vdp1_wall_drop  > 999 ? 999 : vdp1_wall_drop),
                     (sat_wall_flip   > 999 ? 999 : sat_wall_flip),
                     sky_mode, rbg0_rpt_late,
                     sat_wall_lead_x,
                     sat_lead_span_drop ? '!' : "-sf"[sat_lead_mode % 3],
                     (sat_lead_cols > 9999 ? 9999 : sat_lead_cols));
            sat_wall_nodraw = 0; vdp1_wall_drop = 0; sat_wall_flip = 0; sat_lead_cols = 0; sat_lead_span_drop = 0; }
            /* row 13: was row 5, but r_parallel's SLVidle ('SLV') p3 row ALSO writes row 5 in
               the shipping (rp_disabled) config -> they collided.  Moved to the free row 13. */
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 13, ovbuf);
            /* row 15 (SCU-DSP feasibility, deliverable #1): per-frame sprite cost split.
               pj = R_ProjectSprite time (the arithmetic a DSP could offload); fl = master
               R_DrawVisSprite fill (memory-bound, ~2x for total incl. slave right-half);
               n = things projected / vissprites filled.  Hold a monster-heavy scene still
               and read: pj<<fl => projection is not the sprite cost, so offloading it is
               pointless.  Tenths-ms; FRT-quantised (~0.02ms/tick) so pj jitters +-0.1ms. */
            int sp_pj = 0, sp_fl = 0, sp_np = 0, sp_nd = 0;
            RP_SprStats(&sp_pj, &sp_fl, &sp_np, &sp_nd);
            /* th e/d = world-things emitted on VDP1 / declined; fb = baked THIS frame (instant).
               On row 15 (bottom, clear of centre-screen monsters that hide the THp row).
               (`sb` = SESSION bake% was described here until 2026-08-10; mh_bake_sum/mh_emit_sum
               were deleted on 08-09 and the field had never been in the format string.)
               ⚠ `th` and `td` on this row RUN ON DIFFERENT CLOCKS: th<e>/<d> is PER FRAME (zeroed at
               the bank build, :5374/:5688), td is a ~1 s WINDOW sum (zeroed at the row-18 block).
               So `th0/0 td12/3/40` is CONSISTENT, not a contradiction -- do not read it as a bug.
               ⚠ And `td0/0/0` next to `th0/0` does NOT mean "VDP1 declined nothing": every exit path
               INSIDE the platform hook bumps one of the three, so all-zero means the hook was never
               CALLED -- the sprites were rejected upstream by core's compiled area floor
               (core/r_things.c:1649-1656), which counts nothing.  See
               [[thing-area-floor-kills-vdp1-sprites]]. */
            /* 2026-08-09: `ec` was an EXACT duplicate of row 17's, and `ef` (thing_emit_floor) was
               only ever assigned 0 in two places and read nowhere -- a constant presented as
               controller state, which is the most expensive kind of field there is.  Both cut, and
               the columns go to `td`, which was already being accumulated AND reset every window
               and simply never printed: the worst of both worlds.  `td` says WHY a world sprite was
               refused by VDP1 -- size / no free slot / command budget -- so it belongs exactly here,
               next to the th<emitted>/<declined> it explains. */
            snprintf(ovbuf, sizeof ovbuf, "SPR fl%d.%d n%d/%d th%d/%d td%d/%d/%d fb%d ",
                     sp_fl/10, sp_fl%10, sp_np, sp_nd,
                     sat_things_n, sat_things_decl,
                     thd_size, thd_slot, thd_budget, thing_bake_n);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 15, ovbuf);
            /* row 18: VDP1 VRAM PRESSURE -- the two things the map re-cut needs and that nothing
               reported before (2026-08-05).  Everything here is a ~1 s window sum, reset on print.
                 tx<res>/<tot> = WTEX wall-texture slots RESIDENT / total (19 = 15 narrow 16KB + 4
                   wide 32KB = 368KB, 72% of VDP1 VRAM).  PERSISTENT LRU -> tx climbs with level
                   PROGRESS (distinct wall textures seen so far), it is NOT a per-frame occupancy.
                 bk = wall-texture RE-BAKES this window = the thrash signal.  tx short of tot with
                   bk 0 => the wall cache is idle and slots can be ceded to the command banks; bk
                   staying high => the level already thrashes and ceding would cost real ms.
                   THIS IS THE GATE on cutting WTEX -- do not cede a slot without reading it first.
                 q  = resolves REFUSED this window because the only candidate slot was still being
                   re-plotted by VDP1 from the DISPLAYED command list (3-state lock, 2026-08-09).
                   Each one is a wall drawn as a flat coloured quad for ONE frame INSTEAD of showing
                   somebody else's texture -- i.e. `q` counts the "inversions de textures" that used
                   to happen.  q 0 with bk high = plain LRU churn, the guard is not the limiter;
                   q rising = the pool is genuinely too small for the view and the honest fix is
                   fewer distinct textures on screen, not a bigger risk window.
                 td<size>/<slot>/<budget> -- ⚠ PRINTED ON ROW 15, not here (2026-08-09); this legend
                   stayed behind in the row-18 block and a reader decoding a photo would hunt for it
                   on the VRM line.  Why a world sprite was REFUSED by VDP1 (it then falls back to
                   the software masked fill).  All three used to be one opaque `th` decline:
                   size   = patch bigger than THINGS_TEX_SLOTSZ (3584 B).  The refused sprites are
                            the NEAR ones = the most expensive software fills => this is the counter
                            that says whether 1p wants FEWER, BIGGER slots.
                   slot   = every slot already feeds this frame's list => raise THINGS_TEX_SLOTS
                            (the MP case: 4 views + up to 3 other-player colours share 4 slots).
                   budget = command bank / split queue full => raise VDP1_BANK_CMDS.
                 lb<budget>:<wall>/<plane>/<sprite>.<nocol> = the per-frame TEXTURE LOAD BUDGET, in
                   MILLISECONDS OF DISC (pad R+X cycles 10/20/40/0; 0 = off = the old ungated
                   behaviour; DEFAULT 20 since 2026-08-07 -- it used to be a count of reads AND
                   default-off, i.e. armed only by the chord).  ⚠ 2026-08-10: this legend said
                   `<flat>`, ONE counter; the format has printed THREE since the plane and sprite
                   gates landed.  <wall>/<plane>/<sprite> = tiers drawn flat / planes drawn potato /
                   sprites skipped this window because texturing them would have hit the disc and the
                   budget was spent -- <wall> is the number to trade against `Bp` on row 2.
                   ⚠ <sprite> is NOT purely a budget refusal: r_things.c:1505 is the SLAVE path and
                   is unconditional, and it produces the half-sprite artefact (left half drawn, right
                   half missing).  <nocol> = of the wall+plane flats, how many had NO cached dominant
                   colour and fell back to the neutral index (a texture never yet seen at all; it
                   self-heals the first time the texture is drawn textured).
                   ⚠ ANY non-zero here arms sat_budget_refused (core/r_segs.c:259), which is STICKY
                   and has NO CLEAR SITE: from that instant sat_wall_io_flat primes R_WallPotatoColor
                   for every non-IO-free tier, walking every other column of a whole texture through
                   R_GetColumn -- INSIDE the Bp bracket.  Memoised per texture per level, so it
                   cannot sustain a spike, but it can produce a single 150 ms outlier frame. */
            {
                /* SATURN 2026-08-14: `cb<builds>/<distinct>` -- the two numbers that separate the
                   only worlds left for the R_GenerateComposite residual (row-20 `k33`), now that the
                   1p composite pool is dead by arithmetic (`xc0/0/60`: floor rung wants 96 KB
                   contiguous, the level had 60).  distinct << builds = THRASH (the same few
                   textures rebuilt in a loop -> something to win by keeping them alive);
                   distinct ~= builds = CHURN (new multi-patch textures constantly -> no cache can
                   help and the only lever is a cheaper BUILD). */
                snprintf(ovbuf, sizeof ovbuf, "VRM tx%d/%d bk%d q%d cb%d/%d lb%d:%d/%d/%d.%d   ",
                         vdp1_tx_used, vdp1_tx_total,
                         (wtex_bakes_win > 9999 ? 9999 : wtex_bakes_win),
                         (wtex_qrefuse > 999 ? 999 : wtex_qrefuse),
                         (r_composite_builds > 999 ? 999 : r_composite_builds),
                         (r_composite_distinct > 999 ? 999 : r_composite_distinct),
                         sat_tex_load_budget,
                         (sat_wall_flat_io  > 999 ? 999 : sat_wall_flat_io),
                         (sat_plane_flat_io > 999 ? 999 : sat_plane_flat_io),
                         (sat_spr_flat_io   > 999 ? 999 : sat_spr_flat_io),
                         (sat_wall_flat_nocol + sat_plane_flat_nocol > 999
                          ? 999 : sat_wall_flat_nocol + sat_plane_flat_nocol));
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 18, ovbuf);
                wtex_bakes_win = thd_size = thd_slot = thd_budget = wtex_qrefuse = 0;
                R_CompositeWindowReset();   /* builds + distinct + the 16-slot set, one writer */
                sat_wall_flat_io = sat_wall_flat_nocol = 0;
                sat_plane_flat_io = sat_plane_flat_nocol = sat_spr_flat_io = 0;
            }
            /* row 19 FLT: the RESIDENT FLAT POOL (core/r_flatcache.c).  Counters are CUMULATIVE
               PER LEVEL on purpose -- the whole claim is that `ld` (real flat disc reads) stops
               climbing once the neighbourhood is resident, so a rate would hide exactly the thing
               to look at.  `A+`/`A-` = the R+Z live A/B (the slab stays carved in both states, so
               both sides have an identical memory layout -- see [[interbuild-perf-noise]]).
                 p<n>  slots carved (0 = zone too tight at level load -> pool-less, old behaviour)
                 r<n>  slots holding a flat right now
                 ld<n> slot fills = flat lumps actually read from the disc THIS LEVEL
                 ev<n> LRU evictions (climbing with ld = the working set exceeds the pool)
                 f<n>  views where every slot was already busy -> fell back to the zone path */
            {
                snprintf(ovbuf, sizeof ovbuf, "FLT A%c p%d r%d ld%d ev%d f%d          ",
                         sat_flatcache_on ? '+' : '-',
                         sat_flatcache_slots, sat_flatcache_live,
                         (sat_flatcache_load  > 99999 ? 99999 : sat_flatcache_load),
                         (sat_flatcache_evict > 99999 ? 99999 : sat_flatcache_evict),
                         (sat_flatcache_full  > 9999  ? 9999  : sat_flatcache_full));
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 19, ovbuf);
            }
            /* row 20 PSP REMOVED 2026-08-07 -- its question is CLOSED and the pool needed the
               ~350 B for the boot sprite-header sweep.  What it established, in three capture
               rounds, so nobody re-derives it:
                 row-2 `P` ~= `k` (the VDP1 wall kick) ~= its TAIL ~= R_EmitWorldThingsVDP1,
                 and `c` ~= `e` in 13/13 captures ==> that call was 96-99 % DISC WAIT,
                 2-7 CD commands per frame at ~37 ms, because its W_CacheLumpNum was the one
                 sprite path never gated by sat_tex_load_budget.  Gated; `P` fell to 7-11 ms.
               Innocent, measured, do NOT re-test: R_DrawPlanes (1 ms), the texture bake (0), the
               weapon (0), the wall emit loop (4-8 ms flat), NetUpdate (0), the slave join (0), and
               the Z_Malloc rover (< 1000 steps/frame).  Read the outcome on row-2 `P` and row-12
               `L` now.  The producers survive (sat_p_thg10 and friends) -- one snprintf to revive. */
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
            /* ⚠ 2026-08-07: cutting this row to buy pool for the level-load probe made the pool go
               DOWN, 4.98 -> 4.80 KB (pre-flight FAIL).  The pool is `__heap_end - _end` and `_end`
               moves with SECTION LAYOUT, so it is NOT a monotone function of code size -- the same
               trap as [[interbuild-perf-noise]] one level lower.  **Never "free pool" by deleting
               code without re-measuring; the deletion can cost you.** Row restored. */
            snprintf(ovbuf, sizeof ovbuf, "THp n%d/%d d%d o%u.%u f%u    ",
                     t_p50, t_p99, d_p99, occ10 / 10, occ10 % 10, mh_frames);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 9, ovbuf);
            /* `D%` CUT 2026-08-09 and REPLACED by `hp`.  D was the VDP1 plot done-rate sampled from
               EDSR.CEF -- and CEF is known to LATCH 30-60% of the time on real hardware
               ([[vdp1-cef-latches-on-hw]]), which the row-3 `Dr%` note had already concluded
               independently before discarding its own copy of the same number.  Two findings, one
               discredited field, and a trustworthy replacement already on screen: row 17 `LP%`.
               `hp<peak>/<cap>!<fail>` = newlib sbrk high-water vs HEAP_SIZE, IN BYTES, plus the
               refusal count.  RESTORED, not added: syscalls.c told the reader three times to "watch
               row-22 hp" and row 22 did not exist, while the same file names this heap as the
               project's designated FIRST pool lever.
               🔴 2026-08-12: BYTES, not KB.  The `>>10` is what made this field ambiguous exactly when
               it mattered -- `hp1` covers [1024, 2047], a 2x uncertainty, and the whole P0 decision
               (HEAP_SIZE 12 KB -> 4 KB, +8192 B of pool) had to be sized on the pessimistic end of it.
               At a 4 KB cap a KB-resolution field would read `hp1/4k` right up to the failure.
               🔴 And `!<fail>` = dg_heap_fail, _sbrk's ENOMEM branch, which was MUTE.  ANY non-zero
               means RAISE HEAP_SIZE: the failure mode is a LOAD-gate halt in M_StringJoin /
               M_StringDuplicate BEFORE the first frame, not a perf regression.  (The lumpinfo calloc
               this comment used to fear is NOT the risk -- that array is a Z_Malloc in the LWRAM zone,
               and `calloc` is referenced by no project object.  The late allocator is m_menu's fopen.) */
            snprintf(ovbuf, sizeof ovbuf, "FMp %d/%d/%d mx%u hp%d/%d!%d ",
                     f_p50, f_p90, f_p99, mh_ms_mx,
                     dg_heap_peak, dg_heap_size, dg_heap_fail);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 10, ovbuf);
            /* row 11 (ENDGAME limits high-water): how close this ~1s window got to the render
               HARD-HALT caps that I_Error-freeze a big WAD (docs/ENDGAME_ROADMAP.md Axis 2).
               vp = peak visplanes / MAXVISPLANES(256); ds = peak drawsegs / MAXDRAWSEGS(256);
               zf = zone free (KB); lg = largest contiguous purgeable run (KB) = the fragmentation-
               vs-exhaustion signal.  vp/ds are core running-maxes zeroed here each window; when
               either climbs toward 256 on Doom II MAP13/15 that is the cap that crashes next.
               🔴 vp IS NOW `vp<peak>.<poolovf>` (2026-08-09).  The second digit is the one that was
               missing for months: `vp` alone is measured against MAXVISPLANES 256, but the SPAN POOL
               holds only VP_POOL_PLANES = 64 plane-pairs.  Past 64, every overflower shares ONE
               fallback slice pair and each new one's memset wipes the previous one's spans -- so at
               `vp120.0` you are fine and at `vp120.28` you are looking at a graceful-but-real span
               glitch that the old row could not distinguish.  It is a window HIGH-WATER (core keeps
               it across the per-view reset), NOT a freeze: raise VP_POOL_PLANES if it is ever hot.
               ⚠ THE SECOND DIGIT COUNTS SLICES, NOT PLANES.  r_visplane_pool_ovf++ lives inside
               R_PoolSlice (core/r_plane.c:117) and R_PoolSlice is called TWICE per plane (:580-581
               in R_FindPlane, :692-693 in the R_CheckPlane split), so the printed value is 2x the
               number of glitched planes: `.28` = 14 planes.  HALVE IT before you reason about it.
               🔴 SPLIT 2026-08-10: `op`/`tc`/`rl` MOVED TO ROW 22.  SRL::Debug::Print gives EXACTLY
               40 visible cells (320px / 8px font) and this row rendered up to 59 -- so `rl` was off
               the right edge in EVERY capture ever taken, and `tc` in most of them.  That is not
               cosmetic: ATLAS says `rl>0` invalidates every LOOK finding of a session, i.e. the row
               was silently voiding the captures it was supposed to validate.  The row cannot be made
               to fit by shortening labels (10 conversions, 29 worst-case digit columns), so it is
               split by READ CADENCE: continuous high-waters stay here, guard latches that read 0 in
               every healthy frame go to row 22.  The trailing pad is REQUIRED -- nothing clears the
               text rows per frame, and this row no longer always exceeds 40 (it used to hide its own
               ghosting off-screen).  Worst case now 39 + pad; the old 59 also overran ovbuf[56]. */
            snprintf(ovbuf, sizeof ovbuf, "LIM vp%d.%d ds%d ss%d%s zf%dk lg%dk       ",   /* SATURN: ss = solidsegs peak vs MAXSEGS 32, '!' = overflow guard fired (M7 freeze root-cause) */
                     r_visplane_peak, r_visplane_pool_ovf_pk,
                     r_drawseg_peak, r_solidseg_peak, r_solidseg_ovf ? "!" : "",
                     Z_FreeMemory() >> 10, Z_LargestAllocatable() >> 10);
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 11, ovbuf);
            /* row 22 (GUARD LATCHES): the three "a crash was prevented" counters evicted from row 11.
               op = openings sink (PER VIEW, zeroed in R_ClearPlanes -- in 3/4p this shows the LAST
               view only); tc = CONDEMNED TEXTURES (composite OOM sentinel, sticky per texture for the
               whole level -- a non-zero tc means those walls are flat for good, so it changes how you
               read `gy` and `st`); rl = short CD reads zero-filled and then CACHED.  All three are
               cumulative since boot except op.  `rl>0` VOIDS THE LOOK GATE for the session -- that is
               why it may never again share a row with anything that can push it off the edge.
               zb = the ZONE BLOCK COUNT, a free by-product of Z_LargestAllocatable's walk (2026-08-12).
               It sits here because it is a zone-health number, and because it is the missing factor in
               the 214 ms R_GetColumn hole: R_GenerateComposite calls Z_LargestAllocatable TWICE per
               build on the 1p path, that function is O(blocks), and nobody knew whether "blocks" meant
               600 or 1500 -- 0.4 ms vs 1.6 ms per walk.  MEASURED 2026-08-12: zb reads 762..802 over
               five captures, so ONE walk is ~790 x 30 cyc = ~0.83 ms.  That settled it: the allocator
               is the subject.
               It is a LAST-CALL value, not a peak: whatever the most recent walk saw -- and since the
               early-exit form Z_CanAllocate stops at the first sufficient run, zb now reads LOW on a
               healthy zone and climbs toward the zone total as it tightens.
               🔴 zw (2026-08-14) REPLACES the 08-12 `zc`, which was UNUSABLE FOR THE SAME REASON `cb`
               was: it summed over the ~1 s overlay window while row-20 `g`/`n` are latched to ONE
               frame (the PK-Bp frame), so `zc149` at 3.7 fps meant ~40 walks per frame, not 149, and
               dividing g by it was the exact clock error already catalogued two days earlier.  And zb
               could not stand in: the overlay's OWN Z_LargestAllocatable() call for row-11 `lg` runs
               immediately before this print and clobbers z_block_count, which is why zb read 785..808
               on every capture regardless of scene -- it was reporting the zone total, not the hot
               path's walk depth.
               zw = zone BLOCKS walked on the SAME FRAME as `g`, latched in core/r_parallel.c's PK-Bp
               block.  It needs no division and no clock conversion:
                    walk_ms = zw x ~30 cycles / 28600
               so the subtraction against `g` is direct.  THE TEST: if walk_ms accounts for most of
               `g`, r_data.c:615's per-column zone walk is the R_GetColumn hole and the early-exit
               Z_CanAllocate must be made to bite harder (or the call hoisted out of the column loop).
               ANSWERED 2026-08-14: walk_ms was 0,59 ms of 183 -- the walk is exonerated, and the
               residual was neither a baseline nor a miss: `e46` of `x46648` put 99 % of the worst
               call in R_GenerateLookup's vanilla `printf`, which on Saturn blits a 26-row console.
               `np` below counts the textures that reach that site (see core/r_data.c).
               Row 21 is deliberately left free: it is claimed by the LOD governor row. */
            /* `xc<use>/<poolKB>` = the 1p composite-cache A/B (pad L+Right).  `xc0/32` = the slab is
               CARVED but inert (today's shipping behaviour); `xc1/32` = composites live in it.  The
               pool KB must be identical on both sides -- if it differs, the two photos are not the
               same experiment. */
            /* `xc<use>/<poolKB>/<lf>` -- `lf` = Z_LargestAllocatable (KB) AT THE CARVE ATTEMPT, which
               is the only field that can explain a pool of 0.  The ladder's floor rung needs
               32 KB slab + 64 KB margin = 96 KB CONTIGUOUS at level load, and the margin is what
               serves the ~35 KB sky/face patches during play -- it must not shrink.
                 lf 0    => R_SetupTextureCaches returned BEFORE the carve (streaming off)
                 lf < 96 => the ladder correctly REFUSED; the pool does not fit, and that is
                            arithmetic, not policy.
               ⚠ `zb` was DROPPED here: it is clobbered by row 11's own Z_LargestAllocatable() call
               for `lg`, so it never measured the renderer.  `zw` is the real one. */
            /* `pn<KB>/<yields>` = the COMPOSITE PIN (core/r_data.c), pad L+Left.  KB currently held
               non-purgeable; `yields` = times the whole ring was released because a 48 KB run could
               no longer be found.  A CLIMBING yield count means the pin is fighting the zone and
               buying nothing -- that is the signal to turn it off, and it is why the pin can never
               cause the contiguous-OOM that killed the 1p slab.
               `pf<pct>` = the worst useful fraction of a patch decode this window: how far into the
               patch R_GenerateComposite's copy actually reached.  Low = bounding the decode (the
               R_GenerateLookup trick) would take the rest back for free.
               ⚠ `xc` DROPPED: the 1p composite pool is closed by arithmetic (`lf60` < the 96 KB the
               floor rung needs), so it printed a constant.  `zb` was dropped earlier -- it was
               clobbered by row 11's own Z_LargestAllocatable() call for `lg`.  `zw` is the real one. */
            /* `Lo<step>/<hits>` = the DISTANCE LOD (pad L+B).  step 0 = off, 1..3 = rw_scale under
               FRACUNIT/16, /8, /4 -- progressively nearer tiers flattened to their dominant colour
               instead of paying R_GenerateComposite.  `hits` = tiers flattened this ~1 s window.
                 hits 0 while step > 0  => the threshold does not bite, go up a step
                 hits climbs but `k` (row 20) does not fall => the composites being built are NOT
                 the distant ones, and the LOD is aiming at the wrong walls
               ⚠ `pn` dropped: the composite pin is default-OFF and inert (`lg` 24-38 KB vs its
               48 KB floor); the chord and its counters still exist, see the legend. */
            {
                int lo_step = sat_lod_eff <= 0  ? 0
                            : sat_lod_eff <= 200 ? 1
                            : sat_lod_eff <= 400 ? 2 : 3;
                /* Trailing spaces sized for the WIDEST this row ever prints: `Lo3/9999 pf100` is
                   4 chars longer than `Lo0/0 pf10`, and SRL::Debug::Print does not clear the tail --
                   which is why the owner's capture read `pf100 0`, a leftover digit from the
                   previous, longer line. */
                snprintf(ovbuf, sizeof ovbuf, "GRD op%d tc%d rl%d zw%u np%d Lo%d/%d pf%d       ",
                         r_opening_ovf, r_composite_ovf, r_readlump_short,
                         (sat_bp_zw > 999999u ? 999999u : sat_bp_zw),
                         (r_nopatch_col > 9999 ? 9999 : r_nopatch_col),
                         lo_step, (sat_wall_lod_hits > 9999 ? 9999 : sat_wall_lod_hits),
                         r_composite_pf);
            }
            if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 22, ovbuf);
            /* ROW 21 -- THE LOD GOVERNOR, the row that was reserved for it all along.
                 A<0/1>  0 = manual rung (or off), 1 = the governor is steering
                 d<B/P/M/-> the phase it elected last time it fired; `-` = held (Bw dominated, and
                         Bw has no quality knob at all, so electing it would degrade the innocent)
                 w<0..3> wall-LOD rung: 0 = full quality, 3 = 800 px
                 p<0..2> plane rung applied as a FLOOR over the owner's SQ (0 none / 1 LD / 2 FLAT)
                 e<±n>   ⚠ **SIGNED WHOLE MILLISECONDS OF INTEGRATED ERROR** -- one accumulator, not
                         two.  >0 = behind the 70 ms render target, <0 = ahead.  Fires a degrade at
                         +300, gives quality back at -900.
                         🔴 The two-counter version (08-15/16) NEVER FIRED: each branch reset the
                         other, and with `REC 50:36.0 95:86.0` the median frame took the low branch
                         and wiped the debt every time.  If `e` sits pinned at one value while the
                         rungs never move, that failure is back.
                 px<n>   the wall threshold in force (sat_lod_eff)
                 nr<n>   tiers the DISTANCE FLOOR rescued: small on screen but inside
                         sat_lod_mindist, i.e. foreground the LOD is forbidden to flatten.  0 means
                         the floor is inert and the area rung is doing all the work.
                 rc<n>   sprites dropped by the ROLE CULL (~1 s).  Independent of everything else
                         here; it is on this row because there is room, not because it is governed.
               ⚠ The target is on `rend` = Bw+Bp+P+M, which is NOT the frame: MST runs ~20-25 ms
               higher.  70 ms of render defends roughly 10-11 fps. */
            {
                int gov_e = sat_gov_debt / 10;
                if (gov_e >  999) gov_e =  999;
                if (gov_e < -999) gov_e = -999;
                /* `nr` (near-rescues) RETIRED 2026-08-16 for `sb`: it validated the distance floor,
                   which is settled, and the budget is the half of the `B` axis now doing the work.
                   `sb<budget>/<cut>` = the rung's seg allowance, and how many segs it actually
                   flattened this ~1 s window.  `sb0/0` while `w>0` means the budget is inert and
                   the AREA rung is carrying the axis alone -- which is exactly the state that let
                   `Bp110,8` survive three governor fires on `ds118`. */
                snprintf(ovbuf, sizeof ovbuf, "GOV d%c w%d p%d e%d sb%d/%d L%d i%d rc%d ",
                         (char)sat_gov_axis, sat_lod_auto_step, sat_gov_p_step,
                         gov_e, sat_seg_budget,
                         (sat_seg_budget_cut > 999 ? 999 : sat_seg_budget_cut),
                         sat_gov_lead_step, sat_gov_inert,
                         (sat_thing_role_cut > 999 ? 999 : sat_thing_role_cut));
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 21, ovbuf);
                /* the field's own row owns its reset ([[debug-overlay-legend]]) */
                sat_wall_lod_near = 0; sat_thing_role_cut = 0; sat_seg_budget_cut = 0;
            }
            /* ROW 24 -- THE GAME TIC, BROKEN DOWN.  Row 1 `T` is 69-83 ms on HARDWARE (~40 % of a
               181-222 ms frame) against 8-14 ms for the same build on Ymir, and until now NOTHING
               inside it was timed -- the renderer hunt was optimising `R` with the largest single
               cost invisible.  This row exists to size it before anything is built.
                 T<ms>   TryRunTics on the SAME FRT clock as `th`/`s`.  ⚠ NOT row-1 `T`, which is
                         d_ms-based and was seen SATURATING at 72-73 ms on hardware across three
                         different frame rates while the thinkers alone measured 106-110 -- a
                         quantity that stops moving while the work grows is a clock artefact.  When
                         the two disagree believe THIS one; agreement means both clocks are healthy.
                 th<ms>  P_RunThinkers, MEAN MS PER FRAME (same clock, so `T - th` is the honest
                         "everything else in the tic": player move, specials, the tic loop itself)
                 s<ms>   the full BSP walk inside P_CheckSight, same clock.  A SUBSET of `th`.
                 sc<r>/<w> sight REJECT trivial-rejects / full walks, ~1 s window.  `r0` means NO
                         REJECT matrix, so EVERY check walks the tree.  MEASURED at 13-21 ms/frame
                         on console, which is what size-gated the skip in p_setup.c P_LoadReject.
                 hc<n>   temporal sight-cache hits (~1 s).  Large `hc` with large `s` means the
                         cache is working and the survivors are still expensive.
               DECISION RULE: if `s` is most of `th`, the lever is the sight oracle. If `th - s` is,
               the thinkers themselves are memory-bound and the lever is data layout, not
               algorithms.  Either way MEASURE FIRST ([[budget-before-mechanism]]). */
            {
                unsigned int th10 = _f ? (sat_tic_think_frt * 10u / 224u) / _f : 0u;
                unsigned int sg10 = _f ? (sat_tic_sight_frt * 10u / 224u) / _f : 0u;
                /* 40 VISIBLE COLUMNS is the hard budget ([[debug-overlay-line-width]]), so `x` had to
                   buy its place: `hc` is retired here.  It existed to judge the temporal sight cache
                   while sight was 13-21 ms/frame; with the REJECT matrix back, `s` reads 1,2-4,9 ms
                   and `sc<rejects>/<walks>` already tells that story on its own. */
                /* `T` RETIRED 2026-08-16 to seat `v` inside the 40 columns.  It answered its
                   question: across six hardware captures `T - th` was 2-4 ms every time, i.e. the
                   game tic IS the thinkers and nothing else. (RP_TicBegin/End still run and
                   sat_tic_total_frt is still reset below -- print it again in one line if needed.)
                   `v` = VBLANK FIELDS SEEN PER FRAME. It must read 60/fps: 10,9 at 5,5 fps. Lower
                   means the vblank handler is missing fields, which made the millisecond clock run
                   slow and silently DROPPED game tics -- read it beside `x`, they are one story. */
                static unsigned int vbl_prev = 0;
                unsigned int vbl_now  = sat_vbl();
                unsigned int vb10     = _f ? ((vbl_now - vbl_prev) * 10u) / _f : 0u;
                vbl_prev = vbl_now;
                unsigned int xt10 = _f ? (sat_tic_runs  * 10u) / _f : 0u;  /* tics RUN per frame  */
                unsigned int av10 = _f ? (sat_tic_avail * 10u) / _f : 0u;  /* tics TryRunTics elected */
                unsigned int bl10 = _f ? (sat_tic_built * 10u) / _f : 0u;  /* tics NetUpdate WANTED     */
                /* 🔴 ROW 23 (was blank) 2026-08-17 -- THE INSIDE OF `th`.  Hardware reads `th` at
                   24-38 ms on 178-200 ms frames: a fifth of the frame that is not rendering, and
                   the only number describing it was `th` itself.  `s` already carves out
                   P_CheckSight's BSP walk; this carves the rest so a console session comes home
                   with the whole picture:
                     n  = thinkers RUN this frame -- separates "many" from "expensive"
                     mo = P_MobjThinker, the actor world
                     mv = P_CheckPosition/P_TryMove inside it -- the BLOCKMAP walk, Doom's classic
                          hot spot, a SUBSET of mo (so mo - mv is state/animation)
                   `th - mo` is what is left: the sector thinkers (doors, platforms, lights).
                   🔴 ROUND 2, after the 64 s console run: at 63 s the frame read `T165` against
                   `R141` -- the game tic OVERTOOK the whole renderer -- with mo146,4 mv75,1 s2,4, so
                   ~69 ms of `mo` had no name.  Two more terms, both SUBSETS of mo:
                     pt = P_PathTraverse, the traversal behind hitscan shots.  Disjoint from `mv`:
                          shots never call P_CheckPosition.
                   🔴 ROUND 3 -- `sp` RETIRED (it ANSWERED: `sp0,5/3`..`sp0,8/4`, three spawns a
                   frame under a millisecond, hypothesis dead) and its columns spent SPLITTING `mv`,
                   which the same run showed to be the biggest single item in the whole frame --
                   `R66` against `T148` with `mv66,0`, i.e. the blockmap check alone costing as much
                   as the entire renderer.  Both new fields are SUBSETS of `mv`:
                     sb = `R_PointInSubsector` -- the BSP DESCENT, paid once per call before any
                          collision work.  Never timed until now.
                     bt = the THINGS blockmap loop, which walks every corpse still linked in.
                   **`mv - sb - bt` is the LINES loop**, by subtraction: three terms for two probes.
                   All tenths dropped: these terms run 3-70 ms and the FRT tick is already 4,5 us,
                   so the decimal was precision the measurement does not have -- and six fields at
                   40 columns leave no room for it ([[debug-overlay-line-width]]). */
                {
                    unsigned int mo_ms = _f ? (sat_thk_mobj_frt  * 10u/224u)/_f/10u : 0u;
                    unsigned int ph_ms = _f ? (sat_thk_phys_frt  * 10u/224u)/_f/10u : 0u;
                    unsigned int sm_ms = _f ? (sat_thk_state_frt * 10u/224u)/_f/10u : 0u;
                    unsigned int mv_ms = _f ? (sat_thk_move_frt  * 10u/224u)/_f/10u : 0u;
                    unsigned int sb_ms = _f ? (sat_thk_sub_frt   * 10u/224u)/_f/10u : 0u;
                    unsigned int bt_ms = _f ? (sat_thk_blk_frt   * 10u/224u)/_f/10u : 0u;
                    unsigned int thn   = _f ? sat_thk_n / _f : 0u;
                    snprintf(ovbuf, sizeof ovbuf,
                             "THK n%u mo%u ph%u sm%u mv%u sb%u bt%u        ",
                             thn > 9999u ? 9999u : thn,
                             mo_ms > 999u ? 999u : mo_ms, ph_ms > 999u ? 999u : ph_ms,
                             sm_ms > 999u ? 999u : sm_ms, mv_ms > 999u ? 999u : mv_ms,
                             sb_ms > 999u ? 999u : sb_ms, bt_ms > 999u ? 999u : bt_ms);
                    ovbuf[40] = ' ';
                    if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 23, ovbuf);
                    sat_thk_mobj_frt = sat_thk_move_frt = sat_thk_n = 0;
                    sat_thk_phys_frt = sat_thk_state_frt = 0;
                    sat_thk_sub_frt  = sat_thk_blk_frt   = 0;
                }
                /* 🔴 2026-08-18 -- `sp` EXISTS BECAUSE I MISREAD THIS ROW, TWICE.  I compared `b`
                   (what NetUpdate WANTED) against `v` and reported the clock healthy at 94-95 %.
                   `x` is what actually RAN, and an independent cross-check settles it: TNT MAP11
                   has 422 THINGS and row 23 reads n~1921 P_MobjThinker calls a frame -- 1921/422 =
                   4,55 tics, which matches `x` and NOT `b`.  The game runs at a THIRD to two thirds
                   of real time and the fps counter cannot see it.
                   `sp` = 100 * x / (0,583 * v) = the game speed in percent, so nobody has to do that
                   division on a photograph again.  `a` RETIRED: the code's own decision rule needed
                   `a` only to separate "elected but not run" from "never built", and a == x on every
                   capture ever taken answers it.  `mk` RETIRED with the grate feature.
                   `sc` = the SECTOR thinkers, so `th - mo - sc` is the bare list walk + Z_Free. */
                unsigned int sc_ms = _f ? (sat_thk_sect_frt * 10u/224u)/_f/10u : 0u;
                unsigned int owed10 = (vb10 * 583u) / 1000u;          /* 35 Hz / 60 Hz = 0,583 */
                unsigned int sp     = owed10 ? (xt10 * 100u) / owed10 : 0u;
                snprintf(ovbuf, sizeof ovbuf, "TIC th%u s%u x%u.%u v%u.%u sp%u%% sc%u        ",
                         th10 / 10u, sg10 / 10u,
                         xt10 / 10u, xt10 % 10u, vb10 / 10u, vb10 % 10u,
                         sp > 999u ? 999u : sp, sc_ms > 999u ? 999u : sc_ms);
                ovbuf[40] = ' ';
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 24, ovbuf);
                sat_tic_think_frt = 0; sat_tic_sight_frt = 0;
                sat_tic_runs = 0; sat_tic_avail = 0; sat_tic_built = 0; sat_thing_masked_cut = 0;
                sat_thk_sect_frt = 0;
                sightcounts[0] = sightcounts[1] = 0; sat_sight_cachehit = 0;
            }
            r_composite_pf = 0;   /* own the reset HERE: row 18's R_CompositeWindowReset runs before
                                     this print, so clearing it there zeroed `pf` unseen */
            sat_wall_lod_hits = 0;   /* same rule: the field's own row clears it, right after printing */
            r_visplane_pool_ovf_pk = 0;
            r_visplane_peak = 0;   /* zero the core running-maxes -> next window re-accumulates its own peak */
            r_drawseg_peak  = 0;
            r_solidseg_peak = 0;   /* r_solidseg_ovf stays latched (sticky) so a single overflow event stays visible */
            /* (Rows FLR / PAR / FBK REMOVED 2026-08-06 -- they were three cut rows that still
               FORMATTED a full string into a static buffer and then threw it away ((void)rXX),
               so each cost .bss + .rodata + the snprintf marshalling in the HWRAM budget that
               feeds the TLSF pool.  Dead code costs pool 1:1 ([[boot-loop-can-be-tlsf-pool-
               starvation]]); this paid for the resident flat pool's ~1.1 KB.
                 FLR "Vs/Vp/d%/n"     = VDP1-floor candidate quad sizer -- the whole SAT_FLOOR_TEX
                                        offload path was DELETED 2026-08-02 [[ftex-m5-cut-pool-tripled]].
                 PAR "ss/Q/Qp/q4%"    = "every floor+ceiling as a VDP1 quad" (PowerSlave model)
                                        sizer for the same deleted path.
                 FBK "c/m/s/K/W"      = wall CPU-fallback sizer; the Phase-1 wall clamp shipped and
                                        its live state moved to row 13 `C+`/`C-`.
               The producers (sat_floor_vq_*, sat_prof_ss_*, fb_*) are untouched, so re-adding a
               row is one snprintf if any of these questions ever comes back.) */
            /* row 12: CD-streaming HEALTH + the texture cache that is supposed to tame it (CD
               mode only -- cart mode reads from RAM).  Reads left-to-right as the thrash story:
                 p  = persistent-handle A/B (pad L+A; default 0 = LoadBytes per read)
                 k  = mean ms per GFS chunk command (R0.2 k-meter, the calibration number)
                 t  = cumulative seconds spent INSIDE CD commands -- THE thrash magnitude; on a
                      well-fed level it creeps, on a thrashing one it races (this is what inflates
                      the derived render `R` on row 1 into hundreds of ms).
               Then the streaming texture cache (core/r_cache.c) meant to amortize that traffic by
               keeping recently-visible composites resident:
                 TX = live pool KB -- 0 = NO pool carved => cacheless => every miss re-reads CD.
                 e  = cumulative evictions this level -- high + climbing = the pool is too small
                      for the working set (churn); the smoking gun of the fragmentation-thrash.
                 lf = largest free block (KB) at the carve attempt -- small lf => zone too tight
                      to carve a useful pool (root cause, not a bug).
               Ymir now models CD latency (~36-41 ms/cmd) so k/t are meaningful here, not HW-only. */
            if (sat_wad_base == nullptr)   /* CD-streaming mode */
            {
                extern int sat_cd_persistent;
                extern unsigned int w_cd_ms10;   /* core w_wad.c -- also the load budget's clock */
                /* `L<s>s/<n>` = the LAST detected level load: seconds inside CD commands, and how
                   many commands.  This is the number that answers "why is loading slow" -- `t`
                   cannot, it is cumulative over boot + every load + the in-play thrash.
                   Dropped to make room: `p` (persistent handle, settled DEFAULT-OFF), `e` and `lf`
                   (composite-pool eviction/carve -- both constant 0 in 1p, where the pool is
                   deliberately never carved). */
                /* The companion `S` field (P_SetupLevel's share) is GONE, having answered in one
                   capture: **S73 against L270s/4704** -- P_SetupLevel reads 73 lumps, so the BOOT
                   owns the load, not the level.  Watch `L` fall as the boot readers are fixed. */
                /* `k` (mean ms per command) DROPPED: it is derivable from t and row-0 `ld`, and its
                   cumulative form misleads -- it reads ~57 ms only because the boot's thousands of
                   sequential reads dominate it.  ⚠ I misread it as `k6.6` off two photos ('5' as
                   's'); it has been ~57 ms all along.  For the in-play slope use Δt/Δld. */
                /* `L` (the level-load gap detector) is PARKED 2026-08-07 to pay for SAT_ZONE_RA:
                   it answered (L270s/4704 -> the boot owned the load -> -Repack + the sprite-header
                   sweep), and the zone walls are now the live question.  Restore both lines
                   together with lvl_load_ms10 / lvl_load_n when the boot cost is back on trial. */
                /* `px` = garde-PATCH hits (core r_data.c): wall columns served from the
                   placeholder because no zone run could hold the whole patch.  **px > 0 means
                   this build would have HALTED at `Zmalloc fail 35104` before 2026-08-07** --
                   it is the crash, converted into a flat wall and a counter.  Room for it here
                   because the CD row gave up `k`, `L`, `S`, `p`, `e` and `lf` as they settled. */
                /* `ob` = composite offsets caught OUTSIDE texturecompositesize (core r_data.c,
                   2026-08-08).  **It must stay 0.** Non-zero is the owner's "wrong texture for one
                   frame": R_GenerateLookup's two early returns leave every column pre-seeded to
                   "composite, offset 0" while the composite is sized ZERO, so the column read ran
                   off the end into the neighbouring zone block -- another texture's composite,
                   which is why the wrong texture was a REAL one and not noise. */
                /* `gy` = VDP1 flat quads forced to the NEUTRAL GREY index -- the owner's grey
                   walls, whose trigger is a wall-dense view exhausting the wtex slots. */
                /* `st` = LEAD-FILL spans whose source had been PURGED by drain time (core
                   r_segs.c, 2026-08-09).  It belongs on this row because it is a garde of exactly
                   the same kind as px/ob: each one used to be a WRONG TEXTURE -- the slave drew a
                   stale `dc_source` that the master's R_DrawPlanes had already freed and another
                   texture had reused.  Now the span draws flat for one frame and says so here.
                   Cumulative like its neighbours; it must TEND TO 0 in a calm scene, and a steady
                   climb means the zone is purging under the render, not that the fix is failing. */
                extern int r_patch_ovf, r_composite_oob, sat_lead_stale;
                snprintf(ovbuf, sizeof ovbuf, "CD t%us px%d ob%d gy%d st%d ",
                         w_cd_ms10 / 10000, r_patch_ovf, r_composite_oob, vdp1_wall_nocol,
                         (sat_lead_stale > 9999 ? 9999 : sat_lead_stale));
                (void)sat_cd_persistent;
                if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 12, ovbuf);
            }
            /* row 18: memory-latency calibration (one-shot cold 32 KB read per bank, FRT
               ticks).  rL = LWRAM/HWRAM ratio -- >1.0 means LWRAM (cmd buf + visplanes) is
               the slow bank, = the memory-bound penalty + the L2-relocate upside, measured
               on THIS hardware (Ymir will read ~1.0 -- it does not model the bank gap). */
            unsigned int rL10 = mem_hw_ticks ? (mem_lw_ticks * 10u / mem_hw_ticks) : 0;
            /* (MEM row DELETED 2026-08-06 -- its Print was commented out ('overlay lean'),
               so it formatted lw/hw/rL into a dead buffer every second.  The 9th such row.) */
            /* (CLS sky-vs-floor classifier row DELETED 2026-08-06 -- it sized "free the HW-sky bank
               for a textured VDP2 floor", a question closed with the ftex path on 2026-08-02.
               sat_sky_px / sat_floor_px are still tracked; re-add one snprintf if it comes back.) */
        }
#if VDP2_RBG0_TEST
        {
            /* (RAMCTL readback row DELETED 2026-08-06 -- a one-shot bring-up probe; the bank split
               has been settled since [[sgl-rotation-anchors-bank-a1]].  ramctl_before/after are
               still captured, so re-adding the sprintf is a one-liner.) */
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
            extern int sat_drp_preload_kb;              /* R5.1: this map's level-load preload */
            /* (DRP streaming-status row DELETED 2026-08-06 -- cut as "not perf/composition" but
               still running TWO full snprintf branches every frame.  All six sat_drp_* counters
               survive.) */
        }
#endif
        /* fps-only mode: SRL::Debug tiles persist, so a row we stop writing would GHOST.  Blank
           rows 1-22 (every per-frame row: 1-8 the core B/P/M/REC/PK/SLV block, 11 LIM, 12 V1 probe,
           13 LOS, 15 SPR, 17 SPL, 18 SLV, 19 DEP, 20 BP, 22 GRD -- all gated on mode==0 for both
           writers, so nothing re-fills them here) so only row 0 (fps + MST) remains -> the
           mode0<->mode1 fps delta is clean.  Mode 2 hides the whole NBG3 layer (nbg3_show=0), so no
           blanking is needed there.
           🔴 2026-08-10: the bound was 19, which GHOSTED row 20 (core/r_parallel.c's BP line) in
           every fps-only run and would have ghosted the new row 22.  Raised to 22; row 21 is inside
           the range in advance, for the LOD governor row. */
        if (sat_dbg_overlay_mode == 1) {
            static const char bl[] = "                                        ";
            for (int rr = 1; rr <= 22; ++rr) SRL::Debug::Print(0, rr, (char *)bl);
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
/* SATURN M7 ROTATION DECROCHAGE (2026-07-31).  The RPT->VRAM copy, split out of DG_DrawFrame so it
   can run AFTER the framebuffer blit instead of before it.
   WHY: M7 added a THIRD display party.  Before it there were two (VDP1 walls + NBG1 software) and
   the old anticipation (sat_plane_border, r_main.c) covered their offset.  Now the DOMINANT FLOOR
   lives on RBG0, and its view angle reaches the hardware through this table.  The copy used to sit
   right after rbg0_set_transform(), i.e. ~250 lines and one full software blit BEFORE the NBG1
   picture that must line up with it -- and an M7 frame is 25-40 ms, so at least one vblank falls in
   between.  The hardware floor therefore adopted the new angle up to a FIELD earlier than the
   ceilings and walls drawn for that same angle: the wall/floor junction slides while turning.
   Deferring the copy puts the table and the framebuffer on the same side of the blit.

   🔴 BUT MODE 1 IS STILL ONE FIELD LATE, and the proof is in this file: the RAM->VRAM DMA of the
   RPT is normally done by SGL's **_BlankIn ISR** (see the RBG0_RPT_TRANSFER note ~400) -- i.e.
   INSIDE the vblank -- because that is when VDP2 reads the rotation parameter table.  VDP2 latches
   the whole constant parameter set (Xst..KAst) once per frame during vblank; nothing we write to
   the table afterwards affects the field already being displayed.  Mode 1 copies AFTER the blit,
   and the blit starts ON the vblank edge (sat_field_fence) and runs ~5.5 ms -- so the copy lands
   several ms INTO the field whose parameters VDP2 already took.  The hardware floor therefore
   adopts the new view one field AFTER the software picture drawn for it.  Every frame, by
   construction.  A frame is 2-3 fields, so 1 field in 2-3 shows old floor against new everything
   else: a still capture catches it roughly every other frame, under a perfectly steady input.
   That is the owner's *"entre le sol vdp2 et un autre sol cpu"*, and it is a RETARD, not geometry.

   Mode 2 (default) copies immediately BEFORE the fence, i.e. during the field that PRECEDES the
   blit: the write completes, then the vblank latches it, then the blit paints the matching software
   picture in that same field.  Both go live together for real.
   ⚠ Mode 2 assumes the blit lands on the NEXT vblank -- true under Fl1.  Under Fl2 (the wall-age
   lock, a dead instrument) the blit waits one extra field, so mode 2 would put the floor one field
   EARLY there.  Fl1 is the ship setting.
   Pad R+Left (1p) cycles 0/1/2; row 13 `Rp<n>`.  0 = the original pre-2026-07-31 timing.
   (`rbg0_rpt_late` itself is DECLARED next to sat_field_lock so the row-13 overlay, which is
   emitted earlier in this file, can read it.  This is its documentation.) */
static int rbg0_rpt_pending = 0;   /* set when the frame owes a deferred copy           */

/* (vdp1_flip_late REMOVED 2026-08-02 -- it was permanently 0 and the pool needed the bytes.  The
   lesson it recorded, so nobody re-adds it: VDP1 needs the OPPOSITE treatment from RBG0.  RBG0's
   table is a register-side commit -- written late, live at once, so deferring it to the blit aligned
   it (measured, `ovbuf`).  VDP1's list must be PLOTTED for a field before the hardware shows it, while
   the NBG1 blit writes displayed VRAM and appears immediately: VDP1 is structurally BEHIND and needs
   LEAD, not delay.  That is why the original design kicked the walls right after the BSP walk, and
   why the present now happens at the kick -- see vdp1_wpn_kick.) */

/* (the M7 rotation-decrochage probe statics live up with the other overlay counters -- see LAG) */

#if RBG0_RPT_TRANSFER == 2
/* Reproduce _BlankIn's RPT DMA (no slSynch).  Source = SGL's RAM RPT buffer through the UNCACHED
   0x26 alias, so slScrMatSet's cached stores are seen.  RA = the FULL XST..KY block (0x54) written
   by slScrMatSet, stopping BEFORE KAST (the coefficient-table address is set once by slKtableRA);
   RB (unused until dual-param) rides +0x68 so it never strays into B1's map when the RPT is in A0. */
static void rbg0_rpt_to_vram(void)
{
    memcpy(rbg0_rpt_vram,                          (const void *)0x260FFE1C, 0x54);
    memcpy((void *)((char *)rbg0_rpt_vram + 0x68), (const void *)0x260FFE84, 0x30);
}
#endif

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
    if (rbg0_kind == RBG0_KIND_BITMAP) {
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
    } else {
    /* CELL path (RPA slot): 8x8 grid of 8x8 cells = one 64x64 flat at RBG0_CEL_VRAM (A1).  The
       RPB slot (2nd flat) is uploaded by rbg0_upload_flat_rb() into the same bank at a fixed
       offset -- see Stage 2.  The SAME D4 orientation + texel offset the bitmap path uses
       (rbg0_tex_orient / _xoff / _yoff, pad Y + L+d-pad) is applied per-texel here too, so the
       cell floor matches the bitmap floor's tuned world-alignment (without it the cell floor sat
       at identity orientation = "of-travers" vs the walls). */
    unsigned char *cel = (unsigned char *)RBG0_CEL_VRAM;
#if RBG0_CELL_4BPP
    /* 16-COLOUR (4bpp) cells -- the snow fix.  Quantize the flat to <=16 BASE indices, remap every
       texel to a 4-bit slot, and put the 16 SHADED colours in a CRAM window (shade lives in the
       palette so a light change rewrites 16 CRAM entries, not the cells).  Stack arrays only (a static
       .bss here starves the TLSF pool = boot loop -- see the bitmap path's row[] note). */
    int hist[256];
    for (int i = 0; i < 256; ++i) hist[i] = 0;
    for (int i = 0; i < 64 * 64; ++i) hist[flat[i]]++;
    unsigned char pal16[16]; int npal = 0;                     /* up to 16 most-frequent distinct bases */
    for (int j = 0; j < 16; ++j) {
        int best = -1, bc = 0;
        for (int i = 0; i < 256; ++i) if (hist[i] > bc) { bc = hist[i]; best = i; }
        if (best < 0) break;
        pal16[npal++] = (unsigned char)best; hist[best] = 0;
    }
    if (!npal) { pal16[0] = 0; npal = 1; }
    unsigned char remap[256];                                  /* base index -> nearest slot (RGB dist) */
    for (int i = 0; i < 256; ++i) {
        int bj = 0, bd = 1 << 30;
        for (int j = 0; j < npal; ++j) {
            int dr = (int)colors[i].r - colors[pal16[j]].r;
            int dg = (int)colors[i].g - colors[pal16[j]].g;
            int db = (int)colors[i].b - colors[pal16[j]].b;
            int d = dr*dr + dg*dg + db*db;
            if (d < bd) { bd = d; bj = j; }
        }
        remap[i] = (unsigned char)bj;
    }
    for (int j = 0; j < 16; ++j) {                             /* 16 SHADED colours -> CRAM window */
        int s = cm_q[(j < npal) ? pal16[j] : pal16[0]];        /* cm_q folds qlevel+contrast */
        CRAM_CEL16[j] = (unsigned short)(0x8000 | ((colors[s].b >> 3) << 10)
                                                | ((colors[s].g >> 3) << 5)
                                                |  (colors[s].r >> 3));
    }
    for (int cy = 0; cy < 8; ++cy)                             /* pack 4bpp cells (32B each), ROW-MAJOR */
        for (int cx = 0; cx < 8; ++cx) {
            unsigned char *c = cel + (cy * 8 + cx) * 32;
            for (int ry = 0; ry < 8; ++ry)
                for (int rx = 0; rx < 8; ++rx) {
                    int px = cx * 8 + rx, py = cy * 8 + ry;
                    int u = (px + rbg0_tex_xoff) & 63, v = (py + rbg0_tex_yoff) & 63, fx, fy;
                    switch (rbg0_tex_orient & 7) {            /* D4 orientation (parity with bitmap) */
                        default:
                        case 0: fx = u;      fy = v;      break;  case 1: fx = v;      fy = 63 - u; break;
                        case 2: fx = 63 - u; fy = 63 - v; break;  case 3: fx = 63 - v; fy = u;      break;
                        case 4: fx = 63 - u; fy = v;      break;  case 5: fx = u;      fy = 63 - v; break;
                        case 6: fx = v;      fy = u;      break;  case 7: fx = 63 - v; fy = 63 - u; break;
                    }
                    int idx4 = remap[flat[fy * 64 + fx]];     /* 0..npal-1 (<16) */
                    int b = ry * 4 + (rx >> 1);               /* row-major; high nibble = even (left) col */
                    if ((rx & 1) == 0) c[b] = (unsigned char)((c[b] & 0x0F) | (idx4 << 4));
                    else               c[b] = (unsigned char)((c[b] & 0xF0) |  idx4);
                }
        }
#else
    /* 256-COLOUR cells (RBG0_CELL_4BPP 0): kept as a compile-time fallback -- correct but SNOWS with the
       8bpp framebuffer (3 reads/dot); the shipped path is 4bpp above. */
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx)
        {
            unsigned char *c = cel + (cy * 8 + cx) * 64;
            for (int ry = 0; ry < 8; ++ry)
                for (int rx = 0; rx < 8; ++rx)
                {
                    /* Destination byte = ROW-MAJOR (ry*8+rx): VDP2 256-color cells use the SAME raster
                       dot order as the (working) 256-color bitmap floor bmp[y*512+x] -> byte b paints
                       screen dot (row=b/8, col=b%8), so byte (ry*8+rx) carries the pixel at (col=rx,
                       row=ry).  CN_12BIT has no per-cell flip (sl_def.h), so any wrong order transposes
                       EVERY cell uniformly = seams at every boundary (looks like "some cells rotated"). */
                    int px = cx * 8 + rx, py = cy * 8 + ry;          /* flat texel this cell dot maps to */
                    int u  = (px + rbg0_tex_xoff) & 63;             /* texel offset (parity with bitmap) */
                    int v  = (py + rbg0_tex_yoff) & 63;
                    int fx, fy;                                     /* D4 orientation (parity with bitmap) */
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
                    c[ry * 8 + rx] = cm_q[flat[fy * 64 + fx]];   /* baked quantized shade */
                }
        }
#endif
    }
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
    /* ⚠ Rows ABOVE hz stay at entry 0x0000, which is NOT "no insert" -- it inserts CRAM index 0 =
       BLACK.  That is harmless ONLY because the insert is routed to RBG0, which has no pixels up
       there.  Any future attempt to route it to NBG1 as well must first give those rows a real ramp
       or the whole upper view blends to black (learned the hard way, 2026-08-16). */
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

/* SATURN 2026-08-16 -- the FAR-DEGRADATION LADDER (veil / relaxed punch / adaptive view distance)
   lived here for one session and is gone.  Rung 1 was rejected on sight; rungs 2 and 3 were killed
   by their own counters (`pe0` on six captures of eight, `fc0` on the three heaviest).  The
   post-mortems are at the removal sites in core/r_plane.c and core/r_bsp.c. */

static void rbg0_proto_init(void)
{
    if (rbg0_kind == RBG0_KIND_BITMAP) {
    /* SlaveDriver-inspired BITMAP RBG0 (PLAX.C initPlax): bitmap (char) 512x256 in A1, the
       coefficient/rotation table in A0, OVER_0 (repeat), perspective via the coefficient
       table.  NO pattern-name map.  Same banks as the BOOTING cell path (A1 char / A0 K).
       NBG3 debug is fully off (B1 holds only the RPT now). */
    if (rbg0_rpt_moved) { slRparaInitSet(RBG0_RPT_B1); rbg0_rpt_moved = 0; }  /* restore RPT to B1 after a cell->bitmap switch (boot path never enters here) */
    rbg0_rpt_vram = RBG0_RPT_B1;
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
    } else {
    /* CELL RBG0 (revived) -- the SEGA S_8_9_2 recipe: char (cells) in A1, pattern-name map in
       B1, coefficient/K-table in A0.  3 rotation banks vs the bitmap's 2, so B1 is spent on the
       map (no NBG3/cell-sky in B1 here).  The old cell path SNOWED because its commit never
       parked B1 as a rotation bank; the mature commit below (RDBS=0x8D + A0/A1/B1 parked +
       block-flush + slCashPurge) fixes that class.  Stage 1 = single-param (RA); slPlaneRB /
       sl1MapRB / slKtableRB (RPB) are added by rbg0_cell_init_rb() when dual-param is armed. */
    /* 0) MOVE the RPT out of B1 into A0 (joins the K-table) so B1 carries ONLY the pattern-name map.
       RPT + per-dot map read sharing B1 starved the rotation = whole-plane snow (HW 2026-07-07).  SEGA
       S_8_9_2 keeps RPT+K together and the map in its own bank; this reproduces that separation. */
    slRparaInitSet(RBG0_RPT_A0); rbg0_rpt_moved = 1;
    rbg0_rpt_vram = RBG0_RPT_A0;
    /* 1) cells filled per-flat by rbg0_upload_flat(); zero them for the pre-first-flat frame.
       16c cell = 32B, 256c cell = 64B -> 64 cells * cell size. */
    memset((void *)RBG0_CEL_VRAM, 0, 64 * (RBG0_CELL_4BPP ? 32 : 64));
    /* 2) pattern-name table (1-WORD) tiling the flat's 8x8 cell grid, palette_number 1 (0x1000), map
       in B1.  char# unit is 0x20 bytes: 256c cell (0x40) = 2 units -> idx*2; 16c cell (0x20) = 1 -> idx. */
    {
        unsigned short *map = (unsigned short *)RBG0_MAP_VRAM;
        for (int my = 0; my < 64; ++my)
            for (int mx = 0; mx < 64; ++mx)
            {
                int cellidx = (my & 7) * 8 + (mx & 7);
                int charno  = RBG0_CELL_4BPP ? cellidx : (cellidx * 2);
                map[my * 64 + mx] = (unsigned short)(charno | 0x1000);
            }
    }
    /* 3) RBG0 cell config + per-line coefficient table (Mode-7 perspective). */
    slOverRA(0);
    slCharRbg0(RBG0_CELL_4BPP ? COL_TYPE_16 : COL_TYPE_256, CHAR_SIZE_1x1);
    slPageRbg0(RBG0_CEL_VRAM, 0, PNB_1WORD | CN_12BIT);
    slPlaneRA(PL_SIZE_1x1);
    sl1MapRA(RBG0_MAP_VRAM);
    slMakeKtable(RBG0_KTAB_VRAM);
    /* R+Down A/B: rbg0_cell_koff drops the coefficient (VRAM) read -> 2 rotation reads (char+map) vs 3. */
    slKtableRA(RBG0_KTAB_VRAM, K_FIX | K_LINE | K_2WORD | (rbg0_cell_koff ? K_OFF : K_ON));
    slRparaMode(K_CHANGE);
    slBMPaletteRbg0(1);
    }

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
    /* RDBS low byte = (B1)<<6 | (B0)<<4 | (A1)<<2 | (A0).  1=coeff, 2=pattern-name, 3=char/bitmap.
       BITMAP: A1=char(3), A0=coeff(1), B1=0 -> 0x0D.  CELL K_ON: + B1=pattern-name(2), A0=coeff -> 0x8D.
       CELL K_OFF (the CRKTE-bandwidth test): A0=NONE(0), A1=char(3), B1=pattern-name(2) -> 0x8C.  This is
       exactly what CRKTE gives (coeff out of VRAM -> 2 rotation banks char+map, A0 free).  If THIS still
       snows, CRKTE cannot help and the cell floor is definitively dead with the framebuffer. */
    uint16_t rdbs = (rbg0_kind == RBG0_KIND_BITMAP) ? 0x000Du
                  : (rbg0_cell_koff ? 0x008Cu : 0x008Du);
    uint16_t v = (uint16_t)((ramctl_before & 0xFC00u) | 0x0300u | rdbs);
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
    /* HAND-PARK the rotation banks at 0xEEEE (rotation reads them via RDBS, NOT the cycle table) -- the
       mechanism the shipping bitmap floor is proven to work with (slScrAutoDisp returns NG for BOTH kinds,
       so it is not the oracle; this park is).  A0=coeff, A1=char are rotation banks in BOTH kinds. */
    VDP2_CYCA0L = 0xEEEE; VDP2_CYCA0U = 0xEEEE;
    VDP2_CYCA1L = 0xEEEE; VDP2_CYCA1U = 0xEEEE;
    if (rbg0_kind == RBG0_KIND_CELL) {
        /* CELL: B1 = pattern-name MAP = the 3rd rotation bank -> park it (sky/NBG3 forced off -> map alone).
           B0 = NBG1 framebuffer: PIN its two 8bpp reads by hand.  Cell uses slScrAutoDisp(RBG0ON) ALONE
           (NBG1 kept OUT of the mask so SGL doesn't reset the RBG0 map to its default B0 bank) -> SGL never
           schedules NBG1, so we author B0 ourselves (else HUD/weapon/other floors go black). */
        VDP2_CYCB0L = 0x55EE; VDP2_CYCB0U = 0xEEEE;   /* NBG1 framebuffer (two 8bpp char reads) */
        VDP2_CYCB1L = 0xEEEE; VDP2_CYCB1U = 0xEEEE;   /* B1 = rotation pattern-name map (park)  */
    } else {
        /* BITMAP: B1 is free (RPT only) -> host NBG3 / cell-sky if present, else scrub the stale read. */
#if RBG0_NBG3 || VDP2_CELL_SKY
        /* NBG3 and/or the cell sky live in B1: leave CYCB1 EXACTLY as slScrAutoDisp's allocator authored
           it (NBG0 sky = 1 PN + 2 char, NBG3 = 1 PN + 1 char).  Do NOT scrub it -- a hand-pinned/scrubbed
           value would starve the sky's 2nd 8bpp char read = snow on HW (memory rbg0-hw-sky-feasible). */
#else
        VDP2_CYCB1L = 0xFEEE; VDP2_CYCB1U = 0xEEEE;   /* NBG3 off + no cell sky: scrub the stale NBG3 read */
#endif
    }
    volatile uint8_t *const shadow = (volatile uint8_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E);
    volatile uint8_t *const chip   = (volatile uint8_t *)0x25F80000;
    for (int off = 0x0E; off <= 0xFE; off += 2)
        *(volatile uint16_t *)(chip + off) = *(volatile uint16_t *)(shadow + off);
    for (int b = 0; b < 4; ++b) {                                          /* snapshot CYC for readout */
        volatile uint16_t *s = (volatile uint16_t *)(shadow + 0x10 + b * 4);
        cyc_before[b] = ((uint32_t)s[0] << 16) | (uint32_t)s[1];
    }
}

/* Runtime SWITCH of the RBG0 floor KIND (bitmap <-> cell/dual) -- the "separate mode, don't replace"
   requirement: both paths live in the binary, selected per-map (or by the R+Up test chord).  Re-runs
   the SGL RBG0 config + the commit.  BITMAP: hand-commit (RDBS + park A0/A1 + block-flush, no slSynch --
   slBitMapRbg0 is deficient).  CELL: let slScrAutoDisp ASSIGN the 3-read cell cycle into the shadow
   (SGL knows how; returns NG if unschedulable), then block-flush that computed cycle to the chip (the
   vblank ISR never pushes CYC/RAMCTL) -- the no-slSynch equivalent of Jo/SEGA's slScrAutoDisp+slSynch.
   Forces a flat re-upload because the A1 layout differs (bitmap 512-wide rows vs 8x8 cell grid).  Called
   from DG_DrawFrame at a safe point (top of the floor block) when rbg0_kind_want != rbg0_kind. */
static void rbg0_reinit(void)
{
    rbg0_kind = rbg0_kind_want;
    rbg0_proto_init();        /* re-config SGL RBG0 for the new kind (bitmap vs cell) + VRAM + transform */
    /* SAME MECHANISM for BOTH kinds -- the HAND-PARK that the shipping bitmap floor is proven to work
       with: RDBS dedicates the rotation banks, the cycle table parks them at 0xEEEE, block-flush, NO
       slSynch.  (slScrAutoDisp returns NG for BOTH kinds -- HW-confirmed ad=0 for the working bitmap
       floor too -- so it is NOT the oracle; the hand-park is what actually works.)  The only cell/bitmap
       differences: RDBS (0x8D vs 0x0D, in commit_ramctl) and parking B1 too (cell's map, in commit_cyc).
       Cell's B1 sky/NBG3 confound is now removed (both forced off in cell mode) -> B1 = map alone. */
    /* HW-localized (R+Left): cell renders CLEAN without NBG1 -> the COMBINED slScrAutoDisp(NBG1ON|RBG0ON)
       NG's and corrupts the rotation, but slScrAutoDisp(RBG0ON) alone is clean.  So for CELL, configure
       RBG0 ALONE first (clean rotation cycle), then add NBG1 in a SEPARATE call (SRL note: "allocate RBG0
       before NBG0-3").  Bitmap keeps the combined call (it works). */
    if (rbg0_kind == RBG0_KIND_CELL) {
        /* CELL: slScrAutoDisp(RBG0ON) ALONE gives a clean, textured floor (HW-proven via R+Left).  Do
           NOT call slScrAutoDisp(NBG1ON) even separately -- it re-runs SGL's allocator which RESETS the
           RBG0 map to SGL's DEFAULT bank (B0, our framebuffer!) -> RBG0 reads the framebuffer as its map
           -> BLACK floor.  Instead ADD NBG1's display bit by poking the SGL BGON shadow directly (bit 1);
           NBG1's B0 framebuffer cycle is pinned in commit_cyc.  The block-flush + vblank ISR push BGON. */
        rbg0_autodisp_ret = slScrAutoDisp((uint32_t)RBG0ON);
        if (!rbg0_cell_nofb) {
            /* Display NBG1 by editing the BGON display bits directly (bits 0-5), NOT via slScrAutoDisp:
               keep NBG1(1)+RBG0(4), clear sky NBG0(0)+NBG3(3); the transparent-enable bits (8-15, incl.
               NBG1 colour-0 transparent so the floor shows through) are preserved. */
            volatile uint16_t *bgon = (volatile uint16_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E + 0x20);
            *bgon = (uint16_t)((*bgon & ~(uint16_t)(NBG0ON | NBG3ON)) | (uint16_t)(NBG1ON | RBG0ON));
        }
    } else {
        rbg0_autodisp_ret = slScrAutoDisp((uint32_t)(NBG1ON | RBG0ON));
    }
    rbg0_commit_ramctl();   /* RDBS 0x0D (bitmap) / 0x8D (cell) */
    rbg0_commit_cyc();      /* hand-park rotation banks (A0/A1[/B1 cell]) + block-flush shadow -> chip */
    slCashPurge();            /* SGL wrote cells/map/K via CACHED addrs -> flush so HW reads fresh VRAM  */
    rbg0_tex_dirty = 1;       /* force rbg0_upload_flat to rebuild the A1 texture in the new layout      */
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

/* (A floor-window MARGIN -- W1 opened 4/8/16 rows above the sky/floor boundary, on the theory that
   the boundary's two halves commit at different instants: the sky map is VRAM, read during display,
   while the window is REGISTERS, latched at vblank.  SETTLED-NEGATIVE 2026-08-02, owner: "ça ne
   change rien au problème, peu importe le mode" at every margin.  That result is worth keeping: it
   is what says THE WINDOW IS NOT THE LIMITER.  Removed rather than parked -- the probe that replaced
   it, rbg0_sky_off, subsumes it by opening the window all the way to row 0.) */
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

#endif




/* ------------------------------------------------------------------ */
/* SAROO boot read-integrity probe (2026-07-23).  First SAROO field test
   died in R_InitTextures ("Missing patch in texture BRNSMALL") = a WAD
   lump-name lookup failed, i.e. the directory or the PNAMES content came
   back wrong from the ODE's CDC emulation (boot log showed the MUS path,
   no CD involved before that).  Read header + directory + PNAMES TWICE
   each and CRC both passes: c1 != c2 on a photo PROVES unstable reads
   with no expected value needed; stable-but-crashing points at a
   deterministic misread (or the cart path) instead.  ~50 KB of extra
   boot reads through the same LoadBytes path the game uses; console-log
   output only; the 3 s STARTING countdown right after keeps the lines
   photographable.  Retire once SAROO boots clean. */

/* Scratch lives in LWRAM, NOT HWRAM .bss: the first WPROBE build carried two
   2 KB static buffers + ~2.7 KB code and sank the HWRAM TLSF pool 8.4 -> 1.7 KB
   -> tlsf_add_pool rejected it at boot = black screen on HW (the exact
   boot-loop-starvation failure the pre-flight rule exists for; build.ps1 now
   gates on the map).  LWRAM is dead at DG_Init time (Doom zone + RP ring are
   set up much later in D_DoomMain), so the top of it is free boot scratch. */
#define WPROBE_BUF    ((unsigned char *)0x002F0000)  /* one-sector CD bounce */
#define WPROBE_BIG    ((unsigned char *)0x002C0000)  /* assembled range, <=128 KB */
#define WPROBE_BUFSZ  2048

/* Verdicts latched for the death screen (wprobe_fatal_rows, called by
   DG_Fatal): rows 2-3 of ANY fatal photo then carry the CD-path CRCs (WP)
   and the cart-copy directory CRC boot vs death (CP) -- the 2026-07-24 field
   photos (missing patch BRNSMALL then DOORBLU, different per boot) never
   showed the probe lines because they scroll off before the fatal.
   Known-good stripped-shareware values: dir 34db0cd8, pnames a41a9ba2. */
static uint32_t wprobe_dir_c1, wprobe_dir_c2, wprobe_pn_c1, wprobe_pn_c2;
static int      wprobe_state = -1;   /* -1 never ran, 0 ok, 1 bad */
static uint32_t cprobe_dir_boot = 0; /* cart-copy dir CRC at boot (cached window) */
static int      cprobe_ran = 0;

/* !! LoadBytes' first parameter is a SECTOR offset, not a byte offset
   (srl_cd.hpp: "sectorOffset ... Number of sectors to skip" -> GFS_Load) --
   same convention as the proven cart path (sat_cart_load_region's
   `size_t sector`).  The first WPROBE build passed BYTE offsets: the
   directory read asked GFS for sector 4,155,420 (~8.5 GB), failed, and left
   the CD block wedged so even the game's own "DOOM1.WAD" lookups died
   ("not found on CDB", Ymir 2026-07-24, no-boot).  This reader does the
   byte->sector math itself: whole-sector LoadBytes into WPROBE_BUF with the
   game's 8-retry pattern, sliced into dst. */
static int wadprobe_read(SRL::Cd::File *f, int32_t off, int32_t len,
                         int32_t fsize, unsigned char *dst)
{
    if (off < 0 || len <= 0 || off > fsize - len)
        return 0;
    int32_t end = off + len;
    for (int32_t sec = off / WPROBE_BUFSZ; sec * WPROBE_BUFSZ < end; sec++)
    {
        int32_t base = sec * WPROBE_BUFSZ;
        int32_t want = fsize - base;              /* last sector: partial */
        if (want > WPROBE_BUFSZ) want = WPROBE_BUFSZ;
        int32_t got = -1;
        for (int r = 0; r < 8 && got != want; r++)
            got = f->LoadBytes((size_t)sec, want, WPROBE_BUF);
        if (got != want)
            return 0;
        int32_t s = (off > base) ? off - base : 0;
        int32_t e = (end < base + want) ? end - base : want;
        memcpy(dst + (base + s) - off, WPROBE_BUF + s, (size_t)(e - s));
    }
    return 1;
}

static uint32_t wadprobe_crc32(const unsigned char *p, int32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int32_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void wadprobe_boot(void)
{
    SRL::Cd::File f("DOOM1.WAD");
    unsigned char *hdr = WPROBE_BUF;   /* aligned LWRAM; fields extracted before reuse */
    int bad = 0;
    wprobe_state = 1;                  /* pessimistic until the probe completes */
    if (f.LoadBytes(0, 12, hdr) != 12)
    {
        printf("WPROBE: header read fail\n");
        return;
    }
    int32_t numlumps = (int32_t)(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24));
    int32_t diroff   = (int32_t)(hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24));
    printf("WPROBE hdr %c%c%c%c n=%d dir@%d\n",
           hdr[0], hdr[1], hdr[2], hdr[3], (int)numlumps, (int)diroff);
    int32_t dirlen = numlumps * 16;
    int32_t fsize  = diroff + dirlen;   /* the dir sits at the END of a WAD */
    if (numlumps <= 0 || numlumps > 8000 || diroff <= 0 || dirlen > 0x20000)
        return;

    int ok1 = wadprobe_read(&f, diroff, dirlen, fsize, WPROBE_BIG);
    uint32_t d1 = ok1 ? wadprobe_crc32(WPROBE_BIG, dirlen) : 0;
    int ok2 = wadprobe_read(&f, diroff, dirlen, fsize, WPROBE_BIG);
    uint32_t d2 = ok2 ? wadprobe_crc32(WPROBE_BIG, dirlen) : 0;
    int rderr = !(ok1 && ok2);
    if (rderr || d1 != d2) bad = 1;
    printf("WPROBE dir c1=%08lx c2=%08lx %s\n",
           (unsigned long)d1, (unsigned long)d2,
           rderr ? "RDERR" : (d1 == d2 ? "stable" : "UNSTABLE"));
    wprobe_dir_c1 = d1;
    wprobe_dir_c2 = d2;

    /* PNAMES content: patch-name lookups (the op that failed on SAROO) read
       names out of this lump, so a misread here crashes identically to a
       misread directory.  Scan the ASSEMBLED directory (WPROBE_BIG still
       holds pass 2) -- also immune to records straddling sector boundaries,
       which the first chunked scan mishandled. */
    if (ok2)
    {
        int32_t pn_off = 0, pn_len = 0;
        for (int32_t r = 0; r + 16 <= dirlen; r += 16)
        {
            if (memcmp(WPROBE_BIG + r + 8, "PNAMES\0\0", 8) == 0)
            {
                pn_off = (int32_t)(WPROBE_BIG[r]     | (WPROBE_BIG[r + 1] << 8) |
                                   (WPROBE_BIG[r + 2] << 16) | (WPROBE_BIG[r + 3] << 24));
                pn_len = (int32_t)(WPROBE_BIG[r + 4] | (WPROBE_BIG[r + 5] << 8) |
                                   (WPROBE_BIG[r + 6] << 16) | (WPROBE_BIG[r + 7] << 24));
                break;
            }
        }
        if (pn_off > 0 && pn_len > 0 && pn_len < 0x20000)
        {
            int okp1 = wadprobe_read(&f, pn_off, pn_len, fsize, WPROBE_BIG);
            uint32_t p1 = okp1 ? wadprobe_crc32(WPROBE_BIG, pn_len) : 0;
            int okp2 = wadprobe_read(&f, pn_off, pn_len, fsize, WPROBE_BIG);
            uint32_t p2 = okp2 ? wadprobe_crc32(WPROBE_BIG, pn_len) : 0;
            int prderr = !(okp1 && okp2);
            if (prderr || p1 != p2) bad = 1;
            printf("WPROBE pnames c1=%08lx c2=%08lx %s\n",
                   (unsigned long)p1, (unsigned long)p2,
                   prderr ? "RDERR" : (p1 == p2 ? "stable" : "UNSTABLE"));
            wprobe_pn_c1 = p1;
            wprobe_pn_c2 = p2;
        }
        else
        {
            printf("WPROBE: PNAMES not found in dir!\n");
            bad = 1;
        }
    }

    wprobe_state = bad;

    if (bad)
    {
        /* Freeze the evidence on screen long enough for a photo. */
        SRL::Debug::Print(0, 2, "WPROBE UNSTABLE - PHOTO THIS");
        unsigned int t = vbl_count;
        while (vbl_count - t < 600) ;
        SRL::Debug::Print(0, 2, "                            ");
    }
}

/* CRC the lump directory of the cart-resident WAD copy through the CACHED
   window -- the exact path R_InitTextures reads in cart mode.  With
   [Mimas-Eng] exmem_4M active on SAROO, the whole WAD lives in the emulated
   cart and ALL lump reads exercise the same cached A-Bus path as Tethys's
   resource heap (the OTw suspect).  Compare with WPROBE's CD value /
   34db0cd8 known-good. */
static uint32_t cprobe_dir_cart(void)
{
    const unsigned char *w = CART_RAM_CACHED;
    int32_t numlumps = (int32_t)(w[4] | (w[5] << 8) | (w[6] << 16) | (w[7] << 24));
    int32_t diroff   = (int32_t)(w[8] | (w[9] << 8) | (w[10] << 16) | (w[11] << 24));
    if (numlumps <= 0 || numlumps > 8000 || diroff <= 0 ||
        (uint32_t)diroff + (uint32_t)numlumps * 16u > (uint32_t)CART_RAM_SIZE)
        return 0;
    return wadprobe_crc32(w + diroff, numlumps * 16);
}

/* Death-screen rows 2-3: WP = CD-path double-read CRCs ('=' both passes
   agreed, '!' split), CP = cart directory CRC at boot vs re-CRC'd NOW at
   death -- DRIFT here = the cart copy/read degraded during gameplay. */
static void wprobe_fatal_rows(void)
{
    if (wprobe_state >= 0)
    {
        SRL::Debug::Print(0, 2, "WP d%x%c p%x%c %s ",
                          (unsigned)wprobe_dir_c1,
                          (wprobe_dir_c1 == wprobe_dir_c2) ? '=' : '!',
                          (unsigned)wprobe_pn_c1,
                          (wprobe_pn_c1 == wprobe_pn_c2) ? '=' : '!',
                          wprobe_state ? "BAD" : "ok");
    }
    if (cprobe_ran)
    {
        uint32_t now = cprobe_dir_cart();
        SRL::Debug::Print(0, 3, "CP b%x n%x %s ",
                          (unsigned)cprobe_dir_boot, (unsigned)now,
                          (now == cprobe_dir_boot && now != 0) ? "ok" : "DRIFT");
    }
}

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

    SRL::Debug::Print(0, 1, "WAD PROBE...");
    wadprobe_boot();       /* SAROO read-integrity diag -- see above */

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
            /* CPROBE: CRC the cart copy's directory through the CACHED window
               right after the load -- a mismatch vs the WPROBE CD value means
               the copy/read is wrong before the game even starts; DG_Fatal
               re-runs it at death (CP row) to catch in-game drift. */
            cprobe_dir_boot = cprobe_dir_cart();
            cprobe_ran = 1;
            printf("CPROBE cart dir=%08lx (cd %08lx)\n",
                   (unsigned long)cprobe_dir_boot, (unsigned long)wprobe_dir_c2);
            static char ws[45];
            sprintf(ws, "WAD OK sz=%u", sat_wad_size);
            SRL::Debug::Print(0, 1, ws);
            unsigned int t = vbl_count; while (vbl_count - t < 120) ;
        }
    }
    /* SATURN sprite distance-LOD default (core sat_sprite_rotlod_dist -- ships 0 in
       the shared core so DoomJo is untouched): beyond ~750 map units a ~56-unit
       monster is <=~12 px tall, its facing unreadable, so draw the front lump and
       collapse distant crowds' rotation-lump variety (PU_CACHE/CD churn win, biggest
       in split where N views multiply it).  Applies in cart AND CD modes; always
       UNDER the per-map .DRP rotation ceiling (lump[0] exists at every level). */
    {
        extern int sat_sprite_rotlod_dist;
        sat_sprite_rotlod_dist = 768;
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
    /* Arm the level-load sky upload: P_SetupLevel calls this BEFORE P_LoadReject, so the sky wins
       the 35 KB contiguous run it needs and the REJECT matrix takes what is left. */
    sat_sky_precache_hook = &sat_sky_precache;
#endif
#if SKY_DEBUG_SHOW
    slPriorityNbg0(6); slPriorityNbg1(5);   /* sky ON TOP to verify Stage A */
#else
    /* LAYER INVERSION: software (NBG1) ON TOP with Doom's correct occlusion; the VDP1
       walls render BELOW NBG1, filling the index-0 (transparent) wall gaps NBG1 leaves
       where the software wall draw is skipped.  NBG3 debug = 7 (top).
       NBG1 game = 6  >  every sprite priority = 5  >  NBG0 sky = 4 (3 with RBG0).
       (Since 2026-08-12 the HIGH sprite registers are 6, not 7, so NBG3's 7 is strictly on top --
       see the SAT_SPR_HI_PRIO note below.) */
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
       register 0.  registers 1..7 = SPR_HI (above NBG1) -- anything that SETS a priority bit lands
       here.  So a command that ORs a priority bit into its CMDCOLR jumps above NBG1 while the
       walls/floors stay below.  This is the sprite-vs-world layer split.

       🔴 2026-08-12 -- SPR_HI 7 -> 6 SO THE DEBUG TEXT SITS ABOVE THE WEAPON.
       Owner: "zb est caché par l'arme vdp1 ... idéalement, la couche debug devrait au dessus de
       vdp1".  He is right, and the reason it was not is exact.  NBG3 (the SRL debug text) is at
       priority 7 and so was every sprite register 1..7 -- and on a TIE, VDP2 Table 11.1 of the
       KRONOS-CORRECTED manual (../saturn-refs/manuals/text/VDP2-ST-058-R2.txt, PDF p.244) gives the
       order Sprite > RBG0 > NBG0 > NBG1 > NBG2 > NBG3.  **NBG3 is dead last of every layer**, so at
       7-vs-7 the weapon wins every pixel.  Raising the text is impossible: 7 is the 3-bit maximum.
       So the sprites come down one instead.

       ⚠ THE TIE IS STRUCTURALLY UNAVOIDABLE, and this is where it now sits.  Constraints:
       wall commands (reg 0 = 5) must stay STRICTLY below NBG1, so NBG1 >= 6; the weapon must be
       above NBG1; NBG3 <= 7.  There is no assignment in 0..7 that makes all three strict.  With
       SPR_HI = 6 the surviving tie is sprite-vs-NBG1 at 6, and Table 11.1 resolves it in the
       SPRITE's favour -- i.e. the weapon still composites over the software framebuffer exactly as
       before.  Everything else is untouched: walls 5 < NBG1 6, RBG0 4 < 6.  The ONLY intended
       behavioural change is that the debug text now draws over sprites.
       ⚠ REVERT = set SPR_HI back to 7 (one token).  If a capture shows the weapon vanishing behind
       the game framebuffer, the tie rule is not holding as documented on this path: revert and pad
       the row-22 print rightward instead (the owner's own fallback: start `zb` at column 28, clear
       of the weapon's columns 14-27). */
#ifndef SAT_SPR_HI_PRIO
#define SAT_SPR_HI_PRIO 6
#endif
    slPrioritySpr0(5);
    slPrioritySpr1(SAT_SPR_HI_PRIO); slPrioritySpr2(SAT_SPR_HI_PRIO); slPrioritySpr3(SAT_SPR_HI_PRIO);
    slPrioritySpr4(SAT_SPR_HI_PRIO); slPrioritySpr5(SAT_SPR_HI_PRIO); slPrioritySpr6(SAT_SPR_HI_PRIO);
    slPrioritySpr7(SAT_SPR_HI_PRIO);
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
    /* MATURE commit for BOTH kinds (disasm + SlaveDriver): reserve the RDBS (0x0D bitmap / 0x8D cell)
       + park the rotation cycle slots BY HAND -- SGL never did (no rbank_set) -- then block-flush the
       shadow.  Do NOT slSynch: it would recompute the cycle pattern from the inconsistent shadow =
       the boot-loop.  The old cell path's slSynch/wrong-B1 commit was the snow; this is the fix. */
    rbg0_commit_ramctl();         /* RDBS = 0x0D (bitmap) / 0x8D (cell: B1=pattern-name) */
    rbg0_commit_cyc();            /* park rotation banks (A0/A1[/B1 cell]) + block-flush shadow -> chip */
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
    sat_apply_mode();   /* boot the render mode M (owns sat_wall_skip/things/sky/SQ); pad Z cycles it */

    /* LAYER INVERSION: the weapon + HUD now render in SOFTWARE (NBG1, on top) -- do NOT
       route them to VDP1.  VDP1 carries ONLY the walls, BELOW NBG1.  (The VDP1 weapon/
       HUD path is left in the file but unhooked.) */

#if VDP1_WALL_TEST
    /* Route one-sided (solid) walls to the VDP1 world renderer AND skip their
       software column draw -> see the VDP1 coverage + the perf it buys back. */
    sat_wall_hook = sat_wall_vdp1;
    /* SATURN L5: the near-wall edge splitter (core asks which columns VDP1 will actually accept). */
    sat_wall_edge_hook = sat_wall_edge_split;
    /* sat_wall_skip is owned by sat_apply_mode (M-gated: 1 in every mode but M0-software-walls). */
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
/* SATURN 2026-08-16 -- SKY UPLOAD RETRY BACKOFF.  Both uploaders below source their pixels from
   R_GetColumn, which returns the zero-init `r_column_stub` whenever the zone cannot find one
   contiguous run for the sky patch (a 256x128 TNT sky -- RSKY1/2/3 -- is 35080 B, and r_data.c's
   garde-PATCH documents runs as short as 32 KB on this map).  Every zero then becomes near-black,
   so a stubbed upload paints a UNIFORMLY BLACK SKY.  That was survivable; latching `sky_loaded_tex`
   afterwards was not -- it made one tight moment at level load permanent for the entire level.
   The uploaders now refuse to latch on a stubbed read and re-arm this counter instead; the sky
   heals by itself the moment a run opens up, exactly the contract every garde in r_data.c has.
   Backoff so a persistently tight zone does not pay 256 Z_LargestAllocatable scans per frame. */
static int sky_retry_wait = 0;
#define SKY_RETRY_FRAMES 8

static void sky_upload(void)
{
    unsigned char *vram = SKY_VRAM;
    unsigned char  nb   = (unsigned char)sat_near_black();
    int stubbed = 0;

    /* One cheap probe before committing to 256 column fetches (see SKY_RETRY_FRAMES above). */
    if (R_GetColumn(skytexture, 0) == (const unsigned char *)r_column_stub)
        { sky_retry_wait = SKY_RETRY_FRAMES; return; }

    for (int col = 0; col < 256; ++col)
    {
        const unsigned char *src = R_GetColumn(skytexture, col);  /* 128-tall */
        if (src == (const unsigned char *)r_column_stub) stubbed = 1;
        for (int y = 0; y < 128; ++y)
        {
            unsigned char p = src[y];
            if (!p) p = nb;     /* keep the sky OPAQUE: 0 is the transparent code */
            vram[y * SKY_VRAM_STRIDE + col]       = p;
            vram[y * SKY_VRAM_STRIDE + col + 256] = p;   /* 2nd tile */
        }
    }
    if (stubbed) { sky_retry_wait = SKY_RETRY_FRAMES; return; }   /* do NOT latch: retry, do not go black for good */
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
    int stubbed = 0;

    /* One cheap probe before committing to 512 column fetches (see SKY_RETRY_FRAMES above). */
    if (R_GetColumn(skytexture, 0) == (const unsigned char *)r_column_stub)
        { sky_retry_wait = SKY_RETRY_FRAMES; return; }

    /* Sample the sky's FULL width down to the 512-column layer.  Doom skies are 1024 wide (sh=1,
       every other column); a 512- or 256-wide sky gives sh=0 and R_GetColumn's own width mask
       repeats it, exactly as the renderer would.  NEAREST-NEIGHBOUR on purpose: these are PALETTE
       INDICES, and averaging index 5 with index 200 yields index 102 -- an unrelated colour. */
    int skyw = texturewidthmask[skytexture] + 1;
    int sh   = 0;
    while ((512 << sh) < skyw) sh++;

    for (int ccol = 0; ccol < 64; ++ccol)
        for (int rx = 0; rx < 8; ++rx)
        {
            const unsigned char *src = R_GetColumn(skytexture, (ccol * 8 + rx) << sh);  /* 128-tall */
            if (src == (const unsigned char *)r_column_stub) stubbed = 1;
            for (int crow = 0; crow < SKY_CELL_ROWS; ++crow)
                for (int ry = 0; ry < 8; ++ry)
                {
                    unsigned char p = src[crow * 8 + ry];
                    if (!p) p = nb;                /* keep the sky OPAQUE (0 = VDP2 transparent code) */
                    cells[(ccol * SKY_CELL_ROWS + crow) * 64 + ry * 8 + rx] = p;
                }
        }
    memset(cells + SKY_NB_CELL * 64, 0, 64);        /* TRANSPARENT filler (index 0): floor shows below the horizon */
    if (stubbed) { sky_retry_wait = SKY_RETRY_FRAMES; return; }   /* do NOT latch: retry, do not go black for good */
    sky_loaded_tex = skytexture;
}

/* Build the NBG0 sky PN map: sky cells ABOVE the horizon (sky_horizon_row), transparent filler at/
   below it.  Map (B1) is uncached -> writes land with no purge; cheap (4096 entries) so it can be
   rebuilt live when the pad nudges sky_horizon_row. */
/* The VRAM half, split out of sky_cell_build_map so it can be DEFERRED past the field fence -- see
   sky_mode.  `sky_mode == 2` also lifts the boundary one whole cell so the sky stops 8 px HIGHER
   than the floor's top row instead of meeting it exactly. */
static void sky_cell_write_map(void)
{
    unsigned short *map = (unsigned short *)SKY_MAP_VRAM;
    int thresh = sky_horizon_row >> 3;   /* cell-row boundary (8px cells) */
    if (sky_mode == 2) thresh--;
    if (thresh < 0) thresh = 0;
    /* CLAMP to what is actually stored: only SKY_CELL_ROWS rows exist in VRAM (the B1 budget note
       above), and the horizon is re-derived live by the auto-track -- an un-clamped thresh would
       index cells past the last row, i.e. into the map. */
    if (thresh > SKY_CELL_ROWS) thresh = SKY_CELL_ROWS;
    for (int my = 0; my < 64; ++my)
        for (int mx = 0; mx < 64; ++mx)
        {
            int cellidx = (my < thresh) ? ((mx & 63) * SKY_CELL_ROWS + my) : SKY_NB_CELL;  /* sky above the horizon; transparent filler at/below it */
            map[my * 64 + mx] = (unsigned short)((cellidx * 2) | 0x1000);       /* char#=idx*2, palette bank 1 */
        }
}

static void sky_cell_build_map(void)
{
    sky_cell_write_map();
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

/* SATURN 2026-08-16 -- LEVEL-LOAD SKY UPLOAD.  Installed into the core's `sat_sky_precache_hook`
   (p_setup.c), which fires inside P_SetupLevel right BEFORE P_LoadReject.  That is the emptiest
   the zone ever gets for a level -- geometry loaded, things/composites/flats not yet -- so the
   35080-byte one-run sky patch is served while the run still exists, instead of at the first
   displayed frame where it now loses to the REJECT matrix.  Hardware proved the per-frame retry
   alone cannot rescue it: `px` climbed 37->40 against `lg22k`->`lg20k`, i.e. the run never
   reappears mid-level.  The retry stays as the backstop for the case this one still misses. */
extern "C" void sat_sky_precache(void)
{
    if (skytexture <= 0 || skytexture == sky_loaded_tex)
        return;
    sky_retry_wait = 0;
#if VDP2_CELL_SKY
    sky_cell_upload();
#elif VDP2_HW_SKY
    sky_upload();
#endif
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
#define VDP1_EDSR  (*(volatile unsigned short *)0x25D00010)   /* status: bit1 CEF = draw done, bit0 BEF = prev-frame overran */
#define VDP1_LOPR  (*(volatile unsigned short *)0x25D00012)   /* SEGA UM: last op cmd addr (PREV frame) -- transfer-over meter */
#define VDP1_COPR  (*(volatile unsigned short *)0x25D00014)   /* SEGA UM: current op cmd addr (plot progress) */
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
/* SPLIT (the multi firing tear, was the parked "8-slot follow-up"): 2-4 views x (gun+flash) =
   up to 8 lumps/frame thrashed the shared 2-slot parity half -- a later view's bake OVERWROTE a
   texture the SAME frame's earlier quads reference = torn/wrong weapon on every fire.  Fix: the
   SAME 64KB re-addressed as 16 x 4KB HALF-RES slots = [parity(8)][view(2 each)].  The split
   weapon draws at 0.5 scale (pspritescale, viewwidth 160), so a half-res bake is 1:1 on screen =
   zero visual loss, ~1/4 the bake cost, and 4KB/frame fits (max frame SHTGD0 120x131 -> 64x66;
   the >4KB tail rows of such raise/lower frames are cropped -- bottom-anchored + view-clipped =
   invisible).  Doom has NUMPSPRITES==2, so 2 dedicated slots per view suffice: no cross-view
   eviction.  Within-frame overwrite (gun HIT + flash MISS landing on the gun's slot -- the
   one-frame fire tear) is prevented by the per-frame `used` guard below, same recipe as
   thing_cache. */
#define WPN_SPL_SLOTSZ 0x1000u            /* 4 KB half-res split slot */
#define WPN_SPL_SLOTS  16                 /* 2 parities x 4 views x 2 psprites, same 64KB region */
#else
#define WPN_TEX_BASE   0x25C45000u
#define WPN_TEX_SLOTSZ 0xB000u            /* 44 KB -> up to ~160x140 padded */
#define WPN_TEX_SLOTS  4
#define WPN_SPL_SLOTSZ WPN_TEX_SLOTSZ     /* legacy RGB path: no split re-addressing */
#define WPN_SPL_SLOTS  WPN_TEX_SLOTS
#endif
#define WPN_CACHE_N   (WPN_SPL_SLOTS > WPN_TEX_SLOTS ? WPN_SPL_SLOTS : WPN_TEX_SLOTS)
/* `used` = a quad of THIS frame's list references the slot (set on HIT and on bake, cleared each
   frame in sat_vdp1_wpn_begin): a same-frame MISS must never bake over a used slot, or the
   already-emitted quad plots the wrong texture (the "torn gun for one frame on fire" bug --
   gun HIT slot s, flash lump changes -> MISS with the rr cursor parked on s). */
static struct { int lump; const unsigned char *cmap; int padW; int H; unsigned char used; }
                    wpn_cache[WPN_CACHE_N];
/* TEAR-SAFE: partition the weapon slots by frame parity (like thing_cache) -- bake ONLY into the
   write-parity half so the DISPLAYED half is never re-baked mid-plot.  1p fires gun+flash = 2 lumps
   <= WPN_SLOTS_PER, so it fits; split uses the per-(parity,view) half-res slots above. */
#define WPN_SLOTS_PER (WPN_TEX_SLOTS / 2)
static int           wpn_cache_rr[2];      /* 1p: per-parity round-robin cursor */
static unsigned char wpn_rr_spl[2][4];     /* split: per-(parity,view) cursor within its 2 slots */
static int           wpn_cache_split = -1; /* addressing mode the cache holds (1p/split); a flip
                                              relocates+rescales every slot -> invalidate all */

/* (vd1_dr_live CUT 2026-08-10 with vdp1_vblank_dr -- see the mh_vbl note.) */
static int          vdp1_bank;     /* weapon bank VDP1 is currently displaying */
static int          vdp1_wbank;    /* bank being written this frame */
static int          vdp1_wnext;    /* next command slot in the write bank */
static int          vdp1_wactive;  /* 1 = R_DrawPlayerSprites ran this frame */
static int vdp1_hud_force_recopy = 0;   /* palette change (level load / flash) -> force a HUD recopy next capture */

/* HUD on VDP1 (docs/LOWRES_RENDER_STUDY.md Phase 2): the 1p status bar (framebuffer rows
   192-223, composed by ST_Drawer) is re-drawn as a VDP1 prio-7 NORMAL sprite so it is
   IMMUNE to the NBG1 x2 zoom of lowres mode (M7) -- crisp bar over the chunky 3D view --
   AND sits ON TOP of the weapon (its bob/recoil no longer pokes over the HUD).  8bpp: the
   bar is already palette-indexed in the framebuffer, so the texture is a RAW 10KB copy (no
   pal_rgb555 convert), displayed through CRAM bank 1 (= colors[], the SAME palette NBG1
   uses) -> pixel-identical to the software bar, just un-zoomed.  HUD_Y is parameterised for
   future per-viewport multiplayer HUDs. */
#define HUD_W        320
#define HUD_H        32
#define HUD_Y        192          /* Mimas 224-fb: 1p status bar owns rows 192..223 (ST_Drawer) */
#define VDP1_HUD_TEX 0x25C78000u  /* 8bpp 320*32 = 10KB @ things-pool end (0x25C78000); clear of the
                                     28KB things pool below, and everything above it is free VDP1
                                     VRAM since the ftex F-banks were cut 2026-08-02. */
#define HU_MSG_H          8       /* HU message: the 7px hu_font line at HU_MSGY=0 (+1px margin) */
#define VDP1_HUD_MSG_TEX0 0x25C7A800u /* two 8bpp 320*8 = 2.5KB message slots, DOUBLE-BUFFERED (tear- */
#define VDP1_HUD_MSG_TEX1 0x25C7B200u /* free): 0x25C7A800..0x25C7BC00, before the F-banks (0x25C7C000) */

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
/* VRAM UNION DISSOLVED 2026-08-02.  This pool 0x25C71000..0x25C78000 (28KB) used to ALIAS the ftex
   flat-cache tail 0x25C71000..0x25C7B800 (42KB) -- the 44KB tail could not hold both, so they were
   kept mutually exclusive by render mode and sat_apply_mode enforced an interlock.  The ftex floor
   deport is gone with M5, so THINGS owns the tail outright and the interlock is deleted.
   FREED VDP1 VRAM, currently unclaimed: the two F command banks 0x25C7C000..0x25C80000 (2 x 8KB =
   16KB, the top of VDP1 VRAM).  Natural claimants, in order: more THINGS_TEX_SLOTS (a same-type
   horde already shares slots, so extra slots buy DISTINCT textures), or wall-texture slots. */
#define THINGS_TEX_BASE   0x25C71000u
#define THINGS_TEX_SLOTS  4                /* slots PER parity == max distinct things offloaded/frame (VRAM cap) */
#define THINGS_TEX_SLOTSZ 0x0E00u          /* 3584 B -> fits any shareware monster frame @ 8bpp */
/* KEY = (lump, xlat, cmap) where `cmap` is NULL for every standard light level.  Until 2026-08-06
   the LIGHT was baked into the texels and cmap was always in the key -- so one monster walking
   toward you changed colormap every few units and RE-BAKED on each change, thrashing the 4 slots.
   Walls never had this: they bake the RAW index full-bright and pick a CRAM light bank at draw
   time.  Things now do the same (wall_light_colr).  Only NON-standard colormaps (invulnerability)
   still bake their map in and keep it in the key -- the 7 CRAM banks are built from colormap
   levels 0..31 and cannot express the inverted map. */
static struct { int lump; const unsigned char *cmap; const unsigned char *xlat;
                unsigned char used; unsigned int lru; }
                    thing_cache[2][THINGS_TEX_SLOTS];   /* [parity][slot]: key + per-frame used bit + LRU tick */
static unsigned int thing_lru_tick;        /* monotonic use counter -> evict the smallest (oldest) lru */
/* AIMD adaptive things-per-frame budget (sat_walls_kick): grow slowly on a finished plot, back off
   fast on an overrun.  THING_ADAPT_MAX must match core THING_EMIT_MAX (the scratch-array bound). */
#define THING_CAP_GROW  8                  /* clean frames before +1 (slow additive increase) */
#define THING_ADAPT_MAX 16                 /* outdoor ceiling == core THING_EMIT_MAX */
/* (thing_cap_clean / thing_emit_floor / thing_overrun_run CUT 2026-08-09 -- see the note at the
   top toggles: all three were write-only, and the row-15 `ef` they fed printed a constant 0.) */
/* (sat_things_n / sat_things_decl / thing_bake_n are defined earlier, before the overlay block) */
/* SPLIT-SCREEN QUEUE: split renders its views BEFORE the kick and the walls only flush AT the
   kick, so a per-view thing emitted straight into the command bank would land BEFORE the walls
   and be painted over (VDP1 = painter order).  Instead the per-view emissions (core calls the
   hook from R_RenderViewPass, per view, with that view's live vissprites/drawsegs/window):
     - BAKE immediately, into the parity the NEXT begin will write (vdp1_bank ^ 1 -- the pair
       the displayed list never references, so pre-kick VRAM writes stay tear-safe), and
     - QUEUE the two commands' fields here; vdp1_things_flush() emits them at the kick AFTER
       vdp1_walls_flush -> painter order walls -> things -> weapon(2), same as 1p.
   1p keeps the direct path (its emission already happens at the kick, after the walls). */
#define THING_ACC_MAX      (4 * THING_ADAPT_MAX)  /* 4 views x per-view AIMD ceiling */
#define THING_FLUSH_MARGIN 16                     /* bank tail kept free: 4 views' weapon(2) + HUD + end */
static struct { unsigned short texoff, csize, colr;  /* precomputed CMDSRCA / CMDSIZE / CMDCOLR */
                short x0, y0, x1, y1;             /* quad rect (screen, view offset baked in) */
                short cx0, cy0, cx1, cy1;         /* FUNC_UserClip visible box */
                unsigned char flip; } thing_acc[THING_ACC_MAX];
static int thing_acc_n;                    /* queued entries this split frame */
static int thing_acc_open;                 /* 1 = a split frame's per-view emissions are underway */
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
   THREE POOLS keyed by size since 2026-08-06 (was two).  Same VRAM region, same 368 KB, +37% slots.

   MEASURED, not guessed -- TEXTURE1 of the 7 reference WADs (wads_temoins: shareware, Ultimate,
   Doom II, Plutonia, TNT, Hell Revealed, Scythe) run through THIS file's exact sizing formula
   (padW = (W+7)&~7 over texturewidthmask+1, matelas H+2*WTEX_VPAD), 2877 textures:

     class size    textures it holds     the 2-class map wasted 27-40% of every slot it filled
       8192 B          402  (14%)        <- 8 KB is the WRONG cut: it holds a RAW 64x128 but not
       8448 B         1795  (62%)           its MATELAS, so the most common texture in Doom bakes
       8704 B         1799  (63%)           unpadded and falls back to a 16 KB slot anyway
      12288 B         1808  (63%)
      16384 B        ~2500  (87%)

   The cliff is exactly 64*(128+4) = 8448 = the matelas'd 64x128, and NOTHING above it pays: +4
   textures out of 2877 between 8448 and 8704, +13 up to 12288.  So: ONE new class, at 0x2100.
   Adding a 4 KB class on top of it buys +0.6 .. +2 slots -- measured, not worth a fourth pool.

   Slots at constant VRAM: 19 -> 26 (16 small + 6 narrow + 4 wide), and 12 KB falls out spare.
   Fallback chain small -> narrow -> wide (a texture may always take a BIGGER slot), so the worst
   case -- a scene that wants only 16 KB textures -- degrades to exactly the old behaviour, never
   worse.  Watch `bk` on overlay row 18: it is the thrash signal and the gate on this whole cut. */
#define WTEX_BASE      0x25C05000u
#define WTEX_SMALL_N   16
#define WTEX_SMALL_SZ  0x2100u                                      /* 8448 B -> 64x(128+4) @ 8bpp */
#define WTEX_NARROW_N  6    /* 15 -> 6: the 16 KB pool now only serves what does NOT fit 8448 B
                               (~24% of textures).  Watch `bk` (row 18) if a texture-varied level
                               re-bakes: the small pool falls back here, not the reverse. */
#define WTEX_NARROW_SZ 0x4000u                                      /* 16KB -> 128x128 @ 8bpp */
#if SAT_WPN_VDP1
#define WTEX_WIDE_N    4   /* SAT_WPN_VDP1: cede the last 2 wide slots (64KB) to the 4-slot 16KB VDP1
                              weapon cache (WPN_TEX_BASE=0x25C61000).  Watch the wtex_bakes 'bk'
                              counter: 4 wide-wall slots remain (was 6). */
#else
#define WTEX_WIDE_N    6
#endif
#define WTEX_WIDE_SZ   0x8000u                                      /* 32KB -> 256x128 @ 8bpp */
#define WTEX_SMALL_BASE  WTEX_BASE                                            /* 0x25C05000 */
#define WTEX_NARROW_BASE (WTEX_SMALL_BASE  + WTEX_SMALL_N  * WTEX_SMALL_SZ)   /* 0x25C26000 */
#define WTEX_WIDE_BASE   (WTEX_NARROW_BASE + WTEX_NARROW_N * WTEX_NARROW_SZ)  /* 0x25C3E000 */
#define WTEX_SLOTS       (WTEX_SMALL_N + WTEX_NARROW_N + WTEX_WIDE_N)         /* 26 (was 19) */
/* Pool ends 0x25C5E000; WPN_TEX_BASE is 0x25C61000 -> 0x3000 = 12 KB SPARE at the pool tail,
   contiguous and unclaimed.  >= one 8 KB VDP1 command bank if the bank widening wants it. */
#define WALL_CMD_CAP   (VDP1_BANK_CMDS - 8)   /* walls stop here -> room for end + margin */
/* RUNTIME wall cap = the view-count-scaled reservation (sat_walls_kick sets it before the flush).
   1p keeps WALL_CMD_CAP.  In a co-op split the walls saturate the bank and the OVERLAYS emitted
   after them (weapon copy-2 per view + the HUD band strip(s) + the HU message, all guarded at
   VDP1_CMD_GUARD) had only VDP1_BANK_CMDS-2 - WALL_CMD_CAP = 6 slots -> in 3/4p the 4 weapons ate
   them and the HUD dropped (the doubled-band artifact).  Reserving more (below) drops the FARTHEST
   walls instead (already the overflow tail) so the weapon + HUD always fit.  Read by the wall-emit
   guards; never above WALL_CMD_CAP. */
static int vdp1_wall_cap = WALL_CMD_CAP;

/* 8BPP PALETTE LIGHTING (replaces gouraud, which can't light a 256-colour BANK: VDP1 applies
   gouraud to the palette CODE before the CRAM lookup -> it shifts the index, not the RGB).
   Each texel = the RAW Doom palette index (1 byte, NEVER re-baked -- not for light, not for
   flash).  Per-wall light = a CRAM 256-colour BANK chosen by CMDCOLR (bank<<8): bank 1 = NBG1's
   live full-bright PLAYPAL, banks 2..7 = the PLAYPAL pre-shaded by 6 colormap levels.  So a wall
   texel idx -> CRAM[bank*256+idx] = the EXACT (multiplicative) colormap colour, matching the
   software floors/sprites; flash re-tints the banks in CRAM (see wtex_rebuild_banks). */
/* vpad = rows of VERTICAL PADDING baked above AND below the texture in this slot (the "matelas").
   Doom textures tile vertically, so the pad rows are copies of the opposite edge: row -1 IS row
   H-1.  They exist so a quad grown for the seam can grow its CHARACTER by the matching number of
   texels and keep the mapping EXACT -- see wall_emit.  0 when the padded bake would not fit the
   slot (a texture that exactly fills its 16KB/32KB slot); such a texture simply never grows. */
/* `locked` is a THREE-state cross-frame guard (SATURN 2026-08-09), not a boolean:
     0 = free to evict
     1 = referenced by the list being BUILT this frame  (evicting it corrupts the frame we are writing)
     2 = referenced by the list VDP1 is still DISPLAYING (evicting it corrupts the frame on screen)
   State 2 is the half of the 2026-07 corruption fix that was never posted.  The comment on
   VDP1_DBLBANK (top of file) names the hazard exactly -- "the VDP1 reads a bank we're already
   overwriting (AND A TEXTURE WE'RE RE-BAKING) the next frame" -- and it was closed for the command
   banks (double-banked) and for the things pool (thing_cache[parity][]) but NOT here: the wall
   texture pool is 364 KB of the 512 KB VDP1 VRAM (12 KB spare, dg_saturn:4106), so it cannot be
   duplicated.  In 1-cycle auto VDP1 re-plots the displayed list EVERY vblank with CMDSRCA pointing
   into these slots, so a slot re-baked while that list is live shows ANOTHER REAL TEXTURE -- the
   owner's "inversions de textures sous grosse charge".  Refusing the eviction degrades to a flat
   coloured quad for ONE frame instead, and self-heals (the slot drops to 0 next flush).
   Same invariant, same reasoning as the flat pool: core/r_flatcache.c:136-138. */
static struct { int texnum; unsigned int addr, cap; short padW, H, vpad;
                unsigned int lru; unsigned char locked; }
                wtex_cache[WTEX_SLOTS];
static int wtex_saw_stale = 0;     /* wtex_find_victim skipped a state-2 slot during this resolve
                                      (wtex_qrefuse itself is declared up at the row-18 counters) */
#define WTEX_VPAD 2   /* rows each side; also the largest sat_wall_grow that can stay exact */
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
    for (int i = 0; i < WTEX_SMALL_N; ++i)
    { wtex_cache[i].addr = WTEX_SMALL_BASE + (unsigned int)i * WTEX_SMALL_SZ;
      wtex_cache[i].cap = WTEX_SMALL_SZ; }
    for (int i = 0; i < WTEX_NARROW_N; ++i)
    { wtex_cache[WTEX_SMALL_N + i].addr = WTEX_NARROW_BASE + (unsigned int)i * WTEX_NARROW_SZ;
      wtex_cache[WTEX_SMALL_N + i].cap = WTEX_NARROW_SZ; }
    for (int i = 0; i < WTEX_WIDE_N; ++i)
    { wtex_cache[WTEX_SMALL_N + WTEX_NARROW_N + i].addr = WTEX_WIDE_BASE + (unsigned int)i * WTEX_WIDE_SZ;
      wtex_cache[WTEX_SMALL_N + WTEX_NARROW_N + i].cap = WTEX_WIDE_SZ; }
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

/* CMDCOLR (= CRAM 256-colour bank base, bank<<8) for a wall's colormap = its light level.  Every
   wall command -- textured band, flat, banded -- takes its CMDCOLR from here.
   (The `vdp1_wall_over` priority bit that briefly rode here, lifting every wall quad ABOVE NBG1 for
   the double-write modes, went with them on 2026-08-03.  Wall quads keep the priority-select bits
   CLEAR -> sprite register 0 = 5 = BELOW NBG1, which is the z-invariant the whole path rests on.) */
static inline unsigned short wall_light_colr(const unsigned char *cmap)
{
    int L = (int)((cmap - colormaps) >> 8);              /* colormap level 0..33 */
    if (L < 0) L = 0; else if (L > 33) L = 33;
    return (unsigned short)(((unsigned int)wlight_bank_lut[L] << 8)
                            );
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
#define WALL_ACC_MAX 128   /* RESTORED to 128 (2026-07-09): the 128->96 pool-reclaim tried earlier this date DROPPED
                              WALLS in split co-op (per-view budget 96/nv = 24/view in 4p too tight; the software
                              fallback did not fully cover dense split views on HW).  Pool pressure from the clear-slave/
                              nearSprites/AIMD-damp levers' .text is reclaimed via HEAP_SIZE 32->24KB (syscalls.c) instead
                              -- never rob the wall budget for the pool.  1p peaks ~57 << 128; split = 128/nv (32 in 4p). */
/* Fill px past which the remaining (FARTHEST -- BSP is near-first) walls fall back to CPU software
   instead of overloading the VDP1 plot before vblank.  Was a live pad L+Left/Right sweep; the HW
   sweep proved wall-offload a NET LOSS (MST 76->129 as the budget drops -- it rejects the cheapest
   walls; see memory wall-offload-vdp1-slave-dead 2026-07-07), so it is baked back to ~off (200k).
   FBK `s` counts the rejects. */
#define WALL_PX_BUDGET 200000
static int wall_px_acc;    /* fill claimed so far this frame (reset with wall_acc_n) */
/* vx/vxr = the view's framebuffer x-range [vx, vxr] this wall belongs to (split-screen: 0..159 for
   the left view, 160..319 for the right).  x1/x2 are stored ALREADY offset by viewwindowx, so the
   emit works in absolute framebuffer coords; vx/vxr drive the per-view user-clip window. */
static struct { short x1, yl1, yh1, x2, yl2, yh2, slot, v0, v1, vx, vxr, vyt, vyb; int texnum, u1, u2;
                unsigned char mode, special, view, pot; const unsigned char *cmap; } wall_acc[WALL_ACC_MAX];
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
        if (wall_px_acc + a > WALL_PX_BUDGET) return 1;   /* overflow guard -> CPU software fallback (far walls shed) */
        wall_px_acc += a;
    }
    int vx = viewwindowx, vy = viewwindowy;
    int i = wall_acc_n++;
    /* low-detail: x arrives as the HALVED column (0..viewwidth-1); the framebuffer is full width,
       so screen x = vx + (x<<detailshift).  detailshift==0 (1p / hi-detail) => byte-identical. */
    {
        wall_acc[i].x1 = (short)((x1 << detailshift) + vx); wall_acc[i].yl1 = (short)(yl1 + vy); wall_acc[i].yh1 = (short)(yh1 + vy);
        wall_acc[i].x2 = (short)((x2 << detailshift) + vx); wall_acc[i].yl2 = (short)(yl2 + vy); wall_acc[i].yh2 = (short)(yh2 + vy);
    }
    wall_acc[i].texnum = texnum; wall_acc[i].u1 = u1; wall_acc[i].u2 = u2;
    wall_acc[i].v0 = (short)v0; wall_acc[i].v1 = (short)v1; wall_acc[i].cmap = cmap;
    wall_acc[i].vx  = (short)vx;
    wall_acc[i].vxr = (short)(vx + (viewwidth << detailshift) - 1);
    wall_acc[i].vyt = (short)vy;                        /* this view's framebuffer y-band [vy, vy+h-1] -- */
    wall_acc[i].vyb = (short)(vy + viewheight - 1);     /* clips the VDP1 walls vertically (3/4p quadrants) */
    wall_acc[i].view = (unsigned char)sat_split_view;   /* 4-way budget bin (0..3) */
    wall_acc[i].special = (unsigned char)(sat_wall_textured ? 1 : 0);   /* force textured in pot2 */
    wall_acc[i].pot = (unsigned char)wall_potato_mode;  /* CAPTURE the VDP1 wall STYLE per view at
                                                           accumulate time (walls flush after the split
                                                           loop, when wall_potato_mode = the LAST view's;
                                                           per-view/rotating SQ needs this per-wall value) */
    return 0;                            /* queued for VDP1 */
}

/* best victim in [lo,hi): an empty slot, else the least-recently-used slot in state 0.
   States 1 AND 2 are both untouchable -- see the wtex_cache declaration.  A skipped state-2
   slot is recorded so the caller can tell "the pool is genuinely full this frame" (state 1)
   apart from "the pool is full only because the screen still needs it" (state 2, overlay `q`). */
static int wtex_find_victim(int lo, int hi)
{
    int victim = -1; unsigned int oldest = 0xFFFFFFFFu;
    for (int i = lo; i < hi; ++i)
    {
        if (wtex_cache[i].locked)
        {
            if (wtex_cache[i].locked == 2) wtex_saw_stale = 1;
            continue;
        }
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
    /* MATELAS sizing.  Prefer the padded bake, but NEVER let the padding push a texture out of the
       pool it would otherwise fit: a 128x128 fills a narrow slot exactly, and spending a scarce wide
       slot on it to gain 2 pad rows would trade cache stability (watch `bk`) for a 1px seam.  Such a
       texture bakes unpadded and never grows -- it keeps the exact mapping, which is the state the
       owner asked for in 66e590c. */
    int vp = (H >= WTEX_VPAD) ? WTEX_VPAD : 0;
    unsigned int raw = (unsigned int)(padW * H) * 1u;               /* 8bpp: 1 byte/texel */
    unsigned int pad = (unsigned int)(padW * (H + 2 * vp)) * 1u;
    unsigned int size;
    if      (vp && pad <= WTEX_SMALL_SZ)  size = pad;   /* 62% of all WAD textures land here */
    else if (raw <= WTEX_SMALL_SZ)      { size = raw; vp = 0; }
    else if (vp && pad <= WTEX_NARROW_SZ) size = pad;
    else if (raw <= WTEX_NARROW_SZ)     { size = raw; vp = 0; }
    else if (vp && pad <= WTEX_WIDE_SZ)   size = pad;
    else if (raw <= WTEX_WIDE_SZ)       { size = raw; vp = 0; }
    else return -1;                                     /* too big even for a wide slot */

    /* Prefer the tightest pool that fits, then fall UP the chain (small -> narrow -> wide): a
       texture may always take a bigger slot, never a smaller one.  A scene wanting only 16 KB
       textures therefore degrades to exactly the pre-2026-08-06 two-pool behaviour, never worse. */
    int victim;
    wtex_saw_stale = 0;                                 /* set by wtex_find_victim on a state-2 skip */
    if (size <= WTEX_SMALL_SZ)
    {
        victim = wtex_find_victim(0, WTEX_SMALL_N);
        if (victim < 0) victim = wtex_find_victim(WTEX_SMALL_N, WTEX_SMALL_N + WTEX_NARROW_N);
        if (victim < 0) victim = wtex_find_victim(WTEX_SMALL_N + WTEX_NARROW_N, WTEX_SLOTS);
    }
    else if (size <= WTEX_NARROW_SZ)
    {
        victim = wtex_find_victim(WTEX_SMALL_N, WTEX_SMALL_N + WTEX_NARROW_N);
        if (victim < 0) victim = wtex_find_victim(WTEX_SMALL_N + WTEX_NARROW_N, WTEX_SLOTS);
    }
    else                                                /* wide: only the wide pool fits */
        victim = wtex_find_victim(WTEX_SMALL_N + WTEX_NARROW_N, WTEX_SLOTS);
    if (victim < 0)                                     /* all fitting slots used -> flat in flush */
    {
        if (wtex_saw_stale) wtex_qrefuse++;             /* refused to overwrite what is ON SCREEN */
        return -1;
    }

    /* SATURN LOAD BUDGET, VDP1 HALF (2026-08-06) -- THE HOLE THE ROW-20 SPLIT EXPOSED.
       The bake loop below calls R_GetColumn per column, which in the CD-streaming build FAULTS THE
       WHOLE TEXTURE IN (~42 ms of synchronous disc, plus a composite build).  That made the VDP1
       bake a SECOND, INDEPENDENT consumer of the same textures as the software wall draw -- and it
       was NOT gated: `sat_wall_io_flat` (r_segs.c) only ever governed the SOFTWARE column path.  So
       the budget could flatten 143 software tiers in a window and change nothing, because the walls
       routed to VDP1 fetched their textures anyway.  And because this runs inside vdp1_walls_flush,
       inside sat_walls_kick, the cost lands on `P` -- which is why `P` read 124..281 ms on frames
       where R_DrawPlanes itself (`d` on row 20) measured ~0.0 ms.
       Refusing here returns -1, and the flush ALREADY degrades slot<0 to `wmode = 2` = a flat
       coloured quad -- the same well-tested path taken when every slot is busy.  The wall textures
       itself on a later frame as the budget refills.  A/B live on pad R+X (lb0 = off = the old
       always-bake behaviour); read `b` on row 20 for refusals and `t` for the resolve pass. */
    if (sat_tex_load_budget && !R_TextureIOFree(texnum))
    {
        if (R_LoadBudgetLeft())
        {
            /* Pay -- and PRIME the dominant colour first, while we are faulting the texture in
               anyway.  R_WallPotatoColor does the load itself, so the bake loop below then finds
               every column resident: ONE disc read, not two.  Without this the colour cache stays
               empty in normal play (the potato mode is off) and every LATER refusal of this same
               texture would fall back to the neutral index -- measured `nocol` = 100 % of flattened
               tiers when the software half shipped without priming.  No `spent++`: the budget is
               a CLOCK now (core r_segs.c), and this call's own disc read is already on it.
               LAZY since 2026-08-08 (sat_budget_refused): the walk is every other column of the
               whole texture through R_GetColumn, and nothing reads its product until the budget
               has actually refused something. */
            if (sat_budget_refused) R_WallPotatoColor(texnum);
        }
        else return -1;                                 /* flat quad this frame, textured later */
    }
    wtex_bakes++;                                       /* cache miss -> a real bake follows (the `k` cost) */

    /* bake the RAW palette index (1 byte/texel) full-bright; light is applied at draw time via
       the CMDCOLR CRAM bank.  No re-bake ever (light or flash) -> max cache stability.
       Write 16-bit packed (two adjacent columns per halfword: hi byte = even col, lo = odd, on the
       big-endian SH-2) -- VDP VRAM is 16-bit and the SGL/SRL references upload by DMA, never byte
       writes, so 8-bit stores to VDP1 VRAM are not relied on.  The byte layout is identical. */
    volatile unsigned short *t = (volatile unsigned short *)wtex_cache[victim].addr;
    int halfW = padW >> 1;                                    /* 16-bit words per texture row */
    if (vp && wtex_cache[victim].cap < (unsigned int)(padW * (H + 2 * vp))) vp = 0;  /* victim too small */
    for (int x = 0; x < W; x += 2)
    {
        const unsigned char *c0 = R_GetColumn(texnum, x);        /* even column (high byte) */
        const unsigned char *c1 = (x + 1 < W) ? R_GetColumn(texnum, x + 1) : c0;  /* odd (low) */
        int wx = x >> 1;
        for (int y = 0; y < H; ++y)
            t[(y + vp) * halfW + wx] = (unsigned short)(((unsigned int)c0[y] << 8) | c1[y]);
        /* the pad rows ARE the texture, wrapped: above row 0 comes row H-1, below row H-1 comes
           row 0.  So the halo is the real neighbouring texel, not a smear or a guessed colour. */
        for (int k = 0; k < vp; ++k)
        {
            int ty = H - vp + k;                                 /* top pad  <- last vp rows  */
            t[k * halfW + wx] = (unsigned short)(((unsigned int)c0[ty] << 8) | c1[ty]);
            t[(vp + H + k) * halfW + wx] =                       /* bottom pad <- first vp rows */
                (unsigned short)(((unsigned int)c0[k] << 8) | c1[k]);
        }
    }
    wtex_cache[victim].texnum = texnum;
    wtex_cache[victim].padW = (short)padW; wtex_cache[victim].H = (short)H;
    wtex_cache[victim].vpad = (short)vp;
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
static int wall_ext = 768; /* how far a tile's extrapolated extent may run past the view window
                              before the squish fallback.  The extrapolated tile + user-clip window
                              is the CORRECT path (exact texels); off-window pixels only cost idle
                              VDP1 iteration (writes suppressed), so the allowance is generous:
                              768 covers the worst overhang of a routed wall (<= SAT_WALL_CPU_MAG
                              = 3 px/texel x texw-1 = 765 for texw=256).  The fallback is now only
                              a VDP1 coordinate-range guard (with the +/-1000 absolute clamps in
                              the test below), NOT the edge-tile default -- the old 96 was what
                              squished ("ecrasement") wall textures against the screen edges.
                              NOTE: magnified walls DO reach here since SAT_WALL_SUBDIV (as
                              re-sampled sub-segs); hyper-magnified sub-segs that could only land
                              in the fallback are routed to software upstream (r_segs.c subdiv
                              guards).  HW tuning knob if edge-heavy scenes overrun the vblank:
                              768 -> 512 -> 384 (judge via row-11 DF split, live A/B only). */

/* Flat quad screen-y clamp: a flat fill has NO texture, so clamping its geometry is FREE (no v ->
   no swim) and bounds the VDP1 fill.  The clamp band is now THIS VIEW's [vyt, vyb] (read per-wall
   from wall_acc), not the full screen -- see wall_emit_flat (fixes the 3/4p vertical bleed).
   (Too-close TEXTURED walls are handled upstream by the core CPU fallback, not here.) */

static int wall_vbands(int wi)   /* number of vertical texture-height bands this wall needs */
{
    /* H from the TEXTURE, not from the cache slot: this is now called BEFORE the slot exists
       (lazy resolve, vdp1_walls_flush) -- and it is the same number either way, because
       wall_tex_resolve bakes wtex_cache[].H = textureheight>>16.  The old `: 128` guess is gone. */
    int H = textureheight[wall_acc[wi].texnum] >> 16;
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

/* 🔴 SATURN 2026-08-16 -- THE 1 px SEAM, owner-localised: *"les bandes verticales manquantes se
   produisent aux jonctions entre deux murs vdp1"*.  The software path cannot have this defect --
   it walks COLUMNS, so column n belongs to exactly one seg by construction.  A quad rasteriser
   has to be TOLD where the shared edge is, and it is not: wall A's right edge and wall B's left
   edge are two independent roundings of the same world point, so one column can fall inside
   neither.  Widening the RIGHT edge by one pixel makes the overlap explicit -- the neighbour
   repaints it with its own first column, so the only visible cost is one column of overdraw, and
   at the far end the spill lands where the wall itself would have been.
   This is the cheap patch, not the fix: the fix is to derive both edges from ONE number.  Kept as
   a live int so it can be A/B'd to zero if the overlap ever reads worse than the gap. */
static int sat_wall_xgrow = 1;



static void wall_emit_band(int x1, int x2, int yl1, int yh1, int yl2, int yh2,
                           int u1, int u2, int texw,
                           unsigned short charAddr, unsigned short charSize, unsigned short colr,
                           int vx, int vxr, int vyt, int vyb)
{
    int xspan = x2 - x1, du = u2 - u1;
    int xg = sat_wall_xgrow;          /* seam closer: widen the right edge, never the mapping */
    unsigned short cmd[16];

    if (xspan <= 0 || du == 0 || texw <= 0)            /* degenerate -> single quad */
    {
        if (vdp1_wnext >= vdp1_wall_cap) return;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0002; cmd[2] = 0x00E0;                 /* DISTORSP | COLOR_4 8bpp | SPD | ECD-off */
        cmd[3] = colr;                                    /* CMDCOLR = CRAM light-bank base */
        cmd[4] = charAddr; cmd[5] = charSize;
        cmd[6]  = (short)x1;        cmd[7]  = (short)yl1;
        cmd[8]  = (short)(x2 + xg); cmd[9]  = (short)yl2;
        cmd[10] = (short)(x2 + xg); cmd[11] = (short)yh2;
        cmd[12] = (short)x1;        cmd[13] = (short)yh1;
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
        if (vdp1_wnext >= vdp1_wall_cap) break;
        int xs = x1 + WDIV((long long)(ub        - u1) * xspan * sdu, adu, inv_du);  /* /du */
        int xe = x1 + WDIV((long long)(ub + texw - u1) * xspan * sdu, adu, inv_du);  /* /du */
        int lo = (xs < xe) ? xs : xe, hi = (xs < xe) ? xe : xs;
        if (hi < x1 || lo > x2) continue;                /* tile outside the visible range */

        int yls = yl1 + WDIV((long long)(yl2 - yl1) * (xs - x1), xspan, inv_xspan);
        int yhs = yh1 + WDIV((long long)(yh2 - yh1) * (xs - x1), xspan, inv_xspan);
        int yle = yl1 + WDIV((long long)(yl2 - yl1) * (xe - x1), xspan, inv_xspan);
        int yhe = yh1 + WDIV((long long)(yh2 - yh1) * (xe - x1), xspan, inv_xspan);

        /* correct path = extrapolated tile + window-clip: allowance is view-window-relative
           (split-safe), plus ABSOLUTE +/-1000 clamps on every corner coordinate so the larger
           extrapolation can never wrap the signed 13-bit VDP1 coordinate field (the y's are
           extrapolated AT xs/xe and can explode on sloped walls; sole known residual: a
           texw=256 right-edge tile with xe in (1000,1087] still falls back). */
        if (lo >= vx - wall_ext && hi <= vxr + wall_ext
            && lo > -1000 && hi < 1000
            && yls > -1000 && yls < 1000 && yhs > -1000 && yhs < 1000
            && yle > -1000 && yle < 1000 && yhe > -1000 && yhe < 1000)
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
                if (vdp1_wnext >= vdp1_wall_cap) break;
            }
            /* Phase-1 vertical [0,223] clamp REMOVED (owner's red diagnosis 2026-07-02).  With the
               SPAN routing reverted, a wall that projects off the TOP of the screen renders CORRECTLY
               as a FULL quad clipped by the VDP1 system clip: DISTORSP maps the texels across the whole
               (partly off-screen) projection, so the on-screen part shows the right texels -- no squish,
               no black.  The old clamp trimmed the texel with a SINGLE cut from one corner, which on a
               sloped-top tile (yls != yle) squished the texture AND left a black wedge under the clamped
               edge at the wall/screen-edge.  Full-quad + system-clip is the fix (overdraw = idle fill). */
            /* VERTICAL GEOMETRY vs TEXTURE ALIGNMENT (owner 2026-07-31: "les textures des murs
               software / vdp1 ne sont pas alignees").  This quad used to be grown 1px top AND
               bottom to close vertical seams -- but DISTORSP maps the WHOLE character corner to
               corner, so growing the vertices without being able to grow the character stretches
               `rows` texels over (span + 2) screen rows and shifts them up one row.  The resulting
               error is 0 at the band centre and 1 SCREEN PIXEL at each band edge -- which is
               rows/span TEXELS.  Near walls (span ~200, rows 128) barely notice it; a FAR wall
               (span ~20) misaligns by ~6 texels, and it repeats at EVERY texture-height band down
               the wall.  Software draws the same texture with the exact mapping, so a VDP1 wall and
               a software wall side by side visibly disagree -- worse the further away they are.
               The seam is closed WITHOUT that cost since 2026-08-02: the grow now happens in
               wall_emit, which extends the character by the matching number of texels (the MATELAS)
               and passes the already-grown y's down here.  This quad is therefore always
               texture-EXACT for whatever y range it is handed -- do not re-introduce a grow here. */
            memset(cmd, 0, sizeof cmd);
            cmd[0] = 0x0002; cmd[2] = 0x04E0;  /* DISTORSP | Window_In | COLOR_4 8bpp | SPD | ECD-off */
            cmd[3] = colr;                                 /* CMDCOLR = CRAM light-bank base */
            cmd[4] = charAddr; cmd[5] = charSize;
            cmd[6]  = (short)xs; cmd[7]  = (short)yls;     /* A col0  top */
            cmd[8]  = (short)(xe + xg); cmd[9]  = (short)yle;     /* B colW  top */
            cmd[10] = (short)(xe + xg); cmd[11] = (short)yhe;     /* C colW  bot */
            cmd[12] = (short)xs; cmd[13] = (short)yhs;     /* D col0  bot */
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
            cmd[8]  = (short)(cxe + xg); cmd[9]  = (short)cyle;
            cmd[10] = (short)(cxe + xg); cmd[11] = (short)chye;
            cmd[12] = (short)cxs; cmd[13] = (short)chys;
            vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
        }
    }
}

/* SATURN L5 -- NEAR-WALL EDGE SPLIT, platform half (core/r_segs.c sat_wall_try_edge is the caller).
   Replay wall_emit_band's tile loop above and report the widest run of tiles it will ACCEPT, so the
   core can keep only the REJECTED border columns on the CPU instead of dumping the whole wall there.
   This lives here, next to the emitter, on purpose: the acceptance test IS the emitter's guard, and
   duplicating it in core would let the two drift silently.
   `why` on failure: 1 = LATERAL (the tile lands outside the view window +/- wall_ext) -- borders can
   rescue that; 2 = MAGNITUDE (texw * magnification blows the 13-bit coordinate field) -- no
   screen-space split can ever help, only a narrower baked sub-texture. */
extern "C" int sat_wall_edge_split(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                                   int u1, int u2, int texw,
                                   int *oxL, int *oxR, int *ouL, int *ouR, int *why)
{
    extern int detailshift, viewwidth;
    int vx = viewwindowx, vy = viewwindowy;
    int X1 = (x1 << detailshift) + vx, X2 = (x2 << detailshift) + vx;
    int vxr = vx + (viewwidth << detailshift) - 1;
    int YL1 = yl1 + vy, YH1 = yh1 + vy, YL2 = yl2 + vy, YH2 = yh2 + vy;
    int xspan = X2 - X1, du = u2 - u1;
    int adu, sdu, inv_du, inv_xspan, ubase, uu1, uu2, umin, umax, ub;
    int bestA = 0, bestB = 0, curA = 0, have = 0, run = 0;
    int lateral = 0, magnitude = 0;
    int xa, xb, xL, xR, uAtL, uAtR;

    *why = 0;
    if (xspan <= 0 || du == 0 || texw <= 1) return 0;
    adu = (du < 0) ? -du : du; sdu = (du < 0) ? -1 : 1;
    inv_du = wrecip(adu); inv_xspan = wrecip(xspan);
    ubase = u1 & ~(texw - 1);
    uu1 = u1 - ubase; uu2 = u2 - ubase;
    umin = (uu1 < uu2) ? uu1 : uu2;
    umax = (uu1 < uu2) ? uu2 : uu1;

    for (ub = umin & ~(texw - 1); ub < umax; ub += texw)
    {
        int xs = X1 + WDIV((long long)(ub        - uu1) * xspan * sdu, adu, inv_du);
        int xe = X1 + WDIV((long long)(ub + texw - uu1) * xspan * sdu, adu, inv_du);
        int lo = (xs < xe) ? xs : xe, hi = (xs < xe) ? xe : xs;
        int yls, yhs, yle, yhe, inwin, ok;
        if (hi < X1 || lo > X2) continue;                  /* tile outside the visible range */
        yls = YL1 + WDIV((long long)(YL2 - YL1) * (xs - X1), xspan, inv_xspan);
        yhs = YH1 + WDIV((long long)(YH2 - YH1) * (xs - X1), xspan, inv_xspan);
        yle = YL1 + WDIV((long long)(YL2 - YL1) * (xe - X1), xspan, inv_xspan);
        yhe = YH1 + WDIV((long long)(YH2 - YH1) * (xe - X1), xspan, inv_xspan);
        inwin = (lo >= vx - wall_ext && hi <= vxr + wall_ext);
        ok = inwin && lo > -1000 && hi < 1000
             && yls > -1000 && yls < 1000 && yhs > -1000 && yhs < 1000
             && yle > -1000 && yle < 1000 && yhe > -1000 && yhe < 1000;
        if (ok)
        {
            if (!run) { curA = ub; run = 1; }
            if (!have || (ub + texw - curA) > (bestB - bestA))
                { bestA = curA; bestB = ub + texw; have = 1; }
        }
        else
        {
            run = 0;
            if (!inwin) lateral = 1; else magnitude = 1;
        }
    }
    if (!have) { *why = lateral ? 1 : (magnitude ? 2 : 0); return 0; }

    /* back to screen columns through the SAME linear map (interpolation between the anchors) */
    xa = X1 + WDIV((long long)(bestA - uu1) * xspan * sdu, adu, inv_du);
    xb = X1 + WDIV((long long)(bestB - uu1) * xspan * sdu, adu, inv_du);
    xL = (xa < xb) ? xa : xb;  xR = (xa < xb) ? xb : xa;
    uAtL = (xa < xb) ? bestA : bestB;
    uAtR = (xa < xb) ? bestB : bestA;
    if (xL < X1) xL = X1;
    if (xR > X2) xR = X2;
    *oxL = (xL - vx) >> detailshift;      /* back to the core's view-local column space */
    *oxR = (xR - vx) >> detailshift;
    *ouL = uAtL + ubase;
    *ouR = uAtR + ubase;
    return (*oxR > *oxL);
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

    int vp = wtex_cache[slot].vpad;                    /* matelas rows baked each side (0 = none) */
    if (H <= 0 || vspan <= 0)                          /* no valid v-range -> whole texture once */
    {
        int th = (H > 255) ? 255 : (H > 0 ? H : 1);
        unsigned short ca = (unsigned short)((base + (unsigned int)vp * (unsigned int)padW
                                              - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | th);
        wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
        return;
    }

    int inv_vspan = wrecip(vspan);   /* vspan > 0 here; reciprocal -> multiply per band */
    int v = v0, nb = 0;
    while (v < v1 && nb < MAXVBANDS)
    {
        if (vdp1_wnext >= vdp1_wall_cap) break;
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
        /* MATELAS (owner 2026-08-02, "wg2 ferme les trous existants a l'arret. Fais le matelas").
           Grow the quad by g SCREEN pixels top/bottom to cover the seam, and grow the CHARACTER by
           the MATCHING number of TEXELS so the mapping stays exact.  g screen px is g*rows/span
           texels; rounding that to dt costs at most half a texel, against the g*rows/span texels
           the naked quad grow costs (~6 at span 20 -- the misalignment fixed in 66e590c).
           The texels come from the pad rows, which are the vertically-wrapped neighbours, or from
           inside the texture when this band does not start/end on a texture seam.
           The QUAD ALWAYS grows by the pixels asked for.  Refusing to grow when dt did not fit was
           the first cut, and it silently switched the feature off for most walls (owner: "wg n'a
           plus l'air de grandir les murs en haut et en bas") -- a 128x128 texture fills a narrow
           slot EXACTLY so it bakes vpad=0, and any wall under ~43px tall wants dt>=3 from a 2-row
           pad.  So the character takes as many matching texels as EXIST and the shortfall
           (2*dt - dtt - dtb texels) degrades to the plain stretch: exact where the pad covers it,
           partial in between, and never worse than the pre-matelas grow. */
        int gt = 0, gb = 0, dtt = 0, dtb = 0;
        if (sat_wall_grow > 0)
        {
            int sA = yh1b - yl1b, sB = yh2b - yl2b;
            int span = sA > sB ? sA : sB;
            int dt = (span > 0) ? (sat_wall_grow * rows + (span >> 1)) / span : 0;
            int at = vp + vmod;                        /* texel rows available above this band */
            int ab = vp + (H - vmod - rows);           /* and below                            */
            if (at < 0) at = 0;
            if (ab < 0) ab = 0;
            dtt = (dt < at) ? dt : at;
            dtb = (dt < ab) ? dt : ab;
            while (rows + dtt + dtb > 255) { if (dtb) dtb--; else if (dtt) dtt--; else break; }
            gt = gb = sat_wall_grow;
        }
        unsigned int taddr = base + (unsigned int)(vp + vmod - dtt) * (unsigned int)padW * 1u;  /* 8bpp */
        unsigned short ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | (rows + dtt + dtb));
        wall_emit_band(x1, x2, yl1b - gt, yh1b + gb, yl2b - gt, yh2b + gb,
                       u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
        v = vb; ++nb;
    }
}

/* Fallback FLAT-colour quad for a wall drawn without its texture (low-detail Z mode / cache
   miss).  A palette polygon: CMDCOLR = light-bank<<8 | the texture's dominant index, so it is
   lit by the SAME CRAM bank as the textured walls and flashes via CRAM too.  1px generous. */
static void wall_emit_flat(int wi)
{
    if (vdp1_wnext >= vdp1_wall_cap) return;
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
    /* SATURN 2026-08-06 -- the owner caught this the moment the gate shipped: *"on a pas la couleur
       si on ne lit pas le cd"*.  Exactly right.  R_WallPotatoColor walks the texture through
       R_GetColumn, i.e. it performs THE VERY ~42 ms disc fault the budget just refused -- so gating
       the bake and then asking for the colour here would have paid the full disc cost AND drawn a
       degraded flat quad.  Under a live budget, PEEK it (returns -1, never loads) and fall back to
       the neutral index; the colour is PRIMED in wall_tex_resolve on the frame the budget decides to
       PAY, so it self-heals.  Same defect, same fix as the software wall path (r_segs.c
       sat_wall_flat_color).  Budget OFF -> unchanged, so lb0 stays a clean A/B reference. */
    /* SATURN 2026-08-08 -- THE GREY WALLS, and THIS is the site the owner was seeing: his trigger
       is *"quand l'écran est surchargé de murs"*, i.e. the wtex slots run out, `wall_tex_resolve`
       returns -1 and the wall degrades to a flat quad HERE.  Until today this branch only PEEKED
       whenever the budget was armed -- and the budget became armed BY DEFAULT on 2026-08-07 -- so
       every capacity-driven flat quad came out mid-grey instead of the wall's own colour.  The
       peek-only rule guarded against a ~42 ms disc fault; on this trigger the texture is RESIDENT
       and the walk is pure CPU, memoised for the level.  Same predicate as the software side
       (core r_segs.c sat_wall_flat_color): look if looking is free, grey only if it truly is not. */
    int pot = R_WallPotatoColorPeek(wall_acc[wi].texnum);
    if (pot < 0)
    {
        if (R_TextureIOFree(wall_acc[wi].texnum) || R_LoadBudgetLeft())
            pot = R_WallPotatoColor(wall_acc[wi].texnum);
        else
        {
            pot = SAT_WALL_FLAT_UNKNOWN;
            vdp1_wall_nocol++;   /* the VDP1 flat path had NO counter at all until now */
        }
    }
    unsigned short col  = (unsigned short)(colr | ((unsigned int)pot & 0xFF));
    /* DEBUG PAINT bit0 (core r_data.c sat_wall_paint, pad L+X): flat GREEN, and through CRAM BANK 1
       (= NBG1's own palette) rather than the wall's light bank, so it stays the SAME green in a dark
       room -- a distance-shaded green would be hard to tell from the red CPU walls. */
    if (sat_wall_paint & 1) col = (unsigned short)(0x0100u | 112u);   /* PLAYPAL: bright green */
    /* +1px x overlap each side (clamped to THIS view's x-range), mirroring the textured path's
       seam-fill window (wall_emit_band, ~4365) so adjacent flat quads OVERLAP instead of leaving a
       1px hairline gap -- visible when every wall is flat (iso mode 2; and far-wall fallbacks). */
    int fvx = wall_acc[wi].vx, fvxr = wall_acc[wi].vxr;
    int fx1 = (x1 > fvx)  ? x1 - 1 : fvx;
    int fx2 = (x2 < fvxr) ? x2 + 1 : fvxr;
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x0004;                                  /* FUNC_Polygon (flat) */
    cmd[2] = 0x00C0;                                  /* SPD (opaque) | ECD-off */
    cmd[3] = col;                                     /* CMDCOLR = bank<<8 | index -> CRAM */
    cmd[6]  = (short)fx1; cmd[7]  = (short)(yl1 - 1);
    cmd[8]  = (short)fx2; cmd[9]  = (short)(yl2 - 1);
    cmd[10] = (short)fx2; cmd[11] = (short)(yh2 + 1);
    cmd[12] = (short)fx1; cmd[13] = (short)(yh1 + 1);
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
    taddr = base + (unsigned int)(wtex_cache[slot].vpad + vmod) * (unsigned int)padW;  /* skip the matelas */
    ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
    cs = (unsigned short)(((padW >> 3) << 8) | rows);
    wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr, vx, vxr, vyt, vyb);
}

/* the VDP1 wall mode (0=textured 1=banded 2=flat) for wall wi, CAPTURED per view at accumulate time
   (wall_acc[wi].pot).  The flush forces flat for a wall with no texture slot, and textured for special
   walls.  Per-wall (was the global read-at-flush) so per-view / rotating split SQ styles the VDP1 walls
   of each viewport correctly -- the flush runs after the split loop, when the global wall_potato_mode is
   only the LAST view's.  1p / uniform split -> identical value, so byte-identical there. */
static int wall_potato(int wi)
{
    return (wi >= 0 && wi < wall_acc_n) ? (int)wall_acc[wi].pot : wall_potato_mode;
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
    wtex_bakes_win += wtex_bakes;        /* fold the PREVIOUS flush's count into the row-18 window */
    wtex_bakes = 0;                      /* count this frame's texture re-bakes (the `k` driver) */
    if (wall_acc_n == 0) { wall_px_acc = 0; return; }   /* re-arm the per-frame overflow guard: the
                             mode-4 wall_acc clear hit this early-return and the stale px_acc
                             then rejected EVERY wall forever (VD1=1, all walls CPU, 10fps --
                             owner overlay capture 2026-07-03) */
    wtex_tick++;
    /* AGE THE LOCKS, don't clear them (SATURN 2026-08-09).  1 -> 2: the slots this frame's list
       used are exactly the slots VDP1 will be re-plotting from while we build the next one, so
       they stay untouchable for one more flush.  2 -> 0: that list is off the bank now (commands
       are double-banked and the kick flipped), so the slot is genuinely free. */
    for (int i = 0; i < WTEX_SLOTS; ++i)
        wtex_cache[i].locked = (unsigned char)((wtex_cache[i].locked == 1) ? 2 : 0);

    /* (the `t` RESOLVE bracket is gone -- it measured a flat 0.0 ms with 0 refusals on every
       capture, i.e. THE TEXTURE BAKE IS NOT THE COST, and carrying it cost HWRAM.)
       The eager "resolve EVERY accumulated wall" pass that used to sit here is gone too: it baked a
       texture, paid the disc, and locked a slot for walls the loop below then drew FLAT anyway
       (surplus exhausted, potato mode, iso mode).  That waste is what drove `tx` to 26/26 and
       MANUFACTURED the evictions.  Resolution now happens inside the decision loop, only for a wall
       that has already won a textured slot in the budget -- see the wall_tex_resolve call below. */
    unsigned short em0 = frt_read();   /* SATURN: everything below is the EMIT loop -> row 20 `f` */

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
    int budget = vdp1_wall_cap - vdp1_wnext;
    int nv = sat_local_players; if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
    int nviews = sat_split_active ? nv : 1;             /* d_main renders nv views in split (2..4) */
    int surplus = budget - wall_acc_n;                 /* cmds available beyond the all-flat baseline */
    if (surplus < 0) surplus = 0;
    int surplus_per_view = surplus / nviews;
    int extra_used[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < wall_acc_n; ++i)
    {
        wall_acc[i].slot = -1;                                /* no slot until one is actually won */
        if (i >= budget) { wall_acc[i].mode = 0; continue; }   /* n > budget (cap makes this unreachable) */
        int v = (nviews > 1) ? (int)wall_acc[i].view : 0;     /* per-view surplus bin (0..nviews-1) */
        if (v >= nviews) v = nviews - 1;
        /* 3-way: 0=textured 1=banded 2=flat.  A SPECIAL wall (door/switch, wall_acc[i].special) is
           forced TEXTURED for readability even in pot2.  The "no texture slot -> flat" arm has moved
           DOWN, past the budget test: asking for a slot is the expensive part (a bake, and in the
           streaming build a synchronous disc read), so it is now the LAST question, not the first. */
        int wmode = sat_iso_flat            ? 2   /* iso mode 4: force every VDP1 wall FLAT (overdraw-vs-fill probe) */
                  : (wall_acc[i].special)  ? 0
                  : wall_potato(i);
        if (wmode != 2)                                        /* textured/banded: charge extra to surplus */
        {
            int extra = ((wmode == 1) ? wall_banded_cost(i) : wall_tilecount(i)) - 1;
            if (extra < 0) extra = 0;
            if (extra_used[v] + extra <= surplus_per_view)
            {
                /* THE ONLY resolve site.  Near-first, same order as the old eager pass, so the LRU
                   still favours the nearest walls -- but a wall that lost the surplus race never
                   gets here, so it costs no bake, no disc and no slot lock.  A refusal (pool full,
                   or the victim is still on screen -- see the 3-state lock) falls through to flat. */
                int slot = wall_tex_resolve(wall_acc[i].texnum, wall_acc[i].cmap);
                if (slot >= 0)
                {
                    wall_acc[i].slot = (short)slot;
                    extra_used[v] += extra;
                    wall_acc[i].mode = (wmode == 1) ? 3 : 1;   /* banded=3, textured=1 */
                    continue;
                }
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
        /* DROP COUNT (2026-08-03).  The core is committed by now: it handed this wall to VDP1 and
           the software column loop skipped it (that handoff is sound -- sat_wall_vdp1 returns 1 to
           reject and r_segs falls back to software BEFORE the loop, and the orphan counter reads
           N0).  So the ONLY way a claimed wall can still vanish is a silent early return in here --
           the wall-cap guard, a texture slot that will not resolve, a degenerate quad.  Rather than
           audit every `return` in three emit functions, watch the command pointer: if it did not
           move, nothing was written and this wall is a hole.  Row 13 `N<orphan>/<drop>`. */
        unsigned int wn0 = vdp1_wnext;
        int emitted = 1;
        if      (sat_wall_paint & 1)    wall_emit_flat(i);   /* DEBUG PAINT: every VDP1 wall green */
        else if (wall_acc[i].mode == 1) wall_emit(i);
        else if (wall_acc[i].mode == 3) wall_emit_banded(i);
        else if (wall_acc[i].mode == 2) wall_emit_flat(i);
        else                            emitted = 0;         /* mode 0 = nothing to draw, not a drop */
        if (emitted && vdp1_wnext == wn0 && vdp1_wall_drop < 9999) vdp1_wall_drop++;
    }

    sat_p_emit10 = (unsigned short)(frt_read() - em0) * 10u / 224u;   /* row 20 `f` */
    wall_acc_n = 0;
    wall_px_acc = 0;   /* re-arm the per-frame overflow guard */
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

/* FIELD LOCK fence -- see the long note in DG_DrawFrame.
   Fl1 = wait for the next vblank edge, whatever the frame cost.

   ⚠ The PINNED mode I wrote first (hold one field count, hysteresis) was WORSE on hardware, and the
   owner caught it: "FL1 a toujours l'air plus stable que fl2".  He is right and the reason is
   structural.  Fl1 blits on a vblank edge EVERY frame -- the software commit phase is therefore
   always identical, and only the PERIOD varies (that is judder, not misalignment).  Pinning does the
   opposite: whenever a frame overruns the pin, the wait condition is already satisfied, the blit
   happens wherever it happens, and THAT frame is not phase-locked at all.  Fl2 traded a perfect
   phase for a steady period, which is exactly backwards for this artifact.

   Fl2 = the fence, THEN hold the blit until the walls it must agree with are actually on screen.
   In 1-cycle auto a list rooted during field K is re-read at the K+1 vblank, plotted through field
   K+1 and swapped in at K+2 -- so it is VISIBLE from K+2, and the software picture (which commits
   at the blit, single-buffered NBG1) must commit at K+2 too.  age = blit_vbl - kick_vbl:

       age 2  coherent -- new walls and new floors appear on the same field
       age 1  the software picture leads by a field: NEW floors against OLD walls = the hole
       age 3  the mirror case: the walls landed a field early, floors trail

   The fence pins the BLIT to a field edge; nothing pins the KICK, whose phase inside the field
   drifts with the fractional frame period (MST 36 ms = 2.16 fields => the kick walks 0.16 field per
   frame and crosses a vblank every ~6 frames).  Each crossing costs one field of age -- a beat, the
   same shape as the one the fence killed, one layer down.  Measured on the owner's 2026-08-02
   captures with the probe on the LIVE path: `A1/2`, so both ages occur inside every one-second
   window.  Waiting for `age >= 2` costs one extra field on exactly the frames that would have been
   incoherent, and nothing on the others.

   ⚠ HISTORY, both halves of which matter.  Fl2 was once declared dead ("fl2 est aussi stable que
   fl1, mais on a toujours les ratés") -- that verdict was VOID: its condition read `vdp1_kick_vbl`,
   and until 2026-08-02 the only stamp of that variable sat on vdp1_wpn_kick's empty-bank fallback,
   which gameplay never reaches (the wall-list path returns ~30 lines earlier).  The variable held a
   title-screen value, `vbl_count - it` was always hundreds, `>= 2` was always true: the wait never
   executed once.  That experiment measured a no-op.

   🔴 With the probe on the live path Fl2 was then measured PROPERLY, and it is a REAL negative:
   4 owner captures in Fl2 read `A2/2` -- including the ones that show the offset.  The lock does
   exactly what it claims and the holes are unchanged.  Cost: `a18.2` ms of work against `MST40` ms
   of period, i.e. a whole extra field for nothing.  SHIP Fl1.  Fl2 stays only as the instrument
   that proves the age CAN be pinned to 2.
   => THE WALLS ARE NOT LATE BY A FIELD, and no further presentation-timing experiment is worth
   running.  The residual is a one-TIC skew (one 35 Hz turn tic = ~7 deg = ~25 screen px at 320/90,
   and a frame swallows up to 3 of them): the VDP1 quad geometry and the software plane's clip
   boundary must be derived from different viewpoints.  Zero at rest, proportional to turn rate,
   worse on multi-tic frames -- which is exactly what the owner sees. */
static void sat_field_fence(void)
{
    static unsigned int fv_prev = 0;
    if (fv_prev == 0 || (vbl_count - fv_prev) > 60u) fv_prev = vbl_count;   /* boot / stall -> resync */
    { unsigned int fv = vbl_count; while (vbl_count == fv) { } }
    sat_field_n = (int)(vbl_count - fv_prev);
    if (sat_field_n > 99) sat_field_n = 99;
    fv_prev = vbl_count;
}

/* SATURN MANUAL PRESENT fence + swap grant -- design at the sat_mp_* state block (~1670).
   Sits at the field-lock call site so it doubles as the Fl1 blit fence: it returns right
   after the vblank the swap executes at the end of, and the blit that follows starts on that
   edge -> picture N and wall-list N go live on the same field, every frame.
   TVSTAT bit 3 = VBLANK flag (1 inside vblank).  v2 (2026-08-19, after the owner's capture
   session): VBE erase & change, because it is the one swap both Ymir and the ST-013 contract
   time at the SAME instant (end of the vblank the write lands in) -- see the state block.
   Edge discipline: always catch a FRESH IN edge (wait out a vblank we arrive inside of):
   a write landing late in a vblank risks missing the hardware's change sample for that
   boundary, and a missed one-field FCT pulse is VOID (red p.38 note), not deferred.  The
   VBE erase this triggers on the retiring buffer is partial on NTSC (x10 deficit) and
   harmless -- the in-list colour-0 polygon owns the real erase.  Wait = align-to-vblank
   (avg ~half a field) + the vblank itself; measured into sat_mp_wait_ms (row 8). */
static void sat_mp_fence(void)
{
    uint32_t w0 = DG_GetTicksMs();
    /* (The legal two-pulse AUTO revert -- erase 0x0002 then 0x0000, Table 4.3(a) note 5 --
       left with the L+B toggle on 2026-08-19.  Re-entering 1-cycle needs it back.) */
    if (!sat_mp_pending)
        return;                                /* no kick this frame -> nothing to present */
    {   /* 1: plot done?  Parked at the empty-bank END (HW), or unmoved since the kick
           (Ymir's unmodelled register / a plot that finished before we first sampled).  A
           MOVING plot spins here -- normally already done, the kick was fields ago -- and a
           stalled one exits on the vblank bound as a force-swap (may tear once, never
           freezes).  sat_mp_wd counts those: it is the manual-mode overrun metric.
           (Known alias, 2026-08-19 audit: on HW the COPR sampled at kick time can still
           read the PREVIOUS plot's parked END == sat_mp_end_ca, so an unstarted plot
           passes the gate.  Benign on the gameplay path -- the kick precedes this fence
           by ~10+ ms, the plot is long done -- and on the menu path the ~2.5 ms mini-plot
           finishes inside the align-to-vblank wait below.) */
        unsigned int t0 = vbl_count;
        for (;;)
        {
            unsigned short c = VDP1_COPR;
            if (c == sat_mp_end_ca || c == sat_mp_copr_kick) break;
            if ((vbl_count - t0) >= SAT_MP_WD_VBL) { sat_mp_wd++; break; }
        }
    }
    /* 2: VBE erase & change at a fresh vblank-IN edge (p.40 window; SlaveDriver 0xfffe). */
    while ( (TVSTAT & 8)) { }                  /* arrived inside a vblank: wait it out    */
    while (!(TVSTAT & 8)) { }                  /* fresh IN edge                           */
    VDP1_TVMR = 0x0008;                        /* VBE=1 (TVM=000 unchanged)               */
    VDP1_FBCR = 0x0003;                        /* erase & change: swap at THIS vblank's END */
#if VDP2_CELL_SKY
    /* Deferred sky map, INSIDE the same vblank: the new sky boundary, the new wall list
       and the picture blit that follows all land on the field starting at the OUT edge.
       ~1 ms of halfwords against ~2.4 ms of vblank; an overrun only delays the blit start
       by its tail (same tear class as writing it after the fence, which auto mode does). */
    if (sky_map_pending) { sky_cell_write_map(); sky_map_pending = 0; }
#endif
    /* 3: ride to the OUT edge -- the swap has just executed (both machines).  The caller
       blits right after, racing the beam from the top of the field (Fl1 phase, ~2.5x). */
    while ( (TVSTAT & 8)) { }
    VDP1_TVMR = 0x0000;                        /* red step 7: no auto V-blank erase next  */
    sat_mp_active  = 1;
    sat_mp_pending = 0;
    sat_mp_wait_ms = (int)(DG_GetTicksMs() - w0);
}

/* (vdp1_vblank_dr CUT 2026-08-10.  It sampled EDSR.CEF at every vblank into mh_vbl_done/tot, whose
   only consumer -- row 10 `D%` -- was cut on 08-09.  Its own comment said "cut it or print it, do not
   leave it here a second time", so: cut, along with its OnVblank registration and vd1_dr_live.  It
   was an ISR running 60x/s to feed nobody.  If a done-rate is ever wanted again, note that the
   number is discredited on real hardware anyway ([[vdp1-cef-latches-on-hw]]) -- row 17 `LP%` is the
   signal that survived HW validation. */

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
    for (int i = 0; i < WPN_CACHE_N; ++i) wpn_cache[i].lump = -1;
    wpn_cache_rr[0] = wpn_cache_rr[1] = 0;
    memset(wpn_rr_spl, 0, sizeof wpn_rr_spl);
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
                                           wtex_cache[i].locked = 0; wtex_cache[i].vpad = 0; }
    wtex_tick = 0; wall_acc_n = 0;
#endif

    VDP1_TVMR = 0x0000;                              /* 16bpp, VBE=0 (erase in display: full-screen safe) */
    VDP1_EWDR = 0x0000;                              /* erase to 0 = transparent */
    VDP1_EWLR = 0x0000;
    VDP1_EWRR = (unsigned short)(((320 >> 3) << 9) | 223);
    VDP1_FBCR = 0x0000;                              /* BOOT in 1-cycle auto (known-good, Ymir-safe, == ship) */
    VDP1_PTMR = 0x0002;
#if SHOW_FPS
    /* (OnVblank += vdp1_vblank_dr REMOVED 2026-08-10 -- consumer-less ISR, see the cut note.) */
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
        for (int i = 0; i < WPN_CACHE_N; ++i) { wpn_cache[i].lump = -1; wpn_cache[i].used = 0; }
        vdp1_hud_force_recopy = 1;   /* consumed by vdp1_hud_begin -> invalidates every region's csum */
    }
#if SAT_WORLD_THINGS_VDP1
    if (!thing_acc_open)   /* split queued this frame's things PRE-kick: its open reset the
                              counters already -- resetting here would zero the overlay/mh stats */
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
    /* weapon cache: same per-frame `used` clear (all slots -- the displayed parity's flags are
       never read).  Every weapon emission runs AFTER this begin inside sat_walls_kick (1p and
       split), and the menu/intermission fallback caller emits no weapon -> never stale. */
    for (int i = 0; i < WPN_CACHE_N; ++i) wpn_cache[i].used = 0;
    /* SATURN VDP1 YAW ANTICIPATION (owner 2026-07-31).  The VDP1 walls TRAIL everything else in any
       motion, by the same amount at both junctions -- so the whole VDP1 world layer is late, and the
       lag is IRREDUCIBLE: VDP1 must plot its list for a field before the hardware shows it, and the
       root already flips at the earliest possible point (the kick, right after the BSP walk).
       Concealment is not available either: the code records two attempts at it (SAT_FLOOR_HOVER,
       SAT_CEIL_FILL) and both failed for the same reason -- the old border fills a HOLE left by a
       lagging PLANE, but here it is the WALL that lags, and a displaced wall is not a hole.
       What is left is to draw the walls where they will BELONG when they appear: predict one frame
       of view change and pre-shift the layer.  For YAW that is a pure screen translation, and slot 0
       of this bank is already a LocalCoord -- so the whole world layer (walls AND things, which lag
       together and must stay together) moves for ONE halfword, no extra command, no fill.
       Linear-at-centre, the same approximation r_main.c:1049 uses for the old anticipation; exact at
       the centre, slightly short at the edges, which is the right way to be wrong.
       NOT handled here: forward/back, which is a perspective SCALE, not a translation.  If the trail
       persists while walking, the next step is a per-quad scale over wall_acc -- more code, so it
       waits for evidence that this half is not enough. */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A;                                 /* bank cmd0 = local coord (origin, unshifted:
                                                        the anticipation moves wall_acc coordinates,
                                                        which the clip windows derive from too) */
    cmd[7] = VIEW_Y_OFFSET;                          /* local Y origin -> walls centred like NBG1 */
    vdp1_cmd_at(VDP1_BANK[vdp1_wbank], 0, cmd);
    vdp1_wnext   = 1;
    /* SATURN MANUAL PRESENT: in-list full-screen colour-0 erase as the bank's first DRAWING
       command.  Manual mode does not RELY on the HW erase: the VBE pulse in sat_mp_fence is
       there for its end-of-vblank SWAP timing, and its vblank erase only part-covers the
       retiring buffer on NTSC (~10x deficit) -- so each plot wipes its own back buffer here
       (~2.5 ms of plot inside a multi-field window).  Index 0 = VDP2-transparent,
       the same colour the retired AUTO per-field HW erase wrote (EWDR=0).  Costs one command
       slot; every downstream count (vdp1_wnext, LOPR meter, weapon slots) is cursor-based, so
       no special accounting. */
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

#if SAT_WPN_VDP1
    int split = sat_split_active;
    if (split != wpn_cache_split)
    {   /* 1p<->split flips the slot ADDRESSING (4x16KB full-res <-> 16x4KB half-res over the same
           64KB): every cached texture is at the wrong address/resolution for the new mode. */
        for (int i = 0; i < WPN_CACHE_N; ++i) { wpn_cache[i].lump = -1; wpn_cache[i].used = 0; }
        wpn_cache_rr[0] = wpn_cache_rr[1] = 0;
        memset(wpn_rr_spl, 0, sizeof wpn_rr_spl);
        wpn_cache_split = split;
    }
    unsigned int slotsz = split ? WPN_SPL_SLOTSZ : WPN_TEX_SLOTSZ;
    int wp, pp = vdp1_wbank & 1;
    int splview = 0;
    if (split)
    {   /* per-(parity,view) dedicated pair -- NUMPSPRITES==2 (gun+flash), so THIS view's lumps
           always fit its own 2 slots: no cross-view eviction, no within-frame overwrite (the
           multi firing tear).  sat_split_view is set per view by R_DrawSplitPlayerSprites. */
        extern int sat_split_view;
        splview = sat_split_view & 3;
        wp = pp * (WPN_SPL_SLOTS / 2) + splview * 2;
    }
    else
        wp = pp * WPN_SLOTS_PER;                      /* write-parity slot base (tear-safe) */
#else
    unsigned int slotsz = WPN_TEX_SLOTSZ;
    int pp = vdp1_wbank & 1;
    int wp = pp * WPN_SLOTS_PER;                      /* write-parity slot base (tear-safe) */
#endif
    for (int i = 0; i < 2; ++i)                       /* cache lookup within the pair: (lump, cmap) */
        if (wpn_cache[wp+i].lump == lump && wpn_cache[wp+i].cmap == cmap) { slot = wp+i; break; }

    if (slot >= 0)
    {
        wpn_cache[slot].used = 1;    /* this frame's quad references it: a later same-frame
                                        MISS (e.g. the flash) must not bake over it */
        padW = wpn_cache[slot].padW;
        H    = wpn_cache[slot].H;
    }
    else
    {
        /* miss: unpack the patch into the next round-robin slot of the pair -- but never into a
           slot a quad already emitted THIS frame references (the one-frame fire tear) */
        int W = (int)bswap16((unsigned short)patch->width);
        H     = (int)bswap16((unsigned short)patch->height);
        padW  = (W + 7) & ~7;
#if SAT_WPN_VDP1
        if (split)
        { int r = wpn_rr_spl[pp][splview];
          if (wpn_cache[wp + r].used && !wpn_cache[wp + (r ^ 1)].used) r ^= 1;
          slot = wp + r; wpn_rr_spl[pp][splview] = (unsigned char)(r ^ 1); }
        else
#endif
        { int r = wpn_cache_rr[pp];
          if (wpn_cache[wp + r].used && !wpn_cache[wp + ((r + 1) % WPN_SLOTS_PER)].used)
              r = (r + 1) % WPN_SLOTS_PER;
          slot = wp + r;
          wpn_cache_rr[pp] = (r + 1) % WPN_SLOTS_PER; }

        const unsigned int *colofs = (const unsigned int *)patch->columnofs;
#if SAT_WPN_VDP1
        /* 8BPP: 1 byte/texel = the LIGHT-SHADED Doom palette index (cmap[s[i]]); the CRAM bank 1
           (full-bright PLAYPAL) in WPN_CMDCOLR turns it back into the shaded colour.  Index 0 =
           transparent (SPD-off), so the padded gaps + true-black pixels show the scene through. */
        /* texel 0 is the HW transparent code, so a real black weapon pixel (shaded index 0) would
           punch a hole.  Remap 0 -> the darkest NON-zero palette index (looks black, stays opaque).
           Computed once per texture build from the live full-bright palette (bank 1 = colors[]). */
        int blk = 1, blkbest = 0x7fffffff;
        for (int p = 1; p < 256; ++p)
        {
            int lum = colors[p].r + colors[p].g + colors[p].b;
            if (lum < blkbest) { blkbest = lum; blk = p; }
        }
        if (split)
        {   /* HALF-RES bake (2x point decimation, like the things mips): the split weapon draws
               at 0.5 scale so this is 1:1 on screen.  Crop the rows that exceed the 4KB slot
               (only the tallest raise/lower frames; bottom-anchored + view-clipped = invisible). */
            padW = (((W + 1) >> 1) + 7) & ~7;
            H    = (H + 1) >> 1;
            while (H > 0 && (unsigned int)(padW * H) > WPN_SPL_SLOTSZ) H--;
            if (H < 1) return;
            volatile unsigned char *tex =
                (volatile unsigned char *)(WPN_TEX_BASE + (unsigned int)slot * WPN_SPL_SLOTSZ);
            for (int i = 0; i < padW * H; ++i) tex[i] = 0;           /* texel 0 = transparent gap */
            for (int x = 0; x < W; x += 2)
            {
                const post_t *post = (const post_t *)((const unsigned char *)patch + bswap32(colofs[x]));
                while (post->topdelta != 0xFF)
                {
                    const unsigned char *s = (const unsigned char *)post + 3;
                    int top = post->topdelta;
                    for (int i = 0; i < post->length; ++i)
                    {
                        int r = top + i;
                        if (r & 1) continue;                          /* keep even source rows */
                        r >>= 1;
                        if (r >= H) break;                            /* cropped tail */
                        int c = cmap[s[i]];
                        tex[r * padW + (x >> 1)] = (unsigned char)(c ? c : blk);
                    }
                    post = (const post_t *)((const unsigned char *)post + post->length + 4);
                }
            }
        }
        else
        {
        if ((unsigned int)(padW * H) > WPN_TEX_SLOTSZ) return;       /* too big to cache */
        volatile unsigned char *tex =
            (volatile unsigned char *)(WPN_TEX_BASE + (unsigned int)slot * WPN_TEX_SLOTSZ);
        for (int i = 0; i < padW * H; ++i) tex[i] = 0;               /* texel 0 = transparent gap */
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
        wpn_cache[slot].used = 1;
    }

    unsigned int texaddr = WPN_TEX_BASE + (unsigned int)slot * slotsz;
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
        if (split) scale <<= 1;    /* half-res texture x the 0.5 split view scale = 1:1 on screen
                                      (padW/H below are the BAKED dims, halved by the split bake) */
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
                                   const unsigned char *xlat,
                                   int x0, int y0, int x1, int y1,
                                   int cx0, int cy0, int cx1, int cy1, int flip)
{
    int padW, H, W;
    int split = sat_split_active;      /* split = per-view PRE-kick call -> queue (see thing_acc) */

    if (split && !thing_acc_open)
    {   /* first thing of this split frame: open the queue.  The per-frame housekeeping that 1p
           gets from sat_vdp1_wpn_begin (which in split only runs at the KICK, i.e. after these
           emissions) happens here instead: reset the overlay counters and the target parity's
           `used` bits (begin's own clear is post-emission in split, and is skipped entirely on
           a gated-present HOLD frame -- stale bits would fake "out of textures"). */
        int np = (vdp1_bank ^ 1) & 1;
        thing_acc_open = 1; thing_acc_n = 0;
        sat_things_n = sat_things_decl = thing_bake_n = 0;
        for (int i = 0; i < THINGS_TEX_SLOTS; ++i) thing_cache[np][i].used = 0;
    }
    if (split ? (thing_acc_n >= THING_ACC_MAX)
              : (vdp1_wnext >= vdp1_wall_cap - 1))   /* direct path needs 2 cmds (clip + quad) */
    { sat_things_decl++; thd_budget++; return 0; }

    W    = (int)bswap16((unsigned short)patch->width);
    H    = (int)bswap16((unsigned short)patch->height);
    padW = (W + 7) & ~7;
    /* too big for a slot -> software.  `thd_size` is the counter that decides whether the 1p half of
       the pool wants FEWER, BIGGER slots: the refused sprites are the NEAR ones, i.e. the most
       expensive software fills there are. */
    if ((unsigned int)(padW * H) > THINGS_TEX_SLOTSZ) { sat_things_decl++; thd_size++; return 0; }

    /* Write parity: 1p emits at the kick, after begin flipped vdp1_wbank; split emits BEFORE the
       kick, so target the parity begin WILL pick (vdp1_bank ^ 1, VDP1_DBLBANK).  Either way it is
       the pair the displayed list does not reference -> baking now never tears the shown frame. */
    int p = (split ? vdp1_bank ^ 1 : vdp1_wbank) & 1;
    int slot = -1;

    /* cache lookup: (lump, cmap, xlat).  xlat is the OTHER-PLAYER colour remap (NULL for everything
       else) and MUST be part of the key: two marines share the same PLAY* lump and, at equal light,
       the same cmap -- keyed on those two alone the second player would reuse the first one's slot
       and wear his colour. */
    /* standard light level -> the texels are baked full-bright and the light rides in CMDCOLR, so
       the colormap is NOT part of the key (see the cache declaration). */
    int  cmL      = colormaps ? (int)((cmap - colormaps) >> 8) : -1;
    int  lit_bank = (cmap && colormaps && cmap >= colormaps && cmL >= 0 && cmL <= 31);
    const unsigned char *key_cmap = lit_bank ? 0 : cmap;

    for (int i = 0; i < THINGS_TEX_SLOTS; ++i)
        if (thing_cache[p][i].lump == lump && thing_cache[p][i].cmap == key_cmap
            && thing_cache[p][i].xlat == xlat) { slot = i; break; }

    int bake = (slot < 0);
    if (bake)
    {   /* MISS: evict the OLDEST (min lru) slot NOT feeding this frame's list.  An empty slot has
           lru 0 = always picked first (fill before evicting a resident texture). */
        int oldest = -1;
        for (int i = 0; i < THINGS_TEX_SLOTS; ++i)
            if (!thing_cache[p][i].used &&
                (oldest < 0 || thing_cache[p][i].lru < thing_cache[p][oldest].lru))
                oldest = i;
        if (oldest < 0) { sat_things_decl++; thd_slot++; return 0; }   /* every slot feeds the current list -> out of textures this frame (the counter that sizes THINGS_TEX_SLOTS) */
        slot = oldest;
    }
    thing_cache[p][slot].used = 1;
    thing_cache[p][slot].lru = ++thing_lru_tick;           /* most-recently-used (hit OR bake) */
    unsigned int texaddr = THINGS_TEX_BASE + (unsigned int)(p * THINGS_TEX_SLOTS + slot) * THINGS_TEX_SLOTSZ;

    if (bake)
    {   /* unpack the full patch into this slot (once per key per parity, then reused) */
        thing_cache[p][slot].lump = lump; thing_cache[p][slot].cmap = key_cmap;
        thing_cache[p][slot].xlat = xlat; thing_bake_n++;
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
                /* texel = cmap[xlat[src]] -- translation FIRST, exactly the software order in
                   R_DrawTranslatedColumn / R_SlaveDrawTransColumn.  One extra test per texel, paid
                   only on a MISS (a bake happens once per key per parity, 'fb' on row 15). */
                for (int i = 0; i < post->length; ++i)
                { int v = xlat ? xlat[s[i]] : s[i];
                  int c = lit_bank ? v : cmap[v];   /* full-bright unless a non-standard colormap */
                  tex[(top + i) * padW + x] = (unsigned char)(c ? c : blk); }
                post = (const post_t *)((const unsigned char *)post + post->length + 4);
            }
        }
    }

    if (split)
    {   /* QUEUE: the walls have not flushed yet -- park the two commands' fields; the kick's
           vdp1_things_flush() emits them right after vdp1_walls_flush (painter order kept). */
        thing_acc[thing_acc_n].texoff = (unsigned short)((texaddr - VDP1_VRAM_BASE) >> 3);
        thing_acc[thing_acc_n].csize  = (unsigned short)(((padW >> 3) << 8) | H);
        thing_acc[thing_acc_n].colr   = (unsigned short)((WPN_CMDCOLR & 0xE000u)
                              | (lit_bank ? wall_light_colr(cmap) : (WPN_CMDCOLR & 0x1F00u)));
        thing_acc[thing_acc_n].x0  = (short)x0;  thing_acc[thing_acc_n].y0  = (short)y0;
        thing_acc[thing_acc_n].x1  = (short)x1;  thing_acc[thing_acc_n].y1  = (short)y1;
        thing_acc[thing_acc_n].cx0 = (short)cx0; thing_acc[thing_acc_n].cy0 = (short)cy0;
        thing_acc[thing_acc_n].cx1 = (short)cx1; thing_acc[thing_acc_n].cy1 = (short)cy1;
        thing_acc[thing_acc_n].flip = (unsigned char)(flip != 0);
        thing_acc_n++;
        sat_things_n++;
        return 1;
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
    /* prio-select bits from WPN_CMDCOLR, CRAM BANK from this thing's light (bank 1 = full bright
       when the texels already carry a non-standard map). */
    cmd[3] = (unsigned short)((WPN_CMDCOLR & 0xE000u)
                              | (lit_bank ? wall_light_colr(cmap) : (WPN_CMDCOLR & 0x1F00u)));
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

/* Kick-time drain of the split thing queue into the command bank, AFTER vdp1_walls_flush.
   Same two commands per entry as the 1p direct path (FUNC_UserClip box + prio-7 distorted
   quad).  Entries beyond the command budget are DROPPED (their software fill was already
   skipped -> the sprite vanishes this frame, exactly the overrun failure mode), so a drop
   also feeds the AIMD back-off below -- the per-view cap shrinks until walls+things fit. */
static void vdp1_things_flush(void)
{
    unsigned short cmd[16];
    int i;
    for (i = 0; i < thing_acc_n; ++i)
    {
        if (vdp1_wnext >= vdp1_wall_cap - THING_FLUSH_MARGIN) break;
        memset(cmd, 0, sizeof cmd);
        cmd[0]  = 0x0008;                          /* FUNC_UserClip */
        cmd[6]  = thing_acc[i].cx0; cmd[7]  = thing_acc[i].cy0;
        cmd[10] = thing_acc[i].cx1; cmd[11] = thing_acc[i].cy1;
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);

        memset(cmd, 0, sizeof cmd);
        short xl = thing_acc[i].flip ? thing_acc[i].x1 : thing_acc[i].x0;
        short xr = thing_acc[i].flip ? thing_acc[i].x0 : thing_acc[i].x1;
        cmd[0] = 0x0002;                           /* distorted sprite */
        cmd[2] = 0x04A0;                           /* 256-bank | ECD-off | SPD CLEAR | Window_In */
        cmd[3] = thing_acc[i].colr;                /* prio 7 (above NBG1) | this thing's CRAM light bank */
        cmd[4] = thing_acc[i].texoff;
        cmd[5] = thing_acc[i].csize;
        cmd[6]  = xl; cmd[7]  = thing_acc[i].y0;
        cmd[8]  = xr; cmd[9]  = thing_acc[i].y0;
        cmd[10] = xr; cmd[11] = thing_acc[i].y1;
        cmd[12] = xl; cmd[13] = thing_acc[i].y1;
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
    }
    if (i < thing_acc_n)
    {   /* Bank-overflow drop: the tail's software fill was ALREADY skipped (sat_thing_vdp1 marked at
           queue time, R_DrawMasked/R_SlaveDrawMasked continue past it) -> those enemies VANISH this
           frame.  This is a HARD, RELIABLE signal (unlike the noisy 1p EDSR-CEF), so back off
           AGGRESSIVELY and RESET the learned floor -> the cap converges to a fit fast (ideally next
           frame), keeping the vanish to a rare 1-frame transient on scene entry.  The AIMD-damp
           hysteresis/floor is deliberately NOT applied here: a damped (slow) back-off or a high learned
           floor keeps the queue overflowing EVERY frame = PERSISTENT vanishing enemies in split (the
           2026-07-09 regression).  Damping stays only on the 1p CEF back-off (sat_walls_kick). */
        sat_things_decl += thing_acc_n - i;
        sat_things_n    -= thing_acc_n - i;
        /* Converge in ONE frame (not -2/frame = several frames of vanishing): clamp the per-view cap to
           what actually fit this frame (fit / nv views), so next frame the total re-queued (~fit) fits
           the bank -> no re-drop.  Undershoot is safe (extra sprites just go software, never vanish). */
        { extern int sat_local_players; int nv = sat_local_players; if (nv < 1) nv = 1;
          int fit = i / nv; if (fit < sat_thing_emit_cap) sat_thing_emit_cap = fit;
          if (sat_thing_emit_cap < 0) sat_thing_emit_cap = 0; }
    }
    thing_acc_n = 0; thing_acc_open = 0;
}
#endif

/* VDP1 status bar -- 1-FRAME-LATENT capture/emit split.  The VDP1 world list is closed +
   flipped by sat_walls_kick DURING R_RenderPlayerView, BEFORE ST_Drawer composes this frame's
   bar; so we cannot capture-and-emit in one place.  vdp1_hud_capture() (DG_DrawFrame, after
   ST_Drawer) snapshots frame N's bar into VDP1_HUD_TEX; vdp1_hud_emit() (frame N+1's kick,
   after the weapon) draws it.  A status bar ticks slowly -> the 1-frame lag is imperceptible.
   ADDITIVE: the software bar is still blitted underneath (fallback), fully covered by this
   opaque prio-7 sprite -> it never vanishes even if a heavy plot cuts the emit. */
/* VDP1 HUD regions (crisp split HUD, docs/LOWRES_RENDER_STUDY.md).  Under the M7 x2 NBG1 zoom the
   software HUD garbles, so each mode's HUD is captured from the framebuffer into VDP1_HUD_TEX and
   emitted as an un-zoomed prio-7 sprite.  Region count by mode: 1p bar = 1 (rows 192..223, 320x32),
   2p panels = 1 (rows 160..223, 320x64), 3/4p bands = 2 strips (rows 96..111 + 208..223, 320x16 each
   -- the compact bands are interleaved with the packed views so they cannot ride a single tail).
   VRAM: stacked from VDP1_HUD_TEX (0x25C78000); worst = 2p 20KB -> 0x25C7D000, which runs into the
   1p-HUD region (1p-only, so never live at the same time) and then into what used to be the ftex
   F-banks -- free VDP1 VRAM since the floor deport was cut 2026-08-02.  Things end exactly at
   0x25C78000, so the HUD stack is clear on both sides in every reachable mode. */
#define VDP1_HUD_REGIONS 2
static struct vdp1_hud_region { short hy, hh, spd; unsigned int tex, csum; } vdp1_hud_reg[VDP1_HUD_REGIONS];
static int          vdp1_hud_nreg  = 0;   /* regions captured this frame (0 = no VDP1 HUD -> software stands) */
static int          vdp1_hud_ready = 0;
static unsigned int vdp1_hud_texoff = 0;  /* running VDP1_HUD_TEX offset while adding regions */

static void vdp1_hud_begin(void)
{
    vdp1_hud_nreg = 0; vdp1_hud_texoff = 0;
    if (vdp1_hud_force_recopy)   /* palette change: invalidate geometry so vdp1_hud_add recopies */
    {
        for (int i = 0; i < VDP1_HUD_REGIONS; ++i) vdp1_hud_reg[i].hy = -1;
        vdp1_hud_force_recopy = 0;
    }
}

/* Add one HUD strip: framebuffer rows [hy,hy+hh) (full 320) -> VDP1_HUD_TEX + running offset.
   spd 1 = opaque (SPD SET, idx0 drawn: 1p bar / 2p panels); 0 = SPD-off (idx0 transparent: 3/4p
   bands, so the empty minimap-slot area of a strip does not paint).  Checksum-gated per region;
   a geometry/offset change forces a recopy. */
static void vdp1_hud_add(int hy, int hh, int spd)
{
    if (vdp1_hud_nreg >= VDP1_HUD_REGIONS) return;
    struct vdp1_hud_region *r = &vdp1_hud_reg[vdp1_hud_nreg];
    unsigned int tex = VDP1_HUD_TEX + vdp1_hud_texoff;
    const unsigned int *s32 = (const unsigned int *)(framebuffer + hy * 320);
    unsigned int csum = 2166136261u;
    for (int i = 0; i < 320 * hh / 4; ++i) csum = (csum ^ s32[i]) * 16777619u;
    if (csum != r->csum || r->hy != hy || r->hh != hh || r->tex != tex)   /* change/geometry -> recopy */
    {
        r->csum = csum;
        memcpy((unsigned char *)tex, framebuffer + hy * 320, 320 * hh);
    }
    r->hy = (short)hy; r->hh = (short)hh; r->spd = (short)spd; r->tex = tex;
    vdp1_hud_nreg++;
    vdp1_hud_texoff += 320u * (unsigned)hh;
}

/* Emit every captured HUD region as a prio-7 NORMAL sprite (1:1 -> crisp, VDP1 ignores the NBG1
   zoom).  The capture site sets the regions (1p / 2p-M7 / 3-4p-M7) and clears them for non-M7 /
   non-level, where the software HUD stands.  ADDITIVE: the software HUD is still (decimated-)blitted
   underneath as a never-vanish fallback, covered by these sprites. */
static void vdp1_hud_emit(void)
{
    if (!vdp1_hud_ready || !vdp1_wactive) return;
    for (int i = 0; i < vdp1_hud_nreg; ++i)
    {
        if (vdp1_wnext >= VDP1_CMD_GUARD) break;
        const struct vdp1_hud_region *r = &vdp1_hud_reg[i];
        unsigned short cmd[16];
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0000;                                   /* NORMAL sprite (native size, no scale) */
        cmd[2] = r->spd ? 0x00E0 : 0x00A0;                 /* SPD SET opaque / SPD-off idx0 transparent */
        cmd[3] = WPN_CMDCOLR;                              /* prio 7 (above NBG1) | CRAM bank 1 (= colors[]) */
        cmd[4] = (unsigned short)((r->tex - VDP1_VRAM_BASE) >> 3);
        cmd[5] = (unsigned short)(((HUD_W >> 3) << 8) | r->hh);   /* 40 cols x hh px */
        cmd[6] = 0; cmd[7] = (unsigned short)r->hy;         /* top-left (0, hy) */
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext++, cmd);
    }
}

/* HU MESSAGE on VDP1 (LOWRES only, docs/LOWRES_RENDER_STUDY.md Phase 2): the pickup/message line
   (HU_MSGY=0, 1 hu_font line) is mangled by the M7 x2 zoom.  In lowres 1p the core (hu_stuff.c)
   redirects w_message via V_UseBuffer(sat_hu_msg_buf) to draw its glyphs DIRECTLY into a VDP1 VRAM
   slot (byte writes -- proven by the 8bpp weapon bake; the SH-2 cache is write-through so they land
   in VRAM), and we emit that slot as a prio-7 sprite with SPD-OFF (index 0 transparent -> only the
   glyphs draw over the view).  Drawing into VRAM (not an HWRAM scratch) keeps the boot-loop pool
   healthy -- a 320x8 .bss scratch was cheap but a 320x16 one drove _end past the SGL work area
   (NEGATIVE pool).  DOUBLE-BUFFERED by frame parity: the slot being emitted/plotted is never the
   one the core draws into -> tear-free.  1-frame latent, like the status bar. */
extern "C" unsigned char *sat_hu_msg_buf;    /* core hu_stuff.c: message draw target (NULL = draw to fb) */
extern "C" int            sat_hu_msg_drawn;   /* core: 1 = the message widget was on this frame */
static unsigned int vdp1_hud_msg_emit_addr = 0;  /* VRAM slot the emit references (last frame's draw) */
static int          vdp1_hud_msg_active = 0;

/* Emit the last-drawn message slot as a prio-7 NORMAL sprite, index-0 transparent (glyphs over the
   view).  Lowres only; skipped when no message was on. */
static void vdp1_hud_msg_emit(void)
{
    extern int sat_local_players;
    /* 1p only (the message VRAM redirect is armed only in 1p): also gate on the CURRENT count so a
       1p-M7 -> split drop-in can't emit last frame's stale 1p message over the new split view. */
    if (sat_local_players != 1 || !sat_lowres || !vdp1_hud_msg_active || !vdp1_hud_msg_emit_addr
        || !vdp1_wactive || vdp1_wnext >= VDP1_CMD_GUARD) return;
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x0000;                                   /* NORMAL sprite (1:1, no scale) */
    cmd[2] = 0x00A0;                                   /* 256-bank | ECD-off | SPD OFF (idx0 transparent) */
    cmd[3] = WPN_CMDCOLR;                              /* prio 7 (above NBG1) | CRAM bank 1 (= colors[]) */
    cmd[4] = (unsigned short)((vdp1_hud_msg_emit_addr - VDP1_VRAM_BASE) >> 3);
    cmd[5] = (unsigned short)(((320 >> 3) << 8) | HU_MSG_H);  /* charSize 0x2808 (40 cols x 8 px) */
    cmd[6] = 0; cmd[7] = 0;                             /* top-left (0,0) */
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
    {   /* VDP1 transfer-over meter (SEGA VDP1 UM p.52-53 LOPR/COPR/BEF; docs/VDP1_LIMITS_SOURCED.md).
           The list VDP1 is displaying used bank vdp1_bank with vdp1_last_cmds cmds -- BOTH still hold the
           PREVIOUS frame's values HERE (updated just below).  LOPR = the cmd addr where that plot ENDED;
           compare to the list end to see if it finished ("transfer-over" = the flicker).  Cmd addrs are
           (VRAM byte offset)>>3, exactly like the CMDLINK writes -- so end = (bank_off + n*32)>>3. */
        unsigned int bank_off = VDP1_BANK[vdp1_bank & 1] - VDP1_VRAM_BASE;   /* 0x100 (bank0) / 0x2100 (bank1) */
        unsigned int base_ca  = bank_off >> 3;
        unsigned int end_ca   = (bank_off + (unsigned int)vdp1_last_cmds * 32u) >> 3;
        vdp1_lopr  = VDP1_LOPR;
        vdp1_copr  = VDP1_COPR;
        vdp1_endca = (unsigned short)end_ca;
        {   /* LP = how far LOPR got through the W bank.  BOTH "completed" directions read 100:
               - LOPR ABOVE end  (in the F/floor bank ~0xF800): got>span -> clamp to span -> 100.
               - LOPR BELOW base (Lc=0xc): the plot FINISHED the W bank and jumped to the empty/idle
                 chain before we sampled -- HW-VERIFIED 2026-07-26 (short lists that finish read Lc;
                 long lists that overrun read a mid-bank addr).  This is COMPLETION, not 0%.
               Only LOPR strictly INSIDE [base,end] is a real transfer-over (LP<100 = the flicker). */
            int span = (int)end_ca - (int)base_ca;         /* = vdp1_last_cmds * 4 (cmd-addr units) */
            int got  = (int)vdp1_lopr - (int)base_ca;
            int overran = 0;
            if      (got < 0)     vdp1_lp_pct = 100;       /* finished W -> jumped past it (idle/F)   */
            else if (span <= 0)   vdp1_lp_pct = 100;       /* empty list                              */
            else if (got >= span) vdp1_lp_pct = 100;       /* finished into the F bank                */
            else { vdp1_lp_pct = got * 100 / span; overran = 1; }   /* real transfer-over (LP<100)    */
            /* MEASURED BUDGET (feed-forward, no guess): on an overrun the plot completed exactly
               (got/4) commands this frame -- that IS the per-frame VDP1 command budget, read straight
               from the hardware.  Snap to it.  On a clean frame the budget is >= the list and we can't
               see the surplus, so drift up +1 / THING_LP_CLEAN to re-probe headroom when a scene eases.
               vdp1_budget_cmds==0 means "not yet measured" -> the AIMD treats it as unlimited (slot cap). */
            /* A NEW MAP IS A NEW SCENE: a measurement taken in the previous level's worst corridor
               is not a budget for this one, and nothing else in the loop ever clears it. */
            { extern int gamemap;
              if (gamemap != vdp1_budget_map)
              { vdp1_budget_map = gamemap; vdp1_budget_cmds = 0; vdp1_budget_clean = 0; } }
            if (overran) { vdp1_budget_cmds = got / 4; vdp1_budget_clean = 0; }
            else if (vdp1_budget_cmds > 0 && ++vdp1_budget_clean >= THING_LP_CLEAN) {
                int gap = WALL_CMD_CAP - vdp1_budget_cmds;    /* geometric climb, additive near the top */
                int step = gap >> 4;
                if (step < 1) step = 1;
                if (step > THING_BUDGET_STEP) step = THING_BUDGET_STEP;
                vdp1_budget_cmds += step;
                if (vdp1_budget_cmds > WALL_CMD_CAP) vdp1_budget_cmds = WALL_CMD_CAP;
                vdp1_budget_clean = 0; }
            /* WEAPON GUARANTEE (closed loop).  The gun is the last world command, so the plot
               reached it iff the list completed OR LOPR passed its slot.  This is the ONLY place
               the guarantee is actually verified -- everything else is allocation policy. */
            if (vdp1_wpn_slot_disp > 0)
            {
                if (!overran || (got / 4) >= vdp1_wpn_slot_disp)
                {
                    if (++vdp1_wpn_safe >= WPN_SAFE_DECAY)
                    {   /* sustained headroom -> hand one command-equivalent back to the world */
                        if (vdp1_wpn_reserve > WPN_RESERVE_MIN) vdp1_wpn_reserve--;
                        vdp1_wpn_safe = 0;
                    }
                }
                else
                {   /* the gun was cut: shed things (then far walls) BEFORE it, starting next frame */
                    vdp1_wpn_cut++;
                    vdp1_wpn_safe = 0;
                    vdp1_wpn_reserve += WPN_RESERVE_UP;
                    if (vdp1_wpn_reserve > WPN_RESERVE_MAX) vdp1_wpn_reserve = WPN_RESERVE_MAX;
                }
            } }
        {   int u = 0; for (int i = 0; i < WTEX_SLOTS; ++i) if (wtex_cache[i].texnum >= 0) u++;
            vdp1_tx_used = u; vdp1_tx_total = WTEX_SLOTS; }
    }
    vdp1_last_cmds = vdp1_wactive ? vdp1_wnext : 0;
    vdp1_wpn_slot_disp = vdp1_wactive ? vdp1_wpn_slot_end : 0;   /* the list LOPR will report on next kick */
    /* (vd1_wpct CUT 2026-08-09 with the row-8 `w%` it fed -- a division per frame to restate row 17
       `c` as a percentage of a constant.) */
    /* (the vd1_dr_live gate went with vdp1_vblank_dr, 2026-08-10.) */
    /* Phase-0 fallback profiler: snapshot the just-rendered frame's tally into cur + windowed peaks
       (r_segs.c accumulated the counters across this frame's segs), then reset below. */
    fb_cur_clamp = sat_fb_clamp_t; fb_cur_mag = sat_fb_mag_t; fb_cur_px = sat_fb_px;
    fb_cur_wclamp = sat_fb_wclamp_t;
    if (sat_fb_clamp_t  > fb_pk_clamp)  fb_pk_clamp  = sat_fb_clamp_t;
    if (sat_fb_mag_t    > fb_pk_mag)    fb_pk_mag    = sat_fb_mag_t;
    if (sat_fb_starve_t > fb_pk_starve) fb_pk_starve = sat_fb_starve_t;
    if (sat_fb_px       > fb_pk_px)     fb_pk_px     = sat_fb_px;
    sat_fb_edge_t = 0; sat_fb_edge_w = 0;
    sat_fb_edge_b[0] = sat_fb_edge_b[1] = sat_fb_edge_b[2] = sat_fb_edge_b[3] = 0;
#endif
    sat_fb_clamp_t = sat_fb_mag_t = sat_fb_starve_t = sat_fb_px = 0;   /* reset each frame (also when SHOW_FPS off) */
    sat_fb_wclamp_t = 0;
    /* Advance the per-seg visit tag ONE step per rendered frame, HERE and nowhere else: the kick is
       past this frame's BSP walk and before the next one, and it is per FRAME, not per split VIEW
       (the views drain into the shared bank above).  A per-view counter -- framecount is one -- would
       make every seg visible in only ONE view look "not seen last frame" every frame and arm the
       entry coverage permanently.  See core/r_segs.c sat_seg_entry_cover. */
    sat_seg_frame++;
    /* SATURN mode-switch VDP1 erase: while sat_vdp1_switch_clear is armed, skip the coherent-pair
       HOLD and take the empty-bank present path below -> the old mode's walls are wiped in 2 frames
       (the new mode rebuilds its pair from the very next frame). */
    if (vdp1_wactive && sat_vdp1_switch_clear == 0)
    {
        unsigned short end[16];
        memset(end, 0, sizeof end);
        /* PRESENT: close this frame's wall list with a JUMP to the empty bank, point the root at it
           and start the plot at once.  HISTORY -- this used to be the COHERENT-PAIR HOLD (owner
           2026-07-03, against alternating perfect/destroyed frames): the root was NOT flipped here,
           VDP1 kept replotting last frame's walls+floors pair while vdp1_ftex_flush built the F
           floor bank, chained it and flipped once to the complete fresh pair.  The 2026-07-31 M7
           latency fix already bypassed that hold whenever no floors were claimed -- which was every
           frame -- and with the VDP1 floor deport itself cut (M5, 2026-08-02) there is no second
           half left to pair with, so the bypass is now the only path and the hold is gone.  The JUMP
           terminator (rather than a plain END) is kept verbatim from the shipped path: the empty
           bank's END closes the chain and the sysclip values must match the root's. */
        end[0]  = 0x0009 | 0x1000;                   /* sysclip (non-drawing) + JUMP_ASSIGN */
        end[1]  = (unsigned short)((VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3);
        end[10] = 319; end[11] = 223;                /* keep the sysclip values (== root's) */
        vdp1_cmd_at(VDP1_BANK[vdp1_wbank], vdp1_wnext, end);
        vdp1_bank = vdp1_wbank;
        *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) =
            (unsigned short)((VDP1_BANK[vdp1_wbank] - VDP1_VRAM_BASE) >> 3);
        /* SATURN MANUAL PRESENT: PTM=1 plots ONCE, now, into the clean back buffer (the swap
           is granted at the frame-end fence).  The wall chain terminates through the empty
           bank's END -> stage that address for the fence's COPR completion compare. */
        sat_mp_end_ca = (unsigned short)((VDP1_BANKE_ADDR + 0x20u - VDP1_VRAM_BASE) >> 3);
        VDP1_PTMR = 0x0001; sat_mp_copr_kick = VDP1_COPR; sat_mp_pending = 1;
        vdp1_wactive = 0;
        return;
    }
    else
    {
        if (sat_vdp1_switch_clear > 0) sat_vdp1_switch_clear--;   /* SATURN: consume a mode-switch erase frame */
        link = (VDP1_BANKE_ADDR - VDP1_VRAM_BASE) >> 3;
    }
    /* SATURN MANUAL PRESENT: this (empty-bank) path runs on menu/intermission and mode-switch
       frames.  Under manual mode the empty bank draws nothing AND no per-field HW erase runs,
       so presenting it would leave the last level frame's pixels on screen; build a mini
       ERASE list instead (local-coord + full-screen colour-0 polygon + END) in the other
       bank and present that -- the manual-mode equivalent of the retired AUTO per-field wipe. */
    {
        int wb = vdp1_bank ^ 1;
        unsigned short mc[16];
        memset(mc, 0, sizeof mc);
        mc[0] = 0x000A; mc[7] = VIEW_Y_OFFSET;           /* local coord (== the wall banks') */
        vdp1_cmd_at(VDP1_BANK[wb], 0, mc);
        memset(mc, 0, sizeof mc);
        mc[0]  = 0x0004; mc[2] = 0x00C0; mc[3] = 0x0000; /* colour-0 SPD polygon, full screen */
        mc[6]  = 0;   mc[7]  = 0;
        mc[8]  = 319; mc[9]  = 0;
        mc[10] = 319; mc[11] = 223;
        mc[12] = 0;   mc[13] = 223;
        vdp1_cmd_at(VDP1_BANK[wb], 1, mc);
        memset(mc, 0, sizeof mc);
        mc[0] = 0x8000;                                  /* END */
        vdp1_cmd_at(VDP1_BANK[wb], 2, mc);
        vdp1_bank = wb;
        link = (VDP1_BANK[wb] - VDP1_VRAM_BASE) >> 3;
        sat_mp_end_ca = (unsigned short)((VDP1_BANK[wb] + 2u * 32u - VDP1_VRAM_BASE) >> 3);
    }
    /* atomic single-halfword flip of the root command's jump target */
    *((volatile unsigned short *)VDP1_ROOT_ADDR + 1) = (unsigned short)link;
    VDP1_PTMR = 0x0001; sat_mp_copr_kick = VDP1_COPR; sat_mp_pending = 1;
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
#if SAT_WORLD_THINGS_VDP1
        thing_acc_n = 0; thing_acc_open = 0;   /* and the split frame's queued things (their bakes
                                                  hit the non-displayed parity -> harmless; the
                                                  cache keys persist = free hits next frame) */
#endif
        vdp1_kicked_this_frame = 1; /* suppress the DG_DrawFrame fallback kick */
        return;
    }
#endif
    {   /* SATURN PERF: time the VDP1 present (close bank + flip root LINK) -> overlay 'pr'. */
        unsigned short pk0 = frt_read();
        /* VDP1 command RESERVATION (fixes the 3/4p HUD-drop / doubled-band): cap the walls short of
           VDP1_CMD_GUARD by the overlays the split emits AFTER them -- weapon copy-2 (one per view)
           + the HUD band strip(s) + the HU message + margin.  1p is unchanged (WALL_CMD_CAP); only
           the split reserves more, yielding its farthest walls (the overflow tail) so weapon+HUD
           always plot.  Set BEFORE vdp1_walls_flush; read by every wall-emit guard. */
        {
            int nv = sat_split_active ? sat_local_players : 1;
            if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
            if (nv <= 1) vdp1_wall_cap = WALL_CMD_CAP;        /* 1p: 1 weapon + 1 HUD fit the nominal 6-slot margin */
            else {
                int nhud    = (nv >= 3) ? 2 : 1;              /* 3/4p = 2 band strips; 2p-M7 = 1 panel region */
                int reserve = 3 * nv + nhud + 3;             /* <=3 cmds/view for weapon(2) [clip+gun+flash] + HUD
                                                                strip(s) + HU msg + margin: GUARANTEES the HUD/panel
                                                                always plots (far walls yield instead).  2p INCLUDED:
                                                                its M7 VDP1 HUD needs 7 overlay slots > the old 6-slot
                                                                margin (WALL_CMD_CAP), so a dense 2p-both-firing frame
                                                                dropped the panel -- same bug class as 3/4p (review
                                                                2026-07-15).  2p reserve=10 -> cap 244 (drops ~4 far
                                                                walls only when saturated). */
                int cap     = VDP1_CMD_GUARD - reserve;
                if (cap > WALL_CMD_CAP)        cap = WALL_CMD_CAP;         /* never above the nominal wall budget */
                if (cap < WALL_ACC_MAX + 16)   cap = WALL_ACC_MAX + 16;    /* floor: keep budget >= wall_acc_n so the
                                                                             flush can give every accumulated wall at
                                                                             least a FLAT quad (mode!=0); below
                                                                             WALL_ACC_MAX far walls would VANISH to sky,
                                                                             not degrade.  Unreachable at the current
                                                                             reserves (min cap 237) -- kept coherent. */
                vdp1_wall_cap = cap;
            }
        }
        sat_vdp1_wpn_begin();       /* reset the bank + local-coord (walls flushed BETWEEN the copies) */
#if SAT_WPN_VDP1
        /* SINGLE-EMIT the player weapon, LAST (owner 2026-07-31).  VDP1 has NO z-buffer: it
           rasterises every command into ONE framebuffer in painter order, so where the gun overlaps
           a wall or a monster the LAST-drawn command wins that pixel (priority only decides that
           pixel vs NBG1, it cannot undo an overwrite).  "Gun above walls AND above monsters" is
           therefore a HARD ordering requirement: walls -> things -> weapon -> HUD.
           The old design drew the weapon TWICE (a copy before the walls as an anti-flicker prefix,
           a copy after them for the on-top) because "after" is the tail a plot OVERRUN cuts.  That
           traded a real cost for a hedge: the gun is a large sprite, so copy (1) paid its FULL fill
           time every frame -- inside the very budget whose exhaustion it was insuring against.
           Removing it gives that time back to the plot, which the LOPR budget re-probes upward on
           its own (vdp1_budget_cmds drifts +1 / THING_LP_CLEAN clean frames).
           "Always displayed" is no longer a hedge but a CLOSED LOOP: the weapon is the last world
           command, so "did the plot reach it" is directly observable (LOPR >= its slot) instead of
           inferred.  vdp1_wpn_slot_end records where it lands; vdp1_wpn_kick checks LOPR against the
           DISPLAYED list's copy and grows vdp1_wpn_reserve when it was cut, so the next frame sheds
           things/far walls BEFORE the gun.  See the reserve block below.
           Emitted after the BSP walk, before the end-of-planes present (the only pre-flip window);
           sat_psprite_early makes R_DrawMasked skip the late software draw.  SPLIT fans out per view
           (R_DrawSplitPlayerSprites).  sat_wall_skip gate -> M0 keeps the SOFTWARE weapon. */
        auto sat_emit_weapon = [](void)
        {
            /* (no LocalCoord reset needed: the anticipation lives in the world geometry itself, so
               the gun and the HUD -- which build their own screen coordinates -- never see it.) */
            if (sat_psprite_early && !viewangleoffset && sat_wall_skip && !sat_wpn_soft)
            {
                extern int sat_split_active;
                extern void R_DrawSplitPlayerSprites(void);
                if (sat_split_active) R_DrawSplitPlayerSprites();   /* per-view weapons */
                else                  R_DrawPlayerSprites();
                vdp1_wpn_slot_end = vdp1_wnext;   /* slot the plot must reach for a visible gun */
            }
            else vdp1_wpn_slot_end = 0;   /* no VDP1 gun this frame (M0 / software weapon / side view)
                                             -> nothing to guarantee; leave the reserve loop idle */
        };
#endif
#if VDP1_WALL_TEST
        vdp1_walls_flush();         /* walls BETWEEN the two weapon copies */
#endif
#if SAT_WORLD_THINGS_VDP1
        /* ADAPTIVE things budget (AIMD).  The VDP1 raster is SHARED with the walls, whose share
           swings wildly (open outdoor = few segs, spare VDP1 -> many enemies fit; tech room = dense
           = VDP1 already near full -> flickered at only th4).  So grow the cap slowly while the plot
           FINISHED (vdp1_prev_done, EDSR CEF), back off fast (-2) when it OVERRAN (would drop+vanish
           things).  It settles just under the per-scene flicker threshold: high outdoors, low indoors. */
        /* THINGS emit budget.  1p = WBUDGET (baked default): drive ec from the REAL remaining VDP1
           command budget -- the walls (+weapon copy 1) just flushed into vdp1_wnext, so (free slots /
           ~2 cmds-per-thing) = how many things fit this frame.  This BYPASSES the EDSR-CEF, which
           false-latches 30-60% "not done" on real HW and used to collapse ec to 0 (monsters never
           reached VDP1 -- HW-proven, [[aimd-wbudget-hw-cef-collapse]]).  A real plot-TIME overrun (not
           command count) would still show as tail flicker -- that IS the diagnostic.  Split now uses the
           SAME WBUDGET (2026-07-20): the old CEF-driven damped path collapsed ec to 0 on HW, dumping every
           world sprite to the slave-disabled master software fill in M7 (the MP fps killer) -- so split
           drives the same command budget, just divided per view (nv views each emit up to ec). */
        if (!sat_split_active) {
            /* ---- BUDGET-DRIVEN LOD (2026-07-26) --------------------------------------------------
               Allocate this frame's VDP1 work against the MEASURED budget (vdp1_budget_cmds, read from
               LOPR) instead of the static slot cap -- so the default IS the real per-frame budget, not
               a guess.  vdp1_wnext here = the weapon(1)+walls already flushed, so (budget - wnext) =
               the commands left for THINGS + weapon(2).  Two levers, in gameplay-priority order (shed
               far THINGS before near WALLS):
                 (1) THINGS: cap = room>>1; the shed sprites fall to the SOFTWARE fill (crisp->software,
                     visible + STABLE, never blink); actor-first rank keeps the monsters you fight crisp.
                 (2) WALLS : only if the walls ALONE eat the budget (room<=trigger) AND the master has
                     headroom -> push near walls to software (lower sat_wall_cpu_span) to free VDP1.
                     Gated on rp_master_ms so we NEVER trade a blink for decrochage (two-engine balance). */
            int cap_cmds = vdp1_wall_cap;                       /* hard SLOT ceiling (248) */
#if SHOW_FPS
            if (vdp1_budget_cmds > 0 && vdp1_budget_cmds < cap_cmds)
                cap_cmds = vdp1_budget_cmds;                    /* measured TIME budget (usually tighter) */
#endif
            /* vdp1_wpn_reserve is withheld FIRST: the gun is emitted after the things, so anything
               the things allocate here is spent ahead of it in the plot.  THING_FLUSH_MARGIN keeps
               its SLOTS free; the reserve keeps its plot TIME free (the resource that actually cuts). */
            int room = cap_cmds - vdp1_wnext - THING_FLUSH_MARGIN - vdp1_wpn_reserve;
            int budget_cap = (room > 0) ? (room >> 1) : 0;      /* ~2 VDP1 cmds per emitted thing */
            if (budget_cap > THING_ADAPT_MAX) budget_cap = THING_ADAPT_MAX;
            if (sat_thing_emit_cap < budget_cap)      sat_thing_emit_cap += 2;         /* smooth ramp up */
            else if (sat_thing_emit_cap > budget_cap) sat_thing_emit_cap = budget_cap; /* snap down to fit */
            if (sat_thing_emit_cap < 0) sat_thing_emit_cap = 0;
#if SHOW_FPS
            /* (2) WALL LOD: engage only when things are already shed and the walls STILL overrun the
               VDP1 budget (room for things <= trigger), AND the master (software) has room to take them.
               Lower the span -> more near walls -> software (CPU), freeing VDP1; relax back to the core
               default when VDP1 fits again OR the master is saturated (never worsen decrochage). */
            {   int room_for_things = cap_cmds - vdp1_wnext - vdp1_wpn_reserve;  /* left after walls + gun */
                int master_ok = (rp_master_ms > 0 && rp_master_ms < SOFT_BUDGET_MS);
                if (vdp1_budget_cmds > 0 && room_for_things <= WALL_LOD_TRIGGER && master_ok) {
                    if (sat_wall_cpu_span > WALL_SPAN_MIN)         sat_wall_cpu_span -= WALL_SPAN_STEP;
                } else if (sat_wall_cpu_span < SAT_WALL_CPU_SPAN_DEF) {
                    sat_wall_cpu_span += WALL_SPAN_STEP;
                    if (sat_wall_cpu_span > SAT_WALL_CPU_SPAN_DEF) sat_wall_cpu_span = SAT_WALL_CPU_SPAN_DEF;
                }
                sat_wall_cpu_v1 = sat_wall_cpu_span + WALL_PREWARM_BAND;   /* keep the pre-warm band width -> V1 is the real VDP1-exit */
            }
#endif
        } else {
            /* SPLIT WBUDGET (2026-07-20): identical command-budget policy to the 1p branch above, but
               sat_thing_emit_cap is PER-VIEW and nv views each emit up to it into the shared queue, so
               the free room is divided by 2*nv (nv=1 would give the 1p room>>1).  Replaces the old
               EDSR-CEF damped back-off, which the false "not done" latch (30-60% on HW) collapsed to
               ec0 -- every world sprite then fell to the software masked fill, which in M7 (sat_lowres,
               slave off) runs entirely on the master = the MP fps collapse.  The hard flush guard
               (vdp1_things_flush stops at wall_cap - THING_FLUSH_MARGIN) still bounds a mispredict, and
               the nearest-first rank keeps the monsters you are fighting crisp on VDP1. */
            int nv = sat_local_players;                  /* sat_split_active is true here */
            if (nv < 1) nv = 1; else if (nv > 4) nv = 4;
            int room = vdp1_wall_cap - vdp1_wnext - THING_FLUSH_MARGIN - vdp1_wpn_reserve;
            int budget_cap = (room > 0) ? (room / (2 * nv)) : 0;   /* per-view slots (~2 cmds/thing) */
            if (budget_cap > THING_ADAPT_MAX) budget_cap = THING_ADAPT_MAX;
            if (sat_thing_emit_cap < budget_cap)      sat_thing_emit_cap += 2;         /* smooth ramp up */
            else if (sat_thing_emit_cap > budget_cap) sat_thing_emit_cap = budget_cap; /* snap down to fit */
            if (sat_thing_emit_cap < 0) sat_thing_emit_cap = 0;
        }
        /* (VDP1 thing FILL budget removed 2026-07-25: fill is not the limiter -- sat_thing_fill_budget
           stays 0 (core default = off).  The count budget above (WBUDGET, command-slot-driven) is what
           actually bounds VDP1 things.) */
        /* World sprites to VDP1 prio 7, AFTER the walls and BEFORE weapon (2) so the gun stays on
           top.  Occlusion = FUNC_UserClip; fuzz/translated/oversize/over-budget stay software.
           1p: emit directly (vissprites still live).  Split: the views already emitted per view
           into the queue (R_RenderViewPass) -- drain it here, after the walls. */
        /* SATURN: bracket the WORLD-THINGS emit (row 20 `e c n`).  Measured, not assumed: this call
           IS the kick's tail (row-2 `T` == `e` in 7/7 captures) and the tail is essentially all of
           `P` -- 32..159 ms on the worst frame, bimodal at a FIXED viewpoint.  Two explanations have
           already died here: the VDP1 bake (SPR `fb` = 1-2 per frame -- things SHARE sprite lumps,
           so the pigeonhole argument from THINGS_TEX_SLOTS was wrong) and the Z_Malloc rover (`z`
           read 0, i.e. < 1000 steps/frame).  What is left is the DISC: pass 2 does one
           W_CacheLumpNum(..., PU_CACHE) per eligible sprite, and PU_CACHE means the zone may purge
           that patch the moment anything else allocates -- the flat treadmill, one lump class over.
           Row-0 `ld` climbing ~16/s while the flat cache is pinned (FLT `ld` flat, `f0`) is what put
           the disc back on trial.  So bracket the CD clock across the same call: `c` = ms inside GFS
           commands, `n` = how many.  w_cd_ms10 (core w_wad.c) is updated SYNCHRONOUSLY inside
           sat_cd_load_raw, so the delta is exactly this call's disc time -- which is also why the
           load budget can spend that same clock per frame (core r_segs.c R_LoadBudgetLeft). */
        { unsigned short g0 = frt_read();
        if (sat_split_active) vdp1_things_flush();
        else                  R_EmitWorldThingsVDP1();
        sat_p_thg10 = (unsigned short)(frt_read() - g0) * 10u / 224u; }
#endif
#if SAT_WPN_VDP1
        sat_emit_weapon();          /* LAST world command -- above the walls AND above the monsters */
#endif
        vdp1_hud_emit();            /* (3) status bar ON TOP of the weapon (1p; prev frame's capture) */
        vdp1_hud_msg_emit();        /* (4) HU message glyphs, crisp over the view (lowres only) */
        vdp1_wpn_kick();
        sat_present_frt += (unsigned short)(frt_read() - pk0);
    }
    vdp1_kicked_this_frame = 1;
}
#endif

/* SATURN clear-on-slave (docs/BLIT_DMA_PLAN.md Inc3, pad R+C).  The end-of-frame framebuffer clear
   (index-0 wipe of the 3D view, df_post) is a ~60-70 KB HWRAM memset -- pure master ms.  When
   sat_clear_slave is on it is dispatched to the 2nd SH-2 (measured 80-99% idle) via RP_AuxDispatch; it
   writes HWRAM (not the B-bus), so the SCU-DMA hang law that killed the async BLIT does NOT apply.  The
   clear overlaps the game tic + next-frame REC and is joined before any fb write: the core joins at
   R_RenderPlayerView entry (level frames), DG_DrawFrame joins at the top (transition frames) before the
   blit reads fb.  The blit already cache_purges, so the master reads current RAM whoever cleared. */
static volatile unsigned int g_clear_bytes = 0;
static int clear_slave_pending = 0;
static void dg_fb_clear_slave(void)
{
    memset(framebuffer, 0, g_clear_bytes);
}

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

    /* clear-on-slave backstop: join a slave fb clear left pending by the previous frame's df_post
       before this frame's blit reads fb (level frames already joined it at R_RenderPlayerView; this
       catches menu/intermission/transition frames). */
    if (clear_slave_pending) { RP_AuxWait(); clear_slave_pending = 0; }

    uint32_t df0 = DG_GetTicksMs();   /* SATURN PERF: DG_DrawFrame ms split (entry) */

    /* SATURN sky -> VDP2: (re)upload on level/episode change; position the layer.
       SKY_FIXED keeps it static; otherwise scroll by viewangle (90deg = 256 sky
       px via SKY_ANGLESHIFT, slowed by SKY_PARALLAX_SHIFT; VDP2 wraps the plane). */
#if VDP2_HW_SKY
    if (skytexture > 0 && skytexture != sky_loaded_tex)
    {
        if (sky_retry_wait > 0) sky_retry_wait--;   /* stubbed last time: back off, then re-attempt */
        else                    sky_upload();
    }
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
    {
        if (sky_retry_wait > 0) sky_retry_wait--;   /* stubbed last time: back off, then re-attempt */
        else                    sky_cell_upload();  /* re-pack the sky into B1 cells on level/episode change */
    }
#if VDP2_SPLIT_HW_SKY
    /* Part 5 (docs/RBG0_SKY_SPLIT_ANALYSIS.md §5): in a co-op split, elect ONE view to receive the HW
       NBG0 sky (windowed to its band); the other views keep their software sky.  Static election = P1
       (view 0) -- couples with P1's HW floor in 2p; dynamic election (sat_sky_px_view[] + hysteresis,
       SKY_ELECT_HYST) is the documented next step, coverage already captured by d_main.  The core reads
       sat_sky_view in the NEXT frame's split loop (leaving that view's sky region index-0); the window
       + scroll below aim the single NBG0 layer at the SAME view THIS frame -- steady-state the choice is
       constant, so there is no phase skew.  Re-poke the W0 window only when the elected band changes. */
    int hwsky_split = (hwsky_split_on && sat_local_players >= 2 && gamestate == GS_LEVEL && !automapactive
                       && !sat_lowres);   /* M7: the NBG0 sky is un-zoomed full-320 while the NBG1 view is
                                             x2-zoomed -> horizontal misalignment; fall back to the software
                                             sky, which packs into fb[0,160) + zooms WITH the view. */
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
        /* SATURN: gate RBG0 DISPLAY on being in a level (like show_sky).  Outside a level the
           intermission/finale/title are 320x200 opaque assets; the framebuffer's bottom rows
           (200..223, ex-HUD) get memset to index 0 = VDP2 TRANSPARENT below -- if RBG0 stayed
           on it would show the LAST level's floor through that strip ("intermission floor band").
           Keep it on during the pause menu (still GS_LEVEL) so the frozen frame's floor persists. */
        int rbg0_on       = (rbg0_active || rbg0_split_p1) && (gamestate == GS_LEVEL);
        { static int prev_split_p1 = -1;                        /* SATURN: force ONE flat re-upload on the 1p<->split */
          if (rbg0_split_p1 != prev_split_p1) {                 /* transition (the stale-pic root cause is fixed in core */
              rbg0_tex_dirty = 1; prev_split_p1 = rbg0_split_p1; } }  /* r_plane.c: sat_dom_last_sec dangled across level loads) */
        sat_split_p1hw    = rbg0_split_p1;                       /* core (d_main): punch the HW floor only for P1 in split */
        rbg0_floor_win_xend = rbg0_split_p1 ? 159 : 319;         /* window X: P1's left half in split, full screen in 1p */
        sat_vdp2_floor    = (rbg0_mode == 1 || !rbg0_on) ? 0 : 1;  /* 1p drives the punch; split is overridden per-view in d_main */
        /* CELL FLOOR uses B1 as a whole rotation bank (pattern-name MAP, RDBS=0x8D) -> it CANNOT share
           B1 with the HW cell sky (NBG0) NOR the NBG3 debug overlay.  Force BOTH off whenever the cell
           floor is live so B1 = map alone (else B1 contention = the snow the first two HW tests hit).
           The sky region falls back to backdrop for now; a real SOFTWARE sky is Stage 3. */
        int cell_floor_live = (rbg0_kind == RBG0_KIND_CELL) && (rbg0_mode == 0) && rbg0_on;
        uint16_t sky_bit  = (!cell_floor_live && sat_vdp2_sky && show_sky) ? NBG0ON : 0;   /* HW sky bit (M-owned); OFF in cell floor -> B1 free */
        uint16_t rbg0_bit = (RBG0_DISPLAY && rbg0_mode == 0 && rbg0_on) ? RBG0ON : 0;   /* HW floor: pot0 + (1p or 2p-split-P1) */
        uint16_t nbg3_bit = (RBG0_NBG3 && nbg3_show && !cell_floor_live) ? NBG3ON : 0;  /* NBG3 overlay (B1); OFF in cell floor -> B1 free */
        uint16_t nbg1_bit = (cell_floor_live && rbg0_cell_nofb) ? 0 : NBG1ON;           /* R+Left test: drop the framebuffer to prove it's the NG cause */
        if (cell_floor_live) {
            /* CELL: refresh the RBG0 rotation cycle every frame with slScrAutoDisp(RBG0ON) ALONE -> clean
               textured floor (HW-proven; skipping it made the texture buggy, and RBG0ON alone keeps the
               map on B1, unlike a combined/NBG1 call which resets it to B0 -> black).  Re-assert NBG1's
               display by editing the BGON bits directly (never slScrAutoDisp(NBG1ON)).  One call/frame =
               acceptable speed (two were slow).  B0's framebuffer cycle stays pinned from commit_cyc. */
            slScrAutoDisp((uint16_t)RBG0ON);
            if (nbg1_bit) {
                volatile uint16_t *bgon = (volatile uint16_t *)((uintptr_t)&VDP2_RAMCTL - 0x0E + 0x20);
                *bgon = (uint16_t)((*bgon & ~(uint16_t)(NBG0ON | NBG3ON)) | (uint16_t)(NBG1ON | RBG0ON));
            }
        } else {
            slScrAutoDisp((uint16_t)(sky_bit | nbg1_bit | nbg3_bit | rbg0_bit));
        }
#if VDP2_CELL_SKY && VDP2_SKY_FORCE_CYC
        sky_cell_force_cyc(sky_bit, nbg3_bit);   /* RBG0-on makes the allocator put NBG0's char in A1; force it back to B1 */
#endif
#else
        slScrAutoDisp((uint16_t)(show_sky ? (NBG0ON | NBG1ON | NBG3ON)
                                          : (NBG1ON | NBG3ON)));
#endif
        /* Scrub the 1p STATUS-BAR rows' index-0 -> near-black so the HW sky doesn't show
           through the direct-palette bar.  1-PLAYER ONLY: in 3/4p rows 192..207 are the
           BOTTOM of players 3&4's 3D view (their band is at 208..223), and scrubbing the
           VDP1 walls' index-0 gaps there to near-black leaves a black block below those
           views (the "doesn't reach the HUD" bug -- a 1p-max-draw-zone leftover). */
        if (show_sky && sat_local_players <= 1)
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
        if (rbg0_kind != rbg0_kind_want || rbg0_reinit_force) { rbg0_reinit_force = 0; rbg0_reinit(); }  /* R+Up kind / R+Down coeff A/B */
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
              else
              {
                  /* NO FLOOR PUNCHED THIS FRAME -- and the formula is NOT a safe substitute.
                     `ft` is the sentinel whenever no visplane matched the latched (height,flat,band)
                     dominant triple, which happens routinely INSIDE a sector: the triple is re-picked
                     only on a sector change (core r_plane.c ~1311), so turning to face a floor with a
                     different flat or a different light band punches nothing.  The old code then
                     jumped the horizon to the player-height formula, which can sit well BELOW the
                     scene's real floor top -- and the sky map is rebuilt from it, painting NBG0 sky
                     cells down over rows the floor occupies.  It costs nothing on that frame (nothing
                     is transparent), but the sky map is VRAM and the software picture commits at the
                     blit, so on the transition frame the OLD map meets the NEW picture and the
                     floor's top rows show sky.  That is the owner's symptom, and it is a LOGIC bug,
                     not a calibration one: with no floor in view there is nothing to uncover, so the
                     right move is to HOLD the last known-good horizon.  hz == sky_horizon_row also
                     makes the rebuild test below false, so this is a true no-op frame.
                     ⚠ MEASURED AND EXONERATED 2026-08-02: the owner captured the artifact plainly
                     visible with this counting ZERO for the whole window, so it is NOT the cause.
                     The hold is kept because it is right on its own merits, not as a fix. */
                  if (sky_horizon_row > 0) hz = sky_horizon_row;
              }
              /* (The SKY-OFF probe that lived here -- horizon forced to row 0, no sky cells at all,
                 W1 fully open -- did its job and was removed.  Its result is the reason this file
                 stops chasing the RBG0 side: with the sky gone the artifact went with it, so the SKY
                 BOUNDARY is the limiter and RBG0 does reach those rows.  The rotation calibration
                 (slDispCenterR / slSetScreenDist) is NOT the target here.) */
          }
          /* re-run sky_cell_build_map (sky boundary + floor window) when the horizon cell OR the window X
             extent changes (the latter on a 1p<->split toggle) so the window tracks P1's half. */
          if ((hz >> 3) != (sky_horizon_row >> 3) || rbg0_floor_win_xend != last_xend)
          {
              last_xend = rbg0_floor_win_xend; sky_horizon_row = hz;
              if (sky_mode == 0) sky_cell_build_map();      /* legacy: map + window, both here */
              else
              {
                  /* Split the commit: the WINDOW is registers (latched at vblank) so it belongs
                     here, before the fence; the MAP is VRAM read during display, so it must land
                     with the picture -> deferred to just after the fence.  See sky_mode. */
                  sky_map_pending = 1;
#if RBG0_FLOOR_WINDOW
                  rbg0_floor_window_apply(sky_horizon_row & ~7);
#endif
              }
          } }
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
        /* dest = the LIVE RPT base (B1+0x1ff00 bitmap / A0+0x1ff00 cell); RB (unused until dual-param)
           rides +0x68 relative so it never strays into B1's map when the RPT is in A0.
           DEFERRED by default (rbg0_rpt_late): the copy now happens right after the NBG1 blit so the
           hardware floor and the software picture adopt the new view angle on the SAME field -- see
           rbg0_rpt_to_vram.  slScrMatSet has already written the RAM buffer here and nothing touches
           it before the blit, so postponing only the VRAM write is safe. */
        if (rbg0_rpt_late) rbg0_rpt_pending = 1;
        else               rbg0_rpt_to_vram();
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
    /* SATURN M7 FREEZE ROOT-CAUSE FIX (2026-07-17, HW): rewind the SGL slave work-area pointers
       (GBR+72 write / GBR+68 read) on EVERY frame, not only no-render frames.  On render frames the
       reset normally comes from the slave PLANE DISPATCH (rp_restart & co, r_parallel.c) -- but in
       M7/lowres the planes render MASTER-INLINE (r_plane.c !detailshift gate sends non-potato planes
       off the slave worklist), so NO plane dispatch fires and NOTHING rewinds the pointer.  Meanwhile
       vdp1_kicked_this_frame==1 (walls kicked) skips the no-render branch below too -> in M7 the
       pointer is NEVER reset.  The un-gated RBG0 block above still calls slSetScreenDist each frame
       (+8B GBR+72 bump, LIBSGL sglC23.o) -> ~280 B/s creep -> overruns the GBR+20 vblank user-callback
       after ~1-2 min -> _BlankIn jsr to garbage = TOTAL FREEZE (everywhere, even standing still; the
       W72 overlay row shows the live d+ creep).  rp_sgl_workptr_reset() joins any pending aux first
       and restores to the post-init base, so this is idempotent with the M4 dispatch reset and safe
       on the (idle) M7 slave.  [[m7-lowres-fill-bound-not-34p]] */
    rp_sgl_workptr_reset();
    if (!vdp1_kicked_this_frame) {
        /* NO-RENDER frame (menu/intermission/automap): also kick the empty bank so no stale walls
           stay rooted (the reset above already rewound the SGL work pointers for this path). */
        unsigned short pk0 = frt_read();   /* SATURN PERF: present-kick ms (menu/intermission path) */
        sat_vdp1_wpn_begin(); vdp1_wpn_kick();
        sat_present_frt += (unsigned short)(frt_read() - pk0);
    }
    /* (The end-of-frame vdp1_ftex_flush fallback went with the VDP1 floor deport 2026-08-02: the
       root flip it used to perform now happens in vdp1_wpn_kick, on every path.) */
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
    /* Menus/title/finale are 320x200 assets; on the 224 framebuffer rows 200..223 are
       uncovered.  Outside a level (no ST_Drawer), blacken that strip so it's not stale garbage.
       EXCEPT the intermission: WI_drawMetaBand (core wi_stuff.c) fills those rows itself with the
       grain-extended art + the meta line, so leave them alone there. */
    if (gamestate != GS_LEVEL && gamestate != GS_INTERMISSION)
        memset(framebuffer + 200 * 320, 0, 24 * 320);
    {
        /* SATURN split-screen HUD, painted into the framebuffer before the blit.  Gate on
           the SAME predicate d_main uses to split-render (a real co-op game, in a level,
           not the full-screen automap) -- else a stale sat_local_players on the demo/attract
           loop, or an open automap, would get a split HUD painted over a view d_main did NOT
           split-render.  usergame is externed as int elsewhere in this file (matches core). */
        extern int sat_local_players, usergame;
        /* W5 FIX (2026-07-15): track the split player-count at FRAME level (updates even in 1p, so it
           sits OUTSIDE the players>1 gate) -> a return to 2p after ANY excursion (1p or 3-4p) forces
           the panel repaint below.  The 2p panel is W5-signature-gated, but its framebuffer rows
           [160,224) get CLOBBERED whenever we leave 2p (1p status bar / 3-4p bands overwrite them),
           so a return with an unchanged HUD signature would else skip the repaint = stale/blank
           panels.  (Pre-existing bug, surfaced by the count cycle; unrelated to lowres.) */
        static int hud_split_last_lp = -1;
        int hud_lp_changed = (sat_local_players != hud_split_last_lp);
        hud_split_last_lp = sat_local_players;
        /* SATURN 3/4p floor+ceil default = FLAT ("fxxf").  In 3+ player split RBG0 is OFF (floors are
           software) AND -- if running M7 -- the LD half-rate path is a NO-OP (R_TexturedSpan is
           high-detail only, r_plane.c:810), so an "l" floor silently costs FULL.  Solid-colour
           floors/ceilings drop that; walls + sprites stay FULL.  2p keeps LD (its P1 floor is
           RBG0-HW).  Applied ONCE per count transition -> a live pad SQ override (Y floor / L+Y ceil)
           in 3/4p sticks.  Dial back to fll* / full live via those pads. */
        if (hud_lp_changed)
        {
            int flat34 = (sat_local_players >= 3) ? SQ_FLAT : SQ_LD;
            for (int k = 0; k < 4; k++)
            {
                sq_floor_view[k] = flat34;
                sq_ceil_view[k]  = flat34;
            }
        }
        /* !menuactive: the platform paints the split HUD band AFTER the core drew the pause
           menu into the framebuffer, so painting it here would cover the menu.  Skip the
           repaint while the menu is up -> the menu (NBG1) stays on top of the band. */
        if (sat_local_players > 1 && usergame && gamestate == GS_LEVEL && !automapactive && !menuactive)
        {
            if (sat_local_players == 2)
            {
                /* 2p: two 160x64 compact-HUD panels in the bottom 64 rows (P1 left, P2 right),
                   each player's widgets on top, then a per-half damage/pickup flash.  W5 (runtime
                   blit_cfg[].w5): only repaint (and mark the band dirty) when a player's HUD
                   signature changed OR the count just changed -- else skip the paint AND the blit
                   skips [160,224).  w5=0: always repaint (the flag then also always blits, below). */
                static unsigned int w5_2p_sig = ~0u;
                int w5_2p = 1;   /* W5 permanently ON (blit baked c5) */
                unsigned int sig = ST_SplitHudSig();
                if (!w5_2p || sig != w5_2p_sig || hud_lp_changed)
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
                    ST_DrawQuadHud(q, qx[q], oy);                /* widgets: red hu_font numbers, keys */
                }
                hud4p_apply_flash(n);
            }
        }
    }
    /* VDP1 HUD (1p): ST_Drawer / HU_Drawer already composed this frame's elements (D_Display runs
       them before I_FinishUpdate).  Snapshot them now for NEXT frame's kick to draw crisp over the
       NBG1 zoom (1-frame latent).  Status bar = every mode; HU message = lowres only (the core
       redirect draws directly into a VDP1 VRAM slot, double-buffered here for the NEXT frame). */
    {
        extern int sat_local_players, usergame;
        /* Under the M7 x2 zoom the software split HUD garbles -> capture it for a crisp prio-7 VDP1
           sprite.  The split predicates MUST mirror the split-HUD PAINT gate (usergame && GS_LEVEL &&
           !automap && !menu) -- NOT just sat_lr -- else on the attract/demo loop with a leftover co-op
           count the paint is skipped (d_main renders the demo 1p) but the capture would snapshot the
           demo VIEW rows as garbage HUD strips.  1p bar keeps its historical GS_LEVEL-only gate. */
        int lvl_ok   = (usergame && gamestate == GS_LEVEL && !menuactive && !automapactive);
        int hud1p    = (sat_local_players == 1 && gamestate == GS_LEVEL);
        int hud2p_lr = (sat_local_players == 2 && sat_lowres && lvl_ok);
        int hud34_lr = (sat_local_players >= 3 && sat_lowres && lvl_ok);
        vdp1_hud_begin();
        if (hud1p)         vdp1_hud_add(HUD_Y, HUD_H, 1);         /* 1p bar    rows 192..223 (320x32) opaque */
        else if (hud2p_lr) vdp1_hud_add(HUD2P_TOP, HUD2P_H, 1);   /* 2p panels rows 160..223 (320x64) opaque */
        else if (hud34_lr) {                                      /* 3/4p bands: 2 strips, SPD-ON (opaque) */
            /* SPD-ON (not off): the band art is all-opaque, and in 3p the empty q3/minimap-slot area of
               the bottom strip is index-0 -> with SPD-ON it paints CRAM bank-1 idx0 (near-black) OVER
               the x2-zoomed right-half of P3's band that NBG1 shows there = a clean corner (SPD-off left
               that garble visible).  In 4p the slot holds an opaque band so SPD is a no-op.  Also closes
               any stray index-0 pixel in the band art punching a see-through hole. */
            vdp1_hud_add(96,  HUD4P_H, 1);                        /* top    strip rows  96..111 (q0/q1 bands)   */
            vdp1_hud_add(208, HUD4P_H, 1);                        /* bottom strip rows 208..223 (q2[/q3] bands) */
        }
        vdp1_hud_ready = (vdp1_hud_nreg > 0);
        if (hud1p && sat_lowres) {
            /* The core drew this frame's message into the armed VRAM slot (sat_hu_msg_buf); latch it
               for the next kick's emit.  Then arm+clear the OTHER slot for frame N+1 (double-buffer)
               so the slot being plotted is never the one being redrawn -> tear-free. */
            unsigned char *drawn   = sat_hu_msg_buf;
            vdp1_hud_msg_emit_addr = (unsigned int)(uintptr_t)drawn;
            vdp1_hud_msg_active    = (drawn != 0) && sat_hu_msg_drawn;
            unsigned char *next    = (unsigned char *)(uintptr_t)
                ((drawn == (unsigned char *)VDP1_HUD_MSG_TEX0) ? VDP1_HUD_MSG_TEX1 : VDP1_HUD_MSG_TEX0);
            memset(next, 0, 320 * HU_MSG_H); /* clear the next write slot (VRAM, write-through) */
            sat_hu_msg_buf = next;           /* arm the core redirect for frame N+1's HU_Drawer */
        } else {
            sat_hu_msg_buf = 0;              /* non-lowres/split/non-level: message draws to the fb */
            vdp1_hud_msg_active = 0;
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
    /* SATURN FIELD LOCK (owner's diagnosis 2026-08-02: "un probleme de synchronisation d'affichage
       entre vdp1 et cpu ... toutes les frames ne sont pas erronees, on dirait qu'on a le probleme a
       une certaine frequence, alors que je tourne de facon continue").

       The two layers commit on DIFFERENT clocks.  VDP1 boots in 1-CYCLE AUTO (VDP1_FBCR = 0, PTMR =
       plot-at-frame-change, ~5959): it replots its list and swaps its two framebuffers on EVERY
       VBLANK -- 60 Hz, nothing to do with us.  The software picture commits when this blit runs --
       once per GAME frame, and the frame is not a whole number of fields (the owner's capture: 56.0
       fps / MST 17 => ~17.9 ms against a 16.68 ms field).  The ~1.2 ms of drift per frame walks a
       full field every ~14 frames, so the vblank that makes the new walls visible lands sometimes
       before and sometimes after the blit: ~4 mismatched frames per second, at a steady cadence, out
       of a perfectly steady turn.  That is a BEAT, and it is why five geometric corrections in a row
       failed -- on the good frames they over-correct and on the bad ones they under-correct.

       Nothing is misplaced.  The two clocks just have to stop sliding: fence the blit to a vblank
       edge and the whole loop phase-locks -- frame period becomes a whole number of fields, the
       kick/plot/swap/blit offsets stop drifting, and the mismatch becomes CONSTANT instead of
       periodic (a constant one we can then place, which the beat made impossible).

       The price is fps quantisation to the NEXT WHOLE FIELD COUNT -- not to 30: a 17.9 ms frame
       becomes 2 fields (30 fps, -46%), 40 ms becomes 3 (20 fps, -17%), a 77 ms M7 frame becomes 5
       (12 fps, -8%).  The cost collapses as the frame gets slower; 56 fps hurts precisely because it
       sits just past ONE field.  Failure mode: a frame landing ON a boundary (~50 ms = 3.00 fields)
       -- jitter then flips the count 3/4 and the beat returns as judder.  sat_field_n reports the
       count on row 13 so that is READ, not guessed; it must be steady.
       Deliberately NOT the parked vdp1_couple_nbg1 brick (~7456): that one waits on EDSR.CEF, which
       Ymir never models, so it would burn VDP1_COUPLE_MAX_VBL vblanks a frame there and lock
       nothing.  This needs no CEF and behaves the same on Ymir and on hardware.
       Pad R+Right (1p), row 13 `Fl<mode>/<fields>`.  Default 0 = the free-running loop.
       Called from just before the blit copy, NOT here -- see the call site for why. */
    uint32_t df1 = DG_GetTicksMs();        /* SATURN PERF: ms split -- end of pre, start of blit */
    {
    /* JOIN any aux slave job BEFORE the NBG1 mask blit -- the blit reads the framebuffer the job may
       still be writing.  Near-free when nothing is pending.  (Its original purpose, joining the slave
       ftex F-build so the coherent pair flipped before the mask, went with the VDP1 floor deport on
       2026-08-02, and the R+Z blit/F-build OVERLAP branch with it.) */
    RP_AuxWait();
#if VDP2_RBG0_TEST && RBG0_RPT_TRANSFER == 2
    /* RBG0 RPT, mode 2 -- copy it HERE, in the field BEFORE the blit, so the vblank the fence is
       about to wait for latches it and the hardware floor goes live in the same field as the
       software picture.  VDP2 reads the rotation parameter table during vblank (which is exactly
       why SGL does this DMA from _BlankIn), so a copy made after that vblank -- mode 1, after the
       blit -- misses the field it belongs to.  See the declaration for the full derivation. */
    if (rbg0_rpt_late == 2 && rbg0_rpt_pending)
    {
        uint32_t rp_t0 = DG_GetTicksMs();
        rbg0_rpt_to_vram();
        rbg_rpt_sum += DG_GetTicksMs() - rp_t0;
        rbg0_rpt_pending = 0;
    }
#endif
    /* FIELD LOCK fence -- see the long note at its declaration.  It sits HERE, immediately before
       the copy, not up at the end of the render: the NBG1 framebuffer is SINGLE-buffered, so the
       blit is visible as it is written and its phase against the beam decides whether it tears.
       Started at the vblank edge the copy runs ~2.5x faster than the beam (224 lines in ~5.5 ms
       against ~13.5 ms of active display) and stays ahead of it the whole way = no tear.  Fencing
       further up let RP_AuxWait and the VDP2 setup run first, so the copy actually started several
       ms INTO the field and crossed the beam -- the owner's "j'ai l'impression de constater plus de
       déchirures".  The loop still phase-locks either way; only the tearing moved. */
    /* SATURN MANUAL PRESENT (unconditional since 2026-08-19): the swap-grant fence ends right
       after the vblank the VDP1 swap executes at the end of, which IS the Fl1 edge for the
       blit below.  Runs on menu/intermission frames too (the empty-bank kick arms pending
       there; without a grant the last level frame's walls would stay displayed).  It subsumes
       the parked field-lock fence (sat_field_fence): every frame is edge-locked by construction. */
    sat_mp_fence();
#if VDP2_CELL_SKY
    /* SKY MAP, deferred half (sky_mode >= 1).  HERE, at the top of the field the blit is about to
       paint: the map is VRAM and VDP2 reads it during display, so writing it any earlier would show
       the new sky boundary against the PREVIOUS picture for up to a whole field -- the sky spilling
       down over floor rows that are still on screen.  ~8 KB of halfwords, finished long before the
       beam reaches the horizon rows. */
    if (sky_map_pending) { sky_cell_write_map(); sky_map_pending = 0; }
#endif
    unsigned short blit_t0 = frt_read();   /* SATURN PERF: time the blit (-> sat_blit_ms10) */
    /* SATURN lowres (docs/LOWRES_RENDER_STUDY.md): the software render is packed into the LEFT
       160 columns; VDP2 hardware-enlarges NBG1 x2 horizontal so it fills the screen.  Re-applied
       EVERY frame (a one-shot scale can be dropped by the SGL vblank register re-push).  M7-multi:
       1p AND 2p (2p packs P1 fb[0,80)+P2 fb[80,160) -> the SAME whole-layer zoom restores both to
       their screen halves); 3/4p + non-lowres -> 1:1.  If the picture SHRINKS to the left on HW
       instead of enlarging, the SGL convention is inverse -> set VDP2_ZOOM_FACTOR 0.5. */
    int sat_lr = (sat_lowres
                  && gamestate == GS_LEVEL && !menuactive && !automapactive);
                  /* menus/intermission/full-screen automap draw full-320 -> no zoom (else their
                     un-packed content would be x2-stretched into the left half; the 3D view is
                     not rendered while the automap is up, so dropping the zoom costs nothing) */
    slScrScaleNbg1(toFIXED(sat_lr ? VDP2_ZOOM_FACTOR : 1.0), toFIXED(1.0));
    /* W5: split the copy at hud_top (= the clear boundary).  [0,hud_top) is the re-rendered
       3D view -> always blit.  [hud_top,224) is the HUD band -> blit only when it changed
       (core sat_hud_dirty / 2p signature) or an overlay may have painted over it. */
    int hud_top = (sat_local_players >= 3) ? 224 : (sat_local_players == 2 ? 160 : 192);
    /* W5 is the runtime blit_cfg[].w5 axis (pad L+A).  Off -> blit all 224.  On -> blit the HUD
       band only when it changed, or an overlay/layout change may have painted over it. */
    static int w5_last_players = -1;
    static int w5_last_lowres  = -1;
    int hud_force = (sat_local_players != w5_last_players)
                 || (sat_lr != w5_last_lowres)   /* lowres flip: HUD packing changed -> re-blit */
                 || menuactive || (gamestate != GS_LEVEL);   /* overlays / layout change */
    w5_last_players = sat_local_players;
    w5_last_lowres  = sat_lr;
    int hud_blit = sat_hud_dirty || hud_force;   /* W5 permanently ON (blit baked c5) */
    {   /* blit baked to c5 (CPU memcpy + W5) 2026-07-16; the opt-in slDMACopy DMA path was
           HW-dead (B-bus write-bandwidth-bound, no win) -> removed.  docs/TOGGLE_AUDIT.md. */
        /* Single-CPU blit: master copies the picture.  W5: 3D-view
           rows always, HUD band only when changed. */
        cache_purge();
        if (sat_lr)
        {
            /* LOWRES: the 3D view is packed in the LEFT 160 columns -> copy 160 B/row (half the
               blit).  The HUD is drawn full-320, so 2:1 DECIMATE it into the left 160 cols (read
               every other source byte) so the x2 zoom restores it -- chunky (crisp when the VDP1
               HUD sprite covers it).  1p/2p: the HUD is a contiguous tail [hud_top,224).  3/4p: the
               compact bands are INTERLEAVED (rows 96..111 top quads + 208..223 bottom quads) inside
               the [0,224) view range, so decimate THOSE rows too -- else the un-decimated band rides
               the packed 160B copy and the x2 zoom DOUBLES q0/q2's band across the full width (the
               "software HUD doubled in x" seen on HW whenever the crisp VDP1 band is dropped under a
               full VDP1 command budget in 4p).  Framebuffer stays intact (decimate only reads it). */
            int is34 = (sat_local_players >= 3);
            for (int y = 0; y < hud_top; ++y)
            {
                unsigned char       *d = DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE;
                const unsigned char *s = framebuffer + y * 320;
                if (is34 && (((unsigned)(y - 96) < 16u) || ((unsigned)(y - 208) < 16u)))
                    for (int x = 0; x < 160; ++x) d[x] = s[x << 1];   /* 3/4p band row -> decimate */
                else
                    memcpy(d, s, 160);                                /* packed view row */
            }
            if (hud_blit)
                for (int y = hud_top; y < 224; ++y)
                {
                    unsigned char       *d = DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE;
                    const unsigned char *s = framebuffer + y * 320;
                    for (int x = 0; x < 160; ++x) d[x] = s[x << 1];
                }
        }
        else
        {
            for (int y = 0; y < hud_top; ++y)
                memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
            if (hud_blit)
                for (int y = hud_top; y < 224; ++y)
                    memcpy(DOOM_VRAM + (y + VIEW_Y_OFFSET) * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
        }
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
    /* RBG0 RPT, mode 1 -- after the blit.  Kept as the A/B reference and as the catch-all for any
       frame that never reached the mode-2 site (gamestate != GS_LEVEL, fence skipped): mode 2
       already cleared `pending`, so this is a no-op there. */
#if VDP2_RBG0_TEST && RBG0_RPT_TRANSFER == 2
    if (rbg0_rpt_pending)
    {
        uint32_t rp_t0 = DG_GetTicksMs();
        rbg0_rpt_to_vram();
        rbg_rpt_sum += DG_GetTicksMs() - rp_t0;
        rbg0_rpt_pending = 0;
    }
#endif
    uint32_t df2 = DG_GetTicksMs();        /* SATURN PERF: ms split -- end of blit, start of clear */
    /* LAYER INVERSION: clear the 3D VIEW to index 0 so next frame the SKIPPED wall columns stay
       transparent -> the VDP1 walls (below NBG1) show through.  The HUD rows are left intact
       (1p: status bar 192..223 owned by ST_Drawer; 2p: panels 160..223 owned by hud2p).  3/4p:
       clear all 224 -- the compact HUD bands + minimap are repainted opaque each frame before
       the blit, so clearing their rows here (they're interleaved per quadrant) is harmless. */
    {
        extern int sat_local_players;
        int clear_rows = (sat_local_players >= 3) ? 224 : (sat_local_players == 2 ? 160 : 192);
        extern int sat_lowres;
        /* SATURN M7 FREEZE ROOT-CAUSE FIX (2026-07-17, HW-confirmed via pad R+C): in M7/lowres the
           slave does NO plane-split (planes render master-inline at detailshift=1, r_plane.c gate), so
           the slave is idle during render and the clear-on-slave aux dispatch/join handshake is no
           longer covered by the plane-split's TAS sync -- it corrupts the r_bsp .bss (ds_p stomped ->
           ds=1.4e9 -> HARD FREEZE on real HW, Ymir-clean; toggling sat_clear_slave OFF via R+C launched
           the level).  Clear on the MASTER in lowres (M4/M6 keep the slave-clear win); the ~2-3ms cost
           is on the packed M7 frame only.  [[clear-slave-nearsprites-aimd-shipped]] listed this risk. */
        /* SATURN 2026-08-08 -- `!sat_lowres` RESTORED.  It was removed on 2026-07-30 on the premise
           quoted below ("the plane-split now runs in M7, which restores exactly the TAS-sync
           coverage").  That premise is FALSE IN PRACTICE: row 5 reads **Pb0%** in every capture of
           2026-08-08, i.e. the plane-split contributes nothing to the plane phase, so the sync it
           was supposed to provide is not there -- and the exact failure the July guard was written
           for came back.  Owner-observed: after a few seconds of a heavy scene the picture stops
           being cleared, with a visible left/right boundary, and the game is unplayable.
           Measured on his captures: R245 against Bw2.7+Bp20.5+P62.1+M48.7 = 134, so **111 ms per
           frame in NO render phase at all**, with `to9` (>=9 slave-dispatch timeouts per second)
           and `T40`/`dg32` -- the GAME TIC, which renders nothing, inflated too.  A uniform stall,
           not a rendering cost.  Confirmed by the owner toggling R+C: `cs0` makes it disappear.
           Cost of clearing on the master here is ~2-3 ms against a 111 ms stall.
           ⚠ Do not lift this again on the strength of "the plane-split covers it" without reading
           `Pb` on row 5 first -- that is the number that decides whether the coverage is real.
           2026-08-08, SAME DAY: RE-LIFTED, because the guard was a MASK, not the fix.  Turning the
           clear off only removed one more consumer that was waiting on an ALREADY-DEAD slave; the
           owner found the real cause -- only the LEFT half of the CPU sprites drawn (the slave owns
           [half, viewwidth), r_things.c ~1952) with `SLV id100%` FROZEN instead of oscillating at
           98%.  The kill is the masked-phase allocation race, closed in r_things.c by
           sat_masked_inflight.  Kept as a live comment, not as code: if the stall ever comes back,
           `!sat_lowres` here is a one-line workaround that buys a playable build while you look. */
        if (sat_clear_slave && gamestate == GS_LEVEL && sat_local_players <= 1) {
            /* SATURN M7 2026-07-30: the `!sat_lowres` hard-off is GONE -- the plane-split now runs in M7
               (r_plane.c), which restores exactly the TAS-sync coverage whose ABSENCE (slave-idle M7, no
               plane-split) was blamed for the ds_p .bss stomp above.  HW-validated as the default. */
            /* SATURN 2026-07-20 (freeze fix, cont.): ALSO require single-player.  The same New-Game-into-
               coop race that now skips the plane-split (r_plane.c) leaves the slave IDLE during a
               sat_lowres=0 split frame -- so a clear-slave dispatch here would hit the very r_bsp .bss
               corruption described above (idle-slave clear, no plane-split TAS sync to cover it).  MP
               clears on the master too (split is master-only in the parked world regardless of count). */
            /* offload to the idle slave; a render (R_RenderPlayerView) is guaranteed next frame to
               join it.  Non-GS_LEVEL frames clear on the master (no render would join a slave clear
               before a menu/intermission redraws fb -> gate keeps those on the master). */
            g_clear_bytes = (unsigned int)clear_rows * 320u;
            RP_AuxDispatch(dg_fb_clear_slave);
            clear_slave_pending = 1;
        } else {
            memset(framebuffer, 0, clear_rows * 320);
        }
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
            int cur_sq = (sq_wall << 6) | (sq_sprite << 4) | (sq_floor << 2) | sq_ceil;
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
    /* 🔴 SATURN 2026-08-17 -- THE SLOW-MOTION WAS HERE, AND IT WAS A LATCH.
       The old guard read `if (result > safe_ms + 5000) return safe_ms + (one field per new fv)`.
       That is a one-way trap: the first time anything makes the true clock jump more than 5 s --
       a level load, a long CD read, one stomped us_acc -- safe_ms falls 5 s behind and the test
       `result > safe_ms + 5000` is then TRUE FOREVER, because nothing ever resynchronises it.
       From that moment the clock advances only ~one field per CALL that happens to see a new
       frt_at_vbl, i.e. ~3 fields per frame however long the frame really was.
       That is exactly what the overlay says.  `b` (tics NetUpdate wanted) is pinned at 1,1-1,6
       whatever the frame costs -- 82 % of correct at 26 fps (v2,3) but 23 % at 5 fps (v12,0) --
       i.e. a CONSTANT ~31-46 ms of clock per frame, which is 2-3 fields.  The game world ran at a
       fifth of real time and no counter downstream could see why, because `v` proved the vblank
       handler itself was healthy.
       THE REAL GUARD IS A RATE LIMIT, NOT A COMPARISON.  vbl_count says exactly how many fields
       have really elapsed since the last call, so the clock can be bounded by that -- a rogue
       us_acc is still clamped, but the clock KEEPS TRUE TIME instead of freezing, and it converges
       the moment the corruption stops.  Monotonic too, so it can never run backwards. */
    static uint32_t     safe_ms   = 0;
    static unsigned int last_vbl  = 0;
    static int          primed    = 0;
    unsigned long long us_snap;
    unsigned short fv, f;
    unsigned int sr, sr_masked, vc;
    uint32_t result, maxstep, A;

    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    sr_masked = sr | 0xF0;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr_masked) : "memory");
    us_snap = us_acc;
    fv      = frt_at_vbl;
    vc      = vbl_count;
    f       = (unsigned short)(((unsigned short)FRT_FRCH << 8) | (unsigned short)FRT_FRCL);
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");

    result = (uint32_t)((us_snap + ((unsigned short)(f - fv) * ns_per_frt) / 1000) / 1000);

    if (!primed) { primed = 1; safe_ms = result; last_vbl = vc; return result; }

    /* Fields really elapsed since the last call, +1 for the partial field the FRT delta adds. */
    A       = us_per_frame / 1000U;
    maxstep = ((uint32_t)(vc - last_vbl) + 1u) * A + 1u;
    if (result > safe_ms + maxstep) result = safe_ms + maxstep;   /* clamp, never freeze */
    if (result < safe_ms)           result = safe_ms;             /* monotonic */

    safe_ms  = result;
    last_vbl = vc;
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

    /* M7-multi: re-apply the render mode whenever a live drop-in (START on pad 2 -> sat_dropin_want
       -> G_SatDropInSetPlayers) changes sat_local_players WITHOUT a pad tap.  Since want_lr = (M ==
       M7_LOWRES) is now COUNT-INDEPENDENT, the R_SetLowRes flip inside is a guarded no-op on a pure
       count change (M7 stays lowres across 1p/2p/3-4p) -- this call is belt-and-suspenders that keeps
       the rest of sat_apply_mode's per-mode state coherent; the R_SetViewWindow satvw cache re-fits
       the views automatically on the viewport-height change.  Runs before the pad-connect early-out. */
    {
        static int last_lp = -1;
        if (sat_local_players != last_lp)
        {
            last_lp = sat_local_players;
            sat_apply_mode();
            /* SATURN 2026-07-15 (HW-measured ~4% on a dense 4p Bp scene, 191->183 SPL): mark-suppress
               DEFAULT-ON in 3/4p ONLY, OFF in 1p AND 2p.  Restricted to 3/4p because that is where RBG0
               is FULLY off (2p still drives the P1-half HW floor, whose re-election reads the visplane
               coverage counter mark-suppress perturbs -> keep 2p at the safe off default until A/B'd).
               Re-asserted on each count change; L+B still overrides within a count for A/B. */
            sat_mark_suppress = (sat_local_players >= 3) ? 1 : 0;
        }
    }

    /* SATURN M7 PAUSE FIX (2026-07-19): the lowres render packs the 3D view into the LEFT 160 cols and
       a VDP2 x2 zoom stretches it back -- but the zoom is dropped whenever a menu is up (so full-320
       menu text is not stretched, DG_DrawFrame ~line 6811).  Zoom off + still-PACKED view = the view's
       SOFTWARE layers (ceiling + walls) fill only the left half while the RBG0 floor (HW, full-screen)
       stays => the reported "ceiling gone on the right when paused".  The 3D view IS re-rendered every
       paused frame (core d_main.c:351 has no menuactive gate), so we just render it FULL-RES while a
       menu is up over a 1p level -> the frozen view AND the menu are both full-320 and correct.
       CONTINUOUS-ENFORCE but call R_SetLowRes ONLY on a real mismatch: no per-frame recompute (the
       ~74ms R_ExecuteSetViewSize is one-time at the open/close transition), and anything that flips
       sat_lowres mid-pause self-corrects in one frame.  Split (players>1) is left untouched -- its
       packed multi-view layout depends on lowres.  R_SetLowRes only sets sat_lowres + setsizeneeded
       (the recompute runs before the next render), so it is safe to call from here. */
    {
        int mode_lowres    = (sat_m == M7_LOWRES);              /* sat_lowres's value during normal play */
        int pause_over_lvl = menuactive && gamestate == GS_LEVEL && !automapactive
                             && sat_local_players <= 1;
        int want_lowres    = pause_over_lvl ? 0 : mode_lowres;
        if (sat_lowres != want_lowres) R_SetLowRes(want_lowres);
    }

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
        if (!(cur & PER_DGT_TL) && (cur & PER_DGT_TR))   /* L+Z (R released): cycle the VDP1 ISOLATION mode */
        {
            sat_iso_mode = (sat_iso_mode + 1) % 3;
            sat_apply_iso();                             /* all / walls-only / walls+things / walls+weapon / flat */
        }
        /* Pad R+Z (R held, L released): live A/B of the RESIDENT FLAT POOL (core/r_flatcache.c).
           ON = visplanes read their flat from the contiguous LRU slab (a flat the player keeps
           looking at is read from the disc ONCE); OFF = the classic W_CacheLumpNum/PU_CACHE path,
           i.e. the treadmill.  The slab stays CARVED in both states, so the two sides of the A/B
           have a byte-identical memory layout -- which is the only way this measurement is honest
           ([[interbuild-perf-noise]]: ~600 B of .bss shift is worth +-6 ms of Bp).  Watch row-2 `P`
           and row 19 `FLT ld`.  R+Z was free: the R+Z present-couple A/B lives under
           VDP1_MANUAL_CHANGE (0 = parked), and R+Z otherwise fell through to the Z-alone mode
           cycle, which is a no-op now that only M7 is in it ([[parked-single-mode-m7-baseline]]). */
        else if (!(cur & PER_DGT_TR) && (cur & PER_DGT_TL))
            sat_flatcache_on = !sat_flatcache_on;   /* declared extern "C" at file scope */
        else
        {   /* Z (no modifier): cycle only the LIVE playable modes {M7}; M0+M5 are parked (off the cycle). */
            int ci = 0;
            for (int i = 0; i < SAT_M_CYCLE_N; ++i) if (sat_m_cycle[i] == sat_m) { ci = i; break; }
            sat_m = sat_m_cycle[(ci + 1) % SAT_M_CYCLE_N];
            sat_apply_mode();
        }
    }

#if VDP2_RBG0_TEST
    /* (Pad R+Up RBG0 floor-kind toggle REMOVED 2026-08-03 at the owner's request, with the wall
       modes.  The 4bpp cell floor itself is untouched and still on pause -- rbg0_kind_want just has
       no runtime writer now, so the build ships whatever RBG0_KIND_* it is initialised with.) */
    /* (Pad R+Down cell-coeff A/B + R+Left drop-framebuffer A/B CUT 2026-07-16: both probes answered
       -- 4bpp cells are snow-free with K_ON perspective AND the framebuffer ON.  Chords freed, and
       the rbg0_reinit_force gray-flash they triggered is gone.  docs/TOGGLE_AUDIT.md.) */
#endif

    /* (Pad L+A blit A/B ring CUT 2026-07-07: W5 HUD-skip is a real idle win -> now permanently ON
       (blit_mode fixed = c5); the slDMACopy paths were HW-dead.  docs/BLIT_DMA_PLAN.md.) */

    /* Pad L+A now toggles the R2 CD persistent-handle read path (sat_cd_persistent, w_file_saturn.cxx)
       for a live HW A/B of streaming fluidity: ON = GFS_Seek+Fread on the open handle (rides the CD
       read-ahead), OFF = LoadBytes per read (a full GFS_Load).  Edge-triggered on A while L held (L
       taps ',' and A taps fire -- harmless; toggle at a level boundary, not mid-combat).  Only
       meaningful in CD mode; inert (but harmless) in cart mode.  Row 12 shows p<0/1> fb<n>. */
    if (!(cur & PER_DGT_TL) && (changed & PER_DGT_TA) && !(cur & PER_DGT_TA))
    {
        extern int sat_cd_persistent;
        sat_cd_persistent = !sat_cd_persistent;
    }

    /* SATURN 2026-07-09 -- three perf-lever live A/B toggles, one HW session.  Letter+modifier chords
       (the established pattern; the incidental Doom tap is harmless).  The d-pad is deliberately NOT
       used (L/R + d-pad would eat movement in normal play).  R+C = clear-on-slave (R+C was the freed M5
       staging slot); R+X = the texture LOAD BUDGET; L+X = paint every VDP1 wall a flat quad.  X's TAB
       (automap) is suppressed below whenever L or R is held, and the mp split-VDP1 X toggle is gated
       to X-alone, so these never collide.
       ⚠ 2026-08-10, four dead field names removed from this sentence: it used to say "R+X =
       nearSprites cull ... Row 7 shows cs/ns/ad; row 15 SPR shows the AIMD ec/ef effect.  L+X now
       CYCLES the AIMD mode ad0/1/2".  `ns` was cut from row 7 (08-09) and its chord had already been
       reclaimed; `ad` was never in the row-7 format; `ec`/`ef` were cut from row 15 (08-09); and the
       L+X AIMD cycle was cut on 2026-07-16 (see :7488).  Read instead: row 7 `cs`, row 17 `ec`. */
    if (!(cur & PER_DGT_TR) && (cur & PER_DGT_TL)                 /* R held, L released */
        && (changed & PER_DGT_TC) && !(cur & PER_DGT_TC))
        sat_clear_slave ^= 1;
    /* Pad L+C (L held, R released, 1p only): cycle the CUMULATIVE perf-lever level 0->1->2->3->4->0
       (core sat_opt, defined + fully documented in core/r_segs.c).
         0 = all off (the 2026-07-29 reference)   1 = +L1 span fill (r_plane.c)
         2 = +L2 clip-scan hoist (r_segs.c)       3 = +L3 SAT_VROWS reciprocal hoist (r_segs.c)
         4 = +L4 wall subdivision cap 6->3        5 = +L5 near-wall CPU-borders/VDP1-core split
       Levels 1-3 are byte-identical to 0, so any fps delta they show is pure cost, not quality;
       4 and 5 change pixels.  L5 targets the ~22ms nose-to-wall Bp: watch row-2 `Bp` AND row-8 `e`
       (tiers actually split -- `e0` means it never engaged, so a flat Bp says nothing).
       Default 5.  This chord REPLACES the removed sat_m7_slave level cycle (the M7 slave stack is
       now hard-wired ON, HW-validated).  Same posture rule as before: C is the RUN button and "both
       shoulders released" is the default play stance, so the chord needs a deliberate strafe-hold and
       cannot fire from neutral.  1p-gated: in split, L+C stays the hwsky toggle (inert in 1p).
       Row 7 shows /o<lvl>.  Compare row-1 `R` and row-2 `Bw Bp P M` between levels -- L1 must move P,
       L2/L3 must move Bp, L4 moves Bp + row-8 `VD1 w%`. */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)   /* L held, R released, 1p */
        && (changed & PER_DGT_TC) && !(cur & PER_DGT_TC))
        sat_opt = (sat_opt + 1) % 6;                                    /* 0->1->2->3->4->5->0 */
    /* (Pad R+X DEPORT-PREVIEW CUT 2026-08-02 with M5 itself: it dry-ran M5's convex-exact classifier
       to size the offload, and four staircase HW captures settled that offload negative.  It was also
       the ONLY live consumer of the ~9KB SAT_FLOOR_TEX machinery, so it kept the whole dead path
       compiled.  R+X is free.  See docs/M7_FEATURE_AUDIT.md.) */
    /* (L+X AIMD ad0/1/2 toggle CUT 2026-07-16: wbudget baked as the 1p default -- the damped ad1 was
       HW-proven to collapse thing-emit to 0 via the false-latching EDSR-CEF; ad0/ad1-1p are dead.
       See [[aimd-wbudget-hw-cef-collapse]].) */
    /* (Pad L+X WEAPON-off-VDP1 CUT 2026-08-02.  It was the monster-blink lever -- route the ~18k-px
       weapon to software so a plot-time overrun stops eating the things.  Three independent reasons
       to drop it: its ON state left the walls un-re-cleared (a pre-existing VDP1 transition glitch,
       reproducible with L+X alone, HW-confirmed); the weapon-FILL lever measured inert on HW when
       the distance-prioritized fill budget was tested (2026-07-25, docs/VDP1_LIMITS_SOURCED.md); and
       sat_apply_iso re-zeroed sat_wpn_soft on every SQ chord, so any A/B silently reset mid-run.
       core still owns the flag at 0.  L+X is free.) */
    /* (Pad L+Left/Right 1p VDP1 fill-budget A/B REMOVED 2026-07-25: fill is not the flicker limiter --
       see docs/VDP1_LIMITS_SOURCED.md.  L+Left/Right are free again in 1p; the VDP1 isolation cycle is
       on L+Z.  The split levers below still take L+Left/Right at players>1.) */
    /* Pad R + X (1p): PER-FRAME TEXTURE LOAD BUDGET -- off / 1 / 2 / 4 textures faulted in per
       frame.  In the CD-streaming build a wall texture that is not resident costs a SYNCHRONOUS
       ~42 ms disc read inside R_GetColumn, charged to `Bp`, for a wall that may be three screen
       columns wide: that is the entire 480..790 ms `Bp` frames in the owner's TNT MAP11 captures.
       Past the budget the tier draws FLAT (sat_dc_solid skips R_GetColumn -> no composite, no
       patch, NO DISC) and textures itself over the following frames as the budget refills.
       No distance test: the BSP walk is front-to-back, so the budget is spent on the NEAREST
       walls by construction.  0 = off = every texture faults on sight (the old behaviour).
       The chord cycles 10 -> 20 -> 40 -> 0 -> 10 MILLISECONDS of disc per frame (it was a count
       of reads, 0/1/2/4, until 2026-08-07); the DEFAULT is 20, so the gate is armed at boot.
       Read `lb<budget>:<wall>/<plane>/<sprite>.<nocol>` on row 18.  (R+X was documented for sat_near_sprites but
       NEVER bound -- verified on PER_DGT_TX, the only sites are L+X and split-X.) */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TR) && (cur & PER_DGT_TL)
        && (changed & PER_DGT_TX) && !(cur & PER_DGT_TX))
        sat_tex_load_budget = (sat_tex_load_budget == 10) ? 20
                            : (sat_tex_load_budget == 20) ? 40
                            : (sat_tex_load_budget == 40) ? 0  : 10;
    /* Pad R + Down: TEST cheat cycle -- off -> GOD -> GOD+NOCLIP -> off, applied to every local
       player and re-established each tic by core P_Ticker (survives level/map warp, deaths, the
       E1M8 super-damage floor).  R held, L released, Down edge (free chord -- R+Down was the cut
       plane-Z knob); the incidental back-step tap to Doom is harmless.  A HUD line confirms state. */
    if (!(cur & PER_DGT_TR) && (cur & PER_DGT_TL)                 /* R held, L released */
        && (changed & PER_DGT_KD) && !(cur & PER_DGT_KD))
        SAT_CycleCheat();
    /* SATURN split perf levers (co-op split ONLY -- gated on sat_local_players>1 so 1p is untouched).
       L + d-pad (L+Left/Right freed when WALL_PX_BUDGET was cut).  The incidental one-tap strafe/turn
       is harmless (tune while standing).  L+Left = piste-3 split thing-cull (drop tiny-projected
       sprites per view); L+Right = piste-5 rotating SQ-balance cycle (0 off / 1 = 1 view degraded per
       frame / 2 = 2 views).  Row 17 (SPL, split-only) shows tc<0/1> bal<0/1/2>. */
    if (sat_local_players > 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)   /* L held, R released */
        && (changed & PER_DGT_KL) && !(cur & PER_DGT_KL))
    { extern int sat_split_thingcull; sat_split_thingcull ^= 1; }
    if (sat_local_players > 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)   /* L held, R released */
        && (changed & PER_DGT_KR) && !(cur & PER_DGT_KR))
        sat_split_balance = (sat_split_balance + 1) % 3;
    /* 1p: the same two chords (free here -- the split levers above are gated on >1 player) dial the
       WALL ENTRY COVERAGE live: CPU frames drawn over a wall that just came into view, 0..3.  This
       replaces the removed yaw-anticipation gain on the same chord -- same symptom, opposite model.
       0 = off = the pre-2026-08-02 behaviour (a wall entering the view shows sky for one frame);
       1 = the default.  Raise it only if a hole survives at 1: each extra frame is software columns
       on every wall that just appeared, so watch row-2 `Bp` while you do.  Row 13 shows `En<n>`. */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_KL) && !(cur & PER_DGT_KL))
    { if (sat_wall_entry > 0) sat_wall_entry--; }
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_KR) && !(cur & PER_DGT_KR))
    { if (sat_wall_entry < 3) sat_wall_entry++; }
    /* Pad L+Up (L held, R released, 1p): cycle the VDP1 WALL GROW 0 -> 1 -> 2 -> 0 (sat_wall_grow) --
       the owner's counter-proposal to growing the software plane: grow the LATE layer instead of the
       one that is on time.  It is the better place in principle (one grown quad covers its own gap
       on every side at once, instead of every neighbouring plane having to reach out for it).
       ⚠ It costs exactly what commit 66e590c removed: DISTORSP maps the WHOLE character corner to
       corner, so moving the vertices without changing the character stretches `rows` texels over
       `span + 2*grow` rows -- an error of grow*rows/span TEXELS at the band edges, worst on FAR
       walls (~6 texels at span 20 for grow 1), repeating down the wall.  That IS the software-vs-VDP1
       misalignment he had me fix.  Judge Wg1 on the gap first; if it closes it, the clean form is a
       PADDED character (bake texw+2 x rows+2 with the edge texels duplicated and grow the quad to
       match) -- exact mapping AND a 1px halo, at ~+6% tile VRAM.  Row 13 shows `Wg<n>`. */
    /* Pad L+X (L held, R released, 1p; freed when M5 was cut): WALL PATH PAINT 0 -> 1 -> 2 -> 3.
       1 = every VDP1 wall a flat GREEN quad, 2 = every CPU wall flat RED, 3 = both.  Answers the
       owner's question directly -- "certains murs disparaissent en avant/arriere, potentiellement
       sur un passage vdp1 cpu mais c'est incertain": with both on, a wall changing path CHANGES
       COLOUR on the exact frame it happens, and a wall drawn by neither is a hole with no texture
       left to hide it.
       ⚠ bit1 first drove sat_potato_walls FROM HERE and the owner got no red: sat_apply_mode /
       sat_apply_sq own that flag and re-derive it from sq_wall, so the write was clobbered.  The
       paint now ORs itself into `wall_solid` (core r_segs) directly -- no shared flag to fight over.
       ⚠ AND the solid COLUMN it selects must live in r_draw.c (sat_dc_solid), not in r_parallel's
       executors: those never run in the shipping config (rp_disabled).  That second miss is why the
       owner reported "pas de cpu rouge" TWICE. */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_TX) && !(cur & PER_DGT_TX))
        sat_wall_paint = (sat_wall_paint + 1) & 3;
    /* Pad L+Down (L held, R released, 1p): SKY/FLOOR BOUNDARY MODE 0 -> 1 -> 2 (see sky_mode for
       what each one is; 1 is the shipped fix, 0 is the A/B reference, 2 adds the 8px lift).
       sky_horizon_row = -1 forces the rebuild test true on the next frame so the change takes
       effect at once (sky_cell_build_map otherwise only runs on an 8px horizon crossing). */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_KD) && !(cur & PER_DGT_KD))
    {
        sky_mode = (sky_mode + 1) % 3;
#if VDP2_CELL_SKY
        sky_horizon_row = -1;   /* force the rebuild test true next frame so the change lands at once */
#endif
    }
    if (sat_local_players <= 1 && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_KU) && !(cur & PER_DGT_KU))
        sat_wall_grow = (sat_wall_grow + 1) % 5;   /* 0..4: 3 and 4 exist to TEST whether the
                                                      residual slip is simply a gap wider than the
                                                      grow on a double-tic frame -- see row-13 `t` */
    /* (Low-res is no longer a pad toggle -- it is render MODE M7 in the pad-Z cycle, since it is
       whole-view (shared projection + whole-layer VDP2 zoom), not a per-zone lever.  sat_apply_mode
       sets sat_lowres when M7 is selected.  This freed the old L+R+X binding.) */

    /* (Pad R+Up/Down vertical decrochage-fill + R+Left/Right border-cap knobs CUT 2026-07-07 --
       tuning finished, values baked: sat_plane_vscale=4 (r_main.c), sat_plane_border_max=10
       (dg_saturn.cxx:~2738).  R+Up/Down are taken since; R+Left is the band knob below.) */
    /* Pad R+Left (R held, L released, 1p): RBG0 RPT TIMING 0 -> 1 -> 2 -> 0.  The owner's
       *"entre le sol vdp2 et un autre sol cpu"* junction: 1 = the copy after the blit (one field
       LATE -- VDP2 latched the table at the vblank the blit started on), 2 = DEFAULT, the copy
       before the fence so the same vblank latches it.  0 = the original early timing.  Judge it on
       the hardware-floor / software-floor seam while walking straight -- no walls involved, which
       is what makes it the clean test.  Row 13 `Rp<n>`.  (This chord held the software-plane-grow
       knob, removed 2026-08-02: growing the on-time layer to chase the late one was the wrong end
       of the problem.) */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TR) && (cur & PER_DGT_TL)
        && (changed & PER_DGT_KL) && !(cur & PER_DGT_KL))
        rbg0_rpt_late = (rbg0_rpt_late + 1) % 3;
    /* Pad R+Up (R held, L released, 1p): PATH DWELL 0 -> 4 -> 8 (row 13 `En<entry>/<dwell>`).
       Frames a seg that just changed CPU<->VDP1 path stays covered by the CPU -- bounds the flip
       RATE where the hysteresis only widened the threshold.  Pins to the CPU only, never to VDP1:
       forcing VDP1 could hit a tier with no VDP1 claim and leave it drawn by nobody.  Costs
       software columns -> watch row-2 `Bp`.  0 = off = the 2-frame exit overlap alone.
       (Freed when the RBG0 floor-kind chord was removed 2026-08-03.  THIS BLOCK WAS MISSING from
       the first dwell commit -- the variable and the readout shipped, the chord did not, so R+Up
       did nothing and the owner read it as a failed idea.) */
    if (sat_local_players <= 1 && !(cur & PER_DGT_TR) && (cur & PER_DGT_TL)
        && (changed & PER_DGT_KU) && !(cur & PER_DGT_KU))
        sat_wall_dwell = (sat_wall_dwell == 0) ? 4 : (sat_wall_dwell == 4) ? 8 : 0;
    /* (Pad R+Right -- the LEAD-FILL cycle -- REMOVED 2026-08-19 with the lead-fill PARK: the
       manual present eliminated the stale-pair offset the lead-fill repainted, so its boot
       default is now 0 (core r_segs.c) and no chord re-arms it.  Revive for an A/B by setting
       sat_wall_lead_x > 0 -- the whole core mechanism is intact, only dormant.  Historical ⚠
       kept: X once shared R+A with sat_wall_clamp's chord; chord audits must match on
       PER_DGT_T*, not on the button letter.  The R+Right slot is FREE.) */
    /* Pad L+Left (L held, R released, 1p): live A/B of the COMPOSITE PIN (core/r_data.c).
       ON = the last composites stay non-purgeable under a 64 KB budget; OFF = the classic PU_CACHE
       demote, i.e. the treadmill `cb20/6` measured (SIX textures rebuilt 20x a second, `k33` each).
       No slab and no contiguous run is involved, so unlike the 1p pool A/B both sides have an
       identical layout by construction.  Watch fps, `k` (row 20), `cb` (row 18) and `pn` (row 22):
       a CLIMBING yield count means the pin is fighting the zone and buying nothing. */
    if (!(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_KL) && !(cur & PER_DGT_KL)
        && sat_local_players <= 1)
    {
        /* 🔴 RETARGETED 2026-08-17 -- this chord now A/Bs the PATCH-LUMP pin, which is the one that
           can hold: the composite pin it used to toggle is defaulted OFF and has never held a byte
           (`pn0/1`..`pn0/25`, its 48 KB floor against a 24-38 KB `lg`), while the lump pin's floor
           clears the flatten's 13 KB worst lump with headroom.  Row 16 `P<kb>/<yields>` is the
           readout; row 20 `c` is what must collapse.  The composite pin keeps its variable and its
           flush so reviving it stays a one-line change. */
        /* 2026-08-17 (4th HW run): a THREE-way cycle now -- 2 = 128 KB (default), 1 = 64 KB, 0 = off.
           The console proved 64 KB never yields (`P57..61/0` for 64 s) AND never covers the ~12 fat
           calls a frame, so the open question is the BUDGET, not on-vs-off.  Flush on every step, not
           just on 0: keeping a 128 KB ring while claiming the 64 KB rung would make the A/B lie. */
        sat_lpin_on = (sat_lpin_on + 2) % 3;   /* 2 -> 1 -> 0 -> 2 */
        R_LumpPinFlush();
    }

    /* SATURN 2026-08-15: the governor's PLANE axis is decided in core (r_parallel.c) but applied
       here, because sat_apply_mode() and the SQ knobs live on this side.  One re-apply per actual
       change, never per frame: sat_apply_mode touches the whole quality block and calling it 60x a
       second to write the same values would be exactly the kind of pointless work this session has
       been removing. */
    if (sat_gov_p_dirty) { sat_gov_p_dirty = 0; sat_apply_mode(); }

    /* (Pad L+B -- the MANUAL PRESENT A/B -- REMOVED 2026-08-19, one day after it was added:
       the owner's Ymir captures validated v2 (20-24 fps vs 18.4-19.4 auto+leadfill, `w0`,
       fence 13-15 ms, holes gone), so the manual present is now unconditional (sat_mp_*),
       the AUTO revert left with the toggle, and the lead-fill is parked.  The L+B slot is
       FREE.) */

    /* (Pad L+Right — the FAR-DEGRADATION LADDER — REMOVED 2026-08-16, one session after it was
       added: rung 1 rejected on sight, rungs 2 and 3 killed by their own counters. */

    /* (R+C M5 BSP-staging A/B cut -- settled-negative; the staging mechanism stays inert in core.
       R+C is now the CEILING SQ knob below; C alone still cycles the plane-split pmode.) */

#if VDP2_RBG0_TEST
    /* Pad R + Y cycles the WALL software quality SQ (full/ld/band/flat), applied to the CPU wall
       fallback + the VDP1 wall style (band/flat).  (Was the RBG0 floor A/B -- folded into the M
       axis.)  R taps '.' to Doom -- the established diag-chord pattern. */
    if (!(cur & PER_DGT_TR) && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    {
        if (sat_local_players > 1)   /* split: cycle the split WALL SQ (FULL->BAND->FLAT; NO LD -- detailshift is whole-frame) for all views */
        { int w = (sq_wall_view[0]==SQ_FULL) ? SQ_BAND : (sq_wall_view[0]==SQ_BAND) ? SQ_FLAT : SQ_FULL;
          for (int k=0;k<4;k++) sq_wall_view[k] = w; }
        else { sq_wall = (sq_wall + 1) & 3; sat_apply_mode(); }
    }
    /* (Pad C 3-way plane-split cycle CUT 2026-07-16: TAS is the shipped winner, static + row-split
       were HW-proven losers -> RP_DrawPlanesSplit is now unconditional TAS.  C-alone chord freed.
       docs/TOGGLE_AUDIT.md.) */
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
    /* (Pad L+Up/Down HW-sky horizon nudge CUT 2026-07-07 -- tuning finished, baked into
       SKY_HORIZON_ROW=96 (dg_saturn.cxx:~361).  L+Up/Down are free.  sky_cell_build_map() is
       still driven live by the horizon auto-track at ~5669.) */
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
    /* Pad Y (alone, L/R released) cycles the FLOOR software quality SQ, applied to the software
       floor spans.  Flats cycle full/ld/flat -- band is
       skipped (meaningless for a flat: the potato span is already distance-shaded per row).
       SATURN 2026-07-19: gated on !menuactive -- Y is ALSO Doom's 'y' confirm (line 7017), so the
       "start a new game? (press y)" prompt was firing this toggle too (floor->FLAT = the reported
       fxff-on-new-game).  A menu is up -> Y is a confirm, not an SQ toggle. */
    if (!menuactive && (cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
    {
        if (sat_local_players > 1)   /* split: cycle the split FLOOR SQ (skips LD when M7/lowdetail) for all views */
        { int f = sq_plane_cycle(sq_floor_view[0]);
          for (int k=0;k<4;k++) sq_floor_view[k] = f; }
        else { sq_floor = sq_plane_cycle(sq_floor); sat_apply_mode(); }
    }
    /* Pad L+Y (R released) cycles the CEILING software quality SQ (full/ld/band/flat), independent
       of the floor (core sat_ceil_potato/sat_ceil_ld).  (Was the slave-F-build A/B -- sl1 kept as
       the compile default.)  Active-low: !(cur&TL) = L held. */
    if (!menuactive && !(cur & PER_DGT_TL) && (cur & PER_DGT_TR)
        && (changed & PER_DGT_TY) && !(cur & PER_DGT_TY))   /* !menuactive: Y is also Doom's confirm (see floor above) */
    {
        if (sat_local_players > 1)   /* split: cycle the split CEILING SQ (skips LD when M7/lowdetail) for all views */
        { int c = sq_plane_cycle(sq_ceil_view[0]);
          for (int k=0;k<4;k++) sq_ceil_view[k] = c; }
        else { sq_ceil = sq_plane_cycle(sq_ceil); sat_apply_mode(); }
    }
    /* Pad R+A: live A/B of the Phase-1 WALL CLAMP (partially-occluded tiers kept on VDP1 via the
       world-anchored cut + software wedge, core sat_wall_cut_floor/_ceil).  Row 6 W<n><+/-> = tiers
       kept + state.  (MOVED off L+R+Y -- the L+R chord fires the overlay toggle, so L+R+Y was
       unreachable.)  R held + A; the incidental fire tap to Doom is harmless. */
    if (!(cur & PER_DGT_TR) && (changed & PER_DGT_TA) && !(cur & PER_DGT_TA))
        sat_wall_clamp ^= 1;
    /* Pad R+B: the 4th SQ zone -- SPRITE software quality (full<->ld), INDEPENDENT of the wall/floor/
       ceil detailshift (core sat_sprite_ld).  ld halves the software sprite fill (split/M0/M6); the
       VDP1 world-things (1p M4) ignore it.  R held + B; the incidental USE tap to Doom is harmless
       (mirrors the R+A wall-clamp chord).  Row 7 SQ 4th char. */
    if (!(cur & PER_DGT_TR) && (changed & PER_DGT_TB) && !(cur & PER_DGT_TB))
    { sq_sprite = (sq_sprite == SQ_FULL) ? SQ_LD : SQ_FULL; sat_apply_mode(); }
    /* 🔴 CHORD COLLISION, found + removed 2026-08-16.  This block used to toggle sat_mark_suppress
       on **exactly** the chord the LOD ladder above uses (L held, R released, B edge -- byte-for-byte
       the same predicate), so every single L+B press cycled the LOD rung AND flipped RBG0
       mark-suppress underneath it.  The two are not independent -- mark-suppress changes how the
       dominant floor SPLITS, i.e. it moves Bp and the visplane count, the very numbers the LOD rung
       was being judged on.  Every LOD capture taken since the ladder shipped carries that
       contamination, alternating on each press.
       The pad is saturated, so rather than invent a chord for it, sat_mark_suppress keeps its
       DEFAULT (0) and loses its binding: it has never actually had a clean A/B, so nothing measured
       is being given up.  ([[interbuild-perf-noise]] is about builds; this was the same disease
       inside one build.) */
    /* (The pad-Y floor PERF-SIM cycle and the visplane work-steal A/B that used to contend for Y
       behind #elif went with SAT_FLOOR_PERFSIM / the ftex cut on 2026-08-02.  Y is now
       unconditionally the SQ block above -- no compile-time contention left on this button.) */

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
    if (sat_local_players > 1 && (cur & PER_DGT_TL) && (cur & PER_DGT_TR)   /* X ALONE (no L/R -> those are the perf toggles) */
        && (changed & PER_DGT_TX) && !(cur & PER_DGT_TX))
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
            if (pad_map[i].mask == PER_DGT_TX
                && (sat_local_players > 1 || !(cur & PER_DGT_TL) || !(cur & PER_DGT_TR)))   /* also eat TAB when L/R held (perf toggles) */
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
