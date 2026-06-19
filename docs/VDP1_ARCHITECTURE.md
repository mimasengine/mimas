# VDP1 — architecture, limits, and what DoomSRL should put on it

Written 2026-06-19 as the companion to [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md).
DoomSRL now renders **all walls** on VDP1 (the hybrid renderer: VDP1 walls *below*
software NBG1 floors/sprites — the "layer inversion"). This doc is the hardware-level
account of *what VDP1 is*, *what it costs*, *what we do well / badly on it*, and ends
with **convictions + a capacity budget** — including the **2/4-player multiplayer**
question (does the VDP1 load hold up?).

It also corrects several **stale constraints** in older notes that the move to **8bpp
palette wall textures** (commit `c039d63`-era, the `COLOR_4` / CRAM-light-bank build)
made false — see §5.

**TL;DR convictions:**
- **VDP1's binding constraint is not raw fill rate — it is *overdraw* (unbounded
  iteration of off-screen quad pixels) and the *command/texture-cache budget*.** Raw
  fill has huge headroom at our frame times (§3).
- **The 8bpp move was a structural win, not a tweak:** half the VRAM/texture (11→22
  cache slots), lighter texel fetch, exact multiplicative lighting via CRAM banks, and
  it flipped VDP1 from chronic overrun (`VD1 …B`) to finishing with headroom
  (`VD1 …D`). Several old "VDP1 is VRAM/fill-starved" claims are now obsolete (§5).
- **Keep on VDP1:** opaque textured walls (8bpp + CRAM light-banks), flat fallback for
  far/over-budget walls. **Keep OFF VDP1:** half-transparency (6× fill — catastrophic),
  gouraud lighting (additive, wrong on palette banks), floors (visplanes ≠ quads → VDP2
  RBG0), and the **damage/pickup flash** (belongs on VDP2 colour-offset, not VDP1
  gouraud) (§6).
- **Multiplayer holds on the VDP1 side.** Split-screen tiles N viewports into VDP1's
  *single* framebuffer via clip windows; smaller viewports *shrink* per-wall overdraw
  (the close-wall explosion mostly self-disables), and fill stays far under budget. The
  things that must scale are **command-bank size** (cheap VRAM) and **texture-cache
  slots** (8bpp already doubled them). The real N-player wall is **REC on the CPU
  (×N)** — which VDP1 does not address (§7).

---

## 1. The chip

VDP1 is the Saturn's **framebuffer rasterizer** — the "sprite/polygon" processor. It is
the *opposite* of VDP2: VDP2 streams fixed background planes per-dot with zero CPU cost;
VDP1 **draws arbitrary textured quads into a framebuffer** that VDP2 then composites.
Everything DoomSRL puts on VDP1 is paid for in VDP1's own silicon time, fed by SH-2
command lists.

| Property | Value | Consequence for us |
|---|---|---|
| Clock | **~28.64 MHz** (the video clock) | the per-pixel budget below |
| Primitive | **quadrilaterals only** (4 points) | no triangles; a "polygon" is a distorted sprite |
| Sprite kinds | normal / scaled / **distorted** (4 arbitrary corners) | walls = distorted sprites (`FUNC_DISTORSP`) |
| Texture mapping | **forward / affine only** (no perspective correct) | the wall **swim/warp**; mitigate by subdivision |
| Hidden-surface | **no Z-buffer** | software sort / **painter's algorithm** (we paint far→near) |
| Shading | **flat** + **additive gouraud** | gouraud is cheap but *additive* and *breaks on palette textures* (§6) |
| Transparency | half-transparency **(6× cost)** or free **mesh** dither | half-transparency is a trap (§3) |
| Clipping | system-clip + user-clip **window** | clips the **write**, *not* the iteration (overdraw, §3) |
| VRAM | **512 KB** (`0x25C00000`) | commands **+** textures **+** CLUTs share it |
| Framebuffers | **2 × 256 KB**, hardware page-flip | back buffer mapped at `0x25C80000`; max 512×256×16bpp |

The two facts that shape *everything* in our renderer: **no Z-buffer** (so depth is the
CPU's job — the painter's order, the layer inversion, the close-wall handoff) and
**affine-only mapping** (so oblique walls warp and we tile/subdivide).

