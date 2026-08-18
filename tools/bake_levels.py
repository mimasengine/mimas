#!/usr/bin/env python3
# bake_levels.py -- rewrite a map's geometry lumps into Mimas's EXACT in-memory layout,
# big-endian, so P_Load* can Z_Malloc once and W_ReadLump straight into the array.
#
#   python tools/bake_levels.py <in.wad> <out.wad>
#
# *** NOT IN THE BUILD PIPELINE -- MEASURED AND REJECTED, 2026-08-18. ***
# Baking removes the conversion pass, but it GROWS the lumps, and a Saturn level load is bound by
# synchronous CD reads, not by CPU.  Measured over wads_temoins:
#     lump       +disc bytes (MAP11)   staging buffer killed
#     NODES              +0                  22 KB     <- free, but needs no baking at all:
#                                                         mapnode_t and node_t are already the
#                                                         same 28 bytes in the same order
#     SEGS            +5,466                 33 KB
#     VERTEXES        +5,988                  6 KB     <- 1 byte of CD for 1 byte of RAM
#     LINEDEFS       +16,170                 23 KB     <- ~50-100 ms of CD to save ~5 ms of CPU
# p_setup.c takes the staging-buffer win a different way instead: it reads each raw lump into the
# TAIL of its own final array and expands in place -- same saving, zero disc cost, no on-disc
# format and no version skew to get wrong.  See the note above P_LoadVertexes.
#
# Kept because it is still the right tool the day level data moves into the RAM cart, where the
# engine reads lumps in place and the conversion pass, not the transfer, is the cost.
#
# WHY.  Every P_LoadX today pays three costs the Saturn cannot afford at level load:
#   1. a staging buffer -- W_CacheLumpNum(raw) lives alongside the final Z_Malloc'd array,
#      so the peak is (final + raw).  NODES is the worst: 28n + 28n, i.e. exactly double.
#   2. a conversion pass over every field, on the CPU, during a load the player is watching.
#   3. a SHORT()/LONG() byte swap on EVERY field: the WAD is little-endian and the SH-2 is
#      big-endian, so vanilla Doom's "free" load is not free here.
# Baking moves all three to the build machine.  The lumps ship in engine layout; the engine
# reads them as bytes.
#
# WHAT IS BAKED: VERTEXES, LINEDEFS, SEGS, NODES -- the four that are pure, immutable geometry,
# and the four that were just shrunk (see r_defs.h).  SECTORS/SIDEDEFS are NOT baked: they carry
# mutable state and texture NAMES that only R_TextureNumForName can resolve at run time.
#
# SAFETY.  A stale baked WAD against a rebuilt engine would be silent corruption -- the exact
# failure mode of a stale DOOMRP.DRP.  So the tool emits a MIMASLVL lump carrying the struct
# sizes it baked for, and the engine I_Errors on any mismatch instead of misreading bytes.
# A WAD with no MIMASLVL takes the vanilla path unchanged, which is also the A/B.
import sys, struct, os

MAGIC   = b'MMSL'
VERSION = 1

SZ_VERTEX, SZ_LINE, SZ_SEG, SZ_NODE = 8, 24, 14, 28    # MUST match r_defs.h (_Static_assert'd)

# m_bbox.h
BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT = 0, 1, 2, 3
# r_defs.h slopetype_t
ST_HORIZONTAL, ST_VERTICAL, ST_POSITIVE, ST_NEGATIVE = 0, 1, 2, 3
# r_defs.h LS_*
LS_DXPOS, LS_DXNEG, LS_DYPOS, LS_DYNEG = 0x04, 0x08, 0x10, 0x20
# r_defs.h seg sentinels
SEG_NOSECTOR, SEG_NULLSECTOR = 0xffff, 0xfffe
ML_TWOSIDED = 4

MAPLUMPS = ('THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS',
            'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP')


