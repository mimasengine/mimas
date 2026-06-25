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
** Mimas -- SCSP sound + music backend (SRL build).
**
** SFX  -- Doom's 8-bit unsigned DMX samples converted to signed PCM and
**         cached into 512 KB sound RAM on first use; played on SCSP slots 0-7.
**         Direct SH-2 slot writes (same as SaturnDoom -- no SRL wrapper needed).
**
** Music (MUS sequencer, default) -- SCSP slots 8-22, three PCM waveforms
**         (sawtooth/sine/triangle).  Identical to SaturnDoom; no SRL calls.
**
** Music (CDDA, -DSATURN_CDDA_MUSIC) -- SRL::Sound::Cdda replaces
**         CDC_CdPlay / CDC_CdSeek / slInitSound / slCDDAOn / SDDRVS.DAT.
**         The 68K is left running (SRL inits it); SFX use direct slot writes.
*/
#include <srl.hpp>

extern "C" {
#include <stdio.h>
#include <string.h>
#include "i_sound.h"
#include "doomtype.h"
#include "w_wad.h"
#include "deh_str.h"
#include "m_config.h"
#include "sounds.h"    /* S_music[], NUMMUSIC -- music-number -> CD-track lookup */
}

/* Music backend chosen at RUNTIME in I_InitSound (replaces the old build-time
   -DSATURN_CDDA_MUSIC switch).  Both backends are now compiled in; the public
   I_*Music / I_*Song wrappers near the end dispatch on sat_music_use_cdda:
     1 = CDDA (Red Book audio) -- only when the WAD is fully resident (cart/small
         WAD, CD drive free) AND the disc has audio tracks;
     0 = MUS/SCSP software synth -- the fallback, needs no CD, so it plays during
         big-WAD CD streaming.
   sat_streaming_mode (1 = WAD streamed from CD) is the main input.
   sat_music_use_cdda is DEFINED in core/s_sound.c so the shared core links it. */
extern "C" int sat_streaming_mode;
extern "C" int sat_music_use_cdda;
/* Step 4d: "will the CD be idle during play?" -- !streaming, or big-WAD per-map cart
   staging available (provided by w_drp_saturn.cxx).  Picks CDDA vs the MUS synth. */
extern "C" int sat_cd_free_during_play(void);

static int cdda_track = 0;   /* CDDA: current audio track, 0 = stopped */

/* ------------------------------------------------------------------ */
/* Hardware                                                             */
/* ------------------------------------------------------------------ */

/* SFX debug overlay (off by default; dbg_print shim lives in dg_saturn.cxx):
 *   row 6  SFX#<n> ch<c> ns<samples> r<rate> o<sram_off>  (last sound, or CACHE-FAIL)
 *   row 7  PLAY <8-ch live map>  r<sram high-water>  f<cache failures>
 * Used to localise the silent-SFX bug to MVOL=0 and the rapid-retrigger drop. */
#define SFX_DIAG 0
#if SFX_DIAG
extern "C" void dbg_print(int x, int y, char *str);
#endif

#define SOUND_RAM       0x25A00000u
#define SOUND_RAM_SIZE  0x80000u
#define SCSP_SLOT(n)    ((volatile unsigned short *)(0x25B00000u + ((n) << 5)))
#define SCSP_CONTROL    (*(volatile unsigned short *)0x25B00400u)

#define SMPC_COMREG     (*(volatile unsigned char *)0x2010001Fu)
#define SMPC_SF         (*(volatile unsigned char *)0x20100063u)
#define SMPC_SNDOFF     0x07u

#define R_KYON   0
#define R_SA     1
#define R_LSA    2
#define R_LEA    3
#define R_EG1    4
#define R_EG2    5
#define R_TL     6
#define R_MOD    7
#define R_PITCH  8
#define R_LFO    9
#define R_DSP   10
#define R_MIX   11

#define KYONX        0x1000u
#define KYONB        0x0800u
#define PCM8B        0x0010u
#define LPCTL_LOOP   0x0020u

/* ------------------------------------------------------------------ */
/* SFX                                                                  */
/* ------------------------------------------------------------------ */

#define NUM_CHANNELS 8

extern "C" uint32_t DG_GetTicksMs(void);

typedef struct
{
    unsigned int sram_off;
    unsigned int nsamples;
    unsigned int rate;
} cached_sfx_t;

static cached_sfx_t  sfx_cache[256];
static int           sfx_cache_used = 0;
static unsigned int  sram_alloc     = 0x100;
static uint32_t      chan_end_ms[NUM_CHANNELS];
static boolean       sound_ready    = false;

#if SFX_DIAG
/* "Missing sounds" hunt: I_UpdateSound shows which channels are live + the
 * sound-RAM high-water mark + the running cache-failure count, to see whether
 * sounds actually spread across channels and whether the SFX RAM/cache runs dry. */
