# RBG0 — "grille de cells" sur hardware réel (perspective correcte sur Ymir)

**Diagnostic & plan d'action — 2026-06-26**

Symptôme décisif : sur hardware réel le sol RBG0 n'est PAS de la neige aléatoire
(bus noise). C'est une **grille régulière de cells/tuiles** (glyphes 8x8) étalée
sur tout l'écran. Sur Ymir : sol en perspective correct. Couche confirmée = RBG0.
Registres committés 3 façons (dont `slSynch` one-shot), `slCashPurge` fait à l'INIT.

---

## 1. Ce que le symptôme "grille de cells" prouve

La distinction neige vs grille structurée est **décisive et tranche le diagnostic**.

- **Neige aléatoire** = le moteur de rotation lit des **banques non assignées** /
  affamées sur le bus → octets aléatoires. C'était l'ancien bug (RDBS/CYC),
  corrigé via `rbg0_commit_ramctl` (RAMCTL=0x138D) — voir `docs/RBG0_SNOW_FIX_PLAN.md`.
- **Grille régulière de glyphes** = le moteur **lit correctement** la
  pattern-name table (map, B1) ET les characters (cells, A1). On VOIT le vrai
  contenu des cells, tuilé proprement. **Donc le FETCH marche** : RDBS/CYC/banques
  sont bons. Ce n'est plus un problème de bus/starvation.

Ce qui manque, c'est uniquement la **géométrie** : le plan se tuile à plat
(affine 1:1, page 64x64 cells répétée par `slOverRA(0)`=REPEAT) au lieu de se
gauchir vers l'horizon. Autrement dit la **transformation de rotation (RPT) et/ou
la table de coefficients K ne s'appliquent jamais** sur silicium réel.

→ Le problème est dans la chaîne **transform / coefficient / cache**, PAS dans le
fetch des cells. Les 5 recherches convergent sur ce point sans exception.

**Prouvé en source** (vérifié) : à plat avec `picnum < 0` au titre, les cells sont
`memset` à 0 (`dg_saturn.cxx:1287`) et `rbg0_upload_flat` sort tôt
(`dg_saturn.cxx:1263`) — donc la grille N'EST PAS des cells périmées au titre.
C'est bien un échec de transform en jeu.

---

## 2. Les causes classées (probabilité décroissante)

Note importante établie par 4 des 5 recherches via **désassemblage de LIBSGL.A** :
la "cause cache" et la "cause RPT-jamais-transférée" sont en réalité **les deux
faces du MÊME mécanisme**. Je les classe en conséquence.

### (a) — TRÈS PROBABLE — La RPT VRAM n'est jamais transférée sans `slSynch` (et le buffer source est CACHED)

C'est le candidat n°1, soutenu par 4 recherches sur 5 avec preuve par désassemblage.

- `slScrMatSet` **n'écrit PAS** la rotation-parameter table en VRAM. Il pointe GBR
  sur le buffer RAM fixe de SGL `_RotScrParA = 0x060FFE1C` (Low Work RAM **cachée**),
  y écrit les termes de matrice via stores GBR-relatifs, et pose un flag "dirty"
  à `@(204,gbr)=1`. Il ne touche jamais la RPT VRAM `0x25E7FF00` (= `VDP2_VRAM_B1+0x1ff00`).
- Le transfert RAM→VRAM de la RPT est fait **uniquement** par l'ISR VBlank-IN de
  SGL (`_BlankIn`, sglI00.o) : `DMA_Trans(_RotScrParA → RPT VRAM, 0x30 octets)` ×2
  (RA, puis RB à +0x68). Ce DMA est **armé** par les flags frame-change que seul
  `slSynch` arme (sglI01.o). Sans `slSynch` par frame, `_BlankIn` fait son early-out
  et **saute le DMA de la RPT**.
- Conséquence : `rbg0_set_transform()` par frame met à jour seulement le buffer RAM
  caché ; la RPT VRAM garde la transform **du one-shot d'init** (`slSynch` à
  `dg_saturn.cxx:1639`). Le moteur de rotation relit la RPT VRAM **chaque champ** →
  transform figée/identité → tuilage à plat = exactement la grille observée.
- **Le commentaire du code est FAUX** (vérifié) :
  `dg_saturn.cxx:1224` et `:1232` affirment *"slScrMatSet writes the rpara straight
  to VRAM, so this needs no slSynch"*. Le désassemblage prouve l'inverse : écriture
  RAM cachée, transfert VRAM gated par `slSynch`.
