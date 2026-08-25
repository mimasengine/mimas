"""Emit annot.ass for devlog #1, part 1 or part 2, from MEASURED piece durations.

    python ep1_annot.py <piece_dir> a > a-annot.ass
    python ep1_annot.py <piece_dir> b > b-annot.ass

Nominal durations drift on encode (DEVLOG_VIDEO.md gotcha 4), so every start is
computed from ffprobe, never from the plan.  Text lives here rather than in a
shell heredoc (gotcha 1).

STYLE RULE: no pronouns.  The mechanism is the subject.

PACING: 16-22 s a screen, 3-5 lines, >=4 s of blank between two.  And the screens
must read as a PARAGRAPH: symptom, then what it is, then what was tried and why it
was wrong, then the corrective and its price, then the cause, then the fix.

VERIFIED CLAIMS -- the two that cost a round each:
  * the swap unit is the FIELD.  "1/60 second" is the MANUAL'S OWN name for
    1-cycle mode (ST-013, FBCR table: "0 0 0  1-cycle mode  Change every 1/60
    second"; p.38 "60 frames/sec  1-cycle mode").  Not a rounding of ours.
  * CEF = Current End bit Fetch status, EDSR bit 1 at 100010H.  The manual's own
    caveat is quoted on the card, not paraphrased.
  * NO claim that emulator and console disagreed about the swap: the version that
    still tore on the emulator was never run on hardware.  What IS said is that no
    register reports the displayed framebuffer, so the swap cannot be observed.

ASS colours are &H00BBGGRR -- BLUE first.  Twins of devlog_style.py:
    AMBER (218,148,47)  -> &H002F94DA      GREEN ( 94,174,136) -> &H0088AE5E
    BONE  (237,228,211) -> &H00D3E4ED      RUST  (200,106, 79) -> &H004F6AC8

MarginV is 176, not the template's 58: Doom keeps a 32-line status bar at the
bottom of a 224-line picture = the bottom 154 px of a 1080p master.

The medallion windows in build_ep1.sh (a07/a08/a09) MUST match I01..I05 here.
"""
import subprocess, sys, os

A = r"{\c&H002F94DA&\b1\fs40}"   # heading
B = r"{\c&H00D3E4ED&\b0\fs32}"   # body
G = r"{\c&H0088AE5E&\b0\fs32}"   # a win
R = r"{\c&H004F6AC8&\b0\fs32}"   # a problem, a debt

