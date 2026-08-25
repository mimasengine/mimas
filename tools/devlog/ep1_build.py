"""Assemble devlog episode 1 -- the SIX-BLOCK cut (v5, 2026-08-24).

    python ep1_build.py            # pieces, voice, subtitles, mix, master
    python ep1_build.py pieces     # just re-cut the pieces
    python ep1_build.py post       # voice + subtitles + mix + master (pieces kept)
    python ep1_build.py loud       # measure the master, then level it once

Structure, and why it is this one.  The first three cuts were slide decks: cards
carried the story, footage illustrated nothing.  The owner's order for this one,
fixed 2026-08-24, is a six-block arc, and block 3 follows a dramatic order that
was argued for explicitly:

    1  title over gameplay                              24 s
    2  clean Doom 1 shareware, uncut, eight lower-thirds  2:06
    3  THE PROBLEM -- symptom, failed corrections, the
       clue, three frames, the layer cut, the patch       2:40
    4  July against August -- freeze, side by side, PIP    0:51
    5  THE SOLUTION -- fafling, the field, CEF, the two
       words, the sequence, what it opens                 2:21
    6  split-screen payoff, TNT, the two diagrams, credits 2:20

STYLE RULE, absolute: NO PRONOUNS on screen.  The mechanism is the subject.  The
spoken voice says "I" -- it is a personal account -- but nothing written does.

Sources, and the owner's limits on them (do not widen):
    185508  the first 20 s are OFF LIMITS
    185618  never past 10 s
    Doom - 2 Player - nodebug   the game starts at 8.6 s (before that: skill menu)
    184951  the first 24 s are the title screen and the skill menu under a FULL
            debug overlay -- unusable, which is why D1a starts at 24
    185408  same trap, found at QC: the first ~20 s are the skill menu under the
            full red overlay.  Everything from 20 s is a berserk melee, so the
            whole clip is red-tinted -- fine as footage, wrong under a diagram.
    185147  opens on the E1M1 intermission screen and does not reach E1M2
            gameplay until ~6.5 s.  That is why the E1M2 lower-third sits at
            block-relative 71 s and not at the cut.
    EXCLUDED entirely: Doom TNT - 4 Player, Doom-Probe-*, PLAYTHROUGH* (Tethys)

Two deviations from the written plan, both deliberate, both flagged here:

  * BLOCK 1 uses 185046 @0-24 rather than @12-36, and block 2 then starts that
    clip at 24.  The plan's window would have shown the same 12 s twice, a minute
    apart.  This way no shot repeats inside the episode.
  * BLOCK 6's diagrams are NOT a 45 %-width side panel.  D2 and D3 are landscape
    by construction -- D3's zone bar is 1660 px wide and carries eight numbered
    runs -- and squeezing them into a portrait panel makes them unreadable.  They
    are cropped to their own content and laid over the RUNNING game at 55 %
    brightness instead.  The complaint the panel was answering was "no more black
    screens in this block", and that is satisfied: the game never stops.

Colour discipline (DEVLOG_VIDEO.md gotcha 14): drawtext wants RGB 0xRRGGBB, ASS
wants BGR &H00BBGGRR.  Mixing them fails silently -- rust renders blue.
"""
import os
import subprocess
import sys

DL = "C:/Users/pcico/Downloads"
VD = "C:/Users/pcico/Videos"
WK = f"{VD}/mimas-devlog1"
OUT = f"{WK}/ep1"
HERE = os.path.dirname(os.path.abspath(__file__))

CARDS = f"{WK}/cards5"
DIAG = f"{WK}/diag2"
C1 = f"{WK}/c1"
FR = f"{WK}/frames/all-2201"
VO = f"{WK}/vo"

J2201 = f"{VD}/2026-07-20 11-22-01.mp4"
J2613 = f"{VD}/2026-07-20 11-26-13.mp4"
D1A = f"{DL}/GenkiArcade-20260820-184951.mp4"      # E1M1 first room, from 24 s
D1B = f"{DL}/GenkiArcade-20260820-185046.mp4"      # E1M1 fight / acid / last room
D1C = f"{DL}/GenkiArcade-20260820-185147.mp4"      # E1M2
T1A = f"{DL}/GenkiArcade-20260820-185408.mp4"      # TNT MAP01, door + melee
T1B = f"{DL}/GenkiArcade-20260820-185508.mp4"      # TNT MAP01 + hardware sky
SP1 = f"{DL}/Doom - 2 Player - nodebug.mp4"
SP2 = f"{DL}/Doom - Base - notext1.mp4"   # 53.6 s, the only wholly unused capture
SP3 = f"{DL}/Doom - Base - notext2.mp4"
MUS1 = f"{DL}/Grave Loop Signals.wav"              # darker  -> blocks 1-4
MUS2 = f"{DL}/Grave Loop Signals (1).wav"          # lighter -> blocks 5-6

V_INT = ["-c:v", "libx264", "-preset", "veryfast", "-crf", "16",
         "-pix_fmt", "yuv420p", "-r", "60"]
A_INT = ["-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2"]

# 720x480 SAR 32:27 -> DAR 16:9; neighbour, because the picture is 320x224.
FV_AUG = "fps=60,scale=1920:1080:flags=neighbor,setsar=1"
# July is 1080p30 OBS, pillarboxed at 1716x1008+88+38.
FV_JUL = "fps=60,crop=1716:1008:88:38,scale=1920:1080:flags=neighbor,setsar=1"
# the C1/C4/C5 stills are already that 1716x1008 crop.
FV_STILL = "scale=1920:1080:flags=neighbor,setsar=1"

INK = "0x0B0906"
RUST = "0xC86A4F"
GREY = "0xA39B8C"       # for marking something that is NOT the subject

# block 4a.  ep1_b4_freeze.py picked t3.767 for having the largest late component
# on the owner's three hand-picked July frames -- and then picked the WRONG
# COMPONENT inside it: a 460 px blob at native x74..102 y12..49 that is not a
# sync hole at all.  The owner painted the real ones in frames/t03.767_green.png
# (and marked a separate, since-fixed sloped-window height bug in _yellow.png,
# which the automatic pass was drifting towards).  A late-layer difference finds
# everything that MOVED between two frames; only the person who knows the
# renderer can say which of those is the artefact this episode is about.
#
# Recovered from that painted mask by connected components, padded 8 px and
# mapped from the 1716x1008 capture crop into the 1920x1080 still:
B4_T = 3.767
B4_BOXES = [                        # (x, y, w, h, delay, colour, thickness)
    (0, 47, 182, 782, 1.00, RUST, 5),     # the left edge -- a whole wall column gone
    (988, 258, 190, 199, 1.35, RUST, 5),  # right of centre, the largest interior hole
    (721, 266, 95, 120, 1.70, RUST, 5),   # and the small one beside it
    # The owner's _yellow.png marks a band the eye finds on its own once the
    # frame is held for seven seconds: a DIFFERENT bug -- the VDP2 plane window
    # height, fixed since -- and leaving it unmarked invites the audience to
    # count it as a sync hole.  Grey and thin, so it reads as "not this", and it
    # arrives last, after the three that are the subject.
    (0, 611, 1920, 90, 3.30, GREY, 3),
]


