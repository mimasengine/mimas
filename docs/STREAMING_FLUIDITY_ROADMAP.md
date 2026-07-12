# Big-WAD CD streaming — FLUIDITY roadmap (solo + split-screen)

> **STATUS: PLAN (2026-07-02).** Companion to [`STREAMING_ANALYSIS.md`](STREAMING_ANALYSIS.md)
> (capacity + music — Phase 0/1 + `.DRP` repack + cart staging shipped). That doc answers
> *"does a big WAD load and run at all?"*; **this doc answers "how do we make it SMOOTH"** —
> kill/mask the CD hitches in solo and 2–4p split — and consolidates a 53-agent
> code+refs+SEGA-docs study (LIBCD.A disassembled; SlaveDriver/SlideHop/Azel/Jo-Engine/
> wolf4sdl/d32xr/doom3do/Z-Treme read at source level).
>
> **Two corrections to the record made by this study** (see §2): the
> `tools/repack_wad.py` patch-field bug (fixed 2026-07-02, uncommitted) invalidates the §7.10
> "every blob fits the 4 MB cart" verdict; and the §2.3 PU_LEVEL estimates are superseded by
> exact per-map audits (TNT/Plutonia have maps that **do not fit the zone at all** — a memory
> problem upstream of any streaming polish).

Tags: `[HW]` real-hardware measured · `[Ymir]` emulator · `[src]` read at source ·
`[disasm]` LIBCD.A disassembly · `[SEGA]` in-repo SEGA manuals (MANGFS/MANCDC/MANSTM) ·
`[est]` estimate/model. **Ymir does not model CD/cart timing** — every latency claim below is
`[est]` until the R0 instrumentation runs on console.

---

## 0. TL;DR — the phased plan

| Phase | Lever | Cost | Expected effect |
|---|---|---|---|
| **R0** | Instrument CD reads (FRT) + measure `k` on HW; re-emit corrected `.DRP`; pick cart-overflow policy | 1 build + 1 tool run | Ground truth for every decision below |
| **R1** | **Kill the per-sector bounce** (multi-sector reads) + 2048-align `.DRP` entries | small, code-only | ÷16–33 CD commands on *every* unaligned read — boot, level load, in-game page-ins |
| **R2** | **Async streaming engine** (persistent handle + `GFS_NwCdRead` prefetch + per-frame budgeted pump) | medium | In-game page-ins stop blocking the frame — the cartless fluidity lever, ×N payoff in split |
| **R3** | Offline layout: **boot index** (kills the 6–7k-read boot), access-ordered blobs, staging progress/masking | tool + loader | Boot ~166 s → **~2–8 s** `[est]`; transitions masked |
| **R4** | Memory levers (reopen texturecolumnlump; geometry diet; small cuts) | large, shared core | Makes TNT/Plutonia-class maps *fit at all*; restores the split slab |
| **R5** | Split-screen specifics: intermission preload, per-frame LRU aging, per-view page-in budget | small–medium | Multi stops multiplying solo hitches by N views |

> **Status 2026-07-12.** **R1 SHIPPED both paths** (`25450df`: 16 K chunked bounce in
> `Saturn_Read` + `drp_read`; overlay row-0 `ld<chunks>/<per-sector-equivalent>` — the ratio
> is the live win, Ymir-visible). **R1.3 (2048-align `.DRP` entries) DROPPED** — superseded
> by the chunked bounce (≤ceil(size/16K)+1 reads regardless of alignment) and it would bloat
> the per-map blobs against the 4 MB cart. **R2.2 persistent handle SHIPPED DEFAULT-OFF**
> (`0bb3fc8` — HW regression on the fast-seek Phoebe SD ODE: no physical seek to save +
> read-ahead churn + open-handle timing drag; stays a real-CD compile opt-in). **R3.1 sprite
> half SHIPPED** (core `7d708bd` / port `de8f2de`, + boot trust-but-verify cross-check);
> **R3.1 texture half CLOSED as MOOT** (§6).

Dependency: R0 → R1 → (R2 ∥ R3) → R5; R4 is orthogonal and gates TNT/Plutonia support.
The shipped `.DRP`+cart staging path ([`STREAMING_ANALYSIS.md` §7.9–7.12](STREAMING_ANALYSIS.md))
remains the **best case** (CD idle + CDDA); this roadmap is what makes the **cartless** and
**cold-miss** paths smooth, and fixes what the cart path staged incompletely (§2.1).

