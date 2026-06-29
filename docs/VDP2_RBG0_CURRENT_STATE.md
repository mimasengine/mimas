# VDP2 RBG0 — Current State (authoritative)

> **Status: CURRENT (2026-06-27, reorg 2026-06-29).** This is the live reference for the VDP2 RBG0
> hardware floor. It supersedes the floor-related conclusions in
> `VDP2_ARCHITECTURE.md`, `VDP2_LAYER_BUDGET.md`, `VDP2_CONFIG_CATALOG.md`, the
> trimmed-to-data `RBG0_FLOOR_PLAN.md` (kept only for its FLAT-profiler numbers),
> and the §6 floor routing in `VDP1_ARCHITECTURE.md`. The fully-obsolete cell-era
> plans `VDP2_FLOOR_CONSOLIDATION.md`, `RBG0_SNOW_FIX_PLAN.md`, and
> `RBG0_STRUCTURED_GARBAGE.md` were **deleted** in the 2026-06-29 docs reorg (their
> useful mechanism lives here). Where any doc and this one disagree about the floor, **this doc wins.**

## TL;DR

The RBG0 floor **ships** as a **512x256 8bpp BITMAP** (`RBG0_BITMAP = 1`,
`dg_saturn.cxx:297`) and is **CLEAN on real Saturn hardware** (commits `19768ca`
"hardware bitmap floor -- 2 banks, B1 freed, no snow on HW" and `41dd895`).

It is **not** off, **not** the old cell floor, and does **not** snow. It uses **2
rotation banks** (not the cell floor's 3), and dropping the pattern-name map
**freed B1**, which **lifted the "floor XOR sky" law**. The floor is gated to
**potato-0 + 1-player**; everything else falls back to the software floor.

## Bank layout (shipping)

The Saturn has **4 VRAM banks × 128 KB**. The shipping bitmap floor consumes 2
rotation banks + the framebuffer bank; B1 holds only the rotation-parameter table
and is otherwise free.

| Bank | Address       | Holds (shipping bitmap floor)                                   |
|------|---------------|-----------------------------------------------------------------|
| A0   | `0x25E00000`  | RBG0 **coefficient / K-table** (`RBG0_KTAB_VRAM`, `slMakeKtable`) |
| A1   | `0x25E20000`  | RBG0 **512x256 8bpp bitmap** (`RBG0_BMP_VRAM`, `slBitMapRbg0`)   |
| B0   | `0x25E40000`  | **NBG1 game framebuffer** (320x200 8bpp, software-rendered)      |
| B1   | `0x25E60000`  | **rotation-parameter table (RPT) at `B1+0x1ff00` only → otherwise FREE** |

Key facts about this layout:

- **No pattern-name MAP.** A bitmap RBG0 has no tile map, so the per-dot
  pattern-name read is gone. That is the bank the cell floor spent in B1.
- **RPT is in B1, NOT A1.** `SRL::Core::Initialize` already relocated the
  rotation-parameter table to `B1+0x1ff00` via `slRparaInitSet`. The code
  deliberately does **not** call `slRparaInitSet` again (`dg_saturn.cxx:1437`);
  pointing the RPT at a RAM struct was the old "uniform black" bug.
- **B1 is free for NBG3 / a cell sky.** The NBG3 debug overlay's font/page/map
  live in B1 (SRL default), away from the RPT, and coexist with the floor.
- **2 rotation banks, not 3.** Cell path = K (A0) + cells (A1) + map (B1) = 3.
  Bitmap path = K (A0) + bitmap (A1) = 2. The framebuffer (B0) is non-negotiable
  in both.

> Note: an in-tree comment block (`dg_saturn.cxx:289-296`) still says "the bitmap
> MUST be in A0, NOT A1" — that was a **wrong** earlier conclusion, corrected
> right below it (`:301-303`). The **active** `#define`s put the **bitmap in A1**
> and the **K-table in A0**, matching SlaveDriver's `PLAX.C` (char in A1,
> coefficient table in A0).

## The snow: cause and fix

The "snow" (random white bands across the whole screen) was a property of the
**old CELL floor**, and it was **VRAM cycle-pattern starvation**, not a transform
or cache bug:

- A rotation read happens **3× per dot** on the cell path: **coefficient** +
  **pattern-name (map)** + **character (cell)**. Each read needs its own VRAM
  access-cycle (CYC) slot.
- The hand-poked RAMCTL reserved the banks but the **coefficient read's CYC slot
  was never issued** → the rotation engine fetched from an unscheduled bank →
  stale streaks = snow. (Removing the RAMCTL poke entirely snowed *worse* —
  commit `8571761` — so it was load-bearing but insufficient alone.)

