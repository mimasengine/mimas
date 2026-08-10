# ATLAS — Mimas

**Written 2026-08-09. This file replaces `docs/README.md` as the entry point to the project.**

There are 45 files in `docs/`. Most of them describe code that no longer exists. This one describes
the code that does. Read §0 and §1; read the rest on demand.

---

## 0. What this file is, and the ONE rule

Mimas is not a benchmark. The owner's objective, in his words:

> « L'objectif de Mimas est d'être une plateforme lisant n'importe quel WAD et s'adaptant pour
> rester jouable et appréciable. »

So the acceptance model is **four binary gates per (WAD, map)**:

| Gate | Question |
|------|----------|
| **LOAD** | Does the map load at all? (memory / contiguity walls) |
| **SURVIVE** | Does it run without hitting a cap that `I_Error`-freezes or corrupts? |
| **PLAY** | Is it controllable and responsive? |
| **LOOK** | Is what you see correct and readable? (right textures, no flicker, no holes) |

### THE ONE RULE: frame rate is not an acceptance criterion.

This is not a stylistic preference, it is a measured fact about this codebase. The single worst
degradation in the engine — world sprites falling off VDP1 onto the software masked fill — costs
about **2.9 ms**. On a 100 ms frame that is 3%: invisible on the fps counter, and it ruins the
feel. Three separate mechanisms in this port degrade the image *specifically in order to protect
the frame time*, so a healthy fps number is compatible with a wrecked picture, by design.

Corollaries that follow directly, and that this document is organised around:

1. **A controller you cannot see is a controller you cannot trust.** Several actuators in §2 have
   never once been observed to move, and at least two are one-way ratchets with no reset site.
2. **A guard you cannot count is a bug you will misattribute.** §3's first subsection is the list
   of degradation paths that fire *silently*. That list is the actionable part of this document.
3. **An instrument that reads zero in the failure case is a dead sensor.** Two of them are
   documented in §3 and §8.

### Overlay row map (the decoder for every hardware photo)

| Row | Owner | Content | Gated? |
|-----|-------|---------|--------|
| 0 | `dg_saturn.cxx:1983` | fps / MST / `to<rate>:<A><P><M><W>` | — |
| 1 | `dg_saturn.cxx:2008` | `T` game-tic ms, `b` blit ms, boot/FATAL text | — |
| 2 | `r_parallel.c:1756/2019/2228` | `R = Bw+Bp+P+M` — **THIS IS THE LAST FRAME** | — |
| 3–4 | `dg_saturn.cxx:2092/2101` | profiler detail | — |
| 5 | `r_parallel.c:1713/2117/2286` | `SLV b% id% Pb% w` (slave balance) | — |
| 6–7 | `dg_saturn.cxx:2126/2160` | state line: `M<n>`, `ns`, `ms` | — |
| 8 | `dg_saturn.cxx:2181` | `VD1 w% fbw<n> fbm<n>` | — |
| 9–10 | `dg_saturn.cxx:2378/2381` | `FMp <p50>/<p90>/<p99> mx<n> D<n>%` | — |
| 11 | `dg_saturn.cxx:2393` | `LIM vp ds ss[!] zf<n>k lg<n>k op tc rl` | — |
| 12 | `dg_saturn.cxx:2472` | `CD t px ob gy st` | **CD-streaming only** (`sat_wad_base == nullptr`, :2425) |
| 13 | `dg_saturn.cxx:2261` | `LOS C En Wg P N<orphan>/<drop>/<flip> L<x><m>/<spans>` | — |
| 14–16 | `dg_saturn.cxx:2136/2118` | misc | — |
| 17 | `dg_saturn.cxx:2062` (1p) / `:2024` (split) | `V1 c B LP% ec ws W<res>/<cuts>` **1p only** / `SPL … tc bal` | player-count |
| 18 | `dg_saturn.cxx:2323` | `VRM tx<n>/26 bk q cb lb<b>:<w>/<p>/<s>.<nocol>` | — |
| 19 | `dg_saturn.cxx:2346` | `FLT A<+/-> p r ld ev f` | — |
| 20 | `r_parallel.c:2146` | parallel profiler | — |
| **21, 22** | **free** | ← §6 claims row 21 | — |
| 23 | `dg_saturn.cxx:7756` | `PLAYERS: n` | — |

**Row 11 mixes five different clocks in forty columns.** `vp`/`ds`/`ss` are ~1 s window peaks;
`zf`/`lg` are instantaneous; `op` is **the last view only** (`R_ClearPlanes` zeroes it per view,
`r_plane.c:481`); `tc`/`rl` are cumulative since boot. `op0` on a photo does **not** mean the
openings pool never overflowed this second.

**Row 2 vs rows 0–1 is a category error.** Row 2 is the last frame; rows 0–1 are ~1 s means.
Differencing them proves nothing.

---

## 1. THE FOUR GATES — the per-WAD checklist

Run this standing in front of the CRT with a camera. Record the build mode first, because it
changes which counters exist at all.

### 1.0 Before you start

| Step | Why |
|------|-----|
| Build with `-Repack`, always | A stale `DOOMRP.DRP` fails **silently** and costs ~4 min of boot. The only symptom is "boot is slow". `sat_drp_state` has six failure codes and **nothing prints them** (`w_drp_saturn.cxx:260-311`; the row-21 DRP line was deleted 2026-08-06). |
| Read the build log line `pre-flight: HWRAM TLSF pool = <n> KB` | `build.ps1:327-357`. Below ~4915 B the build throws; below ~7168 B it warns. A boot loop is usually pool starvation, not a code bug. **The pool is per-WAD-build**: `build/Mimas.map` gives 9104 B (`_end 0x060f7c70` / `__heap_end 0x060fa000`), `build/Mimas-Doom1s.map` gives 8416 B. Measure the target you ship. |
| Note cart vs CD-streaming | Row 12 (`px ob gy st`) prints **only** in streaming mode (`dg_saturn.cxx:2425`). On a 4 MB cart build the meters for the wrong-texture fix are invisible. |
| PWADs must be merged offline | `W_AddFile` I_Errors on a second file (`w_wad.c:200-204`, `FEATURE_WAD_MERGE` undef). Use `tools/merge_wad.py`, then `build.ps1 -Wad <out>`. |

### 1.1 LOAD — will the map come up?

**Predict offline before you burn a disc.** The three arithmetic predictors, in order of size:

| # | Predictor | Formula | Threshold | Source |
|---|-----------|---------|-----------|--------|
| 1 | **LINEDEFS** (biggest single block) | lump bytes × 64/14 = ×4.5714 | it is the block that halves the zone | `p_setup.c:411`, `line_t` 64 B `r_defs.h:170-206`, `maplinedef_t` 14 B |
| 2 | **SEGS** (the observed victim) | lump bytes × 32/12 = ×2.6667 | **> ~110 KB ⇒ halt** | `p_setup.c:193`, `seg_t` 32 B `r_defs.h:236-254` |
| 3 | **total PU_LEVEL** (exhaustion) | blockmap + blocklinks + vertexes×2 + sectors×88/26 + sidedefs×20/30 + linedefs×4.571 + ssectors×2 + nodes×52/28 + segs×2.667 + linebuffer | **> ~600 KB ⇒ will not fit at all** | `p_setup.c:1021-1036` |

SEGS is the *victim* because `P_LoadSegs` is the **last** of the eight geometry allocations, so it
asks on the most-consumed zone. LINEDEFS is the *cause* because it is what carves the zone in half.
Full predictor for the contiguity wall = `max(LINEDEFS×4.571, SEGS×2.667)`.

Pre-flight count of maps over the 110 KB segs threshold (script at
`…/scratchpad/loadpred.py`): Doom1s 0/9, Doom2 0/32, Doom-ud 0/36, Plutonia 2/32, HR 3/32,
SCYTHE 3/32, **TNT 9/32**. Exhaustion class (>600 KB total): SCYTHE MAP30 763 KB, MAP29 684 KB.

**On the CRT:**

| Read | Where | Verdict |
|------|-------|---------|
| `LIM zf<n>k lg<n>k` at level start | row 11 | `zf` ≫ size but `lg` < size ⇒ **fragmentation**. `zf` < size ⇒ **exhaustion**. Two different fixes. |
| `FLT p<slots>` | row 19 | 16/12/8/5 = a rung carved. **`p0` = pool-less, and the `r`/`ld`/`ev` counters are then pinned at 0 by construction** (`r_flatcache.c:114-115`) — a dead sensor, not a clean bill of health. Smallest rung needs 5×4096 + 48×1024 = **69632 B contiguous** (`r_flatcache.c:14,32,78`). No re-carve site exists inside a level. |
| the FATAL screen | row 1 + console | `Zmalloc fail <size> t<tag> ra=<addr> (fr<n>K lg<n>K …)` + a top-8 resident-block dump (`z_zone.c:302-351`). `t50` = PU_LEVEL, `t1` = PU_STATIC. Resolve `ra` against `build/Mimas.map`. **Photograph this screen — it is the instrument.** |

**Hard LOAD blockers with no guard** (fail-fast, the WAD never reaches a map):

| Halt | Source | Note |
|------|--------|------|
| `P_SpawnMapThing: Unknown type %i` | `p_mobj.c:835-838` | **THE blocker for modern PWADs.** `FEATURE_DEHACKED` is `#undef` (`doomfeatures.h:28`) so an embedded DEHACKED lump cannot define the type either. |
| sprite frame ≥ 29 / bad rotation set | `r_things.c:236-251` | boot-time |
| missing flat / texture name | `r_data.c:1337`, `:1389` | usual result of merging a PWAD without its resources |
| `Too many scrolling wall linedefs! (64)` | `p_spec.c:1464-1468` | at level load, and **fully predictable offline**: count `special == 48` linedefs |

**Silent LOAD failure worth knowing about:** `R_InitTextures` tries to carve every `texture_t` into
ONE PU_STATIC slab, but only if `Z_LargestAllocatable() > need + 128 KB` at boot
(`r_data.c:1045-1065`). There is **no flag**. If it fails on a texture-heavy PWAD, ~240 KB of small
unpurgeable blocks chop the zone and everything downstream fails for the whole session. The symptom
is a permanently low `lg` (19–38 KB) plus `FLT p0`.

### 1.2 SURVIVE — will it run without corrupting or freezing?

**Watch, do not photograph.** Several of these counters are per-frame or per-view values sampled
once a second; a map that overflows on 1 frame in 60 reads clean in almost every photo.

| Read | Row | Cap | Meaning of a hit |
|------|-----|-----|------------------|
| `vp<n>` | 11 | printed against 256, **binds at 64** | **THE most important misread on the overlay.** `MAXVISPLANES=256` but `VP_POOL_PLANES=64` (`Makefile:167-168`). Past 64 slice-pairs, `R_PoolSlice` hands out the *same* fallback for `top` AND `bottom` (`r_plane.c:97-117`). `vp` between 64 and 256 = silent visual corruption of the excess flats, and the row-11 comment (`dg_saturn.cxx:2388`) still says 96. |
| `ds<n>` | 11 | 256 | `ds256` = at least one seg was **dropped entirely** (`r_segs.c:2128-2130`): not drawn, not clipped, sprites behind it mis-occluded. Saturating counter — one lost seg and two hundred read identically. |
| `ss<n>` + `!` | 11 | 32 | `!` = the solidsegs guard fired (`r_bsp.c:166`). Root cause of a real hardware freeze. **Sticky for the session by design** — after one event every photo shows `!`. |
| `op<n>` | 11 | 20480 shorts | openings sink. **Per-view value**, zeroed in `R_ClearPlanes` (`r_plane.c:481`). Three consumers share ONE sink array (`r_segs.c:2357/2474/2487`) — if one drawseg overflows both `sprtopclip` and `sprbottomclip` they alias and sprites in that x-range vanish. |
| `tc<n>` `rl<n>` | 11 | — | `tc` = **condemned textures** (composite OOM sentinel, `r_data.c:294-330`) — the legend does not say so, and the sentinel is sticky for the level. `rl` = short CD reads zero-filled (`w_wad.c:425-438`); **`rl>0` invalidates every LOOK finding from that session.** |
| `to<rate>:<A><P><M><W>` | 0 | — | slave wait timeouts (`r_parallel.c:739-762`, ~24 ms FRT bound). **Must stay 0.** Digits clamp at 9, so a burst and a leak read identically once saturated. |
| `LP<n>%` | 17 (1p only) | 100 | VDP1 transfer-over. `LP<100` = the plot did not finish the list = flicker. HW-verified 2026-07-26. **Use this, not `D%` on row 10** — see §5. |