---

## 1. Ground truth — what a CD read actually costs today

The whole fluidity problem reduces to one verified cost model:

1. **`GFS_Load` is a full open/seek/play/wait/close cycle** `[disasm]`. LIBCD.A `GFS.O`
   `_GFS_Load@0x774` = `GFS_Open(fid)` (alloc handle + CD-block partition) → `GFS_Seek`
   (pointer-only) → ONE `GFS_Fread(nsct)` (issues the CDC play + transfer) → `GFS_Close` →
   `gfs_mngTermAccess` → `GFCB_MovePickup` + `GFCD_WaitPause` + `GFCB_DeleteAllData`: every
   call **stops the stream, waits for the drive to pause, and discards all read-ahead**.
2. **Mimas's bounce path issues one `GFS_Load` PER 2048 B sector** `[src]`
   ([w_file_saturn.cxx:220-234](../src/w_file_saturn.cxx)) — and this is the **common case**:
   big WADs are bundled **raw** (`build.ps1` no-strip), 0/2919 Doom II lumps are
   sector-aligned (3/1188 on the shipping shareware) `[src]`. A 64 KB lump = **33 full
   open/seek/play/wait-pause/close cycles** for one lump. Same discipline in `drp_read`
   ([w_drp_saturn.cxx:187-215](../src/w_drp_saturn.cxx)).
3. **Everything is synchronous on the master SH-2** `[src]` — a cold lump mid-game stalls the
   frame, and in split-screen it stalls **all 2–4 views at once** (views render sequentially
   on the master, [d_main.c:336-398](../core/d_main.c)).
4. **The async arsenal is already linked, just unused** `[src][SEGA]`. SRL wraps only blocking
   calls (`LoadBytes`→`GFS_Load`), but LIBCD.A (linked by `shared.mk:14`) exports the whole
   non-blocking API: `GFS_NwCdRead/NwFread/NwExecOne/NwGetStat/NwStop`, `GFS_SetTmode`
   (SCU/SDMA0/SDMA1/CPU), `GFS_SetGmode(RESIDENT)`, `GFS_CdMovePickup`, `GFS_GetFad`, the
   full `STM_*` lib (absolute-FAD-range streams), raw `CDC_*`. SRL itself calls `CDC_*`
   directly for CDDA — direct SGL calls under SRL are the *existing* model, not a
   transgression. The CD-block (own SH-1 + ~512 KB buffer, 24 partitions) reads
   **autonomously** once told to — zero SH-2 cost until the drain.
5. **On an OPEN handle, GFS pre-reads to EOF by default** `[disasm][SEGA]`
   (`GFS_RPARA_DFL=0x7FFFFFFF`): a persistent handle + `GFS_Seek` (pointer-only, no CDC
   command) + `GFS_Fread` **rides the CD-block read-ahead** — this is DoomJo's model
   (`DoomJo/project/w_file_saturn.c:52-97`) and the structural win over repeated `GFS_Load`.
6. **Config lock: `open_max=1`** (`SRL_MAX_CD_BACKGROUND_JOBS=1`, [Makefile:16](../Makefile))
   — one GFS handle total. A persistent streaming handle + occasional other opens needs **2**;
   one-line Makefile change, `GfsWork` auto-resizes (~+1–2 KB .bss → run the TLSF pre-flight).
7. **Default transfer mode is CPU copy** `[disasm]` (`GFTR_Setup` sets `TMODE_CPU`; even
   `TMODE_SCU` silently falls back to CPU for LWRAM/A-bus destinations). So today's reads also
   *burn master cycles* proportional to bytes; SDMA cycle-steal is an option only for HWRAM
   destinations (SlideHop ships it).
8. **Nothing is measured** — no CD timing instrumentation exists in the repo (FRT only times
   blit/memory); the ~300 ms seek / ~150 KB/s numbers are literature `[est]`; official figures:
   2X ≈ 307 KB/s raw, retry 15, ECC 5 `[SEGA]`. **Ymir models none of this.** The per-call
   overhead `k` of a 1-sector `GFS_Load` decides whether the current bounce costs seconds or
   *minutes* per level of play; it is unknown.

