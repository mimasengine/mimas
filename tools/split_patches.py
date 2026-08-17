#!/usr/bin/env python3
"""
split_patches.py -- cut oversized wall patches into narrow strips, OFFLINE.

WHY (measured on hardware, 2026-08-16)
--------------------------------------
Every 256x128 patch in TNT is **35080 bytes**, and Doom's renderer caches the WHOLE
lump to read one column -- so it needs 35 KB **in one contiguous run**.  The Saturn
zone cannot promise that: `lg` (longest allocatable) was measured between **20 and
38 KB** depending on the scene, because ~366 KB of small unpurgeable PU_STATIC
texture blocks chop the middle of the heap.  When the run is short, R_GetColumn
serves the zero-init placeholder and the surface renders flat or black -- that is
what ate the sky, and at `lg20k` no 256x128 patch fits at all, walls included.

The engine cannot dodge a cliff like that.  The WAD can.

THE TRANSFORM (the owner's idea, and it is free)
------------------------------------------------
Doom composes a texture from patches **per column**.  When patches do not overlap,
R_GenerateLookup marks every column single-patch and R_GetColumn reads straight out
of that patch's lump -- **no composite is ever built**.  So a 256-wide patch can be
replaced by two 128-wide patches side by side inside the SAME 256-wide texture:

    TEXTURE1: SOMEWALL 256x128, patches = [ (x=0, WIDE) ]
      becomes  SOMEWALL 256x128, patches = [ (x=0, WIDE_a), (x=128, WIDE_b) ]

Identical pixels, identical mapping, **zero renderer change** -- and the largest
single lump drops 35 KB -> 18 KB (-> 9 KB at --max-width 64).

The sky already proves the mechanism: TNT's SKY1 is 1024x128 built from four
256-wide patches, and it is R_GetColumn's SINGLE-PATCH branch that serves it
(`r_patch_ovf` moved, `r_composite_ovf` did not).

USAGE
-----
    python tools/split_patches.py in.wad out.wad [--max-width 128] [--dry-run]

Runs before strip_wad.py in the pipeline; both keep every lump 4-byte aligned,
which the cartridge path requires (it reads lumps IN PLACE, and a misaligned
32-bit read returns silent garbage).
"""

import struct, sys, argparse

# ---------------------------------------------------------------- WAD container

class Wad:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.sig, n, off = struct.unpack('<4sii', d[:12])
        self.lumps = []                       # [name, bytes]
        for i in range(n):
            fp, sz, nm = struct.unpack('<ii8s', d[off + i*16 : off + i*16 + 16])
            self.lumps.append([nm.rstrip(b'\0').decode('latin1'), d[fp:fp+sz]])

    def index(self, name):
        for i, (nm, _) in enumerate(self.lumps):
            if nm.upper() == name.upper():
                return i
        return -1

    def get(self, name):
        i = self.index(name)
        return self.lumps[i][1] if i >= 0 else None

    def set(self, name, data):
        i = self.index(name)
        if i < 0:
            raise KeyError(name)
        self.lumps[i][1] = data

    def write(self, path):
        out = bytearray(b'\0' * 12)
        dirents = []
        for nm, data in self.lumps:
            while len(out) & 3:               # 4-byte align: the cart reads in place
                out += b'\0'
            dirents.append((len(out), len(data), nm))
            out += data
        while len(out) & 3:
            out += b'\0'
        diroff = len(out)
        for fp, sz, nm in dirents:
            out += struct.pack('<ii8s', fp, sz, nm.encode('latin1')[:8].ljust(8, b'\0'))
        out[0:12] = struct.pack('<4sii', self.sig, len(dirents), diroff)
        open(path, 'wb').write(bytes(out))

# ------------------------------------------------------------------ patch codec

def patch_header(b):
    w, h, lo, to = struct.unpack('<hhhh', b[:8])
    ofs = struct.unpack('<%di' % w, b[8:8 + 4*w])
    return w, h, lo, to, ofs

def column_bytes(b, start):
    """Return the raw post stream for one column, terminator included."""
    i = start
    while i < len(b) and b[i] != 0xFF:
        length = b[i+1]
        i += 4 + length                        # topdelta,len,pad,pixels[len],pad
    return b[start:i+1]

def split_patch(b, maxw):
    """Yield (origin_x, patch_bytes) strips, each at most maxw columns wide."""
    w, h, lo, to, ofs = patch_header(b)
    cols = [column_bytes(b, o) for o in ofs]
    for x0 in range(0, w, maxw):
        cw = min(maxw, w - x0)
        head = struct.pack('<hhhh', cw, h, lo - x0, to)
        table_end = 8 + 4*cw
        body, table, cur = b'', b'', table_end
        for c in cols[x0:x0+cw]:
            table += struct.pack('<i', cur)
            body  += c
            cur   += len(c)
        yield x0, head + table + body

