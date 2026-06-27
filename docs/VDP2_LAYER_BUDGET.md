> ⚠️ PARTIELLEMENT OBSOLÈTE re: le sol RBG0 (état d'avant 2026-06-27). Le sol SHIPPE en BITMAP 512x256 8bpp (RBG0_BITMAP=1, commits 19768ca/41dd895), PROPRE sur HW : 2 banques — bitmap en A1, K-table en A0, fb en B0, RPT en B1+0x1ff00, B1 sinon LIBRE (pas de map). Les conclusions « sol=software/off », « neige présente », « poke CYC absent » (rbg0_commit_cyc EST dans l'arbre), « RDBS=0x8D » (c'est 0x0D), « ciel XOR sol » et « overlay incompatible » sont RENVERSÉES : l'overlay NBG3 = toggle L+R, un ciel cell peut cohabiter. Sol gaté à potato-0 + 1 joueur. La loi matérielle (§0/§2/§5) reste valable. Voir docs/VDP2_RBG0_CURRENT_STATE.md.

# VDP2 Layer Budget — Mimas (Doom Saturn) : ciel, sol, overlay et les 4 banques VRAM

> Document de décision. Auteur : architecte graphique Saturn (Mimas).
> Date : 2026-06-26. Public : Romain.
> Objet : répondre à 8 questions sur le conflit overlay-debug (NBG3) vs sol RBG0,
> cartographier l'usage VDP2, et décider de la stratégie (notamment le toggle écran-titre).
>
> **Synthèse en une phrase** : au-dessus du framebuffer obligatoire, le matériel ne permet
> que **le ciel HW *OU* le sol RBG0 texturé** — jamais les deux — et l'overlay debug (NBG3)
> ne peut **pas** cohabiter avec le sol RBG0 dans aucune config viable prouvée. La
> recommandation est donc un **toggle écran-titre** entre configurations, exactement comme
> Romain le propose.

---

## 0. Rappel des invariants matériels (la loi, pas un réglage)

- **VRAM = 512 Ko = 4 banques × 128 Ko**, en mode 4-banques (RAMCTL bits VRAMD=8 et
  VRBMD=9 tous deux à 1). Adresses : A0=`0x25E00000`, A1=`0x25E20000`, B0=`0x25E40000`,
  B1=`0x25E60000`. Plus **4 Ko de Color RAM (CRAM)**.
  Sources : `src/dg_saturn.cxx:250,257,261`, `docs/VDP2_ARCHITECTURE.md` (sec1.4),
  `SaturnRingLib/modules/sgl/INC/sl_def.h:945-948`.
- **Chaque banque expose 8 timings d'accès T0-T7** par cycle mémoire en résolution normale
  (320/352, ce que Mimas utilise). En hi-res (640/704) il n'y en a que 4. Programmés via
  CYCA0/CYCA1/CYCB0/CYCB1 (`0x25F80010`+). Source : `docs/VDP2_ARCHITECTURE.md` sec3.
- **B0 contient TOUJOURS le framebuffer du jeu** (NBG1, bitmap 8bpp, le rendu Doom).
  Non négociable. Il reste donc **exactement 3 banques** pour tout le reste.
  Source : `src/dg_saturn.cxx:183` (DOOM_VRAM=`0x25E40000`), `docs/VDP2_ARCHITECTURE.md` sec5.
- **La loi de la panne (manuel VDP2, verbatim)** : si la lecture requise par une couche
  n'est pas planifiée dans un timing de la banque qui contient réellement ses données,
  la lecture *n'a pas lieu* et la couche se corrompt en **traînées blanches horizontales
  ("snow")** — ce n'est **pas** un repli propre vers la transparence.
  C'est exactement la neige que Romain a vue sur hardware.
  Source : `docs/VDP2_ARCHITECTURE.md` sec2, `docs/VDP2_FLOOR_CONSOLIDATION.md:80-84`.

---

## 1. Pourquoi l'overlay et le sol se battent (le conflit B1, expliqué simplement)

L'overlay de debug est la couche **NBG3** (texte cellulaire de `SRL::Debug::Print` /
`SRL::ASCII`). **SRL la câble en dur dans la banque B1** : la table de tuiles (pattern-name)
à `VDP2_VRAM_B1+0x1E000` (=`0x25E7E000`), les cellules de police construites vers le bas
depuis `VDP2_VRAM_B1+0x1D000`. Il n'existe **aucune API SRL** pour la déplacer ailleurs —
c'est une constante de compilation.
Sources : `SaturnRingLib/saturnringlib/srl_ascii.hpp:19,67,99`, `srl_vdp2.hpp:1516-1526`.