def read_wad(path):
    d = open(path, 'rb').read()
    magic = bytes(d[0:4])
    n, ofs = struct.unpack_from('<ii', d, 4)
    lumps = []
    for i in range(n):
        fp, sz = struct.unpack_from('<ii', d, ofs + i * 16)
        raw = bytes(d[ofs + i * 16 + 8: ofs + i * 16 + 16])
        nm = raw.split(b'\x00')[0].decode('latin1').upper()
        lumps.append([nm, raw, bytes(d[fp:fp + sz]) if sz > 0 else b''])
    return magic, lumps


def bake_vertexes(raw):
    n = len(raw) // 4
    out = bytearray(n * SZ_VERTEX)
    for i in range(n):
        x, y = struct.unpack_from('<hh', raw, i * 4)
        struct.pack_into('>ii', out, i * SZ_VERTEX, x << 16, y << 16)
    return bytes(out)


def bake_linedefs(raw, vx):
    n = len(raw) // 14
    out = bytearray(n * SZ_LINE)
    for i in range(n):
        v1, v2, flags, special, tag, sn0, sn1 = struct.unpack_from('<HHhhhhh', raw, i * 14)
        if v1 >= len(vx) or v2 >= len(vx):
            raise SystemExit("linedef %d: vertex index %d/%d out of range (%d)"
                             % (i, v1, v2, len(vx)))
        x1, y1 = vx[v1]
        x2, y2 = vx[v2]
        dx, dy = x2 - x1, y2 - y1

        # P_LoadLineDefs, verbatim: !dx wins over !dy, and FixedDiv(dy,dx) > 0 is just
        # "dx and dy share a sign" once both are known non-zero.
        if dx == 0:
            slope = ST_VERTICAL
        elif dy == 0:
            slope = ST_HORIZONTAL
        elif (dx > 0) == (dy > 0):
            slope = ST_POSITIVE
        else:
            slope = ST_NEGATIVE
        if dx > 0:
            slope |= LS_DXPOS
        elif dx < 0:
            slope |= LS_DXNEG
        if dy > 0:
            slope |= LS_DYPOS
        elif dy < 0:
            slope |= LS_DYNEG

        bbox = [0, 0, 0, 0]
        bbox[BOXLEFT], bbox[BOXRIGHT] = (x1, x2) if x1 < x2 else (x2, x1)
        bbox[BOXBOTTOM], bbox[BOXTOP] = (y1, y2) if y1 < y2 else (y2, y1)

        struct.pack_into('>HHhhhhhhhhhBB', out, i * SZ_LINE,
                         v1, v2, flags, special, tag, sn0, sn1,
                         bbox[0], bbox[1], bbox[2], bbox[3], slope, 0)
    return bytes(out)


def bake_segs(raw, lines, sides_sector, numsides):
    n = len(raw) // 12
    out = bytearray(n * SZ_SEG)
    for i in range(n):
        v1, v2, angle, ldnum, side, offset = struct.unpack_from('<HHhHhh', raw, i * 12)
        if ldnum >= len(lines):
            raise SystemExit("seg %d: linedef index %d out of range (%d)"
                             % (i, ldnum, len(lines)))
        if side not in (0, 1):
            raise SystemExit("seg %d: side %d is neither 0 nor 1" % (i, side))
        flags, sn = lines[ldnum]
        front = sn[side]
        if front < 0 or front >= numsides:
            raise SystemExit("seg %d: front sidedef %d out of range (%d)"
                             % (i, front, numsides))
        fsi = sides_sector[front]

        # P_LoadSegs, verbatim -- including the OTTAWAU "glass hack" sentinel, which must stay
        # distinct from one-sided or the renderer draws those lines differently.
        if flags & ML_TWOSIDED:
            back = sn[side ^ 1]
            bsi = SEG_NULLSECTOR if (back < 0 or back >= numsides) else sides_sector[back]
        else:
            bsi = SEG_NOSECTOR

        struct.pack_into('>HHHHHhH', out, i * SZ_SEG,
                         v1, v2, (ldnum << 1) | (side & 1), fsi, bsi,
                         offset, angle & 0xffff)
    return bytes(out)


