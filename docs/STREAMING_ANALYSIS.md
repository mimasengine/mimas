# Big-WAD CD Streaming on the 2MB Saturn (No Cart) — Deep Analysis

Status: **Phase 0 SHIPPED 2026-06-23** (port `99ced62` + core `b3acd9f`) — Doom II
and all >4MB IWADs now load and play from CD with **no cart**, proven playable on
Doom II MAP01 (Ymir). Phase 1 (bounded LRU cache) shipped split-screen-only (§0).
Per-level repack Steps 1–4 + compressed cart load-once are also **code-complete**
(§7.9–7.12, 2026-06-24): the `.DRP` emitter/codec, disc-build integration, the
`P_SetupLevel` per-map blob loader, and the per-map compressed cart store all landed
(several still uncommitted in the core/port working tree). **Current work = hardware
validation** of the cart staging + CD-vs-CDDA path (does not reproduce on Ymir, see
§7.12). Below: the original synthesis of 6 parallel investigations (Doom core memory
model, Mimas RAM map, SlaveDriver/PowerSlave, d32xr, FastDoom, web survey of
PSX-Doom / RP2040-Doom / Saturn GFS).

---

## 0. Status (2026-06-23)

### Shipped — Phase 0 (Doom II loads + plays from CD, no cart)
- **S1 — skip `R_PrecacheLevel` in CD-streaming mode**: DONE. Gated on the new
  `sat_streaming_mode` global (`core/p_setup.c:747,847`; defined in core so both
  ports link — DoomJo leaves it 0, precache unchanged). Graphics stream lazily as
  self-purging PU_CACHE instead of front-loading the level's whole working set.
- **`MAXVISPLANES` 512→256** (S2 *Change B*, the blunt cut): DONE
  (`Makefile` `-DMAXVISPLANES=256`, made `-D`-overridable in `core/r_plane.c`).
  Frees ~166 KB of the 884 KB LWRAM zone. **S2 *Change A* (the `SAT_VISPLANE_POOL`
  span arena): SHIPPED** — `Makefile` `-DSAT_VISPLANE_POOL=1 -DVP_POOL_PLANES=96`,
  plumbed `core/r_plane.c:80-89,256-260` (plane pool carved, `VP_SLICE_BYTES` pairs,
  overflow telemetry `:72`). The pool runs at 96 planes against the 256-plane cap.
  *RECONCILED 2026-06-24: the prior "Change A was NOT done — still on the table" note
  was stale; the pool IS on. Marked SHIPPED.*
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

### CD-read reliability — "W_ReadLump: only read 0 of N" — SHIPPED (`a693a4e`)
Root cause found: `GFS_Load` (under `SRL::Cd::File::LoadBytes`) **requires a
4-byte-aligned destination** (`GFS_ERR_ALIGN = -21`). `Saturn_Read`'s old tail read
wrote to `buffer + (2048 - offset%2048)`, which is unaligned whenever `offset%2048`
isn't a multiple of 4; GFS rejected it and `Saturn_Read` collapsed the negative code
to `0` → the "only read 0 of N" failure (missing textures/sounds). Fix
(`src/w_file_saturn.cxx`): bounce non-aligned reads sector-by-sector through a
4-byte-aligned buffer (fully-aligned reads keep the one-call fast path), and keep the
WAD file CLOSED so each `LoadBytes` is a churn-free `GFS_Load` (latent perf win).
*RECONCILED 2026-06-24: SHIPPED — committed `a693a4e` (the "CD-read retry" in that
commit). The sector-by-sector alignment bounce through a 4-byte-aligned static
`sect_buf` plus the `sat_cd_load` LoadBytes retry wrapper are in
`src/w_file_saturn.cxx:151-222`. Working tree clean.*

### Phase 1 — bounded LRU texture cache — DONE-UNCOMMITTED (split-screen only), pending Ymir
- **z_zone multi-zone extensions** (S4 foundation): `Z_MainZone` / `Z_InitZone` /
  `Z_Malloc2` / `Z_Free2` / `Z_LargestFreeBlock` / `Z_ForEachBlock` added to
  `core/z_zone.{c,h}` on the existing `memblock_t` — additive, the classic
  allocator hot path is untouched.
- **S4(a) bounded LRU composite cache** (`core/r_cache.{c,h}`). *RECONCILED
  2026-06-24: SUPERSEDED in the core working tree (uncommitted, ~50-line
  `r_cache.c` diff). The original design below (single-player gating + adaptive
  128KB-margin / 256KB-cap / 24KB-min carve) is now WRONG.* The cache is now
  **SPLIT-SCREEN ONLY**: `R_SetupTextureCaches()` returns early
  `if (!sat_streaming_mode || sat_local_players <= 1)` (`core/r_cache.c:110`).
  **1p streaming runs CACHELESS** — the classic main-zone PU_CACHE path. The
  adaptive carve is replaced by a **FIXED 96KB slab** (`TEXCACHE_FIXED`,
  `r_cache.c:34-42,120-127`) with a `largest < FIXED+MARGIN` guard. Reason (in
  the code comment): a 1p slab STARVED PU_CACHE + fragmented the zone (worse than
  cacheless); split-screen genuinely needs the contiguous slab to fix the 2p
  composite-fragmentation OOM. Seam still `R_GenerateComposite` / `R_GetColumn`
  (touch on hit, lifecount 3); DoomJo (`sat_streaming_mode=0`) byte-identical.
  **Next action: commit + push core `r_cache.c`, bump core in port.** The one
  cross-port action is still **adding `core/r_cache.c` to DoomJo's makefile** when
  it pulls this revision (else undefined-symbol link errors). Ymir/HW validation
  of the 2p slab path still pending.
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

