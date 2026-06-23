/*
** DoomSRL -- local multiplayer pad input (docs/MULTIPLAYER_PLAN.md, Iter 1).
**
** Builds player p's ticcmd straight from Smpc_Peripheral[p] (the port-1 multitap pads,
** index 0 = player 1 ... index 3 = player 4), using the SAME button mapping as player 1's
** pad in dg_saturn.cxx: D-pad = move/turn, TL/TR = strafe, TA = fire, TB = use, TC = run.
** Wired into the core hook sat_build_local_ticcmd (default NULL -> DoomJo/1p unaffected).
*/
#include <srl.hpp>

extern "C" {
#include <stdio.h>
#include "doomtype.h"
#include "d_ticcmd.h"
#include "d_event.h"   /* BT_ATTACK / BT_USE */
}

#define MP_INPUT_PROBE 0   /* overlay rows 9/10: which Smpc_Peripheral index is which pad (debug) */

extern "C" void (*sat_build_local_ticcmd)(ticcmd_t *, int);

/* Raw Smpc_Peripheral index for local player p.  SRL (srl_input.hpp) shows the SMPC buffer is
   port-1 pads at [0..5], a 9-entry gap, then port-2 pads at [15..20].  So: P1 = port-1 pad 1
   ([0]); for P2.. prefer the port-1 multitap pad [p] if present, else fall back to the port-2
   controller [15 + (p-1)].  Covers both "2 controllers (port 1 + port 2)" and "multitap". */
static int mp_raw_index(int p)
{
    if (p <= 0) return 0;
    if (Smpc_Peripheral[p].id != PER_ID_NotConnect) return p;        /* multitap pad on port 1 */
    int port2 = 15 + (p - 1);
    if (port2 < 24 && Smpc_Peripheral[port2].id != PER_ID_NotConnect) return port2;
    return p;
}

/* Count the connected local pads (1..4), reusing mp_raw_index's exact precedence so the count and
   the per-player slot mapping never disagree (docs/MULTIPLAYER_PLAN.md, Iter 1 optionality).  P1 is
   slot [0] (assumed present -- poll_pad bails otherwise); pads must be contiguous from P1, so stop
   at the first gap (a port-2-only controller gives 2, a 3-pad multitap gives 4, etc.). */
extern "C" int sat_count_local_pads(void)
{
    int n = 1;
    for (int p = 1; p < 4; ++p)
    {
        int idx = mp_raw_index(p);
        if (idx >= 0 && idx < 24 && Smpc_Peripheral[idx].id != PER_ID_NotConnect)
            n++;
        else
            break;
    }
    return n;
}

/* 1 while the 2nd local pad holds A (the co-op opt-in button; Saturn pads are active-low); 0 if
   there is no 2nd pad.  Read at the title screen to arm/disarm local multiplayer. */
extern "C" int sat_mp_pad2_a(void)
{
    int idx = mp_raw_index(1);
    if (idx < 0 || idx >= 24 || Smpc_Peripheral[idx].id == PER_ID_NotConnect) return 0;
    return !(Smpc_Peripheral[idx].data & PER_DGT_TA);
}

/* 1 while the 2nd local pad holds START (the title-screen player-count cycle; active-low);
   0 if there is no 2nd pad.  Read at the title to cycle sat_local_players 1->2->3->4->1. */
extern "C" int sat_mp_pad2_start(void)
{
    int idx = mp_raw_index(1);
    if (idx < 0 || idx >= 24 || Smpc_Peripheral[idx].id == PER_ID_NotConnect) return 0;
    return !(Smpc_Peripheral[idx].data & PER_DGT_ST);
}

extern "C" void DG_BuildLocalTiccmd(ticcmd_t *cmd, int p)
{
    if (p < 0 || p >= 12) return;

#if MP_INPUT_PROBE
    /* rows 9/10: EVERY Smpc_Peripheral[0..20] id (ff = empty).  Connect a 2nd controller and
       watch which index turns 02 = where port 2 lives.  Visible on NBG3 (pad Y -> mode 1/2). */
    if (p == 1)
    {
        static const char hx[] = "0123456789abcdef";
        char b[46], *q; int i;
        q = b; *q++='I';*q++='D';*q++='0'; *q++=' ';
        for (i = 0; i <= 10; i++) { *q++ = hx[(Smpc_Peripheral[i].id>>4)&15]; *q++ = hx[Smpc_Peripheral[i].id&15]; *q++=' '; }
        *q = 0; SRL::Debug::Print(0, 9, b);
        q = b; *q++='I';*q++='D';*q++='b'; *q++=' ';   /* b = 11.. */
        for (i = 11; i <= 20; i++) { *q++ = hx[(Smpc_Peripheral[i].id>>4)&15]; *q++ = hx[Smpc_Peripheral[i].id&15]; *q++=' '; }
        *q = 0; SRL::Debug::Print(0, 10, b);
    }
#endif

    /* TEST harness (docs/MULTIPLAYER_PLAN.md): emulators can't map 4 controllers, so J3 mirrors
       J1 and J4 mirrors J2 -- read the same pad as the mirrored player.  J1/J2 read their own. */
    int src = (p == 2) ? 0 : (p == 3) ? 1 : p;
    int idx = mp_raw_index(src);
    if (Smpc_Peripheral[idx].id == PER_ID_NotConnect) return;  /* no pad -> no input */

    unsigned short d = Smpc_Peripheral[idx].data;              /* Saturn digital pad = active-low */
    #define PR(m) (!(d & (m)))                                /* bit clear = pressed */

    int run  = PR(PER_DGT_TC);
    int fwd  = run ? 50  : 25;
    int side = run ? 40  : 24;
    int turn = run ? 1280 : 640;

    if (PR(PER_DGT_KU)) cmd->forwardmove = (signed char)(cmd->forwardmove + fwd);
    if (PR(PER_DGT_KD)) cmd->forwardmove = (signed char)(cmd->forwardmove - fwd);
    if (PR(PER_DGT_KL)) cmd->angleturn   = (short)(cmd->angleturn + turn);   /* left  */
    if (PR(PER_DGT_KR)) cmd->angleturn   = (short)(cmd->angleturn - turn);   /* right */
    if (PR(PER_DGT_TL)) cmd->sidemove    = (signed char)(cmd->sidemove - side);
    if (PR(PER_DGT_TR)) cmd->sidemove    = (signed char)(cmd->sidemove + side);
    if (PR(PER_DGT_TA)) cmd->buttons |= BT_ATTACK;
    if (PR(PER_DGT_TB)) cmd->buttons |= BT_USE;

    #undef PR
}

/* Called once from main() to wire the hook (the platform owns the pad). */
extern "C" void sat_mp_input_init(void)
{
    sat_build_local_ticcmd = DG_BuildLocalTiccmd;
}