static unsigned int   dbg_cachefail = 0;
#endif

extern "C" int   snd_sfxdevice       = SNDDEVICE_SB;
extern "C" int   snd_musicdevice     = SNDDEVICE_SB;
extern "C" int   snd_samplerate      = 11025;
extern "C" int   snd_cachesize       = 0;
extern "C" int   snd_maxslicetime_ms = 0;
extern "C" char *snd_musiccmd        = (char *)"";

/* ------------------------------------------------------------------ */
/* Music sequencer (MUS -- no CDDA)                                    */
/* ------------------------------------------------------------------ */

#define MUS_SLOT_BASE  8
#define MUS_N_SLOTS   15
#define MUS_PERC_CHAN 15
#define WAVE_N        32
#define MUS_HZ       140

#define WAVE_SAW  0
#define WAVE_SINE 1
#define WAVE_TRI  2
#define N_WAVES   3

static const unsigned int note_rate_base[12] = {
    8372,  8870,  9397,  9956,
    10548, 11175, 11840, 12544,
    13290, 14080, 14917, 15804
};

static unsigned short note_pitch[128];
static unsigned int   wave_off[N_WAVES];
static uint8_t        chan_wave[MUS_N_SLOTS];
static int8_t         chan_note[MUS_N_SLOTS];
static uint8_t        chan_vol [MUS_N_SLOTS];
static int            mus_volume = 127;

typedef struct {
    const uint8_t *data;
    int            len;
    int            pos;
    uint32_t       next_tick;
    boolean        looping;
    boolean        playing;
    boolean        paused;
    uint32_t       start_ms;
    uint32_t       paused_at;
    uint32_t       pause_accum;
} mus_state_t;

static mus_state_t mus;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
/* ------------------------------------------------------------------ */

static void smpc_command(unsigned char cmd)
{
    int guard = 1000000;
    while ((SMPC_SF & 1) && guard--) ;
    SMPC_SF     = 1;
    SMPC_COMREG = cmd;
    guard = 1000000;
    while ((SMPC_SF & 1) && guard--) ;
}

static unsigned short pitch_word(unsigned int rate)
{
    int oct = 0;
    unsigned int x = rate, fns;
    if (x == 0) return 0;
    while (x < 44100u && oct > -8) { x <<= 1; oct--; }
    while (x >= 88200u && oct < 7) { x >>= 1; oct++; }
    fns = ((x - 44100u) * 1024u) / 44100u;
    return (unsigned short)(((oct & 0xFu) << 11) | (fns & 0x3FFu));
}

static unsigned short tl_word(int vol)
{
    int tl;
    if (vol <= 0) return 0xFFu;
    tl = (127 - vol) * 5 / 4;
    return (unsigned short)(tl > 255 ? 255 : tl);
}

static unsigned short mix_word(int sep)
{
    int delta = (sep - 128) / 8;
    unsigned short pan;
    if (delta >= 0)
        pan = (unsigned short)(delta > 15 ? 15 : delta);
    else
        pan = (unsigned short)(0x10u | (-delta > 15 ? 15 : -delta));
    return (unsigned short)((7u << 13) | ((unsigned)pan << 8));
}

static cached_sfx_t *cache_sfx(sfxinfo_t *sfxinfo)
{
    cached_sfx_t *c;
    const unsigned char *lump;
    unsigned int lumplen, rate, length, i;
    volatile unsigned short *dst;
    const unsigned char *src;

    if (sfxinfo->driver_data != NULL)
    {
        c = (cached_sfx_t *)sfxinfo->driver_data;
        return c->nsamples ? c : NULL;
    }
    if (sfx_cache_used >= (int)(sizeof(sfx_cache) / sizeof(sfx_cache[0])))
        return NULL;
    c = &sfx_cache[sfx_cache_used++];
    memset(c, 0, sizeof(*c));
    sfxinfo->driver_data = c;

    if (sfxinfo->lumpnum < 0) return NULL;
    /* SATURN LEAK FIX (PU_CACHE=8, was PU_STATIC=1): in CD-streaming mode (no cart)
       W_CacheLumpNum COPIES the lump into the Doom zone.  We immediately upload the
       PCM to the separate SCSP sound RAM below and NEVER read this work-RAM copy
       again (later plays use driver_data->sram_off), so keeping it PU_STATIC leaked
       ~one block per unique sfx -> the zone PU_STATIC floor grew every fight until
       Z_Malloc OOM'd (~1 min into Doom II).  PU_CACHE lets it purge; cart mode is
       unaffected (zero-copy mapped pointer, tag ignored). */
    lump    = (const unsigned char *)W_CacheLumpNum(sfxinfo->lumpnum, 8 /* PU_CACHE */);
    lumplen = W_LumpLength(sfxinfo->lumpnum);
    if (lumplen < 32 || lump[0] != 0x03 || lump[1] != 0x00) return NULL;

    rate   = lump[2] | ((unsigned)lump[3] << 8);
    length = lump[4] | ((unsigned)lump[5] << 8) |
             ((unsigned)lump[6] << 16) | ((unsigned)lump[7] << 24);
    if (length > lumplen - 8 || length <= 48) return NULL;
    length -= 32;
    src = lump + 24;

    if (sram_alloc + length + 2 > SOUND_RAM_SIZE)
    {
        printf("SCSP: sound RAM full, %s skipped\n", sfxinfo->name);
        return NULL;
    }
    dst = (volatile unsigned short *)(SOUND_RAM + sram_alloc);
    for (i = 0; i + 1 < length; i += 2)
        *dst++ = (unsigned short)(((src[i] ^ 0x80u) << 8) | (src[i+1] ^ 0x80u));
    if (i < length)
        *dst++ = (unsigned short)((src[i] ^ 0x80u) << 8);

    c->sram_off = sram_alloc;
    c->nsamples = length;
    c->rate     = rate;
    sram_alloc += (length + 3u) & ~3u;
    return c;
}

