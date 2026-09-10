"""Convert TeensyEyes eye artwork into creeper-eyes header format.

TeensyEyes stores sclera and iris polar-unwrapped (width = angle, height =
radial position) and renders them with a per-eye radius.  creeper-eyes wants
a Cartesian 200x200 sclera that a 128x128 window pans across, plus a
256x64 polar iris strip, 128x128 eyelid threshold maps, and a packed
angle/distance table.

Also renders preview PNGs using the same arithmetic the firmware uses, so the
result can be checked without flashing.
"""

# Annotations are evaluated lazily, so the built-in generic syntax below
# (list[int], Image.Image | None) works on the 3.9 that ships with some
# PlatformIO installs as well as on newer interpreters.
from __future__ import annotations

import json
import math
import os
import sys
from typing import Any, Sequence, TextIO

from PIL import Image

# Pillow's load() hands back a PixelAccess object whose class has moved
# between releases and is not re-exported anywhere stable.  It behaves as a
# mutable mapping from an (x, y) tuple to a pixel, which is all this file
# needs, so it is aliased rather than imported.
PixelMap = Any

# These are evaluated at import time, not lazily like the annotations, so
# they need the built-in generic syntax that arrived in 3.9 -- which is the
# oldest interpreter this has to run on.
RGB = tuple[int, int, int]          # an 8-bit-per-channel pixel
Config = dict[str, Any]             # one design's parsed config.eye
PolarTable = list[list[int]]        # IRIS x IRIS, packed (angle<<7)|distance

SCLERA: int = 200          # SCLERA_WIDTH / SCLERA_HEIGHT
IRIS_MAP_W: int = 256
IRIS_MAP_H: int = 64
SCREEN: int = 128          # SCREEN_WIDTH / SCREEN_HEIGHT
IRIS: int = 80             # IRIS_WIDTH / IRIS_HEIGHT
IRIS_R: float = IRIS / 2.0  # 40


def rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-bit RGB into RGB565, keeping the top 5, 6 and 5 bits."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def from565(v: int) -> RGB:
    """RGB565 back to 8-bit RGB, replicating high bits into the low ones so
    white stays white rather than drifting to 248,252,248."""
    r: int = (v >> 11) & 0x1F
    g: int = (v >> 5) & 0x3F
    b: int = v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def parse_color(c: int | str | None, default: int = 0) -> int:
    """config.eye colours are either an int or a string like "0xFFE0"."""
    if c is None:
        return default
    if isinstance(c, int):
        return c
    return int(str(c), 0)


def load_cfg(d: str) -> Config:
    """Read one eye design's config.eye."""
    with open(os.path.join(d, "config.eye")) as f:
        c: Config = json.load(f)
    return c


def strip(path: str) -> Image.Image:
    """Load a polar-unwrapped texture as RGB."""
    return Image.open(path).convert("RGB")


def build_sclera(d: str, cfg: Config) -> Image.Image:
    """Polar strip -> Cartesian 200x200, black hole where the iris sits."""
    eye_r_src: float = float(cfg.get("radius", 125))
    iris_r_src: float = float(cfg.get("iris", {}).get("radius", 90))
    # Keep the design's own iris-to-eyeball proportion.
    eye_r: float = IRIS_R * (eye_r_src / max(iris_r_src, 1.0))

    back: int = parse_color(cfg.get("backColor"), 0)
    scl: Config = cfg.get("sclera", {})
    src: Image.Image | None = None
    for name in ("sclera.png",):
        p: str = os.path.join(d, name)
        if os.path.exists(p):
            src = strip(p)
    flat: int = parse_color(scl.get("color"), back)

    out: Image.Image = Image.new("RGB", (SCLERA, SCLERA))
    px: PixelMap = out.load()
    if src is not None:
        sp: PixelMap = src.load()
        sw: int
        sh: int
        sw, sh = src.size
    cx: float = SCLERA / 2.0
    cy: float = SCLERA / 2.0

    for y in range(SCLERA):
        dy: float = y - cy + 0.5
        for x in range(SCLERA):
            dx: float = x - cx + 0.5
            r: float = math.hypot(dx, dy)
            if r < IRIS_R:
                px[x, y] = (0, 0, 0)  # iris/pupil hole; the renderer draws over it
                continue
            if src is None:
                px[x, y] = from565(flat)
                continue
            row: int
            if r <= eye_r:
                # row 0 is the outer edge, last row sits against the iris
                t: float = (r - IRIS_R) / max(eye_r - IRIS_R, 1e-6)
                row = int((1.0 - t) * (sh - 1))
            else:
                row = 0  # extend the outermost ring out to the corners
            ang: float = math.atan2(dy, dx) + math.pi   # 0..2pi
            col: int = int(ang / (2 * math.pi) * sw) % sw
            px[x, y] = sp[col, max(0, min(sh - 1, row))]
    return out


