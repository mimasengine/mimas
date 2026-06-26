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

### A.bis — Ymir L1 (post-QW + visplane hash `sat_visplane_hash`, 2026-06-18)

> **RECONCILED 2026-06-24 :** L1 n'est plus un `#define SAT_VISPLANE_HASH` mais un
> **toggle RUNTIME `sat_visplane_hash`** (défaut 1, A/B live au pad-Y, core ea452d8).
> QW1/QW2/L1 sont tous **SHIPPÉS + default-on** ; la table HW §C reste le **seul livrable
> de mesure restant** pour les verdicts de gain.

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

## A.ter — Ymir shareware E1M1, NOUVEAU profiler fenêtré (1j, h1, 2026-06-24, build b:20:07)

Premières captures avec le profiler fenêtré (p50/p95/max, peaks par phase, sizers floor,
calibration mémoire). **⚠️ Doom shareware NON-STRIPPÉ (~14MB ici, >4MB → CD-STREAMING, pas
de cart)** — donc CD-thrash comme Doom 2, PAS résident. ⚠️ Spots DIFFÉRENTS d'une capture à
l'autre (FLR `n` varie 584→3823 = vues plus/moins chargées) → le p50/p95 inter-config
**mélange mode ET spot** ; pour une échelle propre, rester immobile à UN spot plane-heavy et
cycler pad Z.

**RUN 2 (build b:21:59, garde `RP_REC_SANE` actif → `d0` = ZÉRO frame droppée).** **E1M1 STRIPPÉ
(`tools/strip_wad.py`) → CART-RÉSIDENT, PAS de streaming** (d'où `d0` : aucune lecture CD). Méthode :
**parcours COMPLET du niveau, photo en FIN** → **p50/p95/PK agrègent TOUT le niveau (représentatifs)** ;
seuls `FLR`/`MST`/inst reflètent le spot final (une salle bois `n3`).

| pot | fps i/avg | p50 | p95 | mx | MST | SLVi | Dr | PK Bw | PK Bp | PK P | PK M |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pot0    | 36.4/26.3 | 32.0 | 58.0 | 88.7 | 27 | 21% | 94% | 7.0 | 54.4 | 44.0 | 26.3 |
| pot0.5  | 41.0/27.5 | 28.0 | 54.0 | 75.5 | 24 | 24% | 93% | 6.6 | 53.6 | 34.8 | 24.6 |
| pot1    | 53.0/45.1 | 18.0 | 42.0 | 75.2 | 18 | 39% | 93% | 6.9 | 53.9 | 18.4 | 23.9 |
| pot2-bd | 52.0/37.9 | 18.0 | 40.0 | 71.0 | 19 | 39% | 93% | 7.4 | 54.5 | 17.0 | 24.4 |

MEM **rL0.7** (lw512 hw664) — ⚠️ **SWAPPÉ** vs run1 (lw662 hw512 → rL1.2) : les deux valeurs ont
littéralement échangé (total ~constant 1176) ⇒ **MEM-bench NON FIABLE sur Ymir** (variance inverse =
pollution VBlank-IRQ probable landant dans l'un OU l'autre bench). HW-only, ET **à durcir** (min-of-N,
IRQ off) avant de s'y fier. *(RUN 1 b:20:07, spots différents + non-gardé → PK/mx pollués CD
`Bp~300 mx433-647`, superseded ; p50/p95 run1 ≈ run2.)*

**Ce que ça établit (Ymir = mécanique/sanity, PAS les gains memory-bound) :**
1. **Garde anti-glitch OK** (`d0`) → les PK redeviennent du RENDU (plus de `Bp~300`). ⚠️ peut rester
   du leak <300ms ; le **p50/p95 reste le signal robuste**.
