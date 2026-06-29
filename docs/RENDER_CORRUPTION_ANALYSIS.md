# Mimas — Doom II first-frame render: master-stack corruption root-cause analysis

> **STATUS — RESOLVED (shipped 2026-06-23, core `b3acd9f`).** Root cause was the
> **Rank-4** candidate (`spanstart_l[]`/`spanstart[]`/`spanstop[]` sized `[224]` but
> indexed by 0..255 byte `top`/`bottom`); fix = size all three `[256]`. The original
> **#1 suspect (`openings[]` overflow) was DISPROVEN** — its preventive guard (§4b/§5)
> was **never shipped** and is only a latent backstop. §3/§4/§5 below are kept as a
> historical investigation record, demoted accordingly. This is a one-time render
> corruption analysis; it is unrelated to the steady-state render path — for the RBG0
> floor see `VDP2_RBG0_CURRENT_STATE.md` and for VDP1↔NBG1 present sync see
> `VDP1_PRESENT_SYNC_PLAN.md`.

## 0. RESOLUTION (shipped 2026-06-23, core `b3acd9f`) — root cause was Rank 4, not #1

**This crash is FIXED.** The root cause turned out to be the **Rank-4** candidate
below (§2 Rank 4 / "y-axis stack write"), NOT the document's #1 suspect. The
arrays `spanstart_l[]` (on the master's C stack), `spanstart[]`/`spanstop[]` (BSS)
were sized `[SCREENHEIGHT=224]` but are indexed by visplane `top`/`bottom`, which
are **BYTE values 0..255** (the `0xff` column sentinel + `bottom==viewheight`). A
`top`/`bottom` byte in [224..255] drove the index past the array end; the OOB write
past `spanstart_l[]` smashed the master's saved return address (PR) on the stack →
CPU exception on RETURN from the render — exactly the "`R:5-end` printed, fault on
RETURN" signature. **Fix: size all three arrays `[256]`** (`core/r_plane.c:169-170`
`spanstart[256]`/`spanstop[256]`, `:695` `spanstart_l[256]`). Confirmed in the
`b3acd9f` commit message and proven playable on Doom II MAP01 (Ymir).

The document's #1 suspect — `openings[]` overflow in `R_StoreWallRange` — was **not**
the cause; the preventive openings guard (§4b/§5) was **not** shipped. It remains a
valid *latent* defensive backstop (it is the only large unguarded bulk write in the
render path) and could still be added if openings telemetry ever shows a genuine
overflow, but it is not required for the Doom II first-frame crash.

The ranked-hypothesis analysis below is preserved as the historical investigation
record — note that the confirmation plan in §4 was superseded by directly fixing
Rank 4.

---

Branch: feat/multiplayer-xsplit. Build: build/Mimas2.elf + build/Mimas2.map.
Constants in this build: `SCREENWIDTH=320`, `SCREENHEIGHT=224` (core/i_video.h:27-28),
`MAXVISPLANES=256` (Makefile:90, overrides core default 512), `MAXDRAWSEGS=256`
(core/r_defs.h:51), `MAXOPENINGS = SCREENWIDTH*64 = 20480` (core/r_plane.c:148-149),
`MAXVISSPRITES=128` (core/r_things.h:25), **`RANGECHECK` IS defined** (core/doomdef.h:42),
`SAT_VISPLANE_POOL=0` → inline-array visplane_t (core/r_defs.h:432-461).

---

## 1. Crash signature and deduction

Full Doom II (DOOM2.WAD), CD-streaming mode (no RAM cart), loading MAP01. The level
loads (geometry fits the zone). The crash is the FIRST FRAME render:

- Marker row 6 froze at `D:2-preRPV` — we are inside `D_Display`'s call to
  `R_RenderPlayerView`; `D:3-postRPV` (right after the call) never printed.
- Marker row 7 froze at `R:5-end` — `R_RenderViewPass` (core/r_main.c) ran to completion:
  R_Clear*, R_RenderBSPNode, R_DrawPlanes, R_DrawMasked, SAT_RP_END all executed.
