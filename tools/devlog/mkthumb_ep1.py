"""Devlog #1 thumbnail: the held frame, with the sync holes boxed.

    python mkthumb_ep1.py OUT.png

mkthumb.py builds the side-by-side "versus" thumbnail, which is the wrong shape
for this episode: there is no comparison to make in a still, and the one image
this episode owns is the frame the block-4 freeze holds -- three holes where VDP1
had not finished, boxed in rust.  The owner asked for that frame, and it is the
right ask: the thumbnail then shows the actual subject instead of a title card.

The boxes are the SAME rectangles the video draws, recovered from the owner's
painted mask (frames/all-2201/t03.767_green.png) by connected components; they
live in ep1_build.B4_BOXES in 1920x1080 space and are mapped back here.  If they
ever move in the video they must move here -- a thumbnail that boxes a different
thing than the video does is worse than no boxes.

Writes OUT.png, OUT.jpg (upload this, must stay under 2 MB) and OUT_small.png at
320x180.  Always judge it at 320x180: that is the size that decides.
"""
import argparse
from PIL import Image, ImageDraw, ImageEnhance
from devlog_style import INK, BONE, AMBER, RUST, GREY, font

W, H = 1280, 720
SRC = "C:/Users/pcico/Videos/mimas-devlog1/frames/all-2201/t03.767.png"

# in the 1716x1008 capture crop, padded 8 px -- see ep1_build.B4_BOXES
HOLES = [(0, 44, 163, 730), (883, 241, 170, 186), (644, 248, 85, 112)]

# A 16:9 window on the capture crop, and NOT the whole frame.  Full width the
# holes are 0.75x and the frame also carries a debug row at the top and a status
# bar at the bottom -- at 320x180, which is the size that decides, that is three
# competing things.  1300x731 keeps every hole (they span y 44 to 774), drops
# both bars, and lands near 1:1 in the thumbnail instead of shrinking.
CROP = (0, 40, 1300, 731)          # x, y, w, h in the 1716x1008 capture crop


def lift(im, gamma=0.66, contrast=1.20, sat=1.12):
    """These frames average about 15/255. Unlifted, the thumbnail is a black
    rectangle in the feed."""
    lut = [min(255, int(((i / 255.0) ** gamma) * 255 + 0.5)) for i in range(256)]
    im = im.point(lut * 3)
    im = ImageEnhance.Contrast(im).enhance(contrast)
    return ImageEnhance.Color(im).enhance(sat)


def build(a):
    src = Image.open(a.src).convert("RGB")
    cx, cy, cw, ch = CROP
    frame = lift(src.crop((cx, cy, cx + cw, cy + ch))).resize((W, H), Image.LANCZOS)
    c = Image.new("RGB", (W, H), INK)
    c.paste(frame, (0, 0))
    d = ImageDraw.Draw(c, "RGBA")

    kx, ky = W / float(cw), H / float(ch)
    for x, y, w, h in HOLES:
        bx, by = (x - cx) * kx, (y - cy) * ky
        d.rectangle([bx, by, bx + w * kx, by + h * ky],
                    outline=RUST + (255,), width=7)

    # A plate behind the words, not a stroke: at 320x180 a stroked headline over
    # a busy frame turns to mud, and this frame is busy exactly where it matters.
    d.rectangle([0, 0, W, 150], fill=(0, 0, 0, 165))
    d.rectangle([0, H - 92, W, H], fill=(0, 0, 0, 185))

    def put(s, f, x, y, fill):
        d.text((x, y), s, font=f, fill=fill, stroke_width=4, stroke_fill=(0, 0, 0))

    put(a.head, font("mono_bold", 92), 40, 22, AMBER)
    put(a.sub, font("mono_bold", 40), 40, H - 74, BONE)
    tag = "DEVLOG #1"
    tw = d.textbbox((0, 0), tag, font=font("mono_bold", 40))[2]
    put(tag, font("mono_bold", 40), W - 40 - tw, H - 74, RUST)

    c.save(a.out)
    c.save(a.out.replace(".png", ".jpg"), quality=93, optimize=True)
    c.resize((320, 180), Image.LANCZOS).save(a.out.replace(".png", "_small.png"))
    print("wrote", a.out, "+ .jpg + _small.png")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--src", default=SRC)
    p.add_argument("--head", default="ONE FRAME BEHIND")
    p.add_argument("--sub", default="SEGA SATURN  -  REAL HARDWARE")
    build(p.parse_args())
