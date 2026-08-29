# Le moteur FPS de Ponut64 (« stuff ») — analyse comparative Mimas

**Date** : 2026-08-29. **Objet** : `github.com/ponut64/stuff`, clone local
`../ponut64-stuff`, commit unique `ed6124b` « put up for e1m1 » (2026-08-28).
Ponut64 vient d'y mapper E1M1 du shareware sur son moteur 3D. Analyse par 9
agents + contre-vérification adversariale des 4 dimensions critiques (les
affirmations réfutées sont corrigées ici). Faits SGL/VDP2 recoupés consignés
dans `../saturn-refs/knowledge/` (HW_VDP2 §5, SRL_SGL_GOTCHAS §3).

---

## 1. Vue d'ensemble — ce que c'est, et ce que ça n'est pas

Un **portal/PVS engine sectorisé, tout-VDP1** : le monde est un mesh de quads
(64 secteurs max), chaque quad porte son secteur, ses règles de subdivision et
sa texture ; visibilité = PVS d'adjacence profondeur-2 + portails testés par
frame. **Pas de BSP, pas de WAD, pas de colonne software** : `DOOM.GVP` est un
**remesh low-poly d'E1M1 fait sous Blender** (35 Ko : 993 sommets, 782 quads,
25 secteurs, 12 portails), converti par un outil externe au dépôt. C'est un
moteur généraliste 3D dans lequel E1M1 entre comme un modèle, pas un port de
Doom.

**État réel constaté (correction 2026-08-29)** : E1M1 **boote et se joue**,
à **~17 fps sur émulateur** (constaté par Romain ; cible moteur = 30 fps,
`SynchConst=2`). La note de dev `main.c:18-19` (« *loading Doom E1M1 — Memory
corruption occurs. Likely out of RAM* ») est donc **périmée ou non fatale** —
ne pas la citer comme état courant. Les autres notes de l'auteur
(`main.c:33-53`) restent son propre diagnostic des limites :
- « *A single light costs 1-2ms. Oof* » ; « *on the edge of in-budget ; I wish
  I could get just another two hundred polygons* ».
- « *only 1-3 enemies can be on screen at once… difficult to conceive of a
  game whose art style will carry a strict DOOM-clone approach* ».

Mise en perspective : ~17 fps émulateur pour un remesh de 782 quads sans le
gameplay Doom (pas de monstres Doom, pas de thinkers, pas de WAD), contre
Mimas à 7-16 fps **console** sur les données réelles et le jeu complet. Et
l'émulateur n'est pas un oracle : sur Mimas, l'écart Ymir/console mesuré est
énorme (tic 8-14 ms Ymir vs 69-83 ms console) — son 17 fps émulateur est
probablement optimiste vs vrai HW.

Base SDK : Jo Engine **non compilé** — simple distribution (toolchain GCC 8.2 +
LIBSGL.A stock + linker stock). Trois patches seulement : `workarea.c` custom
(1500 polys, **Zbuffer 512 longwords = 2048 o** — confirmation indépendante de
notre pin SRL), `SL_DEF.H` exposant `SpriteBuf` comme `SPRITE_T` (commande
VDP1 + pointeur `NEXT`), makefile « blank SGL ». Tout le reste = SGL brut +
SBL-CD (GFS) — exactement la situation as-built de Mimas.

## 2. Les flats (question centrale)

**Il n'y a pas de chemin « flat »** : sols, plafonds et murs sont tous des
distorted sprites VDP1 8bpp. Le VDP2 ne rend **aucune surface** : NBG0 = ciel
bitmap 512×512 scrollé selon la vue, NBG1 = arme + HUD (fenêtré), NBG2 =
texte. Zéro RBG dans tout le code.

Le problème du gros quad VDP1 (distorsion perspective + temps de plot) est
traité par **subdivision à 6 niveaux** :
1. **3 niveaux pré-cuits au chargement**, en model-space, selon des règles
   2 bits/niveau (`|`, `—`, `+`) précalculées offline par quad
   (`preprocess_planes_to_tiles_for_sector`, renderSub.c:484) → tables de
   « tuiles » par secteur.
2. **3 niveaux dynamiques au runtime**, coupés seulement si le quad passe sous
   les seuils `z_rules = {512, 256, 128}` (renderSub.c:226) — taille écran
   ~constante, tessellation dégressive avec la distance.
3. La subdivision runtime se fait **obligatoirement en view-space** (« doing
   subdivision in screen-space does not work, warping », renderSub.c:1913),
   milieux moyennés en 3D puis reprojetés via une LUT 1/z.