**Un-instrumented SURVIVE caps** (nothing on screen; see §3):
`MAXVISSPRITES 128` (`r_things.h:25`, sink at `:449-458`) — no counter at all, no peak;
`WALL_ACC_MAX 128`, `MAXWALLTILES 12`, `MAXVBANDS 4`, `SAT_LEADH_MAX 128`.

**Remaining unguarded vanilla `I_Error`s on the gameplay path** — reachable on a hostile PWAD:
`MAXPLATS 30` (`p_plats.c:288`), `MAXBUTTONS 16` (`p_switch.c:183`),
`MAX_ADJOINING_SECTORS 22` (`p_spec.c:352-362`). All three are cheap to convert to sinks.

### 1.3 PLAY — is it controllable?

This gate has **almost no instrumentation**, and the one defect below means it is currently failing
silently in 3p/4p.

| Read | Verdict |
|------|---------|
| **Nothing on the overlay** shows game-tic-vs-real-time drift | Row 13's legend documents a field `t` ("largest tics advanced in a frame", `dg_saturn.cxx:2229-2232`) that is **not in the snprintf** at `:2248`. Stale legend. |
| Timing by stopwatch | The only way today. Time a known event (e.g. a lift cycle) against the PC. If the Saturn is slower, the game clock is behind — see defect **P1** in §4. |
| Feel: taps that do nothing | Player 1's pad is edge-diffed once per vblank inside `DG_GetKey` (`dg_saturn.cxx:7238-7291`), and there is **no sample between `r_main.c:1188` and `:1234`** — the whole `Bw+Bp` block. A press+release inside that gap cancels exactly and produces no event. Players 2–4 read the pad *level* (`mp_input.cxx:107-122`) and cannot lose a held button. |
| Feel: keys that stick | `keyq_push` silently drops when the 32-entry ring is full (`dg_saturn.cxx:7186-7192`), and `I_GetEvent` drains **at most one key RELEASE per tic** (`i_input.c:286-324`, unconditional `break` at `:322`). A dropped `ev_keyup` leaves `gamekeydown[key]` true forever. |
| 3p/4p with a real multitap | Players 3 and 4 are **hard-wired to pads 0 and 1** (`mp_input.cxx:101-104`, `int src = (p == 2) ? 0 : (p == 3) ? 1 : p;`). No compile guard. They mirror P1 and P2. |

### 1.4 LOOK — is the picture correct?

The hardest gate to read, because the biggest degradation on it is **uncounted**.

| Read | Row | Verdict |
|------|-----|---------|
| `ob<n>` | 12 (CD only) | **Must be 0.** Composite offset out of bounds (`r_data.c:646-651`) — this was the wrong-texture bug. Any non-zero invalidates the gate. |
| `st<n>` | 12 (CD only) | Lead-fill spans whose source had been purged by drain time, drawn flat instead of drawing a neighbour's pixels (`r_segs.c:640-677`). This is **the fix working**. **Correction to the brief: `st` is cumulative SINCE BOOT, not per level** — grep confirms no reset site anywhere, not even `P_SetupLevel`. Two level captures cannot be differenced without the previous total. |
| `px<n>` `gy<n>` | 12 (CD only) | `px` = garde-PATCH (a crash converted into a flat wall). `gy` = a VDP1 flat quad forced to neutral grey — the **visible tip** of the uncounted flat-quad iceberg (§3). |
| `N<orphan>/<drop>/<flip>` | 13 | `orphan` = a wall tier claimed by **neither** the software nor any VDP1 path = you see sky through the wall (`r_segs.c:1348/1411/1412`). Display clamps at 999. |
| `q<n>` | 18 | wtex slot eviction refused → that wall draws as a flat quad for one frame. `q` rising = the 26-slot pool is genuinely too small for the view. |
| `tx<n>/26` | 18 | **three class-partitioned pools, not one** (16×8448 B + 6×16 KB + 4×32 KB, `dg_saturn.cxx:4134`). `tx13/26` can mean the wide pool is full while small slots idle. |
| `lb<b>:<w>/<p>/<s>.<nocol>` | 18 | disc-budget refusals: walls flat / planes potato / sprites skipped. The `.nocol` digit is wall+plane **summed** — you cannot tell a grey wall from a grey floor. |
| `th<emitted>/<declined>` | 15 | declined world sprites fell to the software masked fill. **This is the 2.9 ms degradation.** The three *reasons* (`thd_size`/`thd_slot`/`thd_budget`) are maintained and reset every second and **never printed** (§3). |
| `ec<n>` | 15 / 17 | **`ec` is a CAP at its ceiling (16), not a count.** `ec16 th4/0` means the budget was fine and there were only 4 things. |
| `cd<n>` | — | a **retry** meter, not a read counter. |

---

## 2. THE CONTROLLER REGISTER

Everything in this table changes the picture or the workload at runtime. "Recovery @10 fps" is the
wall-clock time to return to full quality, because **the frame is the clock for most of these and
the frame is 10–25× longer than whoever tuned them assumed.**

### 2.1 The register

