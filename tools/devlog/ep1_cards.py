"""The text cards for devlog #1, v5 card set.

    python ep1_cards.py OUTDIR

Only the cards that have NO visual form live here.  Everything that can be shown
on a real frame is shown on one instead -- C1/C3/C4/C5 come from the footage and
from ep1_c1_layers.py, C8 is the field diagram in ep1_diagrams.py.  What is left
is quotation and argument, which is what a card is actually good at.

    TITLE
    C2   four geometric corrections, struck through one at a time   (5 stages)
    C3   the clue, and the counts that back it
    C6   lead-fill, and what it cost
    C7   fafling names the cause
    C9   the completion flag lied
    C10  two words in the manual                                    (3 stages)
    C11  the sequence with no ambiguity
    C12  what it opens
    CREDITS

Rules the previous cut broke, kept here as code:
  * cards that carry a LIST are emitted as stages, so nothing is read before it
    is spoken.  A full list on screen invites the viewer to read ahead and stop
    listening.
  * every claim that came out of a manual is set as a quotation with its page,
    because the audience for this checks.
  * no pronouns anywhere on screen.
"""
import argparse
import os

from PIL import Image, ImageDraw

from devlog_style import W, H, INK, BONE, AMBER, RUST, GREEN, GREY, RULE, font

L, TOP = 150, 96          # the text column, and where a card's title sits


def _t(d, x, y, s, f, fill, anchor="l"):
    b = d.textbbox((0, 0), s, font=f)
    w = b[2] - b[0]
    if anchor == "c":
        x -= w / 2
    elif anchor == "r":
        x -= w
    d.text((x, y), s, font=f, fill=fill)
    return w


def _card(title, sub=None):
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    _t(d, L, TOP, title, font("mono_bold", 46), AMBER)
    d.line([(L, TOP + 66), (L + 620, TOP + 66)], fill=RULE, width=3)
    if sub:
        _t(d, L, TOP + 92, sub, font("mono", 28), GREY)
    return im, d


def _body(d, y, lines, size=34, fill=BONE, lead=52):
    """Body copy.  A leading tilde marks a line that should land in AMBER."""
    for s in lines:
        f, c = font("mono", size), fill
        if s.startswith("~"):
            s, f, c = s[1:], font("mono_bold", size), AMBER
        elif s.startswith("!"):
            s, f, c = s[1:], font("mono_bold", size), RUST
        elif s.startswith("+"):
            s, f, c = s[1:], font("mono_bold", size), GREEN
        _t(d, L, y, s, f, c)
        y += lead if s else lead // 2
    return y


def _quote(d, y, lines, credit=None, size=30, lead=42):
    d.rectangle([L, y, L + 5, y + lead * len(lines) - 10], fill=RUST)
    for i, s in enumerate(lines):
        _t(d, L + 24, y + lead * i, s, font("mono", size), BONE)
    y += lead * len(lines)
    if credit:
        _t(d, L + 24, y + 6, credit, font("mono", 26), GREY)
        y += 44
    return y


# ------------------------------------------------------------------- title ---
def title():
    im = Image.new("RGB", (W, H), INK)
    d = ImageDraw.Draw(im)
    _t(d, W / 2, 380, "MIMAS", font("mono_bold", 132), AMBER, "c")
    _t(d, W / 2, 546, "Doom on the Sega Saturn", font("mono", 42), BONE, "c")
    d.line([(W / 2 - 300, 626), (W / 2 + 300, 626)], fill=RULE, width=3)
    _t(d, W / 2, 660, "EPISODE 1", font("mono", 30), GREY, "c")
    _t(d, W / 2, 712, "ONE WORD IN THE MANUAL", font("mono_bold", 44), RUST, "c")
    return im


# ---------------------------------------------------------------------- C2 ---
C2_FIXES = ["shift the quad",
            "shift the clip window",
            "sweep the projection gain",
            "re-project the corners"]


