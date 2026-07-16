# Décharger le maître sur l'esclave — étude disruptive (2026-07-15)

> Workflow `wf_86743cf6-937` : 3 agents ground-truth + 7 pistes « 2ᵉ renderer » chacune
> pressurée puis tuée en adversarial (17 agents, 0 erreur). Ce doc = la synthèse + le prototype
> async-DIVU livré derrière un toggle. Voir aussi [LOWRES_RENDER_STUDY.md] (leviers 3/4p),
> mémoire `slave-second-renderer-bp-study`, `dual-sh2-span-steal-fafling`, `m7-lowres-fill-bound-not-34p`.

## 0. Le retournement (à lire en premier)

Deux corrections de prémisse qui décident de tout :

1. **L'esclave n'est PAS idle en 1p/2p.** Il tourne à plein sur le *plane-steal* TAS de Fafling
   (`sat_plane_tas=1`, `core/r_parallel.c:1026`) qui auto-équilibre le fill sol/plafond entre les
   2 SH-2 (w-wait ≈ 0). Il n'est ~97 % idle **qu'en 3p/4p**.

2. **En 3p/4p l'esclave est idle parce que Mimas a déjà gagné** — walls/sols/sprites sont partis
   sur VDP1/RBG0/VDP2. Cet offload est *exactement* ce qui affame l'esclave du travail
   *compute-bound cache-warm* (le fill) où un 2ᵉ SH-2 paye, ET collapse la projection qu'un
   « renderer esclave » voudrait faire. Le résidu maître = **BSP-walk + build commandes VDP1**,
   série et memory-bound : le pire cas pour un 2ᵉ SH-2.

**La parallélisation vraiment sous-exploitée n'est pas un 2ᵉ CPU — c'est le diviseur matériel
du maître lui-même (async DIVU).** Mono-CPU, zéro RAM, tous modes, jours de boulot. C'est le seul
levier disruptif qui survit à l'analyse. Prototype livré §5.

## 1. Les deux murs (chiffrés)

| Mur | Chiffre vérifié (this run) | Conséquence |
|-----|----------------------------|-------------|
| **K1 — RAM** | HWRAM libre = **exactement 5 584 octets** (`_end 0x060f8a30 → __heap_end 0x060fa000`, `build/Mimas.map`). Un 2ᵉ état de vue = ~100–117 KB de `.bss` (openings 40 960 B à lui seul). Le pool visplane est **déjà dieté à ~68 KB** (Makefile `-DMAXVISPLANES=256 -DVP_POOL_PLANES=96`, PAS les 332 KB vanilla). | ~18× au-dessus. Seul rab = la zone LWRAM (1 040 384 B) → mais uncached → déclenche K2. Cart 4 MB PAS libre (IWAD strippé = 4 174 732 B, ~19 KB de queue). Bare Saturn = pas de cart. |
| **K2 — memory-bound** | Esclave (LWRAM/cart, taxe bus ~2.1×) = **+5.8 ms plus LENT** que le maître au wall-prep (mesuré 3× HW). | Le 2ᵉ SH-2 ne paye QUE pour du **fill compute-bound cache-warm**. Il PERD sur toute *traversée/prep* memory-bound (BSP, seg-setup, sight). |

## 2. Les 7 pistes « 2ᵉ renderer » — toutes tombent

| # | Piste | Dodge K1 | Dodge K2 | Gain 3/4p | Verdict |
|---|-------|:---:|:---:|:---:|:---:|
| 1 | **Slave finalise les commandes VDP1** (maître ring compact → slave projette les quads) | ✅ | ❌ | <0.2 fps | **mort** |
| 2 | **Renderer minimal asymétrique** pour 1 quadrant sur le slave | ⚠️ shareware only, OOM gros WAD | ⚠️ « slack » s'inverse (même working-set → contention bus) | +1–1.4 fps *shareware* | **mort** (casse sur l'endgame) |
| 3 | **Slave fait le compositing** (bande HUD, minimap, blit) | ✅ | ⚠️ le seul morceau utile (blit 5.5 ms) est bus-bound, **mesuré pire** en dual-CPU | +0.05 fps (< bruit ±6 ms) | marginal → mort |
| 4 | **Cart 4 MB / re-budget RAM** pour rendre le split par-vue faisable | ❌ | ❌ **amplifié** : 2 vues @2.1× = 4.2T > 4T → **régression** | négatif | **mort** |
| 5 | **SCU-DSP en coproc T&L** (vertex transform) | ✅ (RAM privée on-chip) | ❌ offload le MAC cheap, garde la **division** série (DSP sans diviseur — et la division EST le goulot) | ~0, voire négatif | **mort** |
| 6 | **Précalcul slave pendant le tic** (SlaveDriver) | ⚠️ | ⚠️ seul le view-*indépendant* est partageable, et c'est cheap | +0.3–1 fps | marginal → mort |
| 7 | **Pipeline inter-vues** (slave BSP vue N+1) | ❌ (drawsegs pointent dans openings[]) | ❌ (BSP = memory-bound) | 0 à négatif | **mort** |

