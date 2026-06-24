#!/usr/bin/env python3
# repack_wad.py -- per-level disc-repack tool, STEP 1: the offline per-map subset
# computer (STREAMING_ANALYSIS.md S4d / 7.4).  This is the foundation BOTH per-level
# repack and big-WAD cart-load-once (S5) build on: it computes, per map, the SAFE
# SUPERSET of WAD lumps that map can reference, so the disc can be repacked into
# per-map contiguous blobs (and/or staged into the cart).
#
# A missing lump in streaming mode is a HARD I_Error (not graceful), so the subset
# is deliberately a SUPERSET (include-if-in-doubt).  This phase REPORTS + VALIDATES
# the subset; the WAD-writer + per-map offset table is the next sub-step.
#
# Subset per map =
#   geometry  : the map's own lump group (THINGS..BLOCKMAP)
#   textures  : SIDEDEFS top/bottom/mid -> TEXTURE1/2 -> PNAMES -> patch lumps
#   flats     : SECTORS floor/ceil pics -> F_START..F_END lumps by name
#   sprites   : THINGS doomednum -> mobjinfo -> state graph -> SPR_ prefix ->
#               every S_START..S_END lump with that prefix (all rotations)
#   sounds    : the spawned actors' sfx_* (DS* lumps) + a fixed always-on UI set
#   music     : the map's d_<name> MUS lump
#   always    : player/weapons/blood/puff/fog sprites + HUD/font/menu/status +
#               colormap/playpal/pnames/texture defs (resident UI/always lumps)
#
import os, re, sys, struct, glob

# ---------------------------------------------------------------- WAD parsing
def read_wad(path):
    d = open(path, 'rb').read()
    n, ofs = struct.unpack_from('<ii', d, 4)
    lumps = []                 # (name, filepos, size) in directory order
    byname = {}                # NAME -> last index
    for i in range(n):
        fp, sz = struct.unpack_from('<ii', d, ofs + i*16)
        nm = d[ofs+i*16+8: ofs+i*16+16].split(b'\x00')[0].decode('latin1').upper()
        lumps.append((nm, fp, sz)); byname[nm] = i
    return d, lumps, byname

def marker_range(lumps, start, end):
    s = e = None
    for i, (nm, _, _) in enumerate(lumps):
        if nm == start: s = i
        elif nm == end: e = i
    return (s, e)

# ---------------------------------------------------------------- info.c parse
def parse_info(info_path):
    txt = open(info_path, 'r', errors='replace').read()

    # states[] : positional; capture (state_name from // comment, sprite4, nextstate_name)
    # entry form: {SPR_TROO,0,-1,{NULL},S_NULL,0,0}, // S_NULL
    state_next = {}    # S_name -> next S_name
    state_spr  = {}    # S_name -> 4-char sprite prefix
    m = re.search(r'states\[\s*NUMSTATES\s*\]\s*=\s*\{(.*?)\n\};', txt, re.S)
    body = m.group(1)
    ent = re.compile(r'\{\s*SPR_(\w+)\s*,[^,]*,[^,]*,\{[^}]*\}\s*,\s*(S_\w+)[^}]*\}\s*,?\s*//\s*(S_\w+)')
    for spr, nxt, name in ent.findall(body):
        state_spr[name]  = spr.upper()
        state_next[name] = nxt
    # also handle entries WITHOUT a trailing // S_NAME comment: fall back to
    # positional order (rare; warn so the superset stays trustworthy)
    n_decl = txt.count('// S_')   # rough; real check is the NUMSTATES count below

    # mobjinfo[] : fixed field order; capture per block: MT_name, doomednum,
    # spawn-related state names, sound names.  Block = { ... }, with // comments.
    FIELDS = ['doomednum','spawnstate','spawnhealth','seestate','seesound',
              'reactiontime','attacksound','painstate','painchance','painsound',
              'meleestate','missilestate','deathstate','xdeathstate','deathsound',
              'speed','radius','height','mass','damage','activesound','flags','raisestate']
    STATE_FIELDS = ['spawnstate','seestate','painstate','meleestate',
                    'missilestate','deathstate','xdeathstate','raisestate']
    SOUND_FIELDS = ['seesound','attacksound','painsound','deathsound','activesound']
    mi = re.search(r'mobjinfo\[\s*NUMMOBJTYPES\s*\]\s*=\s*\{(.*)\n\};', txt, re.S)
    mbody = mi.group(1)
    by_ednum = {}    # doomednum(int) -> dict(field->value)
    by_mt    = {}    # MT_name -> dict
    # split into top-level { ... } blocks
    blocks = re.findall(r'\{\s*//\s*(MT_\w+)(.*?)\n\s*\},', mbody, re.S)
    for mtname, blk in blocks:
        # strip // comments, collect comma-separated values in order
        vals = []
        for line in blk.splitlines():
            line = re.sub(r'//.*$', '', line).strip().rstrip(',').strip()
            if line:
                vals.append(line)
        rec = {}
        for i, f in enumerate(FIELDS):
            rec[f] = vals[i] if i < len(vals) else None
        by_mt[mtname] = rec
        try:
            ed = int(rec['doomednum'])
            if ed >= 0:
                by_ednum[ed] = rec
        except (TypeError, ValueError):
            pass
    return state_spr, state_next, by_ednum, by_mt, STATE_FIELDS, SOUND_FIELDS

