# Bug — P1 hardware floor (RBG0) black in 2-player split started from the menu

**Status: OPEN — root-cause analysis narrowed (see §Analysis 2026-07-02); fix not yet built.**

Sections up to "Kept change" record ONLY the observed behaviour.

## Setup

- 2-player split screen, potato level 0.
- Player 1's floor = the VDP2 hardware rotation floor (RBG0). Player 2 = software floor.
- Hardware sky (NBG0 cell sky) ON (the default).

## What happens

- Starting a **2-player game from the MENU**: Player 1's floor is **sometimes correctly
  textured, sometimes fully BLACK (opaque)**. It alternates across launches — about one
  launch out of two. Observed sequence over successive launches: textured, black, textured.
- The black is **stable for the whole game** once a launch lands on it (it does not flicker
  in and out during play).

## What does NOT reproduce it

- **Adding the 2nd player IN-GAME** (dropping a 2nd pad into a running 1-player game):
  the floor is **always textured** — the black never happens on this path.
- **Hardware sky turned OFF** (live toggle pad L+C): the floor is **always textured** from
  the menu too — the black never happens with the sky off. With the sky ON, the black
  returns.

## Measured (on-screen debug, textured launch vs black launch)

- RBG0's data read back on both a textured launch and a black launch was **identical**:
  RAMCTL bank-select, the RPT (rotation start coord + matrix), the K-table word, and the
  floor bitmap word. The floor bitmap texels are present in both cases.

## Changes that were built + tested and did NOT change the behaviour

(the black still occurred ~one launch in two after each)

1. Forcing a floor-texture re-upload for the first tics of a fresh split level (`leveltime < 8`).
2. Re-asserting the RBG0 bank-select register (RDBS / RAMCTL) every frame.
3. Flipping the sky/floor priority so the floor is above the sky (`VDP2_SKY_OCCL_DIAG 0`).

(1) and (2) were reverted. (3) was reverted (it is the HW sky session's shipping config).

## Kept change (a separate latent bug, NOT this one)

- `core/r_plane.c`: reset the dominant-floor sector cache (`sat_dom_last_sec`) on level
  reload (keyed on `leveltime`). This fixes a sector pointer that dangled across level
  reloads; it did **not** fix the black floor.

---

## Analysis 2026-07-02 — LIBSGL.A disasm (sglB015/sglA modules, `_BlankIn`/`_BlankOut`/`slScrMatSet`/`slScrAutoDisp`)

### Hard facts (disasm-proven) — three project beliefs are WRONG

1. **The build is NOT "no-slSynch" at the ISR level.** With `SRL_FRAMERATE = 0` (Makefile),
   SRL calls `slDynamicFrame(ON)` / `SynchConst = 1`. `_BlankOut` then *reloads* `SynchCount`
   (path `blnkout_09`: `SynchCount = ((flags&16)>>4)+1`) and decrements it every vblank-out, so
   `SynchCount == 0` is hit routinely — **`_BlankIn`'s gated block runs on its own, no `slSynch`
   call needed**. That block does, per hit:
   - CPU-DMA the SGL shadow (`0x060FFCC0`) → VDP2 chip regs **`0x00..0x8F`** (size 0x90); and
   - if `BGON & (RBG0ON|RBG1ON)` **and** the rotation dirty flag `@0x060FFCCC` is set (it
     always is — `slScrMatSet`/`slZrotR` set it after every write): **DMA the RPT RAM buffer
     `0x060FFE1C` → RPT VRAM `B1+0x1FF00`, 0x30 bytes (RA), then RAM`+0x68` → VRAM`+0x80` (RB)**.
   - Caveat: the `SynchCount` reload path depends on SGL's FBCR/DMASetFlag state machine, which
     this port bypasses (manual VDP1 FBCR). When the reload path is NOT taken, `SynchCount`
     (a byte) wraps 255→…→0, i.e. the pushes stall for up to ~4.3 s then fire once. **So the
     shadow→chip push and the RPT DMA run erratically — sometimes every frame, sometimes 1/256
     vblanks — depending on the VDP1 state the port left behind.** This is a launch-time source
     of nondeterminism nobody knew existed.
2. **The RPT VRAM has TWO concurrent writers**, not one: our mid-frame `memcpy` (0x54 bytes,
   `dg_saturn.cxx` RBG0_RPT_TRANSFER==2) and the ISR DMA above (0x30 bytes, at vblank-in — the
   same instant the VDP2 fetches the table). Same source buffer, so steady-state they agree,
   but during a launch (transform switching 1p-menu↔split geometry, load frames with no
   DG_DrawFrame) the interleaving can hand the chip a torn table.
   Also: the ISR writes table B at **VRAM+0x80** — the code comment "RB at +0x68" misread the
   disasm (+0x68 is the *source* offset). Our memcpy's +0x68 RB copy lands in a hole the chip
   may not even read.
3. **`slScrMatSet` (K_FIX path, `ssms_k1u_*`) recomputes KAST (0x54), ΔKAst (0x58), ΔKAx (0x5C)
   in the RAM buffer on EVERY call** — the memcpy comment "KAST is written once at init, NOT by
   slScrMatSet" is false. (Benign only because in K_FIX mode the chip walks `slMakeKtable`'s
   generic 64KB 1/n table sequentially; the VRAM 0x54..0x5F words are never pushed by anyone.)

