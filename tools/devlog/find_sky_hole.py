"""Find ONE big, close hole in an otherwise legible indoor room.

The frames that read best are not the ones with the most damage -- t5.567 has the
largest total hole area of the whole clip and the owner could not tell what he
was looking at.  What explains the bug is a room that reads normally with ONE
region where a wall should be and the VDP2 sky shows instead.

So the score is not area.  It is:

    indoor   the frame is dark overall -- a bright patch is then anomalous,
             not just "the player walked outside"
    sky      pixels bright enough to be SKY1 (rock + sky are mid greys, the
             interior of this level is not)
    compact  the mask survives a 9x9 erosion.  Scattered slivers die; one solid
             hole lives.  Erosion is the cheap stand-in for connected-component
             labelling (no scipy here).
    single   most of the surviving area sits in one place -- measured as the
             fraction of surviving mask inside its own bounding box.  A frame
             broken in five places has a huge box and a low fill.
    lonely   the two neighbouring frames must NOT have it (one-frame event)

Usage:  python find_sky_hole.py <clip.mp4> [n]
"""
import subprocess, sys
import numpy as np

src = sys.argv[1]
TOP = int(sys.argv[2]) if len(sys.argv) > 2 else 24
W, H, FPS = 240, 115, 30.0

cmd = ["ffmpeg", "-v", "error", "-i", src,
       "-vf", f"crop=1716:1008:88:38,crop=1716:824:0:76,format=gray,scale={W}:{H}:flags=area",
       "-f", "rawvideo", "-pix_fmt", "gray", "-"]
raw = subprocess.run(cmd, stdout=subprocess.PIPE).stdout
F = np.frombuffer(raw, np.uint8).reshape(-1, H, W)
n = len(F)
print(f"{n} frames", file=sys.stderr)


def erode(m, k):
    """Binary erosion by a k x k square, as k*2 shifted ANDs."""
    r = k // 2
    out = m.copy()
    for d in range(1, r + 1):
        out[:, d:, :] &= m[:, :-d, :]
        out[:, :-d, :] &= m[:, d:, :]
    m2 = out.copy()
    for d in range(1, r + 1):
        out[:, :, d:] &= m2[:, :, :-d]
        out[:, :, :-d] &= m2[:, :, d:]
    return out


rows = []
STEP = 256
for k in range(0, n, STEP):
    j = min(k + STEP, n)
    f = F[k:j]
    dark = f.mean(axis=(1, 2))                     # how dark the room is
    sky = (f > 110)
    solid = erode(sky, 9)
    area = solid.mean(axis=(1, 2))
    # bounding box fill of the surviving mask -> "is it in ONE place?"
    fill = np.zeros(len(f))
    ys, xs = np.any(solid, axis=2), np.any(solid, axis=1)
    for i in range(len(f)):
        if not ys[i].any():
            continue
        y0, y1 = np.argmax(ys[i]), H - np.argmax(ys[i][::-1])
        x0, x1 = np.argmax(xs[i]), W - np.argmax(xs[i][::-1])
        box = (y1 - y0) * (x1 - x0)
        fill[i] = solid[i].sum() / box if box else 0.0
    for i in range(len(f)):
        rows.append((k + i, dark[i], area[i], fill[i]))

idx = np.array([r[0] for r in rows])
dark = np.array([r[1] for r in rows])
area = np.array([r[2] for r in rows])
fill = np.array([r[3] for r in rows])

# one-frame event: the neighbours must be much cleaner
prev = np.concatenate([[0.0], area[:-1]])
nxt = np.concatenate([area[1:], [0.0]])
lonely = area - np.maximum(prev, nxt)

ok = (dark < 78) & (area > 0.0015) & (fill > 0.14) & (lonely > 0.0004)
score = np.where(ok, area * fill * 100, 0.0)
t = idx / FPS

out = []
for i in np.argsort(-score):
    if score[i] <= 0:
        break
    if any(abs(t[i] - u) < 0.30 for u, *_ in out):
        continue
    out.append((t[i], score[i], area[i] * 100, fill[i], dark[i]))
    if len(out) == TOP:
        break
print("t      score  hole%  fill  dark")
for u, s, a, fi, d in out:
    print(f"{u:6.3f} {s:6.2f} {a:5.2f} {fi:5.2f} {d:5.1f}")