| Controller | Measures | Actuates | Gate | Clock | Recovery @10 fps | Observable | Status |
|---|---|---|---|---|---|---|---|
| **`vdp1_budget_cmds`** `dg_saturn.cxx:1598`, law `:5993-6000` | VDP1 LOPR: commands the plot actually completed last frame | the ceiling every other VDP1 allocator spends against | `#if SHOW_FPS` AND `budget>0` AND 4 consecutive `LP==100` frames | **frames** | **26.0 s** from a latched 45 to 248 (65 probes × 4). Partial to 180 (enough for `ec16`) = 8.0 s. Old law was 487 s. **But `vdp1_budget_clean` is zeroed by ANY overrun (`:5992`), so a scene overrunning 1 frame in 4 never climbs at all — the true worst case is unbounded.** | row 17 `B` — **1p only** | live, retuned today, **not HW-validated** |
| **`sat_thing_emit_cap`** (1p) `r_things.c:1662`, law `dg_saturn.cxx:6234-6239` | nothing — pure feed-forward divide of the measured budget | world sprites emitted as VDP1 quads; the rest fall to the software masked fill | `!sat_split_active`; `cap = min(16, (budget − wnext − 16 − wpn_reserve) >> 1)` | frames, +2/frame up, snap down | 0.8 s for its own ramp — **but it inherits its input's 26 s / unbounded** | row 15/17 `ec`; output row 15 `th<e>/<d>` | live, healthy dynamics, imports every pathology |
| **`sat_thing_emit_cap`** (split clamp) `dg_saturn.cxx:5814-5833` | command-bank overflow at drain time | clamps per-view cap to `(entries that fit)/nv` in ONE frame | `sat_split_active`, fires at `wnext >= wall_cap − 16` | frames | 0.8 s, but re-clamps every dense frame ⇒ permanent sawtooth near 0 in 3/4p | row 15 `ec` / `th` | live; deliberately skips the damp (the 2026-07-09 persistent-vanishing regression) |
| **`vdp1_wpn_reserve`** `dg_saturn.cxx:1614`, law `:6004-6020` | whether the plot reached the gun | withholds commands from things AND walls | `vdp1_wpn_slot_disp > 0` | **frames**: +8 per cut, −1 per **48 consecutive** clean frames | **278.4 s** (4 min 38 s) rail-to-floor; 696 s at 4 fps. Rise is 0.8 s ⇒ **asymmetry 348:1**. **VERIFIER: the true worst case is NEVER** — the whole loop is gated on `vdp1_wpn_slot_disp > 0` (`:6004`), which is 0 in M0 / software weapon / side view, yet the reserve is **still subtracted** at `:6234` and `:6268`. Latched at 64, then a mode with no VDP1 gun ⇒ frozen at 64 with no decay path. | row 17 `W<res>/<cuts>` — **1p only**, and the reserve is subtracted from the split room at `:6268` where it is invisible | **live — this is the unfixed sibling of today's 487 s bug.** No gamemap reset exists. |
| **`sat_wall_cpu_span`** + `SOFT_BUDGET_MS` `r_segs.c:395`, law `dg_saturn.cxx:6246-6255` | `rp_master_ms` and `room_for_things` | pushes near walls off VDP1 onto the software renderer — the only *adaptive* actuator that can give VDP1 sprites room back | `budget>0 && room<=4 && rp_master_ms < 50` | frames, ±40 | 0.7 s relax. **Engage has never been observed.** | row 17 `ws` — 1p only | **live, gate is wrong — see §2.3** |
| **`vdp1_wall_cap`** `dg_saturn.cxx:4145`, law `:6134-6157` | player count only | hard per-frame ceiling on wall commands, cut short by the overlays the split emits after them | `nv>1`; 1p keeps `WALL_CMD_CAP=248` | stateless, per frame | 1 frame | none directly; effect on row 17 `c` and row 8 `fbw` | live, deterministic, correct |
| **wall textured-surplus allocator** `dg_saturn.cxx:5090-5131` | `surplus = wall_cap − wnext − wall_acc_n` — **the STATIC cap, never the measured budget** | which walls get textured tiles vs a flat coloured quad, split equally per view | `wall_acc_n>0` | stateless | 1 frame | row 12 `gy` (CD only), row 18 `q` | live — **this is the "textures grises" controller** |
| **`WALL_PX_BUDGET`** `dg_saturn.cxx:4339` | VDP1 fill pixels claimed this frame | rejects the farthest walls to software | always armed; value baked at 200000 (A/B cut 2026-07-07) | per frame | 1 frame | row 8 `fbw` (windowed peak) | live but effectively inert |
| **wtex LRU + 3-state lock** `dg_saturn.cxx:4159-4177, 4413, 4478, 4543, 5064-5069` | slot still being re-plotted from the DISPLAYED list | refuses eviction → that wall is flat for one frame | reached only for a wall that won the surplus race | **flushes** (1→2→0) | 0.2 s. **VERIFIER: the aging loop sits BELOW the `wall_acc_n == 0` early return at `:5059`, so on frames with no VDP1 walls the locks do not age — "2 flushes" can exceed 2 frames.** | row 18 `tx bk q` | live, landed today; correct, and **not** the cause of the texture bug |
| **`sat_tex_load_budget`** `r_segs.c:230-261`, refill `r_bsp.c:102-103` | **real disc milliseconds** (`w_cd_ms10`) | walls flat / planes potato / sprites skipped past the budget | `!= 0` (pad R+X cycles 10/20/40/0, default 20). Inert on cart. | **real time** — the only correctly-clocked budget in the codebase | 1 frame | row 18 `lb…` | live — **this is the model the four frame-clocked controllers above should have copied** |
| **`sat_budget_refused`** `r_segs.c:243/259` | any load-budget refusal, ever | gates eager dominant-colour priming | one funnel | event-latched | **NEVER** — no clear site anywhere | **none** | live; cost bounded by `wallpot_cache` memoisation (`r_data.c:779-784`), so a footnote not a defect |
| **`r_flatcache` rung ladder** `r_flatcache.c:27-93` | `Z_LargestAllocatable` at level load | carves the resident flat slab: {16,96K} {12,80K} {8,64K} {5,48K} | `sat_streaming_mode` | **level load only** | **NONE within a level** — no re-carve site | row 19 `p` | live |
| **`r_flatcache` LRU** `r_flatcache.c:110-175` | per-**view** recency | evicts flats | `fc_slab != NULL && sat_flatcache_on` | views | per view | row 19 `r ld ev f` — **pinned at 0 when `p0`** | **unmeasurable in the failure case** |
| **`r_cache` texcache** `r_cache.c:129-198, 203-280` | `Z_LargestAllocatable`; then 3-**view** aging | bounded composite pool | `if (!sat_streaming_mode \|\| sat_local_players <= 1) return;` — **dead in 1p** | level load / views | level load | **none** — all four counters `(void)`-cast at `dg_saturn.cxx:2470-2471` | parked in 1p by measurement (9.7–21 fps → 0.9–6.8 fps, `r_cache.c:150-167`), live in MP and blind |
| **R4 lazy texture directories** `r_data.c:516, 435-448, 275-302` | zone state | serves a placeholder column instead of `I_Error` | streaming | per call | self-healing, zone-driven | row 12 `px`, row 20 `e` (% of `R_GetColumn` that rebuilt) | live. The pin (`PU_STATIC` during fill) is the trap — dropping it is use-after-free |
| **`SAT_WALL_HYST` + per-seg exit** `r_segs.c:410, 931-951` | per-seg routing history in ONE packed byte | stops walls strobing between renderers | always | **frames** (fixed 2026-08-03; it used to decrement per *visit*) | 0.2 s | row 13 `N…/<flip>` | live. Known aliasing: a seg absent exactly 16/32/… frames aliases back onto the current tag (~1.6 s at 10 fps) |
| **`sat_wall_entry`** `r_segs.c:559` | new visibility | CPU also draws a new VDP1 wall for its first N frames (VDP1 presents a field late) | `!= 0`, default 1 | frames | 0.1–0.3 s | row 13 `En` | live |
| **lead-fill** `r_segs.c:603-677` | last frame's VDP1 quad coverage | software-fills only the rows VDP1 has not covered yet | `sat_wall_lead_x != 0` | frames | stateless | row 13 `L<x><m>/<spans>`, row 12 `st` | live — **today's wrong-texture bug lived here**; fixed by storing (tex,col) + `R_GetColumnCached` (`r_data.c:812+`) |
| **`rp_plane_dead`** `r_parallel.c:1067/1080/1087` | 4 consecutive deferred plane-join timeouts | **permanently** stops dispatching visplanes to the slave | `rp_plane_pending` && join fails ×4 | join failures | **NEVER** — no clear site in the file | **the latch is invisible.** Only row 0 `to` digit 2, cumulative, clamped to 9 | **live — second silent one-way degradation.** VERIFIER: `rp_plane_join_fails` only advances on frames where the master stole every plane before the slave was scheduled (`:1099`), so the latch is *rarer* than feared but correspondingly impossible to attribute after the fact |
| **`rp_wait` FRT timeout** `r_parallel.c:739-762` | **FRT ticks (~224/ms)** — real time | master does the work itself on timeout; every fallback idempotent | 4 sites A/P/M/W | real time | per call | row 0 `to<rate>:<A><P><M><W>` | live, correctly clocked. Hazard: `rp_frt` must mask interrupts across the H/L pair or a vblank composes a value up to 256 ticks low ⇒ instant false timeout (fixed 2026-07-31) |
| **TAS.B plane work-steal** `r_parallel.c:1012-1105` | atomic per-plane claim | load-balances the plane phase, master down / slave up | `!rp_plane_dead` | per claim | per frame | row 5 `Pb%` `w` | live, default on |
| **`sat_mark_suppress`** `dg_saturn.cxx:7263`, `r_plane.c:644` | **player count** | suppresses visplane forking on floor-punched sectors | `sat_local_players >= 3` | config change | immediate | row 7 `ms` | live — the only load-independent auto-adaptation, and the one that behaves. ~4% on a dense 4p Bp scene |
| **`sat_sprite_rotlevel`** `r_things.c:120`, armed `w_drp_saturn.cxx:583` | the map's `.DRP` record | quantizes sprite rotations 8/4/2/1 | `sat_drp_state == 1` | per map load | next map | **NONE** | live — **this IS an adaptation to WAD size and it has zero instrumentation.** Level 1 destroys the "who is it facing" cue: a PLAY regression that looks like a rendering bug |
| **`sat_sprite_rotlod_dist`** `r_things.c:130`, armed to 768 at `dg_saturn.cxx:3536` | per-sprite distance | far sprites serve their FRONT lump | `!= 0 && rlvl > 1 && !player` | none | n/a | **NONE** | live, safe by construction (lump[0] exists at every rot level) |
| **`sat_split_thingcull`** `r_things.c:189-195` | fixed xscale thresholds | drops near-nothing sprites before the vissprite alloc, per view | `sat_split_active` | none | n/a | row 17 SPL `tc` | live; byte-identical in 1p |
| **`R_EmitWorldThingsVDP1` area floors** `r_things.c:1649-1656, 1734` | on-screen area % of the **viewport** | 2% decorations / 0.5% actors floor for VDP1 eligibility | always | per frame | stateless | **NONE** | live. Scale-adaptive: in a 4p quadrant the absolute pixel floor is ~4× smaller, so the same monster flips eligibility on player count alone |
| **`sat_thing_cap`** (distinct-texture grant) `r_things.c:148, 1763-1775` | actor-first ranking, then area | how many distinct sprite textures hold a VDP1 slot | `THING_TEX_TRACK=32` | per frame | 1 frame | aggregate only (row 15 `th`); **the three reason counters are not printed** | live |
| **maketic cap / realtics clamp** `d_loop.c:157-176, 757-759` | **game tics vs real time** | how far the tic builder may run ahead | `if (new_sync)` — **and `new_sync` is forced 0** | tics (35 Hz) | stateless | **NONE** | **INERT — see defect P1, §4.** |
| **VDP1 gated-present watchdog** `dg_saturn.cxx:1550-1555, 6088-6096` | vblanks without a CEF | force-swaps the VDP1 framebuffer | `vdp1_present_manual` (default 0) | **VBLANKS** | 16 vbl = **0.27 s NTSC / 0.32 s PAL**, independent of frame rate | **NONE** | parked — **and it is the counter-example: this is what a correctly-clocked recovery looks like here** |
| **`sat_split_balance`** `dg_saturn.cxx:958-990` | rotation | degrades software quality of 1–2 views per frame, fairly | manual 3-state (pad L+Right) | manual | n/a | row 17 SPL `bal` | parked, HW-pending. The only quality controller that degrades **rotationally** rather than by rank — the right shape for MP |
| **`sat_thing_fill_budget`** `r_things.c:170, 1821-1837` | accumulated sprite AREA | the monster-BLINK fix | ships 0 = uncapped; removed from the emit path 2026-07-25 | per frame | n/a | none | parked — **keep**: if the blink returns with `ec16` and no overflow flag, this is the already-wired mechanism |
| **`sat_near_sprites`** `r_things.c:180/750` | fixed distance | culls far non-shootable decorations | baked ON, chord reclaimed | none | n/a | row 7 `ns` | **a constant printed as if it were a knob** |
| **`sat_wall_dwell`** `r_segs.c:886-904` | flips | pins a flipping seg to the CPU for N frames | ships 0 = off | frames | 0.4–0.8 s | row 13 `En` 2nd digit | parked. Pins to CPU only, never VDP1 — deliberate and right |
| **`thing_emit_floor` / `thing_overrun_run` / `thing_cap_clean`** `dg_saturn.cxx:678-679, 4030-4032` | nothing | nothing | both live branches assign 0 every frame | none | n/a | row 15 `ef` = a hard-wired 0 presented as controller state | **inert dead code — delete on sight** |
| **`rp_disabled`** `r_parallel.c:178, 1716-1718` | — | would disable the parity column renderer | **forced to 1 at boot** (`main.cxx:72` → `r_main.c:1360-1361`) | — | n/a | none | inert. The 6-timeout self-heal people think protects the slave **does not run**; the live protection is `rp_plane_dead` + per-call `rp_wait` |
| **SCSP sfx bump allocator** `i_sound_saturn.cxx:231-265` | sound RAM used | skips the sound; it stays silent | `sram_alloc + len + 2 > SOUND_RAM_SIZE`, or `Z_LargestAllocatable() < len` | monotonic, **no eviction** | **NEVER** within a session | **NONE** in a shipping build (`#if SFX_DIAG` is 0, `:64`) | **unmeasurable, and squarely on LOOK/PLAY**: a big WAD goes progressively silent and nothing says so. `driver_data` is cached *before* both guards (`:235` vs `:242/:261`), so the same sfx can be permanently silent because it was first triggered during a tight moment. Usable pool is **480 KB, not 512** (`sram_alloc` bases at 0x8000, `:511`) |

### 2.2 Structural hazard: the whole flicker controller is inside `#if SHOW_FPS`

`SHOW_FPS` is 1 today (`dg_saturn.cxx:50`), so this is latent, not current. But the block at
`:5957-6043` contains the LOPR read, `vdp1_lp_pct`, the gamemap reset, `vdp1_budget_cmds` **and**
the entire weapon-reserve loop; `:6227-6230` is what applies the budget to `cap_cmds`; `:6241-6256`
is the whole wall LOD; and `rp_master_ms` is assigned inside `fps_update()` (`:6696-6699`).

Setting `SHOW_FPS 0` to ship — the natural move — silently deletes **three controllers at once**
and leaves `vdp1_wpn_reserve` pinned at 6 while still being subtracted at `:6234/:6246/:6268`. A
gameplay-critical controller must not live inside a debug guard.

### 2.3 The pairs — and what they are NOT

The first draft of this register named two "deadlock pairs". **The verifier killed both, and the
correction matters more than the original claim**, because "deadlock" sends the next reader hunting
for a cycle that is not there and implies (falsely) that fixing one end alone cannot help.

**Pair 1 — `vdp1_budget_cmds` ↔ `sat_wall_cpu_span`. NOT a deadlock.**
The claimed arc "low budget ⇒ long frame ⇒ `rp_master_ms ≥ 50` ⇒ relief disabled" **does not exist
in code**. VDP1 re-plots the displayed list every vblank in 1-cycle auto (`dg_saturn.cxx:4167-4168`),
so the plot's ~16.7 ms window is independent of the game frame length: a 25 fps scene can overrun
LOPR and satisfy `master_ok` simultaneously. And the only path from low budget to long frame is
`ec→0` ⇒ software masked fill ⇒ **~2.9 ms**, nowhere near enough to carry a frame across 50 ms.
There is also an escape the deadlock story denies: the clean-frame drift-up (`:5993-6000`) has **no
`master_ok` gate**, and shedding things shortens the VDP1 list, making clean frames *more* likely —
a stabilising arc.

**What is actually wrong with the wall LOD**, stated correctly:

| # | Defect | Evidence |
|---|--------|----------|
| a | The gate keys on **total frame time** as a proxy for software-wall headroom. Wrong signal. | `:6247` `master_ok = (rp_master_ms > 0 && rp_master_ms < 50)` |
| b | The threshold is **above the port's operating range**: `rp_master_ms = 1000/fps` (`:1966`, `:2070`), so 50 ms is literally "faster than 20.0 fps". Mimas ships at 4–13 fps. | `:1621`, `:1966`, `:2070` |
| c | The value is a **~1-second-old windowed average**, refreshed inside the 1 Hz overlay block (`:1907`), read by a gate that runs every frame. | `:1907` / `:2070` vs `:6247` |
| d | `rp_master_ms` is **0 until the first 1 s tick**, and the gate requires `>0` ⇒ the wall LOD is unconditionally disabled for the first second of every run. | `:6247` |
| e | The relax branch is the unconditional `else` (`:6248-6253`), +40/frame ⇒ even a few engaged frames are erased within 7. | `:6250-6252` |

