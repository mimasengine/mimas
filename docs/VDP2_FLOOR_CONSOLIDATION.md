# Sol VDP2 — consolidation avant la session hardware

> Document de référence préparé pour la session de test sur Saturn réelle.
> Consolide les 7 lectures parallèles + vérification contre l'arbre courant
> (`src/dg_saturn.cxx`, `core/r_plane.c`) le 2026-06-25.
>
> **État shippé courant** : `VDP2_RBG0_TEST=0` (sol RBG0 compilé OUT),
> `VDP2_HW_SKY=1` (ciel NBG0 ON), sol logiciel. C'est le build known-good.

## 0. Ce qui est CÂBLÉ dans le build (prep 2026-06-25, compile-validé 2 configs)

Trois ajouts prêts à flasher ce soir (build par défaut = known-good inchangé) :

1. **Poke CYCxx direct** — `rbg0_commit_cyc()` ([dg_saturn.cxx](../src/dg_saturn.cxx),
   sous `#if VDP2_RBG0_TEST`) écrit `CYCA0/A1/B0/B1 @0x25F80010` **directement à la
   puce**, juste après `rbg0_commit_ramctl()`. Les banques de rotation A0/A1/B1 →
   `0xFFFFFFFF` (idle, la rotation les possède via RDBS) ; B0 (framebuffer NBG1
   vivant) **laissé intact** par défaut (`RBG0_CYC_POKE_B0=0`, bench-tunable). C'est
   **le geste qui manquait** = le fix présumé de la snow (§2 Option 2).
   → **Activer** : `VDP2_RBG0_TEST=1` + `VDP2_HW_SKY=0`. **Lire** : **row 13** =
   `CYb` (cycle-pattern AVANT, ce que SRL a laissé), **row 15** = `CYa` (APRÈS —
   A0/A1/B1 doivent lire `FFFFFFFF` ; si `CYa==CYb`, l'ISR SGL clobber → poke
   par-frame = Étape 4b), **row 14** = `RAMCTL` (inchangé).
2. **Classifieur ciel/sol §4** — `sat_sky_px` / `sat_floor_px`
   ([core/r_plane.c](../core/r_plane.c), toujours compilés) comptent les pixels
   couverts par le ciel vs le sol dominant **par frame**. → **Lire** : **row 19** =
   `CLS sky{n} flr{n} {S|F}` (S = ciel-dominant → favoriser ciel HW ; F =
   sol-dominant → favoriser sol VDP2). `flr` ≠ 0 seulement en mode floor-on.
3. **Vp go/no-go (déjà là)** — `RP_PROF=1` par défaut → **row 17** =
   `FLR Vs{} Vp{} d{}% n{}`. **`Vp` (= pic candidat-quad sol VDP1) est sur la
   row 17**, PAS row 13 (la string `FLAT` row 13 est cachée en split ; la valeur
   visible est mirroir-ée sur row 17). C'est le nombre qui débloque le sol VDP1.

> Build vérifié : config défaut **et** config floor-test (`RBG0_TEST=1`/`HW_SKY=0`)
> compilent et lient proprement (`OK bin=…`). Le core (`r_plane.c`) est modifié →
> à committer dans le submodule + propager à DoomJo quand tu valides (pas encore
> fait, working-tree only).

---

## 1. État des lieux : ce qu'on sait du sol VDP2

### 1.1 Le pari RBG0 (rappel)

