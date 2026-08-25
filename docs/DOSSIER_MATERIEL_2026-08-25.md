# DOSSIER — le matériel au maximum

**2026-08-25.** Réponse au brief : « pour chaque matériel, les cas d'usage extrêmes
envisageables ; qu'est-ce qui est sur le chemin critique et pourquoi ; comment soulager
ou déporter ; un nouveau plan. »

Compagnon de `HEADROOM_2026-08-25.md` (le tableau de bord chiffré). Ici : le POURQUOI
par puce, les hypothèses extrêmes — y compris celles qui meurent, avec la loi qui les
tue — et le plan. Toute mesure vient de
`docs/captures/2026-08-25_console_overlay.csv` (106 frames console) ; **aucun chiffre
d'Ymir**.

---

## 0. Les captures d'aujourd'hui : ce qu'on a payé, ce qu'on en a tiré

**Payé** : 9 vidéos console (toi, ce matin) → 121 frames extraites → **106 lisibles
décodées** et persistées dans `docs/captures/` (plus jamais à re-décoder). Côté tokens :
la passe d'analyse + le balayage headroom = 13 agents ≈ **2,1 M tokens** de sous-agents,
plus les passes de décodage vision de ce matin.

**Tiré** — chaque ligne ci-dessous est NOUVELLE aujourd'hui et console-prouvée :

1. **La loi du split** : 4p peint exactement les pixels de la 1p (61 440) pour 3,5× le
   temps. L'inflation est la **génération par vue** (`Bw` ×3,5, drawsegs ×3,8), pas le
   remplissage (`P` plate à 14,8-15,6 ms dans les 4 modes). Constante : 0,63 ms/drawseg.
2. **Le budget 4p ne ferme pas** : ~31 ms/frame (20 %) hors de tout bracket, en deux
   trous distincts (§1).
3. **Le cap de tics est mort** : `x` épinglé à 5,0 (17/31 frames 4p) — le monde 4p
   tourne à 89 % de sa vitesse.
4. **Le gouverneur dort dans tout le split** : `w0 p0 sb0` sur 76/76 frames split.
5. **Le fence expire déjà en 4p** : `sat_mp_wd` monte à w7 dans une seule vidéo.
6. **`slScrAutoDisp` ≈ 17 ms/frame en split** (dg-pre 21-24 vs 3-6 en 1p).
7. **`rbg0_upload_flat` flambe en 2p** (9-12 ms, rebuild 131 Ko non demandé).
8. ~~**`fl` = 10 ms de fill sprite en 3p avec 0 sprite dessiné**~~ — **🔴 RÉSOLU
   2026-08-25, et c'était une erreur de LECTURE : voir §7.1.** Le format est
   `"SPR fl%d.%d …"` — `fl` est le **nom du champ**, et son `l` se lit `1` à l'écran.
   La valeur réelle est **0,0 à 0,3 ms** sur 13 captures, 2 WADs, 4 modes. Il n'y a pas
   de fill sprite logiciel : les things roulent déjà sur le VDP1. La sonde est saine ;
   c'est moi qui la lisais mal.
9. Les caps sont froids (visplanes 35/256, drawsegs 56/256), `tx` 18-19/26 partout
   (la créance « 300 Ko WTEX dormants » est morte), slave 2 % en 3/4p, minimap 3 ms.
10. Les identités de l'autre session tiennent (`mo+pt+w ≈ th` : 92/100 à ±2 ms) ;
    `mo/n` = 18,7 µs/appel, inchangé.

On n'en a pas « rien fait » : **tout le plan ci-dessous est assis dessus.**

---

## 1. Le chemin critique 4p — ce qui coûte cher et POURQUOI

Frame 4p médiane 161 ms :

```
Bp 44,5 ── R_StoreWallRange ─── CPU maître, données LWRAM      ← génération, ∝ drawsegs
P  15,5 ── plans (44-86 % = émission VDP1 pr)                  ← plate dans les 4 modes
Bw 14,7 ── BSP + projection sprites                            ← génération, ∝ vues
k  11,1 ── kick VDP1 (1× pour 4 vues)                          ← émission, ∝ commandes
fence 19,0 ── attente présentation (g 8,5 = COPR)              ← attente pure, expire déjà (w7)
dg-pre 22-24 ── dont slScrAutoDisp ≈ 17                        ← SGL, pas nous
b  4,2 ── memcpy blit    T 12,8 ── 5 tics   M 1,7   overlay ~4
─────────
+ ~31 ms NON BRACKETÉS (deux trous : 11,2 ms dans Bp hors pr/lp ; 11,5 ms hors boucle et hors kick)
```

**Pourquoi c'est cher** : Doom génère sa scène par un parcours BSP dont le coût est
proportionnel aux *surfaces acceptées*, pas aux pixels. Quatre caméras = quatre
parcours, et chaque quadrant de 160 px **accepte encore un frustum de 90° complet**
(`FIELDOFVIEW` constant, `core/r_main.c:47`). On paie quatre scènes entières pour
peindre une seule surface d'écran.

**Comment soulager, dans l'ordre de rendement** : réduire ce que chaque vue *accepte*
(FOV), supprimer les attentes (fence, slScrAutoDisp), réveiller le régulateur, nommer
les 31 ms inconnus. Le détail chiffré est dans HEADROOM §2.

---

## 2. Par matériel — état, cas extrêmes, verdict

### 2.1 SH-2 maître (28,6 MHz) — le goulot, et les trois échappatoires

Il fait tout : tics, BSP ×N, émission VDP1, blit, séquenceur MUS, fence. La question
« est-on obligés de toujours ajouter du travail à MSH2 pour nos nouvelles idées ? » a
une vraie réponse structurelle : **il existe exactement trois échappatoires**, et le
plan les exploite toutes :

1. **Le déclaratif VDP2** — fenêtres, scrolls, couches : zéro CPU par frame une fois
   configuré. C'est le ciel 4 quadrants (§2.5).
2. **La compilation hors-ligne du WAD** — tout ce que `flatten/split/repack` savent
   déjà faire : zéro CPU à l'exécution. C'est le nodebuilder et le dicing SlaveDriver
   (§3).
3. **Le domaine son** — le 68K + SCSP vivent dans leur propre monde ; tout ce qui est
   exprimable en « son » est gratuit pour MSH2 (§2.6).

Tout le reste (slave, DSP SCU, DMA) a été mesuré ou interdit — voir plus bas.

### 2.2 SH-2 esclave — 98 % oisif en split, et c'est structurel

**État** : `b` = 11/5/2/2 % (1p/2p/3p/4p), `Pb` = 26/48/27/24 %. Il vole des spans de
plans (TAS shippé) et c'est tout ce qui a jamais payé.

**Cas extrêmes envisagés et leur sort** :

| hypothèse | verdict | pourquoi |
|---|---|---|
| 2e renderer (une vue par SH-2) | **MORT ×3** (`slave-second-renderer-bp-study`) | l'état de clip (solidsegs/openings) est un fil séquentiel |
| wall-prep → slave | **MORT ×3 HW** (`RANK3_WALLPREP`) | taxe mémoire slave **×2,1** sur travail memory-bound |
| **émission des quads (le kick) → slave** — ta question | **MORT PAR ARITHMÉTIQUE, re-pricé aujourd'hui** | le kick est VRAM-write + LWRAM-read = memory-bound : 11 ms maître → 17-23 ms slave. Le maître économise 11, attend ~14 au join. **Net ≈ −3 ms.** Et après le levier FOV, k tombe à ~8 : encore pire. |
| paralléliser la GÉNÉRATION entre vues | **MORT** | aucune unité indépendante : BSP→solidsegs→drawsegs→CheckPlane est une chaîne ; les caches textures sont déjà dédupliqués (`bk0`) |

**Ce qui changerait le verdict** : du travail **compute-bound à cache chaud** (la seule
classe où le slave a jamais payé). Le fill logiciel en split écrit **zéro pixel**
(`SEG f0 k0` sur presque toutes les frames split) — il n'y en a plus. ⚠ **La porte de
sortie que ce paragraphe gardait ouverte est FERMÉE** : `fl` vaut 0,0-0,3 ms (§0.8
corrigé), donc il n'y a **aucun** fill sprite logiciel à reconvertir en travail slave.
Le slave reste sans client en split.

### 2.3 VDP1 — de la marge de PLOT, pas de marge d'ÉMISSION

**État** : 32,9 Ko VRAM libres (carte exacte, ferme sur 0x80000) ; en 3p le VDP1 finit
sa liste en 15 ms d'une frame de 149 avec gate-spin ~0 → **~9× de marge de plot**. Le
goulot n'est pas le VDP1, c'est le CPU qui le nourrit (`pr` 4,9→12,8 ms, 64,5
µs/commande).

**Tes trois questions** :