The bitmap floor solves it by construction plus a manual commit:

1. **Bitmap drops the map** → **2 reads/dot** (coeff + char), no starving third
   read.
2. **Manual RDBS = 0x0D** via `rbg0_commit_ramctl` (`dg_saturn.cxx:1461`):
   `(A1=char/bitmap=3)<<2 | (A0=coeff=1)`, B1=0 (no map). The shadow
   `VDP2_RAMCTL` is updated too so a per-vblank ISR re-push stays coherent.
3. **Park the A0/A1 rotation cycle slots at `0xEEEE`** via `rbg0_commit_cyc`
   (`dg_saturn.cxx:1487`), mirroring SlaveDriver's cycle table, then
   **block-flush the contiguous shadow register image `0x0E..0xFE` to the chip**
   (base = `&VDP2_RAMCTL - 0x0E`). This is exactly what `slSynch` would push,
   minus `slSynch`.

`slBitMapRbg0` never calls `rbank_set`, so the A1 bitmap bank is **reserved by
hand** through the two commit functions above. Both run **once at init**
(`dg_saturn.cxx:1777-1778`), after `slScrAutoDisp` so `RBG0ON` is already live.

**No `slSynch` is used on the bitmap path.** Calling it would recompute the cycle
pattern from the (intentionally hand-built, "inconsistent") shadow → the
boot-loop. `rbg0_commit_cyc` **is in the tree** — earlier docs claiming it was
absent/reverted are stale.

## "Floor XOR sky" is LIFTED

The old hardware law — *you can have the hardware sky OR the RBG0 floor, never
both on a 4-bank budget* — was a consequence of the cell floor's 3-bank
footprint (the map in B1 collided with everything else there).

Dropping the map **freed B1**. Consequences:

- A **cell sky can live in B1** → **floor + sky CAN coexist.**
- The **NBG3 debug overlay coexists** with the floor (re-enabled as a runtime
  **L+R toggle**, default off — commit `597bc68`, `RBG0_NBG3 = 1`).
- The **real remaining swing bank is sky-vs-NBG3** (both want B1), **not**
  floor-vs-sky.

Mimas currently runs a **software sky** (`VDP2_HW_SKY = 0`) so that A0 is free for
the K-table — but that is now a *build/quality* choice, **not** a hardware bank
law.

## Bank-storage-bound, not cycle-bound

Mimas is limited by **bank STORAGE**, not by VRAM access cycles. An 8bpp bitmap
costs ~**2 of the 8** per-bank cycle slots — comfortable. The constraint is "how
many 128 KB banks does each layer claim," which is why the win was *dropping a
bank* (the map), not *freeing cycles*.

## Cell floor (3 banks) — the dead-end, for reference

The legacy `RBG0_BITMAP = 0` cell path remains in-tree under `#if` but is the
abandoned dead-end:

| | Cell floor (dead) | **Bitmap floor (ships)** |
|---|---|---|
| Banks | K(A0) + cells(A1) + **map(B1)** = **3** | K(A0) + bitmap(A1) = **2** |
| B1 | map → evicts NBG3, blocks B1 sky | **free** (RPT only; NBG3/sky OK) |
| Reads/dot | 3 (coeff+map+cell) → **snow** | 2 (coeff+char) → **clean** |
| RDBS | `0x8D` (B1=pattern-name) | **`0x0D`** (no map) |
| HW result | snow + dead sky | **CLEAN, ships** |

## Distance fog (prototype, gated OFF)

Distance lighting on the floor is **NOT** done via `K_LINECOL` on the coefficient
table. It is a **VDP2 LINE-COLOR SCREEN blended into RBG0 via color-calc**
(`rbg0_linecol_apply`, `dg_saturn.cxx:1378`; `RBG0_LINECOL_TEST`, commits
`bed9d81`/`bda7fce`). Currently **gated OFF** — RUNG A only proved the plumbing
(a flat darken); the per-line distance gradient (RUNG C) is a future session.

Two gotchas that each cost a build:

- **Do NOT add `LNCLON` to the `slScrAutoDisp`/BGON mask** — it kills NBG1 (the
  whole software framebuffer vanishes). Enable the line-color display with
  `slLineColDisp(LNCLON)` alone.
- **CCRR (chip `0x10C`) is OUTSIDE the `0x0E..0xFE` block-flush** → it must be
  **direct-poked** (`dg_saturn.cxx:1384`). The enable regs (LCTAU/LCTAL, LNCLEN,
  CCCTL) are inside the flush.

