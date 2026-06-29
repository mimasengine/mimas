> ✅ STATUT (2026-06-29) : ce document garde sa **valeur de référence pour la loi matérielle VDP2**
> (§0 invariants 4 banques × 8 timings + loi de la « snow » par starvation de cycle-pattern ; §2
> coûts par dot ; §5 exemples réels). Mais ses **conclusions de coexistence sont OBSOLÈTES** : le sol
> RBG0 a SHIPPÉ en **BITMAP 512×256 8bpp** (RBG0_BITMAP=1, commits 19768ca/41dd895), PROPRE sur HW.
> La « neige » était une starvation de cycle-pattern, résolue par **bitmap + RDBS=0x0D + cycles A0/A1
> parqués** (`rbg0_commit_ramctl`/`rbg0_commit_cyc`, DÉJÀ dans l'arbre — PAS slSynch). B1 est LIBRE
> (pas de map) → le sol + un ciel cell B1 peuvent coexister ; l'overlay NBG3 est un toggle L+R. Sol gaté
> potato-0 + 1 joueur. Les thèses « ciel XOR sol », « overlay incompatible », « poke CYC absent »,
> « RDBS=0x8D », « sol=software/snow » sont RENVERSÉES.
> **Réf. faisant autorité pour la réalité du sol : `docs/VDP2_RBG0_CURRENT_STATE.md`.**

# VDP2 Layer Budget — Mimas (Doom Saturn) : la loi des 4 banques VRAM (ciel, sol, overlay)

> Auteur : architecte graphique Saturn (Mimas). Public : Romain.
> Objet conservé : cartographier la **loi matérielle** d'usage du VDP2 (4 banques × 8 timings,
> coûts par dot, exemples réels de jeux). L'analyse de coexistence overlay/sol qu'il contenait est
> historique et résolue par le sol bitmap shippé — voir `docs/VDP2_RBG0_CURRENT_STATE.md`.

---

## 0. Rappel des invariants matériels (la loi, pas un réglage)

- **VRAM = 512 Ko = 4 banques × 128 Ko**, en mode 4-banques (RAMCTL bits VRAMD=8 et
  VRBMD=9 tous deux à 1). Adresses : A0=`0x25E00000`, A1=`0x25E20000`, B0=`0x25E40000`,
  B1=`0x25E60000`. Plus **4 Ko de Color RAM (CRAM)**.
  Sources : `src/dg_saturn.cxx:250,257,261`, `docs/VDP2_ARCHITECTURE.md` (sec1.4),
  `SaturnRingLib/modules/sgl/INC/sl_def.h:945-948`.
- **Chaque banque expose 8 timings d'accès T0-T7** par cycle mémoire en résolution normale
  (320/352, ce que Mimas utilise). En hi-res (640/704) il n'y en a que 4. Programmés via
  CYCA0/CYCA1/CYCB0/CYCB1 (`0x25F80010`+). Source : `docs/VDP2_ARCHITECTURE.md` sec3.
- **B0 contient TOUJOURS le framebuffer du jeu** (NBG1, bitmap 8bpp, le rendu Doom).
  Non négociable. Il reste donc **exactement 3 banques** pour tout le reste.
  Source : `src/dg_saturn.cxx:183` (DOOM_VRAM=`0x25E40000`), `docs/VDP2_ARCHITECTURE.md` sec5.
- **La loi de la panne (manuel VDP2, verbatim)** : si la lecture requise par une couche
  n'est pas planifiée dans un timing de la banque qui contient réellement ses données,
  la lecture *n'a pas lieu* et la couche se corrompt en **traînées blanches horizontales
  ("snow")** — ce n'est **pas** un repli propre vers la transparence.
  C'est exactement la neige que Romain a vue sur hardware (résolue depuis par le sol bitmap +
  cycles A0/A1 parqués — voir `docs/VDP2_RBG0_CURRENT_STATE.md`).
  Source : `docs/VDP2_ARCHITECTURE.md` sec2.

---

## 1. Note historique — le « conflit B1 » overlay vs sol (RÉSOLU)

> **OBSOLÈTE.** Ce document avait posé comme thèse centrale que l'overlay NBG3 (câblé par SRL en
> banque B1) et un sol RBG0 **cellulaire** (dont la map est forcée en banque B → B1) se disputaient
> irrémédiablement B1, rendant overlay et sol mutuellement exclusifs. **Cette thèse ne tient plus** :
> le sol shippé est un RBG0 **bitmap** sans pattern-name → **pas de map** → **B1 reste libre**.
> L'overlay NBG3 redevient un simple toggle (L+R), et un ciel cell peut même cohabiter en B1.
> Détail du layout réel : `docs/VDP2_RBG0_CURRENT_STATE.md`.

