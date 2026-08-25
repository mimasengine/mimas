"""Same detector, streamed so a 5-minute clip fits in memory."""
import subprocess, sys, numpy as np

src, tag = sys.argv[1], sys.argv[2]
W, H, FPS, MAX = 240, 115, 30.0, 24
cmd = ["ffmpeg", "-v", "error", "-i", src,
       "-vf", f"crop=1716:1008:88:38,crop=1716:824:0:76,format=gray,scale={W}:{H}:flags=area",
       "-f", "rawvideo", "-pix_fmt", "gray", "-"]
raw = subprocess.run(cmd, stdout=subprocess.PIPE).stdout
F = np.frombuffer(raw, np.uint8).reshape(-1, H, W)          # keep uint8
n = len(F)
print(f"{tag}: {n} frames", file=sys.stderr)

prof = F.mean(axis=1).astype(np.float32)
prof -= prof.mean(axis=1, keepdims=True)
shift = np.zeros(n - 1, np.float32)
for i in range(n - 1):
    a, b = prof[i], prof[i + 1]
    best, bs = -1e18, 0
    for s in range(-MAX, MAX + 1):
        v = float(np.dot(a[s:], b[:W - s])) if s >= 0 else float(np.dot(a[:W + s], b[-s:]))
        if v > best: best, bs = v, s
    shift[i] = bs
pan = np.abs(0.5 * (shift[:-1] + shift[1:]))

area = np.zeros(n - 2, np.float32); dl = np.zeros(n - 2, np.float32)
STEP = 512
for k in range(0, n - 2, STEP):
    j = min(k + STEP, n - 2)
    a = F[k:j].astype(np.int16); b = F[k+1:j+1].astype(np.int16); c = F[k+2:j+2].astype(np.int16)
    E = np.abs(b - a) + np.abs(c - b) - np.abs(c - a)
    area[k:j] = (E > 25).mean(axis=(1, 2))
    dl[k:j] = np.abs(b.mean(axis=(1, 2)) - 0.5 * (a.mean(axis=(1, 2)) + c.mean(axis=(1, 2))))

ok = (area < 0.30) & (dl < 5.0) & (pan >= 4.0)
score = np.where(ok, area * np.minimum(pan, 16.0), 0.0)
t = (np.arange(len(score)) + 1) / FPS
out = []
for i in np.argsort(-score):
    if score[i] <= 0: break
    if any(abs(t[i] - u) < 0.60 for u, *_ in out): continue
    out.append((t[i], score[i], area[i] * 100, pan[i]))
    if len(out) == 24: break
for u, s, ar, p in out:
    print(f"{u:.3f} {s:.2f} {ar:.1f} {p:.1f}")