### What the sky can and cannot touch (exonerated paths)

- `slScrAutoDisp`'s `ape` cycle allocator **resets its work buffer every call** (`sad_cl_loop`)
  and allocates purely from (screen mask, screen configs/addresses). No history, deterministic.
  A sky_bit toggle just switches CYCB1 between two both-correct shapes.
- RAMCTL/RDBS is shadow-coherent (`rbg0_commit_ramctl` writes both) — and re-asserting it was
  already tested with no effect.
- Sky cells/map (B1 0x60000/0x6A000) don't overlap the RPT (B1+0x1FF00).

### Remaining suspect set (fits ALL observations: stable-for-the-game, data identical, L+C repairs)

The chip regs **0x90..0x11E are never re-pushed by anything** (ISR push stops at 0x8F, the init
block-flush at 0xFE, one-shot boot slSynch covers all but runs once): windows W0/W1
(WPSx/WCTLA/WCTLC/WCTLD, LWTA0), LCTA, KTCTL/KTAOF, RPTA, priorities (incl. PRIR 0x100), CCCTL
shadow-side, CCRR/CCRLB, RPMD. A wrong value latched there at launch persists for the whole
game (matches "stable"), is invisible to every readback we have (write-only chip; our overlays
read the SGL shadow), and the **L+C toggle repairs exactly this family** — `nbg0_sky_window_
apply/clear` re-pokes WCTLA + W0 + LWTA0 (and flips sky_bit, i.e. the ape mask).
The ~1-launch-in-2 rate needs no counter: the first frames' `sky_bit` depends on the **stale
`sat_sky_px_view[0]`** of the *previous* game at the instant of the relaunch (was P1 looking at
sky or not — ~coin-flip), and the erratic `SynchCount` phase (fact 1) is a second independent
coin.

### Discriminating experiment (1 build, 4 pad chords, run on a BLACK launch)

Each chord re-asserts ONE register family; whichever brings the floor back names the culprit:
1. re-poke windows: WPSx0/1 + WCTLA/WCTLC/WCTLD + LWTA0 (shadow→chip, recipe already in
   `rbg0_floor_window_apply`/`nbg0_sky_window_apply`);
2. re-push 0x90..0xFE block (KTCTL/KTAOF/RPTA/LCTA/priorities NBGx);
3. direct-poke 0x100..0x11E (PRIR, CCCTL/CCRR/CCRLB, RPMD);
4. force CYC A0/A1/B0/B1 chip from the shadow (the one family the per-vblank push DOES cover —
   control group; should do nothing if fact 1's push was live).
Also print on the overlay at launch: `SynchCount`, shadow BGON, shadow CYCB1, `sky_win_view`,
`sat_sky_px_view[0]` — captured on a textured launch vs a black launch.
