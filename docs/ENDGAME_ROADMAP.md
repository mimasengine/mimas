# Endgame roadmap — running OTHER WADs (Doom II / Final Doom / PWADs)

**The objective is running non-shareware WADs. Shareware `DOOM1.WAD` is only a temporary
dev vehicle** (small, simple — it stresses neither capacity nor completeness). Every lever
must be judged by *"does it help load / survive / play a big WAD?"*, **not** by shareware fps.

This doc is the **umbrella**. It has four axes; **capacity (Axis 1) is already covered in
depth by [`STREAMING_FLUIDITY_ROADMAP.md`](STREAMING_FLUIDITY_ROADMAP.md)** (phases R0–R5) and
is only summarised here. The other three axes (**survival / perf-scaling / correctness**) are
*not* in that doc and are the new material.

---

## 0. The reframe — it already boots Doom II

Mimas is **game-agnostic by design**: `D_FindIWAD` is Saturn-patched to return the fixed slot
`DOOM1.WAD`, and `D_IdentifyVersion` detects the game from lump **contents** (MAP01 → commercial/
Doom II, E1M1 → Doom, …). Any IWAD dropped in as `DOOM1.WAD` (via `build.ps1 -Wad <name>`) boots
at the right gamemode. **Doom II has been loaded to MAP01 and benchmarked on hardware** — but
**no full playthrough of a large map has ever been validated.**

So the endgame is **not** "make it run". It is: **survive the big maps + fit in RAM + stay fluid
+ play correctly.** Four axes, in priority order below.

---

## 1. The reality map (per WAD)

| WAD | Boots | Survives big maps | Real blocker |
|---|---|---|---|
| **Shareware** | ✅ cart (zero-copy map) | ✅ (dev vehicle) | none — temporary |
| **Ultimate Doom** | ✅ streams | ✅ **fits everywhere** (+117 K worst) | ~none — **best big-WAD case** |
| **Doom II** | ✅ streams | ✅ **MAP13 loads (R4 done)** | first wall was **memory FRAGMENTATION** (segs array), fixed by **R4** (Axis 1); crash-caps monitored, NOT hit (`vp30/ds56`) |
| **TNT: Evilution** | ✅ streams | ❌ **23/32 maps don't fit** (MAP20 −318 K) | **memory diet R4** (~−433 K) |
| **Plutonia** | ✅ streams | ❌ 5 maps 1p / 10 in 4p (MAP28 −272 K) | **R4** for the heavy maps |
| **generic PWAD** | ✅ ≤4 MB cart / else streams | depends | no PWAD-merge, no DEHACKED |

**Key split (HW-CORRECTED 2026-07-06):** *Doom II's actual first wall was MEMORY-FRAGMENTATION*, not
the crash-caps first predicted here — MAP13 `Z_Malloc`-failed on the segs array (`P_LoadSegs`: 245 K
free but no 57 K contiguous run), while the crash-caps sat idle (`vp30/ds56` « 256, row 11). **R4
(Axis 1) fixed it** — MAP13 now loads, largest-contiguous 48 K→130 K. *Final Doom is gated by MEMORY
too* (a bigger diet). Crash-proofing (P0) stays a latent risk on open vistas, not yet hit.

---

## 2. The one constraint everything orbits — the 944 KB zone

2 MB work RAM, split into two banks the port treats very differently:

- **LWRAM 1 MB** (SH-2-only, ~2.1× slower): holds the **Doom zone = 944 KB** (`966,656 B` =
  `0x100000 − RP_CMD_BUF_SIZE 0x14000`; `dg_saturn.cxx:1037-1041`). *(Docs/comments still saying
  "884 KB" are stale — that was the old 160 K cmd-buf.)* This one arena must hold the resident
  **PU_STATIC** floor **+** the map's **PU_LEVEL** geometry **+** (streaming) every cached lump.
- **HWRAM 1 MB** (fast, bus-shared): code/.text + .bss + the 320×224 framebuffer + newlib heap +
  the SRL **TLSF pool** (`_end..__heap_end`, ~**49–52 KB** free — boot-loops if starved).
- **4 MB cart** = *optional accelerator only*. Either map a ≤4 MB WAD whole (zero-copy), or, for a
  big streamed WAD, stage each map's LZSS `.DRP` blob CD→cart once/level (→ CD idle → CDDA). **The
  cart does NOT relieve the zone** — only the CD.

PU_STATIC floors (from the memory audits, `STREAMING_FLUIDITY_ROADMAP.md §2.2`): Doom II 494.7 K
/ TNT 618.8 K / Plutonia 552.9 K / Ultimate 403.0 K.

---

## Axis 1 — CAPACITY / streaming  →  see `STREAMING_FLUIDITY_ROADMAP.md`

Fully covered there (R0–R5). One-line summary of the pillars that matter to this roadmap:

- **R4 memory diet (the keystone).** `TEXTURECOLUMNLUMP` lazy directory frees **−157 K (Doom II)
  / −253 K (TNT) / −239 K (Plutonia)** of PU_STATIC. This is the single highest-ROI item: it
  unblocks Final Doom's zone **and** funds headroom for everything else (bigger crash-caps, the
  streaming cache, the split-screen slab). **`STREAMING_FLUIDITY_ROADMAP.md §7 / R4`.**
- **R1/R2 streaming fluidity.** Today every CD read is a **synchronous** `GFS_Load`, one per
  2048-byte sector, blocking on the master → a cold lump mid-frame **stalls the frame** (freezes
  all views in split). The async arsenal (`GFS_Nw*`) is linked but **unused**. **R1** = multi-
  sector bounce, **R2** = async read-ahead.
- **R5 split-screen** streaming specifics (intermission preload, per-view page-in budget).

---

## Axis 2 — SURVIVAL / engine crash-caps  *(NEW — not in the streaming doc)*

**The streaming doc guarantees the map FITS. It does NOT guarantee it doesn't CRASH.** A map that
fits the 944 KB zone can still `I_Error`-freeze on a static render limit. These are the caps a
big Doom II map will hit:

| Limit | Value | Behaviour on overflow | Risk on Doom II |
|---|---|---|---|
| **MAXVISPLANES** | 256 (`-D`, core dflt 512) | **`I_Error` HARD HALT** (`r_plane.c:511/602/1121`) | wide open vistas (MAP13/15) |
| **MAXDRAWSEGS** | 256 (`r_defs.h:51`) | **`I_Error` HARD HALT** (`r_segs.c:1241-1246`) | >256 visible wall segs |
| **zone exhaustion** | 944 KB | **`I_Error` HARD HALT** (`Z_Malloc` fail) | biggest maps / streaming cache |
| VP_POOL_PLANES | 96 | graceful — shared fallback slice = **flat glitch** | common on open maps |
| MAXVISSPRITES | 128 | graceful — `overflowsprite` = sprite vanish | monster-dense rooms |
| MAXSEGS (solidsegs) | 32 | depth-bounded, rarely trips | low (same as vanilla) |

