#!/usr/bin/env python3
"""Generate the project IP.BIN (Saturn boot header) from the SRL template.

Copies SaturnRingLib's prebuilt IP.BIN (security code, area codes, boot
parameters all kept intact) and rewrites only the ASCII identity fields of
the SEGA disc header:

    0x10  Maker ID        (16 bytes)
    0x20  Product Number  (10 bytes)   <- SAROO's per-game config key
    0x2A  Version         ( 6 bytes, "Vx.yyy")
    0x30  Release Date    ( 8 bytes, YYYYMMDD -- stamped with the build date)
    0x60  Game Title      (112 bytes)

The version minor digits come from the project build number:

    --build-num git                -> git rev-list --count HEAD
    --build-num define:FILE:MACRO  -> parse '#define MACRO <n>' out of FILE
    --build-num 42                 -> literal value

Invoked by pre.makefile (SRL pre_build hook); the Makefile points SRL_IPBIN
at the generated file so xorrisofs boots the disc with it.

Sibling copy: Mimas/tools/make_ip.py == Tethys/tools/make_ip.py (keep in sync).

SAROO note: the per-game section key in SD:/SAROO/saroocfg.txt is the Product
Number (optionally + version).  Keep the product stable across builds; the
version moves with every build, so use the ID-only section form:  [Mimas-Eng]
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HW_ID = b"SEGA SEGASATURN "
HEADER_MIN = 0x100


def put_field(data: bytearray, off: int, size: int, text: str, name: str) -> None:
    raw = text.encode("ascii")
    if len(raw) > size:
        sys.exit(f"make_ip: {name} '{text}' is {len(raw)} bytes, max {size}")
    data[off:off + size] = raw.ljust(size)


def resolve_build_num(spec: str) -> int:
    if spec == "git":
        out = subprocess.check_output(
            ["git", "rev-list", "--count", "HEAD"], text=True)
        return int(out.strip())
    m = re.fullmatch(r"define:(.+):(\w+)", spec)
    if m:
        path, macro = m.group(1), m.group(2)
        src = Path(path).read_text(errors="replace")
        dm = re.search(rf"#define\s+{macro}\s+(\d+)", src)
        if not dm:
            sys.exit(f"make_ip: '#define {macro} <n>' not found in {path}")
        return int(dm.group(1))
    return int(spec)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--template", required=True,
                    help="source IP.BIN (SRL's modules/sgl/IP.BIN)")
    ap.add_argument("--out", required=True, help="destination IP.BIN")
    ap.add_argument("--maker", help="Maker ID, <=16 chars")
    ap.add_argument("--product", help="Product Number, <=10 chars")
    ap.add_argument("--title", help="Game Title, <=112 chars")
    ap.add_argument("--build-num",
                    help="'git', 'define:FILE:MACRO', or a literal integer; "
                         "written as version V0.NNN (NNN = value mod 1000)")
    ap.add_argument("--version",
                    help="explicit 6-char version field (overrides --build-num)")
    ap.add_argument("--date", default=time.strftime("%Y%m%d"),
                    help="release date YYYYMMDD (default: today); "
                         "'keep' leaves the template value")
    args = ap.parse_args()

    data = bytearray(Path(args.template).read_bytes())
    if len(data) < HEADER_MIN or data[0:16] != HW_ID:
        sys.exit(f"make_ip: {args.template} is not a Saturn IP.BIN "
                 f"(missing '{HW_ID.decode()}' header)")

    if args.maker:
        put_field(data, 0x10, 16, args.maker, "maker")
    if args.product:
        put_field(data, 0x20, 10, args.product, "product")

    version = args.version
    if version is None and args.build_num is not None:
        n = resolve_build_num(args.build_num)
        if n > 999:
            print(f"make_ip: build number {n} > 999, "
                  f"version wraps to {n % 1000:03d}", file=sys.stderr)
        version = f"V0.{n % 1000:03d}"
    if version is not None:
        put_field(data, 0x2A, 6, version, "version")

    if args.date != "keep":
        if not re.fullmatch(r"\d{8}", args.date):
            sys.exit(f"make_ip: date '{args.date}' is not YYYYMMDD")
        put_field(data, 0x30, 8, args.date, "date")

    if args.title:
        put_field(data, 0x60, 112, args.title, "title")

    Path(args.out).write_bytes(data)

    def show(off, size):
        return data[off:off + size].decode("ascii").rstrip()

    print(f"make_ip: {args.out}  "
          f"maker='{show(0x10, 16)}'  product='{show(0x20, 10)}'  "
          f"version='{show(0x2A, 6)}'  date='{show(0x30, 8)}'  "
          f"title='{show(0x60, 112)}'")


if __name__ == "__main__":
    main()