/* ------------------------------------------------------------------ */
/* Music init helpers                                                   */
/* ------------------------------------------------------------------ */

static void upload_waveforms(void)
{
    static const unsigned short saw_words[WAVE_N / 2] = {
        0x8088u, 0x9098u, 0xA0A8u, 0xB0B8u,
        0xC0C8u, 0xD0D8u, 0xE0E8u, 0xF0F8u,
        0x0008u, 0x1018u, 0x2028u, 0x3038u,
        0x4048u, 0x5058u, 0x6068u, 0x7078u
    };
    static const unsigned short sine_words[WAVE_N / 2] = {
        0x0019u, 0x3147u, 0x5A6Au, 0x767Du,
        0x7F7Du, 0x766Au, 0x5A47u, 0x3119u,
        0x00E7u, 0xCFB9u, 0xA696u, 0x8A83u,
        0x8183u, 0x8A96u, 0xA6B9u, 0xCFE7u
    };
    static const unsigned short tri_words[WAVE_N / 2] = {
        0x8090u, 0xA0B0u, 0xC0D0u, 0xE0F0u,
        0x0010u, 0x2030u, 0x4050u, 0x6070u,
        0x7F70u, 0x6050u, 0x4030u, 0x2010u,
        0x00F0u, 0xE0D0u, 0xC0B0u, 0xA090u
    };
    const unsigned short *tables[N_WAVES] = { saw_words, sine_words, tri_words };

    for (int w = 0; w < N_WAVES; w++)
    {
        wave_off[w] = sram_alloc;
        volatile unsigned short *dst =
            (volatile unsigned short *)(SOUND_RAM + sram_alloc);
        for (int i = 0; i < WAVE_N / 2; i++)
            *dst++ = tables[w][i];
        sram_alloc += WAVE_N;
    }
}

static void init_note_table(void)
{
    for (int n = 0; n < 128; n++)
    {
        int semi = n % 12;
        int oct  = n / 12;
        unsigned int rate = (oct >= 5) ? note_rate_base[semi] << (oct - 5)
                                       : note_rate_base[semi] >> (5 - oct);
        note_pitch[n] = pitch_word(rate);
    }
}

static int patch_to_wave(unsigned int patch)
{
    unsigned int cls = patch / 8u;
    switch (cls)
    {
    case 5: case 6: case 11: return WAVE_SINE;
    case 8: case 9:          return WAVE_TRI;
    default:                 return WAVE_SAW;
    }
}

/* ------------------------------------------------------------------ */
/* SCSP note control                                                    */
/* ------------------------------------------------------------------ */

static void music_note_off(int chan)
{
    if (chan_note[chan] < 0) return;
    volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + chan);
    slot[R_KYON] = (unsigned short)((slot[R_KYON] & ~(KYONB | KYONX)) | KYONX);
    chan_note[chan] = -1;
}

static void music_note_on(int chan, int note, int vol)
{
    if ((unsigned)chan >= MUS_N_SLOTS) return;
    music_note_off(chan);

    volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + chan);
    int eff_vol = (int)(chan_vol[chan] = (uint8_t)vol) * mus_volume / 127;
    unsigned int woff = wave_off[chan_wave[chan]];

    slot[R_SA]    = (unsigned short)(woff & 0xFFFFu);
    slot[R_LSA]   = (unsigned short)(woff & 0xFFFFu);
    slot[R_LEA]   = (unsigned short)((woff + WAVE_N - 1u) & 0xFFFFu);
    slot[R_EG1]   = 0x001Fu;
    slot[R_EG2]   = 0x0008u;
    slot[R_TL]    = tl_word(eff_vol);
    slot[R_MOD]   = 0;
    slot[R_PITCH] = note_pitch[note & 0x7F];
    slot[R_LFO]   = 0;
    slot[R_DSP]   = 0;
    slot[R_MIX]   = 0xE000u;

    unsigned short kyon = (unsigned short)(KYONX | KYONB | PCM8B | LPCTL_LOOP |
                                           ((woff >> 16) & 0xFu));
    slot[R_KYON] = kyon;
    chan_note[chan] = (int8_t)note;
}

