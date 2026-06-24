# Big-WAD CD Streaming on the 2MB Saturn (No Cart) — Deep Analysis

Status: **Phase 0 SHIPPED 2026-06-23** (port `99ced62` + core `b3acd9f`) — Doom II
and all >4MB IWADs now load and play from CD with **no cart**, proven playable on
Doom II MAP01 (Ymir). Phase 1 (robust streaming / bounded LRU cache) is the current
work. See **§0** for what shipped vs. what remains. Below: the original synthesis of
6 parallel investigations (Doom core memory model, DoomSRL RAM map,
SlaveDriver/PowerSlave, d32xr, FastDoom, web survey of PSX-Doom / RP2040-Doom /
Saturn GFS).

---

## 0. Status (2026-06-23)

### Shipped — Phase 0 (Doom II loads + plays from CD, no cart)
- **S1 — skip `R_PrecacheLevel` in CD-streaming mode**: DONE. Gated on the new
  `sat_streaming_mode` global (`core/p_setup.c:747,847`; defined in core so both
  ports link — DoomJo leaves it 0, precache unchanged). Graphics stream lazily as
  self-purging PU_CACHE instead of front-loading the level's whole working set.
- **`MAXVISPLANES` 512→256** (S2 *Change B*, the blunt cut): DONE
  (`Makefile:90` `-DMAXVISPLANES=256`, made `-D`-overridable in `core/r_plane.c`).
  Frees ~166 KB of the 884 KB LWRAM zone. **S2 *Change A* (the `SAT_VISPLANE_POOL`
  span arena) was NOT done** — still on the table as further headroom.
- **libc heap 48→88 KB**: DONE (`src/syscalls.c`) — the full Doom II 2919-lump
  directory fits.
- **Cart guard**: DONE (`src/dg_saturn.cxx`) — refuse to cart-load a WAD bigger than
  the 4 MB cart (its directory is at the end → truncation → black screen) and fall
  back to CD streaming.
- **sfx lump PU_CACHE not PU_STATIC** (the streaming PU_STATIC-leak class, see the
  "Quick freebies" note): DONE (`src/i_sound_saturn.cxx`) — fixed a zone OOM ~1 min
  into Doom II combat (the SCSP-uploaded sfx lump is never re-read, so the PU_STATIC
  copy leaked one block per unique sound).
- **Render-corruption fix** (the Doom II first-frame freeze):
  `spanstart_l`/`spanstart`/`spanstop` sized `[256]` not `[224]`. See
  `RENDER_CORRUPTION_ANALYSIS.md §0`.

### CD-read reliability — "W_ReadLump: only read 0 of N" — fix implemented, pending Ymir
Root cause found: `GFS_Load` (under `SRL::Cd::File::LoadBytes`) **requires a
4-byte-aligned destination** (`GFS_ERR_ALIGN = -21`). `Saturn_Read`'s old tail read
wrote to `buffer + (2048 - offset%2048)`, which is unaligned whenever `offset%2048`
isn't a multiple of 4; GFS rejected it and `Saturn_Read` collapsed the negative code
to `0` → the "only read 0 of N" failure (missing textures/sounds). Fix
(`src/w_file_saturn.cxx`): bounce non-aligned reads sector-by-sector through a
4-byte-aligned buffer (fully-aligned reads keep the one-call fast path), and keep the
WAD file CLOSED so each `LoadBytes` is a churn-free `GFS_Load` (latent perf win).
*Implemented + compiles; awaiting Ymir validation before commit.*

### Phase 1 — bounded LRU texture cache — IMPLEMENTED, pending Ymir
- **z_zone multi-zone extensions** (S4 foundation): `Z_MainZone` / `Z_InitZone` /
  `Z_Malloc2` / `Z_Free2` / `Z_LargestFreeBlock` / `Z_ForEachBlock` added to
  `core/z_zone.{c,h}` on the existing `memblock_t` — additive, the classic
  allocator hot path is untouched.
