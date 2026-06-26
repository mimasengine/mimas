# VDP1 4bpp vs 8bpp — étude du passage 8bpp → 4bpp pour Mimas

> Document de décision. Date : 2026-06-26. Public : Romain.
> Objet : étudier le passage des textures VDP1 de 8bpp (256-couleurs color-bank, l'état
> shippé) vers 4bpp (16-couleurs) — ce qu'on y gagne, ce que ça ouvre, ce qu'on y perd, et
> comment maximiser le passage.
> Méthode : fan-out 5 dimensions (HW des modes 4bpp, réalité du code, faisabilité de la
> quantization sur le WAD réel, ports Saturn de référence, migration du lighting) +
> vérification adverse des affirmations porteuses. Companion de
> [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) et [`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md).

> **Vocabulaire Doom (load-bearing pour ce doc).** Un **flat** = la texture **64×64** de
> **sol/plafond** (`FLOOR4_8`, `NUKAGE1`, `CEIL3_5` ; brute, 4096 octets d'index PLAYPAL,
> entre `F_START`/`F_END`). Une **texture (de mur)** = un composite de patches via
> `TEXTURE1`/`PNAMES`, taille variable (`STARTAN3`, `TEKWALL1`). Quand ce doc dit « 4bpp pour
> les flats », il parle des **sols/plafonds**, jamais des murs.

---

## 0. Synthèse en une phrase

Le 4bpp n'est **ni** un tweak gratuit **ni** la catastrophe qu'on croit : son **seul gain**
est la VRAM (texel ½ octet), son **seul coût irréductible** est la **quantization destructive
256→16 couleurs par texture**, et tout le reste (lighting, flash) dépend d'un **choix de
sous-mode** que l'analyse naïve rate. **Recommandation : ne PAS migrer les murs ; faire naître
le sol VDP1 directement en 4bpp pour ses flats** — c'est le seul endroit où le ½-octet/texel
compte (VRAM serrée), où la quantization est indolore (flats ~0 RMS) et où il n'y a pas de
seam (surface 100%-VDP1, jamais software).

> **⚠️ Cadrage (Romain, 2026-06-26) — le 4bpp ne touche PAS le problème VDP1 #1.** Le levier
> 4bpp est **VRAM/stockage** ; le problème dur du VDP1 est l'**overdraw** (itération des pixels
> hors-écran d'un quad). Ils sont **orthogonaux** : le 4bpp ne réduit ni l'itération, ni le
> budget commandes, ni le décrochage CPU/VDP1. C'est un **enabler** du plan sol VDP1 (il lui
> libère la VRAM), **pas** un fix de l'overdraw. Voir §9.

---

## 1. État shippé (vérité-terrain, vérifiée contre l'arbre 2026-06-26)

- Les murs VDP1 sont dessinés en **color mode 4 = `CL256Bnk` = 8bpp 256-couleurs color-bank** :
  `cmd[2]=0x00E0` (sans window) / `0x04E0` (Window_In), champ color-mode `(0xE0>>3)&7 = 4`.
  Sources : `src/dg_saturn.cxx:2042,2099,2117,2139`.
- Le **texel mur = index PLAYPAL 8 bits BRUT**, jamais re-baké. Source :
  `src/dg_saturn.cxx:1969-1982`.
- **Lighting = banques CRAM pré-ombrées sélectionnées par `CMDCOLR`** : bank 1 = PLAYPAL live
  full-bright (== palette NBG1 software), banks 2-7 = PLAYPAL pré-ombré par 6 niveaux de
  colormap. `colr = wlight_bank_lut[L]<<8`. Sources : `src/dg_saturn.cxx:1704-1751,1780-1785`.
- **Flash dégâts/pickup + fade de transition = re-tint des 7 banques CRAM**
  (`wtex_rebuild_banks`, `dg_fade_bake`) — **pas** de re-bake de texture. Le colour-offset
  VDP2 (`COAR/COAG/COAB`) est recommandé mais **pas construit**. Sources :
  `src/dg_saturn.cxx:1791-1808,1823-1854`.
- **Cache textures murs = 22 slots** : 16 narrow (16KB, ≤128²) + 6 wide (32KB, ≤256×128) dans
  448KB @ `0x25C05000`. Le passage RGB555→8bpp avait déjà **doublé** les slots (11→22) et
  « *turned VDP1 from VRAM- and fill-starved into having headroom* ». Sources :
  `src/dg_saturn.cxx:1726-1735`, `docs/VDP1_ARCHITECTURE.md:144-188`.
- **Murs proches (span > 480px) → SOFTWARE** : colonnes CPU dans NBG1 par-dessus (layer
  inversion). Un même mur peut être **partie VDP1, partie software**. Source : `core/r_segs.c`
  (fallback magnification).
- **Sol/plafond aujourd'hui = software** (visplanes NBG1). Le plan sol VDP1
  ([`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md)) n'émet **aucun** quad sol (stub) et exige un
  **cull VRAM « obligatoire »** + une banque commande F + un cache flat 32KB.

