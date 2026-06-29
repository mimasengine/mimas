# Split-screen (2-player) benchmarks — living comparison table

Ymir measurements of the local 2-player VERTICAL split (two 160px half-views), for
comparing optimizations. **Ymir understates memory/bus-bound cost** — treat absolute ms
as relative/mechanical; gain *verdicts* need hardware (see [[doomsrl-rec-benchmarks]]).
Overlay rows: `SPL sw v0 v1 k` (row 6, the split-block breakdown, ms per piece) +
`MST` + `Bw/Bp/P/M` (row 20) + `VAL hp/cov/pool` + `vp` (row 2).

`sw` = both `R_SetViewWindow` (size-table recompute). `v0`/`v1` = each
`R_RenderPlayerView`. `k` = the single VDP1 wall kick (flush + plot).

## Baseline — after the R_SetViewWindow size-table cache (core f61265b, 2026-06-21)

`sw` is now **0** in all scenes (was ~148ms = the old bottleneck, fixed).

| scene | pot | vp | v0 | v1 | **k** | MST | inst fps | SLVidle | Bw | Bp | P | M | cov |
|-------|-----|----|----|----|------|-----|----------|---------|----|----|----|----|-----|
| simple (corridor/eau) | 0 | 26 | 36 | 35 | 11 | 88 | 11.3 | 25% | 2.4 | 5.8 | **22.7** | 1.3 | 1008 |
| simple | 1 | 26 | 13 | 14 | 13 | 46 | 21.3 | 62% | 2.3 | 5.5 | 3.4 | 1.2 | 1008 |
| simple | 2 | 26 | 15 | 14 | **23** | 35 | 28.0 | 65% | 2.7 | 5.9 | 3.2 | 1.3 | 1008 |
| dense (screens room) | 0 | 33 | 33 | 40 | 18 | 94 | 10.6 | 36% | 2.7 | **11.0** | 22.0 | 1.5 | 1770 |
| dense | 1 | 33 | 22 | 25 | 18 | 67 | 14.8 | 59% | 2.3 | **11.4** | 7.6 | 1.7 | 1770 |
| dense | 2 | 33 | 21 | 25 | **28** | 55 | 18.1 | 58% | 2.3 | 11.4 | 8.0 | 1.7 | 1770 |

Heap stable `hp 36516/49152`; `vp` peaks 34 (split) / 32 (1p E1M1); `pool 0` (SAT_VISPLANE_POOL off).
On **console**: pot0 ≈ **~100ms / ~7fps = "playable"** (Romain, 2026-06-21).

### Reading the baseline — three levers, ranked

1. **The kick (`k` = 11-28ms) = the VDP1 wall flush CPU work (`vdp1_walls_flush`) — and the cost
   is the PERSPECTIVE-MATH DIVISIONS, FIXED 2026-06-22.** Investigation chain (each step read
   the code / measured, not assumed): (a) NO spin-wait in the kick path ("exposed VDP1 wait" was
   wrong); (b) added `bk` telemetry (bakes/frame) → **`bk`=0 on Ymir in BOTH split and 1p** (bk
   is deterministic = HW too) → the 22-slot texture cache does NOT thrash, baking is NOT the cost;
   (c) read `wall_emit`/`wall_emit_band` → the cost is ~6 int + ~4 int64 **software divisions per
   tile/band** by the per-wall constants `du`/`xspan`/`vspan` (the SH-2 has no hardware divide).
   **FIX (`SAT_WALL_RMUL`, default 1): reciprocal-multiply** — precompute `round(2^22/den)` once
   per band/wall, multiply (hardware) instead of divide; sign-folded, round-to-nearest, sub-pixel
   error. **Validated on Ymir (Romain, 2026-06-22): `k` drops, no seams.** Flip `SAT_WALL_RMUL` 0
   for the original divisions (A/B). [append the measured k-drop in the table below]
2. **Bp (software wall-prep) = 5.5-11.4ms/view**, the dominant per-view software term in dense
   scenes. Lever: **detailshift** (halve the columns → ~half Bp; ~5-11ms/frame; a quality hit).
   (Offloading wall-prep to the slave is CONFIRMED DEAD — memory-bound, tried 3×; the slave win
   that DID ship is the TAS plane work-steal, default-on, core 73f8cdc / parent 4857f87.)
   detailshift caveat: the VDP1 wall emit
   (`sat_wall_vdp1`) does NOT shift x by detailshift today (walls would squish to the left
   half) → needs `x<<detailshift` in the emit + R_SetViewWindow width/columnofs/colfunc.
3. **P (floors) = 22ms at pot0, ~3-8ms at pot1/2** → potato is the floor lever (known). At
   pot1/2 P is already cheap, so it is NOT the split ceiling.

**Slave idle 58-65% at pot1/2** → headroom now consumed by the shipped TAS plane work-steal.

> **NOTE.** The slave is NOT idle-doing-nothing: it already runs **P3 plane-split
> + masked-by-half SHIPPED** within each view (the slave draws half the visplane worklist and the
> RIGHT-half vissprites of every view — `r_parallel.c` `RP_DispatchPlanes`/`RP_DispatchMasked`,
> gates `sat_plane_parallel`/`sat_masked_parallel`=1). The 58-65% idle is the headroom **AFTER**
> that phase work; it is now taken by the dual-SH2 plane work-steal (TAS, default-on, core 73f8cdc
> / parent 4857f87). Offloading Bp (wall-prep) to the slave was tried 3× and is CONFIRMED DEAD
> (memory-bound). (NB: this is the master/slave LEFT/RIGHT masked split, not the x-split
> 2nd-renderer, which is compiled off — overflows 2MB.)

## Future rows (append measured runs here for comparison)

| change | scene | pot | v0 | v1 | k | MST | fps | notes |
|--------|-------|-----|----|----|----|-----|-----|-------|
| _(detailshift toggle)_ | | | | | | | | TBD |
