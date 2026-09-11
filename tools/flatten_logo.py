#!/usr/bin/env python3
"""Single source of truth for the Seabass "Sound Bass" brand mark.

The design is a fish whose body is filled with equalizer bars: a deep base
slab per bar with a brighter level segment centred on the waveform axis,
in the app's Current/Kelp palette.

This script owns the geometry and emits every form the mark ships in:

  * src/gui/qml/icons/seabass_soundbass.svg -- the app icon, window icon
    and the Now Playing watermark.
  * src/gui/win/app_icon.ico               -- the .exe icon on Windows.
  * --website DIR                          -- the same SVG plus the PNG
    favicon/apple-touch set for the website repository.

Two constraints shape how the SVG is written:

  * Qt's SVG renderer (QtSvg, behind Qt Quick's Image, QIcon and therefore
    the window and taskbar icon) does not apply <clipPath>. The fish
    outline is therefore *baked into* every bar's geometry: each bar is
    emitted as the polygon of (bar rectangle intersected with the fish
    outline). An earlier version relied on a clip path and Qt drew the
    bars unclipped, so the logo looked like a stretched barcode.
  * Every curve is flattened to line segments, so Qt, browsers and this
    script's own rasteriser all produce exactly the same shape.

The fins are attached along the body outline itself rather than overlapping
it, because the gaps between bars are transparent -- an overlapping fin
would show through them.

Run from the repository root; no dependencies beyond the standard library,
except Pillow for --ico/--website (raster output).
"""

from __future__ import annotations

import argparse
from pathlib import Path

# ---------------------------------------------------------------------------
# Geometry, in "fish units": the body is centred on the origin, runs from the
# tail peduncle at x=-21 to the nose at x=20, and is 13 units tall each side
# of the waveform axis. The flat edge at x=-21 is where the caudal fin butts
# on, so the join is seamless without overlapping anything.
# ---------------------------------------------------------------------------

BODY_NOSE_X = 20.0
BODY_TAIL_X = -21.0
BODY_HALF_H = 13.0

# Closed outline of the body, as cubic segments and straight lines.
BODY = [
    ((20, 0), (20, 7.8), (11, 13), (0, 13)),
    ((0, 13), (-10, 13), (-17, 8.7), (-21, 3.5)),
    "L(-21,-3.5)",
    ((-21, -3.5), (-17, -8.7), (-10, -13), (0, -13)),
    ((0, -13), (11, -13), (20, -7.8), (20, 0)),
]

# Forked caudal fin. Starts and ends on the body's flat peduncle edge, so the
# two shapes meet exactly; the middle curve bows towards the body to cut the
# fork. Replaces the original's leftmost bar, which the fork split into two
# floating wedges that read as a chevron beside the fish rather than a tail.
TAIL = [
    "M(-21,-3.5)",
    ((-21, -3.5), (-24, -5.5), (-26.2, -7.5), (-28.5, -10.2)),
    ((-28.5, -10.2), (-26.4, -5.8), (-26.4, 5.8), (-28.5, 10.2)),
    ((-28.5, 10.2), (-26.2, 7.5), (-24, 5.5), (-21, 3.5)),
]

# Dorsal and anal fins. Their bases are not straight lines: each is the body's
# own outline between two x positions, lifted out at runtime, so a fin grows
# out of the body instead of sitting on top of it. That matters here because
# the gaps between bars are transparent -- a fin that merely overlapped the
# body would show through them. The pair also squares up the composition,
# which a bare fish shape leaves very wide and short inside a square icon.
DORSAL_BASE = (-8.0, 2.0)
DORSAL_APEX = (-6.5, -17.5)
DORSAL_LEADING = ((-0.5, -15.5), (-4.0, -17.3))  # front of base -> apex
DORSAL_TRAILING = ((-7.5, -16.0), (-7.8, -14.3))  # apex -> rear of base

ANAL_BASE = (-7.0, -1.0)
ANAL_APEX = (-6.5, 16.0)
ANAL_LEADING = ((-2.5, 14.8), (-4.5, 16.0))
ANAL_TRAILING = ((-7.2, 15.0), (-7.3, 13.7))