- **S4(a) bounded LRU composite cache** (`core/r_cache.{c,h}`, new): multi-patch
  wall composites build into a recency-evicted sub-zone (one PU_STATIC slab
  carved from leftover zone *after* geometry, adaptive 128KB-margin / 256KB-cap /
  24KB-min) instead of accumulating in the main zone. Seam = `R_GenerateComposite`
  / `R_GetColumn` (touch on hit, lifecount 3); aged per view in
  `R_PostTexCacheFrame` (`R_RenderPlayerView`); per-level carve/teardown in
  `P_SetupLevel`. Gated `sat_streaming_mode && single-player && !sat_xsplit`, with
  graceful fallback to the classic main-zone PU_CACHE path → worst case is exactly
  today's behaviour; DoomJo (`sat_streaming_mode=0`) is byte-identical.
  Adversarially reviewed (allocator/lifecycle/portability) — clean; the one
  cross-port action is **adding `core/r_cache.c` to DoomJo's makefile** when it
  pulls this revision (else 6 undefined-symbol link errors).
- **S4(b) composite-on-demand `decals[]`** — deferred (heavier structural change);
  the classic `R_GenerateComposite` producer is reused under the LRU instead.
- **Not yet cached**: single-patch wall lumps, flats, sprite patches (still classic
  PU_CACHE) — the same pool can be extended to them in a follow-up.
- **S4(d) per-level disc repack**, **S4(c) per-level bump arena**, **S5 optional
  cart accelerator** — unchanged, future.

---

## 1. Problem statement & hard constraints

We want to run full **Doom II** (14.6 MB IWAD: 3.6 MB sprites, 4.1 MB wall-texture
patches, 0.6 MB flats) plus Ultimate/Plutonia/TNT on a stock Sega Saturn. These
WADs exceed the optional 4 MB RAM cart, so they fall to **CD-streaming mode**
(`src/w_file_saturn.cxx`): the WAD stays on CD, lumps are read on demand via
`SRL::Cd::File::LoadBytes` and **copied into the Doom zone** (`Z_Malloc`).

**Observed failure:** Doom II boots to the menu, then dies at *level load* with
`Z_Malloc: failed on allocation of 8800 bytes`. The trigger is
`R_PrecacheLevel` (`core/p_setup.c:835-836`, gated by `precache=true` at
`core/g_game.c:141`), which loads **all** of a level's graphics into the zone
up-front.

### Hard constraints (non-negotiable)

1. **2 MB work RAM only.** 1 MB HWRAM (SDRAM, shared/contended, fast) + 1 MB
   LWRAM (DRAM, SH-2-only, slower per access). The Doom zone is **884,736 bytes
   (0xD8000)** in LWRAM: `DG_ZoneBase` returns `LOW_WORK_RAM (0x00200000)` with
   size `LOW_WORK_RAM_SIZE(0x100000) - RP_CMD_BUF_SIZE(0x28000)`
   (`src/dg_saturn.cxx:562-566`). The remaining 160 KB of LWRAM is the VDP1
   command buffer (`RP_CMD_BUF`, fixed at `0x002D8000`, `core/r_parallel.h:9-10`).
2. **The cart is OPTIONAL — a potential accelerator, never a requirement.** Cart
   is ~4× slower than work RAM, no burst-write, SCU-DMA cannot write it,
   code-exec prohibited (cold DATA store only). The solution must work with **no
   cart present**.
3. **Proof it is achievable:** Doom *shareware* already streams fine from CD in
   the 884 KB zone with no cart. Commercial Saturn CD games stream hundreds of MB
   through 2 MB RAM. So big-WAD streaming WITHOUT the cart is a solved class of
   problem — we are doing the data model wrong, not hitting a hardware wall.

---

## 2. Diagnosis: why Doom II overflows the 884 KB zone

The 884 KB zone holds three strata at level-play time. Two are resident
(unpurgeable), one is purgeable cache.

### 2.1 The failure is true exhaustion, not fragmentation