---

## 2. Cartographie VDP2 — les 4 banques × 8 cycles

> **STOCKAGE ≠ CYCLES.** Ce sont deux contraintes indépendantes :
> - **Stockage** : 128 Ko/banque, alloué linéairement.
> - **Cycles d'accès** : 8 slots T0-T7/banque. Un dot bitmap 8bpp coûte **2 lectures char**
>   (2 slots/8). Une lecture RBG0 (PN, char, ou K en mode per-dot) coûte **toute** la banque
>   (8 slots). Source : `docs/VDP2_ARCHITECTURE.md` sec3, `SaturnRingLib/saturnringlib/srl_vdp2.hpp:89-106,162-168,220-224`.

**Coût par dot en résolution normale** (`docs/VDP2_ARCHITECTURE.md:114-120`) :

| Type de couche | Lectures char/dot | Slots utilisés (sur 8) |
|---|---|---|
| Bitmap NBG 16-col (4bpp) | 1 | ~1 |
| **Bitmap NBG 256-col (8bpp)** — framebuffer & ciel | **2** | **~2** |
| Bitmap NBG 2048+/RGB direct | 4-8 | 4-8 |
| Cell NBG 256-col | 1 PN + 2 char | ~3 |
| RBG0 (chaque lecture : PN, char, K-per-dot) | toute la banque | **8 (banque entière)** |

> **Note** : l'analyse banque-par-banque de l'occupation réelle (config ciel HW, config sol RBG0
> bitmap shippé, layout exact A0/A1/B0/B1) vit désormais dans `docs/VDP2_RBG0_CURRENT_STATE.md`.
> Le tableau de coût ci-dessus reste la **loi** ; les cas concrets sont là-bas.

### 2.c CRAM (4 Ko, mode CRM16_2048 = 8 palettes de 256)

Occupation Doom (`src/dg_saturn.cxx:190,449-452`, `srl_vdp2.hpp:1487-1489,1498`) :
banque 0 ≈ palette police NBG3, banque 1 = PLAYPAL live, banques 2-7 = 6 light-banks
pré-assombris. → **Les 8 palettes sont effectivement toutes utilisées.** Conséquence directe :
mettre la table K en CRAM (CRKTE) risque de **collisionner avec la palette 8bpp** — non prouvé.

---

## 3. Coexistence overlay / sol / ciel — RÉSOLU (note historique)

> **OBSOLÈTE.** Ce document explorait 4 pistes (NBG3 vers A0, slots libres de B0, RBG0 bitmap,
> table K en CRAM/CRKTE) et concluait qu'**aucune** ne faisait cohabiter overlay + sol RBG0 de façon
> prouvée, d'où une matrice de configs jugeant la plupart « snow / infaisable ». La **piste RBG0
> bitmap** (qui supprime la map et libère B1) — alors écartée pour la bande passante — est exactement
> celle qui a **shippé** (RBG0_BITMAP=1, 19768ca/41dd895), propre sur HW : sol bitmap + ciel + overlay
> (toggle L+R) coexistent. La matrice de verdicts B/E/F/G « snow/infaisable » et la conclusion « pas de
> coexistence prouvée » sont **caduques**. Layout définitif et configs réelles :
> `docs/VDP2_RBG0_CURRENT_STATE.md`. La seule combinaison restée morte est **wall-prep→slave** (côté
> rendu, sans rapport VDP2). La piste « sol dégradé back-screen, 0 banque » (ancienne config H) reste
> un concept valable mais non poursuivi, le bitmap ayant réglé le besoin.

---

## 5. Comment les vrais jeux Saturn saturent le VDP2 — exemples concrets

### Les "tricks" du matériel
- **Coefficient en CRAM (CRKTE)** : place la table K dans la moitié haute de la CRAM
  (`0x100800-0x100FFF`, CRAM mode 1) au lieu d'une banque VRAM → la lecture coefficient
  per-line/per-dot ne consomme **plus de slot VRAM** → libère une banque entière. C'est ce qui
  laisse un sol en rotation cohabiter avec plusieurs couches de scroll.
  Source : VDP2 User's Manual ch.3.4 / ch.6.4 (`docs.exodusemulator.com/.../hard/vdp2/hon/p03_40.htm`,
  `.../p06_40.htm`). **Incompatibilité** : CRKTE exige CRAM mode 1 ; l'Extended Color Calculation
  (blending étendu) exige CRAM mode 0 → mutuellement exclusifs (SegaXtreme "VDP2 questions" p.2).