# Seven bars spanning the body edge to edge: wide enough to survive a 16 px
# favicon, where the original's ten narrow bars turned into a smudge.
BAR_COUNT = 7
BAR_GAP = 1.1
# Half-height of each bar's bright level segment, tail end first. Bars whose
# level exceeds the body at that x simply fill it, which is what keeps the
# nose and the peduncle solid.
BAR_LEVELS = [3.5, 6.5, 10.5, 8.0, 11.5, 5.5, 3.0]

# The eye is centred on a bar rather than placed at a hand-picked x. The
# gaps between bars are transparent, so an eye wide enough to cross one
# paints its ring over nothing: a notch of background bitten out of the
# eye on a light ground, and an outer edge that dissolves on a dark one.
# Hand-placed, it overhung its bar by 0.71 units and did exactly that.
EYE_BAR = 5
EYE_Y = -4.0
EYE_RING_R = 2.2
EYE_WHITE_R = 1.6
EYE_PUPIL_R = 0.9
EYE_PUPIL_DX = 0.4

# ---------------------------------------------------------------------------
# Palette. Each gradient spans the *body's* full height in user space rather
# than its own bar's, so all seven bars share one light source; the original
# gave every bar its own top-to-bottom ramp, which stopped the body reading as
# a single object. The bottom stops stay light enough to hold their own
# against a dark taskbar.
#
# One deep tone backs every bar, so the body reads as a single dark mass and
# only the lit segments alternate Current/Kelp. Alternating the backing tone
# as well gave four competing colours per row and the level read was lost.
# ---------------------------------------------------------------------------

PALETTE = {
    "deep": ("#2a5f7f", "#123a52"),
    "current": ("#8fdcff", "#2f9ad6"),
    "kelp": ("#72f0cf", "#15a583"),
    "fin": ("#4fb8e6", "#1b5f83"),
}
EYE_DARK = "#0d2b36"
EYE_WHITE = "#eef6fb"

VIEWBOX = 200.0
MARGIN = 6.0
FLATTEN_STEPS = 48


# ---------------------------------------------------------------------------
# Curve flattening and polygon clipping
# ---------------------------------------------------------------------------


def _point(spec: str) -> tuple[float, float]:
    """Parses the "M(x,y)" / "L(x,y)" shorthand used in the outlines above."""
    body = spec[1:].strip().lstrip("(").rstrip(")")
    x, y = body.split(",")
    return float(x), float(y)


def flatten(segments, steps: int = FLATTEN_STEPS) -> list[tuple[float, float]]:
    """Turns a list of cubic segments and M/L shorthands into a polygon."""
    pts: list[tuple[float, float]] = []
    for seg in segments:
        if isinstance(seg, str):
            pts.append(_point(seg))
            continue
        p0, p1, p2, p3 = seg
        for i in range(1, steps + 1):
            t = i / steps
            u = 1 - t
            x = u**3 * p0[0] + 3 * u**2 * t * p1[0] + 3 * u * t**2 * p2[0] + t**3 * p3[0]
            y = u**3 * p0[1] + 3 * u**2 * t * p1[1] + 3 * u * t**2 * p2[1] + t**3 * p3[1]
            pts.append((x, y))
    return pts


def clip_to_rect(polygon, x0, y0, x1, y1):
    """Sutherland-Hodgman: the part of `polygon` inside an axis-aligned rect."""

    def clip_edge(poly, inside, intersect):
        out = []
        if not poly:
            return out
        prev = poly[-1]
        for cur in poly:
            if inside(cur):
                if not inside(prev):
                    out.append(intersect(prev, cur))
                out.append(cur)
            elif inside(prev):
                out.append(intersect(prev, cur))
            prev = cur
        return out

    def at_x(x):
        return lambda a, b: (x, a[1] + (b[1] - a[1]) * (x - a[0]) / (b[0] - a[0]))

    def at_y(y):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]), y)

    poly = polygon
    poly = clip_edge(poly, lambda p: p[0] >= x0, at_x(x0))
    poly = clip_edge(poly, lambda p: p[0] <= x1, at_x(x1))
    poly = clip_edge(poly, lambda p: p[1] >= y0, at_y(y0))
    poly = clip_edge(poly, lambda p: p[1] <= y1, at_y(y1))
    return poly


