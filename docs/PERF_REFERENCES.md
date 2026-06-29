# Saturn Doom — Performance References & Lessons

Research notes on how other Saturn / dual-SH2 / optimized-Doom projects exploit
the hardware, and what is **directly transferable to Mimas**.

> **Status: REFERENCE — external-project catalog & transferable lessons.**
> The architecture question this doc once framed as open ("two philosophies /
> should we go hardware") is **decided**: Mimas shipped **hybrid** — VDP1 carries
> **walls only** (8bpp + CRAM light-banks); the dominant flat (floor) is a clean
> 512×256 8bpp RBG0 **bitmap** on VDP2. For the floor reality see
> [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md); for VDP1↔NBG1
> present sync see [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md); the
> unshipped VDP1 world-renderer bet lives in `VDP1_WORLD_PLAN.md`. The
> d32xr / Hexen / FastDoom *software* lessons below still stand and are the
> reason to keep this file.

Context: Mimas splits Doom's classic column/span software rasterizer across the
two SH-2s. Walls render on VDP1; the floor is a VDP2 RBG0 bitmap; sprites and
HUD composite via NBG1. See `src/dg_saturn.cxx` and `core/r_parallel.c`.

Compiled June 2026 from web research (sources at the bottom).

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

**Lesson for Mimas:** if we ever go the VDP1 route (Tier 1), this is the exact
blueprint for how to structure geometry for VDP1 — don't reinvent it. The price
is **affine texture warping** (Saturn quads are not perspective-correct).

## 2. Doom Saturn (official, 1997) — the cautionary tale

Jim Bagley **had** a VDP1, full-screen, 60 fps engine running. John Carmack
rejected it over **affine texture degradation** and forced a software renderer,
which shipped rushed and notoriously slow. Carmack later publicly regretted it.

**Lesson for Mimas:** the "ugly" affine VDP1 warp is the price of speed; a
purist who blocks the hardware path condemns the port to a slow software one. We
already accepted a visual compromise by targeting Saturn at all.

## 3. Doom 32X: Resurrection / `d32xr` — the closest cousin (open source + documented)

**Same architecture as Mimas**: 2× SH-2, software render, no hardware TMU.
This is the single most actionable reference because the optimizations are
documented and the split is **more sophisticated than Mimas's current
even/odd column parity**.

- Source: <https://github.com/viciious/d32xr>
- Optimization writeup: <https://github.com/viciious/d32xr/wiki/Engine-optimizations,-part-1>
- Result: **2–4× faster** than the original 32X Doom port.

Key techniques (transferable):

1. **Split by *phase*, not just by drawing.** d32xr's 2nd CPU does *wall prep* +
   visplane computation *while the primary walks the BSP*. **Mimas tried this 3×
   and it is CONFIRMED DEAD**: wall-prep→slave is memory-bound and multiplexed on
   this hardware; even a TAS work-steal queue doesn't revive it. The slave stays
   a draw/plane executor (`rp_slave_body` in `core/r_parallel.c`).
2. **Lock-free work queue via the SH-2 `TAS` instruction** (test-and-set, locks
   the bus): both CPUs atomically pull work, **zero overdraw** because Doom walls
   don't overlap; better load balance than fixed parity. **SHIPPED in Mimas as
   the dual-SH2 plane work-steal ("TAS"), default-on** (core `73f8cdc` / parent
   `4857f87`); the old even/odd parity split is retired and row-split is parked.
3. **Sprite split at the *mean X* coordinate** (equal pixel counts per CPU), not
   at screen center.
4. **Pre-sort visplanes** by width / flat number / lighting to minimize pipeline
   stalls in the floor/ceiling phase.
5. **Decoupled tick rates**: input at 30 fps, game logic at 15 fps. (Mimas runs
   Doom's standard 35 Hz.)

## 4. Hexen Saturn (Probe, 1997) — proof the software→VDP2 path can work

Hexen does **software rendering to the VDP2 framebuffer via DMA** (exactly
Mimas's model) **and drives the slave SH-2 to ~80%**.

**Lesson for Mimas:** the software→VDP2 path *can* perform decently when the
**slave is saturated** (the d32xr lesson) — which Mimas now does via the shipped
TAS plane work-steal. Hexen validates the partitioned-slave approach.

## 5. FastDoom (viti95) — free algorithmic wins, hardware-agnostic

Pure software/algorithmic optimizations, portable to SH-2 as-is.

- Source: <https://github.com/viti95/FastDoom>
- **Potato mode**: renders at quarter width (max 80×200) then stretches —
  i.e. the "low-detail + VDP2 hardware zoom" idea, proven to pay off.
- **Pre-processed colormap** (avoid real-time conversion): ~4% on column draw.
- **Skip rendering unneeded visplanes**; optimized `R_RenderSegLoop`.
- **Cache-sized code paths**: separate column/span routines tuned to fit L1.
  This corroborates Mimas's own finding that `-O3` *slowed* the slave via
  I-cache bloat (`core/r_parallel.c`, the SATURN PERF 1.4 note) and the I-cache
  fix in the project memory (`sh7604-ccr-bits`): small cache-resident loops beat
  aggressive unrolling on these CPUs.

## Other Saturn FPS

Most third-party Saturn games barely used the slave SH-2 (the two CPUs can't hit
memory simultaneously without bus contention, so partitioning work is laborious).
Titles like Alien Trilogy ran poorly for this reason. **Hexen and the Lobotomy
games are the exceptions — and the lesson is exactly "use the slave hard."**

---

## Priority for Mimas

1. **`d32xr`** — same architecture, documented, 2–4× gains. Already pillaged: the
   **`TAS` work queue replaced even/odd parity (shipped, default-on)**, and the
   **wall-prep→slave phase split was tried 3× and is dead** (memory-bound). What
   remains untapped: mean-X sprite split and visplane pre-sort.
2. **Hexen** — proof that software + VDP2 + a saturated slave performs; the slave
   is now loaded via the TAS plane work-steal.
3. **FastDoom** — free algorithmic wins (Potato/low-detail + VDP2 zoom,
   colormap preprocessing, skip dead visplanes, cache-fit loops).
4. **SlaveDriver** — the blueprint for the unshipped VDP1 world-renderer bet
   (`VDP1_WORLD_PLAN.md`).

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
