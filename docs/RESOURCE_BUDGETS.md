# RESOURCE_BUDGETS — bilan des ressources par mode de rendu

**Date : 2026-08-20.** Session d'analyse READ-ONLY (une session parallèle édite
`src/dg_saturn.cxx` — les greps « worktree » sont datés du jour et peuvent bouger).
Autorités timing : **mp-4p-hw-baseline** (8 vidéos console 2026-08-20) et
**m7-critical-path** (row-2 console 2026-07-29 — identité des termes valide, ms périmés).
Carte mémoire : `build/Mimas.map` (2026-08-19) et `build/Mimas-Tnt.map` / `Mimas-Doom1s.map`
(2026-08-20, post-inc-2c).

**Règles de lecture** : Ymir n'est JAMAIS un oracle de temps (tics 69-83 ms console vs
8-14 Ymir ; fps quantifiés vblank) — tout chiffre non tagué [HW] est [Ymir]/[est]/[src].
Chaque opportunité porte sa **soustraction** (budget disponible vs coût du mécanisme)
et son **falsificateur** (le champ d'overlay/capture qui tranche). Les leviers morts
sont en §8 avec leur référence — ne pas les re-proposer.

---

## 0. Lecture rapide

| Mode | MST [HW 08-20] | Le mur | Marges principales |
|---|---|---|---|
| **1p** | 44-151 ms (6,6-23 fps) | master render-bound (R suit fps) ; tic co-leader sur gros WAD (T69-83) | slave au plafond pratique (+3 fps pris) ; VDP1 léger overrun en scène chargée ; A0 VDP2 à sonder |
| **2p** | 100-172 ms (5,8-10 fps) | master (2 vues séquentielles) ; fill sensible (M7 +8-18 %) | slave Ps 3,3-4,0 ms [Ymir] ; VDP1 marge ~8× ; ciel HW coupé (récupérable) ; RBG0 vue élue non validé |
| **3p/4p** | 131-212 ms (4,7-7,6 fps) | master ÉMISSION/BSP-bound (render = 86 % de la frame) ; gouverneur AVEUGLE | slave b1-5 % (structurel) ; VDP1 plot g0 mais banque de cmds = la limite ; A0+A1 VDP2 sans consommateur ; ~300 Ko WTEX dormants (à confirmer) |

Le master SH-2 est le mur dans TOUS les modes ; il ne bloque nulle part (aucun idle à
recouvrir) — chaque ms doit être **retirée**, pas déplacée [m7-critical-path, code-audit
2026-07-30]. Les processeurs « libres » (slave en split, DSP, 68K) ont chacun un verdict
chiffré ci-dessous.

---

## 1. CPU

### 1.1 Master SH-2 (28 MHz) — ventilation par mode

**1p** [HW 2026-08-20, 8 vidéos] : MST 44-151 ms, render-bound (R suit le fps ; SEG
c160-1236) ; T tic 3-18 ms shareware. Ventilation row-2 de référence (couloir modéré,
console 2026-07-29 — **ms antérieurs aux fixes d'août**, seule l'identité des termes reste
autoritaire) : MST 41, R 32 (Bw 2,4 + Bp 11,3 + P 14,0 + M 0,6), T 1, S 0, dg 7, blit 1,3 ;
present ≈ 3,9 **inclus dans R** (identité row-1 : MST = R + T + S + b + dg, où b = blit).
- **P n'est pas le fill des plans** : 44-86 % de P = `pr` (émission VDP1 master) ; le vrai
  fill plans = P − pr = 1,6-3,3 ms [row2-phase-terms-truth, 3 captures 2026-08-10 Ymir].
  Dimensionner un offload de plans contre P surestime ~6×.
- **Bp n'est pas le segloop Doom** : texturage mur déjà gaté (`sw_draws`) ; la masse = la
  machinerie VDP1-claim (scans clip redondants, hoists faits en `sat_opt` L1-L4) + le
  plancher classe A + l'assurance de transition. ⚠ Le routage/émission `pr` (5-14 ms) est
  compté **dans P** (bracket row-2) — ne pas le soustraire aussi de Bp (double compte).
- Le « gros skip Bp » n'existe pas [audit adversarial 2026-08-19].

**Tic (T)** sur gros WADs, console TNT : T 69-83 ms typique = ~40 % de la frame
[HW 2026-08-16], jusqu'à T 165 [HW 2026-08-17] ; th (thinkers) = 21-49 % du CPU machine ;
mv (P_CheckPosition/blockmap) jusqu'à 23 % de frame en fusillade ; résidu mo ~25-45 ms =
~1900 P_MobjThinker memory-bound (mobj_t 156 o — `_Static_assert` p_setup.c:89 — dans le
tas LWRAM lent). Le tic a DÉPASSÉ le renderer une fois (T165 > R141) [HW 2026-08-17].
Les T spikes (T920-T6200, T1626) = UNIQUEMENT post-chargement (maketic catch-up) — soldé
[mp-4p-hw-baseline]. s (P_CheckSight) = 2,2-7,8 ms = sujet MORT (cache sight shippé).

**2p** [HW 2026-08-20] : MST 100-172 ms, vue 1 stable 36-40 ms ; M7 paie +8-18 % vs M4
(moitiés 160 lignes = fill encore significatif) [HW 2026-07-15]. Aucune ventilation row-2
2p complète n'existe (lacune §9).

**3p/4p** [HW 2026-08-20] : 3p MST 131-192 ; 4p MST 166-212, SPL 20-42 ms/vue équilibré
(bal0), T plat 14-17 ms. Le rendu = 86 % de la frame 4p (140 ms de vues + 13 ms de kick
sur 178) [HW 2026-07-15]. M7 vs M4 en 3/4p = NEUTRE (le low-res ne halve que les boucles
par-pixel, minoritaires en quadrants 96 lignes) — **le levier 3/4p = couper la
géométrie/émission côté maître, pas le fill** [m7-lowres-fill-bound-not-34p].

### 1.2 Slave SH-2 — état par mode

| Mode | b% mesuré | État |
|---|---|---|
| 1p | b7-29 % [HW 08-20] | Pile s3 défaut (plane-split TAS + masked-split + clear-on-slave), +3 fps HW pris (24→27, sweep 2026-07-29/30 [m7-slave-share-per-category]). **Plafond pratique atteint** : seul le FILL de P est offloadable, la génération (R_MakeSpans, Bw/Bp) est sérielle [m7-critical-path F]. |
| 2p | b0 % pré-fix [HW 08-20] ; Ps 3,3-4,0 ms post-`sat_mp_slave` [Ymir] | Gate `sat_local_players<=1` levé la nuit du 08-20 (kill R+B) ; **re-mesure console à faire**. |
| 3/4p | b0 % pré-fix [HW 08-20] ; b1-5 % même R+B ON [Ymir] | Parts plans/masked **structurellement petites** en quadrants — le slave n'y est pas un gisement, le master est le mur [mp-4p-hw-baseline R13]. |

**Part slave par phase** (code-vérifié [m7-slave-share-per-category]) : THINGS via
masked-split (lvl 1), SOLS+PLAFONDS via plane-split TAS (lvl 2, worklist partagée —
le slave prend ~60 % de la phase P en 1p, Pb59-61 %), MURS **jamais** (rp_disabled=1,
la colonne pleine ne passe pas par le REC), clear framebuffer sur slave (dg 8→5 ms).
Le b% par phase **en split** n'a jamais été relevé (lacune §9).

Lois : taxe bus 2,1× sur tout memory-bound (+5,8 ms mesurés wall-prep ×3 [HW]) ; le 2e
SH-2 ne paie QUE sur du fill compute-bound cache-chaud ; speedup dual plafonne S≈1,3-1,5
[HW_MEMORY_AND_BUS §3]. Un join qui spinne non-caché ralentit le job attendu (pause
I-cache posée dans rp_wait).

### 1.3 SCU-DSP, 68K, DIVU/DMAC

- **SCU-DSP** : réellement idle (prouvé au binaire — SGL ne touche jamais 0x25FE0080+)
  [dsp-idle-sight-tic-verdict]. ~14,3 MHz VLIW, 1 Ko de RAM données, **pas de division
  HW**. Morts : sight (traversal memory-bound), projection sprites (division-heavy,
  <0,4 ms). Seule cible propre = transform par-quad SI les sprites deviennent des quads
  (parké, plomberie jamais chiffrée).
- **68K + SCSP** : « halté = gratuit » vaut pour un build sans driver ; le build défaut
  est `-Mus` → le driver SGL 68K est VIVANT et sa charge n'a jamais été mesurée (§5).
- **DIVU** : mort en async sur les divisions core (<2 % de Bp, réfuté HW 2026-07-15) ;
  **payant en usage direct synchrone** (kick sols VDP1 inc-2 : pr 69-243 → 12-14 ms
  [Ymir]) — 39 cycles vs ~500 de __divdi3.
- **DMAC on-chip / SCU-DMA** : slDMACopy spinne (zéro overlap) ; SCU-DMA→VRAM VDP2 =
  FREEZE (loi documentée, 4 confirmations HW) ; le blit fb→NBG1 est DÉFINITIVEMENT
  synchrone : 5,5-11 ms B-bus-bound, W5 (−1,3 ms) déjà pris [blit-dma-lever].
- **FRT** : déjà consommé comme **instrument de profilage** (TCR=φ/128 dans les bodies
  slave et les sondes rp_frt/tic — auto-taxe ~2-4 µs/appel, 10-18 % du mo mesuré) ; pas
  une ressource de calcul disponible [game-tic-overtook-renderer, perf-audit-2026-07-07].

---

## 2. RAM

### 2.1 HWRAM (1 Mo SDRAM, 0x06000000)

Empreinte statique (du .map ; aucune section en LWRAM, tout le binaire vit en HWRAM) :

| Build | .text | .data | .rodata | .bss | _end | **Pool TLSF** (_end→0x060FA000) |
|---|---|---|---|---|---|---|
| Mimas 08-19 | 425 328 | 61 984 | 121 984 | 334 912 | 0x060F5640 | **18 880 o** |
| Tnt **et** Doom1s 08-20 | 432 416 | 62 000 | 121 968 | 339 120 | 0x060F8280 | **7 552 o (7,38 Ko)** |

`_end` identique entre Tnt et Doom1s ⇒ le −11 328 o est de la **dérive source pure**
(émetteur sols inc-* ~5 Ko 1:1 + fvdp1_* ~3,7 Ko de .bss), pas un coût de config WAD.
Plancher : famine historique 3,97 Ko ; boot mesuré entre 4,67 (pend) et 5,00 Ko (boote),
et le plancher DÉRIVE avec la config [HW 2026-08-06] ; build.ps1 hard-fail < 4,8 Ko.
**Marge actuelle ×1,5 vs le plancher de boot mesuré (×1,9 vs la famine 3,97) — le
prochain inc de ~5 Ko passe sous le plancher.** Pré-vol build.ps1 obligatoire.

Au-dessus du pool : WORK_AREA SGL 12 288 o @0x060FA000 + 12 288 o (piles SGL/système,
hors map, jusqu'à 0x06100000). Gros .bss/.data (nm 08-19) : framebuffer 71 680 (320×224, NON réductible),
openings 40 960, finesine 40 960, **doom_stack 40 960 (high-water JAMAIS mesuré — seul
gros candidat restant)**, states 27 076, viewangletox 16 384, drawsegs 12 288,
hud2p_panel 10 992 (payé dans TOUS les modes), zlight/walljobs/plane_worklist 3×8 192,
ticdata 5 120 (post-BACKUPTICS-32), heap newlib 4 096 (pic <2 Ko — **levier épuisé**).

### 2.2 LWRAM (1 Mo DRAM, ~2,1× plus lente) — zone Doom

- Zone Z_Malloc = **1 040 384 o (1016 Ko)** depuis L2-RECLAIM (ring RP 8 Ko inerte).
  ⚠ Le « 944 Ko » d'ENDGAME_ROADMAP §2 est PÉRIMÉ (RP_CMD_BUF est 0x2000, pas 0x14000).
- Fragmentation, pas capacité : lg (plus gros contigu) mesurés — TNT MAP01 137 Ko,
  TNT MAP11 **21-48 Ko** (avec zf 194-240 Ko « libres »), Doom II MAP13 107-130 Ko,
  SCYTHE MAP30 11 Ko. Distribution sur le corpus 415 cartes : jamais faite post-structs.
- Planchers PU_STATIC résidents par IWAD : Doom II 494,7 Ko / TNT 618,8 / Plutonia 552,9 /
  Ultimate 403,0 [ENDGAME §2]. Mur de contiguïté de boot RÉSOLU 2026-08-18 (structs
  seg 14 o / node 28 o / line 24 o + slab mobj −24 Ko) : tout wads_temoins charge sauf
  famille Nuts (1,9-2,7 Mo de mobjs).
- Consommateurs par mode : minimap 3p scratch 17,5 Ko (zone, pas pool) ; cache composite
  `sat_texcache_use` = 0 en 1p / 1 en split (OFF 1p PAR MESURE — 3-4× pire, ne pas
  re-lever).

### 2.3 Cartouche 4 Mo (A-Bus, ~4× lente, alignement 4 obligatoire)

IWAD shareware strippé = 4 174 732 o → **~19 Ko de résidu seulement**. Ne soulage JAMAIS
la zone LWRAM (données froides en masse uniquement) ; staging DRP par map
(drp_cart_staged, décode ~7-12 ms). La Saturn nue = 0 cart : tout levier cart est un
accélérateur optionnel, pas une base.

---

## 3. VDP1 (VRAM 512 Ko — les 2 framebuffers 256 Ko sont SÉPARÉS, jamais dans ce budget)

### 3.1 Carte VRAM (as-built, worktree 2026-08-20)

| Région | Adresse | Taille | Notes par mode |
|---|---|---|---|
| root + cmd vide | 0x25C00000 | 0x100 | LINK 1 halfword = le present |
| bank0 / bank1 (ping-pong) | +0x100 / +0x2100 | 2×8 Ko (256 cmds) | seules les banques de cmds sont double-buffées |
| **WTEX murs 26 slots** | 0x25C05000–0x25C5E000 | **364 544 o (356 Ko)** (16×8448 + 6×16 Ko + 4×32 Ko) | textures NON double-buffables ; verrou 3 états. 1p mesuré tx17/22 [HW 07-26] ; split : relevé owner « tx6/26 bk0 » ≈ **~300 Ko dormants** — à confirmer par capture (§9) |
| (libre) | 0x25C5E000 | 12 Ko | queue du pool murs |
| Arme | 0x25C61000 | 64 Ko (4×16 Ko) | re-adressée 16×4 Ko demi-res en split |
| Things | 0x25C71000–0x25C78000 | 28 Ko | THINGS_TEX_SLOTS **4**/frame (6 REVERTÉ : écrase le HUD) ; usage réel : THp médian **n0-n1 dans TOUS les modes** [HW 08-20] — voir opportunité 1 |
| HUD + messages | 0x25C78000–0x25C7BC00 (+pile 2p → 0x25C7D000) | ~15 Ko | la pile 2p occupe 0x25C7C000-0x25C7D000 |
| Flats sols VDP1 | 0x25C7D000 | 12 Ko (3×4 Ko LRU) | sûrs TOUS modes ; +4 Ko possibles en 1p pur |

### 3.2 Temps de plot et budget d'émission

- Budget = **un field** (~16,7 ms) ; dépassement = transfer-over, la QUEUE de liste est
  abandonnée (contenu perdu, pas du temps). Le limiteur est l'**itération/overdraw**, pas
  le fill (clipping supprime l'écriture, pas le parcours) [doc UM p.83 + HW ×2].
- 1p scène chargée : LP 94-97 % à 178 cmds avec slots 178/248 et tx17/22 non saturés
  [HW 2026-07-26] — overrun léger réel, de TEMPS de plot.
- **Split : VD1 9-31 ms, gate g0-19 → marge de plot ~7-8× vs MST 166-212** [HW 08-20].
  En 4p le plot finit avant la frame (g0) : **la banque de COMMANDES est la vraie
  limite, pas le temps** [mp-4p-hw-baseline R13]. Les commandes ne sont pas rares en 1p
  (c63-164 / B248).
- Caps par mode : WALL_CMD_CAP 248 ; réservation `vdp1_wall_cap` → murs 244 (2p), 240
  (3p), 237 (4p). WALL_ACC_MAX = **128** (dg:5194, tranches contiguës /nv par vue ; pic
  1p ~57 — ⚠ les valeurs 120/144 de VDP1_LIMITS/TOGGLE_AUDIT sont périmées).
- Present v2 VBE = LE present (défaut 2026-08-19) : fence 13-15 ms, +1,5 à +5 fps, trous
  clos ; **loi de quantization vblank** : une frame à ~33 ms perd un vblank entier pour
  3 ms de coût ajouté → critère d'adoption de tout chemin VDP1 = CPU ~0 ET punch > coût.
  Re-validation console OUVERTE (tous les rounds sols inc-0..2c = Ymir only).

---

## 4. VDP2 (4 bancs × 128 Ko + CRAM 4 Ko)

| Banc | 1p (M7 shippé) | 2p | 3p/4p |
|---|---|---|---|
| A0 | coef K RBG0 (RDBS 0x0D) — **sur-déclaration probable en K_LINE, sonde 0x0C jamais tirée → 128 Ko peut-être récupérables** | idem si RBG0 vue élue | RBG0 OFF → **sans consommateur déclaré** (RAMCTL/CYC par mode jamais tabulés — lacune) |
| A1 | bitmap RBG0 (sol dominant) | idem | idem A0 |
| B0 | NBG1 framebuffer 8bpp (CYCB0 0x55EEEEEE) — TOUJOURS | idem | idem |
| B1 | ciel NBG0 cell + NBG3 debug (CYCB1 0x04437EEE, 5 slots/8) | **ciel COUPÉ** (gate cb201c7 — récupérable ~10-15 lignes, <200 o, 0 VRAM, jamais validé HW) | NBG3 seul |

- RBG0 exige `sat_local_players<=1 ou ==2` → OFF en 3/4p ; la vue élue 2p (part5) est ON
  au pad mais NON validée HW.
- CRAM : 8 banques de 256 pleines (police + PLAYPAL + 6 niveaux pré-ombrés) ; **7 banques
  = plafond matériel de la lumière VDP1** (snap ±2-3 niveaux) [doomsrl-vdp1-capacity §CRAM,
  vdp1-manual-present-audit inc-1c] ; fenêtre banque 0 idx 16-31 déjà prise. NBG2 libre
  partout ; les fenêtres scroll sont GRATUITES — 0 banc, 0 cycle, registres dans le
  block-flush [vdp2-window-in-blockflush]. Famine de cycles = NEIGE sur HW, invisible Ymir.
- Vivants : N1 (sol HW du P2 en 2p via RPMD=3, fenêtre verticale x=160) ; N2 (sol cell +
  palette par tuile = couverture ×1,67). Morts : RBG1, plafond-dominant-1p (arithmétique),
  K_DOT sans compter le banc (§8).

---

## 5. SCSP et CD

**SCSP** (RAM son 512 Ko, SÉPARÉE — coût zone Doom nul) : le driver SGL 68K occupe le bas,
garde `sram_alloc` à 0x8000 en mode CDDA — les uploads SFX écrasaient son code, lecture
corrompue [src i_sound_saturn.cxx:496-511 ; fait partagé SRL_SGL_GOTCHAS #19] → **SFX
utilisables 480 Ko** ; 8 canaux SFX + 15 slots MUS sur les 32 slots SCSP ; high-water
réel JAMAIS relevé (overlay row 7 `r`) ; precache SFX dérivé du spawn shippé mais
HW-pending depuis 2026-07-10. Charge 68K jamais mesurée (build défaut `-Mus` = driver
vivant).

**CD** : une commande en jeu = ~37-41 ms [HW] ; GFS_Load SYNCHRONE par secteur de 2048 o,
bloquant le master (gèle toutes les vues en split) ; l'arsenal async GFS_Nw* est linké
mais INUTILISÉ (le R2.3 pump reste le chantier) ; budget de chargement par frame au pad
R+X (lb 0/1/2/4 : Bp max 1194→133 ms). Au repos : en build `-Mus` (le défaut), le lecteur
est INACTIF hors chargements — toute la bande passante CD est disponible pour le
streaming ; en build CDDA la musique occupe le lecteur. Un ODE rapide INVERSE le modèle
de coût seek — tous les chiffres CD du poste sont ODE (SD/Phoebe/SAROO), aucun ne vient
d'un vrai lecteur optique 2× (§9).

---

## 6. Soustractions par mode (le disponible, en une ligne chacun)

**1p** : master 0 ms d'idle ; slave déjà au plafond utile ; VDP1 ~0-1 ms de marge en scène
chargée (LP94-97) mais des slots cmds libres ; VDP2 : A0 à sonder ; pool HWRAM 7 552 o
(7,38 Ko) ;
zone : lg 21-137 Ko selon carte ; tic = le 2e mur sur gros WAD (T69-83) — leviers restants
maigres (SIGHT_CACHE_TICS sans RAM ; mo/mv memory-bound sans mécanisme bon marché connu).

**2p** : slave = 3-4 ms/frame de plans [Ymir, à valider console] ; VDP1 plot ~8× de marge
+ ~300 Ko WTEX dormants (à confirmer) ; ciel HW récupérable ; RBG0 vue élue à valider ;
fill encore significatif (M7 +8-18 %) → les leviers pixel paient encore ici.

**3p/4p** : le disponible est PARTOUT SAUF sur le master : slave b1-5 % structurel, VDP1
plot g0 (mais cmds = limite), A0+A1 VDP2 dormants, ~300 Ko WTEX dormants — et le master
émission/BSP-bound les rend inutilisables tels quels. Le SEUL levier qui touche le mur =
réduire ce que le master GÉNÈRE par vue (LOD émission, gouverneur par vue) ; le
gouverneur actuel n'élit RIEN en split (cible 95 ms = 1 vue 1p, e négatif partout).

---

## 7. TOP 5 des opportunités (gain plausible HW × coût d'implémentation)

### 1. Things→VDP1 en split : re-cibler le budget sur MST et valider console
- **Soustraction** : disponible = marge de plot VDP1 ~7-8× (VD1 9-31 ms, g0-19, 4p) +
  slots cmds ; coût actuel payé par le master = 12-16 fills sprite logiciels/frame
  (SPR td12-16, THp médian n0-n1 partout [HW 08-20]) ≈ ~5-15 ms/frame chargée [est —
  fill `fl` ~13,6 ms mesuré en scène gib/combat, row-15 SPR, sprites-vdp1-dsp-verdict
  [HW] ; ⚠ fl ≈ 0 hors scène chargée et le clip-walk de M reste après le passage VDP1
  [row2-phase-terms-truth] — le gain dépend fortement de la scène]. Mécanisme quasi
  entièrement shippé
  (hystérésis par texture, drop far-first équitable, gel de rampe, THp x, things bleus
  L+X) ; reste : re-cibler l'AIMD sur MST (pas le field) en split + A/B console.
- **Coût** : constantes + 1 session console. **Gain plausible** : le plus gros levier MP
  restant [mp-4p-hw-baseline, leviers 1-2].
- **Falsificateur** : captures console — THp médian ≥4 avec x~0 et g~0 et MST en baisse =
  gagné ; flicker image/rien qui revient = la machinerie de drop est encore insuffisante.
- Réfs : mp-4p-hw-baseline (R13), vdp1-budget-latch-kills-sprites (chapitre final).

### 2. Gouverneur par vue + LOD d'ÉMISSION en 3/4p
- **Soustraction** : disponible = rien côté processeurs (le master est le mur) ; le
  levier = réduire la génération. 4p : rendu 140 ms/178 ; le gouverneur (cible 95 ms
  calibrée 1 vue 1p) n'élit jamais en split → les leviers d'émission existants (LOD
  murs distance, seg-budget, caps things par vue) ne s'engagent JAMAIS. Coût d'une
  cible par vue ≈ `GOV_TARGET/nv` + redimensionner les leviers élus = petit.
  ⚠ Élire des leviers de FILL ne paiera pas (M7 3/4p = neutre) — n'élire que des
  leviers d'ÉMISSION/géométrie.
- **Coût** : ~heures. **Gain** : à chiffrer PAR le falsificateur, pas avant — aucun A/B
  split n'existe ; majorant connu des leviers élus en 1p : le seg-budget `lb` a fait
  Bp max 1194→133 ms, le LOD `Lo` existe (le doc M7_FEATURE note le 3/4p NON MESURÉ).
- **Falsificateur** : A/B console 4p même spot : SPL par vue baisse quand `Lo`/`lb`
  s'activent = gagné ; SPL immobile = mort, ranger le levier.
- Réfs : mp-4p-hw-baseline (gouverneur aveugle), m7-lowres-fill-bound-not-34p,
  wall-distance-lod-potato-seed.

### 3. Pool HWRAM : sonde high-water de doom_stack + récupération de code mort
- **Soustraction** : pool 08-20 = 7 552 o vs plancher boot ~4,8-5,0 Ko → marge
  2 432-2 637 o (2,4-2,6 Ko) ; le prochain inc sols (~5 Ko 1:1) passe SOUS le plancher =
  boot loop. Candidats : doom_stack 40 960 o (high-water jamais mesuré — sonde watermark
  ~20 lignes AVANT toute coupe), buffers overlay par-ligne ≈1 Ko (recensement « ~24 ×
  char[45] » du 2026-08-09 [streaming-load-budget] — le worktree 08-20 n'en montre plus
  que 5 sous cette forme, recompter), sat_plane_border ~250 o de .text mort.
- **Coût** : sonde ~20 lignes + 1 session (couvrir TNT + split + mêlée + load). **Gain** :
  si high-water ≤ 24 Ko → +16 Ko de pool (marge ×3), débloque les incs suivants.
- **Falsificateur** : le watermark lui-même ; ≤ 24 Ko → couper à 24 (+16 Ko) ;
  24-32 Ko → coupe partielle à 32 (+8 Ko, marge ~×2) ; > 32 Ko = ne pas couper, chercher
  ailleurs (gc-sections, risque linker SGL — gotcha #16).
- Réfs : backuptics-32-pool-reclaim, streaming-load-budget-and-flat-treadmill,
  boot-loop-can-be-tlsf-pool-starvation.

### 4. Ciel HW NBG0 en split (récupérer la coupe cb201c7)
- **Soustraction** : disponible = B1 porte déjà ciel+NBG3 en 1p (coexistence prouvée HW) ;
  coût de récupération = ~10-15 lignes, <200 o, 0 VRAM [M7_FEATURE_AUDIT §1]. Gain
  mesuré en 1p : +1,8-2,1 fps extérieur, ne perd jamais [HW 2026-06-28] ; en split le
  ciel est aujourd'hui du fill logiciel par vue → gain [est] du même ordre par vue
  extérieure, jamais validé HW en split.
- **Falsificateur** : A/B 2p extérieur console (fps + absence de neige B1) ; neige =
  famine de cycles en split → ranger.
- Réfs : M7_FEATURE_AUDIT §1, hw-render-path-comparison, rbg0-hw-sky-feasible.

### 5. Sonde A0 (RDBS 0x0D→0x0C) → 128 Ko de VDP2, puis N2 (sol cell ×1,67)
- **Soustraction (sonde)** : disponible potentiel = un banc VDP2 ENTIER (128 Ko) si la
  déclaration K en K_LINE est bien une sur-déclaration (SGL ne déclare RDBS=01 que pour
  K_DOT) ; coût de la sonde = 4 octets + 1 boot HW.
- **Falsificateur (sonde)** : boot 0x0C : sol RBG0 propre = 128 Ko libres ; neige = A0
  réellement consommé, fermer la question.
- **Débouché N2 (sol cell ×1,67) — chantier SÉPARÉ, pas un acquis** : couverture RBG0
  ×1,67 → moins de spans logiciels master (fill plans restant = P−pr = 1,6-3,3 ms, gain
  direct borné ; la couverture élargie réduit aussi le punch/claim CPU). ⚠ Risque
  documenté : le sol CELL 256c a NEIGÉ sur HW (3 lectures/dot) ; le fix 4bpp est NON
  COMMITÉ / EN PAUSE avec artefacts triangulaires HW-only. Falsificateur N2 propre :
  boot cell sur console — sol propre = go, neige/artefacts = ranger.
- Réfs : vdp2-second-surface-plan (pt 3), doomsrl-vdp2-capacity,
  vdp2-floor-snows-on-hardware, rbg0-cell-floor-4bpp-snow-fix.

**Mentions** (sondes à ~1 ligne, information > gain) : lire MODR bits 15-12 (révision
silicium — débloque ou enterre Pclp/HSS/EOS, le pré-clipping étant LE levier théorique
contre l'overrun d'itération) ; monter SIGHT_CACHE_TICS 4→N (T gros-WAD, 0 RAM, troque
de la fraîcheur IA) ; relevé overlay tx/bk par mode (chiffre les ~300 Ko WTEX dormants).

---

## 8. MORTS — ne pas re-proposer (verdict + référence)

| Levier | Verdict | Réf |
|---|---|---|
| wall-prep→slave | +5,8 ms PIRE, tué ×3 HW | dual-sh2-span-steal, RANK3_WALLPREP §6 |
| Offload mur VDP1→CPU/slave (WALL_PX_BUDGET, steal dynamique) | PERTE NETTE HW : budget 200k→12k px = MST 76→129 ms (pr −6 mais Bp +50) — le budget rejette les murs LOINTAINS, ~gratuits VDP1 mais chers par-pixel CPU | wall-offload-vdp1-slave-dead |
| 7 formes « 2e renderer/offload slave » | 5 mortes + 2 MARGINALES (< bruit ±6 ms : compositing, précalcul-pendant-tic) ; 8e forme lead-fill-sur-slave NON TRANCHÉE (plafond ~1 ms, défaut mode 1) | slave-offload-async-divu, SLAVE_OFFLOAD_STUDY |
| 2e renderer slave (3 formes) | ~430 Ko au-dessus des 2 Mo / mois de refactor / pipeline impossible | slave-second-renderer-bp-study |
| async-DIVU (+ Prop-1/2 sprites) | réfuté HW (dv0/dv1 dans le bruit) | slave-offload-async-divu |
| row-split plans | ~0,3 fps pire que TAS, marche dupliquée | dual-sh2-span-steal-fafling (verdict) + row-split-parked-with-w-vision |
| REC level-4 / steal-all murs | record-bound (26,4 vs 0,4 ms), supprimé | m7-slave-share-per-category |
| Pré-tessellation ×4 (BSP-split, bake, WAD-bake, PVS) | morts (12 agents adversariaux) | pretessellation-levers-dead |
| DSP-sight, DSP-projection | pas de division HW, 1 Ko data, traversal | dsp-idle-sight-tic-verdict, sprites-vdp1-dsp-verdict |
| SCU-DMA→VDP2 (blit async) | FREEZE machine, loi documentée | blit-dma-lever |
| Blit dual-CPU / slDMACopy | bus-bound, zéro gain | blit-dma-lever |
| Scratchpad 2 Ko | net-négatif mesuré | HW_MEMORY_AND_BUS §4 |
| CCR / mode cache SH-2 | cc01 lu sur HW = 4-way déjà actif, config optimale — jamais re-proposer | ccr-cache-mode-verified |
| REJECT réactivé | 45-125 Ko PU_LEVEL = aimant OOM ; sight-cache le remplace | m7-critical-path (levier B corrigé) |
| Cache composite 1p | 3-4× pire (mesuré owner) | streaming-load-budget |
| Double-buffer wtex | 364/512 Ko — impossible par l'addition | wtex-cross-frame-lock |
| Sols TEXTURÉS VDP1 par-subsecteur (ftex M1-M3) | catastrophique HW, supprimé (×3,93 pool) | vdp1-floor-optimality-levers |
| `sat_vdp1_floor=1` | réveille cross_hi → murs coupés | vdp1-manual-present-audit inc-0c |
| THINGS_TEX_SLOTS 4→6 par constante | écrase le HUD (reverté) — exige relogement | vdp1-manual-present-audit step-1 |
| Plans-VDP1 en MP | fenêtre de plot pleine, Pm 3-8 ms < machinerie (arbitré owner) | mp-4p-hw-baseline |
| Gouraud murs, demi-transparence VDP1 | morts ×2 / à proscrire | doomsrl-vdp1-capacity |
| slSynch comme modèle de frame | −1 à −2,5 fps sur les 5 spots HW, zéro bénéfice fps (⚠ « mute le SFX » était FAUX — race MVOL/KYONEX corrigée à part ; la branche vdp1-full-slsynch l'adopte exprès) | hw-render-path-comparison, slsynch-not-a-miracle-fix, slsynch-full-rewrite-branch |
| RBG1 ; plafond-dominant-1p VDP2 ; K_DOT | impasse doc ; arithmétique négative ; 1 banc entier | vdp2-second-surface-plan |
| Precache graphique / streaming par portes | infaisable en 2 Mo nu | precache-streaming-verdict |

---

## 9. Lacunes de mesure (ce que ce bilan n'a PAS pu chiffrer)

1. **tx/bk WTEX par mode** — le « tx6/26 bk0 split » est un relevé owner non consigné ;
   1 capture overlay par mode le règle.
2. **Ventilation row-2 complète en 2p ET en 3/4p** (Bw/Bp/P/M par vue) — n'existe pas ;
   et la seule ventilation 1p complète (2026-07-29) est antérieure aux fixes d'août
   (R_GetColumn 08-14, pr/DIVU 08-20) — une re-capture 1p console est due. Le b% slave
   PAR PHASE en split n'a jamais été relevé non plus.
3. **RAMCTL/CYC par mode** — ce que deviennent A0/A1 quand RBG0 est OFF (3/4p) n'est
   écrit nulle part.
4. **doom_stack high-water** — sonde jamais posée (opportunité 3).
5. **SCSP high-water** (`r` row 7) et charge 68K du driver MUS — jamais relevés.
6. **LP%/transfer-over en split** — la sonde LOPR n'a été validée qu'en 1p.
7. **Pool TLSF sous config MP réelle** — les .map sont des configs 1p ; aucun delta split.
8. **Révision silicium VDP1** (MODR) — jamais lue ; Pclp/HSS/EOS conditionnels.
9. Tous les chiffres sols-VDP1 inc-0..2c (pr, v, caps 16k/40k devinés) = **Ymir only** ;
   le present v2 lui-même attend sa re-validation console.
10. b% slave console POST-fixes MP du 08-20 — la baseline b0 % est pré-fix.
11. **Distribution zf/lg sur le corpus wads_temoins (415 cartes) post-structs 08-18** —
    jamais faite ; les « typiques » de §2.2 ne couvrent que 4 cartes (et seul MAP11 a
    un zf).
12. **Tous les chiffres CD = ODE** (SD/Phoebe/SAROO) — le modèle de coût d'un vrai
    lecteur optique 2× (celui des consoles non modifiées) est inconnu.
