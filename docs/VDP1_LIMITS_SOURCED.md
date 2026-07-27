# VDP1 — limites **officielles (SEGA)** et **prouvées par la pratique**

> **But de ce document (2026-07-25).** Registre unique, à provenance tracée, de ce que le
> VDP1 coûte *réellement* — pour arrêter de deviner sur le flicker. Chaque fait porte sa
> **provenance** :
>
> - **【OFF】** = officiel SEGA. **Manuel VDP1 User's Manual (ST‑013)**, contenu dans
>   `refs/JUNE96_DTS.ISO:/DOCUMENT/SATURN/PROGRAM2.PDF` (le fichier `GRAPHICS.PDF` ne
>   contient QUE les outils graphiques — pas le VDP1). Citation « UM p.N » = page imprimée
>   du manuel ; décalage sheet = N+14 dans PROGRAM2.PDF.
> - **【COM】** = communautaire / mesuré hors-SEGA (Copetti, gendev/SpritesMind, Beyond3D).
>   Utile mais **PAS** dans le manuel SEGA — ne jamais le présenter comme officiel.
> - **【HW】** = prouvé par nos propres tests matériel Mimas (mémoires + docs internes +
>   la session de test flicker en cours).
>
> Compagnon de [`VDP1_ARCHITECTURE.md`](VDP1_ARCHITECTURE.md) (modèle) et
> [`VDP1_CAPACITY_STUDY.md`](VDP1_CAPACITY_STUDY.md) (budget). Ce doc-ci est la **référence
> chiffrée sourcée** : si un chiffre n'est pas ici avec un tag, il est à vérifier.

---

## 0. Les 3 conclusions qui pilotent le flicker (lire en premier)

1. **La contrainte VDP1 n'est PAS le fill.** 【OFF】 « 1 pixel dessiné par cycle d'horloge
   28,6364 MHz » (UM p.20, Table 4.2 p.37) → un plein écran texturé 320×224 ≈ 71 680 px ≈
   **~2,5 ms** au pire ; contre une frame de 70–130 ms, le fill est **abondant**. 【HW】 confirmé
   (frame master-bound ; le fill remplit en quelques ms).

2. **Le flicker = « transfer‑over » (terme officiel SEGA).** 【OFF】 UM p.53 (bit BEF) :
   *« If there are many commands, or if there are many pixels to be drawn because of
   enlargement, drawing may not be terminated in one frame. This is referred to as
   "transfer‑over"… it is necessary to reduce the drawing commands or to reduce the pixels
   drawn. »* → les commandes tardives (murs lointains / sprites / arme) ne sont jamais
   dessinées cette frame → clignotement. **Cause = commandes trop nombreuses OU trop de
   pixels itérés par agrandissement (overdraw)** — PAS le fill brut.

3. **Le vrai capteur existe et est officiel : LOPR / COPR / BEF** (§4). Il mesure
   directement le transfer‑over. Il **remplace** le `D%`/`Dr%` basé sur CEF, qui est 【HW】
   **inutile** (artefact de désync présent, voir §3). C'est *la* sonde à construire.

---

## 1. 【OFF】 Chiffres officiels SEGA (VDP1 User's Manual ST‑013 / PROGRAM2.PDF)

### 1.1 Horloge & débit pixel
- **28,6364 MHz** (modes hi‑clock NTSC) / **26,8426 MHz** (NTSC 320 large) — Table 4.2, **UM p.37**.
- **1 pixel dessiné en synchro avec cette horloge** : *« the data for 1 pixel is drawn in
  sync with this »* — **UM p.20**. ⇒ **débit de base = 1 px/clock ≈ 28,6 Mpx/s** (mode simple).
- **⚠️ Le manuel ne donne AUCUN multiplicateur par mode** (texturé, distordu, gouraud,
  demi‑transparence). Ces facteurs (2×, 6×…) sont **【COM】**, pas SEGA — voir §2.

### 1.2 Table de commande
- **Taille = 1EH (30) octets de données ; frontière/pas = 20H (32) octets** — **UM p.25**, p.66.
- 16 mots (offsets 00H–1EH) ; le 16ᵉ mot (**+1EH**) est *« (Dummy) Skipped during table
  fetch »* → 15 mots (30 o) vivants, alignés 32 o.
