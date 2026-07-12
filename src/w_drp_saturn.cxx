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
** Mimas -- per-level repack (.DRP) loader  (STREAMING_ANALYSIS.md §7.4/7.9-7.11).
**
** Step 3 of per-level repack.  In CD-STREAMING mode (big WADs, no cart) the disc
** optionally carries DOOMRP.DRP next to the full WAD: per-map blobs holding that
** map's subset lumps, LZSS-compressed and concatenated, plus a per-map offset table.
** This module:
**   - auto-detects the .DRP (the marker IS the file: magic "DRP1" + codec 1 +
**     n_lumps + dir_crc32 must match the loaded WAD; any mismatch -> stay raw),
**   - on P_SetupLevel selects the current map's entry table,
**   - on each W_ReadLump page-in, if the lump is in the current map's blob, reads
**     its compressed stream from the .DRP and LZSS-decodes it into the lump buffer.
**
** Out-of-subset lumps (and every lump when the .DRP is absent/mismatched, or in
** cart mode) fall through to the normal full-WAD read -- both layouts stay loadable.
** The hooks in core (W_ReadLump, P_SetupLevel) are gated by -DSAT_REPACK so the
** shared core stays byte-identical for DoomJo, which does not build this file.
**
** Format + the big-endian read discipline this mirrors: see tools/repack_wad.py.
*/
#include <srl.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "doomtype.h"
#include "w_wad.h"      /* lumpinfo, numlumps */
#include "z_zone.h"
}

extern "C" unsigned char *sat_wad_base;     /* cart base (NULL in CD-streaming mode) */
extern "C" int            sat_streaming_mode;

/* Step 4b cart staging (STREAMING_ANALYSIS §7.9 "Cart load-once") -- provided by
   dg_saturn.cxx.  In big-WAD CD-streaming mode with a 4MB cart, the map's compressed
   blob is staged CD->cart once per level, so page-ins decode from cart (CD idle ->
   CDDA on every map).  sat_cart_usable = cart bytes free (0 = none / cart holds raw
   WAD); sat_cart_cached_base = the cart's cached read alias. */
extern "C" unsigned int    sat_cart_usable;
extern "C" unsigned char  *sat_cart_cached_base;
extern int sat_cart_load_region(SRL::Cd::File &f, size_t sector, int len, unsigned int cart_ofs);

/* R1 multi-sector bounce helper, shared with the WAD reader (defined in
   w_file_saturn.cxx).  Reads are serial on the master so the single staging buffer
   is reentrancy-safe across the two sites. */
extern "C" {
typedef int (*sat_cd_loader_fn)(size_t sector, int32_t bytes, void *dst);
size_t sat_cd_bounce(sat_cd_loader_fn load, size_t sector, size_t sub, void *buffer, size_t n);
}

/* Compile-time A/B (hardware): 1 = stage the map blob into cart (Step 4b); 0 =
   always read the blob from CD (Step 3 behaviour).  Lets the cart path be toggled
   off without code surgery if hardware shows it regresses. */
#define SAT_DRP_CART_STAGE 1

/* ------------------------------------------------------------------ */
/* little-endian byte assembly (endian-safe on the big-endian SH-2)    */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* signed little-endian 16 -- the sprite-header index stores patch width/offsets
   (offsets are signed) as int16, same LE discipline as the rest of the container. */