- **« Utiliser les commandes/quads restants ? »** Oui — mais comme levier de **valeur**,
  jamais de vitesse, car chaque commande coûte 64,5 µs de MSH2. Le meilleur candidat :
  **things→VDP1 en split** — ⚠ **la justification chiffrée de cette opportunité est
  MORTE** : elle reposait sur « `fl` = 10 ms de fill logiciel à reconvertir », or `fl`
  vaut 0,0-0,3 ms (§0.8 corrigé) et les things sont **déjà** sur le VDP1. Il n'y a pas
  d'échange à faire. Le préalable, lui, est LEVÉ : la row `V1` est dégatée en split
  depuis le 2026-08-25 et la première lecture existe — `V1- c232 B0 f166/0/0 ec2 W6/0`
  en 4p. ⚠ `B0` n'est pas un bug : `vdp1_budget_cmds == 0` signifie « pas encore
  mesuré » et l'AIMD le traite comme illimité — et comme Ymir ne modélise ni LOPR ni le
  latch CEF, **`B` lira 0 pour toujours sur émulateur.** C'est un champ console-only.
  La banque 384 est morte aujourd'hui : +8,2 ms d'émission pour la remplir.
- **« Mon idée des plans en quads est vraiment morte ? » NON — la version RUNTIME est
  parquée, la version HORS-LIGNE n'a jamais été tentée.** Les rounds 6-8 d'inc-2 ont
  tué le *scan runtime* (v132 pour ~1 tuile : scan × 15 petits plans + famine de slots).
  Mais **SlaveDriver — le moteur de PowerSlave/Duke3D/Quake Saturn — est en source
  GPL3 sur GitHub depuis août 2025** (`Lobotomy-Software/SlaveDriver-Engine`). Sa
  recette est exactement l'inverse de la nôtre : le monde est **pré-découpé en quads à
  la compilation du niveau**, le runtime ne scanne rien. Notre pipeline WAD est déjà un
  compilateur (flatten/split/repack) : une passe de **pré-tessellation des
  sous-secteurs en quads de sol** est le chaînon manquant, et le code de référence est
  lisible et GPL-compatible. Ce qui reste vrai : régions interdites (VDP1 > RBG0/ciel),
  budget de plot (transfer-over LOPR), famine de slots — le dicing hors-ligne résout le
  scan, PAS la famine de slots : l'étude doit d'abord compter les flats simultanés par
  scène. **C'est l'étude structurante de l'automne, pas un quickwin.**
- **« Doom a tourné sur moins puissant ; Quake/Duke sur Saturn » — le fait est un
  argument POUR cette direction.** SlaveDriver fait ~20 fps en 1p en mettant TOUT sur
  le VDP1 et rien dans un renderer colonne. Mimas y converge déjà (murs+things VDP1 en
  split, inversion de couches). Les surfaces logicielles restantes sont les plans — et
  la génération par vue, qu'aucun moteur de 1996 n'a eu à payer ×4 (personne n'a shippé
  un Doom 4 joueurs sur console 32-bit).

### 2.4 VDP2 — la puce la plus sous-employée, et la seule à contenu gratuit

**État 3/4p** : RBG0 off ⇒ **A0+A1 = 256 Ko sans consommateur**, W1 libre, NBG2 libre,
NBG3 = texte debug.

**LE cas extrême : le ciel HW sur les QUATRE quadrants — oui, c'est faisable, et la
rotation suit chaque joueur.** Ta lecture est la bonne. Le montage :

| couche | fenêtre | vue servie | lacet |
|---|---|---|---|
| NBG0 | W0-inside = colonne gauche | **0 et 2** | **scroll par ligne** : lignes 0-95 = lacet J1, lignes 112-207 = lacet J3 (`slLineScrollTable0`, NBG0/NBG1 seulement) |
| NBG2 | W1-inside = rect haut-droit | **1** | scroll H de couche = lacet J2 |
| NBG3 | W0-outside ∧ W1-outside = bas-droit (les fuites tombent sous les bandeaux HUD opaques) | **3** | scroll H de couche = lacet J4 |

- **Chaque quadrant scrolle avec le lacet de SON joueur** — c'est tout l'intérêt. (Pas
  de tangage : le Doom vanilla n'en a pas, le problème n'existe pas.)
- Le raccord vertical des deux bandes NBG0 est **cuit dans la carte de pattern-names**
  (dupliquer les entrées PNT à partir de la ligne 112 — zéro VRAM de cellules en plus).
- **Contrainte vérifiée au manuel** : NBG2/NBG3 n'ont **aucune** fonction d'échelle
  (Table 1.4). Notre ciel split vit sous `ZOOM_HALF`. Solution : **cuire la réduction
  0,5 dans les tuiles uploadées au chargement** — et alors NBG0 peut l'adopter aussi et
  **abandonner ZOOM_HALF**, qui est précisément le suspect des 17 ms de
  `slScrAutoDisp` (double slots de lecture). Les deux leviers se renforcent.
- Couleurs OK (256 partout, NBG1 8bpp ne tue pas NBG3, NBG0 256 ne tue pas NBG2).
- **Prix** : en 4p, NBG3 = ciel ⇒ **plus de texte debug dans ce mode** (toggle debug ↔
  ciel). En 3p, pas de sacrifice : 3 vues, NBG0(×2)+NBG2 suffisent. En 2p :
  NBG0+NBG2 = **les deux moitiés en HW** (aujourd'hui une seule vue élue) — mais les
  données NBG2 doivent cohabiter avec le sol RBG0 dans A0 (audit de cycles dédié).
- **Le gain** : ton observation — « le coût CPU est énorme en extérieur » — est
  exactement ce que la sonde doit chiffrer. Le compteur existe (`sat_sky_px_view[]`,
  `core/r_plane.c:1108`) et n'est pas imprimé. À 7 cycles/px : ~2 ms/vue extérieure,
  ~6 ms les trois, **0 en couloir**. Levier dépendant de la carte, comme le tien.
- **Test** : géométrie/raccords/comptages sur **Ymir (valide)** ; la neige (cycles
  VDP2 re-déclarés) est **console-only**. Build derrière un accord de pad.

**Le reste du VDP2** : RBG0 en 3/4p reste MORT (un plan, une matrice, et le sol y est
déjà `SQ_FLAT` — `VDP2_RBG0_CURRENT_STATE.md`) ; bandeaux HUD en couche cellule ≈ 1 ms
(rangé, à faire un jour de pluie) ; brouillard line-color tue NBG1 (mort).

### 2.5 Le boîtier son : TROIS processeurs, pas un — et je te dois une correction

J'ai écrit « 68K inatteignable, rien à prendre ». C'était **la bonne réponse à la
mauvaise question** — la question « peut-il décharger du travail SH-2 *existant* ? »
(non : `S` = 0 ms sur 106/106). Ta question est « peut-il faire PLUS ? » — et là tu as
raison, il est **sous-exploité**, pas surexploité. Le boîtier contient :

1. **Le 68EC000 (11,3 MHz)** — CPU libre, espace d'adressage = les 512 Ko de RAM son.
   « Il suffit de passer par la RAM son » : oui, c'est exactement le canal — le SH-2
   écrit/lit la RAM son à 0x25A00000 (bus B, 16-bit). Le coût du transfert est réel
   mais **fixe et petit pour des lots compacts** ; ce qui est mort, c'est le
   par-pixel/par-colonne, pas le par-lot.
2. **Les 32 slots PCM/FM du SCSP** — on en use ~15 pour la musique + les SFX.
3. **Le DSP du SCSP (128 pas, 44,1 kHz)** — jamais touché. **Tes liens le prouvent
   utilisable en 3D** : le projet AVR fait de la multiplication matrice-vecteur sur
   **5 flux de sommets à 44,1 kHz** (≈ 220 000 transformations/s), le 68K servant de
   chef d'orchestre, données en RAM son, résultats relus par le SH-2. Limites
   documentées : Q1.15, RAM son partagée, incompatible avec les effets audio DSP.

**Cas d'usage extrêmes, du plus payant au plus fou :**

| cas | ce que ça achète | verdict |
|---|---|---|
| **Séquenceur MUS v2 SUR le 68K** | Le séquenceur actuel tourne sur MSH2, appelé **une fois par frame** (`I_UpdateSound` → `mus_step`) : à 6 fps en 4p, l'horloge MUS 140 Hz est servie par paquets de **~22 ticks** — le timing musical se quantifie audiblement. Sur le 68K : tempo parfait **indépendant du fps**, plus de slots, enveloppes, percussions réelles. Et « niveau son je n'ai rien fait » : c'est LE chantier contenu. | **GO étude** — c'est le vrai travail son, MSH2 n'y perd ni n'y gagne une ms |
| **SFX résidents en RAM son** | ~470 Ko de RAM son sous-employés (sram_alloc part de 0x8000). Le « vrai hitch = SON » (`precache-streaming-verdict`) : précharger TOUS les SFX du shareware en RAM son au boot, éviction gérée par le 68K lui-même. | **GO étude** — attaque un hitch mesuré, coût MSH2 nul en jeu |
| Effets DSP SCSP (réverb par niveau, à la PSX Doom) | ambiance gratuite, zéro MSH2 | GO un jour — pur contenu ; incompatible avec AVR-style compute |
| **DSP SCSP = co-processeur géométrie (AVR)** | 220 k transfos/s dispo | **🔴 LIGNE PÉRIMÉE — voir §7.2-A.** Ta remarque était juste : AVR est un EXEMPLE, pas l'enveloppe. L'enveloppe réelle, établie sur source : MAC virgule-fixe 24×13 **sans branchement, sans diviseur**, 128 pas × 44,1 kHz, une seule table-gather 12 bits comme unique opération data-dépendante, ne voit **que** la RAM son, ne répond jamais avant ~250 µs. Ce n'est pas une unité géométrique, c'est un petit DSP — et il est **général** (le décodeur ADPCM 8 voies de celeriyacon le prouve). La porte est fermée par l'arithmétique, pas par l'imagination : **1,55 % d'instructions de multiplication** dans l'ELF livré contre 51 % de mouvements mémoire, et 0,71 % dans `R_RenderSegLoop`. Le seul client réel est la **décompression**, pas le calcul. |
| Le 68K « tic co-processeur » (IA, sight) | — | **MORT, mais pas pour la raison écrite ici — voir §7.2-B.** Le transport est ABORDABLE (4,1 cycles maître/octet). Ce qui tue : (a) *la décision est moins chère que l'expédition de la ligne* — une décision Doom par-mobj coûte 0,5-3 cycles/octet de la ligne qu'elle lit ; (b) tout le domaine tic = **11 ms d'une frame 4p de 158**, donc un 68K magique instantané plafonne à +7-9 % ; (c) le déterminisme PROUVABLE exige une relecture INCONDITIONNELLE = une deuxième barrière de présentation, dans une frame qui porte déjà 19 ms de la première. ⚠ « 68K inatteignable » est **faux** et doit être corrigé partout : il est atteignable, et ce n'est pas pour ça qu'il échoue. |

### 2.6 SCU-DSP et SCU-DMA — fermés, la loi tient

Inchangé (HEADROOM §7) : le DSP SCU n'a aucun candidat qui survive à la fois à
l'arithmétique ET à la loi du bus B (sa sortie serait en VRAM VDP1 = l'interdiction qui
a gelé la console ×4, F00001). `slDMACopy` spin sur canal-libre. **La différence avec le
boîtier son** : le SCSP-DSP travaille *dans sa propre RAM* et le SH-2 relit un petit
lot — c'est pour ça qu'AVR marche là où le SCU-DSP meurt.

