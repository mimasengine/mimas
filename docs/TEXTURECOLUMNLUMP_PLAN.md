# Cutting the `texturecolumnlump` / `texturecomposite` PU_STATIC floor — deep plan

Status: **PHASE 0 COMPLETE 2026-06-24 — MEASURED; GATE VERDICT = DEFER (NO-GO for now).**
The directory floor was measured exactly by parsing the shipping WADs' `TEXTURE1/2`/`PNAMES`
lumps (`tools/measure_texfloor.py`, mirrors `R_InitTextures`/`R_GenerateLookup`; numbers
cross-checked and adversarially verified high-confidence). **It is 117–253 KB, NOT the
~400–600 KB estimated below** (the estimate assumed ~1000–1400 Doom II textures; the real
count is **428**). This trips the plan's own Phase-0 gate (§7: "deprioritize if <250 KB") for
3 of 4 IWADs. Option E specifically would *worsen* pool pressure (its `height×width` slabs
are 2.2–2.7× the current `height×mpc` composites against a fixed 256 KB pool cap). **See the
new §9 (PHASE 0 RESULTS) for the full measured table, the Option-E pool-pressure finding, the
cheaper-lever ranking, and the gate verdict.** The original research below (§1–§8) is retained
as the design study; §1's byte estimates are superseded by §9's measurements.

