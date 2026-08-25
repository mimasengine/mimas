"""The explanatory diagrams for devlog #1, drawn to sit OVER the footage.

    python ep1_diagrams.py OUTDIR

Three survive from the first cut, and two of those are rewritten:

    D4  the field timeline    <- THE card.  now carries the notion of a CYCLE:
                                 what a field is, what a frame is, and what
                                 actually closes the drawing window.
    D3  the fragmented zone   <- rewritten as a MECHANISM: a piece is offered to
                                 each free run and refused, then shrunk until it
                                 drops in.  v1 showed a state, which read as
                                 "clumsy" because a state explains nothing.
    D2  the frame budget         unchanged, validated.

D1 (two chips / two pictures) and D5 (four corrections) are GONE.  D1 is replaced
by the real decomposed frame (see ep1_c1_layers.py); D5 by a plain struck-through
list, which four labels do better than any drawing.

D4 and D3 are emitted as CUMULATIVE STAGES, not single cards: a dense diagram
held for half a minute is the slide deck this format was rewritten to escape.
Each stage adds one idea; the editor cuts them with the voice.

Numbers are the measured ones.  Sources: docs/DEVLOG_EP1_SCRIPT.md sections 6, 8, 10.
"""
import argparse
import os

from PIL import Image, ImageDraw

from devlog_style import W, H, INK, BONE, AMBER, RUST, GREEN, GREY, RULE, font


def _t(d, x, y, s, f, fill, anchor="l"):
    b = d.textbbox((0, 0), s, font=f)
    w = b[2] - b[0]
    if anchor == "c":
        x -= w / 2
    elif anchor == "r":
        x -= w
    d.text((x, y), s, font=f, fill=fill)
    return w


def _quote(d, x, y, lines, size=24, fill=GREY, lead=32):
    """A manual citation: hanging rust bar, italic-ish grey body."""
    d.rectangle([x, y, x + 4, y + lead * len(lines) - 8], fill=RUST)
    for i, s in enumerate(lines):
        _t(d, x + 18, y + lead * i, s, font("mono", size), fill)
    return y + lead * len(lines)


# ============================================================== D4 — fields ===
# Geometry shared by every stage so they overlay cleanly when cut together.
FX0, FSLOT, FN = 130, 196, 8          # ruler origin, one field, field count
FGAME = 6                             # one Mimas frame at ~10 fps, in fields
FX1 = FX0 + FSLOT * FN
FGX = FX0 + FSLOT * FGAME
RY, AY, MY = 236, 360, 740            # ruler, automatic bar, manual bar
RCOL = 1330                           # right column: free once the bars stop at FGX


def _d4_base():
    """Title, the manual's two definitions, and the field ruler.

    The ruler is the whole point of the redraw: the previous version had bars but
    no CYCLE, so there was no answer to "why does it stop after six?".  The answer
    is in the manual's own definitions -- a frame is delimited by the SWAP, so the
    window is closed by the swap itself, not by any clock.
    """
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)

    _t(d, FX0, 54, "THE FIELD IS THE BUDGET", font("mono_bold", 46), AMBER)
    _quote(d, 1010, 44, [
        '"A field is the time it takes a scanning line to scan',
        ' one screen.  A frame is the time it takes from one',
        ' change of the frame buffer to the next change."',
        '                        -- ST-013, Introduction',
    ], size=22, lead=28)

    _t(d, FX0, 124, "the window is closed by the SWAP itself.", font("mono", 28), BONE)

    # the ruler: one cell per field, a tick at every vertical blank
    for i in range(FN + 1):
        x = FX0 + FSLOT * i
        d.line([(x, RY - 14), (x, RY + 16)], fill=RULE, width=3)
    d.line([(FX0, RY), (FX1, RY)], fill=RULE, width=3)
    for i in range(FN):
        _t(d, FX0 + FSLOT * i + FSLOT / 2, RY + 24, "1/60 s", font("mono", 22), GREY, "c")
    _t(d, FX1 + 16, RY - 12, "V-BLANK", font("mono", 22), RULE)
    return im, d


