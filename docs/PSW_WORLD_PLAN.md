# PSW — renderer monde tout-VDP1 « à la PowerSlave » (branche psw-world, 2026-08-31)

Décision propriétaire : GO expérimentation. Sol dominant reste **RBG0**. Nouveau code gaté
build (`SAT_PSW=1`, variante `-Psw`) + **toggle live R+C** sur le build PSW (A/B même-spot,
loi maison anti bruit inter-build). Le build normal ne contient AUCUN octet PSW.
Dossier parent : `POWERSLAVE_GAP_VERDICT.md` (§6 = géométrie + décision RBG0).
Recon 4 cartes (A émission VDP1, B fvdp1/RBG0/VDP2, C core/build, D recettes SlaveDriver)
— workflow wf_6750c762, faits `fichier:ligne` cités ci-dessous.

## L'insight qui dimensionne tout

Le pipeline murs actuel est DÉJÀ un pipeline de quads peintre :
- `sat_wall_hook` (core/r_segs.c:461) reçoit des trapèzes ÉCRAN
  `(x1,yl1,yh1,x2,yl2,yh2,texnum,u1,u2,v0,v1,cmap)` ;
- `wall_acc[128]` (dg:6220) accumule en ordre BSP **near-first** → un overflow refuse
  les murs LOINTAINS (le bon failure mode : trou au loin → RBG0/ciel derrière) ;
- `vdp1_walls_flush` (dg:7860) émet en **ordre inverse = loin→près** (dg:7997) ;
- l'émission (`wall_emit*` dg:6541-6914) fait déjà tuiles-u monde-ancrées + UserClip +
  lumière CRAM (`wall_light_colr` dg:6109) + budgets.

Ce que PSW supprime : le PRODUCTEUR par-colonne (`R_StoreWallRange` = Bp 11-53 ms,
silhouettes/visplanes/openings/drawsegs) — remplacé par une projection par-seg à 2 points.
Point de coupe unique : **`RP_QueueWall(start,stop)` r_segs.c:2713** (seul appelant de
StoreWallRange, curline/frontsector/backsector/rw_angle1 vivants).

## Étape 1 — murs peintre + RBG0 + ciel (CETTE SESSION)

1. **Build** : Makefile pattern WARP_FLAG (:111-115) → `SAT_PSW ?=` / `-DSAT_PSW=1` dans
   `SRL_CUSTOM_CCFLAGS` (:181-189, nourrit core .c ET src .cxx) ; build.ps1 `[switch]$Psw`
   + touch des fichiers gatés (make ne trace pas CFLAGS). Pré-vol pool intégré (:364-391).
2. **core/r_segs.c** : branche PSW dans RP_QueueWall → `R_PswStoreWall(start,stop)` :
   prologue de StoreWallRange SANS boucle par colonne — rw_distance, scale aux 2 bouts
   (R_ScaleFromGlobalAngle), worldtop/bottom par tier (mid 1-side ; upper si back ceil <
   front ceil et pas double-ciel ; lower si back floor > front floor), pegging → v0/v1,
   u aux 2 bouts (rw_offset − tan·dist), cmap par échelle mi-seg → appelle `sat_wall_hook`
   par tier. Mid 2-sides (grilles) : SKIPPÉ étape 1 (transparence, étape 3+).
   `#if SAT_PSW` + gate runtime `sat_psw_active`.
3. **Coupes runtime (mode ON)** : R_DrawPlanes court-circuité (corps r_plane.c:1443) ;
   spans/colonnes déjà morts (`sat_wall_skip=1` par M7) ; things : la passe occlusion
   :2034 no-op sans drawsegs (clip = viewport seul — les monstres des sous-secteurs
   ÉLAGUÉS par R_CheckBBox ne sont jamais projetés, la fuite = partiellement-visibles
   seulement, ASSUMÉE étape 1) ; masked midtex disparaissent (drawsegs) — ASSUMÉ.
4. **Dominant RBG0 sans visplanes** : élection étape 1 = **secteur du joueur**
   (floorheight/pic/band, latch au changement de secteur, comme sat_dom_last_sec) →
   écrit `sat_vdp2_floor_h/_pic/_band` ; upload flat + RPT inchangés (autonomes,
   dg:4348/4295). Horizon : `sat_vdp2_floor_top_y = ligne d'horizon (centery projeté)` →
   nourrit `rbg0_floor_window_apply` (W1, dg:4868 — la recette existe déjà) et
   `sky_horizon_row` (:9907). Pas de punch — empilement DÉJÀ bon : quads VDP1 reg0
   prio 5 > RBG0 prio 4 > NBG0 ciel prio 3 (dg:5331-5429).
