# Devlog video — method and recipes

Ported from the Tethys devlog ep6 cut (2026-08-20). Every command below actually
ran; the numbers are what came out. Tools: [`tools/devlog/`](../tools/devlog/).

Evidence tags as elsewhere: `[HW]` hardware, `[Ymir]` emulator, `[src]` read at
source, `[est]` estimate.

## 0. What you need

- **A hardware capture with the debug overlay on.** It is not illustration, it
  is the episode's data: the Tethys frame-rate dossier was read entirely off one
  3 min 43 capture and replaced a hardware round-trip. Shoot the same route
  every time so two builds are comparable.
- **A second capture of the same route with clean audio** if the overlay build
  and the shipping build differ.
- **A reference capture** (PSX, PC, a previous build) if the episode compares.

## 1. Cut structure that worked

| | |
|---|---|
| 0:00 | title card, 10 s, over a darkened gameplay frame |
| 0:10 | the run, with annotations |
| mid | side-by-side comparison block (§4), 20 s |
| | comparison result card, 22 s |
| | back into the run, annotations to the end |
| | "what's left" card |
| | teaser card + teaser clip |
| | credits card |

Build each piece as its own `.mp4` at the final format, measure each one, then
concat. Do **not** trust nominal durations — see gotcha 4.

## 2. Cards

`python tools/devlog/mkcard.py --demo out/` renders one of each to look at.
Hold a PNG for its duration:

```
ffmpeg -loop 1 -i card.png -t 22 -r 60 -c:v libx264 -preset medium -crf 18 \
       -pix_fmt yuv420p -y segN.mp4
```

Cards are static. `zoompan` for a slow push cost **17.8 MB and minutes of
encode for a ten-second card**; the same card as a still is **923 KB** and
encodes instantly. Motion on a card buys nothing at 1080p60.

## 3. Annotations

Copy `tools/devlog/annot_template.ass`, one Dialogue block per screen, burned in
at the very end (§6). Rules that were paid for:

- **A text lasts its whole screen.** Anything shorter reads as a mistake.
- Heading amber, body bone, a win green, a problem/debt/joke rust.
- Colours in ASS are `&H00BBGGRR` — **blue first**.
- Never hand-write an `.ass` through a shell heredoc with apostrophes in it;
  write the file with an editor/tool instead (gotcha 1).

## 4. The side-by-side comparison block

The most useful single recipe here. Both machines play the **same** section with
a live clock on each; the shorter side freezes and says so while the longer side
runs on. Anchor the two starts on the same in-game event, not on a timestamp.

```
ffmpeg -ss 33.0 -i REF.mp4 -ss 90.0 -t 20.0 -i OURS.mp4 \
  -f lavfi -t 20.0 -i "color=c=0x0B0906:s=1920x1080" -filter_complex \
  "[0:v]trim=0:13,setpts=PTS-STARTPTS,scale=944:531:flags=lanczos,setsar=1,fps=60,
   tpad=stop_mode=clone:stop_duration=8,trim=0:20,setpts=PTS-STARTPTS[l];
   [1:v]scale=944:531:flags=lanczos,setsar=1,fps=60[r];
   [2:v][l]overlay=8:170[x];[x][r]overlay=968:170,
   drawbox=x=7:y=169:w=946:h=533:color=0x4A4030@1:t=2,
   drawbox=x=967:y=169:w=946:h=533:color=0x4A4030@1:t=2,
   ...clocks at y=716, 'finished' at y=782 enable='gte(t,13)'[v]" ...
```

Clock text, live, one per side — the left one clamped so it stops with the
freeze:

```
text='%{eif\:trunc(min(t\,13))\:d}.%{eif\:trunc(mod(min(t\,13)*10\,10))\:d} s'
text='%{eif\:trunc(t)\:d}.%{eif\:trunc(mod(t*10\,10))\:d} s'
```

- `tpad=stop_mode=clone` freezes the shorter side instead of ending the stream.
- Put the caption **under** the clocks, e.g. `of our 20.0 s, 4.3 s is black
  between rooms` — the honest framing of a loss.