def d4a_ruler():
    im, _ = _d4_base()
    return im


def d4b_auto():
    """Row A: in 1-cycle the hardware closes the window at every single tick."""
    im, d = _d4_base()
    _t(d, FX0, AY - 54, "1-CYCLE   FBCR = 0x0000   the default",
       font("mono_bold", 34), RUST)

    d.rectangle([FX0, AY, FX0 + FSLOT, AY + 64], fill=(112, 58, 44))
    d.rectangle([FX0, AY, FX0 + FSLOT, AY + 64], outline=RUST, width=3)
    for hx in range(int(FX0 + FSLOT), int(FGX), 22):
        d.line([(hx, AY + 64), (hx + 32, AY)], fill=(58, 40, 34), width=3)
    d.rectangle([FX0 + FSLOT, AY, FGX, AY + 64], outline=(74, 52, 44), width=2)
    _t(d, (FX0 + FSLOT + FGX) / 2, AY + 18, "never drawn", font("mono", 28), (132, 102, 90), "c")

    for i in range(1, FGAME + 1):                    # a swap at EVERY tick
        cx = FX0 + FSLOT * i
        d.polygon([(cx - 11, AY + 78), (cx + 11, AY + 78), (cx, AY + 100)], fill=RUST)
    _t(d, FX0, AY + 112, "the HARDWARE closes it -- every field, finished or not.",
       font("mono", 28), BONE)
    _quote(d, FX0, AY + 152, [
        '"When the change mode of the frame buffer is in a one cycle',
        ' mode, one frame is equal to one field."',
    ], size=22, lead=28)
    return im


def d4c_price():
    """What an overrun actually costs.  Not time -- content."""
    im = d4b_auto()
    d = ImageDraw.Draw(im)
    y = AY + 226
    _t(d, FX0, y, "the list restarts at 00000H -- an undrawn tail is ABANDONED.",
       font("mono", 28), GREY)
    _t(d, FX0, y + 40, "overrun does not cost time. it costs content.",
       font("mono_bold", 30), RUST)
    return im


def d4d_manual():
    """Row B: in manual change nothing on the ruler closes the window. The CPU does."""
    im = d4c_price()
    d = ImageDraw.Draw(im)
    _t(d, FX0, MY - 54, "MANUAL CHANGE   FCM = 1, FCT = 1", font("mono_bold", 34), GREEN)

    d.rectangle([FX0, MY, FGX, MY + 64], fill=(40, 78, 63))
    d.rectangle([FX0, MY, FGX, MY + 64], outline=GREEN, width=3)
    _t(d, (FX0 + FGX) / 2, MY + 18, "VDP1 draws the whole frame", font("mono", 28), BONE, "c")

    d.polygon([(FGX - 11, MY + 78), (FGX + 11, MY + 78), (FGX, MY + 100)], fill=GREEN)
    _t(d, FGX + 24, MY + 74, "CPU", font("mono_bold", 26), GREEN)
    _t(d, FX0, MY + 112, "the CPU closes it -- one write, when the drawing is actually done.",
       font("mono", 28), BONE)

    by = MY + 158                                     # the brace under the bar
    d.line([(FX0, by), (FGX, by)], fill=AMBER, width=3)
    d.line([(FX0, by - 9), (FX0, by + 9)], fill=AMBER, width=3)
    d.line([(FGX, by - 9), (FGX, by + 9)], fill=AMBER, width=3)
    _t(d, (FX0 + FGX) / 2, by + 14, "one Mimas frame -- 4 to 8 fields, and it varies",
       font("mono", 26), AMBER, "c")

    _quote(d, RCOL, AY + 40, [
        '"The number of characters that can be',
        ' drawn in one FIELD is limited.',
        ' Therefore, in order to draw more',
        ' characters, the manual mode must be set."',
        '   -- ST-013 p.38, errata-corrected scan',
    ], size=22, lead=28)
    return im


