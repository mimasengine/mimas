# Pistes Ponut64 → Mimas — dossiers détaillés

**Date** : 2026-08-29. Suite de [PONUT64_ENGINE_ANALYSIS.md](PONUT64_ENGINE_ANALYSIS.md) §9,
dont les 9 pistes STEAL/INVESTIGATE ont été instruites une par une (9 agents, lecture du code
Ponut64 + du code Mimas as-built + base `saturn-refs/knowledge`). Règles appliquées : chaque
claim sur Mimas porte un fichier:ligne ; l'arithmétique de budget précède le mécanisme ; le
plan d'expérience est à la plus petite portée (toggle vif + sonde + critère GO chiffré) ; les
affirmations non prouvées sont listées.

## Résultat d'ensemble

**Quatre pistes reposaient sur une prémisse FAUSSE de l'analyse du 2026-08-29 (corrigée
depuis)** : le light diminishing VDP1 est déjà shippé (7 banques CRAM exactes) ; les sols VDP1
sont déjà texturés (et parkés) ; les percussions sont jetées par notre séquenceur MUS, pas par
un driver ; les lumps natifs ont déjà été bakés, mesurés et rejetés. Une cinquième
(DIVU entrelacé) avait déjà été construite et réfutée sur console. Le dépôt Mimas est en
avance sur le moteur de Ponut64 sur presque tous les mécanismes qu'on voulait lui « voler » ;
ce qui reste réellement à prendre tient en deux expériences de qualité visuelle à coût nul
(MSB shadow, fade des things) et une sonde de comptage (LUT/DIVU).

### Tableau de synthèse — ce que chaque piste APPORTE à Mimas

| # | Piste | Apport | Nature du gain | Chiffre | Coût | Verdict |
|---|---|---|---|---|---|---|
| 1 | MSB shadow (×2 des banques CRAM) | **Qualité** | seam mur-VDP1/software plus fin (moitié sombre) | ±9 % → ±6 % de luminance ; 7 → 11-12 crans | 0 CRAM, 0 CPU, ~60 lignes | **CONDITIONNEL** (sonde SPCTL + toggle console) |
| 2 | Fade-out things (bits CC + prio) | **Qualité + feature** | pop → fondu ; haze de distance sur things VDP1 | 100 %→0 en 1 frame devient ~25 %→0 | 0 VDP1, ~8 o, ~40 lignes | **CONDITIONNEL** (type 3 confirmé + veil survit CC_TOP) |
| 3 | Pre-clipping bit 11 (+ rotation v0) | **Perf VDP1 plot** | détection seulement, pas le transfer-over | 0,2-1,2 ms VDP1 contre **+0,1-0,16 ms MASTER** | ~20 lignes | **CONDITIONNEL** étape 1 seule ; rotation v0 NO-GO |
| 4 | DIVU entrelacé | **Perf MSH2** | < 1,2 ms (< 2 %) | déjà réfuté console (dv1, 07-15) | restructuration chemins chauds | **NO-GO** (sauf sonde ≥ 2 ms) |
| 5 | ponèSound | **Qualité audio** visée | **0** sur les percussions ; −53 Ko de RAM samples | cause = `mus_step` ch15, pas un driver | port + requalif HW | **NO-GO** → Route P (drums, slots 23-31) |
| 6 | Pyramide uv_cut sols VDP1 | **Perf VDP1 plot** | sur un mode PARKÉ ; sols déjà texturés | ≤ 1,5-2 ms de temps VDP1 idle | ~150 lignes + 1,3 Ko VRAM/slot | **NO-GO** |
| 7 | Fenêtre d'effacement | **Perf VDP1 plot** | 0 ms (A) ; 0,36-0,89 ms (B) | marge plot 7-8×, g0 en split | 2 halfwords / +1 slot | **NO-GO valeur** |
| 8 | LUT réciproque étroite | **Perf MSH2** | 0,8-5 ms selon C (non mesuré) | concurrent DIVU 39 c à 0 RAM | 8 Ko .bss + init | **CONDITIONNEL** (compter C d'abord) |
| 9 | Lumps Saturn-natifs | **Temps de chargement** | déjà fait/rejeté ; résiduel SIDEDEFS | −100-200 ms/warp MAP15 | lecteur MIMASLVL + checksum TEXTURE1/2 | **NO-GO** (sauf bracket ≥ 200 ms CD) |

**Déport MSH2** : aucune piste ne déplace du travail du master vers un autre processeur. Les
deux pistes de qualité (1, 2) confient le travail au pipeline pixel du VDP2 (coût CPU nul par
construction) ; les pistes 4 et 8 réduisent des cycles master mais sous le bruit ou sous
condition ; la piste 3 en AJOUTE au master.

**Ce qu'il faut faire ensuite, dans l'ordre** : (a) la sonde SPCTL/SDCTL (3 lignes, commune
aux pistes 1 et 2 — le type sprite réel n'a jamais été lu) ; (b) les compteurs `dv`/`dm` de la
piste 8 (2 lignes) ; (c) si type 3 confirmé : l'expérience fade-things 1p (~40 lignes, toggle
vif), puis MSB shadow ; (d) Route P percussions (le seul chantier audio qui traite la cause).

**Découvertes annexes des agents** : `cd/data/DOOM1.WAD` est actuellement un WAD Doom II/TNT
(107 lumps DS, DSBOSSIT en tête) — piège `build-stale-stash-wad-swap` ; toute mesure « shareware »
faite dessus est fausse. `build/Mimas.map` (26-08) donne un pool TLSF de 66 944 o contre
7 552 o dans RESOURCE_BUDGETS (08-20) — écart non réconcilié, pré-vol obligatoire avant toute
promesse mémoire. Le « doublon d'erase plein écran » soulevé par le dossier 3 est **réfuté à la
lecture (2026-08-29 soir)** : le second polygone (dg_saturn.cxx:8298-8315) est sous
`#if VDP1_MANUAL_CHANGE`, dont la valeur est **0** (:135, parké depuis le 2026-07-02) — il n'est
pas compilé ; un seul polygone couleur-0 par plot, comme le dossier 7 l'établit. Reste une
incohérence de COMMENTAIRE, pas de code : « ~10x deficit » de l'erase VBE (:8282) contre 1,22×
dans HW_VDP1.md:485-487.

---

## 1. Light diminishing VDP1 par banques CRAM + MSB shadow

**Apport pour Mimas — QUALITÉ VISUELLE (mineure), perf 0, déport MSH2 0, RAM 0.**
Prémisse d'hier FAUSSE : le light diminishing VDP1 est déjà shippé (bank 1 vive + 6 banques
colormap exactes, sélection `wall_light_colr` ~5 instructions, sur murs + things + sols VDP1).
Ce qui reste à prendre chez Ponut = le doublement par MSB shadow : 7 → 11-12 crans, tous
dans la moitié sombre, erreur de luminance au seam mur-VDP1/colonne-software ±9 % → ±6 %.
Ne traite PAS l'autre moitié du mismatch (quad uniformément éclairé vs gradient par colonne).
Coût : 0 octet CRAM, 0 ms CPU, 2 registres, ~60 lignes, 1 session console.

**Verdict** : CONDITIONNEL: la piste principale est DÉJÀ shippée (7 banques CRAM exactes sur murs+things+sols VDP1 — corriger PONUT64_ENGINE_ANALYSIS.md §3/§9-1) ; le delta MSB ×2 ne vaut qu'une session probe+toggle et n'est GO que si (1) la sonde SPCTL montre SPWINEN=0 et un type à bit SD atteignable, et (2) le toggle console montre les murs à demi-luminance sans disparition ni coût fps — sinon NO-GO, gain plafonné à ~3 crans sombres.

### 1. Mécanisme chez Ponut64