- Champs : `CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE`, 4 sommets
  `CMDXA..CMDYD` (16 o), `CMDGRDA`.
- **⚠️ « ~16 cycles / fetch de commande » = 【COM】, ABSENT du manuel.** Le manuel décrit le
  fetch qualitativement seulement (UM p.25).

### 1.3 Overdraw / itération — **LE point clé, confirmé source primaire**
- 【OFF】 **UM p.83 (Pre‑Clipping Disable)** : *« One drawing command comprises a group of
  several lines, and the respective lines comprise a number of dots. **Each dot is drawn
  based on clipping area (drawing area) information**… For lines that are completely
  separated from the drawing area… drawing efficiency can be raised by specifying the
  drawing not be started… VDP1 normally performs this detection, but… the overhead
  (**up to five CPU clock cycles for one line**) becomes conspicuous [on small elements].
  In the case of large elements that extend greatly out of the drawing area, it is more
  efficient to perform pre‑clipping. »*
  → **Le clipping supprime l'ÉCRITURE du dot, PAS l'itération.** Un primitif est
  edge‑walké en lignes→dots ; les dots hors‑écran coûtent quand même le temps d'itération,
  **sauf** si le bit **pré‑clipping (Pclp = CMDPMOD bit 11)** est mis, qui cull des *lignes
  entières* hors‑zone en amont (détection ≤5 clocks/ligne, donc rentable seulement sur les
  gros primitifs très hors‑cadre). **← C'est la confirmation officielle du modèle overdraw,
  ET un levier (Pclp) non encore exploité dans Mimas.**
- 【OFF】 **Sprites distordus écrivent certains pixels DEUX FOIS** — **UM p.8** : *« holes
  are filled to prevent the dropout of pixels. For this reason, there may be some pixels
  that are written twice… Because drawing is done with lines, part of the character pattern
  may result outside the graphic formed by linking the four vertices. »* → coût réel d'un
  mur distordu **> aire naïve** ; et demi‑transparence non garantie sur distordu.
- 【OFF】 Plan frame‑buffer borné ±1024 : *« nothing is written for parts that exceed the
  range of the frame buffer plane »* — **UM p.21** (confirme : write supprimé hors‑plan).
- 【OFF】 **High‑Speed‑Shrink (HSS)** — **UM p.44** : pour sprites scaled/distordus avec
  magnification < 1, échantillonne **seulement les pixels pairs OU impairs** (bit EOS) →
  **divise le fill par ~2** pour les sprites rétrécis (murs lointains).

### 1.4 Clipping
- 【OFF】 **System clip** (sélect 1001B) : coin haut‑gauche **fixé (0,0)**, bas‑droite =
  `CMDXC/CMDYC` — **UM p.110**.
- **User clip** (1000B) : rectangle libre. `CMDPMOD` : **Clip (bit10)** enable, **Cmod
  (bit9)** dessiner dedans(0)/dehors(1), **Pclp (bit11)** pré‑clip disable — **UM p.83‑84**.
- **Erase/write PAS affecté par le clipping** — UM p.49.

### 1.5 VRAM & framebuffers
- 【OFF】 **VRAM 512 Ko (4 Mbit)**, `000000H–07FFFFH` — **UM p.24**. Contient commandes +
  character patterns + CLUT + tables gouraud (UM p.18). Contention VRAM : **>10 wait
  cycles** possibles (priorité system‑controller > drawing) — UM p.19.
- **2 framebuffers de 256 Ko (2 Mbit) chacun**, `080000H–0BFFFFH` ; 1 affiché
  (inaccessible CPU) + 1 dessin — **UM p.20**.
- **Modes FB (Table 4.2, UM p.37)** : Normal 512×256 **16 bpp** (320/352 ×224/240) ;
  Hi‑res 1024×256 **8 bpp** (640/704) ; Rotation 512×512 **8 bpp** ; HDTV 512×256 16 bpp.
- Swap : `TVMR` (VBE/TVM) + `FBCR` (FCM/FCT/EOS/DIE/DIL), auto 1/60 s ou manuel. UM p.35‑38.

