/*
** DoomSRL -- SCSP sound + music backend (SRL build).
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
#ifdef SATURN_CDDA_MUSIC
#include "sounds.h"    /* S_music[], NUMMUSIC */
static int cdda_track = 0;
#endif
}

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

extern "C" void I_InitSound(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;

#ifdef SATURN_CDDA_MUSIC
    /* CDDA mode: SRL already initialised sound (SRL::Core::Initialize called
       SRL::Sound::Hardware::Initialize).  The 68K is running.
       Place SFX allocator past a safe offset to avoid the SRL sound driver area.
       Clear SFX slots 0-7 from SH-2. */
    sram_alloc = 0x1000;   /* reserve first 4 KB for SRL sound driver */
    for (int s = 0; s < 8; ++s)
    {
        volatile unsigned short *slot = SCSP_SLOT(s);
        for (int r = 0; r < 16; ++r) slot[r] = 0;
        slot[R_EG2] = 0x001Fu;
    }
    /* SRL/SGL leaves the SCSP master volume at 0 in CDDA mode (CD-DA is mixed
     * on a separate path, so music is audible while every slot stays muted --
     * that's the SFX bug). Force MVOL=15, preserving MEM4MB/DAC18B as set by
     * the SGL driver. */
    SCSP_CONTROL = (unsigned short)((SCSP_CONTROL & 0xFFF0u) | 0x000Fu);
    sound_ready = true;
    printf("I_InitSound: CDDA (SRL), SFX sram @ 0x%x, ctl=0x%x\n",
           (unsigned)sram_alloc, (unsigned)SCSP_CONTROL);
    return;
#endif

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
    printf("I_InitSound: SCSP direct (68K halted), waves @ 0x%x/0x%x/0x%x\n",
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
#ifndef SATURN_CDDA_MUSIC
    if (mus.playing && !mus.paused)
        mus_step(DG_GetTicksMs());
#endif
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

#ifdef SATURN_CDDA_MUSIC
    /* Defensive: keep the SCSP master volume up in case the SGL driver zeroed
     * it after I_InitSound (see the MVOL note there). */
    SCSP_CONTROL = (unsigned short)((SCSP_CONTROL & 0xFFF0u) | 0x000Fu);
#endif
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

#ifndef SATURN_CDDA_MUSIC
/* ---- MUS sequencer ------------------------------------------------ */

extern "C" void I_InitMusic(void)
{
    init_note_table();
    memset(&mus, 0, sizeof(mus));
    for (int i = 0; i < MUS_N_SLOTS; i++)
    {
        chan_note[i] = -1;
        chan_vol[i]  = 100;
        chan_wave[i] = WAVE_SAW;
    }
    printf("I_InitMusic: MUS/SCSP sequencer (%d channels)\n", MUS_N_SLOTS);
}

extern "C" void I_ShutdownMusic(void) { I_StopSong(); }

extern "C" void I_SetMusicVolume(int volume)
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

extern "C" void I_PauseSong(void)
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

extern "C" void I_ResumeSong(void)
{
    if (!mus.playing || !mus.paused) return;
    mus.pause_accum += DG_GetTicksMs() - mus.paused_at;
    mus.paused = false;
}

extern "C" void *I_RegisterSong(void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    if (len < 16 || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1Au)
    {
        printf("I_RegisterSong: not a MUS file\n");
        return NULL;
    }
    return data;
}

extern "C" void I_UnRegisterSong(void *handle) { (void)handle; }

extern "C" void I_PlaySong(void *handle, boolean looping)
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

extern "C" void I_StopSong(void)
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

extern "C" boolean I_MusicIsPlaying(void) { return mus.playing; }

#else /* SATURN_CDDA_MUSIC */
/* ---- CDDA backend via SRL::Sound::Cdda ---------------------------- */

/* Track assignment (track 01 = data):
     musicnum  1 (mus_e1m1)   -> CD track 02
     musicnum 32 (mus_introa) -> CD track 33 */
extern "C" int sat_streaming_mode;

extern "C" void I_InitMusic(void)
{
    cdda_track = 0;
    printf("I_InitMusic: CDDA via SRL (%d tracks)\n", NUMMUSIC - 1);
}

extern "C" void I_ShutdownMusic(void) { I_StopSong(); }

extern "C" void I_SetMusicVolume(int volume)
{
    /* SRL::Sound::Cdda::SetVolume expects 0-255; Doom passes 0-127. */
    SRL::Sound::Cdda::SetVolume((uint8_t)(volume * 2));
}

extern "C" void I_PauseSong(void)
{
    if (!cdda_track) return;
    SRL::Sound::Cdda::StopPause();
}

extern "C" void I_ResumeSong(void)
{
    if (sat_streaming_mode || !cdda_track) return;
    SRL::Sound::Cdda::Resume();
}

extern "C" void *I_RegisterSong(void *data, int len)
{
    static const uint8_t cdda_track_map[] = {
        0,
        2, 3, 4, 5, 6, 7, 8, 9, 10,   /* 1-9  e1m1-e1m9  */
        0, 0, 0, 0, 0, 0, 0, 0, 0,    /* 10-18 e2m1-e2m9 */
        0, 0, 0, 0, 0, 0, 0, 0, 0,    /* 19-27 e3m1-e3m9 */
        11, 12, 13, 14, 15,            /* 28-32 inter/intro/bunny/victor/introa */
    };
    (void)len;
    for (int i = 1; i < NUMMUSIC; i++)
    {
        if (S_music[i].data == data)
        {
            if (i < (int)(sizeof(cdda_track_map) / sizeof(cdda_track_map[0])))
                return (void *)(intptr_t)cdda_track_map[i];
            return NULL;
        }
    }
    return NULL;
}

extern "C" void I_UnRegisterSong(void *handle) { (void)handle; }

extern "C" void I_PlaySong(void *handle, boolean looping)
{
    if (sat_streaming_mode || !handle || !sound_ready) return;
    int track = (int)(intptr_t)handle;
    if (track < 2) return;
    cdda_track = track;
    SRL::Sound::Cdda::PlaySingle((uint16_t)track, looping);
}

extern "C" void I_StopSong(void)
{
    if (!cdda_track) return;
    cdda_track = 0;
    SRL::Sound::Cdda::StopPause();
}

extern "C" boolean I_MusicIsPlaying(void) { return cdda_track > 0; }

#endif /* SATURN_CDDA_MUSIC */

extern "C" void I_BindSoundVariables(void)
{
    M_BindVariable("snd_musicdevice",     &snd_musicdevice);
    M_BindVariable("snd_sfxdevice",       &snd_sfxdevice);
    M_BindVariable("snd_samplerate",      &snd_samplerate);
    M_BindVariable("snd_cachesize",       &snd_cachesize);
    M_BindVariable("snd_maxslicetime_ms", &snd_maxslicetime_ms);
    M_BindVariable("snd_musiccmd",        &snd_musiccmd);
}
