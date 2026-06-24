#!/usr/bin/env python3
# measure_texfloor.py -- Phase 0 of docs/TEXTURECOLUMNLUMP_PLAN.md.
#
# Analytically reproduces core/r_data.c R_InitTextures + R_GenerateLookup against
# a WAD's TEXTURE1/TEXTURE2/PNAMES lumps to compute, with ZERO emulator:
#   * the per-column directory floor  = 4 * Sum(texture width)   (texturecolumnlump + texturecolumnofs)
#   * today's composite footprint     = Sum over multi-patch textures of height*(#multi-patch columns)
#   * the Option-E whole-texture slab = Sum over not-pure-single-patch textures of height*width
#   * per-texture classification (pure-single / multi-non-overlapping / true-multi-patch / broken)
#   * per-map wall-texture working set (SIDEDEFS-referenced) -> the LRU resident bound
#
# Column classification mirrors R_GenerateLookup EXACTLY:
#   for each patch: x in [max(0,originx), min(texwidth, originx+patchwidth)) -> patchcount[x]++
#   column patchcount>1 => multi-patch (composite); ==1 => single (direct lump); ==0 => "no patch" (broken).
#
import os, sys, struct, glob

def read_wad(path):
    with open(path, 'rb') as f:
        data = f.read()
    ident = data[0:4]
    if ident not in (b'IWAD', b'PWAD'):
        return None
    numlumps, infotableofs = struct.unpack_from('<ii', data, 4)
    lumps = []        # (name, filepos, size) in directory order
    byname = {}       # NAME -> last directory index (W_CheckNumForName returns last match)
    for i in range(numlumps):
        filepos, size = struct.unpack_from('<ii', data, infotableofs + i*16)
        raw = data[infotableofs + i*16 + 8: infotableofs + i*16 + 16]
        name = raw.split(b'\x00')[0].decode('ascii', 'replace').upper()
        lumps.append((name, filepos, size))
        byname[name] = i
    return data, lumps, byname

def lump_bytes(data, lumps, idx):
    _, pos, size = lumps[idx]
    return data[pos:pos+size]

def patch_width(data, lumps, idx):
    # patch_t header: width(short) height(short) leftofs(short) topofs(short) ...
    _, pos, size = lumps[idx]
    if size < 2:
        return 0
    return struct.unpack_from('<h', data, pos)[0]

