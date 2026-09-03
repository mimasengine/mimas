#!/usr/bin/env python3
"""Offline verifier for core/r_bsp.c psw_leaf_poly (Mimas psw-world round 25).

Replicates the SH-2 fixed-point builder EXACTLY (psw_clip_line shrink loop,
C truncating division, t clamp, psw_poly_shave, caps) and compares every leaf
polygon against the exact rational truth (map bbox clipped by all ancestor
node half-planes + the leaf's seg half-planes, no caps, no fixed point).

Outputs per map: leaves OVERSIZED (vertices violating a true half-plane by
more than EPS map units -> red parasite tiles) and UNDERSIZED (builder area
< truth area -> hole class / the triangle).
"""
import struct, sys, math
from fractions import Fraction

WAD = sys.argv[1] if len(sys.argv) > 1 else "cd/data/DOOM1.WAD"
FRACUNIT = 1 << 16
CLIP_VMAX = 40
POLY_VMAX = 20
PATH_MAX = 96
NF_SUBSECTOR = 0x8000

def read_wad(path):
    f = open(path, 'rb')
    ident, num, diroff = struct.unpack('<4sII', f.read(12))
    f.seek(diroff)
    lumps = []
    for _ in range(num):
        fp, sz, name = struct.unpack('<II8s', f.read(16))
        lumps.append((name.rstrip(b'\0').decode(), fp, sz))
    return f, lumps

def map_lumps(lumps, mapname):
    for i, (n, fp, sz) in enumerate(lumps):
        if n == mapname:
            out = {}
            for j in range(i + 1, min(i + 11, len(lumps))):
                out[lumps[j][0]] = (lumps[j][1], lumps[j][2])
            return out
    return None

def load(f, entry, fmt):
    fp, sz = entry
    f.seek(fp)
    data = f.read(sz)
    n = sz // struct.calcsize(fmt)
    return [struct.unpack_from(fmt, data, k * struct.calcsize(fmt)) for k in range(n)]

# ---- C arithmetic helpers (GCC sh2: arithmetic >>, truncating /) -------------
def c_div(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q

def clip_line_fixed(L, pts):
    """psw_clip_line verbatim."""
    ox, oy, dx, dy = L
    n = len(pts)
    if n < 3:
        return []
    out = []
    ca = dx * (pts[0][1] - oy) - dy * (pts[0][0] - ox)
    for i in range(n):
        j = (i + 1) % n
        cb = dx * (pts[j][1] - oy) - dy * (pts[j][0] - ox)
        if ca <= 0 and len(out) < CLIP_VMAX:
            out.append(pts[i])
        if (ca <= 0) != (cb <= 0):
            d = ca - cb
            if d != 0 and len(out) < CLIP_VMAX:
                na, nd = ca, d
                while na >= (1 << 46) or na <= -(1 << 46):
                    na >>= 8
                    nd >>= 8
                t = c_div(na << 16, nd) if nd else 0
                if t < 0: t = 0
                elif t > FRACUNIT: t = FRACUNIT
                bx = pts[i][0] + (((pts[j][0] - pts[i][0]) * t) >> 16)
                by = pts[i][1] + (((pts[j][1] - pts[i][1]) * t) >> 16)
                out.append((bx, by))
        ca = cb
    return out

def poly_shave(pts, cap):
    pts = list(pts)
    while len(pts) > cap:
        best, bestc = 0, -1
        n = len(pts)
        for i in range(n):
            p = (i - 1) % n
            j = (i + 1) % n
            c = (pts[i][0] - pts[p][0]) * (pts[j][1] - pts[i][1]) \
              - (pts[i][1] - pts[p][1]) * (pts[j][0] - pts[i][0])
            c = abs(c)
            if bestc < 0 or c < bestc:
                bestc, best = c, i
        del pts[best]
    return pts

def clip_line_exact(L, pts):
    """Same keep rule with exact rationals, no caps."""
    ox, oy, dx, dy = L
    n = len(pts)
    if n < 3:
        return []
    out = []
    def cr(p):
        return dx * (p[1] - oy) - dy * (p[0] - ox)
    ca = cr(pts[0])
    for i in range(n):
        j = (i + 1) % n
        cb = cr(pts[j])
        if ca <= 0:
            out.append(pts[i])
        if (ca <= 0) != (cb <= 0):
            t = Fraction(ca, ca - cb)
            bx = pts[i][0] + (pts[j][0] - pts[i][0]) * t
            by = pts[i][1] + (pts[j][1] - pts[i][1]) * t
            out.append((bx, by))
        ca = cb
    return out

def area2(pts):
    a = 0
    for i in range(len(pts)):
        j = (i + 1) % len(pts)
        a += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1]
    return a  # signed, CCW positive (y up)

