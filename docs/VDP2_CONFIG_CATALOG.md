> ✅ STATUT 2026-06-29 — **catalogue de recherche** (énumération de configs candidates). Réalité shippée : le sol RBG0 vit en **BITMAP 512×256 8bpp PROPRE** (`RBG0_BITMAP=1`, commits 19768ca/41dd895), gaté potato-0 + 1 joueur. 2 banques rotation : bitmap=A1, coeff/K=A0, fb=B0, RPT=B1+0x1ff00 → **B1 LIBRE (pas de map)**. Le commit CYCxx est **RÉSOLU** (`rbg0_commit_cyc` + `rbg0_commit_ramctl` déjà dans l'arbre, block-flush direct sans slSynch, RDBS=0x0D) — la « neige » était de la **starvation de cycle-pattern** du sol cellulaire, pas un commit-gap. La loi « overlay/ciel **XOR** sol » est **LEVÉE** : B1 libre → overlay NBG3 **et** un ciel B1 cohabitent avec le sol. Fog distance = écran line-color + color-calc RBG0 (PAS K_LINECOL), prototype gaté OFF. slSynch ABANDONNÉ (pas réfuté). **Référence sol faisant autorité : [`docs/VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md). Présent VDP1↔NBG1 : [`docs/VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md).** Ci-dessous : énumération historique conservée pour les paris non-shippés ; les fiches sol-cellulaire « snow/bloqué CYCxx » sont obsolètes (cf. la réf sol).

# Catalogue maître des configurations VDP2/VDP1 — Mimas (Doom Saturn)

> Document de décision et de référence. Auteur : architecte graphique Saturn (Mimas).
> Date : 2026-06-26. Public : Romain.
> Objet : enumerer, dedupliquer et classer **toutes** les configs VDP2/VDP1 interessantes,
> pour batir un **toggle ecran-titre** qui les teste sur hardware. Pour chaque config :
> ce qui **PART**, ce qui est **INUTILISE** (= l'opportunite), le **gain perf** (mesure vs
> estime), la **qualite**, la **faisabilite honnete**, et **le levier disruptif** a tenter.
>
> Fusionne 5 enumerations paralleles (ciel-centric, sol-centric, baseline-minimal,
> disruptif-exotique, perf-grounding). Croise avec [`docs/VDP2_LAYER_BUDGET.md`](VDP2_LAYER_BUDGET.md)
> et [`docs/VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md) (réalité sol shippée).

---

## 0. Les verites qui structurent tout le catalogue

Avant la matrice, les faits durs qui dictent le classement (ils reviennent dans **chaque**
config) :

1. **LE GAIN, C'EST LE *SKIP*, PAS LA TECHNO DE SOL.** Le `+20-33 % fps` (`-17 a -26 ms`) vient
   du fait de **ne pas dessiner** le span du flat dominant (ecrire l'index 0, `core/r_plane.c:1078-1081`).
   RBG0, VDP1-strips, gradient back-screen, fake-mode7 captent **tous le meme** gain : ils ne
   different que par ce qui **transparait** dans le trou, et par le cout en banques/CRAM/VDP1.
   *C'est le fait le plus contre-intuitif du catalogue* — il ecrase la fausse opposition « ciel XOR sol ».
   (cf. `VDP2_LAYER_BUDGET.md §6`.)

2. **B1 N'EST PLUS LE FORK** (résolu, cf. réf sol). Le sol shippé est un **bitmap RBG0** : pas de
   map pattern-name → la RPT vit en `B1+0x1ff00` et **B1 reste LIBRE**. Conséquence : overlay NBG3,
   back-screen color word **et** un ciel B1 cohabitent avec le sol. La « neige » du sol cellulaire
   était de la **starvation de cycle-pattern**, réglée par `rbg0_commit_ramctl`/`rbg0_commit_cyc`
   (déjà dans l'arbre, block-flush direct **sans** slSynch) + RDBS=0x0D + cycles A0/A1 parqués —
   **pas** un commit-gap. Les seules variantes qui évincent B1 sont les anciennes configs sol
   **cellulaires** (9/10/11), conservées ci-dessous comme paris non-shippés.

3. **LE PRIX RETRECIT AU POTATO SHIPPE.** Le `+20-33 %` est **pot0, pre-potato** (A/B Romain
   2026-06-19). Au potato reel (pot1/pot2) le terme `P` est deja effondre (62 → 17 → 11 ms) et le
   slave est deja ~80 % idle → l'offload de sol devient surtout une affaire de **QUALITE** (sol
   texture/perspective), pas un gros gain fps. **Les configs valent le plus au pot0.**
   (cf. `REC_BENCHMARKS.md §C.2`, perf-grounding.)

> **Ancrages mesures (HW, haute confiance sauf indication) :** floor-skip `+50-65 %` pot0→pot1,
> `+76 %` (x1.8) pot0→pot2-fl ; `P` = 46-48 % d'une frame ~130 ms pot0, s'effondre 62→17→11 ms ;
> `dom%` E1M1 nukage `d48 % n20`, E1M1 ext `d95 % n21` ; slave idle 29-63 % (master = long pole
> partout) ; VDP1 `Dr` 25-42 % pot0 (TENDU, deborde 60-75 % des frames), 82-91 % pot2-fl ; blit
> ~5.5 ms ; framebuffer 512×256×8bpp = 128 Ko = **toute** la banque B0. `Vp` ~150-170 (lecture
> photo, a confirmer). Le `+20-33 %/-17-26 ms` du « prix RBG0 » est **estime** (A/B pot0).

---

## 1. Vue d'ensemble — la matrice maitresse

**Tri retenu : risque/effort croissant** (de la baseline prouvee-HW vers le fantasme infaisable).
C'est l'axe utile pour un toggle : on cable d'abord ce qui est sur, on garde le reste en recherche.
Le **gain** n'est *pas* l'axe de tri car le skip donne ~le meme gain a presque tout le monde — le
discriminant reel est **risque × qualite**.

Legende banques : `A0 / A1 / B0 / B1 / CRAM`. **fb** = framebuffer (NBG1, B0, obligatoire).
Gain : ✓ = mesure HW, ~ = estime/extrapole, `0` = nul par design, `?` = a estimer.

| # | Config | A0 / A1 / B0 / B1 / CRAM | Ce qui PART | Ce qui est INUTILISE (= l'opportunite) | Gain perf | Qualite | Faisabilite |
|---|--------|--------------------------|-------------|----------------------------------------|-----------|---------|-------------|
| **1** | **SHIP / Classic** (A/D) | ciel HW / **LIBRE** / fb / overlay NBG3 / 8 pal | Rien (build connu-bon) | A1 (0/8, 128 Ko) ; ~6/8 slots partout ; slave 29-62 % idle | `0` (baseline) ✓ | Reference : sol software texture full, ciel HW, overlay | **PROUVE HW** (seule config validee bout-en-bout) |
| **2** | **Pure-qualite** (= SHIP pot0, knobs max) | ciel HW / LIBRE / fb / overlay-ou-off / 8 pal | Rien degrade | A1 libre ; slave le + charge (29 % idle pot0) | NEGATIF (le + bas, ~7.5-7.9 fps) ✓ | **MEILLEURE** sur chemin prouve : sol texture per-pixel + murs textures + ciel parallaxe | **PROUVE HW** (identique a SHIP, pot0 sans degradation) |
| **3** | **Fast-floor / gradient** (H / F4) | ciel HW / LIBRE / fb / overlay NBG3 **+ table gradient back-screen** / 8 pal | **Texture du sol** + flats/hauteurs per-secteur → degrade ombre par distance (d32xr) | A1 libre ; slave libere ~62 ms (reabsorbe ciel SW) ; VDP1 intouche | **~+20-33 %** (le skip) ~ | PIRE sol (gradient non texture) MAIS garde ciel **ET** overlay, 0 tearing | **HAUTE** : skip deja shippe ; gradient = primitives back-screen prouvees ; **0 banque, 0 commit CYC**. *Reste : le commit `slBackColTable` sans slSynch — a verifier HW* |
| **4** | **Eye-candy / nuages** (S3) | ciel HW / **nuages NBG2 (cells)** / fb / overlay NBG3 / 8 pal (tendu) | Rien struct. ; si nuages veulent leur palette → 1 light-bank CRAM sacrifiee | A1 consomme ; VDP1 intouche ; pas de pression cycle (NBG2 ~3 slots) | `0` (pur visuel) ~ | MEILLEUR ciel (parallaxe 2 plans Hexen/Doom-64) ; sol inchange si pas de skip | **LIKELY-OK** : NBG2 dans A1 libre, meme classe que ciel HW prouve, pas de poke. Inconnu = NBG2 jamais allume + pression CRAM |
| **5** | **Rich-sky** (S6) | bg ciel NBG0 / **fg ciel NBG2 (cells)** / fb / overlay NBG3 / 8 pal | Si fg veut sa palette → 1 light-bank CRAM | Plus de banque spare (A1 depense) ; VDP1 intouche ; slave inchange | `0` (pur visuel ; +20-33 % si on ajoute le skip) ~ | **MEILLEUR ciel du catalogue** : vraie parallaxe 2 plans + translucidite color-calc au raccord | **LIKELY-OK** (analogie ciel HW prouve, pas de poke). Inconnu = NBG2 jamais allume + CRAM |
| **6** | **VDP1-floor / multi-surface** (S5 / F3) | ciel HW / LIBRE / fb / overlay NBG3 **(garde !)** / 8 pal | Rien cote VDP2 ; cote VDP1 : cull du HUD/arme mort pour banque cmd F + cache flat 32 Ko | A1 libre ; slave libere du sol ; **VDP1 = ressource consommee, TENDUE** (Dr 25-42 %) | **~+15-24 %** (`-4 a -9 ms` ; multi-surface) ~ | MEILLEURE couverture (sols+plafonds+marches), garde ciel+overlay. PIRE : swim horizon + **plus de tearing** (Dr chute) | **FONDATION PROUVEE-HW** (driver async no-slSynch valide ; murs+arme shippes) mais **emitter = STUB** (`sat_floor_vdp1_stub` renvoie 1, 0 strip). Gate reel = `Dr%` post-floor |
| **7** | **Fake-mode7 line-scroll** (exotique) | ciel HW / **NBG2 cells + table line-scroll** / fb / overlay NBG3 / 8 pal | **Reponse au yaw** (la texture ne tourne pas quand on pivote) → faux pour free-look Doom | Frees toute la pile RBG0 (pas de K, pas de CYC) ; overlay survit ; A1 = 1 banque cell | **~+20-33 %** (le skip, sous la couche) ~ | Pire que RBG0 (pas de yaw → shimmer/shear sur flats gridded) mais mieux que gradient (scaling profondeur) | **COMPILE / dodge le minefield CYCxx** (line-scroll = NBG normal ~1-2 slots, pas banque rotation). Inconnu = commit line-scroll sans slSynch + look sous yaw |
| **8** | **Sky+textured-floor RBG0 bitmap** (C / F2) — **SHIPPÉ** | ciel HW / **bitmap RBG0** / fb / overlay NBG3 **(revient !)** / 8 pal | **Flats dynamiques** : 1 bitmap statique 512×256 ; changer flat/lumiere = re-upload 128 Ko | B1 libere (bitmap = pas de PN read) → overlay + back-screen reviennent | **~+20-33 %** (le skip) ~ | PIRE que F1 (mono-texture, pas de flats/secteur) mais garde ciel HW + overlay | **SHIPPÉ HW PROPRE** (commits 19768ca/41dd895, gaté potato-0 + 1 joueur) : bitmap A1, coeff/K A0, RPT B1+0x1ff00 → B1 libre. Commit RÉSOLU (`rbg0_commit_cyc`/`ramctl`, RDBS=0x0D). Voir VDP2_RBG0_CURRENT_STATE.md |
| **9** | **Sky+textured-floor RBG0 cell, K en CRAM** (G / S4) | ciel HW **(garde !)** / cells RBG0 / fb / map RBG0 **(EVINCE overlay)** / **K-table (CRKTE)** + pal | **OVERLAY NBG3** (map force B1) ; risque 1+ light-banks CRAM | Slave libere du sol ; VDP1 intouche ; **0 banque spare** ; CRAM = ressource contestee | **~+20-33 %** + sol texture perspective-exact ~ | **MEILLEUR sol des configs ciel-keeping** : texture perspective-exact + K_LINECOL possible. MAIS overlay parti | **NON PROUVE** : seule voie « ciel+sol **cell** texture » sur 4 banques (le bitmap config 8 garde déjà ciel+overlay sans ça), mais (1) CRKTE collisionne CRAM mode 1 vs color-calc ; (2) SGL n'expose **aucun** K-en-CRAM → poke brut. Recherche |
| **10** | **Textured-floor RBG0 cell** (B/E / F1) — **superseded** | **K-table** / cells RBG0 / fb / map RBG0 **(EVINCE overlay)** / 8 pal | **Ciel HW** (→ ciel SW sur slave) **ET overlay** | A0/A1 ~126 Ko storage tail ; slave ~80 % idle pot1/2 reabsorbe ciel SW ; VDP1 intouche | **~+20-33 %** (le skip) ~ | **MEILLEUR sol de la famille** : perspective-exact texture, distance-dimme. Perd ciel HW + overlay | **SUPERSEDED par config 8 (bitmap).** Le cell-floor **snowait** (starvation cycle-pattern) ; le bitmap shippe propre **et** garde l'overlay → préférer config 8. Variante cell parquée |
| **11** | **K-table LIT (per-line K_LINECOL)** (F6) — **piste qualité** | = F1 mais K en **K_LINECOL** / cells / fb / map / 8 pal | = F1 (ciel HW → SW, overlay off). Rien de plus | = F1 ; les 8 slots de la banque K deja owned → K_LINECOL coute **0 cycle de plus** | `0` de plus vs F1 (meme skip) ; **QUALITE** ~ | **MEILLEUR variant RBG0** : perspective-exact **ET** distance-shade (fog Doom). Jo ship exactement ca | Fog actuel sur le bitmap shippé = écran line-color + color-calc RBG0 (PAS K_LINECOL). K_LINECOL reste une piste qualité non-shippée (`slKtableRA` déjà dans l'arbre `dg_saturn.cxx:1250` en K_LINE → un flag) |
| **12** | **WORLD-on-VDP1** (tout le monde → VDP1) | ciel HW / LIBRE / fb / overlay NBG3 / 8 pal | Base : rien cote VDP2 (deporte le monde du CPU vers VDP1). Variante RBG0 : ciel+overlay | A1 100 % idle ; VDP2 = pur compositeur ; **VDP1 NON idle (Dr 25-42 %)** | **~+15-24 %** (deux SH-2 liberes ; master n'attend jamais VDP1) ~ | Walls inchanges ; sol affine swim + **plus de tearing**. Permet « ciel+sol+overlay » SI sol = gradient 0-banque | **FONDATION PROUVEE** mais **cull VRAM requis** (~52 Ko honnetes : 20 Ko HUD mort + 32 Ko wtex shrink ; les 176 Ko WPN **aliasent** wtex = 0 Ko). Swim/Dr = verdict HW only |
| **13** | **4bpp framebuffer** (libere une demi-banque B0) | ciel HW / LIBRE / **fb 4bpp 64 Ko** / overlay / sous-palettes 16-col | **Profondeur 8bpp de Doom** (256 PLAYPAL + 34 light) → 1 palette 16-col pour tout l'ecran en bitmap | 64 Ko de B0 mais **echoue** (ne peut heberger la map RBG0 : double whole-bank interdit) | **blit ~5.5→2.75 ms** (-2.75 ms, bus-bound) ~ MAIS qualite catastrophique | Catastrophique pour la vue 3D (16 couleurs). Viable **seulement** en strip HUD 4bpp | **FANTASME pour la vue 3D** (8bpp non negociable). La meme economie blit = **W5** (blit que les rangees 3D) sans perte → preferer W5 |
| **14** | **Reduction-FB (ZMCTL 1/2 ou 1/4)** | ciel HW (reduit) / capacite cycle / fb / overlay / pal | A 1/4 : **NBG2 ET NBG3 (overlay)** desactives ; reduction **coute** des reads (×2/×4) | Minimal — la reduction adresse les **slots** (que Mimas n'a pas en pression), pas le stockage | **~0** pour Mimas ~ | PIRE (cap couleur + flou horizontal) pour 0 gain | **COMPILE mais POINTLESS** : resout une contrainte (slots) que Mimas n'a pas, casse l'overlay. Non-starter |
| **15** | **DUAL-ROTATION RBG0+RBG1** (F5) | A0=K / A1=cells / fb / B1=map **+ RBG1 = 3 banques de +** | **Physiquement impossible** : fb(1)+RBG0(3)+RBG1(3) = 7 reclamations / 4 banques (5 meme avec 2 K en CRAM) | N/A — sur-souscrit, -1 banque | Serait > F1 (skip sol **ET** plafond) mais UNREACHABLE | Serait le meilleur (sol+plafond perspective-exact) mais infaisable | **INFAISABLE** (arithmetique banque). SGL expose les entrees RB mais la VRAM l'interdit. NO-GO |
| **MP** | **2P split** (software force) | ciel HW alloue mais **NBG0 eteint** / LIBRE / fb (2 viewports) / overlay dispo / 8 pal | **Ciel HW force OFF** (1 registre scroll ≠ 2 yaw) ; **RBG0 impossible** (1 matrice ≠ 2 cameras) | A0 (banque ciel) idle/dark ; A1 libre ; B1 dispo (pas de RBG0 → overlay + dispo) | Aucun offload VDP2 — le levier MP = **potato (pad-Z)** ~ | Ciel SW + sols SW, per-view-correct ; potato applique aux 2 vues | **PROUVE HW shippe** (2026-06-22). MP force software ciel+sol. *« le mode vdp2 marche pas en multi »* |

> **Potato ladder** (variations software de la config #1, toutes **prouvees HW**, memes banques) :
> pot0 ~7.5-7.9 fps (P~62 ms) → pot0.5 ~8.1 (P~50) → pot1 ~11.6-11.9 (P~17, sols flat) →
> pot2-fl ~13.4 (P~11, murs+sols flat). Range complet `+76 %` (x1.8). Ce sont des **modes**, pas
> des configs de banque distinctes — le toggle potato (pad-Z) est orthogonal au toggle layout.

---

## 2. Fiche par config

### 2.A — Famille BASELINE / SUR (prouve-HW, garde l'overlay, zero commit risque)

#### Config 1 — SHIP / Classic (docs A/D)
- **Bank map :** A0 = ciel HW (NBG0 bitmap 8bpp 512×256, pal1, prio4) · A1 = **LIBRE (0/8)** · B0 = fb (NBG1 8bpp 320×200, prio6) · B1 = overlay NBG3 (prio7) · CRAM = 8 palettes (0 = font, 1 = PLAYPAL live, 2-7 = 6 light-banks).
- **Ce qui part :** rien. C'est le build connu-bon (`VDP2_HW_SKY=1`, `VDP2_RBG0_TEST=0`, `dg_saturn.cxx:249/236`). Sol 100 % software (chemin P, master+slave 50/50).
- **Inutilise (= l'opportunite) :** A1 entierement libre (banque + 8 slots) ; ~6/8 slots libres partout ; slave 29-62 % idle ; VDP1 TENDU (pas idle) au pot0.
- **Gain perf :** `0` — c'est la baseline. **Mesure HW** : pot0 nukage ~130 ms/7.6 fps ; P~62 ms (~48 % du render) ; blit ~5.5 ms ; `dom%` d48 % (nukage) / d95 % (ext).
- **Qualite :** reference. Sol software per-distance (zlight), flats/hauteurs corrects partout, flats dynamiques, overlay dispo.
- **Faisabilite :** **PROUVE HW** — la seule config validee bout-en-bout.
- **A essayer de maniere disruptive :** remplir A1 (la ressource la plus sous-exploitee de **toutes** les configs ciel-keeping) avec une NBG2 nuages OU une NBG HUD/status-bar dediee (sort le composite+blit des 32 rangees du bas du chemin framebuffer). Plus gros : appliquer le **skip du flat dominant** (deja code `r_plane.c:1078`) → devient la config 3, +20-33 % sans toucher le bank map.

#### Config 2 — Pure-qualite (= SHIP a pot0, knobs max)
- **Bank map :** identique a SHIP. La difference est purement les knobs software au max (pot0, pas de low-detail).
- **Ce qui part :** rien de degrade. Le rendu software le plus riche.
- **Inutilise :** A1 libre (nuages possibles, +0 REC) ; slave le plus charge ici (29 % idle pot0 — le sol le tient). Le **moins** de spare de la famille, par design.
- **Gain perf :** **NEGATIF** vs tout le reste — le plus bas (~7.5-7.9 fps pot0). Pas de skip, pas d'offload. **Mesure HW**.
- **Qualite :** **MEILLEURE** sur le chemin prouve : sol texture perspective per-pixel + murs textures VDP1 + ciel parallaxe HW + light diminishing full. ≥ tout sol RBG0 texture pour le flat dominant, et strictement mieux ailleurs (multi-surface, pas de swim).
- **Faisabilite :** **PROUVE HW**.
- **A essayer de maniere disruptive :** ajouter la NBG2 nuages (A1 libre, +0 REC) + le color-offset HW (damage-flash/fondus, 0 banque, registre seul) → pousser la qualite **au-dessus** des configs RBG0 tout en restant 100 % prouve, en echange des fps deja-bas.

> **Potato ladder** (memes banques, software) : pot0 / pot0.5 / pot1 (sols flat) / pot2-fl (murs+sols flat) = ~7.6 / 8.1 / 11.6 / 13.4 fps. **Tous prouves HW.** Au pot2-fl, VDP1 finit large (Dr 82-91 %) → disruptif : remettre des murs textures « gratuits » sur le budget VDP1 libere.

### 2.B — Famille CIEL-CENTRIC (garde le ciel HW sur A0 ; varie A1 + B1)

#### Config 3 — Fast-floor / gradient back-screen (docs H / F4) — **le sleeper**
- **Bank map :** A0 = ciel HW · A1 = LIBRE · B0 = fb · B1 = overlay NBG3 **+ table gradient back-screen** (`B1+0x1fee0`, region back-screen deja vivante) · CRAM = 8 pal. **0 nouvelle banque, 0 poke RAMCTL/CYC.**
- **Ce qui part :** la **texture** du sol + flats/hauteurs per-secteur. Le trou index-0 montre un **degrade vertical ombre par distance** (~1 scanline = 1 bande de profondeur, style d32xr/Doom-32X).
- **Inutilise :** A1 libre (nuages NBG2 possibles par-dessus) ; slave libere ~62 ms (reabsorbe le ciel SW si besoin) ; VDP1 intouche (0 cout tearing).
- **Gain perf :** **~+20-33 %** (`-17 a -26 ms`, master **ET** slave) — **le meme prix** que RBG0 car le gain EST le skip. **Estime** (A/B pot0). Dimensionne par `dom%` : ~+15 ms a d48 %, ~+28 ms a d95 %. Plus faible au pot1/2 (P deja effondre).
- **Qualite :** PIRE sol (degrade non texture, pas de flats/secteur) MAIS garde ciel HW **ET** overlay (B1 intouche), 0 nouveau tearing, 0 minefield. Lit comme un sol « fog/profondeur » — jouable (d32xr ship exactement ca).
- **Faisabilite :** **HAUTE** — le skip est shippe (`r_plane.c:1078`), le gradient repose sur des primitives back-screen deja vivantes. **PAS de commit RAMCTL/CYC, PAS de RBG0.** Seul travail neuf : relacher le band-match du skip + ecrire la table gradient (« meilleure piste tout-en-un »). **A verifier HW :** `slBackColTable`/`slLineColTable` sont des shadow-writes (famille flush-in-slSynch) → tester si un poke direct colle comme FBCR/PTMR (§3 idee transversale).
- **A essayer de maniere disruptive :** animer la table gradient per-frame (trick reflets d'eau de Sonic R, tcrf-documente) → sol nukage/lave **shimmering** a ~0 cout, en detectant le flatnum liquide. Puis poser une NBG2 nuages dans A1 → ciel + sol-gradient-anime + nuages + overlay + le gain, **0 banque jeu**.

#### Config 4 — Eye-candy / nuages NBG2 (docs S3)
- **Bank map :** A0 = ciel HW (backdrop, prio3) · A1 = **nuages NBG2 (cells, prio entre ciel et jeu)** · B0 = fb · B1 = overlay NBG3 · CRAM = 8 pal (tendu — nuages partagent/volent une banque). Sol reste software (ou + skip de la config 3).
- **Ce qui part :** rien structurellement (A1 spare). Si nuages veulent leur palette → 1 light-bank CRAM sacrifiee (eclairage software plus grossier).
- **Inutilise :** VDP1 intouche ; slave inchange ; pas de pression cycle (NBG2 cell ~3 slots, A1 etait 0/8). Si sol reste software → **0 gain** (branche pur-visuel).
- **Gain perf :** **`0` REC** (pur eye-candy — le ciel est deja skippe). Pour du fps, ajouter le skip de la config 3 (= config 3 + nuages). **Base : `VDP2_ARCHITECTURE §6.5` « NBG2 nuages : +0 REC, visuel seul ».**
- **Qualite :** MEILLEUR ciel (parallaxe 2 plans Hexen/Doom-64 : NBG0 lent + NBG2 rapide vs viewangle). Sol inchange (texture software full) si pas de skip. Overlay garde.
- **Faisabilite :** **LIKELY-OK** : NBG2 dans A1 libre, pas de RBG0, pas de poke, pas de slSynch — meme classe que le ciel HW prouve. Inconnu = NBG2 jamais allume dans l'arbre + pression CRAM (8 deja utilisees).
- **A essayer de maniere disruptive :** scroller les nuages a un 2e taux yaw-couple independant + color-calc (`slColorCalc`) pour alpha-blend le raccord → nuages translucides derivants ~gratuits. Ou repurposer NBG2 en bande montagne-horizon (line-scroll table → parallaxe-profondeur fake).

#### Config 5 — Rich-sky 2 plans (docs S6)
- **Bank map :** A0 = bg ciel NBG0 (loin, scroll yaw lent) · A1 = **fg ciel NBG2 (cells, proche, scroll rapide)** · B0 = fb · B1 = overlay NBG3 · CRAM = 8 pal (2e plan partage/vole une banque). Sol software (ou + skip).
- **Ce qui part :** si fg veut sa palette → 1 light-bank CRAM. Overlay garde.
- **Inutilise :** **plus de banque spare** (A1 depense) ; VDP1 intouche ; slave inchange (software floor) sauf si skip applique.
- **Gain perf :** **`0` REC** seul (pur visuel, comme config 4). Fps seulement si combine au skip (alors +20-33 %).
- **Qualite :** **MEILLEUR ciel du catalogue** : vraie parallaxe 2 plans (ciel lointain + foreground derivant a un taux angulaire different) + translucidite color-calc au raccord. Le « showpiece exterieur ».
- **Faisabilite :** **LIKELY-OK** par analogie au ciel HW prouve (NBG0 marche ; NBG2 = meme classe dans A1 libre, pas de poke). Inconnu = NBG2 jamais allume + CRAM.
- **A essayer de maniere disruptive :** piloter le scroll du fg par une **line-scroll table** (X scroll per-raster) pour faker un horizon courbe / parallaxe-montagne dans un seul plan. Ou ne l'activer que sur les maps grand-exterieur (classifieur au load) → les maps fermees reclament A1 pour le gradient ou les nuages.

#### Config 6 — VDP1-floor / multi-surface (docs S5 / F3) — **le vrai cheval 1P**
- **Bank map :** A0 = ciel HW (garde) · A1 = LIBRE (VDP2-wise) · B0 = fb · B1 = overlay NBG3 **(garde — VDP1 n'a pas besoin de B1 !)** · CRAM = 8 pal. **Le sol vit dans la VRAM VDP1** (banque cmd F + cache flat 32 Ko @`0x25Cxxxxx`), **hors** des 4 banques VDP2.
- **Ce qui part :** rien cote VDP2 (ciel **ET** overlay gardes — propriete unique). Cote VDP1 : il faut **culler** la region arme/HUD morte pour carver banque cmd F + cache flat (`VDP1_WORLD_PLAN §7.2`).
- **Inutilise :** A1 libre (nuages possibles) ; slave libere du sol ; **VDP1 = ressource consommee, et TENDUE au pot0 (Dr 25-42 %)** → le sol mange du headroom VDP1 reel → plus de tearing.
- **Gain perf :** **~+15-24 %** (`-4 a -9 ms` cites pour les strips ; l'inverted-hybrid tue P + fill EX + blit de ces pixels). **Multi-surface** : autres hauteurs + plafonds non-sky. Master n'attend jamais VDP1 (`CRITICAL_PATH §0`) → le gain **banque** vraiment. **Estime**, inputs mesures : Qp~169 quads monde, Vp~158 quads sol, q4 80-93 % pure-quad.
- **Qualite :** MEILLEURE couverture (multi-surface : sols+plafonds+marches), garde ciel **ET** overlay. PIRE : swim affine pres de l'horizon (mitige par bandes fines + flat-clamp) + **plus de tearing** (Dr descend sous le confort).
- **Faisabilite :** **FONDATION PROUVEE-HW** (driver async VDP1 valide 2026-06-16, murs+arme shippes) mais l'**emitter est un STUB** aujourd'hui (`sat_floor_vdp1_stub` renvoie 1, emet 0 strip). Plumbing prouve, emitter non bati. Note : le sol RBG0 bitmap (config 8) a entre-temps SHIPPÉ pour le flat dominant ; VDP1-floor garderait sa valeur **multi-surface** (autres hauteurs + plafonds) en complément (cf. §3 « RBG0 + VDP1 = partenaires »). **Gate reel = `Dr%` post-floor**, pas Vp.
- **A essayer de maniere disruptive :** per-subsector inverted-hybrid (`VDP1_WORLD_PLAN`) — emettre sols/plafonds **pendant** la BSP walk sur le kick mur existant (pas de 2e kick) → nulle floorplane/ceilingplane a `R_Subsector` → la **generation P disparait** aussi. Puis **composer avec la config 9/10** : RBG0 prend le seul flat dominant (pire swim), VDP1 prend le residuel — **partenaires, pas rivaux** (`CRITICAL_PATH §5`). Batir le toggle A/B (`VDP1_FLOOR_TEST_AB`) **d'abord** pour mesurer Dr + swim sur une scene fixe avant de committer.

#### Config 7 — Fake-mode7 line-scroll (exotique, ciel-keeping)
- **Bank map :** A0 = ciel HW · A1 = **NBG2 cells (1 banque flat) + table per-line H-scroll** · B0 = fb · B1 = overlay NBG3 · CRAM = 8 pal. **Pas de banque K, pas de map RBG0.**
- **Ce qui part :** la **rotation vraie** : le H-scroll per-line varie par Y-ecran (= profondeur) → scaling avant/arriere mais **pas de yaw angulaire**. Quand le joueur tourne, le sol scrolle/scale mais la texture ne tourne pas → faux pour le free-look Doom.
- **Inutilise :** **frees toute la pile RBG0** : pas de table K, pas de commit rotation (**pas de minefield CYCxx**), pas de RDBS. **Overlay (B1) survit** (pas de map rotation forcee en B1) — son vrai gain. A1 = 1 banque cell.
- **Gain perf :** **~+20-33 %** (le skip, sous la couche) si le span software est skippe en-dessous. **Moins cher a committer** que RBG0 (line-scroll = read NBG normal ~1-2 slots, pas banque rotation entiere). **Estime**.
- **Qualite :** PIRE que RBG0 (pas de yaw → texture ne suit pas le pivotement) mais MIEUX qu'un gradient flat (scaling/parallaxe profondeur). Acceptable pour flats quasi-isotropes ; les flats gridded de Doom lisent shimmer/shear en tournant.
- **Faisabilite :** **COMPILE** — line-scroll NBG est une vraie feature SGL qui **dodge le minefield CYCxx**, mais le commit line-scroll-enable sans slSynch est non teste ici (probablement un BGON+CYC poke normal). **Geometriquement NON PROUVE** que ca rende acceptable sous yaw. Moins cher a committer que RBG0, plafond qualite plus bas.
- **A essayer de maniere disruptive :** combiner line-scroll (scaling profondeur) + table per-line COLOR (fog/lighting distance) sur le **meme** NBG → sol fake-3D 1-banque avec diminishing-light integre, en dodgeant a la fois la K-table ET le conflit overlay. Si le yaw-shimmer est le seul defaut, le gater aux maps corridor (turn-rate bas) ; sol software ailleurs.

### 2.C — Famille SOL-CENTRIC (sol meilleur/moins cher ; ciel software accepte)

#### Config 8 — RBG0 BITMAP floor (docs C / F2) — **SHIPPÉ ✅**
- **Bank map :** A0 = ciel HW (garde) + coeff/K · A1 = **bitmap RBG0 (512×256 8bpp pre-tile, pas de PN read)** · B0 = fb · B1 = overlay NBG3 + back-screen + RPT `B1+0x1ff00` **(libre : bitmap droppe le PN read)** · CRAM = 8 pal.
- **Ce qui part :** **flats dynamiques** — 1 bitmap statique. Changer le flat dominant/lumiere = re-upload 128 Ko (vs 4 Ko cells) → rejete pour la bande passante per-frame. Variete per-secteur perdue (hauteurs encore via la matrice affine).
- **Inutilise :** B1 libere de la map → overlay revient + back-screen revit (debug-pendant-sol enfin possible) ; A0 ciel ~2/8 slots ; slave idle.
- **Gain perf :** **meme +20-33 %** (skip identique). **Avantage sur F1** : garde ciel HW + overlay → pas de cout ciel-SW re-ajoute au slave. Mais le re-upload (changement de flat) peut spiker une frame. **Estime**.
- **Qualite :** PIRE que F1 (mono-texture, pas de flats/secteur) mais garde ciel HW + overlay. Perspective-exact dans le flat unique. Bon pour cours exterieures (d95 % un flat), pauvre pour nukage mixte (d48 %).
- **Faisabilite :** **SHIPPÉ HW PROPRE** (commits 19768ca/41dd895, gaté potato-0 + 1 joueur). Le bitmap = 2 reads (bitmap A1 + coeff/K A0) → pas de banque map → **B1 libre**. Le commit est **RÉSOLU** : `rbg0_commit_ramctl`/`rbg0_commit_cyc` (block-flush direct **sans** slSynch) + RDBS=0x0D + cycles A0/A1 parqués. La « neige » du sol cellulaire était de la **starvation de cycle-pattern**, pas un commit-gap. SRL supporte 512×256/512×512 bitmap. **Détails faisant autorité : [`docs/VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md).**
- **A essayer de maniere disruptive :** baker le flat dominant dans le bitmap **une fois par map au load** (pas per-frame) et l'accepter comme texture sol unique du niveau (d32xr ship un look sol). B1 étant libre → poser une NBG2 nuages OU un ciel B1, OU garder l'overlay live pour le profiling. Fog distance = écran line-color + color-calc RBG0 (prototype gaté OFF).

#### Config 9 — RBG0 cell, K en CRAM (CRKTE) — ciel HW garde (docs G / S4)
- **Bank map :** A0 = ciel HW **(garde — la SEULE voie ciel+sol-texture)** · A1 = cells RBG0 (char) · B0 = fb · B1 = map RBG0 **(EVINCE overlay NBG3)** · CRAM = K-table (CRKTE, RAMCTL bit15) partageant 4 Ko avec les 8 palettes.
- **Ce qui part :** **OVERLAY NBG3** (map force B1 ; B1 cable a NBG3 par SRL — mutuellement exclusif). Aussi a risque : 1+ light-banks CRAM si K collisionne. Span software sol skippe.
- **Inutilise :** slave libere du sol ; VDP1 intouche ; **pas de banque spare** (4 pleines : ciel/cells/fb/map). CRAM est la ressource contestee, pas les cycles VRAM.
- **Gain perf :** **~+20-33 %** (meme skip) + flat dominant texture perspective-exact (qualite, pas fps de plus) sur une 3e unite HW (charge ni master ni VDP1, 0 tearing ajoute, `CRITICAL_PATH §5`). **Estime.**
- **Qualite :** **MEILLEUR sol des configs ciel-keeping** : vraie perspective-correct texture + K_LINECOL per-line possible. MAIS overlay parti + 1 seul flat/hauteur (1 plan RBG0). Ciel garde.
- **Faisabilite :** **NON PROUVE** — la voie qui garde ciel **ET** sol **cell** texture sur 4 banques (note : le bitmap config 8 SHIPPÉ garde déjà ciel+overlay sans CRKTE), mais : (1) CRKTE risque collision CRAM/palette-8bpp (mode 1 vs color-calc etendu) ; (2) SGL/SRL n'exposent **aucun** K-en-CRAM (`srl_vdp2.hpp` alloue toujours K en VRAM) → poke registre brut. `VDP2_LAYER_BUDGET config G`. Recherche, pas ship.
- **A essayer de maniere disruptive :** prouver CRKTE **d'abord** en isolation (poke RAMCTL bit15 + K a `CRAM 0x100800`, RBG0 off, observer la palette) **AVANT** de cabler le sol — decoupler les deux inconnues. Si CRKTE collisionne, fallback K_LINE/TwoAxis (peut couter 0 cycle, pourrait tenir en haut de A0 a cote du ciel). Si ca colle, ajouter K_LINECOL = fog Doom gratuit sur le flat dominant.

#### Config 10 — RBG0 CELL floor (ciel software, overlay off) (docs B/E / F1) — **superseded par config 8**
- **Bank map :** A0 = K-table (coefficient, freed par le ciel SW) · A1 = cells/char RBG0 (le flat dominant 64×64 swizzle en cells 8×8, re-upload ~0.6 ms au changement) · B0 = fb · B1 = map RBG0 **(EVINCE overlay NBG3 ET le back-screen color word)** · CRAM = 8 pal.
- **Ce qui part :** ciel HW (NBG0 off → ciel software sur le slave libere). **Overlay GONE** (map veut B1). Back-screen color collisionne aussi.
- **Inutilise :** A0 K-table en K_LINE/TwoAxis ~0 storage past la base → ~126 Ko de A0 libres en storage (mais les 8 slots owned par le read coefficient). A1 ~124 Ko libres past les 4 Ko cells. Slave ~80 % idle pot1/2 absorbe le ciel SW gratis. VDP1 intouche.
- **Gain perf :** **~+20-33 %** (`-17 a -26 ms` master ET slave) quand `dom%` du sol = le flat dominant. **Le gain vient du SKIP index-0**, pas de RBG0. **Estime** (A/B pot0). Au pot1/2 le prix retrecit (P deja 17/11 ms) ; valeur la-bas = QUALITE.
- **Qualite :** **MEILLEUR sol de la famille** : perspective-EXACT (pas de swim), texture, distance-dimme via cells colormap-swizzle. Perd ciel HW (ciel SW correct-ish, moins crisp) + overlay (cout dev seul).
- **Faisabilite :** **SUPERSEDED par config 8 (bitmap, SHIPPÉ).** Le sol cellulaire **snowait** sur HW — la cause s'est avérée être de la **starvation de cycle-pattern** (RAMCTL/CYC), pas un commit-gap : le bitmap (1 read de moins) + `rbg0_commit_ramctl`/`rbg0_commit_cyc` + RDBS=0x0D + cycles parqués shippe **propre** sans slSynch (cf. VDP2_RBG0_CURRENT_STATE.md), **et** garde l'overlay (B1 libre). Le cell-floor est donc parqué : le bitmap couvre le même besoin sans évincer B1.
- **A essayer de maniere disruptive :** ajouter l'eclairage K-table per-line (K_LINECOL=0x10, `slKtableRA` deja dans l'arbre `dg_saturn.cxx:1250` en K_LINE) → le flat dominant recoit le diminishing-light/fog de Doom **gratuit** sur la meme banque coefficient → fixe la brightness uniforme-par-secteur, 0 banque de plus (= config 11 fusionnee). (Note : sur le bitmap shippé le fog passe par écran line-color + color-calc, pas K_LINECOL.)

