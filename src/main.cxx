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
    run_on_doom_stack();
}
