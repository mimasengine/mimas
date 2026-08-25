# Devlog #1 — « le present manuel VDP1 »

> Dossier de préparation. Méthode et recettes : [`DEVLOG_VIDEO.md`](DEVLOG_VIDEO.md).
> Cartes : `python tools/devlog/ep_vdp1_present_cards.py out/`.
> Tags de preuve : `[HW]` console, `[Ymir]` émulateur, `[src]` lu au source, `[est]` estimation.

**Style imposé pour tout ce qui est publié — vidéo, description, post SegaXtreme :
PAS DE PRONOM.** Ni « we », ni « I ». Les faits et les mécanismes sont sujets :
*« The lead-fill is gone »*, *« Six presentation mechanisms, all dead »*,
*« For two months the driver did nothing »*. Les cartes et annotations ci-dessous
respectent déjà la règle ; si tu réécris une ligne, garde-la.

---

## 0. Périmètre — arrêté, vérifié

**C'est le premier épisode monté.** Les captures antérieures n'étaient ni montées ni
commentées : il n'y a pas de « depuis la dernière fois », il faut **présenter le projet**
(§2, pièce A2) avant d'attaquer le bug.

**Fenêtre de travail : 2026-07-20 → build `fd95f4c` (19/08 05:30 CEST).** Le récit remonte
au **16-17/06** pour la naissance du bug, mais rien après `fd95f4c` n'est dans les images.

**Calcul de la coupure** — les captures de slygamer sont horodatées par le Genki en heure
NZ : `GenkiArcade-20260820-184951` → 18:49 **NZST (UTC+12**, pas d'heure d'été en août) →
**08:49 CEST**. Les fichiers arrivent dans `Downloads` entre 09:00 et 09:37 CEST, cohérent.

| commit | horodatage CEST | dans les captures ? |
|---|---|---|
| `4095d77` present manuel VBE | 19/08 04:46 | ✅ |
| `fd95f4c` step-1 émission | 19/08 05:30 | ✅ **le build capturé** |
| `f217751` split : les murs cèdent aux things | **20/08 16:01** | ❌ **après** |

🔴 **Conséquence à ne pas rater** : le clip 2 joueurs montre le present manuel **et** les
planchers de sprites relevés, mais **PAS** « les murs cèdent aux things » ni la récupération
du ciel HW en split. Ça part dans le teaser (§11, carte `H2`), pas dans l'annotation du clip.

---

## 1. L'angle, en une phrase

**Un bug ouvert le 17 juin, fermé le 19 août : pendant deux mois, le code censé faire passer
le VDP1 en mode manuel écrivait la valeur qui veut dire « reste en automatique » — parce que
le manuel officiel de SEGA se trompait d'un mot.**

Le vrai sujet est le corollaire : **une demi-douzaine de correctifs avaient été construits
autour du bug, et aucun n'a jamais marché parce qu'ils soignaient le symptôme.** Source
corrigée → les correctifs partent à la poubelle, et la place se libère.

Ouverture demandée : *ce n'est pas magique.* Ce n'est pas des fps gratuits, c'est du budget
de rendu. Ce que ça ouvre est la fin de l'épisode, pas le début.

---

## 2. La colonne vertébrale : la chronologie, git en main

### Acte I — la naissance (juin)

| date | commit | ce que ça dit |
|---|---|---|
| 06-16 | `06e2239` | `VDP1 wall world-renderer + layer inversion` — **le premier mur sur VDP1** |
| 06-17 | core `84f3130` | `per-distance wall light + close-wall CPU fallback + early kick` — **le bug est déjà nommé** (« sky-at-the-seam lag »), et le commit embarque déjà son premier correctif : *exit-coverage* + kick anticipé |
| 06-18/19 | `df5b82b` | `VDP1_MANUAL_CHANGE=0 — restore the (torn) VDP1 walls` — **1ʳᵉ tentative de present manuel, annulée** |
| 06-19 | `a7b0996` | `drop the NBG0 sky in enclosed rooms — hides VDP1 wall tearing` — le symptôme est **caché** |

> **Un jour** entre le premier mur VDP1 et l'apparition du défaut. Il a ensuite survécu à
> **quatre topologies de renderer** successives.

### Acte II — six presents, tous morts (juin → août)

| date | mécanisme | pourquoi ça a raté |
|---|---|---|
| 06-27/28 | **draw-gated present**, branche `vdp1-draw-gated-present` (`3aecc84`) | sans déchirure sur HW, mais les murs traînent d'un field à `Dr` 30-60 % → étagère |
| 06-28 | **branche full-slSynch** | slSynch ne pose jamais `EWLR/EWRR` → le VDP1 efface un rectangle de largeur **zéro** → les murs s'agglutinent ; poker les registres soi-même **fige le menu** |
| 06-30 | `3d2c3d0` **couple NBG1** | gated OFF, jamais rallumé — il attend `CEF`, que Ymir ne modélise pas |
| 07-02 | `f0e3fcb` / `5863609` | `present A/B toggles`, puis `present parked` |
| 07-03 | `14f4dd3` **coherent-pair present** | protège une paire murs+sols que M7 ne construit jamais → pure latence, supprimé le 02/08 |
| 08-02 | `7f83f34` **field-lock** `Fl1`/`Fl2` | épingle **le blit**, pas le **swap**. `A2/2` sur toutes les captures — appariement parfait — **et les trous toujours là** → parké le 03/08 |

