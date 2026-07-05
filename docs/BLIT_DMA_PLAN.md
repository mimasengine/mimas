# M3/M4 — DMA blit + clear plan (2026-07-02)

> **Status: PLANNED.** Lever M3/M4 from [CRITICAL_PATH.md](CRITICAL_PATH.md) §4. Baseline:
> the NBG1 blit costs **b ≈ 5.5–11 ms of pure master time** (224 × `memcpy` 320 B,
> [dg_saturn.cxx:4070](../src/dg_saturn.cxx#L4070)) + the framebuffer clear (`df_post`,
> a few ms). Every ms recovered is master headroom (`fps = 1000/MST`).

## 0. What we know (evidence)

- **Raw-register SCU DMA is DEAD (3×)** — bus lock on real HW even with the VBLANK-IN
  start factor; own post-mortem says: *"If the ~10ms is pursued again: … SGL slDMACopy —
  NOT this raw-register path"* ([dg_saturn.cxx:54-64](../src/dg_saturn.cxx#L54)). The
  224-entry indirect table builder (`dma_table_build`) is still in the tree.
- **`slDMACopy` is the proven path**: wolf4sdl-saturn ships the IDENTICAL blit
  (320 B rows → `BM_512x256` 8bpp, `nbg1Ptr += 128` = 512-B stride) as per-row
  `slDMACopy` × 224 + one trailing `slDMAWait`
  (saturn-refs/wolf4sdl-saturn/saturn.cpp:443-455). saturn-refs ANALYSIS.md: this
  *"beat SCU-DMA and memcpyl; the stride math is identical to Mimas's layout"*.
- SGL API: `slDMACopy(src,dst,len)` = SCU level 0, async; `slDMAWait()`; `slDMAStatus()`
  (SaturnRingLib sl_def.h:2358-2361). Level 0 moves up to 1 MiB. **LWRAM cannot be an
  SCU-DMA source** (SRL_API.md) — our framebuffer is HWRAM ✓. Write-through cache ⇒ RAM
  is always current ⇒ the DMA path needs **no `cache_purge`** (bonus vs the CPU blit).
- The **clear is structural** (layer inversion: NBG1 index-0 holes reveal the VDP1 walls)
  and player-count-dependent (192/160/224 rows) — it cannot be dropped, only moved.
- **A/B harness already exists**: `blit_cfg[]` + pad L+R (`blit_mode`), `b` (row 1/2),
  row-11 `DF pre/blit/post`. Per [the inter-build-noise lesson], every increment below is
  a **new `blit_cfg` entry judged live in-session**, never build-vs-build.

## 1. Increment plan (each = one `blit_cfg` entry, live A/B)

### Inc 1 — synchronous DMA blit — **HW RESULT 2026-07-02: NO WIN (blit is B-bus-bound)**
> Pad L+A flips the letter `c`→`d` (DMA path confirmed running, image updates) but the `b` ms
> and MST/fps DO NOT change. The SCU DMA writes VDP2 VRAM over the same B-bus port at the same
> bandwidth as the CPU longword memcpy, and `slDMAWait` blocks the master for that same transfer
> time. So **synchronous DMA cannot beat the CPU blit** — the blit is write-bandwidth-bound, not
> CPU-bound (consistent with the "bus-bound S~1.3 / dual-CPU never wins" finding). The lever is
> real only ASYNC (Inc 2): overlap the ~11 ms transfer with the game tic. Inc 1 stays in as the
> L+A `c`/`d` toggle + the proven-working DMA primitive that Inc 2 builds on.

### Inc 1 — synchronous DMA blit (the wolf4sdl clone) — LOW RISK — **BUILT 2026-07-02, HW-PENDING**
`blit_cfg[1]` (`dma=1`): kick `slDMACopy(fb+y*320, VRAM+y*512, 320)` for y=0..223, then
`slDMAWait()`, then the existing clear. Byte-identical pixels. No `cache_purge` (fb is
write-through). **Live A/B = pad L+A** — a clean 2-state flip single-CPU(0) ↔ DMA(1); row-1
shows `b<ms><c/d>` (c=CPU one press → d=DMA). NOT L+R (= debug overlay). **Boot stays
single-CPU** (blit_mode 0) so an unvalidated DMA hang can never brick boot — opt in with L+A.
Implementation: [dg_saturn.cxx](../src/dg_saturn.cxx) blit_cfg table + the
`blit_cfg[blit_mode].dma` branch in DG_DrawFrame + the L+A handler in poll_pad.
- **The dead dual-CPU configs are REMOVED from the cycle** (2026-07-02): they were
  measured-dead AND the first version cycled through them (5 presses to reach DMA — read as
  "L+A doesn't fire"), and dispatching the blit to the slave (`slSlaveFunc`) can collide with
  the evolved slave pipeline (F-build/plane-split) → a wedge (a candidate for the reported
  cycle-freeze). Dual code stays behind `DUAL_CPU_BLIT` for revival but no cfg selects it.
- **Verify on HW**: image intact, `b` value, **no RBG0 snow** (DMA bursts share the B-bus
  with the VDP2 fetch — watch the floor at pot0 1p), **`Dr` unchanged** (VDP1 also lives
  on the B-bus), consecutive-kick semantics (224 back-to-back `slDMACopy` — wolf4sdl
  proves it, but confirm no dropped rows). If L+A → `d` FREEZES, that is the datapoint that
  `slDMACopy` also hangs the bus like the raw path did (reset; boot-safe default protects).
- **Expected**: b 5.5–11 → **~2–4 ms** (DMA 32-B bursts beat byte-wise CPU on B-bus).

### Inc 2 — asynchronous blit (the real prize) — MEDIUM RISK
`cfg 6`: kick the 224 rows at the end of `DG_DrawFrame` and **return without waiting**;
the DMA overlaps the game tic (`nr` = 27–50 ms — far longer than the transfer). Fence =
`slDMAWait()` **before the first framebuffer write of the next frame** (ST_Drawer /
EX both write fb inside core `D_Display` *before* the next `DG_DrawFrame`), so export a
core hook: `void (*sat_frame_fence)(void)` called at the top of `D_Display`
(core-owned pointer, NULL default → DoomJo unaffected; same pattern as
`sat_build_local_ticcmd`). The fence does `slDMAWait()` + the clear.
- **Hazards**: single-buffered fb (next frame must not write before the fence — that is
  exactly what the fence guarantees); SGL/SRL internal users of DMA L0 (CD? sound?) —
  check `slDMAStatus` collisions.
- **Expected**: master blit cost → **~1 ms of kicks**; net **−4…−9 ms of MST**.

### Inc 3 — get the clear off the master (M4)
Once Inc 2 lands the clear sits in the fence (still master ms). Options, in order:
1. **Slave clear**: fence does `slDMAWait` then `slSlaveFunc(clear)`; master continues.
   Clear must complete before the EX phase writes fb — naturally satisfied (REC runs
   first, 30+ ms), but add a done-flag check before the first plane/parity dispatch.
2. **DMA fill** (read-add 0 = hardware memset) needs raw register config that
   `slDMACopy` does not expose → **only** if (1) disappoints, and then via a minimal,
   SGL-shadow-respecting poke — the raw history commands caution.
- **Expected**: **−2…−3 ms** more.

### Inc 4 — W5, blit only live rows — **HW-CONFIRMED 2026-07-05: −1.3…−1.5 ms 1p idle**
> Row-1 `b` (windowed mean, 1p standstill E1M1): c-=10.0, c5=8.7 (**−1.3**), d-=10.3, d5=8.8
> (**−1.5** vs d-). The W5 delta = **14 % = the 32 HUD rows [192,224)/224 exactly** — not noise.
> `sat_hud_dirty` works (ammo 47/50/49/48 tracked live). **Caveat**: the test spot was facing-wall
> (the décrochage spot), which is PRE-stall-bound (~25 ms, `mx72` vs `MST98`), so the −1.3 ms is
> swamped and MST/fps do NOT move there (98/100/100/98 doesn't track `b`). `b` is the clean signal;
> to see W5 convert to fps, measure at a standstill in an **open** scene (no PRE stall). **c5 is the
> winner** (pure upside: −1.3 ms idle / 0 combat); DMA (`d`) confirmed no-win (+0.3 ms), park it.

The blit loop is split at `hud_top` (= the clear boundary: 192 1p / 160 2p / 224 3-4p): the
3D-view rows `[0,hud_top)` always blit; the HUD band `[hud_top,224)` blits only when it
changed. 3/4p (`hud_top=224`) is a no-op (interspersed bands + minimap change every frame).
- **Change detection**: core `sat_hud_dirty` (st_stuff.c), set by the STlib widgets — this
  needed the **missing diff added to `STlib_drawNum`** (the vanilla `oldnum` field was set but
  never tested, so numbers redrew every frame; now they redraw only on change → the HUD
  framebuffer goes static when idle). 2p uses `ST_SplitHudSig()` (a value signature over both
  players' health/armor/ammo/weapons/keys + damagecount/bonuscount) to also skip the panel
  **repaint** when idle. Core changes are shared with DoomJo (benign: fewer redraws, identical
  pixels; refresh still forces a full draw).
- **W5 is a RUNTIME axis of `blit_cfg`** (the `w5` field), not a compile flag — folded into the
  **pad L+A** cycle so path × W5 = 4 combos: `0 c-` (CPU) → `1 c5` (CPU+W5) → `2 d-` (DMA) →
  `3 d5` (DMA+W5). Row-1 `b<ms><c/d><-/5>` names the state. Boot = `c-` (baseline). Forced full
  HUD blit when `menuactive`, `gamestate != GS_LEVEL`, or the player count changes.
- **Expected**: −14 % of the transfer **on idle frames only** (~1.5 ms 1p, ~3 ms 2p when the
  HUD is static); 0 during combat (HUD changes every frame). Orthogonal to the sprite-VDP1
  deport (that offloads the render `M` phase; W5 is the blit).
- **Verify on HW**: HUD not stale/frozen (watch the face blink ~every 2 s → a momentary HUD
  blit), `b` drops when standing still with `c5`/`d5`, menus/intermission still refresh the HUD.

## 2. Order & exit criteria

Inc 1 → Inc 2 → Inc 3 → Inc 4, one HW session each, 2-spot protocol + pad L+R A/B.
- **Adopt** an increment iff: pixels correct + no snow + `Dr` not degraded + `b`/`MST`
  better with the toggle ON in-session.
- **Abort to previous cfg** on any HW hang (the F00001 history), snow, or torn menus
  (the STEP-4a tearing symptom = fence bug).
- Cumulative target: **MST −6…−12 ms** ⇒ pot0 ~8 → **~9–9.5 fps**, pot2 13.4 → **~15**.

## 2b. Measuring small blit deltas (row 5)

The row-1 `b` is a 1-second window average in **integer ms** — it rounds away the ~1.5 ms W5
delta. **Overlay row 5** `BLT <path><w5> <mean.hundredths> n<samples>` is the precise A/B: it
accumulates the per-frame FRT blit time (`sat_blit_ms10`, tenths) since the last L+A toggle and
shows the **mean in hundredths-ms** + the sample count (capped 4096). Protocol: stand still (HUD
idle — not firing / no damage), let `n` climb a few hundred, read the mean; toggle L+A (resets
`n`) and compare across `c-`/`c5`/`d-`/`d5`. The blit is scene-independent (fixed 224 rows,
bus-bound), so the mean is ~constant across the map — the W5 saving (~1.5 ms 1p) is the same
everywhere; measure anywhere at a standstill.

## 3. Notes

- Ymir does not model SCU-DMA timing faithfully — numbers are HW-only (measurement law).
- The retired M5 lesson applies: judge ONLY via the live toggle, never across builds.
- If Inc 1 hangs like the raw path did (it should not — SGL manages the channel), the
  fallback is the parked idea list: dual-CPU blit variants are already measured DEAD;
  W5 alone (~14 %) is then the only remaining blit lever.