# ================================================================ D3 — zone ===
ZX0, ZW, ZY, ZH = 130, 1660, 300, 96
ZKB = 1016.0                                          # the Doom zone, in KB
FREE = [(150, 26), (240, 44), (330, 18), (420, 39),   # measured: 240 KB total,
        (500, 22), (600, 31), (740, 48), (900, 12)]   # longest single run 48 KB
STATIC = [(64, 72), (655, 60)]                        # the two parked PU_STATIC blocks
PIECE = 110.0                                         # LINEDEFS, and it wants one run


def _kb(v):
    return ZX0 + ZW * (v / ZKB)


def _d3_base(title):
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    _t(d, ZX0, 54, title, font("mono_bold", 46), AMBER)
    # The title used to be "FREE IS NOT THE SAME AS CONTIGUOUS", which states the
    # mechanism without ever saying what it explains.  Title asks, strapline
    # answers.  And the answer is NOT "the machine lacked the RAM": 240 KB were
    # free.  They were in eight pieces, and the biggest was 48 KB -- which is the
    # entire point of the bar below, and the opposite of running out.
    _t(d, ZX0, 116, "the RAM was free. it was not in one piece.",
       font("mono_bold", 32), BONE)
    _t(d, ZX0, 166, "the Doom zone: 1,040,384 bytes, in the slower of the two work-RAM banks",
       font("mono", 28), GREY)

    d.rectangle([ZX0, ZY, ZX0 + ZW, ZY + ZH], fill=(58, 52, 44))
    for a, n in FREE:
        d.rectangle([_kb(a), ZY, _kb(a + n), ZY + ZH], fill=(40, 76, 62))
    for a, n in STATIC:                               # the two blocks that do the cutting
        d.rectangle([_kb(a), ZY, _kb(a + n), ZY + ZH], fill=(112, 58, 44))
        _t(d, (_kb(a) + _kb(a + n)) / 2, ZY + ZH + 10, "PU_STATIC", font("mono", 20), RUST, "c")
        _t(d, (_kb(a) + _kb(a + n)) / 2, ZY + ZH + 34, "%d KB" % n, font("mono", 20), RUST, "c")
    d.rectangle([ZX0, ZY, ZX0 + ZW, ZY + ZH], outline=RULE, width=2)
    _t(d, ZX0, ZY - 44, "grey = in use     green = free     rust = parked, and it cuts the space",
       font("mono", 26), GREY)
    return im, d


def d3a_zone():
    """Stage 1: the zone, its free runs measured and labelled."""
    im, d = _d3_base("WHY 38 MAPS REFUSED TO LOAD")
    for a, n in FREE:
        if n >= 18:
            _t(d, (_kb(a) + _kb(a + n)) / 2, ZY + 32, str(n), font("mono_bold", 24), BONE, "c")
    _t(d, ZX0, ZY + 200, "240 KB free, in eight pieces.", font("mono_bold", 38), BONE)
    _t(d, ZX0, ZY + 254, "48 KB in the biggest one.", font("mono_bold", 38), GREEN)
    return im


def _d3_piece(d, x, y, kb, colour, label):
    """The block being offered.  The label goes ABOVE it: at this scale 110 KB is
    180 px wide and any caption longer than three words gets clipped inside."""
    w = ZW * (kb / ZKB)
    d.rectangle([x, y, x + w, y + ZH], fill=colour)
    d.rectangle([x, y, x + w, y + ZH], outline=BONE, width=2)
    _t(d, x + w / 2, y - 38, label, font("mono_bold", 28), colour, "c")
    return w


