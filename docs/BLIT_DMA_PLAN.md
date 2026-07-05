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

### Inc 1 — synchronous DMA blit (the wolf4sdl clone) — LOW RISK
`cfg 5`: kick `slDMACopy(fb+y*320, VRAM+y*512, 320)` for y=0..223, then `slDMAWait()`,
then the existing clear. Byte-identical pixels.
- **Verify on HW**: image intact, `b` value, **no RBG0 snow** (DMA bursts share the B-bus
  with the VDP2 fetch — watch the floor at pot0 1p), **`Dr` unchanged** (VDP1 also lives
  on the B-bus), consecutive-kick semantics (224 back-to-back `slDMACopy` — wolf4sdl
  proves it, but confirm no dropped rows).
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

### Inc 4 — W5, blit only live rows — TRIVIAL after Inc 1
Parameterize the row loop: rows 0–191 always; rows 192–223 (1p status bar) only when the
HUD changed (`sat_hud_dirty` set from `ST_Drawer` — one-line core hook). 2p: rows 160+.
- **Expected**: −14 % of the remaining transfer.

## 2. Order & exit criteria

Inc 1 → Inc 2 → Inc 3 → Inc 4, one HW session each, 2-spot protocol + pad L+R A/B.
- **Adopt** an increment iff: pixels correct + no snow + `Dr` not degraded + `b`/`MST`
  better with the toggle ON in-session.
- **Abort to previous cfg** on any HW hang (the F00001 history), snow, or torn menus
  (the STEP-4a tearing symptom = fence bug).
- Cumulative target: **MST −6…−12 ms** ⇒ pot0 ~8 → **~9–9.5 fps**, pot2 13.4 → **~15**.

## 3. Notes

- Ymir does not model SCU-DMA timing faithfully — numbers are HW-only (measurement law).
- The retired M5 lesson applies: judge ONLY via the live toggle, never across builds.
- If Inc 1 hangs like the raw path did (it should not — SGL manages the channel), the
  fallback is the parked idea list: dual-CPU blit variants are already measured DEAD;
  W5 alone (~14 %) is then the only remaining blit lever.
