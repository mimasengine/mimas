"""Isolate the late VDP1 layer from three consecutive captured frames.

The owner's reading of 11-22-01 @ 1.567 / 1.600 / 1.633:

    1.567  everything agrees
    1.600  the CPU picture has moved, the VDP1 walls have not
    1.633  VDP1 catches up

So "unchanged between 1.567 and 1.600" is the VDP1 layer.  On its own that test
also keeps the weapon, the status bar, the debug overlay and every flat dark
region -- anything that simply did not move.  The third frame removes them:

    late = unchanged(f0, f1) AND changed(f1, f2)

Static furniture is unchanged in BOTH pairs and drops out.  What survives moved
exactly one frame later than the room did, which is the definition of the bug.

Work is done on the native 320x224 grid, not on the 1716x1008 capture: the clip
is H.264 of an upscaled picture, so "identical" is only true after the mosquito
noise around edges is averaged away.  Masks are opened (erode then dilate) to
drop single-pixel speckle, then scaled back up nearest.

What motion CANNOT tell us: a flat dark ceiling looks the same whoever drew it.
Roughly four fifths of this view is textureless, so it stays unassigned -- the
masks are evidence about what moved, not a full layer separation.

    python isolate_vdp1.py [t0 t1 t2]        default 01.567 01.600 01.633
"""
import os
import subprocess
import sys

import numpy as np

FRAMES = r"C:\Users\pcico\Videos\mimas-devlog1\frames\all-2201"
OUT = r"C:\Users\pcico\Videos\mimas-devlog1\isolate"
SW, SH = 320, 224          # native Saturn grid
CW, CH = 1716, 1008        # the capture crop these frames are in
SAME, MOVED = 14, 26       # per-pixel thresholds, mean abs over RGB
VIEW0, VIEW1 = 9, 200      # rows of the 3D view: below the overlay, above the bar

ts = sys.argv[1:4] if len(sys.argv) >= 4 else ["01.567", "01.600", "01.633"]


def load(tag):
    p = os.path.join(FRAMES, f"t{tag}.png")
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", p, "-vf", f"scale={SW}:{SH}:flags=area",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE).stdout
    return np.frombuffer(raw, np.uint8).reshape(SH, SW, 3).astype(np.int16)


def save(arr, name):
    p = os.path.join(OUT, name)
    subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{SW}x{SH}", "-i", "-", "-vf", f"scale={CW}:{CH}:flags=neighbor",
         "-frames:v", "1", "-y", p],
        input=arr.astype(np.uint8).tobytes())
    return p


def shift3(m, dy, dx):
    o = np.zeros_like(m)
    ys = slice(max(dy, 0), SH + min(dy, 0))
    yd = slice(max(-dy, 0), SH + min(-dy, 0))
    xs = slice(max(dx, 0), SW + min(dx, 0))
    xd = slice(max(-dx, 0), SW + min(-dx, 0))
    o[yd, xd] = m[ys, xs]
    return o


def opening(m):
    """3x3 erode then 3x3 dilate: single-pixel speckle dies, slabs survive."""
    e = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            e &= shift3(m, dy, dx)
    d = e.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            d |= shift3(e, dy, dx)
    return d


def match(a, b, mask, span=24):
    """Offset s minimising |b(x) - a(x-s)| over the mask footprint.

    Correlating column sums is fine for a whole-frame pan but useless for a
    small region: the profile is swamped by everything around it.  Matching the
    masked pixels directly answers "how far did THIS content travel".
    """
    ga, gb = a.mean(axis=2), b.mean(axis=2)
    best, bs = 1e18, 0
    for s in range(-span, span + 1):
        sa = np.roll(ga, s, axis=1)
        m = mask.copy()
        if s > 0:
            m[:, :s] = False
        elif s < 0:
            m[:, s:] = False
        if m.sum() < 200:
            continue
        c = float(np.abs(gb[m] - sa[m]).mean())
        if c < best:
            best, bs = c, s
    return bs


os.makedirs(OUT, exist_ok=True)
f0, f1, f2 = (load(t) for t in ts)

d01 = np.abs(f1 - f0).mean(axis=2)
d12 = np.abs(f2 - f1).mean(axis=2)

view = np.zeros((SH, SW), bool)
view[VIEW0:VIEW1] = True

late = opening((d01 <= SAME) & (d12 >= MOVED) & view)   # held, then moved -> VDP1
room = opening((d01 >= MOVED) & view)                   # moved with the camera -> CPU
still = (d01 <= SAME) & (d12 <= SAME) & view            # never moved -> weapon, HUD, flats

px = view.sum()
print(f"frames  {ts[0]}  {ts[1]}  {ts[2]}")
for name, m in (("late (VDP1)", late), ("moved (CPU)", room), ("flat/static", still)):
    print(f"  {name:14s} {m.sum():6d} px  {100.0 * m.sum() / px:5.1f} % of the view")

# the numbers that make the claim checkable, in native Saturn pixels
r01 = match(f0, f1, room)
r12 = match(f1, f2, room)
l01 = match(f0, f1, late)
l12 = match(f1, f2, late)
print(f"  room  moved {r01:+3d} px over {ts[0]}->{ts[1]},  {r12:+3d} px over {ts[1]}->{ts[2]}")
print(f"  late  moved {l01:+3d} px over {ts[0]}->{ts[1]},  {l12:+3d} px over {ts[1]}->{ts[2]}")

save(np.where(late[..., None], f0, 0), "1-vdp1-late-isolated.png")
chk = f0.copy()
chk[late] = (255, 0, 255)
save(chk, "2-vdp1-late-magenta.png")
save(np.where(room[..., None], f0, 0), "3-cpu-moved-isolated.png")
key = np.zeros_like(f0)
key[still] = (60, 60, 60)
key[room] = (0, 220, 0)
key[late] = (255, 0, 255)
save(key, "4-key.png")

# the mask cut on f0, pinned onto all three frames: it must stay put while the
# room slides under it, then release.  This is the C1 card, in three stills.
for tag, f in (("f0", f0), ("f1", f1), ("f2", f2)):
    o = f.copy()
    o[late] = (0.35 * o[late] + 0.65 * np.array([255, 0, 255])).astype(np.int16)
    save(o, f"5-pinned-{tag}.png")

print("->", OUT)