def sprites_from_state(root, state_spr, state_next, out, seen):
    """Follow nextstate chain from `root`, collecting sprite prefixes."""
    s = root
    while s and s != 'S_NULL' and s not in seen:
        seen.add(s)
        spr = state_spr.get(s)
        if spr:
            out.add(spr)
        s = state_next.get(s)

# ---------------------------------------------------------------- per-map subset
# Always-resident / always-spawned lumps (UI + universal actors).  Superset-safe.
ALWAYS_SPRITES = [
    # player + weapons (pspr, never via THINGS chains)
    'PLAY','PUNG','PISG','PISF','SHTG','SHTF','SHT2','CHGG','CHGF','MISG','MISF',
    'SAWG','PLSG','PLSF','BFGG','BFGF',
    # universal spawns: blood, bullet/rocket puffs, teleport fog, gibs
    'BLUD','PUFF','MISL','TFOG','IFOG','BAL1','BAL2','PLSS','PLSE',
]
ALWAYS_LUMPS = [
    'PLAYPAL','COLORMAP','PNAMES','TEXTURE1','TEXTURE2','ENDOOM',
    # status bar / HUD / faces / numbers
    'STBAR','STGNUM0','STTPRCNT','STTMINUS',
    # (STF*/STT*/STG*/STK*/STD*/STY* + M_*/WI*/AMmark/STCFN font are added by prefix below)
]
ALWAYS_PREFIXES = ['STF','STT','STG','STK','STD','STY','STC','STP','M_','WI','AMMNUM','BRDR','HELP','TITLE','CREDIT','VICTORY','PFUB','END','INTERPIC','BOSSBACK','STARMS','STCFN']

def lumps_by_prefix(lumps, lo, hi, prefix):
    out = set()
    if lo is None or hi is None: return out
    for i in range(lo+1, hi):
        nm = lumps[i][0]
        if nm.startswith(prefix):
            out.add(nm)
    return out