**P0 — crash-proofing** (peer keystone to R4): make the three HARD-HALTs **graceful-degrade** (or
raise them, funded by R4's freed room). This is the difference between *"boots MAP01"* and *"plays
the game"*. Note the coupling: raising the count-caps costs a little zone, so it rides on the R4
diet. VP_POOL/VISSPRITES already degrade gracefully (glitch, not crash) — lower priority, raise
if the glitches are ugly.

---

## Axis 3 — PERF scaling (render + sight)  *(NEW)*

### 3a. Render caps are shareware-tuned
The **mechanisms** generalise (RBG0-dominant floor even wins *more* on a big open vista), but every
**numeric cap** was calibrated on E1 shareware and will **spill or clip** on Doom II:
`WALL_ACC_MAX 128` ("1p peaks ~57 « 128", `dg_saturn.cxx:2943`), `WALL_CMD_CAP 248`,
`WALL_PX_BUDGET 200000`, `FTEX_PX_BUDGET 60000`, `MAX_FLOOR_ACC 80`, `FTEX_SLOTS 7`. On a heavy
map walls/floors spill from VDP1 back to the master CPU (`row-8 fbw/fbf`), stacking on the same
SH-2 that's already paying the ballooning tic. **Generalise/auto-scale the caps for big maps.**

### 3b. Sight / tic — REJECT is DEAD for the endgame; use RAM-free levers
The game-tic (`T ~27-29 ms`, `sat_tic_ms`) is sight-heavy, and **`rejectmatrix` is NULL in
streaming mode** (dropped to save zone), so every `P_CheckSight` runs the full division-heavy BSP
walk. **`REJECT`-keep is not the fix**: its size is **quadratic** (`numsectors²/8`: 300→11 K,
600→45 K, **1000→125 K**) and fragmentation-fatal *precisely at the map sizes where the CPU saving
would matter*. It's a shareware/cart-only artifact. The **right trade is a few KB FIXED** on:

- **Temporal visibility cache** — direct-mapped, keyed on `pnum = s1*numsectors+s2` (already
  computed, `p_sight.c:314`), `{result, tic}`, reused ~4–8 tics. Monsters re-check the *same*
  monster→player pair each tic → ~8–16 KB collapses the repeats. **Constant RAM.**
- **Distance early-out** — `dx*dx+dy*dy > cap` before `P_CrossBSPNode` (dx/dy already in
  `strace`), **zero RAM**. Kills the pathological long walks (A_VileChase, open-map LookForPlayers).
- **Coarse cluster-reject** — K×K over K≪numsectors clusters (64 clusters = 512 B). **Tiny fixed.**
- **Spread across tics** — a small FIFO + per-tic full-walk budget; defers overflow. Bounds T's
  worst case; monster reaction 1–2 tics staler (imperceptible).

All four are **validatable live** via the LOS overlay (`rej`/`walk` per window, row 5) that already
ships. `P_CrossBSPNode` is already a targeted trace (O(path), not O(numsectors)) — so the temporal
cache + distance early-out attack the true residual, not a full matrix.

---

## Axis 4 — CORRECTNESS / mods  *(NEW — mostly cosmetic, one mod-blocker)*

The engine is **complete enough** for the three commercial IWADs: the full 137-entry Doom II
monster/thing set is present (`info.c`), intermission/finale text (`f_finale.c` doom2/tnt/plut),
cast call, secret exits (MAP31/32), status bar — all compiled in.

Gaps:
- **TNT/Plutonia mis-identify as generic `doom2`** (file is always `DOOM1.WAD`, content-scan only
  yields doom2, `-pack` override unreachable) → **wrong intermission level names + wrong finale
  text** (Doom II's C-texts instead of T-/P-texts). Maps play fine. *Cosmetic.*
- **No PWAD merge** (`FEATURE_WAD_MERGE` undef) and **no DEHACKED** (`FEATURE_DEHACKED` undef, no
  parser sources) → blocks the **mod/PWAD endgame** (map packs, gameplay mods). Not needed for the
  3 commercial IWADs.
- Saves are non-functional (fopen to read-only CD) — same as shareware.
- ENDOOM is a no-op on Saturn (harmless).

---

## Priority sequencing (by "unblocks a big WAD", not by fps)

1. **🔑 R4 memory diet** (`TEXTURECOLUMNLUMP` lazy) — unblocks Final Doom's zone + funds everything.
   *(Axis 1 / STREAMING_FLUIDITY_ROADMAP.md R4.)*
2. **P0 crash-proofing** (visplanes/drawsegs/zone HARD-HALTs → graceful) — turns "boots MAP01" into
   "survives any map"; rides on R4's freed room. *(Axis 2.)* **Pairs with #1.**
3. **R1/R2 streaming fluidity** (async read-ahead, multi-sector) — no frame stalls / split freezes.
   *(Axis 1.)*
4. **Perf scaling** (auto-scale render caps + RAM-free sight levers) — fast on heavy maps. *(Axis 3.)*
5. **Cosmetic correctness + mods** (mission detect; then PWAD/DEHACKED for the mod endgame). *(Axis 4.)*

---

## The immediate next step — measure the real walls, not shareware

Stop measuring shareware. Put **Doom II on the disc** (`build.ps1 -Wad Doom2`), **warp to a big map**
(`SAT_WARP_MAP=13` / `15`), and **watch which cap fires first**. Instrument it with a **"limits
high-water"** overlay row (peak visplanes / peak drawsegs / min zone-free vs their caps) — this is
the missing **R0** instrument that de-ambiguates the whole endgame. `r_visplane_peak`,
`Z_FreeMemory`, `Z_LargestAllocatable` already exist; the drawseg high-water needs a tiny core
counter.

---

## File:line index

- Zone / RAM: `src/dg_saturn.cxx:277-285` (bank map), `:1037-1041` (DG_ZoneBase 944 K),
  `:1061` (cart probe), `:2364-2369` (streaming/cart-stage election); `src/syscalls.c:42-55` (heap/TLSF).
- Crash-caps: `core/r_plane.c:49-60` (MAXVISPLANES 512/256), `:254-260` (visplanes+pool alloc),
  `:96-97` (vp_fallback graceful), `:511/602/1121` (I_Error); `core/r_defs.h:51` (MAXDRAWSEGS 256);
  `core/r_segs.c:1241-1246` (drawsegs I_Error); `core/r_things.h:25` + `r_things.c:341-347`
  (MAXVISSPRITES graceful); `core/r_bsp.c:89` (MAXSEGS 32).
- Render caps: `src/dg_saturn.cxx:2789/2706` (WALL_CMD_CAP/bank), `:2943` (WALL_ACC_MAX),
  `:2944-2989` (WALL_PX_BUDGET), `:165/3427/3851/3856` (FTEX budgets/slots), `:1583-1586` (fill% row).
- Sight/tic: `core/p_sight.c:314-350` (reject + full walk), `:110-118/228-242` (FixedDiv residual);
  `core/d_main.c:643` (sat_tic_ms); `core/p_setup.c:715-759` (P_LoadReject/streaming drop);
  `src/dg_saturn.cxx:1587-1599` (LOS rej/walk overlay, row 5).
- IWAD/features: `core/d_iwad.c:702-712` (D_FindIWAD Saturn), `core/d_main.c:1052-1131`
  (D_IdentifyVersion); `core/doomfeatures.h:24/28` (WAD_MERGE/DEHACKED undef);
  `core/f_finale.c:75-94` (finale text); `build.ps1:63-85` (`-Wad`); `Makefile:90-97` (SAT_WARP_MAP).
- Capacity detail: **`docs/STREAMING_FLUIDITY_ROADMAP.md`** (§2.2 audits, R4 diet, R1/R2 streaming).