def sh(*a, **kw):
    subprocess.run(a, check=True, **kw)


def dur(path):
    return float(subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True, check=True).stdout.strip())


def content_box(png):
    """Bounding box of everything that is not the page black, for the panels."""
    import numpy as np
    w, h = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", png],
        capture_output=True, text=True, check=True).stdout.strip().split(",")
    w, h = int(w), int(h)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", png, "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE).stdout
    a = np.frombuffer(raw, np.uint8).reshape(h, w, 3).astype(int)
    m = (np.abs(a - np.array([11, 9, 6])).sum(axis=2) > 24)
    ys, xs = np.nonzero(m)
    return int(xs.min()), int(ys.min()), int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)


# ---------------------------------------------------------------------------
# piece builders
# ---------------------------------------------------------------------------
def clip(name, src, ss, length, fv, extra=""):
    """A slice of a capture, its own audio kept for the texture bed."""
    vf = fv + ("," + extra if extra else "")
    fo = max(0.0, length - 0.25)
    sh("ffmpeg", "-v", "error", "-y", "-ss", str(ss), "-t", str(length), "-i", src,
       "-vf", vf,
       "-af", f"afade=t=in:st=0:d=0.25,afade=t=out:st={fo}:d=0.25,aresample=48000",
       *V_INT, *A_INT, "-shortest", f"{OUT}/{name}.mp4")


# Block 5 is 2:21 of static cards on page black, which is the flattest stretch
# of the episode.  Setting this true runs a muted capture underneath them at 90 %
# card opacity: measured legible (the text does not lose contrast), and enough
# motion to stop the block reading as a slide deck.  The bed carries NO date
# stamp -- at that opacity it is texture, not evidence, and stamping it would
# invite the viewer to read a shot nobody is being asked to look at.
B5_OVER_GAME = True


# SP2 carries the full debug overlay and the skill menu before 0:35 -- the owner
# named 35 s as the clean entry.  A -ss on a stream_loop only steers the FIRST
# pass, so the clean window is cut out once into its own file and THAT is looped;
# otherwise every card past the eighteenth second ghosts a debug overlay through
# itself, which is the one thing this episode spends ten minutes not doing.
# Bed choice, scored rather than picked (three candidate windows, one sample per
# second -- left-half motion, right-half motion, and a red-pixel count across the
# top row for burned-in HUD messages):
#
#   window                 left move   right move   HUD-message frames
#   SP1  33-60 s (27.0)      20.1         4.2         1 / 54      <- chosen
#   SP2  35-53 s (18.4)      28.5         1.1         5 / 18
#   SP3  20-40 s (20.0)      12.3         8.6         6 / 19
#
# The message column is NOT a reason to prefer one of these, and it was wrong of
# an earlier revision to treat it as one: SP2's "GOD MODE" is test footage, and
# the HUD says so anyway.  Kept in the table because it is measured, and because
# the number itself needs a caveat -- the first scan reported 0/54 for SP1 by
# sampling once a second AND skipping its own first frame, so it missed
# "PICKED UP THE ARMOR." in second one.  A test that skips its first sample
# reports the absence of what it never looked at.
#
# The reason SP1 is here is loop length, and it is a small one: 27 s repeats 3.9
# times across the block's 104 s of half-screen cards, against 5.7 for SP2's
# 18.4 s, and with player 1 now playing at full brightness on the left, a
# repeat is visible in a way it never was under a full-width card.  SP2 has the
# livelier left half (28.5 against 20.1) and the stiller right half, so this is
# a trade, not an upgrade.  Switching back is these two lines.  No footage is
# re-used either way: p39 takes SP1 8.6-32.6, and this starts at 33.
B5_BED = SP1
B5_BED_IN, B5_BED_LEN = 33.0, 27.0


def make_bed():
    """Cut the clean window out of SP2 once, so every loop of it is clean."""
    sh("ffmpeg", "-v", "error", "-y", "-ss", str(B5_BED_IN), "-t", str(B5_BED_LEN),
       "-i", B5_BED, "-vf", FV_AUG, "-an", *V_INT, f"{OUT}/_bed.mp4")