#### Config 11 — Per-line K_LINECOL LIT floor (enhancer SUR config 10) (docs F6)
- **Bank map :** identique a la config 10, mais la K-table en **K_LINECOL** (per-line coefficient = brightness per-bande-profondeur). **Pas de banque de plus vs F1** — la K-table est deja une banque entiere ; K_LINECOL change juste son contenu.
- **Ce qui part :** = config 10 (ciel HW → SW, overlay off). Rien de plus.
- **Inutilise :** = config 10. Les 8 slots de la banque K deja owned par le read coefficient → K_LINECOL coute **0 bande passante de plus** (meme read/dot, contenu different).
- **Gain perf :** **`0` de plus vs config 10** (meme skip). Le gain est **QUALITE** : le diminishing-light/fog de Doom sur le sol HW gratuit, fixe la limite « brightness uniforme par secteur » de F1. **Jo utilise exactement `slKtableRA(..., K_ON|K_LINECOL)`.** L'arbre appelle deja `slKtableRA` en K_LINE → un flag.
- **Qualite :** **MEILLEUR variant** du sol RBG0 texture : perspective-exact **ET** distance-shade (proche brillant, loin fog), matche le zlight software. Le plus gros upgrade visuel du chemin RBG0 a cout banque zero. Recommande par `VDP2_ARCHITECTURE §6.5`.
- **Faisabilite :** le commit RBG0 lui-même est **résolu** (config 8 shippe propre). Le fog **actuel** sur le bitmap shippé passe par écran line-color + color-calc RBG0, **pas** K_LINECOL. K_LINECOL reste une **piste qualité non-shippée** (per-line K) : prouvé ailleurs (Jo ship, constante SGL 0x10, `slKtableRA` deja dans l'arbre en K_LINE → un flag), mais demande la K-table per-line en VRAM (variante cell, pas le bitmap actuel). Risque independant = BAS.
- **A essayer de maniere disruptive :** utiliser le K per-line pour **faker une 2e surface** : au-dessus de l'horizon Doom, pointer le K vers un coefficient hauteur-plafond ; en-dessous, vers le sol → un plan RBG0 rendant sol en bas + plafond incline en haut, 0 banque (le substitut a la config 15). Ou animer la brightness per-line pour un sol lave-glow pulsant. Ou K_DOT (per-dot, 0x20) pour vraie perspective per-pixel si la bande passante suit.

### 2.D — Famille DISRUPTIF / EXOTIQUE (repenser la division VDP1/VDP2/SH-2)

#### Config 12 — WORLD-on-VDP1 (tout le monde → VDP1)
- **Bank map :** A0 = ciel HW · A1 = LIBRE · B0 = fb · B1 = overlay NBG3 · CRAM = 8 pal. Forme de base : rien ne quitte VDP2 ; on **deporte le monde du CPU vers VDP1**. Pour ajouter un sol RBG0 texture il faut dropper ciel HW + overlay (forme augmentee).
- **Ce qui part :** base = rien cote VDP2. Forme RBG0-augmentee = ciel HW + overlay.
- **Inutilise :** A1 100 % idle ; ~6 slots libres partout ; VDP2 = pur compositeur. **VDP1 NON idle (Dr 25-42 % pot0)** → headroom VDP1 = la vraie contrainte.
- **Gain perf :** **~+15-24 %** — master perd ~62 ms P-gen + slave perd sa moitie ~62 ms ; master n'attend jamais VDP1 (PTMR fire-and-forget, FBCR 1-cycle auto) → fps **reel**, pas un stall-shift. **Estime** (extrapole du precedent murs REC 110→5-23 ms).
- **Qualite :** PIRE pres de l'horizon (swim affine, mitige par bandes Y world-anchored) + plus de tearing (liste plus grosse). Murs inchanges. Permet le combo « ciel+sol+overlay » que les docs disent impossible — possible car le « sol » est un gradient 0-banque, pas RBG0.
- **Faisabilite :** **FONDATION PROUVEE** (driver async no-slSynch ship). **Cull VRAM requis** : VDP1 = 512 Ko PLEIN aujourd'hui. **~52 Ko honnetes** seulement (20 Ko HUD mort + 32 Ko wtex shrink ; les **176 Ko WPN aliasent wtex = 0 Ko** reclame — `VDP1_WORLD_PLAN §7.2`). Swim/Dr = verdict HW only. NO-GO si Dr post-floor < 80 %.
- **A essayer de maniere disruptive :** pousser **l'arme** sur VDP1 aussi (software aujourd'hui, son cache 176 Ko mort/aliase) → le **monde entier** (murs+sols+plafonds+arme) sur VDP1, VDP2 devient pur compositeur : ciel NBG0 + fb-comme-HUD NBG1 + overlay NBG3 + sol gradient 0-banque → **ciel+sol+overlay simultanes** (possible car le sol est 0-banque).

