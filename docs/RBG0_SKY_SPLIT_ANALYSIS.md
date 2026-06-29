# Sol HW RBG0 + Ciel HW NBG0 en split-screen — analyse de faisabilité

> Branche `rbg0-rework`. Analyse multi-agents (lecture seule) recoupée contre le code réel
> (2026-06-29), puis corrigée d'après confirmation : **sur HW actuel, RBG0 et le ciel NBG0
> cohabitent proprement** ; les commentaires/mémoires qui disaient l'inverse sont **obsolètes**
> (voir §8). Ce document est de la **conception** ; aucun code n'est modifié ici.

---

## 1. Verdict

**Faisable, par paliers.** Approche recommandée : **fenêtrer le sol RBG0 (VDP2 Window0) sur la
seule bande du joueur élu (P1 par défaut) + hole-punch *viewport-aware* dans `core/`**, les
autres vues gardant leur sol software. C'est gratuit en VRAM/cycles, sans risque neige nouveau,
et **tous les registres de fenêtre tombent dans le block-flush no-slSynch existant**.

- **Sol HW pour P1 / software pour les autres** — palier principal, faible risque.
- **Les deux joueurs en sol HW (RPA/RPB)** — stretch réaliste en 2p (poke RPMD hors-flush + un
  2ᵉ transform/frame).
- **Ciel HW multijoueur** — **PAS bloqué** (RBG0+ciel coexistent sur HW) : c'est de la **pure
  mécanique par-vue** (élection du joueur qui profite le plus du ciel → scroll NBG0 sur son
  `viewangle` → fenêtre NBG0 sur sa bande → hystérésis). Même niveau de difficulté que le stretch
  sol RPA/RPB.
- **Quad (3-4 joueurs)** : reste 100 % software (limite matérielle, voir §6).

---

## 2. Comment marche le split aujourd'hui

- **Un seul appelant** : `D_Display()` — `core/d_main.c:316-393` (PAS `g_game.c`). Boucle
  **séquentielle** sur le master SH-2 (pas de parallélisme master/slave : le renderer dual-compilé
  déborde les 2 Mo, rejeté). **P1 (i=0) rendu AVANT P2.**
- **Garde-fou** : split seulement si `sat_local_players > 1 && usergame` (`d_main.c:328`) — sinon
  `players[1..].mo` NULL sur l'attract/demo loop → deref.
- **Boucle** (`d_main.c:355-365`) : pour `i=0..n-1` → pose `sat_split_view = i`, `sat_wall_skip`,
  `R_SetViewWindow(vpx[i], twop?0:vpy[i], hw, fh)`, puis `R_RenderPlayerView(&players[i])`.
- **Identité du viewport courant** : `sat_split_view` (=i, `d_main.c:360`) + `sat_split_active`
  (`d_main.c:349/368`). Pas de `sat_current_view`. `displayplayer` = chemin 1p uniquement.
- **VDP1 muraille** : accumulée pour toutes les vues, kickée **une seule fois** après la boucle
  (`d_main.c:366`).

### Géométrie des viewports (`SCREENWIDTH=320`, `SCREENHEIGHT=224`)

`hw=160`, `fh = twop ? 160 : 112`. Tables `vpx[4]={0,160,0,160}`, `vpy[4]={0,0,112,112}`
(`d_main.c:341-342`).

| Mode | Joueur | Rect écran (x,y,w,h) |
|---|---|---|
| **2p (vertical G/D)** | P1 | **(0, 0, 160, 160)** — moitié **gauche** |
| | P2 | (160, 0, 160, 160) — moitié **droite** |
| | (HUD bas) | bande Y 160..223, 64 px |
| **3/4p (quadrants)** | P1 | (0, 0, 160, 112) |
| | P2 | (160, 0, 160, 112) |
| | P3 | (0, 112, 160, 112) |
| | P4 | (160, 112, 160, 112) |

