#!/usr/bin/env bash
# Build devlog #1 in TWO parts.  Plan: docs/DEVLOG_EP1_SCRIPT.md
#   part 1 "THREE FRAMES PER SECOND"  20 pieces, 8:12
#   part 2 "ONE WORD IN THE MANUAL"   28 pieces, 9:06
#
#   bash tools/devlog/build_ep1.sh            # everything
#   bash tools/devlog/build_ep1.sh cards      # both card sets
#   bash tools/devlog/build_ep1.sh pieces     # gameplay + medallions + comparison
#   bash tools/devlog/build_ep1.sh med        # the three medallion pieces only
#   bash tools/devlog/build_ep1.sh c1         # the comparison block only
#   bash tools/devlog/build_ep1.sh master     # concat + burn + verify, both parts
#
# Pieces are intermediates at crf 16 / veryfast; the master pass re-encodes at
# crf 18 / medium anyway.
set -euo pipefail

SRC_DL="/c/Users/pcico/Downloads"
SRC_VID="/c/Users/pcico/Videos"
OUT="/c/Users/pcico/Videos/mimas-devlog1"
HERE="$(cd "$(dirname "$0")" && pwd)"

# ---- sources.  OWNER CONSTRAINTS, do not silently widen: -------------------
#   G5 (185508): the first 20 s are OFF LIMITS.
#   G6 (185618): never past 10 s.
#   MP: the game starts at 8.6 s; before that it is the skill-select menu.
J="$SRC_VID/2026-07-20 11-26-13.mp4"                  # the BEFORE, 1080p OBS
M11A="$SRC_DL/DOOM-TNT-latest.mp4"                    # TNT MAP11, 16/08
M11C="$SRC_DL/Doom M11 video.mp4"                     # TNT MAP11, 18/08
O1="$SRC_DL/GenkiArcade-20260820-182004.mp4"          # E1M1 first room, full overlay
O2="$SRC_DL/GenkiArcade-20260820-182100.mp4"          # E1M1 acid + outdoors, overlay
O3="$SRC_DL/GenkiArcade-20260820-182209.mp4"          # E1M1 outdoors then closed
G1="$SRC_DL/GenkiArcade-20260820-184951.mp4"
G2="$SRC_DL/GenkiArcade-20260820-185046.mp4"
G3="$SRC_DL/GenkiArcade-20260820-185147.mp4"
G4="$SRC_DL/GenkiArcade-20260820-185408.mp4"
G5="$SRC_DL/GenkiArcade-20260820-185508.mp4"
G6="$SRC_DL/GenkiArcade-20260820-185618.mp4"
MP="$SRC_DL/Doom - 2 Player - nodebug.mp4"

mkdir -p "$OUT/cards"

V_INT=(-c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p -r 60)
A_INT=(-c:a aac -b:a 192k -ar 48000 -ac 2)

# The capture chain is QUIET: the first master measured I -38.0 LUFS / TP -19.5.
# +22 dB then a limiter at 0.85 lands I -15.5 / TP -1.1.  Measure, never guess.

# 720x480 with SAR 32:27 -> DAR 16:9, so a straight scale to 1920x1080 is right.
# neighbor, never lanczos: this is a 320x224 picture, we want its pixels.
FV_AUG="fps=60,scale=1920:1080:flags=neighbor,setsar=1"
# The July OBS capture pillarboxes its picture at 1716x1008+88+38 (cropdetect).
FV_JUL="fps=60,crop=1716:1008:88:38,scale=1920:1080:flags=neighbor,setsar=1"

# ---------------------------------------------------------------- gameplay ---
clip () {   # $1 out  $2 src  $3 ss  $4 dur  $5 video filter
  local out=$1 src=$2 ss=$3 dur=$4 fv=$5
  local fo; fo=$(python -c "print(max(0.0,$dur-0.25))")
  ffmpeg -v error -stats -ss "$ss" -t "$dur" -i "$src" \
    -vf "$fv" \
    -af "afade=t=in:st=0:d=0.25,afade=t=out:st=$fo:d=0.25,aresample=48000" \
    "${V_INT[@]}" "${A_INT[@]}" -shortest -y "$OUT/$out"
}

card () {   # $1 out  $2 png  $3 dur
  ffmpeg -v error -stats -loop 1 -i "$2" -f lavfi -i anullsrc=r=48000:cl=stereo \
    -t "$3" -vf "scale=1920:1080,setsar=1" \
    "${V_INT[@]}" "${A_INT[@]}" -y "$OUT/$1"
}

