"""Side-by-side "versus" YouTube thumbnail from two gameplay frames.

    python mkthumb.py left.png right.png out.png \
        --left "PLAYSTATION" --left-year 1994 \
        --right "SEGA SATURN" --right-year 2026 \
        --caption "SAME LEVEL  -  REAL HARDWARE  -  DEVLOG #3"

Both sides get the SAME brightness lift, because these games are dark (mean luma
around 15/255) and an unlifted thumbnail is unreadable in the feed - but lifting
one side more than the other turns a comparison into a lie.

Writes out.png, out.jpg (what you upload; must stay under 2 MB) and
out_small.png at 320x180, which is the size that actually decides whether the
thumbnail works. Always look at the small one.
"""
import argparse
from PIL import Image, ImageDraw, ImageEnhance
from devlog_style import INK, BONE, AMBER, RUST, GREY, font

W, H = 1280, 720


def lift(im, gamma=0.62, contrast=1.18, sat=1.14):
    lut = [min(255, int(((i / 255.0) ** gamma) * 255 + 0.5)) for i in range(256)]
    im = im.point(lut * 3)
    im = ImageEnhance.Contrast(im).enhance(contrast)
    return ImageEnhance.Color(im).enhance(sat)


def panel(path, cx, out_w, out_h, do_lift=True):
    """Crop a window of the target aspect centred on cx, then scale up.

    cx is in SOURCE pixels. Use the same cx on both sides when the two frames
    are the same room, or the comparison stops being one.
    """
    im = Image.open(path).convert("RGB")
    w, h = im.size
    cw = int(h * out_w / out_h)
    x0 = max(0, min(w - cw, int(cx - cw / 2)))
    crop = im.crop((x0, 0, x0 + cw, h))
    if do_lift:
        crop = lift(crop)
    return crop.resize((out_w, out_h), Image.LANCZOS)


def build(a):
    gap = 8
    pw = (W - gap) // 2
    c = Image.new("RGB", (W, H), INK)
    c.paste(panel(a.left_img, a.left_cx, pw, H, not a.no_lift), (0, 0))
    c.paste(panel(a.right_img, a.right_cx, W - pw - gap, H, not a.no_lift), (pw + gap, 0))
    d = ImageDraw.Draw(c, "RGBA")
    d.rectangle([0, 0, W, 156], fill=(0, 0, 0, 155))
    if a.caption:
        d.rectangle([0, H - 104, W, H], fill=(0, 0, 0, 180))
    d.rectangle([pw, 0, pw + gap, H], fill=AMBER + (255,))

    def ctr(s, f, x, y, fill, sw=5):
        b = d.textbbox((0, 0), s, font=f)
        d.text((x - (b[2] - b[0]) / 2, y), s, font=f, fill=fill,
               stroke_width=sw, stroke_fill=(0, 0, 0))

    big, yr, bot, vsf = (font("mono_bold", 86), font("mono_bold", 42),
                         font("mono_bold", 46), font("mono_bold", 64))
    ctr(a.left, big, pw / 2, 22, BONE)
    ctr(a.right, big, pw + gap + pw / 2, 22, AMBER)
    if a.left_year:
        ctr(a.left_year, yr, pw / 2, 116, GREY)
    if a.right_year:
        ctr(a.right_year, yr, pw + gap + pw / 2, 116, RUST)

    r, cx, cy = 60, W / 2, H / 2 - (10 if a.caption else -40)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(11, 9, 6, 240),
              outline=AMBER + (255,), width=6)
    ctr("VS", vsf, cx, cy - 44, AMBER, sw=0)
    if a.caption:
        ctr(a.caption, bot, W / 2, H - 80, BONE)

    c.save(a.out)
    c.save(a.out.replace(".png", ".jpg"), quality=93, optimize=True)
    c.resize((320, 180), Image.LANCZOS).save(a.out.replace(".png", "_small.png"))
    print("wrote", a.out, "+ .jpg + _small.png")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("left_img"); p.add_argument("right_img"); p.add_argument("out")
    p.add_argument("--left", default="PLAYSTATION"); p.add_argument("--right", default="SEGA SATURN")
    p.add_argument("--left-year", default=""); p.add_argument("--right-year", default="")
    p.add_argument("--left-cx", type=int, default=426); p.add_argument("--right-cx", type=int, default=426)
    p.add_argument("--caption", default="")
    p.add_argument("--no-lift", action="store_true")
    build(p.parse_args())