def check_map(f, lumps, name, report):
    ml = map_lumps(lumps, name)
    if not ml or 'NODES' not in ml:
        return
    verts = load(f, ml['VERTEXES'], '<hh')
    segs = load(f, ml['SEGS'], '<HHhhhh')          # v1 v2 angle linedef side offset
    ssecs = load(f, ml['SSECTORS'], '<hh')          # numsegs firstseg
    nodes = load(f, ml['NODES'], '<hhhh8hHH')       # x y dx dy bbox[8] child0 child1
    V = [(x << 16, y << 16) for (x, y) in verts]
    bx0 = min(v[0] for v in V) - (128 << 16)
    by0 = min(v[1] for v in V) - (128 << 16)
    bx1 = max(v[0] for v in V) + (128 << 16)
    by1 = max(v[1] for v in V) + (128 << 16)
    base = [(bx0, by0), (bx1, by0), (bx1, by1), (bx0, by1)]

    results = {}
    path = []

    def leaf(num):
        # fixed-point builder
        pts = list(base)
        for L in path:
            pts = clip_line_fixed(L, pts)
            if len(pts) < 3:
                break
            if len(pts) > CLIP_VMAX - 6:
                pts = poly_shave(pts, CLIP_VMAX - 6)
        nseg, fseg = ssecs[num]
        seglines = []
        if len(pts) >= 3:
            for k in range(nseg):
                s = segs[fseg + k]
                v1, v2 = V[s[0]], V[s[1]]
                L = (v1[0], v1[1], v2[0] - v1[0], v2[1] - v1[1])
                seglines.append(L)
                pts = clip_line_fixed(L, pts)
                if len(pts) < 3:
                    break
                if len(pts) > CLIP_VMAX - 6:
                    pts = poly_shave(pts, CLIP_VMAX - 6)
        else:
            for k in range(nseg):
                s = segs[fseg + k]
                v1, v2 = V[s[0]], V[s[1]]
                seglines.append((v1[0], v1[1], v2[0] - v1[0], v2[1] - v1[1]))
        if len(pts) > POLY_VMAX:
            pts = poly_shave(pts, POLY_VMAX)
        if len(pts) < 3:
            pts = []
        # exact truth
        tp = list(base)
        for L in list(path) + seglines:
            tp = clip_line_exact(L, tp)
            if len(tp) < 3:
                tp = []
                break
        results[num] = (pts, tp, list(path) + seglines)

    def walk(bspnum):
        if bspnum & NF_SUBSECTOR:
            leaf(bspnum & (NF_SUBSECTOR - 1))
            return
        nd = nodes[bspnum]
        x, y, dx, dy = nd[0] << 16, nd[1] << 16, nd[2] << 16, nd[3] << 16
        if len(path) < PATH_MAX:
            path.append((x, y, dx, dy))
            walk(nd[12])
            path[-1] = (x, y, -dx, -dy)
            walk(nd[13])
            path.pop()
        else:
            walk(nd[12])
            walk(nd[13])

    sys.setrecursionlimit(10000)
    walk(len(nodes) - 1)

    over, under, bad = [], [], []
    for num, (pts, tp, lines) in sorted(results.items()):
        if not tp:
            if pts:
                over.append((num, 'truth-empty but builder emitted', len(pts)))
            continue
        if not pts:
            bad.append((num, 'builder n<3, truth has area',
                        float(abs(area2(tp))) / (1 << 32) / (64 * 64)))
            continue
        # oversize: max outward violation of any builder vertex vs ANY true half-plane
        maxviol = 0.0
        for (vx, vy) in pts:
            for (ox, oy, dx, dy) in lines:
                cr = dx * (vy - oy) - dy * (vx - ox)
                if cr > 0:
                    h = math.hypot(dx, dy)
                    if h > 0:
                        d = (cr / h) / 65536.0
                        if d > maxviol:
                            maxviol = d
        ab = abs(area2(pts)) / float(1 << 32)          # map units^2
        at = float(abs(area2(tp))) / (1 << 32)
        if maxviol > 2.0:
            over.append((num, 'viol %.1fu' % maxviol, 'areas b=%.0f t=%.0f' % (ab, at)))
        if at > 64.0 and ab < at * 0.95:
            under.append((num, 'area b=%.0f vs t=%.0f (%.0f%%)' % (ab, at, 100 * ab / at)))
        aw = area2(pts)
        if aw < 0:
            bad.append((num, 'builder poly CW (wpos flip risk)', ab))
    print('== %s: %d subs, %d nodes ==' % (name, len(ssecs), len(nodes)))
    print('   oversize >2u : %d' % len(over))
    for o in over[:12]: print('     sub', o)
    print('   undersize <95%%: %d' % len(under))
    for u in under[:12]: print('     sub', u)
    print('   bad (n<3 with area / CW): %d' % len(bad))
    for b in bad[:12]: print('     sub', b)
    report[name] = (over, under, bad)

f, lumps = read_wad(WAD)
report = {}
for e in range(1, 2):
    for m in range(1, 10):
        check_map(f, lumps, 'E%dM%d' % (e, m), report)