def half_card(png):
    """Re-lay a full-width card onto the RIGHT half of the frame.

    The bed is a vertical 2-player split: player 1 moves on the left, player 2
    stands still on the right.  A full-screen card over that hides the only
    moving thing on screen for two minutes.  Cropping the card to its own ink and
    dropping it on the right half leaves player 1 running at full brightness and
    costs a scale factor -- measured per card, 0.83 in the worst case (C12, ink
    1084 px wide), so body text lands at 22-27 px instead of 26-32.

    Only the TEXT cards fit this: the D4 diagrams are 1666-1718 px of ink by
    construction and would land at 0.54, which is not readable at all.  They stay
    full-width, and they are 37 s of the block, not 104.
    """
    from PIL import Image
    os.makedirs(f"{OUT}/half", exist_ok=True)
    dst = f"{OUT}/half/{os.path.basename(png)}"
    im = Image.open(png).convert("RGBA")
    px, W0, H0 = im.load(), im.width, im.height
    x0, y0, x1, y1 = W0, H0, 0, 0
    for y in range(0, H0, 2):
        for x in range(0, W0, 2):
            r, g, b, _a = px[x, y]
            if r + g + b > 90:
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
    PAD = 24
    x0, y0 = max(0, x0 - PAD), max(0, y0 - PAD)
    x1, y1 = min(W0, x1 + PAD), min(H0, y1 + PAD)
    ink = im.crop((x0, y0, x1, y1))
    k = min(900.0 / ink.width, 1000.0 / ink.height, 1.0)
    ink = ink.resize((int(ink.width * k), int(ink.height * k)), Image.LANCZOS)
    out = Image.new("RGBA", (960, 1080), (11, 9, 6, 255))
    out.paste(ink, ((960 - ink.width) // 2, (1080 - ink.height) // 2), ink)
    out.save(dst)
    return dst


def bed(name, png, length, offset, half=False):
    """A card over the muted bed.  See B5_OVER_GAME and half_card()."""
    src = f"{OUT}/_bed.mp4"
    if half:
        # no global eq: the left half is the point, and it plays at full
        # brightness.  The card's own plate is what darkens the right half.
        fg = (f"[0:v]{FV_AUG}[bg];"
              "[1:v]format=rgba,colorchannelmixer=aa=0.90[t];[bg][t]overlay=960:0[v]")
        png = half_card(png)
    else:
        fg = (f"[0:v]{FV_AUG},eq=brightness=-0.30:saturation=0.55[bg];"
              "[1:v]format=rgba,colorchannelmixer=aa=0.90[t];[bg][t]overlay=0:0[v]")
    sh("ffmpeg", "-v", "error", "-y", "-stream_loop", "-1",
       "-ss", "%.3f" % (offset % B5_BED_LEN), "-t", str(length), "-i", src,
       "-loop", "1", "-i", png,
       "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
       "-filter_complex", fg, "-map", "[v]", "-map", "2:a",
       "-t", str(length), *V_INT, *A_INT, f"{OUT}/{name}.mp4")


def hold(name, png, length, fv=None):
    """A still, silent.  Cards and diagrams are already 1920x1080."""
    vf = (fv or "scale=1920:1080:flags=neighbor") + ",setsar=1"
    sh("ffmpeg", "-v", "error", "-y", "-loop", "1", "-i", png,
       "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(length),
       "-vf", vf, *V_INT, *A_INT, f"{OUT}/{name}.mp4")


def title(name, src, ss, length, png):
    """Block 1: the shot dimmed under the title, then brought back up.

    The card is opaque page-black, so it is keyed out rather than re-rendered
    with an alpha channel -- nothing else in it is that colour.

    The card holds to 14.5 s, not 10.6: the third pass puts three spoken lines
    over this shot, and the last of them names the episode.  A title that leaves
    before the line that names it reads as a mistake."""
    fg = (f"[0:v]{FV_AUG},eq=brightness='-0.30*clip((14.8-t)/0.8,0,1)':eval=frame[bg];"
          f"[1:v]colorkey={INK}:0.30:0.12,format=yuva420p,"
          "fade=t=in:st=0.6:d=1.0:alpha=1,fade=t=out:st=13.0:d=1.5:alpha=1[t];"
          "[bg][t]overlay=0:0[v]")
    sh("ffmpeg", "-v", "error", "-y", "-ss", str(ss), "-t", str(length), "-i", src,
       "-loop", "1", "-i", png, "-filter_complex", fg, "-map", "[v]", "-map", "0:a",
       "-af", "volume=-6dB,afade=t=in:st=0:d=1.5,aresample=48000",
       *V_INT, *A_INT, "-t", str(length), f"{OUT}/{name}.mp4")


def three_frames(name):
    """C4: 1.567 / 1.600 / 1.633, fast three times, then slow, then held.

    The cadence is deterministic so the prose can be placed from the subtitle
    file; only the frame's own timecode is burned in, because that label has to
    be exact to the frame it names."""
    tags = ["01.567", "01.600", "01.633"]
    segs = [(t, 1.4) for _ in range(3) for t in tags]      # 12.6 s
    segs += [(t, 2.6) for t in tags]                       # 7.8 s
    segs += [("01.633", 5.6)]                              # 5.6 s
    parts = []
    fnt = "C\\:/Windows/Fonts/consola.ttf"
    for i, (tag, ln) in enumerate(segs):
        p = f"{OUT}/_c4_{i:02d}.mp4"
        dt = (f"drawtext=fontfile='{fnt}':text='{tag}':x=w-tw-70:y=64:"
              f"fontsize=44:fontcolor=0xEDE4D3@0.85:box=1:boxcolor=0x0B0906@0.6:boxborderw=12")
        sh("ffmpeg", "-v", "error", "-y", "-loop", "1", "-i", f"{FR}/t{tag}.png",
           "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(ln),
           "-vf", f"{FV_STILL},{dt}", *V_INT, *A_INT, p)
        parts.append(p)
    concat(parts, f"{OUT}/{name}.mp4")


def freeze(name):
    """Block 4a: run the July window at half speed, stop dead on the artefact."""
    a = f"{OUT}/_b4a.mp4"
    b = f"{OUT}/_b4b.mp4"
    c = f"{OUT}/_b4c.mp4"
    clip("_b4a", J2201, 2.90, 0.90, FV_JUL, "setpts=2.0*PTS")
    # drawbox takes a constant alpha, so the delay is done with enable= alone:
    # the frame lands first, the boxes arrive after it, on the same still.  They
    # come in one at a time, largest first, so the eye is walked across the
    # frame instead of being handed three rectangles at once.
    boxes = ",".join(
        f"drawbox=x={x}:y={y}:w={w}:h={h}:color={c}@0.95:t={t}:enable='gte(t,{d})'"
        for x, y, w, h, d, c, t in B4_BOXES)
    sh("ffmpeg", "-v", "error", "-y", "-loop", "1", "-i", f"{FR}/t03.767.png",
       "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", "7.5",
       "-vf", f"{FV_STILL},{boxes}", *V_INT, *A_INT, b)
    clip("_b4c", J2201, 3.80, 0.60, FV_JUL, "setpts=2.0*PTS")
    concat([a, b, c], f"{OUT}/{name}.mp4")


def face_off(name):
    """Block 4b: the same courtyard turn, July left, August right, at a quarter
    speed, played twice.  Measured, not chosen by eye: a pan detector put the
    only sustained interior turn in each capture at 11.1 s (July) and 37.6 s
    (August), and they land on the same room."""
    fnt = "C\\:/Windows/Fonts/consola.ttf"
    lab = ("drawtext=fontfile='%s':text='%s':x=(w-tw)/2:y=28:fontsize=40:"
           "fontcolor=%s:box=1:boxcolor=0x0B0906@0.72:boxborderw=16")
    fg = (
        f"[0:v]{FV_JUL},setpts=4.0*PTS,scale=960:1080:flags=neighbor,"
        f"{lab % (fnt, '20 JUL 2026   DESYNC', '0xC86A4F')}[l];"
        f"[1:v]{FV_AUG},setpts=4.0*PTS,scale=960:1080:flags=neighbor,"
        f"{lab % (fnt, '20 AUG 2026   MANUAL PRESENT', '0x5EAE88')}[r];"
        "[l][r]hstack=inputs=2,"
        f"drawbox=x=958:y=0:w=4:h=1080:color={RUST}@0.8:t=fill[v]")
    one = f"{OUT}/_b4d.mp4"
    sh("ffmpeg", "-v", "error", "-y",
       "-ss", "11.10", "-t", "2.64", "-i", J2201,
       "-ss", "37.60", "-t", "2.64", "-i", D1A,
       "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
       "-filter_complex", fg, "-map", "[v]", "-map", "2:a",
       "-t", "10.56", *V_INT, *A_INT, one)
    concat([one, one], f"{OUT}/{name}.mp4")


def pip(name, length):
    """Block 4c: August full frame, July inset top right with a rust border."""
    fg = (f"[0:v]{FV_AUG}[bg];"
          f"[1:v]{FV_JUL},scale=576:324:flags=neighbor,"
          f"pad=584:332:4:4:{RUST}[in];"
          "[bg][in]overlay=W-w-56:56[v]")
    # D1C from 12 s, not 0: the clip opens on the E1M1 intermission screen, and
    # a still map screen is the wrong thing to put a moving comparison against.
    sh("ffmpeg", "-v", "error", "-y",
       "-ss", "12", "-t", str(length), "-i", D1C,
       "-ss", "8", "-t", str(length), "-i", J2613,
       "-filter_complex", fg, "-map", "[v]", "-map", "0:a",
       "-af", "aresample=48000", "-t", str(length), *V_INT, *A_INT,
       f"{OUT}/{name}.mp4")


def panel(name, src, ss, length, png):
    """Block 6: a diagram over the RUNNING game, cropped to its own content.

    Scaling a landscape diagram into a 45 %-wide column would make D3's zone bar
    illegible; the game stays underneath at 55 % instead, so the block still has
    no dead screen in it."""
    x, y, w, h = content_box(png)
    # force_original_aspect_ratio, not a bare width.  scale=1780:-2 fits the
    # WIDTH and lets the height go where it likes: D5's content box is 1384x854,
    # narrower than the others, so widening it to 1780 pushed it to 1098 px tall
    # and the title and the last line were cut off by the frame.  Now the panel
    # fits inside 1780x1000 whichever way round it is.
    fg = (f"[0:v]{FV_AUG},eq=brightness=-0.22:saturation=0.72[bg];"
          f"[1:v]crop={w}:{h}:{x}:{y},"
          "scale=w=1780:h=1000:force_original_aspect_ratio=decrease:flags=lanczos,"
          f"pad=iw+64:ih+56:32:28:{INK}@0.86,format=rgba,"
          "colorchannelmixer=aa=0.94[p];"
          "[bg][p]overlay=(W-w)/2:(H-h)/2[v]")
    sh("ffmpeg", "-v", "error", "-y", "-ss", str(ss), "-t", str(length), "-i", src,
       "-loop", "1", "-i", png, "-filter_complex", fg, "-map", "[v]", "-map", "0:a",
       "-af", "volume=-4dB,aresample=48000", "-t", str(length),
       *V_INT, *A_INT, f"{OUT}/{name}.mp4")


def credits(name, src, ss, length, png):
    """The card at 92 %, not keyed out.

    Keying the page black out of the credits let the split-screen status bars
    show straight through the last two lines, which is where the fan-project
    disclaimer sits -- the one line that must be legible.  A near-opaque plate
    keeps the motion as a hint underneath and the text as text."""
    fg = (f"[0:v]{FV_AUG},eq=brightness=-0.46:saturation=0.5[bg];"
          "[1:v]format=rgba,colorchannelmixer=aa=0.92,"
          "fade=t=in:st=0.4:d=1.2:alpha=1[t];[bg][t]overlay=0:0[v]")
    sh("ffmpeg", "-v", "error", "-y", "-ss", str(ss), "-t", str(length), "-i", src,
       "-loop", "1", "-i", png, "-filter_complex", fg, "-map", "[v]", "-map", "0:a",
       "-af", f"volume=-8dB,afade=t=out:st={length - 3}:d=3,aresample=48000",
       "-t", str(length), *V_INT, *A_INT, f"{OUT}/{name}.mp4")


def concat(parts, out):
    lst = out + ".txt"
    with open(lst, "w", encoding="utf-8") as f:
        for p in parts:
            f.write("file '%s'\n" % p.replace("\\", "/"))
    sh("ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
       "-c", "copy", out)


# ---------------------------------------------------------------------------
# the plan: (piece, block, builder)
# ---------------------------------------------------------------------------
def build_pieces():
    # -- block 1: title over the E1M1 corridor, 24 s ------------------------
    title("p01", D1B, 0.0, 24.0, f"{CARDS}/TITLE.png")

    # -- block 2: the clean run, uncut, 2:06 --------------------------------
    clip("p02", D1A, 24.0, 30.29, FV_AUG)
    # D1B is trimmed to 29.70, not 35.07: its last 5.4 s are the E1M1 tally
    # screen already fully counted out -- a frozen picture in the one block
    # whose job is to show the machine running.  The FINISHED beat and the
    # count are kept (they are content); the freeze after them is not.
    clip("p03", D1B, 24.0, 29.70, FV_AUG)
    # D1C from 6.5, not 0: its first 6.5 s are the E1M1 tally screen, and D1B
    # already ends on one.  Back to back that is 14 s of static score card in
    # the middle of the block whose whole job is to show the machine running.
    clip("p04", D1C, 6.5, 53.96, FV_AUG)

    # -- block 3, C1: the symptom ------------------------------------------
    hold("p05", f"{FR}/t01.600.png", 5.0, FV_STILL)
    hold("p06", f"{C1}/T3-green.png", 15.0, FV_STILL)
    # C2: four corrections, struck one at a time.  The last stage holds twice as
    # long as the others: the punchline is on it, and a line is spoken over it.
    for i in range(5):
        hold("p%02d" % (7 + i), f"{CARDS}/C2_%d.png" % i, 8.0 if i == 4 else 4.0)
    # C3: the clue
    sh("ffmpeg", "-v", "error", "-y", "-i", f"{WK}/C3-still-vs-turning.mp4",
       "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-shortest",
       *V_INT, *A_INT, f"{OUT}/p12.mp4")
    hold("p13", f"{CARDS}/C3_counts.png", 16.0)
    # C4: three frames
    three_frames("p14")
    # C5: the layer cut
    hold("p15", f"{C1}/H1-red.png", 6.5, FV_STILL)
    hold("p16", f"{C1}/T1-red.png", 6.0, FV_STILL)
    hold("p17", f"{C1}/T2-magenta.png", 8.5, FV_STILL)
    hold("p18", f"{C1}/T3-green.png", 5.5, FV_STILL)
    hold("p19", f"{C1}/T4-blue.png", 5.5, FV_STILL)
    hold("p20", f"{C1}/T5-cyan.png", 6.0, FV_STILL)
    hold("p21", f"{C1}/key.png", 7.0, FV_STILL)
    # C6: the patch and its price
    hold("p22", f"{C1}/T3-green.png", 4.0, FV_STILL)
    hold("p21b", f"{CARDS}/C2b_presents.png", 20.0)
    hold("p23", f"{CARDS}/C6_leadfill.png", 18.0)

    # -- block 4: July against August ---------------------------------------
    freeze("p24")
    face_off("p25")
    pip("p26", 22.0)

    # -- block 5: the solution ----------------------------------------------
    B5 = [("p27", f"{CARDS}/C7_fafling.png", 16.0),
          ("p28", f"{DIAG}/D4a_ruler.png", 8.0),
          ("p29", f"{DIAG}/D4b_auto.png", 9.0),
          ("p30", f"{DIAG}/D4c_price.png", 8.0),
          ("p31", f"{DIAG}/D4d_manual.png", 12.0),
          ("p32", f"{CARDS}/C9_cef.png", 24.0),
          ("p33", f"{CARDS}/C10_0.png", 7.0),
          ("p34", f"{CARDS}/C10_1.png", 8.0),
          ("p35", f"{CARDS}/C10_2.png", 9.0),
          ("p36", f"{CARDS}/C11_sequence.png", 18.0),
          ("p37", f"{CARDS}/C12_opens.png", 22.0)]
    # the D4 diagrams are too wide to move off centre; every text card is not
    HALF = {"p27", "p32", "p33", "p34", "p35", "p36", "p37"}
    at = 0.0
    if B5_OVER_GAME:
        make_bed()
    for nm, png, ln in B5:
        if B5_OVER_GAME:
            bed(nm, png, ln, at % 50.0, half=nm in HALF)
            at += ln
        else:
            hold(nm, png, ln)

    # -- block 6: the payoff -------------------------------------------------
    # Both diagram panels ride 185508, whose corridors are dark and even; 185408
    # is a berserk melee under a full-screen red wash, which fights a panel.  So
    # the red clip is the plain TNT footage and the calm one carries the text.
    clip("p38", SP3, 0.0, 20.0, FV_AUG)                  # split, no overlay
    # These four lengths had DRIFTED from what was actually built: the last full
    # `pieces` run pre-dated the block-6 rewrite, so p39/p42/p48 kept their older
    # (longer) files on disk while the numbers here said something shorter, and a
    # rebuild silently truncated three lower-thirds -- 14.5 s into a 14 s piece
    # shows nothing at all.  Restored from what each piece has to CARRY:
    #   p39  1.5+11 and 14.5+8.5  -> 24 s      p42  0.5+5.0 -> 6 s
    #   p48  0.8+9.0 and 11.0+6.5 -> 18 s
    # and the D3 step is 4.28, not 4.4, because 48 + 5*4.4 overruns T1B (69.44 s).
    clip("p39", SP1, 8.6, 24.0, FV_AUG)                  # the other player
    clip("p40", T1A, 20.0, 14.0, FV_AUG)                 # TNT MAP01, the melee
    # ORDER, reversed on the owner's note.  It used to run: 277 ms budget -> "415
    # test maps" -> fragmentation -> "now none of them do".  So the example came
    # before the thing it exemplifies, and the corpus was scored before anyone
    # said what it was.  Now: the bench, why 38 of it refused, what changed, and
    # one of those changes in full.
    #   D5   the corpus, and what each WAD is in it to break
    #   D3   x5, the fragmentation, ending on the two levers
    #   D6   the month's list
    #   D2   one row of that list, drawn to scale
    # T1B is 69.44 s with its first 20 s off limits, so 49.4 s of usable footage
    # for 10 + 21.4 + 18 = 49.4.  D5 rides SP3's unused middle instead.
    panel("p41", SP3, 20.0, 10.0, f"{DIAG}/D5_corpus.png")
    for i, png in enumerate(["D3a_zone", "D3b1_try", "D3b2_try", "D3b3_try", "D3c_diet"]):
        panel("p%02d" % (42 + i), T1B, 48.0 + 4.28 * i, 4.28, f"{DIAG}/{png}.png")
    panel("p47", T1B, 20.0, 10.0, f"{DIAG}/D6_month.png")
    panel("p48", T1B, 30.0, 18.0, f"{DIAG}/D2_budget.png")
    clip("p49", T1A, 34.0, 18.0, FV_AUG)                 # a breather before the end
    # the credits ride the third split capture, not TNT: 185508 is spent by here,
    # and closing on two players is the right last image for this episode.
    credits("p50", SP3, 40.0, 19.0, f"{CARDS}/CREDITS.png")


BLOCKS = [
    ("b1", ["p01"]),
    ("b2", ["p02", "p03", "p04"]),
    # p21b: the six dead presents.  A string id rather than a renumbering --
    # every piece after it would otherwise shift, and so would every anchor.
    ("b3", ["p%02d" % i for i in range(5, 22)] + ["p21b", "p22", "p23"]),
    ("b4", ["p24", "p25", "p26"]),
    ("b5", ["p%02d" % i for i in range(27, 38)]),
    ("b6", ["p%02d" % i for i in range(38, 51)]),
]
ORDER = [p for _, ps in BLOCKS for p in ps]


def measure():
    """Absolute start of every piece AND of every block, from the encoded files."""
    start, t = {}, 0.0
    for bname, pieces in BLOCKS:
        start[bname] = t
        for p in pieces:
            start[p] = t
            t += dur(f"{OUT}/{p}.mp4")
        start[bname + "_end"] = t
    start["_total"] = t
    return start


# ---------------------------------------------------------------------------
# voice: (stem, anchor, offset into that anchor)
# ---------------------------------------------------------------------------
VOICE = [
    # block 1 -- the hook, spoken over the title.  New in the third pass: the
    # episode now opens on a question instead of a statement.
    ("n01", "p01", 1.2), ("n02", "p01", 6.6), ("n03", "p01", 11.0),
    # block 2 -- the clean run.  n07 names the subject out loud at the end of
    # the block; the second pass withheld it, which read as coy.
    ("n04", "b2", 22.0), ("n05", "b2", 58.0), ("n06", "b2", 91.6),
    ("n07", "b2", 109.6),
    # C1 -- the symptom
    ("l01", "p06", 1.0), ("l02", "p06", 9.5),
    # C2 -- four corrections
    ("l07", "p07", 0.6), ("l08", "p09", 0.6), ("l09", "p11", 0.4),
    # C3 -- the clue.  Three lines became two: the distinct-picture count is a
    # number to READ, so it stays on the card and left the voice.
    ("n08", "p12", 0.8), ("n09", "p13", 0.6), ("n10", "p13", 6.0),
    # C4 -- three frames
    ("n11", "p14", 1.0), ("n12", "p14", 20.6),
    # C5 -- the layer cut
    ("n13", "p15", 0.8), ("n14", "p16", 0.8), ("n15", "p17", 0.8),
    ("l03", "p18", 1.2), ("l04", "p19", 0.9), ("l05", "p20", 1.0),
    ("l06", "p21", 1.0),
    # C6 -- the patch and its price
    ("l10", "p23", 0.8), ("l11", "p23", 7.2), ("l12", "p23", 11.4),
    # block 4 -- the comparison
    ("n16", "p25", 1.5),
    # block 5 -- the solution
    ("l13", "p27", 1.2),
    ("l14", "p28", 1.0), ("l15", "p29", 1.0), ("l16", "p30", 1.0),
    ("l17", "p31", 0.8), ("l18", "p31", 5.0),
    ("n17", "p32", 1.0), ("n18", "p32", 7.0), ("n19", "p32", 14.5),
    ("n20", "p33", 0.8), ("n21", "p34", 0.8), ("n22", "p35", 0.8),
    ("n23", "p36", 1.2),
    ("n24", "p37", 1.0), ("l19", "p37", 16.5),
]


def build_voice(start):
    # Guard, paid for once: rewriting the table above silently dropped the three
    # C6 lines, and the only symptom was a line count in a log nobody reads.  A
    # recorded take that reaches no piece is a bug, not a choice.
    have = sorted(f[:-4] for f in os.listdir(VO)
                  if len(f) == 7 and f[0] in "ln" and f.endswith(".wav"))
    used = [v[0] for v in VOICE]
    orphan = [s for s in have if s not in used]
    twice = sorted({s for s in used if used.count(s) > 1})
    if orphan:
        print("  WARN recorded but never placed:", " ".join(orphan))
    if twice:
        print("  WARN placed more than once:", " ".join(twice))

    lines = []
    for stem, anchor, off in VOICE:
        src = f"{VO}/{stem}.wav"
        lines.append((start[anchor] + off, src, dur(src), stem))
    lines.sort()
    for (t0, _, d0, a), (t1, _, _, b) in zip(lines, lines[1:]):
        if t0 + d0 > t1 - 0.15:
            print("  WARN %s ends %.2f, %s starts %.2f" % (a, t0 + d0, b, t1))
    inputs, fg = [], []
    for i, (t, src, _, _) in enumerate(lines):
        inputs += ["-i", src]
        fg.append("[%d:a]adelay=%d[v%d]" % (i, int(round(t * 1000)), i))
    fg.append("%samix=inputs=%d:normalize=0:duration=longest,"
              "alimiter=limit=0.95:level=disabled[out]"
              % ("".join("[v%d]" % i for i in range(len(lines))), len(lines)))
    sh("ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", ";".join(fg),
       "-map", "[out]", "-ar", "48000", "-ac", "1", "-c:a", "pcm_s16le",
       f"{OUT}/voice.wav")
    print("voice.wav  %.2f s, %d lines" % (dur(f"{OUT}/voice.wav"), len(lines)))
    return lines


# ---------------------------------------------------------------------------
# subtitles: the date stamps, and the lower-thirds
# ---------------------------------------------------------------------------
A = r"{\c&H002F94DA&\b1\fs40}"   # amber heading
B = r"{\c&H00D3E4ED&\b0\fs32}"   # bone body
G = r"{\c&H0088AE5E&\b0\fs32}"   # a win
R = r"{\c&H004F6AC8&\b0\fs32}"   # a problem, a debt

ASS_HEAD = """[Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080
WrapStyle: 2
ScaledBorderAndShadow: yes

; Ann   bottom LEFT, MarginV 176 -- Doom keeps a 32-line status bar at the foot
;       of a 224-line picture, which is the bottom 154 px of a 1080p master.
; Stamp bottom RIGHT, same MarginV, BONE at 70 %, no plate: it is on screen for
;       the whole shot and a plate that permanent reads as furniture.
;       Lower-thirds are capped at 62 characters so they never reach it.
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Ann,Consolas,34,&H00D3E4ED,&H00D3E4ED,&H00000000,&HA5000000,0,0,0,0,100,100,0,0,3,6,0,1,80,80,176,1
Style: Stamp,Consolas,26,&H4DD3E4ED,&H4DD3E4ED,&HB0000000,&H00000000,0,0,0,0,100,100,1,0,1,2,2,3,80,64,178,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""

AUG20 = "CAPTURED 20 AUG 2026 \u2014 SEGA SATURN"
AUG21 = "CAPTURED 21 AUG 2026 \u2014 SEGA SATURN"
JUL20 = "CAPTURED 20 JUL 2026 \u2014 SEGA SATURN"

# every piece that shows a capture carries the date of that capture.  Cards and
# diagrams carry nothing -- there is nothing to date.  p25 and p26 stamp
# themselves in the picture, because each half needs its own date.
STAMP = {
    "p01": AUG20, "p02": AUG20, "p03": AUG20, "p04": AUG20,
    "p05": JUL20, "p06": JUL20, "p12": JUL20, "p14": JUL20,
    "p15": JUL20, "p16": JUL20, "p17": JUL20, "p18": JUL20,
    "p19": JUL20, "p20": JUL20, "p21": JUL20, "p22": JUL20,
    "p24": JUL20,
    # p41 moved onto SP3 with the reorder (D5 rides the split capture, not TNT),
    # so its stamp follows the FOOTAGE, not the slot: 21 Aug, like p38 and p50.
    "p38": AUG21, "p39": AUG20, "p40": AUG20, "p41": AUG21, "p42": AUG20,
    "p43": AUG20, "p44": AUG20, "p45": AUG20, "p46": AUG20, "p47": AUG20,
    "p48": AUG20, "p49": AUG20, "p50": AUG21,
}

# (anchor, offset, hold, [lines])
LOWER = [
    # ---- block 2: the clean run ------------------------------------------
    ("b2", 3, 13, [A + "DOOM 1 SHAREWARE \u2014 E1M1",
                   B + "Sega Saturn, 1994 \u00b7 two SH-2 at 28 MHz \u00b7 2 MB of work RAM",
                   B + "hardware capture \u2014 no emulator"]),
    # dg_saturn.cxx:4637 -- SAT_WORLD_THINGS_VDP1 defaults to 1: the world
    # sprites are emitted as VDP1 prio-7 quads, like the weapon and the status
    # bar -- which is exactly what C5 cuts apart later in the episode.  Listing
    # them under the CPU was simply wrong, and wrong in the one direction that
    # undersells the machine.
    ("b2", 21, 14, [A + "WHAT DRAWS WHAT",
                    B + "\u2022 walls, world sprites and the weapon \u2014 VDP1 quads",
                    B + "\u2022 sky \u2014 a VDP2 scroll layer",
                    B + "\u2022 the largest floor \u2014 a VDP2 rotation layer"]),
    ("b2", 39, 13, [A + "AND THE REST IS THE CPU",
                    B + "\u2022 every other floor, every ceiling",
                    B + "both SH-2 run the column renderer"]),
    ("b2", 50, 10, [A + "THE WAD IS READ FROM THE DISC, UNMODIFIED",
                    B + "no Saturn level format \u00b7 no re-authoring \u00b7 no RAM cart"]),
    ("b2", 60.5, 5, [A + "E1M2"]),
    ("b2", 74.6, 11, [A + "THE IWAD IS IDENTIFIED BY SCANNING LUMP CONTENTS",
                    B + "not by filename \u2014 one binary, any Doom WAD"]),
    ("b2", 89.6, 13, [A + "ON HARDWARE, 20 AUGUST",
                    B + "\u2022 8 to 15 fps average across these three clips",
                    B + "\u2022 worst case 3.7 \u00b7 best case 22",
                    B + "that is not the subject of this episode"]),
    ("b2", 104.6, 9, [A + "EVERY WALL IN THIS SHOT WAS BROKEN FOR TWO MONTHS"]),
    # ---- block 3, C1 -------------------------------------------------------
    ("p06", 0.5, 14.0, [A + "HOLES",
                        B + "on some frames, and only some."]),
    # ---- C4: one line per frame, on the slow pass -------------------------
    ("p14", 0.6, 11.4, [A + "THREE FRAMES",
                        B + "1.567 \u00b7 1.600 \u00b7 1.633 \u2014 a thirtieth of a second apart"]),
    ("p14", 12.7, 2.4, [B + "everything agrees"]),
    ("p14", 15.3, 2.4, [B + "the room moved. the walls did not."]),
    ("p14", 17.9, 2.4, [B + "the walls move. the room does not."]),
    ("p14", 20.5, 5.4, [A + "EACH LAYER MOVES IN THE FRAME THE OTHER DOES NOT",
                        B + "room   +14 px  \u00b7  +0 px",
                        B + "walls   +0 px  \u00b7  +18 px      [measured, native px]"]),
    # ---- C5: the layer cut -------------------------------------------------
    ("p15", 0.4, 5.8, [A + "VDP1 DREW THIS",
                       B + "one frame late, like everything VDP1 drew"]),
    ("p16", 0.4, 5.3, [A + "AND NOTHING IS WRONG WITH IT",
                       G + "because it does not move with the room"]),
    ("p17", 0.4, 7.8, [A + "VDP1 DREW THESE TOO",
                       B + "same chip, same field, same lateness",
                       R + "but these follow the world"]),
    ("p18", 0.4, 4.8, [A + "AND THIS IS THE PRICE",
                       R + "where the wall should have been, nothing was drawn"]),
    ("p19", 0.4, 4.8, [A + "WHAT SHOWS THROUGH",
                       B + "the floor plane \u2014 VDP2, its own layer, its own clock"]),
    ("p20", 0.4, 5.3, [A + "AND THE REST IS THE CPU PICTURE",
                       B + "a bitmap VDP2 puts on screen as one more layer"]),
    # "two chips" undercounted the machine: the cyan layer is a CPU bitmap, and
    # the CPU here is TWO SH-2 running the column renderer.  Four layers come off
    # four devices -- and naming them beats a number, because two of the four
    # layers (weapon/HUD and walls) come off the same one.
    ("p21", 0.4, 6.3, [A + "FOUR LAYERS, FOUR DEVICES",
                       B + "VDP1 · VDP2 · two SH-2 at 28 MHz",
                       R + "the picture did not agree with itself on which frame it was"]),
    # ---- block 4 -----------------------------------------------------------
    ("p24", 2.2, 3.4, [A + "ONE FIELD OF LATENESS \u2014 HELD STILL",
                       B + "three holes in one frame, where VDP1 had not finished"]),
    # the grey box lands at still-time 3.30, i.e. p24 t=5.1; the caption follows
    # it rather than preceding it, or the sentence names something not yet drawn
    ("p24", 5.7, 3.2, [G + "the grey band is a different bug \u2014",
                       G + "VDP2 plane windowing, fixed since"]),
    ("p25", 12.0, 8.0, [B + "two capture chains — 1080p OBS left, capture card right",
                       R + "there are no frame rates to read here"]),
    ("p26", 1.5, 12.0, [A + "IT WAS NOT AN INCIDENT",
                        R + "July, inset: the join opens on almost every turn",
                        G + "August, full frame: it opens on none"]),
    # ---- block 6 -----------------------------------------------------------
    # "no overlay on this capture" said what the viewer can already see.  These
    # three are load-bearing and checkable: MULTIPLAYER_PLAN.md sec.1 (the port-1
    # multitap hands over pads 2-4 with no init, and FEATURE_MULTIPLAYER stays
    # undef -- there is no netcode at all) and dg_saturn.cxx:8318, where
    # sat_local_players runs 1..4 and each view is rendered in turn.
    ("p38", 2.0, 11.0, [A + "TWO PLAYERS, ONE SATURN",
                        B + "\u2022 two viewpoints \u00b7 one framebuffer \u00b7 one disc",
                        B + "\u2022 up to four on a multitap \u2014 no link cable, no netcode",
                        B + "\u2022 every view is rendered in turn, inside one frame"]),
    # the split block is where the fix pays, so it says what it bought.  Every
    # number here is one the episode has already shown: the 4-to-8 comes from D4.
    ("p39", 1.5, 11.0, [A + "WHAT THE FIX BOUGHT HERE",
                        B + "\u2022 the drawing window was one FIELD",
                        B + "\u2022 now it is one game FRAME \u2014 four to eight times",
                        B + "\u2022 and split-screen draws two views into one"]),
    ("p39", 14.5, 8.5, [A + "SO THE MONSTERS CAME BACK",
                        G + "sprites reach VDP1 in split-screen at last",
                        B + "when the queue runs short, the walls yield first"]),
    ("p40", 2.0, 11.0, [A + "TNT: EVILUTION HAS NEVER RUN ON A SEGA SATURN",
                        B + "Final Doom shipped on PC and PlayStation in 1996.",
                        B + "not on this machine, not then, not since.",
                        G + "32 maps, from the disc, on a stock 2 MB console"]),
    # THREE lower-thirds deleted here rather than remapped, because the reorder
    # gave each of them a card that says the same thing better:
    #   "AND THIS IS WHAT MADE IT PLAYABLE"  -> D2 is now titled "ONE OF THEM,
    #      IN FULL" and D6 sits in front of it, so the relation is stated by the
    #      structure instead of propped up by a caption.
    #   "415 TEST MAPS ... 38 refused"       -> D5 opens the sequence with it.
    #   "NOW NONE OF THEM DO ... 48 -> 130"  -> the first half is D5's closing
    #      line; the 48 -> 130 was the worse offence, a second memory lever the
    #      viewer met as a bare number on an unrelated card.  It is a row of D6
    #      and a numbered lever on D3c now.
    # RESOURCE_BUDGETS.md: b0 % is a PRE-FIX reading.  The gate that kept the
    # slave out of split was lifted the night of 20 August and the console has
    # NOT been re-measured since, so present tense would assert a state nobody
    # has checked.  Past tense plus the date is the whole difference.
    ("p49", 11.0, 6.5, [A + "AND THE CEILING HAS NOT BEEN FOUND",
                        B + "the second SH-2 measured 0 % busy in split-screen",
                        B + "the gate was lifted that same night, never re-measured",
                        B + "and the widened drawing window has no measured ceiling"]),
]


def ts(t):
    return "%d:%02d:%05.2f" % (int(t // 3600), int(t // 60) % 60, t - 60 * int(t // 60))


def build_ass(start):
    over = [l for _, _, _, ls in LOWER for l in ls if len(l) - 25 > 62]
    for l in over:
        print("  WARN over 62 chars:", l[25:])
    # A lower-third anchored past the end of the thing it is anchored TO is
    # invisible, and nothing about the output says so: the .ass still parses, the
    # master still encodes, the line simply never appears.  Found the hard way
    # when a piece was rebuilt 10 s shorter than the file it replaced and took
    # "SO THE MONSTERS CAME BACK" with it.
    for anchor, off, hold_, lines in LOWER:
        span = (start[anchor + "_end"] - start[anchor]) if anchor + "_end" in start \
            else dur(f"{OUT}/{anchor}.mp4")
        if off >= span:
            print("  WARN %s +%.1f never shows (%s is %.1f s): %s"
                  % (anchor, off, anchor, span, lines[0][25:]))
        elif off + hold_ > span + 0.05:
            print("  WARN %s +%.1f+%.1f overruns %s by %.1f s"
                  % (anchor, off, hold_, anchor, off + hold_ - span))
    with open(f"{OUT}/ep1.ass", "w", encoding="utf-8") as f:
        f.write(ASS_HEAD)
        for piece, text in sorted(STAMP.items(), key=lambda kv: start[kv[0]]):
            t0 = start[piece]
            t1 = t0 + dur(f"{OUT}/{piece}.mp4")
            f.write("Dialogue: 0,%s,%s,Stamp,,0,0,0,,%s\n" % (ts(t0 + 0.4), ts(t1), text))
        for anchor, off, hold_, lines in LOWER:
            t0 = start[anchor] + off
            f.write("Dialogue: 0,%s,%s,Ann,,0,0,0,,%s\n"
                    % (ts(t0), ts(t0 + hold_), r"\N".join(lines)))
    print("ep1.ass  %d stamps, %d lower-thirds" % (len(STAMP), len(LOWER)))


# ---------------------------------------------------------------------------
# music, mix, master
# ---------------------------------------------------------------------------
def build_music(total, switch):
    """Track 1 to the seam, Track 2 after it, crossfaded over 3 s.

    The switch is not decoration: it lands exactly where the episode stops being
    about the problem and starts being about the fix."""
    xf = 3.0
    fg = (f"[0:a]atrim=0:{switch + xf},afade=t=in:st=0:d=2[m1];"
          f"[1:a]atrim=0:{total - switch + xf},adelay=0|0[m2];"
          f"[m1][m2]acrossfade=d={xf}:c1=tri:c2=tri,"
          f"atrim=0:{total},afade=t=out:st={total - 5}:d=5[a]")
    sh("ffmpeg", "-v", "error", "-y",
       "-stream_loop", "-1", "-i", MUS1, "-stream_loop", "-1", "-i", MUS2,
       "-filter_complex", fg, "-map", "[a]", "-ar", "48000", "-ac", "2",
       "-c:a", "pcm_s16le", f"{OUT}/music.wav")


def assemble(start):
    with open(f"{OUT}/list.txt", "w", encoding="utf-8") as f:
        for p in ORDER:
            f.write("file '%s/%s.mp4'\n" % (OUT, p))
    sh("ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0",
       "-i", f"{OUT}/list.txt", "-c", "copy", f"{OUT}/picture.mp4")
    total = dur(f"{OUT}/picture.mp4")
    build_music(total, start["b5"])

    # three beds: voice on top, music under it, the game under that.  Both beds
    # are keyed off the voice, so a line never fights the bed it sits on.
    #
    # apad on the KEY is load-bearing, and cost a whole master to find:
    # sidechaincompress ends when EITHER input ends, and the key is the voice.
    # The last spoken line is at 8:24, so without the pad the music and the game
    # were cut dead there and the final 131 s of the episode had NO AUDIO AT ALL
    # -- not "no music", silence.  amix could not rescue it either: all three of
    # its inputs were then the same length, so duration=longest changed nothing.
    fg = (f"[0:a]apad=whole_dur={total + 1},asplit=3[vk1][vk2][vmix];"
          "[1:a]volume=-13dB[mus];"
          "[2:a]volume=-19dB,highpass=f=120[gam];"
          "[mus][vk1]sidechaincompress=threshold=0.03:ratio=8:attack=30:release=450[md];"
          "[gam][vk2]sidechaincompress=threshold=0.03:ratio=6:attack=20:release=350[gd];"
          "[vmix][md][gd]amix=inputs=3:normalize=0:duration=longest,"
          "aresample=192000,alimiter=limit=0.89:level=disabled,aresample=48000[a]")
    sh("ffmpeg", "-v", "error", "-y", "-i", f"{OUT}/voice.wav",
       "-i", f"{OUT}/music.wav", "-i", f"{OUT}/picture.mp4",
       "-filter_complex", fg, "-map", "[a]", "-t", str(total),
       "-ar", "48000", "-ac", "2", "-c:a", "aac", "-b:a", "192k", f"{OUT}/mix.m4a")

    sh("ffmpeg", "-v", "error", "-y", "-i", f"{OUT}/picture.mp4", "-i", f"{OUT}/mix.m4a",
       "-vf", "subtitles=ep1.ass", "-r", "60",
       "-c:v", "libx264", "-preset", "medium", "-crf", "18",
       "-pix_fmt", "yuv420p", "-profile:v", "high", "-level", "4.1",
       "-map", "0:v", "-map", "1:a", "-c:a", "copy", "-movflags", "+faststart",
       f"{WK}/MIMAS-devlog1-ep1.mp4")


def loudness():
    """Final level pass: MEASURE the master, then correct once.

    Two-pass loudnorm, not one: a single-pass loudnorm guesses from a look-ahead
    window and drifts on a ten-minute piece with silence in it.  Target -14 LUFS,
    which is the level YouTube leaves alone.

    TP is asked for at -1.5 and not -1.0 on purpose: loudnorm estimates true peak
    on a resampled signal, so the delivered peak lands about 0.2 dB high.  Asking
    for -1.0 measured back at -0.8 dBFS, which is over the ceiling.
    """
    import json
    src = f"{WK}/MIMAS-devlog1-ep1.mp4"
    out = f"{WK}/MIMAS-devlog1-ep1-loud.mp4"
    r = subprocess.run(
        ["ffmpeg", "-hide_banner", "-i", src, "-af",
         "loudnorm=I=-14:TP=-1.5:LRA=9:print_format=json", "-f", "null", "-"],
        capture_output=True, text=True)
    tail = r.stderr[r.stderr.rfind("{"):]
    m = json.loads(tail[:tail.find("}") + 1])
    print("measured  I %s  TP %s  LRA %s  thresh %s"
          % (m["input_i"], m["input_tp"], m["input_lra"], m["input_thresh"]))
    af = ("loudnorm=I=-14:TP=-1.5:LRA=9:"
          "measured_I=%s:measured_TP=%s:measured_LRA=%s:measured_thresh=%s:"
          "offset=%s:linear=true:print_format=summary"
          % (m["input_i"], m["input_tp"], m["input_lra"], m["input_thresh"],
             m["target_offset"]))
    sh("ffmpeg", "-v", "error", "-y", "-i", src, "-c:v", "copy",
       "-af", af, "-ar", "48000", "-ac", "2", "-c:a", "aac", "-b:a", "192k",
       "-movflags", "+faststart", out)
    subprocess.run(["ffmpeg", "-hide_banner", "-i", out, "-af",
                    "ebur128=peak=true", "-f", "null", "-"])
    print("->", out)


def report(start):
    print("\n%-5s %8s %8s" % ("block", "start", "length"))
    for b, _ in BLOCKS:
        print("%-5s %8s %8.1f s" % (b, ts(start[b]), start[b + "_end"] - start[b]))
    print("%-5s %8s %8.1f s" % ("TOTAL", ts(start["_total"]), start["_total"]))


# YouTube chapters, derived from the measured piece table rather than typed by
# hand: every one of these was already stale by 6 s after a single block-2 trim,
# and a chapter that lands mid-sentence is worse than no chapter at all.
CHAPTERS = [
    ("p01", "Doom on a Sega Saturn"),
    ("p02", "What draws what"),
    ("p05", "The holes"),
    ("p07", "Four geometric corrections"),
    ("p12", "It only happens when the picture moves"),
    ("p14", "Three frames"),
    ("p15", "One frame, cut into layers"),
    ("p21b", "Six ways to present a frame"),
    ("p22", "Lead-fill, and what it cost"),
    ("p24", "July against August"),
    ("p27", "The cause, named from outside"),
    ("p28", "The field is the budget"),
    ("p32", "The completion flag lied"),
    ("p33", "Two words in the manual"),
    ("p36", "The sequence with no ambiguity"),
    ("p37", "What it opens"),
    ("p38", "Two players, one Saturn"),
    ("p40", "TNT: Evilution"),
    ("p41", "The bench: 415 maps"),
    ("p42", "Why 38 refused to load"),
    ("p47", "What actually changed"),
    ("p50", "Credits"),
]


def chapters(st):
    """YouTube needs 00:00 first and every entry strictly increasing."""
    print("Chapters:")
    last = -1
    for piece, label in CHAPTERS:
        t = int(st[piece])
        if t <= last:
            t = last + 1
        last = t
        print("    %02d:%02d %s" % (t // 60, t % 60, label))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    step = sys.argv[1] if len(sys.argv) > 1 else "all"
    os.chdir(OUT)
    if step in ("pieces", "all"):
        build_pieces()
    st = measure()
    report(st)
    if step in ("post", "all"):
        build_voice(st)
        build_ass(st)
        assemble(st)
    if step in ("loud", "all"):
        loudness()
    if step in ("chapters", "post", "all"):
        chapters(st)
    print("done")
