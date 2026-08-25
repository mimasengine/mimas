"""Shared visual language for the Mimas devlog videos.

Ported from the Tethys ep6 cut (2026-08-20), where it was proven end to end on a
six-minute 1080p60 master. Everything here is deliberately boring: static cards,
no animation, no external assets. See docs/DEVLOG_VIDEO.md for the method.
"""
import os

W, H = 1920, 1080

# --- palette -----------------------------------------------------------------
# RGB tuples for PIL; the &H00BBGGRR twins for ASS are in annot_template.ass.
INK   = (11,  9,  6)     # page black, NOT pure black - keeps banding down
BONE  = (237, 228, 211)  # body text
AMBER = (218, 148,  47)  # headings, accents, "us"
RUST  = (200, 106,  79)  # problems, debts, punchlines
GREEN = ( 94, 174, 136)  # wins, things that got better
GREY  = (163, 155, 140)  # secondary / "them"
RULE  = ( 74,  64,  48)  # hairlines and frame borders

# --- fonts -------------------------------------------------------------------
# Consolas is the Tethys face and reads well at 320px-wide thumbnail scale.
# Mimas also ships tools/fonts/DooM.ttf, which is the right face for a Doom port
# on TITLES ONLY - it has no lowercase worth reading at body size.
_HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = {
    "mono":      [os.path.join(_HERE, "consola.ttf"),  r"C:\Windows\Fonts\consola.ttf"],
    "mono_bold": [os.path.join(_HERE, "consolab.ttf"), r"C:\Windows\Fonts\consolab.ttf"],
    "doom":      [os.path.join(_HERE, "..", "fonts", "DooM.ttf")],
}


def font(kind, size):
    from PIL import ImageFont
    for path in FONTS[kind]:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise FileNotFoundError(
        "no font for %r; copy consola.ttf/consolab.ttf next to this script" % kind)