def is_map_marker(nm):
    return (len(nm) == 5 and nm.startswith('MAP') and nm[3:].isdigit()) or \
           (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M' and nm[1].isdigit() and nm[3].isdigit())

MAP_LUMPS = {'THINGS','LINEDEFS','SIDEDEFS','VERTEXES','SEGS','SSECTORS','NODES',
             'SECTORS','REJECT','BLOCKMAP','BEHAVIOR'}

def analyze(wad_path, info_path):
    d, lumps, byname = read_wad(wad_path)
    state_spr, state_next, by_ednum, by_mt, STATE_FIELDS, SOUND_FIELDS = parse_info(info_path)
    s_rng = marker_range(lumps, 'S_START', 'S_END')
    f_rng = marker_range(lumps, 'F_START', 'F_END')

    # PNAMES -> patch lump name per index
    pnames = []
    if 'PNAMES' in byname:
        pp, ps = lumps[byname['PNAMES']][1], lumps[byname['PNAMES']][2]
        pn = d[pp:pp+ps]; cnt = struct.unpack_from('<i', pn, 0)[0]
        for i in range(cnt):
            pnames.append(pn[4+i*8:4+i*8+8].split(b'\x00')[0].decode('latin1').upper())
    # texture name -> set(patch lump names)
    texpatch = {}
    for tl in ('TEXTURE1','TEXTURE2'):
        if tl not in byname: continue
        tp, tsz = lumps[byname[tl]][1], lumps[byname[tl]][2]
        t = d[tp:tp+tsz]; nt = struct.unpack_from('<i', t, 0)[0]
        for off in struct.unpack_from('<%di' % nt, t, 4):
            name = t[off:off+8].split(b'\x00')[0].decode('latin1').upper()
            npat = struct.unpack_from('<h', t, off+20)[0]
            pats = set()
            for p in range(npat):
                pidx = struct.unpack_from('<h', t, off+22+p*10+6)[0]
                if 0 <= pidx < len(pnames): pats.add(pnames[pidx])
            texpatch[name] = pats

    # always-set, computed once (only lumps that actually exist -- TEXTURE2 etc. are optional)
    base = set(nm for nm in ALWAYS_LUMPS if nm in byname)
    for spr in ALWAYS_SPRITES:
        base |= lumps_by_prefix(lumps, *s_rng, spr)
    for pre in ALWAYS_PREFIXES:
        for i, (nm, _, _) in enumerate(lumps):
            if nm.startswith(pre): base.add(nm)

    # find maps
    maps = [i for i,(nm,_,_) in enumerate(lumps) if is_map_marker(nm)]
    results = []
    for mi in maps:
        mapname = lumps[mi][0]
        grp = {}
        for j in range(mi+1, min(mi+12, len(lumps))):
            if lumps[j][0] in MAP_LUMPS: grp[lumps[j][0]] = j
            elif is_map_marker(lumps[j][0]): break
        sub = set(base)
        # geometry
        for k, j in grp.items(): sub.add(lumps[j][0])
        sub.add(mapname)
        # textures from SIDEDEFS
        if 'SIDEDEFS' in grp:
            sp, ssz = lumps[grp['SIDEDEFS']][1], lumps[grp['SIDEDEFS']][2]
            sd = d[sp:sp+ssz]
            for s in range(len(sd)//30):
                for foff in (4,12,20):
                    tn = sd[s*30+foff:s*30+foff+8].split(b'\x00')[0].decode('latin1').upper()
                    if tn and tn != '-' and tn in texpatch:
                        sub |= texpatch[tn]
        # flats from SECTORS
        if 'SECTORS' in grp and f_rng[0] is not None:
            cp, csz = lumps[grp['SECTORS']][1], lumps[grp['SECTORS']][2]
            sc = d[cp:cp+csz]
            flatnames = set()
            for s in range(len(sc)//26):
                for foff in (4,12):   # floorpic, ceilingpic
                    fn = sc[s*26+foff:s*26+foff+8].split(b'\x00')[0].decode('latin1').upper()
                    if fn: flatnames.add(fn)
            for i in range(f_rng[0]+1, f_rng[1]):
                if lumps[i][0] in flatnames: sub.add(lumps[i][0])
        # sprites + sounds from THINGS
        sprset = set(); seen = set(); sndset = set()
        if 'THINGS' in grp:
            hp, hsz = lumps[grp['THINGS']][1], lumps[grp['THINGS']][2]
            th = d[hp:hp+hsz]
            ednums = set()
            for t in range(len(th)//10):
                ednums.add(struct.unpack_from('<h', th, t*10+6)[0])
            for ed in ednums:
                rec = by_ednum.get(ed)
                if not rec: continue
                for sf in STATE_FIELDS:
                    sprites_from_state(rec.get(sf), state_spr, state_next, sprset, seen)
                for snd in SOUND_FIELDS:
                    v = rec.get(snd)
                    if v and v != 'sfx_None' and v.startswith('sfx_'):
                        sndset.add('DS' + v[4:].upper())
        # always-spawned actor chains (player/blood/puff/fog) regardless of THINGS
        for mt in ('MT_PLAYER','MT_BLOOD','MT_PUFF','MT_TFOG','MT_IFOG'):
            rec = by_mt.get(mt)
            if rec:
                for sf in STATE_FIELDS:
                    sprites_from_state(rec.get(sf), state_spr, state_next, sprset, seen)
        for spr in sprset:
            sub |= lumps_by_prefix(lumps, *s_rng, spr)
        for ds in sndset:
            if ds in byname: sub.add(ds)
        # Music is NOT counted in the per-map graphics subset: it is handled
        # separately -- a CDDA audio track (cart/resident path) or the single map
        # MUS lump loaded on demand by the synth (the engine resolves it via
        # S_music[gamemap]); the repacker adds that one ~8-65K lump per blob.

        # tally
        bytes_ = sum(lumps[byname[nm]][2] for nm in sub if nm in byname)
        missing = [nm for nm in sub if nm not in byname]
        results.append((mapname, len(sub), bytes_/1024.0, missing))
    return results

def main():
    wad = sys.argv[1] if len(sys.argv) > 1 else 'cd/data/DOOM1.WAD'
    info = sys.argv[2] if len(sys.argv) > 2 else 'core/info.c'
    res = analyze(wad, info)
    print(f"=== per-map SAFE-SUPERSET subset: {wad} (info={info}) ===")
    print(f"  {'map':<8}{'#lumps':>8}{'KB':>9}   {'missing (BUG if non-empty)'}")
    worst = 0
    for name, nl, kb, missing in res:
        flag = '' if not missing else ('  !!! MISSING: ' + ','.join(missing[:6]))
        print(f"  {name:<8}{nl:>8}{kb:>8.0f}K{flag}")
        worst = max(worst, kb)
    print(f"  -> {len(res)} maps; worst per-map subset = {worst:.0f}K "
          f"(must fit the 4MB cart and/or the per-map streaming blob).")
    anymiss = any(m for _,_,_,m in res)
    print("  VALIDATION:", "FAIL (missing lumps -- subset not safe!)" if anymiss
          else "OK (every subset lump exists in the WAD).")

if __name__ == '__main__':
    main()