def outline_arc(body_polygon, x_from: float, x_to: float, upper: bool):
    """The body's upper or lower edge between two x positions, rear to front.

    Used as a fin's base so the fin and the body share an edge exactly: no
    overlap to show through the transparent gaps between bars, and no sliver
    of background between the two.
    """
    side = (lambda p: p[1] < 0) if upper else (lambda p: p[1] > 0)
    arc = [p for p in body_polygon if side(p) and x_from <= p[0] <= x_to]
    arc.sort(key=lambda p: p[0])
    return arc


def fin(body_polygon, base, apex, leading, trailing, upper: bool, name: str):
    """A fin rooted in the body outline between base[0] and base[1].

    Walks the body's own edge from the rear of the base to the front, out
    along the leading edge to the apex, then back down the trailing edge.

    A base too narrow to catch any of the flattened outline's points would
    leave the fish finless. Raise rather than skip: the icon is written to
    three places at once and a silent success is worse than a stack trace.
    """
    arc = outline_arc(body_polygon, base[0], base[1], upper)
    if len(arc) < 2:
        raise SystemExit(
            f"the {name} fin's base spans {base[0]} to {base[1]}, which catches "
            f"{len(arc)} points of the body outline; it needs at least two"
        )
    rear, front = arc[0], arc[-1]
    poly = list(arc)
    poly += flatten([(front, leading[0], leading[1], apex)])
    poly += flatten([(apex, trailing[0], trailing[1], rear)])
    return poly


# ---------------------------------------------------------------------------
# The mark itself: a flat list of shapes in fish units
# ---------------------------------------------------------------------------


def bar_rects():
    """(x, width, level) for each bar, spanning the body exactly."""
    if len(BAR_LEVELS) != BAR_COUNT:
        raise SystemExit(
            f"BAR_LEVELS has {len(BAR_LEVELS)} entries but BAR_COUNT is {BAR_COUNT}; "
            "the bars would be laid out for a count they no longer have"
        )
    span = BODY_NOSE_X - BODY_TAIL_X
    width = (span - BAR_GAP * (BAR_COUNT - 1)) / BAR_COUNT
    for i, level in enumerate(BAR_LEVELS):
        yield BODY_TAIL_X + i * (width + BAR_GAP), width, level


def build_shapes():
    """Returns [(kind, payload, gradient-or-colour)] in draw order."""
    body = flatten(BODY)
    shapes = []

    shapes.append(("poly", flatten(TAIL), "fin"))

    shapes.append(("poly", fin(body, DORSAL_BASE, DORSAL_APEX, DORSAL_LEADING,
                               DORSAL_TRAILING, True, "dorsal"), "fin"))
    shapes.append(("poly", fin(body, ANAL_BASE, ANAL_APEX, ANAL_LEADING,
                               ANAL_TRAILING, False, "anal"), "fin"))

    for i, (x, width, level) in enumerate(bar_rects()):
        bright = "current" if i % 2 == 0 else "kelp"
        slab = clip_to_rect(body, x, -BODY_HALF_H, x + width, BODY_HALF_H)
        lit = clip_to_rect(body, x, -level, x + width, level)
        if len(slab) < 3 or len(lit) < 3:
            raise SystemExit(
                f"bar {i} at x={x:.2f} level={level} collapsed to "
                f"{len(slab)}/{len(lit)} points; it would not be drawn at all"
            )
        shapes.append(("poly", slab, "deep"))
        shapes.append(("poly", lit, bright))

    bars = list(bar_rects())
    bx, bw, _ = bars[EYE_BAR]
    reach = max(EYE_RING_R, EYE_PUPIL_DX + EYE_PUPIL_R)
    if reach > bw / 2:
        raise SystemExit(
            f"the eye reaches {reach:.2f} from its centre but bar {EYE_BAR} is only "
            f"{bw:.2f} wide, so it would be painted over a transparent gap"
        )
    ex, ey = bx + bw / 2, EYE_Y
    shapes.append(("circle", (ex, ey, EYE_RING_R), EYE_DARK))
    shapes.append(("circle", (ex, ey, EYE_WHITE_R), EYE_WHITE))
    shapes.append(("circle", (ex + EYE_PUPIL_DX, ey, EYE_PUPIL_R), EYE_DARK))
    return shapes