2. **Les PK révèlent la structure du pire-cas** (= la valeur de l'idée « peaks par phase ») :
   - **PK Bp ~54ms POTATO-INDÉPENDANT** (54.4/53.6/53.9/54.5 identiques) = le pire frame mur ; **le
     potato ne touche PAS Bp**.
   - **PK P 44→17 POTATO-DÉPENDANT** (le sol) ; **PK M ~24-26** (sprites combat).
   - ⇒ **Hypothèse forte à valider HW : potato améliore la MOYENNE (p50 32→18) mais PAS le PIRE
     (Bp~54 partout)** → pour la fluidité, attaquer **Bp** (murs), pas le sol.
3. **`Dr 93-94 %`** — VDP1 finit ~93% des frames → marge réelle (« always B » périmé). HW à relire.
4. **`FLR` = le SPOT FINAL (n3, salle bois)**, pas le pire sol du niveau (`FLR` est instantané, non
   fenêtré) → pour le signal floor-offload, **photographier en STATIONNANT** dans la cour/nukage E1M1
   (grand sol ouvert, `n` élevé), pas en fin de parcours. (Idem `MST`/inst = spot final.)
5. **MEM rL non fiable Ymir** (swap 1.2↔0.7) → ne rien conclure ; HW + bench durci.

---

## B. Hardware — baseline PRÉ-QW1/QW2 (réel Saturn, 2026-06-17, 6 photos)

> **RECONCILED 2026-06-24 — section B = baseline STALE.** Ce set est **PRÉ-QW1/QW2/L1**
> ET sa colonne **blit (~11.7–12.1 ms) est SUPERSEDED**. Le blit a été **recalibré à
> ~5.5 ms single-CPU** (`dg_saturn.cxx:366-368`, 2026-06-22) → **soustraire ~6 ms de
> chaque somme frame-ms de ce tableau** pour le build courant. **Conservé** comme seul
> point de comparaison pré-QW (cf. protocole §3, ne pas écraser).

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

Rappel HW : blit **~5.5 ms single-CPU** (RECONCILED 2026-06-22 ; était noté ~12 ms ici,
chiffre PÉRIMÉ ; **dual-CPU blit ESSAYÉ puis DROPPÉ** — aucune config ne bat le single,
blit bus-bound S~1.3), slave **~80 % idle** à pot1/2, REC = 60–70 % de la frame. Échelle
HW ≈ 2–4× les chiffres Ymir (et Bw/P encore plus, memory-bound).

---

## C. Hardware — post-QW1/QW2 + L1/L2 — **À REMPLIR (la fenêtre « pas d'accès HW » est passée)**

> **RECONCILED 2026-06-24.** La note « pas d'accès HW avant ~2026-06-21 » est **datée
> dépassée** : QW2 (`SAT_POTATO_INLINE_SPANS=1`), L1 (devenu le toggle **runtime**
> `sat_visplane_hash`, core `ea452d8`, A/B pad-Y live) et le pool visplane
> (`SAT_VISPLANE_POOL=1`, `VP_POOL_PLANES=96`) sont **SHIPPED dans le build** (Makefile/core),
> blit recalibré ~5.5 ms. **QW1 et L2-SHRINK** (cmd buffer **80 KB** via
> `-DRP_CMD_BUF_SIZE=0x14000`) sont **SHIPPÉS** aussi ; L2-RELOCATE→HWRAM reste non codé.

### C.1 — Hardware E1M1 STRIPPÉ, build `b:19:25:29` (2026-06-25, 36 photos, Romain)

**Premier set HW post-QW/L1/L2.** Doom shareware **strippé** (`tools/strip_wad.py`) →
**cart-résident 4 MB, ZÉRO streaming**. 1 joueur : 4 pots (`pot0`/`pot0.5`/`pot1`/`pot2-fl`,
h1) × 6 spots E1M1. 2 joueurs : `pot0` × {`h0`,`h1`} × 6 spots. Lecture photo (angle/flou →
voir « à relire » plus bas).

**⚠️ Limite de méthode (la leçon #1 de ce set).** Les 6 spots ont des **vues différentes**,
et le profiler fenêtré (p50/p95/PK) **se reset au changement de pot** (config change) → chaque
photo agrège une fenêtre **différente en durée ET en contenu**. Donc **ni `inst` ni p50/p95/PK
ne sont comparables d'un pot à l'autre** ici (ils mélangent vue + pot). **Pour un A/B potato
propre : rester IMMOBILE à UN spot plane-heavy (cour/nukage E1M1) et cycler pad-Z** (la fenêtre
se reset au même endroit) — c'est le protocole à appliquer au prochain run.

**Findings HW SOLIDES (haute confiance, lisibles sur tout le set) :**

1. **MEM `rL ≈ 2.1` STABLE** (`lw≈1496`, `hw≈691` ticks, ~constant sur les 36 photos).
   → **C'EST le killer Ymir.** Ymir lisait `rL` 0.7↔1.2 (swap = bruit, §A.ter). Sur HW c'est
   **stable 2.1 = LWRAM 2.1× plus lente que HWRAM**. Confirme (a) la **loi de mesure** (Ymir ne
   modélise PAS le gap de banque), (b) l'**upside L2-RELOCATE→HWRAM** (déplacer cmd-buf +
   visplanes de LWRAM vers HWRAM = ~2× sur ces accès). **C'est la justification chiffrée de
   coder L2-RELOCATE.**
2. **`Dr` (VDP1 done-rate) — SÉMANTIQUE VÉRIFIÉE DANS LE CODE (2 erreurs corrigées, voir §C.2 D).**
   `dg_saturn.cxx:833`/`:2536` : `Dr = % de world-frames où VDP1 a FINI sa passe` (EDSR CEF) ; le
   commentaire est explicite « **'D'one = VDP1 had headroom, 'B'usy = it overran the frame** » et
   « high Dr => spare VDP1 budget ». Donc **`Dr` HAUT = headroom, `Dr` BAS = VDP1 déborde la frame.**
   Les murs sont des quads **texturés sur VDP1** (`vdp1_last_cmds`, ~142 au pot0). ⇒ **pot0 `Dr`
   ~25–42 % = VDP1 DÉBORDE déjà ~60–75 % des frames rien qu'avec les murs** = VDP1 **tendu au pot0**
   (≠ idle). pot2-fl `Dr` 82–91 % = murs **plats** (~33 cmds) → VDP1 finit. **Conséquence pour
   `VDP1_WORLD_PLAN`** : ajouter ~158 quads sol par-dessus des murs qui débordent déjà = VDP1 devient
   le **goulot unique** au pot0, sauf si (a) les quads sol sont très bon marché ET (b) retirer le
   span-fill soft libère assez de bus pour accélérer VDP1. ⇒ **le sol dominant a meilleur compte sur
   VDP2/RBG0** (3ᵉ unité, ne charge ni master ni VDP1). Le `<80 %` du plan §7.4 est un **plancher de
   confort** : pot0 est dessous.
3. **Magnitudes HW > Ymir** (loi confirmée) : PK `Bp` HW ~74–77 ms (Ymir ~54) ; PK `P` HW jusqu'à
   ~83 ms au pot0 (Ymir ~44). Memory-bound → les phases DRAM (Bw/P/Bp-loop) sont **plus chères sur
   HW**, comme prédit.
4. **`TEX nt125 w10776 d42K mp91` constant** sur tout le set → **le plancher columnlump ~42 KB est
   confirmé sur HW** (sanity check de cohérence des lectures OK).
5. **FLR `Vp` (pic candidat-quad sol VDP1) ≈ 150–170** (`d%` ~49–76 %, `n` ~16–34) — **À CONFIRMER
   relecture**. ⚠️ **CORRECTION** : ce n'est **PAS** un NO-GO. Le seuil `≤120` venait du plan
   **SUPERSEDED** `VDP1_FLOOR_PLAN.md` (sol partageant la banque mur). Le plan **autoritatif**
   `VDP1_WORLD_PLAN.md` (§0.2/§7.1/§7.3) dimensionne une **banque F dédiée (cap 248)** pile pour
   **`Vp ≈ 158`** → la lecture HW ~150–170 **TOMBE SUR LE POINT DE DESIGN du plan**, elle ne l'infirme
   pas. `d%` HW (~49–76 %) **confirme** le « un seul flat » (→ RBG0). Donc côté commandes le sol VDP1
   **tient** ; le vrai gate du plan est **`Dr%`** (finding #2), pas `Vp`.
6. **Anomalie : une photo montre PK `Bp ≈ 306` ms** (spot tech lumineux, 24 fps) = le **leak <300 ms
   qui survit au garde `RP_REC_SANE`** (déjà signalé §A.ter). → **durcir le profiler** (clamp <120 ms
   ou re-stamp FRT par phase) avant de se fier aux PK absolus.

**Table provisoire (inst fps lisibles ; le reste À RELIRE — ne pas traiter comme acquis) :**

| set | pot | h | inst fps (spots, lus) | PK Bp (lu) | PK P (lu) | Dr | rL |
|---|---|---|---|---|---|---|---|
| 1j | pot0    | 1 | 6.0 / 7.7 / 9.0 / 9.4 / 11.3 / 11.8 / 24.0* | 50–77 (*306 anomalie) | 66–84 | 26–41 % (B) | 2.1 |
| 1j | pot0.5  | 1 | _(à isoler — relire pot/spot)_ | ~77 | ~40 | _?_ | 2.1 |
| 1j | pot1    | 1 | 12.0 / 12.8 / 31.0* | ~75 | ~21–28 | ~80 % | 2.1 |
| 1j | pot2-fl | 1 | 14.3* | ~20 | ~25 | ~52 % | 2.1 |
| 2j | pot0    | 0 | _(à isoler par statut split + h0)_ | | | | 2.1 |
| 2j | pot0    | 1 | _(idem h1)_ | | | | 2.1 |

\* = assignation pot incertaine (lue via Bw/Bp/P/Dr, pas via la ligne `pot`). **Le verdict L1
(h0 vs h1 sur HW) et le coût split 2j attendent l'isolation propre des photos 2j.**

**Protocole** (Romain, hardware) : aux 6 spots de référence, lire rows 19/20/11/12/18/17
+ row 2 (pot, blit). A/B QW2 via `SAT_POTATO_INLINE_SPANS 0`. Attendus : QW1 → Bp↓ à
pot1/2 ; QW2 → P↓ + `c`↓ fort ; surveiller tout tear/garbage dans les sols (aucun attendu).

### C.2 — Hardware E1M1 STRIPPÉ, **protocole propre 2-spots immobile** (2026-06-25 PM, 18 photos, Romain)

**Le run que §C.1 demandait** : IMMOBILE à 2 spots, cycle pad-Z (pot) + pad-Y (h) sur place, la
fenêtre p50/p95 se reset au même endroit → **A/B enfin valides**. Spot A = **nukage** (intérieur
sombre, `sky2252`, `FLR Vs131 Vp156 d48% n20`, joueur 26/61/0). Spot B = **cour** (extérieur,
montagnes, `sky23813`, `FLR Vs17 Vp156 d95% n21`, joueur 76/83/198). Lecture haute confiance
(immobile = net).

**Échelle potato (avg fps / `P` ms = remplissage sol-plan, le bucket dominant) :**

| pot | A nukage avg / P / Dr | B cour avg / P / Dr | MST | SLVidle |
|---|---|---|---|---|
| **pot0**    | 7.5–7.9 / ~62 / 25–42 % | 8.3–9.0 / ~53 / 6–33 % | 119–131 ms | 29–33 % |
| **pot0.5**  | 8.1 / 50 / 37 %         | 8.9–9.1 / 41 / 40–43 % | 105–119 ms | 33–38 % |
| **pot1**    | 11.6 / 17 / 41 %        | 11.6–11.9 / 19 / 38–46 % | 78–79 ms | 54–55 % |
| **pot2-fl** | 13.4 / 11 / **84 %**    | 13.4 / 12–13 / **82–91 %** | 72 ms | 62–63 % |

**Findings (haute confiance) :**

- **A. `fps = 1000/MST` exact** (129ms→7.7, 72ms→13.8). **Tout est master-bound** : la frame EST le
  temps maître.
- **B. `P` (remplissage sol) = LE levier dominant.** ~60 ms d'une frame pot0 de ~130 ms ≈ **46–48 %**.
  pot0→pot1 (sols off) : `P` 60→17 ms (**−70 %**) → fps **+50–65 %** (7.6→11.6, 8.5→11.8). La gamme
  complète pot0→pot2-fl : avg **7.6→13.4 (+76 %, ~×1.8)**. **Valide le pari sol-offload** : les sols
  sont la moitié de la frame, content-stable aux 2 spots.
- **C. `h0` vs `h1` (L1 visplane-hash) = NUL.** Au pot0 nukage : h1 avg7.6/P61.7 vs h0 avg7.5–7.9/
  P61.3–62.0 — **identique**. Cour pot1 : h0 11.9/P18.9 vs h1 11.6/P18.9 — **identique**. À `n≈20`
  visplanes, `R_FindPlane` O(n²) ≈ 400 cmp/frame = négligeable vs ~130 ms. **L1 ne donne AUCUN gain
  mesurable aux comptes visplane d'E1M1** (vanilla Doom reste `n<40` ; l'O(n²) ne mord qu'à `n>100`,
  WADs custom). → **toggle mort.**
- **D. `Dr%` = VDP1 DONE-RATE (vérifié `dg_saturn.cxx:833`/`:2536`) — `Dr` HAUT = headroom, BAS =
  VDP1 déborde.** *(2 fois lu à l'envers avant de lire le code — corrigé.)* Les murs sont des quads
  **texturés VDP1** (`vdp1_last_cmds` ~142 au pot0), pas du soft. ⇒ **pot0 `Dr` 25–42 % = VDP1
  DÉBORDE la frame ~60–75 % du temps rien qu'avec les murs = tendu** ; pot1 38–46 % (sols hors-master
  → moins de contention bus → VDP1 finit un peu plus, même charge mur) ; `pot2-fl` **82–91 %** = murs
  plats (~33 cmds) → VDP1 finit large. **Implication plan** : VDP1 déborde DÉJÀ les murs au pot0 ;
  empiler ~158 quads sol texturés → VDP1 = goulot unique, sauf quads très bon marché + gain bus du
  span-fill retiré. C'est ÇA « léger en capacité restante » — et ça **favorise le sol dominant sur
  VDP2/RBG0** (n'ajoute rien au master NI au VDP1) ; VDP1-world ne reste que pour les **petits
  visplanes résiduels** que RBG0 (mono-matrice, 1 flat) ne peut pas servir.
- **E. `SLVidle ≥ 29 %` dans TOUS les modes** (29 % pot0 → 62 % pot2-fl). **L'esclave a toujours du
  slack ; le maître est partout le long pole.** ⇒ **tout ce qui n'accélère QUE l'esclave ne peut
  PAS monter le fps** — conséquence directe pour L2 (ci-dessous).
- **F. `rL = 2.1` stable** (`lw1497 hw692`, chaque photo, 2 spots) — re-confirmé.
- **G. blit `b ≈ 5.3–5.9 ms` stable** (~4 % de la frame) — non-goulot, **réglé** (Romain : toggles
  blit « plus utiles » → OK, geler).
- **H. QW2 (`SAT_POTATO_INLINE_SPANS`) A/B = NÉGLIGEABLE sur HW** (build QW2=0 MUS `b:00:46:28`,
  mêmes 2 spots, pot1+pot2 où QW2 agit). ΔP (off−on) : nukage pot1 **+0.7**, pot2-fl **+1.3** ; cour
  pot1 **−0.1**, pot2-fl **+0.8** → **~0–1.3 ms**, dans le bruit de vue ; `avg` fps inchangé. Direction
  correcte (inline memset un poil moins cher) mais magnitude négligeable. Sanity OK : **pot0 P identique
  on/off** (cour ~53 → byte-identité confirmée). ⇒ **comme L1, micro-opt qui ne rentabilise pas sa
  complexité à E1M1** ; QW2 reste build-overridable (`make SAT_POTATO_INLINE_SPANS=0`) — à garder
  (inoffensif, peut aider sur WAD visplane-lourd) ou retirer pour simplifier. **Bonus** : une frame pot0
  nukage lit **`w5.7`** (master en attente du slave à la barrière plane) = l'**imbalance du split sol**
  ciblée par [`CRITICAL_PATH.md`](CRITICAL_PATH.md) §3 RANK 1 (~3–8 ms récupérables en équilibrant par
  pixels — meilleur levier que QW2). Les autres frames sont `w0.0` (P petit = équilibré) → l'imbalance
  mord surtout au pot0 / en mouvement.

**Verdict L2-RELOCATE (cmd-buf + visplanes LWRAM→HWRAM) — NE PAS coder maintenant.** Le cmd-buf
(`RP_CMD_BUF_ADDR = 0x300000 − SIZE`, haut de LWRAM) est **la file de colonnes de l'ESCLAVE** (et
trafic explicitement bas en Mimas, murs→VDP1) ; or finding E : **l'esclave n'est jamais le goulot**
→ accélérer ses lectures = **0 fps**. Le pool visplane est une structure maître **minuscule**
(`n≈20`) ; le coût maître réel est le FILL `P` (écrit le framebuffer en **HWRAM = déjà rapide**,
lit les flats). ⇒ **`rL=2.1` est réel mais L2 déplacerait de la mémoire HORS du chemin critique
maître → gain fps prédit ≈ 0** à E1M1. La justification §C.1 finding #1 (« rL=2.1 justifie L2 »)
était **nécessaire mais incomplète** : il manquait E (esclave idle, master-bound). **L2 ne
redevient intéressant qu'en ENABLER du plan VDP1-world** (si le trafic cmd grossit quand les sols
passent VDP1) — à revoir avec le plan. *(Nail-it-shut optionnel : 1 build pointant `RP_CMD_BUF_ADDR`
+ `plane_pool` en HWRAM, mesurer MST au nukage → delta ≈ 0 attendu.)*

