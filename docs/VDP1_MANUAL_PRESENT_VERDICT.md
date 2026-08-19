# VDP1 Manual Present + Motion Holes — Verdict 2026-08-18

> **Supersedes [`VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md) (2026-06-29).** Produced by
> two multi-agent audits (12 + 4 agents) that challenged the memory corpus, the current code, the
> Kronos-corrected ST-013 manual, the SlaveDriver source, a full LIBSGL.A disassembly, and the git
> history of both shelved branches. Every load-bearing claim below carries its provenance tag.
> Trigger: the owner's theory — *"fps should be capped, VDP1 should present in sync with CPU
> emission, allowing more sprite emission; we should not have VDP1 switching its framebuffers after
> every vblank"* — plus the standing instruction to challenge the memories (every past attempt
> failed, so they must contain errors). They did; see §F.

## A. Headline: the owner's theory is CONFIRMED on all three verification lenses

1. **It is the manual's own documented remedy.** Kronos-red p.38: *"The number of characters that
   can be drawn in one **field** is limited. Therefore, in order to draw more characters, **the
   manual mode must be set**."* Table 4.3(a) demonstrates a 20 fps manual cadence end-to-end.
   [manual-RED]
2. **It is the shipped commercial contract.** SlaveDriver (PowerSlave) runs manual FCM/FCT in-game
   (`SCL_SetFrameInterval(0xfffe)`, SRUINS.C:1882): plot window = whole game frame, adaptive 1-2
   field cadence with 10-frame hysteresis, 1448 cmds/frame with zero tail-cut risk, erase = VBE
   vblank erase (top ~110 lines) + permanent slot-0 black polygon (lines 110-240). [code-verified]
3. **It is SGL's own native mode.** `SynchConst=N≥2` drives `_BlankOut` to pulse FBCR=2 (erase)
   then FBCR=3 (change) with PTMR=2 ⇒ an N-field plot window. [disasm]

**Why every past failure happened — none of them tested the idea:**

| Poison | Where it bit |
|---|---|
| **CEF gate** (latches 30-60 % in 1-cycle on HW, 0 % on Ymir manual) | 3aecc84 walls-lag; df5b82b revert; parked `vdp1_couple_nbg1`; the Dr%-based verdicts |
| **Unfenced blit** | both shelved branches — the fence (`sat_field_fence`) post-dates every present experiment by a month; *manual change + fenced blit + non-CEF gate have never coexisted in one build* |
| **Ymir verdicts** (no manual CEF, no LOPR, never overruns) | first `MANUAL_CHANGE=1` test "NO walls"; every present verdict formed on Ymir is void |
| **The killer erratum** | uncorrected manual: manual erase = FBCR **0x0000** = silently 1-cycle. Corrected (red): **0x0002**. Any implementation from the DTS CD did nothing |
| **slSynch re-asserts the swap** | mechanism named: `_BlankOut` gated on `DMASetFlag`, set only by the slSynch frame-close (sglJ03.o) — 53a6652's pokes were overridden |

The 2026-06-29 **"décrochage is intrinsic"** verdict rested on `Dr%`/CEF — retired as a dead metric
— and its "shrink can't help" leg was directly refuted by the LOPR-era LOD success [HW 07-26]. Its
"no knob for the blit" leg died when `sat_field_lock` was built (08-02; **parked 08-03**, one-line
re-enable at dg:1915). Its "~16 % fps-cap tax" was an `[est]` never measured, priced on 17-36 ms
frames; at today's 62-143 ms (4-9 fields) the quantization cost is ≤1 field, ~0 on most frames.

**Settled open question:** in Mimas's `SRL_FRAMERATE=0` config, SGL's `_BlankOut` **never writes
FBCR** (`DMASetFlag`'s single writer is the never-run slSynch pipeline — exhaustive-grep disasm of
LIBSGL.A). An OnVblank FBCR poke survives. ⚠️ If anyone ever ships `SRL_FRAMERATE≥1`, SGL writes
FBCR=0 every vblank-out and fights the driver.

## B. The register contract (Kronos-corrected — the recipe any build must follow)

- **Encodings**: 1-cycle = FBCR 0x0000; manual **erase** = **0x0002**; manual **change** = 0x0003.
  VBE is TVMR bit 3 (rewrite TVM with the same value in the same write). [manual]
- **Choreography**: **erase then change** — 0x0002 in the field before, 0x0003 next; erase targets
  the **displayed** buffer during its own display (red p.46 — same op 1-cycle does every field,
  invisible); "Be sure to continue to specify erase then change" (red p.41). Never VBE full-screen
  erase: NTSC vblank budget covers ~82 % of 320×224@16bpp (~10× short by px-units), residue must be
  polygon-filled — SlaveDriver's split exists precisely for this.
- **Write windows (verifier F4 — the trap our first design hit)**: FCM/FCT writes belong
  *immediately after V-blank OUT*; vblank-IN is inside the prohibited span for VBE=0 writes. SBL's
  shipped split: **erase pulse at vblank-IN** (precedent-backed), **change pulse at vblank-OUT**
  (gated). Put the change in a vblank-OUT hook, not in SRL's OnVblank (vblank-IN).
- **Requests are one-field pulses** — no FBCR write ⇒ no action that field; idle fields write
  nothing. FCM is sticky once manual mode is entered.
- **PTM**: keep PTMR=2 — plot auto-starts at each CPU-granted swap and is never restarted in
  between ⇒ spans N fields by construction. (PTM=1 mid-plot restarts from the table top — legal but
  abandons the frame; count it if used.)
- **Completion gate**: **COPR == staged end-command address** — never CEF/BEF (p.52 race caveat +
  HW latch data). Traps: COPR *parks* (a "motion" qualifier can never fire); the parked value
  measured on HW in the old topology was `0xC` not the naïve `0x8` — stage the address, don't
  hardcode; sample only ≥1 field after the kick (stale-park race); Ymir models neither COPR nor
  LOPR ⇒ a short deterministic watchdog (≈P+2 fields) is the Ymir path. ENDR (forced termination,
  ~30 clk, no resume) backs the hard-stall watchdog.
- **Erase order**: pulse erase only **after plot-done** — erasing at a fixed field with the plot
  still running displays an erased front buffer (black flash) for 1-3 fields under exactly the
  heavy scenes the mechanism exists for.
- **Revert path** (A/B toggle back to 1-cycle): specify erase in the field immediately before
  re-entering auto (Table 4.3(a) note 5).
- **Metric shift**: once the present is gated, `LP%=100` is *tautological* (LOPR latches at the
  change we only grant on completion). The live overrun metric becomes **watchdog-fire rate** +
  the fps distribution.

## C. The build plan (hybrid of the three verified hypotheses)

**Session 0 — before building anything (budget before mechanism, zero code, console):** capture on
the worst scenes (E1M1 outdoor, dense tech room, TNT MAP11): row 17 `c B LP% ec W<res>/<cuts>`, row
12 `fbw`, row 1 `pr`, MST/fps distribution. **Go/no-go: LP<100 on a meaningful fraction (~≥20 %) of
frames in artifact scenes.** If LP=100 nearly everywhere, the one-field guillotine is not binding —
the sprite ceiling is upstream (see below) and the manual present is only worth it for the pairing
windows of §D.

**Step 1 — emission-side fixes (ship regardless):** move the LOPR meter + budget-cap application
out of `#if SHOW_FPS` (declarations too, dg:1674-1709; latent today since SHOW_FPS is hardcoded 1);
soften the `room≤0 ⇒ emit_cap=0` snap (floor of 2 near-ranked things); raise the upstream sprite
floors — core `r_things.c:1696-1709` area floors + grant cap 4, with `THING_EMIT_MAX` /
`THING_ADAPT_MAX` moved in lockstep (**core change → flag DoomJo**). Without this the widened plot
window converts to nothing; note the CPU (T165 > R141 on console) may become the sprite binder —
judge `ec` gains on console only.

**Step 2 — the manual present:** ISR state machine (erase@IN after plot-done, change@OUT-hook),
COPR end-address gate + generation stamp, adaptive cadence P∈{2..4} with SlaveDriver hysteresis,
watchdog + ENDR, `sat_field_lock=1` with the fence retargeted to the present field (four-clocks
rule: VDP2 registers before it, VDP2 VRAM after), **wtex 3-state lock re-keyed to present
generation** (aging by flush count assumes a 1-frame plot horizon), in-list colour-0 erase polygon
retained as the watchdog-path fallback (slot accounting: LOPR/COPR base math must include it),
delete/disable the parked CEF `vdp1_couple_nbg1` brick (would fight the fence), A/B pad chord
manual↔auto with the legal revert. TLSF pre-flight mandatory: pool ≈ 8.9 KB at last map.

**Acceptance (console):** watchdog-fire rate ≈0 in the LP<100 scenes; `W` cuts 0; `fbw` 0; `ec` ≥2×
session-0 median in hordes (with step-1 floors raised); fps within 1 field of free-running; 360°
turn shows no ghost walls (erase proof); 5-minute soak without accumulation. Motion holes unchanged
is **not** a failure — see §D.

> **AS-BUILT 2026-08-19 (build green, pool 19.34 KB).** The step-2 driver shipped as `sat_mp_*` in
> `src/dg_saturn.cxx`: pad **L+B** A/B (boot = AUTO = ship; toggle-ON also forces the lead-fill OFF,
> per the owner's goal of retiring it), kick → PTMR=1 + staged end address, in-list colour-0 erase
> polygon (+ a menu-path erase mini-list), frame-end fence with the COPR gate (staged-end ‖
> COPR-frozen-since-kick as the Ymir clause ‖ 4-field watchdog counted in `w`), the 0x0003 pulse
> written immediately after a TVSTAT vblank-OUT edge (the p.38 window), riding to the swap vblank =
> the blit's Fl1 edge, and the legal erase-then-auto revert. Overlay row 8: `MP<on><act> w<wd>
> <ms>ms`. Known transition artifact: ~1 frame of wall garbage on toggle-ON. The TLSF pre-flight was
> unblocked by `BACKUPTICS 128→32` (core/net_defs.h, ~15 KB of dead netplay ticcmd ring — DoomJo
> flag, behaviour-neutral locally). Step 1 (emission-side fixes, sprite floors) NOT built yet.
> HW/Ymir validation pending; success metric = `L0-` + continuous turning with no holes + stable `w`.

> **POST-CAPTURE v2 2026-08-19 (the v1 Ymir session + the one-field divergence).** The owner's L+B
> captures showed the mechanics working (`MP11`, `w0`, wait 14–26 ms, ~15.5 fps vs ~18.5 auto) but
> the holes SURVIVED — intermittently ("seulement certaines frames"), plus one frame with the same
> door texture twice (CPU partial + VDP1 complete, offset). Three-way verification resolved it:
> **(1) emission audit** — no code path emits a command from frame N−1 coordinates; every reachable
> command is rebuilt per frame, the erase polygon covers the full 224 lines; all stale pixels are
> presentation-layer. **(2) Ymir source** (`vdp.cpp BeginHPhaseLeftBorder`): a manual change written
> in the one-line window after the vblank-OUT edge executes at the END of that same line — the new
> list displays from the field that starts ~63 µs after the write. **(3) corrected ST-013** (Table
> 4.3(a) + p.39): the same write swaps at the **next** field boundary, one field later. The two
> contracts diverge by exactly one field, so v1 (write at OUT, ride one field, then blit) was
> HW-correct but one field EARLY on Ymir: each frame showed {picture N−1, list N} for one field out
> of ~4 → intermittent holes with the walls AHEAD, and the double door = the frame where that wall
> had just migrated VDP1→CPU (budget reject at `LP100%`: CPU drew it fresh in picture N−1, the new
> list carried the VDP1 quad at position N). **v2 fix**: swap via **VBE erase & change** (p.40, the
> sequence SlaveDriver ships as SCL `0xfffe`): TVMR.VBE=1 + FBCR=0x0003 at a fresh vblank-IN, VBE
> cleared after OUT — the swap lands at the END of that same vblank on BOTH machines (Ymir's
> LastLine-end eval ≡ the manual's "at the end of V-blank"). The VBE partial erase (~×10 NTSC
> deficit) is harmless — the colour-0 polygon owns the erase. The deferred sky-map write moved
> inside that vblank. Bonus: the armed field v1 burned is gone — fence wait drops from 14–26 ms to
> ~2–18 (avg ~10), i.e. manual present now costs ≈ Fl1. Shared fact recorded in
> `../saturn-refs/knowledge/HW_VDP1.md` §5.

> **✅ VALIDATED → DEFAULT 2026-08-19 (owner: "NAILED IT!").** Second capture session, v2 build:
> 20.0–24.2 fps vs 18.4–19.4 (auto + lead-fill) — **+1.5 to +5 fps net**, `w0` (zero force-swaps),
> fence 13–15 ms (inside the predicted 2–18), MST 41–50 vs 50–55, `L0s/0`, and the motion holes are
> **gone** — the artifact that predated M7 and survived every present experiment since 2026-06-17 is
> closed at the source. Consequences shipped the same day: the manual present is **unconditional**
> (the L+B toggle, the `sat_mp_on/revert/saved-lead` state and the two-pulse AUTO revert removed —
> the AUTO 1-cycle present is retired), the **lead-fill is PARKED** (core `r_segs.c` boot default
> `sat_wall_lead_x = 0`, the R+Right cycle chord removed, mechanism intact and dormant), the parked
> field-lock is doubly-superseded (its call-site branch removed), row 8 reads `MP<act> w<wd> <ms>ms`.
> Both freed pad slots: **L+B and R+Right**. Caveats that remain open: not yet re-validated on
> console (SlaveDriver's shipped VBE sequence covers the HW side on paper; if console ever shows
> holes here, suspect the change-sample timing first — sustained `<ms>` of 14–26+ is the tell);
> split-screen runs the same per-frame present but was never A/B'd under it; step-1 emission fixes
> (sprite floors, LOPR budget un-gating) still not built.

> **STEP 1 AS-BUILT 2026-08-19 (same evening; split validated by the owner, default build validated
> on Ymir).** (a) The measured-budget state (LOPR meter, `vdp1_budget_cmds`, weapon reserve,
> wall-span LOD knobs) and its application moved **out of `#if SHOW_FPS`** — it drives emission, so
> a release build must keep it; only the tx count and fb profiler stay overlay-gated. (b) **New
> overrun signal**: under the gated swap the LOPR guillotine can never fire (LP≡100), so the budget
> now backs off on the fence's **COPR-gate spin** (`sat_mp_gate_ms`, row 8 `g`): every spin ms is a
> frame ms lost 1:1 → `vdp1_budget_cmds` snaps multiplicatively below the list that spun (floor 32);
> the clean-frame drift-up re-probes as before; the LOPR branch stays (inert, revives if the present
> ever reverts). (c) Core floors raised (flag DoomJo, constants only): decorations 2%→1%, actors
> 5‰→2‰, `THING_EMIT_MAX` 16→32 lockstep with `THING_ADAPT_MAX`, boot cap 4→8. (d)
> `THINGS_TEX_SLOTS` 4→6 was tried for ONE build and **REVERTED** (owner captures: monsters in the
> HUD, HUD texels on monsters): the things pool truly ends at 0x25C78000 — `VDP1_HUD_TEX` sits at
> 0x25C78000..0x25C7BC00 (bar + double-buffered message slots) and the 2p HUD stack reaches
> 0x25C7D000 into the "free" F banks; raising the distinct-texture grant needs a relocated region
> + split addressing, not a constant bump. (e) The `ec`→0 snap softened: `budget_cap` floor 2 (1p
> and per-view split) — the two nearest actors always ride; the flush guard still hard-bounds the
> bank. Pool 19.48 KB. Judge on
> `th`/`ec` in hordes, `g` (should stay ~0), fps; the CPU may be the binder on console (T165>R141) —
> final verdict there.

## D. The motion holes ("ils ont toujours existé" — owner correction 2026-08-18)

**History**: the artifact is coeval with the first VDP1 wall — core `84f3130` (06-17, one day after
the first quad) already names *"sky-at-the-seam lag"* and ships exit-coverage + the early kick.
Invariants across all four topology eras: zero at rest, wall↔software junction only, temporal not
geometric (two symmetric-failure campaigns), survived every present mechanism ever tried,
reproduces on Ymir. Taxonomy: tearing, walls-vanish, weapon tear, static 1-px seams, sky/floor
boundary slide are **different artifacts**, all separately fixed or explained.

**What ships today (Fl0, fence parked and unreachable)** — live stale-content mechanisms
[code-verified]: (1) the **kick→blit pairing beat**: root flip at end of emission, list visible
kick+2 fields, blit at frame end; the ≤1-field mismatch window flips sign with the fractional
field-per-frame ratio → the owner's periodic beat; (2) the **blit/beam race**: single-buffered NBG1
memcpy at uncontrolled beam phase (horizontal split of two viewpoints) — Fl1 fixed "most cases"
[Ymir] and does not ship; (3) the **sky-map deferred write** placed assuming the fence — under Fl0
it lands mid-field (small, literal sky-through); (4) **entry-coverage residual gaps** (16-frame tag
alias: arm requires the first-visit branch, aliased segs get neither arm nor decrement; a wider tag
needs a zone-heap array, not the packed byte).

**The adversarial recomputation that survives** (it corrected the census's own headline): the kick
is *late* in the frame (wall/thing emission = 44-86 % of R happens before the flip), so the
post-flip tail τ ≈ 1-2 fields and the dominant ship window is the **lag** sign {NBG1 N, VDP1 N-1} —
the sign entry-cover and lead-fill already patch. And **A2/2 pins the whole inter-blit interval,
not one instant**: age=2 on every frame ⇒ list N visible exactly at blit N through blit N+1 ⇒ fully
coherent pairing — *and the holes persisted on those captures*. **The entire pairing family is
therefore refuted as the residual cause** (while remaining the plausible driver of the *ship*
beat, since the fence is parked). ⚠️ Provenance caveat: the A2/2 captures are most likely **Ymir**
(a18.2/MST40 is Ymir-range) — relabelled in memory.

**What could still be the residual**: (a) **transfer-over on HW** — content incomplete at swap
(CEF/LOPR data proves real mild overruns) — which the §C manual present eliminates by construction;
(b) an **in-frame content error on Ymir** (no overrun there) not yet identified; (c) the residual
only ever existed under Fl2's specific conditions and the ship symptom is simply (1)+(2). The
2026-08-02 capture provenance decides between these — ask the owner.

**Discrimination plan (cheapest first — the number before the fix):**

0. **Owner questions (free — he plays daily):**
   1. When a hole opens while turning, is the gap on the side you are turning **toward** (wall in
      its old place = walls lag) or **away** (wall ahead of the picture = walls lead)?
   2. Is a hole ever bounded by a **horizontal line** partway up the screen (beam race), or always
      full junction height?
   3. Are affected walls ones that just entered view at a screen edge (entry gap), or long-visible
      walls mid-screen?
   4. The 2026-08-02 Fl2 hole captures — **Ymir or console?**
1. **Generation stamp** (~20 lines): 8×8 VDP1 corner quad, colour = frame# low bits; same counter
   into adjacent NBG1 pixels; Ymir frame-step during a continuous turn. On every hole field the
   stamp delta must be ≥1; expected overall mismatch ≈ |τ−2| per frame ≈ 0-25 %. **A single hole
   field with delta=0 refutes the whole pairing family in one capture** and redirects to (b).
2. **Fl1 rebuild on console** (1 int, dg:1915): sizes the beam race's share of the ship symptom —
   its prior validation was Ymir-side; its fps cost at console frame times is unmeasured.
3. **Kick-defer A/B** (move only flip+PTMR next to the fence): makes the pairing error a constant
   ≤2-field lag with no beat — the sign the shipped patches (entry-cover, lead-fill X=1) are built
   for. Free; decisive for the pairing share; weapon lags 2 fields (known cost).
4. **`sat_wall_entry` off/on** (existing chords) and **tic-cap A/B** (code constant): size the
   entry share and the tic-vs-field dependence.

**Convergence**: the §C manual present synced to the blit vblank closes the ship pairing windows
(1) *by construction* and eliminates transfer-over (a) — one mechanism addresses both lists. What
it cannot fix is (b): judge it on tearing/transfer-over/sprites/pairing-beat, never on a promise to
close every hole.

### D.5 — Owner answers + lead-fill audit (same day, 2026-08-18)

The owner answered §D.0: **(1) the walls LAG** (the {NBG1 N, VDP1 N−1} window); **(2) the hole is
exactly the band the lead-fill paints — and the lead-fill DOES paint it today, but it is a costly
CPU corrective; the goal is to fix the offset at the source so the lead-fill can be DISABLED and
its cost recovered**; **(3) wall visibility time is irrelevant** (entry-coverage demoted);
**(4) Ymir AND hardware** (transfer-over demoted as primary; the 08-02 Fl2 captures were probably
Ymir). Success metric for the present work: **lead-fill OFF (`L0-`) + continuous turning with no
holes**, plus the measured CPU recovery.

A focused audit of the lead-fill path (core/r_segs.c) then explained the *residual* holes seen
today with the corrective ON — the math is right (new ∖ old direction; the n−1∩n−2 subtrahend
covers the age beat; timing sound, fill lands before the blit), but **the failure default is
inverted: every "no history" case yields ZERO fill instead of a full fill**, and history is starved
systematically:

1. **Record gate** `!sat_sw_mid` (r_segs.c:1647, :1733, :1807): every *dual-draw* state
   (exit-cover, entry-cover, pre-warm band, dwell pins) emits the quad to VDP1 but never records it
   → on the 3rd frame after any path flip, no n−1/n−2 records → zero fill → the exact sliver.
   Motion generates flips continuously → matches owner points 2 and 3. *Zero-code falsifier:*
   R+Up (dwell 4/8) lengthens dual-draw pins → under current code post-flip holes must get WORSE.
2. **Floor-clamped tiers are never recorded** (SAT_WALL_CLAMP=1 ships): floor-crossing walls have
   no lead coverage at all, every frame, in motion — a persistent state, not a transition.
3. **`SAT_LEADSPAN_MAX` 1536 is crossed during fast turns** (need ≈500-3000 spans); drops are
   silent, biased to the farthest walls. `L1-` (master textured direct) bypasses the span
   machinery entirely → **decisive A/B: `L1s` vs `L1-` during a fast spin** (holes gone on `L1-`
   ⇒ span cap/drops; identical ⇒ record gate). Also: the perf governor forces X=0 in collapsing
   frames — holes there are by design.

**Fix ranking (revised for the owner's objective):** the §C manual present at **age 0** (swap at
the blit vblank) makes every class above belt-and-braces and is the only path that *retires* the
corrective (plus entry-cover, plus most of exit-cover) — the perf endgame. The ~6-line record-on-
emission fix + inverted no-history default + span-overflow master fallback are optional interim
visual fixes; note that a *pipelined* manual present (age 1) would still need the record-gate fix —
prefer the age-0 variant. The stale re-resolve fallback paints an opaque grey sliver (never index
0) — a different artifact, not the holes.

## E. Kill-list disposition (from the 2026-06-29 plan)

- **Re-opened**: manual/gated present (kill metric dead); coupled deterministic present (= the
  theory; both objections obsolete); defer-PTMR (premise was a 1-cycle artifact); "shrink can't
  help" (refuted on HW).
- **Still dead**: NBG1 double-buffer (VRAM arithmetic unchanged; and unnecessary under §C); naive
  NBG1 lagging mirror ("largement pire"); SCU-DMA blit (B-bus hang); any present scheme sold as the
  motion-hole cure.

## F. Memory corrections applied this session (2026-08-18)

1. `vdp1-budget-latch-kills-sprites` — **was stale**: latch partially fixed 08-10 (map reset +
   geometric climb); residuals = mid-map `ec` snap + `SHOW_FPS` gating.
2. `m7-vdp1-latency-coherent-pair-hold` — holes **predate M7** (owner); A2/2 relabelled
   [Ymir-likely]; Fl1 **parked**, not shipped.
3. The "Fl1 shipped/validated stable" framing and the "16 % fps-cap tax" and the "intrinsic"
   verdict are corrected by this document; `docs/VDP1_ARCHITECTURE.md`'s "manual-present driver, no
   tearing" description and its "whole 100 ms to draw" arithmetic describe a non-shipping config —
   flagged stale, not yet rewritten.
