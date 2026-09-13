#!/usr/bin/env python3
"""Check the boards and produce what the fab wants.

    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/make_outputs.py [pitch ...]

For each variant gen_board.py knows (or the ones named), runs, in order, and
stops at the first one that fails:

  1. ERC on the schematic
  2. the schematic's netlist against the board's, pad by pad
  3. DRC on the board, including schematic parity
  4. Gerbers and drill files into the variant's gerbers/, zipped with the
     variant's name so the upload says which DevKit it fits
  5. renders of both sides and the schematic as a PDF, into its images/

The netlist comparison is the one that matters most.  gen_board.py writes the
schematic and the board from the same table, so they should agree, but "should"
is not a test and a board that does not match its own schematic is the classic
way to order five copies of a mistake.

Same Python as gen_board.py -- it needs pcbnew to read the board's nets.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_board import VARIANTS, variant_dir, variant_name  # noqa: E402

# Set per variant by main(); everything below reads them.
NAME = SCH = PCB = OUT = IMG = ""

CLI = os.path.join(os.path.dirname(sys.executable), "kicad-cli.exe")
if not os.path.exists(CLI):
    CLI = shutil.which("kicad-cli") or ""
if not CLI:
    raise SystemExit("kicad-cli not found; run this with KiCad's own python.exe")


def run(*args: str) -> str:
    r = subprocess.run([CLI, *args], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit(f"kicad-cli {' '.join(args[:2])} failed")
    return r.stdout + r.stderr


def report_counts(path: str, pattern: str) -> int:
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.findall(pattern, text)
    return sum(int(x) for x in m)


def erc() -> None:
    rep = os.path.join(OUT, "erc.txt")
    run("sch", "erc", "--severity-all", "--output", rep, SCH)
    errors = report_counts(rep, r"Errors (\d+)")
    warnings = report_counts(rep, r"Warnings (\d+)")
    print(f"ERC: {errors} errors, {warnings} warnings")
    if errors or warnings:
        with open(rep, encoding="utf-8") as f:
            print(f.read())
        raise SystemExit("ERC is not clean")


def parity() -> None:
    import pcbnew

    xml = os.path.join(OUT, "netlist.xml")
    run("sch", "export", "netlist", "--format", "kicadxml", "--output", xml, SCH)
    sch: dict[tuple[str, str], str] = {}
    for net in ET.parse(xml).getroot().find("nets"):
        for node in net.findall("node"):
            sch[(node.get("ref"), node.get("pin"))] = net.get("name")
    board = pcbnew.LoadBoard(PCB)
    pcb: dict[tuple[str, str], str] = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname():
                pcb[(fp.GetReference(), pad.GetNumber())] = pad.GetNetname()
    bad = [(k, sch.get(k), pcb.get(k)) for k in sorted(set(sch) | set(pcb))
           if sch.get(k) != pcb.get(k)]
    print(f"netlist: {len(sch)} schematic nodes, {len(pcb)} board nodes, "
          f"{len(bad)} mismatches")
    for k, a, b in bad:
        print(f"  {k}: schematic {a!r}, board {b!r}")
    if bad:
        raise SystemExit("schematic and board disagree")
    os.remove(xml)


def drc() -> None:
    rep = os.path.join(OUT, "drc.txt")
    run("pcb", "drc", "--severity-all", "--schematic-parity", "--output", rep, PCB)
    with open(rep, encoding="utf-8") as f:
        text = f.read()
    counts = {k: int(v) for v, k in re.findall(r"\*\* Found (\d+) ([a-zA-Z ]+?) \*\*", text)}
    print("DRC:", ", ".join(f"{v} {k}" for k, v in counts.items()))
    if any(counts.values()):
        print(text)
        raise SystemExit("DRC is not clean")


def gerbers() -> None:
    for f in os.listdir(OUT):
        if f.endswith((".gbr", ".drl", ".gtl", ".gbl", ".gts", ".gbs", ".gto", ".gbo",
                       ".gm1", ".gko", ".gtp", ".gbp", ".zip")) or f.startswith(NAME):
            os.remove(os.path.join(OUT, f))
    # The layer set JLCPCB and PCBWay ask for.  Protel extensions (the
    # default) because that is what their upload parsers are keyed on.
    run("pcb", "export", "gerbers", "--output", OUT + os.sep,
        "--layers", "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts",
        "--subtract-soldermask", "--no-x2", PCB)
    run("pcb", "export", "drill", "--output", OUT + os.sep,
        "--format", "excellon", "--excellon-units", "mm",
        "--excellon-zeros-format", "decimal", "--generate-map",
        "--map-format", "gerberx2", PCB)
    zpath = os.path.join(OUT, NAME + "-gerbers.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(OUT)):
            if f.startswith(NAME) and not f.endswith(".zip"):
                z.write(os.path.join(OUT, f), f)
    print(f"gerbers: {os.path.relpath(zpath, HERE)}")


def images() -> None:
    os.makedirs(IMG, exist_ok=True)
    for side in ("top", "bottom"):
        run("pcb", "render", "--output", os.path.join(IMG, f"board-{side}.png"),
            "--side", side, "--width", "1600", "--height", "1200", "--zoom", "1.1",
            "--quality", "basic", PCB)
    run("sch", "export", "pdf", "--output", os.path.join(IMG, "schematic.pdf"), SCH)
    print(f"images: {os.path.relpath(IMG, HERE)}/")


def main() -> None:
    global NAME, SCH, PCB, OUT, IMG
    wanted = sys.argv[1:] or list(VARIANTS)
    for pitch in wanted:
        if pitch not in VARIANTS:
            raise SystemExit(f"no variant for {pitch}; add it to VARIANTS in gen_board.py")
        NAME = variant_name(pitch)
        d = variant_dir(pitch)
        SCH = os.path.join(d, NAME + ".kicad_sch")
        PCB = os.path.join(d, NAME + ".kicad_pcb")
        OUT = os.path.join(d, "gerbers")
        IMG = os.path.join(d, "images")
        print(f"== {NAME}")
        os.makedirs(OUT, exist_ok=True)
        erc()
        parity()
        drc()
        gerbers()
        images()


if __name__ == "__main__":
    main()