- **B-shrink RP_CMD_BUF: SHIPPED.** *RECONCILED 2026-06-24: committed, not merely
  proposed.* `Makefile` `-DRP_CMD_BUF_SIZE=0x14000` (80 KB, 2560 cmds) overrides
  the core default `0x28000` (160 KB, `core/r_parallel.h:18`). Walls now go to
  VDP1 so column-command traffic is lower than when sized. Zone subtraction
  `src/dg_saturn.cxx:608` `LOW_WORK_RAM_SIZE - RP_CMD_BUF_SIZE` → zone grows to
  `0x100000-0x14000` = 944 KB (**+80 KB**). Remaining safety gate (not a code
  gap): keep watching `rp_timeout_count` (overlay `to=` must stay 0,
  `dg_saturn.cxx:809`) on the busiest scene — a frame needing >2560 column cmds
  would stall/timeout.
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
  *RECONCILED 2026-06-24: implemented (core working tree, UNCOMMITTED, ~18-line
  diff) — but as a `Z_Malloc` one-shot rover **re-anchor** retry, NOT a PU_CACHE
  purge (the first scan already purges PU_CACHE). `core/z_zone.c:207,231-236`: on
  reaching the start sentinel without a fit, if `!z_emergency` re-anchor
  `mainzone->rover = blocklist.next` and rescan once (`goto z_retry_scan`).
  Rationale: the rover can sit inside the largest free run, so the scan hits its
  own start before spanning a straddling run → false OOM ("lg>=size paradox"). The
  first scan already purged PU_CACHE, so re-anchor recovers the hidden run. Commit
  + push core pending; Ymir/HW validation under streaming churn pending.*
- **R_InitColormaps colormap reclaim** (core working tree, UNCOMMITTED, ~4-line
  diff): `R_InitColormaps` now `W_ReleaseLumpNum(lump)` after `memcpy`'ing the
  colormap to high-WRAM BSS `saturn_cmap` (`core/r_data.c`) — releases the
  now-dead PU_STATIC zone copy (→ PU_CACHE reclaimable), ~8.7K that was leaking
  PU_STATIC every session. Commit + push core pending; verify the DoomJo CMAP256
  path (it also copies colormap to BSS) tolerates the release.
- Keep `tintmap` (64 KB) and translucency off (never allocated on Saturn).

---

## 4. Recommended plan

### Phase 0 — make Doom II load+play from CD with no cart (smallest change)

1. **S1: skip R_PrecacheLevel in CD-streaming mode** (`core/p_setup.c:835` gated
   on `sat_streaming_mode`, or `precache=false`). This removes the load-time peak
   that throws the 8800-byte error. *Expected: Doom II reaches in-level play;
   graphics stream lazily as PU_CACHE.*
2. **S2: `SAT_VISPLANE_POOL=1` — SHIPPED.** *RECONCILED 2026-06-24: no longer a
   to-do; the span arena is on (`Makefile` `-DSAT_VISPLANE_POOL=1
   -DVP_POOL_PLANES=96`, plumbed `core/r_plane.c:256-260`), running at 96 planes.
   Freed ~150–300 KB resident → headroom for big-map PU_LEVEL geometry.*
3. **S3 RP_CMD_BUF shrink to 80 KB — SHIPPED** (`Makefile -DRP_CMD_BUF_SIZE=0x14000`,
   +80 KB zone; keep watching `rp_timeout_count==0`) and **MAXVISPLANES→256 —
   SHIPPED** (safe with the pool on), and **f_wipe disabled in streaming mode**
   (−143 KB). *RECONCILED 2026-06-24: these are committed, no longer conditional
   to-dos.*

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

## 5. Inspiration table — external technique → Mimas applicability

| Engine | Technique | Mimas applicability |
|---|---|---|
| **d32xr** (32X, ~208 KB zone) | **No R_PrecacheLevel; bounded per-frame LRU texcache (`r_cache.c`) carved from leftover zone, evict by `lifecount`** | **Direct, highest value.** Pure C, drops into shared `core/`. The keystone for S4(a). Miss = CD read amortized over lifecount. |
| d32xr | Composite-on-demand via `decals[]` (`r_data.c:936`), not composite-at-precache | Direct → S4(b). Avoids composites for unseen textures. |
| d32xr | Park math LUTs / setup scratch in framebuffer (`I_TempBuffer`/`I_WorkBuffer`) | Applicable: use off-screen VDP2 / HWRAM scratch instead of zone. Minor. |
| d32xr | Bank-paged ROM, zero-copy `W_GetLumpData` | Maps to Mimas CART path (memory-mapped); not the CD path. |
| **SlaveDriver / PowerSlave** (2 MB, no cart) | **Per-level bump arena reset wholesale (`UTIL.C:339-405`, `mem_init`/`mem_lock`)** | Direct → S4(c). area0→HWRAM, area1→LWRAM; O(1) level free, zero fragmentation. |
| SlaveDriver | VDP1 char-VRAM LRU tile cache (`PIC.C` `map()`/`mapPic`, evict min-`lastUse`) | Strong fit — Mimas is already a VDP1 wall rasterizer. Future: page textures into VDP1 char RAM, keep only compressed art in LWRAM. |
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

