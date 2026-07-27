# Flicker — hardware test protocol (single session)

**One HW session, one question:** *what makes the VDP1 command list overrun* (the "transfer‑over"
that drops late commands → monsters/walls blink)? We MEASURE it with the row‑12 **V1 probe** and
narrow the cause with the **L+Z isolation modes**.

> **Real Saturn only.** Emulators (Ymir) draw the whole VDP1 command list instantly, so they never
> overrun and **never flicker** — on Ymir `LP` reads **100** (correct: the list always completes).
> The real transfer-over (`LP<100`) only happens on hardware, where the plot can run out of frame
> time. **HW-verified 2026-07-26.** Background: `docs/VDP1_LIMITS_SOURCED.md` (SEGA figures + probe).

> **FIX v1 is now in this build (2026-07-26, HW-UNVALIDATED).** The 1st HW session proved it: in a
> combat scene the monsters overran the list (`i0` = LP 94-97%) while `i1` (no monsters) = LP100 →
> the monsters were the tail that got cut = the blink. The fix feeds `LP%` into the things budget:
> when the plot overran last frame the far monsters are moved to the **software** fill (visible +
> stable, NOT vanished) until `LP` returns to 100. **What to verify on HW:** in the same combat
> scene, `i0` should now hold **`LP` ≈ 100 with the monsters no longer blinking** (the farthest few
> may look a touch less crisp = software). If `LP` still dips well below 100 in `i0`, or a monster
> still blinks, note the lowest `LP%` — that means the walls themselves overrun (next lever:
> `SAT_WALL_CPU_SPAN`). The `L+Z` modes below still work for narrowing that down.

---

## The build
- ISO: `build/Mimas.iso` (branch `flicker-clean`). Boots the shareware Doom, single render mode.
- The debug overlay is ON. **One row matters — the row that starts with `V1`** (about the middle):

```
V1 c142 B131 LP92% ec8 ws480 tx18 i0
```

| Field | Meaning | What to watch |
|-------|---------|---------------|
| `c142` | VDP1 commands emitted last frame (max 248) | the raw COUNT — rarely full |
| **`B131`** | **MEASURED VDP1 budget in commands (read from LOPR)** — what the plot TIME actually paid for | when **`c > B`** the tail overran; the allocator caps things to fit `B` |
| **`LP92%`** | **% of the list the VDP1 finished** | **100 = finished. <100 = OVERRAN = the flicker.** (always 100 on Ymir — it never overruns) |
| `ec8` | things the allocator put on VDP1 (rest → software) | drops when `B` is tight = far monsters shed to software (visible, stable) |
| `ws480` | near‑wall→software span (LOD) | drops **below 480** only if the walls alone overrun + the master has room |
| `tx18` | texture‑cache slots used (max 22) | the VRAM budget |
| `i0` | isolation mode (see below) | which layers are on VDP1 |

**Golden rule:** a scene **flickers** exactly when **`LP` drops below 100%**. Everything below is
about finding *which layer* pushes `LP` down.

> On **Ymir** `LP` reads **100** (the list always finishes) — use Ymir to verify the build/modes, and
> read the real `LP` (<100 where it overruns) on a Saturn. The `c` command count is accurate on both.

---

## The one control: **L + Z** cycles the isolation mode (`i`)
Hold the **L** shoulder trigger (R released) and tap **Z**. Each tap advances `i` 0→1→2→0. The
incidental one‑tap is harmless; re‑centre and stand still. **The weapon stays on VDP1 in every mode**
(routing it off has a separate glitch, so it's left on).

| `i` | On VDP1 | Purpose |
|----|---------|---------|
| **0** | walls + monsters + weapon | normal game (baseline) |
| **1** | walls + weapon (**no monsters**) | the `LP` change vs mode 0 = the **monsters'** share of the overrun |
| **2** | **flat** walls + weapon (no monsters) | untextured walls: same shape, half the per‑pixel cost → separates **texture fill** from **overdraw geometry** |

---

## Procedure — EVERY step at the SAME spot, standing still
1. Boot → **New Game** (any skill). Walk to a spot where the flicker is **clear and constant**:
   several monsters + textured walls in view (a room with 3+ enemies), ideally facing a **near
   wall** (the flicker is worst when a wall is close/large on screen).
2. **Stand still**, facing that scene. Do **not** move again until the test is done (moving changes
   the scene and invalidates the comparison).
3. For each mode below: **record a 5‑second VIDEO** (the flicker is intermittent — a still photo
   misses it) and, from the video, note the **lowest `LP%`** you can read on the `V1` row and
   whether it flickers.

| # | Do this | `i` should read | Flicker? | lowest `LP%` | video |
|---|---------|-----------------|----------|--------------|-------|
| 0 | nothing (baseline) | `i0` | | | |
| 1 | **L+Z** ×1 → walls + weapon, no monsters | `i1` | | | |
| 2 | **L+Z** ×1 → flat walls + weapon | `i2` | | | |
| 3 | **L+Z** ×1 → back to normal | `i0` | — reset — | — | — |

Also, **once** (any mode), read the raw **`L…/…`** hex on the `V1` row and note it with the `LP%`
next to it — a one‑time check that the `LP%` math matches the hardware.

Verdict per row: **GONE · MUCH LESS · A LITTLE LESS · SAME**, plus the `LP%`.

That's the whole session: **three short videos**, each with its `i` mode, its flicker verdict, and
its lowest `LP%`.

---

## What the results tell us (developer — no action needed from the tester)
- **`i0` flickers (`LP<100`), `i1` recovers to `LP100`** → the **monsters** are what overrun the
  list → cap/shed far monsters (the count budget).
- **`i1` still flickers (`LP<100`) with monsters gone** → the **walls alone** overrun → the fix is
  the near‑wall software‑fallback threshold (`SAT_WALL_CPU_SPAN`), not the monsters.
- **`i2` (flat walls) recovers `LP` where `i1` didn't** → it was the **per‑pixel texture fill** of
  the walls → flat/banded far walls is the lever.
- **`i2` still flickers like `i1`** → it's **overdraw geometry / command iteration**, not fill →
  the SEGA‑documented near‑wall off‑screen span (`docs/VDP1_LIMITS_SOURCED.md` §1.3) → bound it
  with `SAT_WALL_CPU_SPAN` / the `Pclp` pre‑clip bit.
- **`LP` never drops below 100 anywhere but it still visibly blinks** → the overrun is not in the
  wall list (present/floor path) → investigation moves off this axis.

`LP%` is the ground truth in every row: it should read **100 when a mode looks stable** and **dip
below 100 exactly when that mode blinks**.
