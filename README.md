# Mimas

**A hardware-accelerated Doom engine for the Sega Saturn.**
Bring a WAD, ship a Saturn FPS.

Mimas runs the Doom engine (doomgeneric / Chocolate-Doom) on the Saturn's real
hardware: the world is generated across **both SH-2 CPUs** and drawn with a
hybrid software **+ VDP1/VDP2 hardware** renderer: The hardware-accelerated
Saturn Doom that the infamous 1997 port never was. (id's John Carmack vetoed Jim
Bagley's hardware-accelerated Saturn engine at the time, forcing a slow software
port, and later called it a mistake.)

Because it *is* a Doom engine, the **content is a WAD**: making a WAD is one of
the most documented skills in gaming, so anyone can have their own FPS running on
real Saturn hardware without writing a line of Saturn code.

> ⚠️ **Status:** work in progress, tested on real hardware (via Rhea/Phoebe/Fenrir
> ODEs). Built on SRL (**MIT**). See [Licensing](#licensing).

## Why Mimas

The official 1997 Saturn Doom (Rage / GT Interactive) was a pure software
renderer on both SH-2s at ~10–13 fps, with no light diminishing, downgraded
audio and the wrong sky — widely cited as one of the worst console ports ever.
Mimas puts together, on one Saturn, things **no shipping Saturn Doom has
combined**:

- **Real dual-SH-2 parallel rendering** — a slave-CPU column / visplane renderer.
- **VDP1/VDP2 hardware rasterization** — walls as VDP1 distorted sprites, sky on a VDP2 layer.
- **Big-WAD streaming from CD with no RAM cart** — Doom II / Ultimate / Plutonia / TNT stream off the disc.
- **CDDA + MUS music**, **restored light diminishing**, and **split-screen multiplayer** (parallel REC).

## Hardware map

| Hardware | Use |
|----------|-----|
| Master SH-2 | Game logic + BSP + render command generation |
| Slave SH-2 | Parallel column / visplane renderer (work-stealing) |
| VDP2 NBG1 | 320×224 8bpp framebuffer (software output) |
| VDP1 | Hardware walls (distorted sprites), player weapon |
| VDP2 NBG0 | Hardware sky |
| SCSP | SFX (direct slot) + music (CDDA, or a software MUS synth) |
| SMPC | Pad input (1–2 players) |
| CD | `DOOM1.WAD` + per-level `.DRP` streaming; CDDA tracks |

## Build

### Prerequisites
- **Windows + MSYS2** (`C:\msys64`, provides `make`).
- The **SH-2 toolchain** (GCC 14.2 `sh2eb-elf`), installed by SRL's `setup_compiler.bat`.
- A **Doom WAD you own** (see [Bring your own WAD](#bring-your-own-wad)).

### Fresh clone
```powershell
git clone --recursive https://github.com/mimasengine/mimas.git
cd mimas
git submodule update --init --recursive          # if you forgot --recursive
cd SaturnRingLib; .\setup_compiler.bat; cd ..     # downloads GCC 14.2 sh2eb-elf
python tools/strip_wad.py doom1.wad cd/data/DOOM1.WAD   # provide the IWAD
```

### Build
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # incremental
```
Outputs `build/Mimas.iso` + `build/Mimas.cue` (and a MODE1/2352 `.bin`).

**`build.ps1` parameters:**

| Flag | Effect |
|------|--------|
| `-Clean` | full rebuild |
| `-Wad <name>` | pick an IWAD from `wads_temoins/` → copied to `cd/data/DOOM1.WAD` before building (e.g. `-Wad doom2`) |
| `-Repack` | also emit the per-map LZSS container `cd/data/DOOMRP.DRP` (big-WAD CD streaming) |
| `-Cdda` | multi-file CDDA disc: a small data `.bin` + WAV tracks referenced separately (fast build/mount) |
| `-WarpMap "1 8"` | boot straight into a map (E1M8); Doom II: `-WarpMap 15` |
| `-WarpSkill 4` | skill 1–5 (default `4` = Ultra-Violence) |

Build configuration lives at the top of [`Makefile`](Makefile): `CD_NAME = Mimas`
(disc / artifact name), the SRL/SGL work-area sizes, and the Doom compile flags
(`MAXVISPLANES`, visplane pool, command-buffer size, repack, …).

### Run
```powershell
powershell -ExecutionPolicy Bypass -File run_ymir.ps1            # Ymir emulator
powershell -ExecutionPolicy Bypass -File run_ymir.ps1 -Build     # build, then launch
```
Or burn `build/Mimas.cue` to CD-R, load it via an ODE (Rhea/Phoebe/Fenrir), or
open the `.cue`/`.bin` in any Saturn emulator (Ymir, Kronos, Mednafen).

> **Hardware note:** the fast SCU-DMA framebuffer blit hangs the SH-2 bus on real
> hardware, so `USE_SCU_DMA` (in `src/dg_saturn.cxx`) is `0` (CPU blit — slower
> but safe). A proper DMA fix is pending.

## Bring your own WAD

Mimas ships **no game data**. Provide your own WAD:

- The **shareware** `doom1.wad` (*Knee-Deep in the Dead*) is freely redistributable
  for non-commercial use — fine to use here.
- **Commercial IWADs** (Doom II, Ultimate Doom, Plutonia, TNT) are **not**
  redistributable — use your own copy, don't share it.
- **Custom WADs:** vanilla / limit-removing WADs are the target (the engine is
  Chocolate-Doom-based); complex GZDoom/ZScript mods won't run.

## Repository layout

| Path | Role |
|------|------|
| `core/` | **Shared submodule** (`doom-saturn-core`): Doom game sources + `r_parallel.c` (dual-SH-2 renderer). Compiled verbatim by both Mimas and DoomJo. |
| `src/` | **SRL platform layer** (C++23): `main.cxx`, `dg_saturn.cxx`, `i_sound_saturn.cxx`, `w_file_saturn.cxx`, `w_drp_saturn.cxx`, `syscalls.c` |
| `SaturnRingLib/` | Submodule: the SRL SDK + the `sh2eb-elf` toolchain |
| `cd/` | CD image content (WAD + metadata) |
| `tools/` | `strip_wad.py`, `repack_wad.py` (per-level `.DRP` packer) |
| `Makefile`, `build.ps1` | Build (SRL `shared.mk`) |

This is one of two ports that share `core/`; the other is **DoomJo** (Jo Engine /
C) — the author's earlier port, kept as a clean-license sibling. See `CLAUDE.md`
for the deep architecture notes.

## Inspiration & lineage

Mimas stands on a lot of prior work — both the engine it *is* and techniques it
learned from:

- **doomgeneric / Chocolate-Doom** — the Doom engine itself (all of `core/`).
- **[d32xr](https://github.com/viciious/d32xr)** (Victor Luchits et al., the 32X Doom) — the bounded
  texture-composite cache (`r_cache.c`, ported), the dual-SH-2 visplane split, the visplane hash.
- **[SlaveDriver-Engine](https://github.com/Lobotomy-Software/SlaveDriver-Engine)** (Lobotomy, PowerSlave/Exhumed) — **inspiration** for the
  VDP1 approach (async no-vsync driver, double-buffered command list, CD read-retry, distorted-sprite walls). Mimas's renderer is an
  independent implementation and diverges: e.g. no VDP1 floors yet, and a software/CPU fallback for near-camera edges.
- **PSX Doom** — the per-level asset-subset + LZSS container model (the `.DRP` repack).
- **FastDoom** (viti95) — the low-detail "potato" flat-colour rendering idea.
- **PrBoom+** (Andrey Budko / e6y) — vanilla demo-compat fixes carried in the core.
- **SaturnDoom → DoomJo** — the author's first Saturn Doom port (Jo Engine), source of the SCSP audio layer.
- **[Saturn Ring Library](https://github.com/ReyeMe/SaturnRingLib)** (SRL) by ReyeMe — the SDK Mimas boots on.

## Author

Built by **Romain Cicolini** ([@N0rt0N85](https://github.com/N0rt0N85)).

## Licensing

Mimas is **free software under the GNU General Public License, version 2 or
later** (it builds on the GPL'd Doom engine) — see [`COPYING`](COPYING).
Distributing a build means making the complete corresponding source available.

| Component | License |
|-----------|---------|
| Doom engine — `core/` (doomgeneric/Chocolate-Doom; © id Software/ZeniMax + Simon Howard et al.) | GPL-2.0-or-later |
| `core/r_cache.c` texture cache (ported from d32xr) | MIT — © 2021 Victor Luchits, Derek John Evans, id Software & ZeniMax Media |
| Platform layer — `src/` | GPL-2.0-or-later |
| **Saturn Ring Library** (ReyeMe) | **MIT** (granted by the author; license file being added upstream) |
| SGL — SEGA Saturn Graphics Library (wrapped by SRL) | SEGA proprietary |

**SRL & SGL.** SRL is **MIT** (its author granted this; a license file is being
added upstream), which is GPL-compatible — so linking it with the GPL engine is
fine. The one remaining proprietary component is **SGL** (SEGA's Saturn Graphics
Library, which SRL wraps): it is out of anyone's hands to relicense, and is the
same situation every Saturn homebrew GPL port lives in — treated in practice as
the platform's system library. The Jo Engine port (**DoomJo**) remains a
fully-clean-license sibling.

**Game data.** Mimas contains no Doom assets — bring your own WAD (above). This is
a **non-commercial fan homebrew** project (donations welcome). The Doom engine is
GPL; the *Doom* name and IP belong to id Software / ZeniMax / Microsoft and are
used here only descriptively.

## Acknowledgements

ReyeMe and the SRL contributors; the d32xr, SlaveDriver-Engine, FastDoom and
Chocolate-Doom authors; and the SegaXtreme / *Sega Saturn Shiro* homebrew
community.