Original framing (superseded where noted): three parallel investigations done
(current `core/r_data.c` system, the VDP1 wall-path dependency, the d32xr
composite-on-demand reference model). This document ranks the options to cut the
`texturecolumnlump`+`texturecolumnofs` directory floor that sits in the
~884 KB→964 KB LWRAM zone after the already-landed wins (visplane-pool span arena
**now landed**, `MAXVISPLANES=256`, reject-skip, `RP_CMD_BUF` shrink, sfx `PU_CACHE`). It is the
natural follow-on to [`STREAMING_ANALYSIS.md` **§S4(b)**](STREAMING_ANALYSIS.md) ("composite-on-demand
`decals[]` — deferred (heavier structural change)").

Companion docs: [`STREAMING_ANALYSIS.md`](STREAMING_ANALYSIS.md) (the CD-streaming
architecture; this plan is its S4(b)), [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) and
[`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md) (why the VDP1 wall bake still funnels through
`R_GetColumn`).

> **One-line conclusion.** The VDP1 wall renderer is **not** independent of Doom's
> composite tables — it bakes VRAM through `R_GetColumn` (`src/dg_saturn.cxx:1686-1687`),
> so cutting the tables is a **shared-core** change that touches every wall path, hardware
> and software. The composite **pixel** blocks are already on-demand/purgable; the
> remaining static floor is the two per-column **directory** arrays, and the only way to
> remove them is to adopt d32xr's arithmetic (`col*height`) column-major slab model.

---

## 1. Problem statement & byte budget

The LWRAM zone is ~884 KB (→964 KB with the small headroom recovered so far). In
CD-streaming mode (`sat_streaming_mode=1`, Doom II / Ultimate / Plutonia / TNT, no cart)
the geometry, zone, and texture metadata must coexist with the bounded LRU composite pool
(`core/r_cache.c`). After the visplane and `RP_CMD_BUF` cuts, the single largest
**unconditional, never-freed PU_STATIC** residency is the per-column texture directories
allocated once in `R_InitTextures`.

### The floor, in bytes

The dominant cost is two per-texture, per-column arrays allocated at
`core/r_data.c:726` and `:727`:

```
texturecolumnlump[tex]  = Z_Malloc(texture->width * sizeof(short),          PU_STATIC)  // r_data.c:726
texturecolumnofs[tex]   = Z_Malloc(texture->width * sizeof(unsigned short), PU_STATIC)  // r_data.c:727
```

Combined per-column cost = **`4 bytes × Σ(texture width)`** = `4 × numtextures × avg_width`.

> ⚠ **SUPERSEDED by §9 measurement.** The estimates in the table below were wrong — they
> assumed ~1000–1400 Doom II textures. The **real** count is **428** (Σwidth 40,240 → floor
> **157 KB**), measured exactly from the WAD. See §9 for the verified per-WAD floor.

| WAD | numtextures | avg width | Σ width (cols) | columnlump+ofs total |
|---|---|---|---|---|
| Doom shareware | ~125 | ~100 | ~12,500 | ~~~50 KB~~ → **42 KB** (§9) |
| Doom II (lower) | ~~~1000~~ **428** | ~~~100~~ **94** | ~~~100,000~~ **40,240** | ~~~400 KB~~ → **157 KB** (§9) |
| Doom II (upper) | ~~~1400~~ — | — | — | ~~~600 KB~~ — |

The original "Doom II is the binding case at ~400–600 KB" claim is **false**; the measured
binding case is **TNT at 253 KB** (Doom II 157 KB, Plutonia 202 KB, Ultimate 117 KB). These
arrays are still unconditional PU_STATIC, allocated once at `R_InitTextures`, never freed —
but at a third of the assumed size. See §9.

### The secondary terms (smaller, listed for completeness)

The per-texture scalar/pointer arrays in `R_InitTextures` (`core/r_data.c:653-659`, `:751`)
plus the hashtable (`:542`) are each `numtextures × 4`:

| Array | Line | Bytes (Doom II ~1400) |
|---|---|---|
| `textures` (ptr array) | `653` | ~5.6 KB |
| `texturecolumnlump` (ptr array) | `654` | ~5.6 KB |
| `texturecolumnofs` (ptr array) | `655` | ~5.6 KB |
| `texturecomposite` (ptr array) | `656` | ~5.6 KB |
| `texturecompositesize` (int) | `657` | ~5.6 KB |
| `texturewidthmask` (int) | `658` | ~5.6 KB |
| `textureheight` (fixed_t) | `659` | ~5.6 KB |
| `texturetranslation` | `751` | ~5.6 KB |
| `textures_hashtable` | `542` | ~5.6 KB |
| each `texture_t` (loop) | `702-705` | ~50 KB total |
| sprite width/ofs/topofs (×3) | `r_data.c:795-797` | ~13 KB |

These nine `numtextures×4` arrays sum to **~50 KB**; the `texture_t` structs another ~50 KB.
They are **secondary** — together less than one of the two per-column arrays — and most are
genuinely needed every frame (`texturewidthmask`, `textureheight` are read directly by the
VDP1 path; see §3). The per-column directories are the prize.

### The pixel blocks are NOT the problem (already solved)

`texturecomposite[tex]` — the *composited pixel data* — is **already on-demand and
reclaimable**:
- Built lazily in `R_GenerateComposite` only when a multi-patch column is first drawn
  (`core/r_data.c:230-308`, invoked from `R_GetColumn` at `:418-419`).
- Classic path: `Z_Malloc(PU_STATIC)` then immediately `Z_ChangeTag(block, PU_CACHE)`
  (`core/r_data.c:306-307`) → purgable by the zone allocator.
- Streaming path: pulled from the bounded LRU pool via `R_TexCacheAlloc`
  (`core/r_data.c:252`), evicted by recency in `core/r_cache.c`.

So the pixel floor is **already bounded** (S4(a), shipped). What remains static is the
**directory** that tells `R_GetColumn` where each column lives. That is the d32xr gap.

---

## 2. Current-system anatomy (`core/r_data.c`)

### 2.1 What the five tables hold

Declarations at `core/r_data.c:150-156`. Only `textureheight` is exported via a header
(`extern` in `r_state.h:38`); the other five are **file-private to `r_data.c`** — so the
data-structure surgery is contained, but the **behavioural contract** (`R_GetColumn`'s
return value) is consumed across the core and in `src/dg_saturn.cxx`.

| Table | Type | Holds | Per-texture cost |
|---|---|---|---|
| `texturecolumnlump[tex][col]` | `short**` | WAD lump number of the single patch covering column `col`, **or `-1`** if multi-patch (the composite sentinel). | `width × 2` **(the floor)** |
| `texturecolumnofs[tex][col]` | `unsigned short**` | Single-patch: byte offset *into the patch lump* (past the 3-byte post header). Multi-patch: byte offset *into the composite block*. Meaning bifurcates on the `collump==-1` sentinel. | `width × 2` **(the floor)** |
| `texturecomposite[tex]` | `byte**` | NULL until built; then the composited pixel block (PU_CACHE or LRU pool). | one ptr (block on-demand) |
| `texturecompositesize[tex]` | `int*` | `height × (#multi-patch columns)`; 0 if fully single-patch. | 4 |
| `texturewidthmask[tex]` | `int*` | `(largest pow2 ≤ width) − 1`; wraps `col` in `R_GetColumn` (`:411`). | 4 |
| `textureheight[tex]` | `fixed_t*` | `height << FRACBITS`; pegging in `r_segs.c`. **The only header-exported one.** | 4 |

### 2.2 How `R_GenerateLookup` fills them (`core/r_data.c:315-395`)

Run once per texture at load (called from `R_InitTextures` at `:748`); allocates **no
permanent memory** itself (its only `Z_Malloc` at `:341` is a `width`-byte `patchcount`
scratch, freed at `:394`). It only *fills* the directories:

1. Zero `patchcount[width]` scratch.
2. **First pass** (`:345-366`): for each column each patch covers, `patchcount[x]++` and
   provisionally write single-patch values — `collump[x] = patch->patch` (lump),
   `colofs[x] = LONG(realpatch->columnofs[x-x1]) + 3` (offset into the patch lump, past the
   post header). Last patch wins on overlap.
3. **Second pass** (`:368-392`): per column —
   - `patchcount[x] == 0` → warn + bail (`:370-375`).
   - `patchcount[x] > 1` → **overwrite** `collump[x] = -1` (`:381`), `colofs[x] =
     texturecompositesize[texnum]` (cursor into the not-yet-built composite, `:382`), then
     `texturecompositesize[texnum] += height` (`:390`); >64 KB guard at `:384-388`.
   - `patchcount[x] == 1` → provisional single-patch values stand.

**The `-1` sentinel in `collump[]` is the single source of truth for "this column needs the
composite."** `texturecompositesize` doubles as the running composite offset cursor.

### 2.3 The `R_GetColumn` seam (`core/r_data.c:403-424`)

```c
col &= texturewidthmask[tex];          // :411  wrap to pow2
lump = texturecolumnlump[tex][col];    // :412
ofs  = texturecolumnofs[tex][col];     // :413
if (lump > 0)                          // :415  SINGLE-PATCH fast path
    return (byte*)W_CacheLumpNum(lump, PU_CACHE) + ofs;   // straight from the patch lump
if (!texturecomposite[tex])            // :418  MULTI-PATCH, not yet built
    R_GenerateComposite(tex);          // :419  build whole texture on demand
else if (sat_texcache_active)          // :420  SATURN streaming
    R_TexCacheTouch(texturecomposite[tex]);  // :421  LRU recency bump
return texturecomposite[tex] + ofs;    // :423  into the composite block
```

`lump > 0` → direct from lump (vanilla relies on "lump 0 is never a patch"). `lump <= 0`
(the `-1` sentinel) → composite path.

### 2.4 The LRU pool contract (`core/r_cache.c`) — the d32xr half already ported

DoomSRL already ported d32xr's **pool/LRU** (`core/r_cache.h:7-8`: "Ported from d32xr's
r_cache.c, adapted to this core's classic composite model") but **kept the classic column
model**. The pool's eviction model uses a back-pointer: `texcache_hdr_t.userp =
&texturecomposite[tex]` (`r_cache.c:40,64-65,124-125,159`). On evict it NULLs `*userp`, so
`R_GetColumn` transparently rebuilds.

**Hard constraint for any redesign:** the LRU assumes a **stable `byte** texturecomposite[tex]`
owner slot per texture** that it can NULL on evict. Any new column model must preserve a
per-composite `void**` owner slot, or rewrite `r_cache.c`'s publish/evict contract. Also
the `sat_xsplit` guard (`r_cache.c:138,169`) forbids touching the pool during a parallel
(2p) pass — composites the slave may draw must stay resident for the whole frame.

---

## 3. THE KEY DoomSRL INSIGHT — do VDP1 walls use the composite tables?

**Yes. Fully, transitively, through `R_GetColumn`. The VDP1 path does NOT bypass the
composite system and does NOT read patch lumps directly.** This is the load-bearing finding
that decides how aggressively we can cut.

The VDP1 wall bake is in `wall_tex_resolve()` (`src/dg_saturn.cxx:1649`). On a cache miss
(`wtex_bakes++`, `:1675`) it loops the texture's columns and packs two per 16-bit halfword
into VDP1 VRAM:

```c
const unsigned char *c0 = R_GetColumn(texnum, x);                       // :1686 even col -> high byte
const unsigned char *c1 = (x + 1 < W) ? R_GetColumn(texnum, x + 1) : c0; // :1687 odd col -> low byte
```

So **`R_GetColumn` does the compositing; the VDP1 just memcpy-packs the result into VRAM.**
The VDP1 path additionally reads two scalar tables **directly** (not via `R_GetColumn`):
- `texturewidthmask[texnum] + 1` for width (`src/dg_saturn.cxx:1659,1893,1739,1748`).
- `textureheight[texnum] >> 16` for height (`src/dg_saturn.cxx:1660`).

It does **not** read `texturecolumnlump` / `texturecolumnofs` / `texturecomposite`
directly — only through `R_GetColumn`.

### Every render path that funnels through `R_GetColumn`

| Path | Location | Notes |
|---|---|---|
| **VDP1 wall bake** (the hardware path) | `src/dg_saturn.cxx:1686-1687` | packs `R_GetColumn` cols into `wtex_cache` VRAM (22 slots: 16×8 KB + 6×32 KB, `:1453-1455`) |
| **Software walls** mid/top/bottom (1p CPU fallback, 2p baseline, 3/4p) | `core/r_segs.c:595,623,658` | `dc_source = R_GetColumn(...)` when the VDP1 hook is gated off (split / close / magnified / floor-clipped) |
| **Masked midtextures** (always software) | `core/r_segs.c:180` | `R_GetColumn(texnum, maskedtexturecol[dc_x]) − 3` (rewind to post header) |
| **Software sky** | `core/r_plane.c:1044` | `R_GetColumn(skytexture, angle)` |
| **Hardware sky upload (NBG0)** | `src/dg_saturn.cxx:1323` | `R_GetColumn(skytexture, col)` |
| **Potato dominant-flat colour** | `core/r_data.c:527` (`R_WallPotatoColor`) | histograms each texture at precache |

**Sprites do NOT use `R_GetColumn`.** `r_things.c` reads sprite patch columns directly from
the WAD lump (`W_CacheLumpNum(vis->patch+firstspritelump)` + `patch->columnofs[]`,
`r_things.c:427,464,1111,1124`). Sprites are single-lump patches, never composited — they
bypass the texture tables entirely. **So the refactor touches walls + masked walls + sky
only.**

Downstream: `dc_source` is copied into the RP command queue for the slave drawer
(`r_parallel.c:1283,1298` → `s_dc_source`, `r_things.c:1021,1037`). The slave dereferences
that pointer later in the frame, which is why any composite it may draw must stay resident
all frame (the `sat_xsplit` guard).

### Conclusion of the cut-depth question

The three tables are **load-bearing for every wall-texel path, hardware and software** —
they cannot be cut while keeping Doom's native **post-based** column lumps. The only render
survivors of deleting them would be sprites and pure flat fills. Therefore:

- **You cannot make `texturecolumnlump`/`texturecolumnofs` "lazy"** the way the pixel blocks
  are: `R_GetColumn` reads them on **every column** with no rebuild path, and only
  `R_GenerateLookup` (run once at load) populates them.
- **The only way to remove the per-column directory floor is to remove the need for it** —
  i.e. adopt d32xr's **arithmetic** column addressing (`col*height` into a column-major raw
  slab), which presupposes a **post-processed, post-free** texture representation.

This reframes the whole plan: it is not "free the tables", it is "**change the texture
storage format** so the tables become unnecessary."

---

## 4. The d32xr reference model (composite-on-demand)

d32xr (`c:/Users/pcico/Projects/saturn-refs/d32xr/`) keeps **no per-column directory at
all**. A texture is a contiguous column-major pixel slab plus a tiny global patch list.

`texture_t` (`r_local.h:162-182`):
```c
typedef struct {
    char       name[8];
    VINT       width, height, lumpnum;
    uint16_t   decals;            // (firstdecal << 2) | numdecals   (numdecals 0..3)
    inpixel_t *data[MIPLEVELS];   // pointer(s) to the contiguous w*h column-major slab
    int8_t    *colormaps;         // non-NULL => 4bpp compressed
} texture_t;
```

- **No `texturecolumnlump`/`texturecolumnofs`/`texturecompositesize`.** Column N is
  addressed **arithmetically**: `src = mip->data + colnum * mip->pitch` (`r_phase6.c:126`),
  pitch = (mip) height. Possible because the WAD is **post-processed at build time** into
  raw `width*height` column-major slabs — no Doom `column_t` posts, so no offset table.
- The only "patch list" is the global `decals[]` array (`r_data.c:124`), sized = Σ patchcount
  over **multi-patch textures only** (`r_data.c:119-122`). Single-patch textures get
  `decals == 0` and point `tex->data[j]` straight at the source lump (`r_main.c:446`) — no
  composite ever built. `texdecal_t` (`r_local.h:155-160`) = `{mincol,maxcol,minrow,maxrow,
  texturenum}` ≈ 14 bytes.
- **`R_CompositeColumn`** (`r_data.c:936-1035`) composites **one column on demand**, only
  for textures with decals (`:943-944`). Two callers:
  - **Per-frame LRU bake** — `R_UpdateCache` (`r_phase9.c:13-247`) walks only this frame's
    `viswalls`, picks ≤1 uncached mip per mip-level per frame, allocs a pool slab
    (`R_AddToTexCache`), and composites **every column back over the slab in place**
    (`r_phase9.c:197-217`). Whole-texture bake amortized at ≤1 texture/frame.
  - **JIT single-column** — `R_DrawTexture` (`r_phase6.c:128-147`) composites just the one
    column into per-CPU `mip->columncache` scratch, memoizing the last column
    (`lastcol`). Suppressed when the texture is already pool-resident (`r_phase6.c:382-384`).
- **Recency touch is decoupled from the inner loop** — batched per visible texture in phase
  9 (`R_TouchIfInTexCache`, `r_cache.c:102-118`), not on every column fetch. The draw loop
  just dereferences `tex->data`.

### Permanent allocation, d32xr vs DoomSRL

| Concern | d32xr | DoomSRL today |
|---|---|---|
| Pixel pool + LRU evict | `r_cache.c` | **already ported** → `core/r_cache.c` |
| Per-texture column directory | **none** (arithmetic `col*height`) | `texturecolumnlump`/`ofs` PU_STATIC (**~400–600 KB Doom II**) |
| Patch list | global `decals[]` + `uint16 decals` field | classic `texpatch_t` + `R_GenerateComposite` from `column_t` posts |
| Recency touch | batched phase-9 per visible texture | inline in `R_GetColumn` per fetch (`r_data.c:420`) |

**The missing piece is exactly the on-demand/arithmetic column model** (`texture_t.data[]` +
`decals[]` + the column-major raw slab), not the cache — DoomSRL already has the cache.

**Correctness caveat (must flag):** d32xr caps `numdecals` at 3 (`& 0x3`, `r_data.c:940`).
Textures with >3 overlapping patches are not representable. Fine for the Jaguar/32X asset
set, **a correctness bug for arbitrary PC WADs** (Doom II / PWADs can exceed 3). Any
verbatim port of the packed-`decals` field must either widen the field or keep a fallback
path for >3-patch textures.

---

## 5. Ranked options (minimal → ambitious)

### Option A — Halve the directory: drop `texturecolumnofs`, recompute offsets arithmetically (MINIMAL)

- **Change.** Observe that for a **single-patch** column `colofs` is `columnofs[x]+3` into the
  patch lump (recomputable from the patch's own header), and for a **multi-patch** column it
  is `(running multi-patch index) × height` into the composite. Keep `texturecolumnlump` (the
  sentinel + lump number), **delete `texturecolumnofs`** (`r_data.c:655,727`) and recompute
  the offset in `R_GetColumn`: single-patch → read the patch lump's `columnofs[]`;
  multi-patch → maintain a per-texture **multi-patch column counter** to derive
  `idx*height`. Requires either a per-column 1-bit "is-multi" map (cheap) or a per-texture
  count of multi-patch columns < this column (more work).
- **Memory saved.** `2 × Σ width` = **~200–300 KB** (half the floor).
- **Risk.** **Medium.** The single-patch offset recompute adds a `W_CacheLumpNum` +
  `columnofs[]` deref on the hot fast path (`R_GetColumn` is called per-column per-wall; the
  VDP1 bake calls it twice per VRAM column). PERF(1.4) note (`r_data.c:42-43`) says
  `R_GetColumn` is **not** a RECORD-pass hotspot today, but adding a lump-header deref to the
  *single-patch* path (the common case) is exactly where it could become one. Multi-patch
  index recompute needs a compact side structure or it costs more than it saves.
- **Effort.** Medium — local to `r_data.c`, but the multi-patch index derivation is fiddly
  and easy to get subtly wrong (off-by-one vs the `texturecompositesize` cursor).
- **DoomJo/shared-core impact.** Shared `core/` change → DoomJo inherits it
  (`sat_streaming_mode=0`, so the memory win doesn't matter to DoomJo but the **fast-path
  cost does** — a per-column lump deref would slow DoomJo's software walls). Must A/B on
  DoomJo too.
- **HW/Ymir validation.** Ymir = correctness + mechanics (composite identity, no corruption).
  Hardware = the fast-path cost verdict (does software-wall fps regress?).
- **Verdict.** Half the win, but **adds cost to the hottest path** for the common
  (single-patch) column. Likely a wash or net-negative on fps. **Not recommended as a
  standalone** — only attractive if measurement shows the recompute is free.

### Option B — Shrink the directory width: `unsigned short` lump indices + packed offsets (MINIMAL+)

- **Change.** Both arrays are already 2-byte (`short`/`unsigned short`). Marginal shaving
  only — e.g. dropping the separate `texturecompositesize[]` int array (`r_data.c:657`,
  ~5.6 KB) by recomputing it, or packing the sentinel into the offset's high bit. These are
  **secondary-term** cuts (§1.2), not the floor.
- **Memory saved.** **~5–15 KB** (the scalar arrays, not the per-column floor).
- **Risk.** Low. **Effort.** Low. **Shared-core impact.** Low.
- **Verdict.** Not worth a chantier on its own; **fold into whichever bigger option ships**
  as free cleanup. Does **not** touch the 400–600 KB floor.

### Option C — Lazy directory build per-texture (deferred `R_GenerateLookup`) (MODERATE)

- **Change.** Don't build `texturecolumnlump[tex]`/`texturecolumnofs[tex]` for **all**
  textures at `R_InitTextures`. Build them per-texture **on first `R_GetColumn(tex, …)`**,
  into the **LRU pool** (or a bounded directory sub-pool), keyed by texnum, evictable. The
  directory becomes as on-demand as the pixel block. Keep the classic post-based storage.
- **Memory saved.** Bounded to the **visible working set** of textures. If a level shows
  ~150 distinct wall textures at avg width 110, resident directory ≈ `150 × 110 × 4` ≈
  **66 KB** instead of 400–600 KB → **~330–530 KB saved**, capped by the pool.
- **Risk.** **Medium-high.** (1) `R_WallPotatoColor` (`r_data.c:525-528`) calls `R_GetColumn`
  for **every** texture at precache → would force a directory build for every texture,
  defeating laziness — must be gated/skipped in streaming mode (it already is precache, which
  streaming skips, but verify). (2) The LRU's `userp` back-pointer contract
  (`r_cache.c:40,124,159`) currently owns `&texturecomposite[tex]`; now it must also own the
  directory slots, doubling the eviction bookkeeping. (3) Eviction/rebuild churn: a directory
  evicted mid-frame while the slave holds a `dc_source` into its composite = corruption — the
  `sat_xsplit` guard must extend to directories.
- **Effort.** High — new pool client, two owner slots per texture, careful streaming gating.
- **DoomJo/shared-core impact.** Shared. DoomJo (`sat_streaming_mode=0`) must keep the
  **eager** path → both code paths coexist behind the streaming flag (like S4(a)). Adds the
  same "register the new pool client in DoomJo's makefile" cross-port action as `r_cache.c`.
- **HW/Ymir validation.** Heavy — eviction races are exactly the class that only shows on
  hardware under churn (cf. the streaming PU_STATIC-leak class in MEMORY).
- **Verdict.** Saves most of the floor **without changing the storage format** (keeps
  post-based lumps, keeps `R_GetColumn`'s table-lookup shape) — the **least disruptive big
  win**. But it grows the LRU bookkeeping and the race surface. **Strong candidate for the
  recommended path** *if* the rebuild cost (re-running `R_GenerateLookup` per texture on a
  cache miss) is acceptable — `R_GenerateLookup` is two passes over `width` columns, cheap
  per texture but now on a hot-ish miss path.

### Option D — Full d32xr arithmetic model: column-major raw slabs + `decals[]` (AMBITIOUS)

- **Change.** Port d32xr's representation into shared `core/`: post-process textures into
  contiguous column-major `width*height` slabs at build time (offline, in
  `tools/strip_wad.py`), replace `texture_t` with the d32xr `data[]`+`decals` form, delete
  `texturecolumnlump`/`texturecolumnofs`/`texturecompositesize` entirely, address columns as
  `data + col*height`, and composite on demand via `R_CompositeColumn` into the existing LRU
  pool. `R_GetColumn` becomes a thin arithmetic wrapper (or is inlined into the VDP1 bake and
  the segs loop).
- **Memory saved.** **The entire ~400–600 KB floor** (no directory at all) **plus** the
  `texturecompositesize` array (~5.6 KB). Permanent per-texture cost drops to one
  fixed-size `texture_t` (~40 B) + the small global `decals[]`. Pixel storage is entirely the
  bounded LRU pool — already built.
- **Risk.** **High.** (1) **WAD-format change**: requires `strip_wad.py` to emit
  post-processed column-major slabs (a new on-disc texture format) — every disc must be
  regenerated; the CD-read path and `W_CacheLumpNum` semantics for textures change. (2) The
  `numdecals ≤ 3` cap is a **correctness bug** for Doom II/PWADs with >3 overlapping
  patches — must widen the field or keep a post-based fallback for over-cap textures. (3)
  Single-patch textures point `data[]` straight at the lump — but DoomSRL streams lumps from
  CD as **purgable PU_CACHE**; a single-patch `data[]` pointer into a purged lump dangles.
  d32xr keeps everything in ROM (never purged); DoomSRL must either cache single-patch slabs
  in the pool too or pin them. (4) The VDP1 bake (`src/dg_saturn.cxx:1686`) and every segs
  consumer change shape — `R_GetColumn` is no longer "lump-or-composite", it's "slab+offset".
- **Effort.** Very high — touches the WAD tool, `r_data.c`, `r_cache.c` contract, `r_segs.c`,
  `r_plane.c`, `src/dg_saturn.cxx`, and the disc build. Multi-week.
- **DoomJo/shared-core impact.** **Large and shared.** This is a structural change to
  `texture_t` and the column-fetch contract that **both ports compile verbatim**. DoomJo's
  GCC 9.3 + its own VDP1/software paths must be re-validated. The WAD-format change affects
  DoomJo's disc too. `r_parallel.c` must stay pure C. High coordination cost.
- **HW/Ymir validation.** Ymir: visual identity of every wall/masked/sky surface vs today,
  the >3-patch cap, single-patch-slab residency. Hardware: fps (arithmetic addressing is
  *faster* per column than a double table lookup — potential perf **win** on top of the
  memory win), CD-read pattern with the new format, eviction churn.
- **Verdict.** The **complete** solution — removes the floor *and* may speed the column fetch.
  But it is a WAD-format + shared-`texture_t` change with a real correctness cap and a
  single-patch-residency hazard unique to DoomSRL's streaming (d32xr never purges). **The
  right end-state, too big as a first step.**

### Option E — Hybrid: keep post-based storage, build per-texture column-major slab into the LRU on demand (AMBITIOUS, DoomSRL-shaped)

- **Change.** A middle path that avoids the WAD-format change. Keep Doom's post-based lumps on
  disc. On first `R_GetColumn(tex,…)` for a multi-patch texture, build a **column-major
  slab** (composite of ALL columns, single- and multi-patch alike, laid `col*height`) into the
  LRU pool — i.e. composite the *whole* texture, not just multi-patch columns, so the slab is
  arithmetically addressable with **no directory**. Single-patch textures keep the direct-lump
  fast path (no slab, `collump>0`). Replace the two per-column arrays with a **per-texture
  flag** ("is this texture fully single-patch?") + the existing `texturewidthmask`/`height`.
- **Memory saved.** The per-column directories vanish (**~400–600 KB**); replaced by a
  `numtextures`-byte flag array (~1.4 KB) and the on-demand slabs in the bounded pool. For
  single-patch textures, **zero** extra (direct lump). For multi-patch textures, the slab is
  `width*height` (bigger than today's multi-patch-only composite, since single-patch columns
  are now also in the slab) — a **pool-pressure tradeoff**, not a static floor.
- **Risk.** **Medium-high.** (1) Multi-patch slabs are larger than today's composites (they
  now include single-patch columns) → more pool churn; measure the pool hit-rate. (2) The
  single-patch-column-inside-a-multi-patch-texture case: today those draw direct-from-lump
  (`collump[x]>0` even within a multi-patch texture); the hybrid forces them into the slab.
  This is a small fidelity-neutral change but a pool-size change. (3) `R_GetColumn` becomes:
  "fully-single-patch texture? → lump; else → slab[col*height]" — simpler than today, no
  `colofs` bifurcation. (4) Same LRU `userp` and `sat_xsplit` constraints as today
  (unchanged — still one owner slot per texture).
- **Effort.** High but **less than D** — no WAD-format change, no `strip_wad.py` work, no
  on-disc format, and it **reuses the existing `R_GenerateComposite` producer** to fill the
  whole-texture slab (extend it to composite single-patch columns too). The `decals` cap is
  **avoided** (still using `column_t` posts, not packed decals).
- **DoomJo/shared-core impact.** Shared, behind `sat_streaming_mode` (DoomJo eager/classic).
  No WAD-format coordination. Same makefile note as `r_cache.c`.
- **HW/Ymir validation.** Ymir: visual identity (esp. single-patch columns now drawn from the
  slab), pool hit-rate vs today. Hardware: fps (one less table lookup per column = possible
  win; bigger slabs = more CD reads on miss — net unclear, **must measure**).
- **Verdict.** **The DoomSRL-shaped sweet spot:** removes the full static floor, avoids the
  WAD-format change *and* the `numdecals≤3` cap, reuses the existing composite producer and
  LRU pool, and simplifies `R_GetColumn`. The cost moves from a **static floor** to **bounded
  pool pressure** (the thing already being managed). The open question is slab size vs pool
  capacity — measurable.

---

## 6. Cross-cutting risks & what needs measurement

1. **`R_WallPotatoColor` forces eager builds.** It calls `R_GetColumn` for every texture
   (`r_data.c:525-528`) at precache. Streaming mode skips precache, so this is latent — but
   **verify** it isn't reached on the streaming path, or any lazy scheme (C/D/E) is defeated.
2. **The fast-path cost is the swing vote.** PERF(1.4) (`r_data.c:42-43`) found `R_GetColumn`
   is not a RECORD-pass hotspot *today*. Any option that adds work to the **single-patch**
   branch (A's offset recompute) risks making it one. The VDP1 bake calls `R_GetColumn` twice
   per VRAM column on a miss; the software segs loop calls it per visible column. **Must A/B
   on hardware**, not just Ymir.
3. **Eviction races (C/D/E).** A directory or slab evicted while the slave holds a `dc_source`
   into it = corruption. The `sat_xsplit` guard (`r_cache.c:138,169`) must cover whatever new
   thing the pool now owns. This is the class that only manifests on hardware under churn.
4. **Single-patch slab residency (D, partly E).** d32xr never purges (ROM); DoomSRL streams
   purgable lumps. A `data[]` pointer into a purged single-patch lump dangles. E sidesteps by
   keeping the direct-lump fast path for single-patch textures; D must pin or pool them.
5. **`numdecals ≤ 3` correctness cap (D only).** Doom II/PWADs can exceed 3 overlapping
   patches. D must widen the field or keep a post-based fallback. C and E avoid this entirely
   (they keep `column_t` posts).
6. **Measure before committing:** (a) actual `numtextures` and `Σ width` for Doom II / TNT /
   Plutonia (the 400 vs 600 KB uncertainty); (b) distinct-textures-per-level working set (sets
   C/E's resident bound); (c) multi-patch fraction (sets E's slab-size blow-up); (d)
   `R_GetColumn` call count per frame on the VDP1 path (the fast-path-cost denominator).

---

## 7. Recommended phased plan

The cut-depth finding (§3) is decisive: **the directories cannot be freed, only made
unnecessary or on-demand.** Two viable end-states (C lazy-directory, E hybrid-slab); D is the
purist target but carries a WAD-format change + correctness cap not worth it for DoomSRL.

**Phase 0 — Measure (no code).**
- Instrument `R_InitTextures` to print real `numtextures`, `Σ width`, multi-patch fraction,
  and the `columnlump+ofs` byte total for Doom II / TNT / Plutonia / Ultimate. Resolve the
  400-vs-600 KB and the single-patch-fraction unknowns. Count distinct textures actually drawn
  per level (the C/E resident bound) and `R_GetColumn` calls/frame on the VDP1 path.
- **Gate:** if the real floor is <250 KB on the shipping WADs, deprioritize; if it's
  400–600 KB as estimated, proceed.

**Phase 1 — Option E (hybrid whole-texture slab into the LRU), streaming-gated.**
- Reuse `R_GenerateComposite` (extend it to lay **all** columns, not just multi-patch) to
  build a column-major slab into the existing `core/r_cache.c` pool on first
  `R_GetColumn(tex,…)`. Replace the two per-column arrays with a per-texture
  fully-single-patch flag. `R_GetColumn` → "single-patch texture? lump : slab[col*height]".
- Keep the **classic eager directory path** behind `sat_streaming_mode=0` (DoomJo
  byte-identical, like S4(a)). Same `sat_xsplit` / `userp` discipline.
- Validate on Ymir (visual identity of single-patch columns now from the slab; pool hit-rate)
  then hardware (fps; CD-read pattern). This recovers the ~400–600 KB floor while **avoiding
  the WAD-format change and the `numdecals≤3` cap**.

**Phase 2 — fold in Option B cleanups** (drop `texturecompositesize` as a stored array, pack
the sentinel) once E removes the per-column arrays they coexisted with. Small, free.

**Phase 3 (optional, only if pool pressure from E's bigger slabs proves real) — Option D.**
Move to the post-processed column-major on-disc format via `tools/strip_wad.py`, the packed
`decals[]`, and arithmetic addressing — but only after the `numdecals` cap is widened/handled
and the single-patch residency hazard is designed out, and only if E's pool churn measurement
demands it. This is the d32xr end-state; treat it as a separate chantier with its own
DoomJo + disc-format coordination.

**Do NOT** ship Option A standalone (half the win, adds hot-path cost). **Do NOT** start with
D (WAD-format + correctness cap + residency hazard as a *first* move).

---

## 8. Cross-references

- [`STREAMING_ANALYSIS.md` §S4(b)](STREAMING_ANALYSIS.md) — this plan **is** S4(b)
  ("composite-on-demand `decals[]` — deferred (heavier structural change)"). S4(a) (the
  bounded LRU pool, `core/r_cache.c`) is the **storage target** these options reuse; this
  document is the deferred producer-side half.
- [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) / [`VDP1_FLOOR_PLAN.md`](VDP1_FLOOR_PLAN.md) —
  the VDP1 wall bake (`src/dg_saturn.cxx:1686`) that funnels through `R_GetColumn`, proving
  the cut is a shared-core change, not a VDP1-local one.
- d32xr reference: `c:/Users/pcico/Projects/saturn-refs/d32xr/` —
  `r_local.h:155-182`, `r_data.c:119-124,936-1035`, `r_phase6.c:126-147`,
  `r_phase9.c:13-247`, `r_cache.c:102-250`.
- Key DoomSRL file:line anchors: `core/r_data.c:150-156,315-395,403-424,230-308,654-659,
  726-727,527`; `core/r_cache.c:40,124,138,159,169`; `core/r_segs.c:180,595,623,658`;
  `core/r_plane.c:1044`; `core/r_things.c:427,464,1111,1124`; `src/dg_saturn.cxx:1323,1649,
  1659-1660,1686-1687`.

---

## 9. PHASE 0 RESULTS — measured & adversarially verified (2026-06-24)

Tool: `tools/measure_texfloor.py` — parses each WAD's `TEXTURE1/2`+`PNAMES`, replicating
`R_InitTextures`/`R_GenerateLookup` **exactly** (column-by-column `patchcount`, the `-1`
multi-patch sentinel, `texturecompositesize += height` per multi-patch column). The floor
numbers were cross-checked by a second independent reader of the raw lump headers, and the
whole model (parser-vs-engine, the Option-E slab math, the alternative-lever ranking) was
adversarially verified by three independent auditors — **all high-confidence**.

### 9.1 The measured floor (exact, never-freed PU_STATIC)

| WAD | #textures | Σwidth | **floor = 4·Σw** | pure¹ | mno² | tmp³ | today composite⁴ | Option-E slab⁵ | slab/today | max single slab |
|---|---|---|---|---|---|---|---|---|---|---|
| Doom II | 428 | 40,240 | **157 KB** | 248 | 38 | 142 | 1012 KB | 2246 KB | **2.22×** | 32 KB |
| TNT | 562 | 64,736 | **253 KB** | 308 | 49 | 205 | 1719 KB | 4586 KB | **2.67×** | 128 KB |
| Plutonia | 505 | 51,840 | **202 KB** | 311 | 40 | 154 | 1241 KB | 2694 KB | **2.17×** | 128 KB |
| Ultimate | 287 | 29,912 | **117 KB** | 102 | 42 | 143 | 1218 KB | 2368 KB | 1.94× | 32 KB |
| Shareware | 125 | 10,776 | 42 KB | 34 | 12 | 79 | 659 KB | 976 KB | 1.48× | 32 KB |

¹ **pure** = single patch covering the whole texture → direct-from-lump, **never needs a slab/directory**.
² **mno** = >1 patch but no column overlap (every column `patchcount==1`) → today direct-from-lump per column; **under Option E it MUST be slabbed** (a per-texture flag cannot encode the per-column lump/offset — verified).
³ **tmp** = ≥1 multi-patch column → needs a composite today, a slab under E.
⁴ **today composite** = Σ over tmp of `height × (#multi-patch cols)`, *if every tmp texture were drawn* (upper bound; built on-demand into the LRU).
⁵ **Option-E slab** = Σ over (mno+tmp) of `height × width`, *if all drawn* (upper bound).

### 9.2 Per-map working set (SIDEDEFS-referenced wall textures, the LRU resident bound)

Worst maps (referenced wall-texture set, Option-E slab footprint if co-resident — an upper
bound; the LRU only holds the *visible* sub-set):

| WAD | worst map | #ref tex | Option-E slab | today composite |
|---|---|---|---|---|
| Doom II | MAP19 | 72 | **572 KB** | 311 KB |
| Doom II | MAP08 | 66 | 533 KB | 272 KB |
| Plutonia | (similar) | — | >600 KB | — |

The pool caps at **256 KB** (`TEXCACHE_MAX`). Even *today's* composites for a big map's
referenced set (311 KB MAP19) exceed the cap → the LRU already evicts within a map (correct:
visible set < referenced set). Option E ~2.2× that pressure against the same 256 KB cap.

### 9.3 Verified findings

1. **The floor is 117–253 KB, not 400–600 KB.** Gate (§7): "deprioritize if <250 KB" → Doom II
   (157), Plutonia (202), Ultimate (117) are all under; only TNT (253) marginally clears.
2. **Option E makes pool pressure WORSE, on two axes** (verified high-confidence): (a) each slab
   is `height×width` vs today's `height×mpc` (2.2–2.7× bigger) → the 256 KB pool holds 2.2–2.7×
   *fewer* textures → lower hit-rate; (b) it pulls the **mno** textures and the single-patch
   columns of mixed textures — which today cost the pool **zero** (direct-from-lump) — *into* the
   bounded pool. More misses = more composite rebuilds + more CD re-reads in streaming mode. It
   also loses the `lump>0` direct-lump fast path. The win is real (~157 KB static → gone) but the
   honest cost is higher dynamic churn + CD traffic, not lower. **mno MUST be slabbed** (a single
   per-texture flag cannot encode per-column lump/offset — confirmed from `R_GetColumn`).
3. **The cheaper levers already shipped:** visplane span arena (`SAT_VISPLANE_POOL=1`, ~99 KB vs
   the 256-cap baseline / ~265 KB vs vanilla — **now landed**, contra the stale STREAMING §0 note),
   `RP_CMD_BUF` 160→80 KB (+80 KB zone), `MAXVISPLANES=256` (~166 KB), f_wipe cut (~143–192 KB
   transient), the S4(a) LRU pixel cache. The texture-directory refactor is the **most expensive,
   highest-risk** remaining lever (shared-core, every wall/masked/sky path + the VDP1 bake,
   DoomJo GCC-9.3 re-validation, eviction races, + WAD-format/`numdecals≤3` cap for D).
4. **The real remaining bind is PU_LEVEL geometry (700–900 KB on big maps)** — which this refactor
   does **not** touch. The 157 KB floor is ~16–18 % of the zone / ~40–45 % of the PU_STATIC
   residents, small next to the geometry it competes with.
5. **Candidate cheap unblock (verify on Ymir first):** `R_SetupTextureCaches` sizes the LRU carve
   from `Z_LargestFreeBlock` (**pure-free only**, `r_cache.c:99`), while the real reclaimable run
   is `Z_LargestAllocatable` (**purge-inclusive**, already shown as overlay `ZON mx`). At
   level-load the zone is full of just-loaded PU_CACHE graphics, so pure-free (`TXC lf`) is tiny
   while `mx` may be large. If the overlay shows `mx ≫ lf`, switching the carve to size from
   purge-inclusive space unblocks the LRU **with a ~1-line change and zero shared-core risk**.
   Tradeoff: it purges freshly-loaded level graphics at carve time (one-off re-read); measure.

### 9.4 GATE VERDICT — **DEFER the texture-directory refactor (C/D/E).**

Not justified now: the floor is a third of the estimate and under the deprioritize gate; Option E
(the recommended path) would worsen the pool churn it aims to relieve; the cheap levers already
shipped; and the dominant zone consumer is PU_LEVEL geometry, untouched by this work.

**Recommended order instead:** (1) validate the pending CD-read-alignment fix + S4(a) LRU on
Ymir/hardware across all 32 maps, and read `TXC lf` vs `ZON mx` to test §9.3(5); (2) if the carve
is starved by pure-free accounting, ship the §9.3(5) one-liner; (3) if specific big maps still
`I_Error` at load, attack **PU_LEVEL geometry** directly (per-level bump arena S4(c), geometry
trims) — that is where the bytes are; (4) revisit this refactor **only** if, after the above, the
carve is *still* starved **and** on TNT-class WADs where the floor exceeds 250 KB — and then prefer
**Option C** (lazy directory, keeps the small `height×mpc` composites) over E for DoomSRL's real
WADs, since C caps the actual floor *without* the 2.2× slab inflation that E adds.
