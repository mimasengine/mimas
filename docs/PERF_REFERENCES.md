# Saturn Doom — Performance References & Lessons

Research notes on how other Saturn / dual-SH2 / optimized-Doom projects exploit
the hardware, and what is **directly transferable to DoomSRL**.

Context: DoomSRL is a 100% **software** renderer (Doom's classic column/span
rasterizer) split across the two SH-2s, writing an 8bpp framebuffer that is
blitted to a VDP2 NBG1 bitmap. VDP1 is currently **unused**. Baseline ~5–10 fps
(cart, CPU blit). The SCU DMA blit is disabled (hangs on hardware), so the blit
is a per-frame CPU `memcpy` of 320×200. See `src/dg_saturn.cxx` and
`core/r_parallel.c`.

> **⚠️ STALE as of 2026-06-19.** This file is the *pre-VDP1* research snapshot.
> DoomSRL has since gone hybrid: **VDP1 now renders all walls** (8bpp textures +
> CRAM light-banks, below software NBG1 floors/sprites). For the current VDP1
> hardware model, cost budget, IN/OUT convictions, and multiplayer capacity, see
> [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md); for VDP2, [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md).
> The "two philosophies / should we go hardware" framing below is now decided
> (we went hybrid), but the d32xr/Hexen/FastDoom *software* lessons still stand.

Compiled June 2026 from web research (sources at the bottom).

---

## The two philosophies

Every Saturn FPS / Doom-class engine falls into one of two camps. **DoomSRL is
in the software camp today.**

| Camp | Games | Principle |
|------|-------|-----------|
| **Hardware VDP1** | PowerSlave/Exhumed, Duke Nukem 3D Saturn, Quake Saturn | World rebuilt as textured VDP1 quads; no software framebuffer |
| **Software → VDP2** | Doom Saturn (official), Hexen Saturn, Doom 32X, **DoomSRL** | CPU rasterizer writes pixels; result blitted to a VDP2 bitmap |

Switching camps = rewriting the renderer (Tier-1 effort). Staying in the
software camp but copying the best software techniques = Tier-2 effort, the
better short-term bet.

---

## 1. SlaveDriver Engine (Lobotomy) — the VDP1 blueprint, now open source

The in-house engine by Ezra Dreisbach behind **PowerSlave/Exhumed, Duke Nukem 3D
Saturn, and Quake Saturn** — the high-water mark for Saturn FPS. The Build engine
was deemed impractical to port, so they built a fully-3D engine around VDP1.

- **Source released GPL, August 2025**: <https://github.com/Lobotomy-Software/SlaveDriver-Engine>
- World structure: `Planes` reference **quads + tiles by index**
  (`kLS_Quads`, `kLS_Tiles`); vertices are direct indices into `kLS_Verts`.
  Walls and floors are VDP1 textured/distorted sprites.
- Also reverse-engineered by Rich Whitehouse (Saturn Quake) and Kaiser
  (PowerSlave EX) — multiple study angles available.

**Lesson for DoomSRL:** if we ever go the VDP1 route (Tier 1), this is the exact
blueprint for how to structure geometry for VDP1 — don't reinvent it. The price
is **affine texture warping** (Saturn quads are not perspective-correct).

## 2. Doom Saturn (official, 1997) — the cautionary tale

Jim Bagley **had** a VDP1, full-screen, 60 fps engine running. John Carmack
rejected it over **affine texture degradation** and forced a software renderer,
which shipped rushed and notoriously slow. Carmack later publicly regretted it.

**Lesson for DoomSRL:** the "ugly" affine VDP1 warp is the price of speed; a
purist who blocks the hardware path condemns the port to a slow software one. We
already accepted a visual compromise by targeting Saturn at all.

## 3. Doom 32X: Resurrection / `d32xr` — the closest cousin (open source + documented)

**Same architecture as DoomSRL**: 2× SH-2, software render, no hardware TMU.
This is the single most actionable reference because the optimizations are
documented and the split is **more sophisticated than DoomSRL's current
even/odd column parity**.

- Source: <https://github.com/viciious/d32xr>
- Optimization writeup: <https://github.com/viciious/d32xr/wiki/Engine-optimizations,-part-1>
- Result: **2–4× faster** than the original 32X Doom port.

Key techniques (transferable):

1. **Split by *phase*, not just by drawing.** The 2nd CPU does *wall prep* +
   visplane computation *while the primary is still walking the BSP*. In
   DoomSRL the slave only **executes draw commands** (`rp_slave_body` in
   `core/r_parallel.c`) and is largely idle during the BSP walk —
   **this is our biggest untapped gain.**
