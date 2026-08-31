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
> 1,2-1,8 ms, MST 33 verrouillé = 30 fps). Étape 2 **CONSTRUITE** (commits 2d8034f/2d60939,
> build vert, pool 46,9 Ko) — non validée console. Row 13 = `PSW t r f`.

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