Footprint when on: 0 VRAM banks / 0 CRAM / 0 cycles (rides the K-table +
color-calc registers).

## Gating

The VDP2/RBG0 floor is **one rotation plane** (one height, one transform), so it
ships only where it pays and is safe:

```
rbg0_active = (potato_level == 0) && (sat_local_players <= 1);   // dg_saturn.cxx:2990
```

- **potato-0 only** — the floor's fps value is largest at full detail and shrinks
  at the shipped potato levels.
- **1-player only** — split-screen needs per-viewport transforms, which a single
  RBG0 register set cannot provide.

In any potato level > 0, or in split-screen, the floor **falls back to the
software (CPU) floor** (`sat_vdp2_floor = 0` → sw floor draws, RBG0 display off).
Gate landed in commit `ea6967c`.

## slSynch and VDP1 tearing (do not conflate)

- **slSynch was ABANDONED mid-experiment, NOT disproven.** The session chasing
  the snow got lost; the practical conclusion *"do not use slSynch for the RBG0
  commit — direct-poke RAMCTL+CYC instead"* is correct and is what ships, but the
  broader "slSynch is poison / caps fps" verdict is an **unfinished thread**, not
  a settled proof.
- **VDP1 tearing is a SEPARATE problem.** VDP1 runs **1-cycle AUTO** (`FBCR = 0`)
  and swaps mid-draw. The fix is a properly **draw-gated present** (don't re-kick
  `PTMR` before `CEF`; double-buffer the command list). The naive
  `VDP1_MANUAL_CHANGE = 1` made the **walls vanish**. None of this is an slSynch
  or RBG0 issue.

## 4bpp floor (future option)

Measured over DOOM1.WAD flats: **56% use ≤16 colors**, **44% use >16** (e.g.
`FLOOR4_8` = 21 colors). A 4bpp floor bitmap would halve the bitmap's storage but
cannot represent the 44% of flats with wider ramps without quantization. See
`VDP1_4BPP_STUDY.md` for the storage-vs-quality analysis; 4bpp is a
storage/VRAM lever, orthogonal to fill/overdraw.

## What changed vs the old docs

| Old claim (now stale) | Current reality |
|---|---|
| RBG0 floor is off / ships OFF / still snows | Ships clean on HW as an 8bpp bitmap |
| Floor is a 3-bank cell layout (K+cells+map) | 2-bank bitmap (K in A0, bitmap in A1) |
| `rbg0_commit_cyc` / CYC poke absent from tree | Present and load-bearing (`dg_saturn.cxx:1487`) |
| RDBS = `0x8D` | `0x0D` for the bitmap (no map) |
| "Floor XOR sky" is a hardware law | Lifted — B1 freed; floor + sky coexist |
| Map/overlay (NBG3) can't coexist with floor | NBG3 coexists (L+R toggle) |
| Distance fog via `K_LINECOL` on K-table | VDP2 line-color screen + RBG0 color-calc |
| RBG0 needs per-frame slSynch to commit | No slSynch; direct block-flush at init |
| VDP1 "beats RBG0" for the dominant flat | Dominant flat ships on **RBG0**; VDP1 only ever took *secondary* heights/ceilings (still TODO) |

## Source map

- Build flags: `dg_saturn.cxx:248` (`VDP2_RBG0_TEST=1`), `:261`
  (`VDP2_HW_SKY=0`), `:297` (`RBG0_BITMAP=1`), `:275` (`RBG0_NBG3=1`), `:281`
  (`RBG0_LINECOL_TEST=0`).
- Bank addresses: `dg_saturn.cxx:304-305` (bitmap A1 / K-table A0).
- Floor setup: `rbg0_proto_init`, `dg_saturn.cxx:1389` (bitmap branch
  `:1391-1411`).
- Commit: `rbg0_commit_ramctl` `:1461`, `rbg0_commit_cyc` `:1487`, called
  `:1777-1778`.
- Per-frame transform: `dg_saturn.cxx:3011-3023` (`rbg0_upload_flat` +
  `rbg0_set_transform`, `RBG0_RPT_TRANSFER=2` manual RPT memcpy, no slSynch).
- Gate: `dg_saturn.cxx:2990` (`potato_level==0 && sat_local_players<=1`).
- Distance fog: `rbg0_linecol_apply` `:1378`.
- Commits: `19768ca`, `41dd895` (bitmap floor + tuning); `ea6967c` (gating);
  `597bc68` (NBG3 toggle); `bed9d81`/`bda7fce` (line-color prototype).