- **RBG0 bitmap** : supprime la lecture pattern-name → 2 banques au lieu de 3 (cells/bitmap + K),
  au prix d'un sol mono-image. Source : `docs/VDP2_ARCHITECTURE.md:349`, `srl_vdp2.hpp:269`.
- **Réduction (ZMCTL)** : NBG0/NBG1 seulement ; coût ×1 (none) / ×2 (1/2) / ×4 (1/4) sur les
  lectures char. 1/2 → 16/256 couleurs only ; 1/4 → 16 couleurs only ; active la réduction sur
  NBG0 désactive NBG2 (1/4 désactive NBG2+NBG3). Source : VDP2 User's Manual ch.5
  (`.../hard/vdp2/hon/p05_20.htm`).
- **Back-screen + line-color comme "couches gratuites"** : le back screen (une couleur per-line
  derrière tout, 1 lecture courte/ligne) et le line color screen (couleur per-line mélangeable)
  coûtent **0 banque de cellules**. **Sonic R** anime un line color screen sur le RBG0 pour les
  reflets d'eau ondulants. Sources : VDP2 Manual ch.7 (`.../p07_10.htm`, `.../p07_20.htm`),
  davidgamizjimenez "Sega Saturn al límite" (domaine désormais mort, via snippets de recherche),
  tcrf.net Proto:Sonic_R.
- **Ordre d'allocation RBG0-avant-NBG** : RBG0 réclame des banques entières (8 cycles), donc
  toutes les libs (SGL, SRL, Jo) lui donnent ses banques dédiées **d'abord**, puis casent les NBG
  dans les slots restants. SRL : RBG0 cell réserve les 8 cycles d'une banque (A0→A1→B0→B1),
  RBG0 map force A0 ; NBG par profondeur (16-col=1, 256-col=2, RGB=4 cycles).
  Source : `SaturnRingLib/saturnringlib/srl_vdp2.hpp:162-185,220-224`.

### Jeux et moteurs (5 couches max = NBG0+NBG1+NBG2+NBG3+RBG0)
- **Sonic R** (Travellers Tales) : RBG0 = sol/piste "3D infini" (cells 8×8, plan 128×128) +
  NBG0 background + line color screen animé = sol rotation + scroll + couche gratuite simultanés.
  Source : davidgamizjimenez (snippets), tcrf.net.
- **Panzer Dragoon Saga / Sonic 3D Blast / Street Fighter Alpha/Zero 2** : cités par l'équipe
  de l'émulateur **Ymir 0.1.5** — leurs graphismes se corrompaient tant que l'allocation de
  banques VRAM et les cycles d'accès n'étaient pas honorés exactement. **Preuve concrète** que
  les jeux shippés allouent les banques serré et dépendent du scheduler cycle-pattern.
  Source : `emu-land.net/en/news/ymir_v015`.
- **Panzer Dragoon II Zwei** : "champ sans fin" (plan rotation RBG0) pour l'eau/perspective.
- **Burning Rangers** : dessine les explosions sur VDP1 demi-res, DMA le framebuffer VDP1 dans
  la VRAM VDP2, NBG2 composite/blend → la VRAM 512 Ko sert aussi de scratch cross-VDP.
  Source : Beyond3D / sega-16 (via snippets).

### Fichiers de référence on-disk (dumps réels de cycle/RAMCTL)
- **Jo Engine** (`../DoomJo/joengine/jo_engine/vdp2.c:330-360`, `vdp2_malloc.c:62-80`) : RBG0 via
  SGL haut-niveau, **K-table+rotation-table épinglées en A1**, cells RBG0 en A0, map en B0, commit
  par `slScrAutoDisp` + **slSynch chaque frame**. Jo fonctionne *parce qu'il slSynch* → son RAMCTL/
  CYC shadow atteint toujours la puce. **Mimas ne peut pas copier ça** (pas de slSynch).
  RAMCTL Jo NOSGL connu-bon = `0x1327`. Source : `docs/VDP2_ARCHITECTURE.md:177,409-414`.
- **SGL canonique** (`SaturnRingLib/modules/sgl/INC/sl_def.h:952,964,975,979`) : CGR0_RAM=A0,
  KTBL0_RAM=A1, RBG0_MAP=B0, CGN01_RAM=B1. Toute la pile assume ces valeurs.
