# Bug — P1 hardware floor (RBG0) black in 2-player split started from the menu

**Status: OPEN — not fixed.**

This note records ONLY the observed behaviour (no theory / no root-cause claims).

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
