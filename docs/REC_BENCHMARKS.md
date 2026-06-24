# REC Benchmarks — table de référence vivante (Ymir + hardware)

Table comparative partagée entre sessions pour suivre l'évolution de **REC** (et de
la frame) build après build. À mettre à jour à chaque capture. Plan des leviers =
`docs/REC_REDUCTION.md` ; analyse = mémoire `doomsrl-perf`.

> **⚠️ Loi de mesure.** Ymir **sous-estime massivement** le coût memory-bound (DRAM
> lente, pointer-chasing BSP/visplane). Sur Ymir REC/Bw/P sont 3–10× trop bas et un
> gain DRAM (QW2, L1, L2) **n'apparaît pas**. Ymir sert à : (a) vérifier la
> **correction** (identité visuelle, pas de crash, byte-identity pot0), (b) confirmer
> la **mécanique** (compte de commandes `c`, EX, slave idle%, splits rows 11/12).
> **Tout verdict de gain REC se lit sur hardware**, jamais sur Ymir.

## Légende des colonnes (overlay rows — voir mémoire `doomsrl-overlay-rows`)

| Col | Source | Sens |
|---|---|---|
| **REC** | row 19 | génération mono-master (BSP+planes+sprites) ; slave exécute en bg |
| **Bw / Bp / P / M** | row 20 | sous-phases REC : BSP-walk / wall-prep (R_StoreWallRange) / planes / masked |
| **Bp s/l** | row 11 | Bp split : `s`=trig/scale setup par seg · `l`=boucle R_RenderSegLoop par colonne (clip+visplane-mark+texturecolumn) |
| **P a/m/o** | row 12 | P split : `a`=allocateur flat (W_CacheLumpNum/Release) · `m`=R_MakeSpans+R_MapPlane · `o`=sort/sky/control |
| **EX / W** | row 19 | drain du buffer de commandes (master+slave) / temps où le master attend le slave |
| **c** | row 19 | nb de commandes enregistrées dans la frame |
| **SLV i%** | row 18 | slave idle% pendant sa phase opaque (i100 = rien à dessiner) |
| **AVG / inst** | row 17 | fps EMA(~4 s) / fps instantané. `inst` = comparable au spot ; `AVG` traîne (EMA) |
| **bl/f** | row 5 | blit ms/frame = `bl`÷`f` (config dual-CPU = row 2) |

Identifiants de spot : `vp` (pic visplanes, row 2) + description. **Idéalement, aligner
les spots Ymir sur les 6 spots de référence hardware** (tech/eau, sombre/esc, brun/eau)
pour un vrai A/B Ymir↔HW.

---

## A. Ymir — courant : post-QW1/QW2, profiler rows 11/12 (commit `5f6be72`, 2026-06-18)

QW1 **on** (toujours) · QW2 **on** (`SAT_POTATO_INLINE_SPANS=1`). Blit 50/50 partout.

| Spot (vp) | pot | REC | Bw | Bp (s/l) | P (a/m/o) | M | EX | W | c | SLV i% | AVG/inst | bl/f |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Y-eau (21) | 0 | 26.8 | 2.3 | 9.3 (1.8/7.5) | 13.7 (0.0/6.6/7.0) | 1.3 | 12.4 | 0.4 | 390 | 50 | 18.1/19.0 | 6.9 |
| Y-eau (21) | 1 | 28.1 | 2.3 | 9.8 (1.8/7.5) | 14.6 (0.0/7.9/6.5) | 1.3 | 0.8 | 0.0 | 116 | 100 | 21.3/24.0 | 6.9 |
| Y-eau (21) | 2 | 22.4 | 2.3 | 9.8 (1.9/7.9) | 8.9 (0.0/7.9/0.9) | 1.3 | 0.8 | 0.0 | 116 | 100 | 25.2/28.5 | 6.7 |
| Y-écrans (33) | 0 | 45.7 | 3.0 | 15.8 (3.0/12.8) | 24.9 (0.0/15.1/9.6) | 1.9 | 6.2 | 0.2 | 1019 | 62 | 21.4/15.0 | 7.5 |
| Y-écrans (33) | 1 | 46.1 | 2.9 | 16.2 (2.7/13.4) | 24.3 (0.0/14.5/9.7) | 2.6 | 1.0 | 0.0 | 153 | 100 | 17.5/16.7 | 6.6 |
| Y-imps (33) | 2 | 37.2 | 2.9 | 16.0 (2.8/13.1) | 16.0 (0.1/14.5/1.3) | 2.2 | 1.0 | 0.0 | 175 | 100 | 17.6/19.4 | 6.9 |

### Ce que ces captures établissent (mécanique, valide même sur Ymir)