- Master SH-2: PC=0x06000952 (BELOW our .text at 0x060040e0 → SGL exception/vector
  region in low HWRAM), PR=0x00000212 (GARBAGE return address).
- Slave SH-2: idle-spinning in SGL workarea.c (NOT faulted).

**Deduction.** The exception fires on the RETURN path out of the render, between
`R_RenderViewPass` finishing and control reaching back into `D_Display`. A render
write-path silently corrupted the master's saved return address (PR) during one of the
render sub-phases; the fault only manifests when a function epilogue pops the garbage PR.
This is **stack / return-address corruption from an out-of-bounds (OOB) write**. The slave
being idle (not faulted) means the corrupting write either ran on the master, OR a slave
write corrupted a *pointer/data* the master later dereferenced — not the slave's own stack.

Two prior bug-classes in the code confirm this failure mode is live here:
core/r_plane.c:176-202 (visplane with `maxx >> SCREENWIDTH` does `pl->top[maxx+1]=0xff`
and stomps the zone heap; guarded by skipping such planes) and core/d_main.c:215-222
(a "rogue slave pixel write" corrupts the gamestate enum; guarded by clamping).

---

## 2. Ranked candidate OOB writes (most → least likely)

### Rank 1 — `openings[]` overflow in `R_StoreWallRange` (UNGUARDED bulk write)
- **Write sites:** core/r_segs.c:999-1000 (masked-midtexture column table:
  `lastopening += rw_stopx - rw_x`), core/r_segs.c:1111-1113
  (`memcpy(lastopening, ceilingclip+start, 2*(rw_stopx-start)); lastopening += …`),
  core/r_segs.c:1119-1121 (`memcpy(lastopening, floorclip+start, …); lastopening += …`),
  plus the deferred write through the derived pointer at core/r_segs.c:678
  (`maskedtexturecol[rw_x] = texturecolumn`).
- **Array:** `short openings[MAXOPENINGS]` = 20480 shorts = 40 KB BSS (core/r_plane.c:148-149),
  reset `lastopening = openings` at core/r_plane.c:410.
- **OOB condition:** `lastopening` is advanced with **NO bounds check at any write site**.
  The only `MAXOPENINGS` test is the post-hoc `#ifdef RANGECHECK` `I_Error` in
  `R_DrawPlanes` (core/r_plane.c:884-886) — it runs in the PLANES phase, *after* the
  BSP/seg walk already overran the array. It can also be *passed* if `lastopening` lands
  back inside range, and it never *prevents* the write. Each two-sided/masked seg burns up
  to ~3× its pixel width in shorts (one maskedtexturecol run + two clip-array memcpys).
- **Why Doom II MAP01, not shareware:** consumption scales with the count of two-sided
  segs with mid/upper/lower (masked) textures and silhouette-bearing segs. Doom II MAP01
  (Entryway) opens on a large multi-level room with the nukage pit, many two-sided windows
  and masked grates/railings; shareware E1M1 starts in a tighter, mostly-solid corridor.
  This is the canonical vanilla Doom II "openings/visplane overflow"; limit-removing ports
  (Boom/PrBoom) fixed it by growing `openings` dynamically — doomgeneric/Chocolate-Doom
  here keeps the **fixed vanilla `SCREENWIDTH*64`** array (no dynamic growth).