## 5. Where the gaps are, measured

Do not eyeball loading. `blackdetect` gives the number:

```
ffmpeg -i capture.mp4 -vf "blackdetect=d=0.30:pix_th=0.06" -an -f null - 2>&1 \
  | grep -o "black_duration:[0-9.]*"
```

On the Tethys capture: **22 gaps, 40.82 s total, mean 1.86 s, 17.7 % of a
230.7 s run** `[HW]`. That one command produced the episode's strongest claim.

## 6. Assembly and burn

Concat with the demuxer, **Windows-style paths in the list file**:

```
# list.txt
file 'C:/Users/.../segA.mp4'
file 'C:/Users/.../c1.mp4'

ffmpeg -f concat -safe 0 -i list.txt -c copy -y picture-cut.mp4
```

Then one final pass: burn the subtitles, normalise, faststart.

```
ffmpeg -i picture-cut.mp4 -vf "subtitles=annot.ass" \
  -r 60 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p \
  -profile:v high -level 4.1 \
  -af "volume=2.5dB,aresample=192000,alimiter=limit=0.75:attack=4:release=60:level=disabled,aresample=48000" \
  -c:a aac -b:a 192k -ar 48000 -ac 2 -movflags +faststart -y MASTER.mp4
```

Verify, do not assume:

```
ffmpeg -i MASTER.mp4 -af ebur128=peak=true -f null -   # want ~-15 LUFS, TP < -1 dBFS
```

Tethys ep6 came out at **-15.4 LUFS, true peak -1.9 dBFS**, 5:57.8, 94 MB.

## 7. Thumbnail

```
python tools/devlog/mkthumb.py left.png right.png thumb.png \
  --left-year 1994 --right-year 2026 --left-cx 450 --right-cx 450 \
  --caption "SAME LEVEL  -  REAL HARDWARE  -  DEVLOG #3"
```

Pick the frame pair by measurement, not by taste: sample both captures every
0.5 s, score each frame (mean luma, or a colour mask for the player sprite), and
take the pair where **both** sides are lit and show the same thing. If the two
runs diverge, half the thumbnail ends up empty.

Both sides get the **same** lift. Lifting one more than the other turns a
comparison into a lie — and note that a softer reference capture flatters our
side for free, which is worth saying out loud rather than exploiting.

Always look at the `_small.png` (320×180). That is the size that decides.

## 8. YouTube

Title: `<Game> on Sega Saturn: <two concrete gains> (Devlog #N)`.

Description: **no hard line wraps** — YouTube reflows and your wrapping becomes
ragged. One long line per paragraph and per bullet. Structure that worked:
one-line project header · what changed since last time · the engine/SDK sentence
plus "captured on real hardware, no emulator" · `In this episode:` bullets with
the numbers · project boilerplate · `With help from:` · fan-project disclaimer ·
hashtags · support links · `Chapters:` (first must be `00:00`, minimum 10 s
apart).

## 9. The SegaXtreme post

Same window as the video, or say so in one line if it is wider. Open with the
standing disclaimer ("feel free to correct me…"). Then `── SECTION ──` headers,
and:

- **Cite registers and file:line, not page numbers.** `CCCTL (1800ECH) bit 8` is
  checkable; "page 241" depends on which scan they have.
- **Check every hardware claim against `../saturn-refs/manuals/`** (the
  Kronos-corrected VDP1/VDP2 scans) before writing it. That audience checks.
- Keep a **"two things I got wrong"** section with numbers. On that forum it is
  the part that earns trust, and it is what gets replied to.
- Keep a **"still broken, named"** list. Naming an open defect invites help;
  hiding it invites a comment.

## 10. Gotchas, each one paid for

1. **Bash heredocs and apostrophes.** Writing `.ass`/Markdown with prose
   apostrophes through a shell heredoc fails with `recursive escaping after \c
   not allowed` or `unexpected EOF`. Write those files with an editor tool. This
   bit twice, including once while writing this file.
2. **`alimiter` raises loudness by default.** Auto-makeup gain took a master
   from −16.3 to −13.5 LUFS with the peak still at +0.2 dBFS. Pass
   `level=disabled`, and `aresample=192000` around it for true-peak work.