1. **QW2 confirmé mécaniquement.** À spot constant, pot0→pot1 effondre le compte de
   commandes (Y-eau **390→116**, Y-écrans **1019→153**) et EX (12.4→0.8 ; 6.2→1.0),
   et le slave passe à **i100 %** (les spans de sol ne sont plus enregistrés/exécutés,
   memset inline). **pot0 byte-identical** : sols texturés, slave draws (i50–62 %).
2. **REC reste plat pot0→pot1 sur Ymir** (Y-eau 26.8→28.1, Y-écrans 45.7→46.1). Le
   trafic DRAM économisé (écriture 32 B/span supprimée) **n'apparaît pas sur Ymir** →
   **le verdict REC de QW2 est hardware-gated** (cf. loi de mesure). Le memset inline
   tourne désormais sérialisé sur le master pendant P : gain net = trafic-record évité
   − memset sérialisé → **à trancher sur hardware uniquement**.
3. **`P a` ≈ 0.0 partout** → l'allocateur flat n'est PAS un coût (flats mmappés cart =
   pointeur direct). Le pré-cache-gate multi (M1) n'a aucun signal Ymir ici ; à
   re-mesurer off-cart/HW.
4. **`Bp l` ≫ `Bp s`** (~13 vs ~3) → Bp dominé par la boucle clip/visplane-mark par
   colonne, pas la trig. ⇒ **QW1 risque de peu bouger** (texturecolumn n'est qu'une
   fraction de la boucle) — exactement l'hypothèse §6.6 de REC_REDUCTION. À confirmer HW.
5. **`P m` = le gros de P** (6.6–15.1) ; **Bw minuscule sur Ymir** (2.3–3.0) alors qu'il
   était **7–26 ms sur HW** → le gain de **L1 (hash visplane, attaque Bw/R_FindPlane)
   est invisible sur Ymir**, HW-only.

### A.bis — Ymir L1 (post-QW + visplane hash `SAT_VISPLANE_HASH=1`, 2026-06-18)

Validation de **L1 byte-identity** : Romain s'est replacé aux mêmes spots.

| Spot (vp) | pot | REC | Bw | Bp (s/l) | P (a/m/o) | M | EX | c | SLV i% | inst | FLAT t/d/dom%/n |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Y-eau (21) | 0 | 28.1 | 2.3 | 9.8 (1.8/7.9) | 14.7 (0.0/7.0/7.6) | 1.3 | 12.0 | **390** | 50 | 18.7 | 49k/24k/49%/19 |
| Y-eau (21) | 1 | 29.0 | 2.2 | 9.8 (1.9/7.9) | 15.6 (0.1/7.5/7.9) | 1.3 | 0.8 | **116** | 100 | 33.6 | 49k/24k/49%/19 |
| Y-eau (21) | 2 | 23.0 | 2.3 | 9.8 (1.9/7.9) | 9.5 (0.1/7.5/1.8) | 1.3 | 0.8 | **116** | 100 | 27.5 | 49k/24k/49%/19 |
| Y-écrans (33) | 0 | 47.0 | 2.8 | 15.4 (2.7/12.8) | 26.8 (0.1/14.9/11.7) | 1.9 | 6.9 | 1068 | 62 | 14.5 | 35k/26k/74%/33 |
| Y-écrans (33) | 1 | 46.7 | 2.8 | 15.5 (2.7/12.8) | 26.2 (0.1/14.5/11.6) | 2.0 | 1.0 | 157 | 100 | 15.5 | 35k/26k/74%/33 |
| Y-imps (33) | 2 | 40.2 | 2.8 | 15.7 (2.7/12.8) | 18.3 (0.1/15.1/3.0) | 3.3 | 1.5 | 236 | 100 | 18.7 | 36k/26k/73%/33 |