Déporter sur un plan de rotation VDP2 (RBG0, « Mode-7 ») le **flat de sol
dominant** de la frame — en pratique le sol du secteur où se trouve le joueur
(pick stable par `(height, picnum, lightband)`, **pas** le plus gros en pixels :
le pick par aire-pixel a été essayé puis abandonné car il flickait quand deux
sols se disputent la tête — cf. commentaire `core/r_plane.c:957-960`). Le reste
est rendu en logiciel. Données du profiler row-13 : un seul flat occupe 49–93 %
du remplissage de sol (moyenne ~64 %, jusqu'à 74–99 % en salle chargée) → le
trick « un seul flat » mord. Gain estimé 8–14 ms/frame (~15–24 %) au pot0.

Implémentation retenue : RBG0 **cell-based** (`slCharRbg0`/`slPageRbg0`/
`slPlaneRA`/`sl1MapRA`, géré par registres), **pas** bitmap. Bring-up débogué
via la référence Jo Engine (`jo_background_3d_plane_a`) ; le bug décisif était
les arguments de `slPageRbg0` inversés (arg1 = base CELL, la map passe par
`sl1MapRA`).

### 1.2 L'échec hardware exact : « snow + dead sky »

Sur **Ymir** tout marchait de bout en bout (Romain, 2026-06-18). Sur **Saturn
réelle**, RBG0 a cassé **tout** le VDP2 :

- **« snow »** = bandes blanches de longueur variable sur toute l'image,
- **dead sky** = ciel bleu / non affiché.

Ymir lit directement les registres **shadow** de SGL → il masque le bug. Même
piège émulateur-vs-hardware que le bug cache SCU-DMA.

### 1.3 La cause racine : deux défauts empilés, tous deux fatals sur HW

**(1) COMMIT GAP.** SGL ne flushe `RAMCTL` (dont les bits RDBS de sélection de
banque de rotation) **et** le motif de cycle `CYCxx` depuis ses registres shadow
**qu'à l'intérieur de `slSynch`**. Mimas supprime volontairement le `slSynch`
par frame (cf. §1.5) → le motif de cycle / la réservation de banque RBG0
**n'atteint jamais la puce** → les couches NBG bitmap lisent un motif de cycle
non programmé → **famine** → snow + ciel mort.

> Loi de l'échec (manuel VDP2, verbatim) : *« If the VRAM access address
> specified in the cycle pattern register is not within the specified bank,
> access will not occur and correct screen display will not be possible. »*
> Une couche affamée ne retombe **pas** en transparence propre : elle **corrompt**
> (pixels de bus périmés = traînées blanches).

**(2) SUR-SOUSCRIPTION DE BANQUE.** Un RBG0 cell avec table de coefficients en
VRAM réclame **3 banques entières** : pattern-name (map) + character (cells) +
coefficient (K) — chaque accès occupe **tout** le timing d'une banque. Avec le
framebuffer obligatoire, ça fait **4 banques** → **plus aucune banque pour le
ciel HW**. Le layout original mettait illégalement cells ET K-table dans A1
(deux lectures pleine-banque dans une banque = interdit).

### 1.4 Le modèle de capacité VDP2 (banques / cycles)

- VRAM = 512 KB = **4 banques × 128 KB** : A0/A1/B0/B1 @ `0x25E00000` /
  `0x25E20000` / `0x25E40000` / `0x25E60000`. + 4 KB Color RAM (CRAM).
- Plafond dur = **4 banques** ; le framebuffer en réclame toujours **1** → il
  reste exactement **3**.
- Budget de cycles : chaque banque expose 8 timings T0–T7 en résolution normale
  (320/352). Registres de motif : `CYCA0`/`CYCA1`/`CYCB0`/`CYCB1`.
- Coût par dot (res normale) : bitmap 8bpp = **2 reads** ; le framebuffer et le
  ciel Mimas sont tous deux 8bpp = 2 reads chacun, seuls dans leur banque
  (confortable, 2/8). **RBG0 = 3 reads pleine-banque = 3 banques.**

**Arithmétique qui force la conviction** : framebuffer (1, non négociable) +
RBG0 cell (3) = 4 = TOUT → **0 banque pour le ciel HW**. Ciel HW + framebuffer +
RBG0-cell-K-VRAM = 5 réclamations pour 4 banques = **physiquement
insatisfaisable**.

> **CONVICTION (loi hardware, pas un réglage)** : au-dessus du framebuffer
> obligatoire on peut avoir **le ciel HW OU le sol RBG0 (K en VRAM), JAMAIS les
> deux**. *Sky XOR floor.*

Les deux façons de garder le sol RBG0 :
- **(a)** Lâcher le ciel HW → ciel logiciel, ce qui libère la banque **A0** pour
  la K-table → seul layout 4-banques propre : **B0=fb / A1=cells / B1=map /
  A0=K-table**.
- **(b)** Mettre la K-table en Color RAM via `CRKTE` (garde le ciel HW) — mais
  risque de collision avec la palette 8bpp dans les 4 KB de CRAM. **Non prouvé,
  à investiguer.**

Nuance importante : le **gain REC c'est le SKIP du sol** (index-0, ne pas dessiner
le span), **pas** le RBG0 en soi. Donc un ciel HW + un « sol gradient » ~0 banque
(skip du fill logiciel, back-screen / line-colour gradient à la d32xr) capture
~le même gain (+20–33 %), **garde** le ciel HW, et **ne touche pas** au motif de
cycle. Le vrai axe est *qualité-du-sol vs coût-en-banques*, pas un « sky XOR
floor » brut.

### 1.5 Pourquoi pas de slSynch — et l'Option-1 rejetée

`slSynch` attend un vblank complet = **~16 % de taxe fps** (cap à ~7–12 fps),
**ET** tick le driver son SGL qui se bat avec le SFX SCSP direct de Mimas, ET
historiquement corrompt le setup boot VDP2/son. Verdict : *« actively harmful »*
dans cette architecture (`src/dg_saturn.cxx:2644-2647` documente le no-slSynch
en code).

**Option 1 (commit one-shot via slSynch)** : `rbg0_commit_pattern()` =
`slScrAutoDisp(mode maximal)` + `slSynch()` une fois au `DG_Init` + re-commit au
switch de mode pad-Y. **Testé sur HW = PIRE**, reverté chirurgicalement (5 edits
manuels, pas un `git checkout` — un fix VDP1 concurrent éditait le même fichier).

> **LEÇON** : appeler `slSynch` **du tout** — même one-shot, à l'init — est
> actively harmful ici. Le commit du motif de cycle **ne doit PAS** passer par
> `slSynch`, ni par `slScrCycleSet` (shadow-only, même problème de flush).

### 1.6 État de l'arbre (vérifié 2026-06-25)

