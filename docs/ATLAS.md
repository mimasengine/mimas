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
| 0 | `dg_saturn.cxx:1983` | fps / MST / `to<rate>:<A><P><M><W>` — 🔴 **`a` (the EMA) WAS BROKEN until 2026-08-26 and the break only ever hid GAINS.** It was `avg10 = (avg10*3 + inst10)/4` in tenths of an fps: every `a` in `(inst-4, inst]` is a fixed point of the truncating divide, so a RISE stalled 0.3 fps short **for ever** while a FALL converged exactly. Owner caught it on the wall-fill A/B — `inst` pinned at 8.4, `a` pinned at 8.1, indefinitely. The EMA is now carried at ×4 and rounds. ⚠ **Any A/B photographed before 2026-08-26 read its improvement up to 0.3 fps low**, which at 7-8 fps is ~4 %. ⚠ alpha is UNCHANGED at 1/4 per **second**: “~4 s” is the time constant (63 %), NOT the settling time — an 0.8 fps step still needs **~9 s** to close. Read `inst` for a step, `a` only once it has stopped moving. ⚠ **`MST` is `1000/inst`, so it is FIELD-QUANTIZED, not a work measurement**: NTSC fields are 16.683 ms, so 7 fields = 116.8 ms and 8 = 133.5 ms and nothing lands between them. Work is `R+T+S+b+dg` on row 1; `MST` minus that sum is the **vblank wait**. The 2026-08-26 4p TNT A/B is the worked example: work fell **4 ms** (115.8→111.8) and `MST` fell **12.6** (131.6→119.0) because 115.8 sat 1 ms under the 7-field ceiling and 111.8 sits 5 ms under it — the extra 8.6 ms was idle wait being reclaimed, not work removed. Read a `MST` jump against row 1 before calling it a win of that size. | — |
| 1 | `dg_saturn.cxx:2008` | `R T S b<c><w> dg<pre>/<post> pr rs` — master-frame composition, 1 s window mean. `R` is DERIVED (`MST - T - S - b - dg`); the rest are measured. **`dg` is printed split since 2026-08-25**: `pre` = sky/RBG0 + present-kick + split-HUD + this overlay, `post` = the index-0 view wipe (slave-dispatched in 1p, master `memset` in split). | — |
| 2 | `r_parallel.c` (`rp_p3_prof_show`) | `n<v> Bw Bp P M d<segs>` — `R = Bw+Bp+P+M`, **THIS IS THE LAST FRAME**. **Since 2026-08-25 it is the FRAME, not the last VIEW**: `n<v>` says how many split views were summed (1p reads `n1` and is unchanged), and `d` is that same frame's total drawsegs. Before this, each view overwrote the row, so every 2p/3p/4p phase number ever quoted was one quadrant × N in a spreadsheet (`RESOURCE_BUDGETS.md` §9.2). **`Bp / d` on one photo is the ms-per-drawseg constant** that sizes DECIM, the light-only pass, the cheap-seg rung and the FOV cut — do **not** use row-11 `ds` for it, that one is a per-VIEW high-water. | — 🔴 **`P` IS PRINTED SPLIT SINCE 2026-08-26: `P<total>/<kick>`.** The second half is `sat_p_kick10`, which had been **computed every frame since the `MarkP(0)` probe was written and displayed NOWHERE** — the inputs kept running, only the print was dropped, the same anti-pattern `dd` was found under. **Why it was needed**: row 5 reads `Pm7.2` / `Ps7.1` with `w0.2`, and master and slave run those **concurrently**, so the plane phase should last ~max(7.2, 7.1) + join ≈ **7.5 ms**. Row 2 read **P22.6**. ~15 ms of "P" was neither master plane fill, nor slave plane fill, nor waiting — the largest unnamed block left on the critical path. [r_main.c:1315](../core/r_main.c#L1315) already said what it is in words: *"Everything above this line is charged to `P` today but is not a plane"* — the wall-list flush, the VDP1 kick, and **in split this view's `R_EmitWorldThingsVDP1`** (the wall kick itself is skipped in split — `d_main` fires one for all four views — but the per-view **sprite** emission is not). ⚠ Accumulated on the SAME law as `Bw`/`Bp`/`P`/`M`, so a split photo is a FRAME sum; clamped at 999.9. ⚠ **`P` itself is unchanged** — old captures stay comparable, `P` is still the whole bracket and `kick` is a SUBSET of it, not a sibling. The plane phase proper is `P − kick`. |
| 3–4 | `dg_saturn.cxx` | 3 = `REC 50/95 mx d` — ⚠ **still a per-VIEW distribution in split** (`rend` is folded once per view), so in 4p row 3 describes quadrants while row 2 now describes frames; `GOV_TARGET10 950` was calibrated on the 1p console p50/p95, where the two coincide. Same for row 3's `mx` and the `MX` locator: per-view peaks, deliberately (they size per-phase offloads); 4 = `BPS em<n> pr lp wp`, **all four FRAME sums, printed EVERY FRAME** (2026-08-25). ⚠ It rode the 1 Hz overlay burst until the console pass of that morning caught the consequence: `|wp − Bp|` exceeded 25 % on **7 of 23** 1p frames — once 65,1 against 25,1 — although in 1p they are literally the same bracket. They were not disagreeing, they were describing frames up to a second apart. **A row meant to be differenced against another row must share its cadence**; row 4 now prints from the per-frame block at the end of `DG_DrawFrame`. `em` = the VDP1 command emit loop (`sat_p_emit10`, written since 2026-08-07 and **never printed until now**) — the only instrument on the emission, the term `row2-phase-terms-truth` measured at **44–86 % of `P`**. Read `em` against **row 1 `pr`** in 1p and **row 17 `k`** in split: those are the whole kick, `em` is the loop inside it. *(A `ki` field was added and removed the same day: the owner's Ymir pass showed it equal to row-1 `pr` to the tenth in 1p — 4.2/4.2, 7.6/7.6, 2.4/2.4 — and collapsing to 0.0–0.3 in split, because the single kick fires in the post-views hook and not inside any view's `P` phase. Redundant in one mode, misleading in the other.)* **2026-08-25, 2nd round — `wp` RETIRED, `hd`/`tl` ADDED; the row is now `BPS em<n>hd<n>pr<n>lp<n>tl<n>`.** `wp` was `prof_wallprep`, i.e. **literally row-2 `Bp`** (`r_parallel.c` reads the same variable for both), so it spent 7 of 40 columns restating a number one row above — and those 7 columns are what paid for the two brackets that name the hole. The four are the **partition of `R_StoreWallRange`**: `hd` = the head (scale + texture resolution, silhouette setup, **both** `R_CheckPlane` calls), `pr` = `R_RenderSegLoop`'s per-seg routing preamble, `lp` = its per-column loop, `tl` = the tail (the four `openings` memcpy + the drawseg store). **`hd+pr+lp+tl` MUST equal row-2 `Bp` — verify that identity FIRST on the next console pass**, before reading anything into `hd`/`tl`; the residual is bracket overhead (~0.3–0.6 ms/frame in 4p, each term an upper bound by one FRT read) plus, if it ever fires, the drawseg-overflow early return (gate it with row-11 `ds`; console max 56 of 256, dormant). The hole it names was measured at `wp − pr − lp` = **1p 2.5 / 2p 7.0 / 3p 11.3 / 4p 11.2 ms — 24 % of the 4p `Bp`, with no name on it**. **FIXED WIDTH: exactly 40 cells in every frame** (`%3u.%u`, clamped 999.9), so the longest render equals the shortest and this row can never ghost the previous frame's tail; a clamped field is deliberately indistinguishable from a real 999.9 — a 1000 ms `Bp` term is a hang, not a measurement. ⚠ under the L+R `sat_wallprep_slave` toggle all five read 0 by design (different SH-2 FRT). ⚠ outside overlay mode 0, `hd`/`tl` do not accrue (they share `RP_SegRoutMark`'s gate) while `prof_wallprep` does, so the identity holds only in mode 0 — the only mode that prints it. ⚠ this `pr` is **not** row 1's `pr`. | — | 🔴 **`lp` COST STRUCTURE, read off the SHIPPED r_segs.o disassembly 2026-08-26** — `lp` was 20.4 ms for **1679 columns** of which only **49** reach `colfunc()` (row 14 `c` vs `f`), and `GCS w1/49` proves the other 1630 do not even resolve a texture column: levers C/C2 already cut that. So the cost is NOT dead work, it is the SHAPE of the loop. Three mechanisms, measured on the machine code, not inferred: **(1) `>> HEIGHTBITS` is a FUNCTION CALL** — the SH7604 has no dynamic shift (1/2/8/16 only), so GCC lowers `>>12` to `___ashiftrt_r4_12` (twelve chained `shar` behind a `jsr`), **12 relocations** in r_segs.o, up to **four per column**, and its ABI **pins the operand to r4** inside an already register-starved loop — now inlined as `SAT_SHR12` (r_segs.c). **(2) every per-column accumulator is a FILE GLOBAL** (`rw_x`, `rw_scale`, `topfrac`, `bottomfrac`, `pixhigh`, `pixlow` + steps, r_segs.c:64-89, vanilla Doom intact), so each opaque call forces a reload — visible literally: the same global loaded in a `jsr` delay slot and **reloaded six instructions later**. **(3) register starvation** — 42 `add r15,` stack-address formations in ONE loop body, each needing `mov #N,rX; add r15,rX` first: three instructions to reach one variable. One loop body measured **888 instructions, 336 of them memory accesses (38 %)**. ⚠ **THE FIX HAS NO LIVE TOGGLE** — it is a compile-time refactor, so [[interbuild-perf-noise]] applies in full and the only admissible comparison is `lp` at the SAME spot with `c` and `d` matching as the scene fingerprint. ⚠ **And GCC DECLONED the loop: 7 specialized copies → 1.** Lifting the six software tier-draw blocks out (`sat_tier_draw` / `sat_tier_lead`) cut r_segs.o from 13 687 to **12 711 instructions** and gave back **2.2 KB of pool**, but the surviving generic loop re-tests per column what the clones had folded — **that risk was REAL and the Ymir A/B ANSWERED IT: no.** Same-spot, same fingerprint (`c1679`/`d187` in 4p, `c943`/`d54` in 1p), 7 ON captures against 3 before: **`lp` 20.6 -> 18.74 (-1.86)**, OFF **23.6/22.8/22.4 -> 21.4**, 1p **11.8 -> 11.2** off and **10.9 -> 10.2** on. `hd`/`em`/`Bw` flat. The declone did not cost. ⚠ Round 1 filled all THREE tier descriptors per seg, which put **`pr` +0.66 ms** back (a seg is one-sided XOR two-sided, so half the 96 bytes was unreachable) — cutting `lp` by feeding `pr` is the trade this project refuses, so round 2 fills each descriptor under the same test the loop reads it under. Pool across the whole change: **58.67 -> 63.39 KB**. |
| 5 | `r_parallel.c` (`rp_p3_prof_show`) | **Printed ONCE PER FRAME since 2026-08-25 (2nd round)** — the row was gated on `sat_dbg_overlay_mode == 0` alone while `rp_p3_prof_show` runs once per VIEW, so a 4p frame formatted and blitted it **four times** for one visible line. ⚠ **`mw` CHANGED VALUE with that fix**: `rp_wait_mx` is zeroed at the print site, so the surviving `mw` used to be the max master-wait of the **LAST VIEW ONLY** (views 0..n−2 were zeroed away unread). It is now the max over the whole frame — **larger in split, and correct**; do not read the rise as a slave regression against a pre-08-25 split capture. 1p is byte-identical. `SLV b% Pb% Pm Ps w mw` (slave balance; `id` dropped 2026-08-19, it was 100−b). **`b`/`Pb`/`Ps` are FRAME sums in split since 2026-08-25** — before that `b` divided ONE view's slave ticks by the whole frame's `rp_master_ms`, so the surviving (last) view under-reported by ~N×. ⚠ The console baseline **`b0 %` in every split frame is PRE-fix and was read through that bug** — it is not a measurement of anything, do not compare against it (`RESOURCE_BUDGETS.md` §9.10). | — 🔴 **`Pb%` RETIRED 2026-08-26, `Pv` TAKES ITS CELLS.** `Pb` was `pb_f/p10` = **exactly `Ps` / row-2 `P`** — derivable on sight from two numbers already on the photo (7.1 / 21.7 = 33 %), so it spent 7 of 40 cells restating them. **`Pv` = the VISPLANE WORKLIST BUILD**: `R_DrawPlanes` from entry to the `RP_DrawPlanesSplit` dispatch — ~720 lines of flat resolution, lighting and span setup, **per visplane**, before either CPU writes a pixel (`RP_MarkP(1)`, r_plane.c). **THE SUBTRACTION THAT FORCED IT**: 4p TNT read `P21.7` with a measured kick of **2.2**, while `Pm8.0`/`Ps7.1` run **concurrently** with `w0.0` — so the fill costs `max(8.0, 7.1) = 8.0`, not 15.1. `21.7 − 2.2 − 8.0` left **~11.5 ms** that was neither fill nor wait: **~57 µs per visplane of pure preparation**, at `vp50`/view. ⚠ 🔴 **`Pv` IS PRINTED SPLIT SINCE 2026-08-26: `Pv<build>/<flat>`**, and `w`/`mw` folded into `w<sum>/<max>` to pay for it (they were two fields asking ONE question — how long the master waited on the slave — so sum/max is the coherent spelling; `mw` keeps its meaning, the longest single wait, read against `RP_WAIT_TIMEOUT_FRT` = 100 ms). **The second half is the part that ALLOCATES** (`W_CacheLumpNum` / `R_FlatCacheGet`), so the slave can never run it; **`Pv − flat` is the parallelisable budget** — lighting, planeheight, span setup, pure compute over independent visplanes, the same profile the plane work-steal already exploits. If `flat` dominates, the offload is not worth its mechanism and the answer is no. ⚠ **PROVENANCE, and it is a lesson against myself**: this bracket existed as `RP_FlatCacheEnter/Leave` and I **deleted it earlier the same day**, noting *"two FRT reads per visplane for prof_flatalloc, which nothing has read"*. Deleting the inputs is the right cure for a probe nobody wants; it is the WRONG one for a probe whose **print** was dropped by accident. Owner caught the pattern: the audit criterion is **computed → DISPLAYED**, transitively — "referenced" is not enough, since a dead `extern` counts as a reference. That corrected pass also found 11 unused externs in the platform layer and 2 genuinely dead probes (`r_visplane_pool_peak`, a max-fold per visplane read by nobody). ⚠ **THE PLANE BUDGET NOW CLOSES ON ONE PHOTO**: row-2 `kick` + `Pv` + `max(Pm, Ps)` must land on row-2 `P`; if it does not, one of the four brackets is lying. ⚠ Frame-summed on the same law as the rest of the row — per-view it would have shown the last quadrant only, the bug row 14 and `sat_p_kick10` both had. |
| 6 | `dg_saturn.cxx` — MX (1p) / V1 (split) | **1p** = `MX m<map> <x>,<y> a<ang> t<s>`, the REC-max locator (a per-VIEW peak, deliberately). **SPLIT** = `V1<iso> c B fl<sur>/<slot>/<pot> ec W<res>/<cuts>` — mutually exclusive by construction (MX is gated `sat_local_players <= 1` since 2026-08-25, V1 prints to row 6 only when `> 1`), the same pattern as row 17. ⚠ **the MX locator is therefore UNAVAILABLE in split** — that is the price paid for the first split VDP1 command-bank readout. 🔴 **`B` IS CONSOLE-ONLY AND `B0` IS NOT A BUG** (2026-08-25): `vdp1_budget_cmds == 0` means "not yet measured", and `dg_saturn.cxx:8516` says the AIMD then treats the budget as unlimited (slot cap). It is learned only from an OVERRUN, and the overrun signal is the VDP1's LOPR/CEF latch, **which Ymir does not model** — so `B` reads 0 forever on the emulator (13/13 captures, both WADs, all four player counts) and a non-zero `B` is a hardware-only reading. Never read `c` vs `B` on Ymir. | player-count |
| 7 | `dg_saturn.cxx` | `M<n> <name> SQ:<W><F><C><S> lr/o f<deg> w<rows> K<0/1>` — 🔴 **`K` WAS ADDED, CUT AND RE-ADDED ON 2026-08-26, and the churn is the row working as designed.** Added with the 1p late-kick A/B (pad R+Z), cut hours later by its own result — **16.0 → 19.9 fps, MST 62 → 50**, 1p, same spot, same geometry, so the 1p kick now always runs after the plane dispatch and there is nothing left to select. It came **back the same day on the same chord** for the SPLIT half of the lever, `sat_kick_split`: one kick still, moved from `d_main`'s post-loop site to the **LAST view's plane dispatch**, so the master's plane share of that view is handed to the slave and spent on the kick instead. ⚠ **That is worth ~1.8 ms, not 3.6** — the first write-up of this line said 3.6 and was wrong by 2×: the slave's half of the last view's planes was never on the critical path, so only the MASTER's ~1.8 ms is recovered. ⚠ **1p is byte-identical in both states** — the gate is `sat_split_active`, so `K` is only readable in split. ⚠ **JUDGE IT ON MST AND ON ROW-13 `F`, NOT ON `P`**: the 4p frame sits at ~15 NTSC fields with `F+40%` already spilling, so a 3 ms lever shows as steadiness before it shows as fps — the exact trap that made the 1p A/B read MST50 on both sides. ⚠ **What it does NOT do**: the per-view variant (emit each view's walls at ITS OWN plane dispatch, worth ~5.4 ms, i.e. **three times** the cheap half) is deliberately deferred — it would split the command budget, the things reserve and the wtex priority into four sequential passes, i.e. a refactor of the machinery the 4p flicker session paid for on hardware. **This flag is therefore a mechanism test, not the prize**: it asks whether bringing the split kick forward corrupts anything, whether VDP1 minds starting earlier, and how much of the gain the B-bus tax eats — and a real console reading is what earns the per-view refactor. The owner's z-order objection was answered and he was right — the four views own **disjoint quadrants**, so cross-view command order is unobservable and only WITHIN a view does order matter — but being *possible* is not the same as being *worth it yet*. A knob under test carries a cell, a settled knob carries none. — disjoint. — active A/B state; split-aware (`sq_*_view` when `sat_local_players > 1`). **`f` ADDED 2026-08-25** = the LIVE FIELD OF VIEW in whole degrees, 🔴 **BAKED 2026-08-28 — THE CHORD AND THE 45 RUNG ARE BOTH GONE** (owner: *«supprime 45 et le toggle. On garde ces défauts jusqu'à nouvel ordre»*). `sat_fov_mode[5]` = **1p 90, 2p/3p/4p 65**, and the per-player-count block in `dg_saturn.cxx` is now its ONLY writer — change a default there and rebuild. `f` stays on the row as the **receipt that the applier ran**, not as a knob; it is the one field that proves a photo was taken at the FOV you think it was. **The law is Hor+** — preserve the VERTICAL fov, let the horizontal follow the viewport aspect — and Halo's published figures show Bungie applying it to the degree: CE 70° @4:3 → **108°** @8:3, Halo 2 62° → **44°** @8:9, with the implied vertical fov preserved within 1° in both. Carried onto Mimas (1p 3D area 320×168, 1.2:1 pixels ⇒ aspect 1.59, **V = 64.5°**): a 3/4p quadrant wants **~82°** and the 2p side-by-side view (aspect 0.64) wants **~44°** — so the geometrically correct values **disagree between split modes**, and **2p is where the shipped 90° is most wrong** (its vertical fov is **109°** today against 1p's 64°). 45 existed so 2p could reach its real value and was **cut before it ever shipped a default**: a rung no default can select only costs code. **Shipped: one value for every split mode, 65** — owner's call, *«c'est un peu triché, mais ça ne choquera pas»* — because in versus an fov difference is a **competitive advantage**: it must be identical for every player and stable between rounds, and consistent aiming across 2p/3p/4p beats per-mode purity. **Measured Ymir, same spot, `d(65)/d(90)`: 1p 0.685, 2p 0.692, 4p 0.717** — all inside the predicted 0.70–0.75 band, so the culling genuinely bites. fps 1p 19.7→25.2, 2p 14.0→15.0, 4p 9.4→10.3 (no overlay). ⚠ The knee is at **75** in both split modes; 65 is a deliberate overreach past the correct value, taken for readability on a tiny quadrant — 🔴 **moved off L+Y on 2026-08-26, which was never free**: the ceiling-SQ cycle takes the same L-held/R-released/Y predicate byte for byte, so every press moved the FOV *and* the ceiling quality — fatal for a lever whose validity rests on an identity test at f90. Width: worst case was 34 of 40 cells, ` f%d` takes it to 38. ⚠ **`f90` is the CONTROL ARM, not merely "off"**: `sat_fov_mul` is a RATIO of tangents (`core/r_main.c:47`), exactly `FRACUNIT` at 90°, so `projection`, `yslope` and `basexscale` are bit-identical to a build without the lever — **if anything moves at `f90`, the refactor is wrong and no `f65` reading means a thing.** ⚠ **READ IT AGAINST ROW-2 `d`, A COUNT** — which is precisely what Ymir IS authoritative for, unlike the ms beside it. Same spot, same map, 4p, standing still: predicted **d(65)/d(90) = 0.70–0.75**; a ratio **above 0.85** means the walls are culling, not the angle, and the lever dies before the console is touched. 🔴 **THE HW-SKY CAVEAT IS FIXED, 2026-08-28 — IT WAS A REAL DEFECT, NOT A COSMETIC ONE.** What stood here (*«the HW sky's scroll law is derived from the 90° geometry and is deliberately NOT scaled»*) became shipped behaviour the moment 65 became a default, and it was worse than «mis-tracks»: NBG0's **h-scale** was baked at `4×160/1024 = 0.625`, so at 65° the layer was **38 % too wide and the elected band's horizon panned 38 % faster than its own walls**; the scroll's `+128` («half a 90° FOV is always 128 texture columns») left it **12.6° out** against the software sky in the next view; and the round-3 per-band term that «vanishes» only closes at 90°/`sh=0`, so bands 1/3 were **25° out** at 65°. All three now derive from `sat_fov_half` (`h-scale = 4·view_width/sat_fov_half`, edge `= sat_fov_half>>3`, band term `= sat_fov_half>>2`) and reduce to the old literals exactly at 90°. **By-product: a pre-existing bug on wide skies** — at `sh=1` (1024-wide, i.e. TNT, i.e. every recent capture) the content period is the full 512-px plane, so the band term never closed even at 90° and **any right-column election showed the sky a quarter-turn out**. The software sky needed nothing: `(viewangle + xtoviewangle[x]) >> ANGLETOSKYSHIFT` is FOV-correct by construction and its vertical is `pspriteiscale`, built from `viewwidth` alone. `slSetScreenDist` (the RBG0 focal) is likewise unscaled, which matters in 1p/2p only since RBG0 is off in 3/4p — **the 4p reading is the clean one**. ⚠ It is a GAME change (less peripheral vision in versus) and it now SHIPS in every split mode, judged by the owner and not by a benchmark — the same value for everyone, because an FOV difference between players is a competitive advantage. | — 🔴 **2026-08-26: `ms` and `pm` CUT, `w` ADDED.** This row exists so a photo is never read against the wrong config, and it was spending 8 of its 40 cells on two values that **cannot change**: `pm` (`sat_plane_tas`) has **no writer anywhere in the tree** — the pad-C cycle went when TAS won on 2026-07-16 — and `ms` is **derived from the player count** (`sat_mark_suppress = players >= 3`), i.e. a restatement of row-2 `n<v>`. Both read as live knobs while being constants, which is the exact reason `ns` was cut from this row on 2026-08-09. **`w<rows>` = `sat_wallfill_min`**, the wall-fill offload threshold (pad **R+X**, 0/24/48/96) — 🔴 **PER PLAYER COUNT FOR HALF A DAY ON 2026-08-28, THEN BACK TO ONE GLOBAL RUNG OF 48 — `wh` KILLED ITS OWN TABLE.** The per-mode split was read off a fifteen-capture ladder walk (one rung per scene) and said the candidate bands were *disjoint between modes*: 1p in `[48,96)`, 2p in `[24,48)`, 3/4p bimodal with ~80 % at or above 96. The histogram measures the whole curve in **one frame**, and twenty-eight fresh captures refute all of it: **4p mean `wh` = 2,7 / 5,0 / 1,2 / 0,9**, and bucket d (`≥96`) reads 0 or 1 tenth in **eighteen of the twenty** 4p captures. The old «80 % above 96» came from reading `f` as a histogram when it was the only witness — `f` counts columns SHORTER than the live rung, so at a single rung it cannot separate «nothing above 48» from «everything above 96», which is the exact ambiguity `wh` was built to remove. **And the spread WITHIN 4p — `wh9000` (nothing above 24) to `wh0414` (half the population at or above 48) — is wider than the spread between modes**, so a per-mode constant only freezes one scene's answer and calls it a mode. ⚠ **THE LEVER ITSELF IS NOW ON TRIAL, which is the bigger finding**: mean 4p `lk` at rung 48 is **1,35 — 1350 offloaded pixels per frame** — because VDP1 owns nearly every wall and the CPU fallback is only ~100 columns. Console already priced the rungs (slave busy **4 / 11 / 35 %**, `pr` **29,8 / 30,3 / 32,7** for OFF / 48 / 24), so rung 24 buys 3,5× those pixels for **+2,4 ms of B-bus tax**. There is no case for 24 in any mode. If the console session prices the tax at 48 too, the whole wall-fill offload goes and takes `w`, `f`, `lk`, `wh` and R+X with it. R+X stays meanwhile: the lever is under trial, and a knob under test carries a cell — the one knob actually under A/B, and it was **nowhere on screen**. ⚠ **THE FAILURE THAT FORCED IT**: the four hardware videos of 2026-08-26 had to be identified by INFERENCE — row-14 `lk` for on/off, row-14 `f` for the rung (it counts the columns SHORTER than the threshold: `f49` = above the tallest, `f0` = below the shortest, `f10` = between), row-5 `b%` to confirm (4 / 11 / 35 %). That worked only because all four were the same spot — **those three fields are scene-relative**, so on any other map the inference is gone, and the filenames disagreed with the rungs anyway. A state that has to be deduced from a photo is a state the photo does not carry. Worst case 34 of 40 cells (was 38). 🔴 **2026-08-27 — THE FIFTH VIDEO ARRIVED (`mode2.mp4`, the 96 rung) AND THE INFERENCE HELD, WHICH IS THE ONLY REASON IT COULD BE READ.** `f`/`lk`/`b` placed all five: `f49 lk0 b4%` = OFF (mode3), `f0 lk4 b35%` = 24 (no-mode **and** mode4), `f10 lk3 b11%` = 48 (mode1) **and 96 (mode2), byte for byte** — the two rungs select the *same columns*, because `f` read as a histogram of the ~49 CPU-drawn candidates says ten of them are in [24,48) and **none** in [48,96). A 4p quadrant is 112 rows, so a close wall spans nearly all of it and there is no middle. **The by-product matters more than the rung**: two same-configuration console pairs now exist (48 vs 96, and the two rung-24 videos), and they spread **1.1–1.4 ms on `Bp`** — so the ladder's headline −1.7 ms is barely outside its own noise, and no one-shot console A/B on `Bp` should be believed under ~3 ms. The `pr`-vs-`b%` trend survives because it has four points, not two. |
| 8 | `dg_saturn.cxx:3008` | `VD1 fb<starve>/<mag> MP<n> w<wd> <n>ms g<n> Q<n>/<n> E<n>/<n>` — 🔴 **`<n>ms` = `sat_mp_wait_ms`, THE PRESENTATION-FENCE WAIT, AND IT BECAME A 1 s MEAN ON 2026-08-27 — it was a SINGLE FRAME before, which is how it made me publish a wrong cause.** The fence (`sat_mp_fence`) blocks to a vblank IN edge, so one sample is the frame's arrival PHASE inside the field: near-uniform over 0–16.7 ms. Printed on a 1 Hz row it reads like a measurement. I quoted `16ms` on a late-kick K0 capture against `3ms` on a K1 one and named the fence as the mechanism; the owner's next three K1 captures, same build, same spot, read **3, 18 and 4**. The conclusion held — but only because it is re-derivable from **MST and `R`, which are windowed** (`MST − R` is the blit term and the fence lives inside it: 28 → 19.75 ms). `g<n>` = `sat_mp_gate_ms`, the plot-done half of the same fence, and it reads **g0 on every capture taken so far** — on Ymir by construction (no plot model), on hardware because the kick precedes the fence by ~10 ms. A non-zero `g` is the manual-present overrun signal, `w<wd>` counts the force-swaps. | — |
| 9–10 | `dg_saturn.cxx:2378/2381` | `FMp <p50>/<p90>/<p99> mx<n> D<n>%` | — |
| 11 | `dg_saturn.cxx` | `LIM vp<peak>.<poolovf-PLANES> ds ss[!] o<rows> zf<n> lg<n>` — **exactly 40 cells** (pad, then `ovbuf[40]` cut). ⚠ the old legend was stale twice over: `op tc rl` moved to **row 22** on 2026-08-10, and the cited line number was ~800 lines out. **`o` ADDED 2026-08-25, and REDEFINED the same day: it is the high-water of DEMAND, not of CONSUMPTION.** 🔴 The consumption form **saturated by construction** — all three sinks (`r_segs.c` masked-column, `sprtopclip`, `sprbottomclip`) redirect into `opening_overflow` **without advancing `lastopening`**, so the instant the array is too small the counter pins at `MAXOPENINGS` and can never say *by how much*. Cutting the array on a consumption reading would therefore have destroyed the one instrument able to size the cut. `r_opening_demand` now accumulates what every caller **asked for**, sink or not, so the reading stays meaningful at any array size and the cut is order-independent forever. It is the ~1 s high-water in 320-word rows, against the **16** in `#define MAXOPENINGS SCREENWIDTH*16` (`r_plane.c`) — **cut from 64 on 2026-08-26, the first time that number was ever set by measurement**; vanilla's 64 was a guess whose entire comment was `// ?`, and at 40 960 B of .bss it was larger than the whole TLSF pool it was starving. Row-22 `op` is the overflow-redirect COUNT, the alarm that only rings once it is already too late; `o` is the number that **sizes** the array. Paid for without losing a field: the two literal `k` suffixes went (the legend carries the unit), `zf`/`lg` are **clamped at 999** (the zone is 1016 KB, so a 4-digit reading at level load is exactly what used to push this row to 41 cells and drop the last one silently), and the pool-overflow digit is now printed as **PLANES** (already halved, clamped 99) — **do not halve it again**. ⚠ `o` is a high-water, NOT a ceiling: the theoretical worst is 3 writes × width × `MAXDRAWSEGS` = 245 760 words, 12× the array, so `MAXOPENINGS` stays a probability bet backstopped by the garde. **FIRST READINGS, 2026-08-25 (Ymir, a COUNT so the emulator is authoritative): `o1` or `o2` on 13/13 captures — shareware E1M1 AND TNT MAP01, 1p through 4p, action included, with row-22 `op0` throughout.** `o1` means ≤320 of the 20 480 words were consumed of the then-20 480, i.e. ≤1.6 %. **CUT 2026-08-26 to 16 rows** (5 120 words, 10 240 B). Measured effect on the linker map: `__heap_start` 0x060f2e60 → 0x060eb800, so the **TLSF pool goes 29 088 → 59 392 B — it more than doubles** (+30 304; the 416 B short of the arithmetic is section alignment). 16 rows and not the 8 the arithmetic allowed: `o` is a high-water, **not a ceiling** — the theoretical worst is 3 writes × width × `MAXDRAWSEGS` = 245 760 words, 12× even the OLD array — so the extra 5 KB of margin is cheap beside a 30 KB win, and the **garde remains the real safety net, not the size**. ⚠ The pre-cut check the previous revision of this legend demanded IS DONE: row-4 `tl` reads **0.7 ms** on the same TNT 4p frame, so the four `openings` memcpy are running and `o1` is genuine headroom — not a Mimas wall mode silently skipping the silhouette store. ⚠ Watch row-22 `op`: non-zero on a real map means RAISE `MAXOPENINGS`, never remove the garde. ⚠ The `vp` field two columns to its left **was overflowing on the same frames** (`vp66.2`/`vp67.8` on TNT 4p against `VP_POOL_PLANES=64`) — a 40 KB array sitting idle beside a pool that was running out. **Closed 2026-08-26**: sizing the three visplane slice arrays to `viewwidth` instead of `SCREENWIDTH` reads **`vp67.0`** on the same scene — same 67-plane peak, zero redirects, `VP_POOL_PLANES` untouched. | — |
| 12 | `dg_saturn.cxx` | **TWO TENANTS, mutually exclusive by ONE predicate** 🔴 **WHICH CHANGED ON 2026-08-28: it is now `sat_wad_base == nullptr && sat_local_players <= 1`, so SPLIT ALWAYS GETS `SKY`, disc or cart.** The old build-only predicate made `SKY` — the row whose own definition says it exists to price the multi-view HW-sky plan — **invisible on the only disc the console session ever runs**, a `-Repack` CD build. A probe written to size a plan, on a row that plan's own test can never reach, is not a probe. Player count is the honest split: `SKY`'s four slots are forced to 0 below two players, so it carries nothing in 1p, while the `CD` gardes are a 1p-streaming story. ⚠ **The cost, stated**: in co-op on a CD disc we lose `t`/`px`/`ob`/`gy`/`st` — including `st`, the lead-fill stale counter that must tend to 0. If a split texture bug reappears, drop to 1p to read it. **CD-streaming, 1p**: `CD t<s> px ob gy st`. **SPLIT (any build) or CART (≥4 MB)**: `SKY e<v> <ms0> <ms1> <ms2> <ms3> p<px>` — added 2026-08-25 to size the 3-quadrant HW-sky plan. The four tenths-ms are the **per-view SOFTWARE sky fill**; `e` is the view carrying the HW NBG0 sky, **whose ms reads 0.0 BY CONSTRUCTION** (the `sat_vdp2_sky` branch draws nothing — that IS the HW/software distinction, and it is how a photo identifies the elected view without trusting `e`). A pixel count could NOT have answered this: `R_DrawSkyColumn` does a 128-byte per-column memcpy plus the grain loop, so the px→ms factor swings 2–4× with scene geometry. `p` = total sky px over the live views — the COUNT Ymir is valid for. **All zero in 1p** (the arrays are written only inside `d_main.c`'s split loop and are never reset, hence the mandatory stale-guard). LAST FRAME, like row 2 — never difference it against rows 0–1. ⚠ `e` is the election for the NEXT frame, so it disagrees only on a transition frame. ⚠ this SPENDS the cart half that §P11 and §6.4 both wanted for `ob`/`st`/`gy`. | **cart vs CD-streaming** |
| 13 | `dg_saturn.cxx:2261` | `LOS C En<dwell> P N<orphan>/<drop>/<flip> L<x><m>/<spans> F<min>+<pct>%` — 🔴 **`F` ADDED 2026-08-26 — THE FIELD-SPILL RATE, and the only field that can show a lever worth 1–3 ms.** Histogram of `sat_field_n` over the 1 s window: `<min>` = fields the typical frame occupies, `<pct>` = the share of frames that needed MORE. `F3+0%` = every frame fits comfortably; **`F3+9%` = the frame is SITTING ON THE LINE** and nine per cent of it falls off, which is judder you can see. **Why it had to exist**: the late-kick A/B (row-7 `K`) read **MST50 on BOTH sides** — 50.05 ms is exactly three NTSC fields, so the mean rounds to the floor either way — while the owner could plainly see K0 wandering 19.4–19.7 fps and K1 pinned at 19.7. The arithmetic behind what he saw: spill = (1000/fps − 50.05) / 16.683, so 19.4 fps = **9.0 %** and 19.7 = **4.3 %** — five points of frames, about one extra judder a second, and **not one field of the overlay could show it**. `sat_field_n` had been computed in `sat_field_fence` since the Fl era, with its own comment saying *"row-13 readback … MUST be STEADY: a value flipping N/N+1 means the frame sits on a boundary"* — and was never printed. Same defect class as `ns`/`ms`/`pm`/`dd`. 🔴 **AND THE FIRST VERSION OF THIS PROBE HAD THE SAME DISEASE**: I bumped its histogram inside `sat_field_fence`, which has been dead code since the manual present v2 subsumed it — the note at `sat_field_lock` says so three screens up — so it read a confident **`F0+0%`** on every capture. It is now sampled at the `sat_mp_fence()` CALL SITE, which runs every frame. A probe attached to a function nobody calls does not fail loudly; it reports zero and is believed. ⚠ **On a field-quantised frame MST is the wrong judge and `F` is the right one**: below the next field line the saved ms become margin, not fps, and margin is exactly what `<pct>` measures. 🔴 **2026-08-27 — THE WINDOW NOW FOLDS ON A SAMPLE COUNT, NOT ON THE CLOCK, AND THE FIRST VERSION'S NUMBERS SHOULD NOT BE QUOTED.** Owner, on the first 4p reading: *“j'ai F6+30% mais le pourcentage varie entre 20 et 60”*. That is the denominator, not the machine: a **one-second** window holds one frame per frame — ~9 in 4p Ymir, **~4 on a 250 ms console frame** — and the standard deviation of a proportion is √(p(1−p)/n), so at p=0.3, n=9 it is **±15 points**. A 20–60 swing *is* what noise looks like there. A rate probe must not have a denominator that collapses with the frame rate, so it folds at **≥24 samples** (`SAT_FN_MIN_N`) instead — same precision at 20 fps and at 4 fps, at the cost of a window that takes ~1.2 s in 1p, ~3 s in 4p Ymir, ~6 s on console. Residual **±9 points**: read a 10-point move as the EDGE of significance, not as a result. ⚠ **Consequence for the split late-kick A/B**: its `F` reading of 40 % → 30 % was taken at n≈9 and is **not on its own significant** — four K1 captures at the same spot read 30/50/40/20. Nor is row-8 `<n>ms` (16 → 3), which was a single frame and read 3/18/4 on the other three. **What carries that result is `MST` and `R`, the two fields that were windowed all along**: MST 116 → 105.5 and R 88 → 85.75 over five captures, so `MST − R` — the blit term, which contains the fence — fell 28 → 19.75. Both probes were windowed the same hour; **no number from a pre-fix build is quotable.** — 🔴 **2026-08-26 settled-toggle sweep**: `En` lost its FIRST digit (`sat_wall_entry` baked at its documented default 1) and `Wg` (`sat_wall_grow` baked 2) and `F<sky><rpt>` (`sky_mode` 1, `rbg0_rpt_late` 2) went entirely — **eight cells back on a 40-cell row**. `En` is now the DWELL alone (pad R+Up, 0/4/8). ⚠ The entry digit had been silently pinned at **0** in every capture where anyone pressed L+Left: its predicate was byte-identical to the lump-pin cycle's and it only ever DECREMENTED, floor 0 — and 0 is the pre-2026-08-02 behaviour where a wall entering the view shows sky for one frame. | — |
| 14 | `dg_saturn.cxx` + `d_main.c` | `SEGn<v> dd<st>/<hu>/<ot> wh<a><b><c><d> pl<pct> c f lk` — 🔴 **`k` CUT 2026-08-28 BECAUSE `lk` WAS SILENTLY TRUNCATED.** This row pads to 40 and cuts; at 41 cells the tail already fell off, so **every 4p capture of the last three days ends in a bare `lk` with no number** — the offload meter, unreadable, in the very mode the offload exists for. A truncated field reads as a formatting quirk, not as a lost measurement. `k` pays for it: master-filled wall pixels in thousands, and it has read **`k0` in every capture ever taken**, 1p through 4p — the master's own fill is under a thousand pixels a frame, so the field cannot move and cannot fall. Its question is answered from the other side by `lk` rising, which now has room to say so. — 🔴 **`ov` RETIRED 2026-08-28 AND `wh` TOOK ITS SEVEN CELLS.** `ov` is retired **by its own answer**: it asked what `rp_p3_prof_show` costs and said **1.1–1.2 ms of a 114 ms 4p frame**, so the 788 lines stay exactly as they are. A closed question does not keep a cell. `dd` was the other candidate and it **survives deliberately** — the console residual subtraction (`R − =` − `k`) is still open and `dd` is what attributes its remainder. **`wh<a><b><c><d>` = THE CANDIDATE-HEIGHT HISTOGRAM**, tenths of the CPU-drawn candidate count in the buckets `<24` / `[24,48)` / `[48,96)` / `≥96` — the wall-fill ladder's own rungs, so the four digits read straight off as *what each rung would select*. It exists because deriving the same curve by walking the ladder cost **fifteen captures** and still could not be trusted: each rung is sampled in a DIFFERENT scene while the tester moves, which is [[interbuild-perf-noise]] transposed into time. This gives the whole curve **in one frame, at any rung**. Three compares per candidate column, no division in the hot path. ⚠ **It needs an armed rung**: `R_WallFillArm` returns early at rung 0, so the record path is never reached and `wh` prints dots — park on any non-zero rung to read it. Dots also mean *no candidates this frame*, which is honest but ambiguous with rung 0; check row 7 `w`. ⚠ Frame sum, zeroed at the print, same law as `c`/`f`/`k`/`lk`. — 🔴 **`ov` and `pl` ADDED 2026-08-26, both cost-attribution fields, both placed EARLY because this row pads to 40 and then cuts and `lk`'s tail is the designated sacrifice.** **`ov`** = what `rp_p3_prof_show` costs, frame-summed tenths-ms: 788 lines, ~36 divisions, run **once per VIEW** from `RP_EndFrame` (four times a frame in 4p), and only TWO of its blocks are gated on `sat_dbg_overlay_mode == 0` — the histograms, the peak folds, the per-view accumulators and the governor's input run in the fps-only and overlay-off builds too. Nobody had ever measured it. Bracketed from OUTSIDE the call so the probe cannot hide inside the thing it measures — the exact mistake `RP_PlanePixels` was making inside `Pv` until this same day. Published on the frame's LAST view, so it reads the PREVIOUS frame's total (same law as row-19 `v`). ⚠ **A blanket early return is NOT available**: the LOD governor takes its `p10` from inside this function and is not mode-gated, so if `ov` is big the fix is per-block gating. **`pl`** = the percent of row-4 `em` spent in the PLOT loop — i.e. **the share of the VDP1 kick a second SH-2 could take.** `vdp1_walls_flush` already has the shape an offload needs: a DECISION loop (3-way mode, surplus budget, `wall_tex_resolve`) that ALLOCATES and can bake a texture off the disc, so it is master-only forever exactly like `Pv`'s flat-resolve half; then a PLOT loop (the three `wall_emit_*`) that is a pure transform of an already-decided `wall_acc[]` into VDP1 command records — no zone, no lump, no allocation. A PERCENT on purpose: 4 cells where tenths-ms costs 7, and `em` is one row away, so **`em` × `pl` is the millisecond answer**. ⚠ **`pl` is not the win.** The plot loop writes VDP1 VRAM over the **B-bus**, which is shared: the wall-fill ladder priced that exact effect on hardware the same day — row-4 `pr` rose **29.8 → 30.3 → 32.7** as slave occupancy went 4 % → 11 % → 35 %. The win is `em`'s fall MINUS what `hd`/`pr`/`lp` rise, so judge it on row-2 **`Bp`**, never on `em` alone. ⚠ Do not confuse this with the **settled-negative** wall offload (`[[wall-offload-vdp1-slave-dead]]`, HW 2026-07-07): that one REJECTED walls from VDP1 so the CPU drew their pixels, and it lost because the budget rejects the farthest walls — the cheap ones. Here every wall still plots on VDP1; only the CPU that BUILDS the command list moves. — 🔴 **`dd` ADDED 2026-08-26**: 🔴 **`dd` ADDED 2026-08-26**: `D_Display` OUTSIDE the per-view timers, per frame in **TENTHS of a ms**. `st` = the switch + `ST_Drawer` + `I_UpdateNoBlit` preamble, `hu` = `HU_Drawer`, `ot` = `M_Drawer`/`NetUpdate`/border/pause/misc. **It names the last unmeasured term of the frame**: `R` − (row-17 `=`, the sum of the per-view times) − (row-17 `k`, the VDP1 kick) left **~9 ms** unexplained in 4p, and the board's guess — that the split residual lived inside `R_RenderBSPNode` — was wrong; row-1 `rs0.3` had already ruled out the per-view clear. ⚠ The other two sub-phases are deliberately NOT here: `rp` (the views) IS row-17 `=` and `bl` (the blit) IS row-1 `b`, so accumulating them was duplicate work and their sums were deleted. ⚠ **`dd` sits second, right after the label, on purpose**: this row pads to 40 and then cuts, and the worst-case widths downstream can push past 40 — the field being read today is placed where the cut cannot reach it, and `lk`'s tail is what would go instead ([[debug-overlay-line-width]]). `c`/`f` dropped from a 5-digit clamp to 4 to pay for it: they read ~2 000 and 0, and 99 999 was never reachable for a per-frame column count. ⚠ **Refreshed once per second**, not per frame — it is a windowed mean, so it will not track a single spike. 🔴 **PROVENANCE, and the reason it existed unread for months**: the arithmetic behind `dd` has been running since the `DISPLAY_DEBUG` block was written. Its `dbg_print` was commented out ("[overlay lean] ... off for wall/floor work") while `DISPLAY_DEBUG` stayed **1** — so the display went and the computation did not, including a full `sprintf` every second into a string nobody could read. Two sibling blocks in the same function were worse still (dead end to end) and were deleted. **The rule this violated: silencing a probe means deleting its inputs, not commenting its print.** | the per-column loop's census: `c` = column iterations of `R_RenderSegLoop`, `f` = `colfunc()` calls from inside it, `k`/`lk` = pixels and the lead-fill's share, in THOUSANDS. **FRAME sums since 2026-08-25**, `n<v>` states how many views (row-2 law). Before this, `prof_seg_*` were zeroed in `RP_BeginFrame` = once per VIEW while the row printed once per FRAME, so **in split it showed the LAST QUADRANT ONLY**: the 08-25 console CSV reads `c` median 433 in 1p (range 160–937) against a flat **257 in 2p and 256 in 4p** — a per-view constant that cannot be a frame census. ⚠ **the denominator for µs-per-wall-column is row-4 `lp`, NEVER row-2 `Bp`**: every one of these counters increments strictly between `RP_SegRoutMark` and `RP_SegLoopLeave`, which is exactly the `lp` bracket, whereas `Bp` is all of `R_StoreWallRange` and now carries `hd`/`tl` beside it. ⚠ `lk` is **not** a subset of `k`: `SAT_LEAD_EMIT` counts on both the master and the slave-list path, `SAT_PROF_FILL` only on the master one, so `lk > k` is legal under `L1s`. 🔴 **`lk` GAINED A SECOND MEANING 2026-08-26 and it is now the OFFLOAD METER**: with the wall-fill toggle on (pad **R+X**, `sat_wallfill_min` = off/24/48/96 rows), every wall column at least that tall is **recorded for the second SH-2 instead of being filled by the master**, and its pixels land in `lk` while `f`/`k` fall by the same amount — that fall IS the win, not a regression. The slave drains the queue while the master keeps appending to it, joining before `R_DrawPlanes`:  `Bp` (48.6 ms in 4p) is the window where the slave is idle, whereas the plane phase is already shared almost evenly (row 5 `Pm8.5`/`Ps7.9`), so it keeps its half of the planes. 🔴 **The dispatch is LAZY since 2026-08-26** — it fires on the FIRST recorded span, inside the walk, not when the producer is armed. Arming eagerly held the slave in its back-off spin for the whole wall phase even when the threshold rejected every column, and **that spin is not free**: the 1p A/B read `lp` **12.2** with `lk0` and `b47%` against `lp` **11.8** toggle-off — ~0.4 ms of master time for zero pixels, bus contention from a slave polling an empty queue. A view that queues nothing now never wakes it, and `R_WallFillDone` skips both the join and the cache purge `RP_LeadJoin` carries. ⚠ **`st` is on the `CD` row, NOT row 12** (`CD t.. px.. ob.. gy.. st..`) — the in-code pointer said row 12 and was wrong; owner confirmed `st0`. Read it — spans whose texture had gone by drain time, drawn flat for one frame; it MUST tend to 0. And watch `sat_lead_span_drop`: the queue caps at 1536 records of 24 B, which is why the height threshold exists at all (an unfiltered producer wants ~1920 spans in 4p). The threshold is the **break-even**, not a compromise: a record costs 24 bytes, drawing the column costs `count`. | — |
| 15 | `dg_saturn.cxx` | `SPR fl<ms> n<proj>/<drawn> th<emit>/<decl> td<size>/<slot>/<budget> fb o` — sprite/thing census. 🔴 **READ THE LABEL, NOT THE GLYPH: `fl` is the FIELD NAME, and its `l` renders as a `1` in the 8-px font.** `SPR f10.0` on screen is `fl` = **0.0 ms**, not 10 ms. The 2026-08-25 "10 ms of sprite fill beside `n36/0`" alarm — which reached `docs/DOSSIER_MATERIEL_2026-08-25.md` as a quantified −10 ms things→VDP1 lever, and this legend as "one of the two numbers is lying" — was that misreading and nothing else. Re-read across 13 captures / 2 WADs / 4 modes, `fl` is **0.0–0.3 ms**: there is no software sprite fill left to reclaim, because the things already ride VDP1. The probe (`RP_SprStats`, `prof_spr_fill * 10 / 224`) is sound. ⚠ **The trap generalises**: every field whose NAME ends in `l`, `i` or `t` abuts its own digits in this font. When a value looks an order of magnitude wrong, re-derive it from the `snprintf` format string before believing it. ⚠ Still **not audited** for the per-view/per-frame defect rows 14/16 had fixed; at 0.0–0.3 ms that changes no conclusion, which is why it can sit unaudited. | — |
| 16 | `dg_saturn.cxx` | `GCS w<ms>/<n> m<ms>/<n> P<rung><kb>/<yields>.<evictions>` — who calls `R_GetColumn`: `w` = the seg loop's own tier resolution, `m` = `R_RenderMaskedSegRange`. **FRAME sums since 2026-08-25**, same defect and same fix as row 14 (`prof_gc_st/sn` reset per view in `RP_BeginFrame`). ⚠ **the tenths were dropped from `w`/`m` to pay for the frame sum's extra digit**: at 3-digit ms the old `w%u.%u` form reached 41 content chars against the `ovbuf[40]` cut and would have eaten the last evictions digit with no visible sign. No field was lost. The `P` half is per-LEVEL, not per-view, and is unaffected. | — |
| 17 | `dg_saturn.cxx:2062` (1p) / `:2024` (split) | `V1 c B fl<sur>/<slot> LP% ec ws W<res>/<cuts>` **1p only** / `SPL v0..v3 k =S tc bal h p<c><ms>` — ⚠ **this row already overruns the 40 visible cells in 4p (~42 chars): `p<c><ms>` falls off the right edge in every 4p capture ever taken** (same defect class as the row-11 overrun of 2026-08-10; flagged 08-25, not yet fixed). **The 4th slot is `v3` in 4p and the 3p MINIMAP in 3p** (2026-08-25) — they are mutually exclusive by construction (`d_main.c`: `sat_spl_v3 = (n>3) ? … : 0`; `sat_spl_mmap` is written only under `if (n == 3)`), so the probe costs zero columns. The minimap is the only cost centre unique to 3p and it had never been timed — 3p runs MST 131–192 against 2p's 100–172 and nobody could say how much of the gap is a third view. `=S` includes it. `h` (2026-08-25) = the split-HUD paint, tenths-ms; `p` = WHICH term dominates row 1's `dg` `pre` (`y` sky block / `s` sky TEXTURE upload — should fire once per level, so `s` winning means its guard is not holding / `u` rbg0_upload_flat / `x` set_transform / `r` RPT memcpy / `o` present-kick + HUD capture + overlay), and by how many ms | player-count |
| 18 | `dg_saturn.cxx:2323` | `VRM tx<n>/26 bk q cb lb<b>:<w>/<p>/<s>.<nocol>` | — |
| 19 | `dg_saturn.cxx:2346` | `FLT v s<punch>/<texcol> @<why>.<area> r ld f F<claims>/<refuse>/<cmds>` — **`A<+/->` CUT 2026-08-26**: `sat_flatcache_on` is baked ON. The slab is CARVED in both states, so "off" only ever meant paying for the memory and refusing to read it; the flat treadmill it answered was closed 2026-08-06. — **all the `v`/`s`/`@`/`F` fields read 0: the VDP1 floor claim is PARKED** (`SAT_VDP1_FLOORS 0`). `ld`/`f` are the flat cache and stay live. | — |
| 20 | `r_parallel.c:2146` | parallel profiler | — |
| 17 | `dg_saturn.cxx` (split only) | `SPL <v0> <v1> <v2> <v3> k<kick> =<sum> tb<bal> h<ms> p<c><n>` — per-view render ms, the VDP1 kick, their sum, then the two live split knobs and the split-HUD paint. 🔴 **`tc<n> bal<n>` became `tb<tc><bal>` on 2026-08-25** because the row **overran 40 cells in 4p and lost `po`'s value off the right edge** — visible in the 08-25 captures as a bare `po` with no digits. Then **`tb<tc><bal>` became `tb<bal>` on 2026-08-26**: `sat_split_thingcull` is baked ON, exactly as its own definition always argued — *"impractical to A/B on HW (needs a monster-dense room = can't stand still to read the overlay)"*. A knob nobody can measure should not hold a pad chord. One knob left here (pad L+Right = SQ-balance rotation). ⚠ In **3p** the 4th slot is `sat_spl_v3 + sat_spl_mmap`, i.e. a view **plus** a cost centre, in that mode only. | player-count |
| 21 | `dg_saturn.cxx` | `GOV d w p e sb L i ws` — the LOD governor; `ws` lives here since 2026-08-24 and is now its SOLE home (retired from `V1` on 08-25 as a duplicate). ⚠ **console-confirmed asleep in every split mode**: `w0 p0 sb0` on 31/31 4p and 23/23 3p frames, because `GOV_TARGET10 950` targets `rend`, which is only 47 % of the 4p frame. | — |
| 22 | `dg_saturn.cxx` | `GRD op tc rl zw np Lo pf` — the guard latches evicted from row 11 on 2026-08-10. ⚠ own worst case ~54 chars, **14 cells off the right edge** (unfixed). `op` is the openings overflow-redirect COUNT and is still **the last view only** (`R_ClearPlanes` zeroes it per view) — `op0` beside a hot row-11 `o` is not a contradiction: `o` is the max over every view of the window, `op` is one view of one frame. | — |
| 23 | `dg_saturn.cxx:3319` (`PLAYERS: n` at `:7756` on other screens) | `THK n mo ph sm mv pt w dc` — the inside of `th`. **Every ms field is estimated from WHOLE SAMPLED TICS** (1 tic in 4; `mo`/`ph`/`sm`/`mv`/`sc` and the `th` they are subtracted from all come from the same tics, lifted by one factor). `pt` = the probe's OWN measured cost; `w` = the bare thinker-list walk. **Cross-check on any photo: `mo + pt + w` must land near row 24's `th`, which is exact.** ⚠ Round 5 sampled per CALL and mixed an estimate with an exact `th` — its `w` was noise, not a finding. `sb`/`bt` RETIRED 2026-08-25 (they split an `mv` that the level-structs merge took from 66-75 ms down to 1-3). | — |
| 24 | `dg_saturn.cxx:3372` | `TIC th s x v sp% sc ca` — `th` P_RunThinkers ms, `s` sight, `x`/`v` tics-run / vblanks per frame, `sp` game speed %, `sc` sector thinkers, `ca` sight-cache window | — |

**Row 11 mixes five different clocks in forty columns.** `vp`/`ds`/`ss` are ~1 s window peaks;
`zf`/`lg` are instantaneous; `op` is **the last view only** (`R_ClearPlanes` zeroes it per view,
`r_plane.c:481`); `tc`/`rl` are cumulative since boot. `op0` on a photo does **not** mean the
openings pool never overflowed this second.

**Row 2 vs rows 0–1 is a category error.** Row 2 is the last frame; rows 0–1 are ~1 s means.
Differencing them proves nothing. *(What row 2 is no longer guilty of, since 2026-08-25: being one
VIEW while row 1 was the whole frame. `n<v>` on the row states the sum's width — so `n4` beside
`MST` is finally a legal subtraction, over one frame's worth of each.)*

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
| **`vdp1_budget_cmds`** `dg_saturn.cxx:1598`, law `:5993-6000` | VDP1 LOPR: commands the plot actually completed last frame | the ceiling every other VDP1 allocator spends against | `#if SHOW_FPS` AND `budget>0` AND 4 consecutive `LP==100` frames | **frames** | **26.0 s** from a latched 45 to 248 (65 probes × 4). Partial to 180 (enough for `ec16`) = 8.0 s. Old law was 487 s. **But `vdp1_budget_clean` is zeroed by ANY overrun (`:5992`), so a scene overrunning 1 frame in 4 never climbs at all — the true worst case is unbounded.** | row 17 `B` in 1p, **row 6 `B` in split** since 2026-08-25 | live, retuned today, **not HW-validated — and unobservable on Ymir**, which models neither LOPR nor the CEF latch, so `B` reads 0 there forever |
| **`sat_thing_emit_cap`** (1p) `r_things.c:1662`, law `dg_saturn.cxx:6234-6239` | nothing — pure feed-forward divide of the measured budget | world sprites emitted as VDP1 quads; the rest fall to the software masked fill | `!sat_split_active`; `cap = min(16, (budget − wnext − 16 − wpn_reserve) >> 1)` | frames, +2/frame up, snap down | 0.8 s for its own ramp — **but it inherits its input's 26 s / unbounded** | row 15/17 `ec`; output row 15 `th<e>/<d>` | live, healthy dynamics, imports every pathology |
| **`sat_thing_emit_cap`** (split clamp) `dg_saturn.cxx:5814-5833` | command-bank overflow at drain time | clamps per-view cap to `(entries that fit)/nv` in ONE frame | `sat_split_active`, fires at `wnext >= wall_cap − 16` | frames | 0.8 s, but re-clamps every dense frame ⇒ permanent sawtooth near 0 in 3/4p | row 15 `ec` / `th` | live; deliberately skips the damp (the 2026-07-09 persistent-vanishing regression) |
| **`vdp1_wpn_reserve`** `dg_saturn.cxx:1614`, law `:6004-6020` | whether the plot reached the gun | withholds commands from things AND walls | `vdp1_wpn_slot_disp > 0` | **frames**: +8 per cut, −1 per **48 consecutive** clean frames | **278.4 s** (4 min 38 s) rail-to-floor; 696 s at 4 fps. Rise is 0.8 s ⇒ **asymmetry 348:1**. **VERIFIER: the true worst case is NEVER** — the whole loop is gated on `vdp1_wpn_slot_disp > 0` (`:6004`), which is 0 in M0 / software weapon / side view, yet the reserve is **still subtracted** at `:6234` and `:6268`. Latched at 64, then a mode with no VDP1 gun ⇒ frozen at 64 with no decay path. | row 17 `W<res>/<cuts>` — **1p only**, and the reserve is subtracted from the split room at `:6268` where it is invisible | **live — this is the unfixed sibling of today's 487 s bug.** No gamemap reset exists. |
| **`sat_wall_cpu_span`** + `SOFT_BUDGET_MS` `r_segs.c:395`, law `dg_saturn.cxx:6246-6255` | `rp_master_ms` and `room_for_things` | pushes near walls off VDP1 onto the software renderer — the only *adaptive* actuator that can give VDP1 sprites room back | `budget>0 && room<=4 && rp_master_ms < 50` | frames, ±40 | 0.7 s relax. **Engage has never been observed.** | row 17 `ws` — 1p only | **live, gate is wrong — see §2.3** |
| **`vdp1_wall_cap`** `dg_saturn.cxx:4145`, law `:6134-6157` | player count only | hard per-frame ceiling on wall commands, cut short by the overlays the split emits after them | `nv>1`; 1p keeps `WALL_CMD_CAP=248` | stateless, per frame | 1 frame | none directly; effect on row 17 `c` and row 8 `fbw` | live, deterministic, correct |
| **wall textured-surplus allocator** `dg_saturn.cxx:5090-5131` | `surplus = wall_cap − wnext − wall_acc_n` — **the STATIC cap, never the measured budget** | which walls get textured tiles vs a flat coloured quad, split equally per view | `wall_acc_n>0` | stateless | 1 frame | row 12 `gy` (CD only), row 18 `q` | live — **this is the "textures grises" controller** |
| **`WALL_PX_BUDGET`** `dg_saturn.cxx:4339` | VDP1 fill pixels claimed this frame | rejects the farthest walls to software | always armed; value baked at 200000 (A/B cut 2026-07-07) | per frame | 1 frame | row 8 `fbw` (windowed peak) | live but effectively inert |
| **wtex LRU + 3-state lock** `dg_saturn.cxx:4159-4177, 4413, 4478, 4543, 5064-5069` | slot still being re-plotted from the DISPLAYED list | refuses eviction → that wall is flat for one frame | reached only for a wall that won the surplus race | **flushes** (1→2→0) | 0.2 s. **VERIFIER: the aging loop sits BELOW the `wall_acc_n == 0` early return at `:5059`, so on frames with no VDP1 walls the locks do not age — "2 flushes" can exceed 2 frames.** | row 18 `tx bk q` | live, landed today; correct, and **not** the cause of the texture bug |
| **`sat_tex_load_budget`** `r_segs.c:230-261`, refill `r_bsp.c:102-103` | **real disc milliseconds** (`w_cd_ms10`) | walls flat / planes potato / sprites skipped past the budget | `!= 0` — **baked at 20 ms/frame on 2026-08-26**, chord removed. Inert on cart. 🔴 Its pad R+X was the SECOND owner of that chord: byte-identical to the wall-fill ladder apart from a 1p gate, so in single-player one press moved the wall-fill rung **and** the disc budget (20→40→0→10). The 4p hardware videos of 2026-08-26 escaped only because the split gate kept it silent — `lb20:0/0/0.0` on all four frames. | **real time** — the only correctly-clocked budget in the codebase | 1 frame | row 18 `lb…` | live — **this is the model the four frame-clocked controllers above should have copied** |
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
| **`sat_near_sprites`** `r_things.c:180/750` | fixed distance | culls far non-shootable decorations | **REMOVED 2026-08-26** — baked ON, variable deleted | none | n/a | — | **was a constant printed as if it were a knob** (`ns` cut 08-09); it had had no writer since its chord was reclaimed, so it was neither settable nor observable |
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
| `B` | `vdp1_budget_cmds` (`dg_saturn.cxx:1598`), 0..248 | 248 | measured VDP1 command budget from LOPR. **CONSOLE-ONLY**: the learning signal is the LOPR/CEF latch, unmodelled by Ymir ⇒ `B0` on every emulator capture, in every mode, forever — that is the emulator, not a defect. `B0-` never measured (allocators treat it as unlimited). `B<n>!` = **unchanged all window while below the ceiling ⇒ the climb is BLOCKED** (any overrun zeroes `vdp1_budget_clean` at `:5992`), so recovery is unbounded, not 26 s |
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