### 2.7 Les RAM — qui est rare, qui est riche

| banc | libre | verdict d'usage |
|---|---|---|
| HWRAM pool | **30,9 Ko** (plancher 4,8) | LA ressource rare. Récupérable : +22 Ko prouvés (gc-sections, atof, sha1, i_scale) → +45-110 Ko probables ; `openings` −25,6 Ko après sonde ; HUD split → LWRAM +16,4 Ko. **Pool ×3 accessible.** |
| LWRAM zone | **609-624 Ko** | C'est **la réserve de l'endgame gros-WAD** (TNT/Plutonia la consomment). Sur shareware : caches opportunistes SEULEMENT (gatés sur `zf`), rien de permanent. + le ring RP 8 Ko inerte à réclamer. |
| VRAM VDP1 | 32,9 Ko | qualité (slots things), jamais fps (§2.3) |
| VRAM VDP2 A0+A1 (3/4p) | **256 Ko** | le ciel 4 quadrants (§2.4). PAS de la scratch RAM (bus B) |
| RAM son | **~470 Ko** | SFX résidents + séquenceur v2 (§2.5) |
| Cartouche 4 Mo | 0 (mode B) | E/S seulement. La pièce à jouer : `DOOM1C.WAD` non-flattené → mode A zéro-copie. Signe inconnu (texels bus A par pixel vs −7-12 ms/défaut de lump) : **un build, une session console.** |

### 2.8 Le CD — déjà libre, et le 68K le libère pour toujours

Le build par défaut est `-Mus` (pas de CDDA) ⇒ **le lecteur CD est déjà oisif pendant
le jeu**. Toute amélioration du séquenceur (68K) pérennise ça : jamais de retour à
l'arbitrage CDDA-contre-streaming (le glitch de seek documenté). Le CD reste le budget
de chargement (R2.3 pump, R4) — chantier connu, hors de ce dossier.

---

## 3. Tailler les WADs pour NOTRE port

Le pipeline est déjà un compilateur (flatten, split_patches, repack LZSS + échelle de
rotations). Ce qui manque, par rendement :

1. **Nodebuilder moderne** (ZDBSP/ZokumBSP, sortie vanilla) — mesuré hors-ligne :
   9,1 % des segs du shareware sont des fragments de découpe ; −2 à −3 ms en 4p, coût
   runtime nul. ⚠ reset toutes les baselines par carte ; ne pas empiler.
2. **Sélection/écriture de cartes pour le split** — **CHIFFRÉ, §7.2-F : c'est le n°1 du
   classement, et le SEUL levier des huit études qui attaque le terme que la loi du split
   dit gonflant avec N.** ⚠ **`0,63 ms/seg` est une MOYENNE** (`Bp_total/d_total`) et le
   dossier l'utilisait comme un coût marginal : la régression sur la capture console donne
   **`Bp` marginal = 0,37-0,44 ms/drawseg** et **frame marginale = 0,68-0,95 ms/drawseg**
   (4p : `MST = 112,7 + 0,680·d`, R²=0,42, n=31). N'utiliser 0,63 que pour un échange de
   carte en bloc. E1M1 est la **6e moins chère des 9 cartes shareware** ; warper le 4p sur
   **E1M8** est prédit à 161 → 115-128 ms (6,2 → 7,8-8,7 fps) pour **un drapeau de build
   existant et zéro ligne**. ⚠ le simulateur de drawsegs hors-ligne (ratio 9/27) n'est PAS
   vérifié — la sonde Ymir de comptage (§7.5-1) le valide ou le tue avant toute citation de fps.
   Plafond à dire à voix haute : **92-111 ms des 161 ne dépendent pas de `d`**, donc tout le
   levier WAD empilé et parfait plafonne vers 9-11 fps en 4p.
   ⚠ Trois sous-transformations du brief sont **MORTES, mesurées** : fusion de linedefs
   colinéaires (+0,0 à +1,5 % seulement — l'excès des cartes id est la DÉCOUPE BSP, pas
   l'auteur), suppression des linedefs décoratifs deux-faces (`r_bsp.c:410-421` retourne
   AVANT tout clip : ils émettent déjà ZÉRO drawseg), réduction du nombre de secteurs
   (`P` est plat à 14,3-15,3 ms sur les quatre modes pendant que `d` bouge ×3,8).
3. **`-nomonsters` par défaut en DM 4p** — T=12,8 ms et `mo` tombent ; c'est un réglage,
   pas du code.
4. ~~**Le dicing de sols hors-ligne** (§2.3) — la passe SlaveDriver.~~ **🔴 RAYÉ — NO-GO
   chiffré, §7.2-G.** Le bake enlève le SCAN (~0,5 ms, une CONSTANTE) alors que le coût
   mesuré est **~0,48 ms PAR TUILE ÉMISE** : il n'enlève rien de la pente. Et le prix visé
   n'est pas un prix 4p — le vrai fill logiciel des plans, recalculé **par frame** (et non
   comme une différence de médianes), vaut **8,6 ms en 1p et 3,4 ms en 4p**.
5. **Échelle de rotations DRP** — **la moitié normative de cette ligne était INVERSÉE,
   §7.2-H.** Vérifié en parsant le conteneur livré `cd/data/DOOMRP.DRP` : **28 cartes sur 32
   en 8 rotations pleines, 4 en 4 rotations** (MAP20/21/31/32), aucune en 2 ni en 1. Sur les
   quatre IWADs : **8 cartes sur 132**, toujours d'exactement UN cran. Et le joueur sans
   cartouche est le **BÉNÉFICIAIRE** de l'échelle, pas sa victime : 8 rotations mettent 5
   lumps par frame tournée dans l'ensemble de travail au lieu de 3, soit ~+130 lectures CD de
   première vue par carte, sur la machine précisément dépourvue de cartouche pour les cacher.
   Le vrai levier est un **critère glouton par sprite** dans `repack_wad.py` (§7.3). ⚠ défaut
   latent à corriger quoi qu'il arrive : `Makefile:244` ne passe pas `--rot-level` alors que
   `repack_wad.py:818` défaute à 8 — `make repack` sur TNT/Plutonia livre silencieusement
   quatre cartes qui ne peuvent plus se stager en cartouche.
6. Le ciel : les cartes extérieures coûtent le ciel logiciel en split → réglé par §2.4,
   pas par le WAD.

---

## 4. Les deux tests que tu demandes — oui, et voici les protocoles