---

## 2. The drawing model (commands → framebuffer)

VDP1 executes a **command list** in VRAM. Each command is **32 bytes (16 halfwords)**;
contiguous commands auto-advance, a link field can JUMP. It fetches a command table in
**~16 cycles**, then rasterizes its quad into the **back framebuffer**, then moves on.
When it hits an `end` command it sets `EDSR.CEF` (draw-complete) and stops until the
next plot trigger (`PTMR`).

DoomSRL drives this **asynchronously, without `slSynch`** (see
[[doomsrl-vdp1-async]]): the SH-2 builds a list, pokes `PTMR`, and **returns**; VDP1
rasterizes in parallel; a manual framebuffer change at vblank (`FBCR=0x0003`, gated on
`EDSR.CEF`) presents the finished frame without tearing and without an fps tax. The
command list is **double-buffered** (a fixed root command whose 1-halfword link is the
only per-frame write = an atomic, race-free bank flip), and textures live in a
**persistent per-texnum cache** so VDP1 never reads a half-rebuilt texture.

This async, no-`slSynch`, double-buffered, manual-present driver is the **foundation** —
it is what makes VDP1 usable at all in a renderer that dropped SGL's frame sync. Keep it.

---

## 3. The cost model — what a VDP1 pixel costs (the core mechanism)

This is VDP1's analogue of VDP2's cycle-pattern law. VDP1 draws **pixel by pixel**;
each pixel costs VRAM accesses (texture read + framebuffer write, plus optional
gouraud/transparency reads). The community-measured and manual-derived rates:

| Operation | Approx. rate | Cost vs base | Notes |
|---|---|---|---|
| **Flat (untextured) polygon fill** | ~28.6 Mpx/s (~1 cyc/px) | **0.5×** | the cheap primitive — our **flat wall fallback** |
| **Textured / distorted sprite** | ~14.3 Mpx/s (~2 cyc/px) | **1× (base)** | one texel read + one framebuffer write per pixel |
| **+ Gouraud (additive)** | ~near-free on large quads; falls to ~16 Mpx/s on tiny (10×10) quads | ~1× | additive (unlike PS1's multiplicative); *cheap per pixel*, but **per-command overhead hurts small quads** |
| **+ Half-transparency** | **~6× slower** | **6×** | read-modify-write the framebuffer — **the single most expensive thing VDP1 can do** |
| Command table fetch | ~16 cyc/command | — | per-quad fixed overhead → favours **fewer, bigger quads** |

Two numbers govern our design:

**(a) Raw fill is *not* our limiter.** A full 320×224 view = 71,680 px. At the textured
rate that's **~5 ms**; flat-filled, **~2.5 ms**. At a 100 ms (10 fps) frame VDP1 has the
*entire* 100 ms to draw — room for **~20 full textured screens**. So normal wall
coverage (≈1 screen + modest overdraw) uses a few percent of VDP1's time. **This is why,
after 8bpp, the renderer reads `VD1 …D` (finished, ~90 commands of headroom) instead of
the old `…B` (overran).**

**(b) Overdraw is the real limiter.** VDP1's clipping suppresses the *write* but **not
the iteration**: a distorted sprite still steps over **every pixel of its projected
span**, even off-screen ones. A near wall glued to the camera projects to corners at
`y ≈ ±2000` — VDP1 iterates that whole ~4000-row trapezoid even though only ~200 rows
are on screen. *That* is what produced the chronic `…B` overrun ("sky-through-walls" =
late commands dropped), not real coverage. The fixes are all **geometry-bounding**, not
fill-rate tuning:
- **close walls render in SOFTWARE** (core `r_segs.c` magnification fallback, span > 480
  px → CPU column draw, lands in NBG1 *on top* via the layer inversion) — kills the worst
  explosions;
- **vertical sub-quad tiling** (world-anchored bands, cull fully off-screen bands) bounds
  the rest swim-free;
- **flat fallback** for far/over-budget walls guarantees the list always finishes.

> **Lesson, durable:** any bound on VDP1 fill must be **world-anchored**, never
> screen-anchored. Screen-anchored y-clamping bounds the fill but makes textures *swim*
> (the v-coordinate slides through texel rows as distance changes). Reject it. (Memory
> [[doomsrl-vdp1-world-renderer]] pts 5/8.)

---

## 4. Memory — VRAM ledger (512 KB) + framebuffers

VDP1's **512 KB VRAM** holds **commands *and* textures *and* CLUTs** — they compete.
Current DoomSRL layout (`src/dg_saturn.cxx`):

| Region | Addr | Size | Use |
|---|---|---|---|
| Root + empty bank | `0x25C00000` | ~96 B | fixed sysclip+JUMP, atomic link flip |
| Command bank 0 / 1 | `0x25C00100` / `0x25C02100` | 8 KB each | 256 cmds/bank (32 B each), double-buffered |
| **Wall texture cache** | `0x25C05000` | **448 KB** | 16 × 16 KB (narrow ≤128²) + 6 × 32 KB (wide ≤256×128) = **22 slots** |
| (free tail) | `0x25C75000` | ~44 KB | reclaimed (dead weapon/HUD cache + gone gouraud tables) |
| **Framebuffers** | `0x25C80000` | 2 × 256 KB | *separate* from the 512 KB; hardware page-flip |

Key points:
- **Textures dominate VRAM, not commands.** The renderer's coverage is gated by
  **cache slots** (how many distinct visible textures fit), not by command VRAM.
- **8bpp halved per-texel storage** → 22 slots in the same 448 KB (was 11 at RGB555).
  This is the single biggest reason far-room "sky-through-walls" (slot starvation)
  improved and why multiplayer is affordable (§7).
- **Lighting lives in VDP2 CRAM, not VDP1 VRAM.** Wall texels are raw palette indices;
  per-wall light = a CRAM 256-colour **bank** selected by `CMDCOLR` (bank 1 = NBG1's
  live full-bright PLAYPAL; banks 2–7 = PLAYPAL pre-shaded by 6 colormap levels). So
  lighting costs **zero VDP1 VRAM** and the flash re-tints CRAM banks (no texture
  re-bake). See §6.

---

## 5. The 8bpp reframing — old constraints now FALSE

Older notes (and `PERF_REFERENCES.md` / `RENDERER_AUDIT.md`, written when VDP1 was
unused, and the RGB555-era world-renderer log) carried constraints that the 8bpp +
CRAM-light-bank build **invalidated**. Treat these as corrected:

| Old constraint (now stale) | Current reality (8bpp / CRAM banks) |
|---|---|
| "VDP1 is currently unused" (`PERF_REFERENCES`, `RENDERER_AUDIT`) | VDP1 carries **all walls** (one- + two-sided upper/lower), software walls skipped |
| "Only ~11 wall textures fit → far rooms starve → sky-through-walls" | **22 slots** (16 KB narrow + 32 KB wide); wide-tech 256-wide textures now fit; slot starvation largely solved |
| "VDP1 chronically overruns — `EDSR.CEF` reads `…B` always — fill is the binding constraint" | After 8bpp + close-wall CPU fallback + vertical-tiling cull, it reads **`…D`** (finishes, ~90 cmd headroom). **Fill is no longer the wall bottleneck; estimate accuracy + cache slots are.** |
| "Light walls via VDP1 gouraud" / "bake the lit colormap per texnum" | **Neither.** Texels are raw indices; light = CRAM bank via `CMDCOLR` (exact, multiplicative, no re-bake, no "saute" pop) |
| "Damage/pickup flash re-bakes the whole wall cache (slowdown)" | Flash **re-tints CRAM banks only** (`wtex_rebuild_banks`) — no texture re-bake, no spike |
| "RGB textured fill is the cost floor" | 8bpp texels read **2/word** (vs 1/word at RGB555) → **lighter texel fetch** *and* half the VRAM; flat fallback is ~2× cheaper still |

The through-line: **8bpp turned VDP1 from VRAM- and fill-starved into having headroom.**
The remaining wall artefacts are *budget-estimate* and *affine-warp* issues, not raw
capacity.

---

## 6. What DoomSRL does well / badly on VDP1 — and the IN/OUT convictions

### What we do WELL (keep)

1. **Opaque textured walls as distorted sprites** — the core win. Deporting one- and
   two-sided upper/lower walls to VDP1 (skipping their software column draw) cut REC
   and EX hard (memory: REC 110→5–23 ms, Bp 16→3 ms at the good spots) and removed those
   pixels from the software blit. This is the highest-value thing on VDP1.
2. **8bpp + CRAM light-banks** — half VRAM, lighter fetch, **exact multiplicative**
   lighting that matches the software floors/sprites for free (bank 1 *is* NBG1's
   palette), smooth per-wall (no pop), and a **flash that costs a CRAM re-tint** instead
   of a re-bake. The right lighting model for this hardware.
3. **Flat fallback** (`FUNC_Polygon`, ~½ the fill) for far / over-budget / low-detail
   walls — guarantees the command list always finishes → **zero sky-through-walls** as
   the worst case is a flat wall, never a hole.
4. **The async/double-buffered/manual-present driver** (§2) — no fps tax, no tearing,
   coexists with SGL's vblank ISR.

### What we do BADLY / fight (inherent VDP1 limits, mitigated not solved)

1. **Affine warp / "swim"** on oblique walls — forward mapping has no perspective
   correction. Mitigated by u/v sub-quad tiling (world-anchored). Residual swim on
   grazing side-walls is accepted; perspective tile-placement was tried and **reverted**
   (vanishing-point blow-up). 
2. **Overdraw from unclamped near walls** (§3b) — mitigated by the software close-wall
   fallback + vertical cull. The CPU fallback **re-adds** software cost for the 1–2
   screen-filling walls, the most expensive software walls — an accepted trade (rare).
3. **CPU/VDP1 décrochage** — VDP1 walls lag the software floors/sprites by ~1 variable
   frame (VDP1 is kicked late and only swaps when done). Intrinsic to the *hybrid*
   (walls on VDP1, floors on NBG1). The early kick after the BSP walk
   (`sat_walls_done_hook`) and the per-seg 2-frame handoff reduce it; a unified all-VDP1
   world would remove it entirely (the SlaveDriver model) but that's a rewrite.

### What should stay OFF VDP1 (do NOT route here)

1. **Half-transparency — never.** 6× fill (§3). Doom's translucency (spectre, partial
   invisibility, see-through middles) must stay **software** (it's in NBG1 today) or use
   VDP1's **free mesh dither** if a translucent quad is ever wanted — but not real
   half-transparency. Routing translucent world sprites to VDP1 with `CC` on would be a
   self-inflicted 6× fill wound.
2. **Gouraud lighting — no.** It is *additive* (Doom's colormap is *multiplicative* →
   "trop contraste", rejected on hardware) **and** it cannot light a palette/`COLOR_4`
   bank at all (VDP1 applies gouraud to the palette *code* before the CRAM lookup → it
   shifts the index, not the RGB; PLAYPAL isn't luminance-ordered → garbage). CRAM
   light-banks replace it. Gouraud's only remaining value would be a smooth *gradient*
   on an RGB (`COLOR_5`) quad — not our path.
3. **The damage / item-pickup flash — OUT of VDP1.** This is the explicit question. The
   flash must **not** ride VDP1 gouraud (additive + palette-broken, above). Today it's
   done correctly by **re-tinting the CRAM banks** (cheap, no spike). The *ideal* home is
   even cheaper and is **VDP2, not VDP1**: a per-layer **colour offset** (`COAR/COAG/COAB`)
   adds/subtracts an RGB bias across the whole framebuffer in hardware — **one register
   write, zero per-pixel cost, no re-bake of anything**. Same for screen fades. Move the
   flash to VDP2 colour-offset; keep CRAM-bank re-tint as the fallback. (Cross-ref
   [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md) §6 "bank-free features".)
4. **Floors / ceilings — not VDP1.** Doom visplanes are thousands of arbitrary spans,
   not tile-quads (unlike tile-based PowerSlave) → expressing them as VDP1 quads is
   impractical. Floors belong on **VDP2 RBG0** (Mode-7), per
   [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md). VDP1 floors are explicitly rejected.
5. **Weapon / HUD — leave software.** They were on VDP1 (validated the pipeline) but the
   layer inversion forced them back to software (all RGB sprites share one priority, so
   the weapon couldn't stay *above* NBG1 while walls went *below* it). Software
   weapon/HUD is Doom-native and cheap. Low value to re-add; leave it.

### Candidate that COULD come in (but low priority)

- **World sprites (monsters/items) → VDP1.** Reuses the weapon pipeline. *Modest* fill
  win — masked/sprite phase `M` is only **2.5–9 ms** (memory) — and it adds the genuinely
  hard part: **occlusion vs two-sided walls** without a Z-buffer (a depth-painter merge
  of walls+sprites in one far→near list). Given the **perf-economy rule**
  ([[romain-perf-economy]]) and that `M` is small, **sprites→VDP1 is not worth the
  occlusion complexity now.** Revisit only if a full all-VDP1 world rewrite is taken on.

---

## 7. Multiplayer (2 / 4 players) — does the VDP1 load hold?

Split-screen plan: up to 4 local players via the multitap, tiled viewports
(2p = 320×~84 stacked, 4p = 160×~84 quadrants) — see
[`MULTIPLAYER_PLAN.md`](MULTIPLAYER_PLAN.md). The frame model there is
`Frame ≈ N×REC + EX + blit + sim + UI`: **REC multiplies by N**, total EX (pixel fill)
is ~constant (same screen tiled N ways).

**The VDP1 side holds — and is actually a *better* fit in split-screen than single.**
Reasoning from §3–§4:

1. **One framebuffer, N viewports via clip windows.** VDP1 draws into the *single* shared
   framebuffer; each viewport is a system-clip/user-clip rectangle. Split-screen costs
   **no extra framebuffer** and no extra VDP1 chip — just more command lists into regions
   of the same buffer. The CRAM light-banks are global (shared PLAYPAL) → fine for all
   views.
2. **Smaller viewports shrink the overdraw explosion.** The close-wall fill blow-up (§3b)
   comes from walls projecting to `y ≈ ±2000`. In an 84-px-tall viewport a wall can't
   span 480 px, so the **software close-wall fallback rarely fires** — *more* walls go to
   VDP1, which has headroom, and the worst-case per-wall iteration is bounded by the small
   viewport. The thing that hurts single-player most **self-disables** in split-screen.
3. **Raw fill stays far under budget.** Total real wall coverage ≈ one screen regardless
   of N (the viewports tile it). At ~14 Mpx/s textured that's ~5 ms of VDP1 time against a
   150–220 ms 2p/4p frame — **<4 %**. Fill is a non-issue.

**What must scale (and the cost):**

| Resource | Single-player | 2p / 4p need | Action |
|---|---|---|---|
| **Command bank** | ~150 of 256 cmds used | ~2–4× worst case (per-view walls; less in practice — smaller views = fewer/cheaper tiles) | **Grow the banks.** +256 cmds = +8 KB VRAM; ~44 KB free tail + room to trade texture cache. 512–1024 cmds/bank fits. The atomic-flip + flush logic is bank-size-agnostic. |
| **Texture cache** | 22 slots, rarely full | N views see up to N× distinct textures, but **high spatial overlap** (same level) | 8bpp's 22 slots (was 11) is exactly what buys this. If 4 independent views thrash, add per-view slot budgeting or shrink wide pool. The **flat fallback guarantees no sky** under slot pressure regardless. |
| **Framebuffer** | 1 shared | 1 shared (tiled) | none |
| **Fill / overdraw** | the single-player pain | *reduced* per view (point 2) | none — it improves |

**The real N-player wall is REC (×N) on the CPU — VDP1 does not help it.** VDP1 offloads
**EX/fill and the blit**, not the **generation** (BSP + wall-prep + visplane + command
build). That generation is what multiplies by N (FOV stays 90°/view). So the honest
multiplayer verdict: **VDP1 walls scale gracefully to 4 players and even shed their
worst single-player failure mode; the framerate ceiling in split-screen is set by REC×N
on the SH-2s and by `sim`/`UI`, not by VDP1.** Potato/low-detail (flat VDP1 walls = 1
cmd/wall, ~½ fill) is the lever that makes 4-player playable, and it lands *cheaply* on
VDP1 because flat is the cheap primitive.

> Corollary: don't spend effort hardening VDP1 fill "for multiplayer." Spend it on
> **REC reduction** (the ×N term) and on **growing the command bank / cache-slot budget**
> (cheap, mechanical). That's where 2/4-player lives or dies.

---

## 8. Capacity verdict

- **VDP1 has ample raw-fill headroom** at our frame times (§3a); it finishes with margin
  (`VD1 …D`) since 8bpp. The binding constraints are **overdraw** (bounded by the
  software close-wall fallback + world-anchored vertical cull) and the **command/cache
  budget** (bounded by the flat-fallback no-sky guarantee).
- **The 8bpp + CRAM-light-bank model is the correct one for this hardware** — exact
  multiplicative light, no re-bake, half VRAM, lighter fetch — and it retired the
  gouraud and lit-bake approaches for good.
- **Keep ON VDP1:** opaque textured walls + flat fallback. **Keep OFF:**
  half-transparency (6×), gouraud lighting, the flash (→ VDP2 colour-offset), floors
  (→ VDP2 RBG0), weapon/HUD (software). World sprites are a deferred maybe (occlusion
  cost > the small `M` win).
- **Multiplayer (2/4) holds on VDP1**: one framebuffer tiled via clip windows, overdraw
  *shrinks* with viewport size, fill stays <4 % of frame. Scale the **command bank**
  (cheap VRAM) and rely on the **22 cache slots + flat fallback**. The N-player ceiling
  is **REC×N on the CPU**, which VDP1 cannot offload — that is where the multiplayer
  effort belongs.

---

## 9. Sources

- **SEGA Saturn VDP1 User's Manual (ST-013)** — command table format (32 B/command),
  draw modes (normal/scaled/distorted sprite, polygon, polyline), colour modes
  (4bpp CLUT / 8bpp colour-bank / 16bpp RGB), gouraud & half-transparency, system/user
  clip, `TVMR`/`FBCR`/`PTMR`/`EWDR/EWLR/EWRR`/`EDSR` registers, two-framebuffer change.
- **Copetti, *Sega Saturn Architecture: A Practical Analysis*** —
  <https://www.copetti.org/writings/consoles/sega-saturn/> — VDP1 role, forward/affine
  mapping (no perspective), no Z-buffer (SGL Z-sort/painter's), **"half-transparent
  pixels require six times longer to draw"**, two 256 KB framebuffers, 512 KB VRAM.
- **Saturn HW timing threads** — fill rates (~28.6 Mpx/s @16bpp flat / ~14 Mpx/s textured
  ≈ 2 cyc/px @ 28 MHz; ~35.6 Mpx/s @8bpp framebuffer; gouraud ~28 Mpx/s large / ~16 Mpx/s
  10×10; ~16 cyc command fetch): SpritesMind / gendev
  <http://gendev.spritesmind.net/forum/viewtopic.php?t=2868>, SegaXtreme
  <https://segaxtreme.net/threads/saturns-3d-capabilities.24339/>, Beyond3D
  <https://forum.beyond3d.com/threads/questions-about-sega-saturn.58086/>.
- **Yabause/YabaSanshiro** VDP1 notes — framebuffer modes (512×256×16 / 512×512×8 /
  1024×256×8), back buffer at `0x25C80000`: <https://wiki.yabause.org/index.php5?title=VDP1>,
  <https://www.yabasanshiro.com/blog/2026-05-09_vdp1_emulation_with_compute_shader>.
- **SlaveDriver-Engine (Lobotomy, GPL)** — the all-VDP1 world renderer reference:
  `WALLS.C` (frustum near-clip always on = bounded fill), `SPR.C` (command-area cap with
  truncation, double-buffered banks). Local: `C:\Users\pcico\Projects\saturn-refs\SlaveDriver-Engine`.
  See [[slavedriver-vdp1-reference]].
- **DoomSRL** `src/dg_saturn.cxx` (the VDP1 driver, wall cache, flush, light-banks);
  `core/r_segs.c` (the wall hooks, close-wall CPU fallback); memories
  [[doomsrl-vdp1-async]], [[doomsrl-vdp1-world-renderer]], [[doomsrl-vdp2-capacity]],
  [[doomsrl-perf]]; companion [`VDP2_ARCHITECTURE.md`](VDP2_ARCHITECTURE.md).