---

## 2. LE point que l'analyse naïve rate : il y a DEUX modes 4bpp

C'est la correction la plus importante de l'étude (issue de la passe de vérification adverse).
Le champ color-mode de `CMDPMOD` (bits 5:3) distingue **deux** modes 16-couleurs radicalement
différents (`SaturnRingLib/modules/sgl/INC/sl_def.h:372-394`, confirmé par la réf SlaveDriver
`saturn-refs/SlaveDriver-Engine/SPR.H:92-97`) :

| Mode | Valeur | Où vit la palette | `CMDCOLR` = | Préserve le modèle Mimas ? |
|---|---|---|---|---|
| **CL16Bnk** (mode 0) | `0x0000` | **CRAM** (fenêtre de 16) | sélecteur de banque CRAM (`palette<<4`) | **OUI** — analogue 4bpp exact du `CL256Bnk` actuel |
| **CL16Look** (mode 1) | `0x0008` | **VRAM VDP1** (CLUT 16×16 bits = 32 o) | pointeur d'adresse CLUT (`addr>>3`) | NON — casse le lighting CRAM |

En **`CL16Bnk`**, le texel 4 bits forme les bits **bas** d'une fenêtre de 16 couleurs en CRAM,
`CMDCOLR` fournit les bits hauts → **même indirection CRAM que le 8bpp actuel**. Il suffit de
remplacer `<<8` par `<<4` et `0x20` par `0x00` dans `cmd[2]`. C'est ce que fait SRL pour
`Paletted16` (`srl_scene2d.hpp:473-476`, `srl_cram.hpp:79` : 128 banques de 16 en CRAM). Le
flash reste un re-tint CRAM, le texel reste un index brut.

> **Conséquence (vérifiée) :** l'affirmation « le 4bpp casse le lighting ET le flash » est
> **FAUSSE pour `CL16Bnk`** — elle ne vaut que pour `CL16Look`. Le « joyau » (lighting + flash
> gratuits par CRAM) **survit** en `CL16Bnk`. Le débat « il faut construire le colour-offset
> VDP2 AVANT le 4bpp » s'effondre avec : le flash n'a jamais été un argument contre le 4bpp.

---

## 3. Ce qu'on GAGNE — uniquement de la VRAM (chiffré exact, vérifié au bit)

Le 4bpp ne change **rien** au débit de fill : l'écriture framebuffer reste **1 mot 16 bits par
pixel** quel que soit le format source (`TVMR=0` → framebuffer 16bpp, `src/dg_saturn.cxx:2422`).
Le limiteur réel du VDP1 (overdraw + budget commandes + slots de cache) est **inchangé**. Le
**seul** gain HW est le packing texel : **0.5 o/texel** (4 texels/mot 16 bits) vs **1.0 o/texel**
en 8bpp (`srl_vdp1.hpp:23-44`, `GetSizeShifter` : Paletted16 → shift 3, Paletted256 → shift 2).

Géométrie actuelle 8bpp (`src/dg_saturn.cxx:1726-1735`) : 16 narrow×16KB + 6 wide×32KB =
**448KB = 22 slots**. Deux scénarios 4bpp, arithmétiquement exacts (128²@4bpp = 8192 o tient
**pile** dans 8KB ; 256×128@4bpp = 16384 o pile dans 16KB — zéro gaspillage) :

