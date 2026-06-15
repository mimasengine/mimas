# DoomSRL

Doom (shareware IWAD) for the **Sega Saturn**, built on the **Saturn Ring
Library** (SRL — a modern C++ wrapper over SEGA's SGL, GCC 14.2 sh2eb-elf).

This is one of two ports that share a common core; the other is **DoomJo**
(Jo Engine / C). See `CLAUDE.md` for the deep architecture notes.

## Dependencies (git submodules)

| Submodule | Path | What |
|-----------|------|------|
| [doom-saturn-core](https://github.com/N0rt0N85/doom-saturn-core) | `core/` | Shared Doom game sources + the dual-SH2 renderer (`r_parallel.c`). Compiled verbatim by both DoomSRL and DoomJo. |
| SaturnRingLib | `SaturnRingLib/` | The SRL SDK + the `sh2eb-elf` GCC 14.2 toolchain (installed by `setup_compiler.bat`). |

Also required on the host: **MSYS2** (for `make`; build.ps1 calls
`C:\msys64`), and **`DOOM1.WAD`** (Doom shareware v1.9) — not in the repo.

## Install (fresh clone)

```powershell
git clone --recursive https://github.com/N0rt0N85/DoomSRL.git
cd DoomSRL
# if you forgot --recursive:
git submodule update --init --recursive

# install the SH-2 toolchain (downloads GCC 14.2 sh2eb-elf):
cd SaturnRingLib; .\setup_compiler.bat; cd ..

# provide the IWAD (strip it down to what the port uses):
python tools/strip_wad.py doom1.wad cd/data/DOOM1.WAD
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # incremental
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean   # full rebuild
```

Outputs `build/DoomSRL.iso` + `.cue` (and a MODE1/2352 `.bin`).

## Run

```powershell
powershell -ExecutionPolicy Bypass -File run_ymir.ps1       # Ymir emulator
```

Or burn `build/DoomSRL.cue` to CD-R, or load the `.cue`/`.bin` in any Saturn
emulator (Ymir, Kronos, Mednafen).

> **Hardware note:** the fast SCU-DMA framebuffer blit currently hangs the
> SH-2 bus on real hardware, so `USE_SCU_DMA` (in `src/dg_saturn.cxx`) is set
> to `0` (CPU blit — slower but safe). A proper DMA fix is pending.

## Layout

| Path | Role |
|------|------|
| `core/` | Shared sources (submodule): Doom game code + `r_parallel.c` renderer |
| `src/` | **SRL platform layer**: `main.cxx`, `dg_saturn.cxx`, `i_sound_saturn.cxx`, `w_file_saturn.cxx`, `syscalls.c` |
| `SaturnRingLib/` | SDK + toolchain (submodule) |
| `cd/` | CD image content (DOOM1.WAD + metadata) |
| `Makefile`, `build.ps1` | Build (SRL `shared.mk`) |

## Licensing

Doom sources are GPLv2. **SRL has no explicit open-source license** (copyright
reserved by ReyeMe) — contact the author before any public/commercial release.
The Jo Engine port (**DoomJo**) exists partly as a clean-license alternative.
