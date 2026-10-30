#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Every form of the Seabass mascot the app ships, from Nuno's artwork.

The mark is a drawn fish, wider than tall, transparent around it: not
generated geometry. Its source is artwork/seabass.svgz, whose filters only
Inkscape renders faithfully, so Nuno renders it and the rendering is what
this script starts from:

  * artwork/seabass-hires.png            -- Nuno's rendering, 1800 x 1172
                                            (2026-10-18). The input.
  * artwork/seabass-icon-256.png         -- Nuno's own 256 pixel icon, kept
                                            as the reference for the square
                                            form: the fish spans the full
                                            width, centred vertically.

What it writes:

  * src/gui/qml/icons/seabass_mascot.png -- the fish, 1024 wide, cropped to
                                            its drawn pixels: what the About
                                            page, the home rail and the
                                            watermark show.
  * src/gui/qml/icons/seabass_icon.png   -- 1024 x 1024, the fish spanning
                                            the width as in Nuno's icon:
                                            the window and taskbar icon.
  * src/gui/linux/hicolor/NxN.png        -- the same, at the sizes the
                                            hicolor theme lists (16 to
                                            512): the installed icon on
                                            Linux. hicolor has no 1024
                                            directory, so a lone 1024 icon
                                            is never found.
  * src/gui/win/app_icon.ico             -- the .exe icon on Windows.
  * src/gui/mac/app_icon.icns            -- the app bundle icon on macOS.

Run from the repository root after a new rendering:

    python3 tools/make_app_icons.py

The committed app_icon.icns is then remade on a Mac, because Pillow
writes no 16 and 32 pixel images (Apple's icp4/icp5), which a non-Retina
Mac draws in Finder's list view and Spotlight:

    mkdir seabass.iconset
    for s in 16 32 128 256 512; do
        sips -z $s $s src/gui/qml/icons/seabass_icon.png --out seabass.iconset/icon_${s}x${s}.png
        sips -z $((s*2)) $((s*2)) src/gui/qml/icons/seabass_icon.png --out seabass.iconset/icon_${s}x${s}@2x.png
    done
    iconutil -c icns -o src/gui/mac/app_icon.icns seabass.iconset

A new drawing also changes two measured numbers in the QML: the mascot's
height (AboutPage.qml, tst_AboutPage.qml) and WatermarkLayer's markPadY,
which this script prints.
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "artwork/seabass-hires.png"
MASCOT = ROOT / "src/gui/qml/icons/seabass_mascot.png"
SQUARE = ROOT / "src/gui/qml/icons/seabass_icon.png"
ICO = ROOT / "src/gui/win/app_icon.ico"
ICNS = ROOT / "src/gui/mac/app_icon.icns"
HICOLOR = ROOT / "src/gui/linux/hicolor"
# The fixed-size app directories of hicolor's index.theme.
HICOLOR_SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512)

MASCOT_WIDTH = 1024
SIDE = 1024
# The fish fills this much of the square's width. Nuno's own 256 pixel
# icon (artwork/seabass-icon-256.png) spans the whole width, so the fins
# reach the edge as they do there.
FILL = 1.0


def drawn_fish() -> Image.Image:
    # Tight to the drawn pixels, so the air is even on every side.
    fish = Image.open(SOURCE).convert("RGBA")
    return fish.crop(fish.getbbox())


def mascot(fish: Image.Image) -> Image.Image:
    height = round(fish.height * MASCOT_WIDTH / fish.width)
    return fish.resize((MASCOT_WIDTH, height), Image.LANCZOS)


def square_icon(fish: Image.Image) -> Image.Image:
    scale = SIDE * FILL / max(fish.width, fish.height)
    fish = fish.resize((round(fish.width * scale), round(fish.height * scale)), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIDE, SIDE), (0, 0, 0, 0))
    canvas.alpha_composite(fish, ((SIDE - fish.width) // 2, (SIDE - fish.height) // 2))
    return canvas


def main() -> None:
    fish = drawn_fish()
    small = mascot(fish)
    small.save(MASCOT, optimize=True)
    icon = square_icon(fish)
    icon.save(SQUARE, optimize=True)
    icon.save(ICO, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    icon.save(ICNS, sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    HICOLOR.mkdir(parents=True, exist_ok=True)
    hicolor = [HICOLOR / f"{size}x{size}.png" for size in HICOLOR_SIZES]
    for size, path in zip(HICOLOR_SIZES, hicolor):
        icon.resize((size, size), Image.LANCZOS).save(path, optimize=True)
    for path in (MASCOT, SQUARE, ICO, ICNS, *hicolor):
        print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes")
    # WatermarkLayer places the mascot as if it sat centred in a square of
    # its own width: how far above that square's bottom its ink stops.
    pad_y = 1 - ((MASCOT_WIDTH - small.height) / 2 + small.height) / MASCOT_WIDTH
    print(f"mascot {small.width} x {small.height}; WatermarkLayer markPadY {pad_y:.4f}")


if __name__ == "__main__":
    main()