Le sol RBG0 cellulaire a besoin de **3 lectures séparées**, chacune monopolisant **toute**
la fenêtre de timing d'une banque : pattern-name (map) + caractères (cells) + coefficient
(table K). Or **la map d'un BG en rotation DOIT être dans une banque B** (une banque A donne
`slScrAutoDisp ok=0` → RBG0 affamé → bandes écrasées, `src/dg_saturn.cxx:251-254`). B0 étant
le framebuffer (intouchable), **la map est forcée en B1** (`RBG0_MAP_VRAM=0x25E70000`,
`src/dg_saturn.cxx:257`). C'est précisément la banque de NBG3.

Deux niveaux de collision se superposent :

1. **Stockage** : la map RBG0 et la police/map NBG3 veulent la même région haute de B1.
2. **Cycles** : `rbg0_commit_ramctl()` écrit RDBS=`0x8D`, ce qui marque B1 comme banque de
   rotation "pattern-name" (code 2). Dès qu'une banque est banque de rotation, ses 8 timings
   sont **monopolisés** par la lecture rotation ; les lectures PN+char de NBG3 ne peuvent plus
   y être planifiées → le texte s'affame/se corrompt.
   Sources : `src/dg_saturn.cxx:251-257,266-273,1275-1287`.

C'est pourquoi le cycle pad-Y a **3 états** (`src/dg_saturn.cxx:266-273`) : la map RBG0 (B1)
et l'overlay NBG3 sont **mutuellement exclusifs**. Le diagnostic est auto-confirmant : si le
commit RDBS "prend", l'overlay NBG3 se corrompt. **On ne peut pas lire l'overlay pendant que
le sol HW est affiché.**

---

## 2. Cartographie VDP2 — les 4 banques × 8 cycles

> **STOCKAGE ≠ CYCLES.** Ce sont deux contraintes indépendantes :
> - **Stockage** : 128 Ko/banque, alloué linéairement.
> - **Cycles d'accès** : 8 slots T0-T7/banque. Un dot bitmap 8bpp coûte **2 lectures char**
>   (2 slots/8). Une lecture RBG0 (PN, char, ou K en mode per-dot) coûte **toute** la banque
>   (8 slots). Source : `docs/VDP2_ARCHITECTURE.md` sec3, `SaturnRingLib/saturnringlib/srl_vdp2.hpp:89-106,162-168,220-224`.

**Coût par dot en résolution normale** (`docs/VDP2_ARCHITECTURE.md:114-120`) :

| Type de couche | Lectures char/dot | Slots utilisés (sur 8) |
|---|---|---|
| Bitmap NBG 16-col (4bpp) | 1 | ~1 |
| **Bitmap NBG 256-col (8bpp)** — framebuffer & ciel | **2** | **~2** |
| Bitmap NBG 2048+/RGB direct | 4-8 | 4-8 |
| Cell NBG 256-col | 1 PN + 2 char | ~3 |
| RBG0 (chaque lecture : PN, char, K-per-dot) | toute la banque | **8 (banque entière)** |

### 2.a Occupation ACTUELLE — config shippée (VDP2_HW_SKY=1, RBG0_TEST=0)

C'est le build connu-bon, validé sur hardware. Sources : `src/dg_saturn.cxx:236,249`,
`docs/VDP2_FLOOR_CONSOLIDATION.md:8`.

| Banque | Stockage (contenu) | Cycles (slots/8) |
|---|---|---|
| **A0** `0x25E00000` | Ciel HW (NBG0 bitmap 8bpp 512×256, palette 1, prio 4) | ~2/8 |
| **A1** `0x25E20000` | *Libre / inutilisé pour l'affichage* | 0/8 |
| **B0** `0x25E40000` | **Framebuffer (NBG1 bitmap 8bpp 320×200, prio 6)** | ~2/8 |
| **B1** `0x25E60000` | **Overlay NBG3 texte debug** (police+map, prio 7) | ~1-2/8 |