Already shipped and to preserve (they mask/limit the above): precache + REJECT skips, lazy
PU_CACHE streaming, `.DRP` per-map LZSS blobs + cart staging (CD idle → CDDA), split-only 96 KB
LRU composite slab, retry ×8 whackCD-style, MUS↔CDDA runtime switch, CRAM fades.

---

## 2. New findings this study adds to the record

### 2.1 `repack_wad.py` patch-field bug — `.DRP` blobs shipped incomplete (FIXED, re-measure done)

`tools/repack_wad.py:420` read the texture patch index at `mappatch_t+6` (= `stepdir`, always
1) instead of `+4` (= `patch`) — cross-checked against [measure_texfloor.py:79-81](../tools/measure_texfloor.py)
and [r_data.c:63-70](../core/r_data.c) `[src]`. Consequence: **every emitted blob omitted
nearly all wall-texture patches** (avg ~0.5–1.1 MB/map), which silently took the full-WAD CD
fallback at runtime — i.e. CD traffic *under CDDA* on the cart path, and unbudgeted seeks on
the CD path. **Fixed 2026-07-02** (`+6`→`+4`, uncommitted). Corrected per-map blob sizes
(measured, LZSS-12/4, vs the 4096 KB cart):

| IWAD | Worst corrected blob | Maps > 4 MB cart |
|---|---|---|
| Ultimate Doom | E2M2-class, **2755 K** | **0** — all fit |
| Doom II | MAP28 **4104 K** | **1** (over by 8 K!) |
| Plutonia | MAP22 **4364 K** | **3** (22, 23, 29) |
| TNT | MAP20 **4655 K** | **4** (20, 21, 31, 32) |

The §7.10 verdict ("worst 3523 K, 570 K headroom, CDDA on every map") **was measured with the
bug** and is superseded. Action → R0.3 (overflow policy) + re-emit + re-validate.

### 2.2 Exact memory audits — TNT/Plutonia are a *capacity* problem first

Per-map audit with compiler-verified SH-2 `sizeof` (zone = 966 656 B) `[src]`:
PU_STATIC floor: Doom II 494.7 K / TNT 618.8 K / Plutonia 552.9 K / Ultimate 403.0 K
(new counted post: 142 ST+HU lumps = 66.2 K PU_STATIC forever). Worst PU_LEVEL: Plutonia
MAP28 663 K, TNT MAP20 643 K (the old §2.3 700–900 K estimate was inflated — real `line_t`=64,
`seg_t`=32, `sector_t`=88). Verdicts (margin < 0 = does not fit):

- **Ultimate: fits everywhere** (worst margin +135 K 1p / +117 K 4p).
- **Doom II: fits except MAP15** (−4.4 K 1p / −23 K 4p) and MAP14 in 4p — **one ~30–70 K
  lever fixes all of Doom II** (MUS-out-of-zone or ST/HU purge).
- **Plutonia: 5 maps don't fit 1p** (MAP28 −272 K worst), 10 in 4p.
- **TNT: 23/32 maps don't fit 1p** (MAP20 −318 K worst), 24 in 4p.
- The **4 MB cart does not save TNT-class** (static + PU_LEVEL alone exceed the zone).
- The split LRU slab (96 K + 128 K contiguous margin) **never engages** on TNT/Plutonia
  (32/32 maps) and on 23/32 Doom II maps — split runs cacheless exactly where it needed the
  slab. → R4.4.
- (Attract demos never play in this port — `demosequence` alternates TITLEPIC/CREDIT,
  [d_main.c:831-833](../core/d_main.c) — so TNT's demo-map deficits are moot unless demo
  playback is re-enabled.)

### 2.3 Boot + transition I/O budget (CD mode, big WAD) `[est, model]`

Between `D_DoomMain` and the title, the engine reads **8.2–10 MB in ~6 200–7 400 `GFS_Load`s**:
`R_GenerateLookup` re-reads every patch of every texture ([r_data.c:349](../core/r_data.c)),
and `R_InitSpriteLumps` reads **all ~1381 sprite lumps in full for 8 useful bytes each**
([r_data.c:821](../core/r_data.c)). Boot time = `#GFS×k + #sectors×6.7 ms`: Doom II ranges
**73 s (k=5 ms) → 21 min (k=200 ms)**. Level transitions are cheap by contrast (geometry
91–146 K avg, worst ~300 K) — plus `WI_loadData` ~179 K. Cart staging (9–18 s `[est]`) runs
**after** the player leaves the intermission, as a frozen screen (wipe is cut in streaming).
Levers: R3.1 boot index (−88–95 % of boot reads), R1 multi-sector bounce (÷16 at constant
layout), R3.4 staging masking.