def c2_corrections(step):
    """Four geometric corrections, struck one at a time.

    Staged on purpose: the list read all at once tells the viewer the punchline
    before the voice gets there, and the punchline is the whole beat.
    """
    im, d = _card("FOUR CORRECTIONS, ALL WORSE",
                  "a gap at a join looks like a geometry error.")
    y = 300
    for i, fix in enumerate(C2_FIXES):
        struck = i < step
        _t(d, L, y, fix, font("mono_bold", 38), RUST if struck else BONE)
        if struck:
            w = d.textbbox((0, 0), fix, font=font("mono_bold", 38))[2]
            d.line([(L - 8, y + 22), (L + w + 8, y - 6)], fill=RUST, width=4)
        y += 76
    if step >= 4:
        _body(d, 690, ["all four made it worse -- in both directions.",
                       "",
                       "~getting symmetrically worse is the signature",
                       "~of a TIMING error, not a position error."])
    return im


# --------------------------------------------------------------------- C2b ---
# The episode's own description says the bug "survived four geometric
# corrections, SIX PRESENTATION MECHANISMS and one expensive CPU workaround",
# and until now only the four were ever shown.  Six attempts named, each with
# the reason it died, from DEVLOG_EP_VDP1_PRESENT.md sec.2 "Acte II".  It sits
# where it belongs chronologically: the last of them was parked on 3 August,
# the day lead-fill went in.
C2B_PRESENTS = [
    ("27 JUN", "draw-gated present",
     "no tearing on hardware -- and the walls trail a field on 30-60 % of them"),
    ("28 JUN", "the full-slSynch branch",
     "slSynch never writes EWLR/EWRR, so VDP1 erases a rectangle of width zero"),
    ("30 JUN", "the NBG1 couple",
     "gated off and never re-armed: it waits on CEF, which Ymir does not model"),
    # the audit records no verdict for this one, only the two commits, so the
    # card says what the commits say and does not invent a reason
    ("02 JUL", "present A/B",
     "added as a pair of toggles, parked the same day"),
    ("03 JUL", "coherent-pair present",
     "holds a walls+floors pair this renderer never builds. pure latency"),
    ("02 AUG", "field-lock",
     "pins the BLIT, not the SWAP. perfect pairing on every capture, holes intact"),
]


def c2b_presents():
    im, d = _card("SIX WAYS TO PRESENT A FRAME, ALL DEAD",
                  "the four corrections were the geometry. these were the timing.")
    y = 250
    for when, what, why in C2B_PRESENTS:
        _t(d, L, y + 6, when, font("mono", 24), GREY)
        _t(d, L + 130, y, what, font("mono_bold", 34), RUST)
        w = d.textbbox((0, 0), what, font=font("mono_bold", 34))[2]
        d.line([(L + 122, y + 20), (L + 130 + w + 8, y - 4)], fill=RUST, width=3)
        _t(d, L + 130, y + 44, why, font("mono", 26), BONE)
        y += 112
    # NOT "none of them changed what the hardware was asked" -- several of them
    # did poke registers, and that line would have been the easy, false one.
    # What the audit actually establishes is structural: the three things that
    # together fix it were never once in the same build.
    _body(d, y + 10, ["~six mechanisms. and a manual present, a fenced blit and a",
                      "~gate that ignores CEF were never once in the same build."],
          size=32, lead=44)
    return im


# ---------------------------------------------------------------------- C6 ---
# ---------------------------------------------------------------------- C3 ---
def c3_counts():
    """The clue, and the one statistic that survived being checked.

    The turning line said "the width of a wall" in the first cut.  That is simply
    false, and the owner caught it: one frame of lateness displaces the picture by
    whatever the VIEW TURNED in that frame, so the error is an angular rate, not a
    distance -- nothing about it is anchored to a wall.  Saying so is not just
    pedantry: because the displacement scales with turn rate, every attempt to
    measure it as a fixed offset disagreed with the previous one, which is exactly
    what made four geometric corrections look plausible for two months.

    Two claims sit here and they do NOT have the same status, so the card marks
    which is which.  "no gaps standing still" is the owner's observation, and it
    is close to a tautology mechanically -- two layers a frame apart are
    identical when nothing moves -- but it is NOT measurable on this clip: OBS
    captured 30 fps over a ~12.5 fps game, so two pictures in three are
    duplicates and "zero pan between captured frames" does not mean "the player
    is still".  The counts underneath ARE measured, and they are threshold
    insensitive: 17.2 to 18.9 distinct pictures a second for any threshold from
    0.4 to 1.0, against a frame-difference distribution that is cleanly bimodal.
    A capture cannot hold more distinct pictures than the game produced frames
    -- unless some pictures are made of pieces of two different frames.
    """
    im, d = _card("IT ONLY HAPPENS WHEN THE PICTURE MOVES",
                  "the same capture, two moments")
    y = _body(d, 276, [
        "~walking forward, one frame of lateness is a few pixels.",
        "~turning, it is whatever the view turned in that one frame.",
        "",
        "so the error is not a distance. it scales with turn RATE --",
        "which is why every measurement of it disagreed with the last.",
        "",
        "getting symmetrically worse in BOTH directions is the",
        "signature of a timing error, not a position error.",
    ])
    y = _body(d, y + 44, [
        "+the game produced about 12.5 pictures a second.",
        "+the capture holds 18.3 distinct ones.",
    ])
    _t(d, 1180, y - 104, "[overlay]", font("mono", 24), GREY)
    _t(d, 1180, y - 52, "[measured]", font("mono", 24), GREY)
    _body(d, y + 24, [
        "the extra ones are neither one game frame nor the next.",
    ])
    return im