> ⚠️ **Le split 2p est VERTICAL gauche/droite (frontière x=160)**, PAS haut/bas. La fenêtre P1 du
> 2p est **x∈[0,159], y∈[0,223]** (moitié gauche). Le rect `y<112` ne vaut QUE pour le 3/4p.
> Conséquence : la sélection RA/RB pour « 2 joueurs en sol HW » a besoin d'un **sélecteur vertical**
> (W_CHANGE/W0), pas du K_CHANGE per-ligne (qui est horizontal). Voir §4.

### Framebuffer & bornes

- **UN SEUL bitmap NBG1 partagé** : `framebuffer[320*224]` = `I_VideoBuffer`. Toutes les vues y
  écrivent via `ylookup[]`/`columnofs[]`, **reconstruits par `R_SetViewWindow`** :
  `columnofs[i]=viewwindowx+i`, `ylookup[i]=I_VideoBuffer+(i+viewwindowy)*SCREENWIDTH`
  (`core/r_main.c:856-858`). `viewwindowy`+`viewheight` portent déjà top/bottom de la vue courante
  → **la géométrie de rendu de chaque vue est déjà confinée à sa bande.**
- **Composition** : tout à la fin de `DG_DrawFrame`, APRÈS les n `R_RenderPlayerView`. Blit unique
  `framebuffer → DOOM_VRAM` (NBG1, B0), puis clear. Index 0 = transparent VDP2 → laisse percer ce
  qui est derrière NBG1.
- **Conséquence clé** : quand `DG_DrawFrame` (donc `rbg0_set_transform`) tourne, les globales
  `viewx/y/z/angle` contiennent **le DERNIER joueur rendu** (Pn), pas P1.

---

## 3. Approche recommandée — « P1 sol HW / P2 software » via fenêtre VDP2

### 3.a Le mécanisme fenêtre — et il commit sans slSynch

SRL ne wrappe pas la scroll-window VDP2 (`srl_vdp2.hpp` n'a aucun `slScrWindow*` ; son `SetWindow`
est la fenêtre **VDP1**). → appel SGL direct (déjà le mode du fichier) :

```c
slScrWindow0(0, 0, 159, 223);   // W0 = moitié gauche (2p)  -> WPSX0/Y0/WPEX0/Y0
slScrWindowModeRbg0(win0_IN);   // RBG0 affiché DEDANS W0 seulement ; NBG1 reste plein écran
```

