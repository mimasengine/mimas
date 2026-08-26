# Mimas — TODO

**Rewritten 2026-08-25.** The previous version was a *numbers* document, and that is why it
rotted: nine of its twelve entries were overtaken within ten days, its headline claim was
disproved by its own instrument, and its "branch state" section was wrong the moment it was
written. This one is deliberately two things and nothing else:

1. **A ranked board** — what to do next and what decides it.
2. **A DEAD table with the LAW that kills each entry** — the law is what stops a re-proposal;
   the number is what makes the law credible.

**Rules for editing this file.** Numbers live in the ledger (`docs/captures/`), the measured
board (`docs/HEADROOM_2026-08-25.md`) and the hardware dossier
(`docs/DOSSIER_MATERIEL_2026-08-25.md`) — cite them, do not copy them. Never restate git state
in prose. Every entry states **what decides it** and **whether Ymir is legal for that
decision** (counts, identities, bug reproduction and the sign of a boolean effect: yes;
milliseconds: never — [[ymir-not-a-perf-oracle]]).

---

## The board

### 0. THE CONSOLE SESSION — three discs, one trip. This is the gate.

Everything below rank 3 is priced off **one map** (shareware E1M1). The marginal cost of a
drawseg has a 95 % CI of **0.38–0.98 ms** — a 2.6× spread — so a second measured map is worth
more than any single lever on this board.

- **Disc A — TNT MAP20, 1p**, walked to the fixed spot (−672, −929, facing `a64`) where three
  prior captures already exist. The only genuinely controlled A/B this project can perform, and
  it is the **only** falsifier for the `mobj_t` cache-line reorder (core `2e2b48b`), which is
  shipped into a shared submodule and currently unvalidated. **Kill criterion stated in
  advance: if `mo − ph − sm − mv` still reads 33–36 ms at that spot, revert the reorder.**
- **Disc B — shareware, 4p**, exercising the live chords in the *same* footage
  ([[interbuild-perf-noise]]: a build-vs-build photo is not evidence).
- **Disc C — big WAD, 4p, CARTLESS**: rows 11, 13, 0 `ld`, 12 `t`.
  ⚠ **Decide the cart fork before burning**: row 12 is `CD` on a streaming disc and `SKY` on a
  ≥4 MB cart build. One disc cannot answer both the CD pump and the 3-quadrant HW sky.

**Decides:** everything. **Ymir: illegal** for every millisecond here.

### 1. The split-only per-view residual — the largest unattacked term

`R − (Bw+Bp+P+M) − kick` = **−2.0 / 5.9 / 19.7 / 20.8 ms** in 1p/2p/3p/4p (console medians,
n=22/23/22/31). **Zero in 1p** — so it is a split-law member, not the slop of a derived number.

`rs` has been moved (2026-08-25) onto the candidate that fits that signature: the
`R_ClearClipSegs / ClearDrawSegs / ClearPlanes / ClearSprites / NetUpdate` block in
`R_RenderViewPass`, which runs **once per view, outside every phase bracket**.

**Decides:** read `rs` in split. Non-zero ⇒ the term is named and can be attacked. Still ~0 ⇒
it is deeper, and the next FRT pair goes inside `R_RenderBSPNode` or between `MarkP` and
`BeginMasked`. **Ymir: legal** (zero vs non-zero is a boolean).

### 2. `pr` — a per-drawseg CONSTANT of ~0.21 ms, and it is our own code

Console medians: `pr` = 3.4 / 9.6 / 15.8 / 15.2 ms over `d` = 19.5 / 47 / 75.5 / 73 ⇒
**0.174 / 0.204 / 0.210 / 0.208 ms per drawseg** — ~5 950 SH-2 cycles **before a single pixel**.
`core/r_parallel.c` defines it as the per-seg VDP1/CPU tier routing, hysteresis, clamp,
perspective subdivision and lead-fill arming: Saturn additions, not vanilla Doom.

**Why it matters more than it looks:** from 1p to 4p, `pr` grows **+11.8 ms** and `hd`+`tl`
grows **+9.9 ms** while `lp` **falls 3.8 ms**. `pr`+`hd` carry *all* of `Bp`'s split inflation,
and **every shipped LOD rung reaches only `lp`**.