| Scénario | Layout 4bpp | Effet |
|---|---|---|
| **A — 22 slots demi-taille** | 16 narrow×8KB + 6 wide×16KB = 224KB | **libère exactement 224KB** |
| **B — doubler les slots** | 32 narrow×8KB + 12 wide×16KB = 448KB | **44 slots**, VRAM constante |

**Le scénario B attaque un problème déjà résolu.** Les 22 slots ont déjà éliminé l'essentiel de
la famine (« sky-through-walls »). Doubler les slots n'apporte donc presque rien aux murs. **Le
seul gain qui compte est le scénario A** : les 224KB libérés.

---

## 4. Ce que ça OUVRE — le sol VDP1 sans le « cull obligatoire »

C'est le vrai potentiel. Le plan sol VDP1 ([`VDP1_WORLD_PLAN.md §7.2`](VDP1_WORLD_PLAN.md))
exige une banque commande F dédiée (16KB) + un cache flat (32KB) = **48KB**, dans une VRAM
512KB si serrée qu'il faut un **cull obligatoire** qui *réduit le cache mur de 448→416KB*
(perd 2 slots narrow, surveille `bk` row 16).

Avec le 4bpp en scénario A, **les 224KB libérés rendent ce cull inutile** : on garde les 22
slots murs pleins, on ajoute la banque F + le cache flat, et il reste de la marge pour :

1. la **2e banque commande multijoueur** (+8KB) ;
2. éventuellement des **world-sprites sur VDP1** (mais là le bloqueur est l'occlusion sans
   Z-buffer, pas la VRAM).

Et surtout : **les flats sont le cas idéal 4bpp** (§6). Un flat 64×64 passe de 4KB à **2KB** →
le cache flat passe de 32KB à 16KB, doublant ses slots **là où la VRAM était précisément
tight**.

---

## 5. Ce qu'on PERD

### 5.1 Le coût irréductible : quantization 256→16 par texture
Quel que soit le sous-mode, 16 couleurs/texture impose un **re-bake destructif** de chaque
texture vers une sous-palette de 16. C'est l'inverse exact du joyau actuel (« texel = index
PLAYPAL brut, jamais re-baké »). **C'est ça, le vrai changement de rendu** — pas le flash.

### 5.2 Le seam software/VDP1 (confirmé, sans fix gratuit)
Un mur proche bascule **moitié-software** (colonnes CPU NBG1, 256 couleurs PLAYPAL via
`core/r_segs.c` magnification) **moitié-VDP1**. En 8bpp, les deux moitiés lisent la même CRAM →
couture invisible (bit-match exact, bank 1 == NBG1). En 4bpp, la moitié VDP1 lit une
sous-palette 16-couleurs quantifiée → **escalier de couleur à la jointure**, qui *clignote*
frame à frame (le seuil 480px bascule). Idem sol RBG0 dominant (256) vs strips sol VDP1 (16).
**C'est l'argument décisif pour garder les murs en 8bpp** : tout ce qui peut basculer software
ne doit pas passer 4bpp. (Le sol VDP1, lui, ne bascule jamais software — d'où la cible flats.)

### 5.3 Le budget fenêtres CRAM (nuance précisée par la vérif)
Même en `CL16Bnk` qui « préserve le mécanisme », un piège subsiste : en 8bpp, **7 banques
globales** de 256 servent *toutes* les textures (elles partagent la PLAYPAL). En 4bpp, chaque
sous-palette de 16 a besoin de **ses 7 fenêtres pré-ombrées**. CRAM = 2048 entrées → 128
fenêtres → ~**18 sous-palettes distinctes** avec lighting 7-niveaux complet. Au-delà → re-bake
CRAM par frame (ce que le 8bpp évitait). *Mitigeable* en partageant les sous-palettes entre
textures de même famille — mais c'est une **contrainte nouvelle, réelle**. (NB : la CRAM n'est
pas « saturée à 100% » comme une lecture l'a prétendu — la police NBG3 est 4bpp/16-col, la
moitié haute de la banque 0 est libre.)