---

## 7. The load model, music, and the cart — why we stream, and the two ways to get music (2026-06-24)

This section answers "can't we load the map once and free the CD for music?" — the recurring
question once big-WAD streaming works but plays silent. Short answer: **the map's graphics
don't fit RAM (sprites dominate), so we must stream; streaming keeps the CD head busy, so
CDDA can't play during a level. Music without a cart = the SCSP MUS synth (no CD). Music with
the proven CD-free path = a 4 MB cart (load the map once into cart, run cart-resident).**

### 7.1 We do NOT load the whole WAD — and "load just the map's assets once" already failed

The 14.6 MB WAD stays on the CD; only the directory (~47 KB) + touched lumps are resident.
"Load only the map's required assets once" is exactly what vanilla `R_PrecacheLevel` does — it
loads **only** the textures/flats/sprites the map references (`texturepresent[]`/`flatpresent[]`/
`spritepresent[]`), not the whole WAD. **It OOM'd on Doom II** and is disabled in streaming
mode (`p_setup.c` precache gate, S1). The budget says why:

| | KB |
|---|---|
| LWRAM zone | 944 |
| − PU_STATIC floor (texture dir 157 + HUD 80 + visplane pool 67 + sprites 41 + lumphash 11 + MUS 22–65 + …) | ~450 |
| − map geometry (PU_LEVEL, loaded once, resident all level) | 92 (MAP01) … 413 (MAP14) |
| **= RAM left for graphics** | **~80 (big map) … ~400 (small map)** |

A map's **referenced** graphics: flats ~120–200 KB, wall textures ~150–400 KB, and **sprites —
the killer — several hundred KB to >1 MB** (every frame × angle of every monster/item type
placed; the full Doom II sprite set is 3.6 MB and a busy map pulls a big slice). So the map's
required subset is **~0.5–2+ MB against ~80–400 KB available**. It does not fit — by a lot, and
sprites are the dominant reason. Streaming exists precisely to keep only the **visible** subset
resident (~100–200 KB) and re-read the rest from CD as the view changes. (Geometry *is* already
"load once" — it is `PU_LEVEL`, resident for the whole level; only graphics stream.)

### 7.2 Streaming ⇒ the CD head is busy ⇒ CDDA cannot play

The Saturn drive has one laser head: it can read **data** (WAD lumps) or play a **CDDA audio**
track, not both. During play we re-read sprites/textures on demand, so the head is busy →
CDDA would have to seek off the music track for every lump read → music stutters and the read
slows. You cannot cache your way out: CD audio is ~176 KB/s PCM (a 2-min track ≈ 21 MB, can't
cache), and the big WAD is why we stream in the first place. So **any** on-demand CD read
during a level is incompatible with CDDA.

### 7.3 Two music architectures (a runtime switch, both backends compiled in)

> **STATUS — SHIPPED 2026-06-24 (runtime backend switch).** Both backends are now
> compiled in; `src/i_sound_saturn.cxx` dispatches the `I_*Music`/`I_*Song` calls on a
> runtime flag `sat_music_use_cdda` (defined in `core/s_sound.c`, set in `I_InitSound`).
> Predicate: `sat_music_use_cdda = !sat_streaming_mode && cdda_has_audio_tracks()` —
> CDDA only when the WAD is fully resident (CD free) AND the disc has Red Book audio
> (checked via `SRL::Cd::TableOfContents::GetTable()` → `LastTrack.GetType()==Audio`);
> otherwise the **MUS/SCSP synth** plays. `I_InitSound` makes the exclusive HW choice
> (CDDA: 68K running; MUS: 68K halted + waveform upload) from the same flag, decided
> before the SCSP setup. The MUS-lump tag in `s_sound.c` **stays PU_STATIC for both
> backends** (the synth reads it every frame so it must be resident; CDDA only runs with
> the WAD already resident so freeing ~22-65 KB there is pointless, and PU_CACHE would
> risk a purge-vs-`W_ReleaseLumpNum` crash) — so the once-deferred "free MUS" is **moot**
> now that the synth, not CDDA, covers streaming. **Consequence: big-WAD no-cart streaming
> now has music (MUS synth); a cart/small-WAD build with an audio-track disc auto-uses
> CDDA.** The old `-DSATURN_CDDA_MUSIC` is now a dead no-op. *Pending Ymir validation; the
> MUS-synth's per-tick CPU cost on the ~7 fps system is the tradeoff to watch.*

Both backends already exist in `src/i_sound_saturn.cxx`, selected today at *compile* time by
`-DSATURN_CDDA_MUSIC`. The fix is to compile both and select at **runtime**:

- **MUS / SCSP synth** (`#else` branch — currently compiled out). The SCSP synthesizes the
  tiny in-WAD MUS lump (22–65 KB) with **zero CD access** → coexists with streaming. **This is
  the baseline music path for no-cart big-WAD streaming.** Cost: 22–65 KB resident + a per-tick
  CPU cost on the ~7 fps system. (Note: this is why the MUS lump load at `s_sound.c:640` must
  NOT be freed in streaming mode — the synth reads it continuously; "free MUS" applies only to
  the CDDA path, where the bytes are ignored.)
- **CDDA** (current). Hardware Red Book playback, ~0 RAM/CPU, best quality — but needs the
  drive free, i.e. **only when the level is fully resident** (cart, or a small WAD that fits).

Switch: `sat_streaming_mode && !cart` → MUS-synth; otherwise → CDDA (if the disc has tracks).

### 7.4 Per-level disc repack (S4d) — a STREAMING-SMOOTHNESS win, NOT a music fix

**Important:** repack does **not** free the CD for music. It does not shrink the working set;
it reorders the map's lumps into one contiguous, sector-aligned, access-ordered blob so reads
are **fast and seek-light**. Sprites still stream on demand during play → the head stays busy →
CDDA still cannot play. So repack is orthogonal to music — it makes **streaming smoother**
(shorter/fewer hitches), and it **combines** with MUS-synth (repack = fewer hitches, MUS-synth
= music). It is also what makes the cart load (§7.5) fast.

**How it works** (PSX-Doom `MAPTEXxx.IMG`/`MAPSPRxx.IMG` model; PowerSlave one-file-per-level):
1. **Offline, in `tools/strip_wad.py`:** for each map, compute the referenced lumps — textures
   (SIDEDEFS), flats (SECTORS floor/ceiling pics), sprites (THINGS → `mobjinfo` spawn frames),
   the map's own geometry lumps, the map's sounds. Bundle each map's set **contiguously** on the
   disc in roughly access order, sector-aligned.
2. **Index relocation** (SlaveDriver `tileBase` / PSX `LEVEL.C:66` trick): repacking changes
   lump offsets, so the tool rewrites the directory (or emits a per-map offset table) and the
   loader maps original lump indices → repacked positions, so `W_CacheLumpNum` still works.
3. **Loader:** read from the per-map region. Today's on-demand reads then seek **within** a small
   contiguous blob instead of across 14.6 MB → ~one seek/level + sequential reads (vs ~300 ms
   worst-case scattered seeks now). Optional async prefetch ahead (PowerSlave `GFS_NwCdRead`,
   `FILE.C:163`) needs verifying SRL exposes the non-blocking `Cd` API; blocking `LoadBytes`
   already benefits from the sequential layout.

Can it be a **build option?** Yes — the disc can be built as today's single-WAD or as a
per-level-repacked layout; the loader handles both (a flag/marker lump). But it is a *disc-build
+ loader* change (S4d), independent of the music backend (§7.3) — so "repack **instead of** MUS"
is a category error: without a cart you still need MUS-synth for music regardless of repack.

### 7.5 The cart enhancement — load once → CD free → CDDA, AND faster than CD streaming

With a 4 MB extended-RAM cart (see the cart HW notes): at level load, copy the map's referenced
subset (which fits easily in 4 MB) from CD into the cart **once** (one big sequential read — fast
if the disc is repacked, §7.4). During play the engine sources graphics from the cart, so:

- **The CD is idle during play → CDDA music plays clean.** This is the only proven CD-free path.
- **It is faster than CD streaming.** A cart access has no seek and no ~150 KB/s CD bottleneck;
  copying a lump cart→work-RAM is far faster than CD→work-RAM (no ~300 ms seeks). So in-play
  "misses" are cheap → fewer/no hitches.
- **Caveat — cart is COLD store, not render-RAM.** Cart RAM is ~4× slower than work RAM (A-bus,
  16-bit), no burst-write, SCU-DMA cannot write it, code-exec prohibited. Rendering reads texture/
  sprite pixels many times per frame, so you must **not** render per-pixel directly from the cart
  (that is why the COLORMAP was copied OUT of A-bus memory). The model is: cart = the per-map cold
  store (the "disc" replacement); still copy the **visible** subset cart→work-RAM on demand (the
  existing LRU, source = cart not CD). Render from work-RAM (fast); the cart only shortens the
  miss path and frees the drive. This is S5 (cart as miss-backing), now with the music payoff.
- **Driver cost:** SRL's `CartRam` is a stub; a cart read/write driver must be ported (libyaul
  has one). The game must still run identically with **no** cart (cart is a detected bonus).

### 7.6 The resulting matrix

| Config | Graphics during play | Music | In-play speed | Notes |
|---|---|---|---|---|
| **No cart (baseline)** | stream from CD (visible set) | **MUS-synth** (SCSP, no CD) | streaming hitches | + per-level repack (S4d) = fewer hitches |
| **No cart + repack** | stream from a contiguous per-map blob | MUS-synth | smoother (seek-light) | disc-build option; still no CDDA |
| **4 MB cart** | copy per-map subset CD→cart once, LRU cart→work-RAM | **CDDA** (CD idle) | fastest (no seeks) | needs cart driver (SRL stub); repack speeds the load |
| Cart or small WAD fully resident | all resident | CDDA | no streaming | the classic "load once" — only when it fits |

