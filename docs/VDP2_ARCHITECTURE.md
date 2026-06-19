# VDP2 — architecture, limits, and what DoomSRL should put on it

Written 2026-06-19 after Romain reported the VDP2 path **broken on real Saturn**:
sky dead (blue / not displayed) + **"snow"** (white pixel bands of varying length
over the whole image). This doc explains *why* — at the hardware level (chip, VRAM,
bandwidth) and the software level (SGL register commit) — and ends with **convictions
+ a capacity budget** for what VDP2 should carry in this port.

TL;DR — two independent defects stack:
1. **Commit gap.** DoomSRL never runs `slSynch`, so the VRAM **cycle pattern** and
   **RAMCTL** that the RBG0 floor needs never reach the chip. Ymir reads SGL's *shadow*
   registers and hides it; the real VDP2 reads the actual registers → the bitmap layers
   starve → snow + dead sky. Same emulator-vs-hardware trap as the SCU-DMA cache bug.
2. **Bank over-subscription.** A cell RBG0 Mode-7 floor with a VRAM coefficient table
   needs **3 whole VRAM banks** (pattern-name, character, coefficient) on top of the 2
   bitmap banks (framebuffer + sky) = **5 bank-claims for 4 banks**. It physically
   cannot fit while the hardware sky is also on. Our layout even puts the cells and the
   K-table in the *same* bank A1, which the hardware forbids.

The hardware **sky-only** config (framebuffer NBG1 + sky NBG0, no rotation) was
hardware-validated and is the safe baseline. **RBG0 is what broke it.**

---

## 1. The chip

VDP2 is the Saturn's **background / scroll-plane compositor**. It is *not* a
framebuffer renderer (that is VDP1). Every display dot, VDP2 fetches background data
from VRAM, runs priority + colour math, and emits the final pixel. The cost of a VDP2
layer is paid by **dedicated hardware at a fixed per-pixel rate** — it does **not**
steal SH-2 time. That is the whole appeal: a layer on VDP2 is "free" CPU-wise.

The catch is that VDP2's budget is **VRAM read bandwidth**, and that budget is small
and rigidly structured. Understanding that structure is the entire game.

| Resource | Size | Notes |
|---|---|---|
| VRAM | **512 KB** = 4 banks × 128 KB | banks **A0, A1, B0, B1** at `0x25E00000 / 20000 / 40000 / 60000` |
| Color RAM (CRAM) | **4 KB** | palette; mode set by RAMCTL bits 12-13. Can *also* host the rotation coefficient table (CRKTE bit) |
| Scroll layers | **NBG0-3** (4 normal) | tile or bitmap; scroll/zoom; colour-depth-limited combos |
| Rotation layers | **RBG0, RBG1** | affine/perspective ("Mode-7"); **very** bandwidth-hungry |
| Priority | per-layer 3-bit | + per-tile / per-sprite special priority |

Layers map to VDP2 internal screens; what you *display* is chosen by `slScrAutoDisp`
(BGON bits). What each layer can *fetch* is decided one level lower, by the **VRAM
cycle pattern** — and that is where everything lives or dies.

---

## 2. The VRAM access-cycle model (the core mechanism)

VDP2 reads VRAM on a fixed schedule locked to the dot clock. Per VRAM bank, per memory
cycle, there is a fixed number of **access timings**:

- **Normal horizontal res (320/352): 8 timings — T0…T7 per bank.**
- **Hi-res (640/704) or exclusive monitor: only 4 — T0…T3.**

Each timing is one VRAM read slot. What happens in each slot is programmed by the
**cycle pattern registers**, one per bank:

```
CYCA0  CYCA1  CYCB0  CYCB1     (each = 8 × 4-bit access codes, one per timing T0..T7)
```

The 4-bit access code in each slot says *what* that bank reads at that timing. There
are **10 access types**; the first ones are schedulable per-bank via the cycle pattern:

| code (typical) | access |
|---|---|
| `0x0-0x3` | NBG0..NBG3 **pattern-name** read |
| `0x4-0x7` | NBG0..NBG3 **character-pattern / bitmap** read |
| `0xC-0xD` | NBG0/NBG1 **vertical cell-scroll** table read |
| `0xE` | CPU / DMA access window |
| `0xF` | **no access** (idle) |

RAMCTL controls the rest of the structure:

- **bank split** (`VRAMD`/`VRBMD` bits 8-9): whether VRAM-A is one 256 KB bank or two
  128 KB banks A0/A1 (same for B). DoomSRL/Jo/SRL all run **4-bank** mode.
- **colour-RAM mode** (`CRMD`, bits 12-13).
- **rotation-data bank select / RDBS** (the per-bank 2-bit `a0/a1/b0/b1` fields): which
  banks feed the **rotation** engine. `CRKTE` (bit 15) = put the coefficient table in
  Color RAM instead of a VRAM bank.

### The failure law (verbatim from the VDP2 manual)

> *"If the VRAM access address specified in the VRAM cycle pattern register is not an
> address within the specified bank, access will not occur and correct screen display
> will not be possible."*

This is the "snow." If a layer's required read is **not scheduled in a timing of the
bank that actually holds its data**, the fetch simply does not happen. VDP2 emits
whatever was last on the bus → **horizontal streaks of stale pixels of varying length**
= exactly the symptom Romain saw. A starved layer does **not** fall back to clean
transparency — it corrupts.

### VDP2 always wins the bus

> *"Read access by VDP2 is always prioritized over read/write access by CPU or DMA;
> a wait cycle may be inserted into the CPU or DMA controller."*

