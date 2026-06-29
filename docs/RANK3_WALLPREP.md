# RANK 3 — Wall-prep slave offload (design)

> **STATUS: SETTLED-NEGATIVE.** Wall-prep→slave is **DEAD, confirmed 3×** on HW (memory-bound,
> not offloadable — see §6/§6b). This doc is the canonical record of that negative. The §3 win
> estimate and the §4 inc-2/inc-3 staged plan below are the **tested hypothesis**, kept for
> provenance — they are NOT a live roadmap (inc-2 is NO-GO). NOTE: only `sat_wallprep_slave` is
> dead; the dual-SH2 **plane work-steal `sat_plane_steal` SHIPPED default-on** (parent `4857f87`)
> and stays. For the live render-path picture see
> [`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md) (RBG0 floor — the real `P` lever)
> and [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md) (VDP1↔NBG1 present).

> **Source.** Study→design→verify workflow (2026-06-26), grounded in d32xr
> (`saturn-refs/d32xr`) + the Mimas scaffold, with 3 adversarial verifiers. Goal:
> move the **`Bp` wall-prep bucket (~21 ms, `R_StoreWallRange`/`R_RenderSegLoop`)** off
> the master onto the idle slave SH-2. `Bp` is the **only big master bucket that does NOT
> collapse with potato** (`P` floor-fill collapses; `Bp` stays), so this is the lever for
> the playable pot1/pot2 modes. See [`CRITICAL_PATH.md`](CRITICAL_PATH.md) §3 RANK 3.

## 1. What d32xr does (the reference)

d32xr ("Doom 32X Resurrection") **pipelines** the BSP walk against wall-prep across its two
SH-2s, rather than running them as two serial passes:

- **Master / phase 1** (`r_phase1.c`): BSP descent + occlusion (`solidsegs`) + bump-allocs a
  `viswall_t` + **`R_WallEarlyPrep`** (cheap geometry: heights, texnums, actionbits). After
  each wall, `Mars_R_WallNext()` bumps a "produced" counter.
- **Slave / phase 2** (`r_phase2.c:346`): chases the producer and runs **`R_WallLatePrep`**
  (`R_PointToDist`, `R_ScaleFromGlobalAngle`, scale/scalestep) + **`R_SegLoop`** (per-column
  clip + visplane alloc) — i.e. d32xr **splits vanilla `R_StoreWallRange` in half**.
- **Handoff** = a lock-free single-producer/single-consumer queue: a shared `viswalls[]`
  array + two 8-bit counters (`addedsegs`/`readysegs`) packed in one comm register, ended by
  a `-2` sentinel (`r_phase2.c:357-401`). The master keeps walking the BSP **while** the slave
  consumes → that is the overlap that yields the win.
- **Why d32xr can put clip+visplane on the slave:** that state is **slave-PRIVATE local**
  (`clipbounds_`, `visplanes_hash_` are on the secondary's stack, `r_phase2.c:352-369`).

## 2. The Mimas cut (lower-risk adaptation)

Mimas's `R_StoreWallRange_impl` (`r_segs.c:762-1135`) is **monolithic** — splitting it into
early/late halves like d32xr is a large, risky rewrite. And `R_RenderSegLoop` uses **GLOBAL**
`floorclip[]`/`ceilingclip[]`/`ds_p`/`lastopening`/`lastvisplane` (not slave-local like d32xr).
So the Mimas cut is **not** "split each seg across both CPUs." It is:

> **Run the WHOLE sequential `R_StoreWallRange` flush on the slave, as the single in-order
> consumer, pipelined behind the master's continued BSP walk.**

Single consumer processing `walljobs[0..n)` in index order (= BSP order) **preserves the
occlusion ordering automatically** (identical to today's serial `RP_FlushWalls`,
`r_segs.c:748`). The win is **not** per-seg parallelism; it is **overlapping the slave's
serial flush with the master's BSP walk**. The existing scaffold already fits:
`RP_QueueWall` snapshots 7 `walljob_t` fields during the walk (`r_segs.c:730-743`),
`RP_FlushWalls` replays `R_StoreWallRange` (`r_segs.c:748`), gated by `sat_wallprep_defer`.

| Work | CPU |
|---|---|
| BSP descent `R_RenderBSPNode`, occlusion `solidsegs`, `R_Subsector`→**`R_FindPlane`**, `RP_QueueWall` snapshot, `++walljob_n` | **master** (producer) |
| the full `R_StoreWallRange` per queued wall — `floor/ceilingclip` writes, `ds_p++`, **`R_CheckPlane`** splits, `lastopening+=`, per-column texturecolumn + `0xffffffff/rw_scale` | **slave** (single in-order consumer) |

## 3. Realistic win & the honest catch

> **(Tested hypothesis — DISPROVEN by §6. The slave is +5.8 ms SLOWER at wall-prep, so the
> projected ~8 ms win never materialised. Kept for provenance.)**

**Win ≈ `min(Bw, Bp)` ≈ the BSP-walk time hidden under the flush ≈ ~7–8 ms.** Master B-phase
drops from `(BSPwalk ~8 + flush ~21) = ~29 ms` to `max(8, 21) = ~21 ms` (the master finishes
its walk at ~8 ms, then waits ~13 ms for the slave). That's ~8 ms, **potato-persistent**:
- pot0 (MST ~130): ~6 % → ~7.7→8.2 fps
- pot1/pot2 (MST ~75–80): **~10–11 %** ← the real value, where `P` has already collapsed

> **Why not the full 21 ms?** The master's next phase (planes `P`) depends on the flush output
> (visplanes), so after its walk the master must wait — it has no further independent work to
> overlap. The win is capped at the BSP-walk it hides, not the whole flush.

## 4. Staged plan (each behind the pad toggle `sat_wallprep_slave`, default 0)

> **(Tested hypothesis. inc-1 was built & validated; inc-2/inc-3 are NO-GO — §6 measured the
> slave +5.8 ms slower, killing inc-2 before the build. Kept as the proposed plan that the HW
> numbers refuted.)**

### inc-1 — slave runs the flush, NON-overlapped (correctness harness, **ZERO fps win**)
- `r_parallel.c`: add `rp_wallprep_slave`/`rp_wallprep_body`/`RP_DispatchWallPrep`/
  `RP_WaitWallPrep`/`sat_wallprep_slave`, modelled on `RP_DrawPlanesSplit`/`RP_WaitPlanes`
  (`r_parallel.c:907-945`): args via param (after the purge), `rp_run_on_stack` (4 KB stack),
  `WP_DONE` via the `|0x20000000` uncached alias set last, two-purge sandwich, 30 M-spin
  timeout → serial `RP_FlushWalls` fallback, `rp_sgl_workptr_reset()` first.
- `r_segs.c`: refactor `RP_FlushWalls`→`RP_FlushWallsRange(from,to)`.
- `r_main.c:1082`: `if (sat_wallprep_slave) { RP_DispatchWallPrep(); RP_WaitWallPrep(); } else RP_FlushWalls();` (requires `sat_wallprep_defer=1`).
- `src/main.cxx`: pad toggle (model on `sat_plane_steal`) + force `rp_disabled=1` when on.
- **Safe** (BSP fully done before dispatch → master quiescent → single-writer slave).
- **Signal:** render **byte-identical** (no wall/floor gaps); `Bp` leaves the master timeline
  but **MST/fps unchanged** (master idles in the wait, `w`≈21 ms). Validates cache/stack/
  coherency before chasing the overlap. *Adversarial verdict: coherency & stack HOLD.*

### inc-2 — pipeline the flush behind the BSP walk (**the ~8 ms win**)
- Dispatch `RP_DispatchWallPrep()` **before** `R_RenderBSPNode` (`r_main.c:1078`); the slave
  chases the live `WP_PRODUCED` (uncached alias of `walljob_n`); master sets `WP_EOF=1` after
  the walk; `RP_WaitWallPrep()` joins before `r_main.c:1084`.
- **Two prerequisites (the verified risk):**
  1. **Visplane-allocator partition.** During the overlap the master's `R_FindPlane`
     (`r_bsp.c:536,546`) and the slave's `R_CheckPlane` (`r_segs.c:1097,1100`) **both bump
     `lastvisplane`** → live race. Fix: fork the `SAT_VISPLANE_POOL` bump cursor per-CPU
     (`R_PoolSlice`, `r_plane.c:524`) — master cursor A, slave cursor B, disjoint slices.
     (Cheaper interim: lag the slave's first `R_CheckPlane` until `WP_EOF` — smaller overlap,
     zero race.)
  2. **Per-append cache flush** of `walljobs[i]`/`walljob_n` so the chasing slave reads fresh
     entries (`walljob_n++` is a cached master write; flush a cadence of K appends).
- **Risk: MEDIUM-HIGH** (the allocator race; a mis-fork → visplane corruption / floor gaps).
- **Signal:** **MST down, fps up**; master `B` shrinks toward `max(BSPwalk, slaveBp)`; `w`
  shrinks if well-overlapped, stays high if the slave is now the long pole.

### inc-3 — tighten only if inc-2 leaves measurable `w` (allocator/opening per-CPU partition).

## 5. Red flags & guards (from the adversarial pass)

1. **Occlusion race → wall/floor gaps.** Guard: slave is the **single** in-order consumer
   (`for(i…)` over `walljobs[]`); never hand two segs to two CPUs. Verified by inc-1 byte-id.
2. **Allocator race** (`ds_p`/`lastopening`/`lastvisplane`). Safe in inc-1 (master quiescent).
   inc-2 needs the per-CPU visplane-pool fork (§4 inc-2 #1).
3. **Stack:** no hazard — `R_StoreWallRange`/`R_RenderSegLoop` have no recursion and only
   scalar locals; the 4 KB `rp_plane_slave_stack` has ample margin (`r_parallel.c:824`).
4. **Cache staleness:** two-purge sandwich + uncached `WP_*` aliases; inc-2 adds the per-append
   walljob flush.
5. **GBR-creep** (2nd/3rd slave dispatch/frame): `rp_sgl_workptr_reset()` first in dispatch.
6. **Parity-slave conflict:** force `rp_disabled=1` when `sat_wallprep_slave` on (as the plane
   path does, `r_main.c:1180`).

## 6. Verdict — MEASURED DEAD-END (2026-06-26, HW, E1M1 nukage)

inc-1 was built (behind `sat_wallprep_slave`, pad L+R) and **VALIDATED**: render byte-identical
wp0↔wp1, `to`=0 (no slave timeout). The mechanics (cache/stack/coherency/BSP-order) are correct.

The profiler was extended so `Bp` = wall-prep cost wherever it ran (master inline, or the master's
`RP_WaitWallPrep` spin = the slave flush time) and `Bw` = the pure BSP walk in both modes. HW read:

| | `Bw` | `Bp` (wall-prep) |
|---|---|---|
| **wp0** (master inline) | 6.7 | **17.4** |
| **wp1** (slave flush) | 5.8 | **23.2** |

**The slave is +5.8 ms SLOWER at wall-prep.** inc-2's best case is `win = Bw − (Bp_slave −
Bp_master) = 6.7 − 5.8 = +0.9 ms` — and worse in practice, because in the real pipeline the slave
flushes *while* the master reads the same geometry → extra bus contention. **inc-2 = NO-GO.**

**Root cause (generalizes):** wall-prep is **memory-bound** (cold reads of geometry/textures/
visplanes from LWRAM + the cart-DRAM WAD). The slave starts cache-cold and shares the bus, so it
pays the full `rL=2.1` penalty and **cannot do memory-bound work faster than the master**; the
BSP-walk overlap is exactly cancelled by the slave's cold-cache penalty. This matches the L2-RELOCATE
and plane work-steal negatives: **the 2nd SH-2 only pays for COMPUTE-bound, cache-warm work (the
floor span fill — both halves draw with the flat in cache), never for memory-bound prep/traversal.**

### 6b. Warm-cache (d32xr `Mars_ClearCacheLine`) hypothesis — ALSO REFUTED (2026-06-26, HW)

d32xr keeps its secondary warm by *not* full-purging per phase (it uses targeted line flushes +
a continuous pipeline). We tested whether Mimas's full purge (`master_cache_purge`, the whole-cache
`CCR|=0x10`) was the +5.8 ms: added `wp2` = slave flush **with NO purge** (warm; correct only in a
static scene). HW, immobile, `cc01` (cache enabled, 4-way):

| | `Bw` | `Bp` |
|---|---|---|
| wp0 master inline | 7.4 | 22.2 |
| wp1 slave + purge | 6.2 | 28.3 |
| **wp2 slave + NO purge** | 6.2 | **28.3** |

`Bp(wp2) == Bp(wp1)` → **removing the purge does nothing.** Root cause: the slave is **multiplexed** —
between two wall-prep dispatches it runs the **plane split AND masked** dispatches, each of which
purges *and* refills the cache with span/sprite data, **evicting the wall-prep geometry regardless**.
So the slave is cold at wall-prep even with no wall-prep purge; it cannot be kept warm without a major
restructure (a continuous single-purpose slave renderer, d32xr-style — not worth it on the Saturn's
slower LWRAM/cart-DRAM + master-shares-game-logic).

**Final disposition:** RANK 3 (and the whole slave-reuse avenue) is **dead, confirmed 3×** (inc-1
+5.8 ms; warm-cache refuted). The slave is already optimally used (compute-bound floor fill + masked);
the B phase is memory-bound and unoffloadable. Remove **`sat_wallprep_slave`** from `core/`
(cleanliness for DoomJo) — **but KEEP `sat_plane_steal`: the plane work-steal is the one slave-reuse
win and SHIPPED default-on** (parent `4857f87`). Keep the `cc` (CCR) readout — `cc01` confirms cache
enabled/4-way. The real floor lever is the **dominant-flat VDP2/RBG0 skip** (attacks `P` on a third
HW unit) — shipped as the clean 512×256 8bpp RBG0 bitmap floor; see
[`VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md). The
instrumentation paid off twice: it killed inc-2 *and* the warm-cache idea before either expensive build.