do_cards () {
  echo "== backdrop =="
  # A CLEAN frame, checked BY EYE (DEVLOG_VIDEO.md gotchas 5 and 10).  TNT MAP01
  # at 47 s: hardware sky, textured walls, nothing broken.  Cropped top AND
  # bottom so neither the fps row nor the bright status bar fights the type.
  ffmpeg -v error -ss 47.0 -i "$G5" \
    -vf "crop=720:372:0:28,scale=1920:1080:flags=neighbor" -frames:v 1 -y "$OUT/backdrop.png"
  echo "== cards =="
  ( cd "$HERE" && python ep1_cards.py "$OUT/cards" --backdrop "$OUT/backdrop.png" )
  # part 1
  card a02.mp4 "$OUT/cards/A_T1.png"        10
  card a04.mp4 "$OUT/cards/A_WORST.png"     18
  card a06.mp4 "$OUT/cards/A_READ.png"      14
  card a10.mp4 "$OUT/cards/A_CANNOT.png"    20
  card a11.mp4 "$OUT/cards/A_METHOD.png"    16
  card a14.mp4 "$OUT/cards/A_NUM.png"       18
  card a15.mp4 "$OUT/cards/A_FLOOR.png"     14
  card a17.mp4 "$OUT/cards/A_STEP.png"      15
  card a19.mp4 "$OUT/cards/A_NEXT.png"      16
  card a20.mp4 "$OUT/cards/A_CRED.png"      16
  # part 2
  card b02.mp4 "$OUT/cards/B_T2.png"        10
  card b03.mp4 "$OUT/cards/B_RECAP.png"     19
  card b04.mp4 "$OUT/cards/B_WAD.png"       20
  card b06.mp4 "$OUT/cards/B_CORPUS.png"    20
  card b08.mp4 "$OUT/cards/B_STRUCTS.png"   20
  card b10.mp4 "$OUT/cards/B_STAYOUT.png"   20
  card b11.mp4 "$OUT/cards/B_SEAM.png"      16
  card b15.mp4 "$OUT/cards/B_WRONGTIME.png" 20
  card b17.mp4 "$OUT/cards/B_FAFLING1.png"  20
  card b19.mp4 "$OUT/cards/B_READINGS.png"  24
  card b21.mp4 "$OUT/cards/B_SEQUENCE.png"  22
  card b23.mp4 "$OUT/cards/B_OPENS.png"     20
  card b24.mp4 "$OUT/cards/B_CONCRETE.png"  22
  card b26.mp4 "$OUT/cards/B_BROKEN.png"    20
  card b27.mp4 "$OUT/cards/B_MAKEAWAD.png"  16
  card b28.mp4 "$OUT/cards/B_CRED.png"      18
}

