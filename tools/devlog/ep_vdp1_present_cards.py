"""Cards for devlog #1, "the VDP1 manual present".

    python ep_vdp1_present_cards.py out/ [--backdrop title_frame.png]

Renders every static card of the cut in docs/DEVLOG_EP_VDP1_PRESENT.md, then hold
each PNG for its duration (the durations are in CARDS below):

    ffmpeg -loop 1 -i out/B1_two_clocks.png -t 16 -r 60 -c:v libx264 \
           -preset medium -crf 18 -pix_fmt yuv420p -y b1.mp4

STYLE RULE, load-bearing: NO PRONOUNS in published copy - not "we", not "I".
The mechanism is the subject.  Keep it that way when editing a line.

The backdrop MUST come from a clean frame -- check it for the artefact the
episode is about, or the card ships the bug it is talking about (DEVLOG_VIDEO.md
gotcha 5).  Suggested: GenkiArcade-20260820-185147.mp4 at ~0:45.
"""
import argparse, os
from mkcard import title_card, table_card, list_card, credits_card
from devlog_style import BONE, AMBER, RUST, GREEN, GREY

# (filename, seconds, used in the 8:19 cut?) -- keep in sync with the doc, sec. 9
CARDS = [
    ("A0_title",       10, True),
    ("A2_project",     16, True),
    ("B1_two_clocks",  16, True),
    ("C2_erratum",     20, True),
    ("C3_numbers",     14, False),  # long cut only
    ("E1_deleted",     16, True),
    ("F0_wad_asks",    20, True),
    ("F0b_boot_wall",  18, True),
    ("G1_opens",       16, True),
    ("H1_broken",      18, True),
    ("H2_make_a_wad",  16, True),
    ("H3_credits",     12, True),
]


