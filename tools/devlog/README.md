# devlog/ — video tooling

Ported from the Tethys ep6 cut (2026-08-20), where the whole chain was proven on
a 5:58 1080p60 master. Method and recipes: [`../../docs/DEVLOG_VIDEO.md`](../../docs/DEVLOG_VIDEO.md).

| file | what |
|---|---|
| `devlog_style.py` | palette + font resolution, shared by both scripts |
| `mkcard.py` | full-screen 1920x1080 cards (title / comparison table / list / credits). `python mkcard.py --demo out/` renders one of each |
| `mkthumb.py` | 1280x720 side-by-side "versus" YouTube thumbnail |
| `annot_template.ass` | the on-screen annotation style, with the colour twins |

Needs `pillow` and `ffmpeg` on PATH. The scripts look for `consola.ttf` /
`consolab.ttf` next to themselves first, then in `C:\Windows\Fonts`.