→ 2 banques pleinement utilisées, **A1 entièrement libre**, beaucoup de slots libres partout.
Ciel + jeu + overlay coexistent. **Le sol est software (CPU).**
Sources : `src/dg_saturn.cxx:222,1436-1442,1453,1473`, `docs/VDP2_ARCHITECTURE.md:225-231`.

### 2.b Occupation en CONFIG SOL — RBG0 cellulaire (VDP2_HW_SKY=0, RBG0_TEST=1)

C'est le seul layout 4-banques propre pour un sol texturé. Sources :
`src/dg_saturn.cxx:250,255-261`, `docs/VDP2_ARCHITECTURE.md:230`.

| Banque | Stockage (contenu) | Cycles (slots/8) |
|---|---|---|
| **A0** `0x25E00000` | **Table K (coefficient)** RBG0 — libérée par le ciel software | banque rotation |
| **A1** `0x25E20000` | **Cellules (char)** RBG0 | banque rotation (8/8) |
| **B0** `0x25E40000` | **Framebuffer (NBG1)** — inchangé | ~2/8 |
| **B1** `0x25E70000` | **Map (pattern-name)** RBG0 — **ÉVINCE l'overlay NBG3** | banque rotation (8/8) |

→ Les **4 banques pleines**. Ciel passe en software, sol texturé sur RBG0, **overlay évincé.**
RDBS commit `0x8D` (`src/dg_saturn.cxx:1480`). **Snow toujours présent sur HW** car le poke
CYCxx (la moitié qui guérit réellement la neige selon la loi de la panne) **n'est pas
implémenté** (voir §7).

### 2.c CRAM (4 Ko, mode CRM16_2048 = 8 palettes de 256)

Occupation Doom (`src/dg_saturn.cxx:190,449-452`, `srl_vdp2.hpp:1487-1489,1498`) :
banque 0 ≈ palette police NBG3, banque 1 = PLAYPAL live, banques 2-7 = 6 light-banks
pré-assombris. → **Les 8 palettes sont effectivement toutes utilisées.** Conséquence directe :
mettre la table K en CRAM (CRKTE) risque de **collisionner avec la palette 8bpp** — non prouvé.

---

## 3. Peut-on déplacer l'overlay ? — 4 pistes, verdict pour chacune

### Piste A — NBG0 libéré par le ciel software peut-il accueillir l'overlay ?
**Réponse à la question 2 de Romain.** En config sol, le ciel HW (A0) passe en software,
mais **A0 n'est pas "libre"** : il reçoit immédiatement la **table K** du RBG0
(`RBG0_KTAB_VRAM=0x25E00000`, `src/dg_saturn.cxx:261`). Et même si on parlait de la *couche*
NBG0 (pas de la banque A0) : SRL câble l'overlay en **NBG3/B1**, pas NBG0 ; il n'y a pas d'API
pour le remettre sur NBG0. Re-pointer NBG3 vers A0 par appels SGL manuels
(`slCharNbg3/slPageNbg3/slMapNbg3` avec adresses A0) est *théoriquement* possible (A0 a ~126 Ko
libres une fois la table K placée au bas, et une table K en mode K_LINE/TwoAxis ne coûte
**0 cycle**), mais **non testé sur HW** : est-ce qu'un NBG3 partageant A0 avec la table K
enregistre un cycle pattern valide (`slScrAutoDisp>=0`) ? **À vérifier.**
**Verdict : possible en théorie, NON PROUVÉ, fragile (hérite du même bug de commit sans slSynch).**

### Piste B — Slots libres de la banque framebuffer (B0) ?
Le framebuffer 8bpp utilise seulement ~2/8 slots → **6 slots de cycle libres en B0.** Un NBG3
ne demande qu'~1 slot. **MAIS** : un framebuffer 512×256×8bpp = `0x20000` octets = **exactement
128 Ko = toute la banque B0 en STOCKAGE.** B0 est donc **plein en stockage** malgré ses slots
de cycle libres. La police+map de l'overlay (~`0x3000` octets) n'y rentre pas.
Sources : finding 2 openQuestion, `docs/VDP2_ARCHITECTURE.md` sec3.
**Verdict : NON. Cycles libres mais zéro stockage libre. (NB : notre framebuffer est 320×200,
à confirmer s'il consomme réellement tout B0 ou s'il reste une marge de stockage — à vérifier.)**