---

## 3. Phase R0 — instrument & decide (do first, 1 build)

- **R0.1 FRT-time the CD path** `[HW]`: wrap `sat_cd_load`/`drp_load` with the existing FRT
  infra ([dg_saturn.cxx:1035-1056](../src/dg_saturn.cxx)); overlay (free row, next to the SPL
  line): `GFS_Load` count/frame, KB/s, worst single call, retries (row-0 `cd` already exists),
  `.DRP` hit vs full-WAD-fallback count. This is the k-meter and the A/B harness for R1/R2.
- **R0.2 Measure `k` and sequential throughput** `[HW]`: chrono N=100 consecutive 1-sector
  `GFS_Load`s vs one 100-sector `GFS_Load` vs persistent-handle `Seek+Fread` drain; chrono the
  cart `load_wad` 256 KB-chunk loop (the sequential reference). Decides how much R1 buys and
  calibrates the R2 pump budget.
- **R0.3 Cart-overflow policy** for the 8 over-cart blobs (§2.1): (a) trim cold sprite frames
  (raise/xdeath/rare rotations) from the subset — Doom II MAP28 needs only **8 K**, trivial;
  (b) hot-set partial staging + CD cold-fallback masked by the Option-2 music fade
  ([`TRANSITIONS_PLAN.md`](TRANSITIONS_PLAN.md)) for TNT/Plutonia's 4+3 maps; (c) accept
  MUS-synth (no CDDA) on those maps. Recommendation: (a) for Doom II, (a)+(b) for Final Doom.
- **R0.4 Re-emit + re-validate `.DRP`** with the fixed tool (round-trip self-test), commit the
  tool fix, erratum in `STREAMING_ANALYSIS.md` §7.10 (done alongside this doc).

## 4. Phase R1 — kill the per-sector bounce (sync path, code-only)

- **R1.1 Multi-sector bounce in `Saturn_Read`** — the wolf4sdl `delta/delta2` recipe `[src]`
  (`saturn-refs/wolf4sdl-saturn/id_pm.cpp:12-20`): ONE `GFS_Load(fid, offset>>11, staging,
  size + offset%2048)` then `memcpy(dst, staging + sub, n)` — or head-sector bounce + direct
  aligned body + tail. A 64 KB lump: 33 CD commands → **1–4**. Buffer 16–32 KB; placement is
  the design point (HWRAM .bss is tight — TLSF pool is ~52 KB and the boot-loop pre-flight is
  MANDATORY; a PU_STATIC LWRAM carve or an existing scratch reuse are the candidates).
- **R1.2 Same for `drp_read`** ([w_drp_saturn.cxx:187-215](../src/w_drp_saturn.cxx)).
- **R1.3 2048-align `.DRP` entry `data_ofs`** (~5 lines in `emit()`, container +1–3 MB on
  ~70 MB — free on CD): STORED lumps then read straight into the destination in one
  `GFS_Load`; LZSS page-ins read aligned. **Do NOT 2048-align the raw WAD itself**: +18–33 %
  size kicks the shareware out of the 4 MB cart, for a measured boot gain of only ×1.6–2.9.

## 5. Phase R2 — async streaming engine (the cartless fluidity lever)

Goal: in-game page-ins stop blocking the frame. All pieces are proven in shipped Saturn
titles read at source (`saturn-refs/`): SlaveDriver `FILE.C` (prefetch `NwCdRead(500 sect)` at
open + per-sector drain + `NwExecOne` pump + `whackCD`), SlideHop `pcmstm.c` (game loop inside
a CD state machine, SDMA0), Azel (per-frame ring drain ≤20 sectors), Jo Engine
`jo_fs_do_background_jobs` (8 sect/frame, callbacks — the spec DoomJo gets for free).