# --------------------------------------------------------------- medallions ---
# The full debug overlay is unreadable at 1080p, so each explained field is cut
# out of the SOURCE and magnified over the frame, with a box left around the row
# it came from.  Geometry, measured 2026-08-21 on GenkiArcade-20260820-182004:
# the 320x224 picture fills the 720x480 capture, so a text cell is 18 px wide and
# 17.14 px tall.  Row n (1-based) starts at y = round(17.143*(n-1)).
#   row 1  fps/MST   y=0      row 3  Bw/Bp/P/M  y=34
#   row 6  SLV       y=86     row 9  VD1        y=137
#   row 24 THK       y=388
# Row 25 (TIC) lands ON the status bar at y=411 and cannot be magnified legibly;
# THK carries the same story and is clear.
# Row 1 needs TWENTY cells, not eighteen: "12.4fps a13.4 MST277" is exactly 20,
# and an 18-cell crop shipped "MST9" for MST97 in the first cut.
# Scale-to-frame: x *= 1920/720 = 2.6667, y *= 1080/480 = 2.25.
#
# $1 out $2 src $3 ss $4 dur
#   $5..$10  medallion A: cx cy cw  mw mh  "t0,t1"
#   $11..$14 box A on the frame: bx by bw bh   (bw = 0 -> no box)
#   $15..$24 the same for medallion B, or "-" to skip
med_clip () {
  local out=$1 src=$2 ss=$3 dur=$4
  local acx=$5 acy=$6 acw=$7 amw=$8 amh=$9 awin=${10}
  local abx=${11} aby=${12} abw=${13} abh=${14}
  local b=${15}
  local fo; fo=$(python -c "print(max(0.0,$dur-0.25))")

  local amx=$(( (1920 - amw) / 2 ))
  local abox_x=$(( amx - 14 )) abox_w=$(( amw + 28 )) abox_h=$(( amh + 28 ))
  local fg="[0:v]fps=60,scale=1920:1080:flags=neighbor,setsar=1"
  if [ "$abw" != "0" ]; then
    fg="$fg,drawbox=x=$abx:y=$aby:w=$abw:h=$abh:color=0xC86A4F@1:t=4:enable='between(t,$awin)'"
  fi
  fg="$fg,drawbox=x=$abox_x:y=136:w=$abox_w:h=$abox_h:color=0x0B0906@0.92:t=fill:enable='between(t,$awin)'"
  fg="$fg,drawbox=x=$abox_x:y=136:w=$abox_w:h=$abox_h:color=0xC86A4F@1:t=3:enable='between(t,$awin)'"

  if [ "$b" != "-" ]; then
    local bcx=${15} bcy=${16} bcw=${17} bmw=${18} bmh=${19} bwin=${20}
    local bbx=${21} bby=${22} bbw=${23} bbh=${24}
    local bmx=$(( (1920 - bmw) / 2 ))
    local bbox_x=$(( bmx - 14 )) bbox_w=$(( bmw + 28 )) bbox_h=$(( bmh + 28 ))
    if [ "$bbw" != "0" ]; then
      fg="$fg,drawbox=x=$bbx:y=$bby:w=$bbw:h=$bbh:color=0xC86A4F@1:t=4:enable='between(t,$bwin)'"
    fi
    fg="$fg,drawbox=x=$bbox_x:y=136:w=$bbox_w:h=$bbox_h:color=0x0B0906@0.92:t=fill:enable='between(t,$bwin)'"
    fg="$fg,drawbox=x=$bbox_x:y=136:w=$bbox_w:h=$bbox_h:color=0xC86A4F@1:t=3:enable='between(t,$bwin)'"
    fg="$fg[base];\
[1:v]fps=60,crop=$acw:19:$acx:$acy,scale=$amw:$amh:flags=neighbor,setsar=1[ma];\
[2:v]fps=60,crop=$bcw:19:$bcx:$bcy,scale=$bmw:$bmh:flags=neighbor,setsar=1[mb];\
[base][ma]overlay=$amx:150:enable='between(t,$awin)'[x];\
[x][mb]overlay=$bmx:150:enable='between(t,$bwin)'[v]"
    ffmpeg -v error -stats -ss "$ss" -t "$dur" -i "$src" \
                          -ss "$ss" -t "$dur" -i "$src" \
                          -ss "$ss" -t "$dur" -i "$src" \
      -filter_complex "$fg" -map "[v]" -map 0:a \
      -af "afade=t=in:st=0:d=0.25,afade=t=out:st=$fo:d=0.25,aresample=48000" \
      "${V_INT[@]}" "${A_INT[@]}" -shortest -y "$OUT/$out"
  else
    fg="$fg[base];\
[1:v]fps=60,crop=$acw:19:$acx:$acy,scale=$amw:$amh:flags=neighbor,setsar=1[ma];\
[base][ma]overlay=$amx:150:enable='between(t,$awin)'[v]"
    ffmpeg -v error -stats -ss "$ss" -t "$dur" -i "$src" \
                          -ss "$ss" -t "$dur" -i "$src" \
      -filter_complex "$fg" -map "[v]" -map 0:a \
      -af "afade=t=in:st=0:d=0.25,afade=t=out:st=$fo:d=0.25,aresample=48000" \
      "${V_INT[@]}" "${A_INT[@]}" -shortest -y "$OUT/$out"
  fi
}

do_med () {
  echo "== medallions =="
  #        out  src  ss  dur |  cx cy  cw   mw   mh  window   | bx by   bw   bh
  med_clip a07.mp4 "$O1"  0.0 40.0    0   0 360 1440  76 "3.0,21.0"    0   0  960  43 \
                                      0  34 432 1296  57 "24.0,38.0"   0  77 1152  43
  med_clip a08.mp4 "$O2"  0.0 36.0    0  86 252 1260  95 "3.0,20.0"    0 194  672  43 \
                                      0 137 504 1512  57 "23.0,35.0"   0 308 1344  43
  med_clip a09.mp4 "$O3"  0.0 24.0    0 388 576 1728  57 "2.0,22.0"    0   0    0   0 -
}