def fit_transform(shapes):
    """Scale and offset that centre the mark in the 200x200 viewBox."""
    xs, ys = [], []
    for kind, payload, _ in shapes:
        if kind == "poly":
            xs += [p[0] for p in payload]
            ys += [p[1] for p in payload]
        else:
            cx, cy, r = payload
            xs += [cx - r, cx + r]
            ys += [cy - r, cy + r]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    scale = (VIEWBOX - 2 * MARGIN) / max(w, h)
    ox = (VIEWBOX - w * scale) / 2 - min(xs) * scale
    oy = (VIEWBOX - h * scale) / 2 - min(ys) * scale
    return scale, ox, oy


# ---------------------------------------------------------------------------
# SVG output
# ---------------------------------------------------------------------------

HEADER = """  <!--
    "Sound Bass": the Seabass app icon, window icon and Now Playing
    watermark. A fish whose body is filled with equalizer bars, in the
    app's Current/Kelp palette.

    GENERATED by tools/flatten_logo.py. Edit the generator, not this
    file. The fish outline is baked into each bar's geometry and every
    curve is flattened to line segments, because Qt's SVG renderer
    ignores <clipPath>: it drew the bars unclipped, so the logo looked
    stretched.

    Careful with the prose in here. XML forbids a double hyphen inside
    a comment, and the rest of this codebase writes its dashes that way,
    so an editor reaching for one turns the whole file into something
    QtSvg refuses to decode. Qt then draws nothing at all, silently.
  -->"""


def check_svg(svg: str) -> None:
    """Refuses to emit an SVG that Qt would silently decline to draw.

    The comment block is the only free prose in the file and the only
    place this can go wrong, but the cost of getting it wrong is a blank
    window icon with no error anywhere, so it is worth a hard stop.
    """
    _, _, rest = svg.partition("<!--")
    comment, _, _ = rest.partition("-->")
    if "--" in comment:
        raise SystemExit(
            "refusing to write the SVG: its comment contains a double hyphen, "
            "which is not legal XML and makes QtSvg reject the whole file"
        )
    import xml.etree.ElementTree as ET

    try:
        ET.fromstring(svg)
    except ET.ParseError as exc:
        raise SystemExit(f"refusing to write the SVG: it is not well-formed XML ({exc})")


def emit_svg(shapes, scale, ox, oy) -> str:
    top = oy - BODY_HALF_H * scale
    bottom = oy + BODY_HALF_H * scale
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {VIEWBOX:.0f} {VIEWBOX:.0f}">',
           HEADER, "  <defs>"]
    for name, (start, end) in PALETTE.items():
        out.append(
            f'    <linearGradient id="{name}" gradientUnits="userSpaceOnUse"'
            f' x1="0" y1="{top:.2f}" x2="0" y2="{bottom:.2f}">'
        )
        out.append(f'      <stop offset="0" stop-color="{start}"/>')
        out.append(f'      <stop offset="1" stop-color="{end}"/>')
        out.append("    </linearGradient>")
    out.append("  </defs>")

    for kind, payload, fill in shapes:
        paint = f"url(#{fill})" if fill in PALETTE else fill
        if kind == "poly":
            d = "M" + " L".join(
                f"{ox + x * scale:.2f},{oy + y * scale:.2f}" for x, y in payload
            ) + " Z"
            out.append(f'  <path d="{d}" fill="{paint}"/>')
        else:
            cx, cy, r = payload
            out.append(
                f'  <circle cx="{ox + cx * scale:.2f}" cy="{oy + cy * scale:.2f}"'
                f' r="{r * scale:.2f}" fill="{paint}"/>'
            )
    out.append("</svg>")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Raster output (PNG set and Windows .ico), from the same geometry
# ---------------------------------------------------------------------------