### Piste C — RBG0 bitmap (sans map) libère-t-il B1 pour l'overlay ?
Un RBG0 **bitmap** supprime la lecture pattern-name → **pas de banque map** → B1 redevient
libre pour NBG3. Coût : on perd la sélection de flat par tuile (image statique, 1 banque entière
de cellules pour un bitmap 8bpp 512×256), et il faut re-uploader le bitmap à chaque changement
de flat dominant/lumière → **rejeté pour la bande passante** (`docs/VDP2_ARCHITECTURE.md:349`).
SRL ne supporte que conteneurs 512×256/512×512 pour un bitmap RBG0 (`srl_vdp2.hpp:244-307`).
**Verdict : libère B1 et ramène l'overlay, MAIS sol statique sans flats dynamiques — compromis
visuel lourd, à évaluer si un sol mono-texture est acceptable. À vérifier.**

### Piste D — Coefficient (table K) en CRAM via CRKTE ?
CRKTE (RAMCTL bit15) déplace la table K dans la CRAM → libère une **banque VRAM**. C'est la
**seule** piste vers "ciel HW + sol RBG0 texturé sur 4 banques". **MAIS** :
1. CRKTE libère **A0**, pas B1. La **map RBG0 reste en B1 et évince toujours l'overlay**
   (config F du tableau §4). Donc même avec CRKTE, **overlay + sol restent incompatibles.**
2. Les 8 palettes CRAM sont déjà occupées → **risque de collision K-vs-palette 8bpp**, non prouvé.
3. SRL/SGL n'exposent **aucun** chemin K-en-CRAM (`srl_vdp2.hpp:1202-1244` alloue toujours en
   VRAM) → il faudrait un **poke registre VDP2 brut** hors SRL.
Sources : `docs/VDP2_ARCHITECTURE.md:140-145,295-296`, `docs/VDP2_FLOOR_CONSOLIDATION.md:545-547`,
finding 2/5.
**Verdict : seule voie "ciel+sol" sur 4 banques, mais NE résout PAS l'overlay (B1 toujours pris),
et NON PROUVÉ (collision CRAM). À vérifier.**

**Conclusion §3** : aucune piste ne fait cohabiter **overlay + sol RBG0** de façon prouvée. La
seule qui ramène l'overlay avec un sol (Piste C, RBG0 bitmap) sacrifie les flats dynamiques.

---

## 4. Ce qu'on peut raisonnablement combiner — MATRICE de configurations

Légende : **fb** = framebuffer (NBG1, B0, obligatoire). **K** = table coefficient.
**cells/map** = char/pattern-name RBG0. "—" = libre.

| Config | A0 | A1 | B0 | B1 | CRAM | Overlay ? | Verdict |
|---|---|---|---|---|---|---|---|
| **A — fb + ciel HW** (SHIP) | ciel HW | — | fb | — | palettes | ✅ (rentre en B1) | **Connu-bon, validé HW.** Sol software. |
| **D — fb + ciel HW + overlay** (dev) | ciel HW | — | fb | overlay NBG3 | palettes | ✅ | **Build dev actuel.** Ciel+jeu+overlay OK. Sol software. |
| **B — fb + sol RBG0 cell + ciel SW** | K | cells | fb | map | palettes | ❌ (map en B1) | Seul layout sol texturé propre. **Snow HW** (poke CYC absent). Overlay évincé. |
| **E — fb + sol RBG0 cell + overlay** | K | cells | fb | map **ET** overlay | palettes | ❌ INFAISABLE | Map et overlay veulent B1. **Impossible.** |
| **F — fb + sol RBG0 cell, K en CRAM** | — (libre) | cells | fb | map | K + palettes | ❌ (map en B1) | CRKTE libère A0, **pas B1**. Overlay toujours évincé. Non prouvé. |
| **G — fb + ciel HW + sol RBG0 cell, K en CRAM** | ciel HW | cells | fb | map | K + palettes | ❌ | **Seule voie "ciel+sol"** (4 banques OK). Mais overlay évincé + collision CRAM non prouvée. |
| **C — fb + sol RBG0 bitmap + ciel HW** | ciel HW | bitmap+cells RBG0 | fb | — ou 2e lecture | K en CRAM ? | ✅ si B1 libre | Sol **statique** (pas de flats dynamiques). Ramène l'overlay. À évaluer. |
| **H — fb + ciel HW + "sol gradient" (back-screen)** | ciel HW | — | fb | overlay NBG3 | palettes | ✅ | Sol non texturé (dégradé ombré d32xr), **0 banque**, garde ciel+overlay, capte ~+20-33% REC. |