- **Échantillon SRL qui marche** (vérifié, `Samples/VDP2 - RBG0 Rotation/src/main.cxx:110-120`) :
  chaque frame fait `slPush…/SetCurrentTransform()` (= `slCurRpara+slScrMatConv+slScrMatSet`,
  `srl_vdp2.hpp:1249-1258`) **puis `SRL::Core::Synchronize()`** (= `slSynch`,
  `srl_core.hpp:126`). C'est précisément le `slSynch` par frame que Mimas saute.

Le volet "cache" se ramène ici : `slCashPurge` à l'init (`dg_saturn.cxx:1644`) est
**inopérant** parce que (1) il est init-only alors que `rbg0_set_transform` tourne
chaque frame, et surtout (2) **il n'y a aucun `slSynch` pour faire le transfert** —
purger un cache sans déclencher le DMA ne sert à rien. La RPT n'est pas un problème
de cohérence cache du SH-2, c'est un **transfert manquant**.

### (b) — PROBABLE (corollaire de (a)) — Table de coefficients K : adressage par-ligne dégénéré → pas de recul → tuilage à plat

Soutenu par la recherche n°2. À considérer **après** avoir validé (a), car les deux
peuvent coexister.

- Le **mode K est correct** (vérifié) : `slMakeKtable` + `slKtableRA(K_FIX|K_LINE|K_2WORD|K_ON)`
  (`dg_saturn.cxx:1316-1317`) — même famille que SRL (`srl_vdp2.hpp` chemin vblank=false)
  et Jo. `K_FIX` = table statique pré-bâtie (correct pour un moteur sans `slSynch`).
- Les **valeurs K sont en VRAM NON-cachée** (`RBG0_KTAB_VRAM=0x25E00000`, banque A0,
  région `0x25Exxxxx` uncached). `slMakeKtable` les écrit directement (`mov.l r0,@r4`).
  Donc **PAS un problème de flush cache** côté table K. Ne pas y perdre de temps.
- Le vrai suspect K : les champs **KAST/DKAST/DKA** (qui *adressent* la table par ligne :
  addr = `KAST + line*DKAST`) vivent dans la ROTSCROLL, donc dans le **MÊME buffer RAM
  caché** que (a), transféré par le même DMA `slSynch`-gated. Si `DKAST=0` ou `KAST`
  pointe ailleurs → même coefficient pour chaque ligne → échelle constante → plan
  jamais en recul → grille à plat. **C'est le même bug que (a)** : le fix de (a) couvre ça.
- Suspect K indépendant à écarter : `slMakeKtable` est appelé **une seule fois,
  AVANT la première matrice valide** (`:1316` avant `:1327`) et **jamais rebâtie** ;
  par frame seul `rbg0_set_transform` tourne (réécrit la RPT, pas les valeurs K). Si
  la table a été bâtie sur un état dégénéré, elle reste plate.
- Mineur à écarter (1 ligne) : SRL fait `VDP2_RAMCTL &= 0xffcf` juste après
  `slKtableRA` (`srl_vdp2.hpp:1210,1226`) pour réinitialiser les bits coeff laissés
  par un éventuel `K_DOT` antérieur. Mimas l'omet (poke manuel `RAMCTL=0x138D`,
  `dg_saturn.cxx:1349`). Probabilité faible mais c'est 1 ligne à tester.

### (c) — PEU PROBABLE — Cells/map/plane/over-process mal formés

Écarté par les recherches n°3 et n°4 (analyse + désassemblage).

- Cells, map et K vont tous en VRAM `0x25Exxxxx` **non-cachée** → atteignent la VRAM
  immédiatement, pas de flush nécessaire (et `rbg0_upload_flat` lit du vrai contenu
  → on voit des cells structurées, donc le chemin marche).
- Map word `(cellidx*2)|0x1000` + stride cell 64 octets (= 2 char-units en 256 couleurs)
  + `PL_SIZE_1x1` + `slOverRA(0)`=REPEAT : **auto-cohérent** et identique au pattern
  Mode-7 de Jo/SRL. Un petit plan + REPEAT donne une **texture répétée sans couture**,
  PAS une "grille de tuiles distinctes" — donc l'over-tiling **n'est pas** la cause.
- Diff d'ordre map Mimas (row-major) vs Jo (column-major) = simple transposition de
  la texture, **pas** une cause de grille.

### (d) — ÉCARTÉ — Cells périmées au titre

Réfuté en source (vérifié) : `memset` cells à 0 (`:1287`) + `picnum<0` early-return
(`:1263`). Avant le 1er flat les cells sont index 0 (uniforme), pas des glyphes.

### (e) — ÉCARTÉ — Plane-size / over-process