- **How it reaches the master stack/return address:** the overflow is a *bulk forward
  write* (two `memcpy`s) past `openings[20479]` into adjacent BSS. From Mimas2.map the
  three pointers `ceilingplane`/`floorplane`/`lastvisplane` sit at **0x060c0420/24/28,
  immediately after `openings` (0x060b6420 → 0x060c0420)**. Overrunning `openings` first
  stomps `lastvisplane`; `R_DrawPlanes`/`R_FindPlane` then iterate / bump-allocate from a
  **wild `visplane_t*`** and write through it with data-driven offsets (`rw_x`, `start`) —
  which can land anywhere in HWRAM, including a saved-PR slot. Equivalently, the per-seg
  `sprtopclip`/`sprbottomclip`/`maskedtexturecol` pointers (set to `lastopening - x`,
  core/r_segs.c:999/1112/1120; type `short*`, core/r_defs.h:327-329) become out-of-array,
  and `R_RenderMaskedSegRange` (inside the pass, before `R:5-end`) reads/writes through
  them at a controlled offset. This is the only large unguarded bulk write in the whole
  render path — exactly the shape that stomps a distant return address and only faults on
  unwind (matches "R:5-end printed, fault on RETURN").
- **Why now:** skipping `R_PrecacheLevel` + `-DMAXVISPLANES=256` (a) removed the earlier
  `Z_Malloc` OOM that aborted before the first render, and (b) relaid the zone/BSS layout,
  so the overrun now lands on a live pointer/return-address instead of dead padding.

### Rank 2 — unclamped slave masked column dest (`R_SlaveDrawColumn` / `R_SlaveFuzzColumn`)
- **Write sites:** core/r_things.c:1042-1043 (`byte *dest = ylookup[s_dc_yl] +
  columnofs[s_dc_x]; … dest += SCREENWIDTH`), fuzz variant core/r_things.c:1067-1073.
  Gated live by `sat_masked_parallel = 1` (src/main.cxx:53; gate read r_things.c:1008).
- **OOB condition:** unlike the master's `R_DrawColumn` (which IS guarded by RANGECHECK,
  core/r_draw.c:116-119, and would `I_Error` cleanly — so the master sprite path is NOT
  the silent culprit), the slave copy has **no `(unsigned)s_dc_x >= SCREENWIDTH ||
  s_dc_yl < 0 || s_dc_yh >= viewheight` check**. `ylookup[MAXHEIGHT]`/`columnofs[MAXWIDTH]`
  (core/r_draw.c:63-64) are indexed unchecked; a stale/uninitialized `s_mfloorclip`/
  `s_mceilingclip` (core/r_things.c:1085-1086) lets `s_dc_yh` exceed viewheight and walk
  `dest` off the framebuffer. This is the documented "rogue slave pixel write" class
  (core/d_main.c:215-222), already proven to clobber BSS (`gamestate`).
- **Why Doom II, not shareware:** more masked sprites/segs in MAP01's open first view.
- **How it reaches the stack:** framebuffer is `framebuffer[320*224]` in BSS
  (src/dg_saturn.cxx:388 = I_VideoBuffer @ ~0x060d52f0, ends ~0x060ec47c). A column run
  past the FB end writes forward into the heap — but the ~79 KB cushion to the stack top
  (0x060ffc00) makes a *direct* spill to the saved PR implausible; like Rank 1 it more
  likely corrupts an adjacent pointer/global the master later dereferences. Secondary,
  but the same "wild write" mechanism. (Slave runs its OWN stack `rp_plane_slave_stack`,
  core/r_parallel.c:824, so this is data corruption, not the slave's own return address —
  consistent with the slave found idle, not faulted.)

### Rank 3 — `solidsegs[]` overflow in the BSP clip walk (UNGUARDED, tiny array)
- **Write site:** core/r_bsp.c:126 (`newend++` with no check against
  `&solidsegs[MAXSEGS]`), crunch shuffle core/r_bsp.c:179-185, insert shuffle :128-134.
- **Array:** `cliprange_t solidsegs[MAXSEGS]`, **`MAXSEGS=32`** (core/r_bsp.c:89,93) — vanilla.
- **OOB condition:** a view crossing >30 disjoint open spans before they crunch walks
  `newend` past `solidsegs[31]`. No guard (vanilla design).