Les deux « moins mortes » :
- **#1 (slave finalise VDP1)** dodge K1 proprement (ring ~5 KB, pas de duplication). Elle tombe
  parce qu'en Mimas **la projection est déjà fusionnée en amont dans le walk memory-bound**, et
  les murs VDP1-affine l'ont réduite à ~2 endpoints/seg. Le morceau offloadable (`Bp` endpoint
  scale/y) fait ~1–2 ms ET c'est une **dépendance série du walk** (le clip du seg suivant en a
  besoin) → l'esclave ne peut jamais prendre de l'avance ; le cache-purge par dispatch (~1–2 ms)
  mange déjà le gain.
- **#2 (SlaveDriver asymétrique)** est l'unique forme « slave projette / master émet » jamais
  réfutée en principe (`saturn-refs/SlaveDriver-Engine/WALLS.C:1243-1299`) — mais mappée sur le
  vrai code Mimas, son payload a déjà été mangé par les murs-VDP1. Résidu ~1–2 ms.

## 3. Le levier qui SURVIT : diviseur async du maître (« finding 1 »)

Les 6 agents adverses convergent indépendamment vers la même redirection.

> Kicker le diviseur mappé mémoire (`DVSR 0xFFFFFF00 / DVDNTH 0xFFFFFF10 / DVDNTL 0xFFFFFF14`)
> **tôt**, faire ~39 cycles de setup pendant qu'il calcule en tâche de fond, lire le quotient
> **tard**.

- `FixedDiv` aujourd'hui = `DIV0U`/`DIV1` (32× `rotcl`+`div1`, `core/m_fixed.h:56`) → **bloque le
  pipeline** ~37 cycles, rien ne recouvre. Le diviseur *périphérique* (unité mappée mémoire), lui,
  tourne **en arrière-plan** pendant que la CPU exécute autre chose. C'est ça le gisement.