| Registre | Offset | Dans block-flush 0x0E..0xFE ? |
|---|---|---|
| WPSX0..WPEY0 / WPSX1..WPEY1 | 0xC0–0xCE | ✅ |
| WCTLA/B/**C**(RBG0)/D | 0xD0–0xD6 | ✅ |
| LWTA0/1 (line-window) | 0xD8–0xDE | ✅ |
| RPMD (rotation mode) | 0x110 | ❌ → poke direct |
| CCRR (color-calc ratio) | 0x10C | ❌ → poke direct |

→ **Tous les registres de fenêtre rectangulaire tombent dans `rbg0_commit_cyc()`**
(`src/dg_saturn.cxx:1718`, `for off=0x0E..0xFE`). Une fenêtre **statique** posée avant le commit
d'init est appliquée **sans poke direct**. **Coût : 0 banque, 0 cycle, risque neige nul** (la window
masque le pixel *après* le fetch rotation).

- *Réserve* : le block-flush n'est appelé qu'à l'init → une fenêtre **mobile** par frame demanderait
  un poke direct des 4 WPxx + WCTLC (trivial, reste 0xC0–0xD6). Pour « P1 = moitié gauche fixe »,
  c'est **statique → rien à faire**.

### 3.b Ancrer la transform RBG0 sur P1

`rbg0_set_transform()` (`src/dg_saturn.cxx:1361`) lit seulement `viewx/y/z/angle` +
`sat_vdp2_floor_h`. Comme elle tourne après la boucle, **réinjecter P1 juste avant** :
`R_SetupFrame(&players[0])` (ou sauver/recharger les 4 globales de P1). Primitive existante, coût
nul. `slCurRpara(RA)` sélectionne déjà le jeu de paramètres.

### 3.c Le hole-punch viewport-aware (le gros morceau, `core/`)

État réel (`core/r_plane.c:1106-1138`) : ce n'est **pas un skip mais un PUNCH** (écrit index 0 sur
les pixels du sol-joueur puis `continue`, sautant `R_MakeSpans`/`R_DrawSpan`). La sélection = **secteur
sous l'œil** `R_PointInSubsector(viewx,viewy)` (`r_plane.c:991`) — pas « dominant coverage »
(abandonné, flicker). Match triple : `pl->height == sat_vdp2_floor_h && pl->picnum ==
sat_vdp2_floor_pic && (pl->lightlevel>>LIGHTSEGSHIFT) == sat_vdp2_floor_band`.

- **Déjà par-vue** : la géométrie du punch est confinée par `ylookup`/`columnofs` → un punch émis
  pendant P1 ne peut écrire que dans la bande P1. **Pas de débord géométrique.**
- **Le vrai point** : un seul plan RBG0 → si P2 punche aussi, son trou montrerait le sol/hauteur de
  P1. **Stratégie asymétrique obligatoire** : seul le joueur élu punche ; les autres dessinent leur
  sol software entier.
- **Delta minimal** : un flag `int sat_rbg0_view = 0` + helper `sat_floor_punch_here()` (=
  `sat_vdp2_floor && (!sat_split_active || sat_split_view == sat_rbg0_view)`), à brancher :
  - à la garde du punch sol (`r_plane.c:1111`),
  - **et impérativement** aux sites de cohérence murs↔sol `core/r_segs.c:429/461/479` (sinon les murs
    de la vue non-punchée seraient cullés/CPU-fallback contre une ligne de sol transparente
    inexistante).
- **Gating DoomJo** : aucun `#ifdef` dans `core/`. Pattern = **flag runtime def 0 + court-circuit
  `&&`** (garder `sat_vdp2_floor &&` en tête → DoomJo ne l'allume jamais → reste 0). Reste **C pur**
  (GCC 9.3 OK).

### 3.d La dominante de P1

Aucune variable neuve : `sat_vdp2_floor_h/pic/band` sont recalculés depuis `R_PointInSubsector` à
chaque `R_DrawPlanes`, donc **capturés naturellement pendant l'itération i=0 (P1)**. Il suffit que la
transform les réutilise (= le re-setup P1 du §3.b).

---

## 4. Stretch — RPA/RPB pour les **deux** joueurs en sol HW

Le RBG0 a **deux jeux de paramètres (RA/RB)**. La VRAM RPT a déjà la place (RA@+0x00, RB@+0x68) et
le memcpy copie déjà les deux (`src/dg_saturn.cxx:3367-3368`) — mais seul RA est rempli. Le sol est
en `K_CHANGE` (déjà posé à l'init), pas en `RA` simple.

| Variante | RPMD | Sélecteur | Axe | Verdict |
|---|---|---|---|---|
| A) Horizontal | K_CHANGE (déjà posé) | bit RA/RB **par ligne** (K-table) | haut/bas | 0 reg hors-flush, **mauvais axe** (2p vertical) |
| **B) Vertical** | W_CHANGE (=3) | **W0** | gauche/droite | **bon axe**, mais W0 + **RPMD hors-flush** |

- **Coût : 0 banque VRAM, 0 cycle** (mêmes 2 reads/dot → **la neige ne revient pas**). Surcoût =
  **+1 transform + RPT/frame** (modeste).
- **Verrou** : W_CHANGE écrit **RPMD (0x110), HORS du block-flush** → **poke direct one-shot à
  l'init** (recette CCRR, `src/dg_saturn.cxx:1487`).
- **Limite dure** : RA/RB = **2 transforms max** → 2 joueurs HW OK, **3-4 impossibles** (RPMD =
  K_CHANGE **XOR** W_CHANGE, pas de grille 2×2).
- **Vaut le coup ?** Pas en premier. Livrer §3 d'abord (gratuit, sûr), tenter la variante **B** après
  validation HW.

---

## 5. Ciel HW multijoueur — pure mécanique par-vue (NON bloqué)

> **Correction (2026-06-29)** : le ciel HW (NBG0 cells en B1) et RBG0 **cohabitent proprement sur
> HW** (confirmé HW + bench `hw-render-path-comparison`). Le « bloqueur » NBG0-char-en-A1 des
> versions précédentes de cette analyse est **résolu/obsolète** (voir §8). Le ciel HW multi n'est
> donc **pas conditionné par un fix HW** — c'est de la mécanique par-vue.

### État réel

- Ciel HW actif = `VDP2_CELL_SKY` (cells 256-couleurs sur **NBG0**, banque **B1**), `sky_bit=NBG0ON`
  (`src/dg_saturn.cxx:3314`). **Un seul scroll** (`slScrPosNbg0`, un seul `viewangle`) → une seule
  perspective, d'où le gate actuel `sat_local_players<=1` (`dg_saturn.cxx:3302-3303`).
- Le split force déjà le ciel software : `sat_vdp2_sky` sauvé/forcé 0/restauré autour de la boucle
  (`d_main.c:345/352/372`) → chaque vue dessine son ciel software (`core/r_plane.c:1090-1102`).

### Mesurer « qui profite le plus du ciel »

Compteurs existants mais **globaux** : `sat_sky_px` (`r_plane.c:889`), `sat_frame_has_sky`
(`r_plane.c:883`), reset en tête de `R_DrawPlanes` (`r_plane.c:978`). Comme `R_DrawPlanes` est appelé
une fois par vue, `sat_sky_px` à la fin de frame = ciel de **la dernière vue**.

**Greffe minimale (DoomJo-safe)** : dans la boucle split, capturer `sat_sky_px` dans
`sat_sky_px_view[i]` après chaque `R_RenderPlayerView` (exactement comme les timings `tv[i+1]=d_ms()`
déjà présents à `d_main.c:364`). Le reset étant en tête de `R_DrawPlanes`, la valeur lue est bien le
ciel de CETTE vue.

**Métrique recommandée** : préférer un critère **stable** (le joueur dont le secteur courant a un
plafond `F_SKY1`) plutôt que `sat_sky_px` brut, pour éviter le flicker ; sinon normaliser par la
surface du viewport (3/4p plus petits). **Hystérésis obligatoire** (tenir K frames / seuil de marge)
car l'élection se fait sur N-1 et s'applique en N → saut de scroll visible au changement de leader.

### Fenêtrage NBG0 + rendu mixte

1. **Scroll** : `slScrPosNbg0` avec le `viewangle` du **joueur élu** au lieu du global.
2. **Fenêtre** : confiner NBG0 à la bande de la vue élue via une window VDP2 (WPSx 0xC0–0xCE +
   **WCTLA** pour NBG0, 0xD0 — **dans le block-flush**).
3. **Désglobaliser `sat_vdp2_sky`** : le poser par-vue dans la boucle (`sat_vdp2_sky = (i==elected)`)
   au lieu du `=0` global (`d_main.c:352`). DoomJo n'élit jamais → reste 0.

### Interaction sol RBG0 ↔ ciel NBG0 fenêtrés (contrainte 2 windows)

- **Une SEULE window (W0) suffit** pour confiner sol RBG0 (WCTLC) **et** ciel NBG0 (WCTLA) à la même
  bande du joueur élu → **W1 reste libre**.
- Les 2 windows ne lèvent PAS la limite « 1 transform / 1 scroll » : elles **confinent** la vue HW
  unique et **clippent** pour ne pas polluer les vues software (notamment les trous index-0 des murs
  VDP1 où un ciel au mauvais angle pourrait apparaître).

### Politique d'attribution recommandée

1. Mesurer `sat_sky_px_view[i]` (ou plafond F_SKY1) par vue, frame N.
2. `elected = argmax`, **avec hystérésis** (ne basculer que si le challenger dépasse le leader de
   ≥ X % pendant ≥ K frames).
3. Frame N+1 : `slScrPosNbg0(viewangle de elected)`, window W0 sur sa bande, `sat_vdp2_sky=(i==elected)`.
4. **Couplage sol/ciel** : attribuer sol HW *et* ciel HW au **même** joueur élu (W0 partagée) — c'est
   le modèle 1p généralisé à « 1 élu parmi N ».

---

## 6. Risques & contraintes

| Risque | Détail | Mitigation |
|---|---|---|
| **no-slSynch / registres** | Window rect (WPSx, WCTLx) DANS le block-flush → committable statique. RPMD (0x110) / CCRR (0x10C) HORS → poke direct. | Fenêtre statique = OK. RPA/RPB → poke RPMD one-shot init (recette CCRR). |
| **Budget cycles / neige** | Window gratuite (masque post-fetch, 0 CYC). RPA/RPB n'ajoute pas de read/dot → pas de retour neige. | Ne JAMAIS hand-pin CYCB1 (affame le 2e char read 8bpp). Laisser `slScrAutoDisp` allouer. |
| **Bleed P1→P2** | Géométrie du punch déjà confinée par `ylookup`/`columnofs`. | Window W0 sur la bande de l'élu ; les autres en software complet. |
| **Quad (3-4p)** | RPA/RPB = 2 transforms max ; 2 windows seulement ; RPMD = K_CHANGE XOR W_CHANGE. | 3-4p restent 100 % software. Au mieux 1 quadrant HW + window. |
| **Ne pas casser le 1p pot0** | Le chemin `rbg0_active = pot0 && ≤1p` doit rester byte-identique en 1p. | Nouveau code gaté par `sat_split_active` (=0 en 1p) / `sat_local_players>1`. |
| **Gating DoomJo** | Flags runtime def 0 + court-circuit `&&` + C pur. | Pas de `#ifdef`. `sat_vdp2_floor &&` en tête. Aucun C++isme dans `r_plane.c`/`r_segs.c`. |
| **Divergence Ymir↔HW** | Le no-slSynch est la zone de divergence émulateur↔Saturn. | **Chaque palier validé Ymir PUIS HW réel.** |
| **Latence 1 frame (ciel)** | Élection sur stats N-1 → saut au changement de leader. | Hystérésis / seuil / critère stable (plafond F_SKY1). |

---

## 7. Roadmap ordonnée (du plus sûr au plus ambitieux)

0. **Socle core inerte** : `int sat_rbg0_view = 0` + helper `sat_floor_punch_here()` branché à
   `r_plane.c:1111` et `r_segs.c:429/461/479`. Inerte tant que `sat_vdp2_floor==0`. Test : 1p
   inchangé (Ymir+HW), DoomJo compile (GCC 9.3).
1. **Fenêtre RBG0 statique en 1p** (plein écran) : prouver que la window commit dans le block-flush
   et n'ajoute pas de neige **sur HW**.
2. **Mode « P1 sol HW / P2 SW » en 2p** (le livrable principal §3) : ne plus forcer
   `sat_vdp2_floor=0` en 2p ; `sat_rbg0_view=0` ; re-setup P1 avant transform ;
   `slScrWindow0(0,0,159,223)` + `win0_IN`. **Ymir puis HW.**
3. *(option, parité 2p)* RPA/RPB variante B (W_CHANGE + poke RPMD + RB rempli).
4. **Ciel HW pour le joueur élu** (§5) : window NBG0 partagée avec RBG0 (W0), `sat_vdp2_sky` par-vue,
   scroll sur le `viewangle` de l'élu. **Plus bloqué** — pure mécanique par-vue.
5. **Ciel HW attribué dynamiquement** (§5) : `sat_sky_px_view[]` + élection + hystérésis ; sol+ciel
   routés vers l'élu, fenêtrés sur sa bande, les autres 100 % software.

---

## 8. Commentaires / état obsolètes à nettoyer

Confirmé sur HW (2026-06-29) : **RBG0 et le ciel NBG0 cohabitent proprement**. Les éléments suivants
disent l'inverse et sont **obsolètes** (à corriger lors d'une passe dédiée — pas pendant une session
concurrente sur `dg_saturn.cxx`) :

- `src/dg_saturn.cxx:280-281` — « a VDP2 window ... would not commit without slSynch » → **FAUX** :
  les registres de fenêtre rectangulaire sont dans le block-flush (§3.a). La justification du
  `VDP2_SKY_OCCL_DIAG` (mettre le ciel au-dessus faute de pouvoir clipper le sol) est donc caduque :
  on PEUT clipper via window.
- `src/dg_saturn.cxx:275-277` — `VDP2_SKY_FORCE_CYC` / `sky_cell_force_cyc()` : expérience morte
  liée au bug « sky shows floor » désormais résolu. À retirer.
- `src/dg_saturn.cxx:3299-3300` — « drop [the HW sky] in 2-player : NBG0 ... cannot serve two split
  views » : c'est une **limitation actuelle**, pas une loi HW. Le ciel HW multi est faisable par
  élection + fenêtrage par-vue (§5).
- Mémoire `rbg0-hw-sky-feasible` : la longue section « UNDER INVESTIGATION / THE ACTUAL BUG (NBG0
  reads A1) » a été **corrigée** (statut : RÉSOLU / SHIPPING CLEAN).

---

## 9. Questions ouvertes / à vérifier sur HW

1. **L'ISR SGL `_BlankIn` re-pousse-t-elle WCTL/WPS chaque vblank ?** Non vérifiable (LIBSGL.A
   binaire) → détermine si une window *mobile* exige un poke/frame. Sans réponse : fenêtres
   **statiques** (suffit pour §3).
2. **La window RBG0 introduit-elle de la neige sur Saturn réelle ?** Théoriquement non (masque
   post-fetch), mais à confirmer à l'étape 1 (zone no-slSynch).
3. **W_CHANGE/RPMD poke one-shot survit-il comme CCRR sur HW ?** Probable (K_CHANGE persiste déjà),
   non testé.
4. **Surcoût CPU du 2ᵉ `R_SetupFrame`/transform par frame** : mesurable via `rbg_xfm_sum`/`rbg_rpt_sum`
   (`dg_saturn.cxx:3372-3373` env.), à quantifier avant l'étape 3.
5. **Granularité K-table RA/RB** : confirmée par-ligne (horizontale) → K_CHANGE = split horizontal
   seulement, incompatible 2p vertical sans refonte (d'où variante B/W_CHANGE).
6. **Métrique de dominance ciel + valeurs d'hystérésis** : à régler empiriquement (E1M1 peu de ciel ;
   E1M2/extérieurs beaucoup).

---

### Références code (vérifiées 2026-06-29)

| Sujet | Fichier:ligne |
|---|---|
| Boucle split / géométrie viewports | `core/d_main.c:316-393`, tables `:341-342` |
| `R_SetViewWindow` (ylookup/columnofs) | `core/r_main.c:856-858` |
| `rbg0_set_transform` | `src/dg_saturn.cxx:1361` |
| Gate `rbg0_active` / `sat_vdp2_floor` | `src/dg_saturn.cxx:3312-3315` |
| `slScrAutoDisp` (sky_bit/rbg0_bit) | `src/dg_saturn.cxx:3314-3317` |
| block-flush `rbg0_commit_cyc` (0x0E..0xFE) | `src/dg_saturn.cxx:1718` |
| poke CCRR (recette poke hors-flush) | `src/dg_saturn.cxx:1487` |
| memcpy RPT RA+RB | `src/dg_saturn.cxx:3367-3368` |
| Hole-punch sol (index 0) | `core/r_plane.c:1106-1138` (match `:1111`) |
| Sélection sol = secteur sous l'œil | `core/r_plane.c:991` |
| Cohérence murs↔sol | `core/r_segs.c:429/461/479` |
| Compteurs ciel/sol (globaux) | `core/r_plane.c:883/889/978` |
| Ciel software | `core/r_plane.c:1090-1102` |