def d3b_reject(step):
    """Stage 2: the piece is offered to a free run, and the run is too short.

    Three sub-frames so the editor can slide it.  This is the whole difference
    from v1: a piece being REFUSED is a mechanism, a coloured bar is a state.
    """
    im, d = _d3_base("WHY 38 MAPS REFUSED TO LOAD")
    targets = [(240, 44), (600, 31), (740, 48)]
    a, n = targets[step]
    cx = (_kb(a) + _kb(a + n)) / 2                    # the piece hovers over the run
    w = ZW * (PIECE / ZKB)
    _d3_piece(d, cx - w / 2, ZY - 168, PIECE, RUST, "LINEDEFS  110 KB")
    d.line([(cx, ZY - 62), (cx, ZY - 22)], fill=RUST, width=3)
    d.polygon([(cx - 10, ZY - 30), (cx + 10, ZY - 30), (cx, ZY - 8)], fill=RUST)
    d.rectangle([_kb(a) - 3, ZY - 3, _kb(a + n) + 3, ZY + ZH + 3], outline=RUST, width=4)
    _t(d, cx, ZY + ZH + 64, "%d KB" % n, font("mono_bold", 30), RUST, "c")
    _t(d, cx, ZY + ZH + 100, "too short", font("mono", 26), RUST, "c")
    _t(d, ZX0, ZY + 236, "240 KB free. and not one piece of it long enough.",
       font("mono", 34), BONE)
    if step == 2:
        _t(d, ZX0, ZY + 300, "it fit, and it still failed.", font("mono_bold", 48), RUST)
    return im


def d3c_diet():
    """Stage 3: the same table at 24 bytes a line drops into the 48 KB run."""
    im, d = _d3_base("WHY 38 MAPS REFUSED TO LOAD")
    a, n = 740, 48
    shrunk = PIECE * 24.0 / 64.0
    _d3_piece(d, _kb(a), ZY, shrunk, GREEN, "the same table -- %d KB" % round(shrunk))
    d.rectangle([_kb(a) - 3, ZY - 3, _kb(a + n) + 3, ZY + ZH + 3], outline=GREEN, width=4)
    _t(d, (_kb(a) + _kb(a + n)) / 2, ZY + ZH + 64, "it fits", font("mono_bold", 30), GREEN, "c")

    # Two levers, not one.  The earlier cut showed only the struct diet and then
    # dropped "largest contiguous run: 48 KB -> 130 KB" as a lower-third on an
    # unrelated card, unexplained -- so the viewer met the second lever without
    # ever being told it was one.  Shrink what is asked for, AND grow the longest
    # run the zone can still hand out.
    _t(d, ZX0, ZY + 232, "TWO THINGS HAD TO CHANGE", font("mono_bold", 34), AMBER)
    _t(d, ZX0, ZY + 290, "1.  shrink the request", font("mono_bold", 32), GREEN)
    _t(d, ZX0 + 460, ZY + 290, "line_t:  64 bytes  ->  24",
       font("mono_bold", 32), GREEN)
    _t(d, ZX0 + 40, ZY + 332, "seg_t and node_t shrank too. they freed real RAM, and not one "
       "map:", font("mono", 26), GREY)
    _t(d, ZX0 + 40, ZY + 366, "on every blocked map the dominant term was LINEDEFS.",
       font("mono", 26), GREY)

    _t(d, ZX0, ZY + 420, "2.  grow the longest run", font("mono_bold", 32), GREEN)
    _t(d, ZX0 + 460, ZY + 420, "48 KB  ->  130 KB", font("mono_bold", 32), GREEN)
    _t(d, ZX0 + 40, ZY + 462, "the texture directories became lazy and purgeable, so the "
       "allocator", font("mono", 26), GREY)
    _t(d, ZX0 + 40, ZY + 496, "can reclaim them and close the gaps between the parked blocks.",
       font("mono", 26), GREY)

    # The obvious question the bar invites, answered before it is asked.
    _t(d, ZX0, ZY + 556, "defragmenting all the way to 110 KB is possible. it is not free:",
       font("mono", 28), BONE)
    _t(d, ZX0, ZY + 592, "that RAM is holding the sprites, the textures and the sound this "
       "map is playing.", font("mono", 28), BONE)
    _t(d, ZX0, ZY + 632, "no single allocation gets 110 KB reserved for it.",
       font("mono_bold", 30), RUST)
    # "38 of 415 ... now none of them do" used to close this card.  It moved to
    # D5, which is now what OPENS the sequence: the corpus is introduced, then
    # why 38 of it refused, then what was changed.  Stating the result here as
    # well would have given the answer twice and the question once.
    return im