#### Config 13 — 4bpp framebuffer (libere une demi-banque B0)
- **Bank map :** A0 = ciel HW · A1 = LIBRE · B0 = **fb 4bpp 512×256 = 64 Ko + 64 Ko libre** · B1 = overlay · CRAM = sous-palettes 16-col empilees.
- **Ce qui part :** la profondeur 8bpp de Doom (256 PLAYPAL + 34 light). Un bitmap NBG 4bpp = **1 palette 16-col pour TOUT l'ecran** (le nibble palette par-cellule n'existe qu'en mode CELL, pas bitmap) → **unshippable pour la vue 3D**.
- **Inutilise :** si ca marchait : 64 Ko libres en B0, mais **echoues** (B0 ne peut heberger la map RBG0 : melanger fb char-reads + rotation PN-reads dans une banque = double whole-bank interdit). Les slots n'etaient jamais la contrainte.
- **Gain perf :** **blit HALVE** : 5.5 → ~2.75 ms (bus-bound sur les octets B0) — un gain master ~2.75 ms/frame **independant du sol** + 4bpp = 1 char-read/dot vs 2. **Estime.**
- **Qualite :** catastrophique pour la vue 3D (16 couleurs, pas de light ramps). Viable **seulement** en split HUD-region : vue 3D 8bpp rangees 0-191 + status-bar 4bpp rangees 192-223 avec sa palette.
- **Faisabilite :** **FANTASME pour la vue 3D** (8bpp non negociable pour Doom). Le **meme** gain blit dispo via **W5** (« blit que les rangees 3D quand le HUD est statique ») avec **0 perte qualite** → preferer W5.
- **A essayer de maniere disruptive :** faire **W5** a la place : detecter un HUD statique, blit que les rangees 0-191 → ~14 % d'octets blit en moins, 0 perte, pas de gymnastique palette. Reserver le vrai 4bpp pour un futur automap/menu ou 16 couleurs suffisent (et une banque libre pourrait alors heberger un 2e plan rotation).