**L1 byte-identity CONFIRMÉ sur Ymir.** Aux spots Y-eau (re-capturés au même endroit), `c` est
**identique** au pré-L1 (390/116/116) et Bw/Bp inchangés → le hash retourne exactement les mêmes
visplanes (même draw-list). Aucune corruption sol/mur/sprite. **Bw reste 2.2–2.8 ms** → gain L1
invisible sur Ymir (l'O(n²) DRAM que L1 attaque ne se paie pas sur la mémoire émulée) ⇒ **verdict
gain = hardware uniquement.** Les écarts REC à Y-écrans = variance de frame (monstres/écrans animés).

**Row 13 FLAT (nouveau, levier FUTUR séparé — offload sols sur VDP2 RBG0, PAS L1) :** la grande
salle écrans concentre **~74 % du remplissage sol dans UN flat** sur 33 visplanes (dom% élevé +
n modéré = candidat RBG0 « un gros flat » intéressant) ; Y-eau fragmenté (49 %). À creuser plus
tard si on attaque P par le hardware VDP2.

---

## B. Hardware — baseline PRÉ-QW1/QW2 (réel Saturn, 2026-06-17, 6 photos)

Dernières vraies mesures hardware avant les quick-wins. Pas de Bw/M/splits sur ce set.
**À remplacer par un set post-QW (section C) dès accès hardware.**

| Spot | pot | frame ms | fpsi | REC | Bp | P | EX | blit | SLV idle | VD1 |
|---|---|---|---|---|---|---|---|---|---|---|
| tech/eau | 1 | 80 | 12.4 | 51.2 | 21.0 | 19.0 | 2.7 | 11.8 | i85 | 133B |
| sombre/esc | 1 | 114 | 8.7 | 79.1 | 31.4 | 31.2 | 4.0 | 12.0 | i83 | 134B |
| brun/eau | 2 | 69 | 14.3 | 43.2 | 19.5 | 12.6 | 2.7 | 11.7 | i82 | 34D |
| flash rouge | 2 | 140 | 7.1 | 71.2 | 28.3 | 24.0 | 5.1 | 12.0 | i81 | 42D |
| sombre/esc | 0 | 175 | 5.7 | 105.6 | 31.4 | 55.6 | 30.7 | 11.8 | i41 | 134B |
| brun/eau | 0 | 94 | 10.6 | 53.7 | 20.5 | 21.9 | 12.5 | 12.1 | i59 | 133B |

Rappel HW : blit **~12 ms constant**, slave **~80 % idle** à pot1/2, REC = 60–70 % de
la frame. Échelle HW ≈ 2–4× les chiffres Ymir (et Bw/P encore plus, memory-bound).

---

## C. Hardware — post-QW1/QW2 + L1/L2 — **À REMPLIR (pas d'accès HW avant ~2026-06-21)**

| Spot | pot | REC | Bw | Bp (s/l) | P (a/m/o) | M | EX | c | SLV i% | inst | bl/f | Build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| _(à capturer)_ | | | | | | | | | | | | |

**Protocole** (Romain, hardware) : aux 6 spots de référence, lire rows 19/20/11/12/18/17
+ row 2 (pot, blit). A/B QW2 via `SAT_POTATO_INLINE_SPANS 0`. Attendus : QW1 → Bp↓ à
pot1/2 ; QW2 → P↓ + `c`↓ fort ; surveiller tout tear/garbage dans les sols (aucun attendu).

---

## Protocole de mise à jour

1. Une **ligne par (spot, pot, source)**. Garder Ymir et HW dans des sections séparées
   (échelles incomparables — loi de mesure).
2. À chaque nouveau build perf : noter le **commit** dans le titre de section, refaire
   la passe Ymir (correction + mécanique), puis la passe HW dès accès.
3. Quand un set HW post-QW remplace la baseline B, **garder B en historique** (ne pas
   l'écraser — c'est notre seul point de comparaison pré/post quick-wins).
4. Conclusions de gain REC = **section C (HW) uniquement**. Ymir = sanity + mécanique.

---

# WADs témoins — banc de stress

But : disposer de **scènes de référence reproductibles** dont on connaît à l'avance le
poste de coût dominant, pour **attribuer** chaque chute de fps. WADs locaux dans
`wads_temoins/` (gitignoré ; IWADs copyright id copiés depuis `Downloads/`, PWADs libres
téléchargés depuis idgames — voir `wads_temoins/README.md` pour les chemins/warps exacts).

## Pourquoi un WAD est « gourmand » — 4 postes (+ mémoire)

Sur le rasterizer logiciel, la gourmandise se décompose en axes **séparables**, chacun
mappé à un coût Saturn — c'est ce qui rend les témoins utiles (un témoin = un axe isolé).

| Poste | Déclencheur | Limite vanilla | Coût Saturn |
|---|---|---|---|
| **Visplanes / span-fill** | Beaucoup de sols/plafonds co-visibles à hauteur/flat/lumière distincts | `MAXVISPLANES=128` → **256** (`spanstart_l[256]`) | 1 passe span-draw/visplane ; en **LWRAM lente** → double peine |
| **Walls / drawsegs / openings** | Longues visées dégagées, murs two-sided | `MAXDRAWSEGS=256`, `MAXSEGS=32` | segs à clipper + colonnes au SH-2 esclave |
| **Overdraw** | Géométrie empilée, gros sprites chevauchants (pas de z-buffer) | — | cycles SH-2 gaspillés sur segs occultés ensuite |
| **Simulation (AI/intercepts/vissprites)** | Beaucoup de monstres ; Arch-Viles (IA la + chère), Pain Elementals | `MAXVISSPRITES=128` (flicker non-fatal) | **tout sur le SH-2 maître AVANT le rendu** ; le split dual-SH-2 **n'aide PAS** (l'esclave ne fait que rendre) |
| **Mémoire** (transversal) | Grosses maps, sauvegardes lourdes | save buffer **~180 KB** | sur heap 88K + streaming CD : mode d'échec à part, hors fps |