# ============================================================== D2 — budget ===
def d2_budget():
    """Three bars to ONE scale.  Unchanged: the owner validated this one."""
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    # This card used to arrive titled "WHERE 277 MILLISECONDS WENT", straight
    # after "TNT has never run on a Saturn", with no stated relation to it -- so
    # it read as a non sequitur and needed a lower-third to prop it up.  It is
    # not the reason TNT runs; it is ONE example of the code work of the month,
    # and the title now says so.  The list it exemplifies is D6, just before.
    _t(d, 150, 90, "ONE OF THEM, IN FULL", font("mono_bold", 44), AMBER)
    _t(d, 150, 156, "TNT: Evilution MAP11, on the console.  every bar to the same scale.",
       font("mono", 30), GREY)

    x0, px = 150, 5.4
    d.rectangle([x0, 300, x0 + 16.7 * px, 362], fill=GREEN)
    _t(d, x0, 258, "what a 60 Hz frame gets", font("mono", 30), GREY)
    _t(d, x0 + 16.7 * px + 22, 314, "16.7 ms", font("mono_bold", 34), GREEN)

    d.rectangle([x0, 470, x0 + 277 * px, 532], fill=(70, 62, 50))
    d.rectangle([x0, 470, x0 + 46.6 * px, 532], fill=RUST)
    _t(d, x0, 428, "one frame on that map", font("mono", 30), GREY)
    _t(d, x0 + 277 * px + 22, 484, "277 ms", font("mono_bold", 34), RUST)
    _t(d, x0 + 8, 546, "46.6 ms - ONE call to R_GetColumn", font("mono_bold", 26), RUST)

    d.rectangle([x0, 700, x0 + 61 * px, 762], fill=GREEN)
    d.rectangle([x0 + 61 * px, 700, x0 + 142 * px, 762], fill=(40, 76, 62))
    _t(d, x0, 658, "the same map, after decoding 1 KB of a patch instead of 35",
       font("mono", 30), GREY)
    _t(d, x0 + 142 * px + 22, 714, "61 - 142 ms", font("mono_bold", 34), GREEN)

    _t(d, 150, H - 120, "the fix was not engineering. it was reading the header and stopping.",
       font("mono", 32), BONE)
    return im


# ============================================================== D5 — corpus ===
# The episode said "415 test maps" three times and never once said what they
# were or why those.  This opens the sequence instead: here is the bench, here
# is what each WAD is in it for, here is the score.
#
# NOTE, deliberately: no per-WAD map counts on this card.  The corpus README
# documents the WADs, and the audit documents the 415 total and the 38 -> 0, but
# nothing on file decomposes 415 per WAD -- and the obvious counts (32 each for
# the megawads, 36 for Ultimate, 9 shareware) do not add up to 415 on their own.
# Printing both would invite an audience that checks to do arithmetic that does
# not close.  Names and roles, one total, no sum to audit.
D5_ROWS = [
    ("Doom, shareware",   "the dev vehicle -- and the only one that fits on a cartridge"),
    ("Ultimate Doom",     "open vistas: visplanes and segs, E3M6 and E4M2"),
    ("Doom II",           "MAP13 and MAP15 -- the first map that ever refused to load"),
    ("TNT: Evilution",    "MAP27, the only confirmed visplane overflow in a shipped IWAD"),
    ("Plutonia",          "MAP11 is pure AI, MAP32 the densest legal fight"),
    ("Hell Revealed",     "MAP24, whose save buffer alone is ~180 KB"),
    ("Scythe",            "MAP30, a slaughter map on a 2 MB console"),
    ("Nuts 1 / 2 / 3",    "4,000 to 12,000 monsters. these are meant to fail"),
]