- Mono-CPU → **dodge K1 total** (zéro RAM, zéro 2ᵉ état, zéro handoff/cohérence).
- Latency-hiding → **dodge K2** (pas d'aller-retour bus).
- **Gain uniforme 1p/2p/3p/4p** — frappe les divisions de projection par-seg/par-sprite/par-plane
  (`R_ScaleFromGlobalAngle` `r_main.c:453`, `R_PointToDist`, sprite `xscale/iscale`
  `r_things.c:711,810`, `SlopeDiv`).
- **3 moteurs commerciaux Saturn convergent dessus** : Fafling (1/xx recouvert par la décompression
  texture, `divisions.s:23-111`), d32xr (`r_phase7.c:82-92`), SlaveDriver, SlideHop.
- Mimas ne l'exploite qu'à **un** endroit (ftex, `dg_saturn.cxx:4262 fxdiv` ; `SAT_WALL_RMUL` fait
  déjà le reciprocal-multiply de `wall_emit`). Les divisions **CORE en software** sont la surface
  vierge.

### Caveats HW (à valider — c'est le but du proto)
1. **Unité partagée dans un cœur.** Une interruption (VBlank/SGL) qui utilise le diviseur entre
   le kick et le read corrompt le résultat en vol → glitch d'échelle rare sur un mur. Fenêtre =
   quelques instructions ; le jeu commercial (Fafling) utilisait exactement ces registres.
   → surveiller de rares glitches de texture sur HW.
2. **Latence 39 cycles.** Si l'overlap entre kick et read < 39 cycles, soit le SH-2 stalle (safe,
   correct, gain = ce qu'on a recouvert), soit — selon le silicium/l'ému — la lecture précoce
   rend un quotient faux. `dv1` glitché ⇒ élargir la fenêtre (restructurer la seg-loop) = le
   follow-up.

## 4. Par mode — quoi faire

| Mode | État slave | Levier pour décharger le maître |
|------|-----------|----------------------------------|
| **1p** | occupé (TAS fill, +0.3–0.9 fps) | async DIVU (divisions de projection). Pas de « 2ᵉ renderer » (une vue). |
| **2p** | occupé (TAS + RBG0 HW) | async DIVU. Tout schéma slave *vole* le TAS intra-vue → négatif. |
| **3p** | ~97 % idle | async DIVU **+** couper la géométrie/commandes VDP1 maître (SQ-per-view, thingcull, budget VDP1 — déjà en shipping). Handoff sprite-scale→slave : +0–1 fps, haut risque, basse priorité. |
| **4p** | ~97 % idle | idem 3p, amplifié (140 ms/178 ms = les 4 vues). Lever = **cut master VDP1 geometry**, pas offload slave. |

## 5bis. ⚠️ RÉSULTAT HW (2026-07-15) — async-DIVU RÉFUTÉ

Captures 1p M7, même spot (`m1 3444,-3569 a88`), A/B propre dv0 vs dv1 :

| Mode | `Bp` (readout direct) | `P` | MST | fps |
|------|---:|---:|---:|---:|
| dv0 classique | **24.9** | 27.0 | 87 | 11.4 |
| dv1 async DIVU | **25.7** | 27.1 | 86 | 11.6 |
| dv2 ceiling (invalide) | 33.7 | 16.4 | 83 | 12.0 |

**dv1 ne bat pas dv0** — `Bp` va même dans le mauvais sens (24.9→25.7) ; MST/fps dans le bruit d'1
frame. Cause : `Bp` ~25 ms mais les divisions d'endpoint = 2-3/seg ≈ **~0.4 ms** ; `Bp` est dominé
par la boucle par-colonne (R_RenderSegLoop) + le setup memory-bound, pas par les divisions. Théorie
(~0.1 ms) et HW concordent. **Le port est fill/memory-bound, pas arithmetic-bound** → toute optim
arithmétique (async-DIVU, DSP-MAC) a un ROI structurellement nul ici. Ne pas pousser Prop 1/Prop 2.

`dv2` = probe **invalide** : `scale=FRACUNIT` dégénère la géométrie (P chute, Bp gonfle), ça ne
« supprime » pas la division. Le seul A/B propre est dv0 vs dv1. Bug annexe : le chord R+Up percutait
dg_saturn.cxx:6998 (rbg0_cell_nofb + reinit → **gris PERSISTANT** au switch, pas 1 frame). **Proto
REVERTÉ (arbre revenu au baseline) — étude conservée comme résultat négatif.**

## 5. Le prototype (ce qui avait été construit, maintenant REVERTÉ) — RÉFUTÉ HW, voir §5bis

Toggle **`sat_div_ab`** (core, défaut 0), cyclé live par **pad R+Up** :

| `dv` | Mode | Effet |
|------|------|-------|
| `dv0` | classique `FixedDiv` (DIV0U/DIV1) | défaut, byte-identique à aujourd'hui / DoomJo |
| `dv1` | **async peripheral DIVU** | les 2 divisions d'endpoint de `R_StoreWallRange` via `R_ScaleFromGlobalAngle2` : kick div-1, calcul des opérandes de div-2 (overlap), read div-1, kick div-2, read div-2 |
| `dv2` | **CEILING probe** | saute les divisions d'endpoint (murs dégénérés, timing-only) → borne supérieure absolue du gain possible |

**Fichiers touchés** (tous `// SATURN:`, DoomJo-safe = chemin `dv0` inchangé) :
- `core/r_parallel.c` : `int sat_div_ab = 0;` + token `dv%d` sur la row-2 overlay (à côté du `Bp`).
- `core/r_main.c` : primitives `sat_fdiv_kick`/`sat_fdiv_read` (registres 0xFFFFFF00) + le twin
  fusionné `R_ScaleFromGlobalAngle2` (sémantique == 2× `R_ScaleFromGlobalAngle`, même guard/clamp).
- `core/r_main.h` : prototype de `R_ScaleFromGlobalAngle2`.
- `core/r_segs.c` : gate des 2 appels d'endpoint sur `sat_div_ab`.
- `src/dg_saturn.cxx` : chord **R+Up** cycle `sat_div_ab` 0→1→2.

**Comment lire l'A/B (HW, même spot) :** overlay mode 0, **row 2** = `Bw Bp P M dv<n>`. `Bp` = le
temps `R_StoreWallRange` (contient les divisions d'endpoint). Séquence : R+Up → `dv0` note `Bp`,
`dv1` note `Bp` (le gain async), `dv2` note `Bp` (le plancher). Si `dv2` ≈ `dv0` → les divisions
d'endpoint ne pèsent rien ici, l'async ne peut pas aider → cibler les divisions plus denses
(projection sprite `R_ProjectSprite`, `R_PointToDist`). Si `dv1` glitche visuellement → caveat §3.2
(élargir la fenêtre d'overlap).

## 6. Expériences pas chères pour trancher les autres pistes (méthode soustractive)

Pour chaque piste morte, la borne supérieure = **supprimer complètement le travail** (gratuit ET
latence nulle, mieux que tout offload) et mesurer le plafond fps. Si supprimer ne bouge pas au-delà
du bruit ±6 ms, aucun offload ne peut aider.
- **#5 DSP** : toggle qui skippe la rotation 2×2 → si la ms/vue ne bouge pas, le DSP (qui ne peut
  battre « gratuit ») est mort avant la 1ʳᵉ ligne d'assembleur.
- **#7 pipeline** : déjà mesurable via `sat_wallprep_slave` (RANK3) — si `Bp` slave ≥ `Bp` maître,
  l'inc-2 overlap ne paye pas.
- **#3 compositing** : `continue` past `AM_DrawMiniMap` + band memcpy → borne du gain de bande/minimap.
