# VDP1 / CPU capacity study (1j / 2j / 4j) — post-sprites headroom & effects

> **STATUS: REFERENCE (session synthesis 2026-07-02).** Consolidated CPU + VDP1 capacity
> ledger for the three player modes, the sprite-deport question, and what the leftover
> capacity can buy. Built from a multi-agent sweep of the docs + live source + saturn-refs
> (SlaveDriver) + the SRL/SGL VDP1 surface, then reconciled against two owner corrections
> (see §0). Companion to [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) (chip cost model),
> [`CRITICAL_PATH.md`](CRITICAL_PATH.md) (HW frame breakdown), [`WALL_SUBDIVISION_STUDY.md`](WALL_SUBDIVISION_STUDY.md)
> (the wall-clamp / floor-quad roadmap this feeds).

Tags: `[HW]` real Saturn · `[Ymir]` emulator (understates memory-bound) · `[src]` verified in
current tree · `[est]` doc/community estimate.

---

## 0. Two load-bearing corrections (owner, HW-proven) — read first

1. **`Dr%` is NOT a VDP1 fill gauge.** The VDP1 done-rate (overlay row 2, `Dr 25-42%` at pot0)
   is dominated by the **CPU↔VDP1 present desync (phase/latency)**, **invariant to VDP1
   workload** — proven on HW: on the fastest scene, demand `F31` (~0.4 screen) still read
   `Dr 45%` (see [`vdp1-present-tearing-diagnosis`] memory / `VDP1_PRESENT_SYNC_PLAN.md`).
   → **VDP1 fill headroom is LARGE** (frame is master-bound, walls fill in ~few ms of a
   72-130 ms frame). The earlier "sprite fill competes for a tight VDP1 budget" reasoning is
   **wrong**. VDP1's real scarcity is **command slots + VRAM + the layer architecture**, not fill.
   The present/sync itself is a **separate session** — not analysed here.

2. **Sprite priority is a config, not a hard blocker.** `slPrioritySpr0..7(5)` — all 8 sprite
   priority registers are set to 5 by choice ([dg_saturn.cxx:1947](../src/dg_saturn.cxx)). VDP1
   has per-pixel sprite priority (sprite-type). The real constraint is subtler: the sprite-TYPE
   is **global** to VDP1, and the 8bpp-256-colour wall model uses the whole pixel for the index,
   leaving no per-pixel priority bits → all VDP1 output shares one priority register. Giving
   sprites a distinct priority trades against wall colour depth — a real coupling, dissolved
   cleanly only by the **all-VDP1-world model** (walls+floors+sprites in one painter list).

---

## 1. Baseline (1j, potato-0) — the master-bound wall

`fps = 1000/MST` holds exactly ([dg_saturn.cxx:998](../src/dg_saturn.cxx)) → **100% master-CPU-bound.**
HW frame breakdown (`REC_BENCHMARKS.md §C.2`, `CRITICAL_PATH.md`) `[HW]`:

| Phase | Cost pot0 | Offloadable? |
|---|---|---|
| **P** planes/floor | **~62 ms (46-48%)** | ✅ drained to RBG0 (the realized lever) |
| **Bp** wall-prep | ~17-22 ms | tried→DEAD (slave +5.8 ms, memory-bound, 3×) |
| **Bw** BSP walk | ~7-8 ms | ❌ serial occlusion recursion |
| **M** masked/sprites | **~6 ms calm / PK 24-26 ms combat (potato-INDEPENDENT)** | ½ on slave already |
| blit | ~5.3-5.9 ms | dual-CPU REJECTED (bus-bound) |

P collapses `62→17→11` pot0→1→2; **Bp/M do NOT collapse**. Slave idle `29%→54%→62%`, but the
**master is the long pole everywhere → slave-only gains buy 0 fps.**

**Scarce vs abundant** `[src]`:

| | Value |
|---|---|
| VDP1 **fill** | abundant (frame master-bound; `Dr` = present artifact, not load) |
| VDP1 **command bank** | 256/bank, cap `WALL_CMD_CAP=248`, accumulator `WALL_ACC_MAX=120` (1p peak ~57 walls / ~142 cmds) — **HWRAM .bss/TLSF-gated** |
| VDP1 **VRAM** | 512 KB; wtex 448 KB (22 slots); **~44 KB genuinely free** (12 KB tail + 20 KB dead HUD + 12 KB tail). Dead WPN cache 176 KB **aliases** wtex-wide → reclaims **0** |
| VDP2 **VRAM** | 4×128 KB; **0 free banks in 1j ship** (A0=K, A1=floor bitmap, B0=NBG1 fb, B1=cell sky+NBG3+RPT); A1 frees to 128 KB when the floor is software |
| **HWRAM TLSF pool** | **~57 KB currently** (`_end 0x060ec000 → __heap_end 0x060fa000`) — after the **2026-06-30 lumpinfo→LWRAM-zone fix** (moved lumpinfo to `Z_Malloc` + trimmed `HEAP_SIZE` 88→32 KB → pool 4 KB→~58 KB). The capacity-sweep's "6.8 KB" was a **pre-fix** reading. Still the boot-loop chokepoint — **check the map before every handoff** ([`boot-loop-can-be-tlsf-pool-starvation`]) |
| **LWRAM zone heap** | ~944 KB, but SLOW bank (rL≈2.1 HW) |
| master **build-`k`** | the wall-command build (perspective math + writes) = **11-28 ms** measured; more subdivision ⇒ more `k` on the bottleneck |

