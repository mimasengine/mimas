#!/usr/bin/env bash
# Process the episode-1 voice-over takes into per-line stems.
#
#   bash tools/devlog/ep1_vo.sh            # process both batches + check strips
#
# The owner records one line per take on a modest mic, in order, deleting the
# bad ones -- so the surviving file NUMBERS are not contiguous but their ORDER
# is the script order.  Measured 2026-08-21: peaks -20 to -23 dBFS (no clipping
# anywhere), takes within 2.5 dB of each other, and the lead/tail silence is
# true digital zero, so a peak-threshold trim is safe.
#
# TWO batches, and they are NOT interchangeable:
#   l01..l19   2026-08-21, the first pass -- symptom, corrections, budget
#   n01..n24   2026-08-24, the THIRD pass (takes 56-78).  The second pass was
#              re-recorded because the owner judged the delivery monotone, and
#              the script was rewritten with it: a two-line hook up front ("can
#              it run it?"), the subject named out loud at the end of block 2
#              instead of withheld, and the distinct-picture count dropped from
#              the voice -- that number stays on the C3 card, where it can be
#              read rather than heard.
#
# One take can hold two lines.  ep1_vo_lines.tsv says so with a range on the
# take number (56/0-4.38), and the reason is written next to it.
#
# Chain, in order, and why each link is there:
#   pan        the takes are dual mono; summing avoids the comb filtering that
#              a stereo-to-mono downmix would introduce later in the video mix
#   highpass   85 Hz -- handling noise and room rumble, nothing musical is there
#   deesser    a close mic at moderate quality is always sibilant
#   acompressor  3:1 at -24 dB: read-aloud lines drift 6-8 dB take to take
#   silenceremove  head and tail, so the edit can place lines to the frame
#   loudnorm   -18 LUFS: the VO STEM level.  The music bed sits under it and the
#              game capture is ducked below that; the final mix lands at -14.
set -euo pipefail

SRC="/c/Users/pcico/Documents/Enregistrements audio"
OUT="/c/Users/pcico/Videos/mimas-devlog1/vo"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"

# batch 1: script line -> take number.  Verified 2026-08-21: 19 surviving takes
# for 19 lines, and every take's duration matches its line's length.
TAKES=(13 14 15 16 17 19 20 21 22 23 24 25 26 29 30 31 32 33 34)

# batch 2 lives in ep1_vo_lines.tsv, next to this script: stem, take, block,
# text.  Keeping the text beside the take number is what lets the duration
# check below be a check and not a guess.

CHAIN="pan=mono|c0=0.5*c0+0.5*c1,\
highpass=f=85,\
deesser=i=0.4:m=0.5:f=0.35,\
acompressor=threshold=-24dB:ratio=3:attack=5:release=120:makeup=2,\
silenceremove=start_periods=1:start_silence=0.08:start_threshold=-45dB:detection=peak,\
areverse,\
silenceremove=start_periods=1:start_silence=0.08:start_threshold=-45dB:detection=peak,\
areverse,\
loudnorm=I=-18:TP=-2:LRA=7"

grind () {  # $1 take number, optionally N/start-end, $2 output stem
  local take="${1%%/*}" span="" ss=() to=()
  if [ "$1" != "$take" ]; then
    span="${1#*/}"
    [ -n "${span%%-*}" ] && ss=(-ss "${span%%-*}")
    [ -n "${span#*-}" ] && to=(-to "${span#*-}")
  fi
  ffmpeg -v error "${ss[@]}" "${to[@]}" -i "$SRC/Enregistrement ($take).m4a" \
    -af "$CHAIN" -ar 48000 -ac 1 -c:a pcm_s16le -y "$OUT/$2.wav"
}

strip () {  # $1 list basename, $2 output basename, then the stems
  local list="$OUT/$1.txt" name="$2" out="$OUT/$2.wav"; shift 2
  ffmpeg -v error -f lavfi -i anullsrc=r=48000:cl=mono -t 0.6 \
    -c:a pcm_s16le -y "$OUT/_gap.wav"
  : > "$list"
  for s in "$@"; do
    echo "file '$(echo "$OUT/$s.wav" | sed 's|^/c/|C:/|')'" >> "$list"
    echo "file '$(echo "$OUT/_gap.wav" | sed 's|^/c/|C:/|')'" >> "$list"
  done
  ffmpeg -v error -f concat -safe 0 -i "$list" -ar 48000 -ac 1 -y "$out"
  printf "%-14s %6.2f s   %d lines\n" "$name" \
    "$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$out")" "$#"
}

echo "== batch 1 (2026-08-21) =="
i=0
for t in "${TAKES[@]}"; do
  i=$((i+1)); n=$(printf "l%02d" "$i")
  grind "$t" "$n"
  printf "%s  take %-2s  %6.2f s\n" "$n" "$t" \
    "$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$OUT/$n.wav")"
done

echo
echo "== batch 2 (2026-08-24) =="
printf "%-4s %-5s %-4s %5s %5s %6s  %s\n" stem take blk words secs s/word text
STEMS2=()
while IFS=$'\t' read -r stem take blk text; do
  case "$stem" in ''|'#'*) continue;; esac
  grind "$take" "$stem"
  STEMS2+=("$stem")
  d=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$OUT/$stem.wav")
  w=$(echo "$text" | wc -w)
  printf "%-4s %-5s %-4s %5d %5.2f %6.3f  %.42s\n" \
    "$stem" "$take" "$blk" "$w" "$d" "$(awk -v d=$d -v w=$w 'BEGIN{print d/w}')" "$text"
done < "$HERE/ep1_vo_lines.tsv"

echo
echo "== check strips =="
strip strip  VO-check  $(seq -f 'l%02g' 1 19)
strip strip2 VO-check2 "${STEMS2[@]}"
echo "done -> $OUT"
