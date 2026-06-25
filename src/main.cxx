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
** Mimas -- entry point for the Sega Saturn (Saturn Ring Library / SGL).
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
extern "C" int sat_split_vdp1;        /* split-screen: keep walls on VDP1 per-view (vs software baseline) */
extern "C" void sat_mp_input_init(void);  /* wires the local-MP pad-2..4 input hook */

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
    sat_split_vdp1      = 1;  /* split-screen renders walls on VDP1 per-view (pad X toggles to the
                                 software baseline live for HW A/B).  No effect in 1p (split path off). */
    sat_mp_input_init();      /* wire pad-2..4 -> ticcmd for local multiplayer */
    /* sat_local_players defaults to 1 (single-player).  Local co-op is OPT-IN at the title screen:
       a 2nd pad pressing A arms 2..4-player split (dg_saturn.cxx poll_pad -> sat_count_local_pads).
       Default 1 keeps the normal boot single-player and lets the same disc A/B 1p vs Np on HW. */
    run_on_doom_stack();
}