def build(backdrop=None):
    out = {}

    out["A0_title"] = title_card(
        "MIMAS",
        "one word in SEGA's manual, and a two-month bug",
        "Doom on the Sega Saturn - captured on real hardware",
        backdrop=backdrop)

    # First episode: nobody knows what this is yet.  Four facts, no history.
    out["A2_project"] = list_card("WHAT THIS IS", [
        ("a Doom port that renders with the Saturn hardware", BONE),
        ("", BONE),
        ("walls are VDP1 quads", GREEN),
        ("the sky is a VDP2 scroll layer", GREEN),
        ("the floor is a VDP2 rotation plane", GREEN),
        ("both SH-2 CPUs run the renderer", GREEN),
        ("", BONE),
        ("it eats a normal Doom WAD, streamed from the disc", GREY),
        ("no RAM cart, no Saturn-specific level format", GREY),
    ])

    # The mechanism, before any register is named.
    out["B1_two_clocks"] = list_card("TWO CLOCKS, ONE SCREEN", [
        ("VDP1 swapped its framebuffer every vblank, 60 Hz", BONE),
        ("the software picture committed once per game frame", BONE),
        ("a frame is not a whole number of fields", BONE),
        ("", BONE),
        ("so the two drifted, and a few times a second", RUST),
        ("the screen showed a new floor under an old wall", RUST),
    ])

    # The payload.  Both quotes verbatim; the consequence underneath.
    out["C2_erratum"] = list_card("ONE WORD IN THE MANUAL", [
        ("ST-013 p.39, developer CD:", GREY),
        ('  "writing 0 to the VBE, FCM and FCT registers"', BONE),
        ("", BONE),
        ("ST-013 p.39, Kronos-corrected:", GREY),
        ('  "writing 0 to the VBE and FCT registers', GREEN),
        ('   and 1 to the FCM register"', GREEN),
        ("", BONE),
        ("FBCR = 0x0000 is 1-cycle mode - the manual's own", RUST),
        ("table says so.  For two months the driver did nothing.", RUST),
    ])

    out["C3_numbers"] = table_card(
        "THE PRESENT, A/B", "same scene, same build, emulator only",
        "1-CYCLE + LEAD-FILL", "MANUAL PRESENT (VBE)",
        [("frame time", "50-55 ms", "41-50 ms"),
         ("forced swaps", "n/a", "0"),
         ("fence wait", "n/a", "13-15 ms"),
         ("holes while turning", "yes", "none")],
        big=("18.4-19.4 fps", "20.0-24.2 fps"),
        footer="[Ymir] - console re-validation is NOT done. Console is 3-5x slower.")

    out["E1_deleted"] = list_card("DELETED", [
        ("the lead-fill - the hole was repainted by the CPU,", GREEN),
        ("  on both SH-2, every frame, in motion", GREEN),
        ("the 1-cycle present, and the A/B toggle around it", GREEN),
        ("the field-lock, and its call site", GREEN),
        ("two pad chords", GREEN),
        ("", BONE),
        ("a corrective that works takes the pressure off the cause", GREY),
    ])

    # The second wall of the window: memory.  Two cards, because the
    # counter-intuitive half (contiguity) needs room to land.
    out["F0_wad_asks"] = list_card("WHAT A WAD ASKS FOR", [
        ("the engine's own source asks for a 6 MiB zone heap", BONE),
        ("a Saturn has 2 MB of work RAM, in two banks:", BONE),
        ("  WRAM-H, 1 MB, the only bank SCU-DMA can read", GREY),
        ("  WRAM-L, 1 MB, measured 2.1x slower per access", GREY),
        ("the Doom zone gets 1016 KB of the slow one", BONE),
        ("", BONE),
        ("and capacity is not the wall - CONTIGUITY is:", RUST),
        ("the largest single allocation wants ~110 KB unbroken,", RUST),
        ("so a map can fit in the free bytes and still refuse", RUST),
    ])

    out["F0b_boot_wall"] = list_card("38 MAPS OVER THE WALL, THEN 0", [
        ("measured across the 415 maps of the test corpus", GREY),
        ("", BONE),
        ("seg_t   32 -> 14 bytes     still 38 blocked", RUST),
        ("node_t  52 -> 28 bytes     still 38 blocked", RUST),
        ("line_t  64 -> 24 bytes            0 blocked", GREEN),
        ("", BONE),
        ("on every blocked map the biggest allocation was", GREY),
        ("LINEDEFS.  Name the dominant term before shrinking.", GREY),
        ("", BONE),
        ("Scythe MAP30 boots now, and is unplayable.", RUST),
    ])

    out["G1_opens"] = list_card("WHAT IT OPENS", [
        ("the plot window is now the size of a whole frame:", BONE),
        ("16 -> 32 sprites per frame", GREEN),
        ("decoration and actor floors lowered 2x", GREEN),
        ("", BONE),
        ('"VDP1 will be able to draw a lot more than just what', GREY),
        (' fits in a screen frame"  - fafling, 3 August', GREY),
    ])

    out["H1_broken"] = list_card("STILL BROKEN, NAMED", [
        ("the fence timing is validated on the emulator, not console", RUST),
        ("the game tic now costs more than the renderer:", RUST),
        ("  165 ms of logic against 141 ms of rendering  [hardware]", RUST),
        ("VDP1 floors: eight rounds, still refused", RUST),
        ("the split-screen present was never A/B'd", RUST),
        ("no DEHACKED, no PWAD merge, and a read-only disc", RUST),
        ("  means no saves at all", RUST),
    ])

    # The ambition, said once, at the end, where it is earned.
    out["H2_make_a_wad"] = list_card("MAKE A WAD, GET A SATURN FPS", [
        ("the IWAD is identified by scanning lump contents,", BONE),
        ("not by filename - one binary, any Doom WAD", BONE),
        ("no Saturn level format, no Saturn toolchain:", BONE),
        ("author on PC, run on real hardware", GREEN),
        ("", BONE),
        ("Heretic: same map format, no scripting VM", GREEN),
        ("Strife: same, plus dialogue and quests", GREEN),
        ("Hexen: ACS bytecode, polyobjects, hub saves", RUST),
    ])

    out["H3_credits"] = credits_card("WITH HELP FROM", [
        ("fafling", ["named the cause on SegaXtreme, 3 August",
                     "Kronos-corrected VDP1 / VDP2 manuals"]),
        ("TrekkiesUnite118", ["corrected the history of the retail port"]),
        ("ReyeMe", ["Saturn Ring Library"]),
        ("slygamer", ["hardware capture and playtesting"]),
    ])

    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--backdrop", help="clean gameplay frame for the title cards")
    ap.add_argument("--all", action="store_true", help="also render the long-cut cards")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    cards = build(a.backdrop)
    for name, secs, used in CARDS:
        if not used and not a.all:
            continue
        path = os.path.join(a.outdir, name + ".png")
        cards[name].save(path)
        print("%-16s %2d s  %s" % (name, secs, path))