def d5_corpus():
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    _t(d, ZX0, 54, "THE BENCH: 415 MAPS", font("mono_bold", 46), AMBER)
    _t(d, ZX0, 118, "not a demo reel. every WAD here is in it to break something specific.",
       font("mono", 28), GREY)
    y = 200
    for what, why in D5_ROWS:
        _t(d, ZX0, y, what, font("mono_bold", 32), BONE)
        _t(d, ZX0 + 470, y + 4, why, font("mono", 27), GREY)
        y += 62
    # The closing block has to clear the burned-in date stamp, which sits bottom
    # right from about x=1400, y=880.  The first cut ran a 58-character line
    # straight through it.  Two shorter lines, lifted, and nothing crosses.
    y += 16
    _t(d, ZX0, y, "38 of them did not run slowly.", font("mono_bold", 38), RUST)
    _t(d, ZX0, y + 52, "they refused to load at all.", font("mono_bold", 38), RUST)
    _t(d, ZX0, y + 124, "now none of them do -- except the Nuts family,",
       font("mono_bold", 38), GREEN)
    _t(d, ZX0, y + 172, "which no machine loads.", font("mono_bold", 38), GREEN)
    return im


# ========================================================== D6 — the month ===
# D2 used to arrive alone and unexplained.  It is one item from this list.
D6_ROWS = [
    ("line_t", "64 -> 24 bytes", "38 maps over the wall  ->  0", True),
    ("seg_t, node_t", "32 -> 14, 52 -> 28", "real RAM freed, and zero maps", False),
    ("side_t", "20 -> 16 bytes", "21 KB back", False),
    ("texture directories", "lazy + purgeable", "longest free run 48 -> 130 KB", True),
    ("every P_LoadX", "reads its lump in place", "46 KB to 203 KB of peak, gone", True),
    ("the mobj table", "a slab, not a list", "~24 KB back on MAP30", False),
    ("BACKUPTICS", "128 -> 32", "~15 KB: a ticcmd ring with no network", False),
    ("R_GetColumn", "read the header, stop", "277 ms  ->  61-142 ms", True),
]


def d6_month():
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    _t(d, ZX0, 54, "WHAT ACTUALLY CHANGED", font("mono_bold", 46), AMBER)
    # NOT "the sync fix was one line of register" -- the v2 VBE present is a
    # sequence, not a line, and that sentence would have been an easy exaggeration
    # sitting next to eight verified rows.
    _t(d, ZX0, 118, "one month of it, and none of it is the sync fix. that was a different "
       "problem.", font("mono", 28), GREY)
    y = 210
    for what, how, gain, big in D6_ROWS:
        _t(d, ZX0, y, what, font("mono_bold", 30), BONE if big else GREY)
        _t(d, ZX0 + 400, y, how, font("mono", 28), GREY)
        _t(d, ZX0 + 900, y, gain, font("mono_bold" if big else "mono", 28),
           GREEN if big else GREY)
        y += 62
    _t(d, ZX0, y + 40, "shrinking seg_t and node_t freed real RAM and moved not one map.",
       font("mono", 30), BONE)
    _t(d, ZX0, y + 84, "find the dominant term first. it was LINEDEFS, on every one of the 38.",
       font("mono_bold", 32), RUST)
    return im


def build(out):
    os.makedirs(out, exist_ok=True)
    made = [
        ("D5_corpus.png",  d5_corpus()),
        ("D6_month.png",   d6_month()),
        ("D4a_ruler.png",  d4a_ruler()),
        ("D4b_auto.png",   d4b_auto()),
        ("D4c_price.png",  d4c_price()),
        ("D4d_manual.png", d4d_manual()),
        ("D3a_zone.png",   d3a_zone()),
        ("D3b1_try.png",   d3b_reject(0)),
        ("D3b2_try.png",   d3b_reject(1)),
        ("D3b3_try.png",   d3b_reject(2)),
        ("D3c_diet.png",   d3c_diet()),
        ("D2_budget.png",  d2_budget()),
    ]
    for name, im in made:
        im.save(os.path.join(out, name))
    print("wrote %d diagrams to %s" % (len(made), out))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    build(ap.parse_args().out)
