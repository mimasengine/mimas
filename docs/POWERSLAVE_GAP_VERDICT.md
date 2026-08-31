# PowerSlave vs Mimas — l'écart de perf 1p, et le procès du tout-VDP1 (2026-08-31)

Workflow 10 agents (4 lecteurs → 3 concepteurs → 3 juges adversariaux), déclenché par le
premier test console du fork SlaveDriver par le propriétaire (« perfs incroyables » vs
Mimas « catastrophiques »). Tout chiffre est tagué [HW]/[Ymir]/[src]/[OFF]/ESTIMÉ.
Références : `../SlaveDriver-Engine/docs/HW_USAGE_VS_MIMAS.md`, `RESOURCE_BUDGETS.md`,
`DOSSIER_MATERIEL_2026-08-25.md` §7, `VDP1_WORLD_PLAN.md` (bet A archivé).

---

## 1. L'écart expliqué — classe d'architecture, pas complexité de contenu

**SlaveDriver ne paie JAMAIS de per-pixel CPU.** Coût/frame ≈ k1·secteurs visibles +
k2·tuiles visibles + k3·cmds : flood-fill portails (bbox écran ∩ propagée par UNION,
WALLS.C:1770-1816), re-transform de tous les sommets chaque frame (rectTransform, cap
700 sommets/mur), 1 cmd VDP1 par tuile-monde 64×64 visible, occlusion = user-clip VDP1
par secteur + peintre loin→près = O(portails), pas O(pixels). Seule boucle per-pixel
CPU : le miss du cache texture (28 slots 16bpp + 31 slots 8bpp, décode RLE + expansion
8→16bpp, PIC.C:293-397).

**Mimas paie un plancher écran-proportionnel ~25-30 ms** quelle que soit la carte :
blit NBG1 + clear + émission `pr` (64,5 µs/cmd [HW]) + bookkeeping Bp par colonne
(clip-arrays/visplanes/silhouettes) + fill résiduel. Réf couloir [HW 07-29] : MST 41 =
Bw 2,4 + Bp 11,3 + P 14,0 + M 0,6 + dg 7 + blit 1,3. ⚠ Bp et P scalent AUSSI avec la
carte (Bp 11,3→19-53, P 14→21-28 sur TNT) — le plancher est fixe, pas le total.

