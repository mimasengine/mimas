# L2 — Command buffer → fast HWRAM : spec prête-à-coder

Statut : **RELOCATE = SPEC (non codé)** ; **SHRINK = SHIPPÉ** (voir RECONCILED ci-dessous).
Budget mesuré : voir `docs/REC_REDUCTION.md` L2 + ce doc §6. Gain **HW-only et invisible sur Ymir**.

> **RECONCILED 2026-06-24 :** La moitié SHRINK est **déjà shippée** via Makefile
> `-DRP_CMD_BUF_SIZE=0x14000` = **80 KB** (PAS le 0x10000/64 KB de cette spec). Le buffer
> reste en haut de LWRAM (adresse dérivée `0x300000-SIZE`), `DG_ZoneBase` rend les 80 KB
> à la zone heap streaming. Seule la moitié **RELOCATE** (buffer → HWRAM BSS) reste non codée.
> Le gate §8 « coder après RBG0 » est **caduc** : RBG0-floors a cassé sur HW (snow + ciel
> mort) et a été reverté — il n'y a plus de session RBG0 vivante à coordonner.

## 1. Objectif

Le buffer de commandes maître→esclave est en **LWRAM lente** (`0x002D8000`). Chaque
commande = 32 B écrits par le maître + 32 B lus par esclave/maître. Le mettre en
**HWRAM rapide** (SDRAM) réduit la latence de ce trafic → cible **REC + EX**.
Effet de bord : l'esclave lit les commandes ~1.5× moins vite que le maître sur HW
(banque lente, mesure 2.5) ; en HWRAM rapide l'écart se réduit → **le point
d'équilibre du work-steal 2.5 (et du blit dual-CPU) peut bouger** → re-mesurer W.

## 2. État actuel (refs)