**Textures** : chaque texture 64×64 (= taille exacte d'un flat Doom) est
**pré-découpée à l'init** en pyramide de sous-textures downscalées ≤32×32
(`uv_cut`, tga.c:988 ; 224 sous-IDs par base) ; le quad subdivisé sélectionne
la sienne par table. Effets gratuits : mipmapping (un quad lointain non
subdivisé lit la 32×32) et **densité de texels ~constante ⇒ temps de plot
borné par tuile**. Nuance vérifiée : la voie `uv_tile` « 225 IDs aliasant 16
réductions » ne vaut que pour les textures tuilées 8 px ; la voie principale
découpe réellement.

**Z-sort** : clé par quad = `(max(4Z) + moy(4Z))/2` (« weighted max »,
renderSub.c:2158) → insertion O(1) en tête de listes chaînées dans les buckets
du Zbuffer SGL. Overdraw contrôlé en amont (PVS, abandon de secteurs entiers
dont tous les portails sont hors écran, backface par plan).

**Verdict vs Mimas** : il paie chaque texel de sol en temps de plot VDP1 —
notre mur n°1 (transfer-over). L'hybride Mimas (sol dominant RBG0 gratuit +
sols secondaires VDP1 + fallback spans) est structurellement en avance. Sa
pré-tessellation au chargement est exactement ce que notre verdict
« pré-tessellation MORTE » a enterré — et chez lui, même sur un E1M1
*remodelé low-poly* de 35 Ko, elle a mis la RAM assez sous pression pour
qu'il note « out of RAM » pendant le dev (note depuis dépassée : ça charge).

## 3. Le brouillard de distance (question centrale)

Le mécanisme le plus élégant du moteur. **Ni gouraud, ni palette de fog, ni
half-transparency VDP1** : quatre champs recyclés dans le mot CMDCOLR de
chaque commande (mode color-bank 64 couleurs), décodés par le VDP2 en **sprite
type 4** :