- **Why Doom II:** MAP01 has far more two-sided/window geometry in the opening view than
  E1M1 → higher disjoint-open-span count during the BSP walk. The most *Doom-II-specific
  by geometry* candidate, but each overrun element is only 8 bytes (`cliprange_t`), so it
  more likely corrupts a neighbouring global than a distant return address. Worth telemetry.

### Rank 4 — y-axis stack write `spanstart_l[t2]=x` / `spanstart_l[b2]=x` in the plane drawer
- **Write sites:** core/r_plane.c:703-704 (`R_DrawVisplanePotato`) and :771-772
  (`R_DrawVisplaneTextured`); global analog `spanstart[t2]=x` at :630-633.
  Live path (`SAT_PLANE_LOCAL=1`, r_plane.c:651; `sat_plane_parallel=1`, src/main.cxx:52
  forces `rp_disabled=1` so the guarded `rp_exec_*` parity executors never run).
- **Array:** `int spanstart_l[SCREENHEIGHT]` = 224 ints = 896 bytes, **on the master's own
  C stack** (master draws its half via `R_DrawPlaneWorklist(0, half)` directly,
  r_plane.c:1114). Indexed by `t2`/`b2` read from `pl->top[x]`/`pl->bottom[x]`, which are
  **`byte`** (0..255, core/r_defs.h:455,459).
- **OOB condition:** `spanstart_l[t2]=x` / `spanstart_l[b2]=x` have NO bounds check before
  the write. If a real `top`/`bottom` byte is in [224..254], the write lands up to
  `(254-223)*4 = 124` bytes **past the on-stack array** — directly onto the caller's saved
  registers / saved PR of `R_DrawVisplane*` / `R_DrawPlanes` / `R_RenderViewPass`. The
  `R_PotatoSpan`/`R_TexturedSpan` `(unsigned)y >= SCREENHEIGHT` guard (r_plane.c:661,724)
  protects only the framebuffer *pixel* write — it does NOT cover the `spanstart_l[]` index
  write, which executes earlier in the enclosing loop. This is the most *direct* stack-write
  mechanism (no pointer indirection needed), and it lands on the master stack by construction.
- **Why it is ranked below openings:** it requires a genuine `top`/`bottom` byte ≥ 224.
  The producer (core/r_segs.c:519-520,537-538) truncates `int→byte` with no clamp, and
  `bottom[]` has NO boundary sentinels (only `top[]` gets `0xff` at minx-1/maxx+1,
  r_plane.c:1061-1062; the per-plane `memset(top,0xff,SCREENWIDTH)` at :504/592 clears top
  only). So a stale/garbage `bottom[minx-1]`/`bottom[maxx+1]` in [224..255] can drive
  `b2`/`b1` OOB. This is plausible and layout-sensitive (explains shareware-safe / Doom II
  + new-layout-trips), but it is a *transient single-int* write versus Rank-1's bulk memcpy,
  and most legitimate top/bottom stay ≤ 191 (viewheight=192). If telemetry shows a real
  top/bottom byte reaching the [224,255] band, this jumps to Rank 1.

### Rank 5 — `maxx >> SCREENWIDTH` x-axis stomp (`pl->top[maxx+1]=0xff`)
- **Write site:** core/r_plane.c:1061-1062, seg producer core/r_segs.c:519-520,537-538.
- **OOB condition:** a visplane reaching the drawer with `maxx >= SCREENWIDTH` would write
  `top[]` far past the inline array. **Already backstopped** by the skip-guard at
  core/r_plane.c:934-946 (`if (pl->minx < 0 || pl->maxx >= SCREENWIDTH) { …; continue; }`).
  The producer-side `rw_x` is clamped via `viewangletox[]` (core/r_main.c:597-598), so this
  is latent, not the trigger. Note a *gap*: `R_StoreWallRange`'s RANGECHECK (r_segs.c:778)
  checks `start>=viewwidth || start>stop` but NOT `stop>=viewwidth` — a too-large `stop`
  would propagate to `pl->maxx`, but the :940 skip-guard catches it before the draw stomp.
  Low likelihood as the silent corruptor.

