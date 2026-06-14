/*
** DoomSRL -- dual-SH2 renderer back end (SRL build).
**
** Identical logic to SaturnDoom r_parallel.c.  Changes from the Jo Engine
** version:
**   - slSlaveFunc()  -> SRL::Slave::ExecuteOnSlave (C++ ITask wrapper)
**   - slCashPurge()  -> direct CCR register write (same hardware op)
**   - jo_print()     -> SRL::Debug::Print
**
** Everything else (command queue, executors, sync protocol, cache-coherency
** rules) is unchanged -- it is purely hardware-level and SDK-agnostic.
*/
#include <srl.hpp>

extern "C" {
#include <stdio.h>
#include <string.h>
#include "doomtype.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "r_main.h"
#include "r_draw.h"
#include "r_state.h"
#include "r_parallel.h"
}

#define RP_DEBUG 1

#if RP_DEBUG
extern "C" unsigned short sat_frt(void);
#define frt_now sat_frt
static unsigned short rp_t_begin, rp_t_rec, rp_t_fin;
extern "C" unsigned short rp_frt_entry;
unsigned short rp_frt_entry;
#endif

extern "C" byte *ylookup[];
extern "C" int   columnofs[];
extern "C" int   fuzzoffset[];
extern "C" int   fuzzpos;
extern "C" int   detailshift;
#define FUZZTABLE 50

/* ------------------------------------------------------------------ */
/* Command queue                                                       */
/* ------------------------------------------------------------------ */

enum { RP_COL, RP_TRANS, RP_FUZZ, RP_SPAN };

typedef struct
{
    unsigned char  type;
    unsigned char  unused;
    short          a;
    short          b;
    short          c;
    byte          *src;
    byte          *cmap;
    fixed_t        f1, f2, f3, f4;
} rp_cmd_t;

#define RP_CMDS  ((rp_cmd_t *)RP_CMD_BUF_ADDR)
#define RP_MAX   (RP_CMD_BUF_SIZE / (int)sizeof(rp_cmd_t))

typedef struct
{
    int ready;
    int masked_at;
    int total;
    int go_masked;
    int slave_opaque_done;
    int slave_masked_done;
    int slave_alive;
    int slave_execs;
} rp_sync_t;

static rp_sync_t rp_sync __attribute__((aligned(16)));
#define SYNC ((volatile rp_sync_t *)((unsigned int)&rp_sync | 0x20000000u))

static int  rec_count;
static int  rec_masked_at;
static int  in_masked;
static int  rp_active;
static int  rp_disabled;
extern "C" int rp_timeout_count;
int rp_timeout_count = 0;

static void (*saved_col)(void);
static void (*saved_base)(void);
static void (*saved_fuzz)(void);
static void (*saved_trans)(void);
static void (*saved_span)(void);

/* ------------------------------------------------------------------ */
/* Executors                                                           */
/* ------------------------------------------------------------------ */

