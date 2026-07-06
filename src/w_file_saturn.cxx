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
** Mimas -- WAD file backend (SRL build).
**
** Two modes selected at runtime by DG_Init:
**
**   CART mode (sat_wad_base != NULL)
**       Entire IWAD pre-loaded into 4 MB DRAM cart at boot via SRL::Cd::File.
**       Saturn_Read returns a memcpy from cart RAM; W_CacheLumpNum uses the
**       mapped pointer for zero-copy direct access.
**
**   CD mode (sat_wad_base == NULL)
**       No cart detected.  WAD stays on CD; lumps read on demand via
**       SRL::Cd::File::LoadBytes (byte-offset random access).
**       Level loads are slower; gameplay is unaffected once textures/sprites
**       are cached.
**
** GFS_* calls are replaced by SRL::Cd::File.
*/
#include <srl.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
#include "doomtype.h"
#include "w_file.h"
#include "z_zone.h"
}

extern "C" unsigned char *sat_wad_base;
extern "C" unsigned int   sat_wad_size;
extern "C" void sat_debug_row0(const char *s);

/* ------------------------------------------------------------------ */
/* CD-streaming state                                                  */
/* ------------------------------------------------------------------ */

/* SRL::Cd::File held open for the game's lifetime in CD mode */
static SRL::Cd::File *wad_cd_file = nullptr;

