/*
** DoomSRL -- doomgeneric platform layer for the Sega Saturn (SRL build).
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
extern "C" int             sat_streaming_mode = 0;
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
#define VDP2_RBG0_TEST   0
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
#define VDP2_HW_SKY      1
#define RBG0_CEL_VRAM    ((void *)0x25E20000)  /* VDP2 VRAM A1: cell (char) data    */
/* A rotation BG's pattern-name map must live in a VRAM B-bank.  An A-bank (map in A0/A1,
   tried to keep the NBG3 debug text) gives slScrAutoDisp ok=0 -> RBG0 reads starve ->
   squished bands.  B0 = the game framebuffer (non-negotiable), so the map goes in B1 and
   EVICTS the NBG3 debug-text layer (cycle-pattern conflict) -- this is the reliable-floor
   config.  Bank layout (VDP2_HW_SKY=0): B0 framebuffer / A1 cells / B1 map / A0 K-table
   (A0 freed by the software sky).  Debug overlay is OFF while the floor is on. */
#define RBG0_MAP_VRAM    ((void *)0x25E70000)  /* VDP2 VRAM B1: pattern name table */
#if VDP2_HW_SKY
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
/* RBG0 RAMCTL-commit readback (direct chip write of the rotation bank-select RDBS; see
   rbg0_commit_ramctl).  Shown on overlay row 14 in pad-Y debug modes 1/2. */
static uint16_t ramctl_before = 0, ramctl_after = 0;
/* RBG0 plane geometry, tuned against the software floor 2026-06-18 (live X+d-pad tuning,
   since removed).  PITCH = +4.21deg off the 90deg ground tilt -> raises the plane's far end
   onto Doom's horizon; YAW = +90deg -> orients the flat to the world.  Texture scale came out
   1:1 (no slScale needed) once the pitch was right. */
#define RBG0_PITCH       0x300    /* ANGLE delta on slRotX (~4.21deg) */
#define RBG0_YAW_OFF     0x4000   /* ANGLE yaw offset (90deg)         */

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
extern "C" int            sat_potato_walls; /* core: solid-colour walls (opaque) */

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
extern void (*sat_wall_hook)(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                             int texnum, int u1, int u2, int v0, int v1,
                             const unsigned char *cmap);
