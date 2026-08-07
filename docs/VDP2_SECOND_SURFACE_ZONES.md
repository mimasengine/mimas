# VDP2 — deuxième surface : les 6 zones non couvertes

> Complément de [`VDP2_SECOND_SURFACE_PLAN.md`](VDP2_SECOND_SURFACE_PLAN.md), rendu le 2026-08-02.
> Branche `flicker-clean`. Mêmes conventions : **MESURÉ** / **CODE** / **MANUEL** / **INFÉRÉ**.
> Manuel = ST-58-R2 (`/d/DOCUMENT/SATURN/PROGRAM2.PDF`, extraction `pdftotext -raw` — le mode
> `-layout` entrelace le texte sur ce PDF et le rend illisible). Désassemblage = `build/Mimas.elf`
> via `SaturnRingLib/Compiler/sh2eb-elf/bin/sh2eb-elf-objdump.exe`.
>
> **Consigne appliquée** : aucune conclusion de l'étude précédente n'a été reprise comme acquise.
> Là où elle servait d'appui, elle a été ré-établie — et **elle tombe plus souvent qu'elle ne tient**.

---

## 0. Le verdict est retourné

**La piste « deuxième surface VDP2 pour le PLAFOND en 1p » est abandonnée — non par refus du
matériel, mais par arithmétique.**

1. **Le seuil de 2,5 ms était faux d'un facteur 4 à 12.** Le bon seuil, calculé : `(P − pr − w) ≥ 10,6 ms`
   pour **+1 fps**, `≥ 29,6 ms` pour une coupe visible. La seule capture disponible donne **≤ 10,1 ms**. §5
2. **Et le lot est payé plus cher qu'il ne rapporte.** La contrepartie obligatoire (ciel logiciel) coûte
   0,6 ms en intérieur, **6,0 ms en extérieur**. Bilan **négatif sur 8 cartes sur 9**. **E1M3 et E1M6 sont
   retirées** des cibles. Seule E1M4 survit, pour **+0,2 fps**. §5
3. **Le « chemin déjà armé » n'existe pas** — c'était l'affirmation centrale de l'étude précédente. §6
4. **Ce qui reste vivant est ailleurs** : le **sol** (pas le plafond) en cell avec palette par tuile
   (**×1,67 de couverture**), et le **sol HW du joueur 2** en `RPMD=3`. §3, §2

---

## 1. ZONE 1 — RAMP == RBMP : légal, gratuit, et inutile pour un plafond

### 1.1 La coquille est confirmée, et elle est ironique

**MANUEL**, ST-58-R2 p.85-86, diagramme du registre `18003EH` verbatim :

> `~ RBMP8 RBMP7 RBMP6 ~ RAMP8 RAMP7 RAMP6`
> « RAMP8~RAMP6 18003EH Bit 2~0 For Rotation Parameter A »
> « RBMP8~RBMP6 18003EH Bit 6~4 For Rotation Parameter B »

Confirmé une 2ᵉ fois par le diagramme, une 3ᵉ par la checklist p.282-283. Donc :

| Valeur | RAMP | RBMP | Effet |
|---|---|---|---|
| `0x0011` | 1 (A1) | 1 (A1) | **les deux paramètres sur A1** = le cas de cette zone |
| `0x0001` | 1 (A1) | 0 (A0) | deux bitmaps distincts = ce que la recette voulait |

La recette `§4.2` étape 3 écrivait `0x0011` en annotant « RBMP=0 → A0 » : **la valeur contredisait son
annotation**. Corrigé dans le document parent. La valeur erronée était exactement celle du cas ci-dessous.

### 1.2 Le matériel n'interdit nulle part `RAMP == RBMP` — **MANUEL**

Recherche exhaustive sur le texte intégral du manuel VDP2 (8 formulations de prohibition : *in common*,
*must be different*, *cannot be the same*, *separate VRAM*, *different bank*, *must not*, *may not be*,
*prohibit*). **Trois occurrences pertinentes, toutes dans le même paragraphe p.148**, et toutes à propos
de **RBG0-vs-RBG1** (deux *écrans*) ou rotation-vs-normal — **jamais** à propos de RPA-vs-RPB.

### 1.3 L'inconnue « deux banques en `RDBS=11` » disparaît — **MANUEL**

La table `RDBS` p.149 est libellée **par écran**, pas par paramètre :

> « 1 1 : RAM for RBG0 Character Pattern table (**or Bitmap Pattern**) »

Si `RAMP == RBMP`, tous les accès bitmap tombent dans A1, déjà en `11` → **une seule banque en `11`
suffit**, `RDBS` reste `0x000D` inchangé. L'**inconnue §8.2** de l'étude précédente disparaît, et
**C1 cesse d'être un prérequis** pour ce cas.

