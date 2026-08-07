# Audit M7 — ce qui n'est pas actif dans le mode shippé

> ## ✅ SUITE DONNÉE LE 2026-08-02 — la coupe complète a été exécutée
>
> Décision du propriétaire après lecture de cet audit : **couper plutôt que réparer**. Exécuté le
> jour même, build vert, **−1455 lignes** dans `src/dg_saturn.cxx`.
>
> | Supprimé | Pourquoi |
> |---|---|
> | **M5_CONVEX** + toute la machinerie `SAT_FLOOR_TEX` / `SAT_VDP1_FLOOR` / `VDP1_FLOOR_TEST` / `SAT_FLOOR_PERFSIM` | `sat_vdp1_floor` valait **0 dans tous les modes atteignables** (`hook_consult = (M==M5)`, anneau = `{M7}`) : compilé, jamais exécuté. Quatre captures HW escalier avaient déjà réglé M5 en négatif. |
> | **Pad R+X** (DEPORT-PREVIEW) | Seul consommateur vivant de la machinerie ci-dessus — il la maintenait compilée à lui seul. Mesurait un gisement déjà tranché négatif. |
> | **Pad L+X** (arme hors VDP1) | Trois raisons indépendantes : état ON glitché (murs non re-effacés, confirmé HW), levier de *fill* arme mesuré inerte le 2026-07-25, et `sat_apply_iso` le remettait à 0 à chaque accord SQ. |
> | **Le HOLD de paire cohérente** dans `vdp1_wpn_kick` | Il protégeait une paire murs+sols que M7 ne construit jamais ; le correctif de latence du 2026-07-31 le contournait déjà à chaque frame. Devenu chemin unique. |
> | Overlay **ligne 19** (`DEP`), champ **`fbf`** de la ligne 8, contention compile-time du **pad Y** | Suivent leurs mécanismes. |
>
> **Gains mesurés** — pool TLSF **4 976 → 19 568 octets (×3,93)** ; **16 Ko de VRAM VDP1 libérés**
> (les deux bancs de commandes F, `0x25C7C000..0x25C80000`, aujourd'hui non réclamés).
> **Vérification** : recompilation à `-Wall` du fichier avant/après → **zéro warning nouveau**
> (deux ont disparu). ⚠️ **Pas encore validé sur vrai Saturn** : le chemin de présentation VDP1 a été
> restructuré (le `return` anticipé est devenu le seul chemin).
>
> Les trois bugs du §2 restent : **§2.1 est résolu par construction** (`sat_apply_iso` n'écrase plus
> rien d'observable, R+X n'existe plus) ; **§2.2 (porte SQ_LD)** et **§2.3 (esclave interdit en MP)**
> sont **toujours ouverts**. La section §1 (ciel HW en 2p) est **inchangée**.

> Rendu le 2026-08-02, branche `flicker-clean`. Question posée : *« Y a-t-il d'autres implémentations
> du mode M5 arbitrairement coupées lors du passage à M7 sans m'avertir ? Liste-moi les fonctionnalités
> qui ne sont pas activées en M7. »*
> Méthode : `grep` exhaustif `src/` **et** `core/`, `git log`/`git blame` sur chaque gate, désassemblage
> de `build/Mimas.elf` (`sh2eb-elf-objdump` / `nm --target=coff-sh`) pour distinguer *désactivé* de
> *inatteignable*. Étiquettes **CODE** / **GIT** / **INFÉRÉ** / **NON_VÉRIFIÉ**.

---

## 0. Réponse en cinq lignes

1. **M5 n'a pas été coupé par M7.** Il n'a **jamais** figuré dans l'anneau pad-Z : `db17b04`
   (2026-07-05) crée le ring `{M0, M4, M6}` — **neuf jours avant que M7 existe**. Son code est
   toujours compilé (`cmp/eq #2,r0` présent dans `sat_apply_mode` au désassemblage).
2. **Une seule vraie coupe M7 silencieuse au message** : le ciel HW en split. Votre soupçon initial
   était juste, mais c'est **le seul cas**.
3. **Le patron dominant n'est pas le silence, c'est l'enfouissement** : des désactivations réelles
   dans le corps de commits dont le *titre* annonce autre chose.
4. **Les deux dates les plus destructrices sont antérieures à M7** (2026-06-30 et 2026-07-05).
5. **Le pire dégât n'est pas une coupe M7** : `b33c47c` (2026-07-27), **huit jours après** le park,
   casse trois choses d'un coup — dont votre levier L+X.

---

## 1. Le ciel HW en 2 joueurs

**Gate** — [dg_saturn.cxx:7028-7031](../src/dg_saturn.cxx#L7028-L7031) :

```c
int hwsky_split = (hwsky_split_on && sat_local_players >= 2 && gamestate == GS_LEVEL
                   && !automapactive && !sat_lowres);
```

**Raison, verbatim dans le code** :
> « M7: the NBG0 sky is un-zoomed full-320 while the NBG1 view is x2-zoomed → horizontal misalignment;
> fall back to the software sky, which packs into fb[0,160) + zooms WITH the view. »

**Cause racine — CODE, vérifiée** : seul NBG1 est mis à l'échelle (`slScrScaleNbg1`,
[:3555](../src/dg_saturn.cxx#L3555) et [:7507](../src/dg_saturn.cxx#L7507)). `slScrScaleNbg0` **existe
dans SGL** (`sl_def.h:1785`) et **n'est jamais appelé**. Le scroll du ciel est un décalage de bits
**constant** ([:7055](../src/dg_saturn.cxx#L7055)) : `sx = -(skyang >> (SKY_ANGLESHIFT + SKY_PARALLAX_SHIFT))`.

**En 1 joueur, le ciel HW est INTACT en M7** — `show_sky` ([:7093](../src/dg_saturn.cxx#L7093)) n'a pas
de gate `!sat_lowres`, `sat_vdp2_sky = 1` via `rbg0_want` ([:931](../src/dg_saturn.cxx#L931)),
`cell_floor_live = 0` (kind BITMAP par défaut, [:555](../src/dg_saturn.cxx#L555)). La coupe ne vise que
le split.

**Justifié ?** Techniquement oui, le désalignement est réel. Mais **prudentiel plus que technique** :
la fonctionnalité n'avait **jamais** été validée sur vrai Saturn
(`docs/TOGGLE_AUDIT.md:92` : « NOT yet validated on real Saturn »). **On n'a pas perdu un acquis, on a
refermé un chantier ouvert.**

**Date — GIT** : le `&& !sat_lowres` est posé par **`cb201c7` (2026-07-15)**, **la veille** du passage
de M7 en défaut (`f7114cf`, 2026-07-16). La fonctionnalité était née le 2026-07-01 (`9c240e8`) : elle a
vécu **14 jours**. Le message de `cb201c7` (« per-view SQ toggles + VDP1 budget reservation + SPL probe »)
**ne prononce jamais les mots sky/NBG0** ; c'est documenté uniquement dans `docs/LOWRES_RENDER_STUDY.md:236`,
ajouté par le même commit.

**Récupérable — ~10-15 lignes, <200 o, 0 VRAM** : appeler `slScrScaleNbg0(toFIXED(2.0), toFIXED(1.0))`
quand `hwsky_split && sat_lowres` (et 1.0 sinon, pour protéger le 1p qui marche), halver le pas de scroll
[:7055](../src/dg_saturn.cxx#L7055), retirer le `&& !sat_lowres`. La fenêtre W0
([:3086](../src/dg_saturn.cxx#L3086)) est déjà en coordonnées **écran** — inchangée. Aucun repack des
cellules nécessaire.

> **Trois réserves que je vous dois avant que vous n'y passiez du temps** : (a) jamais validé sur HW —
> c'est rouvrir un chantier ; (b) le ciel deviendra *chunky* ×2 ; (c) **NON_VÉRIFIÉ** : le désalignement
> pourrait être **antérieur à M7** (une demi-vue porte le même FOV 90° sur 160 px alors que le scroll
> est un shift constant), auquel cas le zoom seul ne suffira pas. **À prototyper avant de promettre.**

---

## 2. Les trois choses plus graves — et aucune n'est liée à M7

### 2.1 🔴 `sat_apply_iso` écrase `sat_things_hw` — **votre levier L+X est désarmé**

`sat_apply_mode` écrit `sat_things_hw` à la ligne [935](../src/dg_saturn.cxx#L935), et le
DEPORT-PREVIEW l'écrit à [960](../src/dg_saturn.cxx#L960) — puis sa **dernière instruction**
([:1012](../src/dg_saturn.cxx#L1012)) appelle `sat_apply_iso()`, dont le `default:`
([:1028](../src/dg_saturn.cxx#L1028)) **reforce `sat_things_hw = 1`**.

**Et `sat_apply_mode()` est appelé par CHAQUE changement SQ** — j'ai vérifié les sites d'appel :
[:8062](../src/dg_saturn.cxx#L8062) (SQ mur), [:8171](../src/dg_saturn.cxx#L8171) (SQ plancher),
[:8182](../src/dg_saturn.cxx#L8182) (SQ plafond), [:8195](../src/dg_saturn.cxx#L8195) (SQ sprite),
[:7973](../src/dg_saturn.cxx#L7973) (R+X preview), [:7838](../src/dg_saturn.cxx#L7838),
[:7907](../src/dg_saturn.cxx#L7907).

**Conséquences :**
- **Le DEPORT-PREVIEW (R+X)** pose `sat_things_hw = 0` pour désarmer l'interlock VRAM
  ([:950](../src/dg_saturn.cxx#L950), [:956](../src/dg_saturn.cxx#L956)), se fait écraser, et tourne
  donc **en violation de l'invariant documenté** ligne 946 (« they would silently corrupt VDP1 VRAM »).
- **M0 et M6 perdent leur seule différence** (sprites software) : même dé-parkés, ils ne rendraient
  rien de différent de M4.

**Nuance importante que l'audit n'avait pas faite** : la ligne [:1032](../src/dg_saturn.cxx#L1032)
`sat_wpn_soft = 0;` est **délibérée et documentée** (« weapon ALWAYS on VDP1 — off = pre-existing
transition glitch; reproducible with L+X alone, HW-confirmed »). Ce n'est donc **pas un bug** — mais
alors **L+X** ([:7986](../src/dg_saturn.cxx#L7986) `sat_wpn_soft ^= 1;`) est un **levier mort par
construction**. Les deux ne peuvent pas être vrais en même temps : **c'est une contradiction interne du
code**, et c'est votre règle `code-coherence-challenge` qui s'applique — un des deux doit partir.

**GIT** : `b33c47c` (2026-07-27) a introduit l'appel, **huit jours après** le park M7. **Aucun rapport
avec le lowres. Jamais annoncé.**

**Correctif ~3 lignes** : ne forcer que sur un vrai changement de `sat_iso_mode`, ou déplacer l'appel
**avant** les blocs preview/M.

> ⚠️ **Tant que ce n'est pas corrigé, tout A/B HW impliquant L+X ou R+X est invalide** — vous croyez
> mesurer un levier armé qui ne l'est plus.

### 2.2 🔴 Le SQ plancher/plafond a une porte à sens unique, sur une prémisse fausse

[dg_saturn.cxx:832](../src/dg_saturn.cxx#L832) : `if (cur == SQ_FULL) return detailshift ? SQ_FLAT : SQ_LD;`
— en M7, `detailshift` vaut **toujours** 1 (`r_main.c:712`).

**La justification écrite ([:825-828](../src/dg_saturn.cxx#L825-L828)) est factuellement fausse**, et
j'ai vérifié la chaîne complète moi-même : [r_plane.c:1860](../core/r_plane.c#L1860)
`if ((eff_potato || !detailshift || sat_lowres) && ...)` met explicitement les plans M7 sur la worklist
→ `w->ld = eff_ld` → `R_DrawVisplaneTextured(..., w->ld)` → [r_plane.c:864](../core/r_plane.c#L864)
`if (ld)`, **gaté par aucun test `detailshift`**. **LD est vivant en M7** — c'est même le chemin
optimisé L1.

**Impact concret** : le défaut de boot est `sq_floor = sq_ceil = SQ_LD`
([:799](../src/dg_saturn.cxx#L799)). Dès que vous touchez pad Y ou L+Y, le cycle devient
`LD → FLAT → FULL → FLAT → FULL…` : **vous ne pouvez plus jamais revenir au réglage livré sans
rebooter.** Le défaut shippé n'appartient pas à l'ensemble atteignable de son propre cycle.

**GIT** : `50ac224` (2026-07-16), le jour même du passage de M7 en défaut. La ligne qui le contredit
(`r_plane.c:1860`) vient de core `0e4dc46` (2026-07-18) — **le garde-fou est devenu obsolète deux jours
après avoir été écrit, personne n'a recroisé.**

**Correctif : 1 ligne, 0 octet** — `if (cur == SQ_FULL) return SQ_LD;`

### 2.3 🔴 Le SH-2 esclave est 100 % inutilisé en multijoueur

`sat_local_players <= 1` sur les trois offloads : [r_plane.c:1913](../core/r_plane.c#L1913),
[r_things.c:1877](../core/r_things.c#L1877), [dg_saturn.cxx:7604](../src/dg_saturn.cxx#L7604).

**Raison, verbatim** (core `79ccbdb`, 2026-07-20) : *« ALSO require single-player. `!sat_lowres` was a
proxy for "M7 = master-only", but the M7 pause-fullres flips `sat_lowres`→0 for a 1p menu; starting a
New Game INTO co-op from that menu renders ONE split frame while `sat_lowres` is still 0 → the
`RP_WaitPlanes` 30M spin wedged = the reported freeze. (Un-parked M4 co-op would lose dual-CPU planes
here — acceptable, flagged.) »* — **ANNONCÉ**, le commit énonce lui-même le sacrifice.

**Mais le gate est un proxy grossier** : la vraie cause est une **course de transition** (une frame
split rendue pendant que `sat_lowres` vaut encore 0), pas le nombre de joueurs. Le même stack a rendu
**+3 fps en 1p (27 vs 24, HW-mesuré)** après restauration le 2026-07-30. En 3/4p le maître est **plus**
chargé (4× BSP/projection/émission VDP1) — donc l'attente y est *a priori* supérieure, mais
**NON MESURÉE**.

**C'est le plus gros gisement de perf restant de tout l'audit.** Le freeze qui justifiait le gate est
déclaré corrigé (`RP_WaitPlanes` borné FRT, `r_plane.c:1918`) — **c'est exactement l'argument qui a
permis de lever le `!sat_lowres` en 1p le 2026-07-30, et il n'a jamais été appliqué au gate joueurs.**

**Coût : ~20-40 lignes** (capturer atomiquement la signature de dispatch et refuser si elle a changé,
au lieu de lire l'état global depuis le corps esclave) **+ une session HW co-op.**

---

## 3. Inventaire — ce qui n'est pas actif en M7

| Gravité | Fonctionnalité | Statut | Nature | Récupérable |
|---|---|---|---|---|
| 🔴 | `sat_apply_iso` écrase `sat_things_hw` (L+X, R+X, M0/M6) | **bug actif** | régression, **sans rapport avec M7** | 3 lignes |
| 🔴 | Offload dual-SH2 en multijoueur | OFF par gate | prudentiel jamais levé | 20-40 lignes + session HW |
| 🔴 | Porte à sens unique SQ_LD | OFF par gate | **prémisse réfutée au code** | **1 ligne** |
| 🟠 | Ciel HW NBG0 en split | OFF par gate | prudentiel, raison écrite | 10-15 lignes |
| 🟠 | Bande de rattrapage wall-lag + fill de décrochage | **inatteignable** | **effet de bord non signalé** | **1 ligne** |
| 🟠 | Option menu « Screen size » | actif mais **cassé** | oubli d'adaptation | 3 lignes (+30 pour la bordure) |
| 🟠 | Message HU sur VDP1 en split | OFF par gate | phase 2 différée — **pas une coupe** | 25-40 lignes |
| 🟠 | Modes M0/M4/M5/M6 (anneau pad-Z) | inatteignable **au runtime** | **votre décision, annotée** | 1 ligne (mais condition posée) |
| 🟡 | `rbg0_mode` (cycle 3 modes RBG0) | **inatteignable dans le binaire** | oubli, **antérieur à M7 de 2 semaines** | non recommandé |
| 🟡 | `SAT_FLOOR_PERFSIM` (perf-sim plancher) | inatteignable (`#elif` mort) | enfoui | non recommandé |
| 🟡 | Option menu « Detail: High/Low » | **no-op** | effet de bord `detailshift == sat_lowres` | sans objet |
| 🟡 | Axe SQ sprite (pad R+B) | inerte | idem | sans objet (fb 160 colonnes) |
| 🟡 | 10 lignes d'overlay `(void)`-jetées, `sat_prof_planepix` jamais armé | instrumentation coupée | arbitrage pad/lignes | 1 ligne chacune |
| ⚪ | Adaptations packé/dup du core (20+ gates), drawers `*Low` | adaptations | **c'est la définition de M7** | sans objet |

**Distinction demandée** : *OFF par gate* = récupérable en changeant une condition. *Inatteignable dans
le binaire* = le symbole a été éliminé par GCC, vous croyez l'avoir et vous ne l'avez pas. *Inatteignable
au runtime* = le code est bien dans `.text`, seul le chemin d'accès manque.

---

## 4. Chronologie — ANNONCÉ / ENFOUI / SILENCIEUX

| Date | Commit | Désactivation | Classe |
|---|---|---|---|
| 2026-06-26 | `65fee1d` | work-steal + wall-prep slave | ANNONCÉ |
| 2026-06-27 | `41dd895` | `RBG0_TUNE_PAD=0` | ANNONCÉ (plan écrit **avant** la coupe) |
| **2026-06-30** | **`66463da`** | `rbg0_mode` 3→2 modes ; gradient RBG0 parké ; `rbg0_floor_dim`/`contrast` perdent leurs chords | ANNONCÉ (cycle) / **SILENCIEUX** (le reste) |
| 2026-07-02 | `5863609` | `VDP1_MANUAL_CHANGE=0` ; BSP staging M5 retiré | ANNONCÉ — modèle du genre |
| **2026-07-03** | **`746ce1e`** | pad-Y pris par le cycle ftex → `SAT_FLOOR_PERFSIM` enfermé dans un `#elif` mort | **ENFOUI** |
| **2026-07-05** | **`db17b04`** | anneau pad-Z créé `{M0,M4,M6}` — **M5 n'y a jamais figuré** | **ENFOUI** |
| **2026-07-05** | **`06a8568`** | dernier écrivain de `rbg0_mode` supprimé → replié en constante ; **10 lignes d'overlay coupées d'un coup** | **SILENCIEUX** |
| | | **↑ TOUT CECI EST ANTÉRIEUR À M7 ↑** | |
| **2026-07-14** | **`0fceca8` / core `e0a3638`** | **M7 naît.** masked-split slave coupé ; menu « Detail » no-op ; SQ sprite inerte ; `R_InitBuffer` non adapté | **ENFOUI** (dit dans le message *core*) / **SILENCIEUX** (`R_InitBuffer`) |
| **2026-07-15** | **`cb201c7`** | **ciel HW split coupé** | **SILENCIEUX au message** |
| **2026-07-16** | **`f7114cf`** | **M7 défaut.** M1/M2/M3 supprimés → tuent `sat_vdp1_floor` → rendent inatteignables la chaîne SAT_FLOOR_TEX, **la bande wall-lag**, **le fill de décrochage** | ANNONCÉ / ENFOUI / **SILENCIEUX** (les effets de bord) |
| **2026-07-16** | **`50ac224`** | porte à sens unique SQ_LD | ANNONCÉ (la coupe) / SILENCIEUX (son effet) |
| 2026-07-18 | `c24ed24` etc. | M0 hors anneau ; clear-on-slave et plane-split coupés en M7 | ANNONCÉ |
| 2026-07-19 | `aa657ef` | anneau réduit à `{M7}` | ANNONCÉ, annoté ***« user-requested »*** |
| 2026-07-20 | core `79ccbdb` | slave interdit en multijoueur | ANNONCÉ (« acceptable, flagged ») |
| **2026-07-27** | **`b33c47c`** | **`sat_apply_iso` casse L+X, R+X, M0, M6** | **SILENCIEUX** |
| 2026-07-30/31 | core `001f2be` / `4bc44c3` | **RESTAURATION** de la pile slave M7 en 1p (+3 fps HW) | ANNONCÉ — modèle du genre |

---

## 5. Ce que je recommande, par rapport bénéfice/coût

**Rang 1 — ~20 lignes au total, ~0 octet.**
1. Corriger `sat_apply_iso` (**3 lignes**) — répare L+X, R+X et l'axe M d'un coup. *Bloque tout A/B fiable tant que ce n'est pas fait.*
2. Armer `sat_plane_wallband = 1` (**1 ligne**) — rallume la bande wall-lag **et** le fill de décrochage, dont les largeurs sont **déjà recalculées chaque frame** (`r_main.c:1032-1062`) : coût payé, bénéfice nul aujourd'hui.
3. Supprimer le test `detailshift` de `sq_plane_cycle` (**1 ligne**).
4. Corriger les commentaires devenus faux (~10 lignes) — `rbg0_mode`, `sat_ftex_slave`, `sat_vdp1_floor` (« M1..M3 » alors que c'est M5 seul), `r_plane.c:864` (« 1 texel per 2 screen px » — c'est **4** en M7).
5. Bandeauter `docs/TOGGLE_AUDIT.md` : **six entrées KEEP_LIVE sont fausses**. *Un registre faux est pire que pas de registre.*

**Rang 2 — le vrai gisement.** Lever `sat_local_players <= 1` sur les trois offloads, en corrigeant la course à la racine (~20-40 lignes + une session HW co-op).

**Rang 3 — si vous le voulez.** Le ciel HW en split (~10-15 lignes, avec les trois réserves du §1), et « Screen size » (**à confirmer en jeu d'abord** : Options → Screen Size, un cran — c'est de l'INFÉRÉ non testé).

**Ce que je ne recommande pas de récupérer**, parce que ces coupes-là sont irréprochables (raison HW,
date, doc, souvent la recette de reprise, écrites à l'endroit où l'accord a disparu) : `rbg0_mode`,
`SAT_FLOOR_PERFSIM`, M1/M2/M3, le BSP staging M5, l'anneau blit DMA, `VDP1_MANUAL_CHANGE`, l'AIMD damp,
l'axe SQ sprite, les drawers `*Low`.

---

## 6. Deux mémoires à corriger

- **`slave-sh2-vdp1-flicker-offload`** (« ABANDONNÉ HW ») et la partie « slave 100 % IDLE by
  `!sat_lowres` gate » de **`m7-critical-path`** sont **obsolètes depuis le 2026-07-31** : la pile
  slave M7 a été restaurée en 1p et mesurée à **+3 fps sur HW**.
- **`docs/TOGGLE_AUDIT.md`** est figé au 2026-07-16 et contredit le code sur six points.
