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
** SRL replaces Jo Engine.  Direct SGL calls (slBitMapNbg1, slScrAutoDisp...)
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
extern "C" unsigned char *R_GetColumn(int tex, int col);
extern "C" unsigned char *colormaps;        /* lighttable_t* (byte*), saturn_cmap */
extern "C" int            gamestate;        /* gamestate_t: GS_LEVEL == 0 */
extern "C" int            menuactive;       /* boolean: menu overlay up */
extern "C" int            automapactive;    /* boolean: automap up */
extern "C" int            sat_vdp2_sky;     /* core: skip software sky (=> VDP2) */
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
static void sat_apply_potato(void)
{
    sat_potato_floors = (potato_level >= 1);
    sat_potato_walls  = (potato_level >= 2);
    wall_lowdetail    = (potato_level >= 2);
}
#define GS_LEVEL 0
#define SAT_CMAP_BYTES (34 * 256)           /* COLORMAP: 34 maps of 256 (r_data.c) */

/* Jo Engine compat shim: d_main.c calls jo_print(x, y, str) for debug overlay */
extern "C" void jo_print(int x, int y, char *str)
{
    SRL::Debug::Print((uint8_t)x, (uint8_t)y, str);
}

#if SHOW_FPS
static unsigned short last_dma_ticks;
#endif

static unsigned short pending_cram[256];
static volatile int   palette_dirty = 0;

static unsigned char framebuffer[320 * 200] __attribute__((aligned(4)));
static unsigned int  dma_table[200][3] __attribute__((aligned(16)));

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
        sprintf(r2, "gt%5d vp%3d pot%d z%d", gametic, r_visplane_peak,
                potato_level, VDP2_ZOOM_TEST);
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
        {
            /* row 16: the 6 (co)processors, for headroom-at-a-glance.  MST = master SH-2
               frame ms (the synchronous bottleneck).  SLV = slave SH-2 -> its own row 18
               (i/b%).  VD1 = VDP1 commands this frame + D(one)/B(usy).  VD2 = VDP2 (always
               compositing, spare capacity).  SCU = SCU-DSP/DMA (idle here = free to offload).
               68K = SCSP sound CPU (fixed). */
            static char r6[45];
            unsigned int mst = inst10 ? (10000u / inst10) : 0;   /* frame ms */
            sprintf(r6, "MST%ums SLV>r18 VD1 %d%c VD2~ SCU%s 68Ksnd",
                    mst, vdp1_last_cmds, vdp1_prev_done ? 'D' : 'B',
                    USE_SCU_DMA ? "dma" : "-");
            SRL::Debug::Print(0, 16, r6);
        }
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
    for (int y = 0; y < 256; ++y)
        memset(SKY_VRAM + y * SKY_VRAM_STRIDE, 0, SKY_VRAM_STRIDE);
    slBitMapNbg0(COL_TYPE_256, BM_512x256, (void *)SKY_VRAM);
    slBMPaletteNbg0(1);
    slScrPosNbg0(toFIXED(0.0), toFIXED(0.0));
#if SKY_DEBUG_SHOW
    slPriorityNbg0(6); slPriorityNbg1(5);   /* sky ON TOP to verify Stage A */
#else
    /* LAYER INVERSION: software (NBG1) ON TOP with Doom's correct occlusion; the VDP1
       walls render BELOW NBG1, filling the index-0 (transparent) wall gaps NBG1 leaves
       where the software wall draw is skipped.  NBG3 debug = 7 (top).
       NBG1 game = 6  >  every sprite priority = 5  >  NBG0 sky = 4. */
    slPriorityNbg0(4); slPriorityNbg1(6);
    slPrioritySpr0(5); slPrioritySpr1(5); slPrioritySpr2(5); slPrioritySpr3(5);
    slPrioritySpr4(5); slPrioritySpr5(5); slPrioritySpr6(5); slPrioritySpr7(5);