/* ------------------------------------------------------------------ */
/* MUS sequencer step                                                   */
/* ------------------------------------------------------------------ */

static void mus_step(uint32_t now_ms)
{
    const uint8_t *d = mus.data;
    uint8_t evb, key, ctrl, cval;
    int chan, type, hit_end;
    uint32_t cur_tick, timedelay;
    unsigned int scorestart;

    if (!mus.playing || mus.paused || !d) return;

    cur_tick = (now_ms - mus.start_ms - mus.pause_accum) * MUS_HZ / 1000u;

    while (cur_tick >= mus.next_tick)
    {
        hit_end = 0;
        for (;;)
        {
            if (mus.pos >= mus.len) { I_StopSong(); return; }
            evb  = d[mus.pos++];
            chan = evb & 0x0F;
            type = evb & 0x70;
            switch (type)
            {
            case 0x00:
                key = d[mus.pos++] & 0x7F;
                (void)key;
                if (chan != MUS_PERC_CHAN && chan < MUS_N_SLOTS) music_note_off(chan);
                break;
            case 0x10:
                key = d[mus.pos++];
                if (key & 0x80u) { cval = d[mus.pos++] & 0x7Fu; if (chan < MUS_N_SLOTS) chan_vol[chan] = cval; }
                if (chan != MUS_PERC_CHAN && chan < MUS_N_SLOTS) music_note_on(chan, key & 0x7Fu, chan_vol[chan]);
                break;
            case 0x20: mus.pos++; break;
            case 0x30: mus.pos++; break;
            case 0x40:
                ctrl = d[mus.pos++]; cval = d[mus.pos++] & 0x7Fu;
                if (ctrl == 0 && chan < MUS_N_SLOTS) chan_wave[chan] = (uint8_t)patch_to_wave(cval);
                else if (ctrl == 3 && chan < MUS_N_SLOTS)
                {
                    chan_vol[chan] = cval;
                    if (chan_note[chan] >= 0)
                    {
                        volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + chan);
                        slot[R_TL] = tl_word((int)cval * mus_volume / 127);
                    }
                }
                break;
            case 0x60: hit_end = 1; break;
            default: break;
            }
            if (hit_end || (evb & 0x80u)) break;
        }
        if (hit_end)
        {
            if (mus.looping)
            {
                scorestart  = (unsigned int)(d[6] | ((unsigned)d[7] << 8));
                mus.pos       = (int)scorestart;
                mus.next_tick = cur_tick;
                continue;
            }
            else { I_StopSong(); return; }
        }
        timedelay = 0;
        for (;;)
        {
            if (mus.pos >= mus.len) { I_StopSong(); return; }
            cval = d[mus.pos++];
            timedelay = timedelay * 128u + (cval & 0x7Fu);
            if ((cval & 0x80u) == 0) break;
        }
        mus.next_tick += timedelay;
    }
}

/* ================================================================== */
/* Doom I_ interface -- SFX                                            */
/* ================================================================== */

/* Does the disc have CDDA audio tracks (a track of type Audio after the data
 * track)?  A data-only disc -- the big-WAD streaming disc, or a small-WAD build
 * pressed without Red Book audio -- returns false, so the runtime music backend
 * falls back to the MUS/SCSP synth instead of playing silence.  Checks the
 * well-defined LastTrack (audio tracks follow the data track, so the last track is
 * Audio iff the disc has any); defensive -- any TOC anomaly reads as no-audio
 * (-> MUS).  Safe here: SRL::Cd::Initialize ran in DG_Init before I_InitSound. */
static bool cdda_has_audio_tracks(void)
{
    SRL::Cd::TableOfContents toc = SRL::Cd::TableOfContents::GetTable();
    return toc.LastTrack.Number >= 2 &&
           toc.LastTrack.GetType() == SRL::Cd::TableOfContents::TrackType::Audio;
}