void sat_wall_vdp1(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
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

/* Potato (solid-colour, flat-shaded) -- big EX/fillrate win, visible quality drop,
   aimed at perf-tight builds (e.g. future 2/4-player split-screen).  POTATO_LEVEL
   is the boot level; cycle live in-game with the pad Z button.  Levels:
   0 = off (textured), 1 = floors/ceilings flat, 2 = + VDP1 walls flat (low-detail).
   Level 2 is the DoomSRL-only "po2": the now-dead software wall-potato (walls live on
   VDP1) is replaced by drawing the VDP1 walls as flat mean-colour quads (wall_lowdetail).
   DoomJo (software walls) keeps the original sat_potato_walls path -- this is platform-side. */
#define POTATO_LEVEL 0
static int potato_level = POTATO_LEVEL;
static int wall_lowdetail = 0;            /* DoomSRL: VDP1 walls drawn flat (set at potato lvl 2) */

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
static int blit_mode = DUAL_CPU_BLIT ? 1 : 0;   /* boot: 50/50 if compiled in, else single */
/* Master row count for the current config: mpct% of 224. */
static inline int blit_split(void) { return (blit_cfg[blit_mode].mpct * 224) / 100; }
static void sat_apply_potato(void)
{
    sat_potato_floors = (potato_level >= 1);
    sat_potato_walls  = (potato_level >= 2);
    wall_lowdetail    = (potato_level >= 2);
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
    int len = wad.LoadBytes(0, (int)CART_RAM_SIZE, (void *)CART_RAM_UNCACHED);
    wad.Close();

    if (len <= 12)
    {
        printf("CD read failed (len=%d)\n", len);
        return 0;
    }
    cache_purge();
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
   (Always 'B' on real hardware -- VDP1 never finishes a wall list within one frame; the EDSR
   fill-calibration metric built on it was useless and has been removed.) */
static int vdp1_prev_done = 1;

#if SHOW_FPS
extern "C" int rp_timeout_count;
extern "C" unsigned int rp_master_ms;   /* master frame ms -> prefixes r_parallel.c's row-18 SLV line */
static unsigned int dg_frame_count = 0;
static int vdp1_last_cmds = 0;

static void fps_update(void)
{
    static unsigned int t0     = 0;
    static unsigned int frames = 0;
    unsigned int now = vbl_count;
    unsigned int hz  = (us_per_frame == 20000) ? 50 : 60;

    /* row 1: per-frame live counter; row 0 reserved for boot/fatal */
    {
        static char r1[45];
        sprintf(r1, "F%05u ph%d to%d vbl%u",
                dg_frame_count % 100000, game_phase,
                rp_timeout_count, (unsigned int)(vbl_count & 0xFFFF));
        SRL::Debug::Print(0, 1, r1);
    }
    frames++;
    if (now - t0 >= hz)
    {
        unsigned int elapsed = now - t0;
        unsigned int fps = (frames * hz + elapsed / 2) / elapsed;
        /* tenths of an fps, for resolution at 5-10 fps; EMA (~4s) for a stable
           average to compare builds with. */
        unsigned int inst10 = (frames * 10u * hz + elapsed / 2) / elapsed;
        static unsigned int avg10 = 0;
        avg10 = avg10 ? (avg10 * 3 + inst10) / 4 : inst10;
        static char r2[45];
        /* Trimmed: fps duplicated row 17's inst, and dma/dsta were dead with the
           SCU DMA blit disabled (USE_SCU_DMA=0).  Kept gt (heartbeat) + vp
           (visplane peak) -- vp is the number sky->VDP2 should pull down. */
        sprintf(r2, "gt%5d vp%3d pot%d bl%d %d/%d", gametic, r_visplane_peak,
                potato_level, blit_mode,
                blit_cfg[blit_mode].mpct, 100 - blit_cfg[blit_mode].mpct);
        SRL::Debug::Print(0, 2, r2);
        {
            static char rA[45];
            /* row 17: stable build-comparison number, with the build-identity
               stamp (b:__TIME__) folded in -- build.ps1 touches dg_saturn.cxx so
               __TIME__ refreshes every build, letting you confirm on hardware you
               flashed the latest.  (Was row 18; the boot IRQ probe it shared the
               row with answered 1.1 and was removed.) */
            sprintf(rA, "AVG %u.%u inst %u.%u b:" __TIME__,
                    avg10 / 10, avg10 % 10, inst10 / 10, inst10 % 10);
            SRL::Debug::Print(0, 17, rA);
        }
        /* row 15 freed: the standalone MST line was just a pointer to rows 19/20.  The
           master frame ms (the synchronous bottleneck) now prefixes the slave's row-18
           SLV line; set the shared value here, r_parallel.c prints it. */
        rp_master_ms = inst10 ? (10000u / inst10) : 0;   /* frame ms */
        {
            /* row 16: the AUXILIARY (co)processors only (master+slave = row 18, split r19/r20).
               VD1 = VDP1 commands this frame + D(one)/B(usy).  VD2 = VDP2 (always compositing,
               spare).  SCU = SCU-DSP/DMA (idle = free to offload).  68K = SCSP sound CPU. */
            static char r6[45];
            sprintf(r6, "VD1 %d%c VD2~ SCU%s 68Ksnd",
                    vdp1_last_cmds, vdp1_prev_done ? 'D' : 'B',
                    USE_SCU_DMA ? "dma" : "-");
            SRL::Debug::Print(0, 16, r6);
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
        {
            /* row 21: WAD identity detected from the IWAD's own lumps (free row,
               not overwritten elsewhere).  "(detecting...)" until
               D_SetGameDescription runs; then e.g. "DOOM Shareware",
               "The Ultimate DOOM", "DOOM 2: Hell on Earth".  This is the
               WAD-agnostic self-check: it must match the WAD you flashed. */
            static char rW[45];
            const char *gd = gamedescription ? gamedescription : "(detecting...)";
            sprintf(rW, "WAD: %.38s", gd);
            SRL::Debug::Print(0, 21, rW);
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
    slPushMatrix();
    {
        slRotX((ANGLE)(0x4000 + RBG0_PITCH));            /* 90 deg + baked pitch: plane far end at the horizon */
        slRotZ((ANGLE)(-(int)(viewangle >> 16) + RBG0_YAW_OFF)); /* yaw track + baked 90deg flat orientation */
        /* viewx/viewy are fixed_t (16.16) in map units; slTranslate's FIXED is also 16.16,
           so passing them directly scrolls the floor by the player's map position (1 unit ->
           1 texel for a 64-unit flat).  Z = eye height above the player's floor (viewz -
           floor height) so the plane sits EXACTLY on the floor the player stands on, and
           follows it up/down stairs.  Signs/scale tune with the real texture. */
        slTranslate(-viewx, -viewy, -(viewz - sat_vdp2_floor_h));
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
    if (picnum == loaded && cmap == loaded_cmap) return;
    const unsigned char *flat = sat_vdp2_floor_data();
    if (!flat) return;
    loaded = picnum;
    loaded_cmap = cmap;
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
}

static void rbg0_proto_init(void)
{
    /* 1) cells (64 cells x 64B = a 64x64 flat) are filled per-flat by rbg0_upload_flat()
       on the first level frame; zero them so the brief pre-first-flat frame isn't garbage. */
    memset((void *)RBG0_CEL_VRAM, 0, 64 * 64);

    /* 2) pattern-name table -- 1-WORD format (map word = char#*2 | palette<<12 + offset).
       A 64x64-cell page tiling the flat's 8x8 cell grid: cell (cx,cy) = index cy*8+cx ->
       char# = (cy*8+cx)*2, palette 1 (PLAYPAL, 0x1000), cells at the A1 bank base -> offset 0. */
    {
        unsigned short *map = (unsigned short *)RBG0_MAP_VRAM;
        for (int my = 0; my < 64; ++my)
            for (int mx = 0; mx < 64; ++mx)
            {
                int cellidx = (my & 7) * 8 + (mx & 7);
                map[my * 64 + mx] = (unsigned short)((cellidx * 2) | 0x1000);
            }
    }

    /* 3) RBG0 cell config.
       THE FIX: slPageRbg0's FIRST arg is the CELL/character base, NOT the map.  My old code
       passed (map, cell) swapped -> RBG0 read pixels from the pattern-name table = garbage =
       transparent.  Sequence: slPageRbg0(cell, 0, PNB_1WORD|CN_12BIT) + sl1MapRA(map).  PNB_1WORD
       (not 2WORD).  OneAxis flat for now (no K-table); perspective K-table is the next step. */
    slOverRA(0);                                       /* over-area: repeat the plane  */
    slCharRbg0(COL_TYPE_256, CHAR_SIZE_1x1);
    slPageRbg0(RBG0_CEL_VRAM, 0, PNB_1WORD | CN_12BIT);/* arg1 = CELL base (the fix!)   */
    slPlaneRA(PL_SIZE_1x1);
    sl1MapRA(RBG0_MAP_VRAM);                            /* pattern-name table (B1)      */
    /* PERSPECTIVE: a per-line coefficient table (1/z scaling)
       turns the affine plane into a Mode-7 GROUND.  Static table via slMakeKtable -- the
       vblank-filled variant needs slSynch, which we don't run.  K_LINE = one scale per
       scanline (enough for a floor; far cheaper than a per-dot K_DOT). */
    slMakeKtable(RBG0_KTAB_VRAM);
    slKtableRA(RBG0_KTAB_VRAM, K_FIX | K_LINE | K_2WORD | K_ON);
    slRparaMode(K_CHANGE);                              /* use the coefficient table     */
    slBMPaletteRbg0(1);

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
    uint16_t v = (uint16_t)((ramctl_before & 0xFC00u) | 0x0300u | 0x008Du);
    *RAMCTL = v;
    ramctl_after = *RAMCTL;
    printf("RAMCTL before=%04x after=%04x (rbg0 RDBS commit)\n",
           ramctl_before, ramctl_after);
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
    printf("DoomSRL platform init\n");
    printf("video: %s\n", (TVSTAT & 1) ? "PAL" : "NTSC");

    SRL::Debug::Print(0, 1, "INIT CD...");

    /* CD filesystem init (SRL wraps GFS) */
    SRL::Cd::Initialize();

    SRL::Debug::Print(0, 1, "INIT CART...");
    cart_enable();
    unsigned int cart_sz = cart_probe_size();
    {
        static char cid[45];
        unsigned char id = *CART_ID_ADDR;
        sprintf(cid, "CART id=0x%02x usable=%uKB", (unsigned int)id,
                cart_sz / 1024u);
        SRL::Debug::Print(0, 1, cid);
        printf("cart usable size: %u bytes (%u KB)\n", cart_sz, cart_sz / 1024u);
        unsigned int t = vbl_count;
        while (vbl_count - t < 180) ;   /* 3s pause so the size is readable */
    }
#if FORCE_CD_STREAM
    cart_sz = 0;   /* test override: ignore the cart, force CD streaming */
#endif
    /* The 3.94MB IWAD only fits a full 4MB cart.  A 1M/2M (or 1M-mode AR)
       cart cannot hold it -- stream from CD instead of loading a truncated,
       aliased WAD that renders as a black screen on hardware. */
    if (cart_sz >= 0x400000u)
    {
        SRL::Debug::Print(0, 1, "INIT WAD(cart 4MB)...");
        if (!load_wad())
            DG_Fatal("DOOM1.WAD load failed");
        {
            static char ws[45];
            sprintf(ws, "WAD OK sz=%u", sat_wad_size);
            SRL::Debug::Print(0, 1, ws);
            unsigned int t = vbl_count; while (vbl_count - t < 120) ;
        }
    }
    else
    {
        if (cart_sz)
            printf("cart only %uKB (<4MB) -- IWAD too big, CD streaming\n",
                   cart_sz / 1024u);
        else
            printf("No usable RAM cart -- CD streaming mode\n");
        sat_streaming_mode = 1;
        SRL::Debug::Print(0, 1, cart_sz ? "CART<4MB -> CD STREAM..."
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
    slScrAutoDisp(NBG1ON | NBG3ON | RBG0ON);            /* software sky -> NBG0 off; floor(RBG0) on */
#endif
#else
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON);
#endif

#if VDP2_RBG0_TEST
    /* Commit the RBG0 bank assignment (RDBS) straight to the chip -- the piece SGL would
       push inside slSynch.  After slScrAutoDisp so RBG0ON is already live; once is enough
       (the SGL vblank handler re-pushes BGON/scroll, not RAMCTL). */
    rbg0_commit_ramctl();
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

/* one-sided mid + two-sided upper/lower quads.  Must stay <= the command budget (WALL_CMD_CAP
   ~248) so the zero-clipping flush's all-flat baseline always fits -> no wall is ever dropped to
   sky.  Was 128 -> dense rooms (tech room) overflowed it and the surplus far walls weren't even
   accumulated = "clipping". */
#define WALL_ACC_MAX 240
static struct { short x1, yl1, yh1, x2, yl2, yh2, slot, v0, v1; int texnum, u1, u2;
                unsigned char mode; const unsigned char *cmap; } wall_acc[WALL_ACC_MAX];
static int wall_acc_n;

/* core hook (per one-sided seg, during the BSP walk): stash the wall */
extern "C" void sat_wall_vdp1(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                              int texnum, int u1, int u2, int v0, int v1,
                              const unsigned char *cmap)
{
    if (wall_acc_n >= WALL_ACC_MAX) return;
    int i = wall_acc_n++;
    wall_acc[i].x1 = (short)x1; wall_acc[i].yl1 = (short)yl1; wall_acc[i].yh1 = (short)yh1;
    wall_acc[i].x2 = (short)x2; wall_acc[i].yl2 = (short)yl2; wall_acc[i].yh2 = (short)yh2;
    wall_acc[i].texnum = texnum; wall_acc[i].u1 = u1; wall_acc[i].u2 = u2;
    wall_acc[i].v0 = (short)v0; wall_acc[i].v1 = (short)v1; wall_acc[i].cmap = cmap;
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
#define WALL_FLAT_YHI  199

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

/* Emit the horizontal u-tiles of ONE vertical band: the texture rows at [charAddr, +charSize.h]
   mapped across the wall's u-range, window-clipped to [x1,x2].  The yl/yh args are THIS band's
   screen y at the two seg ends. */
static void wall_emit_band(int x1, int x2, int yl1, int yh1, int yl2, int yh2,
                           int u1, int u2, int texw,
                           unsigned short charAddr, unsigned short charSize, unsigned short colr)
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

    /* normalise u (tiling is periodic in texw) so the arithmetic stays small */
    int ubase = u1 & ~(texw - 1);
    u1 -= ubase; u2 -= ubase;
    int umin = (u1 < u2) ? u1 : u2;
    int umax = (u1 < u2) ? u2 : u1;

    int winset = 0, ntiles = 0;
    for (int ub = umin & ~(texw - 1); ub < umax && ntiles < MAXWALLTILES; ub += texw, ++ntiles)
    {
        if (vdp1_wnext >= WALL_CMD_CAP) break;
        int xs = x1 + ((ub        - u1) * xspan) / du;   /* screen x of col 0  (u=ub)      */
        int xe = x1 + ((ub + texw - u1) * xspan) / du;   /* screen x of col texw (u=ub+texw) */
        int lo = (xs < xe) ? xs : xe, hi = (xs < xe) ? xe : xs;
        if (hi < x1 || lo > x2) continue;                /* tile outside the visible range */

        int yls = yl1 + (yl2 - yl1) * (xs - x1) / xspan;
        int yhs = yh1 + (yh2 - yh1) * (xs - x1) / xspan;
        int yle = yl1 + (yl2 - yl1) * (xe - x1) / xspan;
        int yhe = yh1 + (yh2 - yh1) * (xe - x1) / xspan;

        if (lo >= -wall_ext && hi <= 320 + wall_ext)     /* extend + window-clip (correct) */
        {
            if (!winset)
            {
                /* extend the window 1px each side so adjacent walls OVERLAP -> no seam
                   (the gap that, in motion, let the NBG0 sky show between quads). */
                int wx1 = x1 > 0   ? x1 - 1 : 0;
                int wx2 = x2 < 319 ? x2 + 1 : 319;
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
            int cyls = yl1 + (yl2 - yl1) * (cxs - x1) / xspan;
            int chys = yh1 + (yh2 - yh1) * (cxs - x1) / xspan;
            int cyle = yl1 + (yl2 - yl1) * (cxe - x1) / xspan;
            int chye = yh1 + (yh2 - yh1) * (cxe - x1) / xspan;
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
    int u1 = wall_acc[wi].u1, u2 = wall_acc[wi].u2;
    int texw = texturewidthmask[wall_acc[wi].texnum] + 1;
    int v0 = wall_acc[wi].v0, v1 = wall_acc[wi].v1, vspan = v1 - v0;
    unsigned short colr = wall_light_colr(wall_acc[wi].cmap);  /* per-wall light = CRAM bank */

    if (H <= 0 || vspan <= 0)                          /* no valid v-range -> whole texture once */
    {
        int th = (H > 255) ? 255 : (H > 0 ? H : 1);
        unsigned short ca = (unsigned short)((base - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | th);
        wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, colr);
        return;
    }

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
        int yl1b = yl1 + (int)((long long)(v  - v0) * (yh1 - yl1) / vspan);
        int yh1b = yl1 + (int)((long long)(vb - v0) * (yh1 - yl1) / vspan);
        int yl2b = yl2 + (int)((long long)(v  - v0) * (yh2 - yl2) / vspan);
        int yh2b = yl2 + (int)((long long)(vb - v0) * (yh2 - yl2) / vspan);
        unsigned int taddr = base + (unsigned int)vmod * (unsigned int)padW * 1u;  /* 8bpp */
        unsigned short ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | rows);
        wall_emit_band(x1, x2, yl1b, yh1b, yl2b, yh2b, u1, u2, texw, ca, cs, colr);
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

/* a wall is drawn FLAT only in low-detail (Z) mode now: the cost-based flat fallback was
   replaced by the core close-wall CPU fallback (SAT_WALL_CPU_SPAN, r_segs.c) -- a too-close
   wall is rendered in SOFTWARE (correct, no swim) and never reaches the platform. */
static int wall_is_flat(int wi)
{
    (void)wi;
    return wall_lowdetail;
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
    if (wall_acc_n == 0) return;
    wtex_tick++;
    for (int i = 0; i < WTEX_SLOTS; ++i) wtex_cache[i].locked = 0;

    for (int i = 0; i < wall_acc_n; ++i)                 /* resolve textures (near-first) */
        wall_acc[i].slot = (short)wall_tex_resolve(wall_acc[i].texnum, wall_acc[i].cmap);

    /* mode: 1 = textured, 2 = flat, 0 = skip (only the surplus if wall_acc_n itself > budget).
       Invariant kept across the loop: used + (#walls from i on) <= budget -> a flat ALWAYS fits. */
    int budget = WALL_CMD_CAP - vdp1_wnext, used = 0;
    for (int i = 0; i < wall_acc_n; ++i)
    {
        int remaining = wall_acc_n - i - 1;             /* walls after i; each keeps >=1 flat cmd */
        int textured = (wall_acc[i].slot >= 0 && !wall_is_flat(i));
        if (textured)
        {
            int c = wall_tilecount(i);
            if (used + c + remaining <= budget) { used += c; wall_acc[i].mode = 1; continue; }
        }
        if (used + 1 + remaining <= budget) { used += 1; wall_acc[i].mode = 2; }   /* guaranteed flat */
        else                                  wall_acc[i].mode = 0;                /* surplus (n>budget) */
    }

    for (int i = wall_acc_n - 1; i >= 0; --i)            /* paint far->near */
    {
        if (wall_acc[i].mode == 1)      wall_emit(i);
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
           backdrop instead of the bright sky, so the tearing is far less visible. */
        int show_sky = (gamestate == GS_LEVEL) && !automapactive && sat_frame_has_sky;
#if VDP2_RBG0_TEST
        /* RBG0/debug 3-mode cycle (rbg0_mode, pad Y) -- see the rbg0_mode decl:
           0 = VDP2 floor, no dbg   (RBG0 on, NBG3 off, sw floor skipped)
           1 = dbg + software floor (RBG0 off, NBG3 on, sw floor drawn)
           2 = dbg, no software floor (RBG0 off, NBG3 on, sw floor skipped). */
        sat_vdp2_floor    = (rbg0_mode == 1) ? 0 : 1;        /* mode 1 draws the sw floor; 0,2 skip it */
        uint16_t sky_bit  = (VDP2_HW_SKY && show_sky) ? NBG0ON : 0;   /* no NBG0 when sky is software */
        uint16_t rbg0_bit = (rbg0_mode == 0) ? RBG0ON : 0;   /* HW floor only in mode 0           */
        uint16_t nbg3_bit = (rbg0_mode == 0) ? 0 : NBG3ON;   /* debug overlay only in modes 1,2   */
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
       flat changes), then re-write its rotation params from the matrix each frame
       (slScrMatSet writes the rpara straight to VRAM -> no slSynch needed). */
    if (rbg0_mode == 0)
    {
        rbg0_upload_flat(sat_vdp2_floor_pic);
        rbg0_set_transform();
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
    /* LAYER INVERSION: clear the 3D VIEW (rows 0..191) to index 0 so next frame the SKIPPED wall
       columns stay transparent -> the VDP1 walls (below NBG1) show through.  The status bar (rows
       192..223) is owned by ST_Drawer, left intact. */
    memset(framebuffer, 0, 192 * 320);
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
        potato_level = (potato_level + 1) % 3;
        sat_apply_potato();
    }

#if VDP2_RBG0_TEST
    /* Pad Y cycles the 3 RBG0/debug modes (0 VDP2-floor/no-dbg -> 1 dbg+sw-floor ->
       2 dbg/no-sw-floor -> wrap).  Modes 1 vs 2 isolate the software-floor cost (read
       REC/EX/P/FLAT in each) = what the VDP2 floor saves.  (Y also taps 'y' to Doom --
       harmless, like the potato Z / blit L+R live toggles.) */
    if ((changed & PER_DGT_TY) && !(cur & PER_DGT_TY))
        rbg0_mode = (rbg0_mode + 1) % 3;
#endif

#if DUAL_CPU_BLIT
    /* Pad L+R held together cycles the blit config live (one press = next entry of
       blit_cfg[]: single -> 50/50 -> 60/40 -> 66/34 -> 75/25 -> wrap), to A/B the
       ratios on the same scene without a rebuild.  Buttons are active-low, so both
       held == (cur & (L|R)) == 0; fire once on the rising edge of that chord.
       (L/R also emit ','/'.' to Doom -- harmless taps, the real strafe is 0xa0/a1.) */
    {
        const unsigned short lr = (unsigned short)(PER_DGT_TL | PER_DGT_TR);
        static int lr_was = 0;
        int lr_now = ((cur & lr) == 0);
        if (lr_now && !lr_was) blit_mode = (blit_mode + 1) % BLIT_CFG_N;
        lr_was = lr_now;
    }
#endif

    for (unsigned int i = 0; i < PAD_MAP_LEN; ++i)
    {
        if (changed & pad_map[i].mask)
        {
            int pressed = !(cur & pad_map[i].mask);
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
    static char *argv[] = { (char *)"doom", 0 };
    doomgeneric_Create(1, argv);
    for (;;)
        doomgeneric_Tick();
}
