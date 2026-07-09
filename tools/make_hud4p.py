#!/usr/bin/env python3
"""Generate the Mimas 3/4-player compact HUD band (160x16) + layout.

3/4-player split gives each quadrant a 160x16 OPAQUE HUD band at the bottom of its
3D view (view 160x96 -> ~14% fewer px/quadrant). Opaque in NBG1 so it occludes the
VDP1 wall layer (index 0 = VDP1 shows through -> every band pixel MUST be non-zero).

Look = a compact slice of the REAL STBAR:
  - background: STBAR brushed metal tiled to 160 px, bottom bevel only (no dark top
    line -- it read as a "1px void" between the weapon and the band);
  - baked labels "AM"/"HE"/"AR": only the ORIGINAL engraved STBAR strokes/highlights;
  - the band is split into THREE EQUAL THIRDS, one stat per third, each group
    (label + number + '%') laid out identically and sized for 3 digits so the layout
    never jitters; ALL numbers are the BIG red message font (ammo too), left-justified
    right after their label (label+number snug), vertically centred;
  - keys / frags are an OVERLAY on the 3D view (bottom-right, just above the band):
    STKEYS icons upright (co-op) or the frag count (deathmatch).

Emits:
  - core/hud4p_layout.h  : band size + per-third number LEFT edges + overlay anchors
  - src/hud4p_panel.h    : the 160x16 8bpp band background (metal + engraved labels)
  - tools/hud4p_preview.png (band + a mock of the view-overlay row above it)
"""
import struct, zlib

WAD = "cd/data/DOOM1.WAD"
data = open(WAD, "rb").read()
_, numl, diroff = struct.unpack_from("<4sii", data, 0)
lumps = {}
for i in range(numl):
    off, sz, name = struct.unpack_from("<ii8s", data, diroff + i*16)
    lumps[name.rstrip(b"\0").decode("ascii", "replace")] = (off, sz)

def playpal0():
    off, _ = lumps["PLAYPAL"]
    return [tuple(data[off+i*3:off+i*3+3]) for i in range(256)]

def decode_patch(name):
    off, _ = lumps[name]
    w, h, lo, to = struct.unpack_from("<hhhh", data, off)
    colofs = struct.unpack_from("<%di" % w, data, off+8)
    img = [[None]*w for _ in range(h)]
    for x in range(w):
        p = off + colofs[x]
        while True:
            top = data[p]; p += 1
            if top == 0xFF: break
            cnt = data[p]; p += 2
            for k in range(cnt):
                y = top + k
                if 0 <= y < h: img[y][x] = data[p]
                p += 1
            p += 1
    return w, h, lo, to, img

PAL = playpal0()
_, _, _, _, STBAR = decode_patch("STBAR")

W, H = 160, 16

# ---------- fonts ----------
def huf(ch): return decode_patch("STCFN%03d" % ord(ch))
DIGIT_HUF = [huf(chr(ord('0')+d)) for d in range(10)]   # message font (red) -- ALL numbers
PCT_HUF   = huf('%')
KEYS      = [decode_patch("STKEYS%d" % d) for d in range(6)]
KEY_W     = max(k[0] for k in KEYS)
KEY_H     = max(k[1] for k in KEYS)

# ---------- original STBAR engraved labels ----------
LABELS = { "AM": (8, 14), "HE": (55, 15), "AR": (188, 13) }
LBL_SY, LBL_H = 23, 7
LBL_METAL = range(99, 109)

# ---------- vertical centring ----------
NY  = (H - DIGIT_HUF[0][1]) // 2          # 7px red digits -> 4 (all band numbers)
LY  = NY                                  # labels align with the numbers

# ---------- band layout: AM flush-left (no %, narrower), armor % flush-right, HE centred ----------
# All numbers sized for 3 digits and RIGHT-justified (digits end at *_X); '%' at *_X.  Labels
# baked at *_label with a comfortable GAP_LN to the number (not crammed against the 3rd digit).
GAP_LN = 4
dig_w  = max(g[0] for g in DIGIT_HUF)      # widest red digit
num_w  = 3 * dig_w                          # a 3-digit number
pct_w  = PCT_HUF[0]