`Z_Malloc` **does** purge `PU_CACHE` before failing: its rover loop frees and
coalesces every block with `tag >= PU_PURGELEVEL` and only `I_Error`s after
scanning the whole list (`core/z_zone.c:217-249`, error at :222, purge at
:233-242). So the on-demand graphics caches self-manage. An 8800-byte failure
means: **after purging every purgeable block, no contiguous free run ≥ 8800
bytes remains** — the residue is non-purgeable (`PU_STATIC` + `PU_LEVEL`). That
is exhaustion of the unpurgeable footprint.

### 2.2 Byte budget — what is permanently resident (PU_STATIC, scales with IWAD)

Allocated once at boot by `R_InitData`/`R_InitPlanes`, survives every
`Z_FreeTags(PU_LEVEL)`:

| Allocation | Source | Doom II est. |
|---|---|---|
| **visplane pool** `MAXVISPLANES(512) × sizeof(visplane_t)` | `core/r_plane.c:224`; `visplane_t` carries inline `top[320]+bottom[320]` ≈ 648–664 B, `core/r_defs.h:444-461` | **~332–340 KB** |
| `texturecolumnlump/ofs` per-texture (`width × 4`) | `core/r_data.c:706-707` | **~250–500 KB** ⚠ |
| `texture[]` 8 arrays × numtextures(~1000) | `core/r_data.c:633-639,731` | ~40–60 KB |
| spritewidth/offset/topoffset × numspritelumps(~1300) | `core/r_data.c:775-777` | ~16 KB |
| lumphash × numlumps(~2900) | `core/w_wad.c:564` | ~12 KB |
| sprites[]/spriteframes | `core/r_things.c:211,288` | ~10–20 KB |

The two killers are **visplanes (~340 KB)** and **texturecolumnlump/ofs
(~250–500 KB)**. On Doom II these two PU_STATIC allocations alone can approach or
exceed the entire 884 KB zone *before any level or graphics data*.

### 2.3 Byte budget — per-level geometry (PU_LEVEL, freed/reloaded each map)

For a big Doom II map (~3000 lines), tagged PU_LEVEL, freed at
`core/p_setup.c:769`:

| Lump | Struct | Bytes |
|---|---|---|
| lines (`p_setup.c:402`) | line_t ≈ 80 B | **~240 KB** ⚠ |
| segs (`p_setup.c:184`) | seg_t ≈ 40 B | ~180 KB |
| nodes (`p_setup.c:308`) | node_t ≈ 56 B | ~112 KB |
| sides (`p_setup.c:481`) | side_t ≈ 24 B | ~84 KB |
| sectors (`p_setup.c:273`) | sector_t ≈ 100 B | ~70 KB |
| reject (`p_setup.c:729`) | numsectors²/8 | ~60 KB |
| vertexes/subsectors/blockmap/blocklinks/linebuffer | — | ~130 KB |

**Big-map PU_LEVEL geometry total ≈ 700–900 KB** — by itself rivals the whole
zone. This is the structural floor: even after fixing precache, the very largest
Doom II maps may not fit geometry-only without trimming the resident PU_STATIC.

### 2.4 Byte budget — R_PrecacheLevel (the actual trigger, PURGEABLE)

`R_PrecacheLevel` (`core/r_data.c:924-1037`) loads **all** of a level's flats,
texture patches, and sprite frames, each via `W_CacheLumpNum(..., PU_CACHE)`
(:960, :997, :1031). Doom II working set: ~0.6 MB flats + multi-MB texture
patches + up to 3.6 MB sprites. It cannot all fit — but it is purgeable, so
`Z_Malloc` evicts as it goes. The failure happens because precache front-loads
the *peak* pressure into a zone whose unpurgeable floor (§2.2 + §2.3) is already
~600 KB–1 MB, leaving no room for even an 8800-byte interleaved PU_LEVEL/PU_STATIC
request.

### 2.5 The single biggest lever

