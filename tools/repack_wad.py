#!/usr/bin/env python3
# repack_wad.py -- per-level disc-repack tool (STREAMING_ANALYSIS.md S4d / 7.4-7.9).
#
# STEP 1  (subset computer): for each map, compute the SAFE SUPERSET of WAD lumps
#         that map can reference (geometry + textures + flats + sprites + sounds +
#         always-resident UI).  A missing lump in streaming mode is a HARD I_Error,
#         so the subset is deliberately a superset (include-if-in-doubt).
#
# STEP 1b (this file, the emitter):  write a per-map repack container (.DRP) that
#         holds, per map, the map's subset lumps LZSS-compressed and concatenated
#         into ONE contiguous, access-ordered blob, plus a per-map offset table that
#         maps the ORIGINAL lump index -> (offset-in-blob, csize, usize).  The
#         engine keeps the original (name-stable) WAD directory, so lump NUMBERS are
#         unchanged (Option B, no in-engine index relocation); the Step-3 loader just
#         retargets the subset's lumpinfo[].position into the blob and decompresses
#         each lump on page-in.  Round-trip validated here on PC (decode every lump,
#         compare byte-for-byte to the source WAD).
#
# ============================================================================
#  .DRP container format  (little-endian; the Step-3 C loader reads this)
# ============================================================================
#  HEADER (32 bytes):
#    0   4   magic   = "DRP1"
#    4   4   n_lumps   name-stable directory size = source WAD numlumps
#    8   4   dir_crc32 CRC32 over the source directory (per lump: name[8]+le32(size))
#    12  4   n_maps
#    16  4   map_tab_ofs   file offset of the MAP TABLE
#    20  4   codec     1 = LZSS-12/4 (see below)
#    24  4   reserved0 (0)
#    28  4   reserved1 (0)
#
#  MAP TABLE (n_maps entries x 24 bytes, at map_tab_ofs):
#    0   8   map_name  e.g. "E1M1\0\0\0\0" / "MAP01\0\0\0"
#    8   4   n_entries
#    12  4   entries_ofs   file offset of this map's ENTRY TABLE
#    16  4   blob_ofs      file offset of this map's BLOB
#    20  4   blob_size     total compressed bytes of the blob (= sum of csize)
#
#  ENTRY TABLE (per map: n_entries x 16 bytes, at entries_ofs; sorted by lump_idx,
#  STRICTLY INCREASING -- no duplicate lump_idx, so a uint32 binary search is exact):
#    0   4   lump_idx  index into the name-stable directory (== source WAD index)
#    4   4   data_ofs  offset WITHIN the blob (relative to blob_ofs)
#    8   4   csize     compressed bytes.  **csize == usize  <=>  STORED (raw)**,
#                      otherwise LZSS.  pack_lump emits LZSS only when its encoded
#                      length is STRICTLY < usize, so every LZSS stream has
#                      csize < usize and STORED uses csize == usize -- the two are
#                      disjoint and the equality test is exact.
#    12  4   usize     uncompressed bytes
#
#  BLOB (per map, at blob_ofs): the entries' streams concatenated in entry order.
#
#  CODEC 1 -- LZSS-12/4 (THRESHOLD 2, F 18); window = the output buffer itself:
#    Read a FLAG byte, process its 8 bits LSB-first:
#      bit==1  literal : copy the next 1 input byte to the output.
#      bit==0  match   : read 2 input bytes b0,b1 ->
#                          offset = ((b1 & 0x0F) << 8 | b0) + 1     (1..4096)
#                          length = (b1 >> 4) + 3                   (3..18)
#                        copy `length` bytes from output[outpos-offset], one byte
#                        at a time (overlap/run-length copy is intentional).
#    Stop when `usize` output bytes have been produced (a trailing partial flag
#    byte is ignored).  Decoder needs only the output buffer -- no ring buffer.
#    NORMATIVE for decoders: (a) length is clamped so output never exceeds usize --
#    the LAST token may produce fewer than `length` bytes; a decoder MUST stop at
#    exactly usize even mid-match (the output buffer is usize bytes).  (b) A
#    conformant stream always has offset <= bytes-produced-so-far; a decoder MAY
#    assert 1 <= offset <= outpos and treat a violation (or input exhausted before
#    usize) as a hard corrupt-blob error.
#
# ============================================================================
#  SH-2 (big-endian) C-loader read discipline -- MANDATORY for the Step-3 loader
# ============================================================================
#  * ENDIANNESS: every multi-byte field above is stored LITTLE-ENDIAN.  The Saturn
#    SH-2 is BIG-ENDIAN, so the loader MUST read each u32 by byte assembly
#    (v = p[0] | p[1]<<8 | p[2]<<16 | p[3]<<24) or the core LONG()/SHORT() value-swap
#    macros (core/i_swap.h).  Do NOT cast the buffer to a packed struct and
#    dereference -- that byte-swaps wrong on SH-2 (same discipline as core/w_wad.c).
#  * ALIGNMENT: load the container at a 4-byte-aligned address (GFS guarantees this).
#    map_tab_ofs / entries_ofs / blob_ofs are 4-aligned by construction; data_ofs
#    (within the blob) is byte-granular and NOT aligned -- treat the blob as a raw
#    byte stream (the LZSS/STORED decode is byte-wise, so this is fine).
#  * TYPES: hold all offsets/sizes/counts as uint32_t and use unsigned arithmetic.
#  * dir_crc32: CRC32 (zlib polynomial) over, per lump in source directory order,
#    name[8] (uppercased, NUL-padded) followed by size as 4 LITTLE-ENDIAN bytes.
#    On big-endian, re-serialize size to LE before hashing (verify only).
#  * PER-LUMP PATH (name-stable directory, Option B): keep the original WAD directory
#    resident.  For each W_CacheLumpNum(idx): binary-search THIS map's entry table by
#    lump_idx; HIT  => retarget lumpinfo[idx] at blob_ofs+data_ofs with (csize,usize)
#    and decompress on page-in;  MISS => fall back to the full WAD at the original
#    lumpinfo[].position.  (A complete subset minimises/eliminates the MISS path,
#    which matters most for the cart-CDDA mode where the CD is busy.)
#
import os, re, sys, struct, zlib

