# REC Reduction — exploration, catalogue & gated experiments

Goal: cut **REC** (the mono-master command-generation phase) in solo **and** multi.
REC is the frame's dominant cost since the walls moved to VDP1 and the slave went
~80% idle (see `doomsrl-perf` memory). Method = **map → catalogue → mine → brainstorm
→ gated experiment**, never code-in-the-dark. Every lever below is classified; the
two safest are implemented behind flags and awaiting hardware A/B.

> **Measurement law:** Ymir massively understates memory-bound cost — *every REC
> number must be read on real hardware* (overlay rows 19/20). The 6 reference spots
> are the `doomsrl-perf` capture set (tech/eau, sombre/esc, brun/eau × pot0/1/2 +
> a damage-flash frame).

---

## 0. What REC is, measured (recap, hardware)

`R_RenderPlayerView` (core/r_main.c) is a **serial pipeline on the master**; the
slave only *executes* recorded draw commands in the background:

```
RP_BeginFrame
R_RenderBSPNode  ──► Bw (BSP walk + clip + R_FindPlane + R_AddSprites)  7–10 ms
                     Bp (R_StoreWallRange wall-prep, bracketed)        19–31 ms   ← #1
RP_MarkBSPDone   ──► sat_walls_done_hook  (kick VDP1 walls)
R_DrawPlanes     ──► P  (R_MakeSpans + R_MapPlane + RP_RecordSpan)     12–55 ms   ← #1 at pot0
RP_BeginMasked
R_DrawMasked     ──► M  (vissprite sort + clip + record)                4–9 ms
RP_EndFrame      ──► EX (master+slave drain the command buffer)        ~2.7 ms (pot1/2)
```

Hard facts established this session by reading the code:

| Fact | Source | Consequence |
|---|---|---|
| Command record = **32 B fixed** (`rp_cmd_t`), buffer at **0x002D8000 = LOW work-RAM (slow DRAM)** | r_parallel.{c,h} | every command = 32 B master *write* + 32 B slave/master *read* to the **slow** bank → bandwidth is a direct REC+EX lever |
| **visplanes[] (332 KB) is Z_Malloc'd in the zone heap = LOW work-RAM (slow)** | r_plane.c:139, dg_saturn.cxx `DG_ZoneBase` | R_FindPlane linear-scans + per-column top[]/bottom[] writes hit slow DRAM |
| framebuffer, floorclip/ceilingclip, spanstart, drawsegs, solidsegs, trig tables = **BSS = HIGH work-RAM (fast)** | dg_saturn.cxx:283, r_plane.c BSS | these are already on the fast bank |
| `texturecolumn` (per-column FixedMul) **still computed for VDP1-owned walls** | r_segs.c:488 | dead work → **QW1** |
| Potato floor spans are **recorded (32 B) then memset in the executor** | r_plane.c:239 + rp_exec_span | the record write is pure slow-DRAM traffic → **QW2** |
| `R_FindPlane` is a **linear O(n) scan** per subsector (vs d32xr's hash) | r_plane.c:301 | O(n²)/frame of slow-DRAM reads in plane-heavy scenes |
| Dead-visplane skip (`minx>maxx`) **already present** | r_plane.c:508 | FastDoom's "skip dead visplanes" is *already done* — not an available win |

---

## 1. Classified plan (the deliverable table)

Gain = rough fraction of the targeted phase, to be confirmed on hardware. Risk: 🟢 safe
(no protocol/coherency change) · 🟡 gated experiment (concurrency/determinism to validate)
· 🔴 large/structural. Revert: ⟲ trivial (flag) · ⟲⟲ moderate · ✗ rewrite.

| # | Piste | Phase | Gain (est.) | Risk | Solo | Multi | Revert | Statut |
|---|-------|-------|------------|------|------|-------|--------|--------|
| QW1 | **Trim dead `texturecolumn`** in VDP1-owned wall columns (gate on `sw_draws‖maskedtexture`) | Bp | small–med (per-col FixedMul + 2 array reads ×most cols) | 🟢 | ✓ | ✓ | ⟲ | **SHIPPED** (committed, default-on, byte-identical on DoomJo); HW GAIN measurement pending |
| QW2 | **Potato floors drawn INLINE** (memset in R_MapPlane; skip RP_RecordSpan + executor) | P | med (kills 32 B write + 32 B read / span) | 🟡 | ✓ | ✓ | ⟲ (`SAT_POTATO_INLINE_SPANS 0`) | **SHIPPED** (committed, default-on; ship config IS potato → on the hot path); HW A/B GAIN pending |
| L1 | **visplane hash** in R_FindPlane (d32xr-style) — O(n)→O(1) | Bw/P | med in plane-heavy scenes | 🟡 (core, shared w/ DoomJo; behaviourally identical) | ✓ | ✓ | ⟲ (runtime `sat_visplane_hash=0`) | **SHIPPED** (committed, default-on; now a RUNTIME toggle `sat_visplane_hash`, core ea452d8 — NOT a `#define`; live pad-Y A/B on HW); HW GAIN pending (Bw is 2–3 ms on Ymir vs 7–26 ms on HW) |
| L2-SHRINK | **Command buffer shrink** (return slow-LWRAM space to the streaming zone heap) | REC+EX | med | 🟡 | ✓ | ✓ | ⟲ | **SHIPPED** — Makefile `-DRP_CMD_BUF_SIZE=0x14000` = **80 KB** (not the spec's 64 KB), 1.9× the ~1068-cmd Ymir peak; buffer still top-of-LWRAM (addr derived), `DG_ZoneBase` reclaims the 80 KB to the zone. |
| L2-RELOCATE | **Command buffer → HIGH work-RAM** (BSS `rp_cmd_buf[]` on the fast bank) | REC+EX | med (write+read on fast bank) | 🟡 (HWRAM budget; relocation; shared core) | ✓ | ✓ | ⟲⟲ | **STILL-TODO.** Un-coded (`RP_CMDS` still points to LWRAM; no `RP_CMD_BUF_IN_HWRAM` / `rp_cmd_buf[]`). Spec `docs/REC_L2_SPEC.md` §3 — but recompute the HWRAM budget for the SHIPPED 80 KB BSS (~45 KB heap-C left, not the spec's 61 KB). Gain **Ymir-invisible** (bank-latency only) + freeze-zone. RBG0-merge gate dropped (RBG0 broke on HW, session closed). |
| L3 | **Command compaction** (per-type short records; FUZZ=9 B, COL=28 B vs 32 B) | REC+EX | small–med (less DRAM traffic) | 🔴 (breaks fixed-stride parity index + two-pointer steal) | ✓ | ✓ | ⟲⟲ | catalogued — likely not worth the complexity |
| T1 | **Idle-camera REC skip** (re-present last frame when output provably identical) | all REC | huge *when it fires* | 🟡🔴 (VDP1-hybrid present path + determinism) | ✓ | ✓ | ⟲ | catalogued — see §4 (safe form low-value at 8 fps) |
| T2 | **Pure-yaw wall-prep reuse** (cylindrical projection: yaw shifts columns, not heights) | Bp | large (theoretical) | 🔴 (deep; invalidation) | ✓ | ✓ | ✗ | catalogued — research, not near-term |
| T3 | **Half-rate planes** (regenerate P every other frame) | P | ~½ P | 🔴 (visible artefact on motion) | ✓ | ✓ | ⟲ | catalogued — quality risk |
| L4 | **DIVU overlap** for remaining per-seg divisions | Bp | tiny (few divs left after VDP1 walls) | 🟡 | ✓ | ✓ | ⟲⟲ | **STILL-TODO (catalogued, low value).** Async DIVU-overlap unbuilt; residual tiny because QW1/lever-C already skip the per-column `dc_iscale` divide (r_segs.c:558-570). |
| L5 | **Scratchpad (CCR TW=0x08)** for a hot REC datum | — | ~0 / negative | 🔴 (halves the 4 KB cache on a cache-bound load) | ✓ | ✓ | ⟲ | **rejected** — see §2 |
| M1 | **Per-view parallelism** (master=P1, slave=P2, independent contexts) | N×REC→~1×REC | large (multi only) | 🔴 (context dup + allocator gate + VDP1 ×N) | — | ✓ | ✗ | catalogued — see §5, gate is make-or-break |
| — | PVS/REJECT culling of the BSP walk | Bw | ~0 | — | — | — | — | **rejected** — Doom's renderer does exact visibility via solidsegs; REJECT is sight-only |
| — | Dirty-rect | all | ~0 when moving | — | — | — | — | **rejected** — degenerates to T1 |
| — | Skip dead visplanes | P | — | — | — | — | — | **already implemented** (r_plane.c:508) |

---

## 2. Hardware catalogue (Phase B)

- **Two RAM tiers.** LOW work-RAM (0x00200000, 1 MB, **DRAM, slow**) holds the zone
  heap (visplanes, colormap, textures, streamed lumps) **and** the command buffer.
  HIGH work-RAM (0x06000000, 1 MB, **SDRAM, fast**) holds BSS (framebuffer, clip
  arrays, drawsegs, tables). **The hottest pointer-chased / per-span structures
  (visplanes, command buffer) sit on the SLOW bank** → migrating them is the most
  direct memory-bound lever (L2). Constraint: the 1 MB HWRAM is near full (visplanes
  were *evicted* to LOW-RAM to fit the BSS limit), so only a *shrunk* command buffer
  realistically fits — measure (L2).
- **SH7604 cache = 4 KB, write-through, no copy-back** (`sh7604-ccr-bits`). Cross-CPU
  writes always reach RAM; coherency via the 0x2xxxxxxx cache-through mirror + CP purge.
- **Scratchpad (CCR TW=0x08).** Enabling two-way mode gives ~2 KB on-die scratchpad
  but **halves the cache to 2 KB**. On a cache/memory-bound master this is almost
  certainly a net loss; the only candidates small enough (colormap 256 B, clip arrays
  640 B) are either EX-side or already on the fast bank. **Rejected (L5).**
- **DIVU.** QW1/lever-C already **eliminate the dominant per-COLUMN** `0xffffffff/rw_scale`
  divide for VDP1-owned/solid walls — nested guards at `r_segs.c:558` (`if (sw_draws)`)
  and the `!wall_solid` guard near `r_segs.c:569-570`. Remaining REC divisions are a few
  *per seg* (R_ScaleFromGlobalAngle, rw_scalestep). The async DIVU-overlap (L4) of those
  residual per-seg divides is **unbuilt**, and its value is now small precisely because
  the costly per-column divide is already skip-gated.
- **Bus contention (the Saturn curse).** Two SH-2 cannot both hit RAM at full rate; the
  2.5 work-steal already measured the slave ~1.5× slower reading commands from RAM.
  This **bounds every parallelism gain** — it is why per-view multi (M1) cannot reach a
  clean 2× and why generation-offload on a memory-bound master is capped.

---

## 3. Reference mining (Phase C) — what makes others' REC faster, *hors-slave*

- **d32xr — visplane HASH (`visplanes_hash`, `R_FindPlane` by `flatandlight`).** Ours
  is a linear scan (r_plane.c:301) called per subsector for floor+ceiling → O(n²) slow-
  DRAM reads in plane-heavy rooms. A hash (or even a small picnum bucket) is the single
  most transferable *algorithmic* REC win that needs no slave → **L1** (SHIPPED, default-on
  via the runtime toggle `sat_visplane_hash`, core ea452d8).
- **d32xr — compact `VINT`/16-bit fields + cache-aligned hot functions.** Less memory
  traffic on a memory-bound path. Our shared core uses 32-bit everywhere; the one
  DoomSRL-ownable compaction is the command record (L3).
- **d32xr — deferred wall-prep (phase1 records `viswall_t`, phase2 preps).** Better
  cache locality than vanilla's inline R_StoreWallRange, *and* it is what enables their
  producer/consumer. Big restructure; out of scope (REC parallelisation rejected).
- **FastDoom — "skip dead visplanes" = `pl->minx>maxx`/`modified`.** **Already in our
  R_DrawPlanes** (r_plane.c:508). No win available. Its `dc_iscale` is still a per-
  column divide (no reciprocal table in the path read) — matches our lever-C decision
  to *skip* the divide rather than table it.
- **SlaveDriver — per-SECTOR precompiled command lists linked by JUMP_CALL/RETURN.**
  Amortises *generation* by reusing a sector's command list across frames — the
  hardware analogue of temporal-coherence command reuse (T2/§4). It is a VDP1 design,
  not transferable to the column rasterizer, but it is the proof-of-concept for "reuse
  recorded commands."

---

## 4. Disruptive brainstorm (Phase D) — temporal coherence & friends

**Temporal coherence is the only potentially *multiplicative* family**, but the
VDP1-hybrid present path and Doom's animation make the *safe* forms narrow:

- **T1 idle-skip.** Output = f(view params, world). World changes whenever a tic runs
  *and* something visible moves. The only **provably-safe** trigger is "no game tic ran
  since last render (`gametic` unchanged) **and** view params unchanged" → at our ~8 fps
  (< 35 Hz) a tic runs almost every frame, so the safe trigger **rarely fires during
  play** (it mainly helps menu/idle). The *valuable* form (standing still during play)
  cannot be proven safe — animated flats/textures (NUKAGE, FWATER…), distant monster
  frames, light flicker all change with no view change. **Invalidation set to track if
  ever pursued:** viewx/y/z, viewangle, extralight, fixedcolormap, `levelTimeForAnims`
  (flat/texture animation tick), any visible thinker (sprite) state, palette_changed,
  menu/automap overlay. In the hybrid, a skip must also *not* clear the framebuffer and
  *not* re-kick VDP1 (both happen every frame today) — so it touches the present path
  (freeze-zone-adjacent). **Verdict: real upside, but medium-high risk and low solo
  value at current fps; best reserved for the multi/idle case.**
- **T2 pure-yaw reuse.** Doom's projection is ~cylindrical (`viewangletox`): a pure yaw
  rotates *which* columns are visible without changing wall heights/scales. In principle
  the previous frame's wall-prep could be re-projected with an angular column shift.
  Deep and invalidation-heavy (any translation breaks it; floors are *not* yaw-
  invariant). Research-tier, not near-term.
- **T3 half-rate planes.** Regenerate P every other frame, reuse the visplane spans
  between. Halves P but visibly shears floors under motion. Quality risk; gate-able.
- **Command compaction (L3).** Per-type records would cut DRAM traffic (FUZZ uses 9 of
  32 B, COL 28), but variable-length records break the **fixed-stride parity index** and
  the **two-pointer steal** (both assume `cmd[i]` at `base+i*32`). Not worth the rewrite
  unless EX becomes a bottleneck again.
- **PVS/REJECT — rejected.** REJECT is *sector-to-sector sight* (for `P_CheckSight`),
  not a render PVS; Doom's renderer already culls exactly via the BSP + solidsegs. No
  render-side win, and shareware maps are small anyway.
- **Dirty-rect — rejected.** With a moving camera the whole view is dirty; it
  degenerates to T1.

---

## 5. Multi (Phase E) — per-view parallelism & the make-or-break gate

Model: `Frame_N ≈ N×REC + EX + blit + sim` — only REC multiplies per view. The
principled win is **per-view parallelism**: master renders P1, slave renders P2 with
*independent* render contexts (no second writer on one command list, no mid-phase
barrier — a clean frontier, unlike the rejected mono-view split).

**Make-or-break gate = the allocator. Evaluated cart-agnostically (do NOT assume the
4 MB cart):**

- During render, **R_DrawPlanes calls `W_CacheLumpNum`/`W_ReleaseLumpNum` per
  visplane** and **R_GetColumn calls `R_GenerateComposite` on first sight** — both touch
  the **zone allocator** (Z_Malloc/Z_ChangeTag/rover). Two CPUs in the allocator
  concurrently = heap corruption = the freeze history.
- **Cart path** (WAD mmapped, `lump->wad_file->mapped`): `W_CacheLumpNum` for **flats**
  is a direct pointer, allocator-free (w_wad.c:410). **But the design must not depend on
  this** — the game also runs **CD-streamed / 1 MB / no-cart**, where flats go through
  the zone cache. And texture **composites** allocate on first-sight on *every* path.
- ⇒ **The gate is mandatory, not free:** before the concurrent section, **pre-load +
  LOCK (PU_STATIC, no release) every flat and pre-generate every texture composite of
  *both* views**, so zero allocator calls happen during concurrent render. Two ways:
  (a) a per-view **visibility pre-pass** to enumerate the working set (≈ a second BSP
  walk), or (b) **lock the whole level's flat+texture set at load** — feasible *iff* it
  fits, and **without the cart RAM is tight** (zone heap ≈ 864 KB low-RAM, also holding
  streamed lumps), so (b) may not fit → (a) is the robust path.
- **Second cost:** all mutable `r_*` globals (viewx/y/z, clip arrays, visplanes,
  ds_*/dc_*, walllights, …) must become **per-view** → heavy divergence from the shared
  DoomJo core (gate behind a render-context struct / flag).
- **Hard ceiling:** the VDP1 wall fill also multiplies (N× quads into an already-overrun
  VDP1) — per-view parallelism speeds *generation*, not the VDP1 fill.

**Verdict:** per-view parallelism is the right multi architecture but is a large,
gated project; the allocator pre-cache gate must be built and validated **first** and
**without** relying on the cart. Cheaper near-term multi = Potato (incl. **QW2**, which
helps every view) + temporal coherence for low-motion views + accept lower fps.

---

## 6. Implemented quick wins — detail & measurement protocol

### QW1 — trim dead `texturecolumn` (core/r_segs.c, R_RenderSegLoop)
The per-column texture-offset (`angle`+`finetangent` read + `FixedMul` + shift) feeds
**only** `R_GetColumn` (software draw, gated by `sw_draws`) and the masked-midtexture
save (gated by `maskedtexture`). For a VDP1-owned non-masked seg both are off, so it is
dead work. Changed the guard from `if (segtextured)` to
**`if (segtextured && (sw_draws || maskedtexture))`**.
*Safety:* on DoomJo / VDP1-off, `sat_wall_skip==0` ⇒ `sw_draws==1` always ⇒ condition
always true ⇒ **byte-identical**. Pure dead-work removal, no protocol/coherency change.

### QW2 — Potato floors drawn inline (core/r_plane.c, R_MapPlane)
When `sat_potato_floors`, the span is a flat `memset` of one distance-shaded texel. We
now do that memset **inline in R_MapPlane** and `return` **before** `spanfunc()`, so the
32-byte `RP_RecordSpan` write to slow low-RAM **and** the executor read are eliminated.
Mirrors `rp_exec_span`/`rp_exec_span_low` exactly (fixed `R_POTATO_TEXEL` 2080, the
detailshift ×2 doubling). Gated `#define SAT_POTATO_INLINE_SPANS 1`.
*Safety:* gated on `sat_potato_floors` ⇒ **pot0 byte-identical, DoomJo unaffected**.
Pixel-safe — floor/ceiling spans never overlap the slave's concurrent wall/sprite
columns (Doom has no plane↔wall overdraw); master writes are write-through; the blit
purges before reading ⇒ **no new cross-CPU coherency surface**. Trade-off to *measure*:
the memset now runs serially on the master during P instead of being drained by the
slave in EX — net win only if the saved DRAM record traffic exceeds the serialised
memset (expected yes in the VDP1-walls ship config, where the command buffer is
otherwise nearly empty). Revert: `SAT_POTATO_INLINE_SPANS 0`.

### Protocol (Romain, on hardware — Ymir lies for memory-bound)
1. Same build, both flags as shipped (QW1 always on; QW2 on via the define).
2. Read **row 19** `REC EX W c` and **row 20** `Bw Bp P M` at the 6 reference spots.
3. QW1 expectation: **Bp** down at pot1/pot2 (most walls VDP1-owned), pot0 unchanged-ish
   (more software walls → fewer skipped columns). Bw/P/M flat.
4. QW2 expectation: **P** down at pot1/pot2; pot0 unchanged (Potato off). `c` (command
   count) drops sharply (spans no longer recorded); EX may drop too. Watch for any
   tear/garbage in floors (should be none).
5. A/B QW2 by rebuilding with `SAT_POTATO_INLINE_SPANS 0`.
6. If QW1 shows nothing, it confirms Bp is dominated by clip/visplane marking, not
   texturecolumn (still a free cleanup). If QW2 regresses, the serialised memset > the
   saved record traffic → revert and pursue L2 (command buffer → fast RAM) instead.

---

## 7. Recommended order

1. **QW1 + QW2** — **SHIPPED** (committed, default-on) → hardware A/B GAIN at the 6 spots still pending.
2. **L1 visplane hash** — **SHIPPED** (committed, default-on; RUNTIME toggle `sat_visplane_hash`, NOT a `#define`; live pad-Y A/B on HW). Best remaining *algorithmic* solo win, no slave, helps multi. HW GAIN pending.
3. **L2 command buffer** — **SHRINK SHIPPED** (Makefile `-DRP_CMD_BUF_SIZE=0x14000` = 80 KB; `DG_ZoneBase` reclaims it to the streaming zone). **RELOCATE → fast RAM is STILL-TODO** — attacks the memory-bound core
   directly. **SPEC: `docs/REC_L2_SPEC.md`** (recompute HWRAM budget for the shipped 80 KB BSS).
   RBG0-floors merge gate dropped (RBG0 broke on HW, that session is closed).
4. Multi: build the **allocator pre-cache gate (cart-agnostic)** as the first, isolated
   step before any per-view parallelism.
5. Temporal coherence (T1/T3) only if a concrete idle/menu/multi case justifies the
   determinism work.