**Eliminating R_PrecacheLevel's up-front peak** (make graphics stream lazily as
PU_CACHE) is the #1 lever because it converts an unbounded load-time spike into a
self-purging visible-working-set cache — exactly what shareware already proves
works in 884 KB. The #2 lever is **reclaiming the ~332 KB visplane-pool bloat**,
which is cold scratch with zero render-perf cost.

---

## 3. Ranked solutions (minimal → ambitious)

### S1 — Disable / scope R_PrecacheLevel (graphics stream on demand). **DO FIRST.**

- **Change:** gate `core/p_setup.c:835` so precache is skipped in CD-streaming
  mode. Simplest: set `precache = false` for the streaming path (the flag already
  exists at `core/g_game.c:141`; it is toggled false for demos at :2218-2220, so
  the lazy path is exercised code). Cleaner: condition on `sat_streaming_mode`.
- **What happens:** textures load lazily via `R_GetColumn`→`R_GenerateComposite`
  /`W_CacheLumpNum(...,PU_CACHE)` (`core/r_data.c:398,259`); flats via the span
  drawer; sprites via `core/r_things.c:419,767,1101`. All PU_CACHE → self-purging.
  Resident graphics shrink to the *visible* set.
- **Memory saved:** removes the entire precache peak (multi-MB attempt). Resident
  graphics drop to a few dozen textures + on-screen sprites + ~10 flats (well
  under 100 KB typical).
- **Risk:** CD-thrash. In CD mode a purged-then-revisited lump is re-read via
  `Saturn_Read`/`LoadBytes` (`src/w_file_saturn.cxx:135-185`), slow (~300 ms
  worst-case seek). Playable with occasional hitches on first enemy sighting /
  room entry; not a hard failure. Shareware steady-state already proves the model.
- **Effort:** trivial (1–3 lines). Both d32xr and the PSX/RP2040 ports confirm
  "no precache" is the correct model for constrained RAM.

### S2 — Reclaim the visplane-pool bloat (cold scratch, no perf cost). **DO SECOND.**

- **Change A (structural):** flip `SAT_VISPLANE_POOL=1` (`core/r_defs.h:432`,
  pooled path `core/r_plane.c:226-231`). Moves `top[320]+bottom[320]` out of
  every `visplane_t` into a shared span arena sized to the measured coverage
  peak. visplane_t 664 B → ~28 B. Net resident drop **~150–300 KB**. Pixel output
  byte-identical (comment + Ymir-validated). Already designed and gated.
- **Change B (blunt):** cut `MAXVISPLANES 512→256` (`core/r_plane.c:54`): saves
  ~166 KB. Risk: visplane overflow `I_Error` (`core/r_plane.c:476,567`) on
  plane-heavy open Doom II maps. Safe only paired with the pool. (FastDoom ships
  `MAXVISPLANES=128` and independently chose the same inline-top/bottom cut —
  corroborating, `FastDoom/.../r_plane.c:52`, `r_defs.h:447-451`.)
- **Memory saved:** ~150–300 KB straight into the streaming headroom.
- **Risk:** Change A low (validated); Change B medium without the pool.
- **Effort:** A = flip a flag + size the arena from `r_visplane_coverage_peak`
  (already on the overlay, `src/dg_saturn.cxx:789-791`). Low.

### S3 — Grow the zone within the existing 2 MB (RP_CMD_BUF / HWRAM).

- **B-shrink RP_CMD_BUF:** `RP_CMD_BUF_SIZE = 0x28000` (160 KB, 5120 cmds,
  `core/r_parallel.h:10`). Walls now go to VDP1 so column-command traffic is
  lower than when sized. Halve to `0x14000` (80 KB, 2560 cmds) and reduce the
  subtraction in `DG_ZoneBase` (`src/dg_saturn.cxx:564`) → zone grows to ~944 KB
  (**+80 KB**). Risk: renderer stall/timeout if a frame needs >2560 column cmds
  (`rp_timeout_count` is on the overlay — gate the cut on it staying 0 on the
  busiest scene). Measured-headroom cut, not free.