- **R2.1 `SRL_MAX_CD_BACKGROUND_JOBS` 1→2** (Makefile; TLSF pre-flight).
- **R2.2 Persistent handle on the active file** (`.DRP` blob, else WAD): open once per level,
  `GFS_Seek` (pointer-only) + `GFS_Fread(n)` — rides the CD-block autonomous read-ahead (§1.5).
  Check no other consumer opens files mid-game (SRL TGA/sound loads are boot-time only).
  `GFS_NwStop` before any file switch (MANGFS 4.5(4)a: else Close blocks until drive pause).
- **R2.3 Frame-budgeted pump** `sat_cd_pump()`, called once per frame from `DG_DrawFrame`
  (master only — the slave must NEVER touch CD/zone, preserve the
  [r_things.c:1226-1233](../core/r_things.c) pre-cache constraint): drains K sectors of the
  in-flight request + `GFS_NwExecOne`. K from R0.2 (references: SlaveDriver stops at
  scanline 150; Jo=8; Azel≤20). Prefetch targets, in order: (1) rest of the current lump,
  (2) next entries in the access-ordered blob (R3.2), (3) next-level blob during intermission
  (`GFS_CdMovePickup` pre-seek).
- **R2.4 Deferred-miss rendering** (SlideHop state machine IDLE/SETUP/READING/DONE): a cold
  **sprite** can skip one frame instead of stalling (its slot draws next frame); a cold
  **texture/flat** column is harder (mid-frame dependency) — first iteration: keep those
  synchronous but now 1-command (R1) and prefetch-warmed (R2.3); audit later whether a
  1-frame-late column swap is tolerable.
- **R2.5 CDDA interlock**: the pickup is exclusive (CDDA ⊻ data, `[SEGA]` MANGFS:108) — async
  does NOT change the music matrix (§7.2–7.6). Scheduler must fade/pause CDDA before any CD
  burst on cart-staged maps' cold fallbacks (3DO Doom's `LockMusic` is the precedent; our
  version is the Option-2 fade).
- **R2.6 Transfer mode**: keep default CPU first (SCU/A-bus destinations force CPU anyway
  `[disasm]`); A/B `SDMA0` cycle-steal for HWRAM destinations later `[HW]`.
- **Architecture note**: the engine lives in `src/` (platform) behind the existing
  `W_ReadLump`/`.DRP` hooks — DoomJo has `jo_fs` async natively, so core/ stays pure-C
  unchanged (same seam philosophy as `r_parallel`).

## 6. Phase R3 — offline layout & load-time UX

- **R3.1 Precomputed boot index** — new `.DRP` section "BOOT" (or annex lump): sprite headers
  (1381×3 shorts ≈ 8.3 K) + `R_GenerateLookup` outputs (`collump/colofs/compositesize`,
  157–253 K — `measure_texfloor.py` already computes them) → `R_InitSpriteLumps` +
  `R_InitTextures` consume it in one sequential read, classic fallback if absent (same
  CRC-validated pattern as the `.DRP` itself). Kills boot passes P3+P4 = **−88–95 % of boot
  reads**; with R1, big-WAD CD boot ≈ **2–8 s** `[est]` vs ~1–20+ min today.
  **STATUS: sprite half (P4) SHIPPED + Ymir-VALIDATED 2026-07-12** — title-screen overlay
  read `ld162/278` (classic sprite pass would add ~1381 → index path proven taken), `cd0`
  retries, no MISMATCH, and in-game sprite placement visually correct; HW wall-clock gain
  pending (k-meter). `sprite_headers()` emitter + header
  `sprh_ofs/sprh_n` + `sat_drp_sprite_headers()` one-read loader + fail-closed
  `R_InitSpriteLumps` hook (`SAT_REPACK`); 1381 boot reads → one 8.1 K read, every entry
  verified byte-identical vs the WAD patch headers; boot now also cross-checks entries 0
  and N−1 against the real patch headers (2 reads) and falls back loudly on mismatch.
  **Texture half (P3): CLOSED as MOOT 2026-07-12** `[src]` — since R4 lazy directories,
  `R_GenerateLookup` has **no boot caller** ([r_data.c:460-500](../core/r_data.c): only
  `R_EnsureLookup`, on the draw path via `R_GetColumn`/`R_GenerateComposite`), and the
  patches it reads are the same PU_CACHE lumps the draw/composite consumes immediately
  after — a precomputed lookup index would save ~zero CD reads. §2.3's "P3 ≈ half the boot
  reads" premise described the pre-R4 tree (the gap-filler agent's numbers were not
  re-verified against R4). **The boot index is therefore COMPLETE with the sprite half**;
  the residual ~300 small boot reads (directory, PNAMES/TEXTURE, ST/HU fonts, title) are
  already chunked ÷8–16 by R1. Caveat `[HW]`: on a fast-seek ODE (Phoebe SD) the per-read
  overhead is far smaller than on a real drive — the R2 persistent-handle regression proved
  the seek-avoidance cost model inverts there; quantify the actual win with the R0 k-meter.