**Ciel + X** (X sans nouvelle banque) : nuages parallaxe NBG2 (eye-candy, +0 REC), HUD en
couche NBG, color-math (flash de dégâts, fondus, brouillard). **Jamais** un sol texturé (pas de
banque). Source : finding 5 openQuestions.

**Sol + X** (X sans nouvelle banque) : éclairage par distance per-line K_LINECOL, choix dynamique
du flat dominant. **Jamais** le ciel HW ni l'overlay. Source : finding 5 openQuestions.

**Max simultané utile (1 joueur)** = **framebuffer + EXACTEMENT UN de {ciel HW, sol RBG0 texturé}
+ (overlay debug seulement quand le sol est OFF).** "Ciel ET sol" n'est atteignable que via CRKTE
non prouvé (config G), et **même là sans overlay.** L'alternative sans banque (sol dégradé
back-screen/line-color, config H) est la seule façon d'approcher "ciel + sol + overlay" ensemble.

---

## 5. Comment les vrais jeux Saturn saturent le VDP2 — exemples concrets

### Les "tricks" du matériel
- **Coefficient en CRAM (CRKTE)** : place la table K dans la moitié haute de la CRAM
  (`0x100800-0x100FFF`, CRAM mode 1) au lieu d'une banque VRAM → la lecture coefficient
  per-line/per-dot ne consomme **plus de slot VRAM** → libère une banque entière. C'est ce qui
  laisse un sol en rotation cohabiter avec plusieurs couches de scroll.
  Source : VDP2 User's Manual ch.3.4 / ch.6.4 (`docs.exodusemulator.com/.../hard/vdp2/hon/p03_40.htm`,
  `.../p06_40.htm`). **Incompatibilité** : CRKTE exige CRAM mode 1 ; l'Extended Color Calculation
  (blending étendu) exige CRAM mode 0 → mutuellement exclusifs (SegaXtreme "VDP2 questions" p.2).
- **RBG0 bitmap** : supprime la lecture pattern-name → 2 banques au lieu de 3 (cells/bitmap + K),
  au prix d'un sol mono-image. Source : `docs/VDP2_ARCHITECTURE.md:349`, `srl_vdp2.hpp:269`.
- **Réduction (ZMCTL)** : NBG0/NBG1 seulement ; coût ×1 (none) / ×2 (1/2) / ×4 (1/4) sur les
  lectures char. 1/2 → 16/256 couleurs only ; 1/4 → 16 couleurs only ; active la réduction sur
  NBG0 désactive NBG2 (1/4 désactive NBG2+NBG3). Source : VDP2 User's Manual ch.5
  (`.../hard/vdp2/hon/p05_20.htm`).
- **Back-screen + line-color comme "couches gratuites"** : le back screen (une couleur per-line
  derrière tout, 1 lecture courte/ligne) et le line color screen (couleur per-line mélangeable)
  coûtent **0 banque de cellules**. **Sonic R** anime un line color screen sur le RBG0 pour les
  reflets d'eau ondulants. Sources : VDP2 Manual ch.7 (`.../p07_10.htm`, `.../p07_20.htm`),
  davidgamizjimenez "Sega Saturn al límite" (domaine désormais mort, via snippets de recherche),
  tcrf.net Proto:Sonic_R.
- **Ordre d'allocation RBG0-avant-NBG** : RBG0 réclame des banques entières (8 cycles), donc
  toutes les libs (SGL, SRL, Jo) lui donnent ses banques dédiées **d'abord**, puis casent les NBG
  dans les slots restants. SRL : RBG0 cell réserve les 8 cycles d'une banque (A0→A1→B0→B1),
  RBG0 map force A0 ; NBG par profondeur (16-col=1, 256-col=2, RGB=4 cycles).
  Source : `SaturnRingLib/saturnringlib/srl_vdp2.hpp:162-185,220-224`.

### Jeux et moteurs (5 couches max = NBG0+NBG1+NBG2+NBG3+RBG0)
- **Sonic R** (Travellers Tales) : RBG0 = sol/piste "3D infini" (cells 8×8, plan 128×128) +
  NBG0 background + line color screen animé = sol rotation + scroll + couche gratuite simultanés.
  Source : davidgamizjimenez (snippets), tcrf.net.