# ------------------------------------------------- the comparison block C1 ---
# BEFORE: 2026-07-20, top right, the wall lags the ceiling and the sky shows
# through.  AFTER: 2026-08-20, same room, same junction, clean.  Both normalised
# to 1920x1080 FIRST, then the SAME 712x400 window is cut from both, so the
# magnification is identical -- a mismatched zoom would lie.
# 1.5 s a side at 8x: past 21.9 s the August run faces an unlit wall, and past
# 28.2 s the July run takes damage (the red palette flash filled the last third
# of the first cut).  What is left is the window where BOTH sides show the same
# lit junction.  The artefact lasts one field, so holding each game frame is the
# whole point of the block.
# The captions say DESYNC and nothing else (owner): what the slow motion shows is
# VDP1 and the CPU displaying different frames, not a wall in the wrong place.
do_c1 () {
  echo "== C1 comparison =="
  ffmpeg -v error -stats \
    -ss 26.65 -t 1.50 -i "$J" \
    -ss 20.30 -t 1.50 -i "$G2" \
    -f lavfi -t 12.0 -i "color=c=0x0B0906:s=1920x1080" \
    -f lavfi -t 12.0 -i anullsrc=r=48000:cl=stereo \
    -filter_complex "\
[0:v]fps=60,crop=1716:1008:88:38,scale=1920:1080:flags=neighbor,crop=712:400:1096:80,\
setpts=8*PTS,scale=944:531:flags=neighbor,setsar=1,trim=0:12,setpts=PTS-STARTPTS[l];\
[1:v]fps=60,scale=1920:1080:flags=neighbor,crop=712:400:1096:80,\
setpts=8*PTS,scale=944:531:flags=neighbor,setsar=1,trim=0:12,setpts=PTS-STARTPTS[r];\
[2:v][l]overlay=8:170[x];[x][r]overlay=968:170,\
drawbox=x=7:y=169:w=946:h=533:color=0x4A4030@1:t=2,\
drawbox=x=967:y=169:w=946:h=533:color=0x4A4030@1:t=2,\
drawtext=fontfile='C\\:/Windows/Fonts/consolab.ttf':\
text='THE SAME JUNCTION, ONE EIGHTH SPEED':\
x=40:y=62:fontsize=52:fontcolor=0xDA9427,\
drawtext=fontfile='C\\:/Windows/Fonts/consola.ttf':\
text='the wall is drawn by VDP1. the ceiling around it is drawn by the CPU.':\
x=40:y=128:fontsize=32:fontcolor=0xA39B8C,\
drawtext=fontfile='C\\:/Windows/Fonts/consolab.ttf':text='20 JUL - DESYNC':\
x=40:y=716:fontsize=36:fontcolor=0xC86A4F,\
drawtext=fontfile='C\\:/Windows/Fonts/consolab.ttf':text='20 AUG - manual present':\
x=1000:y=716:fontsize=36:fontcolor=0x5EAE88,\
drawtext=fontfile='C\\:/Windows/Fonts/consola.ttf':\
text='for one field the two are showing DIFFERENT FRAMES - the wall is LATE, not misplaced':\
x=40:y=790:fontsize=30:fontcolor=0xA39B8C,\
drawtext=fontfile='C\\:/Windows/Fonts/consola.ttf':\
text='different capture chains - 1080p OBS left, 480p capture card right':\
x=40:y=838:fontsize=26:fontcolor=0xA39B8C[v]" \
    -map "[v]" -map 3:a "${V_INT[@]}" "${A_INT[@]}" -shortest -y "$OUT/b14.mp4"
}