**And the evidence against it is not admissible either.** "Every capture reads `ws480`" cannot
support "it has never engaged": `ws` is printed only inside the 1 Hz block (`:1907`/`:2062`) while
the law runs every frame and a full engage-and-relax excursion is 7–14 frames ≈ 1 s at 10 fps —
exactly aliasable by a 1 Hz sampler. **Do not retire this actuator on the current evidence. Add a
sticky min-`ws` latch across the window first (one line), then judge.**

**Pair 2 — `vdp1_wpn_reserve` ↔ `sat_thing_emit_cap`. NOT a deadlock; it is negative feedback.**
The claimed final arc runs backwards. Emission order is walls → things → weapon → HUD
(`:6196`, `:6296-6298`, `:6301`) and `vdp1_wpn_slot_end` is captured as `vdp1_wnext` **after** the
things (`:6190`). So shedding things *reduces* the commands the plot must clear before the gun,
making the reached-test at `:6006` **more** likely, which decays the reserve. The loop
self-stabilises. **The genuine defects on these lines are the 348:1 decay asymmetry and the
freeze-when-no-VDP1-gun hole at `:6004` — neither is a cycle.**

**The real coupling that does exist** is much simpler and worth stating plainly: three independent
frame-clocked ratchets (`vdp1_budget_cmds`, `vdp1_wpn_reserve`, `rp_plane_dead`) all subtract from
the same VDP1 command pool or the same CPU, all were tuned as if the frame were 16 ms, and **two of
them have no reset site at level load.** A session accumulates degradation. That is the pattern, and
it needs no cycle to be true.

---

## 3. THE GARDE REGISTER

Guards convert crashes into degraded pictures. That is the right trade and it is why big WADs boot
at all. The problem is that **most of them fire without a counter**.

### 3.1 UNCOUNTED PATHS — the actionable list

Ranked by how much of the LOOK gate they hide. Every one of these is a small edit.

| # | Path | What you see | Why it is invisible | Fix |
|---|------|--------------|---------------------|-----|
| **U1** | **VDP1 wall degraded to a FLAT QUAD** (`wall_acc[i].mode = 2`, decision loop `dg_saturn.cxx:5097-5131`, emit `:4936`) | an untextured coloured wall — the owner's "textures grises" | **No counter exists for "walls drawn flat on VDP1 this frame."** Only the worst *sub*-case is counted: row 12 `gy` fires only when the dominant colour also could not be peeked. **A whole level of correctly-coloured-but-untextured walls reads `gy0`.** Three distinct causes (surplus exhausted / resolve refused / potato) merge into one untracked branch. | one counter at `:5130`, split three ways, on row 18 next to `q` |
| **U2** | **`thd_size` / `thd_slot` / `thd_budget`** — why a world sprite was refused by VDP1 (`dg_saturn.cxx:195-197`, incremented `:5659/:5667/:5698`) | monsters "go soft" — the ~2.9 ms fill the fps counter is blind to | Counted and **reset every second at `:2324`**, but the `td<size>/<slot>/<budget>` field documented at `:2295-2302` is **not in the snprintf at `:2312`**. The data is computed and thrown away. | one `%d/%d/%d` in the row-18 format string. `thd_size` in particular decides whether 1p wants **fewer, bigger** thing slots, and it has never been read |
| **U3** | **`r_visplane_pool_ovf`** — `VP_POOL_PLANES 64` overflow (`r_plane.c:76/109/484`) | floors flickering into each other, garbage silhouettes | Externed at `dg_saturn.cxx:169` and **printed nowhere**. Row 11 shows `vp` against **256** while the real cliff is at **64**, so the overlay actively reassures you at `vp=120`. The row-11 comment (`:2388`) still says 96. Worse: `top` and `bottom` both get `vp_fallback + 1` (`r_plane.c:110`), so they alias, and each new overflower's `memset(top,0xff,SCREENWIDTH)` (`:576/:685`) wipes the previous one's spans. | one digit on row 11. **This is the single cheapest fix in this document.** |
| **U4** | **`MAXVISSPRITES 128`** → `overflowsprite` (`r_things.c:449-458`) | the 129th+ sprites in a view all merge into one throwaway record = vanishing monsters | **No counter and no high-water tracking at all** — unlike `vp`/`ds`/`ss`, which row 11 tracks for exactly this reason | add `vs` to row 11; symmetrical with the three already there |
| **U5** | **`R_StoreWallRange` drawseg-full drop** (`r_segs.c:2128-2130`) | a **whole missing wall**, plus sprites behind it mis-occluded (it registers no clip) | Only `ds` on row 11, which **saturates**: one dropped seg and two hundred read identically | `int r_drawseg_drop` next to `r_solidseg_ovf` |
| **U6** | **`R_DrawColumn` OOB skip** (`r_draw.c:127-138`) | missing columns | No counter. This is the **last line of defence** on the wall/sprite fill path and it is mute — a systematic clip bug (2p split produced one once) shows as missing columns with every counter at 0. The slave twin (`R_SlaveDrawSpriteCol`) does not even have the guard; `r_things.c:1438` clamps instead | one `int r_column_oob` |
| **U7** | **`R_MapPlane` / `R_DrawPlanes` OOB skips** (`r_plane.c:335-349`, `:1337`, `:1396-1405`) | a whole floor or ceiling vanishes | The counters exist (`vp_map_bad`, `vp_draw_bad`) but are inside `#if VP_DIAG` and **VP_DIAG is 0** (`r_plane.c:261`). It was compiled out in 2026-06 on the note "zero corruption confirmed across all of hardware level 1" — **that verdict was taken on the shareware IWAD, before the big-WAD endgame work.** Note there are two separate skip sites; the live one is `:1337` (the `SAT_PLANE_LOCAL` worklist) | flip VP_DIAG, or promote one shared counter out of it |
| **U8** | **L5 edge-split bail causes** `sat_fb_edge_w` / `sat_fb_edge_b[4]` (`r_segs.c:363-364`, `:1176-1192`) | the visible wall "écrasement" (clamp+squish fallback) | The `e<got>/<want>` and `b<L><M><T><R>` fields documented at `r_segs.c:360-362` and `dg_saturn.cxx:2165-2176` are **absent from the row-8 format string**, which prints only `VD1 w% fbw fbm`. `r_segs.c:362` states the design rule explicitly — *"a single capture must be able to explain a NULL result … never judge a lever through an instrument that cannot show why it did nothing"* — and then the instrument was removed. **L5 is ON by default** (`sat_opt = 5`, `r_segs.c:490`) | add `e%d/%d` to row 8. The MAGNITUDE bucket decides whether baking narrower sub-textures is worth building, and it has never been read |
| **U9** | **`fb_pk_clamp` / `fb_pk_px`** — walls pushed to software by the span threshold (`dg_saturn.cxx:1848`, `:6035-6040`) | software walls (a perf cliff, not a visual one) | Row 8 prints `fbw` (bank full) and `fbm` (magnitude); the **largest** population — the one `sat_wall_cpu_span` actuates on — is folded into a peak and never printed. **So the actuator (§2.3) and its effect are dark at the same time.** | two fields on row 8 |
| **U10** | **`sat_lead_record`** — lead-fill quad history full (`r_segs.c:744/776`) | sky/black at a moving wall junction — **identical in appearance to "lead-fill is off"** | No counter. Its sibling (span-list overflow) *is* signalled, as a single `!`. 128 is the same magnitude as `WALL_ACC_MAX`, so a wall-dense view hits both caps together — exactly when lead-fill matters most | make both drops numeric |
| **U11** | **`R_GenerateLookup` "column without a patch" early return** (`r_data.c:469-477`) | the inconsistent directory state that produces the `ob` failure | Marked only by `printf`, which is a **no-op on Saturn**. This is the door that opens on *hostile* WADs — the whole point of the project — and it shares nothing with the counted `r_patch_ovf` cause. So `ob>0` tells you the symptom fired and nothing tells you which of two early returns produced it | one increment |
| **U12** | **`sat_drp_state`** — the six `.DRP` failure codes (`w_drp_saturn.cxx:260-311`) | a stale `.DRP` costs ~4 min of boot, silently | The row-21 status line was **deleted 2026-08-06**; all six counters still exist and nothing prints them | §6 puts row 21 to better use; fold `sat_drp_state` into the LOD row's `R` suffix instead |

### 3.2 The counted guards

| Guard | Source | Observable | Clock | Note |
|---|---|---|---|---|
| solidsegs overflow (`MAXSEGS 32`) | `r_bsp.c:166`, `:59-66` | row 11 `ss<peak>` + sticky `!` | peak windowed; **`!` never resets** | Root cause of the M7/lowres level-start **hard freeze**. Sticky-by-design answers "did it ever?" but makes "is it happening now?" unanswerable. A rate alongside the latch (like `to`) gives both |
| openings sink | `r_segs.c:2356/2473/2486`, `r_plane.c:196-204` | row 11 `op` | **per VIEW** (`r_plane.c:481`) | ONE shared array serves three consumers. Both `sprtopclip` and `sprbottomclip` overflowing on one drawseg ⇒ degenerate clip ⇒ sprites vanish. In 3/4p the print discards views 0..n−2 |
| visplane count sink (`MAXVISPLANES 256`) | `r_plane.c:120-149, 558, 670` | row 11 `vp` (**peak**, not sink hits) | per frame | graceful, localised HOM. `r_visplane_ovf` exists and is not printed |
| garde-COMPOSITE | `r_data.c:294-330` | row 11 `tc` | cumulative since boot | **`tc` counts CONDEMNED TEXTURES, not events.** The sentinel is sticky: `R_GetColumn`'s `if (!texturecomposite[tex])` is false for the stub, so **once a texture is stubbed it stays flat for the whole level.** A permanent per-texture degradation dressed as transient — confirm it is cleared at level load |
| garde-PATCH | `r_data.c:348, 446, 610` | row 12 `px` (CD only) | cumulative | **Three call sites share one counter with very different meanings**: `:446` leaves no directory (retried), `:348` leaves a PARTIAL composite (permanently wrong), `:610` is the per-column hot path. `px5` could be five harmless retries or five broken textures. Splitting costs nothing. `px>0` means this build would have HALTED at `Zmalloc fail 35104` before 2026-08-07 |
| garde-COMPOSITE-OOB | `r_data.c:646-651` | row 12 `ob` (CD only) | cumulative | **must stay 0** — this was the wrong-texture read |
| garde-W_ReadLump | `w_wad.c:425-438` | row 11 `rl` | cumulative | Zero-filled data is **cached and reused**. Does not say which lump; a zero-filled MAP lump would be catastrophic and reads identically. Worth capturing the last offending lumpnum |
| `sat_wall_flat_io` / `sat_plane_flat_io` / `sat_spr_flat_io` | `r_segs.c:276-324`, `r_plane.c:1518-1534`, `r_things.c:615/1504/1861` | row 18 `lb<b>:<w>/<p>/<s>.<nocol>` | ~1 s window | The plane side is the fix that took `P` from 149/229/284 ms down — it works. **Split the sprite counter**: site `r_things.c:1505` is the *slave* path and is **unconditional** (no budget involved); it produces the half-sprite artefact (left half drawn, right half missing) the owner reported, and merging it with budget refusals makes that signature unreadable |
| `vdp1_wall_nocol` | `dg_saturn.cxx:4968-4978` | row 12 `gy` (CD only) | cumulative | the visible tip of **U1** |
| `wtex_qrefuse` | `dg_saturn.cxx:4476-4480` | row 18 `q` | ~1 s window | Correctly counted — but the sibling refusals are not: `wtex_find_victim` returning −1 with no state-2 slot falls through without incrementing, and the "too big even for a wide slot" `return -1` at `:4456` happens *before* `wtex_saw_stale` is reset at `:4462`. **`q0` with `tx26/26` means the pool is full for a reason we do not record** |
| `sat_wall_nodraw` | `r_segs.c:1348/1411/1412` | row 13 `N` (1st field) | ~1 s window | One of the best-designed guards here: it answers the LOOK gate's worst symptom directly. Display clamps at 999 |
| `sat_lead_span_drop` | `r_segs.c:639/733` | row 13 — the mode char becomes `!` | ~1 s window | **Counted, then rendered as a boolean that DESTROYS the mode indicator it replaces.** When it fires you can no longer read whether lead-fill is on master, slave or flat — the two facts you most need together. A drop of 1 and of 1500 read identically |
| `sat_lead_stale` | `r_segs.c:640-677` | row 12 `st` (CD only) | **cumulative since boot** | See §1.4. `721-1084` is a boot total, not a level total |