### Exonerated
- **`drawsegs[256]` (core/r_bsp.c:49):** GUARDED — `R_StoreWallRange_impl` early-returns
  `if (ds_p == &drawsegs[MAXDRAWSEGS]) return;` BEFORE any `ds_p->` write
  (core/r_segs.c:773-775). Overflow = silent seg drop (visual glitch), not memory damage.
- **`vissprites[128]` (core/r_things.c:300):** GUARDED — `R_NewVisSprite` returns
  `&overflowsprite` instead of advancing past the array (core/r_things.c:341-342,337).
  Overflow sprites discarded; no OOB. MAP01 nowhere near 128 on-screen sprites.
- **`visplanes[]` / MAXVISPLANES=256 itself:** NOT a silent corruptor. Both creation sites
  guard with exact `== MAXVISPLANES` and `I_Error` BEFORE the increment/write
  (`R_FindPlane` r_plane.c:485-489; SATURN-added `R_CheckPlane` r_plane.c:576-584; pool
  slice r_plane.c:96-97). Hitting 256 = a CLEAN `I_Error` halt, NOT a garbage-PR fault —
  different signature. The pool also lives in the **low-WRAM zone heap** (`Z_Malloc`,
  r_plane.c:233), so even its documented `top[maxx+1]` overflow stomps the zone heap, not
  HWRAM/stack. **`-DMAXVISPLANES=256` is the UNMASKER, not the trigger:** it removed the
  Z_Malloc OOM and relaid the BSS/zone layout so a pre-existing OOB now lands on live memory.

---

## 3. Original #1 suspect — `openings[]` overflow (DISPROVEN, historical note)

> **DISPROVEN.** This was the lead hypothesis during the investigation, but the real
> cause was **Rank 4** (§2 / §0). The openings overflow never occurred and its guard
> (§4b/§5) was **never shipped**. Retained as a record of the reasoning and as the
> rationale for the *latent backstop* in §5.

The hypothesis was: **`openings[]` overflow via the unguarded `lastopening` advance /
clip-array `memcpy`s in `R_StoreWallRange` (core/r_segs.c:999-1000, 1111-1113,
1119-1121).** It was attractive because:
1. It is the **only large unguarded bulk forward write** in the render path (40 KB array,
   two `memcpy`s per masked/silhouette seg) — the right shape to stomp a distant pointer /
   saved-PR region and fault only on unwind (matching `R:5-end` printed, fault on RETURN).
2. Its consumption would scale with Doom II MAP01 geometry (two-sided/masked windows,
   grates, railings, the open nukage room) that shareware lacks — the textbook vanilla
   Doom II openings overflow; this core keeps the fixed vanilla array.
3. The ONLY `MAXOPENINGS` check is post-hoc in `R_DrawPlanes` (r_plane.c:884) — too late
   and not preventive; consistent with `R:5-end` completing without an `I_Error`.
4. The .map proves immediate adjacency: `openings` (0x060b6420→0x060c0420) is directly
   followed by `ceilingplane`/`floorplane`/`lastvisplane` (0x060c0420/24/28), giving a
   clean overflow→wild-pointer→arbitrary-write chain into a saved PR.

The direct fix at Rank 4 (sizing `spanstart_l[]`/`spanstart[]`/`spanstop[]` to `[256]`)
resolved the crash without ever needing to confirm or clamp this path.

---

## 4. Historical confirmation plan (superseded)

