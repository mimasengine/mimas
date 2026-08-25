<!-- REBUILT 2026-08-24 17:45 after this file was destroyed by a non-atomic
     patch script (open(w) truncates, the write then raised on a bad emoji
     escape; the file was untracked, so git could not restore it).

     Reassembled from the session transcript, which had recorded every command
     that wrote here.  VERBATIM: sec.0-12 (plan v5), sec.11.B (third voice pass),
     sec.12-bis, sec.12-ter, sec.13.  REWRITTEN from the build script and the
     audit docs, and so possibly not word-for-word the lost text: the C8/C10
     five-poison ranking inside sec.8, the sec.12 checklist, and sec.12-quater.
     Every NUMBER below was re-derived from ep1_build.py or re-measured on the
     master, not copied from memory. -->

# Devlog #1 — plan de montage détaillé (v5)

> Méthode et recettes : [`DEVLOG_VIDEO.md`](DEVLOG_VIDEO.md).
> Dossier de fond vérifié : [`DEVLOG_EP_VDP1_PRESENT.md`](DEVLOG_EP_VDP1_PRESENT.md).
> Cartes : `python tools/devlog/ep1_diagrams.py` · voix : `bash tools/devlog/ep1_vo.sh`.

**Règle de style, non négociable** : aucun pronom dans le **texte à l'écran** (cartes,
incrustes, description, post). Le mécanisme est sujet. La **voix off dit « je »** — c'est un
récit personnel, c'est la voix qui le porte, pas les cartons.
Texte écran : **anglais**. Prose de travail : français.

**Fenêtre élargie 2026-08-23** : le récit couvre maintenant jusqu'à **`53e2b2f` (21/08 10:39)**.
C'est ce qui change la carte finale (§8, C10).

---

## 0. Ce qui change par rapport à la v4, et pourquoi