### 5.4 La référence qui va dans l'AUTRE sens (PowerSlave / SlaveDriver)
Donnée forte et **confirmée en vérification**. Le moteur Lobotomy SlaveDriver (PowerSlave,
Duke3D Saturn, Quake Saturn) dessine ses **murs ET sols en 16bpp RGB DIRECT** (`COLOR_5`,
classe `TILE16BPP`, `saturn-refs/SlaveDriver-Engine/WALLS.C:1082`, `PIC.C:84-99`) + **Gouraud
RGB par-sommet** (`greyTable[]` RGB555, `UTIL.C:108-142`, `DRAW_GOURAU`). Le 4bpp (`COLOR_0/1`)
n'y sert **que** pour les fontes/UI. Le FPS Saturn haut de gamme a **dépensé** 2 octets/texel
pour pouvoir lighter en RGB — l'**opposé** du 4bpp. Le modèle Mimas (8bpp index brut + banque
CRAM par niveau) est une **3e voie**, choisie parce que le Gouraud VDP1 décale le *code*
d'index, pas le RGB. Doom Saturn officiel (Rage/Bagley 1997), lui, est full-software (Carmack
a refusé le quad HW) → choppy. **Personne ne va vers le 4bpp pour les murs d'un FPS Saturn.**

---

## 6. Faisabilité mesurée (sur le WAD réel `cd/data/DOOM1.WAD`)

Mesure réelle (parse de l'IWAD : 1207 lumps, 125 textures, 54 flats ; quantization k-means +
Lloyd, RMS pondéré-fréquence en RGB 0-255 ; scripts `scratchpad/wad_quant.py` / `wad_light.py`) :

- **Flats : 100% acceptables (54/54), 30/54 déjà natifs ≤16 couleurs, RMS median 0.0.** Le
  4bpp est **gratuit** pour le sol. C'est le résultat le **plus solide** de l'étude.
  Exemples : `FLAT1`=6 couleurs, `CEIL3_5`=6, `NUKAGE1`=10, `FLOOR7_1`=10.
- **Murs : 91% « acceptables » (RMS ≤14), 0% catastrophique — mais classé « uncertain » en
  vérification.** La métrique pondérée-fréquence *masque* des couleurs-accent écrasées jusqu'à
  ~110/255 sur ~14 textures (famille tech), le RMS-RGB n'est pas perceptuel, et surtout le
  k-means produit des index PLAYPAL **arbitraires**, pas une tranche 16-alignée compatible
  `CL16Bnk`. Donc **« 91% quantizent » ≠ « 91% livrables en 4bpp »**.
- **Les casseurs forment une famille homogène** : écrans/ordinateurs multi-teintes (`TEKWALL1-5`,
  `COMPUTE1`, `COMPTALL`, `EXITDOOR`, `PLANET1`, RMS 15-21) — des murs très vus en E1.
  `STARTAN3` (RMS 12.5) reste in extremis acceptable. Les mono-rampes sont parfaites
  (`BROWNHUG` 0.1, `SUPPORT2` 3.0, `BROWN96` 5.5).