- **SlaveDriver-Engine (SCL)** (`saturn-refs/SlaveDriver-Engine/INITMAIN.C:262-285`, `PLAX.C:94-108`)
  : **table de cycle écrite à la main** `{0xeeee,0xeeee, 0xeeee,0xeeee, 0x44ee,0xeeee, 0x44ee,0xeeee}`
  via `SCL_SetCycleTable`, A0=table K RBG0, A1=char RBG0. `0xE`=slot idle/CPU, `0x4`=lecture
  pattern-name/char ; les banques RBG0 (A0/A1) entièrement consommées.
- **Slot codes (manuel)** : `0x0-3`=PN NBG0-3, `0x4-7`=char NBG0-3, `0x8-B`=interdit, `0xC/D`=
  vertical cell-scroll NBG0/1, `0xE`=slot CPU, `0xF`=pas d'accès. Source : VDP2 Manual ch.3
  (`.../hard/vdp2/hon/p03_35.htm`), `wiki.yabause.org/index.php5?title=VDP2`.

> **Note importante** : *aucune* des 4 références on-disk n'écrit un `slScrCycleSet` pour une
> coexistence **bitmap-Doom + RBG0** ; la seule table cycle littérale (SlaveDriver) est pour un
> RBG0 *tilemap*, pas un framebuffer 8bpp. Les nibbles exacts pour "NBG1 8bpp 512×256 en B0 +
> sol RBG0" restent **à dériver/valider**, pas à copier. **À vérifier.**

---

## 6. État résolu (note historique remplaçant §6-§8)

> **OBSOLÈTE / RÉSOLU.** Les §6-§8 d'origine recommandaient de **bloquer** le sol RBG0 texturé tant
> qu'un « poke CYCxx » manquant ne serait pas écrit, signalaient une « DISCREPANCE : `rbg0_commit_cyc`
> PAS dans l'arbre / RDBS=0x8D », et proposaient un **toggle écran-titre** (Classic / Fast floor /
> Textured floor) pour ne committer le layout qu'une fois. **Tout cela est dépassé** :
>
> - Le sol RBG0 **bitmap** a SHIPPÉ (RBG0_BITMAP=1, 19768ca/41dd895), **propre sur HW**, gaté potato-0
>   + 1 joueur. La « neige » était une starvation de cycle-pattern, **guérie** par bitmap + RDBS=**0x0D**
>   (pas 0x8D) + cycles A0/A1 **parqués**. `rbg0_commit_ramctl()` **et** `rbg0_commit_cyc()` sont
>   **bien dans l'arbre** — la « discrepance / poke absent » est fausse aujourd'hui (et n'a JAMAIS
>   reposé sur slSynch).
> - Le rappel utile reste vrai : **le gain REC (+20-33% fps) vient du SKIP du sol software**, pas du
>   RBG0 lui-même (`docs/VDP2_ARCHITECTURE.md:355-364`). Le RBG0 bitmap apporte en plus un sol texturé
>   réel sans coût CPU de span.
> - slSynch reste **abandonné** comme modèle de frame (~16% fps + mute le SFX direct-SCSP, qui possède
>   le MVOL) — mais il n'a jamais été nécessaire au sol : le commit se fait par pokes RAMCTL/CYC directs.
>
> État, layout de banques définitif et conditions de gating : **`docs/VDP2_RBG0_CURRENT_STATE.md`**.

---

### Sources principales
- Code Mimas : `src/dg_saturn.cxx` (lignes citées en ligne), `core/r_plane.c`.
- Docs repo : `docs/VDP2_ARCHITECTURE.md`, `docs/VDP2_RBG0_CURRENT_STATE.md` (réalité du sol RBG0).
- SRL/SGL : `SaturnRingLib/saturnringlib/srl_vdp2.hpp`, `srl_ascii.hpp`,
  `SaturnRingLib/modules/sgl/INC/sl_def.h`.
- Références : `../DoomJo/joengine/jo_engine/vdp2.c` + `vdp2_malloc.c`,
  `saturn-refs/SlaveDriver-Engine/INITMAIN.C` + `PLAX.C`.
- Web : VDP2 User's Manual (`docs.exodusemulator.com/.../hard/vdp2/hon/`), `wiki.yabause.org`,
  `emu-land.net/en/news/ymir_v015`, `jo-engine.org`, tcrf.net Proto:Sonic_R,
  davidgamizjimenez "Sega Saturn al límite" (domaine mort, via snippets).