`determine_colorbank` (ponut64-stuff/render.c:598-621) : 8 seuils de luma → un mot `colorBank` = index de banque (0-3) éventuellement | 0x200 (= bit 15 après le `<<6` de render.c:898) ; injecté tel quel dans CMDCOLR (`colorBank | cue`, render.c:912). Décodage VDP2 **sprite type 4** : bits 6-7 = 1 des 4 copies CRAM pré-assombries (140/120/100/80 %, peintes par l'artiste), bit 15 = MSB shadow = demi-luminance (« 70/60/50/40 % », commentaire render.c:607-612) → 8 niveaux, zéro coût/pixel. Recoupé dans HW_VDP2.md:446-458.

### 2. État as-built Mimas — la prémisse « zéro diminishing » est FAUSSE

**Le mécanisme central est déjà shippé.** docs/PONUT64_ENGINE_ANALYSIS.md:119-120 et §9-1 (« nos murs/things VDP1 n'ont aucun light diminishing ») contredisent le code — à corriger.

- **CRAM** (mode 2048 entrées, nécessairement CRMD=1 : les banques >3 fonctionnent sur HW ; CRMD hérité de l'init SGL, préservé par dg_saturn.cxx:4725) : bank 0 idx 16-31 = palette ciel cell (:540) ; **bank 1** = PLAYPAL vive (:270), partagée NBG1 + RBG0 (BMPNB bank 1, :4319) + VDP1 full-bright ; **banks 2-7** = 6 copies PLAYPAL passées par les colormaps niveaux {5,10,16,21,26,31} (:6035, :1199). CRAM **pleine** — zéro banque libre.
- **Exactitude** : `wtex_rebuild_banks` (:6081-6098) construit chaque banque via `colormaps + level*256` → le **remap Doom exact** (pas une atténuation linéaire), reconstruit sur flash palette et uploadé au vblank (:1705-1714). La crainte (c) du brief est déjà résolue.
- **Sélection** : `wall_light_colr` (:6069-6075) = LUT niveau-colormap→banque (`wlight_bank_lut`, :6051-6061), ~5 instructions. Consommée par : murs (:6730, cmd[3]=:6515/6599/6624), murs flat/potato (:6860), **things** (:8656-8657, :8688-8689, flush :8755), **sols VDP1 inc-2** (:7317-7322). Arme = ombre baked dans les texels + bank 1 (:8404, WPN_CMDCOLR=0x2100, :5747).
- **La lumière est déjà sous la main à l'émission** (question e) : murs = `wall_acc[i].cmap` (:6241) choisi distance-correct par tier dans core/r_segs.c:2041-2045 (`walllights[rw_scale>>LIGHTSCALESHIFT]`) ; things = paramètre `cmap` (:8598-8600) ; sols = `zlight[li][zi]` (:7301-7317). Coût : déjà payé.
- **Format des commandes** : DISTORSP 0x0002, CMDPMOD 0x00E0 (murs, :6941-6945) / 0x04A0 (things, :8685) = color bank 256 couleurs 8bpp ; CMDCOLR = bank<<8 (murs, bits 15-13 CLEAR = registre prio 0, z-invariant :6066-6068) ou 0x2000|bank<<8 (things/arme, bit 13 → registre ≥1, au-dessus de NBG1, :5333-5335).
- **Sprite type (question d)** : SPCTL jamais écrit par Mimas (grep vide) — il vient du shadow SGL, block-flushé 0x0E-0xFE (:4766-4769, SPCTL@0xE0, SDCTL@0xE2 inclus). Déduction par comportement HW : DC=11 bits (banques 2-7 résolues) + bit 13 → registre ≠0 (arme au-dessus de NBG1) ⇒ **type ∈ {1, 3, 5}** (table HW_VDP2.md:358-367). Type 1 : pas de bit SD → MSB shadow indisponible ; types 3/5 : SD=bit 15, disponible immédiatement. Aucune commande ne pose le bit 15 aujourd'hui (vérifié sur tous les sites cmd[3]) → un switch de type est neutre pour l'existant.
- **Restes de mismatch visuels documentés dans le code** (l'« argument massue » réel) : le **snap 7 banques** (:7308 « hardware-bound ») vs 34 niveaux software, et le **quad uniformément éclairé** là où le software a un gradient par colonne (:7308-7309, r_segs.c:2041 un seul cm par tier). `fixedcolormap` (invuln/light-amp) : sols VDP1 coupés (:7471), things bakés dans les texels (:8600, :8644) — correct.

### 3. Arithmétique de budget (la soustraction d'abord)

**Écart actuel** : ladder VDP1 = {0,5,10,16,21,26,31} ; erreur de quantization max = 3 niveaux (à L=13), ≈ ±9 % de luminance au seam mur-VDP1/colonne-software ; pire trou = 6 niveaux (10→16).
**MSB ×2** (demi-luminance = L'=16+L/2 si luminance≈(32-L)/32) : copies {16, 18.5, 21, 24, 26.5, 29, 31.5} → seulement **~3 crans nouveaux** {18.5, 24, 29} sans re-répartition (7→10). Avec ladder re-réparti {0,4,8,12,16,22,31}+moitiés → **11-12 crans**, trou max 4, erreur max 2 niveaux (~±6 %). **Gain net ≈ 3 % d'erreur de luminance au pire cas, uniquement dans la moitié sombre.** Le gradient intra-quad (l'autre moitié du mismatch) n'est PAS traité par cette piste.
**Coûts** : CRAM +0 octet (bit 15 dans le mot existant) ; CPU +0 ms (la LUT :6036 passe en u16 bank|MSB, même lookup) ; VRAM +0 ; registres = 2 écritures one-shot (SPTYPE nibble, SDCTL) ; code ≈ 3 lignes (sonde) + ~25 (toggle) + ~30 (ladder) ; 1 session console.

### 4. Obstacles et risques

1. **Sémantique MSB** : le manuel (HW_VDP2.md:416-419) décrit l'ombre comme « sprite transparent + fond assombri » ; la lecture « texel lui-même à demi-luminance quand dot≠0 » repose sur Ponut (moteur qui tourne) — si fausse, les murs MSB disparaissent (échec visible, inoffensif sous toggle).
2. **Code réservé normal-shadow 0x7FE** (HW_VDP2.md:432-441) : **latent DÈS AUJOURD'HUI** — texel idx 254 via bank 7 = dot 0x7FE = pixel non affiché (imperceptible : quasi-noir au niveau 31). Shadow activé (SDCTL), il deviendrait un trou demi-luminance sur ciel/RBG0 → remapper 254→253 au bake, ou sortir bank 7 du ladder MSB.
3. **MSB incompatible sprite window** (HW_VDP2.md:442) — Mimas utilise les fenêtres normales W0/W1, pas la sprite window ; la sonde SPCTL (bit SPWINEN) le confirmera.
4. **Quatre horloges** : SPCTL/SDCTL = classe registre → poke chip + shadow SGL (motif RAMCTL :4726-4727) sinon re-push ISR.
5. Si type 5 : CC réduit à 1 bit (2 ratios) — hypothèque la piste-5 fog ; **préférer type 3** (bit13→reg 1 littéralement conservé, CC 2 bits).
6. `fixedcolormap` reste hors CRAM (:7471) — inchangé.

### 5. Plan d'expérience minimal

- **Étape 0 (3 lignes, build courant)** : dans `rbg0_commit_cyc`, `printf` de `shadow[0xE0]` et `shadow[0xE2]` (:4766) → SPTYPE réel, SPWINEN, SDCTL. Lisible sur Ymir (valeur de registre, pas de la perf).
- **Étape 1 (toggle vif ~25 lignes)** : chord libre (vérifier le registre des 18 toggles vivants) → écrit SPCTL=(shadow&~0xF)|3 + SDCTL enable (chip+shadow), et force `colr |= 0x8000` sur TOUS les murs. Sonde : compteur de commandes MSB sur un champ overlay existant (légende mise à jour même session).
- **GO chiffré (console, même build, toggle vif — règle interbuild)** : (i) murs à demi-luminance visibles, zéro disparition ; (ii) prio intactes (arme au-dessus, murs sous NBG1) ; (iii) fps identique ±bruit ; (iv) pas d'artefact 0x7FE visible. Trois échecs possibles = NO-GO définitif documenté dans HW_VDP2.md.
- **Étape 2 (si GO, ~30 lignes)** : `wlight_bank_lut` u16 34 entrées, ladder {0,4,8,12,16,22,31}+MSB, remap 254 au bake, photo A/B du seam.

### 6. Non vérifié

- Valeur réelle de SPCTL/SDCTL (shadow SGL jamais lu) ; type ∈ {1,3,5} est une déduction comportementale, pas une lecture.
- Sémantique MSB « demi-luminance du texel non transparent » (manuel p.258 non relu ; corroboration = Ponut + HW_VDP2.md:442-451) ; idem le rôle exact de SDCTL pour ce self-shadow.
- CRMD=mode 1 : déduit (entrées >1023 fonctionnelles HW), init SGL non lue.
- Propriété complète de la bank 0 (idx 0-15 et 32-255) non tracée.
- L'équivalence « niveau colormap ≈ luminance linéaire (32-L)/32 » de l'arithmétique des crans (les colormaps remappent).
- Les priorités slPrioritySpr effectives par mode (branches #if :5358-5380) citées nominales, non tracées par mode.

### Non vérifié (liste structurée)
- Valeur réelle de SPCTL/SDCTL — jamais écrites par Mimas, héritées du shadow SGL ; le type sprite ∈ {1,3,5} est déduit du comportement HW (banques 2-7 résolues + bit 13 → registre ≥1), pas lu
- Sémantique MSB shadow 'texel affiché à demi-luminance quand dot≠0' — manuel VDP2 p.258 non relu ici ; corroborée seulement par le moteur Ponut64 qui tourne et HW_VDP2.md:442-451
- Rôle exact de SDCTL pour le self-shadow MSB (enable requis ou non) et effet du code réservé 0x7FE une fois le shadow activé
- CRAM en mode CRMD=1 — déduit du fait que les entrées >1023 fonctionnent sur HW, init SGL non lue
- Propriété complète de la bank 0 CRAM (idx 0-15 et 32-255)
- L'équivalence luminance≈(32-L)/32 utilisée pour chiffrer les crans MSB (les colormaps Doom remappent, non-linéaire)
- Valeurs effectives des 8 slPrioritySpr par mode d'affichage (branches #if dg_saturn.cxx:5358-5380)

---

## 2. Fade-out matériel des things lointains (bits CC + priorité)

**Apport pour Mimas — QUALITÉ (le pop des things → fondu) + FEATURE (haze de distance
gratuit sur les things VDP1), perf 0, déport MSH2 0 (le mélange est fait par le pipeline
pixel du VDP2), RAM ~8 o.** Levier VALEUR, pas ms : le sprite à la frontière de drop passe
de 100 %→0 en une frame à ~25 %→0. 2 bits CC libres dans le CMDCOLR des things (type 3 par
défaut SGL), 4 registres de ratio. Conflit réel mais borné : flip CC_2ND→CC_TOP obligatoire
(CCRTMD global) → le veil RBG0 (parké, pad C) doit être re-validé avec son ratio déplacé.

**Verdict** : CONDITIONNEL: GO pour l'expérience 1p (~40 lignes, coût ~0) SI (1) la lecture du shadow SPCTL à l'init confirme le sprite type 3, ET (2) le veil pad-C survit visuellement au flip CC_2ND→CC_TOP avec son ratio déplacé sur slColRateRbg0 — sinon NO-GO tant que le sort du veil (WIP parké) n'est pas arbitré, car CCRTMD est global et la rampe per-thing exige CC_TOP.

### 1. Mécanisme chez Ponut64

- `render.c:623-630` `depth_cueing()` : `cue = clamp((z-200·2^16)>>23, 0..7)`, `|8` si `z>1200·2^16`, `<<10`. Résultat OR-é dans le mot CMDCOLR (`colorBank | cue`, render.c:912).
- `vdp2.c:124` : `vdp2_sprMode[0]=0x0404` réécrit chaque vblank → **sprite type 4** (SD 15, PR 14-13, CC 12-10, DC 10 bits) + condition CC `SPCCN=4`.
- `vdp2.c:173-193` : `slSpriteCCalcCond(CC_pr_CN)` (priorité ≤ n), `slColorCalcOn(CC_RATE|CC_TOP|SPRON|BACKON)`, `slPrioritySpr0(5)`/`Spr1..7(3)`, `slColRateSpr0..7(2..28)`. Le bit 13 (cue|8) bascule le pixel sur SPR1=3 ⇒ condition satisfaite ⇒ CC armé ; les bits 12-10 choisissent 1 des 8 ratios ⇒ le polygone lointain se fond dans la couche dessous. Piège vérifié : la rampe sature à z=1096 avant le gate z>1200 — **gate et rampe doivent se recouvrir** (docs/PONUT64_ENGINE_ANALYSIS.md:111-115).

### 2. État as-built Mimas

- **Sprite type : jamais écrit.** Aucun `slSpriteType`/poke SPCTL dans le dépôt (grep src/ = 0 hit). Le SPCTL chip = shadow SGL poussé par le block-flush 0x0E..0xFE (`rbg0_commit_cyc`, dg_saturn.cxx:4766-4769, 0xE0 inclus). Le boot SGL = **type 3** (SD 15, PR 14-13, **CC 12-11**, DC 11 bits) d'après l'analyse Tethys (saturn-refs/manuals/KRONOS_ERRATA_IMPACT.md:77, 363) — même SGL. Types à 3 bits CC = 0/2/4/7 seulement (HW_VDP2.md:358-371).
- **Priorités** : `slPrioritySpr0(5)` (murs), `Spr1..7 = SAT_SPR_HI_PRIO = 6` (dg_saturn.cxx:5358-5364) ; NBG1=6, NBG0=4 (ou 3 avec RBG0), NBG3 debug=7 (5294-5303). Things/arme : `WPN_CMDCOLR = 0x2000|0x0100` (5747) — bit 13 = PR0 → registre 1 = prio 6.
- **CMDCOLR things** : `(WPN_CMDCOLR & 0xE000) | wall_light_colr(cmap)` aux trois sites d'émission — queue split 8656-8657, direct 1p 8688-8689, drain 8755. `wall_light_colr` = banque CRAM 1..7 `<<8` (6069-6074, LUT remplie ≤ 7 à 6054-6060) ⇒ **les bits 12-11 (CC du type 3) sont toujours à 0 aujourd'hui** — 2 bits libres, 4 registres de ratio (registres 0-3, Table 9.3, HW_VDP2.md:399-402).
- **Color-calc actuel** : `rbg0_linecol_apply` (init, appelé à 4636) pose `slColorCalc(CC_RATE|CC_2ND|RBG0ON)` + `slColorCalcOn(RBG0ON)` (4602-4603) — sprites **hors** CC (pas de SPRON). `CC_2ND=0x200` (sl_def.h:1231) = CCRTMD : ratio pris sur la 2e image (le veil lit `slColRateLNCL`, 4587-4592). Fenêtre CC par ligne armée à l'init : `slScrWindowMode(scnCCAL, win0_IN)` + table LWTA (4565-4609) — CC OFF sous la frontière `bd`.
- **Le veil est PARKÉ** : `rbg0_linecol_mode = 0` par défaut (2054), profondeur au pad C.
- **Drop far-first split** : `vdp1_things_flush` (8713) — `avail = (vdp1_wall_cap - MARGIN - vdp1_wnext)>>1` (8725), water-fill par vue, `skip[v]` = les plus lointains coupés (8727-8742) ⇒ sprite **disparaît** (8710-8711). En 1p la coupe = top-emax de PASS 1 (r_things.c:1902-1961, `sat_thing_emit_cap` AIMD à 1714) mais le perdant reste **software** (dessiné) — le pop dur est surtout un artefact split.
- Hook : `sat_thing_hook(patch, lump, cmap, xlat, x0..cy1, flip)` (r_things.c:141-145) — **pas de paramètre distance** ; `spr->scale` est disponible au site d'appel (r_things.c:2082).

### 3. Arithmétique de budget

- **Bits** : 2 bits CC libres (12-11) ; 0 bit CRAM perdu (DC 11 bits intacts, banques ≤ 7 = bits 8-10) ; pas de collision avec la piste banques CRAM ni avec le code shadow 0x7FE (SD/PR/CC indifférents à la reconnaissance, HW_VDP2.md:427-431).
- **Coût** : 0 commande VDP1, 0 octet VRAM/CRAM en plus ; CPU = classer ≤32 things/frame sur 2-3 seuils ≈ 0,01 ms ; ~8 appels SGL à l'init + 3 `slColRateSpr` au toggle (écritures shadow, prouvées runtime-safe par le pad C, 4583-4592) ; pool ~8 octets ; +1 paramètre au hook (core+platform ; casse DoomJo = informatif, gelé).
- **Gain** : levier VALEUR, pas ms. Le sprite à la frontière de drop passe de 100 %→0 en 1 frame à ~25 %→0 (registre 3 ≈ rate 24/31) ; bonus : haze de distance gratuit sur les things VDP1 (qui n'ont que 7 banques CRAM de light). Blend VDP2 = pipeline pixel, 0 cycle VRAM ajouté.

### 4. Obstacles et risques

1. **CCRTMD global (le conflit central)** : la sélection per-pixel des registres de ratio n'opère qu'avec `CC_TOP` (ratio de l'image du dessus). Le fog est câblé `CC_2ND` — mais il est **parké** (mode 0). Flip requis `CC_2ND→CC_TOP` + déplacer le ratio du veil de `slColRateLNCL` vers `slColRateRbg0` (CCRR) ; la recette ReyeMe n'est validée qu'en CC_2ND → re-valider le veil au pad C. Sans flip, le ratio sprite viendrait du registre NBG1, uniforme — la rampe meurt.
2. **Fenêtre CC** (scnCCAL, armée inconditionnellement à l'init, 4608-4609) : coupe le CC sous `bd` — un thing bas d'écran ne fadera pas là. Pour l'expérience : table pleine-hauteur INSIDE quand le fade est ON et le veil OFF.
3. **Discrimination par condition de priorité** : murs=5, things+arme=6. Condition `==6` (`CC_PR_CN`, sl_def.h:1248) exclut les murs ; l'arme (bits CC=0 → registre 0, rate 0) reste intacte (résidu ≤1/32 possible).
4. **Piège Ponut transposé** : la frontière de drop est ADAPTATIVE (AIMD/avail), pas une distance fixe ⇒ ancrer la rampe sur le **rang** (les 2-3 derniers gardés de `em_idx[]` r_things.c:1952-1960 ; en split les entrées adjacentes à `skip[v]` 8742), jamais sur un seuil de distance absolu.
5. Le fondu va vers NBG1 (la scène derrière le sprite) = un vrai fondu ; dans les trous index-0, vers ciel/RBG0 — cohérent aussi.

### 5. Plan d'expérience minimal (1p d'abord)

- **Étape 0** : à l'init, `dbg_print` du mot shadow SPCTL (`&VDP2_RAMCTL - 0x0E + 0xE0`) — le chip est write-only. Attendu : type 3. Sinon STOP (toute la carte des bits bouge).
- **Étape 1 (~40 lignes)** : init = `slSpriteCCalcCond(CC_PR_CN)` + `slSpriteCCalcNum(6)` + `slColorCalcOn(RBG0ON|SPRON)` + `slColorCalc(CC_RATE|CC_TOP|RBG0ON)` + `slColRateSpr0(0)` ; veil : `slColRateRbg0` remplace `slColRateLNCL`. Toggle **vif** (int runtime, chord à choisir contre l'audit toggles — 18 vivants/0 collision) : ON pose `slColRateSpr1/2/3(8/16/24)`, OFF les remet à 0 — A/B même build (règle interbuild-perf-noise). Tag : PASS 1 marque les 3 derniers gardés (niveaux 3/2/1), +1 param au hook, OR `niveau<<11` dans cmd[3] à 8688.
- **Sonde** : compteur fenêtre `cf` (things émis avec CC≠0) sur la ligne THp existante (greper la ligne dans les 3 fichiers avant, + légende même session).
- **GO chiffré** : (a) SPCTL type 3 confirmé ; (b) toggle ON : arme+murs byte-identiques visuellement (paint L+X), fps/MST delta = 0 ±bruit sur le toggle vif ; (c) capture console : les 3 things frontière visiblement translucides, zéro neige/flicker ; (d) pad C : veil intact sous CC_TOP. Puis phase 2 split : mêmes bits aux entrées frontière du drain (8727-8742), critère = sur les captures THp x>0, ≥80 % des sprites droppés portaient CC≥2 (le pop part d'une opacité ≤50 %).

### 6. Non vérifié

Voir liste structurée.

### Non vérifié (liste structurée)
- Le sprite type 3 comme défaut de boot SGL : établi par l'analyse Tethys (KRONOS_ERRATA_IMPACT.md:77,363), même SGL, mais le SPCTL effectif de Mimas n'a jamais été lu (registre write-only) — d'où l'étape 0 (lecture du shadow SGL).
- La vblank IRQ SGL pousse CCRSA/CCRSB (0x100/0x102, hors block-flush 0x0E..0xFE) comme elle pousse CCRR — inféré du comportement observé sur CCRR (dg_saturn.cxx:4583-4586), pas prouvé pour les registres sprite.
- La fenêtre color-calc (scnCCAL/win0_IN) gate aussi les pixels SPRITE — sémantique WCTLD non relue dans le manuel VDP2.
- Rate 0 = top exactement opaque, ou résidu 1/32 de la 2e image — la table exacte des ratios (p.206) n'a pas été relue ; l'impact visuel est ≤3 % sur arme/things proches.
- En tie de priorité 6-vs-6 (sprite vs NBG1), la 2e image du calcul de couleur est bien NBG1 — inféré de l'ordre Table 11.1 (résolution d'affichage), pas vérifié pour la détermination du 2nd screen CC.
- Le veil ligne-couleur fonctionne sous CC_TOP avec son ratio dans CCRR (slColRateRbg0) — la recette ReyeMe/ScaryGame n'est validée qu'en CC_2ND (slColRateLNCL).
- slColorCalcOn(SPRON) positionne bien SPCCEN (bit 6 de CCCTL) — comportement SGL non vérifié (binaire, pas de source).

---

## 3. Bit pre-clipping-disable (CMDPMOD bit 11) + rotation v0

**Apport pour Mimas — PERF VDP1 (plot) plafond 0,2-1,2 ms, contre +0,10-0,16 ms de
CPU MASTER (le test par commande) — donc légèrement NÉGATIF pour le déport MSH2, et il
n'attaque PAS le transfer-over** : le pre-clipping supprime la DÉTECTION (≤5 cycles/ligne),
pas le parcours ni l'écriture. Seule la rotation v0 (non chiffrée par la doc) viserait le
goulot, et elle entre en conflit avec la couture `xg`/matelas des murs. Qualité 0, RAM 0.

**Verdict** : CONDITIONNEL: GO immédiat pour l'étape 1 seule (bit Pclp sur commandes prouvées entièrement visibles, ~20 lignes, toggle vif, critère LP% -3 points à pclp_n>100) ; la rotation v0 reste NO-GO tant que l'étape 1 n'a pas montré que LP% est sensible au pre-clipping, à cause du conflit couture xg/matelas (dg_saturn.cxx:6497, 6583-6596)

### 1. Mécanisme chez Ponut64

`preclipping()` (`ponut64-stuff/render.c:632-689`), appelée par quad avant émission :
- Flags écran par sommet posés par `clipping()` (render.c:695-705) : X+/X−/Y+/Y− contre les bornes de la zone user-clip courante (`setUserClippingAtDepth`, render.c:721-732).
- v0 hors écran en Y (`clipFlag & 12`) : rotation des sommets (0↔3, 1↔2) + `*flip ^= 1<<5` (V-flip CMDCTRL) pour conserver le mapping ; pre-clipping laissé **actif** (`*pclp = 0`) (render.c:639-654). Idem en X (`& 3`) : 0↔1/3↔2 + H-flip (render.c:655-670).
- Les 4 sommets à l'écran : `*pclp = VDP1_PRECLIPPING_DISABLE` = **2048 = bit 11** (render.c:671-675 ; render.h:43).
- Intention : *« Costs some CPU time… Improves VDP1 performance, especially important for hi-res mode »* (render.c:637). **Aucune mesure dans la source.**

### 2. État as-built Mimas

**Le bit 11 n'est posé nulle part** : aucun CMDPMOD n'excède 0x04E0 dans `src/dg_saturn.cxx`. Sites d'émission et visibilité déjà disponible par commande :

| Site | PMOD | « entièrement visible » décidable avec |
|---|---|---|
| mur texturé fenêtré `wall_emit_band` (6598) | 0x04E0 Window_In | corners xs/xe/yls…yhe déjà calculés (6540-6548) vs fenêtre wx1/wx2/vyt/vyb (6566-6571) ; ⚠ tuiles acceptées jusqu'à ±`wall_ext`=768 hors vue (6410, 6555-6558) |
| mur dégénéré (6514) / squish (6623) | 0x00E0 | x1/x2 ; y clampés bande vue (6618-6621) |
| mur flat potato (6858-6859) | 0x00C0 | fx1/fx2 clampés vue (6854-6855) ; y ±1 non clampés (6861-6864) |
| tuile sol inc-2 `fvdp1_emit_tile` (7320-7321) | 0x00E0 « no clip » assumé (6941-6945) | les 4 corners sx[i]/sy[i] passés (7342-7346) |
| erase full-screen present v2 (8276-8283 et 8294-8301) | 0x00C0 | constante (0,0)-(319,223) = clip système exact (8149-8151) |
| arme DISTORSP (8514-8515) | 0x04A0 Window_In | x0/y0/w/h (8508-8513) vs clip vue (8325-8327) |
| thing 1p (8684-8685) et drain split (8753-8754) | 0x04A0 | quad x0…y1 **et** box user-clip cx0…cy1 tous deux en main (8677-8702 ; 8658-8661) |
| HUD (8861-8862) | 0x00E0/0x00A0 | régions fixes écran |

Flips : les murs ne flippent jamais ; things/arme flippent par **ordre des sommets** (8512-8513, 8682-8683), pas par bits Dir. Le seul Dir-flip (0x0010, arme normal-sprite 8526) est du code mort : `SAT_WPN_VDP1=1` (5319) compile la branche DISTORSP (8495).
Clip système **global unique** (319,223), root + end (8149-8151, 9056-9058) : en split la séparation est 100 % user-clip par commande, pas de clip système par quadrant.
Silicium : **MODR lu = 1 sur la console du owner** (sonde `md` lue puis retirée, 3155-3159) → le bit Pclp existe sur la cible.

### 3. Arithmétique de budget

- Coût documenté de la détection : **≤ 5 cycles/ligne** (`HW_VDP1.md:596-600`, ST-013 p.83) → ≤ 175 ns/ligne à 28,6 MHz.
- Charge type mesurée HW : 178 commandes, LP 94-97 % (`HW_VDP1.md:590-594`). À ~40 lignes/quad (estimation) : ~7 100 lignes → gain **plafond** ~1,2 ms de temps de tracé ; réaliste 0,2-0,8 ms (« up to »). Références : écran plein = 2,6 ms ; les **deux** erase (8276 + 8294) = 448 lignes ≈ 80 µs à eux seuls, à test constant.
- **Sur notre goulot** : le transfer-over est de l'ITÉRATION — *« le clipping supprime l'écriture, pas le parcours »* (`HW_VDP1.md:582-584`, mesuré HW avec Pclp=0). Pclp=1 ne supprime que la **détection** sur des commandes déjà entièrement visibles ; il n'évite aucune écriture ni aucun parcours. La **rotation v0** (commandes débordantes — exactement notre mur proche qui parcourt tout son trapèze) est la moitié qui viserait le goulot, et la doc ne chiffre rien pour elle.
- Coût CPU master : 4-8 comparaisons short sur valeurs déjà en registres ≈ 15-25 cycles × ~180 cmds ≈ **0,10-0,16 ms** — petit mais sur le goulot (T165 > R141). Rotation v0 : flags 4 sommets + swaps, cmds débordantes seulement.

Soustraction honnête : gain VDP1 plausible 0,2-1,2 ms contre ~0,1 ms de CPU master. Marge positive mais mince : le mécanisme ne vaut que si LP% bouge.

### 4. Obstacles et risques

1. Réserve doc : Pclp=1 → *« No pre-clipping **and no horizontal inversion** »* (`HW_VDP1.md:600-602`). L'inversion horizontale interne est peut-être précisément le chemin que la rotation v0 exploite ; et le bit est incompatible avec le miroir Dir (seul site Dir = mort, cf. §2).
2. **Pclp=1 + user clipping actif** (bit 10 ; sites 0x04A0/0x04E0) : combinaison non couverte par notre base. Si le user-clip passe par l'étage pre-clipping, une commande marquée qui déborde sa box d'1 px (couture `sat_wall_xgrow`, 6497-6507) écrirait hors box. Le test doit donc être « quad ⊆ fenêtre de LA commande », jamais « ⊆ écran ».
3. Murs : tuiles acceptées jusqu'à ±768 px hors vue (6555) → beaucoup de quads murs ne seront pas éligibles ; test **par tuile**, pas par mur.
4. Rotation v0 sur murs : elle échange le côté élargi par `xg` (couture 1 px, 6487-6497) et interagit avec le MATELAS vertical (6583-6596) → risque de rouvrir les coutures. À isoler dans une étape séparée.
5. Consoles silicium v0 : bit ignoré → gain absent, pas de casse (`HW_VDP1.md:607-635`) ; notre console est v1 (3155-3159).

### 5. Plan d'expérience minimal

**Étape 1 — Pclp seul (~20 lignes, un build)** :
- `static int sat_pclp = 0;` togglé par un chord pad libre (greper les collisions avant, règle toggle-audit) ;
- OR de `0x0800` conditionné au test trivial de chaque site : erase (test constant, 8277/8295) ; tuile sol si les 4 corners ∈ [0,319]×[0,223] (7342-7346) ; mur texturé si la tuile ⊆ [wx1,wx2]×[vyt,vyb] ; things si quad ⊆ box (8677-8702) ;
- compteur `pclp_n` (cmds marquées) sur une ligne overlay, **légende mise à jour dans la même session**.
- Sonde : LP% (row 17) + `pr` + drops de things, sur la scène de référence à murs débordants (celle du LP 94-97).
- **GO chiffré** : LP% baisse ≥ 3 points avec `pclp_n` > 100 et `pr` stable (le test ne doit pas coûter > 0,3 ms d'émission). **NO-GO** : LP% ±1 point → bit sans effet sur notre charge, on ne code pas la rotation.

**Étape 2 — rotation v0** (uniquement après GO étape 1) : limitée au site mur texturé fenêtré, avec compensation Dir-flip façon Ponut, mesurée avec la même sonde ; surveiller la couture 1 px.

### 6. Non vérifié

Voir liste `unverified`.

### Non vérifié (liste structurée)
- L'incompatibilité Pclp=1 / miroir horizontal Dir est reprise de HW_VDP1.md:600-602 (citation de la table ST-013 p.83) ; je n'ai pas relu le PDF du manuel lui-même.
- Comportement de Pclp=1 sur une commande partiellement hors zone : l'hypothèse « l'écriture reste clippée, seul le parcours change » est inférée de HW_VDP1.md:582 — jamais testée sur HW.
- Combinaison Pclp=1 + user clipping actif (CMDPMOD bit 10) : non documentée dans notre base ; risque d'écriture hors box non exclu.
- Le sens de parcours du plot depuis v0 et l'abandon anticipé hors écran (rationale de la rotation Ponut) : affirmation du commentaire render.c:637, aucune mesure ni citation manuel trouvée.
- Hauteur moyenne ~40 lignes/quad servant au chiffrage 0,2-1,2 ms : estimation, pas mesurée.
- Les 178 commandes / LP 94-97 % : mesure HW consignée dans HW_VDP1.md:590-594, non re-produite ici.
- Ponut64 n'apporte aucune mesure chiffrée de son propre mécanisme (commentaire d'intention seulement).
- Le doublon d'erase full-screen (8276-8283 puis 8294-8301 quand vdp1_present_manual) semble émettre DEUX polygones plein écran par plot — observé à la lecture, non confirmé à l'exécution ; sans impact sur ce dossier mais à vérifier si l'on chiffre l'erase.

---

## 4. DIVU entrelacé start-early / read-late

**Apport pour Mimas — PERF MSH2 < 1,2 ms (< 2 % du MST, sous le bruit inter-build).**
Déjà tranché : l'entrelacement local (dv1) a été construit et RÉFUTÉ sur console le
2026-07-15 au site le plus dense (per-seg) — le corps de la mémoire `slave-offload-async-divu`
le dit ; seul son slug « async-divu » fait croire à un schéma slave (dv1 = entrelacement MASTER
local, kick tôt / read tard). Les autres sites
pèsent chacun moins que les 0,4 ms réfutées ; `__divdi3` est déjà mort partout (FixedDiv
inline ~37-75 cycles, DIVU HW dans inc-2). Qualité 0, RAM 0.

**Verdict** : NO-GO — sauf si la sonde FRT (plan §5) mesure ≥ 2,0 ms/frame de divisions bloquantes sur console en scène chargée, ce que l'arithmétique et le A/B HW dv1 rendent hautement improbable

### 1. Mécanisme chez Ponut64

`mymath.c:106-123` : `SetFixDiv(dividend, divisor)` écrit `*DVSR`, `*DVDNTH = dividend>>16`, `*DVDNTL = dividend<<16` — l'écriture DVDNTL lance la division 64/32 du périphérique DIVU ; le résultat se lit dans `*DVDNTL` ~39 cycles plus tard (une lecture prématurée stall le CPU).

`render.c:802-833` : pipeline logiciel rotatif à un cran. Avant la boucle, Z du vertex 0 est calculé et sa division lancée (`render.c:806`). Dans la boucle : (1) X/Y du vertex courant par MAC (`:813-814`) **pendant** que la division tourne ; (2) lecture `inverseZ = *DVDNTL` (`:817`) ; (3) Z du vertex **suivant** (`:820-821`) ; (4) `SetFixDiv` du suivant (`:824`) ; (5) projection écran du courant avec `inverseZ` (`:827-828`). La latence est donc couverte par du travail indépendant garanti (les MAC du vertex).

`renderAnim.c:261-286` : même schéma, la latence est remplie par l'interpolation d'animation du vertex suivant (`:274-276`) entre le `SetFixDiv` (`:267`) et la lecture (`:279`). Aucune protection d'interruption chez Ponut.

### 2. État as-built Mimas

**FixedDiv** : `core/m_fixed.h:56-108` = asm inline `DIV0U` + 32×(`rotcl`+`div1`), signes en C, garde overflow. 65 instructions 1-cycle ≈ **~70-80 cycles bloquants**, zéro appel de bibliothèque (le commentaire `:53` dit ~37, incohérent avec le corps). `core/m_fixed.c:33,49` : les versions C (`__divdi3`) sont mortes (`#if 0`). `FixedMul` = `DMULS.L`+`XTRCT` inline (`m_fixed.h:37-50`).

**Par-colonne** :
- `core/r_segs.c:2541` : `dc_iscale = 0xffffffffu / rw_scale` — mais gaté (`:2520,2529`) : ne tourne que si `sw_draws || is_edge` (mur possédé par le CPU = proche/transition/edge-fill ; le VDP1 possède le reste) et pas en `wall_solid` (`:2540`). Le réciproque de `SAT_VROWS` est déjà **hoisté par seg** (`sat_is0`, `r_segs.c:1828,1812`).
- `core/r_segs.c:187` : `dc_iscale = 0xffffffffu / spryscale` par colonne **masked** (grilles), chemin splitté master/slave (RP_TO_MASK, `core/r_parallel.c:782`).
- `core/r_parallel.c` : **aucune division** dans les exécuteurs (grep : seulement des commentaires) — les cmds arrivent précalculées.

**Par-seg** : `R_ScaleFromGlobalAngle` = 1 `FixedDiv` (`core/r_main.c:545`), appelé 2× (`r_segs.c:2812` + scale1), plus `scalestep = (scale2-rw_scale)/(stop-start)` (`r_segs.c:2814`) et le cas dégénéré `r_segs.c:2830`. ≈ 3 div/seg.

**Par-quad émis (murs VDP1)** : `r_segs.c:1478-1479,1536-1537` (2 réciproques/quad lead-fill), `:1591,1606` (`<<12)/n`), `:2118,2216,2294` (`sx/mdu`). Volume = budget wcap, dizaines/frame.

**Par-sprite** : `r_things.c:811` (`FixedDiv(projection,tz)` par candidat), `:910` (par vissprite), `:1058` (psprite, 1-2/frame). Pas de division par colonne sprite (`:655` = abs+shift).

**Par-frame** : `r_plane.c:621-622` seulement ; `R_MapPlane` est division-free (grep FixedDiv : aucun hit dans le corps).

**inc-2 sols VDP1 (le DIVU HW existant)** : `src/dg_saturn.cxx:7089-7102` `fvdp1_fdiv` = DIVU périphérique, **BLOQUANT** : écritures DVSR/DVDNTH/DVDNTL (`:7098`) puis lecture immédiate (`:7099`), sous IPL 15 (`:7095-7100`) parce que **les handlers SGL peuvent diviser** (`:7083-7085`). Sites : `:7123,7126` (2/coin × 4 coins/tuile) + `:7167` (≤4 pentes/quad) ≈ 12 div/tuile ; caps `FVDP1_CLAIM_MAX 12`, `FVDP1_TILE_CAP 16` (`:7046-7047`). Le passage `__divdi3` (~500 c) → DIVU (~50 c) est **déjà fait** (round 6, commentaire `:7080-7088`) ; il ne reste que la latence ~39 c à entrelacer.

### 3. Arithmétique de budget (la soustraction d'abord)

Coûts unitaires : DIVU bloquant avec IPL ≈ 50 c ; DIVU entrelacé parfait ≈ 12-15 c visibles (gain ≤ ~35 c/div, **si** 39 c de travail indépendant existent) ; `FixedDiv` inline ≈ 75 c (gain ≤ ~60 c/div) ; `__udivsi3` ≈ 60 c + call.

| Site | freq/frame (estimée) | gain max entrelacé |
|---|---|---|
| endpoints per-seg (r_main.c:545, r_segs.c:2814) | ~150-300 div | **~0,4 ms — MESURÉ et RÉFUTÉ HW (dv1)** |
| dc_iscale colonnes CPU (r_segs.c:2541) | ~50-200 (gaté VDP1-owns) | ~0,1-0,3 ms |
| masked (r_segs.c:187) | 0-300, splitté 2 CPU | ~0-0,4 ms |
| sprites (r_things.c:811,910) | ~60-100 | ~0,15-0,25 ms |
| inc-2 tuiles (dg_saturn.cxx) | ~12 × 20-40 tuiles | ~0,2-0,4 ms |
| quads murs VDP1 | dizaines | <0,1 ms |

**Total récupérable au mieux ≈ 0,5-1,2 ms** sur MST 61-142 ms console = **<2 %**, sous le bruit inter-build (mémoire : photos build-contre-build invalides). Le A/B HW existant concorde : dv1 théorie ~0,4 ms → mesuré Bp 24,9→25,7 (mauvais sens), fps 11,4→11,6 = bruit (`docs/SLAVE_OFFLOAD_STUDY.md:105-115`). Verdict du doc : « fill/memory-bound, pas arithmetic-bound ; ROI structurellement nul » (`:114-115`).

Coût du mécanisme : primitives kick/read par site + gestion IPL (les fenêtres longues type DDA `:7167` exigent IPL 15 étendu ou risquent le clobber DVSR par un handler SGL) + restructuration des chemins les plus chauds du port — exactement la classe de complexité que la maison interdit pour <1 ms.

### 4. Obstacles et risques

1. **Déjà réfuté sur console au site le plus dense.** ⚠️ Correction de la prémisse de la piste : `SLAVE_OFFLOAD_STUDY.md:122-129` prouve que `dv1` était **l'entrelacement LOCAL du maître** (« kick div-1, calcul des opérandes de div-2 (overlap), read div-1, kick div-2, read div-2 » sur les endpoints de `R_StoreWallRange`), pas un schéma slave longue portée. Le slug mémoire `slave-offload-async-divu` est trompeur ; le mécanisme Ponut au site per-seg est littéralement le prototype reverté du 2026-07-15. Les autres sites n'étaient pas dans le A/B, mais chacun pèse moins que les 0,4 ms réfutées.
2. **Interruptions** : les handlers SGL divisent (`dg_saturn.cxx:7083-7085`) ; toute fenêtre entrelacée doit être sous IPL 15 (coût ~12 c qui ronge le gain) ou auditée sans division ISR.
3. **Master/slave** : DIVU par-CPU (pas de conflit), mais le masked tourne des deux côtés → deux instances à maintenir.
4. **Le volume a déjà été coupé en amont** : réciproque hoisté (`:1828`), division sautée quand VDP1 possède le mur (`:2526-2541`) ou en wall_solid, `__divdi3` déjà tué dans inc-2. Les fruits bas sont cueillis.

### 5. Plan d'expérience minimal (soustractif — ne PAS construire le mécanisme)

- **Sonde** : `SAT_DIV_PROBE` (compile-time, défaut 0) : accumulateur FRT + compteur autour de `fvdp1_fdiv` et des 3 sites per-colonne/per-sprite ; publier sur la row 19 à côté de `v` : `dv<n>/<dixièmes-ms>` (mettre à jour la légende, même session — règle overlay). Aucun chord (18 vivants / 0 collision, ne pas toucher).
- **Protocole** : console (jamais Ymir pour un seuil), scène chargée type MAP15/E1M6, 1p puis 4p.
- **Critère GO chiffré** : somme ≥ **2,0 ms/frame** → construire l'entrelacement sur le SEUL site dominant, avec A/B toggle vif façon dv0/dv1. Somme < 2,0 ms (attendu : <1 ms) → **NO-GO définitif**, enterrer avec le chiffre dans SLAVE_OFFLOAD_STUDY.md.

### 6. Non vérifié

- Coût exact de `__udivsi3` (libgcc sh2eb-elf non lu) ; latence DIVU 39 c (manuel SH7604 via commentaire, non re-mesurée).
- Fréquences par frame : fourchettes estimées, non instrumentées (c'est l'objet de la sonde §5).
- DIVU par-CPU master/slave : fait SH7604 standard, non relu dans `saturn-refs` cette session.
- Les ISR Mimas propres (vblank_handler) divisent-ils : non audité.
- Masked lvl1 exécuté côté slave : mémoire + token `r_parallel.c:782`, non tracé de bout en bout.
- Le « ~37 cycles » de `m_fixed.h:53` vs les 65 instructions du corps : non mesuré.

### Non vérifié (liste structurée)
- Coût de __udivsi3 (libgcc sh2eb-elf) estimé ~60 cycles + call — pas lu dans lib1funcs.S
- Latence DIVU 39 cycles = manuel SH7604 repris par le commentaire dg_saturn.cxx:7080-7088, pas re-mesurée
- Fréquences par frame (colonnes CPU, colonnes masked, tuiles inc-2) = fourchettes estimées, pas instrumentées
- DIVU par-CPU (master et slave ont chacun le leur à 0xFFFFFF00) = fait SH7604 standard, pas relu dans saturn-refs cette session
- Aucun audit fait des ISR Mimas (vblank_handler) pour savoir s'ils divisent — seul le commentaire dg_saturn.cxx:7083-7085 atteste que les handlers SGL peuvent diviser
- Le « ~37 cycles » du commentaire m_fixed.h:53 est incohérent avec le corps (65 instructions) — pas mesuré
- Exécution du masked lvl1 côté slave = mémoire projet + token RP_TO_MASK r_parallel.c:782, pas tracée de bout en bout

---

## 5. Adoption de ponèSound (driver 68K MIT)

**Apport pour Mimas — QUALITÉ AUDIO visée : GAIN NUL sur le symptôme, et RAM samples
−53 Ko.** Prémisse d'hier FAUSSE : les percussions ne sont pas jetées par « le driver
SGL/SRL » (le 68K est HALTÉ en mode MUS) mais par notre propre séquenceur `mus_step` qui
saute le canal 15 faute de timbres de batterie (i_sound_saturn.cxx:408/:413). ponèSound n'a
ni séquenceur MUS ni banque de drums ; sa politique >32 slots = refus + retry sans priorité
(les volatile = nos SFX passent en DERNIER). Le vrai chantier = Route P (dossier
2026-08-25) : banque de 8 drums sur les 9 slots HW libres 23-31 de la chaîne existante —
FEATURE audio, ~150 lignes, 0 driver.

**Verdict** : NO-GO — l'adoption de ponèSound ne corrige pas le drop des percussions (cause racine dans notre mus_step, i_sound_saturn.cxx:408/:413, en amont de tout driver), coûte ~53 Ko de RAM samples (471 000 o utilisables contre 523 936 aujourd'hui, pour 532 927 o de SFX shareware) et impose une réécriture du synthé + requalification HW pour un gain nul sur le symptôme. Le chantier valide reste la Route P du dossier 2026-08-25 sur la chaîne direct-slot existante (slots libres 23-31).

### 1. Mécanisme chez Ponut64

**Architecture.** Driver C compilé m68k (`SCSP_poneSound/PROJ/main.c`, 1284 l., binaire `sdrv.bin` **8 824 o** ; MIT, `LICENSE` au repo). Le SH2 copie le binaire en RAM son via SNDOFF→DMA→SNDON (`pcmsys.c:198-212`) puis ne fait plus qu'écrire une struct partagée `sysComPara` à `0x408+47*1024` (`pcmsys.c:54`) et poser `start=1` chaque vblank (`sdrv_vblank_rq`, `pcmsys.c:420-424`). Le 68K spinne dessus (`main.c:1253`) et exécute `pcm_control_loop` une fois par tick : 93 commandes logiques `_PCM_CTRL` (`PCM_CTRL_MAX 93`, `main.c:71`) + 3 ADX, classées looping/protected/semi → ADX → volatile (`main.c:1159, 1224, 1235`).

**LA question aveugle — politique quand >32 slots HW sont demandés : REFUS + RETRY, ni vol de slot, ni priorité.** `find_free_slot` (`main.c:532-546`) balaye `ICSR_Busy[]` depuis `icsr_index` (remis à 0 chaque tick, `main.c:1134`) ; si aucun libre, retour −1 et le caller fait **`break`** — abandonnant TOUTE la suite de sa catégorie pour ce tick (`main.c:1170-1172, 1194-1195, 1213-1214, 1240-1241`). `sh2_permit` reste à 1 → le son refusé retente au vblank suivant. Priorité implicite = index `_PCM_CTRL` croissant ; les **volatile (SFX one-shot typiques) passent en dernier**. Slots **16-17 câblés CDDA** (`ICSR_Busy[16]=ICSR_Busy[17]=1`, `main.c:356-357`) → **30 slots utilisables**. ⚠ Bug latent : le `while` de `find_free_slot` n'a pas de borne — saturé, il lit HORS de `ICSR_Busy[32]` jusqu'à trouver un 0xFFFF adjacent (bénin car les tableaux voisins s'initialisent à −1, `main.c:363-369`, mais dépend du layout BSS).

**CDDA** : le 68K ne fait que le vol/pan des slots 16-17 (`main.c:1272-1280`) ; la lecture reste des commandes CDC côté SH2 (`CDC_CdPlay`, `pcmsys.c:457-477`) — les mêmes que SRL. **Chargement** : GFS partout (`cd_init` `pcmsys.c:151-157`, `load_driver_binary` :178-214, `load_8bit_pcm` :304-344, limite 64 Ko/son :322) ; samples chargés bout à bout dès `0xC028` (`pcmsys.c:55`), plafond `PCMEND 0x7F000` (`pcmsys.h:53`), reset par niveau `pcm_reset` (`pcmsys.c:119-140`). Pas de pitch par déclenchement : `pcm_play` ne pose que permit/volume/loopType (`pcmsys.c:82-88`), le `pitchword` est fixé au chargement.

### 2. État as-built Mimas

Un seul fichier touche RAM son/SCSP/68K : `src/i_sound_saturn.cxx` (grep `0x25A0|0x25B0|SNDOFF` sur `src/` : aucun autre hit ; `dg_saturn.cxx` n'a que des commentaires).

- **SFX** : 8 canaux fixes slots 0-7 (`NUM_CHANNELS 8`, :100), écrits en direct (`I_StartSound` :584-633) ; le choix du canal vient de `core/s_sound.c` (`snd_channels = 8`, s_sound.c:119).
- **Musique MUS (défaut)** : synthé maison slots 8-22 (`MUS_SLOT_BASE 8`, `MUS_N_SLOTS 15`, :135-136), **3 formes d'onde de 32 samples = 96 o** (:283-314). **Cause racine du drop : `mus_step` saute le canal 15 (percussions) sur note-off ET note-on** (`if (chan != MUS_PERC_CHAN…)`, **:408 et :413**) — faute de timbres batterie, pas d'un driver. Le 68K est **halté** (`smpc_command(SMPC_SNDOFF)`, :527). SGL/SRL n'est PAS dans cette chaîne : le libellé de la piste (« la chaîne SRL/SGL jette 34-39 % ») reprend l'erreur de `docs/PONUT64_ENGINE_ANALYSIS.md` §7 ; `docs/DOSSIER_MATERIEL_2026-08-25.md:512-514` donne la vraie cause, confirmée par lecture.
- **RAM son** : bump-allocator sans éviction, `sram_alloc = 0x100` (:113), échec = « sound RAM full, skipped » (:263). Mode CDDA : driver SGL chargé tard (`SRL::Sound::Hardware::Initialize`, :503, via le patch `SAT_DEFER_SOUND_INIT`, `patches/saturnringlib.patch:37`), SDDRVS.TSK ~26 Ko à l'offset 0, `sram_alloc = 0x8000` (:511).
- **Slots 23-31 = 9 slots HW libres aujourd'hui** en mode MUS. Hook vblank disponible (`SRL::Core::OnVblank +=`, `dg_saturn.cxx:5136, 8206`).

### 3. Arithmétique de budget (la soustraction d'abord)

Mesuré cette session sur `wads_temoins/Doom1s.wad` ET `../DoomJo/doom1.wad` (identiques) : **55 lumps DS\*, 535 127 o bruts, 532 927 o convertis 8-bit** (longueur−32, règle de `cache_sfx` :257-258). ⚠ `cd/data/DOOM1.WAD` est REDEVENU un WAD Doom II/TNT (107 DS, 1 232 741 o, DSBOSSIT en tête) — piège `build-stale-stash-wad-swap` re-confirmé : toute mesure dessus est fausse.

| Chaîne | RAM samples utilisable | Manque vs 532 927 o |
|---|---|---|
| Mimas MUS (aujourd'hui) | 0x80000−0x100−96 = **523 936 o** | **~9,0 Ko** (+~110 o de padding ×4) |
| Mimas CDDA | 0x80000−0x8000 = 491 520 o | ~41 Ko |
| **ponèSound** | 0x7F000−0xC028 = **471 000 o (460 Ko)** | **~62 Ko** |

**L'adoption RÉDUIT la capacité samples de ~53 Ko** (47 Ko driver+BSS+comm+pile contre 256 o aujourd'hui). Le « ~464 Ko libres » de l'analyse d'hier était optimiste de 4 Ko. Plus gros SFX shareware : DSBAREXP 18 560 o < limite 64 Ko — OK unitairement.

**Gain sur le symptôme visé : 0.** ponèSound n'a **ni séquenceur MUS ni banque percussions** — le drop se produit dans NOTRE `mus_step` avant toute consultation d'un driver. La correction exige des **samples de batterie** (absents du WAD ; ~8-16 drums GM 11 kHz 8-bit ≈ 30-100 Ko), nécessaires avec OU sans ponèSound. Coût d'adoption : port GFS→WAD (~200 l.), réécriture du synthé vers `_PCM_CTRL` (pitch par note = écrire `pitchword` dans la struct partagée), patch du scan OOB, requalification HW complète du son.

### 4. Obstacles et risques

1. **Mismatch cause/mécanisme** (§3) — le motif d'adoption ne tient pas.
2. **68K permanent** : notre mode MUS le halte (:527) ; l'adoption inverse ce choix (coût bus jamais mesuré ; « 68K halté = gratuit » ne dit rien du 68K vivant).
3. **Exclusion SDDRVS/SDRV.BIN** : les deux se chargent à l'offset 0 → en mode CDDA il faut couper `SRL::Sound::Hardware::Initialize` et re-router le volume par `m68k_com` (`SND_SetCdDaLev` mort). MAIS `CDC_CdInit` reste requis pour le routage CD-DA (:500) et ponèSound ne le fait pas (`load_drv` :216-240) → le champ de mines `cdda-8min` reste entièrement à nous. Coexistence lecture : neutre (mêmes commandes CDC).
4. **Politique de slots sans priorité** : sous saturation, un `break` gèle toute une catégorie ; nos SFX (volatile) passeraient APRÈS la musique — l'inverse de la hiérarchie Doom.
5. Licence : MIT, OK (crédit cWx/fafling/TrekkiesUnite118 dans la source).

### 5. Plan d'expérience minimal (plus petite portée : ZÉRO adoption)

**Étape 1 — sonde (≈10 l.)** : deux compteurs dans `mus_step` (:405-434) — note-ons ch15 jetés / note-ons totaux — affichés via le shim `dbg_print` existant (`SFX_DIAG`, :64-67). Vérifie les 34-39 % en jeu réel.
**Étape 2 — Route P minimale (~150 l., toggle `#define SAT_MUS_PERC`, vif par chord pad libre)** : banque de 8 drums (kick/snare/2 hats/2 toms/crash/ride, ~40-60 Ko 8-bit signé sur la piste data, chargée dans le bump-allocator existant) ; `mus_step` ch15 : note# MUS = instrument → key-on one-shot sur les **slots libres 23-31** (round-robin 9 voix), même recette registre que `I_StartSound`.
**Critère GO chiffré** : compteur « joués/jetés » ≥ **95 %** des note-ons ch15 servis (pic simultané mesuré ≤ 9), **zéro** « sound RAM full » supplémentaire (:263), et validation à l'oreille du propriétaire (règle `ask-before-instrumenting-observables`).
**Si un jour** samples+drums ne tiennent plus dans 512 Ko : reconsidérer une éviction dans NOTRE bump-allocator — pas un driver qui en enlève 53 Ko.

### 6. Non vérifié

Voir champ dédié.

### Non vérifié (liste structurée)
- Les pourcentages 34,6 % / 38,9 % / 34,3 % de note-ons canal-15 : repris de docs/DOSSIER_MATERIEL_2026-08-25.md:512-514 (remesurés là en marchant les lumps MUS) — non re-dérivés cette session ; seule la MÉCANIQUE du saut (i_sound_saturn.cxx:408/:413) est prouvée par lecture.
- Le coût bus/perf d'un 68K qui tourne en continu pendant le jeu : jamais mesuré sur HW (la mémoire '68K halté = gratuit' ne couvre que le halt).
- La terminaison du scan OOB de find_free_slot (PROJ/main.c:534-537) sous saturation dépend du layout BSS produit par le linker m68k — lue dans la source, jamais observée saturée.
- L'octroi de slots ADX/streaming et le host-loop pcm_stream_host : non relus cette session au-delà de PROJ/main.c (le NOPE streaming vient de PONUT64_ENGINE_ANALYSIS.md).
- La taille exacte de la zone de comm sysComPara + pile 68K (j'ai pris le plafond PCMEND 0x7F000 et le départ samples 0xC028 lus dans pcmsys.c:55/pcmsys.h:53 ; les loaders acceptent en fait jusqu'à 0x7F800, soit +2 Ko).
- L'estimation 30-100 Ko pour une banque de drums GM 11 kHz 8-bit : ordre de grandeur, aucun asset choisi ni mesuré.
- Que cd/data/DOOM1.WAD pollué (107 DS, WAD Doom II/TNT) corresponde à un build volontaire en cours ou à un stash périmé : constaté, cause non établie.

---

## 6. Pyramide de sous-textures (uv_cut) pour les sols VDP1

**Apport pour Mimas — PERF VDP1 (plot) ~1,5-2 ms de temps VDP1 IDLE sur un mode
PARKÉ, CPU MSH2 0, qualité 0.** Prémisse d'hier FAUSSE : les sols VDP1 inc-2 sont DÉJÀ
texturés (fenêtre V dans le flat 64×64 en slot VRAM) ; la seule teinte plate est le potato
SOFTWARE. Le mode entier est parké depuis le 2026-08-24 sur triple A/B console (CPU de
décision 2,2-9,2 ms contre < 1 ms de fill économisé) ; la pyramide n'inverse aucun des deux
termes perdants et ne change rien au warping (géométrie, pas texture).

**Verdict** : NO-GO

### 1. Mécanisme chez Ponut64

`uv_cut(data_start, wx, yh)` — `C:/Users/pcico/Projects/ponut64-stuff/tga.c:988` — pré-découpe à l'init chaque texture 64×64 (ou 32×32, line-doublée vers 64×64 d'abord, tga.c:1008-1053) en pyramide de sous-textures **toutes downscalées ≤32×32** : base 64×64→32×32 (tga.c:1056), moitiés 32×64→32×32 (1062-1109), quarts 16×64→16×32 et 64×16→32×16 (1112-1168), huitièmes 8×64→8×32 et 64×8→32×8 (1170-1224), quarts 32×32 (1226+) — 224 sous-IDs par base (docs/PONUT64_ENGINE_ANALYSIS.md:68-75). Le quad subdivisé (règles z view-space {512,256,128}, renderSub.c:226 ; jamais en screen-space — warping, renderSub.c:1913) sélectionne sa sous-texture par table. Effet : densité de texels quasi constante ⇒ **lignes source ≈ lignes destination ⇒ temps de plot borné par tuile**.

### 2. État as-built Mimas

**Prémisse « teinte plate » : FAUSSE pour le VDP1.** `fvdp1_emit_tile` émet des DISTORSP **texturés** : cmd[0]=0x0002, CMDSRCA = fenêtre dans le flat 64×64 résident en slot (src/dg_saturn.cxx:7320-7327), CMDCOLR = banque CRAM issue de l'ombrage R_MapPlane exact à la ligne proche (7304-7317). Fenêtrage **V seulement** (u forcé 0..64 : VDP1 sans registre de stride, 7200-7218 ; v0/v1 = lignes 64 o contiguës, 7219-7220). Les tuiles = cellules **monde 64×64 unités** (grille globale des flats = décomposition exacte par construction, 7287-7293). La seule « teinte plate » sols du dépôt est le **potato software** (SQ_FLAT : memset de la couleur dominante `R_FlatPotatoColor`, core/r_plane.c:336-339 et 524-531) — chemin CPU, et les claims VDP1 refusent en potato (dg_saturn.cxx:7522).

**Le chemin entier est PARKÉ** : `#define SAT_VDP1_FLOORS 0` (dg_saturn.cxx:7040), stubs sans un octet de .bss (7806-7809). Verdict console owner 2026-08-24, trois paires ON/OFF (7006-7021) : `v` (CPU décision flush+hook) 2,2/4,4/9,2 ms contre 1300/4600/100 px punchés (<1 ms de fill économisé à ~7 cyc/px) ; plafond d'économie écrit : 1,5-3 ms quoi qu'on fasse ; taux d'échange dest/src ~1,5 en bas d'écran, ~0,06 à 20 lignes sous l'horizon (7019-7021). Réouverture exigerait un punch ×10 (7030-7033).

**Slots** : « TEX_SLOTS 4 » du brief = `THINGS_TEX_SLOTS` **4** (things, docs/RESOURCE_BUDGETS.md:213) — sans rapport avec les sols. Sols : `FVDP1_SLOTS` **3** slots LRU de 4 Ko à `FVDP1_BASE` 0x25C7D000 (7042-7044 ; RESOURCE_BUDGETS.md:215). Upload = copie CPU de 2048 mots 16 bits à l'acceptation du claim, **jamais d'I/O disque** (pool flat-slab ou lump résident seulement, 7232-7267) ; pré-sonde gratuite `fvdp1_slot_would` (7269-7285).

**Budgets** : charge plot par tuile `tpx = (v1-v0) × largeur_dest` (7704) ; caps `FVDP1_PX_CAP` 40000/frame, `FVDP1_TILE_PX` 16000 (7055/7059) — calibration **devinée** ~14k px-writes/ms ≈ 3 ms (7051-7054). Commandes : un quart du surplus mur (7444-7459). 1p seulement (`sat_split_active` → return, 7470). Toggle vif pad R+Right existe (10870-10876, boot OFF, défini 2124) ; sonde row 19 `FLT v/s/@/F` (3388-3413).

### 3. Arithmétique de budget (la soustraction d'abord)

**Ce que la pyramide réduit** : uniquement `src_rows`. Une tuile lointaine paie aujourd'hui jusqu'à 64 lignes source pour 2-8 lignes dest ; mip-32 ⇒ charge ÷2, mip-16 ⇒ ÷4. Borne absolue : le cap 40000 ≈ ~3 ms de plot ⇒ **économie plot max ~1,5-2 ms de temps VDP1** — or le park écrit que ce mode n'a jamais eu que la **marge IDLE du VDP1** (7026-7027, 7685-7687) : Ymir ne modélise pas le plot, et le fill texturé vs polygone (2 cyc/px vs 1, docs/VDP1_LIMITS_SOURCED.md:155/261) est déjà payé quand le mode est ON. **Économie CPU master : 0** — les deux termes perdants du park (`v` 2,2-9,2 ms de décision, P +2,0/+3,8 d'émission) sont intouchés ; la sélection mip coûte quelques comparaisons (CPU ~0 ✓, mais « punch > coût » ✗ inchangé). Gain indirect possible : des tuiles moins chères sous les caps px ⇒ plus de punch/frame — refus par cap non instrumentés (continue silencieux, 7705).

**Coût** : mips 32+16 = +1280 o/slot (4096+1024+256=5376) ⇒ 3 slots 16128 o vs 12288 ; **tient** : 12 Ko libres à 0x25C5E000 + 4 Ko possibles en 1p pur (RESOURCE_BUDGETS.md:211/215) — et les sols sont 1p-only. Upload : +640 écritures mot par miss (+31 % de 7258-7259) + génération downscale ~5k ops/miss (moyenne 2×2), ou pré-cuisson dans la dalle WRAM (+31 % de dalle : NON, pool ~21 Ko). Code ~150 lignes dans un bloc compilé à zéro aujourd'hui.

### 4. Obstacles et risques

1. **Le porteur est mort** : la pyramide optimise un mécanisme parké sur mesure console ×3, et n'inverse aucun terme du verdict.
2. **Warping (question d)** : la pyramide ne change RIEN à la distorsion — l'interpolation linéaire du quad dépend de sa taille écran, pas de la texture. L'anti-warping de Ponut est la **subdivision géométrique** view-space ; la grille monde 64 de Mimas EST déjà cette subdivision (tuile lointaine = petite à l'écran, garde `FVDP1_TZ_NEAR` 24u + cap 16000 px côté proche, 7050/7059).
3. Pas de stride VDP1 : chaque niveau mip = bloc contigu séparé (compatible avec le design slot) ; alignement SRCA 8 texels OK à u=0.
4. Frontière mip visible entre tuiles adjacentes de niveaux différents (le « Saturn look » de Ponut l'accepte) ; l'ombrage CRAM (snap ±2-3 niveaux, RESOURCE_BUDGETS.md:250) est orthogonal.
5. Tous les chiffres sols inc-0..2c sont **Ymir-only** (RESOURCE_BUDGETS.md:428-429) ; tout critère plot = console + mètre LOPR (VDP1_LIMITS_SOURCED.md:198-205).

### 5. Plan d'expérience minimal (si le mode est un jour rouvert)

Préalable obligatoire : `SAT_VDP1_FLOORS` 0→1 (7040) — une ligne. Puis : (1) génération mip-32 dans `fvdp1_slot_get` (slot 5376 o, +copie 512 mots) ; (2) sélection dans l'acceptation : si `(v1-v0) ≥ 2 × hauteur_dest_projetée`, SRCA=base+4096, uv÷2, `tpx` recalculé ; (3) garde `#define FVDP1_MIP 1` (A/B compile) + toggle vif existant R+Right pour le mode entier. Sondes : row 19 `v` (CPU, doit rester ≤ inchangé), `s` (punch px), row 1 `pr`, LOPR `LP` console. **GO chiffré** : sur la scène de la paire 2 du park, `v` stable ±0,5 ms ET punch/frame ≥ ×10 (dizaines de milliers de px — le seuil de réouverture écrit en 7030-7032) ET fps +1 console. Sans le ×10 de punch, la pyramide n'a pas de terme à réduire : ne pas la construire.

### 6. Non vérifié

- Calibration plot ~14k px-writes/ms : **devinée dans le code même** (7053-7054) ; jamais mesurée console.
- Part des refus de tuiles due aux caps px (pas de why-code dédié, 7705) — le gain indirect « plus de punch via mips » est non quantifiable aujourd'hui.
- Nombre de flats distincts visibles par scène : seul fait sourcé = « plus de 3 » dans la scène bibliothèque du round 8 (7269-7272) ; pas de recensement.
- Seuil de warping vs taille de sous-texture : aucune mesure au dépôt ; « la pyramide ne change pas le warping » est un raisonnement (interpolation = géométrie), pas une mesure.
- Coût CPU de la génération downscale au miss (~5k ops) : estimé, pas mesuré ; fréquence des miss de slot en jeu : inconnue.
- Multiplicateur 2 cyc/px texturé : flaggé non-officiel dans VDP1_LIMITS_SOURCED.md:155.
- « Ship config = potato » (core/r_plane.c:885-890) lu dans un commentaire du chemin slave ; le défaut SQ réel par mode n'a pas été re-vérifié ici.

### Non vérifié (liste structurée)
- Calibration ~14k px-writes/ms du budget plot : devinée dans le code (dg_saturn.cxx:7053-7054), jamais mesurée console ; tous les chiffres sols inc-0..2c sont Ymir-only (RESOURCE_BUDGETS.md:428-429)
- Part des refus de tuiles imputable aux caps FVDP1_PX_CAP/TILE_PX : non instrumentée (continue silencieux, dg_saturn.cxx:7705) — le seul gain indirect de la pyramide est donc non quantifiable
- Nombre de flats distincts visibles par scène : seul fait sourcé = >3 dans la scène bibliothèque round 8 (dg_saturn.cxx:7269-7272), pas de recensement par carte
- Seuil de warping visible vs taille de sous-texture : aucune mesure au dépôt ; l'affirmation que la pyramide ne change pas le warping est un raisonnement géométrique, pas une mesure
- Coût CPU de la génération des downscales au miss de slot (~5k ops estimées) et fréquence des miss en jeu : non mesurés
- Multiplicateur ~2 cyc/px sprite texturé/distordu : marqué non-officiel dans VDP1_LIMITS_SOURCED.md:155
- Le défaut SQ réel par mode (potato = config ship ?) : lu dans un commentaire core/r_plane.c:885-890, non re-vérifié

---

## 7. Fenêtre d’effacement EWLR/EWRR restreinte (present v2)

**Apport pour Mimas — 0 ms (option A : l'erase HW du present v2 court dans le vblank de
la fence, personne ne s'y fie) ; 0,36-0,89 ms de PLOT VDP1 (option B : rétrécir le polygone
couleur-0 in-list) là où la marge de plot mesurée est 7-8× (g0 en split, la banque de
COMMANDES est la vraie limite — et l'option B en consomme un slot de plus en 3/4p).
Déport MSH2 0, qualité 0.** Comme chez Ponut, où l'idée est restée du code mort.

**Verdict** : NO-GO (valeur) — réouverture uniquement sur capture console montrant row 8 `g` > 0 soutenu en 1p/2p sous present v2 ; alors option B (rétrécir le polygone in-list, 2 halfwords, 1p/2p seulement), jamais l'option A (EWLR/EWRR = 0 ms de gain) ni la 3/4p (+1 slot de commande, la ressource limitante)

### 1. Mécanisme chez Ponut64

`setFramebufferEraseRegion(xtl,ytl,xbr,ybr)` — `ponut64-stuff/render.c:144-153` : encode le rectangle d'erase-write dans deux halfwords, lo-res `((x>>3)<<9)|y`, hi-res `((x>>4)<<9)|(y>>1)` (X en unités de 8/16 px sur les bits 14-9 — le piège d'unités de HW_VDP1.md:463-469). Les variables (`render.c:58-64`, défaut plein écran) sont poussées vers les registres dans `vblank_requirements()` — `vdp2.c:126-127`. Intention (commentaire `render.c:141-143`) : « cordonner » une zone d'items statiques jamais redessinés pour épargner du temps VDP1. **La fonction n'a aucun appelant** (grep sur tout le dépôt : définition + header seulement) : la plomberie registre est vivante, la restriction est du code mort chez lui aussi.

### 2. État as-built Mimas

**Registres d'erase au boot** — `src/dg_saturn.cxx:8193-8197` : `TVMR=0`, `EWDR=0` (transparent), `EWLR=0`, `EWRR=((320>>3)<<9)|223` = **0x50DF** (plein écran 320×224, = HW_VDP1.md:470), `FBCR=0` (boot 1-cycle auto), `PTMR=2`.

**Present v2 (le défaut, sans toggle)** : kick `PTMR=1` à la fermeture de banque (`dg_saturn.cxx:9059-9067` chemin murs, `:9101-9103` chemin vide) ; fence `sat_mp_fence` (`:8084-8133`) appelée chaque frame (`:10202`) : gate COPR (`:8103-8108`), puis **VBE erase & change** à un edge vblank-IN frais — `TVMR=0x0008` + `FBCR=0x0003` (`:8114-8117`), `VBE→0` après l'edge OUT (`:8128`). **L'erase HW piloté par EWLR/EWRR ne court donc QUE dans ce vblank**, vise le buffer AFFICHÉ/retirant (fait rouge Kronos, HW_VDP1.md:457-461), est partiel en NTSC et déclaré « harmless — the in-list colour-0 polygon owns the real erase » (`:8081-8082`).

**Le vrai effacement = polygone couleur-0 in-list**, 1re commande de dessin de chaque banque murs (`:8266-8284`) : `FUNC_Polygon 0x0004`, `CMDPMOD 0x00C0` (SPD|ECD-off), `CMDCOLR 0`, quad **(0,0)-(319,223)**, inconditionnel, « ~2.5 ms of plot » (`:8270`). Une mini-liste d'erase plein écran identique sert les frames menu/intermission/mode-switch (`:9081-9100`). L'ancien système est mort : `VDP1_MANUAL_CHANGE=0` (`:135`) compile hors le 2e polygone (`:8291-8302`) et le handler (`:8206`) ; `vdp1_present_manual=0` (`:1744`).

**Zone VDP1 réelle par mode** (fb natif 320×224, `:1204`, `:265`) :
- **1p** : vue 0..191, STBAR software rows 192..223 (`HUD_Y 192`, `:5802`, `:9789`). Arme sous UserClip à la vue (`:8320-8328`), murs clampés au band de vue même dans le fallback squish sans clip (`:6615-6621`), things sous UserClip bbox visible (`:8672-8679`). Sysclip racine 319,223 (`:8149-8151`).
- **2p** : bande HUD software [160,224) (`HUD2P_TOP=160`, `:1210`, `:953`) → VDP1 ≤ y159.
- **3/4p** : quadrants 160×112, vue 96 lignes + bandeau 16px par quadrant (RESOURCE_BUDGETS.md:97) ; 3p : Q4 = minimap software → rien de VDP1 dans x[160,320)×y[112,224).

### 3. Arithmétique de budget (la soustraction d'abord)

Débit polygone plat = 1 px/clock ≈ **28,6 Mpx/s** (VDP1_LIMITS_SOURCED.md:154) → plein écran 71 680 px ≈ **2,51 ms** (recoupe le `~2.5 ms` de `:8270`).

**Option A — restreindre EWLR/EWRR (Ponut littéral)** : gain **0 ms**. Dans le v2, cet erase court dans le vblank de la fence (temps mort), ne mange PAS le plot de la frame suivante, et personne ne s'y fie. Le rétrécir ne rend rien.

**Option B — rétrécir le POLYGONE in-list** (le seul endroit où l'effacement coûte du plot) :
| Mode | Zone épargnable | Gain plot | Coût |
|---|---|---|---|
| 1p | rows 192-223 = 10 240 px | ~0,36 ms | 2 halfwords |
| 2p | rows 160-223 = 20 480 px | ~0,72 ms | 2 halfwords |
| 4p | bandeaux 96-111 + 208-223 = 10 240 px | ~0,36 ms | **+1 slot de commande** (2 rects) |
| 3p | bandeaux + Q4 = 25 600 px | ~0,89 ms | +1 slot |

**Option C — remplacer le polygone par l'erase HW restreint** : budget vblank NTSC = (1708−200)×(263−224) = **58 812 px** (HW_VDP1.md:480-486). Requis 1p vue = 40×192×8 = **61 440 > 58 812 → ne passe pas** ; 4p rect englobant y0-207 = 66 560 → non ; seul 2p (51 200) passe, à 87 % du budget, sans marge d'erreur.

**Contre quoi ?** Marge mesurée HW 08-20 (RESOURCE_BUDGETS.md:224-226) : split **VD1 9-31 ms, gate g0-19, marge de plot ~7-8× vs MST 166-212** ; en 4p le plot finit avant la frame (**g0**) — « la banque de COMMANDES est la vraie limite, pas le temps ». Gain max 0,36-0,89 ms de PLOT = 0,2-0,7 % d'une frame 131-212 ms, et **0 %** partout où g=0. En 3/4p, l'option B consomme un slot de commande — la ressource qui, elle, est la limite.

### 4. Obstacles et risques

- **Rémanence** : le sysclip 319,223 ne protège pas les rows épargnés ; toute commande qui y déborde laisse des pixels définitifs. Les chemins actuels sont clampés (`:6618-6621`, `:8326-8327`, `:8676-8678`) mais tout futur chemin d'émission hérite de la contrainte.
- **Changement de mode/joueurs** : il faudrait armer `sat_vdp1_switch_clear` (`:982`, `:9039-9042`) sur tout changement du descripteur de zone, sinon résidus au switch (la mini-liste `:9088-9092` plein écran reste le filet).
- **Incohérence doc** : le code dit déficit VBE « ×10 » (`:8081`, `:8269`), la base corrigée dit **1,22×** en NTSC 320×224 (HW_VDP1.md:485-487). Sans effet sur ce verdict, mais à réconcilier avant toute tentative « option C 2p ».

### 5. Plan d'expérience minimal (si réouverture)

Préalable OBLIGATOIRE : une capture console montrant **g (row 8) > 0 soutenu** en 1p/2p scène chargée sous v2 — les « LP 94-97 % » 1p datent de l'AUTO (07-26) et LP% est déclaré tautologique sous v2 (`:1788-1789`). Alors seulement : toggle vif (chord libre, vérifier les 18 vivants) qui écrit `cmd[11]`/`cmd[13]` du polygone (`:8281-8282`) à 191 (1p) / 159 (2p) au lieu de 223 ; sonde = row 8 `g` moyenné + `wd` ; scène étalon = celle qui a produit g>0. **GO chiffré : Δg ≥ 0,3 ms reproductible ET zéro rémanence dans les rows épargnés après 5 min + un aller-retour de mode.** Pas de variante 3/4p (slot de commande = mauvaise direction).

### 6. Non vérifié

- g réel en 1p sous present v2 (les chiffres de marge sont split, HW 08-20).
- Clamp Y des things : `cx0..cy1` viennent du core ; la dérivation prouvant cy1 ≤ bas de vue (fuzz/oversize inclus) n'a pas été relue.
- Sols inc-2 (fenêtres AABB) supposés confinés à la vue — non relus ici.
- Débit d'erase HW jamais mesuré par nous (formules du manuel corrigé) ; divergence ×10 vs 1,22× non résolue.
- Les 2,5 ms du polygone = commentaire + arithmétique 1 cyc/px, pas une mesure isolée.
- Position exacte des bandeaux 3/4p (rows 96-111/208-223) : déduite de « quadrants 112, vue 96 » + mémoire, la géométrie `sat_split_view` n'a pas été relue.

**Verdict : NO-GO valeur.** L'erase du present v2 ne vole du plot que via le polygone in-list, et le plot est la ressource excédentaire (7-8× de marge, g0) ; la restriction EWLR/EWRR proprement dite ne rend rien du tout. Comme chez Ponut — où l'idée est restée du code mort.

### Non vérifié (liste structurée)
- g (sat_mp_gate_ms) réel en 1p sous present v2 — les marges 7-8× citées sont mesurées en split (RESOURCE_BUDGETS.md:224-226) ; les LP 94-97 % 1p datent du mode AUTO (2026-07-26)
- Clamp Y des things VDP1 : les bornes cx0..cy1 du FUNC_UserClip (dg_saturn.cxx:8676-8678) viennent du core — la preuve que cy1 ne dépasse jamais le bas de vue (fuzz, oversize) n'a pas été relue dans r_things.c
- Confinement des sols VDP1 inc-2 (fenêtres AABB) à la zone de vue — non relu dans cette instruction
- Débit d'effacement HW réel sur console : uniquement les formules du manuel corrigé (HW_VDP1.md:480-486), jamais mesuré par le projet ; la divergence code «×10» (dg_saturn.cxx:8081,8269) vs base «1,22×» n'est pas résolue
- Le coût de 2,5 ms du polygone d'effacement plein écran = commentaire dg_saturn.cxx:8270 + arithmétique 1 cyc/px (VDP1_LIMITS_SOURCED.md:154), pas une mesure isolée au toggle
- Position exacte des bandeaux HUD 3/4p (rows 96-111 et 208-223) : déduite de «quadrants 112 lignes, vue 96» (RESOURCE_BUDGETS.md:97) + mémoire hud-3-4p-band ; la géométrie sat_split_view n'a pas été relue dans le code

---

## 8. LUT réciproque étroite (8-16 Ko) contre DIVU

**Apport pour Mimas — PERF MSH2 potentielle 0,8 à 5 ms, ENTIÈREMENT conditionnée à une
donnée que personne ne mesure : C, le nombre de colonnes software divisantes par frame
(r_segs.c:2541 + :187).** Concurrent qui plafonne tout : le DIVU on-chip 39 cycles, déjà
écrit (`fvdp1_fdiv`), 0 RAM, 0 pression cache — la LUT ne gagne que ~30 cycles de plus, en
cache-hit seulement, contre 8 Ko de .bss dans un renderer memory-bound. yslope/distscale sont
toujours des tables ; sols DDA/sprites/per-seg = NO-GO chiffrés (< 0,9 ms chacun).

**Verdict** : CONDITIONNEL: sonde de comptage d'abord — la LUT n'a le droit d'exister que si C (divisions par-colonne à r_segs.c:2541 + :187) ≥ ~2000/frame médian en scène lourde console ET si le bras LUT bat le bras DIVU on-chip 39 cycles (déjà en arbre, zéro RAM, zéro pression cache) de > 2 ms MST au même A/B trois bras sur la même build. Tous les autres sites (sols DDA déjà DIVU, sprites, par-seg, yslope/distscale toujours en tables) : NO-GO, gains sous le bruit ±6 ms.

### 1. Mécanisme chez Ponut64
- `zTable` = 65 536 entrées `int` (256 Ko) en LWRAM, pointeur posé mi-allocation pour accepter les indices signés −32767..+32767 (`ponut64-stuff/lwram.c:4-16`, alloc `lwram.c:55`, décl. `main.c:139`).
- Boot : `zTable[i] = fxdiv(scrn_dist, i<<16)` pour tout i ; garde `zTable[0] = zTable[1]` (`lwram.c:15`).
- Usage C : `inverseZ = zTable[pnt[Z]>>16]` (`render.c:311`, `renderSub.c:1150`, `:1292`). Usage asm : 4 instructions `shlr16; shll2; add zTbl; mov.l @r3` puis `dmuls.l` + `sts mach` (`renderSub.c:1547-1554`) — la division de projection par VERTEX devient lookup + multiplication.
- Ce qui rend ça 4-instructions : le domaine est Z en unités entières sur TOUT le 16 bits signé → zéro normalisation, zéro clamp, zéro fallback. C'est exactement ce que paient les 256 Ko — version actée NOPE chez nous.

### 2. État as-built Mimas
- **FixedDiv est déjà inline ~37 cycles** : DIV0U + 32×(rotcl; div1), signes en C, clamp overflow (`core/m_fixed.h:52-108`). Ce n'est PAS un appel libgcc à ~500 cycles — l'hypothèse Ponut (fxdiv cher) ne transfère pas.
- **DIVU on-chip déjà en arbre** : `fvdp1_fdiv` = DVSR `0xFFFFFF00` / DVDNTH `0xFFFFFF10` / DVDNTL `0xFFFFFF14`, IPL montée à 15 (`sr | 0xF0`) sur la fenêtre 3-écritures/1-lecture, ~39 cycles (`src/dg_saturn.cxx:7080-7102`). Emplois sols VDP1 : projections coin (`:7123`, `:7126`, garde `tz < FVDP1_TZ_NEAR :7121`) et pentes DDA 4/quad (`:7167`), bornes tz≥24, |dy|≤1512 (`:7087-7088`).
- **Sites par-COLONNE restants** (les seuls candidats LUT à volume) :
  - `core/r_segs.c:2541` `dc_iscale = 0xffffffffu / rw_scale` — murs texturés SOFTWARE, déjà gaté lever-C par `sw_draws || is_edge` (`:2529`) et sauté en mur solid Potato (`:2540`). Division 32/32 C → appel libgcc `__udivsi3`.
  - `core/r_segs.c:187` — pareil par colonne masked (`spryscale`), steppé `:196`.
- **Par-SEG** (2-4 divs/seg, déjà classé L4 « tiny », `docs/REC_REDUCTION.md:63,88-93`) : `r_main.c:545` (scale clampé [256, 64·FRACUNIT] `:547-550`), `r_segs.c:2830`, `r_segs.c:1478-1479` ; hoist `sat_is0` déjà fait (`r_segs.c:1777-1828`).
- **Sprites** : 2 FixedDiv par sprite projeté (`r_things.c:811` projection/tz, garde MINZ `:808` ; `:910` iscale) — par-sprite, pas par-colonne.
- **yslope[]/distscale[] EXISTENT ENCORE comme tables** (`r_plane.c:290-291`), recalculées uniquement au changement de taille de vue (`r_main.c:828,:834,:898,:903`), cachées pour le split sur (w,h,ds) (`r_main.c:725-730`). Le slave les lit en FixedMul (`r_parallel.c:2097`) ; zéro FixedDiv dans r_parallel.c (grep). Rien n'a été remplacé par du calcul — rien à LUTer.

### 3. Arithmétique de budget
1 ms = 28 600 cycles. Une LUT remplace ~37-70 cycles par un lookup ~5-10 cycles si HIT cache → **gain plafond ~30-60 cycles/division**.
- **Sols DDA** : le poste ENTIER (FLT `v` = CPU sols en dixièmes de ms, `dg_saturn.cxx:2175`) = 1,7-3,0 ms en scène simple ; les divisions y sont déjà à 39 cycles sur quelques centaines d'appels → gain LUT < 0,3 ms. NO-GO.
- **Sprites** : ~60 sprites × 2 divs × ~40 c = < 0,2 ms. NO-GO.
- **Par-seg** : ~150 segs × 3 divs × 40 c < 0,9 ms. NO-GO.
- **r_segs.c:2541/:187, le seul site à volume** : C colonnes/frame × ~40-60 c : C=500 → ~0,8 ms (bruit ±6 ms inter-build) ; C=3000 → ~5 ms (réel). **C n'est compté nulle part — c'est LA donnée manquante.**
- Coût : table 3 968 entrées 16 bits ≈ 8 Ko .bss + init boot (~4 000 divisions, une fois) + branche fallback.
- Placement : `build/Mimas.map` (26-08) : `_end = 0x060E9A80` → pool TLSF = 66 944 o jusqu'à 0x060FA000. −8 Ko → ~59 Ko, très au-dessus du plancher boot 4,8 Ko — MAIS écart massif vs `docs/RESOURCE_BUDGETS.md:161-162` (7 552 o au 08-20) : **pré-vol build.ps1 obligatoire**. LWRAM : sans objet — 2,1× plus lente par accès (`saturn-refs/knowledge/HW_MEMORY_AND_BUS.md:78`) et un PU_STATIC 8 Ko contigu est risqué (lg TNT MAP11 = 21-48 Ko, `RESOURCE_BUDGETS.md:182-183`).

### 4. Obstacles et risques
- **Le concurrent qui plafonne tout : le DIVU on-chip.** 39 cycles FIXES, zéro RAM, zéro pression cache, code déjà écrit. La LUT ne gagne que ~30 c de plus, et seulement en cache-hit.
- **Cache (le risque inverseur)** : 4 Ko 4-way write-through, lignes 16 o (`HW_MEMORY_AND_BUS.md:95-96,:108`). rw_scale steppe linéairement → indices adjacents, ~8 entrées 16 bits/ligne → ~1 miss/8 colonnes DANS un seg, mais chaque seg ressaute dans la table, et chaque ligne volée l'est aux textures/colormap d'un renderer memory-bound (pretessellation-levers-dead). Sur ce profil, l'A/B peut sortir négatif.
- **Pas de CLZ sur SH-2** : un domaine log-normalisé (seul à couvrir [256, 4 M] en 8 Ko sans erreur) exige une boucle de normalisation qui annule le gain. Donc index linéaire `rw_scale>>8` sur [2^15, 2^20), fallback division exacte hors domaine. Erreur bord bas 2^8/2^15 = 0,78 % → ~1 texel de dérive sur une colonne de 64 px à scale 0,5, sub-texel au-dessus de 2^16. Entrées 16 bits (iscale>>2, plage (2^12, 2^17]) suffisent — l'erreur dominante est la quantisation du domaine, pas la largeur d'entrée.
- Les colonnes CPU sont surtout les murs proches/magnifiés (rw_scale haut) → le clamp colle au domaine réel ; mais `is_edge` (`r_segs.c:2529`) arrive à toute distance → fallback non optionnel.
- `:187` (masked) tourne peut-être côté slave (masked lvl1) : LUT lisible par les deux CPU (lecture seule) OK, mais le gain s'imputerait au budget slave, pas au MST.
- Variante DIVU par-colonne : la garde IPL de `fvdp1_fdiv` (`:7095-7100`) coûte ~10 c/appel ; en 32/32 le déclenchement DVDNT réduit la fenêtre.

### 5. Plan d'expérience minimal
**Étape 0 (quasi gratuite, obligatoire) : compter avant de construire.** Deux compteurs par-frame `dv`/`dm` incrémentés à `r_segs.c:2541` et `:187`, affichés sur une ligne overlay existante (greper la ligne dans les 3 fichiers AVANT d'écrire — debug-overlay-placement ; légende mise à jour même session). Relever C sur E1M1 + scène lourde (MAP15 / TNT MAP11), console.
- **Critère d'abandon : C médian < 1 000/frame en scène lourde → piste 8 FERMÉE** (gain < ~1,4 ms < bruit).
- Si C ≥ 2 000 : toggle vif `sat_div_lut` (défaut 0), portée MINIMALE = `r_segs.c:2541` seul. `=1` : LUT 8 Ko .bss HWRAM, fallback exact hors [2^15, 2^20). `=2` : bras DIVU 32/32 (DVDNT). **A/B à trois bras (libgcc / DIVU / LUT) sur la MÊME build** — tranche le risque cache sans bruit inter-build.
- **GO** : ΔMST > 2 ms reproductible console pour le bras gagnant. Si LUT ≤ DIVU → shipper DIVU (zéro RAM) et fermer la LUT définitivement. Pré-vol build.ps1 avant tout passage console.

### 6. Non vérifié
- Coût de `__udivsi3` (GCC 14.2 sh2eb-elf) : non lu dans libgcc — estimé 40-70 cycles ; les gains du §3 en dépendent linéairement.
- C (colonnes divisantes/frame à r_segs.c:2541/:187) : aucun compteur n'existe ; 500/3000 sont des bornes de raisonnement, pas des mesures.
- Quel CPU exécute R_RenderMaskedSegRange par mode (masked lvl1 slave = mémoire, pas relu dans le code aujourd'hui).
- Nombre d'appels `fvdp1_fdiv`/frame : dérivé de FLT `v` (temps total sols), pas d'un compteur de divisions.
- Écart pool 66 944 o (Mimas.map 26-08) vs 7 552 o (RESOURCE_BUDGETS 08-20) : non réconcilié commit par commit ; le worktree a `src/dg_saturn.cxx` modifié non rebuildé.
- Coût cycles d'un line-fill LWRAM vs HWRAM : dérivé du ratio 2,1× mesuré sur UN profil (flaggé tel quel dans HW_MEMORY_AND_BUS.md:78-81).
- Chez Ponut : coût réel de `fxdiv` (source non lue) ; ancrage mi-buffer du pointeur zTable déduit de lwram.c:8-11+55, pas confirmé par une adresse.
- Comptes par frame segs (~150) et sprites (~60) : ordres de grandeur de mémoire projet, pas mesurés aujourd'hui.

### Non vérifié (liste structurée)
- Coût de __udivsi3 (GCC 14.2 sh2eb-elf) : non lu dans libgcc — estimé 40-70 cycles ; les gains chiffrés en dépendent linéairement
- C, le compte de divisions par-colonne/frame à r_segs.c:2541 et :187 : aucun compteur n'existe — 500/3000 sont des bornes de raisonnement, pas des mesures
- Quel CPU exécute R_RenderMaskedSegRange par mode (masked lvl1 slave = mémoire projet, pas relu dans le code aujourd'hui)
- Nombre d'appels fvdp1_fdiv/frame : dérivé de FLT v (temps CPU sols total), pas d'un compteur de divisions
- Écart pool TLSF 66 944 o (build/Mimas.map du 26-08) vs 7 552 o (RESOURCE_BUDGETS.md 08-20) : non réconcilié commit par commit ; src/dg_saturn.cxx modifié non rebuildé dans le worktree
- Coût cycles d'un line-fill cache depuis LWRAM vs HWRAM : dérivé du ratio 2,1× mesuré sur UN profil d'accès
- Chez Ponut64 : coût réel de fxdiv (source non lue) ; ancrage mi-buffer du pointeur zTable déduit de lwram.c:8-11+55, non confirmé par une adresse
- Ordres de grandeur segs/frame (~150) et sprites/frame (~60) : mémoire projet, pas mesurés aujourd'hui

---

## 9. Lumps Saturn-natifs bakés dans strip_wad.py

**Apport pour Mimas — TEMPS DE CHARGEMENT (CD), pool 0.** Déjà joué : `tools/bake_levels.py`
(VERTEXES/LINEDEFS/SEGS/NODES bakés au layout moteur + garde MIMASLVL) a été écrit, mesuré
et REJETÉ le 2026-08-18 — le bake GROSSIT le disque (+8 Ko E1M1, +28,6 Ko MAP15) sur un
chargement borné par les lectures CD synchrones (~57 ms/commande), et le moteur a pris le gain
autrement (chargement en place, 83-189 Ko de staging tués). Le pool TLSF n'est touché par
rien ici (les structs de niveau vivent en zone LWRAM). Résiduel unique : SIDEDEFS
30→16 o/rec (−33 Ko CD sur MAP15 ≈ −100-200 ms/warp).

**Verdict** : NO-GO — la piste telle que posée est déjà implémentée et rejetée sur mesure (tools/bake_levels.py:7-19, 2026-08-18) ; unique réouverture admissible : pilote SAT_SIDEDEFS seul, conditionné à un bracket console préalable montrant ≥200 ms de CD dans P_LoadSideDefs par warp (étape 0 du plan, coût 2 lignes).

### 1. Mécanisme chez Ponut64

Conversion **offline** en big-endian : `ponut64-stuff/tools/tools.cpp:13-45` — `swap_endian_ushort/uint` + `writeUint16/32`, chaque champ est écrit déjà swappé par l'outil PC. Chargement : `ponut64-stuff/mloader.c:710-737` (`loadGVPLY`) — le blob GFS est utilisé **tel quel** : la « lecture » pose des pointeurs (`pntbl/pltbl/nmtbl/attbl/maxtbl/lumatbl`) qui avancent dans le blob, ré-alignement 4 final (:732-735), entrée via `gvLoad3Dmodel` qui `align_4` l'adresse de départ (:760). Zéro parsing, zéro copie, zéro swap runtime : **le fichier EST la struct runtime**. Condition de validité : format disque = exactement la struct mémoire, outil et moteur bumpés ensemble.

### 2. État as-built Mimas — la piste est DÉJÀ jouée

**a) `tools/bake_levels.py` existe** (hors pipeline) : bake VERTEXES/LINEDEFS/SEGS/NODES au layout moteur BE exact (tailles 8/24/14/28, `bake_levels.py:46`) + lump-garde `MIMASLVL` (:37-40, :221-223). En-tête :7-19 : « **MEASURED AND REJECTED, 2026-08-18** », avec la table +octets disque (MAP11 : SEGS +5466, VERTEXES +5988, LINEDEFS +16170, NODES +0) et le verdict « ~50-100 ms of CD to save ~5 ms of CPU ». **Aucun lecteur MIMASLVL côté moteur** (grep core+src : 0 occurrence) — jamais câblé.

**b) Le moteur a pris le gain autrement — chargement EN PLACE** (`core/p_setup.c:165-195`, preuve no-overwrite avec `SAT_INPLACE_TAIL 16` :190, `P_InPlaceOffset` arrondi 4 pour garder le fast-path GFS :192-195). VERTEXES :213-230, LINEDEFS :512-517, SEGS :274-279 ; NODES :403-427 = **swap pur in place** (`mapnode_t`==`node_t`==28 o, assert :109). Staging tué : 83 Ko (Tnt MAP11) à 189 Ko (SCYTHE MAP30) de pic transitoire, **zéro octet disque** (:167-174). Les structs sont épinglées par le compilateur : seg_t 14, node_t 28, line_t 24, side_t 16 (:81-84).

**c) Ce que P_SetupLevel transforme encore vraiment** : SEGS résout `fsi/bsi` en croisant lines/sides (:290-323) ; LINEDEFS calcule slope + 4 bits de signe + bbox16 depuis vertexes (:537-575) ; SIDEDEFS résout 3 noms de texture/side via `R_TextureNumForName` (:606-608) — déjà **hashé** (`core/r_data.c:1793-1815`), pas linéaire ; SECTORS résout 2 flats (:378-379) ; THINGS = spawn réel (:434-484), **non bakable**. Copies swappées simples : VERTEXES (`<<FRACBITS` :228-229), NODES, BLOCKMAP swap in place (:641-643), SSECTORS (:346-350). Aucun tri nulle part ; l'angle du seg est stocké brut (`ang16` :293). Stagings restants (W_CacheLumpNum transitoire, relâché) : SSECTORS :340, SECTORS :370, SIDEDEFS :598.

**d) Sondes** : `ld` n'est PAS un temps de chargement — c'est chunks GFS cumulés / refaults (`src/dg_saturn.cxx:2481,2527` ; `core/w_wad.c:86-99` ; `src/w_file_saturn.cxx:207-216`). Le temps CD réel = k-meter row 12 (`k/w/t`, `w_file_saturn.cxx:219-243`, `w_cd_ms10`). Autorité mesurée : boot = **4704 commandes CD / 270 s**, P_SetupLevel ne lit que **73 lumps** dont 63 = precache sfx (`p_setup.c:53-59, 1214-1218, 1300-1301`) — le level-load n'est PAS le goulot ; ses pics sont des lectures CD synchrones (:178-180).

**e) Alignement** : `strip_wad.py:50-63` padde chaque filepos à 4 (la cartouche lit en place : `core/w_wad.c:569, 735-739`) ; `merge_wad.py` ne padde PAS (grep align/pad : 0 hit).

### 3. Arithmétique de budget (soustraction d'abord)

Tailles lues des WADs (`cd/data/DOOM1.WAD` ; `wads_temoins/Doom2.wad`) :

| Lump (o/rec disque→baké) | E1M1 | MAP15 |
|---|---|---|
| VERTEXES 4→8 | 1868→3736 (+1868) | 6404→12808 (+6404) |
| LINEDEFS 14→24 | 6650→11400 (+4750) | 23660→40560 (+16900) |
| SEGS 12→14 | 8784→10248 (+1464) | 31764→37058 (+5294) |
| NODES 28→28 | +0 | +0 |
| **Total 4 lumps** | **+8082** | **+28598** |
| SIDEDEFS 30→16 | 19440→10368 (**−9072**) | 70830→37776 (**−33054**) |

À ~57 ms/commande CD en moyenne (270 s / 4704 cmd) et chunks 16 Ko, le bake des 4 lumps ≈ **+1-2 commandes ≈ +50-200 ms de warp pour économiser ~5 ms de CPU** (`bake_levels.py:16`). Négatif des deux côtés.

**Gain pool : NUL sur le chemin CD** — les tableaux restent des Z_Malloc PU_LEVEL en zone quoi qu'il arrive ; le staging transitoire est déjà tué par l'in-place. Le pool TLSF ~21 Ko (`docs/RESOURCE_BUDGETS.md`) n'est touché par rien ici : les structs de niveau vivent en **zone LWRAM**, pas dans le pool — la piste conflate les deux.

**Le seul lump qui RÉTRÉCIT : SIDEDEFS** (side_t=16, assert `p_setup.c:84`) : MAP15 −33 054 o ≈ −2 commandes ≈ **−100-200 ms/warp**, + mort du plus gros staging restant (70,8 Ko transitoire), + ~7000 lookups hash évités (~qq ms). C'est TOUT le gisement résiduel. Pré-swap pur (mêmes tailles, 0 octet disque) : n'économise que les `SHORT()`, les boucles restent — bruit contre un double chemin dans core.

### 4. Obstacles et risques

- **Divergence format** = corruption silencieuse ; la garde MIMASLVL existe côté outil (:37-40) mais **le lecteur moteur n'existe pas** — à écrire.
- **SAT_SIDEDEFS crée un couplage NOUVEAU** : les indices texture bakés doivent égaler l'ordre TEXTURE1/2 runtime (hash `r_data.c:1802`) — il faut un checksum TEXTURE1+TEXTURE2 dans MIMASLVL, sinon un repack change l'ordre et tous les murs mentent.
- **merge_wad.py ne padde pas** → un WAD mergé perd le fast-path aligné (bounce `w_file_saturn.cxx:303-414`) et casse la cartouche.
- **Cart « lump = struct en place »** : contre-indiqué en perf — cartouche = A-Bus 16 bits (`w_wad.c:108-109`) et le projet paie déjà `P_StageBSP` pour COPIER les tableaux chauds hors de la LWRAM 2,1× lente (`p_setup.c:961-976`) ; `bake_levels.py:21-22` garde l'outil pour ce jour-là, qui n'est pas venu.
- side_t est muté au runtime (switches, scrollers) → jamais mappable en lecture seule, seulement bakable vers un Z_Malloc.

### 5. Plan d'expérience minimal

**Étape 0 (2 lignes, coût nul, AVANT tout code format)** : bracketer `P_LoadSideDefs` + `P_SetupLevel` entier sur `w_cd_ms10` + `sat_vbl` (le pattern bracket est documenté prêt-à-remettre, `p_setup.c:58-59` : `w_lump_reads` est resté pour ça), affichage one-shot row 12. **Critère de réouverture : ≥ 200 ms de CD dans P_LoadSideDefs par warp, sur console** (pas Ymir — il ne modélise pas la latence CD, `w_file_saturn.cxx:221-223`).

**Étape 1 (seulement si le critère tombe)** : pilote **SAT_SIDEDEFS seul** — pas SEGS, qui GROSSIT le disque : bake 30→16 o/rec + indices textures résolus + MIMASLVL{tailles, checksum TEXTURE1/2} ; moteur : MIMASLVL valide → `W_ReadLump` direct dans `sides[]`, sinon chemin vanilla intact. Toggle vif = présence du lump dans le WAD (A/B sans rebuild moteur) + kill-switch `-DSAT_BAKED_LVL=0`. **GO chiffré** : `t` (row 12) baisse ≥ 0,2 s/warp sur MAP15 console ET validateur offline zéro divergence d'indices sur les 235 maps de wads_temoins.

### 6. Non vérifié

- Coût CPU réel des 3×n lookups hash de P_LoadSideDefs (« quelques ms » = estimation, jamais mesurée).
- ~57 ms/commande CD = moyenne dérivée du commentaire boot (4704 cmd / 270 s, p_setup.c:1214), pas une mesure par-commande du warp.
- Invariance de l'ordre des indices texture dans le WAD shippé (déduite de la construction du hash depuis TEXTURE1/2 ; non re-testée après les répertoires R4 lazy).
- Les chiffres « 83-189 Ko de staging tué » et « +16 Ko/+6 Ko CD » viennent des commentaires du code (p_setup.c:167-169, bake_levels.py:10-16), non re-mesurés.
- Ponut : « zéro parsing » repose sur loadGVPLY + docs/PONUT64_ENGINE_ANALYSIS.md:172-175 ; gvLoad3Dmodel n'a été lu que partiellement (:740-769).
- « MAP15 grosse map type » = Doom2.wad MAP15 lu directement ; MAP11/MAP30 cités d'après les commentaires du dépôt.

### Non vérifié (liste structurée)
- Coût CPU réel des 3×n lookups hash de P_LoadSideDefs — estimé « quelques ms », jamais mesuré sur console
- ~57 ms/commande CD = moyenne dérivée du commentaire boot 4704 cmd/270 s (p_setup.c:1214-1218), pas une mesure par-commande d'un warp
- Invariance de l'ordre des indices texture (TEXTURE1/2 → numéros runtime) dans le WAD shippé — déduite de r_data.c:1802 (hash construit depuis les lumps), non re-testée après les répertoires de textures R4 lazy
- Chiffres « 83-189 Ko de staging tué » (p_setup.c:167-169) et « +16 Ko LINEDEFS / +6 Ko VERTEXES de CD » (bake_levels.py:10-16) — repris des commentaires du dépôt, non re-mesurés
- Ponut64 « zéro parsing » — fondé sur loadGVPLY (mloader.c:710-737) et docs/PONUT64_ENGINE_ANALYSIS.md:172-175 ; gvLoad3Dmodel lu partiellement (mloader.c:740-769 seulement)
- L'affirmation que la mutation runtime de side_t (switches/scrollers) interdit tout mapping lecture seule — connue du comportement Doom vanilla, sites de mutation (p_switch.c/p_spec.c) non relus dans ce dépôt

---
