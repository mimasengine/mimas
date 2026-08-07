# Disc image set — one image per witness WAD

Built 2026-07-30 from branch `flicker-clean`. Every image is a MODE1/2352 `.bin` +
`.cue` pair named after the IWAD it carries, stashed in `build/wads/<wad>/`. Launch
any of them without rebuilding:

```powershell
powershell -ExecutionPolicy Bypass -File run_ymir.ps1 -Wad Doom2
```

`run_ymir.ps1` resolves the `.cue` by search (newest wins), so the descriptive names
need no bookkeeping. Rebuild one with `build.ps1 -Wad <name> -Repack` (add `-Cdda`
for an audio disc).

## The images

| Image | IWAD (`wads_temoins/`) | Music | Size | Witness maps |
|---|---|---|---|---|
| `Mimas-Doom1s` | `Doom1s.wad` — shareware | MUS synth | 20 MB | E1M8 (drawsegs/solidsegs), E1M7 |
| `Mimas-Doom1s-CDDA` | `Doom1s.wad` — shareware | **CD-DA** (track 02) | 20 MB | same, with Red Book audio |
| `Mimas-Doom-ud` | `Doom-ud.wad` — Ultimate Doom E1–E4 | MUS synth | 102 MB | E2M5 (heap), E3M6 + E4M2 (open vistas) |
| `Mimas-Doom2` | `Doom2.wad` — Doom II | MUS synth | 107 MB | MAP15 (walls), MAP29 (visplanes), MAP08, MAP13 |
| `Mimas-Tnt` | `Tnt.wad` — Final Doom | MUS synth | 136 MB | MAP27 (only confirmed IWAD visplane overflow, 332 monsters) |
| `Mimas-Plutonia` | `Plutonia.wad` — Final Doom | MUS synth | 141 MB | MAP11 (pure AI), MAP32 (densest legal combat) |
| `Mimas-Doom2SCYTHE` | `Doom2SCYTHE.wad` — Doom II + Scythe | MUS synth | 206 MB | MAP30 (slaughter) |
| `Mimas-Doom2HR` | `Doom2HR.wad` — Doom II + Hell Revealed | MUS synth | 242 MB | MAP24 "Post Mortem" (~180 KB save buffer) |
| `Mimas-Doom2NUTS` | `Doom2NUTS.wad` — Doom II + nuts | MUS synth | 111 MB | MAP01 ~10 600 monsters (thinker/vissprite ceiling — a deliberately negative witness) |

Map-by-map rationale: [`wads_temoins/README.md`](../wads_temoins/README.md). Not built
(available in `wads_temoins/` if wanted): `Doom-ori`, `MPTEST`, `Nuts2`, `Nuts3`,
`grid1212`, and the standalone PWADs that need merging with an IWAD first.

## What every image carries

- **The M7 slave stack is ON** — masked-split + plane-split + clear-on-slave, i.e. the
  slave SH-2 takes its half of the software things/floors/ceilings fill. This shipped as
  the hard-wired default on 2026-07-30 (`core/r_things.c:1291`) after HW validation
  (`SLV b23% Pb59%`, `to`=0, 27 fps vs 24 master-only); the old `sat_m7_slave` 0–3 A/B
  level and its pad chord were removed with it. Pad **L+C** now cycles `sat_opt` (the
  perf-lever level, overlay row 7 `/o`).
- **Per-map repack** (`-Repack`, rot-level `auto`): the disc holds the raw WAD *and*
  `DOOMRP.DRP`, the per-map LZSS container. Maps that fit keep all 8 sprite rotations;
  only over-cart maps degrade a step at a time. This is why the big-WAD images are
  100–240 MB — the per-map blobs are self-contained, so shared lumps are duplicated.
- **HWRAM TLSF pool**: 7.77 KB on the eight MUS images, 6.72 KB on the CD-DA one
  (build-time pre-flight; floor 4 KB, comfort target 7 KB — the CD-DA build sits just
  under it, so confirm boot on hardware).

> **Provenance:** the eight MUS images were built before the `sat_opt` perf-lever work
> landed in `core/r_segs.c`/`r_plane.c`; the CD-DA image below includes it (`sat_opt`
> default 4, whose L4 step caps wall subdivision 6→3 and *does* change pixels). Rebuild
> the MUS set if you need the whole family on one code state.

## The CD-DA image

`build/images/Mimas-Doom1s-CDDA/` is a **self-contained folder**, not a single file:

```
Mimas-Doom1s-CDDA.cue    TOC: track 01 = data, tracks 02-15 = audio
Mimas-Doom1s-CDDA.bin    19.9 MB — data only, no audio inside
track_02.raw … track_15.raw   headerless 2352-aligned raw CD-DA
```

**All 16 files must travel together** — the `.cue` references every track by relative
name. Total 646 MB = **64 min**, so it fits a plain 74-min CD-R. (The one-big-`.bin`
variant, where `shared.mk` appends the audio into the data track, is the other path:
populate `cd/music/` + `cd/music/tracklist` and build without `-Cdda`.)

### Track numbering — why it is remapped

The source library (`DoomJo/project/music`) numbers tracks **linearly over all 32 music
entries**: `musicnum N → track N+1`. So `track_11..track_28` are the E2/E3 music, and
`inter/intro/bunny/victor/introa` land on **29–33**. Shareware has no E2/E3, and all 32
tracks would be 136 min — impossible on a CD. The disc therefore takes 02–10 and 29–33,
and `-Renumber` closes the hole so the TOC is contiguous (a Red Book TOC must be):

| CD track | source | music |
|---|---|---|
| 02–10 | `track_02..track_10` | `e1m1`–`e1m9` |
| 11–15 | `track_29..track_33` | `inter`, `intro`, `bunny`, `victor`, `introa` |

`cd/data/CDDAMAP.TXT` carries exactly that mapping, at the *top* of the file — the runtime
reads only its first 2047 bytes. Getting this wrong is not cosmetic: a name pointed at a
track that is not on the disc becomes an out-of-range `CDC_CdPlay` — silence at best, a
CD-block timeout ladder at worst.

Rebuild it with:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Wad Doom1s -Cdda -Repack `
  -MusicSrc "C:\Users\pcico\Projects\DoomJo\project\music" -Tracks "2-10,29-33" -Renumber `
  -OutDir "build\images\Mimas-Doom1s-CDDA"
```

> ⚠️ Correct track numbers remove the out-of-range plays, but do **not** clear the known
> long `-Cdda` boot wait. That has two other stacked causes (a second `CDC_CdInit` behind
> GFS's back, and the `CDDAMAP.TXT` read landing inside the post-`CdInit` danger window),
> both needing code changes. MUS remains the default build for a reason.