| Fait | Ref |
|---|---|
| `RP_CMD_BUF_ADDR 0x002D8000`, `RP_CMD_BUF_SIZE 0x28000` (160 KB = 5120 cmds) | [core/r_parallel.h:9-10](../core/r_parallel.h#L9-L10) |
| `RP_CMDS = (rp_cmd_t*)RP_CMD_BUF_ADDR`, `RP_MAX = SIZE/32` | [core/r_parallel.c:93-94](../core/r_parallel.c#L93-L94) |
| Overflow valve : `if (rec_count==RP_MAX) rp_flush();` (drain mi-frame, pas de corruption) | [core/r_parallel.c:1059](../core/r_parallel.c#L1059) |
| Zone heap = `LOW_WORK_RAM_SIZE - RP_CMD_BUF_SIZE` (buffer carvé en haut de LWRAM) | [src/dg_saturn.cxx:399-403](../src/dg_saturn.cxx#L399-L403) |
| Pointe `c` observée ~1068 (pot0), 116-390 (pot1/2) | docs/REC_BENCHMARKS.md |
| HWRAM : `_end 0x060da9e0` → `__heap_end 0x060fa000` = réserve heap C ~125 KB | build/Mimas.map |

## 3. Le changement (4 edits, gatés pour A/B HW)

**Taille : shrink à 64 KB = 2048 cmds** (1.9× la pointe ~1068 ; `rp_flush` reste le
filet si dépassé). Garder la **même taille dans les deux bras** A/B pour n'isoler que
la banque.

> **RECONCILED 2026-06-24 :** le shrink a en fait SHIPPÉ à **0x14000 (80 KB)**, pas
> 0x10000 (64 KB). Le RELOCATE doit réutiliser cette taille **80 KB déjà shippée** (ou
> shrinker plus), et **re-vérifier le build.map** : +80 KB BSS pousse `_end` 0x060da9e0
> → ~0x060eea9e0, laissant **~45 KB** de heap C (et non les ~61 KB calculés ici pour 64 KB).

### 3a. `core/r_parallel.h`
```c
/* RECONCILED 2026-06-24: SHIPPED value is 0x14000 (80KB) via Makefile -D, NOT 0x10000.
   RP_CMD_BUF_SIZE is already -D-overridable in r_parallel.h; keep the shipped 80KB. */
#define RP_CMD_BUF_SIZE  0x00010000   /* spec was 64KB = 2048 cmds (was 0x28000/5120) */

#define RP_CMD_BUF_IN_HWRAM 1         /* A/B: 1 = fast HWRAM BSS, 0 = legacy slow LWRAM addr */
#if RP_CMD_BUF_IN_HWRAM
extern rp_cmd_t rp_cmd_buf[];         /* placed in BSS = fast high-WRAM by the linker */
#else
#define RP_CMD_BUF_ADDR 0x002D8000
#endif
```

### 3b. `core/r_parallel.c`
```c
#if RP_CMD_BUF_IN_HWRAM
rp_cmd_t rp_cmd_buf[RP_MAX];          /* BSS; +64KB high-WRAM, shared by both ports */
#define RP_CMDS  rp_cmd_buf
#else
#define RP_CMDS  ((rp_cmd_t *)RP_CMD_BUF_ADDR)
#endif
```
(`RP_MAX` reste `RP_CMD_BUF_SIZE/sizeof(rp_cmd_t)` ; déplacer sa def ou `rp_cmd_buf`
après pour l'ordre de déclaration.)

### 3c. `src/dg_saturn.cxx` `DG_ZoneBase` (Mimas)
```c
#if RP_CMD_BUF_IN_HWRAM
    *size = LOW_WORK_RAM_SIZE;                 /* buffer no longer in LWRAM -> reclaim full 1MB */
#else
    *size = LOW_WORK_RAM_SIZE - RP_CMD_BUF_SIZE;
#endif
    return LOW_WORK_RAM;
```

### 3d. DoomJo `project/dg_saturn.c` — **même** changement DG_ZoneBase (core partagé →
`r_parallel.h` bascule pour les deux ports). Puis re-build GCC 9.3 + bump core/DoomJo
([[doomjo-core-sync]]). **Vérifier le budget HWRAM de DoomJo séparément** (layout différent).

## 4. Sécurité overflow

`rp_flush()` est déjà la soupape (drain mi-frame, re-arm `rec_count`) — shrinker la rend
seulement plus précoce. À 2048 vs pointe ~1068 elle reste **dormante**. Si on craint une
scène > 2048 non vue : alternative **96 KB / 3072 cmds** (2.9× marge) mais ne laisse que
~29 KB de heap C (§6). Reco : **64 KB**, `rp_flush` couvre le reste. (Valider une fois que
`rp_flush` est ré-exercé — c'est un chemin rare ; si possible logguer s'il fire en jeu.)

## 5. Cohérence (zone freeze)

Les deux banques sont cacheables write-through → **protocole de lecture esclave
inchangé** (purge CP / miroir uncached `|0x20000000` pour les vars SYNC). MAIS la
staleness esclave hardware connue (telemetry **RPBAD row-15** : le maître écrit
write-through, l'esclave peut lire une ligne RAM pas encore évincée — HW-only) doit être
**re-validée sur la nouvelle banque** : SDRAM (HWRAM) a un timing d'éviction différent de
la DRAM (LWRAM). Protocole HW : activer RP_CDIAG, lire RPBAD n au 6 spots sur le bras
HWRAM ; n doit rester 0 comme aujourd'hui.

## 6. Budget

- **Mimas** : *(RECONCILED 2026-06-24 : pour la taille SHIPPÉE 80 KB, +80 KB BSS pousse
  `_end` 0x060da9e0 → ~0x060eea9e0, laisse **~45 KB** de heap C — re-vérifier le build.map
  avant de committer le RELOCATE.)* Chiffres spec d'origine (64 KB) : +64 KB BSS pousse
  `_end` 0x060da9e0 → 0x060ea9e0, laisse **~61 KB** de
  heap C (sur ~125). OK **ssi** malloc (newlib, distinct du Z_Malloc LWRAM) < 61 KB →
  **à vérifier** (grep usages malloc/calloc/sbrk ; Doom utilise Z_Malloc, donc heap C
  probablement quasi vide). Si le link échoue (overflow region) → réduire à 48 KB (1536)
  ou garder le buffer en LWRAM (flag 0).
- **LWRAM** : zone heap **864 KB → 1 MB** (+160 KB) — bonus textures/lumps moins rechargés.
- **DoomJo** : re-vérifier (game.elf actuel 1.54 MB ; +64 KB BSS).

## 7. Protocole A/B hardware (au retour HW, ~21/06)

1. Bras HWRAM (`RP_CMD_BUF_IN_HWRAM 1`) vs bras LWRAM (`0`), **même taille 64 KB**.
2. Rows 19 (`REC EX W c`) + 20 aux 6 spots. Attendu : **REC + EX ↓** sur le bras HWRAM
   (latence banque). Surveiller **W** (l'esclave lit désormais en banque rapide → l'équilibre
   2.5 peut bouger ; re-cycler les ratios blit si besoin).
3. RPBAD row-15 = 0 sur les deux bras (cohérence).
4. Si gain net ≤ bruit → garder LWRAM (flag 0) et clore L2.

## 8. Coordination RBG0 (session parallèle) — **CADUC**

> **RECONCILED 2026-06-24 :** ce gate est annulé. RBG0-floors a **cassé sur HW** (snow +
> ciel mort : RAMCTL/cycle-pattern jamais committé sans slSynch) et a été reverté — il
> n'y a **plus de session RBG0 vivante** à coordonner. Le hunk `DG_ZoneBase` du RELOCATE
> peut être codé directement.

~~RBG0-floors touche très probablement **`dg_saturn.cxx`** (setup VDP2/RBG0) et la
floor-path de `r_plane.c`. **L2 touche `dg_saturn.cxx` (DG_ZoneBase)** → **point de merge
commun**. Coder L2 **après** RBG0 et rebaser le hunk DG_ZoneBase dessus. `r_parallel.{c,h}`
n'a pas d'overlap attendu avec RBG0.~~

## 9. Fichiers & revert

`core/r_parallel.h`, `core/r_parallel.c`, `src/dg_saturn.cxx`, DoomJo
`project/dg_saturn.c`. **Revert trivial** : `RP_CMD_BUF_IN_HWRAM 0` (revient au buffer
LWRAM legacy ; remettre `RP_CMD_BUF_SIZE 0x28000` si on veut aussi annuler le shrink).