do_pieces () {
  echo "== part 1 =="
  clip a01.mp4 "$G5"   20.0  8.0 "$FV_AUG"
  clip a03.mp4 "$G5"   28.0 34.0 "$FV_AUG"
  clip a05.mp4 "$M11A" 12.0 48.0 "$FV_AUG"
  do_med
  clip a12.mp4 "$M11C" 10.0 40.0 "$FV_AUG"
  clip a13.mp4 "$M11C" 55.0 30.0 "$FV_AUG"
  clip a16.mp4 "$G4"    9.0 42.0 "$FV_AUG"
  clip a18.mp4 "$G1"   21.0 33.0 "$FV_AUG"
  echo "== part 2 =="
  clip b01.mp4 "$G5"   62.0  7.0 "$FV_AUG"
  clip b05.mp4 "$G3"    0.0 26.0 "$FV_AUG"
  clip b07.mp4 "$G3"   26.0 22.0 "$FV_AUG"
  clip b09.mp4 "$G6"    0.0 10.0 "$FV_AUG"
  clip b12.mp4 "$J"    26.0 13.0 "$FV_JUL"
  clip b13.mp4 "$J"    39.0 26.0 "$FV_JUL"
  do_c1
  clip b16.mp4 "$G2"    0.0 30.0 "$FV_AUG"
  clip b18.mp4 "$G2"   30.0 22.0 "$FV_AUG"
  clip b20.mp4 "$G3"   48.0 12.0 "$FV_AUG"
  # NOT G1 @0: that capture opens on the title and skill-select screens and the
  # overlay is still up until ~21 s -- part 1 already takes its clean 21-54.
  # MAP11 in a firefight, with MP1 and w0 live in the overlay while the text
  # says the seam is closed, is a better shot for this beat anyway.
  clip b22.mp4 "$M11C" 86.0 21.0 "$FV_AUG"
  clip b25.mp4 "$MP"    8.6 42.0 "$FV_AUG"
}

# ------------------------------------------------------------------ master ---
# $1 = part letter, $2 = piece count, $3 = output name, $4 = pre-limiter gain dB
# The gain is PER PART, measured not guessed: part 2 carries a higher ratio of
# silent cards, so the same +22 dB landed it at -17.4 LUFS against part 1's
# -14.4.  Two videos published together have to match on INTEGRATED loudness --
# that is what a viewer hears when they open the second one.
one_master () {
  local p=$1 n=$2 name=$3 gain=${4:-22}
  echo "== $name: measure =="
  : > "$OUT/$p-list.txt"
  for i in $(seq -w 1 "$n"); do
    f="$OUT/$p$i.mp4"
    [ -f "$f" ] || { echo "MISSING $f"; exit 1; }
    echo "file '$(echo "$f" | sed 's|^/c/|C:/|')'" >> "$OUT/$p-list.txt"
  done
  ( cd "$HERE" && python ep1_annot.py "$OUT" "$p" > "$OUT/$p-annot.ass" )

  echo "== $name: concat =="
  ffmpeg -v error -stats -f concat -safe 0 -i "$OUT/$p-list.txt" -c copy \
    -y "$OUT/$p-picture-cut.mp4"

  echo "== $name: burn + normalise =="
  # The subtitles filter cannot be handed a Windows drive letter: the colon ends
  # the option and the rest of the path lands in `original_size`.  Run from the
  # directory and pass a bare filename -- DEVLOG_VIDEO.md section 6.
  cd "$OUT"
  ffmpeg -v error -stats -i "$p-picture-cut.mp4" \
    -vf "subtitles=$p-annot.ass" \
    -r 60 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p \
    -profile:v high -level 4.1 \
    -af "volume=${gain}dB,aresample=192000,alimiter=limit=0.85:attack=4:release=60:level=disabled,aresample=48000" \
    -c:a aac -b:a 192k -ar 48000 -ac 2 -movflags +faststart -y "$name"

  echo "== $name: verify =="
  ffprobe -v error -show_entries format=duration,size -of default=nw=1 "$name"
  ffmpeg -v error -nostats -i "$name" -af ebur128=peak=true -f null - 2>&1 \
    | tr '\r' '\n' | grep -E "^ +(I|Peak|LRA): " | head -4
}

do_master_a () { one_master a 20 MIMAS-devlog1-part1.mp4 22; }
do_master_b () { one_master b 28 MIMAS-devlog1-part2.mp4 25; }
do_master () { do_master_a; do_master_b; }

case "${1:-all}" in
  cards)  do_cards ;;
  med)    do_med ;;
  c1)     do_c1 ;;
  pieces) do_pieces ;;
  master) do_master ;;
  master1) do_master_a ;;
  master2) do_master_b ;;
  all)    do_cards; do_pieces; do_master ;;
  *) echo "usage: $0 [cards|pieces|med|c1|master|all]"; exit 2 ;;
esac
echo "done -> $OUT"