layout = {}
# AM: almost flush left, no % -> the narrowest of the three
layout["AM_label"] = 1
layout["AMMO_X"]   = layout["AM_label"] + LABELS["AM"][1] + GAP_LN + num_w      # digits RIGHT edge
# AR: armor % flush right (percent ends 2 px from the edge)
layout["ARMOR_X"]  = W - 2 - pct_w                                             # digits right edge = % left
layout["AR_label"] = layout["ARMOR_X"] - num_w - GAP_LN - LABELS["AR"][1]
# HE: centred in the gap between the AM and AR sections
he_gw = LABELS["HE"][1] + GAP_LN + num_w + pct_w
he_lo, he_hi = layout["AMMO_X"], layout["AR_label"]
layout["HE_label"] = he_lo + (he_hi - he_lo - he_gw) // 2
layout["HEALTH_X"] = layout["HE_label"] + LABELS["HE"][1] + GAP_LN + num_w      # digits RIGHT edge
print("AM lbl@%d num@%d | HE lbl@%d num@%d | AR lbl@%d num@%d %%@%d"
      % (layout["AM_label"], layout["AMMO_X"], layout["HE_label"], layout["HEALTH_X"],
         layout["AR_label"], layout["ARMOR_X"], layout["ARMOR_X"]))
assert layout["AM_label"] >= 0 and layout["ARMOR_X"] + pct_w <= W and layout["HE_label"] > he_lo

# overlay (on the 3D view, bottom-right, just above the band): keys or frags
KEY_MARGIN  = 2
KEY_DX      = KEY_W + KEY_MARGIN
KEYS_OVL_X  = W - (3*KEY_W + 2*KEY_MARGIN) - 2
FRAGS_OVL_X = W - 2

# ---------- background: brushed metal (bottom bevel only) + engraved labels ----------
SRC_X0, SRC_W = 54, 48
def metal(bx, by):
    sy = 31 if by == H-1 else 2 + by      # no top bevel
    v = STBAR[sy][SRC_X0 + (bx % SRC_W)]
    return v if v is not None else 105
pan = [[metal(bx, by) for bx in range(W)] for by in range(H)]

def bake_label(word, dx):
    sx, w = LABELS[word]
    for j in range(LBL_H):
        for i in range(w):
            v = STBAR[LBL_SY + j][sx + i]
            if v is None or v in LBL_METAL: continue
            tx, ty = dx + i, LY + j
            if 0 <= tx < W and 0 <= ty < H:
                pan[ty][tx] = v
bake_label("AM", layout["AM_label"])
bake_label("HE", layout["HE_label"])
bake_label("AR", layout["AR_label"])

assert all(pan[y][x] != 0 for y in range(H) for x in range(W)), \
    "band bg must be non-zero everywhere (index 0 = VDP1 shows through the band)"

# ---------- emit ----------
def emit_layout(path):
    with open(path, "w", newline="\n") as f:
        f.write("/* GENERATED by tools/make_hud4p.py -- DO NOT EDIT BY HAND.\n")
        f.write("   3/4-player compact HUD band: size + anchors.  AM is flush-left (no %%, narrower),\n")
        f.write("   the armor %% is flush-right, HE is centred between.  AMMO_X/HEALTH_X/ARMOR_X are\n")
        f.write("   number RIGHT edges (all big red message font, right-justified in a 3-digit slot);\n")
        f.write("   the red '%%' is drawn AT HEALTH_X/ARMOR_X.  Labels are baked with a comfortable\n")
        f.write("   gap.  Keys/frags are a view OVERLAY at (ox + KEYS_X + i*KEYS_DX, oy - KEY_H). */\n")
        f.write("#ifndef HUD4P_LAYOUT_H\n#define HUD4P_LAYOUT_H\n\n")
        f.write("#define HUD4P_W %d\n#define HUD4P_H %d\n" % (W, H))
        f.write("#define HUD4P_QUAD_H 112  /* SCREENHEIGHT/2: quadrant height (view + band) */\n\n")
        f.write("#define HUD4P_AMMO_X  %3d  /* ammo digits RIGHT edge (big red, right-justified, no %%) */\n#define HUD4P_AMMO_Y   %d\n"
                % (layout["AMMO_X"], NY))
        f.write("#define HUD4P_HEALTH_X %3d  /* health digits RIGHT edge; red '%%' at this x */\n#define HUD4P_HEALTH_Y  %d\n"
                % (layout["HEALTH_X"], NY))
        f.write("#define HUD4P_ARMOR_X %3d  /* armor digits RIGHT edge; red '%%' at this x (flush right) */\n#define HUD4P_ARMOR_Y  %d\n"
                % (layout["ARMOR_X"], NY))
        f.write("\n/* --- view overlay (keys / frags), relative to quadrant origin ox and band top oy --- */\n")
        f.write("#define HUD4P_KEY_H %d  /* overlay sits at oy - KEY_H (just above the band) */\n" % KEY_H)
        f.write("#define HUD4P_KEYS_X %3d  /* 1st key x-offset from ox (bottom-right of the view) */\n" % KEYS_OVL_X)
        f.write("#define HUD4P_KEYS_DX %d  /* upright STKEYS in a row (%dw + %dpx margin) */\n"
                % (KEY_DX, KEY_W, KEY_MARGIN))
        f.write("#define HUD4P_FRAGS_X %3d  /* deathmatch frag count RIGHT edge (x-offset from ox) */\n"
                % FRAGS_OVL_X)
        f.write("\n#endif /* HUD4P_LAYOUT_H */\n")
    print("wrote", path)