# ------------------------------------------------------------------ TEXTUREx IO

def parse_textures(b):
    cnt = struct.unpack('<i', b[:4])[0]
    offs = struct.unpack('<%di' % cnt, b[4:4 + 4*cnt])
    out = []
    for o in offs:
        name = b[o:o+8].rstrip(b'\0').decode('latin1')
        masked, w, h, cdir, pc = struct.unpack('<ihhih', b[o+8:o+22])
        patches = [struct.unpack('<hhhhh', b[o+22+i*10 : o+22+i*10+10]) for i in range(pc)]
        out.append([name, masked, w, h, cdir, patches])
    return out

def build_textures(texs):
    cnt = len(texs)
    head = struct.pack('<i', cnt)
    offs, blob, cur = b'', b'', 4 + 4*cnt
    for name, masked, w, h, cdir, patches in texs:
        rec = struct.pack('<8sihhih', name.encode('latin1')[:8].ljust(8, b'\0'),
                          masked, w, h, cdir, len(patches))
        for p in patches:
            rec += struct.pack('<hhhhh', *p)
        offs += struct.pack('<i', cur)
        blob += rec
        cur  += len(rec)
    return head + offs + blob

# ------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--max-width', type=int, default=128,
                    help='widest patch to keep (default 128 -> ~18 KB lumps)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    wad = Wad(a.src)
    pnames = wad.get('PNAMES')
    if pnames is None:
        sys.exit('no PNAMES lump')
    n = struct.unpack('<i', pnames[:4])[0]
    names = [pnames[4+i*8 : 12+i*8].rstrip(b'\0').decode('latin1').upper() for i in range(n)]

    texlumps = [t for t in ('TEXTURE1', 'TEXTURE2') if wad.index(t) >= 0]
    texs = {t: parse_textures(wad.get(t)) for t in texlumps}

    # Which PNAMES entries are too wide?  Only patches actually referenced by a
    # texture are worth splitting -- a sprite is never read column-by-column.
    used = set()
    for t in texlumps:
        for _, _, _, _, _, patches in texs[t]:
            for _, _, pn, _, _ in patches:
                if 0 <= pn < n:
                    used.add(pn)

    plan, newlumps, serial = {}, [], 0
    for pn in sorted(used):
        li = wad.index(names[pn])
        if li < 0:
            continue
        raw = wad.lumps[li][1]
        if len(raw) < 8:
            continue
        w = struct.unpack('<h', raw[:2])[0]
        if w <= a.max_width:
            continue
        strips = []
        for x0, data in split_patch(raw, a.max_width):
            nm = 'MSPL%04d' % serial; serial += 1
            strips.append((x0, nm, len(data)))
            newlumps.append((nm, data))
        plan[pn] = strips
        print('  %-8s %4d px %7d B -> %d x %d px, largest %d B'
              % (names[pn], w, len(raw), len(strips), a.max_width,
                 max(s[2] for s in strips)))

    if not plan:
        print('nothing to split (every referenced patch is already <= %d px)' % a.max_width)
        return

    # Append the strips to PNAMES, then rewrite every texture that used a split patch.
    base = len(names)
    idx_of = {}
    for i, (nm, _) in enumerate(newlumps):
        idx_of[nm] = base + i
        names.append(nm)
    newp = struct.pack('<i', len(names))
    for nm in names:
        newp += nm.encode('latin1')[:8].ljust(8, b'\0')

    rewritten = 0
    for t in texlumps:
        for tex in texs[t]:
            out, changed = [], False
            for (ox, oy, pn, s1, s2) in tex[5]:
                if pn in plan:
                    changed = True
                    for x0, nm, _ in plan[pn]:
                        out.append((ox + x0, oy, idx_of[nm], s1, s2))
                else:
                    out.append((ox, oy, pn, s1, s2))
            if changed:
                tex[5] = out; rewritten += 1

    big_before = max(len(wad.lumps[wad.index(names[pn])][1]) for pn in plan)
    print('\n%d patches split into %d strips; %d textures rewritten'
          % (len(plan), len(newlumps), rewritten))
    print('largest split lump: %d B -> %d B'
          % (big_before, max(len(d) for _, d in newlumps)))

    if a.dry_run:
        print('(dry run -- nothing written)')
        return

    wad.set('PNAMES', newp)
    for t in texlumps:
        wad.set(t, build_textures(texs[t]))
    # Strips go at the end: a patch lump needs no P_START/P_END membership to be
    # found by name, and appending keeps every existing lump index stable.
    wad.lumps.extend([[nm, d] for nm, d in newlumps])
    wad.write(a.dst)
    print('wrote %s' % a.dst)

if __name__ == '__main__':
    main()