def analyze(path):
    r = read_wad(path)
    if not r:
        return None
    data, lumps, byname = r

    # --- PNAMES -> patchlookup[i] = lump index (or -1) + cached patch width ----
    if 'PNAMES' not in byname:
        return {'name': os.path.basename(path), 'note': 'no PNAMES (likely map-only PWAD)'}
    pn = lump_bytes(data, lumps, byname['PNAMES'])
    nummap = struct.unpack_from('<i', pn, 0)[0]
    patchlump = []   # PNAMES index -> lump idx
    patchw    = []   # PNAMES index -> patch width
    for i in range(nummap):
        nm = pn[4 + i*8: 4 + i*8 + 8].split(b'\x00')[0].decode('ascii','replace').upper()
        li = byname.get(nm, -1)
        patchlump.append(li)
        patchw.append(patch_width(data, lumps, li) if li >= 0 else 0)

    # --- TEXTURE1 (+TEXTURE2) ------------------------------------------------
    textures = []     # dicts: name,width,height,patches=[(originx,pnameidx)]
    for tl in ('TEXTURE1', 'TEXTURE2'):
        if tl not in byname:
            continue
        tex = lump_bytes(data, lumps, byname[tl])
        ntex = struct.unpack_from('<i', tex, 0)[0]
        offs = struct.unpack_from('<%di' % ntex, tex, 4)
        for off in offs:
            name = tex[off:off+8].split(b'\x00')[0].decode('ascii','replace').upper()
            width, height = struct.unpack_from('<hh', tex, off+12)   # skip masked(4) at off+8
            patchcount = struct.unpack_from('<h', tex, off+20)[0]    # after obsolete(4) at off+16
            pats = []
            for p in range(patchcount):
                base = off + 22 + p*10
                originx, originy, pidx = struct.unpack_from('<hhh', tex, base)
                pats.append((originx, pidx))
            textures.append({'name': name, 'width': width, 'height': height, 'patches': pats})

    # --- classify each texture (replicate R_GenerateLookup first pass) --------
    texindex = {}     # NAME -> index (last wins, vanilla R_TextureNumForName behaviour-ish)
    classed = []
    sum_w = 0
    n_pure = n_mno = n_tmp = n_broken = 0
    w_pure = w_mno = w_tmp = 0
    today_composite = 0     # Sum height*mpc over true-multi-patch textures (if ALL drawn)
    optE_slab = 0           # Sum height*width over (mno+tmp) textures (if ALL drawn)
    max_today = 0
    max_slab  = 0
    for ti, t in enumerate(textures):
        w, h, npatch = t['width'], t['height'], len(t['patches'])
        sum_w += w
        pc = [0]*w
        for (originx, pidx) in t['patches']:
            if pidx < 0 or pidx >= len(patchw):
                continue
            pw = patchw[pidx]
            x1 = originx
            x2 = x1 + pw
            x = 0 if x1 < 0 else x1
            if x2 > w: x2 = w
            while x < x2:
                pc[x] += 1
                x += 1
        mpc = sum(1 for v in pc if v > 1)     # multi-patch columns
        spc = sum(1 for v in pc if v == 1)
        zpc = sum(1 for v in pc if v == 0)
        if zpc > 0:
            cls = 'broken'; n_broken += 1
        elif mpc > 0:
            cls = 'tmp'; n_tmp += 1; w_tmp += w
            today_composite += h*mpc
            optE_slab += h*w
            max_today = max(max_today, h*mpc)
            max_slab  = max(max_slab,  h*w)
        elif npatch > 1:
            cls = 'mno'; n_mno += 1; w_mno += w     # multi non-overlapping -> needs slab in Option E
            optE_slab += h*w
            max_slab  = max(max_slab,  h*w)
        else:
            cls = 'pure'; n_pure += 1; w_pure += w  # pure single-patch -> direct lump, no slab
        classed.append({'name': t['name'], 'w': w, 'h': h, 'mpc': mpc, 'spc': spc,
                        'cls': cls, 'slab': (h*w if cls in ('mno','tmp') else 0),
                        'today': (h*mpc if cls == 'tmp' else 0)})
        texindex[t['name']] = ti

    # --- per-map wall-texture working set (SIDEDEFS-referenced) ----------------
    maps = []
    for i, (nm, pos, size) in enumerate(lumps):
        is_map = (len(nm) == 5 and nm.startswith('MAP') and nm[3:].isdigit()) or \
                 (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M' and nm[1].isdigit() and nm[3].isdigit())
        if not is_map:
            continue
        # find SIDEDEFS within this map's lump group (next ~10 lumps)
        sd = None
        for j in range(i+1, min(i+12, len(lumps))):
            if lumps[j][0] == 'SIDEDEFS':
                sd = lump_bytes(data, lumps, j); break
            if lumps[j][0] in ('THINGS','LINEDEFS','VERTEXES','SEGS','SSECTORS','NODES','SECTORS','REJECT','BLOCKMAP') and j > i+11:
                break
        if sd is None:
            continue
        names = set()
        nside = len(sd)//30
        for s in range(nside):
            b = s*30
            for foff in (4, 12, 20):   # toptexture, bottomtexture, midtexture
                tn = sd[b+foff:b+foff+8].split(b'\x00')[0].decode('ascii','replace').upper()
                if tn and tn != '-':
                    names.add(tn)
        # sum slab footprint of referenced not-pure textures
        ref_slab = 0; ref_today = 0; ref_n = 0; ref_miss = 0
        for tn in names:
            ti = texindex.get(tn)
            if ti is None:
                ref_miss += 1; continue
            c = classed[ti]
            ref_n += 1
            ref_slab += c['slab']
            ref_today += c['today']
        maps.append({'map': nm, 'ntex': len(names), 'resolved': ref_n,
                     'slab_kb': ref_slab/1024.0, 'today_kb': ref_today/1024.0})

    return {
        'name': os.path.basename(path),
        'numtex': len(textures), 'sumwidth': sum_w,
        'dir_kb': sum_w*4/1024.0,
        'mptex_patchcount': n_mno + n_tmp,           # textures with >1 patch (matches overlay sat_tex_mptex)
        'n_pure': n_pure, 'n_mno': n_mno, 'n_tmp': n_tmp, 'n_broken': n_broken,
        'w_pure': w_pure, 'w_mno': w_mno, 'w_tmp': w_tmp,
        'today_kb': today_composite/1024.0,
        'optE_kb': optE_slab/1024.0,
        'max_today_b': max_today, 'max_slab_b': max_slab,
        'maps': maps,
    }

def main():
    wad_dir = sys.argv[1] if len(sys.argv) > 1 else 'wads_temoins'
    wads = sorted(glob.glob(os.path.join(wad_dir, '*.wad')) + glob.glob(os.path.join(wad_dir, '*.WAD')))
    seen = set()
    results = []
    for w in wads:
        key = os.path.basename(w).lower()
        if key in seen: continue
        seen.add(key)
        r = analyze(w)
        if r: results.append(r)

    print("="*100)
    print("PHASE-0 TEXTURE FLOOR MEASUREMENT  (analytical, mirrors R_InitTextures/R_GenerateLookup)")
    print("="*100)
    hdr = f"{'WAD':<16}{'#tex':>6}{'Sw':>8}{'FLOOR':>8}{'pure':>6}{'mno':>5}{'tmp':>5}{'brk':>4}{'today':>8}{'OptE':>8}{'maxSlab':>9}"
    print(hdr); print("-"*100)
    for r in results:
        if 'numtex' not in r:
            print(f"{r['name']:<16}  {r.get('note','')}")
            continue
        print(f"{r['name']:<16}{r['numtex']:>6}{r['sumwidth']:>8}{r['dir_kb']:>7.0f}K"
              f"{r['n_pure']:>6}{r['n_mno']:>5}{r['n_tmp']:>5}{r['n_broken']:>4}"
              f"{r['today_kb']:>7.0f}K{r['optE_kb']:>7.0f}K{r['max_slab_b']/1024.0:>8.1f}K")
    print("-"*100)
    print("FLOOR = 4*Sw = the texturecolumnlump+ofs PU_STATIC arrays (the cut target).")
    print("today = Sum height*(multi-patch cols) over tmp textures, if ALL were drawn (upper bound).")
    print("OptE  = Sum height*width over (mno+tmp), if ALL were drawn (upper bound). maxSlab = biggest single slab.")
    print()

    # per-map working set for the IWAD binding cases
    for r in results:
        if 'maps' not in r or not r['maps']:
            continue
        print("="*100)
        print(f"PER-MAP WALL-TEXTURE WORKING SET (SIDEDEFS-referenced)  --  {r['name']}")
        print(f"  pool cap = 256K (TEXCACHE_MAX).  slab_kb = Option-E resident need if all referenced textures co-resident.")
        print("-"*100)
        ms = sorted(r['maps'], key=lambda m: m['slab_kb'], reverse=True)
        print(f"  {'map':<8}{'#refTex':>8}{'resolved':>9}{'OptE_slab':>11}{'today':>9}")
        for m in ms[:12]:
            print(f"  {m['map']:<8}{m['ntex']:>8}{m['resolved']:>9}{m['slab_kb']:>10.1f}K{m['today_kb']:>8.1f}K")
        worst = ms[0]
        print(f"  -> worst map: {worst['map']} needs {worst['slab_kb']:.1f}K Option-E slab "
              f"(vs {worst['today_kb']:.1f}K today's composite) for its referenced wall set.")
        print()

if __name__ == '__main__':
    main()