- **« Quantizer au full-bright borne le pire cas » a été RÉFUTÉ** : l'erreur est **non-monotone**,
  elle culmine vers L2-L5 (jusqu'à +40% sur `BROWN96`/`SUPPORT2`), pas au full-bright. En
  pratique une table unique de reps suffit *si* on la choisit au niveau de **pic**, pas au
  full-bright.

---

## 7. Comment MAXIMISER le passage (si on le fait)

1. **`CL16Bnk`, jamais `CL16Look`.** Garde le lighting/flash en CRAM, évite les CLUTs VRAM et
   le re-bake au flash. Gratuit en mécanisme (§2).
2. **Quantization offline, dans `tools/strip_wad.py`** (one-time, qualité median-cut pleine),
   reps choisis au **niveau de lumière de pic** (pas full-bright, §6), avec **dither ordonné
   baké uniquement sur le ciel et les flats à dégradé** (`NUKAGE`/`SLIME`) — **jamais** sur les
   murs (interagit mal avec la colormap multiplicative : le motif s'assombrit et fourmille).
3. **Hybride per-surface** : le color-mode est **par commande VDP1** → 4bpp et 8bpp coexistent
   trivialement dans la même liste (`cmd[2]` color-mode bits). Router en 4bpp **uniquement** si
   `(≤16 couleurs réelles, mesurable au bake)` **ET** `(surface jamais rendue en software)` →
   exclut les murs near-wall, inclut les **flats** et les sprites compacts.
4. **Partager les sous-palettes de 16** entre textures de même famille pour tenir sous les 128
   fenêtres CRAM (§5.3).
5. **Cibler les flats du sol VDP1 d'abord** : seul endroit sans seam (100%-VDP1, jamais
   software), sans perte (flats ~0 RMS), et utile (VRAM tight). Cache flat 32→16KB.
6. **Colour-offset VDP2 (`COAR/COAG/COAB`)** reste souhaitable pour porter flash/fade en
   registre — mais **orthogonal**, pas un prérequis (et ce n'est pas « 3 écritures » triviales :
   il faut l'assigner par-couche participante, et le wrapper SRL `UseColorOffset` ne couvre que
   les ScrollScreen, pas la couche sprite VDP1 → chemin SGL brut `slColOffsetScrn(scnSPR…)`).

---

## 8. Recommandation finale (graduée)

| Cible | Verdict | Pourquoi |
|---|---|---|
| **Murs en 4bpp (full)** | ❌ **Non** | Gain VRAM marginal (starvation déjà résolue à 22 slots) ; perte de fidélité sur la famille tech ; **seam software/VDP1 clignotant** ; budget fenêtres CRAM ; et c'est l'inverse de ce que fait le meilleur FPS Saturn (16bpp RGB + Gouraud). |
| **Flats du sol VDP1 en 4bpp (`CL16Bnk`)** | ✅ **Oui — LE bon cas** | 100% quantizables sans perte ; jamais splittés software (pas de seam) ; débloque le plan sol VDP1 **sans** le cull qui ampute le cache mur ; cache flat 32→16KB. |
| **Sprites acteurs compacts low-color** | 🟡 Peut-être | Réutilise le pipeline ; gain modeste ; à valider via le routage hybride. |

**Ligne directrice :** ne touche pas au joyau 8bpp des murs. **Fais naître le sol VDP1
directement en 4bpp `CL16Bnk` pour ses flats** — c'est là que le ½-octet/texel compte (VRAM
serrée), que la quantization est indolore (flats), et que le seam n'existe pas.

---

## 9. Ce que le 4bpp NE résout PAS : l'overdraw reste le problème VDP1 #1

**Le 4bpp est un levier de STOCKAGE ; l'overdraw est un levier d'ITÉRATION. Ils sont
orthogonaux.** Le 4bpp n'améliore pas — même pas marginalement — le vrai goulot du VDP1.

**Rappel du modèle d'overdraw** ([`VDP1_ARCHITECTURE.md §3b`](VDP1_ARCHITECTURE.md)) : le
clipping du VDP1 supprime l'**écriture** mais **pas l'itération**. Un sprite distordu **parcourt
chaque pixel de son span projeté**, y compris hors-écran. Un mur collé à la caméra projette ses
coins à `y ≈ ±2000` → le VDP1 itère ce trapèze de ~4000 lignes alors que ~200 seulement sont à
l'écran. **Ce coût est purement géométrique, indépendant de la profondeur de couleur du texel :
4 bits ou 8 bits, c'est le même nombre de pas d'itération.** Le passage 8bpp→4bpp ne touche donc
ni l'overdraw, ni le budget commandes (`WALL_CMD_CAP`/`FLOOR_CMD_CAP`), ni le décrochage
CPU/VDP1 (~1 frame variable).

**Pire : ajouter les flats du sol sur VDP1 CRÉE une nouvelle surface d'overdraw.** Un sol balaie
des pieds à l'horizon → la courbure 1/z explose près de l'horizon, et un quad sol near-floor
re-projette vers l'infini (`yslope[ym]→∞` quand `ym→centery`). C'est le blow-up de la §3.3.1 du
plan monde. **Le 4bpp ne le borne en rien** ; il n'est borné que par la discipline
**géométrique** : bandes screen-Y *world-anchored* + `SAT_YCLAMP_GUARD` + flat-clamp des bandes
d'horizon ([`VDP1_WORLD_PLAN.md §3.3.1, §7.4`](VDP1_WORLD_PLAN.md)). Donc le 4bpp-flats **réduit
la VRAM** du sol mais **n'aide pas** — et expose une nouvelle source d'overdraw à dompter par
ailleurs.

