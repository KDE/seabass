#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Synthetic tracks for the export.pdb reference fixture (issue #8).

Seabass is going to append rows to export.pdb, and the only trustworthy
description of how that is done is what rekordbox itself writes. This
makes the input for that: short generated tones, nothing anyone owns, so
the exports made from them can be committed as they are -- no anonymizer
pass, which would rewrite the very bytes the fixture exists to show.

Four sets, imported and exported one after the other (exact steps: #8):

  set-a  12 tracks, the starting library.
  set-b   1 track whose artist, album, genre, label, composer, remixer,
          original artist and artwork are all new, with a non-ASCII
          title (a UTF-16 string) and a comment longer than a short
          string can hold. Shows a track insert that has to insert into
          every lookup table too.
  set-c   1 track reusing every lookup of a01. Shows a track insert and
          nothing else.
  set-d  20 tracks, more than the pages already there have room for, so
          rekordbox has to allocate a new page. One has a title too long
          for a short string (the 0x40 encoding).

Usage: make_pdb_reference_tracks.py OUTPUT_DIR
Needs ffmpeg and python3-mutagen.
"""

import pathlib
import subprocess
import sys

from mutagen.id3 import (APIC, COMM, ID3, TALB, TCOM, TCON, TDRC, TIT2, TOPE,
                         TPE1, TPE4, TPUB, TRCK)

RECIPE = """\
export.pdb reference fixture, issue #8
======================================

The exact steps -- which stick, what to record first, the order of the
four exports, the copy after each, and the player checks -- are in the
issue, so there is one copy of them:

  https://github.com/sebasje/seabass/issues/8#issuecomment-5795427490

In short: RV2 or A4-128GB only (never CORSAIR, WHALESHARK2 or A3),
formatted FAT32 as TESTRIG; export set-a, then set-b, set-c and set-d one
at a time onto the same stick, copying the whole PIONEER folder after
each; keep the stick afterwards.
"""

TONE_SECONDS = 45
BPM = 124


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_audio(path: pathlib.Path, pitch_hz: float):
    # A sine at the given pitch under a click on every beat, so rekordbox
    # has a key and a tempo to find; different pitches give different keys.
    beat = 60.0 / BPM
    expr = (f"0.35*sin(2*PI*{pitch_hz}*t)"
            f"+0.6*sin(2*PI*55*t)*exp(-40*mod(t\\,{beat:.6f}))")
    run("ffmpeg", "-y", "-f", "lavfi", "-i",
        f"aevalsrc={expr}:s=44100:d={TONE_SECONDS}",
        "-ac", "2", "-c:a", "libmp3lame", "-b:a", "320k", str(path))


def make_cover(path: pathlib.Path, colour: str):
    run("ffmpeg", "-y", "-f", "lavfi", "-i", f"color=c={colour}:s=500x500:d=1",
        "-frames:v", "1", str(path))


def tag(path, *, title, artist, album, genre, label, composer, remixer,
        original, comment, number, year, cover):
    tags = ID3()
    tags.add(TIT2(encoding=3, text=title))
    tags.add(TPE1(encoding=3, text=artist))
    tags.add(TALB(encoding=3, text=album))
    tags.add(TCON(encoding=3, text=genre))
    tags.add(TPUB(encoding=3, text=label))
    tags.add(TCOM(encoding=3, text=composer))
    tags.add(TPE4(encoding=3, text=remixer))
    tags.add(TOPE(encoding=3, text=original))
    tags.add(COMM(encoding=3, lang="eng", desc="", text=comment))
    tags.add(TRCK(encoding=3, text=str(number)))
    tags.add(TDRC(encoding=3, text=str(year)))
    tags.add(APIC(encoding=3, mime="image/png", type=3, desc="Cover",
                  data=cover.read_bytes()))
    tags.save(path)


# Pitches of the twelve semitones from A3, so set-a spans every key.
PITCHES = [220.0 * 2 ** (n / 12) for n in range(12)]
COLOURS = ["red", "orange", "yellow", "green", "cyan", "blue", "purple",
           "magenta", "brown", "gray", "olive", "navy", "teal", "maroon"]


def lookups(n):
    # set-a shares its lookups in a pattern, so the base library already
    # has several tracks per artist, album, genre and label -- the normal
    # shape, rather than one row of each per track.
    return dict(
        artist=f"Tone Artist {n % 5 + 1:02d}",
        album=f"Tone Album {n % 4 + 1:02d}",
        genre=["Test House", "Test Techno", "Test Electro"][n % 3],
        label=["Test Label A", "Test Label B"][n % 2],
        composer=f"Tone Composer {n % 3 + 1:02d}",
        remixer=f"Tone Remixer {n % 2 + 1:02d}",
        original=f"Tone Original {n % 2 + 1:02d}",
    )


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1])
    covers = out / ".covers"
    covers.mkdir(parents=True, exist_ok=True)
    for i, colour in enumerate(COLOURS):
        make_cover(covers / f"{i:02d}.png", colour)

    def track(folder, name, n, pitch, cover, comment, title=None, **overrides):
        folder = out / folder
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"{name}.mp3"
        make_audio(path, pitch)
        fields = lookups(n)
        fields.update(overrides)
        tag(path, title=title or f"Tone {name.upper()}", comment=comment,
            number=n + 1, year=2020 + n % 6, cover=covers / f"{cover:02d}.png",
            **fields)
        print(path)

    for n in range(12):
        track("set-a", f"a{n + 1:02d}", n, PITCHES[n], n % 4, "set-a")

    track("set-b", "b01", 0, 233.08, 12,
          "set-b: every lookup is new, and this comment is deliberately "
          "longer than the hundred and twenty six bytes a short device "
          "string can carry, so rekordbox has to pick a longer encoding.",
          title="Tëst Ünïcode Café, Déjà Vu",
          artist="Nouvel Artiste", album="Nouvel Album", genre="Test Ambient",
          label="Test Label C", composer="New Composer", remixer="New Remixer",
          original="New Original")

    track("set-c", "c01", 0, 246.94, 0, "set-c: every lookup reused from a01")

    for n in range(20):
        long_title = ("Tone D20 with a title long enough that it cannot be a "
                      "short device string, which tops out at one hundred and "
                      "twenty six bytes of text") if n == 19 else None
        track("set-d", f"d{n + 1:02d}", n, PITCHES[n % 12] * 1.5, 13 - n % 4,
              "set-d", title=long_title)

    (out / "RECIPE.txt").write_text(RECIPE)
    print(out / "RECIPE.txt")


if __name__ == "__main__":
    main()