static inline int16_t rd16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* CRC32 (zlib polynomial), continuable -- matches tools/repack_wad.py  */
/* dir_crc32(): crc32_run(crc, data) == zlib.crc32(data, crc).          */
/* ------------------------------------------------------------------ */
static uint32_t crc32_run(uint32_t crc, const unsigned char *buf, int len)
{
    crc ^= 0xFFFFFFFFu;
    while (len-- > 0)
    {
        crc ^= (uint32_t)(*buf++);
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* CRC32 over the source directory, per lump: name[8] (uppercase, NUL-padded) +
** size as 4 little-endian bytes.  lumpinfo[] is built from that directory in order
** with names already uppercased, so this reproduces the tool's dir_crc32. */
static uint32_t wad_dir_crc(void)
{
    uint32_t crc = 0;
    unsigned char rec[12];
    for (unsigned i = 0; i < numlumps; i++)
    {
        memcpy(rec, lumpinfo[i].name, 8);
        int sz = lumpinfo[i].size;
        rec[8]  = (unsigned char)(sz & 0xFF);
        rec[9]  = (unsigned char)((sz >> 8) & 0xFF);
        rec[10] = (unsigned char)((sz >> 16) & 0xFF);
        rec[11] = (unsigned char)((sz >> 24) & 0xFF);
        crc = crc32_run(crc, rec, 12);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* LZSS-12/4 decoder (codec 1) -- mirrors repack_wad.py lzss_decompress */
/* window = the output buffer; all reads byte-wise (endian-safe).       */
/* ------------------------------------------------------------------ */
static void drp_lzss_decode(const unsigned char *src, unsigned char *dst, int usize)
{
    int sp = 0, dp = 0;
    while (dp < usize)
    {
        unsigned char flags = src[sp++];
        for (int b = 0; b < 8 && dp < usize; b++)
        {
            if (flags & 1u)
            {
                dst[dp++] = src[sp++];               /* literal */
            }
            else
            {
                unsigned char b0 = src[sp++];
                unsigned char b1 = src[sp++];
                int offset = (((b1 & 0x0F) << 8) | b0) + 1;   /* 1..4096 */
                int length = (b1 >> 4) + 3;                   /* 3..18  */
                int start  = dp - offset;                     /* offset <= dp by construction */
                for (int k = 0; k < length && dp < usize; k++) /* stop at usize even mid-match */
                    dst[dp++] = dst[start + k];
            }
            flags >>= 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* .DRP container access                                                */
/* ------------------------------------------------------------------ */
#define DRP_HDR_SZ   32
#define DRP_MAP_SZ   24
#define DRP_ENT_SZ   16
#define DRP_MAGIC    0x31505244u            /* "DRP1" little-endian */
#define DRP_CODEC_LZSS 1
#define DRP_MAX_MAPS   2048                 /* sanity cap (corrupt-header guard) */

static SRL::Cd::File *drp_file  = nullptr;

/* sat_drp_state (exported for the dg_saturn row-21 overlay): 0 unprobed, 1 ACTIVE,
   -1 not-streaming/cart, -2 no DOOMRP.DRP, -3 header/magic/codec/n_lumps mismatch,
   -4 WAD dir_crc32 mismatch (stale .DRP), -5 map-table read fail.  Logic tests ==1. */
extern "C" int sat_drp_state;    int sat_drp_state   = 0;
extern "C" int sat_drp_n_maps;   int sat_drp_n_maps  = 0;   /* maps in the .DRP */
extern "C" int sat_drp_served;   int sat_drp_served  = 0;   /* lumps served from a blob */
extern "C" int sat_drp_cart;     int sat_drp_cart    = 0;   /* 1 = current map staged in cart */
extern "C" int sat_drp_cart_kb;  int sat_drp_cart_kb = 0;   /* staged blob size (KB) */

static uint32_t       drp_n_maps      = 0;
static unsigned char *drp_map_tab     = nullptr;  /* n_maps * 24, PU_STATIC */

/* R3.1 boot index: precomputed sprite-header section (byte offset + count of
   sprite lumps).  0 count = the .DRP predates R3.1 -> classic per-lump boot path. */
static uint32_t       drp_sprh_ofs    = 0;
static uint32_t       drp_sprh_n      = 0;

/* current selected map */
static unsigned char *drp_entries     = nullptr;  /* n_entries * 16, PU_STATIC */
static int            drp_n_entries   = 0;
static uint32_t       drp_blob_ofs    = 0;

/* Step 4b: current map's blob staged in cart RAM */
static int            drp_cart_staged = 0;        /* 1 = serve this map from cart */
static unsigned char *drp_cart_blob   = nullptr;  /* cached-cart pointer to blob byte 0 */

extern "C" int sat_drp_read_retries;
int sat_drp_read_retries = 0;
#define DRP_READ_RETRIES 8

extern "C" int sat_cd_loads;       /* R1 telemetry (defined in w_file_saturn.cxx) */
extern "C" int sat_cd_persector;   /* R1 per-sector-equivalent baseline */

/* R0.2 k-meter, shared with the WAD reader (defined in w_file_saturn.cxx). */
extern "C" unsigned short sat_frt(void);
extern "C" unsigned int   sat_vbl(void);
extern "C" void sat_cd_clock_add(unsigned short f0, unsigned int v0);

/* Front-only render gate (core/r_things.c), armed by the .DRP header flag below. */
extern "C" int sat_sprite_frontonly;

static int drp_load_raw(size_t sector, int32_t bytes, void *dst)
{
    sat_cd_loads++;
    int got = drp_file->LoadBytes(sector, bytes, dst);
    for (int a = 1; got <= 0 && a < DRP_READ_RETRIES; ++a)
    {
        sat_drp_read_retries++;
        got = drp_file->LoadBytes(sector, bytes, dst);
    }
    return got;
}

static int drp_load(size_t sector, int32_t bytes, void *dst)
{
    unsigned short f0 = sat_frt();
    unsigned int   v0 = sat_vbl();
    int got = drp_load_raw(sector, bytes, dst);
    sat_cd_clock_add(f0, v0);          /* R0.2: .DRP commands count toward k too */
    return got;
}

/* Read `len` bytes at byte `offset` of DOOMRP.DRP.  Same GFS alignment discipline
** as Saturn_Read: GFS needs a sector offset and a 4-byte-aligned dest, so a
** non-sector / non-aligned read is bounced sector-by-sector through an aligned
** scratch.  Returns bytes read. */
static int drp_read(uint32_t offset, void *buffer, int len)
{
    if (!drp_file || len <= 0) return 0;
    size_t sector = (size_t)(offset >> 11);
    size_t sub    = (size_t)(offset & 2047);

    if (sub == 0 && ((uintptr_t)buffer & 3u) == 0)
    {
        sat_cd_persector++;                    /* aligned: old path also did 1 GFS_Load */
        int got = drp_load(sector, (int32_t)len, buffer);
        return got > 0 ? got : 0;
    }

    /* R1: multi-sector bounce (was one GFS_Load per 2048 B sector). */
    return (int)sat_cd_bounce(drp_load, sector, sub, buffer, (size_t)len);
}

/* One-time probe: open DOOMRP.DRP and validate it against the loaded WAD.  Only
** in CD-streaming mode (cart mode maps the whole WAD, so no .DRP is needed). */
static void drp_probe(void)
{
    sat_drp_state = -1;                           /* default: inactive (cart / not streaming) */
    if (sat_wad_base != NULL || !sat_streaming_mode) return;

    static SRL::Cd::File f("DOOMRP.DRP");
    if (!f.Exists()) { sat_drp_state = -2; return; }
    drp_file = &f;

    unsigned char hdr[DRP_HDR_SZ];
    if (drp_read(0, hdr, DRP_HDR_SZ) < DRP_HDR_SZ) { sat_drp_state = -3; drp_file = nullptr; return; }

    uint32_t magic    = rd32(hdr + 0);
    uint32_t n_lumps  = rd32(hdr + 4);
    uint32_t crc      = rd32(hdr + 8);
    uint32_t n_maps   = rd32(hdr + 12);
    uint32_t maptabof = rd32(hdr + 16);
    uint32_t codec    = rd32(hdr + 20);
    uint32_t sprh_ofs = rd32(hdr + 24);       /* R3.1 sprite-header index (0 = absent) */
    uint32_t sprh_n   = rd32(hdr + 28);

    /* The codec word doubles as a flags field (tool emits codec | flags<<8):
       bit 8 = front-only sprites -- monster rotation lumps are stripped from the
       blobs (PLAY* kept for split-screen readability). */
    uint32_t drpflags = codec >> 8;
    codec &= 0xFFu;

    if (magic != DRP_MAGIC || codec != DRP_CODEC_LZSS ||
        n_lumps != numlumps || n_maps == 0 || n_maps > DRP_MAX_MAPS)
    {
        printf("DRP: header mismatch -> raw streaming\n");
        sat_drp_state = -3; drp_file = nullptr; return;
    }
    if (crc != wad_dir_crc())
    {
        printf("DRP: WAD CRC mismatch (stale .DRP) -> raw streaming\n");
        sat_drp_state = -4; drp_file = nullptr; return;
    }

    /* cache the map table resident */
    drp_map_tab = (unsigned char *)Z_Malloc((int)(n_maps * DRP_MAP_SZ), PU_STATIC, NULL);
    if (drp_read(maptabof, drp_map_tab, (int)(n_maps * DRP_MAP_SZ)) < (int)(n_maps * DRP_MAP_SZ))
    {
        printf("DRP: map table read failed -> raw streaming\n");
        Z_Free(drp_map_tab); drp_map_tab = nullptr; drp_file = nullptr;
        sat_drp_state = -5; return;
    }
    drp_n_maps    = n_maps;
    sat_drp_n_maps = (int)n_maps;
    /* R3.1: sanity the sprite-header section before trusting it (a corrupt count
       could else over-read).  n_lumps is a safe upper bound on sprite lumps. */
    if (sprh_n > 0 && sprh_n <= n_lumps && sprh_ofs >= DRP_HDR_SZ)
    {
        drp_sprh_ofs = sprh_ofs;
        drp_sprh_n   = sprh_n;
    }
    /* Front-only .DRP: arm the render gate BEFORE any sprite draws -- with the
       rotations stripped from the blobs, a rotation request would take the full-WAD
       CD fallback (hitches, and CD traffic under CDDA on cart-staged maps).  Flag and
       blob content come from the same tool run, so coherence is by construction. */
    if (drpflags & 1u)
    {
        sat_sprite_frontonly = 1;
        printf("DRP: front-only sprites (rotations stripped, PLAY kept)\n");
    }
    sat_drp_state = 1;
    printf("DRP: active, %u maps (repacked streaming)\n", (unsigned)n_maps);
}

/* R3.1 boot index (STREAMING_ROADMAP §6 R3.1): fill R_InitSpriteLumps' three arrays
** (spritewidth / spriteoffset / spritetopoffset, each already <<FRACBITS) from the
** .DRP's precomputed sprite-header section in ONE sequential read.  In CD-streaming
** mode the classic path caches all ~1381 sprite lumps just for 6 header bytes each
** (~1381 CD reads at boot); this collapses that to a single ~8 KB read.  `n` MUST be
** the engine's numspritelumps (the .DRP is CRC-locked to this exact WAD directory, so
** the sprite lump set is identical by construction -- a count mismatch means a stale
** .DRP and we fall back).  Returns 1 = arrays filled; 0 = caller runs the classic loop.
** Only the R3.1-era tool emits the section, so an older .DRP simply returns 0. */
extern "C" int sat_drp_sprite_headers(int *width, int *loff, int *toff, int n)
{
    if (sat_drp_state == 0) drp_probe();
    if (sat_drp_state != 1 || drp_sprh_n == 0) return 0;
    if (n <= 0 || (int)drp_sprh_n != n) return 0;

    int bytes = n * 6;                                   /* 3 int16 per sprite lump */
    unsigned char *tmp = (unsigned char *)Z_Malloc(bytes, PU_STATIC, NULL);
    if (drp_read(drp_sprh_ofs, tmp, bytes) < bytes) { Z_Free(tmp); return 0; }
    for (int i = 0; i < n; i++)
    {
        const unsigned char *r = tmp + i * 6;
        width[i] = (int)rd16(r + 0) << 16;               /* <<FRACBITS (fixed_t) */
        loff[i]  = (int)rd16(r + 2) << 16;
        toff[i]  = (int)rd16(r + 4) << 16;
    }
    Z_Free(tmp);
    printf("DRP: sprite index %d ok (1 read)\n", n);   /* validation proof the fast path ran */
    return 1;
}

/* ------------------------------------------------------------------ */
/* R5.1 -- budgeted level-load preload of the map's subset             */
/* (STREAMING_FLUIDITY_ROADMAP.md sec.8)                               */
/* ------------------------------------------------------------------ */
/* The selected map's .DRP entry table IS the preload list (superset-safe, computed
** offline).  Called by P_SetupLevel at the very END of level setup -- after the
** texcache carve (the slab must win the contiguous-run contest) and the sfx warm-up
** -- under the load fade: pull lumps into plain purgeable PU_CACHE until the zone's
** largest allocatable run drops to the keep-free floor.  Pass 0 = sprites + flats
** (the hitchiest first-sights: a monster type's first frame, a visplane's first
** flat); pass 1 = the rest (wall patches).  Zone-tight maps hit the guard
** immediately and behave exactly as today; a purged preload simply reverts that
** lump to the lazy path.  With the blob cart-staged the preload never touches the
** CD at all (LZSS decode from cart RAM). */
extern "C" { extern int firstspritelump, lastspritelump, firstflat, lastflat; }

extern "C" int sat_drp_preload_kb;  int sat_drp_preload_kb = 0;   /* row-21 telemetry */
#ifndef SAT_DRP_PRELOAD_KEEP_FREE
#define SAT_DRP_PRELOAD_KEEP_FREE (192*1024)   /* leave this much allocatable for gameplay */
#endif

extern "C" void sat_drp_preload(void)
{
    int done = 0;
    sat_drp_preload_kb = 0;
    if (sat_drp_state != 1 || !drp_entries || drp_n_entries <= 0) return;

    for (int pass = 0; pass < 2; pass++)
    {
        for (int e = 0; e < drp_n_entries; e++)
        {
            const unsigned char *ent = drp_entries + e * DRP_ENT_SZ;
            uint32_t lump  = rd32(ent + 0);
            uint32_t usize = rd32(ent + 12);
            if (usize == 0 || lump >= numlumps) continue;
            int hot = ((int)lump >= firstspritelump && (int)lump <= lastspritelump) ||
                      ((int)lump >= firstflat       && (int)lump <= lastflat);
            if (hot != (pass == 0)) continue;
            if (lumpinfo[lump].cache) continue;          /* already resident (geometry etc.) */
            if (Z_LargestAllocatable() < (int)(SAT_DRP_PRELOAD_KEEP_FREE + usize))
                goto out;                                /* zone budget reached */
            W_CacheLumpNum((int)lump, PU_CACHE);
            done += (int)usize;
        }
    }
out:
    sat_drp_preload_kb = done >> 10;
    if (done)
        printf("DRP: preloaded %dK (subset warm)\n", done >> 10);
}

/* uppercase 8-byte compare of a (lowercase) Doom map lumpname vs a stored name */
static int name8_ieq(const char *want, const unsigned char *stored)
{
    for (int i = 0; i < 8; i++)
    {
        unsigned char a = (unsigned char)want[i];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        unsigned char e = stored[i];
        if (e >= 'a' && e <= 'z') e = (unsigned char)(e - 'a' + 'A');
        if (a != e) return 0;
        if (a == 0) break;                      /* both NUL here */
    }
    return 1;
}

/* Step 4b: stage the current map's compressed blob CD->cart RAM once, so its lumps
** page in from cart (CD idle during play -> CDDA on every map).  Needs a 4MB cart
** (sat_cart_usable) and the blob to fit; otherwise leaves drp_cart_staged=0 and the
** lump reads keep coming from CD (drp_read), exactly as Step 3.  GFS reads from
** sector starts, so the blob's first byte lands at cart offset `sub` -- drp_cart_blob
** points there; data_ofs (entry-relative) is then added per lump. */
static void drp_stage_to_cart(uint32_t blob_ofs, uint32_t blob_size)
{
    drp_cart_staged = 0;
    drp_cart_blob   = nullptr;
    sat_drp_cart    = 0;
#if SAT_DRP_CART_STAGE
    if (!drp_file || sat_cart_cached_base == nullptr ||
        sat_cart_usable < 0x400000u || blob_size == 0)
        return;

    if (blob_size > sat_cart_usable) return;            /* validate BEFORE the add: a corrupt
                                                           blob_size could else wrap sub+blob_size
                                                           and slip past the fit/short-read guards */
    size_t   sector = (size_t)(blob_ofs >> 11);
    unsigned sub    = (unsigned)(blob_ofs & 2047);
    unsigned total  = sub + blob_size;                  /* leading partial sector + blob (can't wrap now:
                                                           sub<=2047, blob_size<=sat_cart_usable<=4MB) */
    if (total > sat_cart_usable) return;                /* doesn't fit -> stay on CD */

    /* Same CD-read retry discipline as drp_load (Saturn LoadBytes can short-read; CD
       reliability is an open HW issue): re-read into the same uncached window + re-purge
       (idempotent) before giving up, so one glitch doesn't cost the whole map its cart. */
    int got = sat_cart_load_region(*drp_file, sector, (int)total, 0);
    for (int a = 1; got < (int)total && a < DRP_READ_RETRIES; ++a)
    {
        sat_drp_read_retries++;
        got = sat_cart_load_region(*drp_file, sector, (int)total, 0);
    }
    if (got < (int)total) return;                       /* short read -> stay on CD */

    drp_cart_blob   = sat_cart_cached_base + sub;
    drp_cart_staged = 1;
    sat_drp_cart    = 1;
    sat_drp_cart_kb = (int)(blob_size >> 10);
#else
    (void)blob_ofs; (void)blob_size;
#endif
}

/* ------------------------------------------------------------------ */
/* core hooks (declared extern "C" in core, gated by -DSAT_REPACK)      */
/* ------------------------------------------------------------------ */

/* P_SetupLevel: point the loader at this map's per-map blob.  `lumpname` is the
** engine's map marker name ("map01" / "e1m1", lowercase). */
extern "C" void sat_drp_select_map(const char *lumpname)
{
    if (sat_drp_state == 0) drp_probe();

    /* release the previous map's entry table + cart staging */
    if (drp_entries) { Z_Free(drp_entries); drp_entries = nullptr; }
    drp_n_entries = 0;
    drp_blob_ofs  = 0;
    drp_cart_staged = 0; drp_cart_blob = nullptr; sat_drp_cart = 0; sat_drp_cart_kb = 0;
    if (sat_drp_state != 1) return;

    for (uint32_t m = 0; m < drp_n_maps; m++)
    {
        const unsigned char *rec = drp_map_tab + m * DRP_MAP_SZ;
        if (!name8_ieq(lumpname, rec)) continue;

        uint32_t n_entries   = rd32(rec + 8);
        uint32_t entries_ofs = rd32(rec + 12);
        uint32_t blob_ofs    = rd32(rec + 16);
        uint32_t blob_size   = rd32(rec + 20);
        if (n_entries == 0 || n_entries > numlumps) return;   /* corrupt -> raw for this map */

        int bytes = (int)(n_entries * DRP_ENT_SZ);
        drp_entries = (unsigned char *)Z_Malloc(bytes, PU_STATIC, NULL);
        if (drp_read(entries_ofs, drp_entries, bytes) < bytes)
        {
            Z_Free(drp_entries); drp_entries = nullptr;   /* read failed -> raw for this map */
            return;
        }
        drp_n_entries = (int)n_entries;
        drp_blob_ofs  = blob_ofs;
        drp_stage_to_cart(blob_ofs, blob_size);   /* Step 4b: CD->cart once (no-op if no cart) */
        return;
    }
    /* map not in the .DRP -> this map streams from the full WAD (e.g. the finale) */
}

/* W_ReadLump: if `lump` is in the current map's blob, fill `dest` (size bytes) from
** the .DRP and return 1; otherwise return 0 so the caller reads the full WAD. */
extern "C" int sat_drp_read_lump(unsigned int lump, void *dest, int size)
{
    if (sat_drp_state != 1 || drp_n_entries <= 0) return 0;

    /* binary search the entry table (strictly increasing lump_idx) */
    int lo = 0, hi = drp_n_entries - 1;
    const unsigned char *e = nullptr;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        const unsigned char *m = drp_entries + mid * DRP_ENT_SZ;
        uint32_t idx = rd32(m + 0);
        if (idx == lump)      { e = m; break; }
        else if (idx < lump)  lo = mid + 1;
        else                  hi = mid - 1;
    }
    if (!e) return 0;                            /* out of subset -> full-WAD fallback */

    uint32_t data_ofs = rd32(e + 4);
    uint32_t csize    = rd32(e + 8);
    uint32_t usize    = rd32(e + 12);
    if ((int)usize != size) return 0;            /* defensive: directory mismatch -> fallback */

    if (drp_cart_staged)                         /* Step 4b: decode straight from cart RAM, no CD */
    {
        const unsigned char *blob = drp_cart_blob + data_ofs;
        if (csize == usize)
            memcpy(dest, blob, usize);           /* STORED */
        else
            drp_lzss_decode(blob, (unsigned char *)dest, (int)usize);   /* LZSS */
        sat_drp_served++;
        return 1;
    }

    uint32_t at = drp_blob_ofs + data_ofs;       /* Step 3 CD path */

    if (csize == usize)                          /* STORED: read raw straight into dest */
    {
        if (drp_read(at, dest, (int)usize) < (int)usize) return 0;
        sat_drp_served++;
        return 1;
    }

    /* LZSS: read the compressed stream into a transient buffer, decode into dest */
    unsigned char *tmp = (unsigned char *)Z_Malloc((int)csize, PU_STATIC, NULL);
    if (drp_read(at, tmp, (int)csize) < (int)csize) { Z_Free(tmp); return 0; }
    drp_lzss_decode(tmp, (unsigned char *)dest, (int)usize);
    Z_Free(tmp);
    sat_drp_served++;
    return 1;
}

/* Step 4d (STREAMING_ANALYSIS §7.9): will the CD be idle during play?  True when the
** WAD is fully resident (cart raw / small WAD, !streaming), OR per-map cart staging
** is available (streaming + 4MB cart + a valid .DRP) so each map's gameplay lumps
** come from cart instead of the CD.  I_InitSound calls this once to pick CDDA vs the
** MUS synth (the WAD directory is populated well before I_InitSound, so probing the
** .DRP here is safe).  Out-of-subset cold misses (e.g. the MAP30 cast) still touch
** the CD -- masked by the Option-2 music fade (transitions, deferred). */
extern "C" int sat_cd_free_during_play(void)
{
    if (!sat_streaming_mode) return 1;
#if SAT_DRP_CART_STAGE
    if (sat_drp_state == 0) drp_probe();
    return (sat_drp_state == 1 && sat_cart_usable >= 0x400000u) ? 1 : 0;
#else
    return 0;
#endif
}
