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

1. **The kick (`k` = 18-28ms) GROWS with pot level** (pot2 > pot0). Counter-intuitive for a
   wall flush (pot doesn't change wall count) → it's an **exposed VDP1 wait**: at high fps
   (pot2) the master kicks before the previous frame's VDP1 plot has finished (`EDSR` CEF),
   so it stalls; at low fps (pot0) VDP1 finishes during the long CPU frame → cheap kick. So
   the kick ≈ the VDP1 wall **fill/overdraw** time, exposed only when the CPU is fast. Lever:
   reduce VDP1 overdraw (world-anchored vertical cull / close-wall software fallback /
   band-cull, see [[doomsrl-vdp1-capacity]]) — the pot2 lever. HW-relevant.
2. **Bp (software wall-prep) = 5.5-11.4ms/view**, the dominant per-view software term in dense
   scenes. Lever: **detailshift** (halve the columns → ~half Bp; ~5-11ms/frame; a quality hit)
   or the parked slave wall-prep offload. detailshift caveat: the VDP1 wall emit
   (`sat_wall_vdp1`) does NOT shift x by detailshift today (walls would squish to the left
   half) → needs `x<<detailshift` in the emit + R_SetViewWindow width/columnofs/colfunc.
3. **P (floors) = 22ms at pot0, ~3-8ms at pot1/2** → potato is the floor lever (known). At
   pot1/2 P is already cheap, so it is NOT the split ceiling.

**Slave idle 58-65% at pot1/2** → spare capacity for the parked Bp offload (freeze-zone).

## Future rows (append measured runs here for comparison)

| change | scene | pot | v0 | v1 | k | MST | fps | notes |
|--------|-------|-----|----|----|----|-----|-----|-------|
| _(detailshift toggle)_ | | | | | | | | TBD |