| bits | rôle |
|---|---|
| 0-5 | texel (64 couleurs) |
| 6-7 | **banque CRAM** parmi 4 copies pré-assombries de la palette (140/120/100/80 %, peintes par l'artiste) |
| 10-12 | **sélecteur des 8 registres de ratio color-calc** (`slColRateSpr0..7` = 2..28/31) |
| 13 | bit de **priorité** → SPR1=3, sous la condition CC « priorité ≤ 4 » ⇒ **arme le color calc** |
| 15 | **MSB shadow** = demi-luminance (double les 4 banques en 8 niveaux) |

Production : `depth_cueing()` (render.c:623) + `determine_colorbank()`
(render.c:598) ≈ 6 instructions **par commande**. Consommation : gratuite,
par pixel, dans le VDP2. Le polygone lointain (z>1200, ~19 m) se **fond à
~90 % dans le ciel NBG0** — haze vers le décor, pas fondu au noir. Lumière =
8 niveaux par polygone, zéro coût par pixel, zéro écriture de texel.

**Défaut vérifié (contre-vérification)** : la rampe 8 niveaux est **binaire de
fait** — le cue sature à 7 dès z=1096 alors que le gate de priorité n'ouvre le
CC qu'à z>1200 : seul le ratio 28/31 sert jamais, les registres 2-23 sont
morts. Leçon générale : gate et rampe doivent se recouvrir. Autre trou : les
billboards/particules passent `colorBank` brut — pas de fog sur les sprites.

**vs Doom** : colormap = 32 niveaux payés par pixel en CPU, fondu au noir.
Ponut = 8 niveaux par polygone, coût par pixel nul, fondu au ciel.
**Correction (dossiers du soir, `PONUT64_STEALS.md` §1)** : la première
version de ce paragraphe affirmait que « nos murs/things VDP1 n'ont aucun
light diminishing » — c'est FAUX. Mimas l'a déjà, par le même mécanisme :
bank 1 vive + 6 banques CRAM = copies de PLAYPAL passées par les colormaps
Doom exacts (niveaux 5/10/16/21/26/31), sélectionnées par `wall_light_colr`
dans CMDCOLR sur murs, things et sols VDP1 (dg_saturn.cxx:6035-6098). Le
seul delta vs Ponut est le doublement par MSB shadow (7 → 11-12 crans, moitié
sombre) et la partie « fade » par bits CC — voir les dossiers 1 et 2.

## 4. Gestion VDP1

- Commandes écrites **au format final** dans le `SpriteBuf` SGL via
  `SPRITE_T`+`NEXT`, chaînées à la main dans les buckets du Zbuffer (SGL
  réduit à backend slSynch + DMA). Buffer partitionné statiquement **sans
  verrou** : master slots [0,600), slave [600,1500). Un seul écrivain du
  buffer de tri (le slave), qui fusionne les commandes du master après un spin
  sur flag uncached.
- Présentation **100 % SGL stock** : `slSynch`, `SynchConst=2` (30 fps),
  `slDynamicFrame(ON)`. Aucun poke FBCR/PTMR. Seuls registres touchés :
  EWLR/EWRR réécrits chaque vblank (recoupe notre
  `vdp1-erase-under-slsynch`).
- **`preclipping()`** (render.c:632) : quad entièrement visible → bit 11 PMOD
  « pre-clipping disable » ; sinon rotation de l'ordre des sommets pour que v0
  soit à l'écran (xor des flips H/V) — les deux réduisent le temps de plot.
- Budget : caps durs 900/600, **drop silencieux de la queue** au dépassement,
  aucune priorisation.
- ⚠ Deux mécanismes cités par les analystes sont du **CODE MORT** chez lui
  (définis, jamais appelés — vérifié par grep) : `setFramebufferEraseRegion`
  (fenêtre d'effacement EWLR/EWRR restreinte ; défauts = plein écran) et
  `setUserClippingAtDepth` (UserClip inséré à une profondeur du flux trié).
  Les *idées* restent bonnes ; l'implémentation active n'existe pas.

## 5. Slave SH-2 et SCU-DSP

Répartition **inverse de Mimas** : le slave porte l'émission (toute la
géométrie monde + z-sort), le master transforme les vertices de secteurs
(asm MAC.L + LUT), fait le viewmodel (NBG1, pas VDP1), les movers/entités de
sa partition, puis la logique jeu. Synchro = flags uncached + spins `nop`,
snapshot des listes au départ du slave (anti-course).

**SCU-DSP** : la seule utilisation vivante du DSP vue en homebrew Saturn. Le
programme « winder » (537 lignes d'asm DSP commentées) teste le winding de
chaque vertex écran contre ≤6 portails et **écrit les clipFlags en place par
DMA pendant que le slave travaille** ; le slave consomme tuile par tuile en
spin. Un vrai pipeline producteur/consommateur à 3 processeurs — mais **aucun
gain mesuré nulle part dans le dépôt**, et l'auteur note lui-même qu'un schéma
par-polygone serait plus rapide « car le CPU ne devrait pas attendre ». Kick
du DSP **par le slave** (pokes SCU depuis SSH2 — fait HW intéressant, non
prouvé ailleurs). Notre verdict DSP (sight = 0,27× MSH2) n'est pas remis en
cause : autre forme (flags async fire-and-forget), pas de valeur démontrée.

**zTable** : LUT réciproque 1/z de **256 Ko en LWRAM** (`scrn_dist/i` pour
tout i 16 bits signé) — la projection perspective devient un lookup sur le
chemin secteurs. Les chemins entités/sprites gardent le **DIVU entrelacé**
(lancer la division du vertex suivant pendant les MAC du courant,
render.c:806-828).

## 6. Pipeline d'assets

Binaire GVP **big-endian pré-swappé, mappé en place** (fixup de pointeurs
seulement, zéro parsing) ; normales et classes d'axe précalculées. Mais la
sectorisation riche (adjacence, « PVS », tuiles) est **reconstruite au boot en
O(N²)** — un substitut artisanal du BSP que le WAD nous donne déjà cuit.
Textures : palette globale unique 256 = 4 banques × 64 pré-graduées
(`tools/palettes.txt`), packées dans un GVP factice, upload direct VDP1.
Spawns embarqués dans le modèle (l'unique item de doom.GVP = player start).

## 7. Le driver son « ponèSound » — la vraie pépite

Binaire 68K de 8,8 Ko (`SDRV.BIN`) **qui tourne réellement sur le sound CPU**
— source C m68k **MIT** : `github.com/ponut64/SCSP_poneSound`. Le SH2 ne fait
qu'écrire une struct partagée en sound RAM non-cachée (93 slots logiques
`_PCM_CTRL` : adresse, pitchword OCT/FNS, pan, volume, `sh2_permit`) + un tick
vblank ; le **68K fait l'allocation des slots matériels, le timing
(`bytes_per_blank`), le keying, la décompression ADX 4-bit temps réel, et le
mixage CDDA (vol/pan par canal)**. Classes de comportement par slot
(VOLATILE / SEMI / PROTECTED / boucles), instancing 3D au-dessus du slot 64,
`pcm_reset` qui préserve N sons résidents au changement de niveau. Le driver
ne réserve que ~48 Ko de sound RAM → **~464 Ko libres pour les samples**.

**Correction (dossier 5 de `PONUT64_STEALS.md`, source 68K lue)** : la
première version de ce paragraphe imputait le drop de 34-39 % des percussions
au « driver SGL/SRL » — c'est FAUX. En mode MUS le 68K est **halté**
(i_sound_saturn.cxx:527) ; les percussions sont jetées par notre propre
séquenceur `mus_step`, qui saute le canal 15 faute de timbres de batterie
(i_sound_saturn.cxx:408/:413). ponèSound n'embarque ni séquenceur MUS ni
banque de drums : son adoption ne joue pas une percussion de plus, et sa
réservation de 47 Ko **réduit** la RAM samples de ~53 Ko (471 000 o contre
523 936 aujourd'hui, pour 532 927 o de SFX shareware). Sa politique au-delà
de 32 slots HW = refus + retry sans priorité (PROJ/main.c:532-546), les
volatile (nos SFX) passant en dernier. Le chantier audio valide est la
Route P du dossier 2026-08-25 : banque de 8 drums sur les 9 slots HW libres
23-31 de la chaîne direct-slot existante. Ce qui reste vrai de ponèSound :
une belle référence de contrat SH2↔68K (struct partagée + tick vblank), MIT.

## 8. Ce que ça valide chez Mimas (contre-preuves externes)

| Verdict Mimas | Confirmation Ponut64 |
|---|---|
| Pré-tessellation coûteuse en RAM | Même son E1M1 low-poly de 35 Ko a provoqué une note « out of RAM » en dev (main.c:18-19, depuis résolue) — la pression mémoire de l'expansion est réelle, à l'échelle d'un tas de 200 Ko |
| Flats tout-VDP1 = plot-bound | Il paie chaque texel de sol en plot ; nous avons mesuré le transfer-over |
| Frontière polygonal vs colonne | ~17 fps émulateur (cible 30) sur un remesh 782 quads sans gameplay Doom ; ses notes : 1-3 ennemis à l'écran, +200 polys manquants, level design restreint |
| Pin SRL Zbuffer 2048 o | Sa workarea.c alloue 512 longwords, dérivé indépendamment (+ offset buckets +128 non documenté) |
| `vdp1-erase-under-slsynch` | Il réécrit EWLR/EWRR chaque vblank lui-même |
| Les quatre horloges / pokes VDP2 | « SGL has set these and likely sets them at VBLANK, so be careful » (vdp2.c:33) |
| LWRAM = données froides | « LWRAM is SLOW!!!! » (bounder.c:407) |
| Grille uniforme pour le rendu | Vestigiale chez lui aussi (CELL_SIZE défini, zéro usage rendu) |

## 9. À récupérer pour Mimas — classement (RÉVISÉ après instruction)

Les 9 pistes de la première version ont été instruites une par une contre le
code as-built : **[PONUT64_STEALS.md](PONUT64_STEALS.md)** (dossiers complets,
fichier:ligne, arithmétique, plan d'expérience, colonne « Apport »). Résumé
des verdicts — la première liste avait **quatre prémisses fausses** :

| # | Piste | Apport | Verdict |
|---|---|---|---|
| 1 | Banques CRAM (+ MSB shadow) | qualité (seam ±9 % → ±6 %) | **déjà shippé** sauf le MSB : CONDITIONNEL (sonde SPCTL + toggle) |
| 2 | Fade-out things (bits CC + prio) | qualité + feature, 0 CPU | CONDITIONNEL (type 3 confirmé, veil survit CC_TOP) |
| 3 | Pre-clipping bit 11 | perf VDP1 0,2-1,2 ms, **+0,1 ms master** | CONDITIONNEL étape 1 ; rotation v0 NO-GO |
| 4 | DIVU entrelacé | perf MSH2 < 1,2 ms | **NO-GO** — déjà réfuté console (dv1, 07-15) |
| 5 | ponèSound | audio : 0 sur le symptôme, −53 Ko samples | **NO-GO** → Route P (drums, slots 23-31) |
| 6 | Sols VDP1 texturés (uv_cut) | perf plot d'un mode parké | **NO-GO** — déjà texturés, mode parké 08-24 |
| 7 | Fenêtre d'effacement | 0 ms (A) / 0,4-0,9 ms plot (B) | **NO-GO valeur** (marge plot 7-8×) |
| 8 | LUT réciproque étroite | perf MSH2 0,8-5 ms selon C inconnu | CONDITIONNEL (compter C d'abord ; DIVU = concurrent à 0 RAM) |
| 9 | Lumps Saturn-natifs | temps de chargement | **NO-GO** — bake_levels.py mesuré et rejeté 08-18 ; résiduel SIDEDEFS |

**Déport MSH2 : aucune piste n'en fait.** Ce qui reste vraiment à prendre :
deux expériences de qualité visuelle à coût CPU nul (MSB shadow, fade des
things — toutes deux gated par une sonde SPCTL de 3 lignes, le type sprite réel
n'ayant jamais été lu) et une sonde de comptage (`dv`/`dm`) pour trancher la
LUT. Le mécanisme de tri à écrivain unique (buckets chaînés, plages de slots
disjointes) reste la référence si l'émission murs passait un jour au slave en
split — à chiffrer contre un slave qui monte déjà à 35 % en 4p.

**NOPE (acté, ne pas rouvrir)** : architecture portal/PVS, pré-tessellation,
zTable 256 Ko, host-loop audio, DSP pour le sight, grille uniforme.

## 10. Idées pour aider Ponut64 (dans l'autre sens)

À lui transmettre (SegaXtreme / issues GitHub), par gain décroissant :

1. **Bug de rampe de fog** : `DEPTH_CUE_OFFSET=200`, `>>23` ⇒ cue sature à 7
   dès z=1096, mais le CC ne s'arme qu'à z>1200 — ses 8 ratios n'en font
   qu'un. Fix 1 ligne : cutoff ≤ 1096, ou offset/pas élargis pour que la
   rampe couvre [cutoff, far] — il récupère un vrai dégradé de brume gratuit.
2. **Sa marge mémoire** : E1M1 charge désormais, mais sa note « out of RAM »
   (main.c:18-19) dit que le tas HWRAM de 200 Ko est au taquet à l'expansion
   (tuiles + buffers view/screen par secteur). Pistes : n'allouer les buffers
   de transform que pour les secteurs du PVS actif (pas les 25) ; auditer le
   .map au build (notre leçon « pré-vol obligatoire » : le pool se lit dans le
   linker map avant le boot) ; les 2×129 Ko de copies viewmodel en LWRAM sont
   une réserve évidente.
3. **Lumières à 1-2 ms** : son `lumatbl` (luma par quad) existe dans le format
   mais est **zéroé au chargement** (mloader.c:796-799) — baker l'éclairage
   statique offline dedans, et ne calculer au runtime que les lumières
   dynamiques dans leur rayon, par événement plutôt qu'en passe continue.
4. **Drop silencieux au cap 900/600** : prioriser par distance/catégorie avant
   de couper la queue (notre leçon « les murs cèdent aux things » : un mur
   coupé se voit plus qu'un sprite coupé) ; au minimum compter les drops à
   l'écran.
5. **Fog des sprites/particules** : render2d passe `colorBank` brut — appliquer
   `depth_cueing` aussi sur ce chemin, sinon les billboards flottent net dans
   la brume.
6. **Brancher son code mort** : `setFramebufferEraseRegion` (et l'UserClip à
   profondeur) sont écrits mais jamais appelés — l'erase réduite au viewport
   3D est du temps VDP1 gratuit, il l'a déjà codée.
7. **S'il passe un jour en présentation manuelle** (il est aujourd'hui sur
   slSynch stock) : nos faits HW — CEF se latche (30-60 % en 1-cycle), le
   flicker vient du transfer-over (temps de plot), et l'effacement vise le
   buffer affiché — sont dans `saturn-refs/knowledge/HW_VDP1.md`.
8. **Mesurer le DSP** : un toggle vif winder-on/off donnerait en une session
   le chiffre qui manque à son dépôt (et à la communauté) sur la rentabilité
   réelle du SCU-DSP en culling.

## 11. Sources

- Dépôt analysé : `../ponut64-stuff` (`github.com/ponut64/stuff`, ed6124b).
- Driver son : `github.com/ponut64/SCSP_poneSound` (MIT) ; fil « ponèSound »
  sur SegaXtreme.
- Rapports bruts des 13 agents : journal du workflow `wf_9279ccbb-c3a`
  (session 2026-08-29).