**FOV 65° sur toggle : OUI.** `focalx` par mode derrière un accord de pad (à choisir
contre `TOGGLE_AUDIT.md`), défaut 90° inchangé, légende ATLAS dans la même session.
Falsifieur **sur Ymir, valide (comptage)** : même point, même carte, 4p — lire `d`
(row 2) à 90° puis 65°. Prédiction **d(65)/d(90) = 0,70-0,75** ; > 0,85 = le levier
meurt avant la console. Identité : à 90° le build doit être bit-identique. Les ms
(−9 à −15 prédits) restent console-only. ⚠ le ciel HW split est calé sur la géométrie
90° : le toggle doit soit mettre à l'échelle la loi de scroll, soit couper le ciel HW
tant qu'il est à 65° (version test).

**Ciel 4 quadrants : OUI, testable en deux temps.** Temps 1 (Ymir, valide) : géométrie,
fenêtres, raccords de bandes, lacets par joueur, comptage de slots de cycles. Temps 2
(console, obligatoire) : la neige. Build derrière un toggle, défaut off jusqu'à la
validation console.

---

## 5. LE PLAN

**Phase A — instruments : ✅ LIVRÉE 2026-08-25, un build, pool 30,17 → 28,41 Ko.**
1. ✅ brackets `hd`/`tl` dans `Bp` (les 11,2 ms sans nom) + largeur fixe row 4 + retrait `wp`
2. ✅ rows 14/16 en somme de frame (le patron row 2)
3. ✅ ciel logiciel **en ms par vue** (row 12 `SKY`, moitié cart) — un compte de pixels ne pouvait
   pas répondre : `R_DrawSkyColumn` fait un memcpy de 128 o par colonne, le facteur px→ms varie ×2-4
4. ✅ row `V1` dégatée en split → **row 6** (préalable de things→VDP1)
5. ✅ sonde de pic `openings` (row 11 `o`)
6. ✅ fix overlay ×4 (rows 5/20 sur `last` + le `snprintf` mort du build shippé)

À vérifier EN PREMIER à la prochaine session : **`hd+pr+lp+tl == Bp`**. Si l'identité ne tient pas,
rien d'autre sur la row 4 n'est lisible. Détail et pièges de lecture : `HEADROOM_2026-08-25.md` §9.

**Phase B — leviers sous toggle (Ymir décide la géométrie, la console décide les ms) :**
7. **FOV 65°** (falsifieur `d`)
8. **ciel HW 4 quadrants** (+ réduction cuite = candidat à tuer ZOOM_HALF)
9. cache du masque `slScrAutoDisp` (comptage Ymir : le masque change-t-il jamais ?)
10. kick VDP1 avancé — variante (A) seule (identité Ymir ; (B) retirée : le fence
    expire déjà, w7)
11. gouverneur re-ciblé (booléen Ymir : row 21 bouge-t-elle enfin ?)
12. ~~clock de `fl`/row 15~~ — **RETIRÉ** : `fl` était mal lu, la valeur est 0,0-0,3 ms
    et l'expérience n'a plus d'objet (§0.8, §7.1).

**Phase C — ta session console :** mesurer B (ms), neige du ciel, `w` watchdog après
(A), lire les nouveaux instruments, et **jouer la pièce cartouche** (`DOOM1C.WAD`, un
build).

