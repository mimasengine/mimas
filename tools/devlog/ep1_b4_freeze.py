"""Locate the gap to frame in block 4a, by measurement rather than by eye.

Block 4a holds one July frame still and puts a rust box on the artefact.  Which
frame, and where the box goes, are both decided here rather than guessed:

  * WHICH -- the owner judged 11-22-01 by hand and named t3.000, t3.767 and
    t3.900 as the only three frames in that window that actually break; the brown
    slab in t3.067/t3.100 is a real wall.  Only those three are considered.
  * WHERE -- the same three-frame test as isolate_vdp1.py: a region that held
    still while the room moved, and then moved one frame later, is the late VDP1
    layer.  The largest connected component of that mask is the box.

Output is printed as pixel coordinates in the 1716x1008 capture crop, which is
what ep1_build.py draws with.

    python ep1_b4_freeze.py
"""
import os
import subprocess
import sys

import numpy as np

FRAMES = r"C:\Users\pcico\Videos\mimas-devlog1\frames\all-2201"
SW, SH = 320, 224
CW, CH = 1716, 1008
SAME, MOVED = 14, 26
VIEW0, VIEW1 = 9, 200
FPS = 30.0
CANDIDATES = [3.000, 3.767, 3.900]      # the owner's three, and only those


def load(t):
    n = int(round(t * FPS))
    p = os.path.join(FRAMES, "t%06.3f.png" % (n / FPS))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", p, "-vf", "scale=%d:%d:flags=area" % (SW, SH),
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE).stdout
    return np.frombuffer(raw, np.uint8).reshape(SH, SW, 3).astype(np.int16), p


def shift3(m, dy, dx):
    o = np.zeros_like(m)
    o[max(-dy, 0):SH + min(-dy, 0), max(-dx, 0):SW + min(-dx, 0)] = \
        m[max(dy, 0):SH + min(dy, 0), max(dx, 0):SW + min(dx, 0)]
    return o


def opening(m):
    e = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            e &= shift3(m, dy, dx)
    d = e.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            d |= shift3(e, dy, dx)
    return d


def biggest(mask):
    """Largest 8-connected component, as (n, y0, y1, x0, x1).  No scipy here."""
    seen = np.zeros_like(mask)
    best = (0, 0, 0, 0, 0)
    ys, xs = np.nonzero(mask)
    for sy, sx in zip(ys, xs):
        if seen[sy, sx]:
            continue
        stack, cells = [(sy, sx)], []
        seen[sy, sx] = True
        while stack:
            y, x = stack.pop()
            cells.append((y, x))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < SH and 0 <= nx < SW and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        if len(cells) > best[0]:
            a = np.array(cells)
            best = (len(cells), a[:, 0].min(), a[:, 0].max(), a[:, 1].min(), a[:, 1].max())
    return best


view = np.zeros((SH, SW), bool)
view[VIEW0:VIEW1] = True

for t in CANDIDATES:
    step = 1.0 / FPS
    (f0, _), (f1, p1), (f2, _) = load(t - step), load(t), load(t + step)
    d01 = np.abs(f1 - f0).mean(axis=2)
    d12 = np.abs(f2 - f1).mean(axis=2)
    late = opening((d01 <= SAME) & (d12 >= MOVED) & view)
    n, y0, y1, x0, x1 = biggest(late)
    if not n:
        print("t%.3f  no late component" % t)
        continue
    # native -> capture crop, with a 3 px pad so the box does not touch content
    sx, sy = CW / float(SW), CH / float(SH)
    bx0, by0 = int((x0 - 3) * sx), int((y0 - 3) * sy)
    bw, bh = int((x1 - x0 + 7) * sx), int((y1 - y0 + 7) * sy)
    print("t%.3f  late %4d px  biggest %4d px  native x%d..%d y%d..%d" %
          (t, late.sum(), n, x0, x1, y0, y1))
    print("        capture box  x=%d y=%d w=%d h=%d   (%s)" % (bx0, by0, bw, bh, os.path.basename(p1)))