**Raffinements vérifiés dans le code (2026-06-26, workflow 4 lecteurs + 5 vérif. adverses) → voir
[`CRITICAL_PATH.md`](CRITICAL_PATH.md) :**
- **`P` n'est PAS « la moitié-master du sol »** : `P = p3_t_planes − p3_t_bsp` = wall-clock de TOUTE
  la phase plane **y compris l'attente master du slave** (`RP_WaitPlanes`, code : « P = master half +
  the wait for the slave half », `r_parallel.c:1483`). `w≈0` (slave équilibré) → le sol est **déjà
  splitté sur les 2 SH-2**, ~62 ms chacun ⇒ retirer les sols libère ~62 ms **master ET slave**.
- **Le master n'attend JAMAIS VDP1** (build shippé : `VDP1_MANUAL_CHANGE 0` → `FBCR=0x0000` 1-cycle
  auto, kick fire-and-forget). ⇒ **`Dr` bas = DÉCHIRURE de la couche murs, jamais un stall fps.** Donc
  sortir les sols vers VDP1 **encaisse bien le gain fps** (master perd le fill) ; le coût est **la
  qualité (tearing), pas le fps** — gradué par le `Dr` post-sol. (Corrige mon « present gated EDSR ».)
- **`Dr` confirmé** (3ᵉ lecture, verdict *confirmed*) : haut = headroom, bas = overrun. Chemin
  critique post-sol = **`Bp` wall-prep ~21 ms** (offloadable au slave, scaffold gated off) puis le
  BSP `Bw` ~7-8 ms (sériel, plancher dur).

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