**Le contenu est co-conçu avec le moteur** : secteurs convexes petits, jamais de grande
salle (Dreisbach : « huge open areas just can't work »), ≤126 textures/niveau, caps 600
secteurs/5500 murs, lumière radiosité pré-cuite par sommet, portes = push-blocks
translatés (pas de secteur mutant). Duke Saturn (même moteur) : niveaux « compressés »,
4 niveaux supprimés. `ENABLEFARCLIP 0` et `mipEnable 0` en retail : le moteur tient son
plot par le CONTENU, pas par des garde-fous runtime.

**Précédents historiques (R4, sourcés)** :
- Saturn Doom (Rage, Jim Bagley) : la V1 était VDP1+VDP2, « full screen at 60fps »
  (source unique : Bagley lui-même, interview RVG). Carmack a interdit le HW (warping
  affine) → réécriture software shippée à ~13 fps. Carmack, 2015 : « in hindsight, I
  probably should have let him experiment ». Le proto VDP1 est LOST.
- PowerSlave Saturn : ~30 fps quasi constant (HG101), lead platform.
- Quake Saturn : 20 fps, textures 64×64 4bpp/16 couleurs, découpe PRÉ-CUITE dans le .LEV.
- fafling v0.3 (05/2026) : Doom retail optimisé 100 % software → **~30 fps**, dips <10
  sur niveaux complexes. C'est le plafond software de référence — et celui de notre D3.
- Mimas = le seul « Doom quads VDP1 » public existant.

⚠ Correction à `HW_USAGE_VS_MIMAS.md` ligne VDP1 : « murs = fb logiciel » côté Mimas est
FAUX as-built — vérifié 08-31 : `vdp1_walls` majoritaire, fallback CPU ~100 colonnes
(dg_saturn.cxx:1010-1014, r_segs.c:1068-1075).

## 2. Les 6 lois NO-GO passées au crible du REMPLACEMENT (R1)

Question centrale : ces lois tiennent-elles si le renderer software (NBG1/blit/visplanes/
clip-arrays/punch) est SUPPRIMÉ et non plus greffé ?

| Loi | Verdict transfert |
|---|---|
| L1 bet A archivé (VDP1_WORLD_PLAN.md) | **Ne tient pas** — jamais construit, présupposait NBG1 maître d'occlusion + trou index-0. Survivent : Qp≈169 quads monde, q4 80-93 % sous-secteurs ≤4 côtés, loi anti-swim UV monde-ancrés, pas d'UV-wrap. |
| L2 sols tout-VDP1 (plot exposé 15-22 ms) | **Ne tient pas tel quel** — « exposé par le retrait du fill » suppose le fill ; sa propre condition de réouverture (A/B `g`=0) est structurellement satisfaite en remplacement. Survit : l'ordre de grandeur du plot sols, pas de stride VDP1. |
| L3 dicing offline (0,48 ms/tuile émise) | **Ne tient pas** — la plus greffe-dépendante (punch, régions interdites, prix « fill » : tout disparaît sans NBG1). Le DOSSIER §7.2-G nomme lui-même l'alternative non jugée : « réécriture de renderer ». |
| L4 64,5 µs/cmd | **Non transférable comme constante** — propriété de l'émetteur Mimas, pas de la puce (SlaveDriver ré-émet ≤1300 records ; à 64,5 µs = ~85 ms, impossible). Survit la forme : émission = CPU maître sériel ∝ cmds. |
| L5 transfer-over / budget plot | **TIENT et mord plus fort** (tout passe en plot). Gérée chez eux par tuiles uniformes + user-clip + contenu + gouverneur. |
| L6 CRAM 7 banques | **TIENT** pour tout 8bpp color-bank ; contournable en 16bpp gouraud (×2 VRAM/texel). |

Le seul essai HW « catastrophique » anti-tout-VDP1 (M1-M3, 07-07) est antérieur au
present v2 ET était une greffe. Correction : « far clip 1024 » de DOSSIER §7.2-G:614 est
FAUX (`ENABLEFARCLIP 0`).

## 3. Verdicts des trois propositions (tous GO_CONDITIONNEL)

### D1 — Second renderer tout-VDP1 (structure SlaveDriver) : le pari est LÉGITIME mais 4 inconnues d'abord

Arithmétique : couloir → disparaît ~20 ms mesurés, apparaît émission 180 cmds×64,5 µs =
11,6 + transform 1-2 (PAS 0 : contention B-bus mesurée dg_saturn.cxx:780-784) → MST_new
22-38 ms — le bas donne 30 fps, le haut RATE le cliff 33,4. Salle ouverte : émission
seule 32,3 ms à 64,5 µs/cmd → **tout le pari = descendre le coût/cmd à 15-25 µs**
(records 28-32 o pré-transformés slave + staging + DMA façon SPR.C). TNT reste tic-bound
(T 81-88 ms) : aucun renderer ne sauve les gros WADs.

Tueurs du juge (aucun n'exige d'écrire le renderer pour être levé) :
1. **RAM contradictoire** : « sélection au boot » ⇒ les deux .bss coexistent → +102 Ko
   sur un pool à 63 Ko = boot-loop. Exige overlay .bss explicite ou deux binaires, ÉCRIT
   avant toute ligne.
2. **« transform slave ≈ 0 » réfuté** par la mesure de contention B-bus du projet.
3. **Le bench jour-1 naïf rendrait un faux feu vert** (table pré-cuite = mesure le
   memcpy en excluant la décision ~13,5 µs + production du record).
4. **L'occlusion portail est load-bearing pour la CORRECTION**, pas une optim ultérieure
   (transfer-over mange la QUEUE = les murs les plus PROCHES — déjà mesuré chez nous).
5. **Fenêtre plot contradictoire** : 33,4 ms supposés vs 16,7 ms loi écrite — à trancher
   au LOPR sur 2 fields AVANT de valider l'arithmétique salle ouverte.
6. Baseline couloir de juillet sur code d'août ; cible chevauchant le cliff ; fafling
   fait déjà ~30 en software — le pari achète surtout une marge au-delà et une signature
   visuelle DIFFÉRENTE, avec le swim affine (l'objection Carmack) à juger à l'œil.

### D2 — Contenu et expérience décisive : volet (b) seul, corrigé

- **(a) KARNAK.LEV → WAD : PARKÉ.** Prédiction : 22-28 fps (le plancher écran-
  proportionnel ne bouge pas) — l'expérience prouverait le plancher, pas « 0 % contenu ».
  1-2 semaines, fidélité ~50 % (pentes/superpositions/gouraud perdus), et KARNAK.LEV =
  asset retail HORS GPL → WAD non distribuable (vidéo au mieux).
- **(b) STATUSTEXT sur le fork : LE bon premier coup.** `-DSTATUSTEXT` dans DEFINES
  (Makefile:47), arbre debug (NDEBUG off), garde JAPAN inoffensive en US. ⚠ Table de
  correspondance CORRIGÉE par le juge : `calc` INCLUT tic+jeu+ÉMISSION (SRUINS.C:2224),
  `draw` = attente résiduelle du plot (:2250) — le pr-équivalent n'est PAS observable
  séparément. La division **calc/polys** (µs/cmd effectif) ne se fait QUE sur scènes
  SANS monstres ; attendre le steady-state du cache texture (vswaps) ; smoke-boot avant
  de graver (l'ISR HBLK n'a jamais tourné sur le port GCC14). Le nombre se lit comme
  PLAFOND DE FAISABILITÉ d'un émetteur à records pré-transformés — pas comme prévision.
- **(c) Transformer nos WADs dans l'archi actuelle : NON pour la géométrie** (gain 3-8 ms
  sur cartes complexes via Bw/Bp, 0 sur le plancher). Payant : densité de thinkers (LOD
  p_tick.c:88 déjà shippé, manque chord+preuve) + réduction des textures distinctes par
  scène (lisse les pics em/Pv). Le monde pré-dicé est du contenu de l'architecture CIBLE.

### D3 — Le maximum sans réécriture : plafond ~30 fps de moyenne shareware

- État vérifié : cap tics ≥9 DÉJÀ shippé (d_loop.c:201) ; punch plafond RBG0 = NO-GO
  maintenu (gain = fraction de P−pr 1,6-3,3 ms) ; 68K/DSP clos ; sat_opt=5 défaut.
- **L-A émission** : le juge tue la forme DMA (32 o en 51 µs = 0,6 Mo/s, physiquement
  absurde pour du bus ; échelle bus mesurée ~10 %) — la masse des 64,5 µs est
  probablement du CPU transform. Sonde pré-enregistrée (~20 lignes) : rediriger les
  stores de la boucle plot de `vdp1_walls_flush` vers un scratch HWRAM, lire `pr`/`em`
  console. Effondrement → GO staging ; inchangé → pivot pré-formatage/offload slave
  (chiffré net +2,8 ms). Réaliste : −2 à −4 ms.
- **L-B gouverneur à paliers vblank** (cibler 33,4/50,1/66,8 ms au lieu de 95 render) :
  le SEUL levier qui convertit des ms en fps affichés. ⚠ 2-4 jours, pas 1 : le gouverneur
  pilote le RENDER seul (rp_rend10) alors que la frontière quantifie la frame TOTALE →
  horloge frame ou cible dynamique high-water(T+S+b+dg), hystérésis ≥10 frames.
  ⚠ Supprimer « cible par vue en split » (ré-introduit le bug corrigé du 08-24).
- **L-C Bp résiduel** : compter 0 tant que non mesuré console.
- Total réaliste : 41 → 34-39 ms ; avec L-B → 30 fps scènes légères, oscillation 20/30
  moyennes. **Scènes lourdes = tic-bound, intouchées** — c'est l'input de la décision D1
  (qui ne les règle pas non plus).

## 4. La semaine de mesures de mort (ordre)

1. **STATUSTEXT sur le fork** (D2b, <1 j, 0 pool) — µs/cmd effectif SlaveDriver sur
   scène vide + polys/frame réels à 30 fps (remplace le cap ≤1300 par un mesuré).
2. **Sonde scratch-HWRAM** sur la boucle plot de `vdp1_walls_flush` (D3, ~20 lignes) —
   bus vs CPU dans les 64,5 µs. Décision pré-enregistrée, pas de 3e voie improvisée.
3. **LOPR sur 2 fields** sous present v2 — trancher fenêtre plot 16,7 vs 33,4 ms.
4. **Re-baseline console** couloir + salle ouverte sur le build du jour (les réfs 07-29
   prédatent GetColumn/gouverneur/inc-2).
5. **L-B paliers vblank** en parallèle (se shippe quoi qu'il arrive).
6. Si ≤25 µs/cmd atteignable ET fenêtre plot OK → **plan mémoire .bss écrit** puis
   tranche verticale E1M1 tout-VDP1 AVEC occlusion portail dès le jour 1 (8-12 semaines).

## 5. Réponses courtes aux questions du 2026-08-31

- **Pourquoi eux si rapides ?** VDP1 dessine 100 % des pixels du monde, CPU = O(portails
  + tuiles), aucun per-pixel ; contenu co-conçu (petits secteurs convexes, 126 textures).
  Mimas paie ~25-30 ms de plancher écran-proportionnel (bookkeeping+blit+émission) avant
  de dessiner quoi que ce soit.
- **Drop le framebuffer / full VDP1 ?** Légitime — les NO-GO jugeaient des greffes, pas
  un remplacement (seuls transfer-over et CRAM survivent) — mais 4 inconnues mesurables
  en <1 semaine avant d'engager les 8-12 semaines.
- **Pourquoi sols/plafonds NO-GO chez nous ?** Parce que chez nous un sol VDP1 doit
  percer le framebuffer (punch), éviter RBG0/ciel (régions interdites), et n'économise
  que 1,6-3,3 ms de fill — le taux de change dest_rows/64 est structurellement mauvais
  en GREFFE. Eux n'ont ni punch, ni framebuffer, ni fill à économiser.
- **Transformer nos WADs ?** Seulement comme contenu de l'architecture cible (dicing
  offline en tuiles 64×64 + textures dédupliquées ≤126). Dans l'archi actuelle : non.
- **KARNAK → WAD ?** Prédiction 22-28 fps (plancher inchangé) ; parké au profit du
  STATUSTEXT A/B (1 jour, même réponse, chiffrée).

## 6. Géométrie Doom sous D1 + décision propriétaire RBG0 (2026-08-31, session suivante)

**La géométrie Doom passe quasi intacte** — D1 garde la sim vivante (dicing XY offline,
hauteurs relues par frame) : movers TOUS fonctionnels (le « intransposable » ne valait que
pour la conversion .LEV), sous-secteurs convexes 80-93 % quad-clean [bet A], lumière
secteur → gouraud = upgrade. Transformations offline sans perte : atlas tuiles 64×64
dédupliquées, pegging → offsets runtime, flats déjà grille-64. **Vrais renoncements** :
swim affine près (subdivision = cmds), pas d'UV-wrap (LOD loin : fusion 2×2/mips/étiré),
drop far-first sous budget, tricks de mappers quirk-dependent (flat bleeding, secteurs
auto-référents — IWADs propres, certains PWADs dégraderont), F_SKY1 = règle à porter,
mid-textures masquées = zone à risque tri intra-secteur.

**DÉCISION PROPRIÉTAIRE : sol dominant reste RBG0** (« en extérieur sur un grand plan, ça
fait toute la différence »). Validée : tue le pire cas arithmétique (salle ouverte
N≈500 → ~200-250 ESTIMÉ ; plafonds extérieurs = ciel = 0 cmd), esquive le taux de change
inc-2, ne retient AUCUNE machinerie software (pas de punch — on n'émet pas les quads du
dominant, RBG0 apparaît derrière) ; sol-RBG0/ciel-NBG0 par fenêtre coupée à l'horizon
(pas de freelook vanilla → ligne fixe), pattern W1 SlaveDriver. **Coût nommé** : frontière
d'occlusion VDP1/RBG0 — le peintre ne peut pas recouvrir ce que le dominant doit cacher
(fosses au-delà d'un rebord : user-clip rect fuit aux coins des arêtes obliques, murs
« flottant » sur le sol). Mitigations par coût : accepter (classe piédestal) → jupes VDP1
sur les seules arêtes de bordure → mini-clamp limité aux segs croisant le bord. Plafonds
intérieurs = ligne de budget sans filet (RBG1 mort).