- `src/dg_saturn.cxx:227` — `#define VDP2_RBG0_TEST 0` (sol RBG0 PAUSÉ, tout le
  feature `#if`'d out).
- `src/dg_saturn.cxx:240` — `#define VDP2_HW_SKY 1` (ciel HW shippé).
- `src/dg_saturn.cxx:1202-1211` — `rbg0_commit_ramctl()` : écrit **uniquement la
  moitié RDBS** de RAMCTL @ `0x25F8000E`, valeur `(before & 0xFC00) | 0x0300 |
  0x008D` (RDBS = `0x8D` : A0=coeff / A1=char / B0=fb / B1=PN), readback
  `ramctl_before`/`after`, appelé en `:1396` sous `VDP2_RBG0_TEST`.
  **Le poke `CYCxx` — la moitié qui tue réellement la snow — est ABSENT.**
- `core/r_plane.c:1078-1081` — floor-skip : si `sat_vdp2_floor` ET match
  `height`+`picnum`+`lightband`, on écrit index 0 sur le span et `continue`
  (RBG0 transparaît).
- Le bug VDP1-floor-stub : `sat_floor_vdp1_stub` (`:1219`) renvoie 1 pour CHAQUE
  candidat mais n'émet **aucune** strip ; `sat_vdp1_floor=0` au boot
  (`:1426-1427`).

> **Note de nommage** : une des lectures appelle la fonction
> `vdp2_ramctl_commit()` — le symbole réel est **`rbg0_commit_ramctl`**
> (`:1202`, appel `:1396`). Idem certaines lignes citées (1031-1039, 1219) sont
> approximatives ; les références ci-dessus sont vérifiées contre l'arbre.

---

## 2. Les deux chemins restants pour un sol VDP2

### Option 2 — commit registre direct du motif de cycle, SANS slSynch

**Le précédent qui valide l'approche** : le **VDP1 est déjà piloté 100 % async
SANS slSynch** en pokant ses registres directement, et c'est **HARDWARE-VALIDÉ**
(2026-06-16 : quad blanc puis l'arme du joueur rendus sur la couche sprite
VDP1 par-dessus le jeu, fps tenu, pas de freeze, `FBCR=0` collé — l'ISR SGL ne
le clobber pas — `EDSR=0003` = draw complet avec **zéro attente CPU**). Map
registre VDP1 (`src/dg_saturn.cxx:1513-1520, 2301-2313`) : base `0x25D00000`,
`PTMR=2` plot-start-and-return, `FBCR=0x0003` double-buffer manuel anti-tear.

**Le geste** : poker `RAMCTL @0x25F8000E` (RDBS — **déjà fait**) **PLUS**
`CYCA0/CYCA1/CYCB0/CYCB1` directement (style Jo NOSGL `RAMCTL=0x1327` +
`CYCA0=0x5555FEEE...`), **sans** `slSynch` ni `slScrCycleSet`.

| | |
|---|---|
| **Coût** | Quelques pokes 16-bit + readback. Sub-ms. Aucune taxe fps (modèle VDP1 validé). |
| **Ce qui peut foirer** | (1) L'ISR vblank SGL re-pushe `BGON`/scroll **chaque frame** — on ignore s'il re-pushe aussi `RAMCTL`/`CYCxx`. La note dit qu'il **ne** re-pushe **pas** RAMCTL, donc un commit one-shot suffirait pour RDBS — **à confirmer pour CYCxx** (peut nécessiter un poke par-frame). (2) Calcul des codes `CYCxx` faux → snow persiste / pire. (3) Reste le problème de banque : RBG0 (K en VRAM) = 3 banques + framebuffer = 4 → **il faut lâcher le ciel HW** (`VDP2_HW_SKY=0`) pour que A0 héberge la K-table légalement. |
| **À vérifier ce soir** | Mettre `VDP2_RBG0_TEST=1` + `VDP2_HW_SKY=0` (le poke `CYCA0/A1/B0/B1` est **déjà câblé** dans `rbg0_commit_cyc`, cf. §0). Lire **row 15** (`CYa` — les banques rotation A0/A1/B1 lisent-elles `FFFFFFFF` ?) et **row 14** (`RAMCTL …8D`). **Observation pass/fail : la snow disparaît-elle ?** Si oui, le sol RBG0 transparaît-il sous les murs/sprites avec occlusion correcte ? **Ne PAS re-tenter slSynch.** |

### Fallback — ciel logiciel + sol VDP2 uniquement

C'est le **pick de Romain**, jugé sain. On droppe le ciel NBG0
(`sat_vdp2_sky=0`, ciel rendu en logiciel — il y a moins de ciel que de sol à
l'écran, le sol est le vrai gain REC), ce qui **libère la banque A0** → le seul
layout 4-banques propre : **B0=fb / A1=cells / B1=map / A0=K-table** (la K-table
sort de A1, ce qui tue le double-read illégal). Le build `VDP2_HW_SKY=0` câble
déjà ce layout (`src/dg_saturn.cxx:246`).

| | |
|---|---|
| **Coût** | Le ciel repasse en software → on **récupère** une partie du coût qu'on avait économisé via NBG0 (mais le ciel est moins coûteux que le sol). Slave libéré par le déport du sol → peut absorber le ciel software re-ajouté. |
| **Ce qui peut foirer** | **La snow est INDÉPENDANTE de la question du ciel.** Ce fallback résout le problème de **banque** mais **PAS** le **commit-gap** : il faut **TOUJOURS** le poke `CYCxx` direct. Lâcher le ciel sans poker CYCxx → snow quand même. |
| **À vérifier ce soir** | Idem Option 2 mais c'est le layout par défaut quand `VDP2_HW_SKY=0`. Vérifier d'abord que le ciel software seul (RBG0 off) reste propre, **puis** activer RBG0 + poke CYCxx. |

> Les deux chemins partagent **le même geste manquant** : le poke direct
> `CYCxx`. Le fallback ne fait que **rendre le layout de banque légal** ; il ne
> dispense pas du commit.

---

## 3. VDP2 floor vs VDP1 dominant-flat floor — comparatif

| Critère | **VDP2 / RBG0** | **VDP1 strips affines (dominant flat)** |
|---|---|---|
| Statut HW | **Cassé** sur HW (snow + dead sky). Chemin direct-CYCxx **non testé**. | **Mur VDP1 shippé + HW-validé** (le driver async no-slSynch est la fondation). Le strip-emitter de sol **pas encore** émis (inc-1 = stub). |
| Fondation | Doit commiter RAMCTL/CYCxx **sans** slSynch (slSynch=poison). | **Roule déjà** sur le driver async no-slSynch prouvé-collant (`FBCR`/`PTMR`). |
| Surfaces | **1 plan = 1 hauteur**. Un seul flat. | **Multi-surface** : autres hauteurs + plafonds non-sky. |
| Qualité | Perspective **exacte** (pas de swim). | Affine par bandes → **swim près de l'horizon** (cap qualité). Erreur ∝ courbure(1/z)×hauteur_bande². |
| Coût CPU | ~0 CPU (matériel dédié). | Base u/v calculée **1×/bande** (~24×) au lieu d'1×/scanline (~112×) → **moins cher** que le software pour la même surface, fill **async** en parallèle. |
| Budget | 3 banques (sky XOR floor). | ~30–72 cmds, 4 KB VRAM, ~1.6–2.5 ms async (~2 % frame). Réutilise le skip `sat_vdp2_floor`. |
| Gain | ~8–14 ms prédit (mais ne ship pas). | ~même 4–9 ms (15–24 %), **et ça ship**. |
| Risque plumbing | Minefield motif de cycle. | Kick/paint-order (sols connus seulement **dans** `R_DrawPlanes`, après le kick mur). 2e banque cmd préférée. |

**Honnêteté** : selon la mémoire (`doomsrl-vdp1-floor-plan`), les strips
dominant-flat VDP1 **battent** RBG0, pour une raison simple et décisive : **VDP1
roule sur le précédent registre-direct déjà prouvé-collant sur HW**, alors que
RBG0 exige de résoudre le minefield motif-de-cycle que personne n'a encore
validé. RBG0 = 1 plan = 1 hauteur ; VDP1 = multi-surface.

**Reframe multi-tier (Romain, 2026-06-23, autoritatif)** : ce n'est **pas** un
XOR. Les sols/plafonds **composent par force** :
- **Sol DOMINANT → RBG0** (le pire cas swim de VDP1 / plus gros 1/z = exactement
  le boulot de RBG0),
- **autres hauteurs + plafonds non-sky → strips VDP1** (plus petits/proches =
  moins de swim),
- queue → NBG1 software, ciel → NBG0.
- Stack de priorité : sky NBG0(3) < RBG0(4) < VDP1(5) < NBG1(6).

> **Recommandation §3** : **construire d'abord le chemin VDP1** (il ship, il est
> multi-surface, il évite le minefield). Garder RBG0 comme **complément optionnel
> pour le seul flat dominant** SI le poke CYCxx direct s'avère collant ce soir.
> Ne **pas** bloquer le sol VDP1 sur la résolution de RBG0. **Le go/no-go VDP1
> dépend d'un seul nombre non encore mesuré : `Vp` (pic de commandes sol, row
> 13)** — GO si `Vp≤64`, ou `≤120` (2e banque). **À mesurer ce soir aussi.**