# ---------------------------------------------------------------- LZSS codec
LZSS_MIN   = 3      # THRESHOLD + 1
LZSS_MAX   = 18     # THRESHOLD + 1 + 15  (4-bit length field)
LZSS_WIN   = 4096   # 12-bit offset field

def lzss_compress(data):
    """Encode `data` with codec-1 LZSS. Returns raw token bytes (no header)."""
    n = len(data)
    if n == 0:
        return b''
    out = bytearray()
    # 3-byte prefix -> recent positions (most-recent first), depth-capped chains.
    chains = {}
    DEPTH = 64
    i = 0
    flag_pos = -1
    flag_bit = 8        # force a new flag byte on first token
    while i < n:
        if flag_bit == 8:
            out.append(0)
            flag_pos = len(out) - 1
            flag_bit = 0
        best_len = 0
        best_off = 0
        if i + LZSS_MIN <= n:
            key = data[i:i+3]
            lo = i - LZSS_WIN
            cand = chains.get(key)
            if cand:
                maxlen = min(LZSS_MAX, n - i)
                depth = 0
                for p in cand:
                    if p < lo:
                        break
                    depth += 1
                    if depth > DEPTH:
                        break
                    # extend match
                    l = 0
                    while l < maxlen and data[p+l] == data[i+l]:
                        l += 1
                    if l > best_len:
                        best_len = l
                        best_off = i - p
                        if l >= maxlen:
                            break
        if best_len >= LZSS_MIN:
            b0 = (best_off - 1) & 0xFF
            b1 = (((best_off - 1) >> 8) & 0x0F) | (((best_len - LZSS_MIN) & 0x0F) << 4)
            out.append(b0); out.append(b1)
            # flag bit stays 0 (match)
            advance = best_len
        else:
            out[flag_pos] |= (1 << flag_bit)     # literal
            out.append(data[i])
            advance = 1
        flag_bit += 1
        # insert positions consumed by this token into the chains
        end = i + advance
        j = i
        while j < end and j + LZSS_MIN <= n:
            k = data[j:j+3]
            lst = chains.get(k)
            if lst is None:
                lst = []
                chains[k] = lst
            lst.insert(0, j)
            if len(lst) > DEPTH:
                del lst[DEPTH:]
            j += 1
        i = end
    return bytes(out)

def lzss_decompress(buf, usize):
    """Reference decoder -- models the Step-3 C loader EXACTLY, including the safe
    behaviours a from-spec C decoder must have: it stops at exactly `usize` even
    mid-match (the C output buffer is usize bytes), and it asserts the conformance
    invariant 1 <= offset <= bytes-produced (a violation = corrupt blob)."""
    out = bytearray()
    p = 0
    blen = len(buf)
    while len(out) < usize:
        if p >= blen:
            raise AssertionError("LZSS: input exhausted before usize (corrupt blob)")
        flags = buf[p]; p += 1
        for _ in range(8):
            if len(out) >= usize:
                break
            if flags & 1:
                out.append(buf[p]); p += 1
            else:
                b0 = buf[p]; b1 = buf[p+1]; p += 2
                offset = ((b1 & 0x0F) << 8 | b0) + 1
                length = (b1 >> 4) + LZSS_MIN
                assert 1 <= offset <= len(out), \
                    "LZSS: offset %d > produced %d (corrupt blob)" % (offset, len(out))
                start = len(out) - offset
                for k in range(length):
                    if len(out) >= usize:          # never overshoot the output buffer
                        break
                    out.append(out[start + k])
            flags >>= 1
    return bytes(out)