- **R3.2 Access-ordered blobs**: `emit()` currently writes `sorted(sub)` by lump index →
  reorder geometry-first, then flats/textures/sprites-by-mobj (or a runtime-traced order).
  Offline-only, helps the cartless path; marginal once cart staging works.
- **R3.3 ISO placement**: only if R0 measures real WAD↔DRP cross-seeks — `xorrisofs`
  sort-weights via `patches/saturnringlib.patch` (shared.mk is upstream). Low priority: the
  corrected subsets make fallbacks rare by construction.
- **R3.4 Staging UX**: `drp_stage_to_cart` runs *after* intermission exit (frozen screen
  9–18 s `[est]`) — either stage **during** the intermission/stats screen (player is reading
  scores; needs the stage loop to pump input/vblank), or add a progress bar (SlaveDriver
  `fs_addToProgress`) + CRAM fade. Pairs with R2.3's next-level prefetch.

## 7. Phase R4 — memory levers (gates TNT/Plutonia; helps everything)

Ranked by measured value (§2.2); items 1–2 are the big shared-core chantiers:

1. **Reopen [`TEXTURECOLUMNLUMP_PLAN.md`](TEXTURECOLUMNLUMP_PLAN.md)** (its own reopen
   condition is now met): lazy per-texture column directories → −253 K (TNT) / −202 K
   (Plutonia) / −157 K (Doom II) static.
2. **d32xr geometry diet** (render from map-lump formats: `seg` 32→12, `node` 52→28,
   `line` 64→~40) ≈ **−180 K** on TNT MAP20. Shared core, touches hot paths — A/B perf first.
   (1)+(2) ≈ −433 K covers the worst −318 K deficit.
3. **Small levers**: MUS out of zone (−4–63 K — alone fixes Doom II MAP15/14); ST+HU
   purgeable/pre-baked (−66 K); `lumpinfo` 28→20 B (−25 K); `plane_pool` sized to telemetered
   peak (−20–31 K); offline sidedef squash for Doom II/Plutonia (−20–40 K worst maps; TNT
   already squashed).
4. **Adaptive split slab**: tier `TEXCACHE_FIXED/MARGIN` down (e.g. 48 K+64 K) from
   `Z_LargestFreeBlock` so 2–4p on big WADs regains the anti-fragmentation slab it currently
   never gets (§2.2).

## 8. Phase R5 — split-screen (multi) specifics

Facts `[src]`: multi = **local split only** (no netplay/bots — `FEATURE_MULTIPLAYER` #undef,
stubs); N views render **sequentially on the master** sharing ONE zone and ONE lump cache; a
synchronous CD read freezes everyone; multi adds almost **no extra lumps** (delta ≈ WIOBJ vs
WIOSTI) — its real cost is the tighter frame budget and 2–4 disjoint working sets churning
PU_CACHE (the measured 2p composite-fragmentation OOM).

- **R5.1 Intermission preload** of the hot subset: the `.DRP` entry table IS the preload list
  (superset-safe, already emitted); missing piece is a ~50-line incremental consumer
  (priority: geometry > flats > textures > sprites) running under the intermission — masks
  first-sight hitches ×N players. Works cartless too (bounded by a byte budget, purgeable).
- **R5.2 LRU tuning for 3/4p**: switch texcache aging from per-view to once-per-frame
  ([r_cache.c:133-135](../core/r_cache.c) comment); extend the pool to flats / sprite patches /
  single-patch walls (explicit "not yet cached" follow-up) so 4 disjoint frusta stop churning
  the shared PU_CACHE.
