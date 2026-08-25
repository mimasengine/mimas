"""Full-screen 1920x1080 cards for the devlog cut: title, table, list.

Cards are STATIC on purpose. The Tethys ep6 cut tried ffmpeg `zoompan` for a
slow push and it cost 17.8 MB for a ten-second card and minutes of encode; the
same card as a still is under 1 MB and encodes instantly. Motion on a card buys
nothing at 1080p60.

    python mkcard.py --demo out/            # renders one of each, to look at

Then hold each PNG for its duration with ffmpeg:

    ffmpeg -loop 1 -i card.png -t 22 -r 60 -c:v libx264 -preset medium -crf 18 \
           -pix_fmt yuv420p -y seg.mp4
"""
import argparse, os
from PIL import Image, ImageDraw, ImageEnhance
from devlog_style import W, H, INK, BONE, AMBER, RUST, GREEN, GREY, RULE, font


def canvas(backdrop=None, dim=0.30):
    """Blank page, or a darkened gameplay frame behind the type.

    A backdrop must be picked from a CLEAN frame - check it for whatever
    artefact the build has, or the card ships the bug it is talking about.
    """
    im = Image.new("RGB", (W, H), INK)
    if backdrop:
        bg = Image.open(backdrop).convert("RGB").resize((W, H), Image.LANCZOS)
        im = Image.blend(im, ImageEnhance.Brightness(bg).enhance(dim), 0.85)
    return im


def _text(d, x, y, s, f, fill, anchor="l"):
    b = d.textbbox((0, 0), s, font=f)
    w = b[2] - b[0]
    if anchor == "c":
        x -= w / 2
    elif anchor == "r":
        x -= w
    d.text((x, y), s, font=f, fill=fill)
    return w


def title_card(title, subtitle=None, footer=None, backdrop=None):
    im = canvas(backdrop)
    d = ImageDraw.Draw(im)
    _text(d, W / 2, 430, title, font("mono_bold", 96), AMBER, "c")
    if subtitle:
        _text(d, W / 2, 556, subtitle, font("mono", 40), BONE, "c")
    if footer:
        _text(d, W / 2, H - 130, footer, font("mono", 30), GREY, "c")
    return im


def table_card(head, sub, col_a, col_b, rows, big=None, footer=None):
    """Two-column comparison. `rows` is [(label, a, b), ...].

    Criterion right-aligned into the gutter, the two columns left-aligned, so
    the eye reads down each column instead of across each row.
    """
    im = canvas()
    d = ImageDraw.Draw(im)
    xl, xa, xb = 560, 620, 1180
    _text(d, W / 2, 171, head, font("mono_bold", 44), AMBER, "c")
    if sub:
        _text(d, W / 2, 227, sub, font("mono", 28), GREY, "c")
    _text(d, xa, 301, col_a, font("mono", 28), GREY)
    _text(d, xb, 301, col_b, font("mono", 28), GREY)
    if big:
        _text(d, xa, 343, big[0], font("mono_bold", 60), BONE)
        _text(d, xb, 343, big[1], font("mono_bold", 60), RUST)
        d.line([(150, 441), (W - 150, 441)], fill=RULE, width=2)
        y = 485
    else:
        y = 380
    for label, a, b in rows:
        _text(d, xl, y, label, font("mono", 28), GREY, "r")
        _text(d, xa, y, a, font("mono", 28), BONE)
        _text(d, xb, y, b, font("mono", 28), RUST)
        y += 58
    if footer:
        _text(d, W / 2, max(y + 60, 885), footer, font("mono", 28), GREY, "c")
    return im


def list_card(head, lines, backdrop=None, colour=RUST):
    """Heading plus a flat list. `lines` may be strings or (text, colour)."""
    im = canvas(backdrop)
    d = ImageDraw.Draw(im)
    _text(d, 150, 210, head, font("mono_bold", 44), AMBER)
    y = 310
    for ln in lines:
        s, c = ln if isinstance(ln, tuple) else (ln, colour)
        _text(d, 150, y, s, font("mono", 32), c)
        y += 52
    return im


def credits_card(head, entries):
    """`entries` is [(name, [role line, ...]), ...]; roles right-aligned into
    the name, which is the layout that stops long role text fighting the name."""
    im = canvas()
    d = ImageDraw.Draw(im)
    _text(d, W / 2, 340, head, font("mono", 32), AMBER, "c")
    y = 452
    for name, roles in entries:
        for i, r in enumerate(roles):
            _text(d, 1186, y + i * 34, r, font("mono", 26), GREY, "r")
        _text(d, 1266, y + (len(roles) - 1) * 17, name, font("mono_bold", 40), BONE)
        y += 34 * len(roles) + 70
    return im


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", metavar="OUTDIR")
    a = ap.parse_args()
    if not a.demo:
        ap.error("nothing to do; pass --demo OUTDIR")
    os.makedirs(a.demo, exist_ok=True)
    title_card("MIMAS", "Doom on the Sega Saturn",
               "captured on real hardware").save(os.path.join(a.demo, "demo_title.png"))
    table_card("WHAT WE JUST WATCHED", "the same room on both machines",
               "PLAYSTATION  1997", "SEGA SATURN  2026",
               [("frame rate", "30 fps, steady", "14.6 to 30 fps"),
                ("screen change", "instant", "1.9 s of black, every time")],
               big=("13.0 s", "20.0 s"),
               footer="two things to fix.  both measured, neither guessed."
               ).save(os.path.join(a.demo, "demo_table.png"))
    list_card("WHAT'S LEFT",
              [("loading: 1.9 s a screen", RUST),
               ("frame rate: 14.6 fps at worst", RUST),
               ("all of it measured on the real machine", GREEN)]
              ).save(os.path.join(a.demo, "demo_list.png"))
    credits_card("WITH", [("slygamer", ["round after round of hardware testing:",
                                        "dozens of builds, dozens of captures"])]
                 ).save(os.path.join(a.demo, "demo_credits.png"))
    print("wrote four demo cards to", a.demo)