static void rp_exec_col(const rp_cmd_t *cm, const int *colofs)
{
    int           count = cm->c - cm->b + 1;
    byte         *dest;
    fixed_t       frac, step, step2, step3, step4, step5, step6, step7, step8;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;
    byte          col_cache[128];

    if ((unsigned short)cm->a >= SCREENWIDTH  ||
        (unsigned short)cm->b >= SCREENHEIGHT ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    if (count <= 0) return;

    memcpy(col_cache, src, 128);
    src   = col_cache;
    dest  = ylookup[cm->b] + colofs[cm->a];
    step  = cm->f1;
    frac  = cm->f2 + (cm->b - centery) * step;
    step2 = step  + step;
    step3 = step2 + step;
    step4 = step2 + step2;
    step5 = step4 + step;
    step6 = step4 + step2;
    step7 = step4 + step3;
    step8 = step4 + step4;

    while (count >= 8)
    {
        dest[0]             = cmap[src[(frac)         >> FRACBITS & 127]];
        dest[SCREENWIDTH]   = cmap[src[(frac + step)  >> FRACBITS & 127]];
        dest[SCREENWIDTH*2] = cmap[src[(frac + step2) >> FRACBITS & 127]];
        dest[SCREENWIDTH*3] = cmap[src[(frac + step3) >> FRACBITS & 127]];
        dest[SCREENWIDTH*4] = cmap[src[(frac + step4) >> FRACBITS & 127]];
        dest[SCREENWIDTH*5] = cmap[src[(frac + step5) >> FRACBITS & 127]];
        dest[SCREENWIDTH*6] = cmap[src[(frac + step6) >> FRACBITS & 127]];
        dest[SCREENWIDTH*7] = cmap[src[(frac + step7) >> FRACBITS & 127]];
        dest  += SCREENWIDTH * 8;
        frac  += step8;
        count -= 8;
    }
    while (count >= 4)
    {
        dest[0]             = cmap[src[(frac)         >> FRACBITS & 127]];
        dest[SCREENWIDTH]   = cmap[src[(frac + step)  >> FRACBITS & 127]];
        dest[SCREENWIDTH*2] = cmap[src[(frac + step2) >> FRACBITS & 127]];
        dest[SCREENWIDTH*3] = cmap[src[(frac + step3) >> FRACBITS & 127]];
        dest  += SCREENWIDTH * 4;
        frac  += step4;
        count -= 4;
    }
    while (count > 0)
    {
        *dest = cmap[src[frac >> FRACBITS & 127]];
        dest += SCREENWIDTH;
        frac += step;
        count--;
    }
}

static void rp_exec_trans(const rp_cmd_t *cm, const int *colofs)
{
    int     count = cm->c - cm->b;
    byte   *dest;
    byte   *xlat = (byte *)cm->f3;
    fixed_t frac, step;

    if (count < 0) return;
    if ((unsigned short)cm->a >= SCREENWIDTH  ||
        (unsigned short)cm->b >= SCREENHEIGHT ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    dest = ylookup[cm->b] + colofs[cm->a];
    step = cm->f1;
    frac = cm->f2 + (cm->b - centery) * step;
    do {
        *dest = cm->cmap[xlat[cm->src[frac >> FRACBITS]]];
        dest += SCREENWIDTH;
        frac += step;
    } while (count--);
}

static void rp_exec_span(const rp_cmd_t *cm, const int *colofs)
{
    unsigned int  position, step;
    byte         *dest;
    int           count;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;

    position = (((unsigned int)cm->f1 << 10) & 0xffff0000)
             | (((unsigned int)cm->f2 >> 6)  & 0x0000ffff);
    step     = (((unsigned int)cm->f3 << 10) & 0xffff0000)
             | (((unsigned int)cm->f4 >> 6)  & 0x0000ffff);

    if ((unsigned short)cm->a >= SCREENHEIGHT ||
        (unsigned short)cm->b >= SCREENWIDTH  ||
        (unsigned short)cm->c >= SCREENWIDTH)  return;
    dest  = ylookup[cm->a] + colofs[cm->b];
    count = cm->c - cm->b + 1;

#define SPAN_PIX(pos) cmap[src[((pos) >> 26) | (((pos) >> 4) & 0x0fc0)]]
    while (count >= 8)
    {
        unsigned int p1=position+step,p2=p1+step,p3=p2+step,
                     p4=p3+step,p5=p4+step,p6=p5+step,p7=p6+step;
        dest[0]=SPAN_PIX(position); dest[1]=SPAN_PIX(p1); dest[2]=SPAN_PIX(p2);
        dest[3]=SPAN_PIX(p3);       dest[4]=SPAN_PIX(p4); dest[5]=SPAN_PIX(p5);
        dest[6]=SPAN_PIX(p6);       dest[7]=SPAN_PIX(p7);
        dest+=8; position=p7+step; count-=8;
    }
    while (count >= 4)
    {
        unsigned int p1=position+step,p2=p1+step,p3=p2+step;
        dest[0]=SPAN_PIX(position); dest[1]=SPAN_PIX(p1);
        dest[2]=SPAN_PIX(p2);       dest[3]=SPAN_PIX(p3);
        dest+=4; position=p3+step; count-=4;
    }
    while (count > 0) { *dest++=SPAN_PIX(position); position+=step; count--; }
#undef SPAN_PIX
}

static void rp_exec_fuzz(const rp_cmd_t *cm)
{
    int   yl=cm->b, yh=cm->c, count;
    byte *dest;
    if (!yl) yl=1;
    if (yh==viewheight-1) yh=viewheight-2;
    count=yh-yl;
    if (count<0) return;
    dest=ylookup[yl]+columnofs[cm->a];
    do {
        *dest=colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
        if (++fuzzpos==FUZZTABLE) fuzzpos=0;
        dest+=SCREENWIDTH;
    } while (count--);
}

static void rp_exec(const rp_cmd_t *cm, int parity, const int *colofs)
{
    if ((cm->a & 1) != parity) return;
    switch (cm->type)
    {
        case RP_COL:   rp_exec_col(cm, colofs);  break;
        case RP_TRANS: rp_exec_trans(cm, colofs); break;
        case RP_SPAN:  rp_exec_span(cm, colofs);  break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Slave side (runs as SRL::Slave::ITask::Start)                       */
/* ------------------------------------------------------------------ */

static void rp_slave_body(void)
{
    const rp_cmd_t *cmds = RP_CMDS;
    int i=0, lim, opq, execs=0;

    /* WT=1: slave writes are write-through to RAM so master sees them */
    {
        volatile unsigned char *ccr=(volatile unsigned char *)0xFFFFFE92;
        *ccr=(unsigned char)(*ccr|0x02);
    }
    /* Purge stale cache lines from the previous frame */
    {
        volatile unsigned char *ccr=(volatile unsigned char *)0xFFFFFE92;
        *ccr=(unsigned char)(*ccr|0x10);
    }
    SYNC->slave_alive=1;

    for (;;)
    {
        opq=SYNC->masked_at;
        lim=(opq>=0 && opq<SYNC->ready) ? opq : SYNC->ready;
        while (i<lim) { rp_exec(&cmds[i++],1,columnofs); execs++; }
        if (opq>=0 && i>=opq) break;
    }
    SYNC->slave_opaque_done=1;

    while (!SYNC->go_masked) ;
    for (i=SYNC->masked_at; i<SYNC->total; ++i) { rp_exec(&cmds[i],1,columnofs); execs++; }
    SYNC->slave_execs=execs;
    SYNC->slave_masked_done=1;
}

/* SRL::Slave::ITask wrapper around rp_slave_body */
class RpSlaveTask : public SRL::Slave::ITask
{
public:
    void Start() override { rp_slave_body(); }
    bool IsDone() override { return SYNC->slave_masked_done != 0; }
};

static RpSlaveTask slave_task;

/* ------------------------------------------------------------------ */
/* Master side                                                          */
/* ------------------------------------------------------------------ */

static int rp_wait(volatile int *flag)
{
    int guard=1000000;
    while (!*flag && --guard) ;
    if (!guard) rp_timeout_count++;
    return guard!=0;
}

static void rp_restart(void)
{
    SYNC->ready=0;
    SYNC->masked_at=in_masked?0:-1;
    SYNC->total=0;
    SYNC->go_masked=0;
    SYNC->slave_opaque_done=0;
    SYNC->slave_masked_done=0;
    SYNC->slave_alive=0;
    rec_count=0;
    rec_masked_at=in_masked?0:-1;
    SRL::Slave::ExecuteOnSlave(slave_task);
}

static void master_cache_purge(void)
{
    volatile unsigned char *ccr=(volatile unsigned char *)0xFFFFFE92;
    *ccr=(unsigned char)(*ccr|0x10);
}

static void rp_finish(void)
{
    const rp_cmd_t *cmds=RP_CMDS;
    int i, mat, tot, ok;
#if RP_DEBUG
    rp_t_rec=frt_now();
#endif
    if (SYNC->ready!=rec_count)
    {
        __asm__ volatile("":::"memory");
        SYNC->ready=rec_count;
    }
    mat=(rec_masked_at>=0)?rec_masked_at:rec_count;
    tot=rec_count;
    SYNC->masked_at=mat;
    SYNC->total=tot;

    for (i=0; i<mat; ++i) rp_exec(&cmds[i],0,columnofs);
    ok=rp_wait(&SYNC->slave_opaque_done);
    SYNC->go_masked=1;
    for (i=mat; i<tot; ++i) rp_exec(&cmds[i],0,columnofs);
    if (ok) ok=rp_wait(&SYNC->slave_masked_done);

    if (!ok)
    {
        for (i=0; i<tot; ++i) rp_exec(&cmds[i],1,columnofs);
        rp_disabled=1;
        printf("r_parallel: slave SH-2 not responding, disabled\n");
    }
    for (i=mat; i<tot; ++i)
        if (cmds[i].type==RP_FUZZ) rp_exec_fuzz(&cmds[i]);

    master_cache_purge();

#if RP_DEBUG
    {
        static char dbg[41];
        rp_t_fin=frt_now();
        sprintf(dbg,"c%4d a%d p%5u r%5u f%5u",tot,
                (int)SYNC->slave_alive,
                (unsigned short)(rp_t_begin-rp_frt_entry),
                (unsigned short)(rp_t_rec-rp_t_begin),
                (unsigned short)(rp_t_fin-rp_t_rec));
        SRL::Debug::Print(0,2,"%s",dbg);
    }
#endif
}

static void rp_flush(void)
{
    rp_finish();
    if (!rp_disabled) rp_restart();
}

/* ------------------------------------------------------------------ */
/* Recorders                                                           */
/* ------------------------------------------------------------------ */

static rp_cmd_t *rp_alloc(void)
{
    if (rec_count==RP_MAX) rp_flush();
    return &RP_CMDS[rec_count];
}

static void rp_commit(void)
{
    __asm__ volatile("":::"memory");
    rec_count++;
    if ((rec_count&7)==0) SYNC->ready=rec_count;
}

static void RP_RecordColumn(void)
{
    if (rp_disabled) { R_DrawColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_COL; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    cm->src=dc_source; cm->cmap=(byte *)dc_colormap;
    cm->f1=dc_iscale; cm->f2=dc_texturemid;
    rp_commit();
}

static void RP_RecordTrans(void)
{
    if (rp_disabled) { R_DrawTranslatedColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_TRANS; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    cm->src=dc_source; cm->cmap=(byte *)dc_colormap;
    cm->f1=dc_iscale; cm->f2=dc_texturemid; cm->f3=(fixed_t)dc_translation;
    rp_commit();
}

static void RP_RecordFuzz(void)
{
    if (rp_disabled) { R_DrawFuzzColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_FUZZ; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    rp_commit();
}

static void RP_RecordSpan(void)
{
    if (rp_disabled) { R_DrawSpan(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_SPAN; cm->a=(short)ds_y; cm->b=(short)ds_x1; cm->c=(short)ds_x2;
    cm->src=ds_source; cm->cmap=(byte *)ds_colormap;
    cm->f1=ds_xfrac; cm->f2=ds_yfrac; cm->f3=ds_xstep; cm->f4=ds_ystep;
    rp_commit();
}

/* ------------------------------------------------------------------ */
/* Frame hooks (extern "C" for r_main.c)                               */
/* ------------------------------------------------------------------ */

extern "C" void RP_BeginFrame(void)
{
    if (rp_disabled || detailshift!=0) { rp_active=0; return; }
#if RP_DEBUG
    rp_t_begin=frt_now();
#endif
    rp_active=1; in_masked=0;
    saved_col=colfunc; saved_base=basecolfunc;
    saved_fuzz=fuzzcolfunc; saved_trans=transcolfunc; saved_span=spanfunc;
    colfunc=basecolfunc=RP_RecordColumn;
    fuzzcolfunc=RP_RecordFuzz;
    transcolfunc=RP_RecordTrans;
    spanfunc=RP_RecordSpan;
    rp_restart();
}

extern "C" void RP_BeginMasked(void)
{
    if (!rp_active||rp_disabled) return;
    in_masked=1; rec_masked_at=rec_count;
    __asm__ volatile("":::"memory");
    SYNC->ready=rec_count;
    SYNC->masked_at=rec_count;
}

extern "C" void RP_EndFrame(void)
{
    if (!rp_active) return;
    rp_finish();
    colfunc=saved_col; basecolfunc=saved_base;
    fuzzcolfunc=saved_fuzz; transcolfunc=saved_trans; spanfunc=saved_span;
    rp_active=0;
}