| point | v4 | v5 |
|---|---|---|
| D1 « two chips » | image saine en exemple | **supprimée** — n'illustrait rien |
| D1 « two pictures » | schéma abstrait, bancal | **supprimée** — remplacée par la **décomposition d'une vraie frame** (C1) |
| zoom ×8 du comparatif | 1 seule frame fautive, en fin d'extrait | **reciblé** sur 3,00–4,00 s de `11-22-01`, où il y a ~10 frames fautives d'affilée, + **arrêt sur image décomposé** |
| D4 fields | pas de notion de cycle | **D4 v2** : règle de fields, définition *field*/*frame* du manuel, **ce qui ferme la fenêtre** |
| D5 corrections | schéma raté | **supprimée** — devient une incruste à puces |
| D3 zone | « maladroite » | **D3 v2** : la pièce qu'on essaie d'insérer, refusée run par run, puis rétrécie |
| `Doom TNT - 4 Player` | plan final | **retiré** (overlay + framerate au plancher) |
| `Doom-Probe-*` | envisagé | **retiré** (tests buggués + overlay) |
| clips 2 joueurs | 1 seul | **3** — `notext1` @0:35, `notext2`, `2 Player - nodebug` @8.6 |
| date de capture | absente | **incruste permanente**, coin bas-droit, séparée du storytelling |

---

## 1. Sources — arrêtées

### Solo « clean » (une seule ligne d'overlay en haut, image intacte)
| clé | fichier | contenu | durée | date écran |
|---|---|---|---|---|
| `D1a` | `GenkiArcade-20260820-184951` @24→fin | Doom 1 sw. E1M1, 1ʳᵉ salle | 30,3 s | 20 AUG 2026 |
| `D1b` | `GenkiArcade-20260820-185046` entier | E1M1 combat / acide / dernière salle | 59,1 s | 20 AUG 2026 |
| `D1c` | `GenkiArcade-20260820-185147` entier | E1M2 salle principale + escaliers | 60,5 s | 20 AUG 2026 |
| `T1a` | `GenkiArcade-20260820-185408` entier | TNT MAP01, porte + mêlée | 58,5 s | 20 AUG 2026 |
| `T1b` | `GenkiArcade-20260820-185508` **@20→fin** | TNT MAP01 + ciel HW · ⛔ **jamais 0–20 s** | 49,4 s | 20 AUG 2026 |
| `T1c` | `GenkiArcade-20260820-185618` **@0→10** | TNT MAP01 · ⛔ **jamais après 10 s** | 10 s | 20 AUG 2026 |

### Split 2 joueurs, **zéro overlay** — la charge utile de la carte finale
| clé | fichier | durée | date écran |
|---|---|---|---|
| `SP1` | `Doom - 2 Player - nodebug.mp4` **@8.6→fin** (avant = menu skill) | 52,0 s | 20 AUG 2026 |
| `SP2` | `Doom - Base - notext1.mp4` **@35→fin** | 18,6 s | 21 AUG 2026 |
| `SP3` | `Doom - Base - notext2.mp4` entier | 61,6 s | 21 AUG 2026 |

⛔ **Écartés** : `Doom TNT - 4 Player` (overlay + framerate au plancher), `Doom-Probe-*`
(tests buggués), `PLAYTHROUGH*` (c'est **Tethys**, pas Mimas).

### Le AVANT, pour le comparatif — fin juillet
| clé | fichier | TC | rôle |
|---|---|---|---|
| `J0` | `Videos/2026-07-20 11-22-01` **3,00 → 4,00 s** | 1 s | **le plan de preuve** — panoramique en intérieur, ~10 frames fautives d'affilée |
| `J1` | `Videos/2026-07-20 11-22-01` 0:00–0:32 | 32 s | 1ʳᵉ salle + salle de gauche |
| `J2` | `Videos/2026-07-20 11-26-13` 0:04–0:36 | 32 s | couloir |
| `J3` | `Videos/2026-07-20 11-26-13` 1:49–2:51 | 62 s | acide + extérieur + fin |

⚠ juillet = 1080p **30 fps** OBS, pillarbox `crop=1716:1008:88:38` · août = 480p **60 fps**
carte d'acquisition, `scale=1920:1080:flags=neighbor,setsar=1`. **À dire une fois à l'écran**
dans le bloc comparatif — deux chaînes d'acquisition, aucun fps à en tirer.

### Musique
- **Track 1** `Grave Loop Signals.wav` (4:19, sombre) → blocs 1 → 4
- **Track 2** `Grave Loop Signals (1).wav` (3:34, plus claire) → blocs 5 → 6

La bascule tombe **exactement où le récit passe du problème à la solution**. C'est la seule
raison d'en utiliser deux.

---

## 2. L'incruste de date — nouvelle, permanente

**Pourquoi** : chaque plan est daté, et le spectateur doit pouvoir dater ce qu'il voit sans
avoir à croire le narrateur. C'est aussi la garantie d'honnêteté du comparatif.

| | |
|---|---|
| position | **coin bas-droit du viewport 3D** — align 3, `MarginR 64`, `MarginV 178` |
| pourquoi là | le storytelling est en **bas-gauche** (align 1, même `MarginV`) ; le haut est pris par la ligne d'overlay du jeu, qui court jusqu'à ~90 % de la largeur |
| garde-fou | les incrustes bas-gauche sont **capées à 62 caractères par ligne** pour ne jamais atteindre le tiers droit |
| style | BONE `&H00D3E4ED` à 70 %, 26 px, pas de gras, interlettrage +1, contour 2 + ombre 2 (pas de plaque : trop lourd sur un plan permanent) |
| texte | `CAPTURED 20 AUG 2026 — SEGA SATURN` · `CAPTURED 20 JUL 2026 — SEGA SATURN` · `CAPTURED 21 AUG 2026 — SEGA SATURN` |
| durée | **tout le plan**, fondu 0,4 s au changement de source |
| comparatif | **un tampon sous chaque moitié**, même `y` |

---

## 3. Structure — ~9:15

| bloc | TC | durée | contenu |
|---|---|---|---|
| **1** | 0:00 | 0:24 | ouverture — Track 1, carte titre |
| **2** | 0:24 | 2:12 | **gameplay clean Doom 1 shareware**, 8 incrustes |
| **3** | 2:36 | 1:46 | **cartes #1 — le problème** (C1→C4) |
| **4** | 4:22 | 0:52 | **comparatif juillet ‖ août** (arrêt/décompo, ×4, PIP) |
| **5** | 5:14 | 2:04 | **cartes #2 — la solution** (C5→C10), Track 2 |
| **6** | 7:18 | 2:00 | **payoff split + TNT clean** + crédits |

---

## 4. Bloc 1 — ouverture (0:00 → 0:24)

Track 1 seule, pas de voix.

- **0:00–0:10** `D1b` @12 (couloir E1M1, mouvement lent), sous-exposé à 45 %, la carte titre
  par-dessus.
- carte titre : `MIMAS` / *Doom on the Sega Saturn* / `EPISODE 1 — ONE WORD IN THE MANUAL`
- **0:10–0:24** l'image remonte à 100 %, le titre s'efface, l'incruste de date entre.

---

## 5. Bloc 2 — gameplay clean Doom 1 (0:24 → 2:36)

`D1a` + `D1b` + `D1c` **bout à bout**, aucune coupe interne : 2:10 d'un parcours continu
E1M1 → E1M2. C'est le bloc « regarde la machine tourner » — il n'a rien à démontrer, il a
à **installer**.

Incrustes bas-gauche. Nouveau régime : **elles peuvent s'enchaîner et porter des puces**,
tant qu'on a le temps de les lire. Règle de lisibilité : **≥ 2,2 s par ligne**, ≤ 62
caractères, ≤ 4 lignes, ≥ 4 s de blanc entre deux groupes.

| TC | tenue | texte écran |
|---|---|---|
| 0:28 | 13 s | `DOOM 1 SHAREWARE — E1M1`<br>*Sega Saturn, 1994 · two SH-2 at 28 MHz · 2 MB of work RAM*<br>*hardware capture — no emulator* |
| 0:46 | 15 s | `WHAT DRAWS WHAT`<br>• *walls — VDP1 quads*<br>• *sky — a VDP2 scroll layer*<br>• *the largest floor — a VDP2 rotation layer* |
| 1:04 | 13 s | `AND THE REST IS THE CPU`<br>• *every other floor, every ceiling*<br>• *things, weapon, status bar*<br>*both SH-2 run the column renderer* |
| 1:24 | 11 s | `THE WAD IS READ FROM THE DISC, UNMODIFIED`<br>*no Saturn level format · no re-authoring · no RAM cart* |
| 1:40 | 11 s | `THE IWAD IS IDENTIFIED BY SCANNING LUMP CONTENTS`<br>*not by filename — one binary, any Doom WAD* |
| 1:56 | 13 s | `ON HARDWARE, 20 AUGUST` `[HW]`<br>• *8 to 15 fps average across these three clips*<br>• *worst case 3.7 · best case 22*<br>*that is not the subject of this episode* |
| 2:16 | 9 s | `E1M2` *(au changement de carte)* |
| 2:28 | 8 s | `EVERY WALL IN THIS SHOT WAS BROKEN FOR TWO MONTHS` |

La dernière incruste est la **charnière** : elle referme le bloc et ouvre le suivant.

---

## 6. Bloc 3 — cartes #1 : le problème (2:36 → 4:22)

Quatre cartes. **Une seule est du texte pur.** La première est une vraie image démontée.

### C1 — `THE STACK` — 40 s — *décomposition d'une vraie frame*

**Remplace D1_two_chips et D1_two_pictures.** Plus aucun schéma abstrait : on prend **la
frame fautive elle-même** et on la met à plat.

**Déroulé, 5 temps :**

| t | ce qu'on voit |
|---|---|
| 0–5 s | la frame fautive plein écran, **arrêtée**. Un cadre rouille se dessine autour du bloc de mur qui ne colle pas. Rien d'autre. |
| 5–13 s | l'image **se sépare en trois plans** qui s'écartent en perspective (décalage 3D léger, ~14°). De l'arrière vers l'avant : **VDP2**, **CPU**, **VDP1**. |
| 13–22 s | chaque plan s'allume seul, les deux autres à 12 %. Étiquette + horloge : `VDP2 — sky scroll + rotation floor · t` / `CPU FRAMEBUFFER — floors, ceilings, things, weapon · t` / `VDP1 — the walls · ` **`t − 1`** |
| 22–32 s | le plan VDP1 **glisse latéralement** de la quantité exacte du décalage, puis revient : on voit le bloc fautif se mettre en place, puis se re-décaler. Sous-titre : *the walls are not misplaced. they are late.* |
| 32–40 s | les trois plans se rempilent, la frame fautive revient, le cadre rouille reste. |

**Ce qu'elle explique** : la pièce et les murs sont produits par **deux puces différentes,
dans deux images séparées, superposées à chaque frame** — et sur cette frame-là, celle des
murs est **plus vieille d'un field**.
**Pourquoi elle est là** : c'est le modèle mental sans lequel rien de la suite n'existe. Et
cette fois c'est **la vraie image**, pas un dessin.

🟡 **Ce dont j'ai besoin de toi pour la fabriquer** — §11.A.

### C2 — `WHY ROTATION IS THE WORST CASE` — 16 s

Sur la même frame arrêtée. Deux vecteurs partent du centre de l'écran : un court
(`WALKING — the picture shifts a few pixels`) et un long, sur toute la largeur
(`TURNING — the picture shifts across the whole screen`). Puis `J0` repart à ×0,25.

> `A TRANSLATION MOVES THE PICTURE. A ROTATION SWEEPS IT.`
> *one field of lateness, walking forward: a few pixels.*
> *one field of lateness, turning: the width of a wall.*
> *this is why every clean example in this episode is a turn.*

**Ce qu'elle explique** : pourquoi les trous apparaissaient surtout en rotation — le décalage
en x par field y est d'un ordre de grandeur au-dessus.
**Pourquoi elle est là** : elle justifie le choix des plans, et elle désamorce d'avance
« pourquoi je ne l'ai jamais vu chez moi ».

### C3 — `FOUR CORRECTIONS, ALL WORSE` — 20 s — *incruste à puces, plus de schéma*

Fond noir, la liste tombe ligne à ligne, chaque ligne se **barre** 1,2 s après.

> `FOUR GEOMETRIC CORRECTIONS WERE TRIED`
> ~~*shift the quad*~~
> ~~*shift the clip window*~~
> ~~*sweep the projection gain*~~
> ~~*re-project the corners*~~
>
> *all four made it worse — in both directions.*
> *getting symmetrically worse is the signature of a **timing** error,*
> *not a position error.*

**Ce qu'elle explique** : un trou à une jonction ressemble à une erreur de géométrie, donc la
géométrie a été corrigée — quatre fois, et symétriquement en pire.
**Pourquoi elle est là** : c'est le retournement du récit, et ça explique deux mois sans
accuser personne. D5 est morte : quatre libellés barrés font le travail mieux qu'un dessin.

### C4 — `THE PATCH, AND ITS PRICE` — 22 s

Sur `J3` (extérieur, mouvement) sous-exposé à 35 %, texte par-dessus.

> `LEAD-FILL`
> *the CPU repainted the missing band itself — on both SH-2, every frame.*
> *it worked.*
> *it cost 1.5 to 5 frames per second.* `[HW]`
>
> *a patch that holds takes the pressure off the cause.*
> *that one took it off for two weeks.*

**Ce qu'elle explique** : le correctif de fortune, son prix, et le piège d'un pansement qui
tient.
**Pourquoi elle est là** : c'est la seule idée du bloc sans forme visuelle — et elle installe
la dette que le bloc 5 vient solder.

---

## 7. Bloc 4 — comparatif (4:22 → 5:14) — **reciblé**

Le ×8 de la v4 ne montrait qu'**une** frame fautive, en toute fin d'extrait. Mesuré : sur
`11-22-01`, **3,00 → 4,00 s** contient un panoramique intérieur avec un bloc de mur détaché
sur ~10 frames consécutives (3,000 → 3,133), puis un second épisode (3,700 → 3,933). C'est
**là** qu'il faut couper.

**a) Arrêt sur image, 10 s.** `J0` défile à ×0,5 puis **s'arrête net** sur la frame retenue.
Le cadre rouille de C1 revient. Une ligne : *one field of lateness — held still.* Puis reprise.

**b) Face à face à ×4, 20 s.** Juillet à gauche, août à droite, même nature de mouvement
(rotation intérieure devant un raccord mur/plafond), 5 s de source chacun.
Tampons de date sous chaque moitié. Bandeau haut :
`20 JUL — DESYNC` / `20 AUG — MANUAL PRESENT`
Bandeau bas, une fois, 6 s :
*two capture chains — 1080p OBS left, 480p capture card right. no fps to read here.*

**c) PIP, 22 s.** Août plein cadre (`D1a`+`D1b`), juillet en incrustation **coin haut-droit**,
30 % de largeur, bordure rouille 3 px, tampon de date sous l'incrustation.
Ce que ça montre : le décrochage n'était pas un incident, il était **partout** en juillet — et
il n'est nulle part en août.

⚠ **Rappel `interbuild-perf-noise`** : aucune photo build-contre-build sur les fps. Le 20/07
sert **uniquement** à montrer l'artefact.

---

## 8. Bloc 5 — cartes #2 : la solution (5:14 → 7:18) — **Track 2 entre ici**

### C5 — `FAFLING NAMED THE CAUSE` — 16 s
> SegaXtreme, 3 August:
> *"if you have VDP1 switching its framebuffers after every vblank,*
> *it is not a good idea in a variable framerate game like Doom"*

**Ce qu'elle explique** : la cause a été nommée **de l'extérieur, publiquement, par quelqu'un
d'autre**. **Pourquoi elle est là** : l'attribution est due, et c'est le pivot du récit.

### C6 — `THE FIELD IS THE BUDGET` — **D4 v2** — 34 s

**Ce qui manquait en v4 : la notion de cycle, et ce qui ferme la fenêtre.** La réponse est
dans le manuel, en toutes lettres.

**Composition, de haut en bas :**

1. **Une règle de fields.** Huit cases égales, chacune `1/60 s`, séparées par un tick vertical
   étiqueté `V-BLANK`. C'est la seule horloge du dessin.
2. **La définition, citée** (petit, en haut à droite, `ST-013, Introduction`) :
   > *"A field is the time it takes a scanning line to scan one screen. A frame is the time it
   > takes from one change of the frame buffer to the next change."*

   → **c'est la bascule qui définit la frame.** La fenêtre n'est pas fermée par une horloge :
   elle est fermée **par la bascule elle-même**.
3. **Rangée A — `1-CYCLE (FBCR = 0x0000)`.** Une flèche de bascule **sur chaque tick**, en
   rouille. La barre de dessin du VDP1 fait **exactement une case**. Le reste est hachuré.
   Étiquette : *the hardware closes it — every field, finished or not.*
   Citation sous la rangée :
   > *"When the change mode of the frame buffer is in a one cycle mode, one frame is equal to
   > one field."*
4. **Le prix du dépassement**, en rouille sous la hachure :
   *the list restarts at 00000H every frame — an undrawn tail is **abandoned, not resumed**.*
   *overrun does not cost time. it costs content.*
5. **Rangée B — `MANUAL CHANGE (FCM = 1, FCT = 1)`.** **Aucune** flèche sur les ticks. Une
   seule flèche, au bout, marquée `CPU`. La barre verte court sur 4 à 8 cases, avec une
   accolade : `one Mimas frame — 4 to 8 fields, and it varies`.
   Étiquette : *the CPU closes it — one write, when the drawing is actually done.*
6. **Ligne de bas de carte**, la prescription du manuel corrigé :
   > *"The number of characters that can be drawn in one **field** is limited. Therefore, in
   > order to draw more characters, the manual mode must be set."* `[ST-013 p.38, corrigé]`

**Ce qu'elle explique** : pourquoi 6 cases et pas 6 — **6 n'est pas une constante matérielle,
c'est la durée de frame de ce jeu-là** à ~10 fps ; à 16 fps c'est 4 cases, à 7 fps c'est 8. Et
ce qui ferme la fenêtre : la bascule, par définition ; en 1-cycle c'est le matériel qui la
déclenche à chaque field, en manuel c'est le CPU.
**Pourquoi elle est là** : c'est LA carte de l'épisode. Une petite barre contre une longue, et
une définition citée qui rend la comparaison indiscutable.

### C7 — `THE COMPLETION FLAG LIED` — 24 s
> *finished-or-not was read from **CEF** — the flag VDP1 raises once it has fetched the end*
> *of its command list. EDSR bit 1, at 100010H.*
>
> *the manual carries its own warning:*
> *"This bit is reset to 0 when the frame buffers are changed or when drawing is started"*
> *"If fetch of the draw terminate command matches when the frame buffer changes,*
> *CEF and BEF might not become 1."*
>
> *on silicon it latches on 30 to 60 % of frames.* `[HW]`
> *the gate that is never ambiguous is the command-address register — COPR.*

**Ce qu'elle explique** : la **deuxième** erreur — lire l'achèvement du dessin sur le mauvais
bit, un bit que la bascule remet elle-même à zéro.
**Pourquoi elle est là** : c'est exactement ce que le public de SegaXtreme vient vérifier, et
chaque ligne est citable.

### C8 — `TWO WORDS IN THE MANUAL` — 22 s

**Nouveau : il y en a deux, pas une.** Les deux graphies face à face, la fausse en rouille et
barrée, la corrigée en vert.

> `ST-013 p.38 — the diagnosis`
> ~~*"The number of characters that can be drawn in one **frame** is limited."*~~ (developer CD)
> *"The number of characters that can be drawn in one **field** is limited."* (corrected)
> → *with "frame", the sentence is a tautology. with "field", it is an instruction.*
>
> `ST-013 p.39 — the prescription`
> ~~*"writing 0 to the VBE, FCM and FCT registers"*~~ (developer CD)
> *"writing 0 to the VBE and FCT registers and **1** to the FCM register"* (corrected)
> → *the first spelling is 0x0000 — which the facing table calls **1-cycle mode**.*
>
> *for two months the driver did exactly what the manual said. and did nothing.*

**Ce qu'elle explique** : le titre de l'épisode. Un mot pour le diagnostic, une clause pour
l'ordonnance — et les deux étaient faux sur le même CD développeur.
**Pourquoi elle est là** : c'est la seule fois où un bug de deux mois se réduit à un mot.

### C9 — `THE SEQUENCE WITH NO AMBIGUITY` — 18 s
> `VBE ERASE AND CHANGE`
> *arm the erase on a fresh V-blank IN — TVMR bit 3, FBCR = 0x0003.*
> *the swap lands at the end of that same blank.*
> *one write, one edge, nothing left to infer.*
>
> *SEGA's own library ships it — SBL, SCL_VBLV.C, interval 0xfffe.*
> *and Lobotomy Software's SlaveDriver engine — PowerSlave, Duke Nukem 3D,*
> *Quake on Saturn — ran manual frame change in game, same interval.*
>
> *this was not exotic. it is what 1996 shipped.*

**Ce qu'elle explique** : la solution, et surtout qu'elle n'a rien d'inventé.
⚠ SlaveDriver = **Lobotomy Software** (Ezra Dreisbach). La part SEGA, c'est `SCL_VBLV.C` dans
la **SBL**. Deux choses distinctes — l'erreur a été faite une fois, pas deux.

### C10 — `WHAT IT OPENS` — 22 s — **réécrite pour la fenêtre étendue au 21/08**

> *"That should solve the issues you have when trying to mix software rendering and*
> *VDP1 rendering for sprites, because* ***VDP1 will be able to draw a lot more than***
> ***just what fits in a screen frame."*** — fafling
>
> **19 AUG** — *a monster had to cover half a percent of the screen to reach VDP1.*
> *now two tenths — about ten pixels tall. and 32 sprites a frame instead of 16.*
> **20 AUG** — *sprites reach VDP1 in split-screen too. when the queue runs short,*
> ***the walls yield to the things*** *— a cut thing is drawn by nobody, a cut wall*
> *degrades to a flat quad. that asymmetry is the fix.*
> **20 AUG** — *the hardware sky came back in split-screen.*
> **21 AUG** — *24 KB of stack handed back to the heap.*
>
> *the limit moved. it did not go away.*

**Ce qu'elle explique** : ce que la correction débloque, daté commit par commit, et la limite
dite honnêtement.
**Pourquoi elle est là** : elle transforme un correctif technique en **capacité**, et elle
enchaîne directement sur le split-screen du bloc 6 — qui est l'illustration littérale de la
ligne « 20 AUG ».

---

## 9. Bloc 6 — payoff split + TNT clean + crédits (7:18 → 9:18)

### 6a — le split, 34 s — *l'illustration de C10*
`SP3` (0 → 20 s, dont la cour extérieure avec le ciel) puis `SP1` @8.6 (14 s, le sprite du
second joueur bien visible). Tampon de date **21 AUG** puis **20 AUG**.

| TC | tenue | incruste |
|---|---|---|
| +2 s | 11 s | `TWO PLAYERS, ONE SATURN`<br>• *two viewpoints · one framebuffer · one disc*<br>• *no overlay on this capture* |
| +18 s | 10 s | `THE OTHER PLAYER IS A VDP1 SPRITE`<br>*that was not possible in split before the present was fixed* |

### 6b — TNT clean, 66 s
`T1a` (0 → 40 s) puis `T1b` @20 (26 s). Tampon **20 AUG**.

| TC | tenue | incruste |
|---|---|---|
| +3 s | 11 s | `TNT: EVILUTION`<br>*a full commercial WAD, streamed from the disc — no RAM cart* |
| +20 s | 22 s | **panneau latéral D2** (§10) |
| +46 s | 22 s | **panneau latéral D3 v2** (§10) |

Les deux diagrammes passent en **panneau latéral** — 45 % de largeur à gauche, le jeu continue
à droite, assombri à 55 %. Plus aucun écran noir dans ce bloc : c'était le reproche principal
des montages précédents.

### 6c — crédits, 20 s
slygamer — *most of this month's hardware testing* · wesker — *hardware testing on TNT MAP11* ·
fafling — *the VDP1 sync fix, and deep knowledge of the retail Saturn Doom* ·
TrekkiesUnite118 — *properly corrected several wrong claims on SegaXtreme* · the SegaXtreme
forum · the Kronos team (Runik, fafling, Benjamin Siskoo) — *the errata-corrected VDP1 and VDP2
manuals* · ReyeMe — *Saturn Ring Library* · doomgeneric · Chocolate Doom.

Pied : *Mimas is a fan project. Doom is id Software's. Not affiliated with SEGA or id.*

---

## 10. Les deux diagrammes conservés

### D2 — `WHERE THE FRAME WENT` — **inchangé, validé**
Trois barres à la même échelle : `16.7 ms` vert (un field) · `277 ms` avec une tranche rouille
de `46.6 ms` étiquetée *texture composites, decoded per column* · `61–142 ms` vert.
Sous-titre : *fixed offline, in the WAD. zero engine lines.* `[HW, 7 captures]`

### D3 v2 — `IT FIT, AND IT STILL FAILED` — **refaite**

Le reproche était juste : la v1 montrait un **état**, pas un **mécanisme**. La v2 montre une
pièce qu'on essaie d'insérer.

**Trois temps, 22 s :**

1. **La zone**, une barre unique = `1,040,384 bytes — the whole Doom zone`. Deux blocs
   `PU_STATIC` garés dedans (**72 KB à 64 KB**, **60 KB à 655 KB**) la coupent en trois runs.
   Chaque run est chiffré. Total affiché : `free: 240 KB · longest run: 48 KB`.
2. **La pièce.** Le plus gros bloc unique que la carte demande — `LINEDEFS, ~110 KB` — dessiné
   **à l'échelle**, en rouille. Elle glisse le long de la barre et se fait **refuser à chaque
   run** (le run clignote rouge, la pièce continue). Sous-titre :
   *240 KB free. and not one piece of it long enough.*
   Puis, en gros : `it fit, and it still failed.`
3. **Le régime.** `line_t: 64 bytes → 24` s'inscrit, la pièce **rétrécit à vue** et **tombe**
   dans le run de 48 KB, qui passe au vert.
   Sous-titre : *seg_t and node_t were shrunk too. they freed real RAM and zero maps.*
   *on every blocked map, the dominant term was LINEDEFS.*
   Puis : `38 of 415 test maps refused to load. now none of them do — except the Nuts family.`

**Ce qu'elle explique** : ce n'est pas la **capacité**, c'est la **contiguïté**. Et la leçon
est la même que celle du present : identifier le terme dominant avant de rétrécir quoi que ce
soit.

---

## 11. 🟡 Ce dont j'ai besoin de toi

### A. La frame de la décomposition (C1)

Six candidates exportées en pleine résolution (`1716×1008`, crop du pillarbox) dans
`C:\Users\pcico\Videos\mimas-devlog1\frames\` :

| fichier | ce qu'on y voit |
|---|---|
| `CANDIDATE_t3.000.png` | gros pan brun à droite, **couture verticale franche**, encoche noire en haut à droite |
| **`CANDIDATE_t3.067.png`** | ⭐ **recommandée** — le pan brun est un rectangle net, la salle est saine des deux côtés, rien ne distrait |
| `CANDIDATE_t3.100.png` | le pan a glissé, **coin noir en haut à droite** = trou franc |
| `CANDIDATE_t3.733.png` | dalle grise vive collée au bord **gauche** |
| `CANDIDATE_t3.767.png` | dalle grise gauche + biseau clair en bas à droite |
| `CANDIDATE_t3.900.png` | ⭐ **secours** — **découpe blanche en dents de scie en haut**, très graphique |

**Ce que je te demande** : reprends la retenue et peins les zones **par-dessus**, en aplats
durs, et renvoie-moi le PNG à côté de l'original (même taille, même cadrage).

| zone | couleur exacte |
|---|---|
| murs VDP1 **corrects** | rouge pur `255,0,0` |
| **le morceau fautif** (le pan en retard) | magenta pur `255,0,255` |
| framebuffer CPU (sols, plafonds, things, arme, HUD) | vert pur `0,255,0` |
| VDP2 (ciel, plan de rotation) | bleu pur `0,0,255` |

⚠ **aplats durs, aucun anti-aliasing, aucun flou, aucune transparence** — je découpe l'image
originale par clé de couleur exacte ; un bord adouci fait une frange sur les trois plans.
Le magenta est important : c'est lui qui devient l'objet du cadre rouille et du glissement
latéral.

### B. La voix — **3ᵉ passe, prises 56 → 78, script réécrit**

Le 2ᵉ lot (prises 18–55) a été **jugé trop monotone par le propriétaire et refait**. Le script
a été réécrit en même temps, et les changements ne sont pas cosmétiques :

| changement | pourquoi c'est mieux |
|---|---|
| **deux phrases d'accroche ajoutées** en tête — *« Can it run it? Let's see that together. »* / *« Welcome to the first devlog episode. »* | l'épisode ouvre sur une **question**, pas sur une déclaration. La 3ᵉ nomme l'épisode pendant que la carte titre est encore à l'écran |
| *« That is not what this episode is about »* → *« But this episode is about something else: **chip synchronisation** »* | le sujet est **nommé**. L'ancienne version retenait l'information, ce qui se lit comme de la coquetterie |
| *« This is Doom »* → *« This is a **DOS Doom port** »* | exact, et ça pose l'enjeu du portage dès la première seconde |
| *« the WAD is read off the disc »* → *« the **WAD content** is read off the disc »* | plus juste : c'est le contenu qui est lu, pas le conteneur |
| *« Two capture chains… no frame rates to read here »* → *« These are two hardware captures, one month apart. »* | ⚠ **la mise en garde a quitté la voix** — elle est donc passée **à l'écran** sur le plan ×4 (`p25`, +12 s) : *there are no frame rates to read here.* Sans ça, on aurait publié un face-à-face daté sans dire qu'on n'en tire aucun fps |
| la ligne des comptes (12,5 produites / 18,3 distinctes) **retirée de la voix** | un chiffre se **lit**, il ne s'écoute pas. Il reste sur la carte `C3_counts`, où on peut le relire |

⚠ **Une seule réserve de fond** : *« turning around? it moves it by the rotation angle »* —
un déplacement ne se mesure pas en angle. La carte à l'écran dit *« turning, it is the width
of a wall »*, qui est concret et juste. Les deux cohabitent : la voix est la version parlée, la
carte est la version vérifiable. Si tu veux aligner, c'est la voix qu'il faut refaire, pas la
carte.

#### Le mappage — vérifié par la mesure, pas supposé

**23 fichiers pour 24 phrases.** La prise **56 en contient deux** : le propriétaire a lu
l'ouverture et l'accroche à la suite.

| hypothèse | durée/mot de la 56 | conséquence sur le reste |
|---|---|---|
| 56 = phrases **1 + 2** | **0,360** | tout le lot tient dans **0,275 – 0,421** |
| 56 = phrase 1 seule | **0,566** | décale tout, et produit **0,723** et **0,797** — physiquement impossible |

Coupe au **seul silence de 1,16 s** du fichier — `5,290 → 6,446` s dans le **brut** (le silence
de tête fait 1,59 s ; l'appliquer aux timecodes du fichier détouré avait donné une coupe fausse
du premier coup). Après coupe : `n01` 0,313 s/mot, `n02` 0,398 — les deux dans la bande.

🟡 **À confirmer à l'oreille** : les deux premières phrases de `vo/VO-check3.m4a`. C'est le seul
endroit de l'épisode où une décision de montage repose sur une inférence plutôt que sur une
écoute, et elle tombe sur les dix premières secondes de la vidéo. Si la coupe est mauvaise, le
correctif est **un seul nombre** dans `tools/devlog/ep1_vo_lines.tsv`.

#### Placement

| prises | vont en |
|---|---|
| `n01 n02 n03` | **bloc 1**, sur la carte titre (qui tient maintenant jusqu'à 14,5 s pour ça) |
| `n04 n05 n06 n07` | **bloc 2**, à +22 / +58 / +97 / +115 |
| `n08` ‖ `n09 n10` | **C3** — le plan, puis la carte |
| `n11 n12` | **C4** — les trois frames |
| `n13 n14 n15` | **C5** — le découpage |
| `n16` | **bloc 4** — le face-à-face |
| `n17 n18 n19` ‖ `n20 n21 n22` ‖ `n23` ‖ `n24` | **C9** ‖ **C10** ‖ **C11** ‖ **C12** |

**Les 19 prises du 1ᵉʳ lot sont conservées telles quelles** (`vo/l01…l19.wav`) : C1, C2, le
reste de C5, C6, C7, D4 et la fin de C12. Total parlé **201 s** sur 10:35.

### C. Deux confirmations
1. **`SP2` = `Doom - Base - notext1` @0:35** : le fichier fait 53,6 s, il ne reste donc que
   **18,6 s** après 0:35. Confirme que c'est bien la portion sans overlay visée, sinon donne un
   autre point d'entrée.
2. **Datation des clips split** : `SP1` est arrivé le 20/08 09:37 (build `fd95f4c`), `SP2`/`SP3`
   le 21/08 07:26 — donc **après** `f217751` (20/08 16:01), qui est le commit « les murs cèdent
   aux things ». Les tampons de date suivent ça. Si tu sais qu'un de ces clips tourne sur un
   autre build, dis-le : c'est le seul point de l'épisode que je ne peux pas vérifier depuis
   l'image.

---

## 12. Ce qui reste à produire après ta réponse

- [ ] `ep1_diagrams.py` : supprimer D1/D5, réécrire **D4 v2** et **D3 v2**, garder D2
- [ ] `ep1_decomp.py` : nouveau — la décomposition C1 à partir de tes deux PNG
- [ ] `ep1_vo.sh` : passe 2 sur les 21 nouvelles prises
- [ ] `ep1_build.py` : réécrit sur 6 blocs, tampons de date, panneaux latéraux
- [ ] miniature + description YouTube + post SegaXtreme (jamais refaits depuis la v4)

---
---

## 12-bis. Montage — état au 2026-08-24, deuxième session

### La voix est arrivée — mappage vérifié

23 prises le 24/08 dans `Documents\Enregistrements audio`, pour 23 phrases (la 10 a été
sautée volontairement : son contenu survit sur la carte `C3_counts`). Fichiers **18, 27, 28,
35 → 55**, moins **39** (0,19 s — clic raté).

⚠ **Tu en avais listé 23, il y en a 24** : `(28)` n'était pas dans ta liste mais est bien une
prise, et `(39)` est vide. Le mappage a été **vérifié par la durée**, pas supposé : 0,284 à
0,458 s par mot sur les 23, moyenne 0,36, sans le moindre décrochage. Avec `(28)` retiré, la
phrase 4 tomberait à 0,198 s/mot (5 mots/s, impossible) et la 5 à 0,594 — l'hypothèse est
écartée par la mesure, pas par le goût.

| | |
|---|---|
| liste des prises + texte | `tools/devlog/ep1_vo_lines.tsv` (stem, prise, bloc, texte) |
| traitement | `tools/devlog/ep1_vo.sh` — deux lots, même chaîne, `-18 LUFS` |
| stems | `vo/n01…n24.wav` (**pas de `n10`**, le trou est conservé exprès) |
| bande de contrôle | `vo/VO-check2.m4a`, 2:08, 0,6 s entre chaque |
| total parlé | lot 1 87,3 s + lot 2 114,3 s = **201,6 s** sur 10:43 |

### L'assembleur

`tools/devlog/ep1_build.py` est **réécrit** pour les six blocs. Un seul script fabrique
maintenant tout l'épisode ; l'ancienne colonne `BLOCK3-spine.mp4` est abandonnée au profit
d'une construction reproductible du bloc 3 pièce par pièce.

```powershell
python tools/devlog/ep1_build.py          # tout
python tools/devlog/ep1_build.py pieces   # seulement recouper les pièces
python tools/devlog/ep1_build.py post     # voix + ASS + mix + master
```

| bloc | départ | durée |
|---|---|---|
| 1 ouverture | 0:00 | 24,0 s |
| 2 gameplay clean | 0:24 | 125,8 s |
| 3 le problème | 2:29 | 163,6 s |
| 4 comparatif | 5:13 | 52,2 s |
| 5 la solution | 6:05 | 141,0 s |
| 6 payoff + crédits | 8:26 | ~130 s |
| **total** | | **≈ 10:45** |

Voix et incrustes sont ancrées **à une pièce ou à un bloc**, jamais à un timecode absolu :
une pièce qui change de durée ne décale plus tout ce qui suit.

### 🔴 Trois pièges trouvés au contrôle, tous corrigés

1. **`185408` (T1a) est inutilisable avant ~20 s** — menu de skill sous l'overlay **complet**,
   exactement le même piège que `184951`. Et tout ce qui suit 20 s est une mêlée **berserk**,
   donc noyée de rouge : bonne image, mauvais fond pour un diagramme. Les deux panneaux ont
   été déplacés sur `185508`, dont les couloirs sont sombres et réguliers.
2. **`185147` (D1c) ouvre sur l'écran d'intermission** « NUCLEAR PLANT » et n'atteint le
   gameplay E1M2 qu'à ~6,5 s. Le PIP du bloc 4 tombait dessus (une carte fixe comme moitié
   d'un comparatif de mouvement) → PIP reciblé à `@12`. L'incruste `E1M2` du bloc 2 est
   vérifiée à ±1 s.
3. **`l09` débordait de 1,5 s sur `n06`** — la dernière carte C2 est passée de 4 à 8 s, ce
   qu'elle méritait de toute façon : c'est celle qui porte la chute.

### 🟠 Deux écarts assumés par rapport au plan écrit

1. **Bloc 1** prend `185046` **@0–24** et le bloc 2 reprend ce clip **à 24 s**, au lieu de
   `@12–36`. La fenêtre du plan aurait montré les mêmes 12 s **deux fois** à une minute
   d'intervalle. Là, **aucun plan ne se répète** dans l'épisode.
2. **Les diagrammes du bloc 6 ne sont pas un panneau latéral 45 %.** D2 et D3 sont **paysage
   par construction** — la barre de zone de D3 fait 1660 px et porte huit runs chiffrés — et
   les comprimer en colonne portrait les rend illisibles. Ils sont **recadrés sur leur propre
   contenu** et posés sur le jeu **qui continue**, assombri à 55 %. Le reproche auquel le
   panneau répondait (« plus aucun écran noir dans ce bloc ») est satisfait : le jeu ne
   s'arrête jamais. À dire si tu préfères quand même la colonne — c'est faisable, mais il faut
   redessiner les deux diagrammes en portrait.

### 🔨 Reste

| # | pièce | état |
|---|---|---|
| 9 | bloc 4 — arrêt, ×4 face à face, PIP | ✅ frame de gel **mesurée** (`ep1_b4_freeze.py`) |
| 10 | tampons de date + tiers-inférieurs (ASS) | ✅ 29 tampons, 26 incrustes, `ep1.ass` |
| 11 | assembleur 6 blocs + mix + loudness | ✅ master `MIMAS-devlog1-ep1.mp4` |
| 12 | miniature + description + post | ✅ `thumb-ep1.png` · §13 |
| 13 | passe de loudness finale (-14 LUFS) | ⬜ après validation image |
| 14 | chapitres YouTube depuis le rapport de blocs | ⬜ |

### La frame de gel du bloc 4 — mesurée, pas choisie

`tools/devlog/ep1_b4_freeze.py` reprend le test à trois frames d'`isolate_vdp1.py` sur **tes
trois positifs seulement** et sort la plus grosse composante connexe :

| frame | plus grosse composante | verdict |
|---|---|---|
| `t3.000` | 219 px | une arête de mur — trop fine |
| **`t3.767`** | **460 px**, `x74..102 y12..49` | **le trou au plafond, en haut à gauche** ✅ |
| `t3.900` | 198 px | l'arme qui bouge — faux positif |

Boîte rouille retenue : `x=380 y=40 w=187 h=198` dans le crop 1716×1008, soit
`x=425 y=43 w=209 h=212` en 1080p.

### Le comparatif ×4 — le même endroit des deux côtés

Un détecteur de panoramique a cherché la seule rotation intérieure **soutenue** de chaque
capture : juillet `11-22-01` **@11,1 s**, août `184951` **@37,6 s**. Les deux tombent sur
**la même cour extérieure d'E1M1**, fenêtre et montagnes comprises — ce n'est pas un choix
esthétique, c'est ce que la mesure a rendu.

---
---

## 12-ter. Passe du 2026-08-24 après-midi — la fin de l'épisode

### 🔴 Le défaut trouvé par le propriétaire : « pas de musique sur la fin »

Ce n'était pas « pas de musique ». **Les 131 dernières secondes du master n'avaient aucun
son** — ni musique, ni jeu, ni rien.

**Cause** : `sidechaincompress` se termine quand **l'une** de ses deux entrées se termine, et
la clé de ducking était la voix. La dernière phrase tombe à **8:24** ; musique et jeu étaient
coupés net à cet instant. `amix=duration=longest` ne pouvait rien rattraper : ses trois
entrées faisaient alors toutes la même longueur.

**Correctif** : `apad=whole_dur` sur la **clé** avant l'`asplit`. Une ligne, et elle porte son
commentaire dans `ep1_build.py` parce que le symptôme est silencieux au sens propre.

⚠️ **Vérifier le niveau *jusqu'au bout* fait maintenant partie du contrôle** : la mesure de
loudness intégrée ne l'avait pas vu — un master muet sur 20 % de sa durée passe l'`ebur128`
sans broncher, il tire juste l'intégré vers le bas.

### 🔴 « width of a wall » était simplement faux

Le propriétaire a tranché : un frame de retard déplace l'image de **ce que la vue a tourné
pendant cette frame**. C'est un **taux angulaire**, pas une distance — rien là-dedans n'est
ancré à un mur.

Et ce n'est pas de la pédanterie : **c'est précisément ce qui déguisait le bug**. Comme le
déplacement suit la vitesse de rotation, chaque tentative de le mesurer comme un décalage fixe
contredisait la précédente — ce qui a maintenu quatre corrections géométriques plausibles
pendant deux mois. La carte `C3_counts` dit maintenant les deux choses.

### 🟠 La nuance C10 — voir §8

Titre **conservé** (`ONE WORD IN THE MANUAL`), carte **nuancée**. Le détail chiffré et les
cinq poisons sont dans la fiche C10 en §8, la même nuance est portée par §13.

### 🟢 Bloc 5 sur du jeu — adopté

`B5_OVER_GAME = True`. Les onze cartes du bloc 5 passent à **90 %** sur `SP2`
(`Doom - Base - notext1`, la seule capture qui ne servait nulle part), assombri à -0,30.
2:21 d'écran noir en moins. Le lit ne porte pas de tampon de date : à cette opacité c'est une
texture, pas une pièce du dossier.

### 🟢 Bloc 6 réécrit — c'était le vrai reproche

L'ancien bloc 6 montrait TNT sans dire **pourquoi TNT**, et fermait sur une incruste
multijoueur d'une ligne. Réécrit en sept incrustes, toutes chiffrées avec des nombres que
l'épisode a **déjà montrés** :

| pièce | incruste |
|---|---|
| `p38` | `TWO PLAYERS, ONE SATURN` — deux vues · un framebuffer · un disque |
| `p39` | `WHAT THE FIX BOUGHT HERE` — la fenêtre de dessin était **un FIELD**, elle est maintenant **une FRAME de jeu**, soit **4 à 8×** (le chiffre vient de D4) |
| `p39` | `SO THE MONSTERS CAME BACK` — les sprites atteignent VDP1 en split ; quand la file manque, **les murs cèdent en premier** |
| `p40` | `TNT: EVILUTION HAS NEVER RUN ON A SEGA SATURN` — Final Doom a été publié sur PC et PlayStation en 1996, **pas sur cette machine** ; 32 cartes, depuis le disque, sur une console 2 Mo d'origine |
| `p42` | `415 TEST MAPS WENT THROUGH THE LOADER` — **38 ont refusé. pas lentement — pas du tout.** |
| `p48` | `NOW NONE OF THEM DO` — sauf la famille Nuts, qu'aucune machine ne charge ; plus long run contigu **48 Ko → 130 Ko** |
| `p48` | `AND THE CEILING HAS NOT BEEN FOUND` — le second SH-2 mesure **0 % occupé en split** `[HW]` ; la fenêtre de dessin n'a jamais été poussée à sa limite |

⚠ **La revendication du « premier »** est posée sur un fait **vérifiable** — Final Doom n'est
jamais sorti sur Saturn — plutôt que sur un absolu invérifiable (« personne ne l'a jamais fait
tourner »). C'est la formulation qui tient devant un public qui vérifie.

Bloc 6 : 129 → **144,4 s**. Épisode : **10:51**.

### 🟢 Un garde-fou de plus

`build_voice()` échoue bruyamment si une prise enregistrée n'atteint aucune pièce, ou si une
prise est placée deux fois. Écrit après avoir perdu les trois lignes de C6 dans une réécriture
de table, avec pour seul symptôme un compte de lignes dans un journal.

---

## 12-quater. Contrôle image dense — trois défauts que l'échantillonnage avait manqués

Chaque passe précédente échantillonnait 15 à 18 points, et **chaque passe trouvait un défaut que
la précédente avait raté**. Contrôle à 36 points sur le master final :

1. **La carte C6 rendait mal depuis le tout premier montage.** `_t(d, L, y - 44, "[Ymir]")`
   dessinait l'étiquette **à l'origine de colonne, sur la même ligne** que *« it cost between one
   and five frames per second »* — les deux glyphes se chevauchaient, 18 s à l'écran, dans toutes
   les versions livrées. `[Ymir]` est maintenant posé **après** la ligne qu'il qualifie.
2. **Deux écrans de score bout à bout au milieu du bloc 2.** `185046` **finit** sur le tableau de
   fin d'E1M1 et `185147` **commence** sur celui d'E1M2 : ~14 s de carte fixe au milieu du bloc
   dont le seul travail est de montrer la machine tourner. `D1C` démarre à **6,5 s**.
3. **La moitié du défaut 2 était de l'autre côté de la coupe.** La **queue** de `185046` restait
   intacte : le tableau y est **entièrement compté à 83,0 s** et figé jusqu'à 89,3 — **6,3 s
   d'image morte**, et `n05` (*« eight to fifteen frames per second »*) tombait dessus. `D1B`
   passe de 35,07 à **29,70 s** : le battement `FINISHED` et le décompte sont gardés, le gel non.

   ⚠️ **La règle qui manquait** : quand un raccord est mauvais, il a **deux côtés**. Regarder la
   première frame du plan suivant sans regarder la dernière du précédent ne corrige que la moitié
   — et la moitié restante ressemble à une correction qui a marché.

⚠️ **Leçon de méthode** : un échantillon de 15 points sur 10 minutes touche 2,5 % de la durée. Ça
trouve les pannes franches, pas les collisions de rendu. Le contrôle dense se fait **une fois,
sur le master final**, pas à chaque itération.

---

## 12-quinquies. Passe de relecture du propriétaire — 2026-08-24 fin de journée

Cinq points relevés à l'écran, tous corrigés dans `ep1_build.py` / `ep1_cards.py`.

| # | horodatage | ce qui n'allait pas | correctif |
|---|---|---|---|
| 1 | 1:04 | *« the world sprites »* listés **sous le CPU** | `dg_saturn.cxx:4637` : `SAT_WORLD_THINGS_VDP1` vaut **1 par défaut** — les sprites du monde sont des quads VDP1 priorité 7, comme l'arme et la barre d'état. Ils passent dans `WHAT DRAWS WHAT`. L'épisode se contredisait lui-même à six minutes d'écart, puisque C5 les découpe comme couche VDP1 |
| 2 | 5:06 | la boîte d'identification ne désignait **aucun** trou | voir 12-sexies |
| 3 | 4:34 | *« FOUR LAYERS, TWO CHIPS »* | la couche cyan est un **bitmap CPU**, et le CPU ici c'est **deux SH-2**. Devenu `FOUR LAYERS, FOUR DEVICES` + *VDP1 · VDP2 · two SH-2 at 28 MHz*. Nommer les quatre vaut mieux qu'un chiffre : deux des quatre couches sortent du **même** appareil |
| 4 | 8:27 | *« no overlay on this capture »* | disait ce que le spectateur voit déjà. Remplacé par deux faits vérifiables : **up to four on a multitap — no link cable, no netcode** (`MULTIPLAYER_PLAN.md` §1) et **every view is rendered in turn, inside one frame** (`dg_saturn.cxx:8318`) |
| 5 | 10:24 | crédit trop tiède | *« properly corrected several **of my** wrong claims »*. ⚠ **Seul mot à la première personne de tout le texte publié** — délibéré : un crédit pour correction ne vaut rien si personne n'est nommé comme celui qui avait tort |

**Ajouté à la demande du propriétaire** : `Claude, by Anthropic — paired on the port`, sur la carte, dans la description YouTube et dans une
section `-- THANKS --` créée pour le post SegaXtreme.

---

## 12-sexies. La frame de gel du bloc 4 — mesurée, puis **corrigée par le propriétaire**

`tools/devlog/ep1_b4_freeze.py` a repris le test à trois frames sur les trois positifs du
propriétaire et sorti la plus grosse composante connexe :

| frame | plus grosse composante | verdict |
|---|---|---|
| `t3.000` | 219 px | une arête de mur — trop fine |
| **`t3.767`** | **460 px**, `x74..102 y12..49` | frame retenue |
| `t3.900` | 198 px | l'arme qui bouge — faux positif |

🔴 **Le choix de la FRAME était bon, celui de la RÉGION était faux.** La composante de 460 px
n'est **pas** un trou de synchronisation. Le propriétaire a peint les vrais dans
`frames/all-2201/t03.767_green.png`, et marqué dans `_yellow.png` un **tout autre bug** — hauteur
de fenêtre sur plan incliné, corrigé depuis — vers lequel la mesure dérivait.

⚠️ **Ce que la mesure pouvait et ne pouvait pas faire.** Un diff de couche tardive trouve tout ce
qui a **bougé** entre deux frames. Il ne sait pas lequel de ces mouvements est *l'artéfact dont
parle l'épisode* — seule la personne qui connaît le renderer le sait. Même famille que
[[ask-dont-guess-the-symptom]] : le symptôme est **visible**, il fallait demander.

Trois régions récupérées du masque peint par composantes connexes, marge 8 px, du crop
1716×1008 vers le 1920×1080 de la pièce :

| région | crop 1716×1008 | 1080p | apparition |
|---|---|---|---|
| bord gauche — une colonne de mur entière | `0,44,163,730` | `0,47,182,782` | +1,00 s |
| droite du centre — le plus grand trou intérieur | `883,241,170,186` | `988,258,190,199` | +1,35 s |
| le petit à côté | `644,248,85,112` | `721,266,95,120` | +1,70 s |

Elles arrivent **une par une**, la plus grande d'abord : l'œil traverse l'image au lieu de
recevoir trois rectangles d'un coup. L'incruste dit désormais *three holes in one frame*.

---

## 12-septies. 🔴 Le fichier que tu lis a été détruit puis reconstruit

Un script de patch a fait `open(path, "w")` — qui **tronque à l'ouverture** — puis son `write` a
levé `UnicodeEncodeError: surrogates not allowed` (un emoji écrit en paire de substitution UTF-16
au lieu de `\U0001F534`). Le fichier n'était **pas suivi par git**, donc pas de restauration.

**Règle, sans exception** : un script de patch écrit dans `path + ".tmp"` puis `os.replace()`. Un
patch qui échoue doit laisser le fichier **inchangé**, jamais vide. Et un fichier untracked et
long se copie **avant** la passe. Voir [[atomic-file-rewrite]].

Rien du montage n'a été perdu : `ep1_build.py`, `ep1_cards.py`, `ep1_vo.sh`, `ep1_vo_lines.tsv`,
les pièces, les cartes, la voix et le master sont intacts. **Le code est la vérité as-built** ;
ce document l'explique.


---

---

## 12-octies. Passe du propriétaire — 2026-08-25

### 🟢 Le bandeau jaune est maintenant nommé

Le gel de 7 s du bloc 4 laisse le spectateur trouver tout seul un **bandeau horizontal**
qui n'est **pas** un trou de synchro : c'est le bug de hauteur de fenêtre du plan VDP2,
corrigé depuis, que le propriétaire avait marqué en jaune. Le laisser sans marque, c'est
inviter le public à le compter comme un quatrième trou.

Boîte **grise et fine** (`t=3`, `0xA39B8C`) contre les trois boîtes **rouille épaisses**
(`t=5`), posée **en dernier** (+3,30 s), et l'incruste suit à 5,7 s :
*the grey band is a different bug — VDP2 plane windowing, fixed since*. La légende arrive
**après** la boîte, jamais avant : nommer ce qui n'est pas encore dessiné ne se comprend pas.

### 🟢 Bloc 5 en demi-écran — la proposition du propriétaire, mesurée

Le lit `SP2` est un split vertical : **joueur 1 bouge à gauche, joueur 2 est immobile à
droite** (vérifié sur trois frames à t=1/8/15 du `_bed.mp4`). Une carte plein écran par-dessus
cache la seule chose qui bouge pendant deux minutes.

`half_card()` recadre la carte sur **son propre encrage**, la met à l'échelle dans 900×1000 et
la pose sur la moitié droite. Joueur 1 garde sa **luminosité pleine**, joueur 2 transparaît à
10 % sous la carte.

⚠️ **Ça ne marche que pour les cartes TEXTE**, et c'est une mesure, pas un goût :

| | encre | facteur vers 900 px |
|---|---|---|
| `C9_cef` | 896 px | **1,00** |
| `C10_0/1/2` | 924 / 956 / 988 | 0,97 / 0,94 / 0,91 |
| `C11_sequence` | 954 | 0,94 |
| `C7_fafling` | 998 | 0,90 |
| `C12_opens` | 1084 | **0,83** — le pire |
| **`D4a…d`** | **1666 → 1718** | **0,54 — illisible** |

Les quatre diagrammes D4 restent donc plein écran. **104 s en demi-écran sur 141**, et les
37 s restantes sont un autre rythme visuel, pas un pavé de texte.

### 🟡 Le lit du bloc 5 — changement de fenêtre, pour une petite raison

🔴 **Une raison invoquée à tort.** Avec le joueur 1 désormais en luminosité pleine, le
message `GOD MODE` de `SP2` devient lisible ~24 s d'épisode, et j'en avais fait un argument.
Le propriétaire a tranché : **ce n'en est pas un** — c'est une capture de test, et le HUD le
dit déjà. Ne pas réutiliser cet argument.

Trois fenêtres, un échantillon par seconde :

| fenêtre | mouvement gauche | mouvement droite | frames avec message |
|---|---|---|---|
| **`SP1` 33–60 s (27,0)** | 20,1 | 4,2 | 1 / 54 — retenue |
| `SP2` 35–53 s (18,4) | 28,5 | 1,1 | 5 / 18 |
| `SP3` 20–40 s (20,0) | 12,3 | 8,6 | 6 / 19 |

**La raison qui reste est la longueur de boucle**, et elle est mineure : 27 s se répètent 3,9
fois sur les 104 s de cartes en demi-écran, contre 5,7 pour 18,4 s — et une répétition se voit
maintenant que la gauche joue en pleine luminosité. `SP2` a la gauche plus vivante (28,5 contre
20,1) et la droite plus immobile : c'est un **arbitrage, pas une amélioration**. Deux lignes
pour revenir en arrière.

⚠️ Et le chiffre de la colonne « message » a dû être corrigé : le premier balayage annonçait
0/54 pour `SP1` en échantillonnant à la seconde **et en sautant sa propre première frame**, donc
en ratant « PICKED UP THE ARMOR. » à la première seconde. Un test qui saute son premier
échantillon rapporte l'absence de ce qu'il n'a pas regardé.

### 🟢 9:30 — D2 amorcée, pas supprimée

Le problème n'était pas le diagramme : c'est qu'il **arrive sans lien énoncé** juste après
*TNT: EVILUTION HAS NEVER RUN ON A SEGA SATURN*. Une incruste l'amorce — `AND THIS IS WHAT MADE
IT PLAYABLE` / *the sync fix bought the drawing window* / *this one bought the frame*. C'est la
**seule réponse chiffrée** de l'épisode à « pourquoi TNT tourne » ; la couper coûtait plus
qu'elle ne rapportait. Une ligne à retirer si la coupe est préférée.

### 🔴 9:43 — la formulation demandée disait le CONTRAIRE du diagramme

Proposé : *« These maps refused to boot because the hardware lacked the ram »*. Or le
diagramme montre **240 Ko libres**, en huit morceaux, le plus gros à 48 Ko : c'est de la
**fragmentation**, pas de la pénurie. Écrire « lacked the RAM » aurait donné à un public qui
vérifie une phrase que la barre juste en dessous réfute.

Titre → **`WHY 38 MAPS REFUSED TO LOAD`**, sous-titre → **« the RAM was free. it was not in one
piece. »** Le titre pose la question, le sous-titre répond. L'ancien titre
(`FREE IS NOT THE SAME AS CONTIGUOUS`) énonçait le mécanisme sans jamais dire ce qu'il
expliquait. Même famille que [[verify-causal-claims-in-published-copy]].

### 🔴 Le rebuild complet a révélé trois pièces « fantômes »

La dernière passe `pieces` complète **pré-datait la réécriture du bloc 6**. Trois pièces
gardaient sur le disque un fichier **plus long** que ce que le script annonçait, et un rebuild
les a silencieusement tronquées — emportant trois incrustes avec elles, dont
`SO THE MONSTERS CAME BACK`, ancrée à 14,5 s dans une pièce devenue longue de 14 s.

| pièce | sur disque | dans le script | ce qu'elle doit PORTER |
|---|---|---|---|
| `p39` | 24 s | 14 s | 1,5+11 et 14,5+8,5 → **24** |
| `p42` | 6 s | 4 s | 0,5+5,0 → **6** |
| `p48` | 18 s | 14 s | 0,8+9,0 et 11,0+6,5 → **18** |

Le pas D3 repasse à **4,28** (et non 4,4) : `48 + 5×4,4 = 70` déborde `T1B`, qui fait 69,44 s.

⚠️ **Garde-fou ajouté** dans `build_ass()` : toute incruste ancrée **au-delà** de la durée de
sa pièce déclenche un `WARN`. Rien dans la sortie ne le disait — le `.ass` se parse, le master
s'encode, la ligne n'apparaît simplement jamais. Même famille que le garde-fou voix orpheline.

### Crédit Claude, simplifié

`Claude, by Anthropic — paired on the port`. Le montage de la vidéo n'a pas à y figurer.

---

## 13. Publication — description YouTube et post SegaXtreme

⚠ **Zéro pronom**, `it` / `its` / `they` compris. La contrainte vaut pour **tout** le texte
publié, pas seulement pour l'écran : le mécanisme est le sujet. (La voix dit « I » — c'est un
récit personnel — mais rien d'écrit ne le fait.)

### 13.1 Titre

> `Doom on Sega Saturn now runs TNT: Evilution — Mimas Devlog #1`
>
> 61 caracteres, sous la coupe YouTube (~70). Les termes cherchables d'abord
> (`Doom`, `Sega Saturn`), le resultat au milieu, le nom du projet a la fin ou
> vit l'identite de serie. Un seul deux-points, celui de `TNT: Evilution`.
>
> Le titre ne repete PAS la miniature (`ONE FRAME BEHIND`) : le titre donne le
> resultat, la miniature garde la question. Un couple qui dit deux choses.

### 13.2 Description YouTube

**Aucun retour à la ligne dur** — YouTube reflue le texte, et un paragraphe pré-coupé devient
déchiqueté. Une ligne longue par paragraphe et par puce.

    Mimas is a hardware-rendered Doom port for the Sega Saturn: walls, world sprites and the weapon on VDP1, the sky and the largest floor plane on VDP2, everything else on two SH-2 at 28 MHz. Every frame in this video was captured from a real console, not an emulator.

    This episode is about one bug, and the bug lasted two months. The first gap appeared on 17 June 2026, one day after the first VDP1 wall reached the screen, and survived four geometric corrections, six presentation mechanisms and one expensive CPU workaround. Two errata on one page of the official SEGA VDP1 manual are part of the answer - and the larger part is worse than a typo: the three things that would have fixed it had never once been tried in the same build.

    In this episode:
    - the symptom, held still: the room moves on one frame, the walls move on the next
    - four geometric corrections, each of which made the artefact worse in both directions
    - the measurement that reframed the problem: a ~12.5 fps game whose 30 fps capture holds 18.3 distinct pictures per second
    - one frame cut into layers: the VDP1 weapon and status bar, the VDP1 walls, the gaps, the VDP2 floor plane, and the CPU picture
    - six ways to present a frame, and the reason each one of them died
    - lead-fill, the workaround: the CPU repainting the missing band on both SH-2 every frame, for a price of 1.5 to 5 fps
    - ST-013 p.38, "in one frame" corrected to "in one field", and the p.39 prescription corrected from 0x0000 to FCM=1
    - and what those errata are actually worth: one of five reasons, with the one that outranks them named on screen
    - VBE erase-and-change: the sequence SEGA's own SBL ships in SCL_VBLV.C, and the one Lobotomy Software's SlaveDriver ran in PowerSlave, Duke Nukem 3D and Quake on Saturn
    - what a wider drawing window opens: sprites reaching VDP1 in split-screen, and walls yielding place to the monsters when the command queue runs short
    - the 415-map test bench, and why 38 of it refused to load with 240 KB free: the RAM was there, it was not in one piece
    - the month of memory work that cleared all but the Nuts family: line_t 64 to 24 bytes, texture directories made lazy and purgeable, every lump read in place

    Mimas runs the Doom shareware IWAD and full commercial WADs streamed straight from the disc, on a stock 2 MB Saturn, with no RAM cartridge. The engine is doomgeneric over Chocolate Doom; the platform layer is built on Saturn Ring Library (SRL) by ReyeMe, over SEGA's SGL. No IWAD is ever distributed with any of this.

    With help from: fafling, who named the cause in a SegaXtreme post on 3 August and knows the retail Saturn Doom in depth; slygamer and wesker for hardware testing; TrekkiesUnite118 for correcting several of my wrong claims on the forum; the Kronos team, Runik, fafling and Benjamin Siskoo, for the errata-corrected VDP1 and VDP2 manuals; Claude, by Anthropic, which paired on the port; and the SegaXtreme forum.

    Mimas is a fan project. Doom is id Software's. Not affiliated with SEGA or id Software.

    #SegaSaturn #Doom #homebrew #retrodev #gamedev #SH2

    Chapters:
    00:00 Doom on a Sega Saturn
    00:24 What draws what
    02:17 The holes
    02:37 Four geometric corrections
    03:01 It only happens when the picture moves
    03:28 Three frames
    03:54 One frame, cut into layers
    04:39 Six ways to present a frame
    04:59 Lead-fill, and what it cost
    05:21 July against August
    06:13 The cause, named from outside
    06:29 The field is the budget
    07:06 The completion flag lied
    07:30 Two words in the manual
    07:54 The sequence with no ambiguity
    08:12 What it opens
    08:34 Two players, one Saturn
    09:18 TNT: Evilution
    09:32 The bench: 415 maps
    09:42 Why 38 refused to load
    10:04 What actually changed
    10:50 Credits

⚠ **Ne pas retaper ces horodatages à la main.** `python tools/devlog/ep1_build.py chapters` les sort du tableau de pièces **mesuré** : `CHAPTERS` associe un chapitre à une **pièce**, pas à une seconde. Une liste tapée à la main était déjà fausse de 6 s après une seule coupe dans le bloc 2, et un chapitre qui tombe au milieu d'une phrase est pire que pas de chapitre.

### 13.3 Post SegaXtreme

Règles maison : registres et `fichier:ligne`, **jamais** un numéro de page seul ; toute
affirmation matérielle vérifiée contre `../saturn-refs/manuals/` avant d'être écrite ; une
section « two things I got wrong » chiffrée ; une section « still broken, named ».

    Mimas - Doom on Saturn, devlog #1: one word in the manual

    

    @That was the right thing to point at. VDP1 was locked to the field, it had been since 17 June, and it took until 19 August to unlock it - not because the idea was hard, but because three things that each had to be true had never once been true in the same build. This post is what "I'll try that" turned into.

    Two corrections to my own reply first, because both matter to anyone following the same advice. The route is not SGL's variable-framerate mode: I did try that in July but it died because slSynch never writes EWLR/EWRR, so VDP1 erases a rectangle of width zero, and writing those two registers by hand freezes the menu. What shipped is the mechanism underneath it - the manual frame change the VDP1 manual calls VBE erase-and-change - driven directly, register by register. And the 30 fps cap never came into it: this engine runs at 7 to 16 fps, so one game frame already lasts four to eight fields.

    Video, 11 minutes, every frame captured from a real console: <<YOUTUBE URL>>

    Standing disclaimer: feel free to correct anything below. Several claims in earlier posts here turned out to be wrong, and the corrections were worth more than the posts.

    -- THE SYMPTOM --

    From 17 June to 19 August, gaps opened between the VDP1 walls and the software picture, mostly at wall/ceiling joins, and only while the view moved. Four geometric corrections were tried - shift the quad, shift the user clip window, sweep the projection gain, re-project the corners - and every one made the artefact worse, symmetrically, in both directions of rotation. Symmetric worsening in both directions is a timing signature, not a position one, and that is where the geometry hypothesis should have died.

    -- AND SIX WAYS TO PRESENT A FRAME --

    Between 27 June and 2 August, six presentation mechanisms were built and abandoned: a draw-gated present (no tearing on hardware, walls a field behind on 30-60 % of frames); a full-slSynch branch (the erase-rectangle failure in the opening); an NBG1 couple (gated off and never re-armed, because it waits on CEF); a present A/B pair of toggles, parked the same day; a coherent-pair present holding a walls+floors pair this renderer never builds; and a field-lock that pins the BLIT rather than the SWAP - perfect pairing on every capture, holes intact.

    -- THE MEASUREMENT THAT REFRAMED THE PROBLEM --

    The July capture is 30 fps OBS over a game running at about 12.5, so two captured frames in three are duplicates and no per-captured-frame test means anything. Counting DISTINCT pictures instead: 938 in 51.4 s, or 18.3 per second, against roughly 12.5 game frames produced per second. The count is stable from 17.2 to 18.9 for any difference threshold between 0.4 and 1.0, and the frame-difference distribution is cleanly bimodal - encoder noise at 0.06-0.17, real changes above 0.4. A capture cannot hold more distinct pictures than the game produced frames, unless some pictures are assembled out of two different frames.

    Three consecutive captured frames, measured on the native 320x224 grid: the room moved +14 px then +0, the walls +0 then +18. Each layer moves in exactly the frame the other one does not.

    -- THE FIRST WRONG THING: CEF --

    Whether the drawing was finished was read from CEF, EDSR bit 1 at 100010H. The manual carries the warning on the same page: "This bit is reset to 0 when the frame buffers are changed or when drawing is started", and "If fetch of the draw terminate command matches when the frame buffer changes, CEF and BEF might not become 1." On silicon my two probe campaigns disagree about CEF: the first read it set on 30 to 60 % of frames, a later one read it always B and retired the metric as dead. They have never been reconciled by measurement, and it does not matter here, because neither reading makes CEF a usable gate. The gate that is never ambiguous is the command address register, COPR.

    -- THE SECOND WRONG THING: TWO ERRATA ON THE SAME DEVELOPER CD --

    ST-013 p.38, developer CD: "The number of characters that can be drawn in one FRAME is limited. Therefore, in order to draw more characters, the manual mode must be set." Corrected scan: "in one FIELD". With "frame", the sentence is a tautology, because the manual defines a frame as the interval between two framebuffer changes. With "field", the sentence is an instruction.

    ST-013 p.39, developer CD: manual ERASE is described as "writing 0 to the VBE, FCM and FCT registers". Corrected scan: "writing 0 to the VBE and FCT registers and 1 to the FCM register". The first spelling is FBCR = 0x0000, which the table on the facing page calls 1-cycle mode - the opposite of what the bullet is prescribing. Any implementation taken from that CD does nothing.

    Worth adding, because the manual calls all three of them "registers" and the mode table on p.38 lists them as one triple (FCT, FCM, VBE): VBE is not an FBCR bit. It is bit 3 of TVMR at 100000H. FBCR is at 100002H and holds EOS, DIE, DIL, FCM, FCT. The erase-and-change sequence therefore spans two registers, which is p.40, not p.39.

    -- WHAT THE MANUAL'S OWN DEFINITIONS SETTLE --

    ST-013, Introduction: "A field is the time it takes a scanning line to scan one screen. A frame is the time it takes from one change of the frame buffer to the next change." And: "When the change mode of the frame buffer is in a one cycle mode, one frame is equal to one field." So the drawing window is closed by the SWAP, by definition. In 1-cycle mode the hardware closes the window every field, finished or not; in manual mode the CPU closes it with one write. At 7-16 fps one game frame lasts 4 to 8 fields, so 1-cycle mode was handing this engine somewhere between a quarter and an eighth of the budget actually available.

    And overrun does not cost time, overrun costs content: the command list restarts at 00000H every frame, so an undrawn tail is abandoned, never resumed.

    -- AND WHAT THOSE ERRATA ARE ACTUALLY WORTH --

    One fifth of an answer. My own audit lists five reasons this took two months, and the errata are one of them. The one that outranks them is structural: a manual present, a fenced blit and a gate that does not read CEF had never once been in the same build - the fence post-dates every present experiment by a month. Two more sit above the errata as well. Every present verdict formed on Ymir was void: it models neither manual CEF nor LOPR, and never overruns. And the CEF gate was mine, not the manual's - the manual prints that caveat correctly. The shipped driver's own comment records that v1 already wrote FBCR = 0x0003, the value the corrected page prescribes, and failed anyway - what changed in v2 is the window, not the value: v1 pulsed it in the p.38 OUT window, v2 arms TVMR.VBE=1 with it at a fresh vblank IN and clears VBE after the OUT edge. The errata are a real trap, established as a trap; nothing establishes that this driver fell into it. Both spellings are still worth publishing, which is why they are in the video.

    -- THE SEQUENCE THAT WORKS --

    VBE erase and change: arm the erase on a fresh V-blank IN with TVMR bit 3 set and FBCR = 0x0003, and the swap lands at the end of that same blank. One write, one edge, nothing left to infer. Nothing exotic about it either - SEGA's own SBL ships that sequence in SCL_VBLV.C with interval 0xfffe, and Lobotomy Software's SlaveDriver ran manual frame change in-game in PowerSlave, whose source is what was read here. The same engine went on to power Duke Nukem 3D and Quake on Saturn.

    -- TWO THINGS I GOT WRONG --

    1. The status bar was described here as CPU-drawn. Wrong: the bar goes through the VDP1 HUD capture/emit path in every mode, and is deliberately one frame late - which is exactly what makes the bar a good control. Same chip, same field, same lateness, and no artefact at all, because a screen-anchored sprite does not move with the room.

    2. An automatic hole detector was written that gated on camera rotation, on the model "the artefact comes from turning". That model threw away the worst frame in the whole capture, where the camera is nearly static. There are two failure modes, not one: LATE, where the displayed buffer is a field behind and the gap is proportional to rotation speed, and MISSING, where the buffer reaches the screen partly drawn, independent of movement. The second one makes the big holes.

    -- THE OTHER WALL: 38 MAPS THAT WOULD NOT LOAD --

    A 415-map bench (Doom, Ultimate, Doom II, TNT, Plutonia, Hell Revealed, Scythe, Nuts 1/2/3) had 38 maps that did not run slowly - they refused to load at all. Not for want of RAM: the Doom zone is 1,040,384 bytes and around 240 KB of it was free. It was in eight pieces, the biggest 48 KB, cut up by two PU_STATIC blocks parked at 64 KB and 655 KB, and the single largest allocation on a blocked map wants about 110 KB in one run. Two levers, not one: shrink the request (line_t 64 to 24 bytes - that one alone took the blocked count from 38 down to the Nuts family alone, while seg_t 32 to 14 and node_t 52 to 28 freed real RAM and moved zero maps, because LINEDEFS was the dominant term on every one of the 38), and grow the longest run the zone can still hand out (texture directories made lazy and purgeable: largest contiguous 48 KB to 130 KB). Defragmenting all the way to 110 KB is possible and is not free - that RAM is holding the sprites, the textures and the sound the map is playing.

    -- STILL BROKEN, NAMED --

    - The frame rate is 7 to 16 fps on a stock console, and the dominant term is not the present. The two real gains of the month were offline WAD work and a struct diet, not this fix.
    - The governor that degrades quality by phase is blind in split-screen: the phase election reads single-player fields.
    - The Nuts family of maps still refuses to load. Every other map in a 415-map test set now does.

    -- THANKS --

    fafling named the cause and knows the retail Saturn Doom in depth; slygamer and wesker ran the hardware tests; TrekkiesUnite118 has properly corrected several of my wrong claims in this forum, more than once; the Kronos team (Runik, fafling, Benjamin Siskoo) produced the errata-corrected VDP1 and VDP2 manual scans, and ST-013 is what this post leans on; ReyeMe wrote Saturn Ring Library. Claude, by Anthropic, paired on the port.

### 13.4 Miniature

`tools/devlog/mkthumb.py`, paire choisie **par mesure** : la même cour extérieure d'E1M1 des
deux côtés — juillet `11-22-01`, août `184951` — le côté juillet pris sur la frame de rotation
qui porte le plus gros décrochage mesuré. **Même *lift* des deux côtés** : soulever un seul
côté transforme une comparaison en mensonge, et une capture de référence plus douce nous
flatterait gratuitement.
