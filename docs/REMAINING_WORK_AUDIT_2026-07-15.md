# Remaining-work audit — 2026-07-15 (code-verified)

*Purpose: replace the stale "remaining levers" lists (esp. the `perf-audit-2026-07-07`
memory, which **predates ~33 commits**) with a picture **proven against `git log` + the
current source**. Method: `git -C core log --since=2026-07-06` (31 commits) + the Mimas
platform log (33 commits) + `grep` of the live code. Every "SHIPPED" below is a real
commit; every "REMAINS" was grepped for and not found (or found only as a parked/WIP stub).*

> **Why this doc exists.** On 2026-07-15 a session re-derived R3.1 from scratch and re-listed
> R4 / masked-split / sound-precache / M7 as "remaining" — all already shipped. The 07-07
> audit was the source. **Always cross-check the newest streaming/perf memory + `git log`
> before calling anything remaining.**

---

## A. Shipped since the 07-07 audit — do NOT re-propose

| Lever (as named in old lists) | Commit(s) | Note |
|---|---|---|
| Sound precache (spawn-derived → SCSP) | `fe5acf3` / `c68adda` | `SAT_PrecacheLevelSounds`; **not** a stub. Zero Doom-zone cost. |
| R3.1 sprite-header boot index | `7d708bd` `617bb0f` `de8f2de` | 1 read vs ~1381; Ymir-validated; texture-half MOOT post-R4. |
| R4 lazy texture dirs + diets | `64716c8` `ec555a5` `5fc883e`(R4.4) `ce79125`(R4.3c) `96baabb` | Doom II MAP13/15 load. **TNT/Plutonia fit still unverified — see D.** |
| masked-split slave **under VDP1-things** | `d64debb` | re-enabled 2026-07-09; `!sat_things_emitted` gate lifted. |
| nearSprites cull + clear-on-slave + AIMD | `1640461` `458c494` | default-ON. |
| RBG0 mark-suppress | `3191022` | dominant floor never split. |
| Crash-proofing: visplanes / OPENINGS / COMPOSITE | `b5e77c5` `e43d6bb` `bc01465` `9cb530b` | graceful sinks, not `I_Error`. |
| Sight temporal cache | `1f74177` | master-side. **Slave-idle prefill NOT done — see B.** |
| SQ independent axis (sprite/floor/ceiling) | `40859e9` `99706b9` | global + per-plane. **Per-VIEW asymmetry NOT done — see B.** |
| Rotation ladder + per-map ceiling + distance LOD | `66fd0a3` `9d247e4` | cart-fit for big WADs. |
| R0.2 k-meter / R1 bounce / R5.1 preload | `2ee6b15` `25450df` | streaming instrumentation + reads. |
| M7 low-res (160-wide packed + VDP2 ×2 zoom) | `e0a3638` `97da525` `0fceca8` | = the horizontal "NBG1-MAG" idea, **1p only**. |
| 3/4p + 2p compact HUD, intermission band | `e25e7a2` `a4b14fd` `ab0668f` | |
| L2-RECLAIM (parity cmd-buf 80K→8K, +72K zone) | `9e59f22` | |
| tic catch-up cap +8 (kill slow-motion) | `9a48d9c` | |

---

## B. Perf — verified REMAINING