### 3.3 One guard that is actively wrong

**`r_column_stub` is zero-filled, and index 0 is NBG1's reserved TRANSPARENT code.**
`r_data.c:164` defines it zero-init and every comment calls it a "flat placeholder". But
`R_DrawColumn` writes `dc_colormap[0]`, the stock COLORMAP maps index 0 → 0 at every light level,
and `dg_saturn.cxx:1099-1104` deliberately forces palette index 0 to stay transparent in NBG1. So
the placeholder is a **hole**: you see the RBG0 floor / NBG0 sky / VDP1 walls through the wall.

The neutral index the rest of the codebase uses for exactly this purpose is **100**
(`SAT_WALL_FLAT_UNKNOWN`, `dg_saturn.cxx:657`). Note the asymmetry — the *slave* lead-fill path
already does the right thing: `R_GetColumnCached` returns NULL for the stub (`r_data.c:848`) and
`R_LeadSlaveDraw` paints the dominant colour (`r_segs.c:700-704`). **Only the master path is
wrong.** Fix: `memset(r_column_stub, 100, 256)` at init, or better, fill it from
`R_WallPotatoColorPeek`.

---

## 4. OPEN DEFECTS, ranked

Ranked by gate impact × how badly the current instrumentation misleads.

### P1 — **PLAY — the anti-slow-motion tic cap is dead code.** Observable: **NO**

`core/d_loop.c:157-174` carries a long `// SATURN:` comment explaining that the `+8` maketic cap is
what keeps the game at true 35 Hz down to ~4 fps. It is behind `if (new_sync)`.

`new_sync` is initialised `true` at `d_loop.c:98` — and `D_StartNetGame` overwrites it with 0 at
`d_loop.c:470-475`, in the `#else` branch taken because `doomfeatures.h:32` does
`#undef FEATURE_MULTIPLAYER`. `D_CheckNetGame → D_StartNetGame` runs unconditionally at boot
(`d_main.c:2149`). **Verified by reading all three sites.**

So the live cap is vanilla `maketic - gameticdiv >= 5` at `d_loop.c:176` = **4 tics/frame max**.
Below 35/4 = **8.75 fps the game clock falls permanently behind real time.**

| Config | measured fps | effective speed = `min(1, 4·fps/35)` |
|---|---|---|
| 1p typical | 10–13 | 100% |
| 3p | 6.7 | **~77%** |
| 4p | 5.5 | **~63%** |

3p and 4p are running in slow motion **right now**, and the fps counter cannot show it. Two sites
die together: the `+8` cap in `BuildNewTic` and `counts = availabletics` in `TryRunTics`
(`d_loop.c:782`). The fix is one line (force `new_sync = 1` in the `#else` branch) but it changes
`TryRunTics`' counts policy too, so it wants a hardware A/B.

### P2 — **SURVIVE + LOOK — `R_RenderMaskedSegRange` allocates while the slave is drawing.** Observable: indirectly

`sat_masked_inflight` exists to mean "the master must not allocate; a `Z_Malloc` purge would pull the
zone out from under the slave". It is set at `r_things.c:1966`, cleared at `:1988`, and its **only**
reader is the sprite-patch refusal at `:616`.

Inside the same window, the master's own `R_DrawSprite` calls `R_RenderMaskedSegRange`
(`r_things.c:1231`) for every drawseg with `maskedtexturecol` behind the sprite → `R_GetColumn` →
`R_GenerateComposite` / `R_EnsureLookup` / `W_CacheLumpNum` → `Z_Malloc` → purge — while the slave
(`R_SlaveDrawMasked`) is dereferencing zone-owned patch pointers.

That is exactly the failure shape `r_things.c:611-614` already describes as **observed**: *"only the
LEFT half of the sprites drawn, SLV id100% frozen"*. Same defect class as today's wrong-texture bug,
one layer up. Cheapest correct fix mirrors the existing one: refuse the masked seg (leave the column
undrawn for one frame) when `sat_masked_inflight` is set and the source is not already resident.

### P3 — **LOOK — `VP_POOL_PLANES 64` vs `MAXVISPLANES 256`, and the counter is not printed.** Observable: **NO**

See **U3** in §3.1 for the mechanism. Two things to fix and they are independent: **print the
counter** (it is the missing observable, one digit on row 11), and **either raise the cap or give
the fallback two distinct arrays**. The measured margin (`Makefile:151-159`) is `vp ≤ 45` over 14 TNT
MAP11 captures — ~40% headroom **on one map**. Doom II MAP13/15 and open big-WAD vistas are exactly
where `vp` climbs, and splits count toward the 64, so the trigger count is well below 64 distinct
planes.

### P4 — **LOOK — `vdp1_wpn_reserve` is the unfixed sibling of today's 487 s bug.** Observable: 1p only

Same defect class, 20 lines below the one that was fixed (`dg_saturn.cxx:6004-6021` vs
`:5988-6000`), and it got neither the re-tune nor the gamemap reset:

- decay **1 per 48 consecutive clean frames** ⇒ 278.4 s rail-to-floor at 10 fps, 696 s at 4 fps;
  rise is 8 frames. **Asymmetry 348:1.**
- **no reset site anywhere** — exhaustive grep finds only `:1614` (init), `:6010` (−1), `:6018-6019`
  (+8). A bad corridor in E1M3 taxes E1M4.
- **VERIFIER: the true worst case is not 278 s, it is never.** The loop is gated
  `if (vdp1_wpn_slot_disp > 0)` (`:6004`), which is 0 in M0 / software weapon / `viewangleoffset`
  side view (`:6184-6192`). In those states the reserve neither grows nor decays — **and is still
  subtracted** from `room` at `:6234` and `:6268`. Latched at 64 then a mode with no VDP1 gun ⇒ 58
  command-equivalents (~29 sprite slots) withheld forever.

Second defect on the same lines: **`vdp1_wpn_cut` is never zeroed** (`:1615` init, `:6016` ++,
`:2058` print — no reset anywhere), while the legend at `:2045-2046` calls it "cuts since the window
reset" and `:2047` says *"cuts must stay 0 — that is the whole acceptance test"*. After the first
transient in a session the acceptance test is permanently failed and the field carries zero
information. This is the identical failure the owner already fixed for `to` on row 0 (see the note
at `:1963`).

Fix: (a) reset `vdp1_wpn_reserve` + `vdp1_wpn_safe` in the gamemap block at `:5991`; (b) make the
decay gap-proportional like the budget now is, or `WPN_SAFE_DECAY` 48→8; (c) make `cut` a
per-window rate, `W<res>/<rate>:<total>`.

### P5 — **SURVIVE — `rp_plane_dead` is a permanent, invisible loss of the second CPU.** Observable: **NO**

Four consecutive deferred plane-join timeouts (`r_parallel.c:1080`) permanently stop dispatching
visplanes to the slave for the rest of the session. **There is no clear site in the file** — grep
gives exactly three sites: `:1067` def, `:1080` latch, `:1087` consume. The only evidence is row 0's
`to` digit 2, cumulative and clamped to 9.

Verifier refinement: `rp_plane_join_fails` only advances inside `RP_PlaneJoin`, which returns early
unless `rp_plane_pending`, and `rp_plane_pending` is cleared on the `m>=0` path at `:1099`. So "4
consecutive" means 4 consecutive frames *where the master's steal claimed every plane before the
slave was scheduled* — **rarer to reach than feared, and correspondingly impossible to attribute
after the fact.** Fix is one line: clear on level load, or after N successful frames.

### P6 — **LOOK — split-screen never applies the measured VDP1 budget.** Observable: **NO in MP**

The 1p branch narrows `cap_cmds` to `vdp1_budget_cmds` (`dg_saturn.cxx:6226-6230`). The split branch
at `:6257-6272` computes room from `vdp1_wall_cap` only — the **248-slot VRAM ceiling** — and never
reads the measured budget. The LOPR sampler at `:5966-5999` runs regardless of player count, so
`vdp1_budget_cmds` **is valid in MP; it is just ignored.**