def bake_nodes(raw):
    n = len(raw) // 28
    out = bytearray(n * SZ_NODE)
    for i in range(n):
        f = struct.unpack_from('<12hHH', raw, i * 28)
        struct.pack_into('>12hHH', out, i * SZ_NODE, *f)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print("usage: bake_levels.py <in.wad> <out.wad>")
        sys.exit(2)
    inp, outp = sys.argv[1], sys.argv[2]
    magic, lumps = read_wad(inp)

    if any(nm == 'MIMASLVL' for nm, _, _ in lumps):
        print("bake_levels: input already carries MIMASLVL -- refusing to double-bake")
        sys.exit(1)

    nmaps = 0
    staging = 0
    for i in range(len(lumps)):
        if i + 1 >= len(lumps) or lumps[i + 1][0] != 'THINGS':
            continue
        nm = lumps[i][0]
        # collect this map's lumps BY NAME within the run, not at a fixed offset
        run = {}
        for j in range(i + 1, min(i + 1 + len(MAPLUMPS), len(lumps))):
            if lumps[j][0] not in MAPLUMPS:
                break
            run[lumps[j][0]] = j
        need = ('VERTEXES', 'LINEDEFS', 'SIDEDEFS', 'SEGS', 'NODES')
        missing = [k for k in need if k not in run]
        if missing:
            raise SystemExit("map %s: missing %s -- cannot bake (all maps or none)"
                             % (nm, missing))

        vraw = lumps[run['VERTEXES']][2]
        vx = [struct.unpack_from('<hh', vraw, k * 4) for k in range(len(vraw) // 4)]

        lraw = lumps[run['LINEDEFS']][2]
        lines = []
        for k in range(len(lraw) // 14):
            v1, v2, flags, special, tag, sn0, sn1 = struct.unpack_from('<HHhhhhh', lraw, k * 14)
            lines.append((flags, (sn0, sn1)))

        sraw = lumps[run['SIDEDEFS']][2]
        numsides = len(sraw) // 30
        sides_sector = [struct.unpack_from('<h', sraw, k * 30 + 28)[0] for k in range(numsides)]

        # the staging buffer each of these no longer needs to hold alongside its final array
        staging += (len(vraw) + len(lraw) + len(lumps[run['SEGS']][2])
                    + len(lumps[run['NODES']][2]))

        lumps[run['VERTEXES']][2] = bake_vertexes(vraw)
        lumps[run['LINEDEFS']][2] = bake_linedefs(lraw, vx)
        lumps[run['SEGS']][2] = bake_segs(lumps[run['SEGS']][2], lines, sides_sector, numsides)
        lumps[run['NODES']][2] = bake_nodes(lumps[run['NODES']][2])
        nmaps += 1

    if nmaps == 0:
        print("bake_levels: no maps found -- nothing to do")
        sys.exit(1)

    marker = struct.pack('>4sHHHHH', MAGIC, VERSION, SZ_VERTEX, SZ_LINE, SZ_SEG, SZ_NODE)
    marker += b'\x00' * (16 - len(marker))
    lumps.append(['MIMASLVL', b'MIMASLVL', marker])

    # rebuild, keeping every filepos 4-aligned (see strip_wad.py: the cart reads in place)
    out = bytearray(12)
    dirents = []
    for nm, raw, data in lumps:
        while len(out) & 3:
            out.append(0)
        dirents.append((len(out), len(data), raw))
        out += data
    while len(out) & 3:
        out.append(0)
    dirofs = len(out)
    for fp, sz, raw in dirents:
        out += struct.pack('<ii', fp, sz) + raw
    struct.pack_into('<4sii', out, 0, magic, len(dirents), dirofs)
    open(outp, 'wb').write(out)

    print("bake_levels: %d maps baked (VERTEXES/LINEDEFS/SEGS/NODES -> engine layout, BE)"
          % nmaps)
    print("  staging buffers removed from the load path: %s bytes across all maps"
          % format(staging, ','))
    print("  %s -> %s bytes" % (format(os.path.getsize(inp), ','),
                                format(os.path.getsize(outp), ',')))


if __name__ == '__main__':
    main()
