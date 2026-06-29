> **STATUS — hardware mechanism reference (§1–§3, §6.5, §7).** This doc's value is the
> VDP2 hardware model: the chip, the VRAM access-cycle/snow law, the per-layer bandwidth
> cost table, and the bank-free register-feature catalog. Those are accurate and load-bearing.
> The RBG0 floor it was originally written to diagnose has since SHIPPED as a clean
> 512x256 8bpp **bitmap** (RBG0_BITMAP=1, commits 19768ca/41dd895): bitmap in A1 + K-table
> in A0, B0=NBG1 framebuffer, B1 free → a B1 sky can coexist (the "floor XOR sky" law is
> LIFTED). Snow was cell-floor cycle-pattern STARVATION, solved by the bitmap (2 reads/dot)
> + manual RDBS=0x0D + parked A0/A1 cycles (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, **in the
> tree, NOT slSynch**). For the live floor reality see **docs/VDP2_RBG0_CURRENT_STATE.md**;
> for VDP1↔NBG1 present sync see **docs/VDP1_PRESENT_SYNC_PLAN.md**. The §4–§6 "two defects /
> sky XOR floor / can't ship" diagnosis below was the 2026-06-19 snapshot and is now
> historical — kept only as a trimmed note.

# VDP2 — architecture, limits, and what Mimas should put on it

Written 2026-06-19 after Romain reported the VDP2 path **broken on real Saturn**:
sky dead (blue / not displayed) + **"snow"** (white pixel bands of varying length
over the whole image). This doc explains *why* — at the hardware level (chip, VRAM,
bandwidth) and the software level (SGL register commit). §1–§3 are the durable
hardware model; §4–§6 are the (now historical) 2026-06-19 diagnosis.

> The original snow/dead-sky diagnosis (§4–§6) has since been resolved in shipped code:
> the RBG0 floor went out as an 8bpp **bitmap** (2 reads/dot) with RAMCTL/CYC committed by
> **direct register write** (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, not slSynch), the
> K-table in its own bank A0, and B1 freed so a sky can coexist. See
> **docs/VDP2_RBG0_CURRENT_STATE.md** for the live floor.

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
  128 KB banks A0/A1 (same for B). Mimas/Jo/SRL all run **4-bank** mode.
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

Mimas's two bitmaps are **8bpp** → **2 char reads each**. Two of them, each alone in
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

## 4. Historical — the 2026-06-19 snow/dead-sky diagnosis (RESOLVED)

> This section diagnosed the original break as **two stacked defects**: (1) a *commit gap*
> — Mimas runs no per-frame `slSynch`, so SGL's shadow RAMCTL/CYC never reached the chip;
> and (2) *bank over-subscription* — a **cell** RBG0 with a VRAM coefficient table claimed
> PN + CP + K = 3 banks, which with the framebuffer left no bank for a hardware sky, and the
> intended layout illegally shared cells + K-table in A1.
>
> **Both are now resolved in shipped code** and the framing is obsolete:
> - The floor ships as an 8bpp **bitmap** (2 reads/dot, no pattern-name read), so it needs
>   only **2 rotation banks** — bitmap in A1, K-table in its own bank A0.
> - RAMCTL (RDBS=0x0D) and the CYC cycle pattern are committed by **direct register write**
>   (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, both in the tree), bypassing SGL's
>   shadow→`slSynch` flush. `slSynch` was tried on HW and made it worse; it stays abandoned.
> - With the bitmap floor dropping the map bank, **B1 is free → a sky can coexist with the
>   floor.** The old "exactly one of {HW sky, RBG0 floor}" / "sky XOR floor" law is **lifted**.
>
> The durable lesson is the §2 failure law (a read not scheduled in the bank holding its data
> → snow) — the snow was bitmap-layer cycle-pattern starvation, cured by the bitmap's lower
> read demand + parked A0/A1 cycles, not by `slSynch`. Live floor: **docs/VDP2_RBG0_CURRENT_STATE.md**.

---

## 5. Convictions — what belongs on VDP2 in Mimas

Principle: **put on VDP2 the surfaces that are large, move coherently, and don't need
the framebuffer's arbitrary per-pixel freedom**. As shipped, the mandatory framebuffer
(NBG1 bitmap) carries the composited 3D view + HUD; the RBG0 floor rides the rotation
engine (bitmap in A1 + K in A0); the sky rides a B1 cell layer; HUD/automap/menus stay
software in the framebuffer (cheap; not worth a bank). The measured offload that made the
floor worth shipping is preserved in §6.5 below. (For the exact shipped bank layout see
**docs/VDP2_RBG0_CURRENT_STATE.md**.)

---

## 6.5 Use-case combinations (what can ride with what)

Reframing question first — **is the sky a bigger REC sink than the floor? No.** The floor
(+ ceiling) visplane fill is large in **nearly every** scene; the sky is **absent
indoors** (only F_SKY1 ceilings/outdoor upper walls) and only grows outdoors — exactly
where the floor is *also* large.

**Hardware A/B (Romain, 2026-06-19) — the floor offload is the single biggest lever**
*(pot0, pre-potato-ship; the numbers below are the original measurement, preserved)*:

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

> The **bitmap** RBG0 floor that shipped (2 banks: bitmap in A1 + K in A0) freed B1, so
> the once-feared "sky XOR floor" 5-banks-for-4 conflict no longer applies — floor + a B1
> sky coexist. See **docs/VDP2_RBG0_CURRENT_STATE.md**. The notes below are the durable
> *value* analysis (what's worth offloading), independent of the resolved bank squeeze.

**The REC win is the SKIP, not RBG0 specifically.** The −17/−26 ms above come from
*not drawing the floor* in software (index-0), not from RBG0 — RBG0 is merely the
prettiest thing to put *behind* the skip. Enhancements worth pursuing on the floor:
- **Progressive distance lighting** — the ideal one. The RBG0 plane recedes with distance
  ≈ one scanline per depth band, so a **per-line colour table (`K_LINECOL`)** on the
  existing coefficient table = Doom's diminishing-light/fog **for free**, fixing the
  "uniform per-sector brightness" limitation. Jo uses exactly
  `slKtableRA(…, K_ON | K_LINECOL)`.
- Dynamic dominant-flat selection (per-frame biggest `(picnum,height)`), ceiling support.
- **Parallax cloud layer (NBG2 cells):** Hexen/Doom-64 eye-candy — **+0 REC** (the renderer
  already skips the sky span). Visual only, not a REC lever.

VDP1 load is **not** reducible via free VDP2 banks: walls (per-column perspective-scaled)
and sprites can't live on an NBG scroll layer — only a rotation plane handles a tilted
surface, and that's the floor. VDP1 relief is a separate problem (quad overdraw) — see
**docs/VDP1_PRESENT_SYNC_PLAN.md** for the present/sync path.

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
- Mimas `src/dg_saturn.cxx`; `docs/VDP2_RBG0_CURRENT_STATE.md` (shipped floor),
  `docs/VDP1_PRESENT_SYNC_PLAN.md` (VDP1↔NBG1 present); memories
  [[doomsrl-rbg0-floor]], [[doomsrl-sky-vdp2]], [[doomsrl-known-bugs]],
  [[saturn-memory-map]].
</content>
</invoke>
