"""Cut the C1 layers out of the owner's hand-painted zone masks.

The owner painted four flat-colour masks over 11-22-01 @ 1.600 (2026-08-24).
Two things were adjusted after checking them against the source:

  * the red mask is RIGHT as painted: weapon and status bar are BOTH VDP1, and
    both one frame late.  `VDP1_WEAPON 1` and the overlay in this very capture
    reads `WPN vdp1`; the bar goes through vdp1_hud_capture/vdp1_hud_emit, gated
    `sat_local_players == 1 && gamestate == GS_LEVEL` -- every mode, not just
    lowres.  Its own comment names the lateness and shrugs at it: "A status bar
    ticks slowly -> the 1-frame lag is imperceptible."  An earlier version of this
    script split the bar off into the CPU layer; that was wrong and is reverted.

  * a fifth layer is added for the CPU picture: everything nobody painted.  Named
    honestly -- it is "everything else", and it still contains VDP1 walls that
    happened not to open a hole.

Layers, and what each one is for:

    red      weapon + status bar -- VDP1, one field late like all VDP1 content
             here, and correct anyway, because both are anchored to the screen
    magenta  the VDP1 walls -- same chip, same lateness, but these follow the world
    green    the holes -- drawn by nobody, and exactly what lead-fill repainted
    blue     the dominant floor -- VDP2 RBG0 rotation plane, seen through the holes
    cyan     the CPU framebuffer -- ceiling and the other floors

Rendering.  Flat opaque colour reads as a diagram, not as a photograph, so each
layer is TINTED instead: the hue is fixed, the brightness is modulated by the
pixel's own luma, and the floor of that ramp is high enough that a dark region
still shows its colour.  The holes are the test case -- their pixels are the dark
floor showing through, so any scheme that merely brightens them shows nothing.

Outputs, all at capture resolution:
    L{n}-{name}.png   the source pixels of that layer, rest black
    H{n}-{name}.png   that layer at full brightness, rest of the frame dimmed
    T{n}-{name}.png   that layer tinted, rest of the frame dimmed   <- the one to use
    key.png           all layers tinted at once
"""
import os
import subprocess

import numpy as np

FRAME = r"C:\Users\pcico\Videos\mimas-devlog1\frames\all-2201\t01.600.png"
ZONES = r"C:\Users\pcico\Videos\mimas-devlog1\frames\zones"
OUT = r"C:\Users\pcico\Videos\mimas-devlog1\c1"
W, H = 1716, 1008
DIM = 0.55                    # the rest of the frame, so the room stays readable
FLOOR, RAMP = 0.55, 0.60      # tint = colour * (FLOOR + RAMP * luma)

PAINTED = [                                  # what the owner handed over
    ("red",     (255, 0, 0)),
    ("magenta", (255, 0, 255)),
    ("green",   (0, 255, 0)),
    ("blue",    (0, 0, 255)),
]
ORDER = [                                    # light-up order for the card
    ("red",     (255, 0, 0),   "weapon + status bar -- VDP1, late, screen-anchored"),
    ("magenta", (255, 0, 255), "the VDP1 walls -- same chip, same lateness"),
    ("green",   (0, 255, 0),   "the holes -- drawn by nobody"),
    ("blue",    (0, 0, 255),   "the dominant floor -- VDP2 RBG0"),
    ("cyan",    (0, 220, 255), "the CPU picture -- a bitmap VDP2 shows as a layer"),
]
PAINT_LAST = ["cyan", "blue", "red", "magenta", "green"]   # least to most specific


def load(path):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE).stdout
    return np.frombuffer(raw, np.uint8).reshape(H, W, 3).astype(np.int16)


def save(arr, name):
    subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W}x{H}", "-i", "-", "-frames:v", "1", "-y", os.path.join(OUT, name)],
        input=np.clip(arr, 0, 255).astype(np.uint8).tobytes())


os.makedirs(OUT, exist_ok=True)
src = load(FRAME)
total = W * H
luma = (src * np.array([0.299, 0.587, 0.114])).sum(axis=2) / 255.0
ramp = (FLOOR + RAMP * luma)[..., None]


def tint(base, mask, rgb):
    out = base.astype(np.float32).copy()
    out[mask] = (np.array(rgb, np.float32) * ramp)[mask]
    return out


masks = {}
for name, rgb in PAINTED:
    m = load(os.path.join(ZONES, f"t01.600.{name}.png"))
    masks[name] = (m == np.array(rgb, np.int16)).all(axis=2)

hand = np.zeros((H, W), bool)
for name, _ in PAINTED:
    hand |= masks[name]
masks["cyan"] = ~hand                  # everything nobody painted

print(f"frame {os.path.basename(FRAME)}   {W}x{H} = {total} px")
for name, _, what in ORDER:
    m = masks[name]
    print(f"  {name:8s} {m.sum():7d} px  {100.0 * m.sum() / total:5.1f} %   {what}")

# green sits INSIDE blue by construction: a hole is where the floor shows through.
for i, (a, _) in enumerate(PAINTED):
    for b, _ in PAINTED[i + 1:]:
        ov = (masks[a] & masks[b]).sum()
        if not ov:
            continue
        tag = "expected -- holes show the floor" if {a, b} == {"green", "blue"} else "edge slop"
        print(f"  overlap {a}/{b}: {ov} px   ({tag})")

dim = (src * DIM).astype(np.int16)
for i, (name, rgb, _) in enumerate(ORDER, 1):
    m = masks[name]
    save(np.where(m[..., None], src, 0), f"L{i}-{name}.png")
    h = dim.copy()
    h[m] = src[m]
    save(h, f"H{i}-{name}.png")
    save(tint(dim, m, rgb), f"T{i}-{name}.png")

key = dim.astype(np.float32)
for name in PAINT_LAST:
    rgb = next(c for n, c, _ in ORDER if n == name)
    key = tint(key, masks[name], rgb)
save(key, "key.png")
print("->", OUT)
