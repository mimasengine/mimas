#!/usr/bin/env python3
# strip_wad.py -- drop lumps unused by the Saturn (SRL) Doom port so an IWAD can fit
# the 4 MB extended-RAM cart (raw shareware = 4,196,020 B, ~1.7 KB over -> won't cart).
#
#   python tools/strip_wad.py <in.wad> <out.wad>
#
# Stripped (all verified unused by the port):
#   GENMIDI / DMXGUS / DMXGUSC : OPL/GUS instrument config -- the Saturn music is a
#                                custom SCSP synth (src/i_sound_saturn.cxx), no OPL/GUS;
#                                zero references in core/ or src/.
#   DP*                        : PC-speaker SFX -- the port loads only "ds%s" (DS*).
# KEPT (used at runtime):
#   DEMO1..3 : the title attract loop plays them (core/d_main.c D_AdvanceDemo).
#   ENDOOM   : only loaded by D_Endoom at exit (I_AtExit), which never fires on Saturn,
#              but kept anyway -- the GENMIDI/DMXGUS/DP* savings already clear the cart.
import sys, struct

CART = 4194304

STRIP_EXACT    = {'GENMIDI', 'DMXGUS', 'DMXGUSC'}
STRIP_PREFIXES = ('DP',)

def keep(nm):
    if nm in STRIP_EXACT: return False
    return not any(nm.startswith(p) for p in STRIP_PREFIXES)

def read_wad(path):
    d = open(path, 'rb').read()
    magic = bytes(d[0:4])
    n, ofs = struct.unpack_from('<ii', d, 4)
    lumps = []                      # (name, raw_name8, data)
    for i in range(n):
        fp, sz = struct.unpack_from('<ii', d, ofs + i*16)
        raw = bytes(d[ofs+i*16+8: ofs+i*16+16])
        nm  = raw.split(b'\x00')[0].decode('latin1').upper()
        lumps.append((nm, raw, bytes(d[fp:fp+sz]) if sz > 0 else b''))
    return magic, lumps

def main():
    if len(sys.argv) < 3:
        print("usage: strip_wad.py <in.wad> <out.wad>"); sys.exit(2)
    inp, outp = sys.argv[1], sys.argv[2]
    magic, lumps = read_wad(inp)
    in_size = sum(1 for _ in [0]) and __import__('os').path.getsize(inp)

    kept    = [(nm, raw, data) for (nm, raw, data) in lumps if keep(nm)]
    dropped = [(nm, len(data)) for (nm, raw, data) in lumps if not keep(nm)]

    # rebuild: header(12) | lump data | directory.
    # CRITICAL: 4-align every lump's filepos (real WADs already do this).  In CART mode
    # the engine reads lumps IN PLACE from the memory-mapped WAD (e.g. numtextures =
    # LONG(*maptex) from TEXTURE1), and an unaligned 32-bit read returns GARBAGE on the
    # big-endian SH-2 -> a bogus Z_Malloc size and a launch crash.  (CD-streaming copies
    # each lump to an aligned zone buffer, so it never hit this -- cart did.)
    out = bytearray(12)
    dirents = []
    for nm, raw, data in kept:
        while len(out) & 3:
            out.append(0)                       # pad so this lump's filepos is 4-aligned
        dirents.append((len(out), len(data), raw))
        out += data
    while len(out) & 3:
        out.append(0)                           # align the directory too
    infotableofs = len(out)
    for fp, sz, raw in dirents:
        out += struct.pack('<ii', fp, sz) + raw
    struct.pack_into('<4sii', out, 0, magic, len(kept), infotableofs)
    open(outp, 'wb').write(out)

    saved = sum(s for _, s in dropped)
    over  = len(out) - CART
    print(f"strip {inp} -> {outp}")
    print(f"  magic {magic.decode('latin1')}  lumps {len(lumps)} -> {len(kept)} "
          f"(dropped {len(dropped)}, {saved} B of lump data)")
    print(f"  dropped: {', '.join(nm for nm,_ in dropped)}")
    print(f"  size {in_size} -> {len(out)} B  (cart {CART}: "
          f"{'FITS by %d B' % -over if over <= 0 else 'STILL OVER by %d B' % over})")

if __name__ == '__main__':
    main()
