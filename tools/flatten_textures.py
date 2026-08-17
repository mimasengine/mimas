#!/usr/bin/env python3
"""
flatten_textures.py -- kill the WALL COMPOSITE TREADMILL, offline.

WHY (measured on hardware/Ymir, 2026-08-17)
-------------------------------------------
Row 14 settled what row 4 could not: the per-column seg loop writes almost NO
pixels (`k0`..`k5`, i.e. 0..5000) while costing 85..179 ms.  A fill-bound `lp` of
100 ms would need ~400 000 pixels.  So `lp` is not fill.

Row 20 named it instead:

    x19305   the worst SINGLE R_GetColumn call = 19,3 ms
    g216 / n252  = 857 us per call, average
    k17      the composite patch loop + R_DrawColumnInCache -- the field whose own
             comment says "kept as a CONTROL: must stay ~2"

and row 18 gave the multiplier: `cb46/8` -- the SAME EIGHT textures rebuilt
FORTY-SIX times in one window.  Sorted by `cb`, the correlation is total:

    cb  2/2  -> lp  30,1        cb 24/8 -> lp 154,0
    cb 10/4  -> lp  23,5        cb 36/8 -> lp 179,3

That is a treadmill, not a rendering cost.  A 256x128 composite needs **32 KB in
one contiguous run** and row 11 says `lg` is **21..34 KB**: the big composites do
not fit, the ones that do evict each other, and every eviction costs another
13..17 ms rebuild.

WHY NOT FIX IT AT RUNTIME
-------------------------
It was fixed at runtime.  Twice.  Both are blocked by the same wall:
  * core/r_cache.c   -- the composite sub-zone pool.  Its smallest rung needs
                        32K slab + 64K margin = 96 KB contiguous.
  * core/r_data.c    -- the composite PIN.  Its floor is 48 KB contiguous, and
                        seven captures read `pn0/1`..`pn0/25`: it yields every time.
`lg` is 21..34 KB.  Neither can engage.  There is nowhere to cache TO.

THE TRANSFORM
-------------
Doom composes a texture from patches **per column**.  R_GenerateLookup only
allocates a composite for columns covered by MORE THAN ONE patch; a column
covered by exactly one is served straight from that patch's lump, and if NO
column is multi-patch then `texturecompositesize` stays 0 and
**R_GenerateComposite is never called at all**.

TNT's own sky already proves it: SKY1 is 1024x128 built from four non-overlapping
256-wide patches, and it is R_GetColumn's single-patch branch that serves it.

So: render each overlapping texture once, offline, and re-emit it as N
NON-OVERLAPPING vertical strips of at most --max-width columns:

    BIGDOOR2  128x128, patches = [(0,0,A), (0,0,B), (64,0,C)]   <- overlapping
      becomes 128x128, patches = [(0,0,FLT0001), (96,0,FLT0002)]

Identical pixels, identical mapping, **zero renderer change**, and two costs
disappear at once: the 13..17 ms composite build, and the 32 KB contiguous run it
needed.  Every strip lump is small enough to live inside the measured `lg`.

CONSERVATIVE BY CONSTRUCTION
----------------------------
Only textures that are FULLY COVERED are flattened.  A texture with holes is a
masked/2-sided middle: its columns must keep real post chains, R_GetColumn's
`+3` opaque assumption does not hold for them, and they are small anyway.  They
are left exactly as they are and counted in the report.

USAGE
-----
    python tools/flatten_textures.py in.wad out.wad [--max-width 96] [--dry-run]

Every lump stays 4-byte aligned, which the cartridge path requires (it reads
lumps IN PLACE, and a misaligned 32-bit read returns silent garbage).
Companion to split_patches.py: that one CUTS an oversized patch, this one
RE-RENDERS an overlapping texture.  This tool subsumes it for the textures it
touches, so run this one first.
"""

import struct, sys, argparse

from split_patches import Wad, patch_header, parse_textures, build_textures

# --------------------------------------------------------------- patch decoding

def patch_columns(b):
    """Decode a patch into [[(y, [indices]), ...], ...] -- one post list per column."""
    w, h, lo, to, ofs = patch_header(b)
    cols = []
    for o in ofs:
        posts, i = [], o
        while i < len(b) and b[i] != 0xFF:
            top, length = b[i], b[i + 1]
            posts.append((top, list(b[i + 3: i + 3 + length])))
            i += 4 + length
        cols.append(posts)
    return w, h, lo, to, cols


def render(tex, patch_raw):
    """Rasterise one texture.  Returns (pix, covered) as column-major lists, or None
       if a referenced patch is missing.  Mirrors R_DrawColumnInCache's clipping."""
    _name, _masked, w, h, _cdir, patches = tex
    pix     = [bytearray(h) for _ in range(w)]
    covered = [bytearray(h) for _ in range(w)]
    for (ox, oy, pn, _s1, _s2) in patches:
        raw = patch_raw.get(pn)
        if raw is None:
            return None
        pw, _ph, _lo, _to, cols = patch_columns(raw)
        for px in range(pw):
            tx = ox + px
            if tx < 0 or tx >= w:
                continue
            for top, data in cols[px]:
                y = oy + top
                for v in data:
                    if 0 <= y < h:
                        pix[tx][y] = v
                        covered[tx][y] = 1
                    y += 1
    return pix, covered