- **Panzer Dragoon Saga / Sonic 3D Blast / Street Fighter Alpha/Zero 2** : cités par l'équipe
  de l'émulateur **Ymir 0.1.5** — leurs graphismes se corrompaient tant que l'allocation de
  banques VRAM et les cycles d'accès n'étaient pas honorés exactement. **Preuve concrète** que
  les jeux shippés allouent les banques serré et dépendent du scheduler cycle-pattern.
  Source : `emu-land.net/en/news/ymir_v015`.
- **Panzer Dragoon II Zwei** : "champ sans fin" (plan rotation RBG0) pour l'eau/perspective.
- **Burning Rangers** : dessine les explosions sur VDP1 demi-res, DMA le framebuffer VDP1 dans
  la VRAM VDP2, NBG2 composite/blend → la VRAM 512 Ko sert aussi de scratch cross-VDP.
  Source : Beyond3D / sega-16 (via snippets).

### Fichiers de référence on-disk (dumps réels de cycle/RAMCTL)
- **Jo Engine** (`../DoomJo/joengine/jo_engine/vdp2.c:330-360`, `vdp2_malloc.c:62-80`) : RBG0 via
  SGL haut-niveau, **K-table+rotation-table épinglées en A1**, cells RBG0 en A0, map en B0, commit
  par `slScrAutoDisp` + **slSynch chaque frame**. Jo fonctionne *parce qu'il slSynch* → son RAMCTL/
  CYC shadow atteint toujours la puce. **Mimas ne peut pas copier ça** (pas de slSynch).
  RAMCTL Jo NOSGL connu-bon = `0x1327`. Source : `docs/VDP2_ARCHITECTURE.md:177,409-414`.
- **SGL canonique** (`SaturnRingLib/modules/sgl/INC/sl_def.h:952,964,975,979`) : CGR0_RAM=A0,
  KTBL0_RAM=A1, RBG0_MAP=B0, CGN01_RAM=B1. Toute la pile assume ces valeurs.
- **SlaveDriver-Engine (SCL)** (`saturn-refs/SlaveDriver-Engine/INITMAIN.C:262-285`, `PLAX.C:94-108`)
  : **table de cycle écrite à la main** `{0xeeee,0xeeee, 0xeeee,0xeeee, 0x44ee,0xeeee, 0x44ee,0xeeee}`
  via `SCL_SetCycleTable`, A0=table K RBG0, A1=char RBG0. `0xE`=slot idle/CPU, `0x4`=lecture
  pattern-name/char ; les banques RBG0 (A0/A1) entièrement consommées.
- **Slot codes (manuel)** : `0x0-3`=PN NBG0-3, `0x4-7`=char NBG0-3, `0x8-B`=interdit, `0xC/D`=
  vertical cell-scroll NBG0/1, `0xE`=slot CPU, `0xF`=pas d'accès. Source : VDP2 Manual ch.3
  (`.../hard/vdp2/hon/p03_35.htm`), `wiki.yabause.org/index.php5?title=VDP2`.

> **Note importante** : *aucune* des 4 références on-disk n'écrit un `slScrCycleSet` pour une
> coexistence **bitmap-Doom + RBG0** ; la seule table cycle littérale (SlaveDriver) est pour un
> RBG0 *tilemap*, pas un framebuffer 8bpp. Les nibbles exacts pour "NBG1 8bpp 512×256 en B0 +
> sol RBG0" restent **à dériver/valider**, pas à copier. **À vérifier.**

---

## 6. Nos cas d'usage Mimas — quelles configs valent le coup

Rappel clé : **le gain REC (+20-33% fps) vient du SKIP du sol software** (on écrit l'index 0
sur le span du flat dominant et on ne le dessine pas), **PAS du RBG0 lui-même**. Donc l'axe réel
est **qualité-du-sol vs coût-en-banques**, et il existe un sol à ~0 banque qui capte ~le même gain.
Source : `docs/VDP2_ARCHITECTURE.md:355-364`, finding 1 (CAPACITY VERDICT).