def pack_lump(data):
    """Return (method, payload): method 0=stored, 1=lzss.  Never expands."""
    if len(data) == 0:
        return 0, b''
    enc = lzss_compress(data)
    if len(enc) < len(data):
        return 1, enc
    return 0, data

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

def dir_crc32(d, lumps):
    """CRC32 over the source directory (name[8]+le32(size)) -- loader fingerprint."""
    c = 0
    for nm, _, sz in lumps:
        rec = nm.encode('latin1')[:8].ljust(8, b'\x00') + struct.pack('<i', sz)
        c = zlib.crc32(rec, c)
    return c & 0xFFFFFFFF

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
    blocks = re.findall(r'\{\s*//\s*(MT_\w+)(.*?)\n\s*\},', mbody, re.S)
    for mtname, blk in blocks:
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
    'PLAY','PUNG','PISG','PISF','SHTG','SHTF','SHT2','CHGG','CHGF','MISG','MISF',
    'SAWG','PLSG','PLSF','BFGG','BFGF',
    'BLUD','PUFF','MISL','TFOG','IFOG','BAL1','BAL2','PLSS','PLSE',
]
ALWAYS_LUMPS = [
    'PLAYPAL','COLORMAP','PNAMES','TEXTURE1','TEXTURE2','ENDOOM',
    'STBAR','STGNUM0','STTPRCNT','STTMINUS',
]
ALWAYS_PREFIXES = ['STF','STT','STG','STK','STD','STY','STC','STP','M_','WI','CWILV',
                   'AMMNUM','BRDR','HELP','TITLE','CREDIT','VICTORY','PFUB','END',
                   'INTERPIC','BOSSBACK','STARMS','STCFN']

# Sky textures (core/g_game.c): chosen in code by episode/map, NEVER named by a
# SIDEDEF -> the static subset would miss them.  Superset-safe: include all.
SKY_TEXTURES = ['SKY1','SKY2','SKY3','SKY4']

# Animated flats/textures (core/p_spec.c animdefs[]): a SIDEDEF/SECTOR names ONE
# frame but the engine cycles EVERY frame between start and end BY DIRECTORY/TEXTURE
# POSITION.  We expand any referenced pic that lands inside a range to the whole
# range.  (start, end) pairs, transcribed from animdefs[]:
ANIM_FLATS = [('NUKAGE1','NUKAGE3'),('FWATER1','FWATER4'),('SWATER1','SWATER4'),
              ('LAVA1','LAVA4'),('BLOOD1','BLOOD3'),('RROCK05','RROCK08'),
              ('SLIME01','SLIME04'),('SLIME05','SLIME08'),('SLIME09','SLIME12')]
ANIM_TEX   = [('BLODGR1','BLODGR4'),('SLADRIP1','SLADRIP3'),('BLODRIP1','BLODRIP4'),
              ('FIREWALA','FIREWALL'),('GSTFONT1','GSTFONT3'),('FIRELAV3','FIRELAVA'),
              ('FIREMAG1','FIREMAG3'),('FIREBLU1','FIREBLU2'),('ROCKRED1','ROCKRED3'),
              ('BFALL1','BFALL4'),('SFALL1','SFALL4'),('WFALL1','WFALL4'),
              ('DBRAIN1','DBRAIN4')]

