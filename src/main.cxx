/*
** DoomSRL -- entry point for the Sega Saturn (Saturn Ring Library / SGL).
**
** SRL::Core::Initialize() replaces jo_core_init(): it calls slInitSystem,
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
    SRL::Core::Initialize(SRL::Types::HighColor::Colors::Black);
    run_on_doom_stack();
}