- **Relocate RP_CMD_BUF to HWRAM:** blocked — HWRAM has only ~41 KB free (the
  88 KB libc heap + code/.bss eat the rest, `src/syscalls.c:43,47-48`); cmd buf
  is hot so cart is out. Not viable; the realistic version is shrink, not move.
- **Move libc lumpinfo off the HWRAM heap:** Doom II's 2919-lump directory is
  ~81.7 KB of the 88 KB libc heap (`src/syscalls.c:45` already anticipates this).
  Moving it to `Z_Malloc` returns ~80 KB to the HWRAM TLSF pool but *adds* ~82 KB
  to the LWRAM zone — trades LWRAM pressure for HWRAM headroom; only useful
  combined with S2.
- **Disable the f_wipe screen-melt:** 2 × 320×224 = ~143 KB PU_STATIC during the
  transition (`core/f_wipe.c:237,249`). Cosmetic; reclaim 143 KB cheaply. **DONE
  in streaming mode (2026-06-23):** the melt Z_Malloc-failed at end-of-level even
  after freeing PU_LEVEL (`Z_Malloc fail 71704 ... lv0K` = the ~620 KB PU_STATIC
  floor fragments the free space below the 70 KB contiguous run the melt needs).
  Now a hard cut when `sat_streaming_mode` (`core/d_main.c` leaving-level branch).
  Bringing back proper start/end fades (no big buffer, no slSynch) is designed in
  **`docs/TRANSITIONS_PLAN.md`** — recommended path is a software CRAM palette
  fade reusing the proven no-slSynch vblank palette-upload seam.
- **Effort:** low–medium. Net ceiling for "grow LWRAM" alone is ~+160 KB
  (RP_CMD_BUF) + ~+200 KB (S2) ≈ effective working set ~1.05–1.2 MB inside 2 MB.

### S4 — Proper CD working-set / streaming engine (the real architecture).

Inspired by SlaveDriver (PowerSlave), d32xr, and PSX-Doom — three independent
2 MB-class machines that stream large games with no cart by treating RAM as a
disposable working set, never a whole-level store.

- **(a) Bounded LRU lump cache instead of zone-copy-per-lump.** Today CD-mode
  `W_CacheLumpNum` Z_Mallocs+copies each lump into the zone. Replace with a
  capped LRU over PU_CACHE slots sized to the *visible* working set, evicting by
  recency. d32xr's `r_cache.c` is the keystone: `R_UpdateCache` walks visible
  viswalls each frame (`d32xr/r_phase9.c:13`), touches cached entries
  (`R_TouchIfInTexCache`, `r_cache.c:102`), adds on miss (`R_AddToTexCache`,
  `r_cache.c:173`), ages+evicts by `lifecount` (3 frames) (`R_EvictFromTexCache`,
  `r_cache.c:127`), with a graceful "no cache, read from source each access"
  fallback (`r_cache.c:580-595`). It is pure C and carves its pool from leftover
  zone (`R_SetupTextureCaches`, `d32xr/r_main.c:570-596`, margin only 8 KB). A
  hit is free; a miss is one CD read amortized across the lifecount — the correct
  CD-streaming model. PowerSlave's equivalent is the `PIC.C` per-frame
  last-use LRU (`map()`/`mapPic`, `PIC.C:250,436`; evict min-`lastUse`
  `PIC.C:271-288`).
- **(b) Composite-on-demand, not composite-at-precache.** Keep composites as base
  patch + small `decals[]` patch list and build columns only when a texture is
  pulled into the cache that frame (d32xr `R_CompositeColumn`, `r_data.c:936`,
  from `r_phase9.c:197`). Avoids ever building composites for unseen textures —
  large saving on texture-heavy Doom II maps. (Note: FastDoom does the OPPOSITE —
  eager `R_GenerateComposite` in precache, `FastDoom/.../r_data.c:891` — and is
  an explicit anti-pattern for us.)