5. **Ciel** : hérité gratuit — pas d'émission F_SKY1 ⇒ région index-0/rien ⇒ NBG0
   transparaît (le mécanisme actuel r_plane.c:1612-1677 fait pareil).
6. **Blit/clear** : blit vue coupé, bande HUD conservée (inverser W5, dg:10307-10386) ;
   clear vue UNE fois à l'entrée du mode puis stop (dg:10400-10456) ; menu/intermission
   couverts par `hud_force` (plein blit, fond monde visible derrière = ok).
7. **Toggle** : chord **R+C** (libre, dg:10925) → `sat_psw_req` latché, appliqué en début
   de frame (classe mode-switch corruption : un écrivain, jamais mid-frame).
8. **Overlay** : rows 13/14 (r_plane, mortes quand les plans sont coupés) = tenant PSW
   `PSW s<subs> w<emis>/<acc> re<refus> [ms]` ; légende ATLAS.md même session.

> **STATUT 2026-08-31 soir** : étape 1 **VALIDÉE CONSOLE** (captures owner : Bp 11,7-25,4 →
> 1,2-1,8 ms, MST 33 verrouillé = 30 fps). Étape 2 **TESTÉE CONSOLE** (« ok, mieux ») après
> deux fixes : traits = overflow 64-bit du clip d'intersection (core 28a8751) ; plafonds qui
> disparaissent = famine des 3 slots flats sous ordre peintre loin-d'abord → 4 slots +
> réservation proche→loin + fallback POLYGON plein-teinte (81f06f8, re-test console attendu).
> Row 13 = `PSW t r f d`. Capture ON de référence : Bp 1,4, MST 50, t26 r0 f6.
> Étape **3a CONSTRUITE** (1be0ea3/6c9e19e, pool 44,25 Ko) : things drainés au rang peintre
> de leur sous-secteur (queue thing_acc + watermark vissprite `s0`, restore UserClip par
> batch, shave 3 cmds/thing, drops → THp `x`).
> **A/B console 3a (2026-08-31 soir)** : couloir ON 50 fps vs OFF 30 (+66 %) ; scène du
> spawn ON **15 fps** (MST66, f69, VD1 38 ms, g35) vs OFF 20 — le peintre n'a AUCUNE
> occlusion de flats, une scène ouverte émet des dizaines de polygones recouverts (loi L5
> plot-time ; les budgets par-commande sont aveugles au fill). Réponse : **budget de FILL
> flats** (96b9346/a8aa38b) — 96 000 px estimés dépensés PROCHE→LOIN dans la pré-passe,
> coupe = row 13 `k` (perte = flats des salles les plus lointaines, fuite RBG0/ciel).
> Pool 43,2 Ko. **Round 2 console (2026-09-01)** : perf PROUVÉE (spawn MST66→24-33, VD1
> 38→6-11 ms) mais estimateur centroïde ×40 trop haut sur le proche ⇒ f1/k32-45 = plafonds
> absents partout. Fix 8c3203b/baa31d1 : estimateur = bbox écran réellement projetée par
> plan (psw_plane_px), pools aire/centroïde retirés.
> **Round 3 console (2026-09-01)** : bbox brute encore trop haute (clamps ±1024 facturés
> ⇒ f1-f8/k29-43) ; murs manquants par angle/distance = refus WALL_PX_BUDGET (row 13 r6,
> fallback software inexistant en PSW) ; sol VDP1 étiré par-dessus RBG0 = jumeau de bande
> du dominant. Fix 71877b8 : (1) refus px murs skippé sous PSW, (2) bbox clampée à la vue
> + budget 56000 px-colonnes ≈ 2 vues lowres + cull des plans hors écran, (3) match
> dominant sans la bande.
> **Round 4 console (2026-09-01)** : r0 ✔, patch RBG0 disparu ✔ ; restaient f64/k0 avec
> VD1 36 ms (le walk paie le quad ENTIER, queues hors écran comprises — le budget
> « visible » était aveugle) et d40 (dalle de flats plus jamais remplie en PSW). Fix
> 61dfd18/69f826d : psw_plane_poly = clip MONDE par plan (near à ph·hw2/rows + 2 bords de
> frustum 90°) ⇒ plus de queue hors écran, walk ≈ visible ; prefetch des flats des
> visplanes via R_FlatCacheGet dans le bloc PSW de R_DrawPlanes.
> **GRILLE-64 (décision owner 2026-09-01, « partir du format PowerSlave », 58c22e0)** :
> les flats émettent en TUILES 64×64 ancrées grille MONDE — tuile intérieure = caractère
> entier 1:1 (u=wx&63, v=(−wy)&63, la phase R_MapPlane exacte ⇒ raccord parfait entre
> tuiles et avec le RBG0), tuile de bord = polygone∩tuile (intersections SNAPPÉES sur la
> ligne de grille ⇒ coutures bit-exactes), fan du caractère entier = warp borné à 64u aux
> frontières de secteur. Budget = COMPTE DE TUILES (PSW_TILE_BUDGET 140 near→far ; un plan
> > PSW_TILE_PLANE_MAX 48 retombe sur UN fan étiré). L'étirement pleine-salle est mort par
> construction. Reste : test console, 3b midtex, sous-rects 8-texels pour les tuiles de
> bord si le warp gêne, verdict.
> **Round 6 (2026-09-02, ddf77aa)** : (1) TOGGLE R+C SUPPRIMÉ — -Psw boote peintre-ON en
> permanence (demande owner : transitions = classe corruption, captures ambiguës) ; A/B =
> les deux disques. (2) **POINÇON RBG0** : un sol plus bas que le dominant, caché derrière
> son rebord, s'affichait SUR le sol VDP2 (le dominant n'émettait rien ⇒ rien ne recouvrait
> les tuiles fantômes, VDP1 > RBG0 en couche) — les sous-secteurs dominants émettent leur
> polygone en POLYGON couleur-0 SPD (recette erase) à leur rang peintre, uniquement les
> frames où un sol plus bas est en vue. Row 13 += `u<n>` ; L+X = poinçons JAUNES.
> **Round 7 (2026-09-02, c123ecb/4af6840/f4ebafa)** : CULL DE LIGNE DE VISÉE des sols —
> 1-3 sondes BSP par point (milieu du trajet, croisement exact à la hauteur d'un sol plus
> haut trouvé à mi-chemin, croisement au rebord du dominant), échelle plan→tuile (2 sondes
> par plan ; par-tuile seulement si mitigé). Tue les tuiles cachées AVANT slot/projection/
> clip ; le poinçon ne s'arme que si un sol bas survit. Occulteur fin raté = overdraw
> d'avant (jamais pire) ; sur-cull possible d'une frange de fosse (proxy coin-le-plus-loin).
> Plafonds symétriques non faits. Reste : test console, 3b midtex, verdict.
> **Round 8 (2026-09-02, d996700/6a6698b)** : (1) cull PLAFONDS symétrique (sondes miroir ;
> R_PswCeilingAt fait occlure le CIEL à sa hauteur = la convention sky-hack gratuite) ;
> (2) **OCCLUSION MURS façon portails** : 40 buckets écran de 8 px, une bande verticale
> ouverte [t,b] chacun — la pré-passe near→far teste plans (bbox projetée) et murs contre
> les bandes des occulteurs strictement plus proches, puis PLIE les murs survivants ; les
> tiers upper/lower d'une fenêtre rétrécissent la bande = le portail PowerSlave émergent ;
> murs lointains 100 % cachés cullés aussi (wall_cull[], row 13 `c<n>`). Pli = lignes
> INTÉRIEURES sur buckets couverts, test = lignes EXTÉRIEURES sur buckets touchés,
> middle-splits ignorés ⇒ conservateur des deux côtés. Verdicts plans dans psw_sub_flag[]
> (pré-passe calcule, émetteur applique). Row 13 = `PSW t r f d k u c`.
> **Round 9 console (2026-09-02)** : 6 symptômes — trous murs/plafonds (ciel/RBG0 au
> travers), plafonds/sols entiers manquants, textures désalignées entre cellules, things
> qui disparaissent / affichés SUR les murs, porte plein écran ×3, coin de quad qui déborde
> sur le sol voisin. Diagnostic : (a) FAMINE DE BANQUE — flats f91-133 sur 256 cmds
> partagées, émission far→near ⇒ le PROCHE sautait ; (b) cull LOS trop dur (sommet lointain
> seul tuait le plan entier) ; (c) buckets murs c0 = jamais un cull par construction (sans
> pli des flats, une bande pleine hauteur n'est jamais touchée en milieu d'écran) ; (d)
> seuil tuile-pleine −32 unités² ⇒ carré débordant ; (e) hyper-magnification ⇒ squish
> pleine-texture par pièce de subdivision. Fix efbc42d (core) + c7926bd : (1) **BANDES
> PORTALES PAR COLONNE dans le core** — [bt,bb] par colonne, pli par seg dans
> R_PswWallRange (one-sided scelle ; two-sided rétrécit au portail via tiers + régions
> plafond/sol du secteur avant = la sémantique ceilingclip/floorclip vanilla) ; tiers
> testés/cullés dans R_PswEmitTier AVANT le hook (`c` = sat_psw_wcull) ; plans testés au
> moment de la NOTE (état strictement plus proche — au flush les bandes seraient fausses)
> via R_PswBandBoxHidden ; verdicts stockés psw_sub_flag/fe/ce, pré-passe = pur budget.
> (2) banque 304 cmds (PSW seul, 0x4100-0x4FFF libre) + budget flats réel = banque − coût
> murs − réserve things, near→far. (3) full-cull LOS = lointain ET proche cachés. (4) seuil
> plein 4094. (5) tuile de bord RECTANGLE AXIAL = texels exacts (sous-bande v du char +
> quad bande pleine largeur + fenêtre UserClip) ; diagonales gardent le fan borné. (6) gate
> wall_hypermag (texw·xspan > 640·du ou du==0) ⇒ mur FLAT (pas de fallback SW en PSW).
> Pool -Psw 34,64 Ko ; build normal intact. Reste : test console, 3b midtex, verdict.
> **Round 10 console (2026-09-02, 82b7639)** : « beaucoup mieux » — restaient des sols/
> plafonds manquants quand PARTIELLEMENT couverts = la sonde par POINT généralisée à
> l'AIRE. Fix : full-cull plan = TOUS les sommets prouvés cachés (un visible ⇒ raffinement
> par tuile) ; skip tuile = les DEUX coins diagonaux cachés (tuile visible = toujours 1
> sonde, court-circuit). À surveiller : MST79-84 / 12,6 fps dans la salle aux caisses
> (k52-54 = budget tuiles saturé — beaucoup de subs) → prochaine cible perf après la
> validation correction.
> **Round 11 console (2026-09-02, 60e9046)** : 4 symptômes → 3 mécanismes. (1) grands
> plans désalignés = le raccourci « >48 tuiles ⇒ UN fan étiré » recréait l'étirement
> pleine-salle sur chaque grand plafond — SUPPRIMÉ, les géants tuilent. (2) plans limités
> en distance + grands plafonds troués = le budget TUAIT les subs lointains et le cap
> tronquait un plan en pleine marche — le budget DÉGRADE maintenant (plan trop cher → fan
> étiré 2 cmds, b4/b5 ; l'émetteur réserve 2 cmds/tuile avant d'entamer une marche ; `k` =
> seulement les plans qui ne peuvent même pas payer leur fan). (3) swim des quads
> débordant l'écran = le fan ré-étirait le caractère sur le bord de coupe MOUVANT du
> near-clip/frustum — une pièce PROPRE (arêtes non-axiales toutes sur des lignes de clip
> de vue ⇒ fuite hors écran par construction) émet ancrée monde : sous-bande v snappée aux
> texels + pleine largeur tuile + fenêtre UserClip au bbox projeté (généralise le rect du
> round 9) ; seule une frontière de secteur DIAGONALE garde le fan borné. Pool 34,05 Ko.
> **Round 12 console (2026-09-02, f247632 core / c2d6a55)** : « toujours les mêmes
> problèmes, swim et trous dans les plafonds » → arrêt des fixes à l'aveugle. (1) TROUS =
> vrai bug trouvé, constant depuis le round 3 : le builder de polygones TRONQUAIT LA QUEUE
> des feuilles > 20 sommets (corde ⇒ un COIN de sous-secteur manquant, endroit fixe, sol
> ET plafond — RBG0 masque le sol, d'où « trous dans les plafonds » seulement). Remplacé
> par une décimation convexe (retrait des coins les plus plats), + garde mi-chaîne pour
> que psw_clip_line ne perde jamais de sommets en silence. (2) SWIM = 2 candidats aux
> remèdes opposés (ondulation affine des grands quads proches → subdivision en profondeur ;
> ré-étirement du fan sur bords de coupe diagonaux → ancrage) ⇒ le disque DISCRIMINE :
> L+X peint par CHEMIN (ROUGE tuile pleine / BLANC bande+fenêtre / MAGENTA fan ; murs
> verts, things bleus, poinçons jaunes), row 13 += `b<bandes> n<fans>`. Question console :
> les quads qui swiment sont de quelle couleur ? Pool 33,22 Ko.
> **Round 13 console (2026-09-02, 16749b9)** : réponse = « c'est le MAGENTA qui swim »
> (les fans) + trous restants derrière des obstacles et à distance. Fix : (1) **le fan
> texturé n'existe plus** — toute pièce de bord = bande+fenêtre ancrée monde (gate clean
> supprimée ; prix = fuite bornée ≤64u sur arête diagonale : invisible sur splitlines BSP,
> couverte par le mur de marche sur dénivelé ; à surveiller sur trim diagonal même hauteur
> et bord de ciel diagonal) ; dégradé budget = APLAT couleur-texel (un aplat ne swim pas) ;
> bande improjetable/famine = aplat aussi. (2) trous derrière obstacles : sonde du CENTRE
> ajoutée (tuile : 2 coins + centre ; plan : tous sommets + centroïde). (3) trous à
> distance : PSW_TILE_BUDGET 140→220 (vestige pré-cull), FLAT_CAP 232, SUB_MAX 240 (tail
> = murs sans flats). L+X : MAGENTA = les aplats désormais. Pool 32,05 Ko.