Voir (c) : `PL_SIZE_1x1` + REPEAT est le réglage canonique d'un sol infini Mode-7.
Ne produit pas une grille de tuiles distinctes par construction.

---

## 3. LE prochain test (le moins cher, le plus discriminant)

L'expérience la plus tranchante n'est PAS d'ajouter `slCashPurge` par frame (sans
`slSynch` il n'y a pas de transfert à rendre cohérent — voir (a)). C'est de
**reproduire le transfert que fait `_BlankIn`**, sans payer `slSynch`.

### Test A — A/B de confirmation en 1 ligne (à faire EN PREMIER)

Dans `DG_DrawFrame`, bloc `rbg0_mode==0` (`dg_saturn.cxx:2863-2867`), ajouter un
`slSynch()` par frame **juste après** `rbg0_set_transform()` :

```cpp
    if (rbg0_mode == 0)
    {
        rbg0_upload_flat(sat_vdp2_floor_pic);
        rbg0_set_transform();
        slSynch();   /* TEST A : prouve que le transfert RPT manquait */
    }
```

- **PASS** (la grille plate se gauchit en sol en perspective qui suit la caméra sur
  HW) → **confirme la cause (a)** : la transform n'atteignait jamais la VRAM. On passe
  ensuite au fix optimisé (Test B) pour ne pas payer le cap vblank de `slSynch`
  (~7-12 fps) ni son conflit SCSP (SFX muets) — raisons documentées à `:2903-2907`.
- **FAIL** (toujours une grille plate) → la RPT n'est pas seule en cause : passer à la
  table K statique pleine (chemin `vblank=false` de SRL, `slMakeKtable` bâtit toute la
  table 0x18000 octets une fois) pour écarter la table de coefficients.

### Test B — Le fix sans `slSynch` (à faire si Test A = PASS)

Remplacer le `slSynch()` par la copie manuelle ciblée de la RPT (ce que `_BlankIn`
fait), en lisant la source via **l'alias non-caché 0x26…** (ou `slCashPurge()` juste
avant pour flusher les writes cachés de `slScrMatSet`) :

```cpp
        rbg0_set_transform();
        /* TEST B : reproduire le DMA RPT de _BlankIn, sans slSynch.
           Source = buffer RAM caché de SGL, lu via l'alias UNCACHED 0x26…
           Dest   = RPT VRAM (VDP2_VRAM_B1 + 0x1ff00). 0x30 octets / plan. */
        memcpy((void *)(0x25E7FF00),        (const void *)(0x260FFE1C), 0x30); /* RA */
        memcpy((void *)(0x25E7FF00 + 0x68), (const void *)(0x260FFE84), 0x30); /* RB */
```

- **PASS** → fix retenu : transfert RPT manuel par frame, coût quasi nul, pas de cap
  vblank, pas de conflit SCSP. C'est la voie d'expédition propre.

### Test C — Discriminant cells-vs-transform (orthogonal, optionnel)

Si on veut isoler "cells lues correctement" de "transform fausse" **sans** toucher
au transfert : dans `rbg0_proto_init`, remplacer le `memset` par une couleur unie
reconnaissable et désactiver la branche K :

```cpp
    memset((void *)RBG0_CEL_VRAM, 0x04, 64 * 64);  /* index palette vif au lieu de 0 */
    /* commenter slMakeKtable/slKtableRA/slRparaMode ; slRparaMode(RA) (affine pur) */
```

Build avec `RBG0_DISPLAY=1`.

- Zone RBG0 devient une **surface unie** de l'index 4 → map+cells+plane OK, la grille
  venait bien de la transform/K (confirme (a)/(b)).
- Toujours une **grille de tuiles 8x8 distinctes** avec un remplissage uni → vrai bug
  pattern-name (largeur de mot `PNB`/`CN_12BIT` ou char-size) → tester `PNB_2WORD`/
  `CN_10BIT` et l'ordre column-major de Jo. (Faible probabilité d'après (c).)

**Recommandation : Test A d'abord** (1 ligne, verdict immédiat), puis Test B si PASS.

---

## 4. Plan de repli

Si Test A et Test B échouent tous deux — c.-à-d. que RBG0 **exige réellement le
transfert par frame de `slSynch`** et qu'aucun transfert manuel ne suffit (p. ex.
le moteur attend aussi un re-push BGON/scroll ou une recompute K que seul l'ISR
vblank complet fait) — il faut l'admettre honnêtement :