**Bottom line:** streaming is forced by the asset budget (the map's graphics don't fit), so the
no-cart baseline gets music from **MUS-synth**, not from changing the load model. **Per-level
repack** is a worthwhile *smoothness* improvement that stacks with MUS-synth (and accelerates the
cart load) but is not itself a music path. The **cart** is the one way to genuinely free the CD
for CDDA *and* run faster than streaming — a strong optional enhancement, never a requirement.

### 7.7 Music → CD-track mapping (the CDDA path; for different WADs/maps)

This matters **only for the CDDA backend** — a disc built with Red Book audio tracks, played
with the WAD fully resident (cart/small WAD). The **MUS-synth path needs no mapping**: it plays
the in-WAD `d_<name>` MUS lump directly, so any WAD's music "just works" while streaming. For
CDDA, each Doom music number must map to a physical CD audio track (track 01 is the data/WAD
track; audio is 02+), and that mapping is **WAD-specific** (Doom 1 `e1m1…`, Doom II `runnin…`,
PWADs arbitrary).

**Current state (hardcoded):** `cdda_RegisterSong()` in `src/i_sound_saturn.cxx` holds a static
`cdda_track_map[]` (musicnum → track), populated for Doom-1 episode 1 only (`e1m1→2 … e1m9→10`).
It matches by `S_music[i].data == data` to recover the musicnum. This is not data-driven — a
different WAD needs a code edit.

**Proposed data-driven format (the follow-up so builders own it):** a small text file on the
disc, e.g. `cd/data/CDDAMAP.TXT`, one `name track` pair per line:

```
# Mimas CDDA music map: <music-lump-name-without-d_>  <CD-audio-track>
# (only needed when the disc carries Red Book audio + the WAD is RAM-resident)
e1m1   2
e1m2   3
runnin 2     # Doom II MAP01
stalks 3     # Doom II MAP02
```

- **Engine:** at `I_InitMusic` (CDDA path only), read `CDDAMAP.TXT` via `SRL::Cd::File`, and for
  each line resolve the name against `S_music[].name` → fill the musicnum→track table (replacing
  the hardcoded array). Absent/short file → fall back to the built-in default (or, if a name has
  no track, that song is silent — better than a wrong track). Name-keyed so it is WAD-agnostic.