**Phase D — structurel (après lecture de C) :**
13. gc-sections + réclamations pool (pré-vol + boot Ymir, risque boot documenté)
14. étude **SlaveDriver source** → passe de dicing hors-ligne (le chantier d'automne)
15. **séquenceur MUS v2 sur 68K** + SFX résidents RAM son (le chantier SON)
16. nodebuilder dans build.ps1 (seul, baselines resettées)

**Deux décisions qui n'appartiennent qu'à toi :**
- **La dette de tics** : 4p à 89 % de la vitesse réelle. Dépenser les gains en fps ou
  restaurer le 35 Hz (+2,6 à +7,7 ms) ?
- **Le FOV comme gameplay** : moins de vision périphérique en versus. (C'est un cadran
  par mode — 90° en 1p/2p, 65° en 3/4p est ma recommandation.)

---

## 6. Provenance

Console : `docs/captures/2026-08-25_console_overlay.csv`. Mémoire/carte :
`build/Mimas-Doom1s.map` + `nm`. Manuels : `../saturn-refs/manuals/`,
`../saturn-refs/knowledge/HW_VDP1.md`/`HW_VDP2.md`. Web (2026-08-25) :
[AVR](https://github.com/Jollyrogerxp/AVR) (SCSP-DSP géométrie, 5 flux @ 44,1 kHz),
[SlaveDriver-Engine](https://github.com/Lobotomy-Software/SlaveDriver-Engine) (GPL3,
août 2025), [Time Extension](https://www.timeextension.com/news/2025/08/the-source-code-for-the-engine-that-powered-the-sega-saturn-fps-powerslave-has-been-released).


---

# 7. RETOUR DES HUIT ÉTUDES (2026-08-25, 9 agents, 0 erreur)

Huit études lancées sur les questions ouvertes du dossier, plus **une passe adverse** qui
a re-désassemblé l'ELF livré, re-dérivé chaque régression depuis
`docs/captures/2026-08-25_console_overlay.csv`, parsé `cd/data/DOOMRP.DRP` et les annuaires
WAD directement, et relu les lignes de source citées. **Six des huit finissent en NO-GO sur
leur question de tête, et les six sont justes.** Le classement complet est en §7.3.

**Discipline tenue** : la passe adverse confirme qu'**aucune milliseconde issue d'Ymir
n'apparaît nulle part** dans les huit études. Chaque ms est console-CSV ou dérivée de doc,
et étiquetée.

## 7.1 Corrections à CE dossier et à ses voisins

| où | ce qui était écrit | ce qui est vrai |
|---|---|---|
| §2.5 tableau, ligne DSP | « prouvé capable de géométrie (AVR) mais pas de client » | Sous-estime la puce ET repose sur un exemple. L'enveloppe réelle est en §7.2-A. La porte est fermée par **1,55 % de multiplications contre 51 % de mouvements mémoire**, pas par l'absence de client. |
| §2.5 tableau, ligne 68K | « il ne voit pas la carte ni les mobjs » | Vrai mais accessoire. Le 68K est **atteignable à 4,1 cycles/octet**. Les trois lois qui le tuent sont en §7.2-B. « 68K inatteignable » à corriger partout. |
| §2.5, jeu SFX | ~470 Ko de RAM son libres, « précharger TOUS les SFX du shareware » | **Deux études ont compté le jeu DS\* sur le MAUVAIS WAD** — `cd/data/DOOM1.WAD` portait le TNT aplati d'un build parallèle en cours de session (21 469 600 o, 3378 lumps). Le vrai shareware (`wads_temoins/Doom1s.wad`) = **55 lumps DS\*, 535 127 o** contre **524 032 o** de RAM son utilisable : ça manque de **~11 Ko**, pas d'un facteur 2,4. Le levier ADPCM est donc **plus fort**, pas plus faible. |
| §3 item 2 | `Bp` ∝ drawsegs à **0,63 ms/seg** | 0,63 est une **MOYENNE**. Le coût **marginal** d'un drawseg de plus est **0,37-0,44 ms** dans `Bp` et **0,68-0,95 ms** sur la frame. |
| §3 item 4 | dicing hors-ligne « étude d'abord » | **RAYÉ**, §7.2-G. |
| §3 item 5 | « les joueurs sans cartouche paient » | **INVERSÉ**, §7.2-H. |
| §2.3 / Phase D item 14 | le dicing comme candidat autonome | **Rayer.** Ce que lire SlaveDriver a vraiment appris est en §7.2-G, et ce n'est pas le dicing. |
| `docs/HEADROOM_2026-08-25.md:183` | pool = `__heap_end − _end` = 30 896 o | La carte livrée (`build/Mimas-Doom1s.map`) donne `0x060f2e60 … 0x060fa000` = **29 088 o = 28,41 Ko**. Toute figure « pool 30,9 Ko » en aval est optimiste de 1,8 Ko. |
| `HEADROOM §4`, ligne de récupération | « panneaux HUD split → LWRAM **+16 400** » | **FAUX — le filtre du linker.** `hud2p_panel` (10 240 o) et `hud4p_panel` (2 560 o) sont `static const` → `.rodata` → ils **partent dans l'image** : une copie runtime vers LWRAM ne libère **zéro** octet de pool. Seul `hud2p_flash_lut` (3 584 o, vrai `.bss`) est relocalisable. La ligne vaut **+3 584 o**. C'est la SEULE violation du filtre linker dans tout le lot — et elle vient du doc de base, pas des études. |
| dossier §0 / `SPR fl` | « 10 ms de fill sprite logiciel en 3p » | **FAUX, et c'était une erreur de LECTURE de ma part** : le format est `"SPR fl%d.%d …"`, `fl` est le **nom du champ** et son `l` se lit `1` à l'écran. Sur 13 captures / 2 WADs / 4 modes le fill sprite logiciel vaut **0,0 à 0,3 ms** — les things roulent déjà sur le VDP1. Il n'y a rien à y déplacer. |

## 7.2 Tes huit questions, une réponse chacune

### A — « Le DSP SCSP ne sait faire que de la géométrie ? On ne peut rien y déporter ? »

**Tu as raison sur la prémisse, et la correction rend la puce MOINS utile, pas plus.**
AVR était un exemple. L'enveloppe, établie sur source (`saturn-refs/scspadpcm`,
`saturn-refs/Azel/…/scspdsp.c`, les PDF SEGA) :

- **128 instructions × 64 bits, exécutées TOUTES, inconditionnellement, une fois par
  échantillon 44,1 kHz.** Pas de branchement, pas de PC, pas d'appel, pas de boucle, pas de
  comparaison, pas de diviseur, pas de racine.
- Un MAC par pas : `ACC(26b) = (X(24b) × Y(13b) >> 12) + B(26b)`. **5 644 800 pas/s, plafond dur.**
- Mémoire : **la RAM son et rien d'autre** — fenêtre de 128 Ko. C'est précisément ce qui lui
  fait échapper à l'interdiction bus-B qui a tué le SCU-DSP.
- Seule opération data-dépendante : `ADRL` charge un `ADRS_REG` 12 bits depuis
  l'accumulateur → **une vraie table-gather de 4096 entrées** (abs, signe, clamp, réciproque,
  racine, une comparaison rendue en 0/1). ~4 pas par lookup, **un seul** `ADRS_REG`.
- **Précision, le point décisif** : X fait 24 bits mais Y seulement **13**. Un `FixedMul`
  Doom est un 32×32→64 : il **ne tient pas en un pas**, il en coûte ~4 → **1,41 M FixedMul/s
  contre 5,2 M/s pour le SH-2 maître. Le DSP vaut 0,27× UN SH-2 maître à la précision Doom.**
  AVR ne s'en sort qu'en Q1.15, et son propre README en donne la conséquence : son monde
  entier fait ~256 unités de large. Une carte Doom en fait 32 768.
- **Plancher de latence ~250 µs** (constante propre d'AVR : 10 échantillons = 227 µs, plus
  l'attente de phase). C'est 7 150 cycles SH-2 = ~1 400 FixedMul que le maître aurait faits
  en attendant. **Filtre primaire : tout lot de moins de ~1 400 MAC est une perte garantie,
  à coût de transport nul.**

**Et le chemin de retour est LÉGAL** — contrairement au SCU-DSP. RAM son → HWRAM par SCU-DMA
est le quadrant permis (vérifié contre `HW_MEMORY_AND_BUS §5.2`). La porte est ouverte ; il
n'y a rien à faire passer dedans.

**Pourquoi rien** : j'ai re-désassemblé l'ELF livré. **3 598 instructions de classe
multiplication sur 231 756 = 1,55 %**, contre **51 % de `mov`** et 20,8 % de branchements.
Par fonction : `R_RenderSegLoop` **0,71 %**, `R_StoreWallRange` 1,05 %, `P_MobjThinker`
**zéro multiplication et 42 % de branchements**. Un déport parfait, gratuit, à latence nulle
et sans perte de précision, de **toutes** les multiplications d'une frame 4p vaut **≤ 7,1 ms
sur 161**, et **≤ 0,96 ms des 45,8 ms de `Bp`**. On ne déporte pas un défaut de cache sur un
DSP qui ne sait ni brancher, ni diviser, ni suivre un pointeur.

**Le seul client réel est la COMPRESSION** : décodage ADPCM côté DSP pour rendre le jeu SFX
résident en RAM son (recette celeriyacon, 8 voies, 1,5/2,5/4,5 bits). Avec le chiffre
shareware corrigé (535 127 o contre 524 032 utilisables), le 4,5 bits (÷1,78 → ~300 Ko)
rend la chose confortable, et le 2,5 bits fait rentrer même Doom II. **0,00 ms maître**, et
ça attaque le hitch mesuré (`precache-streaming-verdict` : « le vrai hitch = SON »). Coût :
une **réécriture du boîtier son**, à cadrer comme UN projet avec MUS v2 — le décodeur veut
les 32 slots, MUS en tient 15.

### B — « Une partie cloisonnée et déterministe du raisonnement sur le 68K ? Un tableau et les calculs ? »

**Atteignable oui ; rentable non ; prouvablement déterministe non, sans reconstruire la
barrière qu'on essaie justement de supprimer.** Trois nombres, et je les écris comme des
lois pour qu'on ne re-litige plus :

> **Loi 1 — l'affranchissement.** Expédier un octet vers/depuis la RAM son coûte au maître
> **4,1 cycles (142 ns)** pendant qu'il cale sur MCRDYN. Une décision Doom par-mobj ou
> par-ligne coûte **0,5-3 cycles maître par octet de la ligne qu'elle lit.**
> **La décision est moins chère que l'expédition de la ligne.**
>
> **Loi 2 — la réponse ne doit pas revenir.** Le seul travail qui échappe à la loi 1 est
> celui dont la sortie s'écrit dans les registres de slot du SCSP — dans l'espace d'adressage
> du 68K, port de retour nul. **Cet ensemble est exactement « le travail dont la sortie est
> du son », et ce n'est pas une coïncidence.**
>
> **Loi 3 — le plafond.** Tout le domaine tic vaut **11 ms d'une frame 4p de 158**
> (console : `T` médian 11,0 ; `th` 8 ; `mo` 6 ; n=259). Un 68K magique, instantané, à
> transport nul, absorbant TOUT le domaine tic, plafonne à **+7 à +9 %** (6,21 → 6,71 fps).
> Et il est inatteignable de toute façon : les thinkers **mutent** la carte, le blockmap et la zone.

Le meilleur candidat de toute la famille (table du joueur le plus proche sur 259 mobjs) nette
**+0,10 ms**. Et le déterminisme prouvable exige une relecture **inconditionnelle** = le
maître cale sur un 68K en retard = **une deuxième barrière de présentation**, dans une frame
qui porte déjà 19 ms de la première et dont le watchdog split expire déjà.

**Ce qu'il faut retenir de ton instinct : il est DÉJÀ shippé.** La table cloisonnée
déterministe existe, c'est le **lump REJECT**, et `core/p_setup.c` porte la mesure console qui
a fait passer le sight de 13-21 ms à 1,2-4,9. C'est pour ça que le champ sight lit 0 sur 73
frames sur 106 aujourd'hui.

### C — « Qu'est-ce qu'on gagne à passer le séquenceur MUS sur le 68K ? »

**Zéro milliseconde, et ~4 des ~43 points de pourcentage de musique que ce port jette.**
`S` lit **0 ms sur 106 frames console sur 106** — vérifié colonne par colonne, et
`core/d_main.c:749-756` confirme que le bracket contient bien `S_UpdateSounds`, donc
`I_UpdateSound`, donc `mus_step`. **Il n'y a pas de temps maître à récupérer.**

Ce que le 68K achèterait vraiment, c'est un tempo indépendant du fps : à 6,2 fps l'horloge
MUS 140 Hz avance de **~22 ticks en une seule rafale par frame** — la musique ne joue pas
« légèrement quantifiée », elle joue **par paquets à 6 Hz**.

**Mais appeler `mus_step` depuis le handler vblank qu'on exécute DÉJÀ chaque champ fait passer
la période de service de 161 ms à 16,7 ms, pour ~15 lignes et 8 octets.** Et si les 16,7 ms
de gigue résiduelle finissent par compter, le **timer propre du SCSP** peut interrompre le
SH-2 à exactement 140 Hz, 68K toujours halté.

En face, la route 68K coûte : un convertisseur MUS→SEQ, un constructeur de banque de timbres
dont le format d'octets ne survit que sous forme de figures dans un PDF, 44 Ko de réservation
système en RAM son, un chemin d'init SRL qui `new[]` **26 610 o dans un pool de 29 088**, et
un conflit non documenté entre l'allocation dynamique de voix du driver et nos SFX à slot
direct. **NO-GO comme geste de perf.**

### D — « La musique via `-Mus` est horrible. C'est tout ce que peut faire cette puce ? »

**Absolument pas, et l'écart n'est pas subtil : tout le budget de timbre de ce port fait
96 OCTETS.** Trois formes d'onde mono-cycle de 32 échantillons en 8 bits (vérifié :
`WAVE_N 32`, `N_WAVES 3`), mappées sur les patches par un test de classe à trois branches,
avec une enveloppe en porte fixe (AR=31, pas de decay, pas de forme de sustain, un release),
**pas de pan, pas de LFO, pas d'envoi d'effet** — contre une puce à 32 slots, 32 enveloppes
matérielles à quatre segments, 32 LFO, FM arbitraire, et un DSP 128 pas portant la
bibliothèque de réverb de SEGA elle-même.

**Mais le plus gros défaut n'est pas le timbre, c'est qu'un tiers de la musique ne joue
jamais.** Le canal MUS 15 (**percussions**) est sauté dans les deux bras, note-on ET note-off
(`i_sound_saturn.cxx:408` et `:413`). Sa part de tous les note-ons, remesurée en marchant
chaque lump MUS : **34,6 % sur le shareware, 38,9 % sur Doom II, 34,3 % sur le WAD
actuellement dans `cd/data`** — le résultat survit donc à l'échange de WAD qui a pollué les
autres études son.

Ajoute la monophonie un-slot-par-canal (les accords sont écrasés dans la même rafale ;
polyphonie de crête mesurée à **15-23 notes**, ce qui tient dans 24 slots sans vol) et tu as
la réponse, dans l'ordre de livraison :

1. **Route V — `mus_step` sur l'ISR vblank.** ~15 lignes + un garde de ré-entrance. 161 → 16,7 ms. **0 pool, 0 RAM son, 0 disque.** À faire en premier, et à te remettre en main : *« est-ce que le rythme bave encore ? »* Si non, la variante timer SCSP est fermée définitivement.
2. **Route P — percussions + pool de 24 voix + vraie ADSR + pan (contrôleur MUS 4).** ~300-400 lignes, `i_sound_saturn.cxx` seul. **C'est là qu'est la musique.**
3. **Route S — banque d'échantillons GM-ish par carte** (~100-150 Ko en RAM son, montée au chargement). Le précédent de SEGA lui-même, `HERBTONE.BIN`, fait 117 Ko **pour un seul morceau**. Demande mesurée : 51 programmes GM distincts.
4. **Route D — réverb DSP** — ⚠ voir le conflit §7.4-3 : elle n'est PAS gratuite.

⚠ **Partitionner la RAM son AVANT** : le cache SFX est un allocateur à bump **sans éviction**,
et une allocation ratée est définitive pour la session.

### E — « Le CDDA est toujours bugué (boot en 8 minutes) »

**GO-APRÈS-SONDE, et la sonde ne coûte AUCUN changement de code.** Des trois suspects de
2026-07-02 : **#1 est réel et structurel**, #2 est réel mais en est la **conséquence**, #3
n'est réel que pour un `build.ps1 -Cdda` nu.

**Cause racine (dérivée de la source) : Mimas initialise le système de fichiers AVANT le bloc
CD, ce qui est à l'envers.** `srl_core.hpp:89` exécute `GFS_Init` ; `srl_sound.hpp:52`
exécute `CDC_CdInit` (commande bloc-CD 0x04) bien plus tard. **Les deux références qui
shippent font l'inverse** et sont sur disque : `wolf4sdl-saturn/saturn.cpp:977-993` est
littéralement `CDC_CdInit(0,0,5,0x0f);` … `GFS_Init(...)`, et Z-Treme re-roote GFS
immédiatement après son CdInit. La commande 0x04 **remet à zéro les filtres du bloc CD** — et
ces filtres sont précisément ce que `GFS_Init` avait configuré et ne rétablit jamais.
⚠ `SAT_DEFER_SOUND_INIT` n'a pas corrigé ça : il a déplacé le CdInit de « après GFS_Init » à
« après GFS_Init **et** un chargement de WAD de 2-4 Mo », c'est-à-dire **plus profond dans la
zone dangereuse**.

**Suspect #4, NOUVEAU et nulle part enregistré** : `Cdda::Resume()` appelle `CDC_TgetToc`,
le hang documenté d'~10 minutes — même classe de défaut, **à une pause de distance**.
Correctif : une ligne, `PlaySingle(cdda_track, true)` au lieu de `Resume()`.

**Sonde (à faire en premier, zéro ligne)** : builder l'image CDDA **documentée**
(`docs/IMAGES.md:90-93` — un `-Cdda` nu livre un disque à 2 pistes alors que le jeu demande
la piste 15, ce qui empoisonnerait la lecture), booter, et regarder lequel des **six marqueurs
printf déjà existants** reste le dernier affiché : `SND0` / `SND1` / `SND2 → Hardware::Initialize` /
`I_InitSound: CDDA` / `CDDAMAP.TXT: N override(s)` / `I_InitMusic: CDDA`. **Une seule
exécution tue deux suspects sur trois.** C'est de la reproduction de bug et la phase est un
booléen : **Ymir est explicitement valide ici.**
Ensuite seulement, le correctif : **~4 lignes de patch** dans
`patches/saturnringlib.patch`, 2 fichiers, 0 octet, 0 étape de build.

⚠ **Mine adjacente à vérifier dans la même pré-vol** : `Hardware::Initialize` fait un `new[]`
transitoire de **26 610 o** (SDDRVS.TSK) dans ce même pool de 29 088 — il reste **~2,4 Ko**,
soit **sous le plancher de boot-loop** tant que l'allocation est tenue. Toute croissance de
pool d'ici un test CDDA peut rendre cette allocation nulle, `GFS_Load` écrira alors à
l'adresse 0, **et ça ressemblera à un nouveau bug CD.**

### F — « Sélection/écriture de cartes pour le split : a-t-on chiffré ? »

**Oui, et c'est le n°1 du classement** — le seul levier des huit études qui attaque
**`d` (les drawsegs)**, c'est-à-dire le terme que la loi du split dit gonflant avec le nombre
de joueurs. Détail et corrections en §3 item 2 (réécrit). L'essentiel :

| | `d` | `Bp` | `MST` | fps |
|---|---|---|---|---|
| **4p E1M1 aujourd'hui** (mesuré console) | 73 | 44,5 ms | 161 ms | 6,2 |
| **4p E1M8** (dérivé, ratio non vérifié) | 24 | 23,1 → 13,8 | 128 → 115 | **7,8-8,7** |
| 4p E1M2 (le mauvais choix, pour l'échelle) | 95 | 54,0-58,1 | 176-182 | 5,5-5,7 |

**Coût : 0 ligne, 0 octet, un drapeau qui existe déjà** (`-WarpMap "1 8"`, + le `-Repack`
obligatoire). ⚠ Le simulateur de drawsegs hors-ligne n'est pas vérifié, et **E1M8 est la carte
la plus exposée à son propre biais « portes lues fermées »** (son gimmick est un sol qui
descend). **La sonde de comptage Ymir (§7.5-1) passe avant toute citation de fps.**

Vérifié au passage, sur données : le départ joueur-1 d'E1M1 est **(1056, −3616)** — exactement
le locator `MX` de la capture, donc la capture EST bien E1M1 shareware.

### G — « Le dicing de sols hors-ligne (SlaveDriver) est-il réel ? »

**NO-GO, et la raison est une PENTE, pas une constante.** Le bake enlève le **scan** — ~0,5 ms,
une constante — alors que le coût mesuré de la chose est **~0,48 ms par tuile émise**, et le
dicing n'en enlève rien : le test de région interdite est **écran, donc dépendant de la vue**,
et le test de budget de plot, le fetch de slot, le choix d'ombrage et la rasterisation de
punch restent tous par-tuile, par-frame.

Trois autres le tuent indépendamment :
- **3 slots de texture de 4 Ko contre 4-12 flats simultanément visibles**, et comme les
  données de motif VDP1 **n'ont pas de registre de stride**, chaque cellule partielle
  horizontalement clippée exige **son propre** motif uploadé — or le WAD dit que les
  partielles dominent les entières **de 2 à 3,5 contre 1**, parce que le sol de secteur Doom
  médian fait 1,5-3,4 cellules de 64×64.
- **Banc de 256 commandes, plafond mur 248, ~64,5 µs d'émission maître par commande** →
  seuil de rentabilité à **149 commandes en 1p**, alors que couvrir une vue Doom en demande **150-500**.
- **Le prix n'est pas un prix 4p.** Recalculé **par frame** (et non comme une différence de
  médianes — 13 frames sur 105 sont même **négatives**, donc le bracket n'est pas fiablement
  imbriqué) : le vrai fill logiciel des plans vaut **8,6 ms en 1p et 3,4 ms en 4p**. C'est la
  loi du split dans sa forme la plus tranchante : la catégorie que le dicing attaque est celle
  qui compte le MOINS à 4 joueurs.

**Ce que lire SlaveDriver a vraiment appris — et ce n'est pas le dicing** : PowerSlave
s'offre **59 slots de texture et un banc de 1540 commandes dans les mêmes 512 Ko** parce que
**toute** surface du jeu est une tuile 64×64 uniforme pré-découpée, murs et sols confondus.
Mimas dépense 368 Ko pour 26 slots parce que les textures de mur Doom sont de taille variable.
**C'est là toute la différence de budget, et c'est une réécriture de renderer.** Second
enseignement : PowerSlave a un **far clip à 1024 unités** que Doom n'a pas — ce qui rejoint
exactement la transformation d'écriture n°1 de l'étude F.

### H — « L'échelle de rotations DRP : que gagnent les joueurs sans cartouche ? »

**Rien, parce que la prémisse est inversée** — et c'est vérifié sur le **conteneur livré**,
pas sur la prose. Parsing de `cd/data/DOOMRP.DRP` : magic DRP1, 3378 lumps, 32 cartes,
**28 en L8 et 4 en L4** (MAP20 3825,3 Ko / MAP21 3696,9 / MAP31 3662,2 / MAP32 3372,6),
**aucune en L2 ni L1**. Sur les quatre IWADs : **8 cartes sur 132**, toujours d'exactement un
cran. La perte visible : les quatre vues de trois-quarts de 16 classes de monstres à moins de
768 unités — les vues gardées sont **exactement aux angles vanilla**, les joueurs sont
exemptés partout, et le LOD de distance sert déjà le face-seul au-delà de 768.

La moitié **factuelle** de la ligne du dossier est vraie : l'échelle est armée **sans** test
de cartouche (`sat_sprite_rotlevel` posé à `w_drp_saturn.cxx:565-583`, alors que
`drp_stage_to_cart` — qui, lui, teste `sat_cart_usable` — tourne plus tard à `:606`). Mais la
moitié **normative** est à l'envers : **le joueur sans cartouche est le bénéficiaire.**

**Le levier vivant est meilleur que n'importe quel conditionnel** : un critère **glouton
par sprite, le plus lourd d'abord**, dans `tools/repack_wad.py` — il restitue les 8 rotations
pleines à **10-14 des 16 classes** sur ces quatre cartes (toute la bestiaire de corps-à-corps :
POSS, TROO, SPOS, SARG, CPOS, HEAD, PAIN, SKUL) et le paie avec les diagonales du seul
Mastermind ou Cyberdemon qu'on affronte à travers une arène — **à taille de blob identique,
staging cartouche identique, comportement CD identique.** ~40 lignes d'outil.
**Mais demande-toi d'abord si le pli est seulement visible** (§7.5-4).

## 7.3 Classement

| # | proposition | gain | coût | confiance |
|---|---|---|---|---|
| 1 | **E1M8 comme carte de référence 4p** (`-WarpMap "1 8" -Repack`) | 161 → 115-128 ms, 6,2 → 7,8-8,7 fps | **0 ligne, 0 octet**, 1 drapeau existant | pente VÉRIFIÉE, magnitude NON |
| 2 | **Percussions + pool de 24 voix + ADSR + pan** | 0,00 ms — **34-39 % des note-ons sont aujourd'hui JETÉS** | ~350 l. dans un seul fichier ; ~200 o `.bss` | HAUTE sur les percussions |
| 3 | **`mus_step` sur l'ISR vblank** | 0,00 ms — service 161 → 16,7 ms | **~15 lignes, 8 octets** | HAUTE |
| 4 | **Réordonner `CDC_CdInit` avant `GFS_Init`** | boot CDDA ~480 s → ~0 s | ~4 lignes de patch | MOYENNE-HAUTE (sonde d'abord) |
| 5 | **Critère glouton par sprite (DRP)** | 0 ms, 0 octet disque net ; 8 rotations rendues à 10-14 classes | ~60 lignes | HAUTE sur les faits, MOYENNE sur l'utilité |
| 6 | **`cdda_ResumeSong` → `PlaySingle`** | supprime un second stall ~10 min atteignable | **1 ligne** | HAUTE sur la chaîne d'appel |
| 7 | **`Makefile:244` → `--rot-level=auto`** | corrige un défaut latent LIVE | **1 ligne** | HAUTE |
| 8 | **Nodebuilder moderne** | −1,8 à −4,0 ms en 4p | 1 étape de build, 0 ligne runtime | MOYENNE — **seul, et après E1M8** |
| 9 | **ADPCM DSP → SFX résidents** | 0,00 ms ; attaque le hitch SON mesuré | **réécriture du boîtier son**, des semaines | MOYENNE-HAUTE sur l'arithmétique |
| 10 | **Pack 4p sous règle de 1024 unités** | −29 à −41 ms de plus | projet de design DM | MOYENNE — justification = équilibre, pas fps |
| 11 | **Banque d'échantillons par carte** | 0,00 ms ; timbre ×1200 | ~550 l. ; 100-150 Ko RAM son | MOYENNE — **conflit §7.4-1** |
| 12 | **`hud2p_flash_lut` (3 584 o) → LWRAM** | +12,3 % de pool | ~5 lignes | HAUTE — vers **LWRAM**, jamais VDP1 |

## 7.4 Sept collisions réelles

1. **RAM son (512 Ko) — QUATRE propositions, UNE banque.** ADPCM résident ~300 Ko + tout le
   DSP + les 32 slots ; banque GM 100-150 Ko ; ring de réverb 16-128 Ko ; MUS-v2-sur-68K
   35-85 Ko + le 68K en marche. **Une seule peut shipper sans un partitionnement conjoint
   conçu d'abord.** Et une fenêtre DSP à la AVR fait **128 Ko**, en conflit direct avec la
   résidence SFX.
2. **Slots SCSP — l'ADPCM et le pool de 24 voix sont MUTUELLEMENT EXCLUSIFS tels qu'écrits.**
   Aujourd'hui : SFX 0-7, synthé MUS 8-22. Le pool réclame 8-31 ; le décodeur ADPCM réclame
   **les 32**. Celui qui est planifié en premier doit porter la carte des slots des deux.
3. **Cycles mémoire du DSP — la réverb n'est PAS gratuite.** La RAM son a 126 cycles mémoire
   utilisables par échantillon de 22,68 µs ; le DSP en prend jusqu'à **64**. Activer
   **n'importe quel** programme DSP multiplie le transport hôte vers la RAM son **par ~5**
   (142 → 709 ns/octet) et affame tout ce qui est en dessous — dont l'upload de la banque GM
   et le décodage ADPCM. **Le dossier listait la réverb SCSP et le schéma DSP/68K comme des GO
   indépendants. Ils ne le sont pas.**
4. **VDP1 VRAM — deux usages veulent les MÊMES 12 288 octets.** Le seul usage survivant
   (relocaliser un banc de commandes pour passer 256 → 384) et `FVDP1_BASE` (libre uniquement
   parce que `SAT_VDP1_FLOORS` vaut 0) sont **le même run**. Les deux sont refusés, donc le
   conflit dort. ⚠ Séparément : le run de 5 120 o à `0x25C7BC00` est **1p-seulement** (la pile
   de panneaux HUD 2p atteint `0x25C7D000`) — tout ce qu'on y pose doit être conscient du mode.
   C'est exactement la classe de bug qui a mis des monstres dans le HUD le 2026-08-19.
5. **Artefacts de build et baselines.** Le warp E1M8, le nodebuilder et le DRP glouton
   changent tous les trois ce qui atterrit dans `cd/data`, forcent tous les trois un `-Repack`
   de ~4 min, et invalident tous les trois **chaque nombre par-carte que le projet possède**.
   En poser deux dans un seul build rend l'A/B console illisible. **Ordre : warp E1M8 seul →
   vidéo console → nodebuilder seul → DRP glouton seul.** ⚠ Le pack de cartes ET le conteneur
   DRP touchent tous deux le défaut d'alignement de `tools/merge_wad.py` (il écrit les lumps
   bout à bout alors que `strip_wad.py` padde délibérément sur 4, parce qu'une lecture 32 bits
   non alignée sur le SH-2 big-endian renvoie du bruit) — **corriger ça UNE fois, avant les deux.**
6. **L'overlay 40 colonnes — trois revendications sur la ligne 18, plus une ligne 12 jamais
   lue.** La sonde MEM veut la ligne 18 (aujourd'hui calculée dans un buffer mort à
   `dg_saturn.cxx:3712` — la **9e** ligne morte de ce type) ; les routes V et P veulent une
   ligne de compteurs et ne nomment aucune ligne ; la lecture décisive de l'étude G est la
   ligne 12 `SKY`, shippée le 2026-08-25 **et jamais lue** (⚠ elle n'existe que sur un build
   cartouche — voir §7.6). La ligne 18 n'en tient **qu'une**.
7. **Octets de pool — petits séparément, à pré-voler ENSEMBLE.** Route P (~200 o `.bss` +
   jusqu'à 2 Ko `.rodata`), DRP (138 o), sonde MEM (~210 o), blob de réverb (~1,3 Ko) facturent
   **le même** pool de 29 088 o, contre un plancher de boot-loop documenté à 4 800 o. Ensemble
   ~4 Ko : survivable, **mais seulement si `build/Mimas.map` est vérifié une fois pour le build
   combiné**, pas quatre fois pour quatre builds. Et ajouter à cette pré-vol le `new[]` de
   26 610 o de `Hardware::Initialize` (§7.2-E).

> **Non-conflit, et c'est le fait d'ordonnancement le plus fort de la revue** : l'étude F
> (drawsegs) et les études son ne partagent **rien** — ni banque, ni bus, ni étape de build,
> ni ligne d'overlay. Elles peuvent être menées en parallèle.

## 7.5 Les cinq expériences décisives

Quatre des cinq sont **légales sur Ymir** (comptes, identités, reproduction de bug) ; seule
la moitié « millisecondes » de la n°1 exige la console.

1. **Sélection E1M8 — sonde de COMPTAGE Ymir, et elle conditionne une table de 132 cartes.**
   Builder trois disques (`"1 8"`, `"1 2"`, `"1 1"`), armer 4 joueurs, les garer **sur** leurs
   départs DM, ne pas bouger, lire la ligne-2 `d`.
   **PRÉDICTION** : `d(E1M8)/d(E1M1) = 0,33 ± 0,08` et `d(E1M2)/d(E1M1) = 1,30 ± 0,15`.
   **CRITÈRE DE MORT** : un ratio hors bande ⇒ le simulateur hors-ligne n'est pas un oracle de
   classement, la table des 132 cartes est nulle, **personne n'ouvre un éditeur.**
   *Identité gratuite dans la même session* : au départ joueur-1 d'E1M1 (1056, −3616), face à
   `a64`, une vue de 160 de large doit lire `d` dans la bande **40-70**.
   **Puis, et seulement si les ratios passent** : UNE vidéo console 4p de 60 s sur E1M8.
   Cette vidéo vaut plus que le choix de carte — elle donne au projet **les ms d'une DEUXIÈME
   carte** et dit enfin si 0,63/0,44/0,95 ms-par-drawseg sont des constantes ou du trivia E1M1.
2. **Percussions + pool de voix — comptes Ymir, et la ms est prouvée nulle d'avance.**
   Instrumenter (a) le pic de slots musicaux simultanés, (b) les note-ons/s par canal-15 vs
   mélodique. `S = 0 ms sur 106/106` ⇒ le maître **ne peut** ni perdre ni gagner de temps ici.
3. **`mus_step` sur vblank — IDENTITÉ Ymir, puis UNE question au propriétaire.**
   Prédiction : `ticks == 140 × secondes ± 1`, **indépendamment du fps**. C'est un `X == Y`,
   exactement ce pour quoi Ymir est fait. Puis, per `ask-before-instrumenting-observables` :
   te remettre le build de 15 lignes et demander *« le rythme bave-t-il encore ? »*
4. **DRP glouton — PUREMENT SUR DONNÉES, aucun build.** `repack_wad.py --report` avec une
   coupe gloutonne « le plus lourd d'abord » ; l'arithmétique dit déjà oui avec marge sur les
   quatre cartes TNT. **Puis le test décisif n'est pas une sonde, c'est une question** : booter
   le disque TNT, warper MAP20, tourner autour d'un Revenant à ~300 unités, comparer à MAP19
   (qui shippe en L8). *« Est-ce que le facing claque en 4 crans de façon perceptible ? »*
   Si non, **60 lignes ne sont jamais écrites.**
5. **Réordonnancement `CDC_CdInit` — REPRO DE BUG YMIR, ZÉRO LIGNE, À FAIRE EN PREMIER.**
   Protocole complet en §7.2-E. Une exécution tue deux suspects sur trois.

## 7.6 Le tableau des morts — avec la LOI qui tue

*La loi est ce qui empêche la re-proposition ; le nombre est ce qui rend la loi crédible.*

| levier | LOI |
|---|---|
| **DSP SCSP comme co-processeur de calcul** (toute forme) | *Le binaire livré fait 1,55 % d'instructions de multiplication contre 51 % de mouvements mémoire ; la boucle la plus chaude du jeu en fait 0,71 %. On ne déporte pas un défaut de cache sur un DSP qui ne sait ni brancher, ni diviser, ni suivre un pointeur, qui ne voit que la RAM son, et qui ne répond pas avant 250 µs.* ⚠ **Porte NON fermée à consigner** : contrairement au SCU-DSP, la relecture EST légale. Il n'y a simplement rien qui vaille le voyage. |
| **68EC000 comme hôte d'une tranche de LOGIQUE de jeu** | *L'affranchissement vers la RAM son coûte au maître ~4,1 cycles/octet ; une décision Doom coûte 0,5-3 cycles/octet de la ligne qu'elle lit. La décision est moins chère que l'expédition de la ligne.* Plus : le domaine tic entier vaut 11 ms sur 158. Plus : le déterminisme prouvable exige une **seconde barrière de présentation**. |
| **Séquenceur MUS sur 68K comme geste de PERF** | *`S` = 0 ms sur 106 frames console sur 106. Il n'y a pas de temps à déplacer.* |
| **Données non-VDP1 en VRAM VDP1** | *28-33 Ko de DRAM bus-B NON CACHÉE qui arbitre contre le moteur de dessin, contre 609-624 Ko de LWRAM CACHÉE mesurée libre dans les quatre modes à 2,1× HWRAM. Un rapport de capacité de 21:1 combiné à un désavantage de vitesse n'est renversable par aucune expérience.* Et le **linker** ferme avant le bus : l'image est un `.bin` plat en HWRAM, donc toute table initialisée ship dedans et une copie runtime libère **zéro** pool. ⚠ Sur la contention le manuel est sans ambiguïté et donne la mauvaise réponse : **les DEUX calent** — le CPU gagne l'arbitrage mais mange les waits, et « le dessin est interrompu et doit attendre ». |
| **Dicing hors-ligne des sols (route SlaveDriver)** | *Le scan est la CONSTANTE de ~0,5 ms ; le coût mesuré est ~0,48 ms PAR TUILE ÉMISE. Le bake enlève la constante et rien de la pente.* |
| **Build nu sans cartouche avec l'échelle de rotations coupée** | *Le joueur sans cartouche est le BÉNÉFICIAIRE de l'échelle : L8 met 5 lumps par frame tournée dans l'ensemble de travail au lieu de 3 — ~+130 lectures CD de première vue par carte — sur la machine précisément dépourvue de cartouche pour les cacher.* Et la portée est de 6 % : 8 cartes sur 132. |
| Fusion de linedefs colinéaires même-texture | *L'excès des cartes id est la DÉCOUPE BSP, pas la fragmentation d'auteur : +0,0 à +1,5 % au-dessus d'un nodebuilder.* |
| Suppression des linedefs décoratifs deux-faces | *`core/r_bsp.c:410-421` retourne AVANT tout appel de clip : ils émettent déjà ZÉRO drawseg ; 0-4 sont même VISITÉS par vue.* |
| Réduction du nombre de secteurs | *`P` est plat à 14,3-15,3 ms sur les quatre modes pendant que `d` bouge ×3,8 : c'est fill-bound et le plafond de visplanes est froid.* |
| DMA interne du SCSP comme moteur HWRAM→RAM son | *Le manuel dit qu'il ne transfère qu'entre les registres de contrôle du SCSP et la mémoire son, 3 812 o max. Il n'existe aucun moteur sur cette machine qui remplisse la RAM son sans que le SH-2 cale par mot de 16 bits.* |
| Stores 32 bits dans le chemin d'upload SFX | *Le port SCSP est 16 bits ; un longword est décomposé en deux cycles de bus. Mêmes cycles mémoire, même coût.* Contrairement à la VRAM VDP2, **pas de gain d'élargissement ici.** |
| Musique pré-rendue résidente en boucle | *LSA/LEA sont des offsets 16 bits depuis SA : 65 536 échantillons max par slot = **5,9 s** à 11 kHz. La route n'existe pas.* |
| Atlas de textures VDP1 dans les trous | *Les données de motif VDP1 n'ont pas de registre de stride ; un sous-rectangle est inadressable et cisaille.* Déjà réglé ; redit parce que ça contraint ce que les trous peuvent contenir. |

## 7.7 Deux pistes sourcées que les études ont laissées ouvertes

1. **Le manuel VDP2 bénit explicitement un banc de VRAM VDP2 comme « RAM de travail
   auxiliaire »** — et **256 Ko sont inertes en 3/4p**. L'étude D l'a sourcé puis écarté sur
   le même argument LWRAM (609-624 Ko libres, cachée, plus rapide). ⚠ Écarté par **domination**,
   pas par impossibilité : si LWRAM devenait un jour contrainte, cette porte est ouverte et
   documentée.
2. **`TABLE mode` du DSP SCSP ignore `DEC` entièrement.** Un programme n'utilisant que des
   tables fixes n'a besoin **ni** de compensation `DEC`, **ni** du 68K, **ni** du Timer B : le
   SH-2 se synchronise sur l'échantillon en sondant `SCIPD` bit 0x400, ou simplement à
   l'horloge murale (N échantillons = N × 22,68 µs exactement). Toute la machinerie
   `DEC`/68K/Timer d'AVR n'existe que parce qu'AVR utilise le mode ring. À consigner : si le
   DSP est jamais rouvert, **la moitié de la complexité d'AVR est évitable.**