En parallèle, **quatre corrections géométriques** (« l'anticipation ») testées sur HW le
02/08, toutes rejetées. La phrase qui clôt cette famille, et qui mérite d'être à l'écran :

> **A displacement model with the right sign must improve one direction. Symmetric
> worsening at every amplitude means the walls are not displaced.**

### Acte III — le correctif de fortune (août)

| date | commit | quoi |
|---|---|---|
| 08-02 | `a360fff` | la map de ciel NBG0 est commitée avec l'image, plus au milieu de la frame |
| 08-03 | `2ee3dc8` | **lead-fill** : `nouveau mur ∖ quad de la frame n−X` — le CPU **repeint la bande du trou** |
| 08-04 | `194916b` | les spans du lead-fill partent sur le **SH-2 esclave** — le correctif coûte assez cher pour mériter un CPU |
| 08-05 | core `4445cb7` | *dwell* : le **taux de bascule** CPU↔VDP1 est borné, parce que chaque bascule affame le ring d'historique du lead-fill |

**Le lead-fill marchait**, et c'est précisément ce qui le rendait dangereux : un correctif
qui marche retire la pression sur la cause.

### Acte IV — le déblocage (3 → 19 août)

| date | événement |
|---|---|
| **08-03** | **fafling** répond sur le fil SegaXtreme (post #16) |
| 08-04 | réponse dans le fil : *« That actually makes a lot of sense… I'll try that! »* |
| 08-18 | **double audit adversarial** (12 + 4 agents) contre les mémoires, le code, le **manuel ST-013 corrigé par Kronos**, la source de **SlaveDriver**, un désassemblage complet de **LIBSGL.A**, et l'historique git des deux branches étagères → [`VDP1_MANUAL_PRESENT_VERDICT.md`](VDP1_MANUAL_PRESENT_VERDICT.md) |
| 08-18 | déblocage RAM : `BACKUPTICS 128 → 32` rend ~15 Ko (le ring de ticcmd d'un netplay inexistant) — sans ça le driver ne rentre pas dans le pool |
| **08-19 04:46** | `4095d77` — **v2 VBE, validé, mis par défaut** ; core `512e815` — *« park the lead-fill — the present it patched around is fixed »* |
| 08-19 05:30 | `fd95f4c` — **step 1** : planchers de sprites relevés, nouveau signal de budget |
| 08-20 08:49 | **captures console de slygamer** ← les images de l'épisode |
| 08-20 16:01 | `f217751` — split : les murs cèdent aux things → **teaser** |

---

## 3. Les cinq poisons — pourquoi *aucune* tentative n'avait testé l'idée

| poison | où ça mordait |
|---|---|
| **Le gate CEF** | `EDSR.CEF` se latche **30-60 %** des frames sur silicium, **0 %** sur Ymir en mode manuel. Toute décision prise sur `Dr%` était fausse sur une des deux machines |
| **Le blit jamais clôturé** | le *fence* (`sat_field_fence`) **post-date d'un mois** toutes les expériences de present. **« present manuel + blit fencé + gate non-CEF » n'ont jamais coexisté dans un seul build** |
| **Les verdicts Ymir** | Ymir ne modélise ni le CEF manuel, ni LOPR, et ne déborde jamais. Tout verdict de present formé sur Ymir est nul |
| **🔴 L'erratum tueur** | manuel du CD développeur, p.39 : *« writing "0" to the VBE, FCM and FCT registers »* → `FBCR = 0x0000`. **Sa propre table en regard appelle `000` le mode 1-cycle.** Correction Kronos, en rouge : *« writing "0" to the VBE and FCT registers and "1" to the FCM register »* → **`0x0002`** |
| **slSynch réaffirme le swap** | `_BlankOut` est gaté sur `DMASetFlag` ; sous slSynch les pokes FBCR sont écrasés à chaque vblank (établi au désassemblage) |

**Point de vidéo** : les deux phrases côte à côte à l'écran (carte `C2_erratum`). Un mot,
deux mois.

---

## 4. Le déblocage, attribué correctement

1. **fafling, SegaXtreme post #16, 3 août** — la cause nommée en deux phrases :
   > *« If you mean that you have VDP1 switching its framebuffers after every vblank, it's
   > not a good idea in a variable framerate game like Doom, which should on top of that be
   > capped at 30 fps since vanilla Doom ran at max 35 fps on the PCs of the time. »*
   > *« That should solve the issues you have when trying to mix software rendering and VDP1
   > rendering for sprites, because VDP1 will be able to draw a lot more than just what fits
   > in a screen frame. »*

   ⚠️ **Nuance obligatoire à l'écran** : sa route (SGL en framerate variable,
   `SynchConst ≥ 2`) **n'était pas praticable ici** — Mimas tourne en `SRL_FRAMERATE=0`, où
   `_BlankOut` n'écrit jamais FBCR ; passer à `≥ 1` remettrait SGL à écrire `FBCR = 0` à
   chaque vblank-out, **contre** le driver. **Le diagnostic vient de lui, le driver est
   maison.** Plus honnête et plus intéressant que « fafling a donné la solution ».

2. **Les manuels VDP1/VDP2 corrigés par l'équipe Kronos** (Runik, **Fafling**, Benjamin
   Siskoo), récupérés sur SegaXtreme. Les errata sont **en rouge**, et le rouge marque
   exactement les endroits où la doc de référence était fausse — ici, l'effacement et la
   bascule de framebuffer.

3. **Le corpus de mémoires du projet, retourné contre lui-même.** Consigne de l'audit :
   « toutes les tentatives ont échoué, donc les mémoires contiennent des erreurs ». Elles en
   contenaient — trois corrigées le même jour (§F du verdict).

4. **Deux précédents commerciaux lus au source.** **SlaveDriver** (PowerSlave) tourne en
   FCM/FCT manuel en jeu — `SCL_SetFrameInterval(0xfffe)`, 1448 commandes/frame, cadence
   adaptative 1-2 fields — et **SGL** a ce mode en natif. Ce n'était pas une idée exotique :
   c'était le contrat que les jeux de 1996 expédiaient.

---

## 5. Pourquoi ça a marché — et le twist v1 → v2

**v1 (matin du 19)** : mode manuel, gate = `COPR` comparé à l'**adresse de fin stagée**
(jamais CEF), pulse `FBCR = 0x0003` **juste après le bord vblank-OUT** — exactement la
fenêtre prescrite par le manuel.

**Mécaniquement correct** (`MP11`, `w0`, 15,5 fps) **et les trous survivaient**, par
intermittence, plus une frame avec la même porte dessinée deux fois, décalée.

**Le twist** : le contrat papier et Ymir divergent **d'exactement un field**.

- **Papier** (Table 4.3(a)) : l'écriture après OUT bascule à la frontière de field
  **suivante**.
- **Ymir** (`vdp.cpp`, `BeginHPhaseLeftBorder`, branche `VPhase == LastLine`) : l'évaluation
  tourne **à la fin de la ligne** où le bord OUT vient d'être levé — une écriture dans cette
  fenêtre d'une ligne (~63 µs) s'exécute **un field plus tôt**.
- **Aucun registre ne dit quel buffer est affiché** : la divergence est **indétectable
  depuis le CPU**. v1 était *juste sur console* et *un field en avance sur Ymir* → 1 field
  sur ~4 affichait {image N−1, liste N} → trous intermittents, murs **en avance** cette fois.

**v2 (soir du 19) : la bascule par VBE erase & change** (ST-013 p.40) — `TVMR.VBE = 1` +
`FBCR = 0x0003` sur un bord vblank-**IN** frais, `VBE → 0` après le OUT. Le swap tombe **à
la fin de ce même vblank sur les deux machines**. C'est la séquence que la SBL expédie
(`SCL_VBLV.C`, intervalle `0xfffe`) et que SlaveDriver utilise en jeu.

Bonus : le field armé que v1 brûlait disparaît → le fence passe de 14-26 ms à 2-18 ms
(moy. ~10). **Le mode manuel devient compétitif au lieu d'être un impôt.**

> **Leçon générale, bonne pour une carte** : quand l'émulateur et le manuel se contredisent
> d'un field et qu'aucun registre ne permet de trancher, **ne pas choisir — trouver la
> séquence où les deux tombent d'accord.**

---

## 6. Ce que ça a retiré, et ce que ça ouvre

**Retiré dans les 24 h :**

| retiré | c'était quoi |
|---|---|
| **le lead-fill** | parké (`sat_wall_lead_x = 0` au boot, chord `R+Right` supprimé) — le CPU repeignait la bande du trou, sur les deux SH-2 |
| **le present 1-cycle AUTO** | supprimé. Un seul chemin reste |
| **le toggle A/B et son état** | `sat_mp_on` / `revert` / `saved-lead` supprimés |
| **le field-lock** | doublement supersédé, la branche de son site d'appel retirée |
| **2 slots de pad** | `L+B` et `R+Right` libérés |

**Réinvesti dans le VDP1 — exactement ce que fafling annonçait :**

| ouvert | quoi | dans les captures ? |
|---|---|---|
| **planchers de sprites** | décorations 2 % → 1 %, acteurs 5 ‰ → 2 ‰, `THING_EMIT_MAX` 16 → 32, cap de boot 4 → 8 — *« the plot window is now the whole frame »* | ✅ `fd95f4c` |
| **nouveau signal de débordement** | sous swap gaté, la guillotine LOPR ne peut plus tirer (`LP ≡ 100`) ; le signal vif devient le **spin du gate COPR** — chaque ms de spin = 1 ms de frame perdue, 1:1 | ✅ |
| **things sur VDP1 en split** | les murs cèdent leur budget aux things (un thing coupé n'est dessiné par personne ; un mur coupé dégrade en quad plat — cette asymétrie *est* le fix) | ❌ `f217751`, **après** → teaser |
| **sols VDP1** | 8 rounds d'incréments, toujours refusés visuellement — la porte n'était pensable qu'avec la fenêtre de plot élargie | ❌ pas shippé |

---

## 7. Le mur de boot — le deuxième chantier de la fenêtre

C'est le sujet qui porte le plus loin, et il a droit à **deux cartes** (§9, `F0` et `F0b`).
Le present est une histoire d'horloges ; celle-ci est l'histoire de **pourquoi la Saturn est
un mauvais hôte pour Doom**, et de ce que ça débloque quand le mur tombe.

### 7.1 Pourquoi c'est dur ici — le décalage avec le PC

Doom n'a jamais été écrit pour une machine comme celle-ci. Les chiffres sont vérifiables :

| | |
|---|---|
| **Ce que le moteur demande** | `DEFAULT_RAM 6 /* MiB */`, `MIN_RAM 6` — dans son propre source, [`core/i_system.c:58`](../core/i_system.c#L58) `[src]` |
| **Ce que la Saturn a** | **2 Mo** de work RAM, en **deux bancs non interchangeables** |
| **Ce que le tas de Doom reçoit** | **1 040 384 o (1016 Ko)** — `LOW_WORK_RAM_SIZE 0x100000 − RP_CMD_BUF_SIZE 0x2000` `[src]` |

Et les deux bancs ne sont pas un tas de 2 Mo :

- **WRAM-H** (`0x06000000`, SDRAM 1 Mo) — le banc rapide, et **le seul que le SCU-DMA sait
  lire** `[src Sega, SCU Final Specifications List n° 04]`. Il porte le code, la pile, et les
  données chaudes du renderer.
- **WRAM-L** (`0x00200000`, DRAM 1 Mo) — **mesuré 2,1× plus lent par accès** `[HW]`. C'est là
  que vit le tas de Doom. Le niveau habite donc le banc lent, par construction.
- **Un seul bus système pour les deux SH-2** : *« Because they are on a single system bus, one
  has to wait for the other »* `[src Sega, PROGRAM1 p.20]`. Plus de pression mémoire = plus de
  contention entre les deux CPU.
- **Pas de cartouche dans la config cible.** Les 4 Mo sont un **bonus**, jamais un prérequis.
- **Le disque est le seul magasin de secours** : pas de swap, pas d'écriture (les sauvegardes
  ne marchent pas, le CD est en lecture seule), et **une commande CD coûte ~37 ms** `[HW]`.

### 7.2 Ce qu'un WAD demande vraiment

Une carte n'est pas une image, c'est **dix lumps convertis en tableaux de structs** :
VERTEXES, LINEDEFS, SIDEDEFS, SEGS, SSECTORS, NODES, SECTORS, THINGS, BLOCKMAP, REJECT. La
facture monte avec la taille de la carte, et trois termes sont vicieux :

- **REJECT est quadratique en nombre de secteurs** : 600 secteurs → 45 Ko, 1000 → 125 Ko. Sur
  ce tas, c'est fatal par fragmentation.
- **Les composites de textures veulent du contigu** : un composite 256×128 réclame **32 Ko
  d'un seul tenant** dans un tas qui n'en offre que 21-34.
- **Les monstres coûtent leur en-tête** : `mobj_t` fait 156 o, la zone en facturait 180 — soit
  un `memblock_t` de 24 o par monstre, **26 Ko d'en-têtes sur la seule Scythe MAP30**. Et la
  famille **Nuts**, c'est **1,9 à 2,7 Mo de mobjs à elle seule** : impossible dans 2 Mo,
  aucun régime n'y changera jamais rien.

### 7.3 Le fait contre-intuitif : ce n'est pas la capacité, c'est la CONTIGUÏTÉ

**Une carte peut tenir dans les octets libres et refuser quand même de charger.** La plus
grosse allocation unique contre le mur fait **~110 Ko**, et elle les veut **d'un seul bloc**.
Deux blocs `PU_STATIC` garés au milieu de la zone (**72 Ko à 64 Ko**, **60 Ko à 655 Ko**)
découpent l'espace libre en morceaux. La signature à lire dans le bandeau est
**`fr` ≫ `lg`** : beaucoup de libre, pas un seul run assez long.

**C'est le meilleur plan de la section pour la vidéo** : *« it fit, and it still failed. »*

### 7.4 Le régime, et le piège qu'il a révélé

Mesuré sur les **415 cartes du corpus `wads_temoins`** (Doom, Ultimate, Doom II, TNT,
Plutonia, Hell Revealed, Scythe, Nuts 1/2/3) :

| | vanilla | shippé | cartes au-dessus du mur |
|---|---|---|---|
| `seg_t` | 32 o | **14 o** | 38 → **38** |
| `node_t` | 52 o | **28 o** | 38 → **38** |
| `line_t` | 64 o | **24 o** | 38 → **0** |
| `side_t` | 20 o | **16 o** | (21 Ko rendus) |

🔴 **`seg_t` et `node_t` ont libéré de la vraie RAM et ZÉRO carte.** Sur chacune des 38 cartes
bloquées, la plus grosse allocation était **LINEDEFS**. Identifier le **terme dominant**
avant de rétrécir quoi que ce soit — la même leçon que le reste de l'épisode, appliquée à la
mémoire au lieu du temps.

Plus, le même jour : **chargement en place** — chaque `P_LoadX` lit son lump brut dans la
**queue de son propre tableau final** et l'étend vers l'avant, plus aucun tampon
intermédiaire. **46 Ko (médiane) à 203 Ko (Nuts3 MAP01) de pic transitoire supprimés pour
zéro octet de disque.** Et une **slab mobj** (~24 Ko rendus sur MAP30).

**Résultat : plus aucune carte du corpus ne refuse de charger, sauf la famille Nuts.**

### 7.5 Le plafond, dit honnêtement

**Scythe MAP30 boote, et est INJOUABLE.** Après chargement il reste `fr18K lg11K` — sous la
taille des lumps de texture (~17,5 Ko). **Charger n'est pas jouer.** La rendre jouable
demande un autre ordre de grandeur — une cartouche 4 Mo — pas un levier de plus. À dire
tel quel : c'est exactement le genre de phrase qui fait revenir les gens.

Autre limite du même jour, et bonne leçon de design : MAP30 chargeait **entièrement** puis
mourait dans un `Z_Malloc` fatal de 36 888 o pour le ring du lead-fill, alloué paresseusement
à la première frame — donc **après** que le niveau ait pris sa part. **Un sous-système
optionnel DEMANDE la mémoire, il ne l'exige pas.**

### 7.6 Pourquoi ça compte — l'horizon

C'est le seul endroit de l'épisode où l'ambition se dit, et elle tient dans le titre du fil
SegaXtreme : **« make a WAD, get a Saturn FPS ».**

- **Mimas est agnostique du jeu** : l'IWAD est identifié en **scannant le contenu des lumps**,
  pas le nom de fichier. Doom 1, Ultimate, Doom II, TNT, Plutonia, et les PWADs de la
  communauté passent par le même binaire.
- **Pas de format de niveau propre à la Saturn, pas de toolchain Saturn.** Un mappeur qui n'a
  jamais touché un SH-2 écrit son WAD sur PC et l'exécute sur du vrai matériel. C'est ça,
  « un nouveau FPS sur Saturn » sans écrire une ligne de code Saturn.
- **Les jeux du même moteur, par difficulté croissante** (position publiée sur le fil, post
  #12) : **Heretic** = même format de carte, pas de VM de script → le plus proche ;
  **Strife** = pareil, plus les dialogues et les quêtes ; **Hexen** = le plus dur (VM de
  bytecode ACS, polyobjects, sauvegarde de hub) — et le hub exige des **sauvegardes**, que le
  CD en lecture seule interdit. Ce n'est pas un détail de plus, c'est un blocage de fond.
- **Ce qui manque encore, à nommer** : pas de **DEHACKED** (donc l'endgame « mods » est
  fermé), pas de fusion de PWAD au runtime, TNT et Plutonia sont **mal identifiés** comme
  doom2 (mauvais textes d'intermission et de fin), sauvegardes non fonctionnelles.

### 7.7 Le reste de la fenêtre, en une ligne

- **Le tapis roulant des composites, tué dans le WAD** (14/08, `a31601f` +
  `tools/flatten_textures.py`) : les textures sont ré-émises **hors ligne** en bandes
  verticales non chevauchantes, toute colonne devient mono-patch, `R_GenerateComposite` n'est
  plus jamais appelé. **`MST` 277 → 61-142. Zéro ligne de moteur.**
- **Le gouverneur de LOD** (15/08) : déclenche sur la frame entière, puis dégrade la phase
  **dominante**.

---

## 8. Les chiffres — et la règle d'honnêteté

🔴 **Règle non négociable** : le gain en fps est **[Ymir]**, les images sont **[HW]**. Ne
jamais afficher l'un sur l'autre sans le tag. Console ≈ 3-5× plus lent qu'Ymir
(`ymir-not-a-perf-oracle`), et **la re-validation console du fence n'est pas faite**.

### A/B du present `[Ymir]`, 2026-08-19

| | AUTO 1-cycle + lead-fill | present manuel VBE |
|---|---|---|
| fps | 18,4 – 19,4 | **20,0 – 24,2** |
| MST | 50 – 55 | **41 – 50** |
| watchdog `w` | — | **0** |
| fence | — | 13 – 15 ms |
| trous en rotation | oui | **aucun** |

Step 1, même soir `[Ymir]` : `ec32` avec `g0` partout, **18-25 fps en mêlée**, fence 3-17 ms.

### Captures console du 20/08 `[HW]` — ligne 0, 1 échantillon / 2 s

| clip | contenu | fps instantané | moyenne `a` | MST |
|---|---|---|---|---|
| `184951` | Doom 1 sw. E1M1, 1ʳᵉ salle | 6,0 – 22,0 | 7,8 – 15,8 | 45 – 166 |
| `185046` | E1M1 combat / acide / dernière salle | 3,7 – 21,0 | 9,2 – 15,1 | 47 – 270 |
| `185147` | E1M2 salle principale + escaliers | 5,9 – 15,2 | 8,2 – 13,7 | 65 – 169 |
| `185408` | TNT MAP01 porte + mêlée | 5,5 – 18,0 | 7,9 – 12,5 | 55 – 181 |
| `185508` | TNT MAP01 balade + ciel HW | 6,2 – 15,5 | 8,5 – 13,4 | 64 – 161 |
| `185618` | TNT MAP01 balade | 6,6 – 15,5 | 10,0 – 12,3 | 64 – 151 |

**Le chiffre honnête à dire** : *8 à 15 fps de moyenne sur du vrai matériel, sur les vraies
cartes PC. Ce n'est pas le sujet de l'épisode — le sujet, c'est qu'il n'y a plus de trous et
que du CPU est revenu.*

⚠️ **Aucune photo build-contre-build 20/07 vs 20/08 sur les fps** : scènes différentes, WADs
différents, overlay différent — `interbuild-perf-noise`. Le 20/07 sert **uniquement** à
montrer l'artefact.

---

## 9. La coupe (cible ~7:40)

Construire chaque pièce en `.mp4` au format final, la **mesurer à `ffprobe`**, puis
concaténer (gotcha 4 : les arrondis dérivent jusqu'à +0,29 s/pièce).

| # | pièce | source | in → out | durée |
|---|---|---|---|---|
| A0 | carte titre | still `185147` ~0:45, assombri | — | 10,0 |
| A1 | **intro** : E1M1, 1ʳᵉ salle, ciel HW | `GenkiArcade-20260820-184951.mp4` | 21,0 → 54,2 | 33,2 |
| A2 | carte **« what this is »** | — | — | 16,0 |
| B0 | **le bug**, ralenti ×0,25 + loupe | `Videos/2026-07-20 11-26-13.mp4` (§10) | 3 s de source | 12,0 |
| B1 | carte « two clocks » | — | — | 16,0 |
| B2 | E1M1 combat + acide + fin | `…-185046.mp4` | 0 → 50,0 | 50,0 |
| C1 | **bloc comparatif** avant / après | §10 | — | 12,0 |
| C2 | carte **l'erratum** | — | — | 20,0 |
| D1 | E1M2 + escaliers / ciel HW | `…-185147.mp4` | 0 → 48,0 | 48,0 |
| D2 | TNT porte + mêlée | `…-185408.mp4` | 9,0 → 52,0 | 43,0 |
| E1 | carte **DELETED** | — | — | 16,0 |
| F0 | carte **what a WAD asks for** (§7.1-7.3) | — | — | 20,0 |
| F0b | carte **38 maps, then 0** (§7.4-7.5) | — | — | 18,0 |
| F1 | TNT balade + sol bleu + ciel HW | `…-185508.mp4` | 0 → 55,0 | 55,0 |
| F2 | TNT balade (queue) | `…-185618.mp4` | 0 → 10,0 | 10,0 |
| G1 | carte **what it opens** | — | — | 16,0 |
| G2 | **split-screen 2 joueurs** | `Doom - 2 Player - nodebug.mp4` | 0 → 50,0 | 50,0 |
| H1 | carte **still broken** | — | — | 18,0 |
| H2 | carte **make a WAD, get a Saturn FPS** (§7.6) | — | — | 16,0 |
| H3 | générique | — | — | 12,0 |
| | | | **total** | **491,2 s = 8:11** |

> **CONSTRUIT.** `bash tools/devlog/build_ep1.sh` fait toute la chaîne
> (`cards` / `pieces` / `c1` / `master` en cibles séparées), et
> `tools/devlog/ep1_annot.py` génère le `.ass` **depuis les durées mesurées à
> `ffprobe`**, jamais depuis le plan. Sortie : `C:/Users/pcico/Videos/mimas-devlog1/`.
> Écarts assumés par rapport au plan initial, chacun pour une raison :
> - **pièces intermédiaires en `crf 16 / veryfast`** — la passe master ré-encode tout
>   en `crf 18 / medium`, donc les encoder deux fois aux réglages finaux doublerait le
>   temps machine pour une qualité que la seconde passe jette.
> - **C1 passe de 20 s à 12 s** (3 s de source par côté au lieu de 5) : sur 5 s le
>   côté droit passait la moitié du temps face à un mur non éclairé — rien à comparer.
> - **fond de carte titre changé** : `185147` à 45 s était une frame d'effondrement de
>   budget pleine de quads rouges et bleus non texturés. La carte aurait expédié
>   l'artefact dont l'épisode parle (gotcha 5, et il a mordu ici). Remplacé par
>   `185508` à 47 s — ciel matériel TNT, murs texturés, rien de cassé — **rogné de sa
>   ligne d'overlay** avant assombrissement.
> - **pas de deux-points dans un `drawtext`** : les guillemets simples ne protègent pas
>   le `:` au niveau du filtergraph, il termine l'option et le graphe ne parse plus.
>   Les légendes de C1 sont écrites sans deux-points.

Les deux cartes `F0`/`F0b` tombent **juste avant le run TNT**, et c'est voulu : TNT est
précisément un gros WAD commercial non-shareware streamé du disque sans cartouche. La carte
pose la question, les images sont la réponse.

**Version longue** (~9:20) : B2 → 59,0 · D1 → 60,4 · D2 → 49,4 · F1 → 69,4 · G2 → 60,5, et
`C3_numbers` (14 s) inséré après C2.

⚠️ Les six clips 1p sont en **720×480**, et le conteneur ment sur la cadence de quatre
d'entre eux (`r_frame_rate` = `120/1` sur `184951`, `185046`, `185618` et le clip 2 joueurs).
**Force `fps=60` au filtrage.** Master 1080p60 : `scale=1440:960:flags=neighbor` puis pad —
**pas de lanczos** sur du pixel Doom plein écran (le lanczos de la recette §4 est pour les
vignettes côte à côte, où l'aliasing serait pire).

---

## 10. Le bloc comparatif — et la réponse à ta question

### Faut-il aligner les deux parcours par des coupes ?

**Non — pas pour ce bloc.** L'artefact dure **un field**. À vitesse réelle, sur du 60 fps
YouTube, il est invisible : ce qu'il faut, c'est **3 à 5 s de la même *nature* de
mouvement** (rotation en intérieur devant un raccord mur/plafond), **ralenties ×0,25 et
zoomées ×4**. Un alignement de parcours ne montrerait rien de plus et coûterait une soirée.

**Ce que je te demande est donc minuscule : un seul timecode de ~4 s** dans les 2:51
utilisables. J'ai balayé le clip à 2 images/s sans le trouver — c'est attendu, à cette
cadence je ne vois qu'une occurrence sur trente. Toi tu sais à quoi il ressemble.

Critères, non négociables (l'artefact n'existe que là) :

1. **En intérieur.** Dehors le trou est invisible : un plan infini (ciel, sol RBG0) n'a pas
   de bord, un mur vieux d'un field n'a rien avec quoi être en désaccord.
2. **Rotation continue et rapide.** L'amplitude est proportionnelle à la vitesse.
3. **Un raccord mur ↔ plafond ou mur ↔ sol *logiciel*** dans le champ.
4. **1 joueur.**

Recette si tu veux le chercher à l'image près (4 images/s, zoom ×2 sur le haut du champ) :

```
ffmpeg -v error -ss 62 -i "C:/Users/pcico/Videos/2026-07-20 11-26-13.mp4" \
  -vf "fps=4,crop=960:600:480:120,scale=960:600:flags=neighbor,tile=3x3" \
  -frames:v 1 -y avant_62.png
```

### Et l'alignement complet des deux parcours ?

**Option secondaire, honnête, 20 s max** — le §4 de la méthode s'applique tel quel : les
deux courses ancrées sur **le même événement de jeu** (pas un timestamp), la plus courte
figée avec `tpad=stop_mode=clone`, une horloge par côté. Tes deux divergences (elle part à
gauche vers les escaliers, tu sors à droite puis fais les secrets) se règlent en coupant le
segment commun : **1ʳᵉ salle → couloir**, avant la divergence.

⚠️ Mais alors le carton doit dire **ce que ça n'est pas** : *« same level, same hardware, one
month apart — not a benchmark »*. Deux scènes, deux chaînes d'acquisition, deux builds : lire
des fps là-dessus serait faux (`interbuild-perf-noise`).

**Ma recommandation : garder C1 pour l'artefact (ralenti + loupe), et ne faire le parcours
côte à côte que si l'épisode a besoin d'air.**

### Le montage du bloc C1

```
ffmpeg \
  -ss <T_AVANT> -t 5.0 -i "C:/Users/pcico/Videos/2026-07-20 11-26-13.mp4" \
  -ss <T_APRES> -t 5.0 -i "C:/Users/pcico/Downloads/GenkiArcade-20260820-185618.mp4" \
  -f lavfi -t 20.0 -i "color=c=0x0B0906:s=1920x1080" -filter_complex \
  "[0:v]fps=60,crop=480:270:720:270,setpts=4*PTS,scale=944:531:flags=neighbor,setsar=1,trim=0:20,setpts=PTS-STARTPTS[l]; \
   [1:v]fps=60,crop=320:180:200:150,setpts=4*PTS,scale=944:531:flags=neighbor,setsar=1,trim=0:20,setpts=PTS-STARTPTS[r]; \
   [2:v][l]overlay=8:170[x];[x][r]overlay=968:170, \
   drawbox=x=7:y=169:w=946:h=533:color=0x4A4030@1:t=2, \
   drawbox=x=967:y=169:w=946:h=533:color=0x4A4030@1:t=2, \
   drawtext=text='20 JUL - lead-fill ON':x=40:y=716:fontsize=34:fontcolor=0xC86A4F, \
   drawtext=text='20 AUG - manual present':x=1000:y=716:fontsize=34:fontcolor=0x5EAE88, \
   drawtext=text='0.25x speed - the gap lasts ONE field':x=40:y=782:fontsize=28:fontcolor=0xA39B8C[v]" \
  -map "[v]" -r 60 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -an -y c1.mp4
```

- Les `crop` sont à recaler **une fois les timecodes connus** : viser le raccord
  mur/plafond, pas le centre de l'écran.
- **Le crop de gauche est plus large** (480×270 sur du 1080p) que celui de droite (320×180
  sur du 480p) pour que le **facteur de loupe soit identique**. Vérifier à l'œil : si un côté
  est plus zoomé que l'autre, la comparaison ment.
- `flags=neighbor` partout : ce sont des pixels, pas une photo.

### Le piège d'honnêteté, à dire à l'écran

Les deux captures ne sont **ni la même scène ni la même définition** (1080p OBS le 20/07,
720×480 sur la carte d'acquisition le 20/08). Une phrase suffit — elle est déjà dans le
filtre — et elle coupe court au commentaire qui arriverait sinon.

---

## 11. Vidéos de référence (Doom DOS / Doom Saturn retail)

### 🥇 Doom DOS — capturer, pas télécharger

Aucune vidéo en ligne ne donnera *ce* parcours sur *ce* E1M1. Chocolate Doom est fidèle au
vanilla à la frame près :

```
choco-doom.exe -iwad doom1.wad -warp 1 1 -width 640 -height 400 -nofullscreen
```

Usage recommandé : **10 s dans l'intro**, cadré *« this is the map, and the speed it was
written for »*. **Jamais en duel de fps** au milieu — 35 contre 12, tout le monde le sait.

### 🥈 Doom Saturn retail — le capturer sur la Saturn

La console et l'ODE sont là : booter le retail et le capturer **sur le même Genki, même
câble, même parcours** vaut dix fois un téléchargement, et c'est la monnaie de cette
communauté.

Fichier de secours (Internet Archive, PAL 50 Hz, VHS/DVD) :

```
curl -L -o saturn-retail-pal.mp4 \
  "https://archive.org/download/doom-sega-saturn-pal-gameplay-full-demostration/Doom%20Sega%20Saturn%20PAL%20Gameplay%20%28Full%20Demostration%29.mp4"
```
<https://archive.org/details/doom-sega-saturn-pal-gameplay-full-demostration> — 45,9 Mo.
⚠️ **PAL 50 Hz** : le retail y tourne encore plus lentement qu'en NTSC. L'utiliser pour une
comparaison de vitesse serait malhonnête — afficher la région, ou capturer en NTSC.

### 🔴 Deux avertissements de cadrage

1. **« Le Doom Saturn retail est du pur logiciel » est FAUX**, et la correction est arrivée
   publiquement sur le fil (TrekkiesUnite118 post #2 ; **fafling** post #5) :
   > *« Saturn Doom does software render the flats and uses VDP1 to accelerate the mixing of
   > that rendering with the walls and sprites which are fully rendered by VDP1. »*

   Le veto Carmack reste vrai comme **histoire de production** ; la conclusion technique ne
   suit pas. Formulation sûre : *le retail met déjà murs et sprites sur VDP1 ; Mimas ajoute
   le sol et le ciel sur VDP2, les deux SH-2 sur le renderer, et les vraies cartes PC.*
2. **Jamais de duel de fps avec fafling.** Sa Fix Patch optimise le binaire commercial
   (11 → ~30 fps sur les niveaux simples). Mimas fait autre chose. **Complémentaire, jamais
   concurrent** — et c'est lui qui a débloqué cet épisode.

**Recommandation nette pour l'épisode #1 : pas de bloc comparatif inter-machines.** Doom DOS
pour 10 s d'intro, le Saturn retail pour un épisode « d'où on part », avec une capture NTSC
maison.

---

## 12. Cartes

```
python tools/devlog/ep_vdp1_present_cards.py out/ --backdrop title_frame.png
ffmpeg -loop 1 -i out/B1_two_clocks.png -t 16 -r 60 -c:v libx264 -preset medium \
       -crf 18 -pix_fmt yuv420p -y b1.mp4
```

⚠️ **Le fond de la carte titre doit venir d'une frame propre** — la vérifier pour l'artefact
dont l'épisode parle (gotcha 5 : la première carte de Tethys ep6 contenait précisément le
bug qu'elle annonçait). Candidat : `185147` à ~0:45, l'escalier vers le ciel HW.

---

## 13. Annotations (texte prêt à coller, sans pronom)

Copier `tools/devlog/annot_template.ass`, un bloc `Dialogue` par écran, **chaque texte dure
tout son plan**. Les temps ci-dessous suivent la coupe §9 — **re-mesurer chaque pièce à
`ffprobe` et décaler** (gotcha 4).

`{A}` ambre (titre) · `{B}` os (corps) · `{G}` vert (un gain) · `{R}` rouille (un problème).

**Découpage MESURÉ du master construit** (`ffprobe`, pas le plan) :
`A0` 0:00.00 · `A1` 0:10.00 · `A2` 0:43.20 · `B0` 0:59.20 · `B1` 1:11.20 · `B2` 1:27.20 ·
`C1` 2:17.20 · `C2` 2:29.20 · `D1` 2:49.20 · `D2` 3:37.20 · `E1` 4:20.20 · `F0` 4:36.20 ·
`F0b` 4:56.20 · `F1` 5:14.20 · `F2` 6:09.20 · `G1` 6:19.20 · `G2` 6:35.20 · `H1` 7:25.20 ·
`H2` 7:43.20 · `H3` 7:59.20 · **fin 8:11.20**.

Le `.ass` est **généré depuis ces mesures** par `tools/devlog/ep1_annot.py` — les textes
ci-dessous sont sa source, pas une transcription à recopier à la main.

| ~t | écran | texte |
|---|---|---|
| 0:14 | A1 intro | `{A}REAL HARDWARE` / `{B}Sega Saturn, ODE, no emulator` / `{B}Doom 1 shareware, E1M1, straight from the WAD` |
| 0:30 | A1 | `{A}THE WALLS ARE VDP1 QUADS` / `{B}the sky is a VDP2 scroll layer, the floor a VDP2 rotation plane` / `{B}both SH-2 run the renderer` |
| 1:00 | B0 le bug | `{A}THE HOLE` / `{B}a sliver of background between a VDP1 wall and the software floor` / `{R}open since 17 June - one day after the first VDP1 wall` |
| 1:11 | B1 carte | *(le texte est sur la carte)* |
| 1:30 | B2 | `{A}SIX PRESENTS, ALL DEAD` / `{B}draw-gated, slSynch, NBG1-couple, coherent-pair, field-lock` / `{R}every one of them fixed a symptom` |
| 1:47 | B2 | `{A}FOUR GEOMETRIC FIXES, ALL WORSE` / `{B}shift the quad, shift the clip window, sweep the gain, re-project` / `{R}symmetric worsening at every amplitude = the walls are not displaced` |
| 2:04 | B2 | `{A}THE PATCH THAT WORKED` / `{B}lead-fill: the hole was repainted by the CPU, on both SH-2` / `{R}a corrective that works takes the pressure off the cause` |
| 2:17 | C1 | *(les cartons sont dans le filtre, §10)* |
| 2:37 | C2 carte | *(le texte est sur la carte)* |
| 3:00 | D1 | `{A}THE SKY IS VDP2` / `{B}NBG0 scroll layer - the CPU never draws a sky pixel` |
| 3:18 | D1 | `{A}AND THEN THE EMULATOR DISAGREED WITH THE MANUAL` / `{B}by exactly one field, and no register says which buffer is on screen` / `{G}the fix is VBE erase and change - the one sequence both machines time alike` |
| 3:50 | D2 | `{A}FAFLING CALLED IT` / `{B}"if you have VDP1 switching its framebuffers after every vblank,` / `{B}it's not a good idea in a variable framerate game like Doom"` / `{G}SegaXtreme, 3 August. The diagnosis is his; the driver is not.` |
| 4:10 | D2 | `{A}NOT MAGIC` / `{B}+1.5 to +5 fps [emulator A/B] - console numbers are not in yet` / `{B}what it really bought is a plot window the size of a whole frame` |
| 4:28 | E1 carte | *(le texte est sur la carte)* |
| 4:44 | F0 carte | *(le texte est sur la carte)* |
| 5:04 | F0b carte | *(le texte est sur la carte)* |
| 5:26 | F1 | `{A}TNT: EVILUTION` / `{B}a full commercial WAD, streamed from the disc, no RAM cart` / `{B}the IWAD is identified by scanning lump contents, not by filename` |
| 5:48 | F1 | `{A}THE FLOOR IS VDP2 TOO` / `{B}RBG0 rotation plane, one bank, shaded by a palette switch` |
| 6:27 | G1 carte | *(le texte est sur la carte)* |
| 6:47 | G2 split | `{A}TWO PLAYERS, ONE SATURN` / `{B}two viewpoints, one framebuffer, both pads, one disc` / `{B}the manual present runs per frame here too` |
| 7:33 | H1 carte | *(le texte est sur la carte)* |
| 7:51 | H2 carte | *(le texte est sur la carte)* |

---

## 14. Miniature

**CONSTRUITE** — `mimas-devlog1/thumb.png` + `.jpg` + `_small.png` :

```
cd tools/devlog
ffmpeg -ss 27.75 -i "C:/Users/pcico/Videos/2026-07-20 11-26-13.mp4" \
  -vf "crop=1716:1008:88:38,scale=1920:1080:flags=neighbor,crop=1200:675:660:40" \
  -frames:v 1 -y thumb_before.png
ffmpeg -ss 21.0 -i "C:/Users/pcico/Downloads/GenkiArcade-20260820-185046.mp4" \
  -vf "scale=1920:1080:flags=neighbor,crop=1200:675:660:40" \
  -frames:v 1 -y thumb_after.png
python mkthumb.py thumb_before.png thumb_after.png thumb.png \
  --left "BEFORE" --right "AFTER" --left-year "20 JUL" --right-year "20 AUG" \
  --left-cx 600 --right-cx 600 --caption "REAL SATURN HARDWARE - DEVLOG #1"
```

**Deux corrections faites en regardant le `_small.png`, et elles se généralisent :**
- Le premier essai cadrait sur **la loupe du bloc comparatif** (712×400) : à 320×180 les
  deux côtés n'étaient plus que de la texture abstraite, Doom n'était pas reconnaissable.
  **Une miniature n'a pas à prouver l'artefact** — un défaut d'un field n'y sera jamais
  lisible. Elle doit être *reconnaissable*. Cadrage retenu : 1200×675, la salle entière.
- La légende `ONE WORD IN THE MANUAL - REAL SATURN - DEVLOG #1` **débordait des deux
  côtés** : `mkthumb` ne mesure pas la largeur du texte. Raccourcie à
  `REAL SATURN HARDWARE - DEVLOG #1`.
- **Le même lift des deux côtés** (le script s'en charge). Ici le piège est inversé par
  rapport à Tethys : le côté « après » est la source la **plus molle** (480p) et se retrouve
  désavantagé — ne pas compenser, le dire.
- Regarder le `_small.png` (320×180). Si le trou n'est pas lisible à cette taille,
  **l'entourer** ou changer de frame.

---

## 15. YouTube

**Titre** :

> `Doom on Sega Saturn: VDP1 walls, VDP2 sky, and a two-month bug closed (Devlog #1)`

**Description** — pas de retour à la ligne dur, une ligne par paragraphe / puce, **pas de
pronom** :

```
Mimas is a hardware-rendered Doom for the Sega Saturn. It takes a normal Doom WAD and renders it with the VDP1 and the VDP2, across both SH-2 CPUs. Everything in this video was captured on a real Saturn. No emulator.

This is the first devlog, and it opens on the oldest bug in the project. It appeared on 17 June 2026, one day after the first VDP1 wall reached the screen, and it survived six different presentation mechanisms, four geometric corrections and one expensive CPU workaround. The cause turned out to be one wrong word in the official SEGA VDP1 manual.

In this episode:
- The hole: a sliver of background between a VDP1 wall and the software floor. Only while turning, only indoors, only for one field.
- Why every fix failed: the draw-completion flag latches on silicon and never on the emulator, the framebuffer blit was never fenced, and the manual's own erratum turned the manual-mode driver into a no-op.
- The erratum: the developer CD says "write 0 to the VBE, FCM and FCT registers". The Kronos-corrected manual says "0 to VBE and FCT, 1 to FCM". FBCR = 0x0000 is automatic mode, which the manual's own table states on the facing page.
- The last twist: the emulator and the manual disagree by exactly one field, and no register reveals which framebuffer is on screen. The fix is VBE erase and change, the one swap sequence both machines time identically, and the one SlaveDriver shipped in 1996.
- What it paid for: the CPU workaround is deleted, and the VDP1 now gets a plot window the size of a whole game frame. Sprite budget doubled, from 16 to 32 things per frame.
- The other wall, and the one that decides how far this can go: memory. The engine's own source asks for a 6 MiB zone heap. A Saturn has 2 MB of work RAM in total, split across two banks that are not interchangeable, and the Doom zone gets 1016 KB of the slow one. A map that fits in the free bytes can still refuse to load, because the largest single allocation wants about 110 KB in one unbroken run.
- The struct diet that fixed it, measured across the 415 maps of the test corpus: seg_t 32 to 14 bytes, node_t 52 to 28, line_t 64 to 24, side_t 20 to 16, plus loading each lump in place instead of through a staging buffer. Shrinking seg_t and node_t freed real RAM and exactly zero maps: on every blocked map the biggest allocation was LINEDEFS. Shrinking that one took 38 maps over the wall down to 0.
- The honest ceiling: Scythe MAP30 now boots, and is unplayable. Loading is not playing, and closing that gap needs a 4 MB cartridge, not one more lever.
- Why any of this matters: make a WAD, get a Saturn FPS. There is no Saturn level format and no Saturn toolchain here. A mapper who has never touched an SH-2 authors on PC and runs on real hardware.
- Two-player split-screen on one Saturn, and TNT: Evilution streamed from the disc with no RAM cart.

Mimas runs on the Saturn Ring Library (SRL) by ReyeMe. The Doom engine core is doomgeneric / Chocolate Doom, GPLv2. No IWAD is distributed: bring your own WAD.

With help from: fafling, who named the cause on the SegaXtreme thread on 3 August, and whose team's corrected VDP1 / VDP2 manuals made the erratum findable; TrekkiesUnite118, who corrected the history of the retail port; ReyeMe, for SRL; slygamer, for the hardware captures.

Non-commercial fan project. Doom is a trademark of id Software / ZeniMax.

#SegaSaturn #Doom #retrodev #homebrew #SH2 #VDP1

Chapters:
00:00 A hardware-rendered Doom
00:59 The hole
01:11 Two clocks, one screen
01:27 Six presents, all dead
02:17 Before and after
02:29 One word in the manual
02:49 The emulator disagreed
03:37 fafling called it
04:20 What got deleted
04:36 What a WAD asks for
05:14 TNT, streamed from the disc
06:19 What it opens
06:35 Two players, one Saturn
07:25 What is still broken
07:43 Make a WAD, get a Saturn FPS
```

⚠️ Ces temps sont ceux du **master construit** (mesurés). Si tu recoupes une pièce,
re-lance `bash tools/devlog/build_ep1.sh master` : `ep1_annot.py` réimprime la table et
les annotations se recalent seules — mais **ces chapitres-là sont à remettre à jour à la
main**.

⚠️ Premier chapitre obligatoirement `00:00`, minimum 10 s d'écart — **re-mesurer après le
concat final**.

---

## 16. Le post SegaXtreme

Même fenêtre que la vidéo, dans **le fil existant** (thread 38228). Ouvrir avec le
disclaimer habituel. En-têtes `── SECTION ──`. **Citer les registres et les `fichier:ligne`,
pas les numéros de page** — sauf pour l'erratum, où la page *est* le sujet : citer alors le
registre **et** la page, avec la mention « Kronos-corrected ». **Pas de pronom.**

1. **`── THE BUG ──`** — l'artefact, sa date de naissance (17 juin), ses invariants : nul à
   l'arrêt, uniquement au raccord VDP1/logiciel, temporel, reproduit sur Ymir **et** console.
2. **`── WHY EVERY FIX FAILED ──`** — les cinq poisons du §3, avec les registres :
   `EDSR.CEF` (bit 1) latche 30-60 % en 1-cycle sur silicium et 0 % en manuel sur Ymir ;
   `FBCR (100002H)` ; **`VBE` est le bit 3 de `TVMR (100000H)`, pas de FBCR** — la table
   p.38 les nomme ensemble et ça a déjà trompé du monde.
3. **`── THE ERRATUM ──`** — les deux phrases côte à côte, la table `000 = 1-cycle` en
   regard, et la conclusion : toute implémentation faite depuis le CD développeur écrit
   `0x0000` et ne fait rien.
4. **`── THE ONE-FIELD DIVERGENCE ──`** — Table 4.3(a) contre `vdp.cpp
   BeginHPhaseLeftBorder` / `VPhase == LastLine` ; l'issue #141 d'Ymir documente que ce
   timing est un compromis assumé. Convergence = VBE erase & change (`SCL_VBLV.C`,
   intervalle `0xfffe`).
5. **`── THE RECIPE ──`** — les 5 étapes de `saturn-refs/knowledge/HW_VDP1.md` §5, écrites
   pour être reprises telles quelles. C'est la partie qui sera copiée.
5bis. **`── THE OTHER WALL: MEMORY ──`** (§7) — c'est la section qui intéressera le plus les
   développeurs du forum, et la seule qui parle de **leur** problème s'ils portent quoi que
   ce soit. Contenu : 6 MiB demandés par `core/i_system.c:58` contre 1 040 384 o accordés ;
   les deux bancs et l'asymétrie SCU-DMA `[SCU Final Specifications List n° 04]` ; **la
   contiguïté et non la capacité** (~110 Ko d'un seul tenant, deux blocs `PU_STATIC` garés
   qui coupent la zone) ; le tableau du régime sur 415 cartes avec **le fait contre-intuitif
   en tête** (`seg_t` + `node_t` = zéro carte libérée) ; le chargement en place (46-203 Ko de
   pic transitoire, zéro octet de disque) ; et le plafond dit franchement (Scythe MAP30
   boote, injouable). Ajouter la leçon de design — **un sous-système optionnel DEMANDE la
   mémoire, il ne l'exige pas** — avec le `Z_Malloc` de 36 888 o alloué paresseusement à la
   première frame qui tuait une carte déjà entièrement chargée.
6. **`── TWO CLAIMS IN THIS THREAD THAT WERE WRONG ──`** (la section qui gagne la confiance
   sur ce forum) :
   - **Le post du 2 août annonçait le correctif** — *« the game now waits for the display's
     tick before presenting a frame, locking the two clocks together »*. C'était le
     field-lock : il épingle **le blit**, pas le **swap**. La sonde lisait `A2/2` sur toutes
     les captures — appariement parfait — **et les trous étaient toujours là**. Parké le
     lendemain.
   - **Le post d'ouverture décrivait le Doom Saturn retail comme du pur logiciel.** Corrigé
     dans le fil même par TrekkiesUnite118 et fafling : les flats sont rendus en logiciel,
     murs et sprites sont sur VDP1. L'erreur avait été propagée ailleurs ; elle est corrigée.
7. **`── STILL BROKEN, NAMED ──`** — re-validation console du fence pas faite (les +1,5 à
   +5 fps sont **[Ymir]**) ; le tic de jeu coûte maintenant plus cher que le renderer sur
   console (165 ms contre 141) ; sols VDP1 refusés à huit reprises ; le present en split
   jamais A/B'd ; Scythe MAP30 boote et est injouable.
8. **Remerciement à fafling**, explicite et précis : le diagnostic vient de lui ; la route
   SGL n'était pas praticable en `SRL_FRAMERATE=0` (SGL réécrirait `FBCR = 0` à chaque
   vblank-out, contre le driver), donc le driver est maison ; et ses manuels corrigés sont ce
   qui a rendu l'erratum trouvable.

---

## 17. Reste à faire

1. **Le timecode du plan « avant »** (§10) — un seul, ~4 s, dans les 2:51 utilisables.
2. **Le rôle exact de slygamer** au générique : `hardware capture and playtesting` est ma
   déduction. Corriger, et dire qui d'autre doit y figurer.
3. Décider si le **parcours côte à côte** (§10, option secondaire) entre dans l'épisode.