#endif
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON);

    /* Enable the core sky-skip: R_DrawPlanes leaves the sky region as index 0
       (transparent) so the VDP2 NBG0 sky shows through. */
    sat_vdp2_sky = 1;
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
    for (int y = 0; y < 200; ++y)
    {
        t[y][0] = 320;
        t[y][1] = (unsigned int)DOOM_VRAM + y * DOOM_VRAM_STRIDE;
        t[y][2] = (unsigned int)framebuffer + y * 320;
    }
    t[199][2] |= DMA_END_FLAG;
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
#define WTEX_BASE      0x25C05000u
#define WTEX_NARROW_N  8
#define WTEX_NARROW_SZ 0x8000u                                      /* 32KB */
#define WTEX_WIDE_N    3
#define WTEX_WIDE_SZ   0x10000u                                     /* 64KB -> 256x128 */
#define WTEX_WIDE_BASE (WTEX_BASE + WTEX_NARROW_N * WTEX_NARROW_SZ) /* 0x25C45000 */
#define WTEX_SLOTS     (WTEX_NARROW_N + WTEX_WIDE_N)                /* 11; ends 0x25C75000 */
#define WALL_CMD_CAP   (VDP1_BANK_CMDS - 8)   /* walls stop here -> room for end + margin */

/* GOURAUD lighting: bake the texture FULL-BRIGHT once per texnum (cache keyed by texnum -> NO
   re-bake/thrash, and the light is SMOOTH per-wall -> no "ca saute" pop of the distance-light's
   one-bake-per-texnum).  Per-wall light via a VDP1 gouraud table.  The Saturn gouraud is ADDITIVE
   (subtractive darkening) so it must stay GENTLE or it crushes darks = "trop contraste"; the curve
   below is tuned soft (small slope + high floor).  34 levels x 4 corner entries x 2B at the end of
   the wall-cache VRAM (free up to the HUD at 0x25C78000). */
#define VDP1_GOURAUD 1
#define WTEX_GTAB      (WTEX_WIDE_BASE + WTEX_WIDE_N * WTEX_WIDE_SZ)  /* 0x25C75000 */
#if VDP1_GOURAUD
#define WALL_PMOD_GRD  0x0004   /* CMDPMOD gouraud-shading bit */
#else
#define WALL_PMOD_GRD  0x0000
#endif
static struct { int texnum; unsigned int addr, cap; short padW, H;
                unsigned short avgcol; unsigned int lru; unsigned char locked; }
                wtex_cache[WTEX_SLOTS];
static unsigned int wtex_tick;     /* per-frame monotonic clock for LRU */

/* one-time: assign each slot its fixed VRAM address + capacity (narrow then wide pool) */
static void wtex_setup(void)
{
    for (int i = 0; i < WTEX_NARROW_N; ++i)
    { wtex_cache[i].addr = WTEX_BASE + (unsigned int)i * WTEX_NARROW_SZ;
      wtex_cache[i].cap = WTEX_NARROW_SZ; }
    for (int i = 0; i < WTEX_WIDE_N; ++i)
    { wtex_cache[WTEX_NARROW_N + i].addr = WTEX_WIDE_BASE + (unsigned int)i * WTEX_WIDE_SZ;
      wtex_cache[WTEX_NARROW_N + i].cap = WTEX_WIDE_SZ; }
#if VDP1_GOURAUD
    /* one gouraud table per colormap level: 4 identical corners = uniform darkening for the level.
       b = per-channel value (16 = neutral/full-bright; VDP1 subtracts (16-b) per channel).  The
       ADDITIVE darkening crushes darks if too strong ("trop contraste") -> keep it SOFT: gentle slope
       L/3 + a HIGH floor (min 10 = subtract at most 6).  Tune here: smaller divisor / lower floor =
       darker & more contrast; bigger divisor / higher floor = flatter & softer. */
    {
        volatile unsigned short *g = (volatile unsigned short *)WTEX_GTAB;
        for (int L = 0; L < 34; ++L)
        {
            int b = 16 - (L / 3); if (b < 10) b = 10; else if (b > 16) b = 16;
            unsigned short c = (unsigned short)((b << 10) | (b << 5) | b);
            g[L*4+0] = c; g[L*4+1] = c; g[L*4+2] = c; g[L*4+3] = c;
        }
    }
#endif
}