def strip_lump(pix, x0, cw, h):
    """One strip patch: exactly ONE full-height post per column, which is what
       R_GetColumn's single-patch branch (colofs = columnofs + 3) requires."""
    head  = struct.pack('<hhhh', cw, h, 0, 0)
    table = b''
    body  = b''
    cur   = 8 + 4 * cw
    for x in range(x0, x0 + cw):
        col = bytes(pix[x])
        post = bytes((0, h, col[0])) + col + bytes((col[h - 1], 0xFF))
        table += struct.pack('<i', cur)
        body  += post
        cur   += len(post)
    return head + table + body


# ------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--max-width', type=int, default=96,
                    help='widest strip to emit (default 96 -> ~13 KB lumps, safe under lg21k)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    wad = Wad(a.src)
    pnames = wad.get('PNAMES')
    if pnames is None:
        sys.exit('no PNAMES lump')
    n = struct.unpack('<i', pnames[:4])[0]
    names = [pnames[4 + i * 8: 12 + i * 8].rstrip(b'\0').decode('latin1').upper()
             for i in range(n)]

    patch_raw = {}
    for pn, nm in enumerate(names):
        li = wad.index(nm)
        if li >= 0 and len(wad.lumps[li][1]) >= 8:
            patch_raw[pn] = wad.lumps[li][1]

    texlumps = [t for t in ('TEXTURE1', 'TEXTURE2') if wad.index(t) >= 0]
    texs = {t: parse_textures(wad.get(t)) for t in texlumps}

    newlumps, serial = [], 0
    n_flat = n_skip_hole = n_skip_tall = n_ok = n_miss = 0
    bytes_before = bytes_after = 0
    worst_before = worst_after = 0

    for t in texlumps:
        for tex in texs[t]:
            name, masked, w, h, cdir, patches = tex

            # Does ANY column carry more than one patch?  That is the exact test
            # R_GenerateLookup uses to decide a composite is needed.
            count = [0] * w
            wide  = False
            for (ox, _oy, pn, _s1, _s2) in patches:
                raw = patch_raw.get(pn)
                if raw is None:
                    continue
                pw = struct.unpack('<h', raw[:2])[0]
                if pw > a.max_width:
                    wide = True
                for px in range(pw):
                    if 0 <= ox + px < w:
                        count[ox + px] += 1
            if max(count, default=0) <= 1 and not wide:
                n_ok += 1
                continue                       # already composite-free and small

            if h > 254:
                n_skip_tall += 1               # one post cannot carry it (length is a byte)
                continue

            r = render(tex, patch_raw)
            if r is None:
                n_miss += 1
                continue
            pix, covered = r
            if not all(all(c) for c in covered):
                n_skip_hole += 1               # real masked texture -> leave its posts alone
                continue

            before = w * h                     # the composite this texture builds today
            bytes_before += before
            worst_before = max(worst_before, before)

            out, biggest = [], 0
            for x0 in range(0, w, a.max_width):
                cw   = min(a.max_width, w - x0)
                data = strip_lump(pix, x0, cw, h)
                nm   = 'MFLT%04d' % serial; serial += 1
                newlumps.append((nm, data))
                out.append((x0, nm))
                biggest = max(biggest, len(data))
                bytes_after += len(data)
            worst_after = max(worst_after, biggest)
            tex[5] = [(x0, 0, nm, 1, 0) for x0, nm in out]   # patch index patched below
            n_flat += 1

    if not n_flat:
        print('nothing to flatten (no texture builds a composite)')
        return

    # Append the strips to PNAMES and resolve every texture's patch indices.
    base   = len(names)
    idx_of = {}
    for i, (nm, _) in enumerate(newlumps):
        idx_of[nm] = base + i
        names.append(nm)
    newp = struct.pack('<i', len(names))
    for nm in names:
        newp += nm.encode('latin1')[:8].ljust(8, b'\0')

    for t in texlumps:
        for tex in texs[t]:
            tex[5] = [(ox, oy, idx_of[pn] if isinstance(pn, str) else pn, s1, s2)
                      for (ox, oy, pn, s1, s2) in tex[5]]

    print('flattened      : %d textures -> %d strips (max %d px)'
          % (n_flat, len(newlumps), a.max_width))
    print('left alone     : %d already composite-free, %d with holes (masked), '
          '%d too tall, %d missing patch' % (n_ok, n_skip_hole, n_skip_tall, n_miss))
    print('worst run needed: %d B composite -> %d B lump' % (worst_before, worst_after))
    print('pixel bytes    : %d -> %d (+%d, +%.1f%%)'
          % (bytes_before, bytes_after, bytes_after - bytes_before,
             100.0 * (bytes_after - bytes_before) / max(bytes_before, 1)))

    if a.dry_run:
        print('(dry run -- nothing written)')
        return

    wad.set('PNAMES', newp)
    for t in texlumps:
        wad.set(t, build_textures(texs[t]))
    wad.lumps.extend([[nm, d] for nm, d in newlumps])
    wad.write(a.dst)
    print('wrote %s' % a.dst)


if __name__ == '__main__':
    main()