> **SUPERSEDED.** A single instrumented Ymir build was *planned* to distinguish the
> openings (#1) and `spanstart_l` (Rank 4) theories via high-water counters. In the end
> Rank 4 was diagnosed directly from the array-size mismatch and fixed without this
> telemetry pass. The counter/guard scaffolding was never shipped; it is kept only as a
> reference for how the limits could be instrumented if a *future* overflow is suspected.
> The peak counters of interest were: `r_openings_peak`/`r_openings_ovf` (vs
> `MAXOPENINGS`), `r_drawseg_peak` (vs `MAXDRAWSEGS`), `r_solidseg_peak` (vs `MAXSEGS`),
> and `r_topbot_peak` — the max `top`/`bottom` byte fed to `spanstart_l[]`, the Rank-4
> tell, which would read **≥224** on a tripping frame (and did, confirming Rank 4).

---

## 5. Latent openings backstop (NOT shipped — defensive only)

> **This is a backstop, not "the fix."** The Doom II crash was fixed at Rank 4 (§0).
> The openings guard below was **never shipped** and is **not required**; it is the only
> large unguarded bulk write in the render path, so it remains a reasonable *latent*
> defensive backstop to add **only if** openings telemetry ever shows a genuine overflow.

If added, the guard would clamp the three `lastopening` write sites
(core/r_segs.c:999-1000, 1111-1113, 1119-1121) so an over-budget seg is **skipped**
rather than overrunning `openings`: masked midtexture dropped; sprite clips fall back to
the full-screen `screenheightarray` / `negonearray` sentinels (core/r_main.c:94-95).
Worst-case visible artifact = a few unclipped sprite edges on an over-budget frame —
never a stack stomp. It mirrors the existing in-class defensive idioms: the `drawsegs`
early-return guard (core/r_segs.c:773-775), the `R_CheckPlane` visplane guard
(core/r_plane.c:576), and the "skip rather than corrupt" pattern of the documented
r_plane.c:185-202 / d_main.c:215-222 guards. It is the minimal, perf-economy form (one
compare per masked/silhouette seg on the common path). Do NOT grow `openings` dynamically
(PrBoom approach) — heavier and unwarranted; only bump `MAXOPENINGS` if telemetry shows
the *legitimate* working set genuinely exceeds 20480.

**Cross-port safety (DoomJo / shared core):**
- The edits are pure C in core/r_segs.c — no C++isms, no unnamed params; compiles clean on
  DoomJo's GCC 9.3 (per CLAUDE.md the core must stay pure C).
- The guard only triggers *beyond* the vanilla limit — i.e. only in the previously-UB
  overflow region. For any frame that fit before, `lastopening - openings <= MAXOPENINGS`,
  the `else` branches run, and behaviour is **byte-identical**. So DoomJo's default
  behaviour is unchanged; it merely gains the same anti-corruption backstop.
- `screenheightarray`/`negonearray` and `MAXOPENINGS` exist identically in both ports
  (shared core), so the fallback compiles and links in DoomJo without new symbols.
- Follow the shared-core workflow: commit in `core/`, push, bump `core` in Mimas, then
  pull into DoomJo (CLAUDE.md "Shared core" section).

### Files cited
core/r_segs.c (519-520, 537-538, 678, 773-775, 999-1000, 1111-1113, 1119-1121, 1134),
core/r_plane.c (64, 148-149, 233, 410, 485-489, 504, 576-584, 592, 630-633, 651, 661,
691, 702-704, 724, 761, 767, 771-772, 884-886, 934-946, 1061-1062, 1114, 1124-1138),
core/r_bsp.c (49, 89, 93, 126, 179-185), core/r_things.c (300, 337-346, 1008, 1042-1043,
1067-1073, 1085-1086), core/r_draw.c (63-64, 116-119), core/r_defs.h (51, 327-329,
432-461), core/r_main.c (94-95, 597-598), core/d_main.c (215-222), core/doomdef.h (42),
core/i_video.h (27-28), src/dg_saturn.cxx (388), src/main.cxx (52-53),
SaturnRingLib/modules/sgl/SRC/workarea.c (26-28), build/Mimas2.map
(openings=0x060b6420, ceilingplane/floorplane/lastvisplane=0x060c0420/24/28,
drawsegs=0x060a3e84, vissprites=0x060c3d78, framebuffer/I_VideoBuffer≈0x060d52f0,
MasterStack=0x060ffc00), Makefile (90).