**Two zero-risk probes before any mechanism**, both readable on the existing row-4 partition:
(a) hoist the per-seg tier decision to per-frame wherever its inputs are frame-constant;
(b) run one frame with the VDP1 wall route forced off — if `pr` collapses, it belongs in the
same budget as `P`, not in `Bp`. **Ymir: legal** for the identity that a hoist changes no
output; illegal for the ms.

### 3. Deathmatch — ~20 lines for the showcase's headline mode

`sat_deathmatch` is declared in `core/g_game.c`, read twice, and has **zero writers anywhere**:
the whole core side ships as unreachable dead code. Missing: one platform writer, a title-screen
cycle beside the existing `PLAYERS:` banner, and frag display (the HUD slot already exists).

**Decides:** first a **count** — do the target maps carry 4 DM starts (shareware E1M1–E1M9 do)?
**Ymir: legal.** Then a play session: this is a game-design gate, not a perf gate.
⚠ Priced on shareware, where `T` is 11 ms of a 158 ms frame. On the endgame class `T` is 81–88 ms
and the *larger* half of the frame — DM there is **unpriced**.

### 4. Audio quality at 0.00 ms

`S` = 0 ms on **106/106** console frames, so the master can neither lose nor gain time here.
MUS channel 15 (percussion) is skipped in *both* arms and is **34.6 % (shareware) / 38.9 %
(Doom II)** of all note-ons — thrown away. The whole timbre budget is **96 bytes**.
Order: `mus_step` on the vblank ISR (~15 lines) → percussion + 24-voice pool + real ADSR + pan
→ per-map sample bank. Detail and the sound-RAM conflicts: `DOSSIER_MATERIEL §7.2-D`, `§7.4`.
⚠ **Conditional on the CDDA fix**: if CDDA returns, verify this is still on the default path
before writing 350 lines.

### 5. CDDA boot (~480 s) — run the free probe first

`GFS_Init` runs **before** `CDC_CdInit`, the inverse of both shipping references on disk.
The probe needs **zero code change** (six printf markers already print). The fix is ~4 patch
lines in `patches/saturnringlib.patch` — which is **shared with Tethys**, so a mistake
propagates. ⚠ A naive reorder can trade an 8-minute boot for silent music (`CdPlay` is silent
without `CdInit`). **Ymir: legal** — bug reproduction and marker ordering are identities; but an
Ymir boot that *completes* is inconclusive, not exculpatory.

### 6. FOV — DEMOTED 2026-08-25, and here is why

The chord (**pad L+Y**, 90/75/65, row-7 `f<deg>`) is shipped and the mechanism is real:
measured on Ymir, `d(65)/d(90) = 0.78` — under the 0.85 kill criterion.
**But the model died anyway.** `c` (column iterations) *rises* 1068 → 1215 as the arc narrows:
fewer walls, each wider on screen. `pr` and `hd` fall, `lp` rises, and **`Bp` moves only −6.7 %**
against a predicted −28 %. The `−9 to −15 ms` figure assumed `Bp ∝ d`; under an FOV change it
is not. Keep it as a **game** option (and it sharpens the image: 1.78 → 2.46 px/degree), not as
a perf lever. ⚠ Still probe-grade: the HW sky's scroll law is 90°-derived and unscaled.

### 7. Openings — the cut is now safe to size, but not yet to make

`openings[]` is **40 960 B of `.bss`, larger than the whole ~28 KB TLSF pool**, and 64 rows was
a vanilla guess nothing measured. The instrument was fixed first (2026-08-25): it folds
**demand**, not consumption, because all three sinks redirect without advancing `lastopening`
and the old counter therefore **saturated by construction**. Take the reading on a big-WAD vista
before cutting anything.

### 8. Grate / monster z-inversion — owner's call when to return

Structural: world things are VDP1 sprite priority 5, masked midtextures are NBG1 priority 6, and
the order is fixed in hardware. **Two fixes were built and withdrawn**, both measured as *worse*
picture: demoting the sprite gave a half-res monster filling the screen (M7 renders 160 columns,
no depth test); the replacement used the sprite's **bounding box**, so the grate vanished in the
transparent margins beside it. The only honest direction left is a **per-row** occlusion test —
**price its `.bss` before writing code**, since the deferred openings cut is what would fund it.

### 9. R2.3 async CD pump — BLOCKED, do not build