# Switch texture PAIRS (core/p_switch.c alphSwitchList[]): a switch wall shows one
# of the pair; using it swaps to the partner.  A map names only one -> add both.
SWITCH_PAIRS = [
    ('SW1BRCOM','SW2BRCOM'),('SW1BRN1','SW2BRN1'),('SW1BRN2','SW2BRN2'),
    ('SW1BRNGN','SW2BRNGN'),('SW1BROWN','SW2BROWN'),('SW1COMM','SW2COMM'),
    ('SW1COMP','SW2COMP'),('SW1DIRT','SW2DIRT'),('SW1EXIT','SW2EXIT'),
    ('SW1GRAY','SW2GRAY'),('SW1GRAY1','SW2GRAY1'),('SW1METAL','SW2METAL'),
    ('SW1PIPE','SW2PIPE'),('SW1SLAD','SW2SLAD'),('SW1STARG','SW2STARG'),
    ('SW1STON1','SW2STON1'),('SW1STON2','SW2STON2'),('SW1STONE','SW2STONE'),
    ('SW1STRTN','SW2STRTN'),('SW1BLUE','SW2BLUE'),('SW1CMT','SW2CMT'),
    ('SW1GARG','SW2GARG'),('SW1GSTON','SW2GSTON'),('SW1HOT','SW2HOT'),
    ('SW1LION','SW2LION'),('SW1SATYR','SW2SATYR'),('SW1SKIN','SW2SKIN'),
    ('SW1VINE','SW2VINE'),('SW1WOOD','SW2WOOD'),('SW1PANEL','SW2PANEL'),
    ('SW1ROCK','SW2ROCK'),('SW1MET2','SW2MET2'),('SW1WDMET','SW2WDMET'),
    ('SW1BRIK','SW2BRIK'),('SW1MOD1','SW2MOD1'),('SW1ZIM','SW2ZIM'),
    ('SW1STON6','SW2STON6'),('SW1TEK','SW2TEK'),('SW1MARB','SW2MARB'),
    ('SW1SKULL','SW2SKULL'),
]
SWITCH_PARTNER = {}
for _a, _b in SWITCH_PAIRS:
    SWITCH_PARTNER[_a] = _b; SWITCH_PARTNER[_b] = _a

# Sounds played from CODE (S_StartSound with no map THING): weapons (core/p_pspr.c),
# doors/plats/switches (p_doors.c/p_floor.c/p_plats.c), pickups (p_inter.c),
# player/HUD/menu (p_map.c/hu_stuff.c/m_menu.c).  The THINGS->mobjinfo sound graph
# never reaches these, so they must be forced resident.  Each DS* lump is a few KB.
# (Sounds are cached LAZILY on first play via cache_sfx->W_CacheLumpNum in
# src/i_sound_saturn.cxx -- I_PrecacheSounds is a no-op stub -- so a code-played
# sound's lump IS paged in at runtime and must be reachable from the map's blob.)
ALWAYS_SOUNDS = [
    'DSPISTOL','DSSHOTGN','DSDSHTGN','DSSGCOCK','DSPUNCH','DSSAWUP','DSSAWIDL',
    'DSSAWFUL','DSSAWHIT','DSBFG','DSPLASMA','DSRLAUNC',           # weapons
    'DSDOROPN','DSDORCLS','DSBDOPN','DSBDCLS','DSSTNMOV','DSPSTOP',
    'DSPSTART','DSSWTCHN','DSSWTCHX',                              # doors/plats/switches
    'DSITEMUP','DSWPNUP','DSGETPOW',                              # pickups
    'DSOOF','DSNOWAY','DSTINK','DSRADIO','DSTELEPT',              # player/HUD/teleport
]

# Finale background flats (core/f_finale.c textscreens[]): chosen by gamemap, loaded
# via W_CacheLumpName(finaleflat), never named by a SECTOR.  Superset-safe: all.
FINALE_FLATS = ['FLOOR4_8','MFLR8_3','MFLR8_4',                  # Doom 1
                'SLIME16','RROCK14','RROCK07','RROCK17','RROCK13','RROCK19']  # Doom 2

# Doom II cast call (core/f_finale.c castorder[], shown on MAP30): renders EVERY
# listed monster's states + plays their see/death/attack sounds regardless of what
# the map actually contains.  NOT forced into MAP30's blob -- see the per-map loop:
# a one-time static end screen served by full-WAD fallback.  Kept for reference.
CAST_MOBJTYPES = ['MT_POSSESSED','MT_SHOTGUY','MT_CHAINGUY','MT_TROOP','MT_SERGEANT',
                  'MT_SKULL','MT_HEAD','MT_KNIGHT','MT_BRUISER','MT_BABY','MT_PAIN',
                  'MT_UNDEAD','MT_FATSO','MT_VILE','MT_SPIDER','MT_CYBORG','MT_PLAYER']

def lumps_by_prefix(lumps, lo, hi, prefix):
    out = set()                # NOTE: returns INDICES (S_*/F_* lumps are unique-named)
    if lo is None or hi is None: return out
    for i in range(lo+1, hi):
        if lumps[i][0].startswith(prefix):
            out.add(i)
    return out

