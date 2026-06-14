/*
** DoomSRL -- WAD file backend (SRL build).
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
}

extern "C" unsigned char *sat_wad_base;
extern "C" unsigned int   sat_wad_size;

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

    if (!wad_cd_file->Exists())
    {
        printf("CD: DOOM1.WAD not found\n");
        wad_cd_file = nullptr;
        return 0;
    }
    wad_cd_file->Open();

    /* Determine size: read in 2048-byte chunks until EOF to count bytes. */
    static unsigned char probe[2048];
    unsigned int total = 0;
    while (!wad_cd_file->IsEOF())
    {
        int n = wad_cd_file->Read(2048, probe);
        if (n <= 0) break;
        total += (unsigned int)n;
    }
    /* Seek back to beginning for subsequent random-access reads */
    wad_cd_file->Seek(0);

    sat_wad_size = total;
    sat_wad_base = NULL;   /* signals CD mode to Saturn_Read */
    printf("CD: WAD %u bytes\n", sat_wad_size);
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

    /* CD mode: SRL::Cd::File::LoadBytes for byte-offset random access */
    if (!wad_cd_file) return 0;
    int got = wad_cd_file->LoadBytes((size_t)offset, (int32_t)n, buffer);
    return (got > 0) ? (size_t)got : 0;
}
