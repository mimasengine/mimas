/*
** DoomSRL -- entry point for the Sega Saturn (Saturn Ring Library / SGL).
**
** SRL::Core::Initialize() calls slInitSystem,
** sets up the VBlank IRQ chain, and initializes CD/sound.  No SGL work-area
** relocation is needed -- SRL allocates its own work buffers internally.
**
** Doom runs on a dedicated 40 KB stack in high work RAM.  The standard SH-2
** master stack is too small for Doom's deeply-recursive BSP traversal.
*/
#include <srl.hpp>

extern "C" void doom_start(void);

/* SATURN parallel-REC (Option C / P3): put the idle slave SH-2 on the plane phase.
   sat_plane_parallel routes the regular-flat visplanes through the master/slave worklist
   split (core/r_plane.c R_DrawPlaneWorklist + core/r_parallel.c RP_DispatchPlanes); r_main.c
   then forces rp_disabled so the parity slave isn't dispatched and the slave is free for it. */
extern "C" int sat_plane_parallel;
extern "C" int sat_masked_parallel;   /* slave draws the right-half vissprites (masked phase) */
extern "C" int sat_wallprep_defer;    /* STEP 1: defer wall-prep to a post-BSP flush (validation) */

/* 40 KB dedicated stack for the Doom main loop, in high work RAM. */
static char doom_stack[40 * 1024] __attribute__((aligned(16)));

static void __attribute__((noreturn, noinline)) run_on_doom_stack(void)
{
    void (*fn)(void) = doom_start;
    void *sp = doom_stack + sizeof(doom_stack);

    __asm__ volatile (
        "mov %1, r15\n\t"
        "jsr @%0\n\t"
        "nop\n"
        :
        : "r"(fn), "r"(sp)
        : "memory", "pr");
    for (;;)
        ;
}

int main(void)
{
    /* 320x224 (the standard NTSC game resolution) instead of SRL's default 320x240:
       Doom renders 320x200 and the VDP1 erases 224 lines, so 240 left a 40px black band
       under the view (status bar/weapon too high).  224 matches the VDP1 -> 24px band, then
       the view is centred in dg_saturn (VIEW_Y_OFFSET). */
    SRL::Core::Initialize(SRL::Types::HighColor::Colors::Black,
                          SRL::TV::Resolutions::Normal320x224);
    sat_plane_parallel = 1;   /* P3: slave SH-2 draws half the visplanes (set rp_disabled via r_main.c) */
    sat_masked_parallel = 1;  /* masked-by-half: slave SH-2 draws the right-half vissprites */
    sat_wallprep_defer  = 0;  /* foundation gated OFF (no overhead); STEP 2 (slave consumer) enables it */
    run_on_doom_stack();
}