def emit_panel(path):
    with open(path, "w", newline="\n") as f:
        f.write("/* GENERATED by tools/make_hud4p.py -- DO NOT EDIT BY HAND.\n")
        f.write("   3/4-player compact HUD band background (160x16): STBAR brushed metal (bottom\n")
        f.write("   bevel only) with the engraved AM/HE/AR label strokes baked in. */\n")
        f.write("#ifndef HUD4P_PANEL_H\n#define HUD4P_PANEL_H\n")
        f.write('#include "hud4p_layout.h"\n\n')
        f.write("static const unsigned char hud4p_panel[HUD4P_W*HUD4P_H] = {\n")
        for y in range(H):
            f.write("  " + ",".join("%3d" % pan[y][x] for x in range(W)) + ",\n")
        f.write("};\n\n#endif /* HUD4P_PANEL_H */\n")
    print("wrote", path)

# ---------- preview ----------
OVL = 9
def blit(dst, patch, x, y):
    w, h, lo, to, img = patch
    for py in range(h):
        for px in range(w):
            v = img[py][px]
            if v is None: continue
            tx, ty = x - lo + px, y - to + py
            if 0 <= tx < W and 0 <= ty < len(dst): dst[ty][tx] = v

def num_l(dst, x, y, num, font):     # LEFT-justified; returns x after the last digit
    if num < 0: num = 0
    ds = []
    t = num
    while True:
        ds.append(t % 10); t //= 10
        if t == 0: break
    for d in reversed(ds):
        blit(dst, font[d], x, y); x += font[d][0]
    return x

def num_r(dst, x, y, num, font):     # RIGHT-justified (frags overlay)
    if num < 0: num = 0
    while True:
        d = num % 10; x -= font[d][0]
        blit(dst, font[d], x, y); num //= 10
        if num == 0: break

def draw(dst, ammo, health, armor, cards, frags, dm):
    # numbers RIGHT-justified (digits end at *_X); '%' at *_X for health/armor
    if ammo is not None:
        num_r(dst, layout["AMMO_X"], OVL+NY, ammo, DIGIT_HUF)
    blit(dst, PCT_HUF, layout["HEALTH_X"], OVL+NY); num_r(dst, layout["HEALTH_X"], OVL+NY, health, DIGIT_HUF)
    blit(dst, PCT_HUF, layout["ARMOR_X"],  OVL+NY); num_r(dst, layout["ARMOR_X"],  OVL+NY, armor,  DIGIT_HUF)
    oy = OVL
    if dm:
        num_r(dst, FRAGS_OVL_X, oy - KEY_H, frags, DIGIT_HUF)
    else:
        for i in range(3):
            if cards[i] is not None:
                blit(dst, KEYS[cards[i]], KEYS_OVL_X + i*KEY_DX, oy - KEY_H)

def emit_png(path, s=6):
    samples = [
        dict(ammo=50,  health=100, armor=0,   cards=[0, 5, None], frags=0,  dm=False),
        dict(ammo=200, health=8,   armor=100, cards=[None]*3,     frags=17, dm=True),
    ]
    PH_ROW = OVL + H
    rows = []
    for smp in samples:
        d = [[16]*W for _ in range(OVL)] + [row[:] for row in pan]
        draw(d, **smp)
        rows.append(d)
    gap = 4
    raw = bytearray()
    for ri, d in enumerate(rows):
        for y in range(PH_ROW):
            for _ in range(s):
                raw.append(0)
                for x in range(W):
                    r, g, b = PAL[d[y][x]]; raw.extend(bytes([r, g, b])*s)
        if ri != len(rows)-1:
            for _ in range(gap*s):
                raw.append(0); raw.extend(b"\x00\x00\x00"*(W*s))
    PH = PH_ROW*len(rows) + gap*(len(rows)-1)
    def chunk(t, q): return struct.pack(">I", len(q))+t+q+struct.pack(">I", zlib.crc32(t+q) & 0xffffffff)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W*s, PH*s, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png); print("wrote", path, "(%dx%d)" % (W*s, PH*s))

emit_layout("core/hud4p_layout.h")
emit_panel("src/hud4p_panel.h")
emit_png("tools/hud4p_preview.png", 6)
