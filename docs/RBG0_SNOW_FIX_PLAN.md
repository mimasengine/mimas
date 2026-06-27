> ⚠️ SUPERSEDED (2026-06-27). Ce plan vise le chemin CELL abandonné (A0=K / A1=cells / B1=map) et traite la neige comme non résolue. RÉALITÉ : le sol SHIPPE en BITMAP 512x256 8bpp en A1, K/coeff en A0, fb en B0, B1 LIBRE (RPT seul) — 2 banques, pas de map ; PROPRE sur HW (commits 19768ca/41dd895). La neige était la STARVATION du motif de cycle (3e read map du chemin cell), corrigée par le bitmap + RDBS=0x0D + cycles A0/A1 parqués 0xEEEE + block-flush du shadow (rbg0_commit_cyc, DÉJÀ dans l'arbre). Larguer la map a LIBÉRÉ B1 → « floor XOR sky » est morte (vrai arbitrage = ciel-vs-NBG3). Le « fix » CYCB0=0x55EE, le plan de test HW et l'« inconnue commit-gap » sont historiques. Garder seulement la mécanique RDBS / invalidation-de-banque / park-0xEEEE. Voir docs/VDP2_RBG0_CURRENT_STATE.md.

# RBG0 — Plan d'implémentation : tuer la snow du sol VDP2 texturé

> Cible : faire **AFFICHER SANS SNOW** sur Saturn réelle la config
> **ciel software + sol RBG0 cell texturé** (`VDP2_RBG0_TEST=1`, `VDP2_HW_SKY=0`),
> layout 4 banques **A0=K / A1=cells / B0=framebuffer / B1=map**.
> Le seul geste manquant = committer le **motif de cycle CYCxx** (+ RAMCTL) sans `slSynch`.
>
> À lire **avec** [`VDP2_FLOOR_CONSOLIDATION.md`](VDP2_FLOOR_CONSOLIDATION.md) (la stratégie
> sol/banques, le « sky XOR floor ») et [`VDP2_LAYER_BUDGET.md`](VDP2_LAYER_BUDGET.md)
> (le conflit overlay NBG3 ↔ banque B1, pourquoi on débogue à l'aveugle).
>
> Numéros de ligne **vérifiés contre l'arbre courant** `src/dg_saturn.cxx` le 2026-06-26
> (les findings citaient un arbre décalé d'env. +9 lignes ; ci-dessous = les vraies lignes).

---

## 1. La cause racine, tranchée

**La snow n'est PAS un problème de valeurs CYC. C'est un problème de MÉCANISME de commit.**
Trois sous-faits, hiérarchisés du plus prouvé au plus dérivé :

1. **(PROUVÉ — manuel + SlaveDriver) Les banques rotation A0/A1/B1 sont DON'T-CARE pour le CYC.**
   Le manuel VDP2 (p03_34) dit verbatim : une banque assignée au moteur de rotation via
   **RDBS** (pattern-name / character / coefficient) voit son **registre de motif de cycle
   INVALIDÉ** — la rotation lit la banque quoi qu'il arrive. Donc poker A0/A1/B1 à
   `0xFFFFFFFF` (idle) **n'était pas la cause** de la snow : ces banques ignorent le CYC.
   La référence shippée SlaveDriver-Engine (`INITMAIN.C` `setVDP2`) park ses banques rotation
   à `0xEEEEEEEE` (CPU), pas `0xFF`, mais c'est cosmétique côté rotation.

2. **(PROUVÉ par déduction) La seule banque qui DOIT être correcte est B0 = le framebuffer
   NBG1 8bpp.** Un bitmap 256 couleurs réclame **2 lectures character par dot** (code `0x5`
   pour NBG1). Le poke `0xFFFFFFFF` mettait **B0 = idle sur les 8 timeslots** → le fetch du
   framebuffer n'était jamais planifié → **la banque du framebuffer est affamée** → barres
   (latch de bus périmé) puis noir. **C'est ça qui a tué l'image** : pas les banques rotation,
   mais B0 mis à idle. Le symptôme « barres puis noir » colle exactement à une banque
   framebuffer privée de ses 2 reads.

3. **(LE BLOCAGE RÉEL — mécanisme de commit, et c'est là que les findings divergent) :
   le CYC poké à la puce ne reste pas, parce qu'un poke one-shot seul est, sur HW, soit
   jamais appliqué (slSynch supprimé) soit ré-écrasé chaque vblank par l'ISR SGL.**
   Mimas tourne **sans `slSynch`** (`SRL_FRAMERATE=0` → `slDynamicFrame(ON)`, Makefile:15).
   SGL ne flushe `RAMCTL`/`CYCxx` de son shadow vers la puce **qu'à l'intérieur de `slSynch`**
   *ou* via son ISR VBlank-In (selon la lecture). **Les findings ne s'accordent pas** sur ce
   que l'ISR fait des registres scroll/CYC :
   - Findings 1 & 2 + le commentaire en place (`dg_saturn.cxx:1542`) : l'ISR re-pushe
     **BGON/scroll mais PAS RAMCTL/CYC** → un poke one-shot à la puce **devrait coller**.
   - Finding 3 : l'ISR `_BlankIn` SCU-DMA un bloc de **144 octets** (0x00–0x8F) du shadow
     `0x060FFCC0` vers `0x25F80000` **chaque vblank**, ce qui **inclut RAMCTL (0x0E) et tous
     les CYC (0x10–0x1E)** → le poke one-shot est **ré-écrasé en ~16 ms** par le shadow périmé.

**Verdict tranché :** la cause de la snow = **le motif de cycle correct n'atteint jamais la
puce de façon stable** (commit-gap), **pas** des valeurs CYC fausses. Le poke `0xFFFFFFFF` a
échoué pour **deux** raisons cumulées : (a) il blanchissait B0 (erreur de valeur sur la seule
banque qui compte), et (b) même corrigé, un poke one-shot nu risque d'être clobberé par l'ISR.
La divergence finding-1/2 vs finding-3 (« l'ISR touche-t-il CYC ? ») est **l'inconnue
décisive** — et elle se résout par UNE observation HW : poker un CYC reconnaissable, puis le
**relire à la puce N frames plus tard** (cf. §3 readout, §5 Étape 3). Si la relecture dérive →
finding 3 a raison (poke par-frame ou shadow-coherent requis) ; si elle tient → finding 1/2.

> **Conséquence de design** : on choisit d'emblée le mécanisme qui est **robuste dans les DEUX
> cas** (shadow-coherent), pour ne pas dépendre de cette inconnue. Voir §2.

---

## 2. Le commit du cycle-pattern — LA solution recommandée

**Chemin retenu = (b+a) « shadow-coherent » : écrire les bonnes valeurs DANS le shadow SGL
ET à la puce.** Robuste que l'ISR re-pushe le shadow (finding 3 : alors le shadow re-pousse
*nos* valeurs, c'est une feature) ou non (findings 1/2 : alors notre poke puce tient). On
**n'appelle JAMAIS `slSynch`** (poison documenté, `VDP2_FLOOR_CONSOLIDATION.md` §1.5).

### Les valeurs à écrire (dérivées, NON encore validées HW — étiqueté)

Layout A0=K / A1=cells / B0=fb / B1=map. Format = mot 32 bits empaqueté `(L<<16)|U`,
chiffre hexa de gauche = timeslot T0. Code par nibble : `0x5`=read character NBG1,
`0xE`=CPU/idle, `0xF`=no-access.

| Banque | Rôle | Mot CYC 32-bit | Justification |
|---|---|---|---|
| **B0** (`fb`) | framebuffer NBG1 8bpp | **`0x55EEEEEE`** | **LOAD-BEARING.** T0=5,T1=5 = les 2 reads obligatoires du bitmap 8bpp ; T2–T7=0xE. **C'est le seul mot qui tue la snow.** (SlaveDriver met ses 2 reads en T0/T1 → on copie ce placement.) |
| **A0** (`K`) | coefficient (rotation) | `0xEEEEEEEE` | CYC **invalidé** (RDBS). Park CPU comme SlaveDriver. |
| **A1** (`cells`) | character (rotation) | `0xEEEEEEEE` | CYC invalidé. Park CPU. |
| **B1** (`map`) | pattern-name (rotation) | `0xEEEEEEEE` | CYC invalidé. (C'est l'ancienne banque NBG3 → l'overlay meurt ici, attendu.) |

> **Honnêteté** : seul `CYCB0=0x55EEEEEE` est *fortement* étayé (manuel + SlaveDriver). Le
> placement exact des 2 reads (T0/T1 vs n'importe où) n'est pas pinné à 100 % pour le mode
> bitmap → on copie SlaveDriver (T0/T1) par sécurité. `0xEEEEEEEE` sur les banques rotation est
> *cosmétique* (CYC invalidé) — `0xFFFFFFFF` marcherait aussi, mais `0xE` est la valeur
> SGL-prouvée. **À vérifier sur HW.**

### Où écrire (adresses, toutes confirmées dans les findings)

**Côté puce** (base `0x25F80000`) :

| Reg | Adresse | Valeur |
|---|---|---|
| RAMCTL | `0x25F8000E` | `(before & 0xFC00) | 0x0300 | 0x008D` *(déjà fait par `rbg0_commit_ramctl`)* |
| CYCA0L | `0x25F80010` | `0xEEEE` |
| CYCA0U | `0x25F80012` | `0xEEEE` |
| CYCA1L | `0x25F80014` | `0xEEEE` |
| CYCA1U | `0x25F80016` | `0xEEEE` |
| CYCB0L | `0x25F80018` | **`0x55EE`** |
| CYCB0U | `0x25F8001A` | `0xEEEE` |
| CYCB1L | `0x25F8001C` | `0xEEEE` |
| CYCB1U | `0x25F8001E` | `0xEEEE` |

**Côté shadow SGL** (High Work RAM, membre `sglK01.o` de `LIBSGL.A`) — on mirrore les MÊMES
valeurs pour survivre à un éventuel re-push ISR :

| Symbole (extern C) | Adresse | Mirror de |
|---|---|---|
| `VDP2_RAMCTL` | `0x060FFCCE` | RAMCTL |
| `VDP2_CYCA0L` | `0x060FFCD0` | CYCA0L |
| `VDP2_CYCA0U` | `0x060FFCD2` | … |
| `VDP2_CYCA1L` | `0x060FFCD4` | … |
| `VDP2_CYCA1U` | `0x060FFCD6` | … |
| `VDP2_CYCB0L` | `0x060FFCD8` | **`0x55EE`** |
| `VDP2_CYCB0U` | `0x060FFCDA` | … |
| `VDP2_CYCB1L` | `0x060FFCDC` | … |
| `VDP2_CYCB1U` | `0x060FFCDE` | … |

Ces symboles se linkent **gratuitement** contre `LIBSGL.A` (SRL les déclare déjà :
`srl_base.hpp:13` pour `VDP2_RAMCTL`, le Sample « VDP2 - Layers » `main.cxx:6` pour les
`VDP2_CYCA0L..CYCB1U`). En C/C++ : `extern "C" uint16_t VDP2_CYCB0L;` (sans underscore ; le `_`
que montre `nm` est le mangling SH ELF).

### One-shot vs per-frame

**One-shot au `DG_Init`**, juste après `rbg0_commit_ramctl()` (`dg_saturn.cxx:1544`). Parce que
le shadow est écrit en même temps, **un seul commit suffit** : si l'ISR re-pushe, il re-pousse
*nos* valeurs ; s'il ne touche pas CYC, notre poke puce tient. Pas de coût par-frame.

> **Note RAMCTL** : `rbg0_commit_ramctl()` ne poke aujourd'hui que la **puce** `0x25F8000E`,
> pas le shadow. Si le finding 3 a raison, RAMCTL est lui aussi clobberé chaque frame →
> **ajouter** `VDP2_RAMCTL = v;` (shadow) dans `rbg0_commit_ramctl`. C'est 1 ligne, gratuit,
> et ça rend RDBS robuste pour la même raison que CYC. **À faire dans le même change-set.**

### Fallback si ça ne colle pas

1. **FAIL par clobber** (relecture puce dérive après N frames, snow persiste) → déplacer le
   poke shadow+puce dans la boucle de frame (juste avant la présentation, près de
   `dg_saturn.cxx:2846`). Coût : 9 stores 16-bit/frame, négligeable.
2. **FAIL valeurs** (relecture tient à `0x55EE…` mais snow persiste) → le problème est ailleurs
   que CYC : revérifier que `slBitMapNbg1`/CHCTLA de NBG1 n'a pas été re-dirigé par
   l'activation de RBG0ON, et tester un B0 alternatif (les 2 reads sur d'autres timeslots).
3. **Surtout PAS** : `slScrCycleSet` (shadow-only même problème de flush, doc §1.5) ni `slSynch`
   (poison). Ne pas non plus tenter de désactiver l'ISR DMA via `PauseFlag`/`SynchCount` : ça
   gèlerait aussi le flush NBG0/NBG1 (sky/fb figés) — le shadow-coherent est plus sûr.

---

## 3. Le canal de debug qui survit (framebuffer)

L'overlay NBG3 **meurt** dès que B1 devient pattern-name de RBG0 (il partageait B1). On écrit
donc le readout **en pixels directement dans le framebuffer NBG1** (`DOOM_VRAM` = bank **B0**),
la seule banque que RBG0 ne touche pas → ça survit au commit.

### Cible & contraintes (toutes confirmées)

- `DOOM_VRAM = 0x25E40000` (B0), stride `512`, 8bpp. Pixel `(x,y)` = `DOOM_VRAM[y*512 + x]`.
  Image visible 320×224, rows 0..223. **VRAM non-cachée → stores directs, aucun flush** (cf.
  `dg_saturn.cxx:74`). `VIEW_Y_OFFSET = 0` (`:201`), donc pas de décalage à appliquer.
- **Piège palette** : NBG1 lit la **CRAM bank 1** (`slBMPaletteNbg1(1)`). À l'instant du
  readout (dans `DG_Init`), la **PLAYPAL de Doom n'est PAS encore chargée** → « index 4 = blanc »
  n'est pas fiable. **Fix** : on poke nos propres entrées CRAM avant de dessiner :
  - `CRAM_DOOM_PAL[1]  = 0x8000` (noir opaque, fond de la boîte readout)
  - `CRAM_DOOM_PAL[254] = 0xFFFF` (blanc opaque = `0x8000|(31<<10)|(31<<5)|31`, les glyphes)
  - `CRAM_DOOM_PAL = 0x25F00200` (base bank-1). Doom écrasera ces entrées plus tard — sans
    importance, le readout est lu et le jeu démarré d'ici là.

### Le blitter (auto-contenu, ~40 lignes — il N'EXISTE PAS de font pixel dans ce fichier)

- Petite table de glyphes **8×8**, 1 octet/row : 16 entrées `'0'..'F'` (+ optionnel quelques
  lettres `R A C Y B` pour labels). Draw : pour chaque nibble, `glyph[nib]`, pour `r` 0..7,
  `c` 0..7, si `(glyph[r]>>(7-c))&1` → `DOOM_VRAM[(py+r)*512 + px+c] = 254`.
- **Ne PAS réutiliser la font Doom** (`V_DrawPatch`/`hu_font`) : elle exige le WAD chargé, les
  lumps, `screens[]` et une palette vivante — rien de garanti à `DG_Init`.

### Layout du readout (5 lignes, marge gauche px=4, pitch 10px, py0=4)

D'abord `memset` une bande opaque : `DOOM_VRAM` rows 0..63 → index **1** (boîte noire, pour ne
pas voir la snow RBG0 derrière). Puis :

| Ligne | Contenu | Lecture |
|---|---|---|
| 0 | `RAMCTL` before \| after (2×4 hex) | RDBS a-t-il pris `..8D` ? |
| 1 | `CYCA0` before \| after (2×8 hex) | banque rotation |
| 2 | `CYCA1` b \| a | |
| 3 | **`CYCB0` b \| a** | **le mot qui compte** — doit lire `55EEEEEE` après |
| 4 | `CYCB1` b \| a | |

Snapshot **avant** (juste avant `rbg0_commit_ramctl()`) et **après** (juste après le poke CYC) en
lisant les regs puce `0x25F80010..1E` (32-bit = `U<<16 | L`) + RAMCTL `0x25F8000E`.

### Hold (idiome déjà présent)

`unsigned int t = vbl_count; while (vbl_count - t < 180) ;` ≈ 3 s @60Hz (`vbl_count` incrémenté
par `vblank_handler`, `dg_saturn.cxx:853`, tourne sans slSynch). Le readout reste figé : SGL ne
touche jamais le contenu bitmap de B0.

### Hook

Appel `rbg0_draw_debug_readout()` **immédiatement après `rbg0_commit_ramctl()` /
`rbg0_commit_cyc()`** au `dg_saturn.cxx:1544`, sous `#if VDP2_RBG0_TEST`.

> **Priorité** : NBG1 a priorité 6, RBG0 priorité 4 → la boîte B0 gagne sur la snow RBG0.
> Si jamais un mode debug remonte RBG0 au-dessus de NBG1, dessiner pendant que NBG1 est au top.

---

## 4. Le change-set minimal

Toutes les lignes ci-dessous = **arbre courant vérifié** (pas les lignes des findings).

### 4.1 Flip du build (config sol texturé)

- `dg_saturn.cxx:248` : `#define VDP2_RBG0_TEST 0` → **`1`**
- `dg_saturn.cxx:261` : `#define VDP2_HW_SKY 1` → **`0`**

Cela bascule automatiquement `RBG0_KTAB_VRAM` vers `0x25E00000` (A0, libéré par le ciel
software ; voir le `#if VDP2_HW_SKY` autour de `:271`/`:273`) et active le path `sat_vdp2_sky=0`
(`:1549`), `sat_vdp2_floor=1` au boot.

### 4.2 Ajouter le commit CYC + shadow RAMCTL

Dans `rbg0_commit_ramctl()` (`dg_saturn.cxx:1326-1335`), **ajouter le mirror shadow de RAMCTL**
après le poke puce :
```c
extern "C" uint16_t VDP2_RAMCTL;   /* SGL shadow @0x060FFCCE */
...
*RAMCTL = v;
VDP2_RAMCTL = v;          /* shadow-coherent : survit à un éventuel re-push ISR */
ramctl_after = *RAMCTL;
```

Ajouter une **nouvelle fonction `rbg0_commit_cyc()`** juste après (vers `:1336`), sous
`#if VDP2_RBG0_TEST` :
```c
static void rbg0_commit_cyc(void)
{
    /* puce */
    volatile uint16_t *const C = (volatile uint16_t *)0x25F80010;
    /* shadow SGL (sglK01.o) */
    extern "C" uint16_t VDP2_CYCA0L, VDP2_CYCA0U, VDP2_CYCA1L, VDP2_CYCA1U,
                        VDP2_CYCB0L, VDP2_CYCB0U, VDP2_CYCB1L, VDP2_CYCB1U;
    static const uint16_t v[8] = {
        0xEEEE, 0xEEEE,   /* CYCA0L/U  A0=K  (CYC invalidé, park CPU) */
        0xEEEE, 0xEEEE,   /* CYCA1L/U  A1=cells */
        0x55EE, 0xEEEE,   /* CYCB0L/U  B0=fb : T0=5,T1=5 = 2 reads 8bpp -- LOAD-BEARING */
        0xEEEE, 0xEEEE,   /* CYCB1L/U  B1=map */
    };
    for (int i = 0; i < 8; ++i) C[i] = v[i];
    VDP2_CYCA0L=v[0]; VDP2_CYCA0U=v[1]; VDP2_CYCA1L=v[2]; VDP2_CYCA1U=v[3];
    VDP2_CYCB0L=v[4]; VDP2_CYCB0U=v[5]; VDP2_CYCB1L=v[6]; VDP2_CYCB1U=v[7];
}
```
> Note : `extern "C"` ne peut pas envelopper une liste de variables dans un corps de fonction de
> cette manière selon le compilo — si ça râle, déclarer les 8 `extern "C" uint16_t VDP2_CYCxx;`
> en **portée fichier** (à côté du `extern uint16_t VDP2_RAMCTL;` existant de SRL).

### 4.3 Appeler les commits + le readout

Au site `dg_saturn.cxx:1540-1545`, étendre le bloc `#if VDP2_RBG0_TEST` :
```c
#if VDP2_RBG0_TEST
    rbg0_commit_ramctl();
    rbg0_commit_cyc();              /* NOUVEAU : motif de cycle, shadow-coherent */
    rbg0_draw_debug_readout();      /* NOUVEAU : hex dump dans le framebuffer + hold 3s */
#endif
```

### 4.4 Ajouter le readout framebuffer

Nouvelle fonction `rbg0_draw_debug_readout()` (sous `#if VDP2_RBG0_TEST`, avant `DG_Init`) :
table glyphe 8×8, `memset` bande rows 0..63→index 1, poke `CRAM_DOOM_PAL[1]=0x8000` /
`[254]=0xFFFF`, lit `0x25F8000E/10..1E` (after) — les `before` viennent de snapshots pris juste
avant `rbg0_commit_ramctl` (réutiliser/étendre les statics `ramctl_before` `:292` ; ajouter des
statics `cyc_before[4]`), dessine 5 lignes, hold `while (vbl_count - t < 180)`.

### 4.5 Core

**Aucune modif `core/` nécessaire** pour la snow elle-même. Le skip software du sol
(`sat_vdp2_floor`) et le classifieur sont déjà en place (cf. `VDP2_FLOOR_CONSOLIDATION.md` §0/§4)
et orthogonaux au commit CYC.

---

## 5. Le plan de test HW (ordonné, risque croissant)

**Ne JAMAIS appeler `slSynch` à aucune étape.** Chaque étape = une observation pass/fail nette.

### Étape 0 — Baseline known-good
- Build shippé (`VDP2_RBG0_TEST=0`, `VDP2_HW_SKY=1`).
- **PASS** : ciel HW propre, **pas de snow**, jeu nominal. Référence avant modif.

### Étape 1 — Ciel software seul, RBG0 OFF (isoler le layout)
- `VDP2_HW_SKY=0`, `VDP2_RBG0_TEST=0`. A0 libéré, ciel software.
- **PASS** : ciel software propre, **pas de snow** → libérer A0 ne casse rien.
- **FAIL** : problème hors RBG0, à isoler avant d'aller plus loin.

### Étape 2 — RBG0 + RDBS seul, readout actif, **CYC commenté** (reproduire la snow connue)
- `VDP2_RBG0_TEST=1`, `VDP2_HW_SKY=0`, commenter `rbg0_commit_cyc();` mais **garder
  `rbg0_draw_debug_readout()`**.
- **Lire la boîte framebuffer** (l'overlay NBG3 est mort, c'est NORMAL — d'où le readout B0) :
  ligne 0 `RAMCTL after` doit finir par `..8D` ; ligne 3 `CYCB0 after` lit la valeur SGL par
  défaut (souvent `..EEEE`, **pas** `55EE`).
- **Observation attendue** : **snow présente** (RDBS seul insuffisant, B0 sans ses 2 reads
  programmés explicitement). Confirme que le readout B0 est **lisible malgré la snow**.

### Étape 3 — **LE test clé** : commit CYC shadow-coherent
- Réactiver `rbg0_commit_cyc();`. `VDP2_RBG0_TEST=1`, `VDP2_HW_SKY=0`.
- **Lire ligne 3 `CYCB0 after` = `55EEEEEE`** (le poke a pris) et ligne 0 `RAMCTL ..8D`.
- **PASS** : **la snow DISPARAÎT**, ciel software propre. → mécanisme + valeurs OK.
- **FAIL-clobber** : snow persiste ET (sur un build de diagnostic) la relecture de `CYCB0`
  quelques frames plus tard a **dérivé** loin de `55EE` → l'ISR re-pushe → **Étape 3b**.
- **FAIL-valeur** : snow persiste alors que `CYCB0 after == 55EEEEEE` et tient → valeurs/banques
  → revérifier B0 (autre placement des 2 reads) et que NBG1 CHCTLA n'a pas bougé.

### Étape 3b — (si clobber) poke par-frame
- Déplacer `rbg0_commit_cyc()` (shadow+puce) dans la boucle frame (avant présentation,
  ~`dg_saturn.cxx:2846`).
- **PASS** : snow disparaît → confirme que l'ISR re-poussait CYC (finding 3). Le coût/frame est
  négligeable.

### Étape 4 — Qualité du sol RBG0 (si Étape 3/3b PASS)
- Le plan converge-t-il sur l'horizon Doom ? (`slRotX(0x4000+RBG0_PITCH)`,
  `slRotZ(-(viewangle>>16)+RBG0_YAW_OFF)`). Suit-il le joueur ? Texture 1:1 ? fps tenu ?
- **PASS** : sol texturé-perspective cohérent, transparaît sous murs/sprites (occlusion par
  priorité 4 < 5 < 6), pas de tearing.

### Étape 5 — Delta perf
- Comparer `REC`/`P` avec sol RBG0 vs baseline software aux mêmes spots.
- **PASS** : `P` chute (cible −17 à −26 ms au pot0 ; surtout gain qualité au pot1/2).

**État final visé** : sol RBG0 texturé visible, **aucune snow**, ciel software propre.

> Si le temps manque : prioriser **Étape 3** (le seul test décisif du chemin VDP2).

---

## 6. Risques & inconnues

1. **L'ISR SGL re-pushe-t-il CYC/RAMCTL chaque frame ? — à vérifier sur HW.**
   C'est LE désaccord entre finding 3 (oui, DMA 144 octets) et findings 1/2 + commentaire en
   place (non, seulement BGON/scroll). Le **commit shadow-coherent (§2) est robuste dans les
   deux cas**, donc on n'est pas bloqué — mais la relecture puce N-frames (Étape 3) tranchera et
   dira si l'Étape 3b (per-frame) est nécessaire. *à vérifier sur HW.*

2. **Le placement des 2 reads de B0 et la suffisance de `0x55EEEEEE` — dérivé, à vérifier sur HW.**
   `CYCB0` doit avoir 2 slots `0x5` ; on copie T0/T1 de SlaveDriver, mais la contrainte exacte
   de placement pour un bitmap 8bpp n'est pas 100 % pinnée. Si snow persiste avec `CYCB0` qui
   tient bien à `55EE` → tester d'autres placements. *à vérifier sur HW.*

3. **Une 2e cause de snow au-delà de CYCB0 ? — à vérifier sur HW.**
   Activer RBG0ON a pu pousser SGL à re-écrire en shadow un registre NBG1 (CHCTLA / base bitmap)
   jamais flushé. Le framebuffer était HW-good en config fb+sky, donc peu probable, mais à
   exclure si l'Étape 3 FAIL-valeur. *à vérifier sur HW.*

> Inconnue secondaire (hors snow, déjà notée dans la consolidation) : **CRKTE (K-table en CRAM)**
> serait la seule voie « ciel HW **ET** RBG0 » mais risque collision palette 8bpp dans 4 KB de
> CRAM — **pas pour cette session**, le chemin retenu est ciel software (A0 libre pour la K-table).