Nobody has measured it on console, and the one configuration that *was* measured is a cart build
where the whole path is inert by construction. **Ymir illegal for this entire item** (its CD
model is protocol-level: no seek, no rotation). Blocked on disc C.
**Free correction to fold in now:** `R_LoadBudgetFrame` is called **per view**, not per frame, so
in 4p the 20 ms budget refills four times — 80 ms of allowed stall in a 158 ms frame. Fixing the
comment that claims otherwise is free; changing the cadence is an owner decision between fairness
and a frame-wide stall ceiling.

---

## DEAD — do not re-propose. The LAW, then the number.

| lever | LAW |
|---|---|
| **SCSP DSP as a compute co-processor** (any form) | *The shipped binary is **1.55 % multiply-class instructions against 51 % memory moves**; the hottest loop in the game is 0.71 %. You cannot offload a cache miss to a DSP that cannot branch, cannot divide, cannot chase a pointer, sees only sound RAM, and answers no sooner than 250 µs.* At Doom precision it is **0.27× one master SH-2**. ⚠ Record the honest distinction: unlike the SCU-DSP, **the readback IS legal**. There is simply nothing worth carrying through it. Its one real client is *compression*, not compute. |
| **68EC000 hosting game logic** | *Postage to sound RAM costs the master ~4.1 cycles/byte; a Doom per-mobj decision costs 0.5–3 cycles per byte of the row it reads. **The decision is cheaper than shipping the row.*** Plus: the whole tic domain is **11 ms of a 158 ms 4p frame**. Plus: provable determinism needs an unconditional read-back = **a second presentation fence**. ⚠ "68K inatteignable" is **false** — it is reachable, and that is not why it fails. |
| **MUS sequencer on the 68K as a PERF move** | *`S` = 0 ms on 106 of 106 console frames. There is no time to move.* |
| **Non-VDP1 data in VDP1 VRAM** | *28–33 KB of **uncached** B-bus DRAM that arbitrates against the drawing engine (the manual: **both** stall), against 609–624 KB of cached LWRAM.* And the linker closes it first: the image is a flat `.bin`, so every initialised table ships in it and a runtime copy frees **zero** pool. |
| **Offline floor dicing (SlaveDriver route)** | *The scan is a ~0.5 ms **constant**; the measured cost is ~0.48 ms **per emitted tile**. The bake removes the constant and none of the slope.* And the prize is 8.6 ms in 1p but **3.4 ms in 4p** — the split law at its sharpest. |
| **Cart-conditioned DRP rotation ladder** | *The cartless player is the ladder's **beneficiary**: 8-way rotations put 5 lumps per rotated frame in the working set instead of 3 (~+130 first-sight CD reads per map) on the machine with no cart to hide them.* Scope is 6 %: 8 maps of 132, always one step. |
| Merging collinear same-texture linedefs | *The excess in id maps is **BSP splitting**, not authored fragmentation: +0.0 to +1.5 % on top of a nodebuilder.* |
| Removing decorative two-sided linedefs | *`core/r_bsp.c` returns **before** any clip call: they emit **zero** drawsegs already.* |
| Reducing sector count | *`P` is flat at 14.3–15.3 ms across all four modes while `d` moves 3.8×.* |
| SCSP internal DMA as an HWRAM→sound-RAM engine | *The manual: it transfers only between the SCSP control registers and sound memory, 3 812 B max.* |
| 32-bit stores in the SFX upload path | *The SCSP port is 16-bit; a longword becomes two bus cycles. Same cost.* Unlike VDP2 VRAM, **no widening win here**. |
| Pre-rendered music resident as a loop | *LSA/LEA are 16-bit offsets from SA: 65 536 samples max per slot = **5.9 s** at 11 kHz.* |
| A VDP1 texture ATLAS in the free holes | *VDP1 pattern data has **no stride register**; a sub-rectangle is unaddressable and shears.* |
| "Drop the farthest drawsegs" (as opposed to flattening them) | *Skipping a solid wall leaves solidsegs open and the visplanes behind it unclosed — a **see-through hole**, not a degradation.* What ships bounds the count and flattens. |
| A render-governor rung retargeted to make the picture worse | *The governor's job is to degrade the picture, and a lever that changes nothing visible beats it.* It currently contributes **0.0 ms**: `w0 p0` on 106/106 frames at 6.2 fps, because its target is expressed against `rend` (75.3 ms) inside a 158 ms frame. Retargeting is ~10 lines — **ask the owner whether he wants quality traded away at 6 fps at all** before writing them. |