extern "C" void I_InitSound(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;

    /* Decide the music backend NOW, before the SCSP/68K setup (which differs per
     * backend and is mutually exclusive -- CDDA keeps the 68K running for CD-DA
     * mixing, MUS halts it and uploads waveforms).  CDDA only when the CD is free
     * during play AND the disc has audio tracks; otherwise the MUS/SCSP synth.
     * "CD free" = WAD fully resident (cart raw / small WAD) OR -- Step 4d -- big-WAD
     * per-map cart staging (streaming + 4MB cart + valid .DRP; the map's lumps come
     * from cart, not the CD).  sat_streaming_mode is final by now (set in DG_Init,
     * before doom_start calls I_InitSound), and the WAD directory is loaded, so the
     * .DRP probe inside sat_cd_free_during_play is safe here. */
    sat_music_use_cdda = sat_cd_free_during_play() && cdda_has_audio_tracks();

    if (sat_music_use_cdda)
    {
        /* CDDA: SRL already initialised sound (SRL::Core::Initialize ->
           Sound::Hardware::Initialize); the 68K stays running.  Reserve the SRL
           driver area, clear only SFX slots 0-7, and force MVOL=15 (SGL leaves it
           0 -> muted SFX) preserving MEM4MB/DAC18B. */
        sram_alloc = 0x1000;
        for (int s = 0; s < 8; ++s)
        {
            volatile unsigned short *slot = SCSP_SLOT(s);
            for (int r = 0; r < 16; ++r) slot[r] = 0;
            slot[R_EG2] = 0x001Fu;
        }
        SCSP_CONTROL = (unsigned short)((SCSP_CONTROL & 0xFFF0u) | 0x000Fu);
        sound_ready = true;
        printf("I_InitSound: CDDA (SRL), SFX sram @ 0x%x, ctl=0x%x\n",
               (unsigned)sram_alloc, (unsigned)SCSP_CONTROL);
        return;
    }

    /* MUS/SCSP synth: halt the 68K, set the SCSP fresh, clear all 32 slots, and
       upload the saw/sine/tri waveforms used by the music voices (slots 8-22). */
    smpc_command(SMPC_SNDOFF);
    SCSP_CONTROL = (1u << 9) | 0xFu;

    for (int s = 0; s < 32; ++s)
    {
        volatile unsigned short *slot = SCSP_SLOT(s);
        for (int r = 0; r < 16; ++r) slot[r] = 0;
        slot[R_EG2] = 0x001Fu;
    }
    upload_waveforms();
    sound_ready = true;
    printf("I_InitSound: MUS/SCSP synth (68K halted), waves @ 0x%x/0x%x/0x%x\n",
           wave_off[WAVE_SAW], wave_off[WAVE_SINE], wave_off[WAVE_TRI]);
}

extern "C" void I_ShutdownSound(void) {}

extern "C" int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[9];
    if (sfxinfo->link != NULL) sfxinfo = sfxinfo->link;
    snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    return W_CheckNumForName(namebuf);
}

extern "C" void I_UpdateSound(void)
{
    if (!sat_music_use_cdda && mus.playing && !mus.paused)
        mus_step(DG_GetTicksMs());
#if SFX_DIAG
    /* Row 7: per-channel live map. For each channel 0-7 show its index while it
     * is still inside its playback window (our chan_end_ms bookkeeping = Doom
     * thinks it's playing), else '.'. Then sound-RAM high-water (r) and the
     * running cache-failure count (f). Lets us see whether sounds actually
     * spread across channels and whether the SFX RAM/cache is running dry. */
    {
        uint32_t now = DG_GetTicksMs();
        char map[NUM_CHANNELS + 1];
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
            map[ch] = (now < chan_end_ms[ch]) ? (char)('0' + ch) : '.';
        map[NUM_CHANNELS] = 0;
        static char ln7[45];
        snprintf(ln7, sizeof ln7, "PLAY %s  r%05x f%u   ", map,
                 (unsigned)sram_alloc, dbg_cachefail);
        dbg_print(0, 7, ln7);
    }
#endif
}

extern "C" void I_UpdateSoundParams(int channel, int vol, int sep)
{
    if (!sound_ready || channel < 0 || channel >= NUM_CHANNELS) return;
    volatile unsigned short *slot = SCSP_SLOT(channel);
    slot[R_TL]  = tl_word(vol);
    slot[R_MIX] = mix_word(sep);
}

extern "C" int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (!sound_ready || channel < 0 || channel >= NUM_CHANNELS) return -1;
    if (sfxinfo->link != NULL) sfxinfo = sfxinfo->link;
    cached_sfx_t *c = cache_sfx(sfxinfo);
#if SFX_DIAG
    { static unsigned int n = 0; static char sb[45];
      if (c) snprintf(sb, sizeof sb, "SFX#%u ch%d ns%u r%u o%05x  ",
                      ++n, channel, c->nsamples, c->rate, c->sram_off);
      else { ++dbg_cachefail;
             snprintf(sb, sizeof sb, "SFX#%u ch%d CACHE-FAIL       ", ++n, channel); }
      dbg_print(0, 6, sb); }