---

## 4. Mode 1 joueur : ciel + sol, et la classification de maps

### 4.1 Le problème

Romain veut sol-dominant OU sol-joueur, **avec** ciel. Mais la conviction HW =
**sky XOR floor** sur VDP2 (impossible d'avoir les deux). D'où son idée :
**classifier les maps** —

- **OPEN large** (peu/pas de plafond à rendre) → avantage = **ciel** → ciel VDP2
  ON, sol VDP2 OFF.
- **CLOSED large** (gros sols + gros plafonds) → avantage = **sol VDP2** → sol
  VDP2 ON, ciel VDP2 OFF.
- **Pas de ciel sur la map** → sol VDP2 ON (le ciel ne coûte rien).

### 4.2 L'heuristique est-elle saine ?

**Oui, le principe est juste**, et il s'appuie sur un fait dur déjà en place :
le gain VDP2 vient du **skip** de la surface la plus visible. La question est
donc *« qu'est-ce qui occupe le plus de pixels REC : le ciel ou le sol ? »* et on
déporte celui-là.

Deux réserves importantes :

1. **Le sol est gros même en extérieur.** En E1M1 outdoor le profiler donne dom%
   sol = 93 %. Donc « OPEN → ciel gagne » n'est **pas** automatique : il faut
   **mesurer** que le REC ciel y bat le REC sol, ne pas l'**assumer** (noté comme
   open question dans deux lectures). En pratique le sol est souvent le plus gros
   poste **partout**, ce qui penche pour « sol VDP2 par défaut, ciel software ».