2. **Lock-free work queue via the SH-2 `TAS` instruction** (test-and-set, locks
   the bus) for wall drawing: both CPUs atomically pull walls, **zero overdraw**
   because Doom walls don't overlap. Better load balance than fixed parity.
3. **Sprite split at the *mean X* coordinate** (equal pixel counts per CPU), not
   at screen center.
4. **Pre-sort visplanes** by width / flat number / lighting to minimize pipeline
   stalls in the floor/ceiling phase.
5. **Decoupled tick rates**: input at 30 fps, game logic at 15 fps. (DoomSRL runs
   Doom's standard 35 Hz.)

## 4. Hexen Saturn (Probe, 1997) — proof the software→VDP2 path can work

Hexen does **software rendering to the VDP2 framebuffer via DMA** (exactly
DoomSRL's model) **and drives the slave SH-2 to ~80%**.

**Lesson for DoomSRL:** the software→VDP2 path *can* perform decently, but only
if (a) the **DMA does the blit** — ours is currently disabled and falls back to a
CPU `memcpy` (`src/dg_saturn.cxx`, `USE_SCU_DMA 0`, hangs on hardware), and
(b) the **slave is saturated** (the d32xr lesson). Hexen validates both open
chantiers at once.

## 5. FastDoom (viti95) — free algorithmic wins, hardware-agnostic

Pure software/algorithmic optimizations, portable to SH-2 as-is.

- Source: <https://github.com/viti95/FastDoom>
- **Potato mode**: renders at quarter width (max 80×200) then stretches —
  i.e. the "low-detail + VDP2 hardware zoom" idea, proven to pay off.
- **Pre-processed colormap** (avoid real-time conversion): ~4% on column draw.
- **Skip rendering unneeded visplanes**; optimized `R_RenderSegLoop`.
- **Cache-sized code paths**: separate column/span routines tuned to fit L1.
  This corroborates DoomSRL's own finding that `-O3` *slowed* the slave via
  I-cache bloat (`core/r_parallel.c`, the SATURN PERF 1.4 note) and the I-cache
  fix in the project memory (`sh7604-ccr-bits`): small cache-resident loops beat
  aggressive unrolling on these CPUs.

## Other Saturn FPS

Most third-party Saturn games barely used the slave SH-2 (the two CPUs can't hit
memory simultaneously without bus contention, so partitioning work is laborious).
Titles like Alien Trilogy ran poorly for this reason. **Hexen and the Lobotomy
games are the exceptions — and the lesson is exactly "use the slave hard."**

---

## Priority for DoomSRL

1. **`d32xr`** — same architecture, documented, 2–4× gains. Pillage first.
   Concretely: give the slave the **wall-prep + visplane** phases (not just
   drawing), and replace the even/odd parity split with a **`TAS` work queue**.
2. **Hexen** — proof that software + VDP2 + a saturated slave performs → fix the
   **SCU DMA blit** and load the slave more.
3. **FastDoom** — free algorithmic wins (Potato/low-detail + VDP2 zoom,
   colormap preprocessing, skip dead visplanes, cache-fit loops).
4. **SlaveDriver** — the blueprint *if* we ever switch to the VDP1 hardware path.

See `docs/SRL_NOTES.md` (integration notes) and the in-repo perf notes in
`core/r_parallel.c` / `src/dg_saturn.cxx`.

---

## Sources

- SlaveDriver Engine (GPL): <https://github.com/Lobotomy-Software/SlaveDriver-Engine> ·
  <https://segabits.com/blog/2025/08/26/source-code-for-lobotomy-softwares-fps-game-engine-for-saturn-slavedriver-uploaded-online-as-open-source-code/>
- d32xr engine optimizations: <https://github.com/viciious/d32xr/wiki/Engine-optimizations,-part-1> ·
  Doom 32X: Resurrection — <https://doomwiki.org/wiki/Doom_32X:_Resurrection>
- Doom Saturn: <https://doomwiki.org/wiki/Sega_Saturn> ·
  Bagley lost prototype — <https://lostmediawiki.com/Doom_(lost_prototype_of_SEGA_Saturn_port_of_first-person_shooter;_1996-1997)>
- Hexen Saturn (VDP2 rendering): <https://segaxtreme.net/threads/software-rendering-with-vdp2-hexen-use-this-to-render-all-3d.16787/> ·
  <https://doom.fandom.com/wiki/Hexen_(Sega_Saturn)>
- FastDoom: <https://github.com/viti95/FastDoom/blob/master/README.md> ·
  <https://doomwiki.org/wiki/FastDoom>
- PowerSlave / SlaveDriver overview: <https://en.wikipedia.org/wiki/PowerSlave> ·
  <https://retrorgb.com/lobotomy-softwares-engine-code-uploaded-to-github.html>
