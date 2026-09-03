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
> **Round 14 console (2026-09-02, 2df88aa)** : « beaucoup mieux ; trous restants entre
> magenta et blanc ; jamais de rouge ». Jamais de rouge = les tuiles pleines n'existent
> quasiment pas (fragments BSP < carré 64 aligné) ⇒ ~chaque tuile = bande+fenêtre à
> 2 CMDS, facturée 1 par la pré-passe ⇒ dépense ~2× le facturé ⇒ le cap d'émission
> (far→near) tronquait le PROCHE = les trous à la frontière magenta/blanc (la famine du
> round 9 ressuscitée dans le budget flats). Fix : pré-passe en COMMANDES contre fbudget
> seul (PSW_TILE_BUDGET supprimé — une fenêtre ne coûte aucun walk), plan tuilé = 2e+1,
> aplat = 4 ; fan d'aplat DÉCIMÉ ≤4 quads (13 quads facturés 2 se faisaient tronquer en
> plein milieu) ; trigger d'émission aligné. NB multijoueur : PSW reste verrouillé 1p
> (latch) ; le MP du disque -Psw = chemin classique + banque 304 ; disque normal intact.
> **Round 15 console (2026-09-02, cf28e0e)** : encore des trous — row 13 `k12-22` avec
> `f34-97` = le budget jetait des plans SUR LE PAPIER (banque à moitié vide) : la
> facturation honnête 2e+1 gardait l'estimateur BBOX qui sur-charge ×3-6 les slivers
> diagonaux. Fix : `psw_tile_est` = tuiles TOUCHÉES (aire/64² + périmètre-L1/128 + 1,
> borne sup — facturer sous le coût = troncature du proche). Peinture bandes BLANC→ORANGE
> (216, le blanc noyait l'overlay). Attendu : k~0, les coins noirs morts. Pool 31,95 Ko.
> **Round 16 console (2026-09-02)** : encore des trous — `k8-18` avec `c113-154 / B296` =
> banque à MOITIÉ VIDE, famine papier 2e édition (l'estimateur tuiles-touchées facture 2/
> tuile mais le réel émet une fraction : sondes LOS des plans mixtes, slivers, clips vides ;
> les grands plans lointains facturent des dizaines de tuiles projetées sur ~10 px). Fix à
> 3 étages : (1) **LOD de distance à la note** — bbox projetée ≤16 px de haut (ou ≤1024 px²)
> ET plan non-mixte ⇒ SOLIDE d'office (bit 0x10/0x20), facturé 4, émis ≤4 quads (les mixtes
> sont exempts : le fan saute les sondes par-tuile ⇒ tache ineffaçable sur le ciel VDP2) ;
> (2) **standby + repêchage sur slack réel** — la pré-passe ne jette plus (bits 0x40/0x80),
> l'émission repêche en solide ssi `wnext réel + 4 + psw_paper_left ≤ cap` (ledger du papier
> committé non émis, décrémenté par sub APRÈS ses 2 passes ; murs/things jamais relâchés =
> double compte conservateur ; plafond mixte JAMAIS repêché — classe tache-sur-ciel) ;
> (3) **facture punch honnête** — le fan punch émettait ≤13 quads facturés 1 (violation
> silencieuse de la loi, `u26` réels pour ~10 facturés) : décimé ≤4 quads + facturé 4 avec
> pré-scan punch_frame AVANT la boucle budget. Bonus : un solide ne vole plus de slot
> texture (les 4 slots servent aux seuls tuilés). `k` = NET après repêchage. Pool 30,8 Ko.
> **Round 17 console (2026-09-02)** : 2×2 captures quasi identiques, l'une trouée l'autre
> non — les trouées sont EXACTEMENT les `k>0` (k11/k5 vs k0), banque réelle 62 % vide
> (`c112-172 / B281-296`) : famine papier MARGINALE, le total facturé oscille autour de la
> limite 232 et un pas de côté fait basculer quelques plans. Le facturier résiduel = les
> plans MIXTES (exemptés du LOD par la classe tache-sur-ciel) : facturés toutes tuiles
> touchées, émis une fraction (sondes LOS). Fix : à la NOTE, les 32 premières tuiles bbox
> d'un plan mixte passent le MÊME verdict 3-sondes que l'émetteur (`psw_tile_hidden`,
> partagé verbatim — les deux DOIVENT être bit-à-bit identiques ou la borne sup casse) ;
> facture = 2×visibles+1 (∧ l'estimation touchées : min de deux bornes sup) ; verdicts
> CACHÉS dans un masque préfixe par sub (psw_sub_fmask/cmask, valide ssi bit mixte) que
> l'émetteur consomme dans les DEUX sens (bit posé = caché→skip, bit clair = visible→peint
> sans re-sonder) — les sondes sont payées UNE fois. + le ledger repêchage RELÂCHE le
> papier murs (les murs tail = les plus lointains, plottés avant tout sub) et 2/thing émis
> → le slack arrive assez tôt pour les standbys lointains. Explique aussi les « k sans
> trous » : des plans fantômes (mixtes entièrement cachés aux sondes) facturés pleins et
> jetés sur le papier sans perte visuelle — désormais facturés 1, toujours accordés.
> Pool 28,09 Ko (masques 1,9 Ko + code sondes). Attendu : k0 STABLE en se déplaçant.
> **Round 18 console (2026-09-02)** : couloirs GUÉRIS (k0 stable) mais la salle acide tient
> k11-16 avec `c193-251` — la facture sondée a libéré l'émission et la famine est devenue
> RÉELLE (murs tuilés ~100-120 cmds + demande flats > banque). Réponse structurelle (owner :
> « c'est si compliqué de ne pas avoir de trous ? ») : la PRÉSENCE avant la QUALITÉ.
> (1) **Allocation à garantie minimum** — tour A near→far : chaque passe éligible facturée
> min(4, e) seulement (personne ne tombe tant que la garantie tient) ; tour B near→far :
> le reliquat upgrade vers les tuiles (charge e−4) + attribue les 4 slots texture. La
> qualité dégrade du lointain d'abord, les trous ne peuvent naître que si TOUT-en-aplats
> déborde la banque (et le repêchage ramasse encore). (2) **Les murs cèdent aux flats**
> (précédent murs-cèdent-aux-things) : la demande GARANTIE des flats (min(4,e)/passe +
> 4/poinçon + réserve things + marge) est soustraite du surplus d'upgrade des murs AVANT
> tout mur tuilé — un mur dégradé reste un mur plat 1 cmd, un plafond jeté est un trou.
> `fl` (V1) va monter = le levier tire. (3) La ceinture d'émission relit la facture
> STOCKÉE (fe/ce sondés) au lieu de psw_tile_est brut — elle forçait des plans mixtes
> PROCHES en aplat magenta à tort en fin de frame (une part de la « zone rose »).
> NB « zone rose > réel » (owner) : géométrie identique dans les deux modes — en rendu
> normal les aplats sont couleur-texel (vert nukage sur RBG0 acide = camouflés) et
> partiellement repeints par les poinçons couleur-0 ; L+X révèle leur étendue réelle =
> le champ lointain dégradé. Attendu salle acide : murs plus plats, plans TOUS présents.
> **Round 19 (2026-09-02) — EXTENSION DE BANQUE, fin du rationnement** (owner : « on est
> passés en full vdp1 pour trouver de la performance, et tout ce que tu fais c'est
> couper » — exact). Constat : la salle acide tourne à ~12 fps CPU-bound avec fence `w0`
> partout = le VDP1 finissait toujours en avance, seul le PLAFOND DE 304 était plein —
> un plafond artificiel. Les 12 Ko libres en queue de pool murs (0x25C5E000..0x25C61000,
> réservés par leur propre commentaire « ≥ one 8 KB VDP1 command bank ») deviennent
> 2×192 slots : slot physique 303 de chaque banque = sysclip+JUMP_ASSIGN statique vers
> son extension (recette du terminateur per-frame, écrite une fois à l'init) ;
> `vdp1_cmd_at()` traduit les slots logiques ≥303 — aucun émetteur ne change. **304 →
> 495 commandes logiques** ; WALL_CMD_CAP 487, PSW_FLAT_CAP 232→420 ; compteur LOPR
> extension-aware (un overrun dans l'extension se mappe en slots logiques). **Round-18
> « murs cèdent aux flats » RETIRÉ** (demande owner) : plus personne ne cède — murs
> tuilés ET plans tuilés tiennent ensemble (~370-430 < 487 dans la salle acide).
> L'allocation garantie A/B reste (police d'allocation saine, upgrade tout quand ça
> tient). Attendu : k0 partout, magenta réduit aux bandes LOD ≤16 px, murs texturés
> comme avant. À surveiller : VD1 `<ms>` (attente fence — le vrai prix du plot en salle
> lourde), `w` doit rester 0, LP/B cohérents. Fence = inchangée (sentinelle = banque
> vide, adresse fixe). Build normal : extension compilée out (#if SAT_PSW), bit-intact.
> **Round 20 console (2026-09-02)** : trois symptômes nommés par l'owner, trois mécanismes.
> (1) **Trou triangulaire avec k0 d0 r0** = le cull plan-entier par ÉCHANTILLONNAGE
> (sommets+centroïde) : chaque rayon échantillonné tombait sur un bloqueur (pièces basses
> voisines) pendant que le MILIEU du plan était visible par une fenêtre au-dessus — un
> échantillon n'est pas une preuve. SUPPRIMÉ (pas re-paramétré). Les sols perdent AUSSI
> leurs sondes par-tuile (pure économie de fill à risque de trou : le peintre recouvre
> tout sol sur-peint, le poinçon couvre le cas dominant — par construction) ; les
> plafonds gardent l'échelle UNIQUEMENT pour le classement mixte + garde-ciel par tuile
> (nécessaire : peindre sur le ciel VDP2 est ineffaçable). (2) **Escalier du rebord**
> (« trait rouge au lieu du trait vert ») : le bord d'une bande est PLAT par tuile (bbox)
> ⇒ bordure diagonale = marches de 64u. Fix : pièces de bord PROCHES (bbox projetée
> ≥24 px) raffinées en SOUS-BANDES DE 8 TEXELS selon l'axe dominant de la diagonale —
> u-strips (1 fenêtre + ≤8 quads, CMDSRCA décalé par pas de 8 texels) quand le bord
> court en x-monde, v-bandes (fenêtre par bande) quand il court en y ; pas 64→8 unités.
> Plafonné PSW_FINE_CAP 64 cmds/frame (dépense réelle au-dessus de la facture — la
> jauge sert naturellement le PROCHE, seul à passer la porte de taille). (3) **« Pourquoi
> autant de rose avec de la marge ? »** = famine de SLOTS TEXTURE flats, pas de
> commandes : 4 slots pour 6-10 flats distincts par scène ⇒ du 5e lump au 10e, plan
> entier en aplat magenta. PSW_FLAT_SLOTS 4→8 : 2 slots muraux small cédés (pool mur à
> tx11/26 = à moitié vide ; WTEX_SMALL_N 16→14 build PSW seul, relayout pool fin
> 0x25C59E00, flats 4-7 jusqu'à 0x25C5DE00, sous l'extension banque). Attendu : le trou
> triangle mort, le rebord droit, le magenta réduit au LOD ≤16 px réel.
> **Round 21 console (2026-09-02)** : owner — (1) « le trou triangulaire est encore là,
> c'est un plafond en partie occulté par un mur » ⇒ fausse route round 20 : le cull
> plan-entier n'était pas le mécanisme. Pli bandes-portales relu = vanilla-correct par
> colonne, bandbox conservateur ⇒ LE suspect restant qui colle (forme = sous-secteur,
> bordé de murs dessinés, insensible à TOUS les changements culls/budget) = **débordement
> silencieux du recorder** (PSW_SUB_MAX 240 : les subs en excès émettent leurs murs, leurs
> flats JAMAIS — la classe round-13 revenue). Fix : 240→384 (+~5 Ko .bss) ET fin du cap
> silencieux : row 13 `r` (mort sous PSW) → **`o` = overflows comptés** — o>0 = remonter
> la constante, o0 + trou = classe inconnue. (2) « le rose toujours là, flat » ⇒ mon
> erreur round 20 : retirer les sondes par-tuile des SOLS a fait exploser leur coût en
> COMMANDES (chaque tuile sur-peinte = 2 cmds) ⇒ facture pleine ⇒ tour B n'upgradait
> plus ⇒ budget-solid partout. Sondes sols RESTAURÉES (classification mixte + facture
> sondée) ; le cull plan-entier reste supprimé. (3) « bancal » (sous-bandes) : assumé —
> silhouette exacte + texture ancrée-monde + une commande = impossible sur VDP1 (le
> mapping n'est world-exact que sur des rects alignés monde) ; le choix = ancrage + pas
> de 8u, l'alternative (warp exact) = le swimmer prouvé. PowerSlave n'a pas ce problème
> parce que ses niveaux sont dessinés sur la grille. Attendu : triangle mort (ou o>0 le
> désigne), rose → orange/texturé, `o` à surveiller en priorité.
> **Round 22 (2026-09-03) — LE MODÈLE PLEIN-CARRÉ (design owner, adopté intégralement)** :
> « on peut tout faire avec les 64×64 en carré, quitte à déborder derrière le mur ».
> Prouvé dans le peintre : le seg couvrant fait toujours face au joueur = côté proche =
> peint APRÈS le débordement ; chords de split = même secteur = texels identiques alignés
> grille. SEULE exception = segs 2-côtés dos-au-joueur ouverts au-delà du plan (sol qui
> CHUTE derrière, plafond qui MONTE, CIEL) — scannés par sub côté core (`R_PswSoftLines`,
> ~10 segs) : seules les tuiles coupées par CES lignes gardent le chemin clippé exact
> (bandes + sous-bandes fines round 20, leur vrai périmètre) ; toute autre tuile = UN
> quad plein, 1 cmd, zéro fenêtre, zéro clip (reject SAT). L'escalier meurt par
> construction sur les bords couverts (le mur EST la silhouette). Sondes 3→5 points
> (spec owner : 4 coins + centre). **Super-tuiles 128** (« plus gros quads à distance ») :
> plan non-mixte, diagonale projetée ≤ 2×28 px, sans ligne molle ⇒ UN quad — le char 64
> sur 128 monde, continu inter-super-tuiles (VDP1 ne wrappe pas ; sur grille 2× alignée
> le même char EST sa répétition, zoomé ×2 ; mip vrai = +4 Ko/slot plus tard). LOD-aplat
> ≤16 px SUPPRIMÉ. Facture : tuile = 1 cmd ⇒ e = fe + (mou ? 9 : 1) aux 4 sites. `o` →
> `o<ovf>/<leafbad>` (leafbad = polygones de feuille pvn<3 = la classe déterministe du
> triangle ; o0/0 + triangle = re-diagnostic). L+X : le ROUGE devient dominant. Pool PSW
> 17,61 Ko (surveiller ; plancher ~5 Ko), normal 61,33 bit-intact.
> **Round 23 (2026-09-03) — STRIPS VERTICAUX EXACTS (note owner : « si les super quads
> sont à texture exacte monde, ils deviennent la norme »)** : les super-128 zoomés ne
> sont PAS exacts (char 64 étiré ×2 — d'où la porte de distance). L'exactitude à toute
> distance existe VERTICALEMENT : les 8 slots flats vivent en deux runs VRAM contigus de
> 16 Ko ({3,0,1,2} à 0x25C7C000, {4,5,6,7} à 0x25C59E00) et les lignes d'un char VDP1
> sont contiguës ⇒ un flat téléversé dans DEUX slots adjacents se lit comme UN char
> 64×128 dont la moitié basse ALIASE le voisin = vraie répétition, phase de grille exacte
> (tout haut de bande 64-aligné = ligne 0 du flat). Horizontal impossible (chaque LIGNE
> devrait être physiquement doublée). Implémenté : PAIRES DYNAMIQUES dans psw_slot_get
> (un flat « chaud », fe ≥ 12 tuiles, demande une paire ; les froids restent simples ⇒
> la capacité en lumps distincts ne baisse que là où le gain existe ; shadow/pairbase +
> break à l'éviction) ; dans la marche non-mixte, chaque colonne du super-cell tente un
> STRIP 64×128 (1 cmd / 2 tuiles, exact, toute distance — mêmes portes SAT/lignes
> molles) avant les tuiles simples. Hiérarchie de coût : loin = super-128 zoomé (1/4),
> près intérieur chaud = strip exact (1/2), reste = tuile (1/1), bord mou = clippé.
> L+X : strips ROUGES (famille plein-exact) — psw_emit_rectquad peint désormais via
> psw_paint_idx (bandes = orange posé à l'appel). Prochain cran possible : triples
> 64×192 (3 slots adjacents), et le mip vrai pour dé-zoomer les super-128.

> **Statut 2026-09-03 (round 24 — console rounds 22+23 : rouge vrai, quads propres,
> strips ×3, supers supprimés).** Console : « jamais vu de rouge » + « quads gris des
> deux côtés de chaque marche (texture du sol en dessous) » + « superquads zoomés trop
> visibles ». (1) RAISON DU ZÉRO-ROUGE TROUVÉE : PLAYPAL 88 = (183,183,183) GRIS — les
> pleins carrés et strips TOURNAIENT, peints gris. Peinture = PLAYPAL 176 (255,0,0)
> désormais. Raison trouvée ⇒ règle du propriétaire satisfaite : chaînes de slots
> étendues aux TRIPLES (strip 64×192 = 3 tuiles / 1 cmd), et le hint de chaîne passe
> AUSSI dans le grab de slots du pré-passage round B (le hint émission-seule trouvait
> tous les voisins pris → strips silencieusement dégradés). (2) SUPERS-128 zoomés
> SUPPRIMÉS (2× visible). L'économie lointaine = les strips exacts. (3) Tuiles grises
> des marches : cause = critère de ligne molle trop étroit — un sol d'en face PLUS HAUT
> restait dur « car la contremarche couvre », mais elle ne couvre que jusqu'à son
> sommet : le débordement au-delà se projette AU-DESSUS, sur le dessus de marche déjà
> peint (plus loin). Core ae62ac7 : dur SEULEMENT si continuation exacte (même hauteur
> ET même flat) ; ciel toujours mou ; PSW_SOFT_MAX 6→8. (4) QUADS PROPRES (directive :
> « marche, rebord… candidats parfaits pour leur propre quad texturé projeté ») : un
> plan clippé ≤ 4 sommets et plus étroit qu'une tuile (grand côté ≤ 128) quitte la
> grille : UN quad projeté exact à la feuille (zéro débordement, zéro trou, zéro ligne
> molle), texture = sous-rect bbox du char (phase monde exacte si pas de wrap ; sinon
> étirement borné ≤ 2× — ces pièces bordent des changements de hauteur, aucune phase
> voisine à préserver). L+X : BLANC (PLAYPAL 4). Hiérarchie : colonne chaude = strip
> ×3 (1/3) puis ×2 (1/2), tuile (1), petit plan = quad propre (1/plan), bord mou =
> clippé exact. Pool Psw 15,92 Ko (plancher ~5 Ko — marge fondante), normal 61,33 Ko
> bit-intact. Commits : core ae62ac7, Mimas a11e5dc. NON validé console.

> **Statut 2026-09-03 (round 25 — console round 24 : orange comprimé, parasites rouges
> « cube dupliqué », rebords toujours en escalier).** (1) **PARASITES ROUGES = LES
> STRIPS, prouvé par élimination hors-ligne** : `tools/psw_leaf_check.py` rejoue le
> builder de polygones de feuille du core bit-à-bit (virgule fixe, boucle de
> rétrécissement, division C, shave) sur les 9 cartes shareware et compare chaque
> feuille à la vérité exacte en rationnels — **3423 feuilles, zéro sur-taille, zéro
> sous-taille, zéro dégénérée, zéro winding inversé**. Le SAT par-arête étant complet
> pour un convexe, une tuile simple ne peut jamais être émise pleinement hors du
> polygone. Restait UN émetteur rouge capable : le SAT plein-rect des strips ne
> rejetait qu'un strip ENTIÈREMENT dehors — un strip chevauchant le polygone sur une
> seule tuile émettait quand même ses 2-3 tuiles = jusqu'à 2 tuiles pleines jetées
> dans le vide au bord d'un voisin (depuis le round 23 — les parasites gris d'avant =
> la même classe ; le « cube dupliqué à droite et à gauche » = le flat du plafond
> voisin, souvent identique, peint dans le vide de part et d'autre). Fix : chaque
> tuile couverte par un strip passe désormais le même test pleinement-dehors qu'une
> single — le strip ne peint plus rien qu'une marche single n'aurait pas peint.
> (2) **TEXTURE COMPRIMÉE = quad-propre v1** : il élargissait la SOURCE à l'alignement
> 8-texels en gardant le quad à la vraie taille (compression ≤ ~1,9× sur petites
> pièces) + perte de phase au franchissement de char. v2 = ANCRÉ MONDE : pièces
> rect-axiales seulement (diagonale/coupe frustum → grille), coins du quad AUX BORNES
> SNAPPÉES du grid texel (8 en u, 1 en v), source = le sous-rect de char correspondant
> — texture exacte et en phase grille PAR CONSTRUCTION, arêtes du quad = les vraies
> arêtes monde ; fenêtre seulement pour rogner la lèvre de snap (≤7 texels) d'une
> pièce désalignée ; v peut courir dans les slots-ombres chaînés (≤192). (3) **ESCALIER
> DES REBORDS = le crop-x de la fenêtre rect** : la bande axiale gardait le quad
> pleine-largeur et rognait en x par la fenêtre — une ligne de crop VERTICALE contre
> une arête projetée INCLINÉE = les marches 64u. Les pièces rect-axiales émettent
> désormais à leur vraie étendue x avec le sous-range u correspondant (1 cmd quand
> 8-alignée, fenêtre-lèvre sinon). Pool Psw 15,55 Ko, normal 61,33 Ko bit-intact.
> Commit Mimas 56eb168 (core intouché). NON validé console.

> **Statut 2026-09-03 (round 26 — « RIEN n'a été corrigé » + directive « les murs
> doivent s'afficher par dessus les plans, à distance équivalente au moins » : LE
> MODÈLE DE DÉBORDEMENT EST MORT).** La directive du propriétaire a nommé le vrai
> mécanisme unifiant les trois symptômes : le plein-carré déborde sa feuille en
> comptant sur « quelque chose recouvre », or le quad couvrant (faces latérales d'un
> cube suspendu, contremarche, mur sous un rebord) appartient à un sub VOISIN à
> distance BSP équivalente — qui peut émettre AVANT. Et chaque classification
> statique de frontière a fui de façon MESURÉE hors-ligne (après correction d'un bug
> d'index sidedef dans le vérificateur qui avait pollué une passe de mesures) :
> règle segs = 248 frontières à contenu différent sans seg couvrant (bordures sur
> splitlines BSP nues — le node builder d'id partitionne LE LONG des linedefs, les
> segs vont d'un seul côté), murs à double face d'épaisseur zéro (montants de porte :
> « one-sided = couvert » FAUX, du vrai contenu à 1,5u derrière), règle sondes = 75
> arêtes / 880 tuiles fautives (des bandes de contenu plus étroites que tout pas
> d'échantillonnage), règle linedefs / arêtes-en-face : réfutées aussi (les cellules
> de feuilles NE PAVENT PAS — segs manquants ⇒ recouvrements). CONCLUSION
> D'ARCHITECTURE : le débordement inter-frontière est infixable ; le nouveau contrat
> est le CONFINEMENT — un plein-carré ou un strip (1 cmd) n'est émis que s'il est
> ENTIÈREMENT dans le polygone de la feuille (convexe : 4 coins dans chaque arête) ;
> toute tuile de bord prend le chemin clippé exact (pièces axiales round-25 = 1 cmd).
> Un flat ne peut plus peindre UN texel hors de sa feuille ⇒ jamais sur un mur, quel
> que soit l'ordre, sur tout WAD — par construction, plus aucune énumération de cas.
> Toute la machinerie lignes molles MEURT (core R_PswSoftLines + sondes voisines +
> psw_sub_soft) ; facturation = retour à la loi round-14 (2e+1) ; le label row 13
> devient **« P26 » = MARQUEUR DE BUILD** (incrémenté chaque round — fin définitive
> du doute « quel disque a été testé ? »). Pool Psw 17,36 Ko (+1,8 Ko rendus par la
> machinerie supprimée), normal 61,33 Ko bit-intact. Commits : core af13e6d, Mimas
> 3590a67. Attendu console : L+X plus orange qu'avant sur TOUTES les bordures
> (le prix du confinement), rouge sur les intérieurs, plus jamais un texel de flat
> sur un mur ; surveiller `f` et `k` (facture 2e+1 plus lourde — si famine, le
> levier suivant est la FUSION des feuilles sœurs de même secteur au chargement,
> zéro risque de justesse). NON validé console.

> **Statut 2026-09-03 (round 27 — verdict console P26 : « correct visuellement,
> catastrophique niveau performance » ⇒ SIMPLIFIER, RÉORDONNER, DÉPORTER SUR LE
> 2e SH-2).** Les 7 captures P26 ont nommé DEUX facturiers CPU, tous PSW : row-2
> `P` 41,5-45,6 ms (le flush — ~105 µs/cmd, coût unitaire IDENTIQUE dans le couloir
> rapide ⇒ facture PAR TUILE TESTÉE : SAT 64-bit en `__muldi3` logiciels ~4/arête/
> tuile, re-jeté jusqu'à 3× par colonne par les sondes stripn, + 4 passes Sutherland
> par tuile de bord — y compris les bords de FRUSTUM qui balaient tout le sol en
> tournant) et `Bw` 12-62 ms (le travail à la NOTE dans la traversée BSP — les
> marches de sondes LOS des plans mixtes, 5 sondes × 1-3 descentes BSP par tuile,
> recalculées CHAQUE frame ; 62 = la scène à fenêtres). Slave SH-2 : 1-9 % occupé.
> Tout le reste innocenté (T1-2, blit 0,1, VD1 5-15 ms, k0/d0/o0 partout — la peur
> famine 2e+1 ne s'est PAS matérialisée). RÉPONSE EN TROIS ÉTAGES : (1) SIMPLIFIER —
> grille incrémentale de masques de coins (un cross-product est AFFINE sur une
> grille : 1 multiplication longue par arête par chunk, puis des ADDITIONS 64-bit
> par coin — les MÊMES entiers bit à bit, redistribués) ; classification O(1) par
> tuile, strips gratuits par runs de classes (l'ancien SAT plein-rect était
> ÉQUIVALENT par convexité), et ARÊTES DE FRUSTUM MOLLES : une tuile contenue dans
> sa feuille mais coupée par le seul bord d'écran émet 1 plein-carré (queue ≤64u,
> mangée par le system clip VDP1) au lieu de 2 cmds + Sutherland — le confinement
> vs la FEUILLE est INTACT, le frustum n'est pas un mur, le near-clip reste dur,
> les strips restent totalement confinés. (2) RÉORDONNER — la marche de masques
> quitte la note : jobs en file, verdict appliqué min(vis, est) à une FENCE en tête
> de flush ; le cull band-box passe AVANT les sondes los_cull (rejet le moins cher
> d'abord) ; réutilisation EXACTE des masques (même œil + même hauteur ≤8 frames :
> tourner sur place ne recalcule RIEN — les verdicts ne dépendent pas de l'angle).
> (3) DÉPORTER — un corps slave draine la file PENDANT la traversée BSP du master
> (slSlaveFunc via rp_sgl_workptr_reset, l'entonnoir r_parallel ; le slave purge
> son cache à l'entrée, file et résultats passent par le miroir non-caché — SH-2
> write-through, le seul risque est la staleness en LECTURE ; fence bornée 6 ms
> avec fallback inline compté + latch-OFF définitif si le corps se coince). Le memo
> de coins partagés fait passer les sondes de 5/tuile à ~2,25/tuile (fonction
> partagée VERBATIM slave/inline/late = masques bit-identiques). PREUVE HORS-LIGNE
> (leçon round 26) : 4000 tirages — équivalence exacte ancien walk ↔ grille
> (séquence d'émission, strips compris), les molles ne changent JAMAIS l'ensemble
> de tuiles couvert et ne franchissent JAMAIS une arête dure, le chunking est sans
> effet (scratchpad grid_walk_check.py). Row 13 : **« P27 »**, champ **`N<note ms>/
> <fence ms>`** entre (Bw − N = la marche vanilla ; fence 0 = l'offload a totalement
> recouvert la traversée), `d` sort (lisait 0 depuis le round 4 ; sa panne est
> MAGENTA en L+X). Pool Psw 17,36 → **11,31 Ko** (.bss file+table ~3,2 Ko + texte ;
> marge ~6 Ko au-dessus du plancher famine ~5 Ko — surveiller), normal **bit-intact**
> (61,33 Ko, 22 233 456 octets). Commit Mimas 5c85d84, core INTOUCHÉ. Attendu
> console : P doit tomber vers ~12-18 ms et Bw vers ~la-vanilla+N ; L+X = du ROUGE
> aussi en ceinture de bord d'écran (ex-orange, voulu), VD1 `<ms>` peut monter un
> peu (queues ≤64u au bord — le prix accepté du 1-cmd). Étage suivant si `P`
> résiduel le justifie : record/execute du flush (pièces de bord + projections
> calculées par les DEUX CPU, émission sérielle master) — l'infra fence/file est
> posée. NON validé console.

> **Statut 2026-09-03 (round 28 — verdict console P27 « extrêmement décevant » :
> Bw GUÉRI, P à peine bougé ⇒ NOMMER le coût par commande + staging SlaveDriver).**
> Les 9 captures P27 : `Bw` 20-62 → **8-12 ms** (l'étage note/masques/slave marche —
> fence 0 partout, N4-8, N19 sur la scène à note lourde) mais `P` **31-48 ms** :
> l'attribution round-26 (« le SAT est la facture ») était FAUSSE, deuxième
> mésattribution d'affilée. Le fait propre des captures : **P est LINÉAIRE en
> commandes** (c74→6,4 ; c357→35 ; c385→39,8 ; c409→38,8 ≈ **95-100 µs/cmd**,
> identique dans le couloir rapide) — la constante « loi L4 » déjà MESURÉE au
> round A/B 3a (64,5 µs/cmd) et oubliée. Réponse (directive owner : « supprime ce
> qui ne sert plus » + « inspire-toi de SlaveDriver ») : (1) **MESURER l'intérieur
> de P au lieu de reparier** — row 13 = « P28 », `e<ef>/<ew>/<y>` : ef = tout le
> côté flats (psw_emit_subflats : poly+projections+lumière+walk), ew = le walk de
> tuiles seul (prep = ef−ew), y = le chemin d'écriture de commandes stagé ;
> P−ef = murs+things+pré-passe. `o` (0/0 depuis r21/22) et `c` (dormant) cèdent
> leurs colonnes. (2) Coupes CERTAINES du coût/cmd, toutes bit-identiques :
> psw_project recouvre ses DEUX divisions DIVU sous UNE fence IPL (le stall 39
> cycles ×2 était sec) ; cache de projections de coins sur les colonnes de
> masques (un coin sert ≤4 tuiles + les strips, lazy) ; (3) **STAGING DE
> COMMANDES, la recette SlaveDriver VERBATIM** (SPR.C getCmdTable/flushCmdBuffer +
> DMA.C dmaMemCpy : la référence ne poke JAMAIS le VRAM VDP1 pendant l'émission) —
> marshaling en HWRAM caché (write-through ⇒ cohérent sans purge), moitiés
> ping-pong de 16 cmds (la moitié remplie n'est jamais celle en vol — la course
> que SlaveDriver tolérait), chaque lot part en UNE SCU-DMA niveau 0 async
> (fence-avant-départ, add 0x101, facteur 7 — constantes de la référence), fence
> au kick AVANT le flip de racine ; destination non-contiguë = fermeture de lot
> (traversée du split de banque = une frontière, pas un drain par cmd) ; canal
> coincé ⇒ latch fallback copie CPU 32-bit. Build normal = boucle directe 16-bit
> intacte (**bit-intact, 61,33 Ko / 22 233 456 o**). PROGRAMME DE SUPPRESSION
> (armé sur les sondes du prochain retour) : accumulation visplanes sous PSW
> (`vp7-39` encore construits pour la seule élection), prologue R_StoreWallRange
> (`Bp` 2,6-5 ms), élection du dominant depuis psw_sub ; RP_CMDS vérifié = carve
> à adresse fixe, pas du pool. ⚠ Pool Psw 11,31 → **7,94 Ko** (staging+sondes ;
> file 64→48, staging 24→16 déjà) — MINCE, la récupération passe par la coupe du
> mort. Commit Mimas c6ce492, core intouché. Attendu console : lire `e` AVANT
> tout verdict — si y domine ef−ew ⇒ le staging paie et on pousse (chunks plus
> gros) ; si ef−ew (prep par plan) domine ⇒ cache des polys note→flush ; si
> P−ef domine ⇒ les murs/things prennent le staging aussi (déjà fait) et le
> résiduel = pré-passe. NON validé console.

> **Statut 2026-09-03 (round 29 — verdict console P28 « qu'est-ce qui coûte,
> encore ? » : les sondes ont répondu, on coupe).** Les 8 captures P28 : `y`
> **0-2 ms partout** = le staging SlaveDriver marche ET les écritures VRAM
> n'étaient qu'~1-2 ms du total (pari « pousser le staging » MORT — la sonde a
> évité le 3e faux investissement). Le split réel : **ew−y 19-26 ms = le walk**
> (dominant : `b59-99` tuiles de BORD à ~120-180 µs pièce — 4 passes Sutherland
> + shoelace + 4-8 projections non cachées chacune ; + les intérieurs des plans
> MIXTES qui reprojetaient 4 coins sans cache), **ef−ew 7-14 = la prep par
> plan** (psw_plane_poly refait au flush + n projections pour bbox/lumière),
> **P−ef 8-10 = murs/things/pré-passe**. Round 29, coupes prouvées : (1)
> **FAST-PATH BORD AXIAL** dans emit64 — quand toutes les arêtes coupantes
> DURES (cutm = fullm & ~a4, lu des masques de coins) sont axiales (le cas
> Doom), la pièce tuile∩poly est un RECT exact clampé par les lignes coupantes :
> O(1), zéro division, **bit-identique** à l'ancien clip (r29_border_check.py,
> 34k tuiles ; un sommet de poly STRICTEMENT dans la tuile met ses DEUX arêtes
> dans cutm — une droite qui traverse un carré sépare ses coins — donc un coin
> diagonal ne peut pas se cacher). Les bits SOFT (frustum) sont ignorés comme
> les carrés classe-2 (queue ≤64u mangée par le system-clip, texels à l'écran
> prouvés égaux point à point) et ÉCONOMISENT souvent la window. (2) Intérieurs
> des plans mixtes sur le cache de projections de coins (emitfull ; le check de
> masque est hissé dans le walk, la branche intérieure non cachée d'emit64 est
> SUPPRIMÉE). (3) psw_emit_subflats : les verdicts note/pré-passe passent AVANT
> les 3 clips monde (un plan caché ne paie plus sa prep), et la lumière near-row
> d'un plan tuilé = UNE projection (sommet de profondeur min ; la row est
> monotone en profondeur, égalité⇒égalité — nr bit-identique) au lieu de n ; le
> cull « entièrement hors écran » tombe pour les plans tuilés (après world-clip
> il ne reste que le sliver de marge 8u, que le system-clip VDP1 mange). (4)
> **SUPPRESSION VISPLANES sous PSW** (la directive) : R_Subsector saute
> R_FindPlane (core 36ecd5d), R_PswFrameFlats (plateforme) nourrit ciel +
> résidence dalle depuis les notes ; l'élection gardait déjà son fallback
> sous-l'œil (couvertures nulles sans span marking) ; `vp` lit 0 en PSW par
> design. (5) Makefile : MAXVISPLANES 96 en build PSW seulement
> (plane_worklist 8 Ko + vpsort + hashnext) ⇒ **pool 7,94 → 12 Ko** — première
> marge confortable depuis r27. Build normal **bit-intact** (61,33 Ko /
> 22 233 456 o exactement). Row 13 : **P29**, champs inchangés. Commits : core
> 36ecd5d, Mimas a02a31d. Attendu console : `b` en forte baisse de coût (pas de
> compte — les bandes restent comptées), `ew` divisé ~2-3×, `ef−ew` ~divisé 2,
> `N` un peu plus bas (plus de R_FindPlane dans Bw) ; le résiduel `P−ef`
> (murs/things/pré-passe) devient alors la cible du round 30. NON validé
> console.

> **Statut 2026-09-03 (round 30 — verdict console P29 « meh » : `ew` N'A PAS
> BOUGÉ, au chiffre près dans le couloir).** Les 7 captures P29 : `ef−ew` a pris
> les coupes r29 (−2 à −6 ms à charge égale ou supérieure : e33/23 f212 contre
> e40/26 f175), `vp0` partout (suppression visplanes active), mais **`ew` est
> IDENTIQUE** (couloir : `e7/6/0 t4 f73 b35` au chiffre près). Trois rounds de
> coupes arithmétiques bit-identiques (SAT→grille, DIVU recouvert + cache de
> coins, clip de bord → rect) laissent la constante ~80-100 µs/cmd DEBOUT ⇒ la
> facture n'est PAS l'arithmétique retirée. En relisant la queue d'émission :
> chaque tuile de bord projetait TOUS les m sommets de sa pièce (boucle amont
> `sxv`) alors que les sorties les plus fréquentes ne les lisent jamais (bande
> pleine-largeur : ses 4 projections snappées seulement ; pièce axiale alignée :
> ses 4 coins snappés) — 8-12 psw_project par bord, le fast-path r29 n'en
> touchait aucune. Round 30 : (1) **projections de pièce PARESSEUSES** —
> projetées au premier usage, dans les trois seuls chemins consommateurs
> (window du lip de snap, window partagée du !done, fan solide) ; effet de bord
> près du near-guard : l'ancienne boucle jetait la tuile ENTIÈRE, les bandes
> émettent désormais — moins de trous en bas d'écran, jamais plus. (2) **Row 13
> « P30 » nomme l'intérieur de ew** : `e<ef>/<ew> B<ms>/<fast>/<bord> q<ms>
> j<ms>/<n> f<cmds>` — B = total emit64 (bords) + tirs du fast-path + compte de
> bords ; q = sondes BSP live des tuiles au-delà du préfixe de masque (plans
> mixtes) ; j = corps de psw_project, scope flush (~2 frt_read/appel de biais
> auto). RÈGLE DE LECTURE : B domine + fast≈0 ⇒ le fast-path ne tire jamais
> (chasse au bug) ; B domine + fast haut ⇒ la queue de bord = projections
> (j le confirme) ; j domine ⇒ le coût unitaire de psw_project est le feu
> (candidat : une seule réciproque 1/tz partagée par les 2 divisions — pas
> bit-identique, à arbitrer) ; AUCUN ne domine ⇒ la constante est structurelle
> (cache 4 Ko / layout de code, pas l'arithmétique) — et le levier change de
> nature. `y` (0-2 deux fois) sort de la row avec sa paire frt_read/cmd ;
> N/fence, t, k, u, b, n cèdent leurs colonnes (compteurs vivants au code).
> Pool Psw 11,66 Ko ; build normal bit-intact (61,33 Ko / 22 233 456 o). Commit
> Mimas d0742e3, core intouché. NON validé console.

> **Statut 2026-09-03 (round 31 — verdict console P30 « décevant. encore. » :
> les sondes ont CONDAMNÉ les bords, et le fast-path ne tirait jamais).** Les 6
> captures P30 : **B 18-21 ms sur ew 23-27 = les tuiles de bord SONT la
> facture** (~120-190 µs par entrée, slivers sans émission compris : bord149 >
> f125) ; **fast 0-3 sur 86-149 = la précondition « toutes arêtes coupantes
> dures axiales » ne tient presque jamais** dans la vraie géométrie BSP (la
> ligne de near-clip est une arête dure DIAGONALE qui traverse une rangée
> entière de tuiles par plan ; les splitlines ancêtres sont diagonales ; et le
> test soft par distance rate les crossings des arêtes très longues — son seuil
> 1/8u est débordé par l'erreur d'interpolation au-delà de ~6000u, précisément
> les grandes zones ouvertes) ; **j 3-5 ms / 600-1100 projections = ~5 µs
> (~135 cycles) pièce** — psw_project tourne à son coût arithmétique, les
> projections sont saines ; **q0 partout** — les sondes hors-préfixe hors de
> cause. Le feu = le gros chemin froid du clip (IPC ~0,3 sur ce chemin vs
> pleine vitesse sur le petit code chaud). Round 31, deux élargissements
> prouvés : (1) **soft EXACT par étiquettes de crossing** — psw_clip_dir porte
> un tag par sommet (gardé = son tag, crossing = l'id de passe 1=near/2,3=côtés
> frustum) ; un clip convexe laisse ses deux crossings ADJACENTS, donc une
> arête dont les deux bouts partagent un tag ≥2 est PROUVÉE sur cette ligne de
> frustum ; union avec l'ancien test de distance (qui garde le cas du sommet
> original posé exactement sur la ligne). Les tuiles coupées par le frustum en
> grande zone redeviennent des fulls classe-2, et leurs faux bits durs-diagonaux
> cessent de bloquer le fast-path. (2) **Le fast-path accepte UNE coupe dure
> diagonale** : rect clampé par les coupes axiales comme avant, puis UNE passe
> de Sutherland de ce rect contre l'arête diagonale (cross 64-bit, fa et fa−fb
> normalisés d'un décalage COMMUN à 30 bits avant la division 16.16 : crossing
> à ~0,002u de l'exact, coutures cohérentes entre voisines) — couvre les
> rangées de la ligne near et les murs diagonaux isolés
> (r31_onediag_check.py, 67k tuiles équivalentes au clip 4-passes, tolérance
> d'arrondi). Row 13 : **P31**, champs inchangés (`B<fast>` compte les deux
> variantes). Pool Psw 10,44 Ko ; build normal bit-intact. Commit 9f86328,
> core intouché. Attendu console : `B<fast>` proche de `B<bord>`, `B<ms>` ÷2-3,
> `ew` vers ~12-17 ; si fast reste bas ⇒ ≥2 diagonales par tuile (feuilles en
> coin de BSP) et l'escalade = BAKE de la grille par feuille (décomposition
> statique par niveau, ~6-8 Ko de zone) documentée comme round 32. NON validé
> console.

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
