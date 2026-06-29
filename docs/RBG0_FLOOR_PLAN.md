> ⚠️ HISTORICAL / SUPERSEDED 2026-06-27 — the RBG0 floor SHIPPED as a clean 512x256 8bpp
> BITMAP (RBG0_BITMAP=1, commits 19768ca/41dd895), verified on real Saturn. Layout: bitmap=A1,
> coeff/K-table=A0, fb=B0, B1 FREE (RPT only) — 2 rotation banks, no map. The "snow", the 3-bank
> cell layout + cell VRAM map, the "floor XOR sky" law, the "use cells not bitmap" advice, the
> slSynch-as-floor-fix, and the "CYCxx commit missing / phases gated" status below are ALL
> OBSOLETE. Snow was cycle-pattern STARVATION, fixed by bitmap + RDBS=0x0D + parked A0/A1 cycles
> 0xEEEE (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, already in tree — NOT slSynch); B1 freed (no
> map) → floor + a B1 sky coexist; floor gated potato-0 + 1 player.
> **Authoritative current doc: `docs/VDP2_RBG0_CURRENT_STATE.md`.**
> This file is retained ONLY for the FLAT-profiler data + the original motivation below.

# VDP2 RBG0 — hardware floor/ceiling offload (the "biggest flat" lever)

Deport the single **largest visible floor/ceiling flat** of each frame to a VDP2
**RBG0 rotation plane** (Mode-7), leaving the software renderer to draw everything
else. Decided GO from hardware data; **dynamic** (RBG0 re-points to the per-frame
dominant flat), not a static per-level pick.

## Why (the data)

Row-13 `FLAT` profiler over 6 L1/L2 views (all pot0):

| scène | dom% | n | P `m` | EX |
|---|---|---|---|---|
| couloir | 67% | 10 | 4.6 | 7.3 |
| salle eau | 49% | 19 | 6.6 | 11.5 |
| salle rouge | 74% | 26 | 12.7 | 6.2 |
| salle verte | 54% | 20 | 8.8 | 7.0 |
| cour ext | **93%** | 11 | 4.4 | 5.5 |
| grand hall | 49% | 33 | 9.2 | 8.8 |

`dom%` = the single biggest `(picnum,height)` flat's share of floor/ceiling fill =
**49–93 %, avg ~64 %**. The fill is concentrated, not fragmented → the single-flat
trick bites. Estimated saving ≈ `dom% × (m + EX_floor)` ≈ **8–14 ms/frame (~15–24 %
of frame) at pot0**; at the potato ship config the CPU prize shrinks but RBG0 then
buys **quality** (the most-visible surface stays textured-in-perspective for free
instead of a flat potato colour).

This idea beats the blanket "all floors → RBG0" we rejected: that failed on
multi-height (one RBG0 = one plane height); a *single* dominant flat is at *one*
height by construction. And under the existing **layer inversion** (software NBG1
on top, VDP1 walls below) the old "VDP1 paints over VDP2" objection is moot.

## API (SGL, all in `modules/sgl/INC/sl_def.h`)

`slCharRbg0(COL_TYPE_256, CHAR_SIZE_1x1)` · `slPageRbg0(page, char, PNB_2WORD|CN_12BIT)`
· `slPlaneRA(PL_SIZE_1x1)` · `sl16MapRA(uint8_t plane[16])` · `slOverRA(mode)` ·
`slRparaInitSet(ROTSCROLL*)` · `slCurRpara(RA)` · `slMakeKtable/slKtableRA(tbl, K_ON|…)` ·
`slPriorityRbg0(n)` · `slScrAutoDisp(…|RBG0ON)`. ROTSCROLL = `struct rdat`
(XST/YST/ZST, DXST/DYST, DX/DY, MATA-F, PX/PY/PZ, CX/CY/CZ, MX/MY, KX/KY, KAST/DKAST/DKA).