/*
** W_SaturnCDInit -- open DOOM1.WAD via SRL::Cd::File and record its size.
** Called by dg_saturn.cxx when no RAM cartridge is found.
*/
extern "C" int W_SaturnCDInit(void)
{
    static SRL::Cd::File wad_static("DOOM1.WAD");
    wad_cd_file = &wad_static;

    sat_debug_row0("CD:exists?");
    if (!wad_cd_file->Exists())
    {
        printf("CD: DOOM1.WAD not found\n");
        sat_debug_row0("CD:NOT FOUND");
        wad_cd_file = nullptr;
        return 0;
    }
    /*
    ** Keep the file CLOSED for the game's lifetime.  SRL::Cd::File::LoadBytes
    ** reads by file id via GFS_Load and works on a closed handle (the
    ** constructor already opened-probed-then-closed it).  When the handle is
    ** left OPEN, every LoadBytes pays a GFS_Close + reopen; staying closed
    ** makes each lump read a single churn-free GFS_Load.
    */
    sat_debug_row0("CD:ready");

    /*
    ** Determine WAD size from the header instead of a sequential probe.
    ** Sequential read via IsEOF()+Read() stops early (wrong sector count),
    ** giving sat_wad_size < actual file size; reads at the lump directory
    ** (which sits at the END of the WAD) are then blocked by the bounds
    ** check in Saturn_Read, causing W_GetNumForName to find nothing.
    **
    ** The WAD header (12 bytes at offset 0) gives us:
    **   [4..7]  numlumps      (little-endian int32)
    **   [8..11] infotableofs  (little-endian int32, offset of lump directory)
    ** So sat_wad_size = infotableofs + numlumps * 16.
    */
    sat_debug_row0("CD:hdr...");
    /* 4-byte aligned so this offset-0 read takes Saturn_Read's fast path (sub==0 &&
    ** aligned dest) and never bounces -- it runs BEFORE Z_Init, so it must not touch
    ** the (not-yet-allocated) zone staging buffer used by the multi-sector bounce. */
    static unsigned char hdr[12] __attribute__((aligned(4)));
    int hgot = wad_cd_file->LoadBytes(0, 12, hdr);
    {
        static char hdbg[45];
        sprintf(hdbg, "CD:hdr got=%d [%c%c%c%c]", hgot,
                hgot>=4?hdr[0]:'?', hgot>=4?hdr[1]:'?',
                hgot>=4?hdr[2]:'?', hgot>=4?hdr[3]:'?');
        sat_debug_row0(hdbg);
    }
    if (hgot < 12 || (hdr[0] != 'I' && hdr[0] != 'P') ||
        hdr[1] != 'W' || hdr[2] != 'A' || hdr[3] != 'D')
    {
        printf("CD: bad WAD header (got=%d)\n", hgot);
        sat_debug_row0("CD:BAD HDR");
        wad_cd_file = nullptr;
        return 0;
    }

    int32_t numlumps     = (int32_t)(hdr[4]  | (hdr[5]<<8)  | (hdr[6]<<16)  | (hdr[7]<<24));
    int32_t infotableofs = (int32_t)(hdr[8]  | (hdr[9]<<8)  | (hdr[10]<<16) | (hdr[11]<<24));
    sat_wad_size = (unsigned int)(infotableofs + numlumps * 16);
    sat_wad_base = NULL;   /* signals CD mode to Saturn_Read */

    printf("CD: WAD %u bytes, %d lumps, dir@%d\n",
           sat_wad_size, (int)numlumps, (int)infotableofs);
    {
        static char msg[45];
        sprintf(msg, "CD:WAD=%u nl=%d", sat_wad_size, (int)numlumps);
        sat_debug_row0(msg);
    }
    return sat_wad_size > 12 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* wad_file_class_t implementation                                     */
/* ------------------------------------------------------------------ */

static wad_file_t *Saturn_OpenFile(char *path);
static void        Saturn_CloseFile(wad_file_t *file);
static size_t      Saturn_Read(wad_file_t *file, unsigned int offset,
                               void *buffer, size_t buffer_len);

extern "C" wad_file_class_t saturn_wad_file =
{
    Saturn_OpenFile,
    Saturn_CloseFile,
    Saturn_Read,
};

static wad_file_t the_wad;

static wad_file_t *Saturn_OpenFile(char *path)
{
    (void)path;
    if (sat_wad_size == 0) return NULL;
    the_wad.file_class = &saturn_wad_file;
    the_wad.mapped     = sat_wad_base;   /* NULL in CD mode */
    the_wad.length     = sat_wad_size;
    return &the_wad;
}

static void Saturn_CloseFile(wad_file_t *file) { (void)file; }

/* SATURN CD read-retry (the SlaveDriver whackCD pattern).  LoadBytes is a single
** synchronous GFS_Load with NO internal retry (srl_cd.hpp): a transient seek/read
** error mid-game returns <=0 and, unretried, becomes the fatal "W_ReadLump: only
** read 0 of N" halt (the crash).  Each LoadBytes is self-contained (it re-seeks by
** file id), so simply re-calling re-attempts the read cleanly.  Retry hard before
** giving up; sat_cd_read_retries is surfaced on the overlay (row 0 `cd`) so a high
** count flags a flaky disc rather than a one-off. */
extern "C" int sat_cd_read_retries = 0;   /* cumulative retried reads this session */
/* R1 telemetry: cumulative GFS_Load "chunk" commands issued (one per sat_cd_load /
** drp_load invocation, retries excluded).  Ymir-measurable even though it can't time
** the CD -- the whole point of R1 is to shrink this per lump (bytes/16K, not bytes/2K).
** Watch the jump across a level warp: with the multi-sector bounce it is ~1/8 of the
** old per-sector count. */
extern "C" int sat_cd_loads = 0;
/* Companion: cumulative GFS_Loads the OLD per-sector path WOULD have issued for the
** same reads (fast-path reads +1; bounces += ceil((sub+n)/2048)).  Overlay shows
** ld<sat_cd_loads>/<sat_cd_persector> -> the ratio IS the R1 win, no rebuild needed. */
extern "C" int sat_cd_persector = 0;
#define SAT_CD_READ_RETRIES 8

static int sat_cd_load(size_t sector, int32_t bytes, void *dst)
{
    sat_cd_loads++;
    int got = wad_cd_file->LoadBytes(sector, bytes, dst);
    for (int attempt = 1; got <= 0 && attempt < SAT_CD_READ_RETRIES; ++attempt)
    {
        sat_cd_read_retries++;
        got = wad_cd_file->LoadBytes(sector, bytes, dst);
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* R1: multi-sector bounce (STREAMING_FLUIDITY_ROADMAP.md sec.4)       */
/* ------------------------------------------------------------------ */
/* GFS needs a sector offset + a 4-byte-aligned dest, so an unaligned lump (the
** common case: raw-bundled big WADs have ~0 sector-aligned lumps) is bounced
** through an aligned scratch.  The old bounce issued ONE full GFS_Load
** (open/seek/play/wait-pause/close) PER 2048 B sector -- a 64K lump = 33 CD
** commands.  Read up to SAT_CD_STAGE_SECTORS sectors per GFS_Load into a shared
** staging buffer, then memcpy the wanted slice out -> the same lump collapses to
** ceil(size/stage)+1 commands (~5 at 16K, ~3 at 32K).
**
** Placement: PU_STATIC in the Doom zone, allocated once on first bounce.  HWRAM is
** out (the TLSF pool is a few KB), and LWRAM is fully partitioned into [zone |
** RP_CMD_BUF] with the render buffer unborrowable (the slave consumes it while a
** master page-in can bounce mid-render).  Post-R4 the zone has >100K of headroom on
** every currently-loadable map, so this ~16-32K carve (CD-streaming mode only) is
** safe; the maps it would push over (Plutonia/TNT worst) already need R4.2 to fit at
** all.  Allocated lazily because the zone (Z_Init) comes up AFTER DG_Init -- the one
** pre-Z_Init read (the WAD header) is aligned to take the fast path and never bounce. */
#ifndef SAT_CD_STAGE_SECTORS
#define SAT_CD_STAGE_SECTORS 8                         /* 16 KB; raise to 16 for 32 KB */
#endif
#define SAT_CD_STAGE_BYTES   (SAT_CD_STAGE_SECTORS * 2048)

static unsigned char *sat_cd_stage = nullptr;

extern "C" unsigned char *sat_cd_stage_get(void)
{
    if (!sat_cd_stage)
        sat_cd_stage = (unsigned char *)Z_Malloc(SAT_CD_STAGE_BYTES, PU_STATIC, NULL);
    return sat_cd_stage;                               /* Z_Malloc I_Errors on OOM, never NULL */
}

/* Deliver [sector*2048 + sub, +n) into `buffer` (any alignment) via `load`, chunked
** through the staging buffer.  Returns bytes delivered.  Reads are serial on the
** master (Saturn_Read and drp_read never nest), so one shared staging buffer is safe. */
typedef int (*sat_cd_loader_fn)(size_t sector, int32_t bytes, void *dst);

extern "C" size_t sat_cd_bounce(sat_cd_loader_fn load, size_t sector, size_t sub,
                                void *buffer, size_t n)
{
    unsigned char *stage = sat_cd_stage_get();
    sat_cd_persector += (int)((sub + n + 2047) >> 11);   /* sectors the old per-sector loop issued */
    size_t done = 0;
    while (done < n)
    {
        size_t remaining = n - done;
        size_t span      = sub + remaining;            /* bytes still needed from `sector` on */
        size_t nsect     = (span + 2047) >> 11;
        if (nsect > SAT_CD_STAGE_SECTORS) nsect = SAT_CD_STAGE_SECTORS;
        int got = load(sector, (int32_t)(nsect << 11), stage);
        if (got <= (int)sub) break;                    /* read error or short of our start */
        size_t avail = (size_t)got - sub;
        size_t want  = remaining < avail ? remaining : avail;
        memcpy((unsigned char *)buffer + done, stage + sub, want);
        done += want;
        if (want < avail) break;                        /* satisfied within this chunk */
        size_t consumed = sub + want;                   /* whole sectors consumed from this chunk */
        sector += consumed >> 11;
        sub     = consumed & 2047;
    }
    return done;
}

static size_t Saturn_Read(wad_file_t *file, unsigned int offset,
                          void *buffer, size_t buffer_len)
{
    size_t n = buffer_len;
    (void)file;

    if (offset >= sat_wad_size) return 0;
    if (offset + n > sat_wad_size) n = sat_wad_size - offset;

    if (sat_wad_base != NULL)
    {
        /* Cart mode: zero-copy memcpy from cached cart RAM */
        memcpy(buffer, sat_wad_base + offset, n);
        return n;
    }

    /* CD mode: LoadBytes(sectorOffset, byteSize, dest) -> GFS_Load, where
    ** sectorOffset is the number of 2048-byte sectors to skip at the start of
    ** the file (NOT a byte offset), byteSize is a byte count, and dest MUST be
    ** 4-byte aligned.  GFS rejects an unaligned destination with GFS_ERR_ALIGN
    ** (-21), which surfaces here as a 0-byte read and upstream as the
    ** "W_ReadLump: only read 0 of N" failure (missing textures/sounds).
    **
    ** Convert the byte offset to a sector + in-sector offset, and NEVER hand
    ** GFS an unaligned pointer:
    **   - a fully sector-aligned read into a 4-byte-aligned buffer goes
    **     straight through (one GFS_Load, the fast path);
    **   - anything else (the common case: a lump whose offset%2048 is not a
    **     multiple of 4) is bounced sector-by-sector through an aligned buffer
    **     and memcpy'd into the caller's buffer, so GFS only ever writes to the
    **     aligned bounce buffer regardless of the lump's offset%4.
    ** (The old code read the tail straight into buffer+(2048-sub), which is
    ** unaligned whenever sub%4 != 0 -- that was the GFS_ERR_ALIGN trigger.)
    */
    if (!wad_cd_file || n == 0) return 0;

    size_t sector = (size_t)(offset >> 11);   /* offset / 2048 */
    size_t sub    = (size_t)(offset & 2047);  /* offset % 2048 */

    if (sub == 0 && ((size_t)buffer & 3u) == 0)
    {
        /* Sector-aligned offset into a 4-byte-aligned dest: one GFS_Load (retried). */
        sat_cd_persector++;                    /* aligned: old path also did 1 GFS_Load */
        int got = sat_cd_load(sector, (int32_t)n, buffer);
        return (got > 0) ? (size_t)got : 0;
    }

    /* Bounce path (R1): read up to SAT_CD_STAGE_SECTORS sectors per GFS_Load into
    ** the aligned staging buffer and copy the wanted slice out -- one CD command per
    ** chunk instead of one per sector.  GFS only ever writes to the aligned staging. */
    return sat_cd_bounce(sat_cd_load, sector, sub, buffer, n);
}
