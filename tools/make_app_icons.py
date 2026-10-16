#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Every icon form of the Seabass mascot, from Nuno's artwork.

The mark is a drawn fish (src/gui/qml/icons/seabass_mascot.png, wider than
tall, transparent around it), not generated geometry: this script only
derives the square forms the platforms want from it.

  * src/gui/qml/icons/seabass_icon.png  -- 1024 x 1024, the fish centred
                                           with a little air around it:
                                           the window and taskbar icon.
  * src/gui/win/app_icon.ico            -- the .exe icon on Windows.
  * src/gui/mac/app_icon.icns           -- the app bundle icon on macOS.

Run from the repository root after a new mascot.png:

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
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
MASCOT = ROOT / "src/gui/qml/icons/seabass_mascot.png"
SQUARE = ROOT / "src/gui/qml/icons/seabass_icon.png"
ICO = ROOT / "src/gui/win/app_icon.ico"
ICNS = ROOT / "src/gui/mac/app_icon.icns"

SIDE = 1024
# The fish fills this much of the square's width; the rest is air, so the
# fins do not touch the icon's edge at small sizes.
FILL = 0.92


def square_icon() -> Image.Image:
    fish = Image.open(MASCOT).convert("RGBA")
    # Tight to the drawn pixels first, so the air is even on every side.
    fish = fish.crop(fish.getbbox())
    scale = SIDE * FILL / max(fish.width, fish.height)
    fish = fish.resize((round(fish.width * scale), round(fish.height * scale)), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIDE, SIDE), (0, 0, 0, 0))
    canvas.alpha_composite(fish, ((SIDE - fish.width) // 2, (SIDE - fish.height) // 2))
    return canvas


def main() -> None:
    icon = square_icon()
    icon.save(SQUARE, optimize=True)
    icon.save(ICO, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    icon.save(ICNS, sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    for path in (SQUARE, ICO, ICNS):
        print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