2. **« Pas de ciel → sol VDP2 ON »** est le cas **le plus clair et le plus sûr** :
   pas de conflit de banque (A0 libre), pas de question de priorité ciel. À
   prioriser comme premier cas validé.

### 4.3 Comment détecter open-vs-closed

Trois granularités, de la plus simple à la plus fine :

**(A) Par map (statique, build-time / table).** Pré-classifier chaque niveau
(E1M1=open, E1M3=mixed…). Simple, zéro coût runtime, mais grossier : une map
« open » a des intérieurs fermés et vice-versa. Le switch de layout VDP2 est un
**choix de build** aujourd'hui (`VDP2_HW_SKY`), pas un toggle pad — un re-commit
RAMCTL/CYC mid-game est fragile (`src/dg_saturn.cxx:232-239`). Donc un switch
**par map** (au chargement, avant que le rendu démarre) est le **plus réaliste à
court terme** : on re-commit les banques une fois au load, pas par-frame.

**(B) Par région / secteur.** Trop fin pour un seul plan VDP2 (une seule
config de banque globale à l'écran) → **pas exploitable** pour basculer le
layout. Utile seulement pour décider quel flat est « dominant ».

**(C) Dynamique par-frame, piloté par les pixels de ciel visibles.** **Le hook
existe déjà** : `core/r_plane.c:1015` pose `sat_frame_has_sky = 1` *uniquement
si un visplane de ciel est réellement en vue cette frame* (reset à 0 en `:952`).
Le platform l'utilise déjà pour **dropper NBG0** dans les salles fermées
(`src/dg_saturn.cxx:2576-2580`, `show_sky` gated par `sat_frame_has_sky`).

> **C'est l'observation clé** : on a déjà un signal par-frame « le joueur voit-il
> du ciel ? ». **C'est désormais instrumenté** (§0 #2) : `sat_sky_px` (aire des
> visplanes ciel) vs `sat_floor_px` (aire du sol dominant), affichés **row 19**
> (`CLS sky{} flr{} {S|F}`), pour mesurer map par map qui domine avant de basculer
> **sat_vdp2_floor / sat_vdp2_sky**.

**MAIS — le piège bascule-dynamique** : basculer la **couche VDP2** par-frame
exige un **re-commit RAMCTL/CYCxx** par-frame (réassigner les banques A0
sky↔K-table). C'est précisément le geste fragile/non prouvé. Donc :

- **Bascule dynamique du FLAG software-skip** (`sat_vdp2_floor` on/off, écrire
  ou non l'index-0) = **cheap et sûr** — c'est juste du software.
- **Bascule dynamique du LAYOUT de banque VDP2** (sky↔floor) = **cher et
  risqué** — re-commit registre par-frame, à ne tenter qu'après que le commit
  one-shot soit prouvé collant.

### 4.4 Schéma de classification concret et testable

Proposition graduée (n'activer une marche qu'après validation HW de la
précédente) :

1. **Cas « map sans ciel »** → `VDP2_HW_SKY=0` (libère A0) + sol RBG0/VDP2 ON.
   Le plus sûr, le premier à valider. **Test ce soir** : une map sans ciel,
   sol VDP2, vérifier *pas de snow + sol correct*.
2. **Cas « map avec ciel » (statique par-map)** : table de classification
   open/closed décidée **au load**, re-commit banques **une fois**. Pas de
   bascule par-frame.
3. **Cas dynamique (futur)** : compteur de pixels ciel vs sol-dominant par-frame
   (`sat_sky_px`/`sat_floor_px`, row 19, **câblé** §0 #2).
   **N'activer que le flag software-skip**, garder le **layout de banque fixe**
   (décidé par-map) tant que le re-commit par-frame n'est pas prouvé.

> **Verdict §4** : l'heuristique est **saine en principe** mais (a) « OPEN→ciel »
> doit être **mesuré** (le sol est gros même dehors), (b) le cas **sans-ciel** est
> le meilleur point d'entrée, (c) la bascule dynamique doit se faire au niveau du
> **flag software** (cheap), **pas** du **layout de banque** (cher/risqué) tant
> que le commit registre n'est pas validé. La granularité réaliste = **par-map au
> load**, signal par-frame `sat_frame_has_sky` déjà disponible pour affiner.

---

## 5. Mode 2 joueurs et + : un seul plan VDP2 pour des vues différentes

### 5.1 La limite hardware fondamentale

Une couche VDP2 (NBG0-3, RBG0/1) a **exactement UN jeu de registres global pour
tout l'écran** : un origine de scroll (`SCXIN`/`SCYIN`) par NBG, une matrice
affine (table de coefficients + paramètres) par RBG. La couche est composée sur
**tout** l'affichage — **pas de scroll/rotation par région**. Deux vues split
avec angle/position caméra différents ont besoin de deux transforms différents,
qu'**une seule couche ne peut physiquement pas fournir**.

### 5.2 Ciel pour 2 joueurs — **NON**

Le ciel HW est NBG0, bitmap 512×256 8bpp scrollé 1:1 avec `viewangle`. Le registre
de scroll unique de NBG0 ne peut encoder **qu'un seul** yaw. Si les deux joueurs
regardent dans des directions différentes, une seule vue aurait le bon ciel.

C'est exactement pourquoi le path 2p shippé **force le ciel en software** :
`src/dg_saturn.cxx:2579` gate `show_sky` avec `&& sat_local_players <= 1`, et le
commentaire dit verbatim que NBG0 *« is a single layer scrolled by one viewangle
and cannot serve two split views → the split renders the SOFTWARE sky instead. »*
**Confirmé shippé + HW-validé** (2026-06-22 ; core `8f1197f`, Mimas `0d1624c`).
Romain : *« le mode vdp2 marche pas en multi, on y réfléchira après. »*

**Et le WINDOWING (W0/W1) ?** Le VDP2 a des registres de fenêtre qui peuvent
**masquer** une couche à une région d'écran (clipper NBG0 à la moitié gauche, une
2e NBG à la droite). **Mais** : (1) chaque vue a besoin de **sa propre couche**
avec **son propre registre de scroll** — le windowing masque, il ne **duplique
pas** les registres ; (2) il faudrait donc une **2e couche sky indépendante** →
le budget 4-banques **interdit** : framebuffer (1) + sky1 (1) + sky2 (1) laisse 1
banque, et on n'a même plus de marge pour autre chose. La line-scroll VDP2 (scroll
par scanline) varie par **ligne raster = Y écran**, pas par moitié **gauche/droite**
→ **dead end** pour un split vertical. **Verdict ciel 2p = NON.**