### 1.6 【OFF】 Registres (Table 2.1, UM p.23) — **base VDP1 = 0x25D00000**
| Adresse | Reg | Rôle | Acc |
|---|---|---|---|
| 0x25D00000 | **TVMR** | TV mode / V‑blank‑erase (VBE bit3, TVM bits2‑0) | W |
| 0x25D00002 | **FBCR** | FB change (EOS/DIE/DIL, FCM bit1, FCT bit0) | W |
| 0x25D00004 | **PTMR** | Plot trigger — PTM 10B=auto début‑frame, 01B=draw‑on‑write (repart du haut), 00B=idle | W |
| 0x25D00006 | **EWDR** | Erase/write fill data (2 px si 8bpp) | W |
| 0x25D00008 | **EWLR** | Erase/write coin haut‑gauche | W |
| 0x25D0000A | **EWRR** | Erase/write coin bas‑droite | W |
| 0x25D0000C | **ENDR** | **Terminaison forcée du dessin** (write 0000H ; ~≤30 clocks ; non reprenable) | W |
| 0x25D00010 | **EDSR** | Statut fin transfert : **CEF bit1**, **BEF bit0** | **R** |
| 0x25D00012 | **LOPR** | **Adresse de la DERNIÈRE commande opérée (frame précédente)** | **R** |
| 0x25D00014 | **COPR** | **Adresse de la commande EN COURS** | **R** |
| 0x25D00016 | **MODR** | Statut mode (miroir des W‑only) | R |

**CEF / BEF (EDSR) — UM p.52‑53 :**
- **CEF (bit1)** = *Current End‑bit Fetch* : 1 = commande draw‑end **fetchée** (dessin de la
  frame terminé) → génère une interruption ; remis à 0 au FB‑change / draw‑start.
  **⚠️ Caveat officiel** : *« If fetch of the draw terminate command matches when the frame
  buffer changes, **CEF and BEF might not become "1"**. »* → **explique 【HW】 « CEF lit B en
  permanence »** (race FB‑change).