---

## The witness corpus — which WAD prices which question

`wads_temoins/` holds 18 WADs; `tools/boot_matrix.ps1` builds and pre-flights them (it does
**not** boot them — there is no headless Saturn here).

🔴 **Only 11 of the 18 are IWADs, and only an IWAD builds standalone.** Verified 2026-08-26 by
reading every header: a PWAD carries maps but inherits its textures, so `flatten_textures.py`
dies on *no PNAMES lump* and `build.ps1` aborts — which is exactly what `grid1212` and `HR` did
on this script's first real run. `Doom2HR` / `Doom2SCYTHE` / `Doom2NUTS` **are** the pre-merged
builds of three of the PWADs. **`grid1212` has no merged counterpart**, so it cannot be tested
until one is minted — and ⚠ minting one first requires fixing `tools/merge_wad.py`, which writes
lumps back-to-back while `strip_wad.py` deliberately 4-pads, because an unaligned 32-bit read on
the big-endian SH-2 returns garbage ([[saturn-cart-lump-alignment]]).

| WAD | kind | prices |
|---|---|---|
| `Doom1s` | IWAD | the reference ledger; every split constant we own |
| `Tnt` | IWAD | `vp`/`ds` pressure, the 1p tic-bound spot (MAP20), the 4 maps that degrade rotations — **and where the visplane-pool overflow was actually measured**, so it is the direct before/after for the slice-stride change |
| `Doom2` | IWAD | lazy texture directories (MAP13) |
| `SCYTHE` | PWAD *(ships a PNAMES, so it builds)* | **zone exhaustion** — MAP30 763 KB, MAP29 684 KB, the last hard `I_Error` |
| `Doom2HR` | IWAD (merged) | `ds` / openings demand — use this, not bare `HR` |
| `Doom2SCYTHE` | IWAD (merged) | the merged Scythe |
| `Doom2NUTS` | IWAD (merged) | **expected to fail** — documented, not a regression |
| `grid1212` | PWAD, **no PNAMES** | the visplane pool — **unbuildable as-is**, needs a merge that does not exist yet |
| `HR`, `HRMUS`, `nuts`, `Nuts2` | PWAD, no PNAMES | unbuildable standalone |
| `Nuts3` | PWAD *(has PNAMES)* | builds |

⚠ **Cost, measured**: four WADs **with** `-Repack` took **517 minutes** — the LZSS repack of a
full IWAD dominates, not the compile. `-Repack` is therefore opt-in; a plain sweep still answers
the three questions the script exists for (compiles / pool clears the boot-loop floor / cue is
BOM-free), but the discs it leaves carry a **stale DRP and are not shippable**.

---

## Retired from this document on 2026-08-25 — do not restore

- **"The game runs at ~1/3 speed"** — the *clock* half was fixed 2026-08-17. The *cap* half was
  real but mis-diagnosed: `new_sync = 0` made the `+8` cap unreachable, so the "+2→+8" fix never
  ran a single frame. Now `>= 9`. See [[gametic-slowmotion-tic-cap]] (retracted and rewritten).
- **The two-regimes table** — every magnitude came from a build that no longer exists and from
  per-view instruments now folded to frame sums. Its *conclusion* survives (`Bp` dominant on
  92/100 frames); its numbers do not.
- **"Any contiguous zone allocation over ~32 KB is a lottery"** — disproved: `lg` min 117 KB,
  median 159, max 370 over n=106, zero frames below 40 KB. `split_patches.py` closed it offline.
- **Composite rebuild cost** — killed offline by `flatten_textures.py`; a texture whose columns
  are single-patch never builds a composite.
- **The TNT quarter-sky, the 1 px VDP1 seam** — both shipped.
- **"A wall routed to VDP1 is never drawn"** — the instrument has existed since 2026-08-03 and
  has **never been read**, and two things changed under it since. **Ask the owner whether he
  still sees it on the current build** before spending a capture.
- **Branch state in prose** — git is the source of truth; a doc can only be wrong about it.