def is_map_marker(nm):
    return (len(nm) == 5 and nm.startswith('MAP') and nm[3:].isdigit()) or \
           (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M' and nm[1].isdigit() and nm[3].isdigit())

MAP_LUMPS = {'THINGS','LINEDEFS','SIDEDEFS','VERTEXES','SEGS','SSECTORS','NODES',
             'SECTORS','REJECT','BLOCKMAP','BEHAVIOR'}

def compute_subsets(wad_path, info_path):
    """Return (d, lumps, byname, maps) where maps = [(name, marker_idx, idx_set)].
    idx_set is a set of ORIGINAL lump indices (not names: map sub-lumps such as
    THINGS share names across maps, so we resolve them by position)."""
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
    # texture name -> set(patch lump names), plus the LOAD ORDER (TEXTURE1 then
    # TEXTURE2) so we can expand animated-texture/switch ranges, which the engine
    # cycles by texture INDEX (R_TextureNumForName order), not by name.
    texpatch = {}
    texorder = []              # texname in engine load order
    texidx   = {}              # texname -> index in texorder
    for tl in ('TEXTURE1','TEXTURE2'):
        if tl not in byname: continue
        tp, tsz = lumps[byname[tl]][1], lumps[byname[tl]][2]
        t = d[tp:tp+tsz]; nt = struct.unpack_from('<i', t, 0)[0]
        for off in struct.unpack_from('<%di' % nt, t, 4):
            name = t[off:off+8].split(b'\x00')[0].decode('latin1').upper()
            npat = struct.unpack_from('<h', t, off+20)[0]
            pats = set()
            for p in range(npat):
                pidx = struct.unpack_from('<h', t, off+22+p*10+4)[0]
                if 0 <= pidx < len(pnames): pats.add(pnames[pidx])
            texpatch[name] = pats
            if name not in texidx:
                texidx[name] = len(texorder); texorder.append(name)

    def name_idx(nm):
        return byname.get(nm)

    # always-set as INDICES (only lumps that actually exist)
    base = set()
    for nm in ALWAYS_LUMPS:
        if nm in byname: base.add(byname[nm])
    for spr in ALWAYS_SPRITES:
        base |= lumps_by_prefix(lumps, *s_rng, spr)
    for pre in ALWAYS_PREFIXES:
        for i, (nm, _, _) in enumerate(lumps):
            if nm.startswith(pre): base.add(i)
    # sky textures: selected in code, never named by a SIDEDEF -> add their patches
    for sky in SKY_TEXTURES:
        for pat in texpatch.get(sky, ()):
            ix = name_idx(pat)
            if ix is not None: base.add(ix)
    # code-played sounds + finale background flats: referenced by name in code, not
    # derivable from map data -> force resident on every map (cheap, superset-safe)
    for nm in ALWAYS_SOUNDS + FINALE_FLATS:
        if nm in byname: base.add(byname[nm])

    # resolve animation ranges once: flats by LUMP index, textures by TEXTURE index
    anim_flat_ranges = []
    for sname, ename in ANIM_FLATS:
        si, ei = byname.get(sname), byname.get(ename)
        if si is not None and ei is not None:
            anim_flat_ranges.append((min(si,ei), max(si,ei)))
    anim_tex_ranges = []
    for sname, ename in ANIM_TEX:
        si, ei = texidx.get(sname), texidx.get(ename)
        if si is not None and ei is not None:
            anim_tex_ranges.append((min(si,ei), max(si,ei)))

    maps = [i for i,(nm,_,_) in enumerate(lumps) if is_map_marker(nm)]
    out = []
    for mi in maps:
        mapname = lumps[mi][0]
        grp = {}
        for j in range(mi+1, min(mi+12, len(lumps))):
            if lumps[j][0] in MAP_LUMPS: grp[lumps[j][0]] = j
            elif is_map_marker(lumps[j][0]): break
        sub = set(base)
        sub.add(mi)                                  # the map marker lump itself
        for k, j in grp.items(): sub.add(j)          # geometry (by index, this map's)
        # textures from SIDEDEFS (collect names, then expand switches + animations)
        texref = set()
        if 'SIDEDEFS' in grp:
            sp, ssz = lumps[grp['SIDEDEFS']][1], lumps[grp['SIDEDEFS']][2]
            sd = d[sp:sp+ssz]
            for s in range(len(sd)//30):
                for foff in (4,12,20):
                    tn = sd[s*30+foff:s*30+foff+8].split(b'\x00')[0].decode('latin1').upper()
                    if tn and tn != '-' and tn in texpatch:
                        texref.add(tn)
        # switch partner (SW1<->SW2): a switch swaps to the other half on use
        for nm in list(texref):
            p = SWITCH_PARTNER.get(nm)
            if p and p in texpatch: texref.add(p)
        # animated textures: if any frame is referenced, the engine cycles the whole
        # texture-index range -> include every frame
        hit = set()
        for nm in texref:
            ti = texidx.get(nm)
            if ti is not None:
                for lo, hi in anim_tex_ranges:
                    if lo <= ti <= hi: hit.add((lo, hi))
        for lo, hi in hit:
            for k in range(lo, hi+1): texref.add(texorder[k])
        for tn in texref:
            for pat in texpatch.get(tn, ()):
                ix = name_idx(pat)
                if ix is not None: sub.add(ix)
        # flats from SECTORS (collect indices, then expand animation ranges)
        if 'SECTORS' in grp and f_rng[0] is not None:
            cp, csz = lumps[grp['SECTORS']][1], lumps[grp['SECTORS']][2]
            sc = d[cp:cp+csz]
            flatnames = set()
            for s in range(len(sc)//26):
                for foff in (4,12):
                    fn = sc[s*26+foff:s*26+foff+8].split(b'\x00')[0].decode('latin1').upper()
                    if fn: flatnames.add(fn)
            flatref = set()
            for i in range(f_rng[0]+1, f_rng[1]):
                if lumps[i][0] in flatnames: flatref.add(i)
            # animated flats: any referenced frame -> include the whole lump range
            for lo, hi in anim_flat_ranges:
                if any(lo <= fi <= hi for fi in flatref):
                    for k in range(lo, hi+1): flatref.add(k)
            sub |= flatref
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
        for mt in ('MT_PLAYER','MT_BLOOD','MT_PUFF','MT_TFOG','MT_IFOG'):
            rec = by_mt.get(mt)
            if rec:
                for sf in STATE_FIELDS:
                    sprites_from_state(rec.get(sf), state_spr, state_next, sprset, seen)
        # The Doom II MAP30 cast call (every castorder[] monster's sprites+sounds) is
        # deliberately NOT forced into MAP30's blob: it is a one-time STATIC end screen,
        # and unioning all ~17 monster sets pushes MAP30 to ~4.05 MB -- it eats the cart
        # headroom and overflows on bigger PWADs.  The Step-3 loader serves the cast via
        # full-WAD fallback (the CD is free on a static screen; cart-CDDA uses the
        # Option-2 music fade).  Gameplay assets above stay complete so play is smooth;
        # only the static finale falls back.  (CAST_MOBJTYPES is kept for reference.)
        for spr in sprset:
            sub |= lumps_by_prefix(lumps, *s_rng, spr)
        for ds in sndset:
            if ds in byname: sub.add(byname[ds])
        # Music: handled separately (CDDA track or single MUS lump on demand) -- not
        # part of the per-map graphics subset.
        out.append((mapname, mi, sub))
    return d, lumps, byname, out

# ---------------------------------------------------------------- report
def report(wad, info):
    d, lumps, byname, maps = compute_subsets(wad, info)
    print(f"=== per-map SAFE-SUPERSET subset: {wad} (info={info}) ===")
    print(f"  {'map':<8}{'#lumps':>8}{'raw KB':>9}{'lzss KB':>9}{'ratio':>7}")
    cache = {}                 # lump_idx -> (method, payload)
    worst = worstc = 0
    for name, mi, sub in maps:
        raw = comp = 0
        for ix in sub:
            sz = lumps[ix][2]
            raw += sz
            if ix not in cache:
                fp = lumps[ix][1]
                cache[ix] = pack_lump(d[fp:fp+sz])
            comp += len(cache[ix][1])
        kb, ckb = raw/1024.0, comp/1024.0
        ratio = (ckb/kb) if kb else 0
        print(f"  {name:<8}{len(sub):>8}{kb:>8.0f}K{ckb:>8.0f}K{ratio:>7.0%}")
        worst = max(worst, kb); worstc = max(worstc, ckb)
    print(f"  -> {len(maps)} maps; worst raw = {worst:.0f}K, worst lzss = {worstc:.0f}K "
          f"(4MB cart = 4096K).")

# ---------------------------------------------------------------- emit container
HDR_FMT = '<4sIIIIIII'    # magic, n_lumps, dir_crc32, n_maps, map_tab_ofs, codec, r0, r1
HDR_SZ  = 32
MAP_FMT = '<8sIIII'       # name, n_entries, entries_ofs, blob_ofs, blob_size
MAP_SZ  = 24
ENT_FMT = '<IIII'         # lump_idx, data_ofs, csize, usize  (all unsigned/non-negative)
ENT_SZ  = 16
CODEC_LZSS = 1

def sprite_headers(d, lumps):
    """R3.1 boot index: for each sprite lump between the S_START/S_END markers, the
    (width, leftoffset, topoffset) shorts from its patch header, packed little-endian
    (offsets are signed).  Mirrors core/r_data.c R_InitSpriteLumps EXACTLY: the same
    marker range (last S_START .. last S_END, W_GetNumForName's last-occurrence rule,
    which marker_range() already reproduces) and the same three header fields.  The
    loader (w_drp_saturn.cxx) reads this in one pass and skips caching every sprite
    lump at boot.  Returns (packed_bytes, count)."""
    s, e = marker_range(lumps, 'S_START', 'S_END')
    if s is None or e is None or e <= s + 1:
        return b'', 0
    out = bytearray()
    n = 0
    for i in range(s + 1, e):                 # firstspritelump .. lastspritelump
        _nm, fp, sz = lumps[i]
        if sz >= 8:
            w, _h, lo, to = struct.unpack_from('<hhhh', d, fp)
        else:
            w = lo = to = 0                   # not a patch -> zeros (engine would read garbage too)
        out += struct.pack('<hhh', w, lo, to)
        n += 1
    return bytes(out), n

def emit(wad, info, out_path):
    d, lumps, byname, maps = compute_subsets(wad, info)
    n_lumps = len(lumps)
    cache = {}                 # lump_idx -> (method, payload)
    def packed(ix):
        if ix not in cache:
            fp, sz = lumps[ix][1], lumps[ix][2]
            cache[ix] = pack_lump(d[fp:fp+sz])
        return cache[ix]

    # Layout: HEADER | MAP TABLE | (per map: ENTRY TABLE then BLOB)
    map_tab_ofs = HDR_SZ
    cursor = map_tab_ofs + MAP_SZ * len(maps)
    map_recs = []              # (name, entries[], entries_ofs, blob_ofs, blob_size)
    for name, mi, sub in maps:
        entries = []           # (lump_idx, data_ofs, csize, usize)
        blob = bytearray()
        for ix in sorted(sub):
            method, payload = packed(ix)
            usize = lumps[ix][2]
            csize = len(payload) if method == 1 else usize    # stored: csize==usize
            data_ofs = len(blob)
            blob += payload if method == 1 else d[lumps[ix][1]:lumps[ix][1]+usize]
            entries.append((ix, data_ofs, csize, usize))
        entries_ofs = cursor
        blob_ofs = entries_ofs + ENT_SZ * len(entries)
        blob_size = len(blob)
        map_recs.append((name, entries, entries_ofs, blob_ofs, bytes(blob), blob_size))
        cursor = blob_ofs + blob_size

    # SATURN R3.1 boot index: precomputed sprite-header section, appended after the
    # map blobs.  Consumed in one sequential read by R_InitSpriteLumps instead of
    # caching every sprite lump at boot.  Located via the header's two spare u32
    # (sprh_ofs, sprh_n); an older loader ignores them, a pre-R3.1 .DRP reports 0.
    sprh, sprh_n = sprite_headers(d, lumps)
    sprh_ofs = cursor if sprh_n else 0
    total = cursor + len(sprh)

    # serialize
    buf = bytearray(total)
    struct.pack_into(HDR_FMT, buf, 0, b'DRP1', n_lumps, dir_crc32(d, lumps),
                     len(maps), map_tab_ofs, CODEC_LZSS, sprh_ofs, sprh_n)
    for k, (name, entries, entries_ofs, blob_ofs, blob, blob_size) in enumerate(map_recs):
        nm8 = name.encode('latin1')[:8].ljust(8, b'\x00')
        struct.pack_into(MAP_FMT, buf, map_tab_ofs + k*MAP_SZ,
                         nm8, len(entries), entries_ofs, blob_ofs, blob_size)
        for e, (lix, dofs, csz, usz) in enumerate(entries):
            struct.pack_into(ENT_FMT, buf, entries_ofs + e*ENT_SZ, lix, dofs, csz, usz)
        buf[blob_ofs:blob_ofs+blob_size] = blob
    if sprh_n:
        buf[sprh_ofs:sprh_ofs+len(sprh)] = sprh

    open(out_path, 'wb').write(buf)
    return bytes(buf), d, lumps

# ---------------------------------------------------------------- round-trip
def verify(container, d, lumps):
    """Parse `container` from scratch and decode every lump; compare to source."""
    magic, n_lumps, crc, n_maps, map_tab_ofs, codec, sprh_ofs, sprh_n = struct.unpack_from(HDR_FMT, container, 0)
    assert magic == b'DRP1', "bad magic"
    assert codec == CODEC_LZSS, f"unknown codec {codec}"
    assert n_lumps == len(lumps), f"n_lumps {n_lumps} != WAD {len(lumps)}"
    assert crc == dir_crc32(d, lumps), "directory CRC mismatch"
    n_lumps_checked = 0
    worst_blob = 0
    for k in range(n_maps):
        nm8, n_entries, entries_ofs, blob_ofs, blob_size = struct.unpack_from(MAP_FMT, container, map_tab_ofs + k*MAP_SZ)
        mapname = nm8.split(b'\x00')[0].decode('latin1')
        worst_blob = max(worst_blob, blob_size)
        prev = -1
        for e in range(n_entries):
            lix, dofs, csz, usz = struct.unpack_from(ENT_FMT, container, entries_ofs + e*ENT_SZ)
            assert lix > prev, f"{mapname}: entry table not sorted by lump_idx"
            prev = lix
            assert 0 <= lix < n_lumps, f"{mapname}: lump_idx {lix} out of range"
            assert csz <= usz, f"{mapname}: csize {csz} > usize {usz} (must never expand)"
            stream = container[blob_ofs+dofs : blob_ofs+dofs+csz]
            if csz == usz:                       # STORED
                dec = stream
            else:                                # LZSS
                dec = lzss_decompress(stream, usz)
            fp, sz = lumps[lix][1], lumps[lix][2]
            src = d[fp:fp+sz]
            if dec != src:
                raise AssertionError(f"{mapname}: lump #{lix} ({lumps[lix][0]}) "
                                     f"round-trip MISMATCH (usz={usz}, got {len(dec)})")
            n_lumps_checked += 1

    # SATURN R3.1: the sprite-header index must reproduce R_InitSpriteLumps' reads
    # byte-for-byte -- recompute from source and compare.
    exp, exp_n = sprite_headers(d, lumps)
    assert exp_n == sprh_n, f"sprh count {sprh_n} != recomputed {exp_n}"
    if sprh_n:
        got = container[sprh_ofs:sprh_ofs + len(exp)]
        assert got == exp, "sprite-header index round-trip MISMATCH"
    return n_lumps_checked, n_maps, worst_blob

# ---------------------------------------------------------------- self-test codec
def codec_selftest():
    import os as _os
    cases = [b'', b'A', b'AB', b'ABC', b'AAAAAAAAAA', bytes(range(256))*4,
             b'the quick brown fox '*50, _os.urandom(5000),
             (b'\x00'*5000), b'abcabcabcabcabcabc'*100]
    for c in cases:
        enc = lzss_compress(c)
        dec = lzss_decompress(enc, len(c))
        assert dec == c, f"codec self-test FAIL len={len(c)} got={len(dec)}"
    return len(cases)

# ---------------------------------------------------------------- main
def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = [a for a in sys.argv[1:] if a.startswith('--')]
    wad  = args[0] if len(args) > 0 else 'cd/data/DOOM1.WAD'
    info = args[1] if len(args) > 1 else 'core/info.c'
    out  = None
    want_report = False
    if_stale = False
    for o in opts:
        if o.startswith('--emit='): out = o.split('=',1)[1]
        elif o == '--emit':        out = 'cd/data/DOOMRP.DRP'
        elif o == '--report':      want_report = True
        elif o == '--if-stale':    if_stale = True   # skip emit if the .DRP already matches the WAD
        elif o == '--selftest':
            print("codec self-test:", codec_selftest(), "cases OK"); return

    # --if-stale: regenerate only when the existing .DRP does NOT match this WAD.  Compares
    # the .DRP header's n_lumps + dir_crc32 to the WAD -- catches a WAD *swap* (which file
    # mtime alone misses, since the swapped-in file can be older than the .DRP).
    if out and if_stale and os.path.exists(out):
        try:
            h = open(out, 'rb').read(HDR_SZ)
            magic, n_lumps, crc = struct.unpack_from('<4sII', h, 0)
            d, lumps, _ = read_wad(wad)
            if magic == b'DRP1' and n_lumps == len(lumps) and crc == dir_crc32(d, lumps):
                print(f"{out} already matches {wad} (CRC {crc:#010x}) -- skipping repack.")
                return
            print(f"{out} stale vs {wad} -- regenerating.")
        except Exception as e:
            print(f"{out} unreadable ({e}) -- regenerating.")

    # plain run -> report; --emit -> emit only (single compression pass, for builds);
    # --emit --report -> both.
    if out is None or want_report:
        report(wad, info)
    if out:
        print(f"\n=== emit + round-trip: {out} ===")
        print("  codec self-test:", codec_selftest(), "cases OK")
        _, d, lumps = emit(wad, info, out)
        container = open(out, 'rb').read()       # re-read the artifact from disk (true round-trip)
        nlc, nmaps, worst = verify(container, d, lumps)
        print(f"  wrote {len(container)/1024.0:.0f}K  ({nmaps} maps, {nlc} lump instances)")
        print(f"  worst per-map blob = {worst/1024.0:.0f}K (4MB cart = 4096K)")
        _sprh, _sprh_n = sprite_headers(d, lumps)
        print(f"  R3.1 sprite-header index = {_sprh_n} sprites, {len(_sprh)/1024.0:.1f}K "
              f"(boot: {_sprh_n} sprite-lump reads -> 1)")
        print("  ROUND-TRIP: OK (every lump decodes byte-identical to the source WAD).")

if __name__ == '__main__':
    main()
