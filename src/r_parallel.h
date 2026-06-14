/*
** DoomSRL -- dual-SH2 renderer back end (see r_parallel.cxx).
*/
#ifndef R_PARALLEL_H
#define R_PARALLEL_H

/* Draw-command queue, carved from the top of low work RAM.
   DG_ZoneBase (dg_saturn.cxx) shrinks Doom's zone heap accordingly. */
#define RP_CMD_BUF_ADDR  0x002D8000
#define RP_CMD_BUF_SIZE  0x00028000   /* 160KB = 5120 commands of 32 bytes */

/* Called from R_RenderPlayerView (r_main.c). */
void RP_BeginFrame(void);
void RP_BeginMasked(void);
void RP_EndFrame(void);

#endif