1. **M7/M8 in multiplayer** *(NEW finding — high value)*. M7 is 1p-gated
   ([dg_saturn.cxx:899](../src/dg_saturn.cxx#L899)) because the VDP2 ×2 zoom is **whole-layer**.
   But the split boundaries fall cleanly under ×2: render each viewport at **half its width
   packed into source [0,160)**, the one global `ZMXN1=0.5` restores every viewport to
   position (2p x=160 boundary ← source 80; 3/4p same on x, y untouched). **Per-player quality
   ratio = identical to 1p M7.** Split is where fps hurts most (2× render) → biggest relative
   win. **Expected ~+40-70 %** in fill-bound split scenes. Blocker is implementation (per-view
   `R_SetViewWindow` targets packed x; reconcile the masked-split slave upsampler), **not**
   architecture — the "out of scope" note in `LOWRES_RENDER_STUDY.md` missed the geometry.

2. **NBG1-VHALF (M8 = M7 + vertical halving, 160×100 = ¼ fill)**. Display side is free
   (ZMYN1, symmetric to ZMXN1). **Render side is the wall** — Doom has no native vertical
   decimation (column drawer: yslope/centery/ylookup half-sampling). `LOWRES_RENDER_STUDY.md`
   Phase 3 = HARD. Do only if M7 measures a big win and more is wanted.

3. **segloop-micro** (~1-4 ms). `R_RenderSegLoop` is only **profiled** (`prof_segloop`,
   [r_parallel.c:1370](../core/r_parallel.c#L1370)), never micro-optimized. Real, small.

4. **asymétrie-SQ-per-view** (PARTIAL). A per-view downgrade exists (RBG0 off for non-P1,
   [dg_saturn.cxx:831](../src/dg_saturn.cxx#L831)) but the full "each viewport its own
   SQ/detailshift" is not wired. Scaffold present ([d_main.c:403-408](../core/d_main.c#L403)).

5. **config-3 back-screen gradient** ("the sleeper"). `rbg0_linecol_mode=0`, gradient OFF,
   parked WIP ([dg_saturn.cxx:1470](../src/dg_saturn.cxx#L1470)). Distance-light cosmetic.

6. **Slave-idle sight prefill**. Temporal cache is master-side; the slave sits 81-99 % idle
   (HW-measured) — a private-bitset prefill on the idle slave is unbuilt.

7. **fuzz zero-framebuffer-read** (PARTIAL). `RP_RecordFuzz` exists
   ([r_parallel.c:1738](../core/r_parallel.c#L1738)); the "never read the shared framebuffer"
   RMW-elimination extension is not confirmed done — verify before pursuing.

---

## C. Streaming / endgame — verified REMAINING

- **R2.3 async pump** (`sat_cd_pump`, frame-budgeted `GFS_Nw*`). The last heavy streaming
  brick. Needs a **real-CD A/B** (R2.2 regressed on ODE, default-OFF `0bb3fc8`; Ymir models
  ~36-41 ms/cmd so relative A/B is possible but absolute k is HW-only).
- **R3.4 staging UX** (stage the map blob during the intermission read, masked).
- **Sound-RAM victim cache** for streamed lumps (SCSP RAM is off-zone, mostly free).

## D. Correctness / mods — verified REMAINING

- **DEHACKED** — absent. Blocks the mod endgame (monster/weapon/string patches).
- **Runtime PWAD-merge** — absent, BUT an **offline `merge_wad` tool exists** (`2917f90`):
  PWADs can be merged into the IWAD before building. Covers static mod discs, not runtime load.
- **TNT/Plutonia mis-ID as doom2** — wrong intermission/finale text + level names.
- Saves non-functional (CD read-only) — by design.

## E. Measurement-gated (not "implement" — "verify")

- **Does TNT/Plutonia now FIT** after the R4 diets? R4 validated Doom II MAP13/15 only. The
  07-07 data (TNT 23/32 maps over, MAP20 −318K) predates R4.3c/R4.4 — **re-audit the zone
  high-water on a TNT/Plutonia boot**.
- **R3.1 HW wall-clock** (Ymir validated the *path*, not absolute boot time).
- **4p HW bench** (never captured); **2-spot 1p bench post-RBG0** (25/06 numbers stale).

---

## Recommended order

1. **M7 multi** (B1) — biggest verified fps win, lands where the game is slowest, quality
   already judged acceptable in 1p.
2. **TNT/Plutonia fit re-audit** (E) — decides whether the real endgame is unblocked or needs
   more R4 diet.
3. **R2.3 async pump** (C) — the structural streaming fluidity lever (HW-gated).
4. Smaller: segloop-micro, SQ-per-view, then M8/VHALF only if M7 proves out.
