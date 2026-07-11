#!/usr/bin/env python3
# SATURN test-tooling: merge a PWAD onto an IWAD into a single standalone IWAD.
#
#   python tools/merge_wad.py <base.iwad> <patch.pwad> <out.wad>
#
# The merge is a plain lump concatenation (all IWAD lumps, then all PWAD lumps),
# written back as an IWAD.  This is exactly what vanilla `-file` loading does at
# runtime: W_CheckNumForName scans the directory BACKWARDS (core/w_wad.c:292,
# "so patch lump files take precedence"), so the PWAD's later lumps -- replacement
# maps (MAPxx marker + its group), graphics, sounds -- shadow the IWAD's.  Map
# loading reads the lumps AFTER the MAPxx marker by index, and the last MAPxx of a
# given name wins, so the PWAD's map data is what loads.  Lets the single-WAD Saturn
# build (build.ps1 -Wad ...) run PWAD megawads (NUTS / Hell Revealed / Scythe ...)
# for the big-WAD survival hunt without runtime multi-file support.
import sys, struct

def read_wad(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic = data[0:4]
    if magic not in (b'IWAD', b'PWAD'):
        raise SystemExit(f"{path}: not a WAD (magic={magic!r})")
    numlumps, infotableofs = struct.unpack('<ii', data[4:12])
    lumps = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        filepos, size = struct.unpack('<ii', data[off:off+8])
        name = data[off+8:off+16].rstrip(b'\0')
        lumps.append((name, data[filepos:filepos+size] if size > 0 else b''))
    return magic, lumps

def write_wad(path, lumps):
    out = bytearray(b'IWAD' + b'\0' * 8)        # header filled in at the end
    directory = []
    for name, ldata in lumps:
        pos = len(out)
        out += ldata
        directory.append((pos if ldata else 0, len(ldata), name))
    infotableofs = len(out)
    for pos, size, name in directory:
        out += struct.pack('<ii', pos, size) + name.ljust(8, b'\0')[:8]
    struct.pack_into('<ii', out, 4, len(lumps), infotableofs)
    with open(path, 'wb') as f:
        f.write(out)

def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: merge_wad.py <base.iwad> <patch.pwad> <out.wad>")
    base, patch, out = sys.argv[1:4]
    _, ilumps = read_wad(base)
    _, plumps = read_wad(patch)
    write_wad(out, ilumps + plumps)
    print(f"merged {len(ilumps)} (base) + {len(plumps)} (patch) = "
          f"{len(ilumps)+len(plumps)} lumps -> {out}")

if __name__ == '__main__':
    main()