3. **The concat demuxer cannot open MSYS `/c/Users/...` paths.** Write
   `C:/Users/...` into the list file.
4. **Frame rounding drifts on concat.** Pieces came out up to +0.29 s long.
   Measure each piece with `ffprobe` after building it and offset the annotation
   timings per piece, or the text slides off its shot by the end.
5. **Check the backdrop frame for the build's own artefacts.** The first title
   card used a frame that contained the white corruption the episode was about.
6. **PowerShell 5.1: never `2>&1 | Select-Object` a build.** It wraps native
   stderr in ErrorRecord and fails a build that succeeded. Capture with
   `*> log` and filter when reading.
7. **Measure colours on the raw source, not on your own re-encode**, before
   publishing a claim about them.
8. **Render the frame and look at it before delivering.** Three passes shipped
   as "verified" came back broken on Tethys; an invariant that holds is not the
   same as a result that is right.
9. **No colon inside a `drawtext` text.** Single quotes do *not* protect `:` at
   the filtergraph level — it ends the option and the whole graph fails to parse
   (`No option name near …`). Escaping as `\:` works; writing the caption without
   a colon works better. Mimas devlog #1, the caption under the comparison block.
10. **Gotcha 5 has teeth — check the backdrop frame, do not assume.** The first
   backdrop picked for devlog #1 was a budget-collapse frame full of red and blue
   untextured quads: the title card would have shipped the exact artefact the
   episode was about. Sample ten candidate frames, look at them, then choose.
11. **A gameplay clip cut from 0 often starts on a menu.** Devlog #1's split-screen
   piece opened on the skill-select screen with "two players, one Saturn" burned
   over it. Sample the first 12 s of every source before choosing `-ss`.
12. **Magnifying a debug-overlay row: measure the cell, then check the bottom
   rows.** On a 720×480 capture of a 320×224 picture a text cell is 18 px wide
   and 17.14 px tall, so row *n* starts at `y = round(17.143*(n-1))`. The last
   row lands **on the Doom status bar** (`y = 411`) and magnifies into an
   unreadable mess — pick the row above it. Recipe: crop from the *source*,
   `scale` ×3–5 with `flags=neighbor` onto a dark plate, and leave a `drawbox`
   around the row it came from (`x *= 2.667`, `y *= 2.25` for a 1080p master).
13. **A comparison block can drift off its subject.** Both sides must show the
   same thing for the *whole* block: check the last frame, not just the first.
   Devlog #1's first cut spent its final third comparing a lit room against an
   unlit wall because the right-hand run turned around.

### 14. `drawtext` veut du RGB, l'ASS veut du BGR — et les deux se ressemblent

`devlog_style.py` porte les couleurs en **RGB** (tuples PIL) et
`annot_template.ass` leur jumeau **`&H00BBGGRR`**. Coller la valeur ASS dans un
`drawtext=fontcolor=` donne une couleur *plausible mais fausse* : le rouille
`&H004F6AC8` sort **bleu**, l'ambre `&H002F94DA` sort bleu aussi. Rien ne casse,
rien n'alerte — la carte est juste dans la mauvaise palette, et ça se voit
seulement quand on la met à côté d'une carte PIL.

Table de conversion, à copier :

| | RGB (PIL) | `drawtext` | ASS |
|---|---|---|---|
| AMBER | `(218,148,47)` | `0xDA942F` | `&H002F94DA` |
| BONE | `(237,228,211)` | `0xEDE4D3` | `&H00D3E4ED` |
| GREEN | `(94,174,136)` | `0x5EAE88` | `&H0088AE5E` |
| RUST | `(200,106,79)` | `0xC86A4F` | `&H004F6AC8` |
| GREY | `(163,155,140)` | `0xA39B8C` | `&H008C9BA3` |
| INK | `(11,9,6)` | `0x0B0906` | `&H0006090B` |

Le contrôle qui l'attrape : rendre **une** carte PIL et **un** plan `drawtext`
côte à côte avant de fabriquer les vingt suivants.