### 1.4 Mais c'est fonctionnellement mort pour un plafond — **MESURÉ**

Sur `cd/data/DOOM1.WAD`, pondéré par l'aire des sous-secteurs :

- secteurs `floorpic == ceilingpic` : **0,59 %** de l'aire de l'épisode (max E1M4 : 2,05 %) ;
- aire de **plafond** utilisant le flat de **sol dominant** de la carte : **0,00 % sur les 9 cartes**.

Un plafond alimenté par le même bitmap afficherait **la mauvaise texture partout**.

**Verdict** : `RAMP == RBMP` n'est pas une fonctionnalité, c'est **la sonde HW la moins chère du
bi-paramètre bitmap** — 0 octet VRAM, 0 banque, 0 créneau, `RDBS` inchangé, ~16-40 o de `.text`.
À exécuter **après** l'écriture de la queue RPT (§6), sinon un résultat négatif est ininterprétable.

---

## 2. ZONE 2 — Le mode 2 joueurs : le seul emploi sain d'un second paramètre

### 2.1 Config VDP2 réelle en 2p M7 — **CODE**

| Élément | État | Source |
|---|---|---|
| Split | **VERTICAL**, frontière x=160 ; chaque vue 160×160 écran, 80×160 packé | [d_main.c:386,418](../core/d_main.c#L386) `vpx[4]={0,160,0,160}` |
| RBG0 | **ON, pour P1 seulement** ; W1 = `[0,80]..[159,223]` | [dg_saturn.cxx:7154](../src/dg_saturn.cxx#L7154), `rbg0_floor_win_xend=159` [:7165](../src/dg_saturn.cxx#L7165) |
| Sol de P2 | **software** | idem |
| Ciel HW NBG0 | **ÉTEINT** — `hwsky_split` exige `!sat_lowres`, or M7 ⇒ `sat_lowres=1` | [:7070-7073](../src/dg_saturn.cxx#L7070), [:7135](../src/dg_saturn.cxx#L7135) |
| W0 | **LIBRE** (fenêtre-ligne CCAL du fog parkée, ratio 0) | [:2852](../src/dg_saturn.cxx#L2852) |
| W1 | occupée (clip RBG0) | [:3071](../src/dg_saturn.cxx#L3071) |
| Priorités | identiques 1p/2p, aucune branche par nombre de joueurs | [:2938](../src/dg_saturn.cxx#L2938), [:3587](../src/dg_saturn.cxx#L3587) |

### 2.2 L'arbitrage `RPMD=2` est sur le mauvais axe — **MANUEL + calcul**

Le split est **vertical** ; l'arbitrage par MSB du coefficient de RPA suit la granularité de la lecture,
donc **par ligne** en `K_LINE` (p.161). Mauvais axe.

**La voie `K_DOT` est morte par quantification** : `ΔKAx` a 10 bits fractionnaires (Fig 6.2 p.155), donc
la frontière tombe à `1024/n` colonnes — **171** (n=6) ou **147** (n=7), **jamais 160**. Erreur de 11 à
13 colonnes, parfaitement visible.

**Le bon outil est `RPMD=3`** (fenêtre de paramètre de rotation) : rectangle **exact** `[160,0]..[319,223]`,
A **et** B gardent leur table K **par ligne** (perspective préservée pour les deux joueurs), le MSB
redevient bit de transparence pour chacun, et le line-color est indépendant par paramètre (p.164).
Recette = 4 appels SGL, et **tous les registres concernés (`RPMD` 0xB0, `WCTLD` 0xD6, `WPSx0` 0xC0-0xC6,
`LWTA0` 0xD8) sont dans le block-flush existant**.

> Corollaire : la contrainte « les deux fenêtres VDP2 sont déjà prises » (§4.3 de l'étude précédente)
> est **fausse en 2p M7** — W0 est libre. Elle ne tient qu'en 1p.

### 2.3 RPA et RPB ont bien chacun une table complète — **MANUEL + CODE**

Xst/Yst/Zst, matrice, Cx/Cy/Cz, kx/ky, KAst/ΔKAst/ΔKAx : tout est **par table** (Fig 6.3). Des hauteurs
de sol et des directions de regard différentes sont donc parfaitement supportées. **Mais**
`rbg0_rpt_to_vram` copie RPB à **+0x68** ([:2506](../src/dg_saturn.cxx#L2506)) alors que le matériel le
lit à **+0x80** → bug bloquant à corriger d'abord (§6.3).

### 2.4 La vraie limite est texturale, pas géométrique

Un seul bitmap = **un seul flat et une seule luminosité bakée** pour les deux joueurs. Un 2ᵉ bitmap
coûterait une banque entière qui n'existe pas. **P2 ne gagne que quand son triple dominant coïncide avec
celui de P1** — fréquence **non mesurée**, et c'est le facteur qui détermine tout le rendement.

### 2.5 Qui finance qui : **le 2p finance le 1p, jamais l'inverse**

Le 2p n'exige **ni** 2ᵉ banque, **ni** `MPOFR`, **ni** `DIAG=0`, **ni** miroir plafond dans `core/`, et
ne paie **aucune taxe ciel** (déjà éteint). Il prouve sur HW **3 des 4 inconnues** du plan plafond 1p
(RPT de B lu correctement, 2ᵉ transform vivante, deux tables K coexistantes). Il ne prouve pas
l'arbitrage MSB par ligne. Le 1p ne finance rien : `RPMD=2` et `RPMD=3` sont **mutuellement exclusifs**
(un seul registre).

### 2.6 Défaut de code à corriger indépendamment — **CODE**

`sat_mark_suppress` est **ON à partir de 3 joueurs, où il est inerte** (`sat_split_p1hw=0` ⇒
`sat_vdp2_floor=0` ⇒ `sat_floor_punch_here()` toujours faux ⇒ la garde [r_plane.c:643](../core/r_plane.c#L643)
ne se déclenche jamais) et **OFF en 2p, seul cas où il agirait**. Le gate `==2` date du 2026-07-05 ; la
mesure HW invoquée pour le justifier (191→183 SPL, ~4 % en 4p) date du 2026-07-15 et **n'est pas
explicable par le code** — soit bruit de scène, soit autre effet capturé.

---

## 3. ZONE 3 — Cell bi-paramètre : le cell gagne, mais le gisement est ailleurs

### 3.1 Géométrie — **MANUEL**, la supposition du brief est fausse

Une **page** VDP2 fait **toujours 64×64 cellules = 512×512 dots** (p.50, p.64), quelle que soit la taille
de caractère. Un flat Doom (64×64 px = 8×8 cellules) est donc **1/64ᵉ de page**, pas une page. Le code le
sait déjà : `rbg0_proto_init` écrit une map 64×64 où `cellidx = (my&7)*8 + (mx&7)` = le flat répété 8×8.

### 3.2 Le cell bat le bitmap 64:1 sur le coût de re-upload — **CODE + désassemblage**

| Chemin | Coût d'un changement de flat dominant |
|---|---|
| Bitmap | **131 072 o** (boucle `y<256`, `memcpy(bmp+y*512, row, 512)`) |
| Cell 4bpp | **2 048 o** (64 cellules × 32 o) — **rapport 64:1** |
| Cell + carte réelle | **0 o** (le flat est déjà résident : 64 flats tiennent dans une banque, DOOM1 n'en a que 6 à 18 par carte) |

**L'argument « les plafonds re-uploadent trop souvent, donc C2′ meurt » disparaît intégralement en cell.**

### 3.3 Mais la carte-de-flats réelle ne relève PAS le plafond de gain — **MESURÉ**

Décomposition de la contrainte, mesurée sur DOOM1.WAD :

| Contrainte levée | Gain d'aire au SOL | Gain d'aire au PLAFOND |
|---|---|---|
| le **flat** (= la carte réelle) | **+1,4 pt** | **+1,2 pt** |
| la **bande de lumière** | **+13,3 pt** | +5,9 pt |
| la **hauteur** | indépassable — un plan de rotation = un seul Z | idem |

**Réponse nette à la question ambitieuse : NON.** La contrainte qui mord n'est pas le flat.

### 3.4 Le vrai gisement, que personne n'avait vu — **MESURÉ**

Les **4 bits de palette du pattern-name** (PN 1-mot, 16 palettes) donnent une **bande de lumière par
tuile, gratuitement** :

| Couverture du punch | avant | après | facteur |
|---|---|---|---|
| **SOL** | 21,9 % | **36,6 %** | **×1,67** |
| PLAFOND | 10,1 % | 17,2 % | ×1,70 |

Faisabilité : la pire carte demande **14 palettes sur les 16** disponibles. Et **ce candidat s'applique au
SOL** — chemin déjà shippé et déjà porteur de gain. Il ne demande **aucun deuxième paramètre**.

**Config recommandée** : char 2×2, PN 1 mot, plan 2H×2V → **4096×4096 unités monde pour exactement
131 072 o** (une banque), **2 048 o de réécriture par tuile franchie** = ~26 ko/s à pleine course, soit
**20 % d'un seul re-upload bitmap par seconde**.

### 3.5 Le correctif RPT de l'étude précédente est **à l'envers** — **CODE**

Le *diagnostic* est bon (`slMakeKtable` écrase bien le RPT en cell, 131 072 o prouvés au désassemblage ;
`ΔKAx` parasite ≈ 0,50 entrée/dot ⇒ iso-coefficients à 45° ⇒ **triangles**). Mais **« étendre la copie RA
de 0x54 à 0x60 » pousserait le `ΔKAx` VIVANT de `slScrMatSet`, non nul par construction, et casserait
aussi le sol bitmap qui marche aujourd'hui.**

> **Le bon geste : ÉCRIRE les 12 octets de queue à la main, `ΔKAx = 0` forcé** — pas les copier.

---

## 4. ZONE 4 — CRKTE : illégal, pas seulement cher. **À retirer de la liste.**

Trois verrous indépendants, chacun suffisant.

**VERROU 1 — le manuel l'interdit.** §6.4 p.163 verbatim :
> « When coefficient table data is **required per line**, the coefficient table **must be stored in VRAM**. »

Le stockage en Color RAM n'est offert que dans le paragraphe **suivant**, celui du per-dot. Mimas lit
**par ligne** (`K_LINE`, [dg_saturn.cxx:2882](../src/dg_saturn.cxx#L2882)). **Le « piège à vérifier » du
brief est réel et fatal.**

**VERROU 2 — incompatible avec `RPMD=2`.** CRKTE est **un bit unique et global** (`RAMCTL` bit 15) : il
enverrait RA **et** RB en CRAM, donc tous deux en per-dot. Or p.162 : *« coefficient data cannot be read
to each dot from the coefficient table for rotation parameter B **while** coefficient data for rotation
parameter A **is being read to each dot** »*. Seul `RPMD=3` est cohérent — c'est-à-dire C4.

**VERROU 3 — le coût « nul » est faux.** Mimas est **déjà** en CRAM mode 1
(`slColRAMMode(CRM16_2048)`, `srl_vdp2.hpp:1498`) — la précondition est gratuite — mais il occupe les
**4 096 octets, les 8 bancs** :

| Banc | Contenu | Confisqué par CRKTE ? |
|---|---|---|
| 0 | blanc overlay + palette cell 16c | non |
| 1 | PLAYPAL live | non |
| 2-3 | light-banks L=5, L=10 | non |
| **4-7** | **light-banks L=16, 21, 26, 31** | **OUI — 2 048 o, 100 % vivants** |

La fenêtre `0x800–0xFFF` confisquée est **exactement les 4 light-banks les plus sombres** : 20 des 34
niveaux d'éclairage, **toute la lumière des surfaces VDP1**. Le niveau 31 s'afficherait au niveau 10 =
**~22× trop clair**, avec incohérence visible entre couches HW et logicielles de la même image.

**Comparaison frontale demandée** : CRKTE n'est pas complémentaire de C1, il en est **strictement
dominé**. C1 libère la même chose **légalement**, pour ~4 octets et zéro CRAM. Et **si C1 est faux, CRKTE
ne sauve pas C2′ non plus** : RB resterait per-line, donc en VRAM, donc en banque `01` — ce que p.150 et
p.167 interdisent **deux fois** sous CRKTE. Les deux branches mènent au même refus.

---

## 5. ZONE 5 — L'arithmétique : le seuil, dérivé

### 5.1 Le seuil, calculé (pas deviné)

`fps = 1000/MST` est exact (le code dérive littéralement `mst = 10000/inst10`). Donc :

> **Δ requis pour +n fps = MST² · n / (1000 + MST · n)**

| MST | fps | Δ pour **+1 fps** | Δ pour **−10 % de temps frame** |
|---|---|---|---|
| 30 | 33,3 | 0,87 ms | 3,00 ms |
| **37** | **27,0** — *M7 1p, HW* | **1,32 ms** | **3,70 ms** |
| 42 | 23,8 | 1,69 ms | 4,20 ms |
| 50 | 20,0 | 2,38 ms | 5,00 ms |
| 77 | 13,0 | 5,51 ms | 7,70 ms |

Mais **2,5 ms n'est pas le Δ** : c'est le budget **brut**, dont seule la fraction `m = c% × f_dom` est
récupérable. Avec `m ≈ 0,125` (central) :

> **Seuil correct : `(P − pr − w) ≥ 10,6 ms` pour +1 fps, `≥ 29,6 ms` pour un cap visible.**
> Le seuil de 2,5 ms était **4× trop bas** pour le barème le plus faible, **12× trop bas** pour un
> barème perceptible. (Il vaudrait +1 fps à MST 50, pas à MST 37.)

**Et le budget mesuré ne l'atteint pas** : la seule capture M7 1p donne `P = 14,0 ms`, `pr = 3,9 ms` ⇒
`P − pr − w ≤ 10,1 ms`. Le candidat est **au mieux exactement à la limite du +1 fps**.

### 5.2 Le croisement ciel/plafond — re-mesuré, et E1M3 est retiré

Re-mesuré avec **deux estimateurs indépendants** (polygones de sous-secteurs par découpe des demi-plans
BSP ; et aires de secteurs par théorème de Green sur les linedefs, sans BSP). Ils concordent à ±2 points.
**Le ciel couvre 2× ce que l'étude précédente annonçait** (E1M1 35 % vs 17 %, E1M3 19 % vs 9 %,
E1M6 20 % vs 11 %).

Le ratio de rentabilité `k* = A_plafond_dom / A_ciel` vaut **0,06 à 0,88 sur 8 cartes sur 9** — il
faudrait qu'un pixel de ciel logiciel soit 1,1× à 17× **moins cher** qu'un pixel de plafond. **Il ne
l'est pas** : le désassemblage donne **13 instructions/px pour le ciel contre 10,5 pour le plafond `ld`**
— le facteur penche du **mauvais** côté (~1,1-1,3× en défaveur du ciel).

> **E1M3 : RETIRÉ** (bilan **−11,0 points**). **E1M6 : RETIRÉ** (−10,5).
> **Seule E1M4 survit** (`k* = 4,50`, +7,7), et son `f_dom` statique de 10 % y plafonne le lot à
> **~0,3 ms ≈ +0,2 fps**.

### 5.3 Le coût du ciel logiciel qu'impose `DIAG=0`

`sat_sky_px` compte les pixels **packetés** de couverture des visplanes de ciel — et il est **illisible
aujourd'hui** : la ligne CLS est construite puis annulée par `(void)r13`.

| Scène (E1M1, ancres HW) | px ciel | coût M7 |
|---|---|---|
| intérieur | 2 252 | **≈ 0,6 ms** |
| cour extérieure | 23 813 | **≈ 6,0 ms** = **−3,5 fps** |

Coût unitaire mesuré par instruction : **~0,50 ms pour 1000 px de ciel logiciel**. À comparer à un lot
plafond plafonné à 1,3-1,9 ms.

### 5.4 Deux débits que la formule omettait

`delta_P = (P − pr − w) × c% × f_dom` **− N_sky × t_sky − coût_re-upload**. Le re-upload de 131 072 o à
chaque ré-élection du plafond dominant coûte **≥ 5,5 ms** (plancher dur dérivé du débit de blit mesuré
23,6 o/µs) — **plus qu'une frame entière de lot**. Fréquence tolérable : **< 1 fois toutes les 4 à 11
frames**. Et ce n'est **pas mesurable aujourd'hui** : `up` (`rbg_upl_sum`) est accumulé, remis à zéro, et
**imprimé nulle part**.

---

## 6. ZONE 6 — Les mesures préalables

### 6.1 « Les lignes au-dessus de l'horizon sélectionnent déjà RPB » : **RETIRÉ**

**MANUEL** p.152, verbatim :
> « (coefficient table address) = KAst + **ΔKAst × (V counter value)** + **ΔKAx × (H counter value)** »

Donc **moitié de TABLE ≠ moitié d'ÉCRAN** : ce qui décide de l'entrée lue à la ligne *y* est `KAst` et
`ΔKAst`. **Désassemblage exhaustif** (cartographie `GBR = 0x060FFC00`, auto-vérifiée par deux systèmes de
coordonnées indépendants : `@(368/372/374,gbr)` → chip `0xB0/0xB4/0xB6` = `RPMD`/`KTCTL`/`KTAOF`, et
`@(624/628/632,gbr)` → `_RotScrParA + 0x54/0x58/0x5C`) :

- **un seul accès dans tout le binaire** : `slKtableRA` écrit `KAst` (`@(624,gbr)`) ;
- **`ΔKAst` et `ΔKAx` ne sont écrits nulle part** — ni via GBR, ni par adresse absolue ;
- `rpara_init` initialise `0x00` à `0x50` — il **s'arrête avant** `0x54` ;
- `slScrMatSet` n'écrit aucun des trois ;
- `rbg0_rpt_to_vram` copie **0x54 octets** ([:2505](../src/dg_saturn.cxx#L2505)) → **ils ne partent jamais
  en VRAM**.

Le code le documente lui-même : *« slScrMatSet only fills SGL's CACHED RAM buffer […] the RPT VRAM
transfer is done by the _BlankIn ISR, armed ONLY by slSynch »* ([:7212](../src/dg_saturn.cxx#L7212)) — et
Mimas n'appelle pas `slSynch`.

> **L'affirmation §4.2 de l'étude précédente est indémontrée et probablement fausse. Le chemin est mort,
> pas armé.** Ne budgétez rien dessus.

**En mode CELL c'est même chiffrable** : `slMakeKtable(A0)` écrit 131 072 o et écrase le RPT placé en
A0+0x1FF00 ; `KAst/ΔKAst/ΔKAx` y valent alors ≈ `0x201` → index de base 0, pente **0,008/ligne et
0,008/dot** → bascule RPB plein écran **avec frontière oblique** = **les triangles**, complètement
expliqués et quantifiés.

**Le paradoxe qui reste ouvert** : la perspective d'un plan Mode-7 vient *uniquement* de la rampe de
coefficients par ligne. Or **le sol RBG0 est perspectif et fonctionne**. Donc `ΔKAst` en VRAM est non nul
sans que le code ne l'écrive. Candidat le plus probable : §6.2.

### 6.2 L'ISR vblank : les **deux** commentaires du fichier sont faux — **désassemblage**

`_BlankIn` programme le DMAC ch.1 avec `TCR=0x90` et `CHCR=0x5601` (`TS=01` = **mot**) → **144 mots =
288 octets = offsets `0x000..0x11E`**, donc **`RAMCTL` (0x0E) et les huit mots `CYC` (0x10-0x1E) inclus**.
Preuve croisée : l'image shadow fait exactement `0x120` octets (`0x60ffcc0` → `0x60ffde0`).

> Le « `0x00..0x8E` » répété quatre fois dans `dg_saturn.cxx` vient d'avoir lu **`TCR` comme un compte
> d'octets**. Et « the vblank ISR never pushes CYC/RAMCTL » est faux aussi.

Le flush est gaté par un octet décrémenté à chaque champ → il se rouvre **au minimum tous les 256 champs
(~4,27 s NTSC)**. **Corollaire dur** : tous les réglages runtime justifiés par « l'ISR ne repousse jamais
ces registres » (fenêtre W1, `LWTA0`, `RPMD`) sont **potentiellement annulés périodiquement**. À trancher
**avant** toute recette à base de poke.

> Ceci corrige §3.5 du document parent : le block-flush de Mimas (`0x0E..0xFE`) **ne couvre pas
> `0x100..0x11E`**, mais l'ISR, elle, les pousse.

### 6.3 Les deux bugs RPB, précisés

`slRparaInitSet` initialise `_RotScrParA` **et** `_RotScrParB` = `_RotScrParA + 0x68`. **En RAM SGL le
stride A→B est `0x68` ; côté VRAM le matériel lit RPB à `+0x80`** (`_BlankIn` : `mov #-128,r0`). La copie
de Mimas est donc *fidèle à la RAM et fausse pour le matériel* — et **tronquée à `0x30`** au lieu de
`0x54`. Les 24 premiers octets de RPB (Xst/Yst/Zst/ΔXst/ΔYst/ΔX) sont aujourd'hui **non initialisés**.

### 6.4 `cyc_before[4]`, et ce qu'on ne peut pas mesurer

Bien capturé ([:3003-3006](../src/dg_saturn.cxx#L3003)), **affiché nulle part** — mais il lit le
**shadow**, et les registres `CYC` sont **write-only** (p.39). Le shadow **est** donc la mesure maximale
disponible. Le seul détecteur de divergence est `RAMCTL` (lisible) plus une sentinelle sur `RPT+0x54`.

### 6.5 Le travail non commité : la sonde LAG **n'existe plus**

Le diff (+210/−10 lignes) l'a **supprimée** et remplacée par un correctif de latence VDP1 actif par
défaut plus un levier d'anticipation de lacet à gain 0 (inerte). **En chantier, pas fini.**
`.bss` mesuré : 24-28 o. `.text` : ~136 o mesurés au désassembleur, ~300-400 estimés.

### 6.6 Renseignement gratuit déjà codé mais coupé

| Grandeur | État | Ce qu'elle vaut |
|---|---|---|
| `sat_prof_dom_pct` | ligne 17 coupée + compteur désarmé | **c'est le `f_dominant` que l'étude devine à 0,25-0,45** |
| `rbg_upl_sum` (`up`) | accumulé, imprimé nulle part | le coût des re-uploads (§5.4) |
| `dg_heap_peak` (`hp`) | **pas de ligne 22** | le critère de la soupape `HEAP_SIZE` — **non décidable** |
| `cyc_before[4]` | capturé, jamais affiché | §6.4 |

**La ligne 18 est la seule réellement libre** (ligne 19 = DEPORT-PREVIEW, ligne 17 = SPL en split).

---

## 7. Ce que les réfutations ont cassé

**M0 (la mesure d'étape 0) : le noyau survit, deux des trois leviers sont morts.**
- **`pad Y` ne coupe pas RBG0.** `rbg0_mode` est **absent de la table des symboles** — GCC l'a replié en
  constante 0. `SAT_FLOOR_PERFSIM` est dans une branche `#elif` morte. Le handler compilé cycle la
  **qualité logicielle** du sol. **La borne empirique du sol HW n'est pas obtenable dans ce binaire.**
- **`pad L+Y` est une inversion logique.** En M7, `SQ_FLAT` ⇒ `R_DrawVisplanePotato` = un `memset` par
  span : **le nombre de pixels écrits est identique**. `ΔP` ne mesure que le *fetch de texture*. Si
  `ΔP ≈ 0`, la lecture correcte est « la phase est bornée par l'**écriture** framebuffer » — exactement
  le terme qu'une surface VDP2 supprimerait. **Le critère d'abandon proposé produirait un faux négatif.**
- **`w` est latché indéfiniment**, pas « périmé d'une frame » : `p3_wait_ticks` a **une seule
  affectation** et **aucune remise à zéro** ; quand le vol de travail rafle toutes les visplanes,
  `RP_WaitPlanes` n'est **pas appelé** et la ligne 5 réaffiche une valeur arbitrairement ancienne.
- **`P − pr` mélange les échelles** : `pr` est une **moyenne sur ~1 s**, `P` un **échantillon d'une
  frame**. Et **soustraire `w` est un biais pessimiste** : si le plafond quitte la worklist, la moitié de
  l'esclave rétrécit aussi — `w` fait partie de l'enveloppe récupérable. **L'enveloppe correcte est
  `P − pr`.**

**C1 : le mécanisme survit, le chiffrage tombe.**
- **Le gain VRAM n'est pas 131 072 o.** `slMakeKtable` écrit **toujours** 128 Ko sans paramètre de
  taille : changer `RDBS` libère une **désignation**, pas un octet. Pour libérer les octets il faut
  **cesser d'écrire A0** (→ N3).
- **Le patch de 4 octets est un A/B à deux variables.** `CYCA0 = 0xEEEE` = 8 créneaux « CPU Read/Write »,
  aujourd'hui **inertes**. Dès que A0 passe `RDBS=00`, ils deviennent **vivants et illégaux** (p.32 : la
  valeur de repos légale est `0xFFFF` ; p.36 : l'accès CPU exige un appariement bank0+bank1 et un créneau
  « no access » précédent — **double violation**). **Le patch minimal correct est 3 mots**
  (`RDBS` + `CYCA0L` + `CYCA0U → 0xFFFF`), pas 1. Sinon un sol cassé est **ininterprétable**.

---

## 8. Tableau de décision

Classement par **(gain attendu × probabilité) / coût pool**. Pool disponible **relu ce jour :
4 496 octets** (`_end = 0x060f8e70`), **marge 400 o** au-dessus de 4096.

| Rang | Candidat | Gain | Prob. | Pool | VRAM | Verdict |
|---|---|---|---|---|---|---|
| **1** | **M0′ — mesure d'étape 0 corrigée** (photo lignes 1+2+5 ; `P − pr` ; RAZ de `p3_wait_ticks`) | 0 ms, mais conditionne tout | 100 % | **~4-50 o** | 0 | **À FAIRE D'ABORD** |
| **2** | **N6 — déverrouiller le renseignement déjà codé** (`sat_prof_dom_pct`, `up`, `hp`, `cyc_before`) sur la ligne 18 | 0 ms — donne `c%` et `f_dom` | 100 % | ~60-250 o (à retirer après) | 0 | **FAIRE** — peut tuer plusieurs candidats d'un coup |
| **3** | **N7 — trancher ce que l'ISR vblank repousse** | 0 ms, retire un risque systémique | 90 % | ~250 o (temporaire) | 0 | **FAIRE avant tout poke** |
| **4** | **N5 — corriger le gate `sat_mark_suppress`** | À MESURER (`Bp`, pad L+B déjà câblé) | 100 % que le défaut soit incohérent | **~0-10 o** | 0 | **FAIRE** — coût nul |
| **5** | **N3 — rampe K manuelle (896 o) + queue RPT écrite (`ΔKAx=0`) + copie RB à `+0x80`** | 0 ms en régime ; **libère ~130 Ko de A0**, supprime les triangles à la racine, supprime un `memset` de 128 Ko au boot | ~70 % | ~150-180 o | **−130 048 o** | **FAIRE** — meilleur ratio VRAM/octet du dossier |
| **6** | **C1 — `RDBS` A0 `01→00`** (patch **3 mots**, `CYCA0 → 0xFFFF`) | 0 ms ; libère une **désignation** | ~80 % | ~35-65 o | 0 | Faire **avec** N3, pas seul |
| **7** | **RAMP == RBMP** — sonde HW du bi-paramètre | 0,00 % en plafond ; **sonde** | légal ~95 % ; que la sonde montre qqch : **faible** | ~16-40 o | 0 | Sonde, **après** N3 |
| **8** | **N1 — sol HW de P2 en 2p** (`RPMD=3` + RPB, fenêtre W0) | À MESURER ; +6 400 px packés/frame | ~55 % | ~200-400 o **+ N3** | 896 o + 96 o, **0 banque** | **Meilleur emploi d'un 2ᵉ paramètre.** Soupape pool requise |
| **9** | **N2 — SOL en cell, carte réelle + palette par tuile** | **×1,67 de couverture** sur un gain déjà acquis | ~45 % | **> 1 024 o** | 131 072 o PN + 12-37 Ko cellules | **Plus gros gisement, plus gros risque.** Soupape obligatoire |
| **10** | **C5 — plafond dégradé sur NBG2** | ≤ 1,26 ms = **≤ +0,95 fps** ; ne paie pas la taxe ciel | ~50 % | ~400-700 o | +9 216 o (queue B0) | Seul plafond encore vivant. **Uniquement si M0′ donne ≥ 10,6 ms** |
| **11** | **C4 — `RPMD=3` au plafond en 1p** | **NET NÉGATIF** sur 8/9 cartes | 40 % techn., **~10 %** de bilan positif | > 1 500 o | 131 072 o + 896 o | **ABANDONNÉ en 1p** — ne survit que sous forme N1 |
| **12** | **C2′ — plafond bitmap bi-paramètre** | **NET NÉGATIF** + re-upload ≥ 5,5 ms | **~15 %** (4 conditions indépendantes) | > 1 500 o | 131 072 o + re-upload jusqu'à 262 144 o | **ABANDONNÉ** |
| **13** | **C3 — cell bi-paramètre au plafond** | même lot, même taxe ciel | ~20 % (jamais armé) | > 1 500 o | 2 banques | **ABANDONNÉ** — son résidu utile est N2 |
| **14** | **CRKTE** | 0 ms — **illégal** | **~0 %** (3 verrous) | ~170 o | 0 VRAM mais **2 048 o de CRAM vivante** | **RETIRÉ DE LA LISTE** |

### Séquence

1. **M0′** — une photo HW, lignes 1+2+5. Calculer **`P − pr`** (pas `P − pr − w`).
   **Couperet : si `P − pr` < 10,6 ms, toute la famille plafond meurt.** Ne pas utiliser `pad Y`
   (mort) ni le critère `ΔP ≈ 0` sur `pad L+Y` (inversé).
2. **N6** — armer `sat_prof_planepix`, sortir `sat_prof_dom_pct` et `up` sur la ligne 18. Donne `c%` et
   `f_dom` réels. *Attention : l'instrument gonfle le `P` qu'il mesure — le chiffre lu n'est pas celui du
   build de production.*
3. **N7** — compteur de flush ISR sur la ligne 18. Tranche si les pokes runtime survivent.
4. **N5** — inverser le gate `sat_mark_suppress`, lire `Bp` en 2p.
5. **N3** — rampe K manuelle + queue RPT + copie RB. **Le seul candidat qui rend 130 Ko pour 170 octets.**
6. **C1** (3 mots) puis **RAMP == RBMP** comme sonde.
7. Selon les chiffres de 1-2 : **N1** (2p) ou **N2** (sol cell), jamais C2′/C3/C4.

**Avant l'étape 5, tirer une soupape pool** — `SRL_MALLOC_METHOD` ([Makefile:14](../Makefile#L14),
~3 192 o, `control_t` TLSF = pur overhead) ou `HEAP_SIZE` ([syscalls.c:55](../src/syscalls.c#L55),
8 192 o) — sachant que **`hp` n'a pas de ligne d'overlay**, donc la seconde n'est **pas décidable en
l'état** : la rendre lisible fait partie de N6.

---

## 9. Note de méthode

**Le pool a pris six valeurs en deux jours** (5 024 → 4 400 → 4 240 → 4 736 → 4 544 → 4 512 → **4 496**),
parce que `__heap_start = _end` et que l'arbre est reconstruit sous nos pieds. **Aucun devis pool n'est
valide plus d'un build.** Relire `build/Mimas.map` avant chaque décision.

**Les adresses de désassemblage de l'étude précédente sont périmées** — `_slBitMapRbg0` est passée de
`0x060680d0` à `0x06067f90` après un rebuild. **Citer par symbole, jamais par adresse.**