HEAD = """[Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Ann,Consolas,34,&H00D3E4ED,&H00D3E4ED,&H00000000,&HA5000000,0,0,0,0,100,100,0,0,3,6,0,1,80,80,176,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""

# ============================================================== PART ONE ===
# 20 pieces, 492 s.  Cards carry their own text and get nothing here.
P1_N = 20
P1 = {
    "a03": [
        (3.0, 18.0, [A + "REAL HARDWARE, NO EMULATOR",
                     B + "Sega Saturn, 1994. Two SH-2 at 28 MHz, 2 MB of work RAM.",
                     B + "The walls are VDP1 quads. The sky is a VDP2 scroll layer.",
                     B + "The largest floor plane is a VDP2 rotation layer - every other",
                     B + "floor, and every ceiling, is still drawn by the CPU."]),
        (25.0, 8.0, [A + "ONE MONTH AGO",
                     R + "the hardest map that would boot ran at 3 frames per second"]),
    ],
    "a05": [
        # The capture is 16 August, i.e. AFTER the fix -- no footage of the 3.6 fps
        # state exists.  Say so on screen rather than let the overlay contradict
        # the text: the fps row is visible in the same frame.
        (3.0, 19.0, [A + "WHERE THIS STARTED",
                     B + "14 August, this map: one frame took 277 ms - 3.6 fps.",
                     B + "a 60 Hz frame gets 16.7, and ONE call inside it cost 46.6 -",
                     R + "a single texture column, worth nearly three whole frames.",
                     G + "this capture is two days later, after the fix."]),
        (26.0, 19.0, [A + "EVERY ROW ON SCREEN IS A TIMER",
                      B + "not decoration - each field is a bracketed measurement,",
                      B + "latched to the same frame the frame time came from.",
                      G + "this is the instrument the whole month was read through."]),
    ],
    # --- the medallions.  Windows must match do_med() in build_ep1.sh ---------
    "a07": [
        (3.0, 18.0, [A + "THE FRAME",
                     B + "MST is the master SH-2 frame time, in milliseconds.",
                     B + "every other measurement has to add up to it.",
                     # NOT "this one took 98": the value drifts across the 18 s
                     # window and the magnified row is on screen to contradict it.
                     R + "a 60 Hz frame gets 16.7 ms. nothing here is close."]),
        (24.0, 14.0, [A + "WHERE THE FRAME WENT",
                      B + "Bw - walking the BSP tree and projecting sprites",
                      B + "Bp - preparing walls",
                      B + "P - floor and ceiling planes, and the VDP1 command emission",
                      B + "M - sprites"]),
    ],
    "a08": [
        (3.0, 17.0, [A + "THE SECOND CPU",
                     B + "b is how much of the frame the slave SH-2 was actually busy.",
                     B + "14 % here. in split-screen it reads 0 -",
                     R + "a whole 28 MHz processor watching the frame go past."]),
        (23.0, 12.0, [A + "THE DRAWING CHIP",
                      B + "which framebuffer pair is live, whether the manual present",
                      B + "is on, how long the CPU waited for VDP1 to finish."]),
    ],
    "a09": [
        (2.0, 20.0, [A + "AND THE GAME ITSELF",
                     B + "doors, lifts, monsters, collision - the simulation,",
                     B + "and how many thinkers ran this frame.",
                     B + "it runs at 35 Hz whatever the frame rate, so it is a fixed tax.",
                     R + "for a long time nobody here had measured it."]),
    ],
    "a12": [
        (3.0, 20.0, [A + "FIVE WRONG ANSWERS, EACH KILLED BY ITS OWN TIMER",
                     B + "the composite builder - 2 ms measured against a 42 ms hole",
                     B + "the zone walk - 563 blocks, 0.59 ms of 183",
                     B + "the disc - zero CD reads on that frame",
                     B + "Z_Malloc - under 1 ms"]),
        (26.0, 13.0, [A + "AND A SIXTH THAT LOOKED CERTAIN",
                      B + "a stray printf inside the wall loop, 46 ms a call. it fit exactly.",
                      R + "a counter proved the branch never ran."]),
    ],
    "a13": [
        (2.0, 19.0, [A + "THE TELL, READ PAST TWICE",
                     B + "46680 / 46738 / 46613 / 46769 / 46648 microseconds -",
                     B + "five frames, five rooms, the same cost to 0.3 %.",
                     B + "work whose input varies does not reproduce like that.",
                     G + "it was a 35 KB texture patch, decompressed from scratch."]),
    ],
    "a16": [
        (4.0, 19.0, [A + "THE GAME LOGIC OVERTOOK THE RENDERER",
                     B + "one frame in a firefight: 165 ms of game, 141 ms of rendering.",
                     B + "the thinkers - doors, lifts, every monster -",
                     R + "cost 21 to 49 % of the whole machine."]),
        (26.0, 15.0, [A + "AND IT IS NOT A SPIRAL",
                      B + "the simulation runs at 35 Hz whatever the frame rate:",
                      B + "a fixed tax, not a runaway.",
                      R + "the biggest single term is collision - up to 75 ms."]),
    ],
    "a18": [
        (3.0, 17.0, [A + "THE GOVERNOR",
                     B + "every frame: measure the render total, elect the phase that",
                     B + "dominates it, and degrade only the knob that owns that phase.",
                     R + "the first version watched one phase alone, and had one knob -",
                     R + "so it could not have chosen even if it had looked."]),
        (22.0, 10.0, [A + "AND WHEN IT CANNOT HELP, IT SAYS SO",
                      B + "the BSP walk has no quality knob at all.",
                      B + "electing it would degrade something innocent, so nothing moves."]),
    ],
}

# ============================================================== PART TWO ===
# 28 pieces, 546 s.
P2_N = 28
P2 = {
    "b05": [
        (3.0, 20.0, [A + "CAPACITY IS NOT THE WALL",
                     R + "38 maps did not run slowly. they refused to LOAD.",
                     B + "the bytes were there - just not in ONE unbroken run.",
                     B + "a level lump is one allocation, and the biggest one",
                     B + "against the wall was about 110 KB."]),
    ],
    "b07": [
        (3.0, 19.0, [A + "SO THE MAPS GOT SMALLER, NOT THE WADS",
                     B + "no re-authoring, no Saturn level format,",
                     B + "not one extra byte read from the disc -",
                     G + "the same WAD, read straight into structures half the size",
                     G + "the PC version uses."]),
    ],
    "b09": [
        (1.0, 8.5, [A + "A 23 MB WAD, STREAMED FROM THE DISC,",
                    A + "ON A 2 MB MACHINE"]),
    ],
    # --- the seam, in order: symptom, what it is, what was tried, the price ---
    "b12": [
        (1.0, 11.0, [A + "THE SYMPTOM",
                     B + "top right - the wall does not reach the ceiling it meets,",
                     R + "and the sky shows through the gap"]),
    ],
    "b13": [
        (2.0, 22.0, [A + "TWO ENGINES, ONE PICTURE",
                     B + "the room - floors, ceilings, sprites, the gun - is drawn by",
                     B + "the CPU into one image. the walls are drawn by VDP1,",
                     B + "a separate chip, into another. the two are shown stacked.",
                     R + "if they are not from the SAME game frame, the join opens."]),
    ],
    "b16": [
        (3.0, 17.0, [A + "THEN THE SWAP ITSELF, FIVE TIMES",
                     B + "draw-gated, slSynch, NBG1-coupled, coherent-pair, field-lock.",
                     R + "each one closed the seam somewhere",
                     R + "and reopened it somewhere else a week later."]),
        (23.0, 9.0, [A + "AND ONE FIX THAT WORKED, EXPENSIVELY",
                     B + '"lead-fill": the CPU painted the missing band itself, on both',
                     R + "SH-2, every frame. the seam went away - for 1.5 to 5 fps."]),
    ],
    "b18": [
        (2.0, 20.0, [A + "WHAT THAT MEANS",
                     B + "VDP1 draws into a buffer, and that buffer is swapped to screen.",
                     B + "left in its default mode the Saturn swaps it at the end of",
                     B + "EVERY FIELD - the manual's own name for that mode is",
                     B + '"change every 1/60 second".',
                     R + "this port runs at 7-16 fps: one game frame lasts 4 to 8 fields."]),
    ],
    "b20": [
        (1.0, 10.5, [A + "AND THE SWAP CANNOT BE WATCHED EITHER",
                     B + "no register reports which framebuffer is on screen.",
                     B + "a first attempt pulsed the swap just after the vertical blank -",
                     R + "mechanically correct, and the holes still came back."]),
    ],
    "b22": [
        (2.0, 18.0, [A + "WHAT IT CLOSED",
                     B + "the seam, and the CPU repaint that had been hiding it.",
                     B + "+1.5 to +5 fps measured on the emulator -",
                     B + "the console side-by-side has not been run yet.",
                     G + "but the frame rate is not what this was for."]),
    ],
    "b25": [
        (4.0, 18.0, [A + "TWO PLAYERS, ONE SATURN",
                     B + "two viewpoints, one framebuffer, both pads, one disc"]),
        (26.0, 15.0, [A + "AND THIS IS WHERE IT MATTERS MOST",
                      B + "split-screen draws two views into a single frame.",
                      G + "a plot window the size of one screen frame",
                      G + "was never going to hold both."]),
    ],
}


def dur(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return float(out)


def ts(t):
    h = int(t // 3600); m = int(t // 60) % 60; s = t - 60 * int(t // 60)
    return "%d:%02d:%05.2f" % (h, m, s)


def main(d, part):
    ann, n = (P1, P1_N) if part == "a" else (P2, P2_N)
    sys.stdout.write(HEAD)
    t = 0.0
    plan = []
    for i in range(1, n + 1):
        name = "%s%02d" % (part, i)
        length = dur(os.path.join(d, name + ".mp4"))
        for off, hold, lines in ann.get(name, []):
            if off + hold > length + 0.01:
                sys.stderr.write("WARN %s: %.2f+%.2f > %.2f\n" % (name, off, hold, length))
            plan.append((t + off, t + min(off + hold, length), lines))
        sys.stderr.write("%-4s %8.3f s   starts %s\n" % (name, length, ts(t)))
        t += length
    sys.stderr.write("TOTAL %.3f s = %s\n" % (t, ts(t)))
    for start, end, lines in plan:
        sys.stdout.write("Dialogue: 0,%s,%s,Ann,,0,0,0,,%s\n"
                         % (ts(start), ts(end), r"\N".join(lines)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