## Étape 2 — sols non-dominants + plafonds (quads de sous-secteurs)

- **Polygones de sous-secteurs au level-load** (p_setup.c après :1234, PU_LEVEL zone
  LWRAM) : clip récursif du bbox map par les splitlines ancêtres (node_t x16/y16/dx16/
  dy16 exacts) + les segs de la feuille. ~150 lignes, une fois par niveau.
- Hook R_Subsector (r_bsp.c:586) : ordre de visite → `psw_sub[]` (plateforme, ~2 Ko
  .bss : sous-secteur + watermark wall_acc + plage vissprites).
- Flush PSW : marche `psw_sub[]` en ordre INVERSE ; par sous-secteur : quad sol étiré
  (si non-dominant, non-ciel) + quad plafond (si non-ciel) puis ses murs (les murs
  gagnent les seams) — briques `fvdp1_project` (dg:7155, psign ±1), `fvdp1_slot_get`
  (dg:7275, 3 slots LRU 0x25C7D000, JAMAIS d'I/O), modèle `fvdp1_emit_tile` (dg:7334),
  lumière formule plan dg:7341. Polygone >4 côtés → fan 2 quads. Flat entier étiré
  (u=0..64/v=0..64 — la contrainte sans-stride dg:7240-7258 est respectée par
  construction). PAS de tile_raster/punch/interdits (peintre pur).
- Élection dominant upgrade : aire pondérée sur les sous-secteurs visités.

## Étape 3 — things peintre + midtex

Plages vissprites par sous-secteur (R_AddSprites garde validcount r_things.c:972) émises
au rang du sous-secteur dans le flush inverse (le lien JUMP_CALL SlaveDriver SPR.C:443
est la version chaînée ; nous contrôlons l'ordre linéairement, pas besoin). Midtex
2-côtés en quads transparents (8bpp index-0).

## Étape 4 — mesure et verdict

Sondes : ms de collecte (R_PswStoreWall) vs émission (`pr`/`em` row 1 existants), compte
quads par catégorie, LOPR (`vdp1_lp_pct` — transfer-over), MST/fps A/B via toggle R+C
même-spot sur CONSOLE. Critère : MST_PSW vs MST_M7 même scène ; trous/artefacts listés
(fuites things partielles, midtex absents, frontière RBG0 aux rebords — attendus).

## Ce qu'on ne touche PAS

Present v2/fence, banques root-link, erase polygone, WTEX resolve, cache things+UserClip,
arme/HUD, budgets AIMD/LOPR, gouverneur, tic. Le mode M7 reste le défaut au boot.
