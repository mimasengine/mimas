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
#include "r_parallel.h"
}

#define SHOW_FPS 1

/* Set to 1 to ignore the RAM cart and always stream the WAD from CD (e.g. to
   run on a 4MB emulator the same way a no-cart / 1M-cart system would). */
#define FORCE_CD_STREAM 0

/* Framebuffer->VDP2 blit method.  1 = SCU DMA (fast; validated on real hardware
   once the indirect descriptor table is built through the cache-through mirror
   so the cacheless SCU bus master reads valid descriptors -- see dma_table_build).
   0 = plain CPU copy (slower, kept as a safe fallback). */
#define USE_SCU_DMA 0

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
/* Potato floors (solid-colour, flat-shaded) -- big EX/fillrate win, visible
   quality drop.  POTATO_FLOORS = the value at boot; toggle live in-game with the
   pad Z button.  Default off so the game starts textured. */
#define POTATO_FLOORS 0
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

#if SHOW_FPS
extern "C" int rp_timeout_count;
static unsigned int dg_frame_count = 0;

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
        sprintf(r2, "gt%5d vp%3d pot%d", gametic, r_visplane_peak, sat_potato_floors);
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
    slPriorityNbg0(5); slPriorityNbg1(6);   /* sky below game; shown via transparency */
#endif
    slScrAutoDisp(NBG0ON | NBG1ON | NBG3ON);

    /* Enable the core sky-skip: R_DrawPlanes leaves the sky region as index 0
       (transparent) so the VDP2 NBG0 sky shows through. */
    sat_vdp2_sky = 1;
    sat_potato_floors = POTATO_FLOORS;   /* boot default; pad Z toggles it live */

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

extern "C" void DG_DrawFrame(void)
{
    static int first_frame = 1;

    if (first_frame)
    {
        first_frame      = 0;
        console_enabled  = 0;
        sat_console_clear();
        dma_table_build();
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
    return;
#endif

    /* SCU DMA blit -- synchronous: wait for previous, kick, wait for this one.
       Made synchronous to prevent an async DMA from bus-starving the SH-2
       and freezing the game loop on real hardware. */
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
        SCU_D0MD = 0x01000007;
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
    SCU_D0MD = 0x01000007;
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

    /* Pad Z (unmapped in pad_map) = live Potato-floors toggle, for A/B testing
       the solid-colour-floor quality vs fps without rebuilding. */
    if ((changed & PER_DGT_TZ) && !(cur & PER_DGT_TZ))
        sat_potato_floors = !sat_potato_floors;

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
