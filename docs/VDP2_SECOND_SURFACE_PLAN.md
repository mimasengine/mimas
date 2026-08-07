# VDP2 — obtenir une DEUXIÈME surface matérielle (plafond dominant)

> # ⚠️ CE DOCUMENT EST PARTIELLEMENT RENVERSÉ
> **Lire d'abord [`VDP2_SECOND_SURFACE_ZONES.md`](VDP2_SECOND_SURFACE_ZONES.md) (2026-08-02).**
> Ce qui tombe :
> - **§0.2 / §4.2 « le chemin est déjà armé »** — FAUX. `KAst`/`ΔKAst`/`ΔKAx` ne descendent **jamais**
>   en VRAM (désassemblage exhaustif). Le chemin est **mort**, pas armé.
> - **§6 seuil « 2,5 ms »** — FAUX d'un facteur 4 à 12. Le seuil calculé est **`P − pr` ≥ 10,6 ms**
>   pour +1 fps ; le budget mesuré est ≤ 10,1 ms. Et l'enveloppe correcte est **`P − pr`**, pas
>   `P − pr − w` (`w` est latché et fait partie du récupérable).
> - **§7 cibles E1M3 et E1M6** — RETIRÉES (bilans −11,0 et −10,5 après re-mesure du ciel à deux
>   estimateurs). Seule E1M4 survit, pour +0,2 fps.
> - **§4.2 / §6.4 C2′** — ABANDONNÉ (rang 12/14). **§3.3 correctif RPT `0x54→0x60`** — À L'ENVERS.
> - **§3.5 block-flush** — incomplet : l'**ISR vblank pousse `0x000..0x11E`** (TCR lu comme des octets).
> - **§5 pool** — périmé (6 valeurs en 2 jours). **§8.2 « deux banques en RDBS=11 »** — disparaît si
>   `RAMP == RBMP`.
>
> Ce qui **tient** : §3.1 (RBG1 mort), §3.2 (l'impasse RPA/RPB tombe, bug `MPOFR`), §1.1 (`P` contient
> `sat_walls_kick`), §2.2 (priorités), §1.3 (le plafond est fragmenté — et c'est pire que mesuré).

> Étude commandée le 2026-07-31, rendue le 2026-08-01. Branche `flicker-clean`.
> Méthode : manuel SEGA **ST-58-R2** (VDP2 User's Manual, dans `refs/JUNE96_DTS.ISO` →
> `/d/DOCUMENT/SATURN/PROGRAM2.PDF`) + lecture du code + **désassemblage de `build/Mimas.elf`**.
> Chaque affirmation est étiquetée **MESURÉ** / **CODE** / **MANUEL** / **INFÉRÉ**.
> Une mémoire de projet n'est jamais une preuve ici : les 7 impasses connues ont été rejugées (§3).

---

## 0. Verdict en dix lignes

1. **RBG1 est mort**, quatre fois, sur du manuel verbatim. Ce n'est pas « RBG1 coûte NBG0 » : il tue **tous** les scrolls normaux, donc **NBG1 = le framebuffer software de Doom**. §3.1
2. **Le deuxième plan existe quand même** : c'est le **bi-paramètre RPA/RPB de RBG0** (`RPMD=2`). Mimas est **déjà** dans ce mode. §4.2
3. **La cause de l'échec passé est identifiée dans le binaire** : `slBitMapRbg0` écrit `MPOFR` en mot entier et met `RBMP` à une banque hors VRAM. Plus une seconde cause : la copie RPT de RPB écrase ses 24 premiers octets. §3.2
4. **La prémisse du brief est fausse** : `P` n'est pas « les plafonds », ce n'est même pas `R_DrawPlanes`. §1
5. **Le gain plafond est structurellement plus bas que l'intuition** : mesure statique sur DOOM1.WAD, le meilleur plafond couvre **5 à 23 %** de l'aire, jamais plus que le plancher. §1.3
6. **Le pool TLSF est à 144 octets du plancher** et bouge à chaque build. §5
7. **Un candidat de repli quasi sans risque existe** (plafond dégradé sur NBG2, dans 16 Ko de B0 jamais lus). §4.4
8. **Rien ne doit être committé avant l'étape 0**, qui coûte une capture et zéro ligne de code. §6

---

## 1. Ce que la capture ne prouve pas (correction de la prémisse)

### 1.1 `P` contient l'émission VDP1 des murs — **CODE, vérifié par moi**

`SAT_RP_BSPDONE()` ouvre le bracket à [`r_main.c:1203`](../core/r_main.c#L1203). Entre lui et
`SAT_RP_MASKED()` ([`r_main.c:1234`](../core/r_main.c#L1234)) se trouvent, **dans cet ordre** :

| Ordre | Contenu | Ligne |
|---|---|---|
| 1 | `sat_walls_done_hook()` = `sat_walls_kick` — **toute** la liste de commandes murs VDP1 + arme + present | [r_main.c:1209](../core/r_main.c#L1209) |
| 2 | `NetUpdate()`, `V_Canary()` | :1225-1226 |
| 3 | `R_DrawPlanes()` | [r_main.c:1227](../core/r_main.c#L1227) |

Donc **`P = pr + w + remplissage-plan-maître`**, où :
- `pr` (ligne 1 de l'overlay) chronomètre déjà `sat_walls_kick` — [`dg_saturn.cxx:6757`](../src/dg_saturn.cxx#L6757), [`:6919`](../src/dg_saturn.cxx#L6919), légende [`:1996`](../src/dg_saturn.cxx#L1996) ;
- `w` (ligne 5) = attente de la barrière esclave, incluse dans `R_DrawPlanes`.

**La capture fournie (`Bw1.0 Bp4.5 P7.6 M0.1`) ne permet pas de dimensionner la cible**, parce que
`pr` et `w` n'ont pas été relevés sur la même photo. Le budget réel est **inconnu à ce jour**.

> Note : les deux agents chargés de ce point se sont contredits. J'ai tranché en relisant `r_main.c`
> moi-même. Le bracket inclut bien le hook mur.

### 1.2 Aucune séparation plafond/plancher n'existe — **CODE**

`p10 = (prof_planes_end − prof_bsp_end) × 10/224` ([`r_parallel.c:2063`](../core/r_parallel.c#L2063)).
Le split fin `P a/m/o` a été **retiré** ([`r_parallel.c:2073`](../core/r_parallel.c#L2073)).
Bonne nouvelle : `is_ceil = (pl->height > viewz)` existe déjà ([`r_plane.c:1476`](../core/r_plane.c#L1476)),
le split coûte ~10 lignes.

**Confirmé au passage** : le plancher dominant **n'est pas rasterisé** en 1p M7
([`r_plane.c:1451-1468`](../core/r_plane.c#L1451-L1468) fait `continue`). Mon doute initial était infondé.
`sat_mark_suppress` (OFF en 1p, [`dg_saturn.cxx:7707`](../src/dg_saturn.cxx#L7707)) ne concerne **que** le
fork de visplane dans `R_CheckPlane` — il agit sur **`Bp`**, pas sur `P`.

### 1.3 Le plafond dominant est structurellement plus fragmenté que le plancher — **MESURÉ**

Couverture par triple (flat, hauteur, bande de lumière), pondérée par l'aire des sous-secteurs
(shoelace sur SEGS/SSECTORS de `cd/data/DOOM1.WAD`). `nC50` / `nF50` = nombre de triples nécessaires
pour couvrir 50 % de l'aire.

| Carte | meilleur PLAFOND | meilleur PLANCHER | aire ciel | nC50 | nF50 |
|---|---|---|---|---|---|
| E1M1 | CEIL3_5 — **8 %** | FLOOR7_1 — 10 % | 17 % | 9 | 7 |
| E1M2 | TLITE6_1 — **10 %** | FLOOR7_1 — 17 % | 35 % | 17 | 6 |
| E1M3 | CEIL3_5 — **8 %** | FLOOR7_1 — 13 % | 9 % | 11 | 6 |
| **E1M4** | TLITE6_5 — **9 %** | FLAT14 — 9 % | **1 %** | 11 | 10 |
| E1M5 | FLOOR6_2 — **14 %** | FLOOR5_4 — 14 % | 13 % | 8 | 7 |
| E1M6 | CEIL3_5 — **20 %** | FLAT5_4 — 22 % | 11 % | 6 | 5 |
| E1M7 | MFLR8_1 — **13 %** | FLAT5_4 — 13 % | 19 % | 12 | 6 |
| **E1M8** | NUKAGE3 — **5 %** | FLAT10 — 76 % | **90 %** | 19 | 1 |
| E1M9 | CEIL3_5 — **23 %** | FLOOR5_1 — 20 % | 30 % | 5 | 5 |

`nC50 ≥ nF50` sur **les 9 cartes**. L'hypothèse « le plafond est plus uniforme que le plancher » est
**réfutée sur ce WAD**. Le facteur `f_dominant` est donc plausiblement dans **0,25–0,45**, pas 0,8.
→ **Le mode plafond sera un mode par carte**, ce que vous aviez explicitement autorisé. E1M4 est la
carte de référence (1 % de ciel, plafond concentré) ; E1M8 est hors sujet (90 % de ciel).

---

## 2. Budget VDP2 AVANT — banques et cycles

### 2.1 La loi, sourcée

| Fait | Source |
|---|---|
| VRAM = 512 Ko = 4 banques × 128 Ko (A0 `0x25E00000`, A1 `…20000`, B0 `…40000`, B1 `…60000`) | ST-58-R2 §3.2 ; `RAMCTL` écrit `\|0x0300` = VRAMD+VRBMD ([dg_saturn.cxx:2929](../src/dg_saturn.cxx#L2929)) |
| 8 timings T0–T7 par banque en 320/224 (4 en hi-res) | ST-58-R2 §3.3 p.31 |
| **Une banque désignée `RDBS` pour RBG0 voit son cycle-pattern IGNORÉ** — elle est monopolisée | ST-58-R2 p.149 : *« VRAM cycle pattern register settings of the VRAM bank selected in RAM used for the rotational scroll are ignored »* |
| Un bitmap s'implante sur frontière **`0x20000` = 1 banque**, indépendamment de taille et profondeur | ST-58-R2 p.85-86, p.95 |
| Tailles bitmap légales sur écran de rotation : **512×256 ou 512×512 uniquement** | ST-58-R2 Table 4.13 p.114, `R0BMSZ` 1 bit |
| Coefficient **par ligne** → *« the coefficient table can be stored in any VRAM bank »* ; *« there is no need to set the coefficient table RAM (01B) »* | ST-58-R2 p.148 et p.149 |

### 2.2 État réel (config nominale : 1p, M7, GS_LEVEL, sol RBG0 bitmap + ciel HW + overlay)

| Banque | Contenu | `RDBS` | Cycle-pattern | Timings libres |
|---|---|---|---|---|
| **A0** | table K `slMakeKtable` (**131 072 o écrits**, désassemblage) + `LINECOL`@+0x1000 + `LINEWIN`@+0x1400 | `01` coeff | `0xEEEE` (ignoré) | **0/8** |
| **A1** | bitmap plancher 512×256 8bpp = 131 072 o | `11` bitmap | `0xEEEE` (ignoré) | **0/8** |
| **B0** | framebuffer NBG1 (`DOOM_VRAM`), 224 lignes × 512 = 114 688 o ; **queue 0x1C000–0x20000 = 16 384 o JAMAIS LUS** | `00` | `0x55EE`/`0xEEEE` | **6/8** |
| **B1** | cellules ciel + map ciel + police/map NBG3 + RPT@+0x1FF00 | `00` | autorisé par `slScrAutoDisp` | **3/8** |

**Total : 0 banque libre, 9 timings sur 32.**
Octets inoccupés = 220 608, mais **seuls ~95 000 sont exploitables par une couche** (queue de B0 +
trous de B1) : les ~125 Ko « libres » de A0 sont dans une banque verrouillée `RDBS`.

**Priorités live — CORRECTION** (`VDP2_SKY_OCCL_DIAG = 1`, [dg_saturn.cxx:380](../src/dg_saturn.cxx#L380),
commentaire : *« This is the shipping config »*) :

> **NBG1 framebuffer (6) > VDP1 (5, et 7 pour things/arme) > ciel NBG0 (4) > plancher RBG0 (3)**

[`:2946`](../src/dg_saturn.cxx#L2946) `slPriorityRbg0(DIAG?3:4)` et [`:3595`](../src/dg_saturn.cxx#L3595)
`slPriorityNbg0(DIAG?4:3)`. La mémoire `part5-hw-sky-split` (« RBG0 6 > NBG1 5 > NBG0 4 ») décrit une
config de diagnostic périmée et **doit être corrigée**. J'avais moi-même annoncé l'ordre inverse en
cours d'étude : c'est faux.

**Conséquence dure** : les cellules de ciel sont rendues **opaques au-dessus de l'horizon**
([`:3821-3833`](../src/dg_saturn.cxx#L3821-L3833)) précisément pour masquer le débordement du plancher.
C'est **exactement la zone où un plafond doit apparaître** → un plafond porté par RBG0 serait
**invisible à 100 %** tant que `DIAG=1`. Voir §4.2, où ce problème se retourne en argument.

---

## 3. Les impasses re-jugées

| Impasse | Verdict | Pourquoi |
|---|---|---|
| **RBG1 comme 2e plan** | **TIENT — absolue** | §3.1 |
| `rbg0-rpb-cell-mode-only` | **TOMBE** | §3.2 |
| `vdp2-floor-snows-on-hardware` | **TOMBE** | §3.3 |
| `sgl-rotation-anchors-bank-a1` | **PARTIEL** | §3.4 |
| `vdp2-window-in-blockflush` | **PARTIEL — à corriger** | §3.5 |
| `big-wad-perf-compute-bound` (« P = plafonds ») | **PARTIEL — la moitié est fausse** | §1.1 |
| `hw-render-path-comparison` | **TIENT** | ciel HW = gain net → tout candidat qui tue le ciel paie d'entrée |
| Réduire NBG1 (4bpp / 256×256) | **TIENT — impasse** | §3.6 |

### 3.1 RBG1 — **MANUEL, quatre citations**

> p.7 : « *If two rotation scroll screens are displayed, **the normal scroll screen cannot be displayed*** (the register that sets RBG1 is used for NBG0) »
> p.148 : « When RBG1 is displayed, RBG0 must also be displayed […] **The Normal scroll screens can no longer be displayed at that time.** »
> p.148 : « the RBG1 pattern name table is stored in **VRAM-B1**, and character pattern table is stored in **VRAM-B0**. »
> p.150 : « When displaying RBG1, 00B must be set in bits used for VRAM-B0 and VRAM-B1. »
> p.162 : « **Mode 0 must be set when displaying RBG1.** » → incompatible avec le bi-paramètre.
> Table 1.4 p.6 : RBG1 « Bitmap Display : **Display Not Allowed** », « Bitmap Size : **None** ».

Prix pour Mimas : le framebuffer NBG1 (HUD, sprites masqués, plafonds, tous les fallbacks) disparaît,
le ciel disparaît, l'overlay disparaît, 16 timings confisqués, et RBG1 ne peut même pas être en bitmap.
**Art antérieur** : zéro occurrence de `RBG1` dans tout SGL 2.1 (INC + DOC + SAMPLE), aucune API RBG1
dans SRL ni Jo Engine, aucun jeu commercial identifié. SGL n'expose que `scnRBGA`/`scnRBGB`, qui sont
les deux **paramètres** de RBG0, pas deux écrans.

### 3.2 `rbg0-rpb-cell-mode-only` — **TOMBE**, et on a la cause racine

**Le matériel prévoit deux bitmaps** (MANUEL, p.85-86) :

> `RAMP8~RAMP6  18003EH  Bit 2~0  For Rotation Parameter A`
> `RBMP8~RBMP6  18003EH  Bit 6~4  For Rotation Parameter B`
> « (boundary address value of the bit map pattern) = (map offset register value 3 bit) × **20000H** »

La checklist « Bitmap Format (RBG0) » p.283 liste **les deux**, et le screen-over est **par paramètre**
(`RAOVR` bits 11~10 / `RBOVR` bits 15~14, p.114 : *« The setting of the screen-over process is not
performed for RBG0 and RBG1, but is performed for the scroll screen by rotation parameter A and the
scroll screen by rotation parameter B »*), avec un mode « tout l'extérieur transparent ».

**Deux bugs cumulés expliquent le « RPB noir/bavure » consigné dans `RBG0_DUAL_PARAM_FINDINGS.md`** :

- **BUG 1 — `slBitMapRbg0` écrase `RBMP`.** Désassemblage de `build/Mimas.elf` :
  `mov r6,r0 ; mov.l r0,@(524,gbr) ; shlr16 r0 ; shlr r0 ; rts ; mov.w r0,@(254,gbr)` →
  `MPOFR` (shadow GBR+0xFE = chip `0x18003E`) écrit en **mot entier** `= adr>>17`, **sans masque**.
  Pour `RBG0_BMP_VRAM = 0x25E20000` : `MPOFR = 0x12F1` → `RAMP = 1` (A1, correct) mais
  `RBMP = 7`. Par contraste, `slBitMapNbg0/1` font un **read-modify-write masqué** du même champ.
  Mimas ne touche jamais `MPOFR` (grep : 0 occurrence).
- **BUG 2 — la copie RPT écrase le début de RPB.** [`dg_saturn.cxx:2506`](../src/dg_saturn.cxx#L2506) :
  `memcpy(rpt + 0x68, src, 0x30)` → écrit `0x68..0x97`. Or le matériel place RPB à **+0x80**
  (ST-58-R2 p.159 : *« RPTA6 bit is ignored […] The bit is set at 0 for rotation parameter A, and
  fixed at 1 for rotation parameter B »*). Les **24 premiers octets de RPB** (XST/YST/ZST/DXST/DYST/DX)
  sont donc déjà corrompus. **Corriger `MPOFR` seul ne suffira pas.**

**Conclusion** : ce n'est pas le VDP2 qui interdit le bitmap bi-paramètre, c'est l'API SGL.
**Réserve honnête** : aucun précédent shippé en bitmap bi-paramètre n'existe (SEGA `S_8_9_2`,
Panzer Dragoon Saga — 29 sites `RPMD=2` —, Jo Engine : **tous en cell**), et le manuel n'affirme jamais
positivement que **deux banques peuvent être simultanément en `RDBS=11`**. L'impasse tombe au niveau du
**diagnostic** ; le test HW reste obligatoire.

### 3.3 `vdp2-floor-snows-on-hardware` — **TOMBE**

Le code documente lui-même la cause et le correctif validés HW
([`dg_saturn.cxx:2884-2887`](../src/dg_saturn.cxx#L2884-L2887)) : *« The old cell path SNOWED because its
commit never parked B1 as a rotation bank »*. Ce n'était pas une famine de bande passante mais un
`RDBS`/park incomplet — cohérent avec p.149. Le défaut **résiduel** du chemin cell n'est plus la neige
mais les **artefacts triangulaires**.

**Diagnostic mécanique proposé (INFÉRÉ, testable en 3 lignes)** : en mode cell, le RPT est déplacé en
`A0+0x1FF00` ([`:2893`](../src/dg_saturn.cxx#L2893)) **puis** `slMakeKtable(0x25E00000)` est appelé
([`:2916`](../src/dg_saturn.cxx#L2916)) et écrit **exactement 131 072 octets** (désassemblage :
boucle 1 = 0x4002 entrées, boucle 2 = 0x3FFE, `(0x4002+0x3FFE)×4 = 0x20000`) → il **écrase le RPT**.
Or `rbg0_rpt_to_vram` ne recopie que `0x54` octets, *« stopping BEFORE KAST »* : `KAst`/`dKAst`/`dKAx`
(RPT +0x54/+0x58/+0x5C) ne sont **jamais** réécrits. Un `dKAx` non nul fait avancer le coefficient
**par dot** le long de la scanline → la frontière de transparence devient une **droite oblique** =
un triangle. **Test : étendre la copie RA de `0x54` à `0x60`.**

### 3.4 `sgl-rotation-anchors-bank-a1` — **PARTIEL**

Vrai comme défaut SGL (`sl_def.h` : `KTBL0_RAM = A1`, `RBG_PARA_ADR = A1+0x1ff00`) mais **inopérant** :
SRL relocalise le RPT en B1+0x1ff00, Mimas pose K en A0 et le bitmap en A1. Le vrai piège d'ancrage est
celui de §3.3. **Corollaire à retenir** : `LINECOL_TBL_VRAM` (A0+0x1000) et `LINEWIN_TBL_VRAM`
(A0+0x1400) sont **physiquement à l'intérieur** de la table K — ça ne marche que parce que la fenêtre
réellement *lue* reste sous 0x1000. Ils devront déménager si A0 change d'usage.

### 3.5 `vdp2-window-in-blockflush` — **PARTIEL, à corriger**

Le block-flush est `for (off = 0x0E; off <= 0xFE; off += 2)` ([`dg_saturn.cxx:3010-3011`](../src/dg_saturn.cxx#L3010-L3011)),
en offsets **puce**. Donc :
- **DANS** le flush : `MPOFR` (0x3E), `PLSZ` (0x3A), **`RPMD` (0xB0)**, `KTCTL` (0xB4), `KTAOF` (0xB6),
  `RPTA` (0xBC/0xBE), `WPSX0..WPEY1` (0xC0-0xCE), `WCTLC` (0xD4), **`WCTLD` (0xD6)**.
- **HORS** flush : `CCRR` (0x10C).

→ L'affirmation « **RPMD 0x110** n'est pas dans le block-flush » **confond un décalage de tableau shadow
avec l'offset puce**. `RPMD` **est** flushé. La mémoire doit être corrigée.

### 3.6 Réduire NBG1 — **impasse, avec un acquis réel**

- Pas de taille bitmap sous 512×256 (`sl_def.h:1346-1349` et ST-58-R2 Table 4.9).
- Alignement toujours `0x20000` : même en 4bpp (64 Ko), un bitmap **occupe un slot de banque entier**.
- 4bpp incompatible avec la PLAYPAL 256 couleurs du framebuffer.
- Le zoom M7 est un **agrandissement** ×2, pas une réduction → **aucun accès VRAM supplémentaire**, et
  il ne désactive **pas** NBG2 (Table 5.2 ne s'applique qu'en réduction). **Bonne nouvelle pour §4.4.**
- **ACQUIS EXPLOITABLE** : B0 déclare `BM_512x256` mais NBG1 n'affiche que 224 lignes depuis (0,0) →
  **`0x25E5C000..0x25E5FFFF` = 16 384 octets contigus jamais lus**. C'est le terrain de §4.4.

---

## 4. Les candidats

### 4.1 C1 — libérer la désignation `RDBS` de A0 (sonde de connaissance, 4 octets)

**Mécanisme.** Mimas lit sa table de coefficients **par ligne** :
`slKtableRA(KTAB, K_FIX|K_LINE|K_2WORD|K_ON)` ([`:2873`](../src/dg_saturn.cxx#L2873)) — `K_DOT` (0x20)
absent. Or [`:2956`](../src/dg_saturn.cxx#L2956) écrit `rdbs = 0x000D`, soit **A0 = `01` = banque
coefficient dédiée**. Le manuel ne l'exige que pour le per-dot (p.148 **et** p.149, deux phrases
indépendantes). Changement : `0x000D → 0x000C`.

**Preuve décisive tirée du binaire.** `_slKtableRA` fait
`mov #32,r0 ; tst r0,r5 ; bt <rts> ; jmp rbank_set ; mov #1,r5` → **SGL n'appelle `rbank_set` (déclarer
la banque `RDBS=01`) que si `mode & K_DOT`**. Avec `K_LINE`, **SGL lui-même ne déclarerait pas A0**.
Le `0x000D` est une **sur-déclaration du porteur**, pas une exigence. Cela tue au passage le
contre-indice « `SCROLL.TXT` appaire `slMakeKtable` à `K_DOT` » : le code de SGL branche explicitement.

| | AVANT | APRÈS |
|---|---|---|
| VRAM | inchangé | inchangé |
| Cycles | A0 0/8 | A0 **8/8** *(scénario NBG)* ou 0/8 *(scénario C2)* — **XOR, pas AND** |
| Pool | — | **~4 octets** |
| Sacrifice | — | aucun si le manuel dit vrai |
| Faisabilité | | **TRÈS PROBABLE** |

**Piège corrigé** : « 128 Ko **et** 8 timings » est faux. Si A0 finit en `RDBS=11` pour porter le
2e bitmap (le but de C2), ses créneaux redeviennent ignorés. C'est **128 Ko OU 8 créneaux**.

**A/B.** Accord pad commutant `0x000D`/`0x000C` en branche bitmap, suivi de `rbg0_commit_ramctl` +
`rbg0_commit_cyc`. **Le `printf` RAMCTL ne sort que sur le port debug Ymir**
([`syscalls.c:17-18`](../src/syscalls.c#L17-L18)) → **inutilisable sur HW**. Il faut **un champ overlay
relisant `*(0x25F8000E)`**, plié dans un champ déjà possédé (règle `debug-overlay-placement`).
Critère : sol identique → A0 libérable ; sol **plat** (perte de perspective, symptôme plus probable que
la neige), noir ou neigeux → l'impasse tient et C2 meurt.

### 4.2 C2′ — plafond sur RPB, RBG0 bitmap bi-paramètre — **candidat phare, sous conditions**

**Trois faits vérifiés dans le binaire.**
1. Mimas est **déjà** en `RPMD=2` : `slRparaMode(K_CHANGE)` [`:2878`](../src/dg_saturn.cxx#L2878).
2. `_slMakeKtable` écrit `0x4002` entrées à **`0xFF000000`** puis `0x3FFE` entrées calculées par le DIVU
   SH-2 (rampe réciproque). MSB = 1 sur la première moitié.
3. MANUEL p.166 : *« when rotation parameter mode 2 is selected […] the MSB of data read from the
   coefficient table used for rotation parameter A **is used for switching rotation parameters**.
   When the MSB is 0 […] rotation parameter A. When the MSB is 1 […] rotation parameter B. Here, the
   MSB of coefficient data read from the coefficient table used for rotation parameter B is used as a
   **transparent bit**. »*

→ **Les lignes au-dessus de l'horizon sélectionnent déjà RPB, aujourd'hui, sur le matériel.**
Le plafond HW n'est pas une architecture à inventer : c'est **un chemin déjà armé qui pointe sur une
banque invalide**.

**Le point que j'ai dû trancher moi-même.** Les deux réfuteurs se contredisent sur la granularité de
l'arbitrage `RPMD=2` : l'un affirme qu'il est *par point* (donc coefficient per-dot, donc banque `RDBS=01`
obligatoire, donc C1 et C2 s'annulent). **C'est faux** : la granularité de l'arbitrage **suit celle de la
lecture du coefficient**. Le manuel p.162 dit *« coefficient data cannot be read to each dot from the
coefficient table for rotation parameter B **while** coefficient data for rotation parameter A **is being
read to each dot** »* — formulation qui n'a de sens que si la lecture par ligne est un cas légitime.
En `K_LINE`, la commutation est **par ligne** = exactement le split par horizon recherché.
**Statut : INFÉRÉ FORT (3 phrases cohérentes), à valider sur HW.** C'est **la** question que le test
de l'étape 3 doit trancher.

**Arithmétique de banques.** Deux bitmaps 512×256 8bpp = `0x20000` chacun, frontière = 1 banque :
A0 (plafond, `RDBS=11`) + A1 (sol, `RDBS=11`), B0 = framebuffer (`RDBS=00`).
**Il ne reste B1 pour la table K que si C1 est vrai.** Si C1 est faux, il faut `RDBS(B1)=01` → **B1 est
monopolisée → ciel HW ET overlay debug meurent**, et l'instrument disparaît avec l'expérience.
Échappatoire de secours : **CRKTE=1** (K en CRAM `0x100800–0x100FFF` = 2048 o ; besoin 2×224×4 = 1792 o),
mais impose CRAM mode 1 et interdit toute banque `01` (p.167) — à ne considérer que si C1 tombe.

| | AVANT | APRÈS (si C1 vrai) |
|---|---|---|
| A0 | table K, `RDBS 01` | **bitmap plafond 131 072 o, `RDBS 11`** |
| A1 | bitmap sol, `RDBS 11` | inchangé |
| B0 | framebuffer + 16 Ko de queue | inchangé |
| B1 | ciel + overlay + RPT | **+ K_RA 896 o + K_RB 896 o + LINECOL 512 + LINEWIN 1024 ≈ +3 328 o** |
| `RDBS` | `0x000D` | **`0x000F`** |
| Cycles | 9/32 | **9/32 — inchangé** (A0 était déjà perdue pour les NBG) |
| Pool | — | **500–900 o de `.text` côté plateforme + le miroir côté core (> 1 Ko)** ⚠️ |

**Recette.**
1. C1 d'abord (sinon l'arithmétique de banques ne ferme pas).
2. Bitmap plafond 512×256 8bpp en A0 ; `rdbs 0x000D → 0x000F`.
3. **Poker le shadow SGL à GBR+0xFE** (chip `0x25F8003E`) avec **`MPOFR = 0x0001`**
   (`RAMP=1` → A1 sol, `RBMP=0` → A0 plafond), **après chaque `slBitMapRbg0`**

   > **⚠️ CORRECTION 2026-08-02 — cette recette portait `0x0011`, qui est FAUX.** Décodage
   > ST-58-R2 p.85 : `RAMP` = bits 2~0, `RBMP` = bits **6~4**. Donc `0x0011` → `RAMP=1` **et**
   > `RBMP=1` = **les deux paramètres sur A1**, pas deux bitmaps distincts.
   > La valeur pour deux bitmaps distincts est **`0x0001`**.
   > `0x0011` n'est pas pour autant absurde : c'est le cas **RAMP == RBMP** (deux vues sur le même
   > bitmap), qui n'a jamais été examiné et fait l'objet d'une étude complémentaire.

   ([`:2880`](../src/dg_saturn.cxx#L2880), rappelé à chaque `rbg0_reinit`). L'offset 0x3E est dans le
   block-flush → il partira à la puce.
4. **Corriger `rbg0_rpt_to_vram`** : la copie RB doit aller à **`rpt+0x80`**, pas `+0x68`
   ([`:2506`](../src/dg_saturn.cxx#L2506)) — sinon les 24 premiers octets de RPB restent corrompus.
5. Transform plafond : **ne pas inverser le signe à la main** — [`:2528`](../src/dg_saturn.cxx#L2528)
   calcule déjà `-(viewz - h)`, une hauteur de plafond (> `viewz`) inverse le signe toute seule.
   Ce qui doit changer, c'est **la direction de la rampe de coefficients** (ancrage `KAst` à l'horizon,
   `dKAst` négatif pour B). ⚠️ Le `dy = abs()` de `r_main.c:752-757` concerne le `yslope` du renderer
   **logiciel** et ne prouve **rien** sur l'indexation de la table K du VDP2 : « le miroir est exact »
   est **INFÉRÉ**, pas établi.
6. **Priorités et fenêtre** — le point que le candidat initial avait manqué :
   - `VDP2_SKY_OCCL_DIAG` doit repasser à **0** (RBG0 4 > ciel 3), sinon **le plafond est invisible à
     100 %** (§2.2). Conséquence assumée : le ciel HW n'est plus utilisable → **ciel logiciel**
     (opaque dans NBG1 prio 6, il masquera correctement le plafond) → **mode par carte, intérieurs**.
     *L'ironie utile* : `DIAG=1` n'existait que pour masquer le **débordement** du plancher au-dessus de
     l'horizon. Avec RPB, cette zone n'est plus du débordement mais du **contenu légitime** — C2′ supprime
     le problème que DIAG contournait.
   - `RBG0_FLOOR_WINDOW=1` ([`:423`](../src/dg_saturn.cxx#L423)) pose W1 = `[0,hz]..[xend,223]` avec
     `slScrWindowModeRbg0(win1_IN)` ([`:3071-3080`](../src/dg_saturn.cxx#L3071-L3080)). **La fenêtre est
     par COUCHE, pas par paramètre** : elle coupe RPB comme RPA. Il faut la désarmer/reconfigurer.
7. Côté core : `sat_vdp2_ceil_h/_pic/_band/_dominant` en miroir de
   [`r_plane.c:1261-1345`](../core/r_plane.c#L1261-L1345), en levant le filtre
   [`r_plane.c:1311-1312`](../core/r_plane.c#L1311-L1312) (`if (p->height >= viewz) continue;`), plus un
   prédicat de skip miroir et un `rbg0_upload_flat_ceil`.

**Risques non résolus.**
- **Deux banques simultanément en `RDBS=11` : NON DOCUMENTÉ.** Si le matériel n'accepte qu'une banque
  bitmap, C2′ meurt.
- `CHCTLB` est **unique** pour RBG0 : les deux surfaces partagent obligatoirement profondeur et taille
  (acceptable pour deux flats 8bpp) et **on ne peut pas faire « A en bitmap, B en cell »**.
- `rbg0_upload_flat` reconstruit **131 072 o** à chaque changement de flat dominant
  ([`:1805`](../src/dg_saturn.cxx#L1805)). Les plafonds étant **plus fragmentés** (§1.3), le flat plafond
  dominant changera **plus souvent** → **mesurer le taux de re-upload avant de shipper**, il peut manger
  tout le `delta_P`.
- `RBMP=7` → « hors VRAM » est une **INFÉRENCE** : le décodage sur 19 bits **aliase probablement** sur
  `0x60000` = B1 (le ciel). Non vérifié dans les deux sens.

### 4.3 C4 — variante `RPMD=3` (fenêtre de paramètre de rotation), en réserve

Même stockage, mais l'arbitrage A/B passe par la **fenêtre de paramètre de rotation** (p.160 :
*« Images obtained by rotation parameter B are displayed in the active area of the designated window »*).
Registres `RPMD` 0xB0, `WCTLD` 0xD6, `WPSX0..WPEY0` 0xC0-0xC6 — **tous dans le block-flush**, pilotables
sans `slSynch` sur le patron existant de `rbg0_floor_window_apply`. Coût runtime : 2 écritures/frame.

**Deux avantages manuel-sourcés** : (a) p.164 — en mode 3, **line-color screen indépendant par
paramètre** → fog de distance séparé sol/plafond, ce qui permettrait de puncher **plus** de triples ;
(b) p.162 — en mode 3, A **et** B peuvent lire leur coefficient par dot.

**Coût spécifique non vu ailleurs** : **les deux fenêtres VDP2 sont déjà prises.** W0 porte la table
per-ligne du color-calc ([`:2852-2853`](../src/dg_saturn.cxx#L2852-L2853)), W1 le clip RBG0
([`:3064-3065`](../src/dg_saturn.cxx#L3064-L3065)). `RPMD=3` en exige une → il faut abandonner le fog
per-ligne **ou** le clip RBG0. Atténuation : avec RPB armé, W1 devient partiellement redondant (§4.2.6),
donc **libérer W1 est cohérent**.

### 4.4 C5 — plafond dégradé sur **NBG2**, dans la queue inutilisée de B0 — **le repli sûr**

**NBG2 n'est utilisé nulle part** dans `dg_saturn.cxx` (0 occurrence). Idée : renoncer à la **texture**
du plafond et n'émuler que son **éclairage de distance**. Pour un plafond à hauteur élue, la ligne écran
`y` détermine entièrement la distance → un plan dont chaque rangée porte une couleur constante reproduit
exactement le dégradé. NBG2 est **cellulaire obligatoire** (Table 1.4).

| | AVANT | APRÈS |
|---|---|---|
| VRAM | B0 : 114 688 lus + 16 384 de queue | B0 : + map 8 192 + cellules 1 024 = **+9 216 o**, queue restante 7 168 |
| A0/A1/B1 | | **strictement inchangés** — le trou de B1 reste intact pour C1/C2′ |
| Cycles | B0 6/8 libres, total 9/32 | B0 4/8, total **7/32** (`CYCB0L 0x55EE → 0x5526`) |
| Pool | | **~400–700 o** ⚠️ |
| Sacrifice | | la **texture** du plafond dominant (garde l'éclairage) |

Contrainte pattern-name respectée (p.33 : au plus 2 banques PN, une dans {A0,B0}, une dans {A1,B1} ;
les deux PN actuels sont en B1 → le créneau {A0,B0} est libre). Placement `T2`/`T3` conforme à Table 3.4.

**Avantage chiffrable sur C2′** : le critère de match n'est plus le triple complet mais **(hauteur, bande)**
— tous les plafonds de même hauteur et même bande peuvent être punchés quel que soit leur flat, donc
`f_dominant` **plus élevé**. Et **NBG2 peut être placé SOUS le ciel** (`slPriorityNbg2(2)`), ce que RBG0
ne permet pas (un seul registre `R0PRIN` pour ses deux surfaces) → **pas de conflit avec le ciel HW**.

**Risque unique et immédiatement visible** : ajouter une lecture PN dans la banque du framebuffer.
Neige ou pas neige, ça se voit à l'œil sur la première photo HW.

### 4.5 C6 — faux mode 7 par line-scroll sur NBG0 — **RÉFUTÉ en texture**

La table de line scroll ne coûte **aucun timing** (elle n'est pas dans la liste des 10 types d'accès,
p.31) et NBG0 a déjà ses 3 créneaux → coût marginal nul. **Mais** la table fournit par ligne `u0`, `v0`
et **`du/dx` seulement** : `dv/dx` est structurellement nul. Or pour un plan horizontal vu avec un lacet
quelconque, `v` varie le long de la ligne → la texture cisaille dès que la caméra n'est pas alignée sur
un axe. Le seul sous-ensemble valide (dégradé sans détail horizontal) **est exactement C5**, en
sacrifiant le ciel en plus. **Écarté.**

---

## 5. Le pool TLSF — le vrai plafond du plan

**Le pool n'est pas une constante : c'est une fonction du build.** `__heap_start = _end` et
`__heap_end = 0x060fa000` est **fixe** → **chaque octet de `.text`/`.data`/`.bss` rogne le pool 1:1.**

Relevés successifs de `build/Mimas.map` **au cours de cette seule étude** :

| Moment | `_end` | Pool | Marge / 4096 |
|---|---|---|---|
| Début d'étude (31/07 11:40) | `0x060f8c60` | 5 024 o | 928 o |
| Mi-étude (31/07 15:41) | `0x060f8ed0` | 4 400 o | 304 o |
| **Build actuel (01/08 10:51)** | **`0x060f8f70`** | **4 240 o** | **144 o** |

> ⚠️ **Le chiffre courant est pollué par du travail non commité.** `src/dg_saturn.cxx` porte
> **+181 lignes non commitées** : une sonde « LAG / décrochage rotation M7 » (`lag_vbl_blit/rpt/v1`,
> `lag_v1_site`, `sat_vsync_fence`, ligne d'overlay 13) — **sans rapport avec cette étude**, apparue
> pendant la session, et incluse dans le build du 01/08.
> **Le budget de référence dépend donc de ce que devient cette sonde** :
> - sonde conservée → **4 240 o, marge 144 o** — seules les étapes 0 à 3 sont finançables ;
> - sonde retirée/commitée à part → retour vers **~5 024 o, marge ~928 o**.
>
> À trancher **avant** de budgéter §6.4. `build/Mimas.elf` et `build/Mimas.map` ont aussi été
> régénérés pendant l'étude.

**Plancher dur réel** : `tlsf_create_with_pool` consomme `sizeof(control_t)` ≈ 3 188 o + 8 d'overhead
→ en dessous de ~3 208 o, `tlsf_add_pool` rend 0, tout `operator new` rend NULL, **boot-loop silencieux**.
Marge avant panne réelle ≈ **1 032 o** ; marge avant le seuil de sécurité 4096 = **144 o**.

**Devis contre budget :**

| Étape | Coût `.text` estimé | Finançable sur 144 o ? |
|---|---|---|
| §6.0 lecture d'overlay | **0** | ✅ |
| §6.1 champ `c%` plafond | ~60–120 o | ⚠️ limite |
| §6.2 sonde C1 | ~4 o | ✅ |
| §6.3 poke `MPOFR` + fix RPT | ~40–80 o | ⚠️ |
| §6.4 C2′ complet (plateforme + miroir core) | **> 1,5 Ko** | ❌ **non finançable** |
| C5 complet | ~400–700 o | ❌ |

**Deux soupapes existent, mesurables, jamais tirées** :
1. `SRL_MALLOC_METHOD = TLSF` → autre chose ([`Makefile:14`](../Makefile#L14)) : rend les ~3 192 o de
   `control_t`, qui sont du **pur overhead** — Mimas n'alloue jamais dans ce pool (tout passe par
   `Z_Malloc`/LWRAM).
2. `HEAP_SIZE` 16 Ko → 8 Ko ([`syscalls.c:55`](../src/syscalls.c#L55)) : rend 8 192 o de `.bss`.
   **Critère mesurable déjà en place** : le commentaire du fichier dit *« Watch row-22 `hp`
   (`dg_heap_peak`) stays < HEAP_SIZE on a full E1 run »*. **Lire `hp` ligne 22** décide si la soupape est
   tirable sans risque.

> **Règle non négociable** : tirer la soupape **avant** d'écrire une ligne de §6.4, pas après le premier
> boot-loop. Et relire `build/Mimas.map` (`_end..__heap_end ≥ 4096`) après **chaque** build.

---

## 6. Ordre d'implémentation, avec A/B mesurable et coût pool

### Étape 0 — **Dimensionner la cible. Zéro ligne de code, zéro octet de pool.**
Une capture montrant **simultanément** la ligne 1 (`R T S b dg pr`), la ligne 2 (`Bw Bp P M`) et la
ligne 5 (`SLV b% id% Pb% w`). Calculer **`P − pr − w`**.
- **Critère d'abandon : si `P − pr − w < 2,5 ms`, toute la piste « 2e surface VDP2 » est abandonnée** au
  profit de `sat_walls_kick`, qui serait alors le vrai terme dominant.
- Dans la **même** session, trois A/B gratuits :
  - **pad L+Y** ([`:8125`](../src/dg_saturn.cxx#L8125)) cycle la SQ **plafond seule** → `ΔP > 0` prouve que
    les plafonds sont un terme réel de `P` ; `ΔP ≈ 0` **réfute la prémisse du brief**.
  - **pad Y modes 1/2** (sol software, RBG0 OFF) → `ΔP` donne la **valeur empirique du sol HW
    aujourd'hui** = la meilleure borne disponible pour ce que vaudrait un plafond HW.
  - **pad L+B** (`sat_mark_suppress`) → attendu : `Bp` et `vp` baissent, `P` inchangé.
- Piège : `p3_wait_ticks` n'est pas remis à zéro quand `RP_WaitPlanes` n'est pas appelé → `w` peut être
  périmé d'une frame.

### Étape 1 — Instrumenter la part plafond de `P`. **~60–120 o.**
Compteur `prof_plane_pix_ceil` après [`r_parallel.c:1518`](../core/r_parallel.c#L1518), en réutilisant
`is_ceil` ([`r_plane.c:1476`](../core/r_plane.c#L1476)). Champ **`c%`** à replier **sur la ligne 2**,
à côté de `P` — **pas** sur la ligne 17 : celle-ci est du **code mort**
(`(void)r17`, [`:2289`](../src/dg_saturn.cxx#L2289)) et de toute façon couverte par l'arme VDP1 en 1p.
Corollaire d'honnêteté : **déplacer `SAT_RP_BSPDONE()` après le hook mur** ([`r_main.c:1209`](../core/r_main.c#L1209))
— 1 ligne, 0 perf, et `P` mesure enfin ce que son nom dit.

### Étape 2 — Sonde C1 (`RDBS` A0 `01 → 00`). **~4 o + le champ overlay RAMCTL.**
Accord pad. **Ajouter un champ overlay relisant `*(0x25F8000E)`** — sans lui, aucune photo HW n'est
décodable et le `printf` Ymir ne prouve rien (le projet documente lui-même que la famine de cycle VDP2
est **invisible dans Ymir**, [`:363`](../src/dg_saturn.cxx#L363)).
**Critère** : sol identique → A0 libérable, C2′ vit. Sol **plat**/noir/neigeux → l'impasse tient,
**C2′ meurt** et on bascule sur C5.

### Étape 3 — Faire apparaître **quoi que ce soit** sur RPB. **~40–80 o.**
Poke **`MPOFR = 0x0001`** après `slBitMapRbg0` (⚠️ *pas* `0x0011`, qui met RAMP **et** RBMP sur A1 —
voir l'erratum §4.2) **+** corriger la copie RPT de RPB (`+0x68 → +0x80`, et **écrire** la queue
`KAst/ΔKAst/ΔKAx` avec `ΔKAx = 0` plutôt que l'étendre — voir ZONES §3.5).
Élargir temporairement W1 à plein écran. **Attendu : le haut de l'écran se remplit du flat du sol.**
- Si oui → **le bi-paramètre bitmap est prouvé sur HW** (premier au monde à ma connaissance) → étape 4.
- Si noir/bavure → basculer sur le **cell bi-paramètre** (recette officielle SEGA `S_8_9_2` :
  `slPlaneRA`+`sl1MapRA` / `slPlaneRB`+`sl1MapRB`, table K partagée), en sachant qu'il **tue le ciel HW
  et l'overlay** (B1 devient banque pattern-name) → **parade vérifiée** : reloger police + map NBG3
  (3 072 + 8 192 = 11 264 o) dans les **16 384 o de la queue de B0**. Faire ce déménagement **avant**,
  sinon on perd l'instrument. Prérequis : le correctif RPT de §3.3 (triangles).

### Étape 4 — Le plafond. **> 1,5 Ko — soupape pool obligatoire d'abord.**
Élection plafond côté core, `rbg0_upload_flat_ceil`, `DIAG → 0`, reconfiguration W1, ciel logiciel,
**mode par carte** (E1M4 en carte de référence).
**A/B** : accord pad armant/désarmant le punch plafond ; lire `P` et `c%` (ligne 2).
**Attendu ON** : `c%` chute vers 0 **et** `P` baisse du même ordre. Si `c%` chute mais que `P` ne bouge
pas, le gain est mangé par les re-uploads de 131 Ko → mesurer `up` ([`:1805`](../src/dg_saturn.cxx#L1805)).

### En parallèle, en assurance — C5 (NBG2)
Indépendant de tout ce qui précède, ne touche ni A0, ni A1, ni B1. Peut être développé et mesuré
pendant que C2′ est en test HW. Test de non-régression obligatoire dans la même session : **le
framebuffer NBG1 prend-il de la neige après `CYCB0L 0x55EE → 0x5526` ?**

---

## 7. Ce qu'on sacrifie, et dans quel mode

| Mode | Cartes | Ciel HW | Overlay | Fog per-ligne | Texture plafond |
|---|---|---|---|---|---|
| **Shippé** | toutes | ✅ | ✅ | ✅ | — (software) |
| **C5** (NBG2) | toutes | ✅ | ✅ | ✅ | ❌ dégradé seul |
| **C2′** (bitmap RPB) | **intérieurs** (E1M4 1 %, E1M3 9 %, E1M6 11 %) | ❌ software | ✅ | ✅ | ✅ |
| **C3** (cell RPB) | **intérieurs** | ❌ | ⚠️ si relogé en B0 | ✅ | ✅ |
| **C4** (`RPMD=3`) | intérieurs | ❌ | ✅ | ❌ (W0 ou W1 sacrifiée) | ✅ indépendant/surface |
| À proscrire | E1M2 (35 % ciel), **E1M8 (90 %)** | | | | |

---

## 8. Inconnues assumées

1. **`P − pr − w` est inconnu** → la borne supérieure de tout candidat est inconnue. **Étape 0.**
2. **Deux banques simultanément en `RDBS=11`** : non documenté. Tue C2′ si faux.
3. **Granularité de l'arbitrage `RPMD=2`** en `K_LINE` : INFÉRÉ FORT, non écrit noir sur blanc.
4. **`dKAst` vivant n'a jamais été lu** (la copie RPT s'arrête avant) → le chiffre « 896 o » pour une
   table K par ligne est un **espoir**, pas une mesure. Dumper `RPT+0x54`/`+0x58` avant de budgéter.
5. **La table de cycles vivante** reste l'inconnue n°1 du sous-système : contradiction interne non
   tranchée sur ce que repousse l'ISR vblank. **Observable pour une ligne** : `cyc_before[4]` est déjà
   capturé ([`:3003-3006`](../src/dg_saturn.cxx#L3003-L3006)) et affiché nulle part.
6. **Le miroir géométrique plafond/plancher côté table K** est inféré, pas établi.