- **(c) Per-level bump arena, reset wholesale on level load.** PowerSlave's
  double-ended stack allocator (`SlaveDriver/UTIL.C:339-405`): area0→HWRAM (fast,
  DMA-reachable; per-frame/decode scratch), area1→LWRAM (bulk level data +
  *compressed* art pool). `mem_init()` per level frees the prior level in O(1)
  with zero fragmentation (`SRUINS.C:1869`); `mem_lock()` pins permanent
  allocations above the reset point (`UTIL.C:355`). This removes the
  fragmentation/exhaustion failure mode under streaming churn.
- **(d) Async prefetched, double-buffered, sequential disc layout.** PowerSlave
  opens ONE file per level, prefetches ~1 MB ahead (`GFS_NwCdRead(...,500*2048)`,
  `FILE.C:163`), drains one sector at a time pumping the async engine
  (`GFS_NwExecOne`, `FILE.C:245`), reads **front-to-back with no seeking**, and
  has a re-seek retry wrapper for dirty discs (`whackCD`, `FILE.C:69`). The big
  win is **disc layout**: each level is one self-contained, sequentially-read
  file. `tools/strip_wad.py` is the natural place to re-pack big WADs into
  level-contiguous, sector-aligned blobs (PSX-Doom's `MAPxx.WAD` /
  `MAPTEXxx.IMG` / `MAPSPRxx.IMG` model — only the subset each map references,
  ~900 KB/level computed offline). The `tileBase` index-relocation trick
  (`SlaveDriver/LEVEL.C:66`) shows how to keep lump indices valid after repack.
- **Effort:** high, staged. (a)+(b) are the high-value pure-C drops shareable
  with DoomJo; (c) is invasive; (d) is a tool change + loader rework. Verify
  whether `SRL::Cd::File` exposes the async `GFS_NwCdRead`-class API (needed only
  for background prefetch in (d); blocking `LoadBytes` suffices for S1–S3 and the
  per-level bulk load).

### S5 — OPTIONAL cart as a pure accelerator (never required).

- **Model:** when a cart is detected, use it as a **zero-copy mapped lump store**
  for WADs that fit (existing CART path, `src/dg_saturn.cxx:623-670`), OR as a
  cold backing store for the streaming LRU's *compressed* lumps so a cache miss
  reads from cart (memory-mapped, ~4× RAM but far faster than CD seek) instead of
  the CD. Cart RAM is memory-mapped → RP2040's "work off the image" applies here
  only.
- **Constraint:** cart cannot hold hot/DMA data (no burst-write, SCU-DMA can't
  write it, code-exec prohibited). The game must run identically with no cart;
  cart only shortens cache-miss latency. The current code already refuses WADs >
  4 MB and falls to streaming (`src/dg_saturn.cxx:646-652,1067`) — keep that, and
  layer cart-as-miss-backing as a detected bonus.
- **Effort:** medium; strictly additive after S1–S4.

### Quick portable freebies (stack with everything)

- Slim `memblock_t`: drop the `id` sentinel, make `tag` a `byte` (FastDoom
  `z_zone.h:52-59`) — ~8 B/block × thousands of lump allocs. Verify `r_parallel.c`
  doesn't rely on the id sentinel first.
- OOM-purge-and-retry seam (FastDoom `Z_MallocEmergency`, `z_zone.c:345-357`) but
  purge `PU_CACHE` instead of only sounds — converts the hard 8800-byte crash
  into graceful eviction.
- Keep `tintmap` (64 KB) and translucency off (never allocated on Saturn).

---

## 4. Recommended plan

### Phase 0 — make Doom II load+play from CD with no cart (smallest change)

1. **S1: skip R_PrecacheLevel in CD-streaming mode** (`core/p_setup.c:835` gated
   on `sat_streaming_mode`, or `precache=false`). This removes the load-time peak
   that throws the 8800-byte error. *Expected: Doom II reaches in-level play;
   graphics stream lazily as PU_CACHE.*
2. **S2: enable `SAT_VISPLANE_POOL=1`** (`core/r_defs.h:432`) and size the arena
   from the measured coverage peak. *Frees ~150–300 KB resident → headroom for
   big-map PU_LEVEL geometry.*