### 5.3 Sol RBG0 pour 2 vues — **NON (pire)**

RBG0 a **une** matrice affine/coefficient pour tout l'écran. Chaque vue split a
besoin d'un affine différent (sa propre pos+yaw+pitch projetés sur le plan sol).
Une matrice ne peut satisfaire deux vues. **Indépendamment**, RBG0 casse déjà en
**solo** (snow + dead sky). Une version par-vue est donc **doublement
impossible**. Le windowing ne sauve rien : 2 vues = 2 matrices = 2 plans RBG0 =
6 banques pour 4. Le sol en MP est donc **software (potato)** — décision
d'archi explicite (2026-06-19), qui contourne entièrement la limite mono-matrice.

### 5.4 La position de Romain : sol VDP2 pour le flat dominant des X joueurs, seulement à pot0/0.5

Le raisonnement de Romain est juste sur **quand** un sol VDP2 aiderait, mais il
bute sur la limite mono-matrice :

- Le sol VDP2 ne **vaut** quelque chose qu'à **pot0/0.5**, car c'est le seul régime
  où les sols sont **texturés** (terme P dominant, ~22 ms @pot0 en 2p split). À
  pot1/pot2 les sols sont déjà des fills flat software (P → 3–8 ms) → un sol VDP2
  n'apporterait **rien**.
- **MAIS** ce régime pot0/0.5 est **exactement** celui que la limite mono-matrice
  **interdit** : deux vues = deux affines, un seul plan RBG0. Un sol VDP2
  « dominant des X joueurs » supposerait un **flat commun** aux deux vues à une
  **hauteur commune** projeté par **une matrice commune** — ce qui n'a de sens
  que si les deux caméras partagent pos+angle+pitch (ce qui n'est jamais le cas
  en split jouable).

