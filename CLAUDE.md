# Mimas

Doom (shareware IWAD, doomgeneric core) ported to the Sega Saturn using the
Saturn Ring Library (SRL) SDK. SRL is a modern C++ wrapper around SEGA's SGL
(Saturn Graphics Library), released publicly in March 2025 by ReyeMe.

## Why SRL

| SDK | Status | Toolchain | Windows setup |
|-----|--------|-----------|---------------|
| Saturn Ring Library (SRL) | Active (v0.9.2, Jan 2026) | GCC 14.2 sh2eb-elf (setup_compiler.bat) | Simple |
| Yaul (libyaul) | Active but packages.yaul.org offline | GCC 14.3 (90 min build from source) | Complex |
| Jo Engine | Active | GCC 9.3 sh-elf (bundled) | Simple |
| SGL standalone | Unmaintained | GCC 9.3 sh-elf | Manual |

SRL chosen for: modern toolchain, simple Windows install, C++ abstractions over
SGL, SH-2 slave CPU support built-in, active development.

**License caveat**: SRL has no explicit open-source license (copyright reserved
by ReyeMe). Contact the author before any public/commercial release.

## Repository layout

| Path | Role |
|------|------|
| `core/` | **git submodule [doom-saturn-core](https://github.com/N0rt0N85/doom-saturn-core)**: the Doom game sources (doomgeneric / Chocolate-Doom, lightly patched) + `r_parallel.c` (dual-SH2 column renderer). **Shared verbatim with DoomJo.** |
| `src/` | **SRL platform layer only** (C++): `dg_saturn.cxx` (VDP2 framebuffer, SMPC pad, CD WAD loader), `i_sound_saturn.cxx` (SCSP sound), `main.cxx` (SRL init + Doom stack), `w_file_saturn.cxx` (WAD backend), `syscalls.c` (newlib stubs) |
| `cd/` | CD image content: DOOM1.WAD + text metadata |
| `docs/` | SRL API reference and notes |
| `SaturnRingLib/` | git submodule: ReyeMe/SaturnRingLib (SDK + toolchain setup) |

## Setup after a fresh clone

```powershell
git submodule update --init --recursive   # core + SaturnRingLib
cd SaturnRingLib
setup_compiler.bat            # downloads sh2eb-elf-gcc 14.2 toolchain
```

Obtain `doom1.wad` (shareware v1.9) and generate the stripped WAD:
```powershell
python tools/strip_wad.py doom1.wad cd/data/DOOM1.WAD
```

## Shared core (doom-saturn-core) — workflow

The Doom sources + the `r_parallel` renderer live in the `core/` submodule and
are compiled **verbatim** by both this port and **DoomJo** (the Jo Engine port,
`../DoomJo`). Edit shared code **once**, in `core/`:

```powershell
# 1. edit + commit + push the shared change in the submodule
cd core; git add -A; git commit -m "..."; git push; cd ..
# 2. record the new core revision in this port
git add core && git commit -m "bump core"
# 3. pull it into DoomJo the same way: cd ../DoomJo/core; git pull; cd ..; git add core; git commit
```

`core/r_parallel.c` must stay **pure C** (no C++): DoomJo compiles it with
GCC 9.3 which errors on C++isms (e.g. unnamed parameters) that Mimas's GCC 14
would only warn about. Its only platform hooks are `slSlaveFunc` (SGL, both
ports) and an `extern dbg_print` debug shim (each port implements it).

## Architecture: C Doom + C++ SRL

Doom sources (`core/`) are C. Platform files in `src/` (`dg_saturn.cxx`,
`main.cxx`, etc.) are C++ that wrap SRL's C++ API. Bridge rules:
- Platform `.cxx` files compile as C++23; `core/*.c` compile as C (gnu11)
- SRL types used ONLY in `src/` platform files, never in `core/`
- `extern "C"` exports: `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetKey`,
  `DG_SetWindowTitle`, `doom_start`

## Hardware map (target state)

| Hardware | Use |
|----------|-----|
| Master SH-2 | Game logic + BSP + sprites via SRL |
| Slave SH-2 | Column renderer (SRL::Slave::ITask) |
| VDP2 NBG1 | 320x200 8bpp framebuffer (SRL bitmap screen) |
| VDP2 NBG0 | Debug text overlay (SRL::Debug::Print) |
| SCSP | SFX: SRL::Sound::Pcm (direct slot); Music: SRL::Sound::Cdda |
| SMPC | SRL::Input::Digital (pad 1+2) |
| SCU DMA | Via SRL VDP2 sync (SRL::Core::Synchronize) |
| CD | SRL::Cd::File for DOOM1.WAD; SRL::Sound::Cdda for music tracks |

## Key SRL entry points for Mimas

### Startup (main.cpp)
```cpp
#include <srl.hpp>
int main() {
    SRL::Core::Initialize(SRL::Types::Colors::Black);
    // VDP2 NBG1 bitmap setup via SRL::VDP2::BmpScreen<>
    SRL::Input::RefreshPeripherals();  // once at start
    doom_start();  // never returns
}
```

### VBlank sync (replaces slSynch / vdp2_sync_wait)
```cpp
SRL::Core::Synchronize();          // blocks until next frame
// or register callback:
SRL::Core::OnVblank += &my_vblank_handler;
```

### Pad input (replaces smpc_peripheral_*)
```cpp
SRL::Input::RefreshPeripherals();
SRL::Input::Digital pad(0);        // port 0
if (pad.WasPressed(SRL::Input::Digital::Button::A)) { ... }
```

### CDDA music (replaces CDC_CdPlay)
```cpp
SRL::Sound::Cdda::Play(fromTrack, toTrack, loop);
SRL::Sound::Cdda::PlaySingle(track, loop);
SRL::Sound::Cdda::StopPause();
SRL::Sound::Cdda::Resume();
SRL::Sound::Cdda::SetVolume(127);
```

### CD file read (replaces cdfs / cd_block)
```cpp
SRL::Cd::Initialize();
SRL::Cd::File file("DOOM1.WAD");
file.Open();
file.LoadBytes(offset, size, buffer);
file.Close();
```

### Slave SH-2 (SRL::Slave)
```cpp
class ColumnTask : public SRL::Slave::ITask {
    void Start() override { /* render columns */ }
    bool IsDone() override { return done; }
};
ColumnTask task;
SRL::Slave::ExecuteOnSlave(task);
while (!task.IsDone()) {}
```

### Debug text
```cpp
SRL::Debug::Print(0, 0, "fps: %d", fps);
SRL::Debug::PrintClearScreen();
```

## Build

```powershell
# From Mimas root:
powershell -ExecutionPolicy Bypass -File build.ps1
# Or directly via make (MSYS2 / Linux):
make all
```

Uses SRL's `shared.mk` build system. Outputs `game.iso` + `game.cue`.

## Conventions

- Platform files: `.cpp` using C++23, SRL namespace fully qualified
- Every Doom source modification tagged with `// SATURN:` comment
- No Jo Engine, no Yaul, no SGL direct calls — all hardware via SRL
- `SRL_MAX_TEXTURES` not needed (Doom does not use VDP1 quads for gameplay)
- Framebuffer: static array in high work RAM, blitted via SRL VDP2 bitmap sync

## SRL documentation

See `docs/SRL_API.md` for the full SRL API reference.
See `docs/SRL_NOTES.md` for Mimas-specific integration notes.
Online: https://srl.reye.me/
GitHub: https://github.com/ReyeMe/SaturnRingLib
