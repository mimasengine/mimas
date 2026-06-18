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

## VRAM map (the hard constraint)

VDP2 = 4 banks × 128 KB. Today: **A0** (0x25E00000) = sky NBG0 bitmap, **B0**
(0x25E40000) = framebuffer NBG1 bitmap, **A1**/(0x25E20000) + **B1**(0x25E60000) = FREE.

RBG0 needs a **dedicated bank with all 8 access cycles** (SRL `srl_vdp2.hpp`:
`if (screen==scnRBG0) numCycles=8`) and, for perspective, a **coefficient table in a
*different* bank** (can't read pattern + K-table from one bank in one cycle):

- **A1** = RBG0 cell data + 16-plane map (the tiled flat)
- **B1** = coefficient table (per-line 1/z) + the ROTSCROLL param

→ fits, but consumes BOTH free banks (no room left for a future cloud-parallax layer).

## Risks (ranked)

1. **VRAM cycle pattern / RAMCTL coexistence (#1, the Phase-0 test).** Our NBG0/NBG1
   use *raw* SGL `slBitMapNbg0/1` at fixed A0/B0; SRL's VRAM allocator (which manages
   RAMCTL + cycle reservation) is bypassed. SRL note (`srl_vdp2.hpp:11`): bitmap RBG0
   needs RAMCTL bank-reserve bits the standard bitmap fns don't set, and "allocate RBG0
   **before** NBG0-3". Adding RBG0 *after* our NBG setup may disturb NBG0/NBG1's cycle
   pattern. **Use SGL cell-based RBG0 (register-managed by slCharRbg0/slPageRbg0/
   slPlaneRA/sl16MapRA), not bitmap** → avoids the unset-RAMCTL trap.
2. **Perspective coefficient table + view tracking (the math).** Per-frame K-table
   (1/z per scanline) + ROTSCROLL matrix from viewx/viewy/viewz/viewangle. Standard
   SGL Mode-7 but unvalidated here.
3. **5th layer in the priority/index-0 scheme.** index 0 gains a 3rd meaning
   (sky / VDP1 wall / big-flat). Resolved by priority + screen region: NBG0 sky(4) <
   RBG0 floor(new) < VDP1 walls(5) < NBG1 game(6). RBG0 line-disabled above the horizon
   (centery) so sky vs floor split cleanly. Touches the scheme the active VDP1 session
   tunes → coordinate.
4. **No native distance fog on a hardware flat** → uniform lighting unless a VDP2
   line-colour gradient approximates Doom's diminishing light. Quality caveat.

## Phases (validate riskiest-cheapest first, hardware each step)

- **Phase 0 — bring-up + coexistence (CURRENT).** RBG0 cell plane, single repeating
  tile, *identity* ROTSCROLL (flat, no perspective, no K-table), priority 5 so it shows
  through the index-0 sky/ceiling region. Goal: does RBG0 display **without breaking
  NBG0/NBG1** (the cycle-pattern risk)? Gated `VDP2_RBG0_TEST`, throwaway. dg_saturn.cxx.
- **Phase 1 — Mode-7 perspective.** Replace the identity ROTSCROLL with a ground-plane
  matrix + per-line K-table driven by viewz/viewangle; hand-set or via slScrMatSet/
  slScrMatConv. Tile a *real* Doom flat (64 cells) instead of the test tile. Validate
  the floor tracks the player on the cour-extérieure (the 93 % spot).
- **Phase 2 — dynamic flat selection + core skip.** Per frame pick the dominant
  `(picnum,height)` (reuse the row-13 grouping), upload that flat to A1, set RBG0 height
  + K-table; core hook `sat_vdp2_floor` skips that visplane (writes index 0, like the
  sky-skip) so RBG0 shows exactly there. Floor OR ceiling (whichever is dominant).
- **Phase 3 — polish.** Horizon line-disable, distance gradient, ceiling support,
  measure the real fps win vs the row-13 prediction.

## API (SGL, all in `modules/sgl/INC/sl_def.h`)

`slCharRbg0(COL_TYPE_256, CHAR_SIZE_1x1)` · `slPageRbg0(page, char, PNB_2WORD|CN_12BIT)`
· `slPlaneRA(PL_SIZE_1x1)` · `sl16MapRA(uint8_t plane[16])` · `slOverRA(mode)` ·
`slRparaInitSet(ROTSCROLL*)` · `slCurRpara(RA)` · `slMakeKtable/slKtableRA(tbl, K_ON|…)` ·
`slPriorityRbg0(n)` · `slScrAutoDisp(…|RBG0ON)`. ROTSCROLL = `struct rdat`
(XST/YST/ZST, DXST/DYST, DX/DY, MATA-F, PX/PY/PZ, CX/CY/CZ, MX/MY, KX/KY, KAST/DKAST/DKA).
