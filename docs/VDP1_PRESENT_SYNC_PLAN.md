# VDP1 / NBG1 Present-Sync — Revised Plan (supersedes the draw-gated manual-present plan)

> Written 2026-06-29 after a multi-agent investigation (present timeline/handshake, SlaveDriver
> reference, the two shelved branches diffed, FBCR/EDSR/B-bus HW semantics → 6 sync strategies
> designed & adversarially verified → synthesis). Companion to [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md).
> Supersedes the assumption behind `vdp1-draw-gated-present` @3aecc84 and `vdp1-full-slsynch` @53a6652.

> **SCOPE.** This plan is about **one thing: presenting the VDP1 walls in lockstep with the
> VDP2/NBG1 content of the same frame.** The facing-a-wall **fps cliff is OUT of scope** — it is a
> **VDP2 cost** (the per-frame RBG0 transform / SGL register work in `DG_DrawFrame`'s PRE phase),
> being analysed separately; it is **not a VDP1 problem** and no lever in this doc touches it. Any
> earlier "close-wall = B-bus contention with VDP1 fill" framing is dropped.

> **Headline verdict.** True VDP1↔NBG1 lockstep at **zero fps cost is impossible** while NBG1 is a
> live mono-buffer and the HW floor+sky are kept (the clean fix — double-buffer NBG1 — is VRAM-blocked).
> The décrochage is **intrinsic**: both shelved branches failed with the *same* symptom from opposite
> directions, proving it is VDP1's swap-at-vblank latency vs NBG1's 0-latency live buffer, not the
> swap mechanism. So the real menu is: **(A)** a correctly bug-fixed gated present that kills tearing
> but still lags by ~1 vblank (free, partial sync), or **(B)** a coupled deterministic present that
> achieves true lockstep at a measured **fps-cap** cost (opt-in). *Sync-max, ~0-fps, keep-HW-floor —
> pick two.*

## 1. Why the old draw-gated plan was wrong/incomplete

Read against current `src/dg_saturn.cxx`:

1. **Asymmetric gate (the capital error).** `vdp1_vblank_present` ([dg_saturn.cxx:2814-2821](../src/dg_saturn.cxx))
   swaps VDP1 on CEF, but the NBG1 blit ([:3279-3301](../src/dg_saturn.cxx)) is an **unfenced live
   memcpy** into `DOOM_VRAM` — no page-flip, no vblank gate. NBG1 frame-N is visible at the next
   scanout the instant the memcpy lands; VDP1 frame-N presents 1-2 vblanks later. The gate cured
   tearing and **revealed décrochage** (walls lag floor) — the inverse error. It gated one surface
   and did nothing to the other. **This is the root reason both shelved branches failed.**
2. **Sticky-CEF, unproven.** `VDP1_EDSR & 0x0002` ([:2816](../src/dg_saturn.cxx)) is sticky from the
   *previous* plot; `vdp1_prev_done` is sampled **before** the new PTMR ([:3001](../src/dg_saturn.cxx),
   comment "did the PREVIOUS frame's plot finish?"). No proof the latched CEF belongs to **this** bank.
3. **Swap parity not locked to the bank flip.** The command-bank flip (`vdp1_bank=vdp1_wbank` [:3015](../src/dg_saturn.cxx)
   + atomic root-LINK [:3022](../src/dg_saturn.cxx)) and the FBCR swap (in the ISR) are decoupled →
   completed walls can land in the buffer the next FBCR change hides = **walls-vanish** at certain angles.
4. **Watchdog ABSENT from master.** `vdp1_vblank_present` has **no** STUCK counter / force-swap. On
   Ymir (no manual-mode CEF latch) the gated path shows **zero** walls; on HW a pathological stall
   freezes walls. `VDP1_PRESENT_STUCK_MAX` lives only on the shelved branch.
5. **Ymir-untestable** for the load-bearing path. Master ships `VDP1_MANUAL_CHANGE=0` (tears) so walls
   appear on Ymir at all → the sync gates are HW-only.

## 2. The core constraint (why "free full sync" doesn't exist)

- **NBG1 is a live single buffer** (`DOOM_VRAM`, no page-flip): the dual-CPU memcpy makes frame-N
  visible at the next scanout, **0 present latency**. There is no knob to delay it without a double-buffer.
- **VDP1 swaps at vblank** and finishes its wall list within one field only **Dr 30-60%** of frames on
  HW (phase/latency, persists even at trivial fill F31 → Dr 45%; it is **not** workload-bound, so
  "shrink the list" can't reach Dr~100%).
- **The clean fix is VRAM-blocked.** Double-buffering NBG1 so the floor presents on the same vblank as
  the VDP1 swap needs a free 128KB VDP2 bank (MPOFN base granularity = whole bank; 224×512 = 114KB >
  64KB half-bank). All 4 banks are spent: A0 sky / A1 RBG0 floor+rotation anchors / B0 NBG1 / B1 map+cell-sky.
- **slSynch is a net loss for this problem.** It syncs VDP2 only (the already-OK layer), costs ~16% fps
  (vblank cap) + mutes SCSP SFX (owns MVOL), and **still** leaves the ~1-frame VDP1 décrochage (proven
  on `vdp1-full-slsynch` @53a6652 — the blit is never fenced into the Synchronize window).

## 3. Ranking of the present-sync options (verified sync/perf, 0-10; perf 10 = free)

1. **D-INC-1 — corrected gated present** (CEF-proven + parity + watchdog). sync 3 / **perf 7**. A real
   **bug fix** (kills walls-vanish + stops tearing); still décroches ~1 vblank. **The foundation — build first.**
2. **Coupled deterministic present** (cadence counter + NBG1 blit fenced to the pre-present vblank).
   sync ~7 / perf 3. The **only true lockstep**; costs a deterministic fps cap. **Opt-in pad-toggle.**
3. **SCSP-MVOL re-assert** — independent freebie, restores CDDA SFX, slSynch-free.
- **DEAD** (do not re-propose without defeating the reason): full-slSynch default; defer-PTMR; naive
  NBG1 mirror/1-frame-delay ("largement pire"); SCU DMA blit (B-bus hang); the dual-present /
  same-bank page-flip (VRAM-blocked); any CEF-gate that leaves the NBG1 blit unfenced (= 3aecc84
  décrochage by construction).

## 4. The plan

### A — D-INC-1: corrected manual-change present (the bug-fix foundation)
Implement the gated present **correctly**, so it stops tearing and never loses walls. It will still
décroche ~1 vblank (NBG1 not coupled) — that is expected; A/B on HW whether *tear-free-but-lagging*
already reads better than *torn*.
- **(a) CEF-proven-this-frame.** After `VDP1_PTMR=0x0002` ([:3023](../src/dg_saturn.cxx)), bounded spin
  `while((VDP1_EDSR&0x0002)&&g<2000)g++;` to drain the stale CEF so a later CEF=1 belongs to **this** plot.
- **(b) Parity locked to the bank flip.** Shared `present_gen` stamped by the kick ([:2998](../src/dg_saturn.cxx))
  and read by the ISR; swap only when `present_pending` was armed by **this** generation (strict
  serialization — no hardware buffer-id read needed).
- **(c) Watchdog `VDP1_PRESENT_STUCK_MAX`** (re-introduce; **absent** in master): force `FBCR=0x0003`
  after N vblanks pending → walls never freeze on Ymir/stall.
- **(d) In-list colour-0 erase polygon** (already understood; keep OFF by default for the TLSF budget,
  enable only with the gated present, since manual change's HW back-buffer erase still fires here — but
  verify on HW that walls don't accumulate; cf [[vdp1-erase-under-slsynch]]).

Pad-toggle this against 1-cycle-auto for the HW A/B. This kills the walls-vanish bug (§1.2-1.4) for good.

### B — coupled deterministic present (the only true lockstep — opt-in, fps-cap)
The move neither shelved branch tried: **gate both surfaces to the same vblank.**
- Present on a **deterministic cadence counter** in `vblank_handler` (NOT on CEF) → Ymir-testable, parity-safe.
- **Couple the NBG1 blit:** hold the finished software framebuffer and run the dual-CPU memcpy **only in
  the single vblank before the present-vblank**, so NBG1-N and VDP1-N go live together. This is *not* the
  rejected 1-frame mirror — no new buffer, the existing live buffer is just *timed*.
- Honest cost: a deterministic 20/15fps cap, tear-free AND décrochage-free, with working SFX. A
  low-fixed-fps **product decision** — ship behind pad-C only if HW A/B says stable lockstep > variable tear.

### C — SCSP-MVOL re-assert (independent freebie)
Re-assert the SCSP master volume once per frame in `vblank_handler` (already runs every frame for CRAM
upload): 1 halfword/frame, restores sustained SFX faster than SGL's re-zero. CDDA-only concern; MUS halts
the 68K and is immune. Land regardless of present strategy. Validate SGL's CDDA driver doesn't re-zero
MVOL more than once per frame first (else brief mutes survive).

### Build order
**A (bug-fix foundation) → measure tear-free-vs-décroche on HW → if the décrochage is unacceptable, add
B behind a toggle → C lands any time.** Do not default-ship a gated present that still décroches until the
HW A/B says it beats the current torn auto-present.

## 5. Increments with GO/NO-GO (the sync gates are HW-only — Ymir can't validate manual-mode CEF)

- **INC-0 baseline** (HW): master as-is (1-cycle auto). Capture Dr (row 2), tearing, décrochage feel
  (strafe past a wall). Establishes the "what we're beating" reference.
- **INC-1 D-INC-1** (HW for sync, Ymir for boot/no-vanish-via-watchdog): the corrected gated present.
  **GO** if walls NEVER vanish at any angle (parity OK), NEVER freeze (watchdog OK on Ymir/stall),
  tearing gone, no fps regression (the drain-spin isn't burning budget). Décrochage still present here —
  **not** a fail; it proves the machinery is correct.
- **INC-2 the verdict A/B** (HW): pad-toggle 1-cycle-auto ↔ D-INC-1. **GO to default-ship D-INC-1** if
  tear-free-but-lagging reads better than torn to Romain's eye. **NO-GO** → keep it a toggle, go to INC-3.
- **INC-3 coupled deterministic present** (HW for sync, Ymir for function): pad-C A/B
  (auto / D-INC-1 / coupled P=2 or 3). **GO** if décrochage visibly gone (walls+floor move together when
  strafing) AND tear gone AND fps ≥18 in the regimes that matter AND Romain judges the stable cap > the
  variable tear. **NO-GO** → shelve coupled (documented), ship D-INC-1 (+ MVOL) only.
- **INC-4 boot budget** (before any handoff): `build/Mimas.map` pool (`_end..__heap_end`) ≥ ~7KB
  (floor ~7040B). present_gen/cadence counters are <32B; the in-list erase polygon tipped the pool in
  3aecc84 → keep OFF unless the gated present needs it, then re-budget. **NO-GO** → reclaim via WALL_ACC_MAX.

## 6. Kill-list (do not re-propose without defeating the stated reason)

- **CEF-gated present that leaves NBG1 unfenced** (the original draw-gated, any "tighter window"):
  reproduces 3aecc84 décrochage by construction. Any scheme that gates VDP1 MUST couple NBG1 on the
  same vblank or do nothing.
- **FULL slSynch by default** (= shelved 53a6652): blit not fenced → décrochage moves not removed; fps
  cap + muted SFX. Resurrect only the MVOL fix, separately.
- **Defer-the-PTMR / defer-the-kick**: at 8-11 vblanks/Tick there is no sub-field phase to recover;
  deferring PTMR inverts the kick-before-blit order AND blanks the walls (auto erase+swap each vblank
  with no re-draw).
- **"Clean" NBG1 double-buffer / same-bank page-flip**: VRAM-blocked (MPOFN granularity = whole 128KB
  bank). Confirmed; do not re-raise.
- **Naive NBG1 mirror / 1-frame delay**: HW-tested "largement pire" (open-loop; shifts the phase without
  coupling). Never re-add blindly.
- **SCU DMA blit (even VBLANK-IN factor)**: hangs the B-bus on HW (`USE_SCU_DMA=0` mandatory); VBLANK-IN
  also re-imposes a vblank cap = the slSynch tax.

## 7. Open hardware questions (gate the riskier increments)

- Does `PTMR` clear `EDSR.CEF` *observably* before the drain-spin's first read on silicon? If the clear
  has latency, every gated frame pays the spin (hits D-INC-1's perf). Listed, unverified.
- ISR ordering: where does `SRL::Core::OnVblank` sit vs SGL's own vblank ISR (`_BlankIn`, which re-pushes
  the register shadow each frame, [dg_saturn.cxx:1579](../src/dg_saturn.cxx),[:1801](../src/dg_saturn.cxx))?
  Does an FBCR poke in OnVblank survive to scanout or get clobbered? Load-bearing for the coupled present;
  untestable on Ymir.
- Coupled present: at the open-hall render cost (7-8 vblanks of CPU work), what cadence period P does the
  blit-fence actually impose? If P≥4 everywhere, coupled present falls below 15fps = ship-killer. INC-0
  must measure max(VDP1 plot, CPU render+blit) in vblank units to confirm a viable P exists.
- **SCSP-MVOL**: does SGL's CDDA driver re-zero MVOL more than once per frame (in its own sub-frame ISR)?
  If so the per-frame re-assert leaves brief mutes. Independent inc to validate before a CDDA build.

## 8. What this supersedes

- **`vdp1-draw-gated-present` @3aecc84**: its gate-VDP1-only mechanism is **superseded** — décrochage is
  intrinsic to the unfenced NBG1 blit, not fixable by gating VDP1 alone. Its reusable machinery (watchdog,
  stale-CEF spin, don't-clobber-shared-texture rule, in-list erase) is folded into **D-INC-1** as a bug
  fix, not a sync scheme.
- **`vdp1-full-slsynch` @53a6652**: **superseded as default** — the blit is never fenced into the
  Synchronize window so the décrochage merely moves, at a fps-cap + (unfixed) SFX-mute cost. Only its
  MVOL fix is salvaged, independently.
- The **VRAM-blocked clean fix** (double-buffer NBG1) and the **naive 1-frame mirror** ("largement pire")
  stay **killed**.

## Reconciliation note
The honest verdict the analysis forces: **"sync max at ~0 fps with HW floor+sky kept" is unreachable** by
any present trick alone, because NBG1 is a live mono-buffer and its double-buffer is VRAM-blocked. We ship
the **bug-fixed gated present (D-INC-1)** + the **MVOL freebie** as real improvements, and the **true
lockstep (coupled deterministic present)** only as an opt-in pad-toggle whose cost (a deterministic fps
cap) is measured and accepted, not hidden. The facing-wall fps cliff is a **separate VDP2 cost** (PRE-phase
RBG0 transform) handled elsewhere — out of scope here.
