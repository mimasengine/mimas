# RBG0 dual rotation parameters (RPA/RPB) — SETTLED: cell-mode only

> **Status: SETTLED-NEGATIVE for the bitmap floor (2026-07-06).** The goal was a two-player
> hardware floor using VDP2's two rotation parameters (RPA = player 1, RPB = player 2) selected
> per screen-region by `RPMD=W_CHANGE` + a rotation-parameter window. **It cannot work with Mimas's
> shipping BITMAP floor.** RPB requires **cell mode**. Proven by SEGA's own SGL sample and two
> shipping games. This doc supersedes the RPA/RPB "stretch" section (§4) of
> [RBG0_SKY_SPLIT_ANALYSIS.md](RBG0_SKY_SPLIT_ANALYSIS.md).

## TL;DR

- **The dual-parameter *mechanism* works on real Saturn** — two rotation planes coexist with **no
  snow** (the review's feared coefficient-bandwidth wall did not appear). That question is answered:
  a two-camera HW floor is viable *in principle*.
- **But RPB never renders a real floor on Mimas's BITMAP floor.** On Ymir the RPB half is black; on
  hardware it smears (RPB's matrix samples the shared bitmap, but RPB has no per-line coefficient/plane).
- **Root cause (definitive):** the second rotation parameter needs its **own plane + map**
  (`slPlaneRB` / `sl1MapRB`), which only exists in **cell mode**. `slBitMapRbg0` sets one shared image
  with **no per-parameter plane**, so there is no `slPlaneRB` counterpart — bitmap RBG0 is
  **single-parameter (RPA only)**.
- Therefore the two-player HW floor is achievable **only by re-architecting the floor to cell mode** —
  the snow-prone, bank-heavy path the project abandoned to get a clean bitmap floor.

## The proof (SEGA's own dual-parameter sample)

`refs/JUNE96_DTS.ISO` → `LIBRARY/SGL21/SAMPLE/S_8_9_2/MAIN.C` is SEGA's official dual-rotation-parameter
example. Its setup:

```c
slRparaInitSet(RBG0_PRA_ADR);        /* RPB lives at RPA + 0x80 (confirmed) */
slMakeKtable(RBG0_KTB_ADR);
slCharRbg0(COL_TYPE_256, CHAR_SIZE_1x1);            /* CELL mode (character), NOT bitmap */
slPageRbg0(RBG0RB_CEL_ADR, 0, PNB_1WORD|CN_12BIT);
slPlaneRA(PL_SIZE_1x1);  sl1MapRA(RBG0RA_MAP_ADR);  /* RA: its OWN plane + map ... */
slKtableRA(RBG0_KTB_ADR, K_FIX|K_DOT|K_2WORD|K_ON);
Cel2VRAM(tuti_cel, RBG0RA_CEL_ADR, 65536);          /* ... + its own cells (texture "tuti") */
slPlaneRB(PL_SIZE_1x1);  sl1MapRB(RBG0RB_MAP_ADR);  /* RB: its OWN plane + map ... */
slKtableRB(RBG0_KTB_ADR, K_FIX|K_DOT|K_2WORD|K_ON);
Cel2VRAM(sora_cel, RBG0RB_CEL_ADR, 28288);          /* ... + its own cells (texture "sora") */
slRparaMode(K_CHANGE);
/* per frame: slCurRpara(RA)+matrix+slScrMatSet; slCurRpara(RB)+matrix+slScrMatSet; slSynch(); */
```

Each parameter gets a **separate plane, map, and cell texture**. There is **no `slBitMapRbg0` for
parameter B** — `SGL21/DOC/SCROLL.TXT` lists `slPlaneRA`/`slPlaneRB` and a single `slBitMapRbg0`. The
two shipping games that use RPB — Panzer Dragoon's `field_d3` and `d5_starfield`
(`saturn-refs/Azel/AzelLib/field/...`) — are **also cell mode** (`CHCN=1`, `PNB`) and set
`RxKTE` on **both** params.

## What we ruled out along the way (all correct, none was the fix)

Ten build/hardware iterations confirmed every input was right, which is how we knew it had to be
architectural:

| Checked | Result |
|---|---|
| `RPMD` on chip | `0x0003` = W_CHANGE ✓ |
| `KTCTL` (coeff enable, bit 8 = RPB) | `0x4141`, high byte = low ✓ |
| `KTAOF` (coeff base; A=bits0-2, B=bits8-10) | `0x0000` for both — K-table at A0 offset 0 ✓ |
| RPB slot offset | RPA **+0x80** (SGL `SCROLL.TXT`: "Parameter B stores parameter A + 20H", 0xE0 total) ✓ |
| RPB VRAM block (dumped) | byte-identical to RA (`RA = RB`) ✓ |
| Commit path | manual block-flush **and** per-frame `slSynch` — both still black |
| SGL internal state | `slRparaMode(W_CHANGE)` proper (not just a poke) — still black |

Everything the emulator's own renderer keys on (`coefenab_B`, `coeftbladdr_B`, `KAst_B`, `DKAst_B`) was
correct — but with no `slPlaneRB`, RPB has no plane to render, so it collapses regardless.

## The silver lining, and the catch (for the future cell floor)

- **Silver lining:** because cell-mode RPA/RPB have *independent* cells, the two players can have
  **different floor flats** — strictly better than the "same texture only" limit assumed for the
  bitmap approach.
- **Catch:** cell mode means separate planes + maps → more VRAM banks and **3 reads/dot** (coeff +
  pattern-name + cell), which is exactly what made Mimas's earlier cell floor **snow** on hardware
  ([VDP2_RBG0_CURRENT_STATE.md](VDP2_RBG0_CURRENT_STATE.md)). And Mimas's software framebuffer (NBG1)
  already claims a bank the dual-plane cell floor would want. The `S_8_9_2` sample has no software
  framebuffer, so its bank budget doesn't translate 1:1.

## Future plan (owner, 2026-07-06): revisit as a cell-floor re-architecture

Not a fix to the bitmap floor — a separate, scoped project, best targeted where its costs are cheapest:

1. **Indoor / no-sky maps** — dropping the HW sky frees a bank and removes NBG0's competing read
   stream, giving the 3-reads/dot cell floor its best shot at fitting + not snowing.
2. **Better per-flat lighting** — cell mode's independent maps/palettes allow richer floor shading
   than the single baked-light bitmap.
3. **Double POV** — RPA = P1, RPB = P2 (different flats OK), via `slPlaneRB`, `RPMD=W_CHANGE` + a
   rotation-parameter window at the split.

Reference recipe: `refs/JUNE96_DTS.ISO` → `LIBRARY/SGL21/SAMPLE/S_8_9_2` (SEGA official) and the
Panzer Dragoon field routines in `saturn-refs/Azel`.

## Sources

- **SEGA Developer Technical Support CD, June 1996** — `refs/JUNE96_DTS.ISO` (gitignored, ~525 MB).
  Key content: Saturn Programming Manual (`DOCUMENT/SATURN/PROGRAM1.PDF`, `PROGRAM2.PDF`), SGL
  (`LIBRARY/SGL21`, incl. sample `S_8_9_2` + `DOC/SCROLL.TXT`), Technical Bulletins (`SATTECHS.PDF`).
- SGL `SCROLL.TXT` — `slRparaInitSet` (RPB = RPA+0x80), `slPlaneRA`/`slPlaneRB`, `slBitMapRbg0`.
- `saturn-refs/Azel` — `field_d3` / `d5_starfield` (shipping dual-param RBG0, cell mode).
- Online mirrors (general, less specific): [Yabause VDP2 wiki](https://wiki.yabause.org/index.php5?title=VDP2),
  [VDP2 User's Manual mirror](https://docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/vdp2/index.htm).