#endif
    if (!c) return -1;

    if (sat_music_use_cdda)
        /* Defensive: keep the SCSP master volume up in case the SGL driver zeroed
         * it after I_InitSound (see the MVOL note there). */
        SCSP_CONTROL = (unsigned short)((SCSP_CONTROL & 0xFFF0u) | 0x000Fu);
    volatile unsigned short *slot = SCSP_SLOT(channel);

    /* Restart cleanly on a slot that may already be playing (rapid same-channel
     * retrigger, e.g. continuous pistol fire on ch0): key the slot OFF and let
     * the SCSP COMMIT that key-off (KYONEX clears on its internal scan) BEFORE
     * we reprogram and key ON. Without the wait the off+on KYONEX commits land
     * in the same scan window, coalesce into "stay/go off", and the shot is
     * silently dropped. Poll KYONEX-clear with a minimum settle floor (in case
     * the bit isn't read-backable) and a hard cap. */
    slot[R_KYON] = (unsigned short)((slot[R_KYON] & ~(KYONB | KYONX)) | KYONX);
    for (int g = 0; g < 600; ++g)
        if (!(slot[R_KYON] & KYONX) && g >= 64) break;

    slot[R_SA]    = (unsigned short)(c->sram_off & 0xFFFFu);
    slot[R_LSA]   = 0;
    slot[R_LEA]   = (unsigned short)(c->nsamples - 1u);
    slot[R_EG1]   = 0x001Fu;
    slot[R_EG2]   = 0x001Fu;
    slot[R_TL]    = tl_word(vol);
    slot[R_MOD]   = 0;
    slot[R_PITCH] = pitch_word(c->rate);
    slot[R_LFO]   = 0;
    slot[R_DSP]   = 0;
    slot[R_MIX]   = mix_word(sep);

    unsigned short kyon = (unsigned short)(KYONX | KYONB | PCM8B |
                                           ((c->sram_off >> 16) & 0xFu));
    slot[R_KYON] = kyon;
    chan_end_ms[channel] = DG_GetTicksMs() + (c->nsamples * 1000u) / c->rate;
    return channel;
}

extern "C" void I_StopSound(int channel)
{
    if (!sound_ready || channel < 0 || channel >= NUM_CHANNELS) return;
    volatile unsigned short *slot = SCSP_SLOT(channel);
    slot[R_KYON] = (unsigned short)((slot[R_KYON] & ~(KYONB | KYONX)) | KYONX);
    chan_end_ms[channel] = 0;
}

extern "C" boolean I_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return DG_GetTicksMs() < chan_end_ms[channel];
}

extern "C" void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;
}

/* ================================================================== */
/* Doom I_ interface -- Music                                          */
/* ================================================================== */

/* ================================================================== */
/* Doom I_ interface -- Music  (runtime backend: MUS-synth or CDDA)    */
/* ================================================================== */
/* Both backends are compiled in; the public extern "C" I_* wrappers at the end
   dispatch on sat_music_use_cdda (decided in I_InitSound).  The per-backend impls
   are static.  mus_step() (above) and mus_PlaySong() call the PUBLIC I_StopSong
   wrapper -- correct, because they only ever run while sat_music_use_cdda==0. */

/* ---- MUS / SCSP software synth ------------------------------------ */

static void mus_InitMusic(void)
{
    init_note_table();
    memset(&mus, 0, sizeof(mus));
    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        chan_note[i] = -1;
        chan_vol[i]  = 100;
        chan_wave[i] = WAVE_SAW;
    }
}

static void mus_SetMusicVolume(int volume)
{
    mus_volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        if (chan_note[i] >= 0)
        {
            volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + i);
            slot[R_TL] = tl_word((int)chan_vol[i] * mus_volume / 127);
        }
    }
}

static void mus_PauseSong(void)
{
    if (!mus.playing || mus.paused) return;
    mus.paused    = true;
    mus.paused_at = DG_GetTicksMs();
    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        if (chan_note[i] >= 0)
        {
            volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + i);
            slot[R_KYON] = (unsigned short)((slot[R_KYON] & ~(KYONB | KYONX)) | KYONX);
        }
    }
}

static void mus_ResumeSong(void)
{
    if (!mus.playing || !mus.paused) return;
    mus.pause_accum += DG_GetTicksMs() - mus.paused_at;
    mus.paused = false;
}

static void *mus_RegisterSong(void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    if (len < 16 || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1Au)
    {
        printf("I_RegisterSong: not a MUS file\n");
        return NULL;
    }
    return data;
}

static void mus_UnRegisterSong(void *handle) { (void)handle; }