# ---------------------------------------------------------------------- C6 ---
def c6_leadfill():
    im, d = _card("THE PATCH, AND ITS PRICE", "lead-fill, 3 August")
    y = _body(d, 300, [
        "the CPU repainted that band itself --",
        "on both SH-2, every frame.",
        "",
        "+it worked.",
        "!it cost 1.5 to 5 frames per second.",
    ])
    # The figure AND its tag were both wrong.  "between one and five" was loose,
    # and "[Ymir]" was the worse half: VDP1_MANUAL_PRESENT_VERDICT.md:146 records
    # 20.0-24.2 fps against 18.4-19.4 for auto+lead-fill, "+1.5 to +5 fps net",
    # from the SECOND CONSOLE CAPTURE SESSION of 19 August.  Tagging a hardware
    # number [Ymir] is the one mislabel this project can least afford.
    #
    # The tag goes at the END of the line it qualifies, not at x=L on the same
    # row: drawing it at the column origin overprinted the line in every cut
    # shipped before 24 August.
    tag = "it cost 1.5 to 5 frames per second."
    _t(d, L + d.textbbox((0, 0), tag, font=font("mono_bold", 34))[2] + 28, y - 42,
       "[HW]", font("mono", 24), GREY)
    _body(d, y + 60, [
        "~a patch that holds takes the pressure off the cause.",
        "~that one took it off for two weeks.",
    ])
    return im


# ---------------------------------------------------------------------- C7 ---
def c7_fafling():
    im, d = _card("THE CAUSE, NAMED FROM OUTSIDE")
    _quote(d, 320, [
        "if you have VDP1 switching its framebuffers",
        "after every vblank, it is not a good idea",
        "in a variable framerate game like Doom",
    ], credit="fafling -- SegaXtreme, 3 August 2026")
    _body(d, 620, ["two months of corrections had never tested that idea.",
                   "",
                   "~one forum post did."])
    return im


# ---------------------------------------------------------------------- C9 ---
def c9_cef():
    im, d = _card("THE COMPLETION FLAG LIED", "CEF -- EDSR bit 1, at 100010H")
    y = _body(d, 296, [
        "finished-or-not was read from the flag VDP1 raises",
        "once it has fetched the end of its command list.",
    ], size=32, lead=46)
    y = _quote(d, y + 30, [
        '"This bit is reset to 0 when the frame buffers are',
        ' changed or when drawing is started"',
        '"If fetch of the draw terminate command matches when',
        ' the frame buffer changes, CEF and BEF might not become 1."',
    ], credit="ST-013 p.52-53", size=26, lead=36)
    _body(d, y + 40, [
        "!the swap that CEF was gating is what resets CEF.",
        "",
        # NOT "31 to 59 %".  That figure exists nowhere in this repository: every
        # source -- DEVLOG_EP_VDP1_PRESENT.md sec.3, dg_saturn.cxx:1720 and 1747,
        # the memory file -- says 30-60 %.  A tighter-looking number with no
        # measurement behind it is the exact thing this audience takes apart.
        "on silicon it latches on 30 to 60 % of frames.   [HW]",
        "+the gate that is never ambiguous is COPR --",
        "+the command-address register.",
    ], size=30, lead=44)
    return im