- **R5.3 Per-view page-in budget** once R2 lands: reuse the cumulative wall-budget slicing
  model ([dg_saturn.cxx:2746-2768](../src/dg_saturn.cxx)) for pump quota, so one busy view
  cannot starve the others' page-ins.
- **R5.4 Priority note**: R1+R2 are worth MORE in multi than solo (one stall = 2–4 players
  frozen; frame budget ~66–100 ms means even a 20 ms synchronous read is visible). Cart
  staging benefits multi identically (blob is view-agnostic) — but verify with the R0.1
  fallback counter whether 4 views touch more out-of-subset lumps than 1p.

## 9. Validation protocol (hardware-only)

Ymir models neither CD nor cart timing — every phase A/Bs on console (the 540-544 bench
protocol), watching the R0.1 overlay: `GFS/frame`, KB/s, worst-call ms, retries, DRP-fallback
count, texcache hit rate, and fps on: (a) big-WAD boot, (b) level load, (c) first-sight
combat burst (cold sprites), (d) room-transition texture rafale, (e) 2p/4p same scenes.
Success criteria: no in-game read > 1 frame `[R2]`; boot < 10 s `[R3.1]`; level load < 5 s
cartless; zero CDDA glitches on cart maps after R0.3.

**R3.1 sprite-index protocol.** (a) *Correctness (Ymir, ~1 min):* boot a repacked disc and
check the console for `DRP: sprite index N ok (1 read)` with **no**
`R3.1 sprite index MISMATCH` line — the boot cross-checks entries 0 and N−1 against the
real patch headers and rebuilds classically (loudly) on any decode/container bug. Sprites
sitting correctly in-game (offsets drive placement) is the visual confirmation.
(b) *Fallback:* boot with a stale or absent `.DRP` (other WAD's, or pre-R3.1 tool) — expect
the classic dotted loop and a normal boot. (c) *Gain:* row-0 `ld` at the title screen —
with the index the boot issues ~10× fewer chunk loads (~200 vs ~1600+ on a big IWAD, the
sprite pass alone was ~1400+); wall-clock delta is HW-only (small on the Phoebe ODE, large
on a real drive — the k-meter quantifies).

---

## 10. Key file:line index

- Sync read path: [w_file_saturn.cxx:151-236](../src/w_file_saturn.cxx) (retry :151, fast
  path :211, bounce :220); `.DRP` loader [w_drp_saturn.cxx](../src/w_drp_saturn.cxx)
  (drp_read :187-215, stage :285-322); SRL wrapper `SaturnRingLib/saturnringlib/srl_cd.hpp:399-423`;
  `GFS_Init(open_max)` `srl_cd.hpp:629` + [Makefile:16](../Makefile).
- Consumers that hitch in-game: `R_GetColumn`/`R_GenerateComposite`
  ([r_data.c:415-419, 271-275](../core/r_data.c)), sprites at draw
  ([r_things.c:427, 1140, 1226-1233](../core/r_things.c)), flats per visplane
  ([r_plane.c:1278-1281](../core/r_plane.c)), first-play SFX
  ([i_sound_saturn.cxx:244](../src/i_sound_saturn.cxx)), boot passes
  ([r_data.c:349, 821](../core/r_data.c)).
- Async API: `SaturnRingLib/modules/sgl/INC/sega_gfs.h:396-417` (Nw*), `sega_stm.h:362`
  (`STM_OpenFrange`), `sega_cdc.h`; SEGA manuals in `saturn-refs/SONIC-Z-TREME/Documentation/`.
- Reference implementations: `saturn-refs/SlaveDriver-Engine/FILE.C` (:63 whackCD, :146-165
  prefetch, :245 pump), `MOV.C:452-456` (idle-time drain), `PIC.C` (VDP1 LRU), `UTIL.C:339-405`
  (bump arena); `saturn-refs/SlideHop/pcmstm.c:465-687`; `DoomJo/joengine/jo_engine/fs.c:125-217`;
  `saturn-refs/wolf4sdl-saturn/id_pm.cpp:12-20`; `saturn-refs/Azel` town cutscene ring;
  `saturn-refs/doom3do` LockMusic; `saturn-refs/Doom (Japan) fix patch v0.3` (retail Saturn
  Doom = full per-level preload, Red Book).
- Tool fix: [repack_wad.py:420](../tools/repack_wad.py) (`+6`→`+4`, 2026-07-02).