Règle de classification décisive : **vanilla** (sous les limites → tourne) vs
**limit-removing** (crash « No more visplanes » / « too many drawsegs » sur l'exe d'origine
→ crash sur le port aussi, sauf si le tableau concerné a été agrandi).

## L'échelle de témoins (sûr → torture)

| Témoin | WAD | Carte | Vanilla | Isole |
|---|---|---|---|---|
| Phobos Anomaly | Doom1.WAD | E1M8 | ✅ | drawsegs/solidsegs — **seul sur cartouche, zéro CD** → baseline propre |
| Industrial Zone | Doom2.wad | MAP15 | ✅ | drawsegs + openings (chemin murs/colonnes) — déjà sur le CD |
| The Living End | Doom2.wad | MAP29 | ✅ | visplanes + overdraw (géométrie étagée) |
| Hunted | Plutonia.wad | MAP11 | ✅ | **AI pure** (≈18 Arch-Viles) — inverse exact des maps géométriques |
| Tricks and Traps | Doom2.wad | MAP08 | ✅ | vissprites + intercepts (gros sprites, espace confiné) |
| Downtown | Doom2.wad | MAP13 | ⚠️ | visplanes — overflow vanilla « après la clôture » → valide le cap 256 |
| **Mount Pain** | Tnt.wad | MAP27 | ⚠️ | **seul overflow visplane IWAD confirmé** (chambre est, face mur) + 332 monstres |
| Go 2 It | Plutonia.wad | MAP32 | ✅ | combat le + dense **légal** (11 Pain El. + 19 Arch-Viles) — ne crashe pas, rame |
| Unholy Cathedral | Doom-ud.wad | E2M5 | ✅ | **mémoire/heap** (le 32X ne la chargeait pas) — réussite = budget validé |
| Post Mortem | +HR.WAD | MAP24 | ⚠️ | **save buffer ~180 KB** — le **SAVE** est le témoin, pas les fps |
| (torture) nuts | +nuts.wad | MAP01 | ❌ | plafond débit thinkers+vissprites — **contrôle négatif**, crash/crawl attendu |

Protocole : **grimper l'échelle dans l'ordre**, point de vue **fixe** par map, lire
REC/frame (rows 19/20/11/12). E1M8 d'abord (zéro latence CD parasite) ; **soustraire la
latence CD** pour toute map non-shareware (Doom II/Plutonia/TNT/Ultimate streament). Si les
fps s'effondrent sur Hunted mais tiennent sur MAP29 → limiteur = **sim maître**, pas le
rasterizer. Pour valider 256 : MAP13 / TNT27, surveiller `r_visplane_peak` franchir 128
sans crash/HOM. ⚠️ **Ne pas mesurer sur slaughter Boom** (Sunder, Okuplok, Cosmogenesis,
Deus Vult, Holy Hell, Chillax) : linedefs/actions Boom non gérés par Chocolate → logique
de carte potentiellement fausse.

## Accès direct aux cartes (warp) — port Saturn — **CÂBLÉ**

Le core gère **`-warp`** (`d_main.c:1934` → `startmap`/`autostart` → `G_InitNew`, **saute le
menu** ; Doom II = `atoi` 1 token, Doom 1 = 2 chiffres). Câblé derrière le flag compile
**`SAT_WARP_MAP`** (Makefile) → `doom_start()` construit l'`argv` `-warp`/`-skill`
(`dg_saturn.cxx`). **Vide par défaut = boot menu normal, aucun changement.**

```sh
make SAT_WARP_MAP=15        # Doom II -> MAP15 (UV)
make SAT_WARP_MAP="4 2"     # Doom 1  -> E4M2 (deux chiffres)
make SAT_WARP_MAP=27 SAT_WARP_SKILL=4   # TNT MAP27, skill 1-5 (4 = UV, défaut)
```

`SAT_WARP_MAP` est tokenisé sur l'espace : `"15"` → `-warp 15` ; `"4 2"` → `-warp 4 2`.
**IDCLEV** (`st_stuff.c:610`) existe dans le core mais exige de taper « idclev15 » au
clavier → non câblé au pad, inutilisable tel quel.