3. If the biggest maps still don't fit geometry-only: add **S3 RP_CMD_BUF shrink
   to 80 KB** (gated on `rp_timeout_count==0`) and/or **MAXVISPLANES→256** (safe
   once the pool is on), and **disable f_wipe** (−143 KB).

This combination (S1 + S2, optionally S3) is the minimal path and is overwhelmingly
likely to make Doom II load and play, because it (a) removes the precache peak and
(b) frees ~150–300+ KB of cold resident scratch — together restoring the
shareware-proven "visible working set fits 884 KB" condition for Doom II's larger
maps.

### Phase 1 — robust streaming (follow-on)

4. **S4(a) bounded LRU lump cache** + **S4(b) composite-on-demand** (port d32xr
   `r_cache.c` / `r_phase9.c` into shared `core/`). Caps resident graphics at the
   visible set with recency eviction; amortizes CD reads. Shareable with DoomJo.
5. **S4(d) per-level disc repack** in `tools/strip_wad.py` (PSX `MAPTEX/MAPSPR`
   model) so each map streams a ~900 KB sequential blob — kills CD-thrash and seek.
6. **S4(c) per-level bump arena** if zone fragmentation under churn proves real.

### Phase 2 — optional cart accelerator

7. **S5**: cart-as-miss-backing for the streaming LRU, detected and additive.

### Validation steps (measure on Ymir, then hardware)

- **Boot + load each Doom II map.** Watch the live overlay: `TLSF u=/f=` (HWRAM
  free, `src/dg_saturn.cxx:1085-1088`), `dg_heap_peak` (libc heap peak,
  `src/syscalls.c:59-60`), zone free, `vp` visplane peak / coverage
  (`src/dg_saturn.cxx:789-791`), `rp_timeout_count`.
- **Confirm zone never `I_Error`s** at load across all 32 maps (MAP07/15/29 are
  the geometry-heaviest stress cases).