This points the wrong way: the slot cap is a VRAM limit, the measured budget is a fill-**time**
limit, and 3/4p is precisely where fill time binds (four views' quads into one 248-slot bank). And
none of it is visible, because the row-17 `V1` line carrying `B`, `LP`, `ec`, `ws`, `W` is gated on
`sat_local_players <= 1` (`:2053`).

### P7 — **PLAY — P1 has a strictly worse input model than P2–P4.** Observable: **NO**

`d_loop.c:206` `if (pl == localplayer) continue;` and `localplayer` is always 0, so P1 never goes
through `sat_build_local_ticcmd` and P2–P4 never go through `gamekeydown[]`. Consequences that hit
**only P1**: taps lost in the `Bw+Bp` dead zone (no `NetUpdate` between `r_main.c:1188` and `:1234`),
one release drained per tic (`i_input.c:322`), and a permanently stuck key if the 32-entry ring
overflows. Consequences that hit **only P2–P4**: no menu, no automap, no weapon cycling, no dclick —
`mp_input.cxx` reimplements a strict subset of `G_BuildTiccmd`.

Cheapest mitigation for the dead zone: a third `NetUpdate` (or a bare `poll_pad`) after
`SAT_RP_BSPDONE`, or make `poll_pad` latch a sticky "was pressed since last drain" bit instead of a
pure edge.

### P8 — **PLAY — players 3 and 4 are hard-wired to pads 0 and 1.** Observable: directly, on hardware

`mp_input.cxx:101-104`, no `#if` around it, unlike `MP_INPUT_PROBE` just above. On real hardware
with a 4-pad multitap `sat_count_local_pads()` reports 4 and four quadrants render, but only two are
controllable. Needs a compile-time or chord switch so the emulator harness is opt-in.

### P9 — **LOOK — `sat_wall_cpu_span`'s gate is wrong (and its evidence is inadmissible).** Observable: aliased

Full treatment in §2.3. Summary: wrong signal, threshold above the operating range, 1-second-stale
value, disabled for the first second of every run, unconditional relax. **Do not retire it on
`ws480` — add a sticky min-`ws` latch first.**

### P10 — **LOOK — `r_column_stub` renders as a transparent hole.** Observable: as an unexplained hole

See §3.3. One `memset`.

### P11 — **Observability — row 12 vanishes on a 4 MB cart build.** Observable: n/a

`dg_saturn.cxx:2425` gates the whole row on `sat_wad_base == nullptr`. `ob` and `st` are the two
counters that prove today's wrong-texture fix is holding, and cart mode is a configuration the owner
actually runs. Only `t` is CD-specific. **Split the row.**

### P12 — **`vdp1_budget_cmds` resets on `gamemap` only, so an EPISODE change does not reset it.** Observable: 1p only

`dg_saturn.cxx:5989-5991` keys on `gamemap != vdp1_budget_map`. In Doom 1 `gamemap` is 1..9 per
episode, so E1M1 → E2M1 leaves `gamemap == 1` and the budget survives the warp. One-line fix: key on
`(gameepisode << 8) | gamemap`. Low severity now that the climb is ~26 s, but the reset exists
precisely so you do not spend those 26 s.

### P13 — **Residual: lead-fill re-resolve is still TOCTOU.** Observable: partially

`R_GetColumnCached` (`r_data.c:830-854`) validates and returns a pointer; the slave pixel loop then
dereferences it for up to `count` rows while the master's `R_DrawPlanes` calls `W_CacheLumpNum` for
flats, which can `Z_Malloc` over the block just validated. The window is now microseconds instead of
a whole phase and the outcome is a partially-wrong column rather than a wholly-wrong texture. **`st`
cannot see this residual** (it counts only spans that resolved to NULL). Watch item, not an action
item — but it is not closed.

### P14 — Documentation defect worth fixing because it is trusted during triage

`r_things.c:139-140` says world sprites on VDP1 are *"NON-occlusion-clipped for now … that is the
FUNC_UserClip follow-up"*. **The follow-up shipped**: `dg_saturn.cxx:5753-5766` emits a
`FUNC_UserClip` box before each thing quad and the hook signature at `r_things.c:141-145` already
carries `cx0/cy0/cx1/cy1`. The residual truth is that the clip is a **rectangle**, so a sprite
half-hidden behind a wall edge still shows the part inside the box — worth saying, but that is not
what the comment says.

---

## 5. DEAD WEIGHT

The TLSF pool is the currency: **dead code costs pool 1:1 with `.bss`**, and a boot loop is usually
pool starvation. Measure `__heap_end − _end` in `build/<CD_NAME>.map` **before and after** every
retirement — and note the pool is **per-WAD-build** (`build/Mimas.map` 9104 B,
`build/Mimas-Doom1s.map` 8416 B; nine `.map` variants exist). Since the objective is *any* WAD, the
tightest build gates the decision.

### 5.1 RETIRE

| Item | Cost | Why safe | Care |
|---|---|---|---|
| **`walljobs[MAXDRAWSEGS]` + deferred wall-prep** `r_segs.c:2066-2114`, `r_parallel.c:1197-1265` | **8192 B `.bss`** (nm: `_walljobs` 0x2000) + ~600 B `.text` | `sat_wallprep_defer` is 0 (`r_segs.c:2075`), forced 0 again (`main.cxx:74`), and its ONLY assignment is `dg_saturn.cxx:7711` inside `#if SAT_DIAG_SLAVE_TOGGLES` with the macro **0** (`:68`). The gate is compile-time, not data-dependent ⇒ **no WAD, split mode, chord or error path can reach it.** Verified against `../DoomJo` too. | **The single largest concrete win** — 8192 B against an 8.4–9.1 KB pool. Conservative version: keep `RP_QueueWall`/`RP_FlushWalls` as a tail call to `R_StoreWallRange`, delete only the array and the slave consumer |
| **`sat_plane_steal`** `dg_saturn.cxx:837, 1940, 1950` | 0 bytes | The symbol **has no definition anywhere** — one extern, two uses inside a dead block, one stale prose mention (`r_plane.c:963`) | **A land mine, not a saving**: flipping `SAT_DIAG_SLAVE_TOGGLES` to 1 — which `:7700-7715` explicitly invites — is an undefined-reference link failure with no obvious cause. Correct `r_plane.c:963` in the same edit: the live symbol is `sat_plane_tas` |
| **`sat_plane_fill_mode`** `r_plane.c:1053, 1089-1096, 1254-1263, 1604-1780` | **2560 B `.bss`** + a 320-iteration per-frame rotate + `swept` tests inside `r_plane.c`'s hottest loop (the `P` term) | Defined 0, **no assignment anywhere** (`dg_saturn.cxx:164` is a bare extern, no `-D` in the Makefile). The named escape hatch is itself dead: `fclaim` needs `sat_vdp1_floor` **and** `sat_floor_vdp1_hook`, and the hook is NULL (`r_plane.c:1098`, `dg_saturn.cxx:884`) | Medium confidence. The mechanism is coherent and complete, which usually means someone intended to wire it. Confirm with the owner, or wire it to a chord and measure once |
| **`sat_vdp1_floor`** `r_plane.c:1046` + 6 sites in `r_segs.c` | small bytes, **six always-false tests inside `R_RenderSegLoop`** — the largest function in the build (nm: 20372 B) and the `Bp` term | Never assigned post-ftex-cut (2026-08-02). **Verified as requested**: `sat_wall_cross_hi` has exactly six call sites (`r_segs.c:1481/1496/1602/1608/1676/1682`) and every one is `sat_vdp1_floor && …` — no other user, so it goes too | clean cut |
| **`rp_exec_col*` / `rp_finish` / `RP_Record*`** `r_parallel.c:214, 411, 500-540, 1786-1830` | **~4.5 KB `.text`** (nm: `rp_exec.part.0` 2308, `rp_finish` 1120, `RP_Record*` 972, `rp_restart` 148) | `rp_disabled` is forced 1 every frame (`main.cxx:72` → `r_main.c:1360-1361`); the `sat_xsplit` escape is unreachable (`SAT_XSPLIT 0`, `r_main.c:1137`, no assignment site) | **VERIFIER CORRECTED THE PROTECT-LIST — read this before cutting.** `rp_masked_slave_body` (332 B) is **LIVE**: `RP_DispatchMasked` is called from `r_things.c:1967` on the normal path. `rp_wait` (132 B) is **LIVE**: called at `:1073`, `:1108`, `:1192`, `:1255`. Conversely `rp_slave_wrapper` (720 B), which the first draft protected, is **DEAD** — its only caller is `rp_restart` (`:863`), whose two call sites are both behind `rp_disabled`. Do it as its own commit with a full E1M1–E1M3 run |
| **`thing_overrun_run` / `thing_cap_clean`** `dg_saturn.cxx:679, 4032` | trivial | Assigned 0 in three places (including once per frame in *each* WBUDGET branch) and **read nowhere** | The value is diagnostic: their presence makes a reader believe a damper exists when the policy is pure feed-forward |
| **`thing_emit_floor` / row 15 `ef`** `:678, 5831, 2277` | one overlay column | Only two statements touch it, both write 0. **`ef0` is a constant presented as controller state** | frees a column on a 40-column row, and it displaced the session bake% |
| **`vd1_win_done` / `vd1_win_tot`** `:1543, 1944, 6032, 2077-2087` | small | `dr` is computed then explicitly `(void)`-discarded with the note *"CEF/vblank-sampling aliasing makes it unreliable"* | Keep the OnVblank handler — it also feeds `mh_vbl_*` |
| **row 10 `D%`** `:1644, 5265, 2371-2383` | one field | **Two independent findings say the number is wrong**: the row-3 note at `:2085` and `[[vdp1-cef-latches-on-hw]]` (EDSR.CEF latches 30–60% on real HW, contradicting the SEGA doc). The same defect drove the `ec0` collapse | Yet the comment at `:2364` still invites the false inference. The trustworthy replacement is already on screen: row 17 `LP%` |
| **`mh_bake_sum` / `mh_emit_sum`** `:1646-1668, 2273` | two accumulators + two adds/frame + a divide/second | computed then `(void)sbpc` | `fb` on row 15 answers the same question |

### 5.2 KEEP — including two the first draft wanted to retire

| Item | Why keep |
|---|---|
| **`sat_potato_walls`** `r_plane.c:1186`, `r_segs.c:1247`, `r_parallel.c:237/427/855` | **VERIFIER OVERTURNED A RETIREMENT, and this was the most dangerous item in the audit.** The claim "no assignment site" is **false**: there are three (`dg_saturn.cxx:891` in `sat_apply_mode`, `:994` and `:1005` in the per-view SQ writers), and it is **reachable by pad R+Y** (`:7557` cycles `sq_wall` 0..3 with `SQ_FLAT == 3`, `:753`; the chord block is live, `VDP2_RBG0_TEST 1` at `:259`). It is published to the slave every frame at `r_parallel.c:855`. Retiring it would delete the **Potato quality tier** — one of the few LOOK-for-PLAY levers the project has for a hostile WAD, i.e. exactly the any-WAD survival knob |
| **`R_PrecacheLevel`'s `R_WallPotatoColor` loop** `r_data.c:1485`, gated `p_setup.c:1067` | Its stated purpose ("so enabling Potato walls in-game doesn't hitch") was called void on the premise above. **The premise was false**, so the purpose stands — in cart mode exactly as originally written. (It does not run in the default CD build: `sat_streaming_mode` is 1, `dg_saturn.cxx:3549`.) Still worth fixing separately: `R_WallPotatoColor` is called **per column** at six loop-invariant sites in `R_RenderSegLoop` (`r_segs.c:1874/1887/1923/1936/1979/1993`) with per-tier constant arguments, while the neighbouring `io_col_mid` is already hoisted (`:1760-1763`). Hoist per tier |
| **`sat_m` modes M0/M4/M5/M6 + the switch machinery** `dg_saturn.cxx:732-860` | Parked because **switching corrupts** (cause still HW-unidentified, 2026-07-19), and `sat_apply_mode` is the documented single writer of every render backend flag. Deleting the other modes collapses that discipline into ad-hoc init and loses the only A/B baselines the project has. **Listed so it is not re-discovered as a finding in three months** |
| **`VDP1_MANUAL_CHANGE`** — 8 `#if 0` blocks, 134 lines | **Costs ZERO bytes** — `#if 0` never reaches the assembler. Retiring it will not move the pool and would burn the one budget item on nothing. Its only cost is comprehension; retire it in a deliberate readability pass, never as a pool measure |
| **`sat_thing_fill_budget`** `r_things.c:170` | Parked-but-complete. If the monster blink returns with `ec16` and no overflow flag, this is the mechanism, already written and wired, needing only a default and a field |
| **`sat_wall_paint`** `r_data.c:763`, pad L+X | The owner's "show me which path owns this wall" diagnostic; earns its keep on every missing-wall report |
| **`r_cache.c`** (MP-only composite cache) | The 1p exclusion is **measured** (`r_cache.c:150-167`: 9.7–21 fps with the flat pool alone vs 0.9–6.8 with the composite cache, 3–4× slower, *"⚠ DO NOT re-lift this"*). But it **does** run in MP streaming with **no** observability. Restore one field: fold `TX<kb>` into row 12 next to `t` |
| **`blit_cfg[]` rows 0/2/3** `dg_saturn.cxx:800-808` | Three of four unreachable and the display chars are already hardcoded (`:2015-2016`), so the live use is one boolean. Async blit via SCU-DMA is documented **impossible** (no CPU bus access during a B-bus transfer). Very low value either way |

### 5.3 RESTORE — instruments that were removed and are load-bearing

| Item | Why |
|---|---|
| **`dg_heap_peak` / `dg_heap_size`** `syscalls.c:55, 82-97`; extern-only at `dg_saturn.cxx:177-178` | **Highest safety-to-cost ratio in this audit.** `syscalls.c:61` says *"⚠ ALWAYS reach for this (or another slack reserve) BEFORE cutting a diagnostic or a feature"* — i.e. the newlib heap is the project's designated **first** pool lever (trimmed 88→32→24→20→18→16→12 KB), and its only instrument was silently removed. Three comments tell the reader to "watch row-22 `hp`"; **row 22 does not exist.** Peak is documented ~6 KB against a 12 KB cap, so there is plausibly ~4 KB more — comparable to `walljobs` — but taking it blind risks `W_AddFile`'s `lumpinfo` calloc failing on a big WAD, which is a **LOAD-gate** failure. Restore `hp<peak>/<cap>k` on row 11 **before** any further trim and before the `walljobs` cut, so the pool change is attributable |
| **`thd_size`/`thd_slot`/`thd_budget`** | See U2. Decide: print or delete. Today you have the worst of both |
| **`sat_fb_edge_*`** | See U8. L5 is on by default, costs ~2.8 KB `.text` (`sat_wall_edge_split` 2232 B + `sat_wall_try_edge` 568 B), and cannot be observed. Add `e%d/%d`, play one level, then either keep it or retire ~2.8 KB and drop `sat_opt`'s ceiling to 4 |
| **row 13 `t`** (max tics advanced) | Directly on the PLAY gate and directly on **P1**. One 8-tic catch-up multiplies any residual one-field display lag by 8 — a plausible reading of the "décrochage" reports |

---

## 6. THE LOD OVERLAY ROW — ready to implement

One row that answers "what has this build given up, and will it come back?" — because today that
answer is spread across a 1p-only row, two rows that only print in CD mode, and four controllers
with no field at all.

### 6.1 Placement: row 21

**Free-row proof** (grepped every writer, not just the obvious three). `SRL::Debug::Print` targets in
`src/dg_saturn.cxx`: 0,1,2,3,4,6,7,8,9,10,11,12,13,14,15,16,17,18,19,23. `src/mp_input.cxx`: 9,10.
`src/i_sound_saturn.cxx`: 6,7 (both `#if SFX_DIAG`, off). `core/r_parallel.c` `dbg_print`:
2,5,15,16,**20**. `core/r_plane.c`: 11,13,14. `core/d_main.c`: 13/14/15 all commented out.
**Union = {0..20, 23}. Rows 21 and 22 are free**; I re-confirmed 21/22 have no `Print` and that row
20 belongs to `r_parallel.c:2146` and row 23 to `dg_saturn.cxx:7756`.

Row 21 is chosen because it is contiguous with the 11–20 block (safely inside the CRT-visible area —
row 23 is already photographed) and because it reclaims exactly the row the `.DRP` exports were made
for (`w_drp_saturn.cxx:167`: *"exported for the dg_saturn row-21 overlay"*).

**Three mandatory companion edits:**

1. The stale comment block at `dg_saturn.cxx:2494-2506` still describes a row-21 `.DRP` status line
   whose two snprintf branches were deleted 2026-08-06. **Rewrite it** — that dangling claim is
   precisely what causes row collisions.
2. The fps-only ghost-blanking loop at `dg_saturn.cxx:2515` is `for (int rr = 1; rr <= 19; ++rr)`
   (**verified**). It must become `rr <= 21`, or LOD ghosts in overlay mode 1. This also fixes row
   20's existing ghost.
3. `core/r_parallel.c:1066-1067` must drop `static` from `rp_plane_join_fails` and `rp_plane_dead`.
   **DoomJo impact: none** — two extra non-static ints in the shared file, no C++ism, no GCC-14-only
   feature, and neither name exists elsewhere in either tree.

### 6.2 Read the suffixes first, values second

One glyph per field, evaluated in priority order `!` > `*` > `v` > `^` > `-` > `=`:

| Glyph | Meaning |
|---|---|
| `=` | at nominal **and the mechanism has been seen to move** — healthy |
| `^` | recovering (moved toward quality since the last print) |
| `v` | degrading (moved away from quality since the last print) |
| `*` | pinned at its **worst** value for the whole window |
| `-` | **has never left nominal since this level loaded — the controller is INERT, not healthy** |
| `!` | a **one-way loss** that will not recover this level/session |

**THE SCAN RULE: any `!` anywhere on this row means the run is permanently degraded — reload the
level.** `ws480` was visible in every capture ever taken and nobody noticed it meant *"this actuator
has never fired"*. `-` says that out loud.

### 6.3 Fields

| Tok | Value | Nominal | Means |
|---|---|---|---|
| `B` | `vdp1_budget_cmds` (`dg_saturn.cxx:1598`), 0..248 | 248 | measured VDP1 command budget from LOPR. `B0-` never measured (allocators treat it as unlimited). `B<n>!` = **unchanged all window while below the ceiling ⇒ the climb is BLOCKED** (any overrun zeroes `vdp1_budget_clean` at `:5992`), so recovery is unbounded, not 26 s |
| `e` | `sat_thing_emit_cap` (`r_things.c:1662`), 0..16 | 16 | `e0*` = every world sprite fell to the software masked fill. Per-view in split |
| `g` | `vdp1_wpn_reserve` (`dg_saturn.cxx:1614`), 6..64 — **lower is better** | 6 | **gun** reserve. Keyed `g`, not `W`, so it can never be misread as the wall-span field on a CRT photo. `g` high + `e` low = the gun reserve is eating the sprites |
| `w` | `sat_wall_cpu_span` (`r_segs.c:395`), 200..480 | 480 | `w480-` = the actuator **has never fired**; see §2.3 for why the gate, not the lever, is at fault |
| `P` | `rp_plane_join_fails`, 0..4 | 0 | `P1v..P3v` = a visible approach to the cliff. `P4!` = `rp_plane_dead` latched, second CPU removed from the plane phase for the **session** |
| `F` | `sat_flatcache_slots` (`r_flatcache.c:39`), 0/5/8/12/16 | 16 | `F<16*` = carved short, no re-carve site inside a level. `F0!` = pool-less **and** row 19's `r`/`ld`/`ev` are dead by construction. `-` = cart build or R+Z bypass off |
| `R` | `sat_sprite_rotlevel` (`r_things.c:120`), 1/2/4/8 | 8 | `R1!` = front lump only, the "who is it facing" cue is gone for every player. `-` = `sat_drp_state != 1` |
| `d` | `sat_tex_load_budget` (`r_segs.c:230`), 0/10/20/40 | 20 | `dv` = it refused something this window. `-` = disarmed or cart build (where `w_cd_ms10` never advances) |

**Format** (38 of ~40 visible columns worst case, hard-bounded by compile-time ranges, so this row
can never truncate):

```c
snprintf(ovbuf, sizeof ovbuf, "LOD B%d%ce%d%cg%d%cw%d%c P%d%c F%d%cR%d%cd%d%c      ",
         vB,sB, vE,sE, vG,sG, vW,sW, vP,sP, vF,sF, vR,sR, vD,sD);
if (sat_dbg_overlay_mode == 0) SRL::Debug::Print(0, 21, ovbuf);   /* 1p AND split */
```

The usual "owner-modified fields leftmost" rule is **waived** here: every range is a compile-time
constant so the row cannot clip, and the order is diagnostic-priority instead — root cause (`B`) →
symptom → per-level configuration, with the owner's one live chord (`d`, R+X) last.

**PLACEMENT TRAP:** the three `*_flat_io` counters that drive `d`'s suffix are zeroed at `:2326`
right after the row-18 print. The LOD block **must** be inserted **before** that block (between the
row-15 print at `:2278` and the row-18 comment at `:2279`) or `d` reads `=` forever.

**Why this row matters more than it looks:** it prints in **1p and split**, unlike row 17 `V1`,
which is 1p-only and has hidden `B`, `ec`, `ws` and `W` in **every multiplayer capture ever taken**.

### 6.4 Columns this frees

| Free | Where | Note |
|---|---|---|
| row 17 `B`, `ec`, `ws`, reserve half of `W` | `:2055` | ~14 columns; keep only `c`, `LP%`, `i`. Enough to fold `V1` into row 8 `VD1` and give row 17 back to `SPL` permanently |
| row 15 `ec` | `:2275-2277` | exact duplicate |
| row 15 `ef` + the whole `thing_emit_floor` mechanism | `:2277`, `:678-679`, `:4030-4032` | delete; frees `.bss` and pool 1:1 |
| row 18 `lb<budget>` leading digit | `:2312` | keep the `:<w>/<p>/<s>.<nocol>` breakdown |
| row 7 `ns` | `:2155-2159` | **delete, not move** — it is a baked constant printed as a live knob |
| row 19 `p` + `A` char | `:2340` | optional, low priority |
| `vdp1_wpn_cut` `/9` | `:2058` | **not redundant, broken** — must stay somewhere, fixed to a per-window rate (see P4) |
| **NOT freed, needs a new home** | row 12 `gy` | the most visible adaptive degradation is stuck behind the CD-only gate (see P11) |

---

## 7. DOC TRIAGE

45 files. Read nine. The rest are lookups, settled negatives, or history.

### 7.1 Read these, in this order

| # | Doc | Status | Errata to apply while reading |
|---|---|---|---|
| 1 | `ENDGAME_ROADMAP.md` | **CURRENT (framing)** — the only doc written in the owner's own four-axis acceptance model; its axes map 1:1 onto the four gates | §1 says TNT/Plutonia are blocked by raw memory — the real wall is **contiguity** (§1.1 here). §3a names `FTEX_PX_BUDGET`/`FTEX_SLOTS`/`MAX_FLOOR_ACC` as live — **all deleted 2026-08-02**. §3b's sight lever **shipped** |
| 2 | `M7_FEATURE_AUDIT.md` | **CURRENT** — the only doc describing the *shipping* render mode, and the newest substantive file (2026-08-02, banner 08-07) | Authoritative record of the ftex/M5 cut (pool 4976→19568 B, **16 KB VDP1 VRAM freed and still unclaimed**). Two bugs still open: SQ_LD gate unreachable, slave forbidden in MP. Its own caveat stands: the post-cut VDP1 present path is **not HW-validated**. **This is the doc that tells you which of the other 43 describe deleted code** |
| 3 | `VDP2_RBG0_CURRENT_STATE.md` | **CURRENT, code-verified** — the shipped hardware floor; the tiebreaker whenever two docs disagree about the floor | Its supersede list (beats VDP2_ARCHITECTURE / VDP2_LAYER_BUDGET / VDP2_CONFIG_CATALOG / RBG0_FLOOR_PLAN / VDP1_ARCHITECTURE §6) is still the correct precedence order |
| 4 | `VDP1_LIMITS_SOURCED.md` | **CURRENT** — provenance-tagged cost model; establishes flicker = **transfer-over**, not fill | It quotes `PROGRAM2.PDF` from the DTS CD, which is the **uncorrected** manual. The Kronos errata in `../saturn-refs/manuals/` land precisely on VDP1 framebuffer erase/switch semantics — **re-check §1.3 and any erase/swap passage against the corrected text** |
| 5 | `FLICKER_HW_TEST.md` | **CURRENT** — the only doc that tells you how to take a *valid* capture | Needs one paragraph: it prescribes `SAT_WALL_CPU_SPAN` as the escalation, and that lever's gate has never been shown to fire (§2.3). Also predates today's budget retune |
| 6 | `VDP2_SECOND_SURFACE_ZONES.md` | **CURRENT** — kills the "second VDP2 surface for the ceiling" idea with a computed threshold | Keep permanently. Also the authoritative record that the RPB/KAst path is **dead, not armed**. The best example in the folder of the standard this project holds itself to |
| 7 | `REMAINING_WORK_AUDIT_2026-07-15.md` | **CURRENT but 3.5 weeks stale** — the "do NOT re-propose, it shipped" list | It is now at risk of the failure it was written to prevent. Since it was written: M7 slave stack, VDP1 lead-fill, per-map load budget, `-Repack` boot fix, the resident flat pool, `R_GetColumnCached` + the wrong-texture fix all landed. **Highest-value single edit in `docs/`: append an 07-15 → 08-09 section** |
| 8 | `TOGGLE_AUDIT.md` | **CURRENT as a lookup table** — skim the banner, then grep; do not read end to end | Carries an explicit 2026-08-02 supersede banner naming every dead symbol. **This is the correct pattern for a doc that ages: amend with a banner, do not silently rot** |
| 9 | `IMAGES.md` | **CURRENT, mildly incomplete** — how to build and launch one disc per witness WAD | `build/wads/` holds three stress PWADs the table does not list (Doom2HR, Doom2NUTS, Doom2SCYTHE). Missing the `-Repack` rule |

### 7.2 Fix first

**`docs/README.md` is the single most misleading file in the folder.** It indexes 29 of 45 docs and
omits **every doc written after 2026-07-02** — including M7_FEATURE_AUDIT, TOGGLE_AUDIT, both
VDP2_SECOND_SURFACE docs, VDP1_LIMITS_SOURCED, FLICKER_HW_TEST, ENDGAME_ROADMAP,
REMAINING_WORK_AUDIT, LOWRES_RENDER_STUDY, SLAVE_OFFLOAD_STUDY, SPRITE_DSP_VDP1_STUDY, IMAGES,
BLIT_DMA_PLAN, and the three RBG0 analyses. Its per-doc status tags predate M7 becoming the shipping
mode and predate the 08-02 cut, so it tags as "PLAN (live unshipped bet)" several things that are
now **deleted code**. A reader who starts there, as the filename invites, is routed to the pre-M7
world. **This is a large part of why the owner is lost.** Replace it with a pointer to this file.

### 7.3 Superseded — quarantine, extract, then trim

| Doc | Superseded by | Extract before trimming |
|---|---|---|
| `CRITICAL_PATH.md` | `[[m7-critical-path]]` + VDP2_SECOND_SURFACE_ZONES | §2 serial/parallel/offloadable taxonomy; §3 slave ledger. **Two claims that actively mislead**: "VDP1 work never caps fps" (true of the *number*, false of the LOOK gate) and "the facing-a-wall cliff is a VDP2 RBG0 transform cost" (**refuted** — it was an M4 per-frame rbg0 upload bug, fixed) |
| `VDP2_SECOND_SURFACE_PLAN.md` | ZONES (self-declared) | §3.1, §3.2, §1.1, §2.2, §1.3 only. Merge into ZONES then delete: two 30–40 KB files where one would do |
| `VDP1_PRESENT_SYNC_PLAN.md` | events | The **verdict** is current (true VDP1↔NBG1 lockstep is impossible at zero fps cost while NBG1 is a live mono-buffer) and the intrinsic-décrochage proof is worth keeping. The strategy menu is not live; brick A is in the tree but off (`dg_saturn.cxx:1550`). Repeats the refuted VDP2-cliff claim |
| `VDP1_ARCHITECTURE.md` | LIMITS_SOURCED on cost | Current on the VRAM ledger, the 8bpp + CRAM-light-bank doctrine, and the MP budget. **Do not take a ms number from it** |
| `VDP2_ARCHITECTURE.md`, `VDP2_LAYER_BUDGET.md` | own banners | Current on the **mechanism** (snow-by-cycle-starvation; the 4-bank × 8-timing law) — that is why the cell floor snowed and the bitmap floor does not. All coexistence conclusions are self-reversed. Consolidation candidate with VDP2_CONFIG_CATALOG: 83 KB across three files for one hardware law. The Kronos-corrected manuals in `../saturn-refs/manuals/` are the better primary source |
| `VDP1_CAPACITY_STUDY.md` | LIMITS_SOURCED + `[[m7-critical-path]]` | §0's two owner corrections survive and are still contradicted by older docs: `Dr%` is present-desync noise not a fill gauge; sprite priority is configuration not a hardware constraint |
| `WALL_SUBDIVISION_STUDY.md` | — | Phases 0–1 **shipped and merged**; Phase 2 points at the deleted VDP1 floor bet. Keep the derivation: walls do not vertically swim because the mapping is linear, so a world-anchored whole-texel cut is exact at both ends |
| `SPRITE_DSP_VDP1_STUDY.md` | events | Its "next increment" shipped and is default-ON; the DSP half is refuted. **Keep its meta-lesson and quote it in the new index**: this doc's first draft said NO-GO and the author called that *"a premature armchair kill — the same mistake made before on the RBG0 floor and the VDP1 walls, both called infeasible, both now shipping"* |
| `RBG0_SKY_SPLIT_ANALYSIS.md` | partly | The main tier **shipped** (`dg_saturn.cxx:6461-6472`). §4 is killed by RBG0_DUAL_PARAM_FINDINGS — **add the back-link**. It also does not know the 2p HW sky was cut in M7 |
| `LOWRES_RENDER_STUDY.md` | corrected by HW | Current as a record. Its performance premise is corrected: **~+8-18% in 2p, ~0% in 3/4p** (3/4p is bounded by 4× BSP/projection/emission, not fill). Durable: why `detailshift` is the wrong tool |

### 7.4 Keep forever (settled negatives — the ideas that look open and are not)

`RBG0_DUAL_PARAM_FINDINGS.md` (101 lines proving VDP2's second rotation parameter needs CELL mode
and therefore can never serve a bitmap floor — attractive, cheap to imagine, definitively dead);
`SLAVE_OFFLOAD_STUDY.md` (kills seven "second renderer on the idle slave" proposals and corrects the
premise: the slave is **not** idle in 1p/2p, and where it is idle in 3/4p that is *because* Mimas
already won); `VDP2_SECOND_SURFACE_ZONES.md`; `RBG0_FLOOR_PLAN.md` (trimmed to 50 lines, retained
for its dominant-flat coverage data 49-93% — **the model for how to retire a doc**).

### 7.5 Archive

`VDP1_WORLD_PLAN.md` — 1167 lines, 78 KB, **26% of the folder**, describing a path that does not
exist in the tree. Keep §3.3.1 (world-anchored anti-swim derivation) and §7/§8 (measured HW
geometry), which fed the wall clamp that **did** ship; archive the other 1000 lines.
`VDP2_CONFIG_CATALOG.md` — 56 KB of enumeration whose surviving value is the measured-HW anchors.
`VDP1_4BPP_STUDY.md` — 311 lines to preserve one decision (**keep 8bpp raw-index + CRAM banks for
walls**).

### 7.6 Promote

**`RBG0_SPLIT_FLOOR_BLACK_BUG.md` — an OPEN LOOK-gate failure with a ~50% hit rate** (P1's hardware
floor comes up fully black about one 2p launch in two), root cause narrowed 2026-07-02, fix never
built, **never re-tested since M7 became the shipping mode**, and **absent from the README index**.
Under the owner's acceptance model this outranks most of the perf docs in the folder. Re-test and
close it, or promote it. Do not let it keep aging silently.

---

## 8. WHAT I COULD NOT DETERMINE

Not padded, not hidden. These are gaps, and an honest gap is worth more than a plausible guess.

| # | Question | State of the evidence |
|---|---|---|
| **1** | **Is the resident flat pool carved on TNT MAP11 or not?** | **Direct contradiction inside the tree.** The brief states `lg39k → p0` (pool-less). `core/r_cache.c:181` states, as the measured justification for adding the 4th 32K+64K texcache rung: *"on TNT MAP11 the ladder took its **8-slot rung**, which means `Z_LargestAllocatable` was 96..129 KB at load"* (I re-read the line). p8 ≠ p0. Either the zone got tighter between 2026-08-06 and the new capture, or one reading is misattributed. **This matters because the whole "the treadmill is unmeasurable exactly where it hurts" argument rests on p0 on that map.** Resolve with one capture of row 19 on TNT MAP11 before acting on it |
| **2** | **Has `sat_wall_cpu_span` ever engaged?** | **Unknowable from existing captures.** `ws` prints once per second (`:1907`/`:2062`); the law runs every frame and a full engage-and-relax excursion is 7–14 frames ≈ 1 s at 10 fps. `ws480` in every photo is equally consistent with "never fired" and "fired and recovered between samples". Needs a sticky min-`ws` latch (one line) before the question can be answered — and therefore before the actuator can be retired |
| **3** | **Is today's `vdp1_budget_cmds` retune correct on hardware?** | **Not validated.** The arithmetic is confirmed exactly (26.0 s from B=45; probe 20 lands on 180; old law 487 s). The HW test: overrun deliberately (walk into a wall-dense room, back out), time `B`'s climb. Expect ~26 s at 10 fps — not instant, not minutes. **One caveat found while re-deriving**: the drift-up counts frames with **no world list** as clean (`:5977-5981` sets `LP=100` when `span <= 0`), so menu/intermission/idle frames probe the budget upward. 26 s is an **in-play upper bound**, not a floor |
| **4** | **Is the post-08-02-cut VDP1 present path correct on hardware?** | `M7_FEATURE_AUDIT.md` says explicitly it is not HW-validated, and nothing since has validated it |
| **5** | **What is the real cause of mode-switch corruption?** | HW-NON-IDENTIFIED. Parity-VDP1 and runaway were both refuted. All modes but M7 are parked because of it |
| **6** | **Does `VP_POOL_PLANES 64` overflow on the witness WADs?** | **Cannot be answered today** — the counter is not printed (U3). The only datum is `vp ≤ 45` over 14 TNT MAP11 captures, on one map, and splits count toward the 64 |
| **7** | **What is the actual `.text`/pool delta of the `rp_exec_*` retirement?** | Estimated ~4.5 KB from `nm`, after removing the two functions the first tally wrongly counted as dead. Not measured end to end, and `_end` moves with section layout — **deleting code can lower the pool.** Measure `__heap_end − _end` before and after, per WAD build |
| **8** | **Does `sat_plane_fill_mode` have a live plan behind it?** | The mechanism is coherent and complete but has never been wired to a toggle, which usually means someone intended to. Retirement is a **medium-confidence** call; ask the owner |
| **9** | **Is `garde-COMPOSITE`'s sentinel cleared at level load?** | Not verified. If it is not, one unlucky allocation early in `P_SetupLevel` condemns a wall texture for the whole map — a permanent per-texture LOOK failure presented as a transient counter |
| **10** | **How often does `MAXVISSPRITES 128` fire on a horde WAD?** | No counter exists anywhere (U4). On a monster-heavy target this fires long before anything else, and produces vanishing monsters with **zero** telemetry |
| **11** | **Does the SCSP allocator actually go silent on a big WAD?** | Mechanism verified in code (no eviction, `driver_data` cached before both guards, `#if SFX_DIAG` off). **Never observed**, because there is no instrument in a shipping build |
| **12** | **Do the L5 edge-split bails ever fire?** | Unknown (U8). The prior expectation in the source is "rarely", which if true makes ~2.8 KB of `.text` retirable — but **that cannot be concluded through an instrument that was removed from the print** |
| **13** | **Is the `sat_masked_inflight` race (P2) currently firing?** | The failure shape is documented as **already observed** at `r_things.c:611-614`, but there is no counter, so nobody can say whether it still happens after this year's fixes |
| **14** | **What is the true PLAY cost of the P1 input dead zone?** | The dead zone's *width* is `Bw + Bp` (readable on row 2) but **no counter tracks a lost tap.** Only the owner's hands can answer this one — and per `[[ask-before-instrumenting-observables]]`, ask him before instrumenting it |

---

### Provenance

Every claim above cites `file:line` from the tree at commit `6e2cb70` (branch `flicker-clean`,
2026-08-09). Where a verifier pass contradicted an earlier claim, **the verifier's finding is what
is written here** and the overturned claim is named as overturned — see §2.3 (two "deadlocks" that
are not), §5.2 (`sat_potato_walls`, `R_PrecacheLevel`), §5.1 (the `rp_exec_*` protect-list inverted),
§4/P4 (the weapon reserve's true worst case), and §8/1 (the TNT MAP11 contradiction).

No hardware behaviour is stated as fact here unless a cited capture, a cited manual, or a cited
in-source measurement supports it. Items marked **unverified** or listed in §8 are exactly that.