static void mus_PlaySong(void *handle, boolean looping)
{
    if (!handle || !sound_ready) return;
    I_StopSong();

    const uint8_t *d = (const uint8_t *)handle;
    unsigned int scorestart = (unsigned int)(d[6] | ((unsigned)d[7] << 8));
    unsigned int scorelen   = (unsigned int)(d[4] | ((unsigned)d[5] << 8));

    memset(&mus, 0, sizeof(mus));
    mus.data        = d;
    mus.len         = (int)(scorestart + scorelen);
    mus.pos         = (int)scorestart;
    mus.next_tick   = 0;
    mus.looping     = looping;
    mus.playing     = true;
    mus.paused      = false;
    mus.start_ms    = DG_GetTicksMs();
    mus.pause_accum = 0;

    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        chan_note[i] = -1;
        chan_vol[i]  = 100;
        chan_wave[i] = WAVE_SAW;
    }
}

static void mus_StopSong(void)
{
    if (!mus.playing) return;
    mus.playing = false;
    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        if (chan_note[i] >= 0)
        {
            volatile unsigned short *slot = SCSP_SLOT(MUS_SLOT_BASE + i);
            slot[R_KYON] = (unsigned short)((slot[R_KYON] & ~(KYONB | KYONX)) | KYONX);
            chan_note[i] = -1;
        }
    }
}

static boolean mus_MusicIsPlaying(void) { return mus.playing; }

/* ---- CDDA backend via SRL::Sound::Cdda ---------------------------- */
/* Track assignment (track 01 = the data/WAD track): musicnum 1 (mus_e1m1) -> CD
   track 02, etc.  The musicnum->track map below is hardcoded (Doom-1 E1 only); a
   data-driven, per-WAD mapping file is the documented follow-up (see
   STREAMING_ANALYSIS.md S4d / the music-mapping note). */

static void cdda_load_trackmap(void);   /* fwd: data-driven musicnum->track map */

static void cdda_InitMusic(void)
{
    cdda_track = 0;
    if (sat_music_use_cdda)        /* read the disc's CDDAMAP.TXT only when CDDA is active */
        cdda_load_trackmap();
}

static void cdda_SetMusicVolume(int volume)
{
    /* SGL CD-DA level is 0-7 (SND_SetCdDaLev; 7 = max), NOT 0-127/0-255.  Doom's music
       volume arrives 0-127 (the menu sends musicVolume*8 = 0..120).  The old `volume*2`
       fed up to 254 into a 3-bit field -> low bits 0 at most settings -> SILENT.  Map
       0-127 -> 0-7, and floor a non-zero request to 1 so it never falls to silence. */
    int v = (volume * 7) / 127;
    if (v == 0 && volume > 0) v = 1;
    if (v > 7) v = 7;
    SRL::Sound::Cdda::SetVolume((uint8_t)v);
}

static void cdda_PauseSong(void)
{
    if (!cdda_track) return;
    SRL::Sound::Cdda::StopPause();
}

static void cdda_ResumeSong(void)
{
    if (!cdda_track) return;       /* no-op when CDDA isn't the active backend (cdda_track==0) */
    SRL::Sound::Cdda::Resume();
}

/* musicnum -> CD audio track, built by cdda_load_trackmap() (defaults below, can be
 * overridden per-WAD by a CDDAMAP.TXT data file on the disc -- STREAMING_ANALYSIS 7.7). */
static uint8_t cdda_trackmap[NUMMUSIC];

