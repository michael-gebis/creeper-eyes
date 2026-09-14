#!/usr/bin/env python3
"""Save and restore everything the board remembers.

    python tools/nvs_backup.py save    --port COM3 backup.json
    python tools/nvs_backup.py restore --port COM3 backup.json

This is the NVS partition: the eye design, the timezone, the clock colours,
the credentials -- and the WiFi network and password, which the radio keeps in
a namespace of its own (`nvs.net80211`) rather than anywhere this firmware can
see.  That last part is the reason the tool exists.  Anything that wipes NVS
takes the WiFi setup with it, and a head that has forgotten its network cannot
be reached over the network to put it back.

**USB only, and deliberately so.** Reading a flash partition means resetting
the chip into its bootloader, which drops the WiFi link, the web server and
the OTA endpoint.  There is no version of this that works over the air; the
port is a required argument rather than something that falls back to a guess.

The saved file is JSON, and carries the partition twice.  `entries` is the
decoded contents for reading -- what is actually in there, namespace by
namespace -- and `image_base64` is the raw partition, which is what a restore
writes back.

Restoring from the raw image rather than from the decoded entries is
deliberate.  NVS pages carry state bitmaps, sequence numbers and CRCs;
rebuilding them from parsed values would mean reimplementing the format
correctly enough to be trusted with the only copy of somebody's WiFi
password.  Writing back the bytes that were read cannot get that wrong.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x0C00
PARTITION_MAGIC = 0x50AA

# NVS on-flash layout, from the ESP-IDF documentation.
PAGE_SIZE = 4096
ENTRY_SIZE = 32
ENTRIES_PER_PAGE = 126
BITMAP_OFFSET = 32
ENTRY_OFFSET = 64

PAGE_ACTIVE = 0xFFFFFFFE
PAGE_FULL = 0xFFFFFFFC
PAGE_FREEING = 0xFFFFFFF8

ENTRY_WRITTEN = 0b10

TYPE_NAMES = {
    0x01: "u8", 0x11: "i8", 0x02: "u16", 0x12: "i16",
    0x04: "u32", 0x14: "i32", 0x08: "u64", 0x18: "i64",
    0x21: "str", 0x41: "blob", 0x42: "blob_data", 0x48: "blob_index",
}
PRIMITIVE_FORMATS = {
    0x01: "<B", 0x11: "<b", 0x02: "<H", 0x12: "<h",
    0x04: "<I", 0x14: "<i", 0x08: "<Q", 0x18: "<q",
}


# ------------------------------------------------------------------ esptool --

def esptool_path() -> str:
    p: str = os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py")
    if not os.path.exists(p):
        raise SystemExit("esptool.py not found; is PlatformIO installed?")
    return p


def python_path() -> str:
    p: str = os.path.expanduser("~/.platformio/penv/Scripts/python.exe")
    return p if os.path.exists(p) else sys.executable


def esptool(port: str, *args: str) -> str:
    cmd: list[str] = [python_path(), esptool_path(),
                      "--port", port, "--chip", "esp32"]
    cmd += list(args)
    r: subprocess.CompletedProcess[bytes] = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    out: str = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0:
        print(out)
        raise SystemExit("esptool failed: %s" % " ".join(args[:2]))
    return out


def read_region(port: str, offset: int, size: int) -> bytes:
    """Read a flash region into memory, via a temporary file."""
    fd: int
    path: str
    fd, path = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        esptool(port, "read_flash", hex(offset), hex(size), path)
        with open(path, "rb") as f:
            return f.read()
    finally:
        os.unlink(path)


# --------------------------------------------------------- partition table --

def find_partition(port: str, want: str = "nvs") -> dict[str, Any]:
    """Locate a partition by reading the table off the chip.

    The CSV in the repo says where it should be; the chip says where it is.
    Those can differ on a board flashed with another table, and writing to the
    wrong offset would be difficult to undo.
    """
    raw: bytes = read_region(port, PARTITION_TABLE_OFFSET,
                             PARTITION_TABLE_SIZE)
    for i in range(0, len(raw), 32):
        entry: bytes = raw[i:i + 32]
        if len(entry) < 32:
            break
        magic: int
        ptype: int
        subtype: int
        off: int
        size: int
        magic, ptype, subtype, off, size = struct.unpack("<HBBII", entry[:12])
        if magic != PARTITION_MAGIC:
            break
        label: str = entry[12:28].split(b"\0")[0].decode("ascii", "replace")
        if label == want:
            return {"label": label, "type": ptype, "subtype": subtype,
                    "offset": off, "size": size}
    raise SystemExit("no %r partition in the table on this chip" % want)


# ---------------------------------------------------------------- NVS parse --

def parse_nvs(image: bytes) -> dict[str, Any]:
    """Decode the partition for reading.

    Best effort, and labelled as such in the output: a restore uses the raw
    image, so a gap here costs nothing but legibility.
    """
    namespaces: dict[int, str] = {}
    raw_entries: list[dict[str, Any]] = []

    page_start: int
    for page_start in range(0, len(image), PAGE_SIZE):
        page: bytes = image[page_start:page_start + PAGE_SIZE]
        if len(page) < PAGE_SIZE:
            break
        state: int
        (state,) = struct.unpack("<I", page[0:4])
        if state not in (PAGE_ACTIVE, PAGE_FULL, PAGE_FREEING):
            continue
        bitmap: bytes = page[BITMAP_OFFSET:BITMAP_OFFSET + 32]

        i: int = 0
        while i < ENTRIES_PER_PAGE:
            status: int = (bitmap[i // 4] >> ((i % 4) * 2)) & 0b11
            if status != ENTRY_WRITTEN:
                i += 1
                continue
            base: int = ENTRY_OFFSET + i * ENTRY_SIZE
            entry: bytes = page[base:base + ENTRY_SIZE]
            ns: int
            etype: int
            span: int
            chunk: int
            ns, etype, span, chunk = struct.unpack("<BBBB", entry[0:4])
            key: str = entry[8:24].split(b"\0")[0].decode("utf-8", "replace")
            data: bytes = entry[24:32]

            value: Any = None
            if etype in PRIMITIVE_FORMATS:
                size: int = struct.calcsize(PRIMITIVE_FORMATS[etype])
                (value,) = struct.unpack(PRIMITIVE_FORMATS[etype], data[:size])
                if ns == 0 and etype == 0x01:
                    namespaces[int(value)] = key
            elif etype in (0x21, 0x41, 0x42):
                dlen: int
                (dlen,) = struct.unpack("<H", data[0:2])
                blob: bytes = page[base + ENTRY_SIZE:base + ENTRY_SIZE + dlen]
                if etype == 0x21:
                    value = blob.split(b"\0")[0].decode("utf-8", "replace")
                else:
                    value = {"bytes": dlen,
                             "base64": base64.b64encode(blob).decode("ascii")}
            elif etype == 0x48:
                total: int
                (total,) = struct.unpack("<I", data[0:4])
                value = {"blob_index": True, "total_bytes": total}

            raw_entries.append({"ns_index": ns, "key": key,
                                "type": TYPE_NAMES.get(etype, hex(etype)),
                                "chunk": chunk, "value": value})
            i += max(1, span)

    grouped: dict[str, dict[str, Any]] = {}
    e: dict[str, Any]
    for e in raw_entries:
        if e["ns_index"] == 0:
            continue  # the namespace table itself, not user data
        name: str = namespaces.get(e["ns_index"], "ns%d" % e["ns_index"])
        grouped.setdefault(name, {})[e["key"]] = {
            "type": e["type"], "value": e["value"]}
    return {"namespaces": namespaces, "entries": grouped}


# -------------------------------------------------------------------- save --

def do_save(port: str, path: str) -> int:
    print("reading the partition table from %s" % port)
    part: dict[str, Any] = find_partition(port, "nvs")
    print("  nvs at 0x%X, %d bytes" % (part["offset"], part["size"]))

    image: bytes = read_region(port, part["offset"], part["size"])
    digest: str = hashlib.sha256(image).hexdigest()

    decoded: dict[str, Any]
    note: Optional[str]
    try:
        decoded = parse_nvs(image)
        note = None
    except Exception as e:                       # noqa: BLE001
        decoded = {"namespaces": {}, "entries": {}}
        note = "could not be decoded (%s); the raw image is still exact" % e
        print("  note: %s" % note)

    doc: dict[str, Any] = {
        "format": 1,
        "what": "ESP32 NVS partition, saved by tools/nvs_backup.py",
        "saved_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "port": port,
        "partition": part,
        "sha256": digest,
        "restore_uses": "image_base64",
        "entries_are": "decoded for reading only; a restore ignores them",
        "decoded": decoded,
        "image_base64": base64.b64encode(image).decode("ascii"),
    }
    if note:
        doc["decode_note"] = note

    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=False)
        f.write("\n")

    print("saved %s (%d bytes of partition, sha256 %s...)"
          % (path, len(image), digest[:16]))
    ns: dict[str, Any] = decoded["entries"]
    if ns:
        print("namespaces found:")
        name: str
        for name in sorted(ns):
            keys: list[str] = sorted(ns[name])
            print("   %-16s %d keys: %s" % (name, len(keys),
                                            ", ".join(keys[:6])
                                            + (" ..." if len(keys) > 6 else "")))
    wifi: list[str] = [n for n in ns if "net80211" in n]
    print("wifi credentials present: %s"
          % ("yes, in " + wifi[0] if wifi else
             "NOT FOUND -- has this board ever joined a network?"))
    return 0


# ----------------------------------------------------------------- restore --

def do_restore(port: str, path: str, yes: bool) -> int:
    doc: dict[str, Any]
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)

    if doc.get("format") != 1:
        raise SystemExit("unfamiliar backup format: %r" % doc.get("format"))

    image: bytes = base64.b64decode(doc["image_base64"])
    if hashlib.sha256(image).hexdigest() != doc["sha256"]:
        raise SystemExit("the image in this file does not match its own "
                         "checksum; refusing to write it")

    saved: dict[str, Any] = doc["partition"]
    print("this backup: nvs at 0x%X, %d bytes, saved %s"
          % (saved["offset"], saved["size"], doc.get("saved_utc", "?")))

    # The chip's own table decides where it goes, and it has to agree.
    live: dict[str, Any] = find_partition(port, "nvs")
    if (live["offset"], live["size"]) != (saved["offset"], saved["size"]):
        raise SystemExit(
            "this board's nvs is at 0x%X/%d but the backup is 0x%X/%d -- "
            "different partition table, refusing to write"
            % (live["offset"], live["size"], saved["offset"], saved["size"]))
    if len(image) != live["size"]:
        raise SystemExit("image is %d bytes, partition is %d"
                         % (len(image), live["size"]))

    if not yes:
        print("\nThis overwrites everything the board remembers, including "
              "its WiFi network.")
        if input("type 'restore' to go ahead: ").strip() != "restore":
            print("nothing written")
            return 1

    fd: int
    tmp: str
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        with open(tmp, "wb") as f:
            f.write(image)
        esptool(port, "write_flash", hex(live["offset"]), tmp)
    finally:
        os.unlink(tmp)

    print("restored. The board reboots into it.")
    return 0


def main(argv: list[str]) -> int:
    ap: argparse.ArgumentParser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=("save", "restore"))
    ap.add_argument("file")
    # Required, with no default: guessing a port on a machine with several
    # serial devices could mean writing a partition to the wrong board.
    ap.add_argument("--port", required=True,
                    help="serial port, e.g. COM3 or /dev/ttyUSB0")
    ap.add_argument("--yes", action="store_true",
                    help="restore without the confirmation prompt")
    args: argparse.Namespace = ap.parse_args(argv[1:])

    if args.action == "save":
        return do_save(args.port, args.file)
    return do_restore(args.port, args.file, args.yes)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
