/*
** DoomSRL -- per-level repack (.DRP) loader  (STREAMING_ANALYSIS.md §7.4/7.9-7.11).
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

/* ------------------------------------------------------------------ */
/* little-endian byte assembly (endian-safe on the big-endian SH-2)    */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

static uint32_t       drp_n_maps      = 0;
static unsigned char *drp_map_tab     = nullptr;  /* n_maps * 24, PU_STATIC */

/* current selected map */
static unsigned char *drp_entries     = nullptr;  /* n_entries * 16, PU_STATIC */
static int            drp_n_entries   = 0;
static uint32_t       drp_blob_ofs    = 0;

extern "C" int sat_drp_read_retries;
int sat_drp_read_retries = 0;
#define DRP_READ_RETRIES 8

static int drp_load(size_t sector, int32_t bytes, void *dst)
{
    int got = drp_file->LoadBytes(sector, bytes, dst);
    for (int a = 1; got <= 0 && a < DRP_READ_RETRIES; ++a)
    {
        sat_drp_read_retries++;
        got = drp_file->LoadBytes(sector, bytes, dst);
    }
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
        int got = drp_load(sector, (int32_t)len, buffer);
        return got > 0 ? got : 0;
    }

    static unsigned char sect_buf[2048] __attribute__((aligned(4)));
    int done = 0;
    while (done < len)
    {
        int got = drp_load(sector, 2048, sect_buf);
        if (got <= (int)sub) break;
        int avail = got - (int)sub;
        int want  = len - done;
        if (want > avail) want = avail;
        memcpy((unsigned char *)buffer + done, sect_buf + sub, want);
        done += want;
        if (want < avail) break;
        sector++;
        sub = 0;
    }
    return done;
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
    sat_drp_state = 1;
    printf("DRP: active, %u maps (repacked streaming)\n", (unsigned)n_maps);
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

/* ------------------------------------------------------------------ */
/* core hooks (declared extern "C" in core, gated by -DSAT_REPACK)      */
/* ------------------------------------------------------------------ */

/* P_SetupLevel: point the loader at this map's per-map blob.  `lumpname` is the
** engine's map marker name ("map01" / "e1m1", lowercase). */
extern "C" void sat_drp_select_map(const char *lumpname)
{
    if (sat_drp_state == 0) drp_probe();

    /* release the previous map's entry table */
    if (drp_entries) { Z_Free(drp_entries); drp_entries = nullptr; }
    drp_n_entries = 0;
    drp_blob_ofs  = 0;
    if (sat_drp_state != 1) return;

    for (uint32_t m = 0; m < drp_n_maps; m++)
    {
        const unsigned char *rec = drp_map_tab + m * DRP_MAP_SZ;
        if (!name8_ieq(lumpname, rec)) continue;

        uint32_t n_entries   = rd32(rec + 8);
        uint32_t entries_ofs = rd32(rec + 12);
        uint32_t blob_ofs    = rd32(rec + 16);
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

    uint32_t at = drp_blob_ofs + data_ofs;

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