def build_iris(d: str, cfg: Config) -> Image.Image:
    """Polar strip -> 256x64.  Row 0 = outer edge, row 63 = pupil edge."""
    for name in ("iris.png", "spiral.png"):
        p: str = os.path.join(d, name)
        if os.path.exists(p):
            return strip(p).resize((IRIS_MAP_W, IRIS_MAP_H), Image.LANCZOS)
    flat: int = parse_color(cfg.get("iris", {}).get("color"),
                            parse_color(cfg.get("backColor"), 0))
    return Image.new("RGB", (IRIS_MAP_W, IRIS_MAP_H), from565(flat))


def build_lid(d: str, which: str) -> Image.Image:
    """Eyelid threshold map -> 128x128 greyscale.

    TeensyEyes ships a binary mask of the eye aperture; this format needs a
    threshold map, where the value is the blink position at which a pixel
    becomes covered (0 = always covered, 254 = only when fully shut).  The
    lid sweeps as a vertical ramp starting along its resting curve -- which
    is the aperture edge -- and finishing at a roughly constant row, which is
    how Adafruit's own maps are built.
    """
    src: Image.Image | None = None
    for name in ("%s.png" % which, "%s-symmetrical.png" % which):
        p: str = os.path.join(d, name)
        if os.path.exists(p):
            src = Image.open(p).convert("L").resize((SCREEN, SCREEN), Image.LANCZOS)
            break
    if src is None:
        return Image.new("L", (SCREEN, SCREEN), 255)  # no lid art: always open

    sp: PixelMap = src.load()
    open_px: list[list[bool]] = [[sp[x, y] >= 128 for y in range(SCREEN)]
                                 for x in range(SCREEN)]
    rows: list[int] = [y for x in range(SCREEN) for y in range(SCREEN)
                       if open_px[x][y]]
    if not rows:
        return Image.new("L", (SCREEN, SCREEN), 255)

    out: Image.Image = Image.new("L", (SCREEN, SCREEN))
    op: PixelMap = out.load()
    if which == "upper":
        y_end: int = max(rows)
        for x in range(SCREEN):
            col: list[int] = [y for y in range(SCREEN) if open_px[x][y]]
            y_top: int = min(col) if col else y_end
            span: int = max(y_end - y_top, 1)
            for y in range(SCREEN):
                op[x, y] = 0 if y <= y_top else min(255, 255 * (y - y_top) // span)
    else:
        y_start: int = min(rows)
        for x in range(SCREEN):
            col = [y for y in range(SCREEN) if open_px[x][y]]
            y_bot: int = max(col) if col else y_start
            span = max(y_bot - y_start, 1)
            for y in range(SCREEN):
                op[x, y] = 0 if y >= y_bot else min(255, 255 * (y_bot - y) // span)
    return out


def build_polar(cfg: Config) -> PolarTable:
    """Packed (angle<<7)|distance, matching Adafruit's table format."""
    slit: float = float(cfg.get("pupil", {}).get("slitRadius", 0) or 0)
    iris_r_src: float = float(cfg.get("iris", {}).get("radius", 90))
    slit_r: float = IRIS_R * (slit / max(iris_r_src, 1.0)) if slit else 0.0

    tbl: PolarTable = [[0] * IRIS for _ in range(IRIS)]
    for y in range(IRIS):
        dy: float = y - IRIS_R + 0.5
        for x in range(IRIS):
            dx: float = x - IRIS_R + 0.5
            dist: float = math.hypot(dx, dy)
            if dist >= IRIS_R:
                tbl[y][x] = 127          # outside: angle 0, distance 127
                continue
            ang: float = (math.atan2(dy, dx) + math.pi) / (2 * math.pi)
            nd: float
            if slit_r <= 0:
                nd = dist / IRIS_R
            else:
                nd = slit_distance(dx, dy, slit_r)
            nd = max(0.0, min(0.999, nd)) * 128.0
            a: int = int(ang * 512.0) & 0x1FF
            dv: int = 127 - int(nd)
            tbl[y][x] = (a << 7) | (dv & 0x7F)
    return tbl


def slit_distance(dx: float, dy: float, slit_r: float) -> float:
    """Normalised 0..1 distance for a vertical slit pupil.

    Same construction TeensyEyes uses: interpolate a circle through a point
    on the slit's vertical axis and one on the horizontal iris edge, then ask
    which of those circles the pixel falls inside.
    """
    xp: float = abs(dx)
    dy2: float = dy * dy
    for i in range(0, 128):
        ratio: float = i / 127.0
        y1: float = slit_r + (IRIS_R - slit_r) * ratio
        x2: float = IRIS_R * ratio
        if x2 <= 0.0001:
            continue
        xc: float = (x2 * x2 - y1 * y1) / (2 * x2)
        r2: float = (x2 - xc) ** 2
        ddx: float = xp - xc
        if ddx * ddx + dy2 <= r2:
            return 1.0 - (i / 127.0)
    return 1.0


def emit(path: str, name: str, sclera: Image.Image, iris: Image.Image,
         upper: Image.Image, lower: Image.Image,
         polar: PolarTable) -> None:
    """Write one header in the format the renderer expects.

    Symbols carry a suffix so several designs can be included at once, and
    the dimension #defines are repeated in every header -- identical values,
    so redefinition is harmless, and it keeps each header self-contained.
    """
    def rows(f: TextIO, decl: str, vals: Sequence[int], per: int) -> None:
        """Emit one array, `per` values to a line."""
        f.write(decl)
        for i, v in enumerate(vals):
            if i % per == 0:
                f.write("\n  ")
            f.write("0X%04X%s" % (v, "," if i < len(vals) - 1 else " };\n"))

    sp: PixelMap = sclera.load()
    ip: PixelMap = iris.load()
    up: PixelMap = upper.load()
    lp: PixelMap = lower.load()

    with open(path, "w", newline="\n") as f:
        f.write("// Generated from TeensyEyes artwork (MIT) by tools/gen_eyes.py\n")
        f.write("// Source: https://github.com/chrismiller/TeensyEyes\n\n")
        f.write("#define SCLERA_WIDTH  %d\n#define SCLERA_HEIGHT %d\n\n" % (SCLERA, SCLERA))
        rows(f, "const uint16_t sclera%s[SCLERA_HEIGHT][SCLERA_WIDTH] = {" % name,
             [rgb565(*sp[x, y]) for y in range(SCLERA) for x in range(SCLERA)], 8)
        f.write("\n#define IRIS_MAP_WIDTH  %d\n#define IRIS_MAP_HEIGHT %d\n\n"
                % (IRIS_MAP_W, IRIS_MAP_H))
        rows(f, "const uint16_t iris%s[IRIS_MAP_HEIGHT][IRIS_MAP_WIDTH] = {" % name,
             [rgb565(*ip[x, y]) for y in range(IRIS_MAP_H) for x in range(IRIS_MAP_W)], 8)
        f.write("\n#define SCREEN_WIDTH  %d\n#define SCREEN_HEIGHT %d\n\n" % (SCREEN, SCREEN))
        f.write("const uint8_t upper%s[SCREEN_HEIGHT][SCREEN_WIDTH] = {" % name)
        vals: list[int] = [up[x, y] for y in range(SCREEN) for x in range(SCREEN)]
        for i, v in enumerate(vals):
            if i % 12 == 0:
                f.write("\n  ")
            f.write("0X%02X%s" % (v, "," if i < len(vals) - 1 else " };\n"))
        f.write("\nconst uint8_t lower%s[SCREEN_HEIGHT][SCREEN_WIDTH] = {" % name)
        vals = [lp[x, y] for y in range(SCREEN) for x in range(SCREEN)]
        for i, v in enumerate(vals):
            if i % 12 == 0:
                f.write("\n  ")
            f.write("0X%02X%s" % (v, "," if i < len(vals) - 1 else " };\n"))
        f.write("\n#define IRIS_WIDTH  %d\n#define IRIS_HEIGHT %d\n\n" % (IRIS, IRIS))
        rows(f, "const uint16_t polar%s[80][80] = {" % name,
             [polar[y][x] for y in range(IRIS) for x in range(IRIS)], 8)


def render_preview(sclera: Image.Image, iris: Image.Image, upper: Image.Image,
                   lower: Image.Image, polar: PolarTable, iscale: int,
                   gaze: tuple[float, float] = (0.5, 0.5),
                   grey: bool = False) -> Image.Image:
    """Reproduce drawEye() so the result can be judged without hardware."""
    sp: PixelMap = sclera.load()
    ip: PixelMap = iris.load()
    up: PixelMap = upper.load()
    lp: PixelMap = lower.load()
    out: Image.Image = Image.new("RGB", (SCREEN, SCREEN))
    op: PixelMap = out.load()

    sx0: int = int(gaze[0] * (SCLERA - SCREEN))
    sy0: int = int(gaze[1] * (SCLERA - SCREEN))
    ix0: int = sx0 - (SCLERA - IRIS) // 2
    iy0: int = sy0 - (SCLERA - IRIS) // 2
    uT: int = 0
    lT: int = 0  # eyes fully open

    for sy in range(SCREEN):
        iy: int = iy0 + sy
        for sx in range(SCREEN):
            ix: int = ix0 + sx
            c: RGB
            if lp[sx, sy] <= lT or up[sx, sy] <= uT:
                c = (0, 0, 0)
            elif 0 <= iy < IRIS and 0 <= ix < IRIS:
                p: int = polar[iy][ix]
                d: int = (iscale * (p & 0x7F)) // 128
                if d < IRIS_MAP_H:
                    a: int = (IRIS_MAP_W * (p >> 7)) // 512
                    c = ip[min(a, IRIS_MAP_W - 1), d]
                else:
                    c = sp[sx0 + sx, sy0 + sy]
            else:
                c = sp[sx0 + sx, sy0 + sy]
            if grey:
                g: int = (77 * c[0] + 150 * c[1] + 29 * c[2]) >> 8
                g = (g >> 4) * 17  # 16 levels, like the SSD1327 path
                c = (g, g, g)
            op[sx, sy] = c
    return out


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

RENAME: dict[str, str] = {"newt": "newt2"}  # ours already has Adafruit's newt
SAMPLES: tuple[tuple[str, int], ...] = (("constricted", 150), ("normal", 250),
                                        ("dilated", 400))


def camel(n: str) -> str:
    """Design name to symbol suffix: dragon -> Dragon."""
    return n[0].upper() + n[1:]


def main(argv: Sequence[str]) -> int:
    """Convert every design in a TeensyEyes checkout.

    Writes headers into include/eyes/ and preview renders into
    docs/images/eyes/, then leaves the config switch and registry row to be
    added by hand -- deliberately, so nothing is silently enabled.
    """
    src: str = argv[1] if len(argv) > 1 else "teensyeyes/resources/eyes/240x240"
    if not os.path.isdir(src):
        print("usage: gen_eyes.py [path-to-TeensyEyes/resources/eyes/240x240]")
        print("\nGet the artwork with:")
        print("  git clone --depth 1 https://github.com/chrismiller/TeensyEyes.git")
        return 1

    root: str = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hdr: str = os.path.join(root, "include", "eyes")
    img: str = os.path.join(root, "docs", "images", "eyes")
    os.makedirs(hdr, exist_ok=True)
    os.makedirs(img, exist_ok=True)

    n: int = 0
    for name in sorted(os.listdir(src)):
        d: str = os.path.join(src, name)
        if not os.path.exists(os.path.join(d, "config.eye")):
            continue
        out: str = RENAME.get(name, name)
        cfg: Config = load_cfg(d)
        scl: Image.Image = build_sclera(d, cfg)
        iri: Image.Image = build_iris(d, cfg)
        up: Image.Image = build_lid(d, "upper")
        lo: Image.Image = build_lid(d, "lower")
        pol: PolarTable = build_polar(cfg)
        emit(os.path.join(hdr, out + ".h"), camel(out), scl, iri, up, lo, pol)
        for tag, isc in SAMPLES:
            render_preview(scl, iri, up, lo, pol, isc).save(
                os.path.join(img, "%s_%s.png" % (out, tag)))
            render_preview(scl, iri, up, lo, pol, isc, grey=True).save(
                os.path.join(img, "%s_%s_grey.png" % (out, tag)))
        print("  %-12s -> include/eyes/%s.h" % (name, out))
        n += 1

    print("\n%d designs generated." % n)
    print("Add switches to include/eyes_config.h and rows to src/main.cpp.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