**Les vrais leviers d'overdraw (déjà shippés pour les murs, à reproduire pour le sol) :**

| Levier | Ce qu'il borne | Statut |
|---|---|---|
| **Fallback software des murs proches** (span > 480px → colonnes CPU NBG1) | tue les pires explosions near-wall | shippé (`core/r_segs.c`) |
| **Sub-quad tiling vertical world-anchored + cull des bandes hors-écran** | borne le swim sans clamp écran | shippé (murs) ; à porter au sol (§3.3.1 plan monde) |
| **Flat fallback** (`FUNC_Polygon`, ~½ fill) pour murs lointains/over-budget | garantit que la liste finit toujours (jamais de trou) | shippé (`dg_saturn.cxx:2225`) |
| **`SAT_YCLAMP_GUARD`** (clamp `ym` loin de `centery`) | borne le blow-up near-floor sur l'axe screen-Y | à construire avec le sol |
| **Cap commandes + truncation far-first** (`Dr%` < 80% = NO-GO) | borne le budget, pas l'overdraw lui-même | partiel |

> **Conclusion §9 :** le 4bpp-flats est validé **comme optimisation VRAM** (il débloque le plan
> sol sans amputer le cache mur, §4) — **et seulement ça**. Le problème VDP1 #1 (overdraw +
> budget commandes + décrochage) reste **entièrement** du ressort du **bornage géométrique
> world-anchored**, un chantier **distinct et prioritaire**. Ne pas vendre le 4bpp comme un gain
> de perf VDP1 : c'est un gain de **place**, pas de **temps**.

---

## 10. Questions ouvertes (à trancher sur HW/émulateur)

1. Le `CL16Bnk` flat-sol bit-matche-t-il acceptablement le sol RBG0 dominant (256-col) à la
   couture inter-surface ? (Quantization flat ~0 RMS → probablement oui, à confirmer.)
2. Combien de **sous-palettes 16-col distinctes** un niveau Doom montre-t-il simultanément ?
   Détermine si les 128 fenêtres CRAM suffisent sans re-bake par frame (§5.3).
3. Le test perceptuel réel (filtrage point VDP1 + échelle 320×200 + distance) rachète-t-il la
   famille `TEKWALL` (RMS 17-20) — ce qui réduirait encore la liste 8bpp-only ?
4. Le sol VDP1 étant aujourd'hui un **stub** (aucun quad émis), le gain 4bpp-flats est
   **hypothétique** tant que le strip-emitter n'existe pas → prioriser le sol VDP1 8bpp d'abord,
   puis basculer les flats en 4bpp si la VRAM serre vraiment.

---

### Sources principales
- Code Mimas : `src/dg_saturn.cxx` (lignes citées en ligne), `core/r_segs.c`.
- Docs repo : [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md),
  [`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md), [`VDP2_LAYER_BUDGET.md`](VDP2_LAYER_BUDGET.md).
- SRL/SGL : `SaturnRingLib/modules/sgl/INC/sl_def.h:372-394` (modes couleur CMDPMOD),
  `SaturnRingLib/saturnringlib/srl_vdp1.hpp:23-44` (GetSizeShifter), `srl_scene2d.hpp:456-476`
  (Paletted16→CL16Bnk), `srl_cram.hpp` (128 banques de 16).
- Référence : `saturn-refs/SlaveDriver-Engine/` — `SPR.H:92-97` (enum COLOR_x), `PIC.C:84-99`
  (TILE16BPP), `WALLS.C:1082,636-720` (murs COLOR_5 + Gouraud), `UTIL.C:108-142` (greyTable).
- Web : VDP1 User's Manual ST-013 (color modes, CLUT, end-code), wiki.yabause.org VDP1,
  copetti.org Saturn, doomwiki.org Sega_Saturn (Doom Saturn officiel), SegaXtreme (CLUT/banks).
- Mesure : `scratchpad/wad_quant.py`, `scratchpad/wad_light.py` (quantization du WAD réel).