# --------------------------------------------------------------------- C10 ---
def c10_two_words(step):
    """Two errata on one page of one developer CD.  Staged: diagnosis, then
    prescription, then what they are actually worth.

    The first cut ended this card on "for two months the driver did exactly what
    the manual said, and did nothing", and that overstates.  Checked against the
    record rather than the story: DEVLOG_EP_VDP1_PRESENT.md lists FIVE poisons
    and the errata are one; VDP1_MANUAL_PRESENT_VERDICT.md names the structural
    one -- "manual change + fenced blit + non-CEF gate have never coexisted in
    one build", the fence post-dating every present experiment by a month.  Two
    more sit above the errata as well: every verdict formed on Ymir was void (no
    manual CEF, no LOPR, never overruns), and the CEF gate was OURS -- the manual
    prints that caveat correctly.  The shipped driver's own comment records that
    v1 wrote 0x0003, the CORRECT value, and failed anyway.  The errata are a real
    trap, established as a trap; nothing establishes that this driver fell into
    it.  So the card keeps the two spellings -- they are worth publishing -- and
    states what they are worth, which is one fifth of an answer.
    """
    im, d = _card("TWO WORDS IN THE MANUAL")
    y = 250
    _t(d, L, y, "ST-013 p.38   the diagnosis", font("mono_bold", 30), GREY)
    bad = '"...can be drawn in one FRAME is limited."'
    _t(d, L, y + 48, bad, font("mono", 30), RUST)
    w = d.textbbox((0, 0), bad, font=font("mono", 30))[2]
    d.line([(L - 6, y + 70), (L + w + 6, y + 62)], fill=RUST, width=3)
    _t(d, L + w + 30, y + 48, "developer CD", font("mono", 24), GREY)
    _t(d, L, y + 100, '"...can be drawn in one FIELD is limited."', font("mono", 30), GREEN)
    _t(d, L, y + 148, 'with "frame" it is a tautology. with "field" it is an instruction.',
       font("mono", 26), BONE)

    if step >= 1:
        y2 = 500
        _t(d, L, y2, "ST-013 p.39   the prescription", font("mono_bold", 30), GREY)
        bad2 = '"writing 0 to the VBE, FCM and FCT registers"'
        _t(d, L, y2 + 48, bad2, font("mono", 30), RUST)
        w2 = d.textbbox((0, 0), bad2, font=font("mono", 30))[2]
        d.line([(L - 6, y2 + 70), (L + w2 + 6, y2 + 62)], fill=RUST, width=3)
        _t(d, L + w2 + 30, y2 + 48, "developer CD", font("mono", 24), GREY)
        _t(d, L, y2 + 100, '"writing 0 to the VBE and FCT registers',
           font("mono", 30), GREEN)
        _t(d, L, y2 + 140, ' and 1 to the FCM register"', font("mono", 30), GREEN)
        _t(d, L, y2 + 190, "the first spelling is 0x0000 -- which the facing table",
           font("mono", 26), BONE)
        _t(d, L, y2 + 224, "calls 1-cycle mode.", font("mono", 26), BONE)

    if step >= 2:
        _t(d, L, 826, "any implementation taken from that CD does nothing.",
           font("mono_bold", 34), AMBER)
        _t(d, L, 884, "and that is ONE of five reasons this took two months.",
           font("mono", 28), BONE)
        _t(d, L, 926, "the one that outranks it: manual change, a fenced blit and",
           font("mono_bold", 30), RUST)
        _t(d, L, 966, "a non-CEF gate had never once been in the same build.",
           font("mono_bold", 30), RUST)
    return im


# --------------------------------------------------------------------- C11 ---
def c11_sequence():
    im, d = _card("THE SEQUENCE WITH NO AMBIGUITY", "VBE erase and change")
    y = _body(d, 292, [
        "arm the erase on a fresh V-blank IN",
        "-- TVMR bit 3, FBCR = 0x0003.",
        "the swap lands at the end of that same blank.",
        "",
        "+one write, one edge, nothing left to infer.",
    ], size=32, lead=46)
    _body(d, y + 40, [
        "SEGA's own library ships it -- SBL, SCL_VBLV.C, interval 0xfffe.",
        "and Lobotomy Software's SlaveDriver engine -- PowerSlave,",
        "Duke Nukem 3D, Quake on Saturn -- ran manual frame change",
        "in game, on the same interval.",
        "",
        "~this was not exotic. it is what 1996 shipped.",
    ], size=28, lead=42)
    return im