- **Disc builder:** add the audio tracks to the `.cue`, drop a matching `CDDAMAP.TXT` in
  `cd/data/`. Document the track order in the cue ↔ the names here. (A future `tools/strip_wad.py`
  / disc-build step can emit a default `CDDAMAP.TXT` from the WAD's `S_music` table.)

Status: **SHIPPED 2026-06-24.** The loader is implemented in `src/i_sound_saturn.cxx`
(`cdda_load_trackmap`, called from `cdda_InitMusic` only when `sat_music_use_cdda`): it seeds
the built-in Doom-1-E1 default, then parses `CDDAMAP.TXT` (if present on the disc) and overrides
by name (`cdda_name_eq` vs `S_music[].name`). `cdda_RegisterSong` now reads the runtime
`cdda_trackmap[]`. A self-documenting template ships at `cd/data/CDDAMAP.TXT` (all examples
commented → no override → built-in map, so the current data-only disc is unaffected). Dormant
unless CDDA is active (audio-track disc + resident WAD); MUS-synth WADs need no mapping.

### 7.8 Per-map subset — MEASURED (`tools/repack_wad.py`, 2026-06-24)

Step 1 of per-level repack (S4d) is the offline per-map subset computer
(`tools/repack_wad.py` — forks `measure_texfloor.py`'s WAD parser, adds `core/info.c`
parsing for the sprite chain). For each map it computes the **safe superset** of lumps the
map can reference: geometry ∪ textures(SIDEDEFS→PNAMES patches) ∪ flats(SECTORS) ∪
sprites(THINGS→`mobjinfo` state-graph→`SPR_` prefix→all rotations) ∪ spawned `DS*` sounds ∪
an always-on UI/weapons/blood/puff/fog set. It **validates** that every subset lump exists
(a miss in streaming = a hard `I_Error`, not graceful) — Doom II passes clean.

**Measured (Doom II, the on-disc `DOOM1.WAD`, music excluded):** per-map subset =
**1.35–4.09 MB, avg 2.6 MB, worst MAP28 = 4090 KB.** Sprites dominate. This is bigger than the
§7.1 "~0.5–2 MB" rough estimate (which was the *visible* set; this is the full referenceable
superset that must be staged). Two consequences:

- **Per-level repack (streaming): works for every map.** The per-map blob lives on the disc
  (1.35–4.09 MB, contiguous, streamed on demand) — it does NOT need to fit RAM, so size is a
  non-issue; the win is seek-light contiguous reads.
- **Big-WAD cart load-once (S5): fits EVERY map if the cart store is COMPRESSED.** Raw, MAP28
  (~4.09 MB) overflows the 4 MB cart. But the per-map subset zlib-compresses to **1.35→0.65 MB …
  4.09→2.57 MB** (ratio ~48–63 %; sprites/patches compress well). **Worst map = 2.57 MB
  compressed vs the 4 MB cart → all 32 maps fit with ~1.5 MB headroom, CD fully idle → CDDA
  always clean.** This is the PowerSlave/PSX "keep art compressed in the arena, expand on
  page-in" model: the cart holds the compressed subset; the LRU decompresses one lump per
  page-in into work-RAM (a light LZSS/RLE codec, not zlib, for fast SH-2 decode — zlib here is
  only the ratio proxy). So the earlier "partial / fall back to MUS on the heaviest" verdict is
  **superseded** — compression keeps CDDA on every map, and also shrinks the streaming blob (less
  CD traffic) and the disc. Other escape hatches if a codec is unwanted: hot-cart + CD
  cold-fallback with a music fade on the rare cold miss (door/area-transition masking), or exile
  the cold death/raise sprite frames + big-rare lumps to CD-fallback. Compression is the clean
  winner (no music dips, all maps).

### 7.9 Consolidated roadmap (music + streaming + cart)

Dependency-ordered. **Option 1 = compression** (cart/blob holds a light-LZSS-compressed subset,
decompressed per-lump on page-in) and **Option 2 = hot-cart + CD cold-fallback + music fade**
(cart holds the hot set; rare cold misses read from CD, masked by an event-timed music fade) are
folded into the steps below.

**SHIPPED**
- Runtime music backend (MUS-synth ↔ CDDA), split-screen multi-listener SFX, `CDDAMAP.TXT`
  loader (§7.3/7.7). Memory robustness: TXC split-gate + fixed slab, `Z_Malloc` OOM retry,
  COLORMAP leak. Per-level repack **Step 1** = offline subset computer (`tools/repack_wad.py`,
  §7.8), validated; measured subset 1.35–4.09 MB raw / 0.65–2.57 MB LZSS-proxy.

**Per-level repack (foundation — do next, benefits ALL maps, mostly PC/Ymir-testable)**
- **Step 1b — DONE (2026-06-24).** WAD-writer: emits the `.DRP` container — a name-stable
  directory (lump numbers unchanged) + per-map offset table + each subset lump **LZSS-compressed**
  (Option 1, baked in). Serves both the streaming path (less CD traffic) and the future cart store.
  PC round-trip byte-validated (re-read from disk → decode every lump → compare to source WAD).
  See §7.10 for results, the codec spec, and the subset-safety hardening.
- **Step 2 — DONE (2026-06-24).** Disc-build integration: `build.ps1 -Repack` (or `make repack
  build`) emits `cd/data/DOOMRP.DRP` before the ISO step, so the disc carries BOTH the full WAD
  (raw fallback) AND the repacked blobs (`cd/data` is packaged wholesale by `xorrisofs`). Opt-in;
  default build is unchanged (raw). Freshness-checked (regenerates only when WAD/`info.c`/tool
  change). **Marker = the `.DRP` itself** (no side file): the Step-3 loader opens `DOOMRP.DRP`,
  validates magic `DRP1` + `dir_crc32` against the loaded WAD, and uses per-map blobs if it
  matches, else falls back to raw streaming — both layouts stay loadable. See §7.11.
- **Step 3 — DONE (2026-06-24).** `P_SetupLevel` loader (`src/w_drp_saturn.cxx`, Option B,
  name-stable directory → no in-engine index relocation): on map select, retarget into the map's
  blob; **decompress per lump on page-in (Option 1)**; out-of-subset reads fall back to the full
  WAD (miss → today's behaviour, not a crash). Auto-detects the `.DRP` marker; row-21 overlay
  reports state. Ymir-testable for correctness; smoothness gain = hardware.

**Cart load-once (S5 — big-WAD; hardware-only validation). DONE (2026-06-24), see §7.12.**
- **Step 4a — DONE.** Factored `sat_cart_load_region(file, sector, len, cart_ofs)` out of
  `load_wad`'s whole-file copy (`src/dg_saturn.cxx`): one CD→cart primitive (uncached write window
  → `cache_purge` → cached alias), reused by the staging path below.
- **Step 4b — DONE (compressed cart store, Option 1):** at level start, `drp_stage_to_cart`
  (`src/w_drp_saturn.cxx`) stages the map's **compressed** blob CD→cart **once** (≤3.5 MB worst →
  fits 4 MB), reusing the Step-3 `.DRP` per-map offset table as the cart lump table; page-ins then
  LZSS-decode **straight from cart RAM** (no CD, no temp buffer). CD goes idle → **CDDA on every
  map**. Gated `#define SAT_DRP_CART_STAGE 1` (hardware A/B). Auto-engages only with a 4 MB cart
  (`sat_cart_usable`); smaller carts / unfit blobs / not-in-`.DRP` maps stay on the CD path.
- **Step 4c — read-fallback FREE; fade DEFERRED.** The cold-fallback *read* (lump not cart-staged →
  full-WAD CD read) already falls out of Step 3 — no code needed. The **music fade** over that seek
  is the *Transition polish* item below (door/level transitions) and is deferred to that work.