def _hex_rgb(value: str):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def render(shapes, scale, ox, oy, size: int, supersample: int = 8):
    """Renders the mark to an RGBA image of `size` px, supersampled."""
    from PIL import Image, ImageDraw

    ss = max(1, min(supersample, 4096 // max(size, 1)))
    px = size * ss
    k = px / VIEWBOX

    def to_px(x, y):
        return ((ox + x * scale) * k, (oy + y * scale) * k)

    top = (oy - BODY_HALF_H * scale) * k
    bottom = (oy + BODY_HALF_H * scale) * k

    ramps = {}
    for name, (start, end) in PALETTE.items():
        a, b = _hex_rgb(start), _hex_rgb(end)
        strip = Image.new("RGB", (1, px))
        pixels = strip.load()
        for y in range(px):
            t = min(1.0, max(0.0, (y - top) / (bottom - top)))
            pixels[0, y] = tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))
        ramps[name] = strip.resize((px, px)).convert("RGBA")

    canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    for kind, payload, fill in shapes:
        mask = Image.new("L", (px, px), 0)
        pen = ImageDraw.Draw(mask)
        if kind == "poly":
            pen.polygon([to_px(x, y) for x, y in payload], fill=255)
        else:
            cx, cy, r = payload
            x, y = to_px(cx, cy)
            rr = r * scale * k
            pen.ellipse([x - rr, y - rr, x + rr, y + rr], fill=255)
        layer = ramps.get(fill)
        if layer is None:
            layer = Image.new("RGBA", (px, px), _hex_rgb(fill) + (255,))
        canvas.paste(layer, (0, 0), mask)

    if ss > 1:
        canvas = canvas.resize((size, size), Image.LANCZOS)
    return canvas


PNG_SIZES = [16, 32, 48, 64, 128, 180, 192, 256, 512, 1024]
ICO_SIZES = [16, 32, 48, 64, 128, 256]

# iOS ignores the alpha channel on an apple-touch-icon and composites what
# is left over black. The body's deep tone is near-black navy, so a
# transparent 180 px icon loses the whole silhouette on a home screen and
# only the lit bars survive. This one size gets an opaque ground, the same
# near-white the mark is drawn against on the site.
OPAQUE_PNG_GROUND = {180: "#eef6fb"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--website",
        metavar="DIR",
        help="also write DIR/assets/seabass.svg and DIR/assets/icons/seabass-<n>.png",
    )
    parser.add_argument("--no-ico", action="store_true", help="skip src/gui/win/app_icon.ico")
    args = parser.parse_args()

    shapes = build_shapes()
    scale, ox, oy = fit_transform(shapes)
    svg = emit_svg(shapes, scale, ox, oy)
    check_svg(svg)

    target = Path("src/gui/qml/icons/seabass_soundbass.svg")
    target.write_text(svg)
    print(f"wrote {target}")

    if not args.no_ico:
        ico = Path("src/gui/win/app_icon.ico")
        images = [render(shapes, scale, ox, oy, s) for s in ICO_SIZES]
        images[-1].save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                        append_images=images[:-1])
        print(f"wrote {ico} ({', '.join(str(s) for s in ICO_SIZES)})")

    if args.website:
        # Finding 3: create the directories before writing anything, so a
        # mistyped --website fails before the app's own files are rewritten.
        root = Path(args.website)
        assets = root / "assets"
        icons = assets / "icons"
        icons.mkdir(parents=True, exist_ok=True)
        (assets / "seabass.svg").write_text(svg)
        print(f"wrote {assets / 'seabass.svg'}")
        for size in PNG_SIZES:
            out = icons / f"seabass-{size}.png"
            image = render(shapes, scale, ox, oy, size)
            ground = OPAQUE_PNG_GROUND.get(size)
            if ground:
                from PIL import Image

                flat = Image.new("RGBA", image.size, _hex_rgb(ground) + (255,))
                flat.alpha_composite(image)
                image = flat.convert("RGB")
            image.save(out, optimize=True)
            print(f"wrote {out}" + (f" (opaque {ground})" if ground else ""))


if __name__ == "__main__":
    main()