| Config | Sol | Ciel | Overlay | Gain REC | Coût/risque | Verdict Mimas |
|---|---|---|---|---|---|---|
| **A/D (SHIP)** | software | HW | ✅ | 0 | aucun (validé HW) | **Baseline à garder.** |
| **H (gradient back-screen)** | dégradé ombré, 0 banque | HW | ✅ | ~+20-33% | sol non texturé, sans flats/hauteurs | **La meilleure piste "tout-en-un"** — garde ciel+overlay, capte le gain, sans commit RAMCTL/CYC. À évaluer en priorité. |
| **B (sol RBG0 cell)** | texturé | software | ❌ | ~+20-33% | snow HW (poke CYC absent), overlay évincé, fragile | À débloquer seulement si on résout le commit CYC. |
| **G (ciel+sol, K en CRAM)** | texturé | HW | ❌ | ~+20-33% | non prouvé (collision CRAM), overlay évincé | Recherche, pas pour ship. |
| **C (sol RBG0 bitmap)** | statique mono-texture | HW | ✅ | ~+20-33% | perd flats dynamiques | À évaluer si le sol statique est acceptable. |

**Recommandation d'usage** : pour un sol qui garde **ciel + overlay**, la config **H** (sol
dégradé style d32xr, 0 banque) est la plus rentable et la moins fragile. Le sol RBG0 texturé
(B/G) reste bloqué tant que le poke CYC n'est pas écrit et validé HW.

---

## 7. Gestion des conflits + recommandation toggle écran-titre

### L'état réel du code (vérifié contre l'arbre, 2026-06-26)
- Build shippé/HEAD : `VDP2_RBG0_TEST=0` (`src/dg_saturn.cxx:236`), `VDP2_HW_SKY=1` (`:249`) =
  ciel HW + sol software + RBG0 off. **Validé HW.**
- **DISCREPANCE confirmée par grep** : `VDP2_FLOOR_CONSOLIDATION.md` section 0 prétend que
  `rbg0_commit_cyc()` (le poke CYCxx direct) et le classifieur `sat_sky_px`/`sat_floor_px` dans
  `r_plane.c` sont "câblés/compilés" dans l'arbre. **Ils n'y sont PAS.** `dg_saturn.cxx` n'a que
  `rbg0_commit_ramctl()` (moitié RDBS, `:1278-1287`) ; **aucun** `rbg0_commit_cyc`, **aucune**
  écriture `0x25F80010`, **aucun** poke CYCA0. `r_plane.c` n'a **aucun** `sat_sky_px`/`sat_floor_px`.
  Ces symboles n'apparaissent que dans les docs et un sample SRL non lié. **Le poke CYC — la
  moitié qui guérit réellement la neige selon la loi de la panne — est TOUJOURS ABSENT.** La prep
  "câblée" a été revertée ou n'a jamais atterri. → corriger la mémoire + le doc consolidation en
  "planifié, PAS dans l'arbre".

### Pourquoi un re-commit mid-game est à proscrire
Changer le **flag** de skip software (`sat_vdp2_floor` on/off) est cheap et sûr. Mais changer le
**layout de banques** (ciel↔sol, réassigner A0) demande un re-commit RAMCTL/CYC par frame, qui est
**fragile et non prouvé** : SGL ne flushe RAMCTL/CYCxx que dans `slSynch`, que Mimas n'exécute
**jamais** (slSynch cape les fps à ~7-12 et son tick driver son rend muet le SFX direct-SCSP ;
testé HW = pire, reverté). Le seul chemin de commit viable = **pokes registres directs**, et le
poke CYC reste non écrit/non prouvé. Source : `docs/VDP2_FLOOR_CONSOLIDATION.md:310-318,150-216`,
`src/dg_saturn.cxx:1270`.

### Recommandation : TOGGLE ÉCRAN-TITRE (exactement ce que propose Romain)
Puisque :
1. l'overlay NBG3 **ne peut cohabiter avec le sol RBG0 dans AUCUNE config prouvée** (§1, §3, §4) ;
2. un re-commit RAMCTL/CYC **en cours de partie est fragile/non prouvé** (ci-dessus) ;

→ la stratégie correcte est de **choisir la configuration AVANT d'entrer en jeu**, depuis
l'écran-titre, et de committer le layout **une seule fois** (au pire par map, au chargement —
jamais par frame). C'est précisément la proposition de Romain, et elle est validée par la loi
matérielle.

**Configs que le toggle écran-titre proposerait** (par ordre de robustesse) :

1. **"Classic" (A/D)** — Ciel HW + sol software + **overlay debug dispo**. Connu-bon, validé HW.
   *Défaut recommandé.*