#### Config 14 — Reduction-FB (ZMCTL 1/2 ou 1/4)
- **Bank map :** A0 = ciel HW (reduit) · A1 = capacite cycle freed · B0 = fb · B1 = overlay · CRAM = pal. Reduction ZMCTL sur NBG0/NBG1 multiplie les char-reads ; 1/2 cape a 256 couleurs, 1/4 a 16 et **desactive NBG2+NBG3**.
- **Ce qui part :** a 1/4 : NBG2 **ET** NBG3 (overlay) ; a 1/2 : NBG2 retrecit. La reduction **coute** des reads (×2/×4), elle retrecit une couche, elle ne **frees** pas une banque storage.
- **Inutilise :** minimal. La reduction adresse les **slots** (que Mimas n'a pas en pression : fb+ciel a ~2/8). Frees aucune banque storage (la contrainte liante).
- **Gain perf :** **~0** pour Mimas (la reduction cible la pression slots, absente ici). **Estime.**
- **Qualite :** PIRE (cap couleur + flou horizontal) pour 0 gain fps.
- **Faisabilite :** **COMPILE mais POINTLESS** — resout une contrainte que Mimas n'a pas tout en cassant l'overlay (a 1/4). Non-starter.
- **A essayer de maniere disruptive :** inverser l'intention — utiliser detailshift/M6 low-detail global (FastDoom, `r_main.c:701`, deja gate 2p) pour HALVER le travail mur+sol software a un cout chunky-160 → l'analogue software qui **paie vraiment**, vs la reduction VDP2 qui ne paie pas.

#### Config 15 — DUAL-ROTATION RBG0+RBG1 (docs F5) — **INFAISABLE**
- **Bank map voulu :** A0=K · A1=cells · B0=fb · B1=map (RBG0) **+ RBG1 = map+cells+K = 3 banques de plus**. Demande = fb(1)+RBG0(3)+RBG1(3) = **7 reclamations pour 4 banques**. Meme avec les 2 K en CRAM : 5 pour 4. CRAM = 8 palettes deja pleines → 2 K en CRAM doublement collisionnent.
- **Ce qui part :** physiquement ne rentre pas. Pour approcher : dropper ciel HW (deja) + overlay (deja) + partager cells (que si sol et plafond = meme flat, ce qui defait le but). RPMD over-mode contraint + halve la bande passante coefficient.
- **Inutilise :** N/A — sur-souscrit, -1 banque.
- **Gain perf :** skipperait sol **ET** plafond (plafonds toujours software) → plus grand que F1. Mais **UNREACHABLE**. Theorique.
- **Qualite :** serait la meilleure (sol+plafond perspective-exact) — mais infaisable. Substitut realiste : sol sur RBG0 (config 10) + plafond dominant sur VDP1 strips (config 6 multi-surface) — 2 unites HW, rentre dans le budget.
- **Faisabilite :** **INFAISABLE sur 4 banques** (prouve par arithmetique). SGL expose les entrees RB (`slPlaneRB`/`sl1MapRB`/`slKtableRB`/`slRparaReadCtrlRB`) mais le plafond VRAM est le mur dur. Les 2 plans partagent le moteur rotation + le CYCxx non resolu. NO-GO.
- **A essayer de maniere disruptive :** remplacer le reve dual-RBG par **RBG0(sol) + VDP1-strips(plafond)** : sol perspective-exact sur une unite HW, plafond multi-surface affine sur le VDP1 idle → 2 unites, 1 bank-set RBG0, rentre dans 4 banques, pas de 2e moteur rotation. La **seule** config « sol+plafond deportes » realisable. Alternative : 1 plan RBG0 time-slice sol-vs-plafond par Y-ecran via le K per-line (config 11 disruptif).

### 2.E — Multijoueur (hors scope single-view, mais documente)

#### Config MP — 2P split (software force)
- **Bank map :** A0 = ciel HW present mais **NBG0 GATE OFF** (`show_sky &&= sat_local_players<=1`, `dg_saturn.cxx:2664` → ciel software) · A1 = LIBRE · B0 = fb (composite 2 viewports) · B1 = overlay NBG3 dispo (pas de RBG0 qui conteste) · CRAM = 8 pal.
- **Ce qui part :** **ciel HW force OFF** (1 registre scroll ne peut encoder 2 viewangles). **RBG0 impossible** (1 matrice affine ne peut servir 2 cameras). Ciel **ET** sol software-only en MP.
- **Inutilise :** A0 (banque ciel) idle/dark ; A1 libre ; B1 dispo (overlay **plus** dispo qu'en solo — pas de conflit RBG0). Le NBG0 freed est gache (pas reutilisable per-view, limite mono-registre).
- **Gain perf :** **aucun offload VDP2** — le seul levier = potato (pad-Z). Sol 2p P~22 ms pot0 dominant → potato = le vrai knob fps MP. **Mesure partielle** (rows 2p « a isoler »).
- **Qualite :** ciel SW (pas de parallaxe HW) + sols SW, per-view-correct. Le potato choisi s'applique aux 2 viewports. Overlay peut rester (pas de contention RBG0).
- **Faisabilite :** **PROUVE HW shippe** (2026-06-22, core `8f1197f` / Mimas `0d1624c`). MP force explicitement ciel+sol software. *« le mode vdp2 marche pas en multi, on y reflechira apres. »*
- **A essayer de maniere disruptive :** windowing VDP2 (W0/W1) + back-screen/line-colour gradient **par moitie** → un fond ombre coherent pour chaque vue, ~0 banque, pas de registre scroll per-angle. Perd le parallaxe couple a l'angle mais donne a chaque vue une base ciel/sol distance-shade « gratuite » — la **seule** chose VDP2 qui survit au mur mono-registre/mono-matrice MP. Non prouve, parke, HW-only.

---

## 3. Idees disruptives transversales (non liees a une seule config)

| Idee | Tag | Pourquoi ca pourrait debloquer gros |
|------|-----|-------------------------------------|
| **COMPOSER au lieu de choisir** : gradient 0-banque (config 3) + nuages NBG2 dans A1 (config 4/5) + skip flat dominant → ciel + sol-gradient-anime + nuages + overlay + le `+20-33 %`, **0 banque jeu, 0 commit RAMCTL/CYC** | **PROUVE-class** | La config la plus riche qui **ship vraiment**. A1 est la ressource la plus sous-exploitee de toute config ciel-keeping |
| **RBG0 + VDP1 = PARTENAIRES, pas rivaux** (`CRITICAL_PATH §5`) : RBG0/gradient prend le **1** flat dominant (95 % des pixels sol en scene ouverte, le pire swim VDP1), VDP1-world prend les petits visplanes residuels (autres hauteurs, plafonds non-sky) | **mixte** (gradient PROUVE-class, VDP1 fondation-prouvee, RBG0 non-prouve) | Dimensionner chacun par `dom%` garde la liste VDP1 assez petite pour son budget async 1.6-2.5 ms. Aucun seul n'est la reponse |
| **TOUT-sur-VDP1** (config 12 max) : monde entier → VDP1, VDP2 = pur compositeur → frees A1 **ET** B1 | **fondation PROUVEE, ampleur non-prouvee** | Frees 2 banques pour un parallaxe nuages OU sortir le HUD/status-bar sur sa propre NBG (W5-style ~14 % blit). VDP2 devient compositeur pur |
| **Bouger TOUT le sol hors VDP2 fait disparaitre la GENERATION (P)**, pas que le fill : per-subsector VDP1 inverted-hybrid (emit pendant la BSP walk, nulle floorplane/ceilingplane a `R_Subsector`) → le bucket dominant 48 %-du-render disparait, **les deux SH-2 liberes** (~62 ms master + ~62 ms slave-equiv) | **fondation PROUVEE, emitter STUB** | Le plus gros levier compute du catalogue — attaque le P de 48 % a la racine, pas juste le fill |
| **Prouver le commit gradient SGL** (`slBackColTable`/`slLineColTable`) **comme le path VDP1 l'a ete** : ce sont des shadow-writes qui flushent normalement en slSynch (jamais lance) → tester si un poke direct colle comme FBCR/PTMR (valide 2026-06-16) | **a verifier HW** | Si ca colle, le sol gradient 0-banque (config 3) est **totalement debloque sans minefield** — debloque la config la plus rentable |
| **Classifier la map au LOAD** (open/closed, has-sky/no-sky) : maps closed/no-sky frees A0 pour une K-table RBG0 (sol texture, le ciel etait gache), maps open gardent ciel HW + gradient | **mecanique saine, commit prouvé au load** | Committer le layout **une fois au load** (jamais per-frame — fragile RAMCTL/CYC) via le toggle. Le signal per-frame `sat_frame_has_sky` existe deja |
| **4bpp framebuffer pour libérer une banque** : 8bpp 512×256 = 128 Ko = toute B0 ; 4bpp halve storage + halve les octets blit (~5.5→2.75 ms) | **fantasme pour la 3D, viable HUD** | Doom est 256-col → trop lossy pour la vue 3D ; viable pour le strip HUD 4bpp en sa propre NBG (= W5 sans perte, prefere W5) |
| **Fake-mode7 line-scroll** (config 7) : NBG per-line H-scroll + zoom widening-vers-le-bas ≈ plan sol **sans** moteur rotation, sans minefield CYCxx, sans cout 3-banques (1 banque cell) | **compile, dodge le minefield, non-prouve sous yaw** | Le trick SNES-mode7-on-Saturn pas cher. Qualite entre gradient et RBG0 ; rentre la ou la neige RBG0 bloque |
| **Color-math = un 2e renderer GRATUIT** : back-screen + line-color + color-offset (COAR/COAG/COAB) + color-calc sont **0 banque, 0 slot**, usables dans **N'IMPORTE QUELLE** config | **PROUVE-class (registres)** | Fog distance, damage-flash, pickup-flash, fondus, translucidite raccord ciel — en hardware. Tue le spike damage-flash (~40 ms re-bake CRAM). Idle dans tout build aujourd'hui |
| **RBG1 dual rotation** (config 15) | **API-reel mais bank-INFAISABLE** | SGL expose les entrees RB mais 7 reclamations/4 banques l'interdit. Substitut realisable = RBG0(sol)+VDP1(plafond) ou 1 plan RBG0 time-slice par K per-line |
| **Mesh/dither transparency sur VDP1** : le mode mesh (damier) de VDP1 = fausse transparence a cout blend zero | **PROUVE-class (path VDP1)** | Specters/invisibilite et sprites fading **sans** depenser une couche color-calc VDP2. Orthogonal au bank ledger |
| **Re-cibler le slave idle = MIRAGE** (sauf un cas) : SLVidle 29-62 % = phase B (BSP+wall-prep) **memory-bound** → un slave cache-cold (rL=2.1 HW) ne bat pas le master sur du memory-bound | **DEAD (mesure)** | Dead-ends prouves : wallprep-slave (+5.8 ms), plane work-steal (regresse). **Le seul bon reuse** : quand le sol quitte le CPU (→VDP1/VDP2), le slave libere absorbe le **ciel software** re-ajoute |

---

## 4. Spec du TOGGLE ecran-titre

### 4.1 Principe de commit (la loi)
Le **layout de banque** se commit **UNE SEULE FOIS**, avant le gameplay (au pire **par map au
load**), **jamais per-frame** : un re-commit RAMCTL/CYCxx mid-game sans slSynch est poison.
Seul le **flag software-skip** (`sat_vdp2_floor` on/off) est cheap a
basculer per-frame. Le **toggle potato** (pad-Z, 7 niveaux) reste **orthogonal** — il varie la
qualite software dans n'importe quel layout sans toucher les banques.

### 4.2 Menu joueur propose (groupe par robustesse)

| Entree menu (player-facing) | Config(s) | Build flag vs runtime | Etat |
|-----------------------------|-----------|-----------------------|------|
| **Classic** *(defaut)* | 1 / 2 (+ potato ladder) | runtime (deja le build shippe) | **PROUVE HW** |
| **Fast floor** | 3 (gradient) | runtime (skip = flag ; gradient = nouvelle table, pas de banque) | **a verifier HW** (commit back-screen) |
| **Rich sky** | 4 / 5 (nuages/parallaxe NBG2 dans A1) | runtime (NBG2 dans A1 libre) | **likely-OK** (NBG2 jamais allume) |
| **VDP1 floor** *(multi-surface)* | 6 | **build flag** (emitter non bati + cull VRAM) | fondation prouvee, **emitter STUB** |
| **Fake-mode7 floor** | 7 (line-scroll) | **build flag** (path neuf) | non-prouve sous yaw |
| **Textured floor** *(RBG0 bitmap)* | **8 (SHIPPÉ)** | **build flag** (`RBG0_BITMAP=1`, gaté potato-0 + 1P) | **PROUVE HW** (cf. VDP2_RBG0_CURRENT_STATE.md) |
| **Textured floor** *(RBG0 cell + K_LINECOL)* | 10 / 11 | **build flag** | superseded par config 8 ; cell-floor parqué |
| **Sky + textured floor** *(CRKTE cell)* | 9 | **build flag** | recherche (collision CRAM ; config 8 couvre déjà ciel+overlay) |
| *(non expose — fantasme/pointless)* | 13, 14, 15 | — | infaisable/pointless |

### 4.3 Exclusions mutuelles (build vs runtime)
- **Garde l'overlay** (B1 libre) : **1, 2, 3, 4, 5, 6, 7** (memes banques de base A0=ciel/B0=fb) **et le sol RBG0 bitmap SHIPPÉ config 8** (bitmap = pas de map → RPT en B1+0x1ff00, B1 libre). Les 7 premières ne committent **aucun** RAMCTL/CYC ; la 8 commit son RAMCTL/CYC **une fois au load** (block-flush direct, prouvé HW).
- **Build-exclusives qui ÉVINCENT l'overlay** : seules les variantes sol **cellulaires** **9, 10, 11** (map RBG0 en B1). **Overlay XOR sol-RBG0-CELLULAIRE** reste vrai — mais le sol bitmap shippé (config 8) **lève** cette contrainte (overlay conservé). Préférer config 8.
- **Potato (pad-Z)** : orthogonal a tout — runtime, dans n'importe quelle entree.

### 4.4 Ordre d'implementation propose (gain/risque)

1. **Config 3 (Fast-floor gradient)** — *highest-confidence fps move*. Skip deja shippe + gradient 0-banque + garde ciel+overlay. **Premier a cabler** (apres validation HW du commit back-screen).
2. **Config 4/5 (nuages/parallaxe NBG2)** — eye-candy +0 REC, A1 libre, meme classe que le ciel prouve. **Bas risque, fort visuel.** Cable en parallele de la 3.
3. **Config 6 (VDP1-floor)** — le vrai cheval 1P (multi-surface, master n'attend jamais VDP1). **Cabler le toggle A/B d'abord** pour mesurer Dr/swim ; emitter a batir.
4. **Config 8 (RBG0 bitmap)** — **SHIPPÉ** (gaté potato-0 + 1P). Le sol texture HW propre qui garde l'overlay ; commit RAMCTL/CYC résolu au load.
5. **Config 10/11 (RBG0 cell + K_LINECOL)** — **superseded par config 8** (cell-floor parqué). K_LINECOL = piste qualité non-shippée.
6. **Configs 9 (CRKTE), 7 (fake-mode7)** — research/experimental, gates HW propres avant tout.
7. **Configs 13/14/15** — **non exposees** (documentees pour ecarter, pas poursuivre).

### 4.5 Defaut
**Config 1 (Classic)** — la seule prouvee bout-en-bout. Le toggle propose les autres comme
experiences, jamais comme defaut tant qu'elles ne sont pas validees HW.

---

## 5. Verdict — le chemin recommande

### Les 3-4 configs a cabler en premier, dans l'ordre

1. **Config 3 — Fast-floor gradient (H/F4).** *Le sleeper et le meilleur rapport gain/risque.* Elle
   capte ~le **meme `+20-33 %`** que RBG0 (le gain EST le skip), garde **ciel HW + overlay**, ne
   touche **aucune** banque et ne commit **aucun** RAMCTL/CYC. `CRITICAL_PATH §5.1` la nomme « lowest-risk
   highest-confidence fps move ». **A cabler en premier.**

2. **Config 4/5 — Rich-sky NBG2 (S3/S6).** *Visuel pur, +0 REC, A1 libre.* Meme classe que le ciel
   HW prouve, pas de poke, pas de slSynch. Donne le plus beau ciel du catalogue (parallaxe 2 plans +
   color-calc) pour ~gratuit. **A cabler en parallele de la config 3** (elles composent : ciel-parallaxe
   + sol-gradient + overlay + le gain).

3. **Config 6 — VDP1-floor multi-surface (S5/F3).** *Le vrai cheval 1P pour la qualite + le fps.*
   Roule sur le **driver async no-slSynch deja prouve-HW** (FBCR/PTMR), garde ciel **ET** overlay,
   et est **multi-surface** (sols+plafonds+marches) la ou RBG0/gradient ne font qu'un flat. Mais
   l'**emitter est un stub** → batir le **toggle A/B d'abord** pour mesurer `Dr%`/swim sur scene fixe.

4. **Config 8 — RBG0 BITMAP floor (C/F2). SHIPPÉ ✅.** *Le sol texture HW qui a gagné.* Bitmap
   512×256 8bpp propre (commits 19768ca/41dd895, gaté potato-0 + 1P), garde ciel HW **ET** overlay
   (B1 libre), commit RAMCTL/CYC résolu au load (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, sans slSynch).
   La variante **cellulaire** (config 10/11, perspective-exact + K_LINECOL fog) est **superseded** :
   elle snowait (starvation cycle-pattern) et évince l'overlay. Réf : [`docs/VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md).

### Les plus gros inconnus — *a verifier sur HW*

- **(config 3)** Le commit `slBackColTable`/`slLineColTable` colle-t-il sans slSynch comme FBCR/PTMR ? *Le seul gate de la config la plus rentable.* — **a verifier HW.**
- **(config 6)** Le `Dr%` post-floor reste-t-il ≥ 80 % une fois ~158 quads sol ajoutes a un VDP1 deja tendu (Dr 25-42 %) ? *Le gate reel, pas Vp.* — **a verifier HW.**
- **(config 8, RÉSOLU)** Le commit RAMCTL/CYC colle sans slSynch (block-flush direct au load) — **prouvé HW** (sol bitmap shippé). La variante cell (10/11) reste parquée. — **résolu, cf. VDP2_RBG0_CURRENT_STATE.md.**
- **(config 9)** CRKTE coexiste-t-il avec la palette 8bpp dans 4 Ko de CRAM (mode 1 vs color-calc) ? — **a verifier HW, en isolation d'abord.**
- **(config 12)** Combien de VRAM VDP1 honnetement reclamable ? (~52 Ko vs le mirage 196 Ko) et le swim affine est-il acceptable ? — **a verifier HW.**
- **(general)** Le framebuffer 320×200 laisse-t-il du storage en B0, ou consomme-t-il toute la banque comme un 512×256 ? — **a verifier** (`VDP2_LAYER_BUDGET §3 piste B`).

### Le fil rouge
**Le gain est le skip ; la techno de sol est du backfill de qualite.** Donc : ship le **skip + gradient
0-banque (config 3)** d'abord (prouve-class, garde tout), ajoute le **ciel NBG2 (config 4/5)** pour
le visuel, puis traite **VDP1-floor (config 6)** et **RBG0 (config 10/11)** comme des **upgrades de
qualite** sur le flat dominant — **jamais** comme le prerequis fps. Trois unites HW, trois tiers de
surface, **un seul skip**. Au **potato shippe** (pot1/2), tout sol fancy devient surtout une affaire
de **qualite** (le slave est deja idle, P deja effondre) — les configs valent le plus au **pot0**.

---

### Sources
- Docs repo : [`docs/VDP2_LAYER_BUDGET.md`](VDP2_LAYER_BUDGET.md), [`docs/VDP2_RBG0_CURRENT_STATE.md`](VDP2_RBG0_CURRENT_STATE.md) (réalité sol shippée), [`docs/VDP1_PRESENT_SYNC_PLAN.md`](VDP1_PRESENT_SYNC_PLAN.md) (présent VDP1↔NBG1), `docs/CRITICAL_PATH.md`, `docs/VDP1_WORLD_PLAN.md`, `docs/VDP2_ARCHITECTURE.md`, `docs/REC_BENCHMARKS.md`.
- Code : `src/dg_saturn.cxx`, `core/r_plane.c`, `core/r_parallel.c`.
- SRL/SGL : `SaturnRingLib/saturnringlib/srl_vdp2.hpp`, `srl_ascii.hpp`, `SaturnRingLib/modules/sgl/INC/sl_def.h`.
- Enumerations sources : 5 agents paralleles (ciel-centric S1-S6, sol-centric F1-F6, baseline/overlay, disruptif/exotique, perf-grounding), fusionnees 2026-06-26.
