# Parallel-REC audit — putting the slave SH-2 on the render GENERATION

**Question:** how to make the second SH-2 do REC work (wall-prep / planes / sprites generation),
not just draw columns, to break the master-only REC ceiling (the 1p speed bonus + multiplayer).

**TL;DR:** the "run a full independent renderer per CPU" model (duplicate everything) **does not fit
the Saturn's 2MB**. The viable, proven model is **d32xr's phase-split**: ONE shared render state, work
split by phase via producer/consumer, ~24 bytes of per-CPU state in GBR-TLS. Reference:
`C:\Users\pcico\Projects\saturn-refs\d32xr` (Doom 32X: Resurrection, same hardware class: 2× SH-2,
software, no TMU). DeepWiki: https://deepwiki.com/viciious/d32xr.

---

## 1. Why duplication fails (measured)

The x-split attempt dual-compiled the 6 render TUs (`r_bsp/segs/plane/things/main/draw`) into a second
`slave_`-prefixed renderer with its OWN copy of every global. Measured cost:

| Item | Size |
|------|------|
| Slave render BSS (duplicated tables/arrays) | **117 KB** (r_plane 52, r_main 29, …) |
| Slave stack (Doom's deep BSP recursion needs ~40KB, like the master) | **~40 KB** |
| Slave visplane pool (`Z_Malloc`, MAXVISPLANES=512) | **332 KB** (LWRAM) |
| **HWRAM heap free** (measured, `__heap_start`→`work_area_start`) | **~96–124 KB** |
| **LWRAM** (zone 864KB + cmd-buf 160KB) | **full** |

→ `_end` overran the SRL TLSF heap region → `tlsf_add_pool: Memory size must be between 20 and …` →
boot crash. The 2MB is full; duplication is ~430 KB over. **Dead** without a 4-part memory surgery
(source-level read-only sharing + array reduction + cmd-buffer shrink to move the slave to LWRAM +
visplane pointer-partition), and even then the margins are ~nil.

Note: `objcopy --weaken-symbol` does **not** reclaim BSS (it only changes link precedence; the storage
stays). Sharing a TU-defined global needs a source-level `extern` under the slave define — invasive.

## 2. How d32xr does it (the model to copy)

**No duplication. One shared render state, work split by PHASE.**

1. **Secondary = persistent mailbox loop** (`marsnew.c:334` `Mars_Secondary`): blocks on a comm
   register (`while ((cmd = MARS_SYS_COMM4) == NONE)`), runs whole **jobs**:
   `R_WALL_PREP` (= `Mars_Sec_R_WallPrep` + `R_SegCommands` + `R_PreDrawPlanes`), `R_DRAW_PLANES`,
   `R_DRAW_SPRITES`, plus non-render jobs (`P_CheckSights`, fire anim, melt wipe, sound DMA).
2. **Shared state, not duplicated:** a `vd` (viewdef) struct — `vd->visplanes`, `vd->viswalls`,
   `vd->vissprites`, `vd->lastwallcmd` — that BOTH CPUs read/write, partitioned by phase.
3. **Wall-prep = producer/consumer** (`r_phase2.c:346`): the secondary processes segs **as the master's
   BSP emits them**, via comm registers `addedsegs`/`readysegs` (`MARS_SYS_COMM6`); `-2` = BSP done.
   Real overlap of BSP(master) ‖ wall-prep(secondary), not a draw split.
4. **Sprites/planes split by SCREEN HALF** between the CPUs (`Mars_Sec_R_DrawSprites(sprscreenhalf)`).
5. **Per-CPU state = 24 bytes in GBR-TLS** (`doomdef.h:1354`): `DOOMTLS_BANKPAGE(0)`,
   `SETBANKPAGEPTR(4)`, `VALIDCOUNT(8)`, `COLUMNCACHE(12)`, `COLORMAP(16)`, `FUZZPOS(20)`. Each CPU
   `ldc gbr` to its own `mars_tls_pri`/`mars_tls_sec`. Plus per-call SCRATCH on the **stack**
   (`clipbounds[161]`, a local visplane hash) — small, not duplicated globals.
6. Author/community result: **2–4×** over the original 32X port.

**Key numbers:** per-CPU cost ≈ **24 bytes (GBR-TLS) + ~1 KB stack scratch** — vs the **117 KB** of the
duplication model. THAT is why it fits.

## 3. Options for DoomSRL (pros / cons)

| # | Option | Fits? | Gain | Cost / risk |
|---|--------|-------|------|-------------|
| **A** | **Full duplication** (the x-split as built) | ❌ no (~430KB over) | 1p-bonus + 4p | **dead** without major memory surgery |
| **B** | **d32xr phase-split, full** — slave does wall-prep (producer/consumer) + planes + sprites-by-half; shared state; 24B GBR-TLS | ✅ yes | **2–4×, proven** (1p AND multi) | **big renderer refactor** (producer/consumer protocol, scratch→stack, mutable→GBR-TLS) + freeze-zone cache coherency |
| **C** | **d32xr phase-split, incremental** — start with ONE self-contained phase (sprites-by-half, or planes) | ✅ yes | partial (~M or ~P) | **low** — isolated phase, small coherency surface; de-risks B |
| **D** | **x-range split + SHARED arrays** (disjoint-x access) + scratch/draw-state in GBR-TLS | ✅ likely | 1p-bonus + multi | medium — must partition the allocators (visplanes/drawsegs); less proven than d32xr |

**Notes**
- **B/C keep the 1p bonus:** the phase-split accelerates EACH view (intra-view parallelism), so 1p
  benefits; multiplayer = views rendered SEQUENTIALLY, each phase-accelerated (exactly d32xr's
  2-player splitscreen, extended).
- **Reusable from the x-split work:** the dispatch infra (`slSlaveFunc` + `rp_sgl_workptr_reset` + a big
  slave stack) and the x-range clip (`sat_view_x0/x1`, r_bsp.c) — the clip = the "screen half" for the
  draw-phase split. The **dual-compile (duplication) is discarded.**
- **DoomSRL vs d32xr:** d32xr refactored Doom's globals into the `vd` struct; DoomSRL kept globals —
  but globals are ALREADY shared (same RAM), so the adaptation = move only the SMALL mutable state
  (`dc_*`, `fuzzpos`, `validcount`, the `rw_*`/seg temporaries during wall-prep) to GBR-TLS / stack,
  not a full struct refactor.
- **The freeze-zone** (the perf notes' fear about the "2.4" wall-prep offload): d32xr **proves it's
  manageable** with the right protocol — comm-register handoff + `Mars_ClearCacheLine` + phase ordering.

## 4. Recommendation

**Option B, entered via C.** Start with **one isolated phase** (recommended: **sprites by screen-half**
— it's `M`, small, self-contained, low coherency; or **planes**) to validate the trio
*producer/consumer + GBR-TLS + cache coherency* on Ymir + hardware, then extend to the wall-prep
producer/consumer (the big `Bp` lever).

## 5. Leads to study (local `saturn-refs/d32xr/`)
- `marsnew.c` — the mailbox loop + GBR-TLS setup (`Mars_Secondary`, `I_Init`).
- `r_phase2.c` — the wall-prep producer/consumer (`Mars_Sec_R_WallPrep`, `addedsegs`/`readysegs`).
- `doomdef.h:1354` — the `DOOMTLS_*` offsets; `mars.h` — the `Mars_Sec_*` job list.
- The `vd`/viewdef shared struct — the heart of the model.
- Web: https://deepwiki.com/viciious/d32xr · https://doomwiki.org/wiki/Doom_32X:_Resurrection