# --------------------------------------------------------------------- C12 ---
def c12_opens():
    im, d = _card("WHAT IT OPENS")
    y = _quote(d, 250, [
        "VDP1 will be able to draw a lot more than",
        "just what fits in a screen frame",
    ], credit="fafling", size=32, lead=44)
    rows = [
        ("19 AUG", "a monster needed half a percent of the screen to reach VDP1."),
        ("",       "now two tenths -- about ten pixels tall. and 32 a frame, not 16."),
        ("19 AUG", "walls stopped falling back to the CPU for want of budget."),
        ("20 AUG", "sprites reach VDP1 in split-screen. when the queue runs short,"),
        ("",       "the WALLS yield to the things -- a cut thing is drawn by nobody,"),
        ("",       "a cut wall degrades to a flat quad. that asymmetry is the fix."),
        ("20 AUG", "the hardware sky came back in split-screen."),
        ("21 AUG", "24 KB of stack handed back to the heap."),
    ]
    y += 40
    for tag, s in rows:
        if tag:
            _t(d, L, y, tag, font("mono_bold", 24), AMBER)
        _t(d, L + 130, y, s, font("mono", 27), BONE)
        y += 40
    _t(d, L, y + 30, "the limit moved. it did not go away.", font("mono_bold", 36), GREEN)
    return im


# ----------------------------------------------------------------- credits ---
def credits():
    im, d = _card("WITH THANKS")
    rows = [
        ("slygamer",         "most of this month's hardware testing"),
        ("wesker",           "hardware testing on TNT MAP11"),
        # fafling NAMED the cause in a forum post; the driver here is not his
        # work, and "the VDP1 sync fix" read as though it were.
        ("fafling",          "naming the VDP1 sync cause, and deep knowledge"),
        ("",                 "of the retail Saturn Doom"),
        # "my" is the one first-person word in the whole published set, and it
        # is deliberate: the owner asked for it so the errors are owned rather
        # than left floating.  A credit for being corrected is worth nothing if
        # nobody is named as the one who was wrong.
        ("TrekkiesUnite118", "properly corrected several of my wrong claims"),
        ("",                 "on SegaXtreme"),
        ("the Kronos team",  "Runik, fafling, Benjamin Siskoo --"),
        ("",                 "the errata-corrected VDP1 and VDP2 manuals"),
        ("ReyeMe",           "Saturn Ring Library"),
        ("Claude, by Anthropic", "paired on the port"),
        ("the SegaXtreme forum", ""),
        ("doomgeneric / Chocolate Doom", ""),
    ]
    y = 270
    for who, what in rows:
        if who:
            _t(d, L, y, who, font("mono_bold", 30), AMBER)
        if what:
            _t(d, L + 560, y, what, font("mono", 26), BONE)
        y += 48
    d.line([(L, y + 30), (W - L, y + 30)], fill=RULE, width=2)
    _t(d, L, y + 60, "Mimas is a fan project. Doom is id Software's.", font("mono", 26), GREY)
    _t(d, L, y + 98, "Not affiliated with SEGA or id.", font("mono", 26), GREY)
    return im


def build(out):
    os.makedirs(out, exist_ok=True)
    made = [("TITLE.png", title())]
    for i in range(5):
        made.append(("C2_%d.png" % i, c2_corrections(i)))
    made.append(("C3_counts.png", c3_counts()))
    made.append(("C2b_presents.png", c2b_presents()))
    made.append(("C6_leadfill.png", c6_leadfill()))
    made.append(("C7_fafling.png", c7_fafling()))
    made.append(("C9_cef.png", c9_cef()))
    for i in range(3):
        made.append(("C10_%d.png" % i, c10_two_words(i)))
    made.append(("C11_sequence.png", c11_sequence()))
    made.append(("C12_opens.png", c12_opens()))
    made.append(("CREDITS.png", credits()))
    for name, im in made:
        im.save(os.path.join(out, name))
    print("wrote %d cards to %s" % (len(made), out))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    build(ap.parse_args().out)