---

## 2. Per-mode ledger (1j / 2j / 4j)

Structural facts `[src]`: views render **sequentially on the master** (per-CPU 2nd renderer
REJECTED, `SAT_XSPLIT=0`, overflows 2 MB) → 2j/4j REC ≈ v0+v1(+v2+v3) **added**, S bus-factor
moot. In split the **slave is busy** (phase-split) → no dual-CPU blit. **VDP1 fill/erase is
full-screen every mode** (doesn't shrink with viewports). Per-view VDP1 wall budget =
`WALL_ACC_MAX/nv`: 1j=120, 2j=60, 4j=30; `4×30 ≤ 248` bank → cannot starve, overflow degrades to software.

| Budget | 1j | 2j | 4j |
|---|---|---|---|
| Geometry | 1×320×192 | 2×160×160 +HUD | 4×160×112 (no HUD) |
| CPU/REC | Bw8·Bp21·P15·M4 ≈48 ms → ~13 fps `[HW pot1]` | v0+v1 add; console pot0 ~7 fps `[HW]`; Ymir pot1 ~21 | **no bench** — Bw×4 tax dominant; dense ~8 fps `[est]` |
| Long pole | P (floor) | Bp×2 + P×2 | **Bw×4** |
| Floor | RBG0 (now valid **1j+2j**, owner update) | RBG0 (owner: now valid) / else software | software |
| System RAM | **mode-invariant** (no per-player buffers) | = | = |
| VDP1 fill | idle | idle (smaller views) | most idle |
| VDP1 cmd | 120 (peak ~57) | 60/view | 30/view (tight → sw fallback) |

**Key insight:** the three RAM budgets are mode-invariant; only **CPU REC time** and the
**per-view command split** scale with player count. VDP1 fill and VDP2 banks get *freer* in split.

---

## 3. Deporting enemy sprites to VDP1

**Prize:** frees the **M term** off master+slave — ~6 ms calm but **PK 24-26 ms in combat,
potato-independent** (up to ~1/3 of a pot2 frame). Not the calm-scene headline, but a **real
combat win**. **Cost is NOT fill** (abundant) — it is: shared command bank (sprites vs walls),
VRAM sprite cache, and the **architecture** (layer inversion / painter merge).

Corrections to earlier pessimism:
- **No cmap texture churn** if sprites use the **8bpp + CRAM-bank lighting** model (like walls,
  [dg_saturn.cxx:2621](../src/dg_saturn.cxx)): texel = raw index, one cached texture serves all
  distances; cache key `(lump,rotation)`, not `(lump,cmap)`. The "churn killer" (assumed the dead
  RGB555 weapon path) collapses.
- **Occlusion is standard, not a blocker:** sprites are already sorted far→near; in one VDP1
  painter list, occlusion is free (SlaveDriver). The "layer inversion" only bites in the *current
  hybrid* — it dissolves in the all-VDP1-world model, which is where sprites+floors belong together.

**Net:** a real, mode-dependent win (biggest in combat / split), not a fps unlock in calm 1j
(master stays P/Bp/Bw-bound). Requires the priority/clipping work → couples to the floor-quad bet.

---

## 4. What the leftover capacity buys (ranked, corrected)

VDP1 fill stays abundant after sprites; effects must be **command-cheap, VRAM-cheap, not
master-loading.**

| # | Effect | Capacity | Verdict |
|---|---|---|---|
| 1 | **Mesh-dither** (spectres, fuzz, plasma, glass) | fill (MESHon **free**) | **TOP** — SlaveDriver uses it for water; works on 8bpp; dodges the 6× trap |
| 2 | **Dynamic CRAM light-bank flash on WALLS** (muzzle/explosion) | 0 fill, vblank CRAM | **feasible TODAY** — walls already select CMDCOLR bank; no sprites needed |
| 3 | **Blob shadows under sprites** (`COMPO_SHADOW` = halve) | 1 cmd/actor | cheap grounding; conditional on sprites-VDP1 |
| 4 | **COAR/COAG/COAB screen flash/fade** | ~0/pixel | **CAVEAT**: SRL `UseColorOffset` covers ScrollScreens only, **NOT the VDP1 sprite layer** ([srl_vdp2.hpp:682]); needs raw `slColOffsetScrn(scnSPR)` — not "one register" |
| 5 | **Non-render slave work** (sight/sfx→slave) | slave (idle-during-BSP) | **the only slave lever that touches master time** |

**Traps (confirmed):** gouraud on 8bpp = garbage (shifts the palette index, [dg_saturn.cxx:2265](../src/dg_saturn.cxx)) → keep CRAM banks; large half-transparency = **6× fill** → use mesh; higher internal res = pointless (master-bound); full-16bpp SlaveDriver world = VRAM/regression.

---

## 5. Bottom line
The system is **master-CPU-bound**; the fps levers are **P (on RBG0, done), Bp (dead to
offload), Bw (serial), and non-render slave work**. VDP1 has abundant fill and a partly-idle
slave, both bridled (command slots; master-bound). Deporting sprites/floors to VDP1 is
attractive from a *fill* standpoint and is a **quality** win (coherent single-pipeline planes),
but does not lift the master floor in calm 1j — it pays in **combat and split**. The cheap,
safe, immediately-useful wins are the **wall CPU-fallback clamp** (see
[`WALL_SUBDIVISION_STUDY.md`](WALL_SUBDIVISION_STUDY.md)), **mesh-dither**, and **CRAM
wall-flash**.
