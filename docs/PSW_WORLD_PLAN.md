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

> **Statut 2026-09-03 (round 32 — GO owner « a+b, je veux le moteur le plus
> optimisé possible » ; verdict P31 : fast tire 24-35 mais B INCHANGÉ ⇒ la
> facture est la queue d'émission entière + l'empreinte code, pas le clip).**
> **LE BAKE** : la décomposition feuille∩grille-64 est de la géométrie MONDE —
> la recalculer chaque frame était la facture. Cuite UNE fois par feuille,
> paresseusement (2 feuilles/frame), dans une arène PU_LEVEL de 12 Ko gardée
> par Z_TrueFree (Z_Malloc2 sans purge, NULL gracieux ; libérée à la sortie de
> niveau ; psw_pvx + chute de leveltime surveillés contre l'allocateur
> déterministe qui rend les mêmes adresses) : 1 octet de CLASSE par tuile
> (out/sliver/full/live/record), 8 octets par tuile de bord (bande
> pleine-largeur v0/vend ; pièce axiale alignée +ua/uwd/qx ; diag = rangées de
> bande + bbox texel arrondie VERS L'EXTÉRIEUR pour la window), 16 octets par
> tuile diag orientée U (les 8 plages v des sous-bandes FINE, précuites). Au
> flush le walk ne garde que le travail VUE : les masques de coins sur TROIS
> demi-plans (near dur, côtés frustum soft — nmask=3 au lieu des n≤28 arêtes),
> le gate de masque de note, les projections (cache cproj), le staging. Une
> tuile de bord cuite = lecture de table + 4 projections + 1-2 commandes.
> FALLBACK LIVE (l'ancien chemin, verbatim) : rangées near-cut, pièces axiales
> mal alignées 8 (leur window de lip exige les sommets exacts), refines
> V-orientés de près, slivers sous-rangée, feuilles non cuites (warm-up),
> famine de slot. Strips : baked-full ∧ view-contained ≡ l'ancien cls3.
> **Le fast-path r29/r31 d'emit64 est SUPPRIMÉ** (le bake possède exactement
> ses tuiles ; ses octets de code étaient du pool que le boot exige — premier
> build à 2,67 Ko < plancher 4,8 ; avec la suppression + MAXVISPLANES 64 en
> build PSW + MQ 48→40 + MLRU 32→16 : **pool 6,78 Ko** — au-dessus du plancher
> mesuré, sous le confort 7 : ⚠ CONFIRMER LE BOOT). Row 13 : **P32**
> `e<ef>/<ew> B<ms>/<bord> K<baked>/<live> j<ms>/<n> f<cmds>` — B = résiduel
> live, K = couverture du bake. Extraction prouvée (r32_bake_check.py : arbre
> de décision + round-trip des records + containment de la bbox extérieure +
> plages de strips ; ⚠ la distribution aléatoire montre `win`≫`axis` — en
> géométrie NON alignée-8 le fallback live domine ; Doom réel est aligné-8,
> `K<live>` console tranchera, v2 = baker aussi les pièces win). Build normal
> bit-intact. Commit 9e2fb82, core intouché. Attendu console : B ≤ 3-6 ms
> (résiduel live), ew ~8-14, P ~22-28 après warm-up ; ensuite **round 33 =
> TOUT le côté flats sur le slave SH-2** (owner : « si on peut tout donner à
> ssh2, allons-y ») — réservation des plages d'index VDP1 par la loi 2e+1,
> pads no-op dans les trous, master = murs/things/kick ; le bake rend la passe
> flats du slave assez courte pour se cacher sous les murs du master. NON
> validé console.

> **STATUT round 33 (2026-09-03)** — Console P32 : « ok, beaucoup mieux » —
> le bake tient ses promesses (B 19-21 → **1-7 ms**, K70/18 K81/17 = couverture,
> fps 12→15 en zones ouvertes, 47 en couloir ; ew 13-20, ef 16-26 = les flats
> restent le poste #1). **ROUND 33 SHIPPÉ : TOUT le côté flats émet sur le
> SSH2.** Le peintre est un ordre d'INDEX : le pre-pass facture déjà chaque sub
> en commandes (loi 2e+1) → la boucle master RÉSERVE bill[k] slots par sub et
> plotte murs/things autour des trous (le staging ferme un batch par
> discontinuité) ; le slave (canal aux r_parallel : pile 4 Ko, purge cache à
> l'entrée, join borné FRT) déroule la job-list via les MÊMES émetteurs vers
> une arène zone (~15,4 Ko PU_LEVEL) — psw_cmd_put/psw_cmd_left détournent
> chaque écriture et chaque garde de capacité ; reliquats = pads JP-skip
> (0x4000) ; au fence un SCU-DMA par job pose son bloc (translation split-303
> miroir de vdp1_cmd_at) ; latches des sondes esclaves en lectures NON-CACHÉES
> (la ligne cache du master est STALE — LE piège du round). Avec lui : rescue
> standby → round C du pre-pass (DEUX modes, near-first ; le ledger
> psw_paper_left MEURT), réserve FINE explicite aux jobs proches (+9), et
> facture bbox EXACTE pour les petits plans (≤12 tuiles bbox : l'estimateur
> analytique sous-lisait 13,1 % des feuilles → 3,3 % résiduel, preuve
> r33_reservation_check.py + splitter DMA exhaustif + invariants du packer).
> Scratch du bake (2 Ko) déplacé en tête d'arène (la pile aux fait 4 Ko).
> Diètes pool : STG_N 16→12, MQ 40→32, MLRU 16→8, MAXVP 64→40 (PSW), sondes
> j+q coupées. Row 13 = **P33** `e<ef>/<ew> B<ms>/<bord> K<b>/<l>
> F<join>/<drop> f<cmds>` — e/B/K chronométrés sur le FRT du SLAVE (forcé
> phi/128) ; `F!` = wedge latché (flats de retour master à vie). Pool
> **6,67 Ko** (plancher 4,8, confort 7 — ⚠ confirmer le boot). Build normal
> bit-intact. Commit 671810d, core intouché. Attendu console : SLV b% 1-7 →
> 30-60 %, P ≈ max(murs, flats) + fence ≈ 20-29 ms, F<join> = qui est le
> chemin long (haut = soupape v2 : rendre les plans les plus proches au
> master). NON validé console.
>
> **HOTFIX même jour (f707de4)** — 1er disque P33 sur console : **<1 fps,
> MST1111, to9:W, row 13 à zéro, sols absents**. Cause MESURÉE (objdump,
> constantes sub-r15) : la passe flats slave débordait la pile aux 4 Ko de
> r_parallel — `psw_emit_plane_tiles` = **0xd20 = 3 360 o de frame à elle
> seule** (caches cmbuf/pjx/pjy/pjs + colonnes de coins), lambda emit64
> 0x524, subflats 0x27c, bake_build 0x434 ⇒ chaîne pire ~5,5 Ko ; dès le
> premier plan tuilé la pile crevait dans le .bss de r_parallel (flags rp
> stompés = les to:W). Fix : pile DÉDIÉE 7 Ko en queue d'arène sf (trampoline
> r14 de rp_run_on_stack verbatim) + canary en pied de pile planté par le
> master à chaque dispatch et vérifié au fence (stompé ou body mort ⇒ pads +
> latch F!) + gate 1p. Arène 15,4→22,5 Ko PU_LEVEL. Leçon : MESURER les
> frames (objdump) avant de mettre une chaîne sur une pile bornée — la passe
> objdump existe maintenant. NON validé console (2e disque).
>
> **ROUND 33b (2026-09-04, 9088c0d)** — 2e disque console : 15 fps, SLV b2%,
> `F0/0` constant, e16-25 non-nuls = master-inline chaque frame. Cause lue
> DANS les captures : row LIM `zf4-27` (vraiment libre) vs `lg~263`
> (obtenable en purgeant) — l'arène sf (recette Z_Malloc2 NO-PURGE + garde
> Z_TrueFree, héritée du bake) était REFUSÉE à chaque niveau : au premier
> flush, le cache de lumps possède déjà la zone. Refus silencieux : aucun
> glyphe ne distinguait « arène absente » de « ça tourne ». Fix : gate
> `Z_CanAllocate` (run contigu libre+purgeable) + allocation PURGEANTE
> `Z_Malloc` (quelques lumps refaultent une fois par niveau) — pour les DEUX
> arènes (le bake ne passait que par chance de timing) ; row 13 gagne `F-`
> = arène refusée (la classe ne peut plus se cacher). WADs vraiment saturés
> → déclin gracieux inchangé (chemin r32 inline). Pool 6,2 Ko ; build normal
> bit-intact. NON validé console (3e disque). Attendu inchangé : SLV b%
> 30-60, P ≈ 20-29, ~20 fps ; ensuite r34 = l'ÉMISSION DES MURS rejoint les
> flats sur le slave (même modèle de réservation ; resolve wtex au pre-pass
> master), puis le plafond master résiduel = Bw+tic+blit ≈ 25-30 fps (verdict
> POWERSLAVE_GAP : le plancher d'archi).
>
> **ROUND 33c (2026-09-04, 6df2f5f)** — 3e disque console : premiers frames
> ARMÉS (F11-14/0, e15-19 slave, f196-232) mais ~8 fps, écran quasi vide,
> VD1 `g56/w↑` = force-swap 4 fields CHAQUE frame : le plot ne finit jamais.
> Cause : psw_emit_subthings pose UN restore de clip PAR APPEL (un appel par
> sub avec things) mais `treserve` facturait +8 forfaitaire ; la réservation
> r33 dépense la facture ENTIÈRE (en r32 les gardes d'émission absorbaient
> la dette en silence) ⇒ `vdp1_wnext` — dont l'incrément de réservation
> était le SEUL non gardé du fichier — dépassait les 495 slots ; le
> TERMINATEUR (écrit sans garde lui aussi) atterrissait en VRAM étrangère
> (wbank1 → textures d'arme !), la liste ne se terminait jamais, le VDP1
> errait, et chaque PTMR pendant un dessin actif était perdu : UNE frame
> chargée empoisonnait la session (l'arme absente des captures = le même
> débordement, garde CMD_GUARD). Fix : (1) treserve = 3n+8 (borne honnête :
> ≤ 1 restore par thing) ; (2) CEINTURE réservation (job qui ne tient plus
> → vbase 0xFFFF, compté dans F../<drop>) ; (3) CEINTURE terminateur (clamp
> slot 494 : perdre un quad, garder la machine) ; (4) pose des jobs à la
> fence par COPIE CPU fenêtre non-cachée — les ~30-100 DMA ch0 dos-à-dos de
> la fence étaient un usage que le staging r28 n'exerce jamais (latence
> DSTA après le start ⇒ le job j+1 peut déchirer le j en vol), et le
> fallback distrust lisait l'arène écrite-slave par lignes MAÎTRE périmées ;
> le port B-bus borne les deux chemins ([[blit-dma-lever]]) et la fence n'a
> rien à recouvrir ⇒ même temps, trois inconnues matérielles retirées ;
> (5) SONDE : row 13 `B` cède sa colonne à `x<n>` = COPR décodé au tir du
> watchdog (`-` jamais, 0-494 slot de banque, `E` root, `?n` VRAM étrangère
> page 4 Ko = preuve d'errance) — le prochain wedge se localise sur photo.
> Pool 6,14 Ko ; build normal bit-intact. NON validé console (4e disque).
> Attendu : `x-` (jamais de wd), F<j>/<drop faible>, SLV b% 30-60, ~20 fps.
>
> **ROUND 34 (2026-09-04, 81f7774)** — 4e disque console : **r33c VALIDÉ**
> (`x-` sur 9 captures, `w0`, `to0:-`, 17-20 fps en scène ouverte, 48 en
> couloir, SLV b27-43 %, F4-20/0). Trois défauts restants, trois mécanismes
> distincts, tous corrigés :
> **(1) ZONES FINES — règle owner** : « la face horizontale ne devrait jamais
> être traitée comme un flat, c'est toujours *fin* comme zone ; les murs
> double faces ne devraient avoir que deux quads ». Une marche/rebord/seuil
> ne peut JAMAIS contenir une tuile 64 pleine ⇒ toutes ses tuiles sont des
> bords ; une pièce ni pleine-largeur ni rect-axe tombe sur *bande grossière
> + fenêtre UserClip* = un quad pleine tuile découpé par un rectangle
> ÉCRAN — sur une arête projetée oblique c'est le fabricant d'escaliers, et
> quand la pièce est un éclat dans un coin la fenêtre laisse la bande
> peindre le reste de la tuile (« gros escaliers moches, tuiles entières qui
> débordent »). Désormais une feuille plus fine que `PSW_THIN_U`=32 u ET
> plus longue qu'une tuile prend les bits SOLID existants : facturée 4, PAS
> de slot texture (les vrais sols le récupèrent), l'émetteur dessine le
> polygone clippé VRAI — exactement UN quad pour le rect n==4. Verdict pris
> sur la feuille NON clippée ⇒ pas de scintillement tuilé/solide quand la
> coupe frustum bouge. ⚠ bbox alignée axes : un rebord OBLIQUE tuile encore.
> **(2) RÉSERVE THINGS invisible à son bénéficiaire** : `sat_walls_kick`
> rabote `vdp1_wall_cap` de `MARGIN + 3·res` pour que murs+flats s'arrêtent
> sous la facture de la queue ; en split ça marche car le flush things tourne
> APRÈS la restauration du cap. En PSW le drain est ENTRELACÉ dans
> `vdp1_walls_flush` ⇒ il se testait contre le cap DÉJÀ raboté, et comme
> l'émission est loin→proche les sprites jetés étaient les PLUS PROCHES
> (console `THp x12`, `x40` = monstres qui disparaissent à bout portant —
> jumeau exact du bug split soldé le 2026-08-21). Le drain garde maintenant
> sur `psw_thing_cap`, la banque non rabotée.
> **(3) DALLES DE FLATS chargées PROCHE-D'ABORD** : `psw_sub` est en ordre de
> visite BSP, donc `R_PswFrameFlats` faisait des flats proches les entrées
> les PLUS VIEILLES du LRU ; une frame avec plus de flats distincts que le
> cache n'en tient (row FLT `r16 ld31`) évinçait exactement ceux dont le
> peintre a besoin en DERNIER. Un plan dont la dalle a disparu n'a ni slot ni
> texel central et est sauté ENTIÈREMENT = un trou, et un trou PROCHE.
> Inversé : une frame en débordement perd désormais ses flats les plus
> LOINTAINS, comme tous les autres budgets du moteur.
> **Sonde** : row 13 gagne `d<n>` (`psw_flat_denied`) = plans sautés faute de
> slot ET de dalle. `d>0` = famine cache/slot ; `d0` = les trous restants
> sont géométriques. Pool 5,86 Ko ; build normal bit-intact. NON validé
> console (5e disque). Attendu : escaliers de rebords partis, `THp x0`,
> `d0`, fps inchangé ou légèrement meilleur (les zones fines coûtent 1 cmd
> au lieu de plusieurs).
>
> **ROUND 34b (2026-09-04, cc258d9)** — 5e disque : `THp x0` ✔ (réserve
> things soldée) et **`d0`** — la sonde ajoutée en r34 a payé immédiatement :
> les gros trous de PLAFONDS ne sont **pas** une famine dalle/slot. Deux
> défauts se composent, le second est une régression r33 de ma part.
> **(1) LE REPÊCHAGE DU ROUND C ÉTAIT DÉSARMÉ EN SILENCE.** r33 a déplacé le
> rescue standby de l'émetteur vers le round C en affirmant « la seule marge
> que le ledger voyait au-delà de ce reliquat était la marge de DROP des
> murs, marginale ». **Faux.** Les rounds A et B dépensent proche→loin
> jusqu'à épuiser `limit`, donc dans toute frame saturée (console : `c456`
> sur 487) `limit - ftile` ≈ 0 et le round C ne repêchait **RIEN** — sols
> comme plafonds. La vraie marge que voyait l'ancien ledger, c'est l'écart
> FACTURE-vs-RÉEL des flats : toute facture est une borne SUPÉRIEURE (2e+1
> avec e surestimé ; un solide facturé 4 qui émet 1). C'est mesurable, donc
> mesuré : l'écart (facturé − émis) de la frame précédente devient un CRÉDIT,
> plafonné à `PSW_RESCUE_CREDIT`=64. Auto-correcteur (une sur-dépense réduit
> le crédit suivant) et contenu par la ceinture de banque r33c ⇒ pire cas =
> des flats lointains tombent, jamais un débordement.
> **(2) LE REFUS DES PLAFONDS MIXTES ÉTAIT TROP LARGE.** Sa raison est
> valide : un fan solide saute les sondes par-tuile, donc là où le plan est
> caché derrière une arête de CIEL il peindrait une tache ineffaçable sur le
> ciel VDP2 (VDP1 est au-dessus, rien ne repasse). Mais il s'appliquait à
> TOUT plafond mixte — et « mixte » est le cas courant (tout plafond vu
> par-dessus un plus bas plus proche) ⇒ chacun d'eux arrivé en standby
> restait un trou de plan ENTIER. Or la fuite exige un plafond-ciel PLUS
> PROCHE que le plan ; `psw_sub` étant en ordre BSP proche-d'abord, un OU
> courant sur les subs déjà passés répond exactement, et de façon
> conservatrice. Pas de ciel plus proche ⇒ repêché comme un sol.
> **Sonde** : `d` devient `d<denied>/<kill>`. Les deux chiffres partitionnent
> les classes de trous : `d>0` = famine cache/slot ; `kill>0` = le BUDGET a
> choisi ; **les deux à 0 = arithmétique d'occlusion** (cull LOS / bandes
> portales / masque) — c'est là qu'il faudra creuser ensuite. Pool 5,64 Ko ;
> build normal bit-intact. NON validé console (6e disque).
>
> **ROUND 34c/d/e (2026-09-04, 5df5236)** — 6e disque : `d0/0` aux deux
> endroits ⇒ par la partition r34b, il ne restait que **l'arithmétique
> d'occlusion**. Audit multi-agents (5 auditeurs, un par mécanisme, chaque
> constat vérifié de façon ADVERSE) : deux défauts confirmés, les hypothèses
> « cache de masques » réfutées.
> **(1) LA FENÊTRE DE JOB PARTAGÉE — la cause, spécifique aux plafonds PAR
> CONSTRUCTION.** En mode slave, le sol et le plafond d'un sous-secteur
> partagent UNE réservation d'indices : la pré-passe les facture ensemble dans
> `psw_sf_bill[k]` et `psw_cmd_left()` est le reste de TOUT le job. La passe 0
> (SOL) tourne d'abord et peut dépasser sa part — le bras fines V-bands émet
> jusqu'à 8 × (fenêtre + quad) = 16 commandes contre une réserve de +9 — et
> chaque commande prise en trop sort de celle du PLAFOND. La passe 1 trouve
> alors `psw_cmd_left() <= 0`, tous les émetteurs sortent sur leur garde, et le
> plafond n'émet RIEN : aucun slot refusé, aucun verdict de budget = `d0/0`.
> Correctif : retenir la garantie round-A du plafond hors de portée de la passe
> sol, la lui rendre (plus ce que le sol a laissé) à la passe 1 ; plafonner la
> boucle V-bands au bord de la fenêtre.
> **(2) LE CULL DE PLAN ENTIER, RESSUSCITÉ (dg:9863).** Le chemin rapide
> own-quad du round 25 applique la spec des 5 sondes de l'owner — donnée pour
> UNE tuile 64×64 — à une pièce fusionnant jusqu'à TROIS rangées de tuiles, et
> fait `return` de `psw_emit_plane_tiles` sur un positif : le plan ENTIER est
> abandonné. C'est mot pour mot le cull que le round 20 avait SUPPRIMÉ comme
> non fondé (« un trou de la taille d'un triangle avec k0 d0 r0 … échantillonner
> des points n'est PAS une preuve qu'une RÉGION est cachée »), et l'échec
> enregistré au round 20 est exactement ce symptôme-ci. Correctif : le chemin
> rapide refuse les pièces multi-rangées sur plan mixte ⇒ la marche de grille
> les cull par tuile, comme elle le fait déjà.
> **Aussi** : le crédit de repêchage r34b est restreint aux frames
> master-inline (en slave la FACTURE EST la banque, donc une facture gonflée
> faisait jeter des jobs PROCHES entiers par la ceinture — console `F15/66`
> alors que le disque précédent lisait `F../0`) ; le packer d'arène rationnait
> loin→proche et gardait donc les jobs LOINTAINS en jetant les PROCHES (ses
> trois limites : cap de jobs, ceinture de prefix-sum, ceinture de banque) ;
> `psw_kill_n` compte désormais des PLANS et non des sous-secteurs (appairage
> 1:1 avec les repêchages par passe du round C) ; la clé du LRU de masques
> gagne le pavage contre lequel le masque est indexé (64 o).
> **Row 13 → `P34 e<ef>/<ew> h<hid>/<zero> F<join>/<drop> f<cmds>
> d<denied>/<kill>`** — `h` partitionne les pertes de plafonds : `hid` =
> déclaré caché au NOTE, `zero` = arrivé à l'émetteur et n'a rien émis.
> `x<wd>` est COUPÉ (question soldée : `x-` sur neuf captures) et `K` cède sa
> colonne.
> **POOL** : r34e mesurait 4,81 Ko, au ras du plancher de boot 4,8. Récupéré à
> **6,55 Ko** en coupant la sonde `x` et en ramenant le tas newlib
> sur-provisionné (console `hp1256/4096!0` = pic 1256 o) de 4096 à 2560 o,
> **gardé par `SAT_PSW`**. Ce gate a révélé un piège de build : make suit les
> SOURCES, pas les CFLAGS, donc builder -Psw puis normal relinkait le
> `syscalls.o` PSW et le pool « bit-intact » lisait 62,83 Ko au lieu de 61,33.
> `src/syscalls.c` rejoint la liste des fichiers TOUJOURS retouchés de
> `build.ps1` ; vérifié en enchaînant les deux builds : normal bit-intact.
> NON validé console (7e disque). Attendu : plafonds pleins ; si des trous
> persistent, `h<hid>/<zero>` dit lequel des deux étages accuser.
>
> **ROUND 35 (2026-09-04, 6be210a)** — 7e disque : aucun changement. Et
> l'audit avait déjà signalé pourquoi je cherchais au mauvais endroit :
> **`kill 0` n'a jamais prouvé « rien en standby »** — `psw_kill_n`
> s'incrémentait une fois par SOUS-SECTEUR même quand les DEUX passes
> tombaient, alors que le round C décrémente par PASSE repêchée. Un sub qui
> perd les deux et en récupère une lit 0. Toutes les captures `d0/0`
> précèdent le fix r34e du compteur ⇒ **la classe BUDGET n'a jamais été
> éliminée**. Les défauts corrigés en 34b/34c/34e étaient réels et vérifiés,
> mais rien ne prouvait qu'ils étaient LA cause.
> **Correctif : LES SOLS CÈDENT AUX PLAFONDS.** Round A passe en deux
> balayages — les plafonds prennent leur garantie `min(4,e)` d'abord,
> proche→loin, plafonnés à la moitié du budget ; les sols dépensent ensuite
> le reste (le plafonnement est un CAP sur le balayage plafonds, pas un
> plancher). Raison : sous saturation (`c456` sur 487, ~120 plans candidats
> pour ~225 commandes) la moitié des plans tombe quoi qu'il arrive — la
> question est LESQUELS. Un SOL perdu est recouvert par le sol matériel RBG0
> (dégrade) ; un PLAFOND perdu n'est recouvert par RIEN (trou). Même loi que
> « les murs cèdent aux things », et **cette asymétrie explique aussi
> pourquoi le symptôme a toujours été spécifique aux plafonds** : les sols
> se trouent autant, RBG0 le masque. Marqueur `P35`, format inchangé. Pool
> 6,58 Ko ; build normal bit-intact. NON validé console (8e disque).
>
> **ROUND 36 (2026-09-06, f38d556 + core d0e6b9d)** — 8e disque, 3 captures :
> `d0/0` partout avec le `kill` désormais honnête ⇒ **la classe BUDGET est
> éliminée SUR PREUVE** (r35 était donc un no-op, comme observé). Restent
> `hid` (5/2/1) et `zero` (3/2/0), et les deux suivent le symptôme : les
> deux captures du MÊME endroit lisent `h5/3` sans les plafonds et `h2/2`
> avec ; le triangle derrière le pilone lit `h1/0`.
> **1. LE FOLD RÉCLAMAIT CE QUE LE PEINTRE REFUSAIT** (core d0e6b9d). Le
> portal-band fold ferme la bande d'une colonne sur la région plafond/sol du
> secteur avant sur la seule foi de `ceilvis`/`floorvis` — prédicats
> géométriques « un renderer vanilla dessinerait ça ». Dans le peintre cette
> région est un candidat VDP1 séparé que le hook de note peut REFUSER : le
> plan refusé voyait quand même sa région déclarée opaque, donc tout ce qui
> était derrière était cullé par une promesse que personne n'honorait — un
> refus en germe = un COULOIR de plans perdus, la forme d'un « gros trou ».
> `sat_psw_fold_cvis`/`_fvis` publient ce que le hook a vraiment gardé. Sûr
> par construction : les bandes ne font que rétrécir, une promesse plus
> faible ne peut pas créer de trou (elle peut coûter des commandes).
> **2. `hid` SPLITÉ** — row 13 `h<clip>.<band>/<zero>`. `clip` =
> `psw_plane_poly` < 3 sommets ; `band` = `R_PswBandBoxHidden`. `clip` est le
> premier suspect : son plan proche vaut `ph*hw2/rows`, et un plafond est ~2×
> plus loin de l'œil qu'un sol ⇒ rayon ~2× plus grand (~162 u contre ~78 u) :
> **plafond-lourd par arithmétique**, la forme exacte du symptôme.
> **3. `zero` EXHAUSTIF** : les deux sorties « non projetable » de l'émetteur
> n'étaient comptées nulle part — un plafond disparu pouvait lire `h0/0`.
> Marqueur `P36`. Pool 6,48 Ko ; build normal bit-intact. NON validé console
> (9e disque).
>
> **ROUND 36b (2026-09-06, f9361e7)** — 9e disque : rien changé. Le
> propriétaire apporte un bisect : **ces trous n'existaient pas avant le
> déport des flats au slave (r33)**. Les 7 captures règlent déjà ceci :
> `band` DOMINE (1 à 12 plafonds refusés/frame), `clip` 0-2, `zero` 0-3, et
> **`denied`, `kill`, `drop` lisent ZÉRO dans les sept** ⇒ toutes les classes
> « pas assez de place » sont mortes ; ce qui reste est un refus
> d'occlusion. Deux découvertes de lecture : **`V1- B` =
> `vdp1_budget_cmds`** (B0 = frame propre ; B>0 = transfer-over, budget HW
> latché — `c395 B459` et `c288 B247`, donc 41 commandes jamais tracées et,
> l'émission allant loin→proche, ce sont les PLUS PROCHES) ; et **les
> captures L+X ne sont pas un miroir fidèle** (`sat_wall_paint` re-route tous
> les murs vers `wall_emit_flat`, un quad plat vert chacun : c370 contre
> c456 sur la même scène).
> **Levier : pad R+X**, marqueur `P36a/b/c/d` (`sat_psw_ceilab`) — a
> shipping, b cull par bandes des plafonds coupé, c terme HAUTEUR du plan
> proche coupé pour les plafonds, d les deux. Une photo est
> auto-descriptive. Application de [[interbuild-perf-noise]] : exiger un
> toggle vif, les photos build-contre-build ne sont pas admissibles.
> Pool 6,33 Ko. NON validé console (10e disque).
>
> **ROUND 37 (2026-09-06, 7e57c97 + core 71eef15)** — verdict de l'A/B R+X :
> **le triangle ne se referme dans AUCUN des quatre modes, mais certaines
> parties du plafond au spawn SE REFERMENT.** Deux défauts distincts ; le
> triangle n'a jamais été dans la classe chassée.
> **Le triangle = effondrement au LOAD.** `psw_leaf_poly` découpe la cellule
> BSP par chaque seg du sous-secteur en gardant le côté du secteur avant. Un
> seg qui ne tourne pas ainsi (mal orienté, longueur nulle après les splits,
> ou numériquement limite sur une feuille déjà pincée) élimine la cellule
> ENTIÈREMENT : `psw_pvn` → 0, plus aucun polygone, sol ET plafond refusés au
> note pour toute la vie du niveau. C'est le « c'est du calcul » du round 22 —
> un verdict de load, d'où l'immunité à tout A/B runtime.
> **Fix : le seg est SAUTÉ, pas obéi.** Garder la cellule ne peut que rendre le
> polygone trop grand, et un flat trop grand est repeint par la géométrie plus
> proche (même troc que l'overflow de path). Un trou n'est pas rattrapable, un
> overdraw si. Sonde `s<segskip>` en fin de row 13, CONSTANTE PAR NIVEAU.
> Fermé au passage : `psw_plane_poly` copiait `psw_pvn[sn]` sommets dans des
> tableaux `PSW_FAN_MAX` sans cap à lui (débordement de pile latent).
> Marqueur `P37<lettre>`. Pool 6,11 Ko ; build normal bit-intact. NON validé
> console (11e disque).
>
> **ROUND 38 (2026-09-06, e679a9d)** — round 37 raté aussi. Le propriétaire
> propose la bonne instrumentation : **peindre chaque plan refusé dans la
> couleur de son site de refus**. Un compteur dit combien sans dire lequel ni
> où ; le marqueur rend le trou auto-descriptif (forme = le plan, couleur = la
> cause).
> **Pad L+DOWN, row 13 `P38.<digit>`** : 0 shipping · **1 FLATS SUR LE MASTER**
> (passe slave OFF — le bisect « pas là avant le déport au slave » devient un
> toggle : ce qui disparaît condamne le chemin r33, ce qui reste l'innocente)
> · **2** + plafonds refusés peints · **3** + sols refusés aussi (invisibles en
> shipping sous RBG0). Couleurs : 176 ROUGE clip monde · 112 VERT bandes ·
> 198 BLEU standby · 163 JAUNE denied · 216 ORANGE non projetable · 250
> MAGENTA émis zéro. Trou NOIR = refusé par aucun site ⇒ perte en aval. Le
> ROUGE retombe sur la feuille BRUTE (donc il prouve que la feuille existe).
> Payé : A/B R+X supprimé, `e<ef>/<ew>` retiré de la row, `HEAP_SIZE`
> 2560→1792 sur slack mesuré (`hp1256/2560!0`) ⇒ pool 4,95→5,70 Ko contre le
> plancher 4,8. Surveiller `!` sur row 10. Build normal bit-intact. NON validé
> console (12e disque).
>
> **ROUND 38b (2026-09-06, 0aa639c)** — 12e disque, deux résultats.
> **(1) LES FLATS SUR LE MASTER CORRIGENT LES PLAFONDS DU SPAWN** ⇒ le chemin
> jobs/réservation du round 33 est CONDAMNÉ pour cette famille ; le bisect du
> propriétaire est prouvé par un toggle. Chercher désormais dans la
> réservation d'index / la fenêtre de job / l'arène / le landing au fence —
> pas dans les verdicts d'occlusion.
> **(2) Le triangle : RIEN n'est peint dessus** — un fait sur la SONDE, pas sur
> le plan. Le marqueur r38 avait quatre sorties muettes. r38b : retry sans le
> terme hauteur du plan proche → feuille brute → **repli sur la bbox écran des
> MURS du sous-secteur** ; le CIEL (`clump < 0`, jamais un plan PSW donc sans
> verdict) est peint BLANC ; et **`m<want>/<got>`** rend la sonde
> auto-contrôlée (`want > got` = le marqueur n'a pas pu être dessiné).
> `o<ovf>` entre (subs jamais pris par le recorder) ; `d<denied>/<kill>` sort.
> `s0` console = le fix seg-skip du round 37 ne se déclenche jamais.
> Pool 5,19 Ko contre le plancher 4,8. Build normal bit-intact. NON validé
> console (13e disque).
>
> **ROUND 39 (2026-09-06, 493c79d)** — audit à 12 agents du chemin r33 ; TROIS
> auditeurs indépendants convergent, le vérificateur adverse ne casse pas.
> **LA PASSE SOL MANGEAIT LA DOTATION DU PLAFOND.** `psw_sf_bill[k]` est la
> SOMME sol+plafond et le slave la donne à UNE fenêtre par job, sol d'abord
> (en mode slave `psw_cmd_left()` EST la fenêtre). r34e retenait une réserve
> plafond mais la plafonnait à 4, alors que le round B facture au plafond son
> `2*ce+1` complet (17, 25, 33) : le sol dépensait toute la montée en gamme et
> le plafond rentrait avec 4 commandes pour 16 tuiles. La marche de tuiles ne
> s'interrompait même pas (son `stop` suit le cap GLOBAL, jamais la fenêtre).
> Exempt sur le master PAR CONSTRUCTION — d'où le résultat console.
> **Correctifs** : `psw_sf_cbill[k]` (moitié plafond seule) devient la réserve
> retenue ; la loi du solide consulte la FENÊTRE (dégrade au lieu de tronquer) ;
> `psw_sf_drop` était INATTEIGNABLE et compte enfin ; `PSW_SF_JOB_CAP` 144→288
> avec sa troncature comptée ; le marqueur voit les pertes PARTIELLES.
> Pool : pré-vol ÉCHOUÉ à 4,69 Ko, récupéré à **6,50 Ko** (psw_sub lumps
> int→short = 1536 o, psw_no_room non-inline, champ `s` coupé, HEAP_SIZE
> 1792→1536). Marqueur `P39`. Build normal bit-intact. NON validé (14e disque).
>
> **ROUND 40 (2026-09-06, 8d20242 + core)** — **LE TRIANGLE : LE RABOTAGE DE
> FEUILLE RETIRAIT DE L'AIRE.** Masque du trou fourni par le propriétaire : un
> long triangle fin, ciel visible derrière, même endroit, immunisé à tous les
> A/B, **peint par aucun marqueur de refus** — donc RIEN n'a été refusé : le
> plan est émis, simplement plus petit que sa feuille.
> `psw_poly_shave` ramenait un polygone de feuille sous le cap de sommets en
> supprimant le coin le plus plat ; supprimer un sommet remplace deux arêtes
> par une CORDE et retire un triangle. Aire perdue = région jamais peinte,
> décidée au CHARGEMENT — d'où l'immunité aux toggles et l'absence de compteur.
> La fonction portait déjà ce bug deux fois (tail-chop, puis coins plus petits) :
> toujours vers l'intérieur.
> **Fix : supprimer une ARÊTE**, en prolongeant les deux arêtes voisines jusqu'à
> leur intersection. Sur un convexe cela AJOUTE de l'aire ⇒ plus jamais de trou ;
> le prix est un sliver de surdessin que le peintre absorbe.
> Sonde `s<shaved>` (feuilles rabotées, constante par niveau) — le champ `s`
> change de sens (ce n'est plus segskip). Marqueur `P40`. Pool 5,44 Ko. Build
> normal bit-intact. NON validé console (15e disque).
>
> **ROUND 41 (2026-09-07, d8196ae)** — 15e disque : spawn ET triangle toujours
> absents. **Neuf rounds, neuf défauts réels corrigés, zéro mouvement** ⇒ je
> devine l'ÉTAGE. On achète la bifurcation au lieu d'un 10e correctif.
> **Pad L+bas, 3 crans** : 0 shipping · 1 flats sur le MASTER · **2 PLAFONDS
> BRUTS** (chaque plafond = UN fan solide de son polygone de feuille COMPLET,
> masque LOS par tuile / `cull_h` / standby / marche de tuiles tous coupés ; le
> fan garde TOUS ses sommets). Trou qui se remplit ⇒ la perte est dans
> `psw_emit_plane_tiles` ; trou qui reste ⇒ le polygone n'atteint pas la zone.
> **Nouveau `t<n>`** = tuiles de plafond sautées par le masque ou `cull_h` — le
> mécanisme « le code pense qu'on couvre tout », jamais affiché jusqu'ici.
> **Peinture des refus supprimée** (verdict `m0/0` rendu ; 960 o + 192 o).
> Pool 4,91 → **7,11 Ko**, au-dessus de la cible de confort, sans avertissement.
> Marqueur `P41`. Build normal bit-intact. NON validé console (16e disque).
>
> **ROUND 42 (2026-09-07, bd2ac37 + core 19e17b2)** — **les deux défauts
> démontrés par lecture.** (1) **Le triangle** : le fold des bandes réclamait
> le tier opaque même quand le tier n'était jamais peint (refus `sat_psw_ref`,
> abandon du chemin magnifié, cull de bande) → tout plafond derrière répondait
> `R_PswBandBoxHidden` = plan ENTIER perdu. Preuve : `h../<band>` = 4 sur
> toutes les captures **y compris au cran 2**, où rien d'autre ne peut tuer un
> plafond. Fix `psw_tier_endx` (borne de colonne réellement peinte) ; le round
> 36 avait cette loi et ne l'avait appliquée qu'aux PLANS. Le jumeau SOL ment
> pareil mais RBG0 le remplit — d'où neuf rounds « plafonds seulement ».
> (2) **Les plafonds du spawn** : le round 39 a câblé « dégrader, pas tronquer »
> sur la seule décision d'ENTRÉE ; un plan qui vide sa fenêtre à mi-marche
> s'arrêtait net (`psw_tile_short` sans lecteur depuis r41), sur le SLAVE
> seulement — `F15/13` contre `F0/0` au cran 1. Fix **`PSW_COVER` = 4
> commandes retenues par job** ⇒ un manque repeint la feuille en fan solide,
> sans avoir à savoir quel terme de `2*ce+1` est faux. Nouveau `c<cover>` ;
> `s<shaved>` sort (0 partout, le rabotage r40 ne se déclenche jamais).
> ⚠ Un fold plus faible cull moins ⇒ plus de commandes : surveiller `f` et
> `V1- c`. Pool 5,83 Ko. Marqueur `P42`. Build normal bit-intact. NON validé
> console (17e disque).
>
> **ROUNDS 43–47 (2026-09-07, 6597363…32fb7eb)** — consolidé. r43 : côté MUR
> sur la ligne (retiré en r46, w3/0 hors de cause). r44 : pot de réserve fine
> financé en amont — famine de fenêtre ÉTEINTE, **prouvé console `F16/0 c0`**.
> r45 : facture des solides exacte (`psw_sub_q`) — juste mais SANS PLAFOND ⇒
> régression 1,5 fps (FMp ../352, mx361, MST666). r46 : garde r37 sur la boucle
> du CHEMIN BSP + `L<noleaf>/<pathskip>.<ovf>` + `x<peekmiss>` (le slave ne peut
> que PEEK les slots, le master CHARGE) ; PSW_SUB_MAX 384→320 ; **console
> `L0/0` = théorie feuille-manquante morte sur données**. r47 (GO propriétaire) :
> **`PSW_UPGRADE_CAP 10`** (fixe la régression) + **pad L+HAUT =
> `psw_pp_base_near`** — premier A/B réel du near-clip (l'audit 19 agents a
> confirmé que tous les « cran 2 » re-dérivaient le verdict qu'ils
> désarmaient, et que ce flag n'a JAMAIS été écrit). Marqueur `P47<N?>`.
> Pool 6,36 Ko. Build normal bit-intact. NON validé console (21e disque).

> **ROUNDS 48–51 (2026-09-07/08, 5de8675…)** — r48 : hard crash console des
> disques r45/r47 ⇒ **ROLLBACK à la base P44** + la corde L+HAUT seule (r45..r47
> hors disque, à re-bisecter un par un). r49 (déclencheur = la géométrie du
> propriétaire, mot pour mot) : un plafond n'arme JAMAIS `cull_h` (la loi r20
> « un échantillon ne prouve pas une région » appliquée PAR TUILE) + round B :
> tuilé ⇒ slot ACQUIS sinon solide honnête. r50 : les 4 captures d'états
> (plein/demi/trou) ferment la famille des refus (h figé, N sans effet, t0) ⇒
> garantie de présence plafond cède à la BANQUE seule, `k<kill>` sur la row —
> **RÉFUTÉ console (k0 sur 6 captures)**. **r51 — LE FIX** : relecture complète
> du chemin de verdict ⇒ le HIT DU MÉMO de masque refacturait le plafond à
> `vis` (dg_saturn.cxx:~8739 sans gate `pass==0` — la moitié oubliée de r49,
> qui avait gaté l'inline et la fence). Au REPOS (mémo hit) : fenêtre de job
> ≈ `vis` ⇒ `ebill>fenêtre` ⇒ branche SOLIDE = **l'aplat slave** ; `vis` 0-1 ⇒
> fenêtre < fan 4 quads ⇒ **fan tronqué = le trou triangulaire**, taille ∝ vis
> = les états plein/demi/trou. Master exempt (fenêtre = banque), mouvement
> exempt (mémo miss). Empreinte : `F../<drop>` 1-13 avec k0 t0 h figé. Fix =
> 1 ligne (gate) ; row : `t` (mort r49) → **`s<solid>`** = passes plafond sans
> slot texture (l'aplat compté ; s haut stable = famine 8 slots = round 52).
> Marqueur `P51`. Pool 5,47 Ko. Build normal bit-intact. 25e disque.
> **Verdict console : RÉFUTÉ** (spawn aplat avec s0/F5-0/c0 ; trou avec
> h1.4/F12-9/s12). L+X owner : spawn = MAGENTA (fans solides), trou = AUCUNE
> peinture, symétrie sol/plafond (poinçons OR absents = subs sans job).

> **ROUND 52 (2026-09-08)** — audit workflow 13 agents (5 finders + vérif
> adversariale) : **3 mécanismes CONFIRMÉS, 5 réfutés sur preuve**.
> (C0) les `-1` de psw_emit_baked contournaient psw_no_room → stop silencieux
> du reste de la marche (zéro drop/tile_short/cover) — le dernier refus non
> compté, slave-only. (C1) **cascade revendication/refus** : cvis publié à la
> NOTE, un étage avant les verdicts d'émission → bandes fermées → feuilles
> lointaines tuées (`h.band`) → sur slave les drops falsifient la
> revendication a posteriori → LE TRIANGLE (master : mêmes kills = occlusion
> exacte, pas de trou). (C2) la retenue cover r42 (−4) jamais financée : plan
> tuilé facturé 2e+1, marche sur 2e−3, coût ~2e ⇒ manque ≥3 par construction
> hors deal fin. Fixes : (A) les deux -1 du baked passent par psw_no_room ⇒
> le cover r42 s'applique enfin ; (B) round B facture `e−4+PSW_COVER` (sf
> seulement, sols+plafonds+cbill). Réfutés : staging/DMA, paire window/quad,
> fan !done comme source du magenta spawn, seal midtexture (réel mais
> mode-indépendant), timing des slots. ⚠ le magenta UNIFORME du spawn reste
> sans mécanisme confirmé — à relire sur ce disque. Discriminateur : cran 1
> au spot du triangle ⇒ h.band reste ~4 + trou disparu = cascade confirmée.
> Marqueur `P52`. 26e disque.
> **Verdict console : RÉFUTÉ sur les deux symptômes** (spawn magenta s0/c0 ;
> trou h1.4 F14/4 s14 c1) — toutes les routes comptées vers le solide sont
> exclues sur console.

> **ROUND 53 (2026-09-08)** — plus d'inférence : **le fan par-pièce est peint
> par sa CAUSE D'ENTRÉE en L+X** (BLANC = okq/near-guard, GRIS = vend≤v0,
> ROUGE = portes fine+coarse refusées sans drop, MAGENTA = famine par-pièce
> et fan plan-entier/cover). Row : `k` (k0 depuis r50) → `n<fanq>` = fans
> émis par frame. Une capture L+X au spawn désigne la porte ; magenta
> persistant avec s0/c0 ⇒ auditer les compteurs s/c eux-mêmes. Triangle :
> le discriminateur cran 1 au spot reste À FAIRE (« master fixe les trous »
> date des trous r34, jamais vérifié sur LE triangle). Marqueur `P53`.
> 27e disque.
> **Verdict console : LES DEUX RÉPONSES.** Capture spawn L+X `h1.4 F14/4
> f162 n25 s14 c1` ⇒ magenta = **famine de slots plan-entier** (s14). Et
> « L+Bas ne remplit pas le trou » ⇒ le triangle est **mode-indépendant** ⇒
> cascade slave réfutée pour le trou.

> **ROUND 54 (2026-09-08, core e86f438 branche `psw-world-r54`)** — les deux
> fixes nommés par la console : (1) **seal midtexture gaté sur les colonnes
> peintes** (`psw_mid_endx`, r_segs.c — le dernier mensonge de fold
> mode-aveugle ; conservateur par construction) ; (2) **wnt=0 sous sf** aux
> grabs round B — plus de chaînes de slots en frames slave, 8 slots = 8
> lumps distincts, les plafonds sont servis (coût : pas de strips 64×128/192
> en slave ; la facture 2e+1 reste une borne sup). ⚠ pointe core `psw-world`
> = cc7f109 (r46 parké) ; le gitlink pointe `psw-world-r54` ; réconciliation
> = décision owner. Attendu : spawn s→~0 + plafonds texturés ; triangle :
> h.band 4→0-1 et remplissage orange. Marqueur `P54`. 28e disque.
> **Verdict console : RÉFUTÉ, avec le fait décisif** — `s14` IDENTIQUE sur
> P52/P53/P54 = population constante ⇒ **la règle fine r34** (feuille ≤32u ×
> ≥64u ⇒ solide à la note, les deux modes) = l'aplat du spawn PAR DESIGN,
> hors de portée de tout fix budget/slot/fenêtre. Et h.band=4 avec le gate
> seal prouvé dans le binaire ⇒ le dernier menteur = **la revendication de
> région** (ceilvis/floorvis).

> **ROUND 55 (2026-09-08, GO owner explicite)** — les deux règles
> renversées : (1) règle fine r34 SUPPRIMÉE (psw_thin/PSW_THIN_U effacés) —
> les feuilles fines prennent la marche normale, texel-exacte (r20 fines +
> r25 axis couvrent les artefacts d'époque) ; facture 2e+1 au lieu de 4.
> (2) revendication de région SUPPRIMÉE du fold (core) — les bandes ne
> plient QUE le peint ; un tier peint n'étend la bande que s'il la touche.
> Prix assumé : culls plans-derrière-plans perdus → `f` monte, le budget
> honnête dégrade les sols d'abord ; si la perf s'effondre en scène ouverte,
> le levier de retour = une occlusion par-feuille SAINE (rangées projetées),
> jamais la revendication. Attendu : spawn TEXTURÉ (s→~0), triangle REMPLI
> (h.band→0-1). Marqueur `P55`. 29e disque.
> **Verdict console : les COMPTEURS ont suivi (band 4→0-1 ✔, s dynamique ✔),
> le VISUEL non** — « c'est toujours du ciel » (trou, pas aplat) + magenta
> spawn persistant. Élimination finale : la seule sortie non blanchie d'un
> plafond jamais-émis = **`h.clip` = 1-2, CONSTANT depuis 15 rounds** — le
> kill `psw_plane_poly < 3` ; l'A/B des clips latéraux 45° planifié en r47
> n'avait jamais tourné.

> **ROUND 57 (2026-09-08, disque unique = r56+r57)** — méthode owner
> (« comment on avance pour de vrai ») : chaque round SUPPRIME UNE CLASSE
> PAR CONSTRUCTION. (r56) les 8 slots texture aux 8 lumps les plus demandés
> (agrégat par lump des factures round-B, sf seulement — le master churne
> déjà). (r57) **un collapse des clips latéraux ne tue plus jamais un
> plan** : retry near-seul depuis les sommets monde (le 2e clip latéral
> clobber son buffer — le fallback re-clippe), `psw_pp_nearfb` ⇒ solide
> forcé facturé 4 ; coût borné par le compteur (1-2 plans/frame). Classes
> mortes par construction : sondes (r49), standby (r50), mémo (r51), stops
> baked (r52), seal (r54), revendication+règle fine (r55), premier-arrivé
> slots (r56), collapse latéral (r57). Attendu : `h.clip`→0-1 résiduel,
> triangle rempli (aplat accepté), spawn s en chute. Marqueur `P57`.
> 30e disque.

> **ROUND 58 (2026-09-08, 31e disque, marqueur `P58`) — REBOOT FRESH-EYES
> (demande owner : « repars de zéro, garde les faits ») + VERDICT P57 :
> double échec (aplats ET trou inchangés ; couloir `s0 n8` = la famine de
> slots N'ÉTAIT PAS l'aplat).** Deux traceurs indépendants, ZÉRO théorie
> transmise, interdiction de croire les commentaires ; livrable = arbres
> de décision exhaustifs. LES DEUX ARBRES ONT RENDU :
> **(1) L'APLAT = LE COVER r42 (hypothèse owner « caché par un aplat »,
> prouvée code)** — une SEULE commande perdue dans le walk plafond ⇒ fan
> solide PLEINE FEUILLE stagé APRÈS les tuiles texturées ⇒ l'ordre de
> liste VDP1 le peint PAR-DESSUS ; slave seulement (psw_cmd_left = fenêtre
> de job vs banque entière master) ; s0 expliqué (le compteur ne tire qu'à
> l'entrée, le cover tourne slot valide). Fix P58 : **le solide ne repeint
> plus jamais par-dessus ni à la place de la texture plafond sur frame
> slave** — cover plafond SUPPRIMÉ (compté `c../<cs>`), pièce famine
> whyfan-3 SAUTÉE (compté `.<fs>`), holdback PSW_COVER rendu au walk
> plafond (+4, +8 avec le covf r52 toujours facturé). Un déficit = TROU
> compté, à refinancer sur mesure.
> **(2) LE TROU : deux classes MUETTES jamais regardées en 57 rounds** —
> (a) l'arrêt CAP-GLOBAL en plein walk (6 sites `return` nus, ni compteur
> ni tile_short, LES DEUX MODES = colle au mode-indépendant ; le jumeau
> baked avait été compté r52, les sites live oubliés) ⇒ compté `x` +
> tile_short armé ; (b) l'overflow du recorder (compteur latché mais PLUS
> IMPRIMÉ depuis r38b) ⇒ row `o<ovf>/<subn>`. Candidat 3 en réserve :
> overflow 32-bit du prédicat de clip (>~23k unités, sonde si o0+x0).
> Innocentés par les arbres : coherency slot slave (purge avant dispatch,
> aucun write master post-dispatch), fallback r57 (flux de buffers propre),
> `h.clip`=1 (kill near LÉGITIME — borne exacte vérifiée ⇒ le quatuor h
> sort de la row). Bonus : psw_tile_cull recompte les culls SOLS (gate
> psign<0 = 2e compteur qui ne pouvait pas tirer ; piste bande OR sol).
> Row 13 refondue : `P58.<diag><N?><!/-> s<esol>/<miss> c<cov>/<cs>.<fs>
> x<capstop> o<ovf>/<subn> f n` (`!` = corps slave coincé → flats master).
> Lecture : plafonds slave TEXTURÉS + cs/fs>0 = cover était l'aplat ;
> triangle : o>0 = recorder, x>0 = cap, o0+x0 = candidat 3.
> **Pré-pin r56 SUPPRIMÉ** (console : s 14→20 = nuisible ; l'arbre le dit
> protecteur de chaînes fossiles ; ~300 o rendus à un pool passé sous le
> plancher). Round B near→far redevient l'allocateur de slots.

> **ROUND 59 (2026-09-08, 32e disque, marqueur `P59`) — VERDICT P58 :
> L'APLAT EST MORT (« je vois bien les textures au spawn ») = le cover
> r42 était l'aplat, hypothèse owner validée console.** `miss=0` partout
> (slots innocentés), esol = échelle+LOD (pas un défaut), déficit résiduel
> ~5 cmds/frame (`c0/0-1.0-4`) = les tuiles manquantes. **`o0` ET `x0`
> avec le triangle à l'écran = recorder et cap-stop ACQUITTÉS** ⇒ r59 :
> (1) **prédicat psw_clip_dir en 64-bit, UTILISÉ** (wedge par fausse
> corde = dernier candidat des arbres ; `y` = sommets au signe 32-bit
> menteur, garde ε, fdiv normalisé par shift commun r31) ; (2) **covf
> plafond +2** = le déficit mesuré financé (la taxe se lirait en esol).
> Pourquoi le déficit existait : fenêtre par-sub PRÉ-FACTURÉE du slave
> (obligatoire pour émettre en parallèle dans des plages d'index
> réservées, loi 2e+1) vs banque entière du master — les pièces de bord
> en chemin fin coûtent jusqu'à 10-16 cmds contre 2 facturées. Owner
> sceptique sur la direction triangle : P59 (in)valide le candidat 3 à
> coût nul ; si y0 + triangle ⇒ P60 = probe visuel CONTOUR du polygone
> clippé (trou dans/hors contour = walker/amont), conçu, non câblé. */

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
