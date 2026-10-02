#!/usr/bin/env python3
"""Turn a compiled-in eye design into an eye file for the board's eye slot.

    uv run tools/make_eye.py dragon             # -> dist/eyes/dragon.bin
    uv run tools/make_eye.py dragon skull newt
    uv run tools/make_eye.py --all              # every design in include/
    uv run tools/make_eye.py --check dist/eyes/dragon.bin

The source is the same C headers the firmware compiles in, so an eye file
and a built-in design with the same name show the same thing.  The format is
described in docs/EYE_FILES.md; in short, a 64-byte header and the five
tables as raw little-endian bytes, in the order the renderer declares them.

Standard library only: no Pillow, no TeensyEyes checkout.  Those are for
regenerating the headers themselves (tools/gen_eyes.py), not for this.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys
from typing import Sequence

ROOT: str = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR: str = os.path.join(ROOT, "dist", "eyes")

MAGIC: bytes = b"CREEPEYE"
VERSION: int = 1
HEADER_BYTES: int = 64
NAME_MAX: int = 15

# The five tables in file order: (symbol prefix, bytes per value, count).
# Must match src/eyestore.h, which the firmware checks against the renderer.
TABLES: tuple[tuple[str, int, int], ...] = (
    ("sclera", 2, 200 * 200),
    ("iris", 2, 64 * 256),
    ("upper", 1, 128 * 128),
    ("lower", 1, 128 * 128),
    ("polar", 2, 80 * 80),
)
PAYLOAD_BYTES: int = sum(width * count for _, width, count in TABLES)

# One C array: `const uint16_t scleraDragon[SCLERA_HEIGHT][SCLERA_WIDTH] = {`
# then values to the closing `};`.  Non-greedy, so each array stops at its
# own terminator.
ARRAY: re.Pattern[str] = re.compile(
    r"const\s+uint(8|16)_t\s+(sclera|iris|upper|lower|polar)(\w+)\s*"
    r"(?:\[[^\]]*\]\s*)+=\s*\{(.*?)\}\s*;",
    re.DOTALL)

NAME_OK: re.Pattern[str] = re.compile(r"[a-z][a-z0-9]{0,%d}$" % (NAME_MAX - 1))


class EyeError(Exception):
    """Something wrong with a header or an eye file, said plainly."""


def headers() -> list[str]:
    """Every header that holds a design: include/eyes/*.h plus Adafruit's
    two at the top of include/."""
    inc: str = os.path.join(ROOT, "include")
    found: list[str] = [os.path.join(inc, f) for f in os.listdir(inc)
                        if f.endswith("Eye.h")]
    sub: str = os.path.join(inc, "eyes")
    found += [os.path.join(sub, f) for f in os.listdir(sub) if f.endswith(".h")]
    return sorted(found)


def design_name(path: str) -> str:
    """dragon.h -> dragon, bigBlue.h -> bigblue, defaultEye.h -> default.
    The same names the firmware's registry uses."""
    stem: str = os.path.splitext(os.path.basename(path))[0]
    if stem.endswith("Eye"):
        stem = stem[:-3]
    return stem.lower()


def find_header(name: str) -> str:
    """A design name, or a path to a header."""
    if os.path.isfile(name):
        return name
    for path in headers():
        if design_name(path) == name.lower():
            return path
    raise EyeError("no design called %r; there are %s" % (
        name, ", ".join(sorted(design_name(p) for p in headers()))))


def read_tables(path: str) -> dict[str, bytes]:
    """The five tables from one header, as little-endian bytes."""
    with open(path, encoding="utf-8") as f:
        text: str = f.read()
    got: dict[str, bytes] = {}
    for m in ARRAY.finditer(text):
        bits, table, body = m.group(1), m.group(2), m.group(4)
        width: int = int(bits) // 8
        values: list[int] = [int(v, 16) for v in body.replace("\n", " ")
                             .split(",") if v.strip()]
        fmt: str = "<%d%s" % (len(values), "H" if width == 2 else "B")
        try:
            got[table] = struct.pack(fmt, *values)
        except struct.error:
            raise EyeError("%s: a value in %s does not fit %s bits"
                           % (path, table, bits)) from None
    for table, width, count in TABLES:
        if table not in got:
            raise EyeError("%s: no %s table" % (path, table))
        if len(got[table]) != width * count:
            raise EyeError("%s: %s is %d bytes, expected %d"
                           % (path, table, len(got[table]), width * count))
    return got


def build(name: str, tables: dict[str, bytes]) -> bytes:
    """Header and payload, ready to upload."""
    if not NAME_OK.match(name):
        raise EyeError("%r cannot be an eye file's name: 1-%d of a-z and 0-9, "
                       "starting with a letter" % (name, NAME_MAX))
    payload: bytes = b"".join(tables[t] for t, _, _ in TABLES)
    header: bytes = struct.pack("<8sHHI16s32s", MAGIC, VERSION, HEADER_BYTES,
                                len(payload), name.encode("ascii"),
                                hashlib.sha256(payload).digest())
    assert len(header) == HEADER_BYTES
    return header + payload


def check(data: bytes) -> str:
    """The same checks the board makes, short of the name clashing with one
    of its built-in designs, which only the board knows.  Returns the name."""
    if len(data) < HEADER_BYTES or data[:8] != MAGIC:
        raise EyeError("not an eye file")
    _, version, hbytes, pbytes, raw_name, digest = struct.unpack(
        "<8sHHI16s32s", data[:HEADER_BYTES])
    if version != VERSION:
        raise EyeError("format version %d; this tool writes %d"
                       % (version, VERSION))
    if hbytes != HEADER_BYTES or pbytes != PAYLOAD_BYTES:
        raise EyeError("header declares the wrong sizes")
    if len(data) != HEADER_BYTES + PAYLOAD_BYTES:
        raise EyeError("%d bytes; an eye file is %d"
                       % (len(data), HEADER_BYTES + PAYLOAD_BYTES))
    name: str = raw_name.rstrip(b"\0").decode("ascii", "replace")
    if not NAME_OK.match(name) or b"\0" in raw_name.rstrip(b"\0"):
        raise EyeError("bad name %r" % name)
    if hashlib.sha256(data[HEADER_BYTES:]).digest() != digest:
        raise EyeError("damaged: the payload does not match its checksum")
    return name


def make(source: str, out_dir: str) -> str:
    """Build one eye file and return where it went."""
    path: str = find_header(source)
    name: str = design_name(path)
    data: bytes = build(name, read_tables(path))
    os.makedirs(out_dir, exist_ok=True)
    out: str = os.path.join(out_dir, name + ".bin")
    with open(out, "wb") as f:
        f.write(data)
    return out


def main(argv: Sequence[str]) -> int:
    ap: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Build eye files for the board's eye slot "
                    "(docs/EYE_FILES.md).")
    ap.add_argument("designs", nargs="*",
                    help="design names, e.g. dragon, or paths to headers")
    ap.add_argument("--all", action="store_true",
                    help="every design in include/")
    ap.add_argument("--out", default=OUT_DIR,
                    help="where to write them (default: dist/eyes/)")
    ap.add_argument("--check", metavar="FILE",
                    help="verify an eye file instead of building one")
    args: argparse.Namespace = ap.parse_args(argv[1:])

    try:
        if args.check:
            with open(args.check, "rb") as f:
                print("ok: %s" % check(f.read()))
            return 0
        sources: list[str] = headers() if args.all else list(args.designs)
        if not sources:
            ap.error("name a design, or use --all")
        for src in sources:
            out: str = make(src, args.out)
            print("  %-12s -> %s" % (design_name(find_header(src)),
                                     os.path.relpath(out)))
    except (EyeError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