- **Step 4d — DONE.** `sat_cd_free_during_play()` (cart-staged-big-WAD OR `!streaming`) replaces the
  old `!streaming`-only predicate in `I_InitSound`, so CDDA + `CDDAMAP.TXT` auto-enable for
  big-WAD-from-cart. `cdda_has_audio_tracks()` still gates on the disc actually carrying audio.

**Transition polish (Option 2's fade — standalone value, any time)**
- Music fade-out/in on door openings / area transitions / level loads — masks *any* CD access
  (level loads, cold misses), smooth either way. Ties into `TRANSITIONS_PLAN.md`.

**Codec — RESOLVED at Step 1b: LZSS-12/4** (12-bit offset / 4-bit length, THRESHOLD 2, F 18;
window = the output buffer itself, so no ring buffer and a ~20-line byte-wise SH-2 decoder;
PSX/Jaguar precedent). Incompressible lumps are STORED raw (never expand). Real LZSS ratio on
Doom II ≈ 64–80 % (vs the zlib proxy in §7.8); good enough to fit the cart (§7.10). RLE rejected
(worse ratio for ~no decode-cost win on already-byte-wise LZSS).

### 7.10 Step 1b results — the `.DRP` emitter, codec, and subset-safety (2026-06-24)

`tools/repack_wad.py` now emits `cd/data/DOOMRP.DRP` and self-validates. Measured on the 14.6 MB
Doom II IWAD (32 maps):

- **Round-trip: OK** — all 34,108 lump instances decode byte-identical to the source WAD (the tool
  re-reads the container from disk, not just the in-memory buffer). Codec self-test (empty / 1-byte
  / runs / random / overlap) passes.
- **Worst per-map blob = 3523 KB LZSS** (raw 4508 KB) — fits the 4 MB cart with ~570 KB headroom.
  Container on disc ≈ 70 MB (per-map blobs are self-contained → one contiguous read/map; trivial on
  a 650 MB CD).
- **`.DRP` format + the big-endian SH-2 C-loader read discipline** (LE byte-assembly, alignment,
  uint32, the binary-search-by-lump_idx + full-WAD-fallback per-lump contract, dir_crc32) are
  specified in the `repack_wad.py` header — the Step-3 loader is to be transcribed from it.

**Subset-safety (adversarially verified, 3-lens workflow + reconciliation against core sources).**
The static subset must cover every lump the engine references *at runtime*, or that reference takes
the Step-3 loader's full-WAD fallback (a streaming hitch / cart-CDDA music dip). Fixed gaps:
- **Animated flats & textures** (`p_spec.c animdefs[]`) — a referenced frame expands to the whole
  cycle range (NUKAGE1→3, BLODGR1→4, …) by directory/texture index.
- **Switch pairs** (`p_switch.c alphSwitchList[]`) — a referenced `SW1xxx`/`SW2xxx` pulls in its
  partner (swapped on use).
- **Sky textures** (`g_game.c`) — `SKY1/2/3/4` are chosen in code, never named by a SIDEDEF →
  forced into the base set.
- **Code-played sounds** (`ALWAYS_SOUNDS`) — weapons/doors/plats/switches/pickups/HUD DS\* lumps
  have no map THING; they're cached lazily on first play (`I_PrecacheSounds` is a **no-op stub** in
  this port; `I_StartSound`→`cache_sfx`→`W_CacheLumpNum`), so they must be reachable from the blob.
- **Finale background flats** (`f_finale.c textscreens[]`) — chosen by gamemap → all added to base
  (≈24 KB).
- **Intentionally NOT included:** the **MAP30 cast call** (all ~17 monster sprite sets). It is a
  one-time *static* end screen; unioning it pushes MAP30 to ~4.05 MB (eats cart headroom, PWAD-
  fragile). It is served by the Step-3 full-WAD fallback (CD free on a static screen; cart-CDDA
  uses the Option-2 music fade). Gameplay assets stay complete so play is smooth; only the static
  finale falls back.

### 7.11 Step 2 results — disc-build integration & the repacked-vs-raw marker (2026-06-24)

The repack is **opt-in**; the default disc is unchanged (raw streaming).

- **Build hooks.** `build.ps1 -Repack` (Windows) and `make repack build` (CI/non-Windows) both run
  `tools/repack_wad.py <wad> core/info.c --emit=cd/data/DOOMRP.DRP` *before* the ISO step. The
  Windows hook runs after any `-Wad` IWAD swap (so it repacks the IWAD that actually ships) and is
  freshness-checked: it regenerates only when the WAD, `core/info.c`, or the tool is newer than the
  existing `.DRP` (≈30 s repack, skipped otherwise). `--emit` alone skips the analysis report so the
  build does a single compression pass; `--report` re-enables the per-map table.
- **Packaging.** `cd/data` is the SRL assets dir (`ASSETS_DIR` in `shared.mk`), copied wholesale
  into the ISO by `xorrisofs`. Dropping `DOOMRP.DRP` there is all that's needed — no Makefile-iso
  surgery. The disc then holds the full WAD (≈14.6 MB) **and** the `.DRP` (≈70 MB of self-contained
  per-map blobs); ~85 MB total, trivial on a 650 MB CD. `cd/data` is git-ignored, so the `.DRP` is a
  pure build artifact (never committed).