So the more VDP2 layers you light up, the more the SH-2 and SCU get wait-stated when
they touch VRAM. (We don't touch VDP2 VRAM in the hot loop — the framebuffer blit
writes it once/frame — so this is a minor tax for us, but it's why heavy VDP2 use and
heavy CPU VRAM use don't mix on Saturn.)

---

## 3. Bandwidth — what each layer costs in access slots

A layer is displayable only if **all** its required reads fit into the **8 slots** of
the bank(s) holding its data. Costs (normal res, no reduction):

| layer kind | reads needed per dot |
|---|---|
| **Bitmap NBG, 16-col (4bpp)** | 1 char read |
| **Bitmap NBG, 256-col (8bpp)** | **2** char reads |
| **Bitmap NBG, 2048+/RGB** | 4–8 char reads |
| **Cell NBG, 256-col** | 1 pattern-name + 2 char reads (+1 if vertical cell-scroll) |
| Reduction ×1/2, ×1/4 | **multiplies** the pattern-name reads (×2, ×4) |

DoomSRL's two bitmaps are **8bpp** → **2 char reads each**. Two of them, each alone in
its bank, is comfortable (2 of 8 slots used per bank).

### RBG0 is the expensive one

A rotation layer reads from arbitrary VRAM addresses every dot, so it cannot share a
bank's timing with anything else. The manual:

> *RBG0 requires three separate accesses — pattern-name, character/bitmap pattern, and
> coefficient table — and each **occupies the entire timing of one cycle, so only one
> type can be specified for one bank**.*

So a **cell-based RBG0 with a VRAM coefficient table = 3 whole banks**:

| RBG0 read | bank claim |
|---|---|
| pattern-name (map) | 1 whole bank |
| character (cells) | 1 whole bank |
| coefficient table (K) | 1 whole bank — *unless* in Color RAM (CRKTE) |

That is the budget killer (Section 5). A **bitmap** RBG0 drops the pattern-name read
(2 banks), and a **CRAM** coefficient table drops the K bank — but both have their own
costs.

---

## 4. Why ours breaks — the two defects in detail

### Defect 1 — the commit gap (no `slSynch`)

SGL programs VDP2 through **shadow registers** in work RAM and flushes them to the
actual chip **inside `slSynch()`** (its frame sync). RAMCTL (bank split, RDBS) and the
CYC cycle-pattern registers are part of that flush.

DoomSRL **deliberately dropped per-frame `slSynch`** ([[doomsrl-known-bugs]]: it
vblank-caps fps to ~7-12 and its SGL sound-driver tick silenced direct-SCSP SFX; the
freeze is instead handled by `rp_sgl_workptr_reset`). Consequence: **the RBG0 cycle
pattern + RAMCTL are set in the shadows but never written to the chip.** On the real
VDP2 the registers keep whatever SRL's init left → the *added* RBG0 read demand isn't
scheduled → the bitmap NBG reads starve → **snow + dead sky**. Ymir reads SGL's shadows,
so it looked fine in the emulator (and that is the trap behind the "hardware-confirmed
end-to-end" claim in older notes — **treat that claim as unverified**; the committed
`HEAD` has *no* commit of the pattern, only `slScrAutoDisp(... | RBG0ON)`).

**Jo Engine, by contrast, calls `slSynch()` every frame** (`core.c:560/629`,
`console.c:307`) — which is why Jo's RBG0 floor works: Jo's whole frame is built around
`slSynch`, so its shadow registers always reach the chip. DoomSRL is not.

**`slSynch` is NOT an option for us — even one-shot.** It was tried on hardware
(`rbg0_commit_pattern()` = `slScrAutoDisp(maximal) + slSynch()`, once at init): it made
the corruption **worse**, not better, and was reverted ([[doomsrl-rbg0-floor]]). The SGL
transfer/sound-driver tick inside `slSynch` fights SRL's VDP2/back-screen setup and the
async-VDP1 path (the same family as the `slScrTransparent` black-screen trap,
[[doomsrl-sky-vdp2]]). So the commit must **not** go through `slSynch` at all.

The only viable commit path is therefore **direct VDP2 register programming**: write
`RAMCTL` (bank split + RDBS) and `CYCA0/A1/B0/B1` to the actual chip registers ourselves
(Jo's NOSGL `RAMCTL=0x1327` / `CYCxx` constants are the known-good reference values),
bypassing SGL's shadow→`slSynch` path entirely. More work, but it's the one mechanism
compatible with DoomSRL's no-`slSynch` architecture. **Caveat: SGL `slScrCycleSet` also
writes only the shadow** (still needs `slSynch` to flush) — so "manual" here means poking
the memory-mapped registers at `0x25F8000E` (RAMCTL) / the CYC registers directly, not
the SGL helper.

### Defect 2 — bank over-subscription (the layout can't fit)

Current intended layout (`src/dg_saturn.cxx`):

| bank | content | read type |
|---|---|---|
| A0 `0x25E00000` | sky bitmap NBG0 | char (2 slots) |
| B0 `0x25E40000` | framebuffer NBG1 | char (2 slots) |
| A1 `0x25E20000` | RBG0 **cells** *and* **K-table** | char **+** coefficient |
| B1 `0x25E60000` | RBG0 map (+ SRL rpara) | pattern-name |

Two things are wrong:
- **cells + K-table share A1.** Char and coefficient reads *each* need the **entire**
  bank timing — they cannot coexist in one bank. The K-table must be its own bank.
- **There is no 5th bank for it.** Framebuffer (B0) is non-negotiable; sky (A0),
  cells (A1), map (B1) fill the rest. A correct cell-RBG0 needs PN + CP + K = 3 banks,
  which with the framebuffer = 4, leaving **zero** for the hardware sky.

So the full config **HW-sky + framebuffer + cell-RBG0-with-VRAM-K is unsatisfiable**.
The earlier note "2 bitmaps + RBG0 fits at the bank level" was wrong: it counted the
two bitmap banks and the cells+map but **forgot the coefficient table needs its own
bank**.

---

## 5. Capacity budget — the 4-bank ledger

Hard ceiling: **4 banks**. The framebuffer always takes one. Everything else competes
for the other three.

| config | B0 | A0 | A1 | B1 | fits? | committed? |
|---|---|---|---|---|---|---|
| **Framebuffer only** | fb | — | — | — | ✅ trivial | baseline |
| **Framebuffer + HW sky** *(known-good on HW)* | fb | sky | — | — | ✅ | yes (was validated) |
| **+ cell RBG0, VRAM K, keep HW sky** | fb | sky | cells **+K❌** | map | ❌ no K bank | + commit gap |
| **SW sky + cell RBG0, VRAM K** | fb | **K** | cells | map | ✅ exactly 4 | needs commit |
| **HW sky + RBG0, K in Color RAM (CRKTE)** | fb | sky | cells | map | ✅ banks ok | needs commit **+** CRAM/palette conflict risk |

Two readings jump out:

- The **safe, shippable** VDP2 today is **framebuffer + hardware sky** — it fits, it was
  hardware-validated, and it needs no rotation gymnastics. RBG0 is what regressed it.
- To ship the **RBG0 Mode-7 floor**, you must **give up the hardware sky** (software-sky,
  freeing A0 for the K-table) **or** move the coefficient table into **Color RAM** (keeps
  the sky, but risks colliding with the 8bpp palette — unproven here). **You cannot have
  the hardware sky and a VRAM-coefficient RBG0 floor at the same time.** That is a
  hardware law, not a tuning knob.

---

## 6. Convictions — what belongs on VDP2 in DoomSRL

Principle: **put on VDP2 the surfaces that are large, move coherently, and don't need
the framebuffer's arbitrary per-pixel freedom** — and spend the scarce VRAM banks on
the highest payoff. Ranked:

1. **Framebuffer (NBG1 bitmap) — mandatory.** The composited 3D view + HUD. 1 bank. Not
   negotiable; this is the screen.

2. **Sky (NBG0 scroll) — the ideal *cheap* VDP2 use, and the safe fallback — but it
   yields to the floor.** A single wrapping backdrop, scrolls with yaw, always behind
   everything, 1 bitmap bank. It's the *known-good* hardware config (fb + sky, no RBG0)
   and the right thing to ship if the floor isn't ready. **But** its REC saving is small
   (absent indoors) and the floor's is large (§6.5), so when only one fits, the **floor
   wins** and the sky drops to software (cheaply, on the freed slave).

3. **RBG0 Mode-7 floor — the biggest single lever; take it, drop the HW sky.**
   Hardware A/B (§6.5) puts the floor offload at **+20 to +33 % fps (−17 to −26 ms)** —
   far above any other VDP2 option. It is the one VDP2 feature that's genuinely
   bank-hungry and it **mutually excludes the hardware sky** on a 4-bank budget, but at
   that price the trade is clearly worth it: software sky (sky is small/rarely-visible and
   its cost lands on the now-idle slave) in exchange for the floor on VDP2. Requires the
   cycle pattern committed by **direct register write** (not slSynch). This is the
   recommended target config.

4. **Status-bar / HUD background, automap, menus — NO (low value).** Could be NBG
   layers, but they're already composited cheaply in the framebuffer and would burn
   scarce banks for little gain. Leave them software. NBG3 cell text stays a dev-only
   overlay (and note it shares B1 with the RBG0 map → can't show with the floor).

5. **Future multi-layer parallax sky (clouds) — PARKED.** Needs a 2nd free NBG bank;
   only affordable in the *sky-without-RBG0* world (A1/B1 free). Mutually exclusive with
   the floor for the same bank-budget reason. ([[doomsrl-sky-vdp2]] future idea.)

### The decision tree

```
Is the RBG0 floor's measured hardware CPU win  >  the cost of losing the HW sky?
│
├─ NO  → ship  Framebuffer + HW sky  (known-good; drop RBG0). ← safe default TODAY
│
└─ YES → ship  Framebuffer + SW sky + RBG0 floor
         requires: (a) K-table in its own bank A0 (4-bank fit), AND
                   (b) commit RAMCTL/CYC by DIRECT register writes — NOT slSynch
                       (even one-shot slSynch was HW-tested worse), NOT slScrCycleSet
                       (shadow-only). Poke RAMCTL @0x25F8000E + CYCxx, Jo 0x1327 style.
         optional: keep HW sky only if the coefficient table goes in Color RAM
                   (CRKTE) without breaking the 8bpp palette — unproven, investigate.
```

### Capacity verdict

**We have the capacity for exactly one of {hardware sky, RBG0 Mode-7 floor} on top of
the mandatory framebuffer — not both** (with a VRAM coefficient table). The 4-bank VRAM
ceiling is the binding constraint; the missing per-frame `slSynch` commit is the second,
orthogonal blocker that breaks *any* hand-rolled cycle pattern on real hardware. Both
must be respected for VDP2 to display correctly on a Saturn.

---

## 6.5 Use-case combinations (what can ride with what)

Reframing question first — **is the sky a bigger REC sink than the floor? No.** The floor
(+ ceiling) visplane fill is large in **nearly every** scene; the sky is **absent
indoors** (only F_SKY1 ceilings/outdoor upper walls) and only grows outdoors — exactly
where the floor is *also* large.

**Hardware A/B (Romain, 2026-06-19) — the floor offload is the single biggest lever:**

| | frame | fps | master EX | slave |
|---|---|---|---|---|
| software floor (today) | 100 ms | 10.0 | 15.6 | 46 % busy |
| no floor (≈ what VDP2 buys) | 74–83 ms | 12.0–13.3 | 2.9 | **100 % idle** |

⇒ removing the floor is worth **−17 to −26 ms = +20 to +33 % fps** (high estimate of the
RBG0 prize; the row-13 model's 8–14 ms was conservative). The sky offload was only ever
"fps up", never quantified, and its area is small. **So the floor is decisively the
higher-value, more consistent VDP2 offload** — Romain's instinct is right, the sky is the
*expendable* one. **Bonus:** the floor work also occupies the *slave* (46 % → 0 %); moving
it to VDP2 frees the slave entirely, which can then **absorb the re-added software sky**
(or more VDP1 walls) — so "VDP2 floor + software sky" is even better than the bank ledger
alone suggests: the sky's software cost lands on otherwise-idle silicon.

The 4-bank ledger then forces these combinations:

| combination | banks | verdict |
|---|---|---|
| **fb + cell-RBG0 floor** (B0,A1,A0,K… +B1 map) | **4 / full** | best REC; **software sky**; nothing else fits — enhance the floor *in place* |
| **fb + sky** (B0,A0) — A1,B1 free | 2 + 2 free | known-good; sky is cheap; the 2 free banks buy **eye-candy, not REC** |
| **fb + sky + parallax clouds** (NBG2 cells in A1/B1) | 4 | Hexen/Doom-64 style; **+0 REC** (pure visual) |
| **fb + sky + cell-RBG0 floor** | 5 needed | ❌ impossible |
| **fb + sky + *bitmap*-RBG0 floor** (no PN = 2 banks) | 4 | fits banks, **but** the flat must be pre-tiled into a 128 KB bitmap and re-uploaded per dominant-flat/light change (~heavy vs 4 KB cells) → rejected on bandwidth |
| **fb + sky + cell-RBG0, K-table in Color RAM** (CRKTE) | 4 | the only "both" that keeps cells; **risk = K-table vs the 8bpp palette in 4 KB CRAM** — unproven, investigate |

**"Sky + X" — the REC win is the SKIP, not the RBG0.** The −17/−26 ms above come from
*not drawing the floor* (index-0), not from RBG0 — RBG0 is merely the prettiest thing to
put *behind* the skip. Put something **cheaper** behind it and you keep ~the same REC win
**and** the hardware sky:
- **Hardware-gradient floor/ceiling (~0 banks).** Skip the floor/ceiling software fill and
  let a **per-line back-screen colour** or the **line-colour screen** show through — a
  vertical gradient shaded by distance (the plane recedes ≈ one scanline per depth band).
  Captures essentially the same +20–33 % (the cost removed is the software fill, which
  both this and RBG0 skip), costs ~no bank → **the hardware sky stays**, and there's **no
  RBG0 cycle-pattern commit to do**. Price = visual: an *untextured* shaded floor (the
  Doom-32X / d32xr flat-floor compromise, [[saturn-perf-references]]) instead of a textured
  perspective floor; a single gradient can't show per-sector flats/heights. This is the
  real sky-only REC lever — the floor-quality-vs-bank-cost axis, not a flat "sky XOR floor".
- **HUD / status bar → its own NBG layer (A1):** lifts the HUD composit + the bottom-32-row
  blit out of the framebuffer path. Small but real.
- **Parallax cloud layer (NBG2 cells):** Hexen/Doom-64 eye-candy — **+0 REC** (the renderer
  already skips the sky span). Visual only, not a REC lever.

VDP1 load is **not** reducible via free VDP2 banks: walls (per-column perspective-scaled)
and sprites can't live on an NBG scroll layer — only a rotation plane handles a tilted
surface, and that's the floor. VDP1 relief is a separate problem (quad overdraw).

**"Floor + X":** all four banks are spent, so X can only be an *enhancement of the floor
itself*, none needing a new bank:
- **Progressive distance lighting** — the ideal one. The RBG0 plane recedes with distance
  ≈ one scanline per depth band, so a **per-line colour table (`K_LINECOL`)** on the
  existing coefficient table = Doom's diminishing-light/fog **for free**, fixing the
  current "uniform per-sector brightness" limitation. Jo uses exactly
  `slKtableRA(…, K_ON | K_LINECOL)`. **This is the recommended next step for the floor.**
- Dynamic dominant-flat selection (per-frame biggest `(picnum,height)`), ceiling support.

**"Sky XOR floor, but dynamic":** swap the *bank layout* at area boundaries —
floor-layout indoors/most scenes, sky(+clouds)-layout in the rare big exterior. Viable,
but needs a runtime RAMCTL/CYC **re-commit by direct register write** at the transition
(cheap if the commit mechanism is solid) and is only worth it if exterior profiling shows
the **sky REC there actually beats the floor REC there** — not assumed, *measured*.
Given the floor is big even outdoors, the swap is a *later* idea, not a default.

### Bank-free VDP2 features worth using (orthogonal to the ledger)

These are **register/colour-math** features — no VRAM bank, usable in *any* config:
- **Colour offset** (COAR/COAG/COAB, per-layer RGB add/sub): the **damage / item-pickup
  flash** and **screen fades** in hardware, instead of re-baking CRAM or blending in
  software ([[doomsrl-known-bugs]] notes a damage-flash spike). Near-free.
- **Line colour screen**: a per-scanline colour over a layer → distance fog on the
  framebuffer 3D view, or the floor gradient above.
- **Colour calculation** (alpha blend between layers): translucency for the sky/clouds
  seam or HUD without software cost.

## 7. Sources

- SEGA Saturn VDP2 User's Manual (Exodus archive): VRAM address map & access cycles —
  `docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/vdp2/hon/` (p01_20 address map,
  p03_xx VRAM/access cycles: 8 timings T0-T7 normal / 4 hi-res; 10 access types; RBG0's
  three whole-bank reads; the "address not in specified bank → no display" law).
- SEGA Saturn Developer's Manual (Exodus archive): VDP2 access priority over CPU/DMA —
  `docs.exodusemulator.com/Archives/SSDDV1R1E/b/b02/hon/p003.htm`.
- Sega Retro, *VDP2*: layer/colour overview.
- r043v `sega.saturn.memory.map/vdp2.h`: RAMCTL fields (rotation bank-select a0/a1/b0/b1,
  vramd/vrbmd, crmd, crkte) and the CYC registers = 8 × 4-bit codes per bank.
- Jo Engine `joengine/jo_engine/vdp2.c` + `jo/sega_saturn.h`: working SGL RBG0 path
  (cells / map / K-table / R-table in **separate** banks; `KTBL0_RAM = VDP2_VRAM_A1`)
  and per-frame `slSynch()` commit (`core.c:560/629`).
- DoomSRL `src/dg_saturn.cxx`; `docs/RBG0_FLOOR_PLAN.md`; memories
  [[doomsrl-rbg0-floor]], [[doomsrl-sky-vdp2]], [[doomsrl-known-bugs]],
  [[saturn-memory-map]].
</content>
</invoke>