- **Measure fps** in steady state and on room transitions / first-enemy
  sightings (where S1's lazy CD reads cost a hitch). Acceptable target: playable
  ~7+ fps console with brief load-in hitches, matching the split-screen baseline.
- **Confirm cart-absent** behavior is identical (force `FORCE_CD_STREAM`,
  `src/dg_saturn.cxx:42`, on a 4 MB system to exercise the no-cart path).
- After S4(a): watch cache hit/miss counters and verify CD reads concentrate at
  load, not steady-state.

---

## 5. Inspiration table — external technique → DoomSRL applicability

| Engine | Technique | DoomSRL applicability |
|---|---|---|
| **d32xr** (32X, ~208 KB zone) | **No R_PrecacheLevel; bounded per-frame LRU texcache (`r_cache.c`) carved from leftover zone, evict by `lifecount`** | **Direct, highest value.** Pure C, drops into shared `core/`. The keystone for S4(a). Miss = CD read amortized over lifecount. |
| d32xr | Composite-on-demand via `decals[]` (`r_data.c:936`), not composite-at-precache | Direct → S4(b). Avoids composites for unseen textures. |
| d32xr | Park math LUTs / setup scratch in framebuffer (`I_TempBuffer`/`I_WorkBuffer`) | Applicable: use off-screen VDP2 / HWRAM scratch instead of zone. Minor. |
| d32xr | Bank-paged ROM, zero-copy `W_GetLumpData` | Maps to DoomSRL CART path (memory-mapped); not the CD path. |
| **SlaveDriver / PowerSlave** (2 MB, no cart) | **Per-level bump arena reset wholesale (`UTIL.C:339-405`, `mem_init`/`mem_lock`)** | Direct → S4(c). area0→HWRAM, area1→LWRAM; O(1) level free, zero fragmentation. |
| SlaveDriver | VDP1 char-VRAM LRU tile cache (`PIC.C` `map()`/`mapPic`, evict min-`lastUse`) | Strong fit — DoomSRL is already a VDP1 wall rasterizer. Future: page textures into VDP1 char RAM, keep only compressed art in LWRAM. |
| SlaveDriver | Async prefetch + per-sector pump + sequential one-file-per-level (`FILE.C`) | Direct → S4(d). Needs SRL async `Cd` API check; blocking `LoadBytes` works for bulk. `whackCD` retry for dirty discs. |
| SlaveDriver | Keep art *compressed* in arena, expand on page | Doom patches are already sparse-column RLE — composite-on-page, never expand all in RAM. |
| **PSX-Doom** (2 MB, slow CD) | **Offline per-level asset split (`MAPxx.WAD`/`MAPTEXxx.IMG`/`MAPSPRxx.IMG`), only the subset each map references (~900 KB/level)** | **Direct → S4(d) tooling.** Extend `tools/strip_wad.py`. The proven Doom-on-2 MB recipe. Audio→SCSP RAM (separate), so doesn't tax LWRAM. |
| PSX-Doom / Jaguar | LZSS lump compression (0x80 name-flag), size derived from next offset | Optional: shrink per-level CD blobs → faster 300 KB/s reads. Doom reader understands the scheme. |
| **RP2040-Doom** (264 KB) | Work off compressed read-only image, never instantiate level data | Cart path only (memory-mapped). CD is not random-access → not directly applicable to streaming. |
| RP2040-Doom | Lean zone: 16-bit ptrs, small-object pools, packed `mobj_t`; renumber textures so per-level subset is low-index | Stacks as headroom freebies; the renumber idea bridges to PSX per-level subset. |
| **FastDoom** | Slim `memblock_t` (drop `id`, byte `tag`); `Z_MallocEmergency` purge-and-retry | Portable freebies. Extend the retry to purge `PU_CACHE`. |
| FastDoom | **Eager `R_GenerateComposite` in precache** | **ANTI-PATTERN — do the opposite.** It is exactly our overflow. |
| Saturn GFS | `GFS_NwCdRead`/`NwExecOne`/`NwGetStat` async ring-buffer | For S4(d) background prefetch — verify SRL exposes the non-blocking variant. |

---

## 6. File:line index

- Zone base / size: `src/dg_saturn.cxx:562-566` (`0xD8000`); cmd buf
  `core/r_parallel.h:9-10` (`RP_CMD_BUF_SIZE 0x28000`).
- Precache: flag `core/g_game.c:141` (`precache=true`), call site
  `core/p_setup.c:835-836`, body `core/r_data.c:924-1037`.
- Zone allocator purge-before-fail: `core/z_zone.c:217-249`.
- Visplane pool: `core/r_plane.c:54` (`MAXVISPLANES 512`), :224 (alloc), :476/:567
  (overflow); struct `core/r_defs.h:432` (`SAT_VISPLANE_POOL`), :444-461 (inline
  top/bottom).
- PU_STATIC tables: `core/r_data.c:633-639,706-707,731,775-777`.
- PU_LEVEL geometry: `core/p_setup.c:130,184,244,273,308,402,481,513,534,729`;
  free `:769`.
- CD streaming read: `src/w_file_saturn.cxx:135-185`; cart-vs-CD branch +
  WAD>cart fallback `src/dg_saturn.cxx:623-670,1024-1071`.
- libc heap / lumpinfo: `src/syscalls.c:43-48,59-60`.
- f_wipe screens: `core/f_wipe.c:237,249`.
- Live telemetry: TLSF `src/dg_saturn.cxx:1085-1088`; visplane peak/coverage
  `:789-791`; heap peak `src/syscalls.c:59-60`.
- External refs: d32xr `saturn-refs/d32xr/{r_cache.c,r_phase9.c,r_main.c:570,r_data.c:936}`;
  SlaveDriver `saturn-refs/SlaveDriver-Engine/{FILE.C,UTIL.C:339,LEVEL.C,PIC.C:250}`;
  FastDoom `saturn-refs/FastDoom/FASTDOOM/{z_zone.h,r_data.c:891}`.