2. **"Fast floor" (H)** — Ciel HW + sol dégradé back-screen (0 banque) + overlay dispo + gain REC.
   *À implémenter/évaluer — meilleure piste sans fragilité.*
3. **"Textured floor" (B)** — Sol RBG0 texturé + ciel software, **overlay OFF**. *À débloquer
   seulement après écriture+validation HW du poke CYC ; sinon snow.*

Granularité de bascule réaliste = **par map au chargement** (re-commit des banques une fois),
**pas par frame**. Source : `docs/VDP2_FLOOR_CONSOLIDATION.md:310-318`.

### Et si on voulait quand même overlay + sol ?
La seule façon prouvée de garder l'overlay AVEC un "sol" amélioré est la config **H** (sol
dégradé, pas RBG0) : elle ne touche pas B1, donc l'overlay reste. Avec le **sol RBG0 texturé**,
il faut accepter **overlay OFF** (ou un sol RBG0 *bitmap*, config C, qui libère B1 mais perd les
flats dynamiques — à évaluer). Aucune piste CRKTE ne sauve l'overlay (la map reste en B1).

---

## 8. Verdict & prochaine étape concrète

**Verdicts** :
- L'overlay NBG3 et le sol RBG0 se battent pour la **banque B1** (map rotation forcée en B-bank,
  B0 pris par le framebuffer) — conflit de stockage **et** de cycles. Inévitable avec un RBG0
  cellulaire.
- **Aucune** config prouvée ne fait cohabiter overlay + sol RBG0 texturé. CRKTE ne sauve pas
  l'overlay. RBG0 bitmap le sauverait mais tue les flats dynamiques.
- Le sol RBG0 texturé **ne ship pas** en l'état : le poke CYCxx (cure de la neige) est **absent
  du code**, et le commit mid-game via slSynch est poison.
- Le **gain de perf vient du skip du sol software**, pas du RBG0 — donc un **sol dégradé à 0
  banque (config H)** capte ~le même gain en **gardant ciel + overlay**.
- **Le toggle écran-titre proposé par Romain est la bonne décision** : config choisie avant le
  jeu, commit unique, zéro re-commit fragile mid-game.

**Prochaine étape concrète (sans code maintenant)** :
1. **Corriger les docs/mémoire** : marquer `rbg0_commit_cyc` + `sat_sky_px/sat_floor_px` comme
   "planifié, PAS dans l'arbre" dans `VDP2_FLOOR_CONSOLIDATION.md` et la mémoire associée.
2. **Garder le build shippé** (config A/D) comme défaut — il est validé HW.
3. **Prototyper la config H** (sol dégradé back-screen/line-color, 0 banque) : c'est le meilleur
   rapport gain/risque et elle préserve ciel+overlay. À chiffrer côté REC.
4. **Spécifier le toggle écran-titre** : 3 entrées (Classic / Fast floor / Textured floor),
   commit du layout au choix, par map au chargement.
5. **À vérifier avant tout sol RBG0 texturé** : (a) le poke CYCxx "prend"-il sans slSynch comme
   FBCR/PTMR le font pour VDP1 (validé 2026-06-16) ou l'ISR vblank SGL le clobbe-t-il chaque
   frame ? (b) CRKTE coexiste-t-il avec la palette 8bpp en CRAM ? (c) le framebuffer 320×200
   laisse-t-il du stockage en B0 ? — toutes ces questions sont **non résolues**.

---

### Sources principales
- Code Mimas : `src/dg_saturn.cxx` (lignes citées en ligne), `core/r_plane.c`.
- Docs repo : `docs/VDP2_ARCHITECTURE.md`, `docs/VDP2_FLOOR_CONSOLIDATION.md`.
- SRL/SGL : `SaturnRingLib/saturnringlib/srl_vdp2.hpp`, `srl_ascii.hpp`,
  `SaturnRingLib/modules/sgl/INC/sl_def.h`.
- Références : `../DoomJo/joengine/jo_engine/vdp2.c` + `vdp2_malloc.c`,
  `saturn-refs/SlaveDriver-Engine/INITMAIN.C` + `PLAX.C`.
- Web : VDP2 User's Manual (`docs.exodusemulator.com/.../hard/vdp2/hon/`), `wiki.yabause.org`,
  `emu-land.net/en/news/ymir_v015`, `jo-engine.org`, tcrf.net Proto:Sonic_R,
  davidgamizjimenez "Sega Saturn al límite" (domaine mort, via snippets).