/* gouraud table address (CMDGRDA, >>3) for a wall's colormap = its light level's darkening. */
static inline unsigned short wall_grda(const unsigned char *cmap)
{
    int L = (int)((cmap - colormaps) >> 8);              /* colormap level 0..33 */
    if (L < 0) L = 0; else if (L > 33) L = 33;
    return (unsigned short)((WTEX_GTAB + (unsigned int)L * 8u - VDP1_VRAM_BASE) >> 3);
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
    unsigned int size = (unsigned int)(padW * H) * 2u;
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

    volatile unsigned short *t = (volatile unsigned short *)wtex_cache[victim].addr;
    unsigned int sr = 0, sg = 0, sb = 0, cnt = 0;        /* mean lit colour -> flat fallback */
    for (int x = 0; x < W; ++x)
    {
        const unsigned char *col = R_GetColumn(texnum, x);   /* H tall */
        for (int y = 0; y < H; ++y)
        {
#if VDP1_GOURAUD
            unsigned short v = pal_rgb555(col[y]);           /* FULL-BRIGHT: light via gouraud */
#else
            unsigned short v = pal_rgb555(cmap[col[y]]);     /* light baked in (per texnum) */
#endif
            t[y * padW + x] = v;
            sr += v & 0x1F; sg += (v >> 5) & 0x1F; sb += (v >> 10) & 0x1F; ++cnt;
        }
    }
    wtex_cache[victim].avgcol = cnt
        ? (unsigned short)(0x8000 | ((sb / cnt) << 10) | ((sg / cnt) << 5) | (sr / cnt))
        : 0x8000;
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
static int wall_ext = 96;  /* extend past a screen edge before the squish fallback (Romain: 32->96
                              pushes the edge "squish" out to the extreme border, much less visible;
                              costs more VDP1 fill -- it rasterizes the whole distorted quad). */

/* Flat quad screen-y clamp (low-detail / Z mode): a flat fill has NO texture, so clamping its
   geometry to the screen is FREE (no v -> no swim) and bounds the VDP1 fill; the layer
   inversion hides any silhouette overspill.  (Too-close TEXTURED walls are handled upstream
   by the core CPU fallback, not here.)  3D view is rows 0..167. */
#define WALL_FLAT_YLO  (-8)
#define WALL_FLAT_YHI  175

static int wall_vbands(int wi)   /* number of vertical texture-height bands this wall needs */
{
    int H = (wall_acc[wi].slot >= 0) ? wtex_cache[wall_acc[wi].slot].H : 128;
    int vspan = wall_acc[wi].v1 - wall_acc[wi].v0;
    int b = (H > 0 && vspan > 0) ? (vspan + H - 1) / H + 1 : 1;   /* +1: a band boundary cross */
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
                           unsigned short charAddr, unsigned short charSize, unsigned short grda)
{
    int xspan = x2 - x1, du = u2 - u1;
    unsigned short cmd[16];

    if (xspan <= 0 || du == 0 || texw <= 0)            /* degenerate -> single quad */
    {
        if (vdp1_wnext >= WALL_CMD_CAP) return;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = 0x0002; cmd[2] = 0x00A8 | WALL_PMOD_GRD; cmd[4] = charAddr; cmd[5] = charSize;
        cmd[6]  = (short)x1; cmd[7]  = (short)yl1;
        cmd[8]  = (short)x2; cmd[9]  = (short)yl2;
        cmd[10] = (short)x2; cmd[11] = (short)yh2;
        cmd[12] = (short)x1; cmd[13] = (short)yh1;
        cmd[14] = grda;
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
            cmd[0] = 0x0002; cmd[2] = 0x04A8 | WALL_PMOD_GRD;  /* DISTORSP | Window_In | gouraud */
            cmd[4] = charAddr; cmd[5] = charSize;
            cmd[6]  = (short)xs; cmd[7]  = (short)(yls - 1);   /* A col0  top */
            cmd[8]  = (short)xe; cmd[9]  = (short)(yle - 1);   /* B colW  top */
            cmd[10] = (short)xe; cmd[11] = (short)(yhe + 1);   /* C colW  bot */
            cmd[12] = (short)xs; cmd[13] = (short)(yhs + 1);   /* D col0  bot */
            cmd[14] = grda;
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
            cmd[0] = 0x0002; cmd[2] = 0x00A8 | WALL_PMOD_GRD;
            cmd[4] = charAddr; cmd[5] = charSize;
            cmd[6]  = (short)cxs; cmd[7]  = (short)cyls;
            cmd[8]  = (short)cxe; cmd[9]  = (short)cyle;
            cmd[10] = (short)cxe; cmd[11] = (short)chye;
            cmd[12] = (short)cxs; cmd[13] = (short)chys;
            cmd[14] = grda;
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
    unsigned short grda = wall_grda(wall_acc[wi].cmap);   /* per-wall light via gouraud */

    if (H <= 0 || vspan <= 0)                          /* no valid v-range -> whole texture once */
    {
        int th = (H > 255) ? 255 : (H > 0 ? H : 1);
        unsigned short ca = (unsigned short)((base - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | th);
        wall_emit_band(x1, x2, yl1, yh1, yl2, yh2, u1, u2, texw, ca, cs, grda);
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
        unsigned int taddr = base + (unsigned int)vmod * (unsigned int)padW * 2u;
        unsigned short ca = (unsigned short)((taddr - VDP1_VRAM_BASE) >> 3);
        unsigned short cs = (unsigned short)(((padW >> 3) << 8) | rows);
        wall_emit_band(x1, x2, yl1b, yh1b, yl2b, yh2b, u1, u2, texw, ca, cs, grda);
        v = vb; ++nb;
    }
}

/* Fallback FLAT-colour quad for a wall drawn without its texture (degressive far wall / cache
   miss).  Uses the texture's MEAN lit colour (cached when the texture was built -> matches the
   textured walls and blends in); only a true cache-miss (no slot) falls back to the dominant
   palette index.  1px generous like the textured quads. */
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
    /* with gouraud the texture (-> avgcol) is FULL-BRIGHT, so the dominant-index fallback is the
       raw (unlit) palette colour too; the per-wall light is applied by the gouraud table below. */
#if VDP1_GOURAUD
    unsigned short col = (wall_acc[wi].slot >= 0)
        ? wtex_cache[wall_acc[wi].slot].avgcol
        : pal_rgb555(R_WallPotatoColor(wall_acc[wi].texnum) & 0xFF);
#else
    unsigned short col = (wall_acc[wi].slot >= 0)
        ? wtex_cache[wall_acc[wi].slot].avgcol
        : pal_rgb555(wall_acc[wi].cmap[R_WallPotatoColor(wall_acc[wi].texnum) & 0xFF]);
#endif
    unsigned short cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x0004;                                  /* FUNC_Polygon (flat) */
    cmd[2] = 0x00C0 | WALL_PMOD_GRD;                  /* no transparency + gouraud */
    cmd[3] = col;                                     /* CMDCOLR = RGB555 fill */
    cmd[6]  = (short)x1; cmd[7]  = (short)(yl1 - 1);
    cmd[8]  = (short)x2; cmd[9]  = (short)(yl2 - 1);
    cmd[10] = (short)x2; cmd[11] = (short)(yh2 + 1);
    cmd[12] = (short)x1; cmd[13] = (short)(yh1 + 1);
    cmd[14] = wall_grda(wall_acc[wi].cmap);
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
   ZERO CLIPPING: EVERY wall is drawn -- at minimum a 1-command FLAT quad (all wall_acc_n <= 128
   flats always fit the ~248 command cap, so no wall is ever dropped to sky).  Then the NEAREST
   walls are UPGRADED to full textured tiles while the command budget allows.  Painted far->near. */
static void vdp1_walls_flush(void)
{
    if (wall_acc_n == 0) return;
    wtex_tick++;
    for (int i = 0; i < WTEX_SLOTS; ++i) wtex_cache[i].locked = 0;

    for (int i = 0; i < wall_acc_n; ++i)                 /* resolve textures (near-first) */
        wall_acc[i].slot = (short)wall_tex_resolve(wall_acc[i].texnum, wall_acc[i].cmap);

    int budget = WALL_CMD_CAP - vdp1_wnext;
    int used = wall_acc_n;                               /* baseline: every wall flat (1 cmd) */
    if (used > budget) used = budget;                    /* (n<=128<budget, safety) */
    for (int i = 0; i < wall_acc_n; ++i) wall_acc[i].mode = 0;   /* 0 = flat */
    for (int i = 0; i < wall_acc_n; ++i)                 /* near-first: upgrade to textured */
    {
        if (wall_acc[i].slot < 0 || wall_is_flat(i)) continue;   /* uncached/low-detail stay flat */
        int extra = wall_tilecount(i) - 1;               /* textured costs this much over flat */
        if (extra < 0) extra = 0;
        if (used + extra > budget) break;                /* budget full -> the rest stay flat */
        used += extra; wall_acc[i].mode = 1;             /* 1 = textured */
    }

    for (int i = wall_acc_n - 1; i >= 0; --i)            /* paint far->near (painter's algorithm) */
        if (wall_acc[i].mode) wall_emit(i);
        else                  wall_emit_flat(i);

    wall_acc_n = 0;
}
#endif

/* One-time: build the fixed root (sysclip + JUMP, link -> empty bank) and the empty
   bank, then put VDP1 in 1-cycle auto mode. */
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

    VDP1_TVMR = 0x0000;
    VDP1_FBCR = 0x0000;                              /* 1-cycle auto erase+draw+swap */
    VDP1_EWDR = 0x0000;                              /* erase to 0 = transparent */
    VDP1_EWLR = 0x0000;
    VDP1_EWRR = (unsigned short)(((320 >> 3) << 9) | 223);
    VDP1_PTMR = 0x0002;
}

/* core hook: begin this frame's player-sprite list in the OFF-screen bank. */
extern "C" void sat_vdp1_wpn_begin(void)
{
    unsigned short cmd[16];
    /* the cache bakes the live PLAYPAL into RGB555; on a palette flash (damage /
       pickup tint) drop it so the weapon + HUD re-tint with the scene. */
    if (palette_changed)
    {
        for (int i = 0; i < WPN_TEX_SLOTS; ++i) wpn_cache[i].lump = -1;
        vdp1_hud_csum = 0xFFFFFFFFu;
#if VDP1_WALL_TEST
        for (int i = 0; i < WTEX_SLOTS; ++i) wtex_cache[i].texnum = -1;
#endif
    }
#if VDP1_DBLBANK
    vdp1_wbank = vdp1_bank ^ 1;                      /* the bank VDP1 isn't showing */
#else
    vdp1_wbank = vdp1_bank;                          /* TEST: single bank (no extra frame?) */
#endif
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x000A;                                 /* bank cmd0 = local coord (0,0) */
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
    VDP1_PTMR = 0x0002;
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
    if (skytexture > 0 && skytexture != sky_loaded_tex)
        sky_upload();
#if SKY_FIXED
    slScrPosNbg0(toFIXED(0.0), toFIXED(0.0));
#else
    {
        /* Negated: invert the scroll direction (Romain -- the un-negated way felt
           wrong-way round; this was the real issue, not the speed). */
        int sx = -(int)(viewangle >> (SKY_ANGLESHIFT + SKY_PARALLAX_SHIFT));
        slScrPosNbg0((FIXED)(sx << 16), toFIXED(0.0));
    }
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
        int show_sky = (gamestate == GS_LEVEL) && !automapactive;
        slScrAutoDisp((uint16_t)(show_sky ? (NBG0ON | NBG1ON | NBG3ON)
                                          : (NBG1ON | NBG3ON)));
        if (show_sky)
            for (int i = 168 * 320; i < 200 * 320; ++i)
                if (framebuffer[i] == 0) framebuffer[i] = nb;
    }

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
    cache_purge();
    for (int y = 0; y < 200; ++y)
        memcpy(DOOM_VRAM + y * DOOM_VRAM_STRIDE, framebuffer + y * 320, 320);
    /* LAYER INVERSION: clear the 3D VIEW (rows 0..167) to index 0 so next frame the
       SKIPPED wall columns stay transparent -> the VDP1 walls (below NBG1) show through.
       The status bar (rows 168..199) is owned by ST_Drawer, left intact. */
    memset(framebuffer, 0, 168 * 320);
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