- **BEF (bit0)** = *Before End‑bit Fetch* (frame précédente) : **0 après un transfer‑over**
  (la liste n'a pas fini). C'est le **flag booléen d'overrun**.
- 100000H–10000CH sont **write‑only** (pas de readback). Seuls EDSR/LOPR/COPR/MODR sont R.

### 1.7 【OFF】 Limite par frame — il n'y a PAS de « max N commandes »
- Le manuel **ne donne aucun nombre fixe** de commandes/sprites max. La limite documentée
  est le **transfer‑over** (§0.2) : ce qui ne finit pas déborde ; remède = moins de
  commandes / moins de pixels. FBCR note *« The number of characters that can be drawn in
  one frame is limited »* (UM p.38).
- **Pseudo Draw Continuation** (UM p.56) : étaler une liste trop grosse sur plusieurs frames
  (ENDR + jump + PTM=01B). ⚠️ dots demi‑transparents colour‑calc'd 2× au raccord.
- **Budget d'erase V‑blank** : px requis = `(X3−X1+1)×(Y3−Y1+1)×8` ; Tables 4.4/4.5 donnent
  l'utilisable (NTSC 320×224 = **58 812** ; 320×240 = **34 684**) — UM p.49‑50.

### 1.8 【OFF】 Modes couleur (UM p.12) & gouraud (UM p.65)
- Colour‑bank 16 col = **4 bpp** (12 bits hauts du bank préfixés) ; 64/128/256 col = **8 bpp** ;
  RGB = **16 bpp** (MSB=1, 5 bits/canal). Priorité + colour‑calc packés dans le mot couleur,
  résolus par **VDP2**.
- Gouraud : table 5 bits/sommet ΔR/ΔG/ΔB, `(val − 10H)` ajouté à la base, clampé 00–1FH.

---

## 2. 【COM】 Chiffres communautaires (mesurés, **PAS dans le manuel SEGA**)
> Source : Copetti (*Sega Saturn Architecture*), gendev/SpritesMind
> [t=2868](http://gendev.spritesmind.net/forum/viewtopic.php?t=2868), Beyond3D, SegaXtreme.
> **À étiqueter « mesuré », jamais « officiel ».**

| Grandeur | Valeur 【COM】 | Cohérence avec 【OFF】 |
|---|---|---|
| Polygone plat (untextured) | ~1 cyc/px, ~28,6 Mpx/s | ✅ = 1 px/clock officiel |
| Sprite texturé / distordu | ~2 cyc/px, ~14,3 Mpx/s | ⚠️ multiplicateur **non officiel** (1 texel read + 1 FB write) |
| + Gouraud | ~gratuit sur gros quads, tombe ~16 Mpx/s sur 10×10 | ⚠️ non officiel |
| **+ Demi‑transparence** | **~6× plus lent** (RMW du FB) | ⚠️ non officiel ; = *« le plus cher »* (Copetti) |
| Fetch de commande | ~16 cyc/commande | ⚠️ **absent du manuel** |
| FB 8 bpp | ~35,6 Mpx/s | ⚠️ non officiel |

**Le seul coût‑cycle officiel** : 1 px/clock ; pré‑clip détection ≤5 clocks/ligne ;
terminaison forcée ≤~30 clocks ; contention VRAM >10 wait cycles (tous §1).

---

## 3. 【HW】 Prouvé par la pratique (Mimas, matériel réel)
- **EDSR.CEF lit `B` (busy) EN PERMANENCE** sur HW réel (`dg_saturn.cxx:780`) → le VDP1 ne
  signale jamais « liste finie » en une frame. **Cohérent avec le caveat officiel CEF** (§1.6,
  race FB‑change) et avec le transfer‑over structurel.
- ⇒ **`D%` / `Dr%` (dérivé de CEF) est INUTILE** : dominé par la désync présent CPU↔VDP1,
  **invariant à la charge** (prouvé : sur la scène la plus rapide, demander `F31` ≈ 0,4 écran
  lisait encore `Dr 45%`). **À retirer de l'overlay.**
- **Fill VDP1 abondant** (§0.1) : jamais le facteur limitant à nos temps de frame.
- **Le flicker = transfer‑over par OVERDRAW** : les murs proches projettent des trapèzes
  itérés hors‑écran (y ≈ ±2000) ; le VDP1 itère toute la travée → sature son temps → les
  commandes tardives (murs loin / sprites / arme) sont droppées → clignotement.
- **Test HW en cours (testeuse)** : le flicker apparaît **au‑dessus du budget ~34000** —
  MAIS à ce point l'overlay lit **`VD1 w13%`** (banc de commandes rempli à **13 %** seulement,
  `fbw38` = 38 murs déjà en software). Fill abondant **+** commandes à 13 % ⇒ **ni
  count‑bound ni fill‑bound → overdraw‑bound** : ce sont les **quelques murs proches
  restants** qui, par leur travée itérée, provoquent le transfer‑over.
- **L+X (arme → software, ~18 000 px de fill libérés) n'a AUCUN effet** sur le flicker →
  **confirme que le fill n'est pas le levier.**
- Leviers connus/dispo (code) : mur proche → **software** (`SAT_WALL_CPU_SPAN=480`,
  `SAT_WALL_CPU_MAG=3`, `SAT_WALL_SUBDIV_MAX=6` dans `core/r_segs.c`) ; **flat fallback**
  (½ fill, garantit la fin de liste) ; cull vertical world‑anchored. Banc commandes 256/banc,
  `WALL_CMD_CAP=248`, `WALL_ACC_MAX=120` (pic 1p ~57 murs / ~142 cmds). Cache wtex **22
  slots** (8 bpp).
- **Ymir ne flicke PAS** : il dessine la liste complète instantanément → jamais de
  transfer‑over. Donc **tout test de flicker doit se faire sur HW réel** ; sur Ymir le bon
  capteur (§4) lira « liste finie » (LOPR = fin), ce qui est correct.

---

## 4. Le **bon capteur** (remplace `D%`) — LOPR / COPR / BEF
Fondé sur §1.6 (officiel, HW‑lisible) :

- **Mètre transfer‑over (LOPR)** : à chaque frame lire **LOPR (0x25D00012)** = adresse de la
  dernière commande opérée la frame *précédente*. La comparer à l'adresse de **fin de liste**
  (dernière commande / END du banc courant) :
  - LOPR atteint la fin ⇒ **liste finie** (pas d'overrun).
  - LOPR **< fin** ⇒ **transfer‑over** : la fraction `(LOPR−base)/(fin−base)` = **% réel de
    liste dessinée**. **C'est LE signal du flicker**, contrairement au `D%`.
- **COPR live (0x25D00014)** : commande en cours — échantillonner en milieu de frame pour
  voir où en est le plot.
- **BEF (EDSR bit0)** : 0 ⇒ la frame précédente a débordé (flag booléen simple).
- **⚠️ Unités à vérifier au 1er run** : LOPR/COPR contiennent des *adresses de commande* —
  confirmer l'unité (offset octet brut vs /N) contre notre base/fin de banc connues avant
  de dériver le %.
- **Ymir** : LOPR devrait lire « fin » en permanence (draw instantané) → utile comme
  contrôle (si LOPR<fin sur Ymir, notre lecture d'unité/adresse est fausse).

---

## 4b. Tel qu'IMPLÉMENTÉ (2026-07-25, branche flicker-clean, `src/dg_saturn.cxx`)
La sonde vit sur **overlay row 12** (1p, overlay full ; déplacée de row 17 où l'arme VDP1 la
masquait — row 12 est vide au‑dessus de LOS en build cart/MUS), lue dans `vdp1_wpn_kick` :
```
V1 c<cmds> LP<pct>% tx<used>/22 L<lopr>/<end> C<copr> i<iso>
```
- **c** = commandes émises la frame précédente (cap `WALL_CMD_CAP=248`) — le limiteur COUNT.
- **LP%** = fraction du banc **MUR** (murs+things+arme) que LOPR a atteinte : **100 = liste
  finie, <100 = transfer-over = LE flicker**. *Sémantique exacte* : la liste est
  `root → W(murs/things/arme) → F(floors @0x25C7C000) → END`. Les floors sont chaînés
  APRÈS exprès (« an overrunning plot cuts floors, not walls », dg_saturn:5468). Comme le
  banc F (cmd-addr ~63488) est bien au-dessus du banc W (~32/1056), LOPR-dans-F dépasse
  `end` → clampé à 100 → **LP% mesure la complétion des MURS**, pas des floors (= pile le
  signal du flicker monstre/mur ; les floors sont surtout sur RBG0 de toute façon).
- **tx** = slots cache wtex occupés / `WTEX_SLOTS=22` — le limiteur VRAM.
- **L/end/C** = LOPR brut / fin-de-liste calculée / COPR brut (unités /8) — **check d'unité
  HW** (deux registres pour dé-risquer la session unique).
- **i** = mode d'isolation (pad **L+Z**, cf. §5b).

> **✅ HW-VÉRIFIÉ 2026-07-26 : la sonde MARCHE sur Saturn réelle.** Sur HW, **LOPR track** :
> une scène qui **déborde** lit un LOPR **mi-banc** (`L6c0/6e8` → (0x6c0−0x420)/(0x6e8−0x420) =
> **94%** = le flicker) ; une qui **finit** lit **`Lc`** (LOPR *sous* le banc = le plot a fini le
> banc W et sauté au chain idle/floor **avant** l'échantillon). **Deux directions de complétion
> lisent 100** : LOPR **au-dessus** de `end` (banc F) OU **sous** `base` (`Lc`). ⚠️ Le clamp initial
> transformait `Lc` en LP0 (à l'envers) — **corrigé 2026-07-26** (`got<0` → LP=100).
>
> **Ymir** ne modélise pas LOPR (bloqué à `Lc`) mais ne déborde jamais → après le fix, `Lc`→**LP100**
> y est *correct*. `c`/`end`/`tx` trackent partout (Ymir inclus).
>
> **Finding 1re session HW** (scène modérée ~27 fps) : **i0** (things ON, 178 cmds) = **LP 94-97%
> = overrun léger RÉEL** (queue coupée, jitter 94↔97 = les éléments les plus lointains clignotent) ;
> **i1** (no-things) + **i2** (flat) = **`Lc`→LP100** (finit). Dans cette scène ce sont donc les
> **things** qui font basculer en overrun. Slots commandes **178/248** + VRAM **17/22** non saturés
> → l'overrun est **temps de plot (fill/overdraw)**, PAS count/VRAM. À re-tester en scène DENSE
> (mur proche + horde) où i1 pourrait déborder aussi (→ ce serait alors les MURS/overdraw).

## 5b. Modes d'isolation (pad L+Z, 1p) — trouver QUELLE couche déborde
L'arme reste **TOUJOURS sur VDP1** : la router hors‑VDP1 (`sat_wpn_soft`) déclenche un **glitch VDP1
de transition pré‑existant** (murs de l'ancienne frame non re‑clearés ; reproduit par **L+X seul**,
confirmé Ymir) — et l'arme est un coût mineur que le LP% montre déjà coupé en fin de liste.
L'isolation varie donc **things + flat** seulement :

| i | Mode | Sur VDP1 | Lecture |
|---|---|---|---|
| 0 | all | murs texturés + things + arme | défaut ship (aucun changement) |
| 1 | no-things | murs texturés + arme | **ΔLP vs 0 = la part des THINGS dans l'overrun** |
| 2 | flat | murs FLAT + arme | **1 cyc/px vs 2 : si LP remonte vs mode 1 ⇒ fill texel ; sinon ⇒ overdraw/commande (travée near‑wall SEGA §1.3)** |

> **Bug weapon‑off (à investiguer sur HW, orthogonal à la sonde)** : `sat_wpn_soft=1` laisse les murs
> de la frame précédente affichés à la transition. **Ce n'est PAS le user‑clip** (murs texturés
> auto‑clippés `0x0008`+`0x04E0` ; flat sans clip `0x00C0`), ni le `sat_vdp1_switch_clear` (mono‑mode
> M7 → jamais armé, et casse la parité double‑buffer si on l'arme). Cause VDP1 non identifiée
> statiquement. Les modes iso l'évitent en gardant l'arme sur VDP1.

## 5. Leviers contre l'overdraw (classés par ce que dit la source)
| Levier | Source | Effet |
|---|---|---|
| Mur proche → software (`SAT_WALL_CPU_SPAN`/`_MAG`) | 【HW】+【OFF】(§1.3) | shed les murs à grosse travée itérée = **la cause directe**. Baisser le seuil = fix précis. |
| **Pré‑clipping Pclp** (CMDPMOD bit11) | 【OFF】 UM p.83 | cull *lignes hors‑cadre* d'un gros mur **sans** le passer en software (garde le mur net sur VDP1). **Non exploité — à prototyper.** |
| Flat fallback (½ fill, 1 cmd) | 【OFF】(§1.3 lignes)+【HW】 | garantit la fin de liste ; pire cas = mur plat, jamais un trou. |
| HSS pairs/impairs (bit EOS) | 【OFF】 UM p.44 | ½ fill sur sprites/murs rétrécis (magnif <1). |
| Cull vertical world‑anchored | 【HW】 | borne le reste sans swim. |
| Grandir banc / cache slots | 【OFF】(§1.7)+【HW】 | seulement si count‑bound (pas notre cas 1p à w13 %). |

---

## 6. Sources & tentatives
- **【OFF】** SEGA VDP1 User's Manual (ST‑013) — `refs/JUNE96_DTS.ISO:/DOCUMENT/SATURN/PROGRAM2.PDF`
  (monter le CD ; `GRAPHICS.PDF` = outils seulement, PAS le VDP1). Texte extrait mis en cache
  scratchpad `P2.txt` lors de la session.
- **【COM】** Copetti <https://www.copetti.org/writings/consoles/sega-saturn/> ; gendev
  <http://gendev.spritesmind.net/forum/viewtopic.php?t=2868> ; Beyond3D ; SegaXtreme.
- **【HW】** mémoires Mimas (`vdp1-cef-latches-on-hw`, `vdp1-present-tearing-diagnosis`,
  `aimd-wbudget-hw-cef-collapse`…) + docs `VDP1_ARCHITECTURE.md`, `VDP1_CAPACITY_STUDY.md`
  + session de test flicker 2026‑07.
- **Tentatives infra‑bloquées** : reddit `r/SegaSaturn` VDP1/VDP2 symbiosis (Claude Code
  refuse de fetch reddit) ; DiGRA 2017 Liboà (copie WebFetch corrompue + rendu PDF local
  sans poppler). À refaire si besoin (télécharger le PDF DiGRA à part et le rendre via agent).
