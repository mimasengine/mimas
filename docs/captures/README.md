# Console capture ledgers

Decoded debug-overlay readings from real Saturn captures. **The decoding is the expensive
step** (one vision pass per frame); the raw videos are cheap to re-extract. So the decoded
rows live here and the videos do not.

---

## `2026-08-25_console_overlay.{json,csv}`

**121 frames, 106 legible**, from nine videos recorded by the owner on 2026-08-25 09:54.

| | |
|---|---|
| WAD | shareware (`build.ps1 -Wad Doom1s`) |
| Build | the frame-sum instrumentation of 2026-08-25 — row 2 `n<v> Bw Bp P M d`, row 4 `BPS em pr lp wp`, row 5 SLV frame-scoped, row 17 4th slot = the 3p minimap. TLSF pool 30.38–30.62 KB |
| Videos | `1PLAYER-DEBUG{1,2}`, `2PLAYER-DEBUG{1,2}`, `3PLAYER-DEBUG{1,2}`, `4PLAYER-DEBUG{1,2,3}` (`.mp4`, 720×480, ~60 s each) |
| Modes | 1p 26 legible / 2p 24 / 3p 23 / 4p 33 |

Re-extract the frames (4 s apart, the spacing these rows were read at):

```bash
ffmpeg -v error -i <VIDEO>.mp4 -vf "fps=1/4" -start_number 0 frames/<VIDEO>/%02d.png -y
```

### Columns

`video, file, legible, note` identify the frame. Then the overlay, by row:

- row 0–1 — `fps mst r t s b dg pr1`
- row 2 — `n bw bp p m d` — **the frame sum**, `n` = views summed, `d` = the frame's drawsegs
- row 4 — `bps_em bps_pr bps_lp bps_wp`
- row 5 — `slv_b slv_pb slv_pm slv_ps`
- rows 8/9/11/14/15/17/18/21/23/24 verbatim — `vd1 thp lim seg spr` / `v1` (1p) or `spl` (split) / `vrm gov thk tic`

A field is **absent when the digit was not certain**. The decoders were told to drop rather
than guess; an empty cell is not a zero. `legible: false` marks menus, load screens and the
title loop.

`2026-08-25_anomalies.json` holds the per-video cross-check notes the decoders raised.

---

## Reading rules — the ones that have already burned someone

1. **Rows 23/24 are per-FRAME sums.** When `MST` rises, `x` (tics per frame) rises with it, so
   `n`, `th`, `mo`, `w` all grow with no cost having changed. **Divide by `x` before comparing
   two frames.**
2. **Row 2 is this frame; row 4 is a ~1 s-old sample of the same quantity** (row 2 prints every
   frame, row 4 rides the 1 Hz overlay burst). ⚠ True for THIS ledger's build only: the same
   day's second round moved row 4 to per-frame (dg_saturn.cxx, end of DG_DrawFrame), so builds
   after 2026-08-25 ~12:00 print rows 2 and 4 on the same frame. In 1p `wp` and `Bp` are the same bracket and
   must agree — but only on a still scene. A large `wp` − `Bp` gap on a moving scene is the
   cadence, not a defect.
3. **`V1` (and therefore `B`/`ec`) is printed in 1p only** — it is gated on
   `sat_local_players <= 1`. Any frame-health filter built on `B` applies to the 1p videos
   alone; in split, row-1 `pr` is the available proxy.
4. **`REC`, `mx` and the `MX` locator stay per-VIEW in split** even though row 2 is now the
   frame. Do not difference them against row 2.
5. Nothing here is an Ymir number. Everything is console. The inverse also holds: do not
   patch a gap in this table with an Ymir reading.

---

## Build boundary — 2026-08-25 12:xx (Phase A instruments)

**This ledger was decoded from a build that no longer exists.** Six instrument changes landed the
same afternoon. The historical rows above stay exactly as recorded; these notes say where old and
new strings decode under different rules, so the two are never pooled.

| column | then (this ledger) | now |
|---|---|---|
| `bps_wp` | `row 4 = em pr lp wp` | **`wp` no longer exists.** Row 4 is `em hd pr lp tl` — `wp` was `prof_wallprep`, i.e. row-2 `Bp` restated. The post-pass identity is **`hd+pr+lp+tl == Bp`**, and checking it is the FIRST thing the next console pass must do. |
| `seg` | `SEG c f k lk`, **per-VIEW in split** | `SEGn<v> c f k lk`, **frame sums**. Old split values were the last quadrant only — that is why `c` reads a flat 257/256 in 2p/4p here. |
| `lim` | `LIM vp ds ss zf<n>k lg<n>k` | `LIM vp.<planes> ds ss o<rows> zf lg` — new `o` field, `k` suffixes gone, `zf`/`lg` clamped at 999, pool-ovf **pre-halved**. |
| `v1` | 1p only, 26/26 vs 0/80 | prints in split too, on **row 6**; `LP`/`ws`/`tx` retired, `i` folded into the row name. A new decode pass needs a `row 6` column this one does not have. |
| — | `MX` locator per-view in split | **MX no longer prints in split at all** (row 6 is V1's there). |
| row 5 `mw` | last view only | frame max — **larger in split, and correct**. Not a slave regression. |

Reading rule 2 above loses its subject: `wp` is gone, and the replacement identity is
`hd+pr+lp+tl == Bp`. Reading rule 3 (`V1` is 1p-only) describes the defect that was just closed —
it stays true for THIS ledger and is false for the next.

⚠ **What is still valid from this file for sizing `hd`/`tl`**: the hole medians (1p 2.5 / 2p 7.0 /
3p 11.3 / 4p 11.2 ms) come from `wp − pr − lp`, a subtraction **within row 4**, all three latching
on the same view — so the cadence bug described in rule 2 does not touch them. What is NOT valid
from this file is any per-frame `wp` vs `Bp` comparison.