> **Verdict §5** : ciel 2p = **NON** (mono-registre, budget banque). Sol RBG0 2p
> = **NON** (mono-matrice, + cassé même en solo). Le windowing **ne résout pas**
> (masque, ne duplique pas les registres ; budget banque foreclos). Position
> Romain (pot0/0.5) : le **timing** est correct (c'est le seul régime utile) mais
> c'est **précisément** le régime mono-matrice-interdit → **non réalisable** tel
> quel. Le MP reste **software (potato) pour sol ET ciel** ; le levier MP réel
> est le potato (pad-Z), pas un sol VDP2. Piste résiduelle non investiguée : un
> **back-screen/line-colour gradient par moitié** (via windowing, ~0 banque, pas
> de registre de scroll par-angle) donnerait un fond **cohérent** à chaque vue
> mais **perd** le parallaxe couplé à l'angle — *à investiguer plus tard, pas ce
> soir*. **Aucun émulateur ne peut valider tout ça — HW only.**

---

## 6. Plan de test hardware pour ce soir

Ordonné **du risque le plus faible au plus élevé**. Chaque étape a une
observation pass/fail nette. **Ne PAS re-tenter slSynch à aucune étape.**

### Étape 0 — Baseline known-good (sanity)
- Build shippé : `VDP2_RBG0_TEST=0`, `VDP2_HW_SKY=1`.
- **PASS** : ciel HW propre, **pas de snow**, jeu nominal. Fixe le point de
  référence avant toute modif.

### Étape 1 — Capturer le baseline REC/P (section C manquante)
- Toujours build shippé. Lire le profiler aux 6 spots de référence (E1M1 outdoor
  ext, MAP01, grands halls) : `REC`/`EX`/`P`, et **row 17 :
  `FLR Vs{} Vp{peak} d{dom%}% n{}`** (`Vp` = pic candidat-quad sol VDP1 ; `RP_PROF=1`
  par défaut donc c'est déjà affiché).
- **PASS** : on obtient le `Vp` (go/no-go VDP1) **et** un baseline P/REC post-QW
  (la section C de `REC_BENCHMARKS.md` est vide — c'est la mesure perf #1 en
  attente). **Décision** : `Vp≤64` → GO sol VDP1 1 banque ; `≤120` → GO 2e
  banque ; explose → bandes plus grossières / near-floor-only / abandon.

> **RÉSULTAT HW 2026-06-25 (36 photos E1M1 strippé, build `b:19:25:29`) — À CONFIRMER
> relecture (lecture photo, angle/flou).** `FLR Vp` pic **≈ 150–170** aux spots E1M1
> ouverts, `d%` ~49–76 % (un flat domine la moitié-aux-3/4 du sol), `n` ~16–34.
> ⚠️ **Le seuil go/no-go `≤64/≤120` ci-dessus est celui du plan SUPERSEDED `VDP1_FLOOR_PLAN.md`
> (sol partageant la banque mur).** Le plan **autoritatif** [`VDP1_WORLD_PLAN.md`](VDP1_WORLD_PLAN.md)
> (§0.2/§7.1/§7.3) dimensionne une **banque F dédiée (cap 248)** pile pour **`Vp ≈ 158`** → la lecture
> HW ~150–170 **tombe sur le point de design du plan, ce n'est PAS un NO-GO**. `d%` HW (~49–76 %)
> **confirme** le « un seul flat » du §1.1. **Le vrai gate HW du sol VDP1 n'est pas `Vp` mais `Dr%`**
> (`VDP1_WORLD_PLAN.md` §7.4 : `Dr%`<80 % = NO-GO) : les photos lisent **`Dr` 26–41 % au pot0** —
> SOUS ce seuil — vs « 92–94 % measured » assumé par le plan ⇒ **à relire en priorité** (cf.
> `REC_BENCHMARKS.md §C.1` finding #2). **CLS** (`sky`/`flr`) : `flr` lit **0** partout (sol-mode OFF
> dans le build shippé → `sat_vdp2_floor=0`), donc la comparaison sky-vs-floor du §4 **n'est PAS
> mesurable** sur ce set ; il faut un build floor-on. En extérieur `sky` > 0 → `S` (ciel-dominant)
> comme attendu. **rL HW = 2.1 stable** (cf. `REC_BENCHMARKS.md §C.1`) — Ymir ne le voit pas.

### Étape 2 — Ciel software seul (valider le fallback layout, RBG0 OFF)
- `VDP2_HW_SKY=0`, `VDP2_RBG0_TEST=0`. Le ciel repasse en software, A0 libre.
- **PASS** : ciel software propre, **pas de snow** (on confirme que le simple fait
  de libérer A0 ne casse rien et que le software-sky est correct). **FAIL** :
  problème ailleurs que RBG0 → à isoler avant d'aller plus loin.

### Étape 3 — (optionnel) RBG0 + RDBS seul (reproduire la snow connue)
- `VDP2_RBG0_TEST=1`, `VDP2_HW_SKY=0`, **mais commenter l'appel
  `rbg0_commit_cyc();`** ([dg_saturn.cxx](../src/dg_saturn.cxx), juste après
  `rbg0_commit_ramctl()`) pour isoler le RDBS seul.
- Lire **row 14** : `RAMCTL b=… a=…` (le low byte se relit-il à `…8D` ?).
- **Observation attendue (connue)** : **snow persiste** (RDBS seul insuffisant).
  Si la NBG3 debug overlay (B1, comme la map) se corrompt, ça **confirme** que le
  commit RDBS a « pris » (B1 réservé). C'est le diagnostic. (Saute cette étape si
  tu veux aller direct au test clé — le poke CYC est déjà câblé.)

### Étape 4 — **LE test clé** : poke direct CYCxx (sans slSynch) — DÉJÀ CÂBLÉ
- `VDP2_RBG0_TEST=1`, `VDP2_HW_SKY=0` (appel `rbg0_commit_cyc()` actif). Les pokes
  `CYCA0/A1/B0/B1` partent **une fois** au `DG_Init` juste après
  `rbg0_commit_ramctl` (rotation A0/A1/B1 → `FFFFFFFF`).
- **Lire row 15** (`CYa`) : A0/A1/B1 doivent lire `FFFFFFFF` (le poke a pris).
- **PASS** : **la snow DISPARAÎT**, le sol RBG0 transparaît sous les murs/sprites
  avec occlusion correcte, ciel software toujours propre. **FAIL #1** : snow
  persiste mais `row 15 == row 13` (poke clobberé par l'ISR) → **étape 4b**.
  **FAIL #2** : snow persiste alors que `row 15` a bien pris `FFFFFFFF` → codes CYC
  faux (essayer B0 forcé via `RBG0_CYC_POKE_B0=1`, ou autre motif). **FAIL #3** :
  pire qu'avant → mauvais layout banque, revérifier A0=K/A1=cells/B1=map.

### Étape 4b — (si étape 4 FAIL par clobber) poke CYCxx par-frame
- Déplacer les pokes CYCxx dans la boucle de frame (avant la présentation), pas
  seulement au DG_Init.
- **PASS** : snow disparaît → l'ISR re-pushait CYCxx, un poke par-frame est requis
  (analogue à confirmer comme pour `FBCR` VDP1).

### Étape 5 — Qualité du sol RBG0 (si étape 4/4b PASS)
- Vérifier pitch/yaw : le plan converge-t-il sur l'horizon de Doom ?
  (`slRotX(0x4000+0x300)`, `slRotZ(-(viewangle>>16)+0x4000)`). Le sol suit-il le
  mouvement joueur ? Texture 1:1 ?
- **PASS** : sol texturé-en-perspective cohérent, pas de tearing, fps tenu.

### Étape 6 — Delta perf
- Comparer REC/P (row 19/20) **avec** sol RBG0 (étape 5) vs baseline software
  (étape 1) aux mêmes spots.
- **PASS** : `P` chute significativement (cible -17 à -26 ms au pot0 d'après l'A/B
  2026-06-19 ; **plus faible au pot1/2** où le slave est déjà ~80 % idle — le gain
  y est surtout **qualité**).

> **Si le temps manque** : prioriser **Étape 1 (lire Vp + baseline)** — c'est le
> nombre qui débloque le chemin VDP1, indépendant de tout le minefield RBG0 — puis
> **Étape 4 (poke CYCxx)** qui est le seul test décisif du chemin VDP2.

---

## 7. Verdict & recommandation

**Faut-il poursuivre le sol VDP2 ?**

- **1 joueur** : **Oui, mais en seconde priorité derrière le sol VDP1.** Le
  chemin VDP1 strips affines roule sur le **précédent registre-direct déjà
  HW-validé** (FBCR/PTMR collés), il est **multi-surface**, et il **évite le
  minefield motif-de-cycle**. Le sol VDP2/RBG0 ne reste intéressant que comme
  **complément** pour le **seul flat dominant** (le pire cas swim de VDP1) — **et
  seulement si** le poke direct `CYCxx` (Étape 4) s'avère collant ce soir. Le cas
  **« map sans ciel »** est le meilleur point d'entrée (A0 libre, pas de conflit).

- **MP (2+)** : **Non.** Limite mono-registre (ciel) et mono-matrice (sol RBG0)
  + budget 4-banques → physiquement impossible pour des vues différentes, et le
  windowing ne duplique pas les registres. Le sol/ciel MP reste **software
  (potato)** ; le levier réel est le potato pad-Z, pas un sol VDP2. Le régime où
  un sol VDP2 aiderait (pot0/0.5 texturé) est **exactement** celui que la limite
  mono-matrice interdit.

**Recommandation d'action** :
1. Ce soir, **mesurer `Vp`** (débloque le sol VDP1, le vrai cheval) **et** tenter
   le **poke CYCxx** (le seul test qui décide du sort de RBG0).
2. Construire le **sol VDP1** comme chemin principal (le strip-emitter inc-2..6,
   aujourd'hui stub). Garder RBG0 1P comme option pour le flat dominant **si**
   CYCxx colle.
3. Pour le 1P avec ciel : valider d'abord **« sans ciel → sol VDP2 »**, puis
   bascule **par-map au load** (re-commit banque une fois), bascule par-frame
   seulement au niveau du **flag software-skip** (`sat_vdp2_floor`), jamais du
   layout de banque tant que le commit n'est pas prouvé.

**Les 2-3 plus gros risques ouverts** :
1. **Le poke `CYCxx` colle-t-il sans slSynch comme `FBCR`/`PTMR` le font ?**
   L'ISR vblank SGL re-pushe BGON/scroll (mais pas RAMCTL d'après la note) — il
   pourrait re-pousser/clobberer CYCxx → poke par-frame nécessaire. **Inconnu, à
   vérifier ce soir (Étapes 4/4b).**
2. **`Vp` peut exploser** (fragmentation x-runs des piliers + u-tiling des flats
   >64 texels) → le sol VDP1 pourrait ne pas tenir le budget commande. **Mesure
   non encore faite.**
3. **CRKTE (K-table en CRAM)** — seule option « ciel HW **ET** RBG0 » — risque
   de collision avec la palette 8bpp dans 4 KB de CRAM. **Non prouvé**, pas pour
   ce soir.

> **TL;DR** : Le sol **VDP1** est le cheval gagnant (ship, multi-surface, sans
> minefield) — mesurer `Vp` ce soir. Le sol **VDP2/RBG0** ne vaut le coup en 1P
> que si le **poke CYCxx direct** marche (Étape 4) ; en **MP c'est non** (limite
> hardware mono-registre/mono-matrice). Le cas **« map sans ciel »** est le point
> d'entrée le plus sûr pour un sol VDP2.