- `slSynch` par frame **n'est pas acceptable** dans Mimas : il plafonne à ~7-12 fps
  (cap vblank) et entre en conflit avec le SCSP (SFX muets). C'est documenté et
  HW-testé pire (`dg_saturn.cxx:1337`, `:2903-2907`). Le pipeline parallèle dual-SH2
  repose sur l'**absence** de `slSynch` (`rp_sgl_workptr_reset`, `core/r_parallel.c`).
- Dans ce cas, **RBG0 n'est pas la voie d'expédition pour le sol.** La voie retenue
  est le **sol texturé VDP1** (plan documenté dans `docs/VDP1_WORLD_PLAN.md` /
  `docs/VDP2_FLOOR_CONSOLIDATION.md`) : même gain mesuré sur le rendu du sol, **sans
  moteur de rotation** donc sans dépendance au transfert RPT vblank-gated. Pas de
  `slSynch`, compatible avec le pipeline parallèle et le son.

Honnêteté prouvé / dérivé :
- **Prouvé** (source vérifiée ce jour) : commentaires faux `:1224/:1232/:2862` ;
  `slCashPurge` init-only `:1644` ; pas de `slSynch` par frame `:2903` ; one-shot
  init `:1639` ; cells `memset`+early-return `:1287/:1263` ; échantillon SRL
  `Synchronize` chaque frame `Samples/VDP2 - RBG0 Rotation/src/main.cxx:120` ;
  `SetCurrentTransform` = `slCurRpara+slScrMatConv+slScrMatSet` `srl_vdp2.hpp:1249`.
- **Dérivé** (désassemblage LIBSGL.A rapporté par les recherches, non re-vérifié
  par moi) : adresses `_RotScrParA=0x060FFE1C`/`_RotScrParB=0x060FFE84`, le DMA RPT
  de `_BlankIn` (0x30 oct ×2, +0x68), le gating par flags frame-change armés par
  `slSynch`, le flag dirty `@(204,gbr)`. C'est cohérent et corroboré par 4 recherches
  indépendantes, mais **le Test A le valide définitivement avant tout commit**.

### Références

- Code Mimas : `c:\Users\pcico\Projects\Mimas\src\dg_saturn.cxx`
  — `:1224/:1232` (commentaire faux), `:1228-1249` (`rbg0_set_transform`),
  `:1258-1281` (`rbg0_upload_flat`, early-return `:1263`), `:1283-1332`
  (`rbg0_proto_init`, `memset :1287`, K `:1316-1318`), `:1345-1355`
  (`rbg0_commit_ramctl`, RAMCTL=0x138D), `:1639` (`slSynch` one-shot),
  `:1644` (`slCashPurge` init-only), `:2863-2867` (path par frame), `:2903-2907`
  (pas de `slSynch`, raison fps/SCSP).
- SRL : `SaturnRingLib\saturnringlib\srl_vdp2.hpp:1249-1258` (`SetCurrentTransform`),
  `:1191-1244` (`SetRotationMode`, K vblank), `:1210/:1226` (workaround `&=0xffcf`),
  `:1529` (`slRparaInitSet` à B1+0x1ff00) ;
  `SaturnRingLib\Samples\VDP2 - RBG0 Rotation\src\main.cxx:110-120` (Synchronize/frame) ;
  `SaturnRingLib\saturnringlib\srl_core.hpp:126` (`Synchronize`→`slSynch`).
- SGL : `SaturnRingLib\modules\sgl\INC\sl_def.h` (struct rdat / ROTSCROLL,
  KAST/DKAST/DKA, flags `K_*`) ; LIBSGL.A membres sglB038/B042/B044/B045 (rotation),
  sglI00 (`_BlankIn`), sglI01 (`_slSynch`).
- Référence DoomJo : `c:\Users\pcico\Projects\DoomJo\joengine\jo_engine\vdp2.c:330-360`
  (setup RBG0 K, tourne AVEC `slSynch` chaque frame).
- Docs Mimas : `docs/RBG0_SNOW_FIX_PLAN.md` (ancien bug neige/CYC),
  `docs/VDP1_WORLD_PLAN.md` + `docs/VDP2_FLOOR_CONSOLIDATION.md` (voie de repli).
- Externe : SGL Programmer's Tutorial 8-9 "Rotating scroll screen"
  (https://segaretro.org / archive exodus `p08_90.htm`) — boucle par frame
  `slCurRpara(RA); slScrMatSet(); … slSynch();` avec `slRparaInitSet`+`slMakeKtable`
  une fois à l'init. yabasanshiro VDP2 rewrite blog + Emulation General wiki
  (https://emulation.gametechwiki.com) — RBG0 "texture sur polygone" lazy en émulation,
  explique pourquoi Ymir tolère la RPT non transférée.