- **The marker = the `.DRP` itself (self-validating, no side file).** The disc *always* carries the
  full WAD, so raw streaming is always possible. The Step-3 loader decides layout by opening
  `DOOMRP.DRP` and checking the header: magic `DRP1`, `codec == 1`, `n_lumps == WAD numlumps`, and
  `dir_crc32 == CRC32(WAD directory)`. **All match → repacked mode** (retarget the subset's
  `lumpinfo[].position` into the blob, decompress per lump on page-in). **Absent or any mismatch →
  raw streaming** (today's behaviour). The CRC fingerprint means a stale `.DRP` (built against a
  different WAD) is auto-rejected rather than mis-read — you can't ship a broken repacked disc by
  accident. (A future loader debug flag could force-raw even when a valid `.DRP` is present, for A/B
  perf measurement.)

**Step 3 + Step 4 both landed (2026-06-24)** — the `P_SetupLevel` loader (`src/w_drp_saturn.cxx`)
that consumes this (auto-detect per the marker above; per-map blob retarget; LZSS page-in; full-WAD
fallback), then the cart load-once layer on top of it. See §7.12.

### 7.12 Step 4 results — cart load-once / per-map compressed cart store (2026-06-24)

**Goal.** Big WADs (Doom II / Ultimate / Plutonia / TNT, >4 MB) can't raw-load into the 4 MB cart,
so they stream from CD — which keeps the CD busy and forces the MUS synth (no CDDA). Step 4 makes
the cart a **per-map compressed store**: at level start, stage that map's LZSS `.DRP` blob CD→cart
**once**, then page lumps in by decoding **straight from cart RAM**. The CD then sits idle during
play → **Red Book CDDA on every map**.

**Implementation (platform-only — ZERO core changes).** The Step-3 `.DRP` hooks
(`sat_drp_select_map`, `sat_drp_read_lump`) were already wired into `core/` under `-DSAT_REPACK`;
Step 4 only changed the SRL platform files, so `core/` stays byte-identical for DoomJo (no submodule
bump, no propagation).
- **4a** `sat_cart_load_region(file, sector, len, cart_ofs)` in `dg_saturn.cxx` — the CD→cart copy
  primitive (uncached write @0x22400000 → `cache_purge` → cached read @0x02400000), factored from
  `load_wad`. `sat_cart_usable` (4 MB cart free for staging; 0 in raw-cart mode) + `sat_cart_cached_base`
  exported.
- **4b** `drp_stage_to_cart` in `w_drp_saturn.cxx` — reads `blob_size` from the map table, stages the
  blob from sector `blob_ofs>>11` (blob byte 0 lands at cart offset `sub = blob_ofs & 2047`), with the
  same 8× CD-read retry as `drp_load`. `sat_drp_read_lump` grows a cart branch: STORED → `memcpy`
  from cart, LZSS → `drp_lzss_decode` reading the cart cached alias directly (no Z_Malloc temp). The
  per-map `cache_purge` inside the staging write keeps the cached alias coherent across map changes
  (same pattern `load_wad` already ships).
- **4d** `sat_cd_free_during_play()` → `I_InitSound` picks CDDA when the WAD is fully resident OR
  cart staging is available; `cdda_has_audio_tracks()` still requires the disc to carry audio.
- Telemetry: row-21 overlay shows `CART<kb>k` (staged) vs `cd` (CD-served) per map.

**Adversarial review (5-dimension workflow, 2-skeptic verify) — fixed before commit:**
- *32-bit overflow:* an unvalidated `blob_size` could wrap `sub + blob_size` and slip past the
  fit/short-read guards → OOB cart reads. Now validated before the add (consistent with the existing
  `n_entries` corrupt-`.DRP` guard).
- *No retry:* the staging read did a single `LoadBytes`; a transient CD short-read demoted a whole
  map to CD-under-CDDA. Now retries 8× like the proven `drp_load`.
- *Stale overlay / stale comment:* `sat_drp_cart_kb` reset on every map select; the now-false
  "CDDA only runs when !streaming" comment corrected.

**Known residual (the deferred fade, Step 4c).** CDDA is committed once at boot, but a map that
*can't* stage — genuinely **not in the `.DRP`** (the MAP30 cast / finale static screen) or whose
blob doesn't fit — reads its lumps from the CD **while CDDA plays the same drive** → audio seek
glitch. Gameplay subsets are complete (§7.10), so this is confined to static finale screens; the
**Option-2 music fade** (door/level transitions, `TRANSITIONS_PLAN.md`) is what masks it and is the
next task.

**Validation status: HARDWARE-ONLY.** Clean SH-2 compile + link. Cart staging + CD-vs-CDDA timing
do **not** reproduce on Ymir, so the gain (and the residual glitch) must be confirmed on real
hardware with a 4 MB cart + a big-WAD repacked disc (`build.ps1 -Wad Doom2 -Repack`). Watch row-21
for `CART<kb>k` per map and listen for CDDA continuity.