static int cdda_name_eq(const char *a, const char *b)   /* case-insensitive, NUL-terminated */
{
    if (!a || !b) return 0;
    for (;; a++, b++)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

static void cdda_load_trackmap(void)
{
    /* Built-in default (Doom-1 episode 1 + the inter/intro cluster; track 01 is the
       data/WAD track, audio is 02+).  An absent CDDAMAP.TXT -> exactly this map, so
       the current data-only disc is unaffected. */
    static const uint8_t defmap[] = {
        0,
        2, 3, 4, 5, 6, 7, 8, 9, 10,   /* 1-9  e1m1-e1m9  */
        0, 0, 0, 0, 0, 0, 0, 0, 0,    /* 10-18 e2m1-e2m9 */
        0, 0, 0, 0, 0, 0, 0, 0, 0,    /* 19-27 e3m1-e3m9 */
        11, 12, 13, 14, 15,            /* 28-32 inter/intro/bunny/victor/introa */
    };
    int i, ndef = (int)(sizeof defmap / sizeof defmap[0]);
    for (i = 0; i < NUMMUSIC; i++)
        cdda_trackmap[i] = (i < ndef) ? defmap[i] : 0;

    /* Optional per-WAD override: a "<music-name> <track>" text file on the disc
       ('#' line comments).  <music-name> matches S_music[].name (the part after
       d_), case-insensitive -> lets a disc builder map any WAD's music to audio
       tracks with no code change.  See STREAMING_ANALYSIS.md 7.7. */
    SRL::Cd::File f("CDDAMAP.TXT");
    if (!f.Exists()) return;
    static unsigned char buf[2048] __attribute__((aligned(4)));
    int got = f.LoadBytes(0, (int32_t)(sizeof buf - 1), buf);
    if (got <= 0) return;
    buf[got] = 0;

    int p = 0, overrides = 0;
    while (p < got)
    {
        char nm[16];
        int nl = 0, trk = 0, td = 0;
        while (p < got && (buf[p]==' '||buf[p]=='\t'||buf[p]=='\r'||buf[p]=='\n')) p++;
        if (p < got && buf[p] == '#') { while (p < got && buf[p] != '\n') p++; continue; }
        while (p < got && buf[p] > ' ' && nl < (int)sizeof(nm) - 1) nm[nl++] = (char)buf[p++];
        nm[nl] = 0;
        while (p < got && buf[p] > ' ') p++;                 /* drop any overlong-name tail */
        while (p < got && (buf[p]==' '||buf[p]=='\t')) p++;
        while (p < got && buf[p] >= '0' && buf[p] <= '9') { trk = trk*10 + (buf[p]-'0'); p++; td++; }
        while (p < got && buf[p] != '\n') p++;               /* to end of line */
        if (!nl || !td || trk < 0 || trk > 99) continue;
        for (i = 1; i < NUMMUSIC; i++)
            if (cdda_name_eq(nm, S_music[i].name)) { cdda_trackmap[i] = (uint8_t)trk; overrides++; break; }
    }
    printf("CDDAMAP.TXT: %d track override(s)\n", overrides);
}

static void *cdda_RegisterSong(void *data, int len)
{
    (void)len;
    for (int i = 1; i < NUMMUSIC; i++)
        if (S_music[i].data == data)
            return (void *)(intptr_t)cdda_trackmap[i];
    return NULL;
}

static void cdda_UnRegisterSong(void *handle) { (void)handle; }

static void cdda_PlaySong(void *handle, boolean looping)
{
    if (!handle || !sound_ready) return;
    int track = (int)(intptr_t)handle;
    if (track < 2) return;
    cdda_track = track;
    SRL::Sound::Cdda::PlaySingle((uint16_t)track, looping);
}

static void cdda_StopSong(void)
{
    if (!cdda_track) return;
    cdda_track = 0;
    SRL::Sound::Cdda::StopPause();
}

static boolean cdda_MusicIsPlaying(void) { return cdda_track > 0; }

/* ---- public dispatch (branch on the runtime backend) ------------- */

extern "C" void I_InitMusic(void)
{
    /* Init BOTH: the MUS table init is cheap and touches no HW that conflicts with
       the CDDA path (the exclusive 68K/SCSP choice was already made in I_InitSound);
       the CDDA reset is a scalar.  Leaves whichever backend is selected ready. */
    mus_InitMusic();
    cdda_InitMusic();
    printf("I_InitMusic: %s\n", sat_music_use_cdda ? "CDDA (SRL)" : "MUS/SCSP synth");
}

extern "C" void I_ShutdownMusic(void) { I_StopSong(); }

extern "C" void I_SetMusicVolume(int volume)
{ if (sat_music_use_cdda) cdda_SetMusicVolume(volume); else mus_SetMusicVolume(volume); }

extern "C" void I_PauseSong(void)
{ if (sat_music_use_cdda) cdda_PauseSong(); else mus_PauseSong(); }

extern "C" void I_ResumeSong(void)
{ if (sat_music_use_cdda) cdda_ResumeSong(); else mus_ResumeSong(); }

extern "C" void *I_RegisterSong(void *data, int len)
{ return sat_music_use_cdda ? cdda_RegisterSong(data, len) : mus_RegisterSong(data, len); }

extern "C" void I_UnRegisterSong(void *handle)
{ if (sat_music_use_cdda) cdda_UnRegisterSong(handle); else mus_UnRegisterSong(handle); }

extern "C" void I_PlaySong(void *handle, boolean looping)
{ if (sat_music_use_cdda) cdda_PlaySong(handle, looping); else mus_PlaySong(handle, looping); }

extern "C" void I_StopSong(void)
{ if (sat_music_use_cdda) cdda_StopSong(); else mus_StopSong(); }

extern "C" boolean I_MusicIsPlaying(void)
{ return sat_music_use_cdda ? cdda_MusicIsPlaying() : mus_MusicIsPlaying(); }

extern "C" void I_BindSoundVariables(void)
{
    M_BindVariable("snd_musicdevice",     &snd_musicdevice);
    M_BindVariable("snd_sfxdevice",       &snd_sfxdevice);
    M_BindVariable("snd_samplerate",      &snd_samplerate);
    M_BindVariable("snd_cachesize",       &snd_cachesize);
    M_BindVariable("snd_maxslicetime_ms", &snd_maxslicetime_ms);
    M_BindVariable("snd_musiccmd",        &snd_musiccmd);
}
