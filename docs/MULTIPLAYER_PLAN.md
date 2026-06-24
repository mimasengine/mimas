# DoomSRL — Local multiplayer (multitap) plan

**Status:** **2-player vertical split + per-viewport compact HUD SHIPPED** (Iter 1 plumbing,
the 2-view sequential render, Iter 4's HUD). Still TODO: 4-player HUD, DM polish, auto/forced
potato in MP.

> **RECONCILED 2026-06-24.** parallel-REC is SHIPPED — but as a **WITHIN-VIEW phase-split**
> (P3 plane-split + masked-by-half, the slave draws half the visplanes and the right-half
> sprites of *each* view), **NOT** the per-CPU 2nd-renderer this plan describes. The full
> per-CPU `R_RenderPlayerView` on the slave (Iter 2/3 x-split) was **REJECTED** — it overflows
> the Saturn's 2MB (`r_main.c:1186`, the `RP_SLAVE_BUILD` dual-compile was removed). 2p views
> render **sequentially on the master**, with the slave doing P3+masked phase work within each.
> The +25% 1p x-split bonus and "4p ≈ today's 1p fps" headlines below rest on that rejected
> path and are unreachable as written. See per-section RECONCILED notes.
**Target:** **4-player local split-screen**, potato floor, VDP1 walls, **REC parallelised across both SH-2**.
**Scope:** *local* split-screen only. No link-cable / no netcode (`FEATURE_MULTIPLAYER` stays off). Coop first, then DM.

This file is the resume point. Iterations are independently testable; do them in order.

> **Strategy (decided 2026-06-19).** Shared-view coop was dropped — a single camera is nonsense for an
> FPS. The real architecture: **floor = potato software** (sidesteps the RBG0 single-matrix limit),
> **walls = VDP1** (capacity-validated for 2/4 players, ceiling is REC×N — see [[doomsrl-vdp1-capacity]]),
> and the bottleneck is **REC generation, which is master-only today while the slave is ~80% idle**. So
> the lever is: **make the idle slave render too.** The unit of work is a task = **`(player, x-range)`**,
> partitioned so both CPUs are always busy:
> - 1 player → split by **x** (left/right) → **1-player speed bonus** (the same mechanism serves 1p).
> - 2 players → one per CPU.
> - 4 players → two per CPU.
>
> This is "global" REC sharing that **also benefits single-player**, and it avoids the producer/consumer
> coherency hazard that got the intra-view `Bw‖Bp` / plane-offload splits **rejected** ([[doomsrl-perf]]):
> spatially-disjoint tasks each own their render state — no two CPUs write the same visplanes/clip.

---

## 1. Current state (what's stubbed / single-view-hardcoded)

| Layer | Where | State |
|-------|-------|-------|
| Netcode | `core/doomfeatures.h:32`, `core/config.h:97` | `FEATURE_MULTIPLAYER`/`ORIGCODE` **undef** → no net code |
| Game settings | `core/d_loop.c:439-449` (`D_StartNetGame` `#else`) | forces `num_players=1`, `consoleplayer=0` |
| Lockstep loop | `core/d_loop.c:188-189`, `:811-813` (`SinglePlayerClear`) | builds a ticcmd only for `localplayer`; wipes `ingame[1..3]` |
| Input | `src/dg_saturn.cxx:902-944` (`poll_pad`/`DG_GetKey`) | reads **only** `Smpc_Peripheral[0]` → key queue |
| Render | `core/d_main.c:287` | one full-screen `R_RenderPlayerView(players[displayplayer])` |
| Render state | core `r_*.c` BSS globals (visplanes, drawsegs, clip, ds_*, rw_*) | **one global instance** — must become per-CPU (§2.4) |
| Composition | `src/dg_saturn.cxx` | hybrid is full-screen-hardcoded: `WALL_FLAT_YHI=191`, `HUD_Y=168`, `memset(framebuffer,0,192*320)`, index-0 scrub `192..223`, VDP1 system clip `319,223` — all → per-view |

**Already in our favour**
- `MAXPLAYERS == 4` (`core/doomdef.h:45`); shareware maps have coop + DM starts.
- Multitap free: SGL fills `Smpc_Peripheral[0..5]` for the port-1 tap (`sl_def.h:2274`); reading `[1..3]`
  directly (like `poll_pad`) gives pads 2-4. No special init.
- **Slave is ~80% idle at pot1/2** ([[doomsrl-perf]] 6-photo set) — the whole basis of the plan.
  *RECONCILED 2026-06-24: the slave is no longer doing-nothing — it runs P3 plane-split +
  masked-by-half within each view (both SHIPPED). The ~80% idle is the headroom AFTER that work.*
- VDP1 walls **hold for 4 players** (capacity reviewed) — walls are NOT the ceiling; REC is.

---

## 2. Design invariants (read before touching `core/`)

`core/` is shared **verbatim** with DoomJo (GCC 9.3, pure C). Every change must:

1. **Be behaviourally identical for DoomJo single-player.** Drive everything off a runtime global
   defaulting to single-player — no `#ifdef` per port.
2. **Link without any DoomJo change.** Never call a platform symbol directly from shared core (DoomJo
   would fail to link). Use a **function pointer** defaulting to `NULL`; the platform assigns it.
3. **Stay pure C** (GCC 9.3): no unnamed params, no C++isms.
4. **Per-CPU render state + allocator gate (the parallel-REC foundation, §4 Iter 2).** Running a second
   `R_RenderPlayerView` on the slave means the mutable `r_*` working set (visplanes[], drawsegs[],
   openings[], ceilingclip/floorclip, ds_*, rw_*, solidsegs, etc.) must be **instanced per CPU** — two
   tasks must never share them. And the per-visplane flat fetch + first-sight texture composite call the
   **zone allocator** (`W_CacheLumpNum`/`R_GenerateComposite` → `Z_Malloc`) which is global mutable state
   → two CPUs there = heap corruption (the freeze zone). Mandatory **pre-cache gate**: before the parallel
   section, resolve+lock every flat and pre-generate every composite **both tasks need** (a per-task
   visibility pre-pass), so the parallel render is allocator-free. On-cart flats are free pointers;
   off-cart (CD/1MB) and composites always allocate → **the gate must work without assuming the cart.**
   This is heavy DoomJo-core divergence → gate the whole parallel path DoomSRL-only.

   > **RECONCILED 2026-06-24.** This mandatory pre-cache gate (B4) was **never built as a general
   > gate** and is now **only needed if the rejected concurrent 2nd-renderer is revived**
   > (STILL-TODO, moot). The SHIPPED parallel path is the **phase-split** (P3 planes + masked),
   > which avoids the freeze zone WITHOUT a general gate **by construction**: the P3 worklist
   > pre-stores the flat `src` pointer so the slave never allocs, and the masked phase runs
   > AFTER composite generation. `r_main.c:1038-1040` confirms `SAT_XSPLIT_PARALLEL=0` precisely
   > because the B4 gate is absent. Per-CPU render state did NOT become a full second renderer
   > (2MB overflow) — only the narrow P3/masked per-CPU column/visplane state shipped.

### Feature-flag surface (proposed)
Define in `core/g_game.c` (next to `deathmatch`/`netgame`), extern in `core/doomstat.h`:
```c
int  sat_local_players = 1;                              /* 1 = single player (default) */
int  sat_deathmatch    = 0;                              /* 0 coop, 1 DM, 2 altdeath    */
void (*sat_build_local_ticcmd)(ticcmd_t*, int) = NULL;   /* platform hook, players 1..N-1 */
```
Default (`sat_local_players==1`, hook `NULL`) → DoomJo + shipping build unchanged.

### HUD dependency — DONE (per-viewport compact HUD)
Each viewport gets a **160×64 compact HUD** (two 32px rows) recomposed from the original STBAR:
`tools/make_hud2p.py` decodes STBAR + moves its wells/labels into two rows, emitting the panel pixels
(`src/hud2p_panel.h`) + the shared field anchors (`core/hud2p_layout.h`). The core draws each player's
widgets via **`ST_DrawCompactWidgets(pnum, ox, oy)`** (st_stuff.c) — no backing-screen/diff, reads
`players[pnum]` directly, so it is safe per player; the platform blits the panel and calls it per
viewport in `DG_DrawFrame` (P1 @ (0,160), P2 @ (160,160)). The 3D view is 160px tall; the bottom 64px
hold the two blocks. `ST_Drawer`'s single 320 bar is skipped in 2p. Per-player flash (damage/pickup) is
a **per-half software wash** (`ST_PlayerPaletteIndex` + nearest-colour LUTs, dg_saturn) since the hardware
palette is shared. Weapon dropped to the view bottom (`sat_psprite_yoff`). Sky forced to **software** in
2p (NBG0 is one layer, can't serve two viewangles).

---

## 3. Perf model — the `(player, x-range)` task split

### 3.1 What scales, and along which axis

`R_RenderPlayerView` REC = **Bw** (BSP walk) + **Bp** (wall-prep, per-column) + **P** (planes/floor) +
**M** (masked/sprites). Today all master-only; rows 19 (REC/EX/W), 20 (Bw/Bp/P/M).

| Term | Scales with | In a split |
|------|-------------|------------|
| **Bw** (BSP walk) | the view's geometry/FOV, **not pixels** | **re-walked per task** (different camera, or same camera re-culled to the x-range) → the **duplication tax**, ~0.65× for half-width |
| **Bp** (wall-prep) | **screen WIDTH** (per-column work) | ∝ width fraction — **independent of height** |
| **P** (planes) | **area** (width×height) | ∝ area; potato keeps it cheap |
| **M** (masked) | **area**, small | ∝ area |

**Key consequence: split by X (vertical, left/right) is cheaper than by Y (horizontal, top/bottom)** —
because the dominant per-view term Bp scales with width, not height. So 2-player should be a **vertical**
(left/right) split, and the 1-player bonus is a vertical x-split.

### 3.2 The bus-contention factor **S** (the #1 unknown)

REC is **bus-bound** (the project's core finding: BSP pointer-chase + visplane/texture reads on a single
shared bus). Two CPUs doing REC concurrently get **sublinear** speedup. Model: total REC work `W` across 2
CPUs completes in wall-clock `W / S`, with **parallel speedup `S ∈ [1, 2]`**:
- `S = 2` ideal (no contention), `S = 1` fully serialised (no gain).
- Reference: the dual-CPU **blit** measured **S≈1.33** (pure bandwidth, near-saturating). REC is more
  **latency**-bound (pointer-chasing → bus idle during stalls) → parallelises *better* → plausibly **S ≈
  1.5–1.8**. **MEASURE on hardware** (invisible on Ymir — memory-bound). Everything below uses **S=1.5
  central**, range 1.4–1.8.

### 3.3 Costed estimate

> **RECONCILED 2026-06-24 — the x-split / 2nd-renderer rows below are REJECTED.** The full
> per-CPU `R_RenderPlayerView` on the slave overflows 2MB (`r_main.c:1186`; the dual-compile was
> removed; `sat_xsplit` is `#define SAT_XSPLIT 0`, compiled off). The **"1p x-split (bonus) +25%"**
> row and the **"4 players ≈ today's 1-player fps"** headline are **unreachable as written** — they
> assume a 2nd view-renderer that does not exist. The SHIPPED path is the phase-split (P/M on the
> slave within one master-driven view), not a per-view 2nd renderer. The model below is kept for
> history; treat its x-split/4p-quadrant fps as theoretical-only.

Representative 1-view REC at the ship config (from the HW 6-photo set, pot1 exploring — re-measure):
**Bw 8 · Bp 21 · P 15 · M 4 → REC ≈ 48 ms.** Per-task REC = `Bw·b(wf) + Bp·wf + P·(wf·hf) + M·(wf·hf)`
(wf,hf = width/height fraction; `b(0.5)≈0.65`, `b(1)=1`). "Other" (EX-potato ~3 + blit ~12 + sim ~8 +
st/hu) ≈ **28 ms**, roughly constant (blit fills the same 320×224 regardless of view count; **note the
slave can't also dual-CPU-blit in MP — it's busy rendering**, so MP keeps the single-CPU ~12 ms blit).

| Config | Views × size | Tasks (per CPU) | Σ REC work | REC wall-clock (÷S=1.5) | Frame | fps | vs 1p today |
|--------|--------------|------------------|-----------|--------------------------|-------|-----|-------------|
| **1p today** | 1 × 320×192 | 1 (master) | 48 | 48 | 76 | ~13 | — |
| **1p x-split (bonus)** | 1 × 320×192 | 2 (1+1) | 50.4 | **33.6** | 61.6 | **~16** | **+25 %** |
| **2p vertical** | 2 × 160×192 | 2 (1+1) | 50.4 | 33.6 | 61.6 | ~16 | +25 % |
| **2p horizontal** | 2 × 320×96 | 2 (1+1) | 77 | 51 | 79 | ~13 | ~0 % |
| **4p quadrants** | 4 × 160×96 | 4 (2+2) | 81.8 | **54.5** | 82.5 | **~12** | −7 % |

Headline (if S≥1.4 and Bw stays modest): **4 players ≈ today's 1-player fps**, and a **free ~+25 %
single-player bonus** from the same machinery. *RECONCILED 2026-06-24: REJECTED — both headlines
depend on the per-CPU 2nd-renderer that overflows 2MB and was removed (see §3.3 note).* Sensitivity
at the range ends:

| Config | S=1.4 | S=1.8 |
|--------|-------|-------|
| 1p x-split REC | 36.0 | 28.0 |
| 4p quadrant REC | 58.4 | 45.4 |

### 3.4 Caveats that cap the optimism (read these)

1. **S is unmeasured.** If the bus serialises REC (S→1.0), 4p REC = 81.8 ms = +70 % → ~8 fps. The whole
   plan rests on **S > 1.3 — validate it FIRST** (Iter 2, 2-player, read row 19 REC vs the 1-view REC).
2. **Bw duplication tax is scene-dependent.** 4p re-walks the BSP 4× (4×~5 ms here, but Bw hit **26 ms**
   in dense rooms → 4×0.65×26 ≈ **68 ms of BSP alone** → 4p tanks). Dense-geometry scenes are the 4p
   worst case; open/arena scenes are benign (→ 4-player **DM in closed arenas** is the sweet spot).
3. **Load imbalance.** Static `(player,x)` assignment ⇒ wall-clock = `max(CPU_A, CPU_B)`, not the average.
   A firefight on P1+P3 (master) with P2+P4 idle ⇒ master-bound. REC isn't EX — the EX two-pointer
   work-steal doesn't apply to recursive BSP. Mitigation: assign so each CPU gets one likely-heavy + one
   likely-light view; revisit if imbalance bites.
4. **Slave runs a FULL renderer**, not today's column-draw consumer (`rp_slave_body`). This is the
   engineering bulk — a second renderer instance on the slave with its own state, not "give the idle
   slave some work" (the perf notes already flagged "reuse the dispatch = a rewrite").
   *RECONCILED 2026-06-24: this was TRIED and REMOVED — a full per-CPU renderer overflows 2MB
   (`r_main.c:1186`). The shipped slave work is the phase-split (P3 planes + masked-by-half), which
   keeps the slave as a within-view phase worker, not a 2nd renderer.*
5. **Sim scales with players in combat** (4× the player actions, hitscan, projectiles) — shared world, but
   the combat T spike could grow. Coop with monsters > DM nomonsters.
6. **No dual-CPU blit in MP** (slave busy) → blit stays ~12 ms; 1p keeps the dual-CPU blit win.

### 3.5 Per-task viewport setup (mechanics)
Factor `R_SetViewWindow(x,y,w,h)` out of `R_ExecuteSetViewSize` (`core/r_main.c:670`) — explicit origin
(not centered), recompute the size-dependent tables (yslope/distscale/light/texmap) only when the **size**
changes (all tasks of one config share a size → compute once); only `viewwindowx/y` + `ylookup`/`columnofs`
change per task (cheap). With per-CPU render state these tables are also per-CPU.

---

## 4. Iterations

### Iter 0 — Multitap input probe (read-only, de-risk hardware)
- **Touch:** `src/dg_saturn.cxx` — one overlay line printing `id`+`data` for ports 0..3.
- **Accept:** multitap configured (Kronos/SSF/Mednafen) → 4 ids `0x02`, button bits move per pad.
- **Risk:** none. Revert after.

### Iter 1 — N-player plumbing (input → ticcmd → sim) — **SHIPPED**
*RECONCILED 2026-06-24: SHIPPED + committed. `src/mp_input.cxx` (`DG_BuildLocalTiccmd` reads
`Smpc_Peripheral[p]`, multitap port-1 with port-2 fallback) wired via the `sat_build_local_ticcmd`
hook (`sat_mp_input_init`, `main.cxx:57`); `sat_count_local_pads`, `sat_mp_pad2_a` co-op opt-in,
title arming `dg_saturn.cxx:2788-2797`. 3/4p use a J3→J1/J4→J2 mirror harness on emulator; the real
4-pad multitap path is coded but HW-unverified. DM input/mode still unwired.*
- **Goal:** drive 2-4 player mobjs from the pads. Shared single view here is a **throwaway test harness**
  (you'll see the other marines move) — **not** a shippable mode.
- **Touch:** the §2 globals; `D_StartNetGame` `#else`; `D_ConnectNetGame` netgame flag; `BuildNewTic`
  (fill `cmds[p]`+`ingame[p]` for locals via the hook); `TryRunTics` gate `SinglePlayerClear`;
  `DG_BuildLocalTiccmd(cmd,p)` reading `Smpc_Peripheral[p]`.
- **Accept:** player-2 marine spawns and moves with pad 2; coop frags/respawn OK.
- **Risk:** low (defaults keep 1p/DoomJo identical).

### Iter 2 — Per-CPU render state + allocator gate → **2-player vertical split** (THE foundation)
*RECONCILED 2026-06-24: 2p vertical split is **SHIPPED** — but via **SEQUENTIAL render on the master**
(P1 then P2 into the two half-views, `d_main.c:320-364`), NOT the master/slave 2nd-renderer described
here. That concurrent per-CPU full renderer was **REJECTED** (overflows 2MB; dual-compile removed;
`SAT_XSPLIT=0`). The slave's contribution is the **phase-split SHIPPED within each view**: P3 plane-split
(`r_parallel.c:799-879` `RP_DispatchPlanes`, gate `sat_plane_parallel=1`) + masked-by-half
(`r_parallel.c:881-909` `RP_DispatchMasked`, slave draws RIGHT-half sprites, gate `sat_masked_parallel=1`).
The B4 allocator pre-cache gate was NOT built (STILL-TODO, only needed to revive the rejected renderer);
the phase-split sidesteps the freeze zone by construction. **S was never validated as >1.3 for a plane
split** — the dual-CPU blit reference is S~1.3 (bus-capped), and the dual-CPU blit itself was DROPPED on
HW (`blit_mode` boot default 0 = single-CPU).*
- **Goal:** master renders P1 into the left half, slave renders P2 into the right half — each a **full
  `R_RenderPlayerView` with its own render-state instance**. This is the highest-risk, highest-value piece.
- **Touch:** instance the mutable `r_*` working set per CPU (§2.4); build the **pre-cache/lock gate** for
  both views' flats+composites (validate WITHOUT the cart); slave dispatch runs a full render task (new
  slave program); `R_SetViewWindow` (§3.5); `D_Display` renders the two tasks.
- **Accept:** two correct independent views; **no heap corruption** over long play (gate works); **measure
  S** = (2×one-view REC) / (row-19 REC) — the number the whole plan hinges on.
- **Risk:** **high** — freeze zone (2nd writer + allocator). 2 players is the simplest balanced partition
  to prove it on.

### Iter 3 — `(player, x-range)` task partitioner → **1-player bonus + 4-player** — **REJECTED (x-split path)**
*RECONCILED 2026-06-24: the `(player, x-range)` 2nd-renderer partitioner is **REJECTED** — `sat_xsplit`
is `#define SAT_XSPLIT 0` (`r_main.c:1024`), `SAT_XSPLIT_PARALLEL 0` (`:1040`); a concurrent 2nd full
renderer overflows 2MB (`d_main.c:325`). The **1p x-split +25% bonus** never materialised. 3/4p views
DO render (sequential, quadrant rects 160×112, no HUD) but ride the master, not a task pool.*
- **Goal:** generalise Iter 2's two render instances into a task pool partitioned by `(player, x-range)`.
  Falls out of the same machinery: **1p x-split** (the +25 % bonus) and **4p quadrants** (2 tasks/CPU).
- **Touch:** the partitioner (player count → task list); per-task viewport rect; load-balance assignment.
- **Accept:** 1p faster than today (REC↓, matches the model); 4p renders 4 correct views; REC tracks §3.3.
- **Risk:** med (reuses Iter 2's foundation). Watch Bw-tax in dense scenes (caveat 2).

### Iter 4 — Per-viewport HUD + potato + DM polish
- **HUD: DONE (2-player)** — compact 160×64 per-viewport HUD (see §2 "HUD dependency"): STBAR-recomposed
  panel + `ST_DrawCompactWidgets` + per-half flash wash + weapon drop + software sky in 2p. Open items:
  face is health-based (no animation/attacker yet); `sat_psprite_yoff=30` to fine-tune on HW; the flash
  LUTs build once on the first 2p frame (~0.2-0.3s one-shot, nearest-colour over PLAYPAL).
- **force/auto potato in MP: PARTIAL** *(RECONCILED 2026-06-24)* — the low-detail-in-split plumbing is
  SHIPPED (`sat_split_lowdetail` driven by the potato Z-cycle `dg_saturn.cxx:388`, applied as
  `detailshift = sat_split_lowdetail` in the split block `d_main.c:341`), but entering split does NOT
  auto-bump the potato level (still manual via the pad-Z cycle), and there is no fps-adaptive controller.
- **TODO:** the **force/adaptive** potato trigger (above); remaining single-view-hardcoded composition
  constants (§1) → per-view; DM mode (`sat_deathmatch`); a 4-player HUD layout (the 160×64 block is 2p-specific).
- **Accept:** 4 views with HUDs; 4-player DM in a closed arena = the sweet spot (caveat 2).

---

## 5. Test setup
**Emulators with multitap (Windows):** Kronos / YabaSanshiro (easiest pad mapping), SSF (most accurate),
Mednafen. Port 1 = 6Player/multitap, map N pads to keyboard sets / USB gamepads. Without 4 controllers:
1 keyboard + 3 USB pads, or 2 key clusters for 2p. **Real hw:** 6Player adaptor in port 1.
**Note:** REC-gain numbers (incl. S) are **hardware-only** — Ymir understates memory-bound cost. Ymir
validates *correctness* (no corruption, views right, allocator gate holds), not the fps/S verdict.

**Order:** Iter 0 → 1 → 2 (measure S, the go/no-go) → 3 → 4. Start coop (`sat_deathmatch=0`).

---

## 6. Open decisions
- 3-player layout (one full-height + two stacked? three vertical strips?).
- Vertical (left/right) confirmed cheaper than horizontal for 2p (§3.1) — confirm it's acceptable visually.
- Load-balance: static `(player,x)` assignment vs a cheap heuristic (last-frame REC per task).
- Ship default stays single-player; multi behind a menu/boot toggle. The 1p x-split bonus could ship
  **independently** of multiplayer (it's just Iter 2+3 at `sat_local_players==1`).
- Save/load disabled in `netgame` (fine for coop); sound origin = player 0's ears (fine in split).
