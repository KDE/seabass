#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Builds the tier 2 fixture: real analysis data, de-identified, no audio.

    tools/make-real-analysis-fixture.py --seabass-cli <build>/seabass-cli \\
        --stick <private stick> --work <dir> [--name real-analysis] [--date YYYY-MM-DD]

<private stick> holds PIONEER/ and "Engine Library/". Nothing under it is
written: seabass-cli anonymize copies before it edits. The steps:

  1. `seabass-cli anonymize --keep-waveforms` on both catalogs, into
     <work>/anonymized.zip. That replaces every title, artist, path and
     comment, drops the play history, the artwork and anything it cannot
     scrub, keeps every waveform, and reads its own export back.
  2. The export is laid out as a stick (PIONEER/, "Engine Library/"), so
     the app's readers open it as they open a stick.
  3. Refuses to go on if any file is audio or an image, by name or bytes.
  4. Writes MANIFEST.json (counts, every paired track's analysis counts,
     the SHA-256 of every file) and README.md.
  5. Zips it to <work>/out/<name>-<date>.zip with fixed timestamps, and
     writes the zip's SHA-256 beside it.

Then run tools/check-fixture-deidentified.py on the zip before it goes
anywhere. That step is not here on purpose: it is the one that decides,
and it should be run and read by a person.
"""

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import zipfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

_spec = importlib.util.spec_from_file_location("fixture_check", HERE / "check-fixture-deidentified.py")
check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check)

# Track row strings of export.pdb, by index (specs/rekordbox_pdb.ksy).
TRACK_ANALYZE_PATH = 14
TRACK_FILENAME = 19

AUDIO_OR_IMAGE_EXTENSIONS = {
    ".mp3", ".m4a", ".mp4", ".aac", ".flac", ".wav", ".aif", ".aiff", ".ogg", ".opus", ".alac", ".wma",
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif", ".tiff", ".heic",
}
AUDIO_OR_IMAGE_MAGIC = [
    (0, b"ID3"), (0, b"fLaC"), (0, b"OggS"), (0, b"RIFF"), (0, b"FORM"), (4, b"ftyp"),
    (0, b"\xff\xd8\xff"), (0, b"\x89PNG"), (0, b"GIF8"), (0, b"BM"),
]

README = """# {name}-{date}

Real track analysis from one DJ's USB stick, de-identified, without audio.
Tier 2 of Seabass's test fixtures: see "Test fixtures in three tiers" in
docs/testing.md of the Seabass repository.

## What is in it

* `PIONEER/rekordbox/`: the rekordbox export (export.pdb, exportExt.pdb,
  exportLibrary.db) of {rb_tracks} tracks.
* `PIONEER/USBANLZ/`: the analysis files (.DAT, .EXT, .2EX) of the
  tracks that are also in the Engine library, with every section kept:
  the grids and all the waveform sections.
* `Engine Library/Database2/m.db`: the Engine library of {en_tracks} tracks
  as the Denon hardware analysed them (grids, overview waveforms, keys,
  loudness), and `OverviewData/`, the .rgb overview files Engine keeps.
* `MANIFEST.json`: counts, the analysis counts of each of the
  {paired} paired tracks, and the SHA-256 of every file.
* `anonymize-MANIFEST.txt` and `anonymize-files.tsv`: what the anonymizer
  wrote about this export.

Every title, artist, album, genre, label, comment, cue comment, file
name, path and playlist name is replaced with a placeholder; the same
real track has the same placeholder in all three catalogs. There is no
audio, no artwork and no play history.

## Licence

The arrangement of this data is released under CC0-1.0
(https://creativecommons.org/publicdomain/zero/1.0/).

## How the tests use it

Configure Seabass with `-DSEABASS_FETCH_FIXTURES=ON` to download and
unpack this zip into the build tree, or with
`-DSEABASS_REAL_ANALYSIS_FIXTURE=<dir>` to use an unpacked copy.
`real_analysis_fixture_test` checks the tree against MANIFEST.json, then
reads every paired track through the app's own readers and compares
their counts with the ones recorded here.

Built by `tools/make-real-analysis-fixture.py` from Seabass {version}
({commit}).
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def repo_version():
    m = re.search(r"project\(seabass VERSION ([0-9.]+)", (REPO / "CMakeLists.txt").read_text())
    return m.group(1) if m else "unknown"


def repo_commit():
    try:
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short=8", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return head + ("+changes" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def pdb_tracks(data):
    """(id, filename, analyze path) of every present track row."""
    out = []
    page_len = int.from_bytes(data[4:8], "little")
    for page in range(page_len, len(data) - page_len + 1, page_len):
        if int.from_bytes(data[page + 8:page + 12], "little") != 0 or data[page + 0x1B] & 0x40:
            continue
        count = int.from_bytes(data[page + 0x18:page + 0x1B], "little") & 0x1FFF
        for index in range(count):
            group, r = divmod(index, 16)
            base = page + page_len - group * 0x24
            if not (int.from_bytes(data[base - 4:base - 2], "little") >> r) & 1:
                continue
            row = page + 0x28 + int.from_bytes(data[base - (6 + 2 * r):base - (4 + 2 * r)], "little")
            offsets = [row + int.from_bytes(data[row + 0x5E + 2 * i:row + 0x60 + 2 * i], "little") for i in range(21)]
            track_id = int.from_bytes(data[row + 0x48:row + 0x4C], "little")
            out.append((track_id, check.device_sql_string_at(data, offsets[TRACK_FILENAME]) or "",
                        check.device_sql_string_at(data, offsets[TRACK_ANALYZE_PATH]) or ""))
    return out


def anlz_sections(path):
    """{tag: section bytes} of one ANLZ file, or {} when there is none."""
    if not path.is_file():
        return {}
    data = path.read_bytes()
    sections = {}
    if data[:4] != b"PMAI":
        return sections
    at = int.from_bytes(data[4:8], "big")
    while at + 12 <= len(data):
        length = int.from_bytes(data[at + 8:at + 12], "big")
        if length < 12 or at + length > len(data):
            break
        sections.setdefault(data[at:at + 4].decode("latin-1"), data[at:at + length])
        at += length
    return sections


def u32(section, at):
    return int.from_bytes(section[at:at + 4], "big") if len(section) >= at + 4 else None


def rekordbox_counts(pioneer, analyze_path):
    rel = analyze_path.lstrip("/")
    if rel.startswith("PIONEER/"):
        rel = rel[len("PIONEER/"):]
    dat = pioneer / rel
    files = {"DAT": dat, "EXT": dat.with_suffix(".EXT"), "2EX": dat.with_suffix(".2EX")}
    tags = {}
    present = {}
    for kind, path in files.items():
        sections = anlz_sections(path)
        present[kind] = path.is_file()
        for tag, section in sections.items():
            tags.setdefault(tag, section)
    counts = {
        "files": sorted(k for k, v in present.items() if v),
        "tags": sorted(tags),
        # PQTZ: num_beats; PWAV: len_preview; PWV3 to PWV7: len_entries.
        "beats": u32(tags["PQTZ"], 20) if "PQTZ" in tags else 0,
        "pwav": u32(tags["PWAV"], 12) if "PWAV" in tags else 0,
    }
    for tag in ("PWV3", "PWV4", "PWV5", "PWV6", "PWV7"):
        counts[tag.lower()] = u32(tags[tag], 16) if tag in tags else 0
    return counts


def engine_counts(conn, track_id):
    row = conn.execute("SELECT overviewWaveFormData, beatData FROM PerformanceData WHERE trackId = ?",
                       (track_id,)).fetchone()
    overview, beat = (row or (None, None))
    entries = 0
    if overview:
        inflated = check.inflate_engine_blob(overview)
        if inflated and len(inflated) >= 8:
            entries = int.from_bytes(inflated[:8], "big", signed=True)
    markers = 0
    if beat:
        inflated = check.inflate_engine_blob(beat)
        # sample rate, sample count (doubles), is_beatgrid_set (u8), then
        # the default grid (count, 24 bytes a marker) and the adjusted one.
        if inflated and len(inflated) >= 25:
            default_count = int.from_bytes(inflated[17:25], "big", signed=True)
            adjusted_at = 25 + 24 * default_count
            if len(inflated) >= adjusted_at + 8:
                markers = int.from_bytes(inflated[adjusted_at:adjusted_at + 8], "big", signed=True)
    return {"overview_entries": entries, "grid_markers": markers}


def build_manifest(root, name, date, rb_tracks, en_rows, paired, ambiguous, version, commit):
    files = {}
    kinds = Counter()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel == "MANIFEST.json":
            continue
        files[rel] = {"size": path.stat().st_size, "sha256": sha256(path)}
        kinds[path.suffix.lstrip(".") or path.name] += 1
    section_tracks = Counter()
    for p in paired:
        for tag in p["rekordbox"]["tags"]:
            section_tracks[tag] += 1
    return {
        "name": name,
        "date": date,
        "description": "Real rekordbox and Engine track analysis from one USB stick, de-identified, without audio.",
        "licence": "CC0-1.0",
        "tool": {
            "seabass_version": version,
            "commit": commit,
            "command": "seabass-cli anonymize --rekordbox <stick>/PIONEER --engine \"<stick>/Engine Library\" "
                       "--keep-waveforms --out <dir>",
        },
        "layout": {"pioneer": "PIONEER", "engine": "Engine Library"},
        "counts": {
            "rekordbox_tracks": len(rb_tracks),
            "engine_tracks": len(en_rows),
            "paired_tracks": len(paired),
            "engine_tracks_matching_several_rekordbox_tracks": ambiguous,
            "files": dict(sorted(kinds.items())),
            "tracks_with_section": dict(sorted(section_tracks.items())),
        },
        "paired": paired,
        "files": files,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seabass-cli", required=True, type=Path)
    ap.add_argument("--stick", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--name", default="real-analysis")
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    args = ap.parse_args()

    stick = args.stick.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    label = "%s-%s" % (args.name, args.date)
    staging = work / "anonymized"
    for leftover in (staging, work / "anonymized.zip", work / label):
        if leftover.is_dir():
            shutil.rmtree(leftover)
        elif leftover.exists():
            leftover.unlink()

    env = dict(os.environ)
    for var, sub in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                     ("SEABASS_HOME", "home")):
        env[var] = str(work / "cli-home" / sub)
    command = [str(args.seabass_cli), "anonymize", "--rekordbox", str(stick / "PIONEER"),
               "--engine", str(stick / "Engine Library"), "--keep-waveforms", "--out", str(staging)]
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    (work / "anonymize.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        print(result.stdout[-3000:] + result.stderr[-3000:], file=sys.stderr)
        sys.exit("anonymize failed (exit %d); see %s" % (result.returncode, work / "anonymize.log"))

    root = work / label
    with zipfile.ZipFile(work / "anonymized.zip") as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = info.filename.split("/")
            if parts[0] == "rekordbox":
                target = root.joinpath("PIONEER", *parts[1:])
            elif parts[0] == "engine":
                target = root.joinpath("Engine Library", *parts[1:])
            elif info.filename in ("MANIFEST.txt", "files.tsv"):
                target = root / ("anonymize-" + info.filename)
            else:
                sys.exit("unexpected entry in the anonymized export: %s" % info.filename)
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)

    for path in root.rglob("*"):
        if not path.is_file():
            continue
        head = path.read_bytes()[:16]
        if path.suffix.lower() in AUDIO_OR_IMAGE_EXTENSIONS or any(
                head[at:at + len(magic)] == magic for at, magic in AUDIO_OR_IMAGE_MAGIC):
            sys.exit("an audio or image file is in the fixture: %s" % path.relative_to(root))

    pioneer = root / "PIONEER"
    rb_tracks = pdb_tracks((pioneer / "rekordbox" / "export.pdb").read_bytes())
    by_filename = {}
    for track_id, filename, analyze_path in rb_tracks:
        by_filename.setdefault(filename, []).append((track_id, analyze_path))

    db_copy = work / "m.db.copy"
    shutil.copyfile(root / "Engine Library" / "Database2" / "m.db", db_copy)
    conn = sqlite3.connect("file:%s?mode=ro" % db_copy, uri=True)
    en_rows = conn.execute("SELECT id, filename FROM Track ORDER BY id").fetchall()
    paired = []
    ambiguous = 0
    for engine_id, filename in en_rows:
        matches = by_filename.get(filename or "", [])
        if len(matches) > 1:
            ambiguous += 1
            continue
        if not matches:
            continue
        rekordbox_id, analyze_path = matches[0]
        if not analyze_path or not (pioneer / analyze_path.lstrip("/").split("/", 1)[-1]).is_file():
            continue
        paired.append({
            "rekordbox_id": rekordbox_id,
            "engine_id": engine_id,
            "rekordbox": rekordbox_counts(pioneer, analyze_path),
            "engine": engine_counts(conn, engine_id),
        })
    conn.close()
    db_copy.unlink()

    version, commit = repo_version(), repo_commit()
    (root / "README.md").write_text(README.format(
        name=args.name, date=args.date, rb_tracks=len(rb_tracks), en_tracks=len(en_rows), paired=len(paired),
        version=version, commit=commit))
    manifest = build_manifest(root, args.name, args.date, rb_tracks, en_rows, paired, ambiguous, version, commit)
    (root / "MANIFEST.json").write_text(json.dumps(manifest, indent=1, sort_keys=False) + "\n")

    out = work / "out"
    out.mkdir(exist_ok=True)
    zip_path = out / (label + ".zip")
    if zip_path.exists():
        zip_path.unlink()
    stamp = tuple(int(x) for x in args.date.split("-")) + (0, 0, 0)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        entries = sorted(p for p in root.rglob("*") if p.is_file())
        for path in entries:
            info = zipfile.ZipInfo(label + "/" + path.relative_to(root).as_posix(), date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)
    digest = sha256(zip_path)
    (out / (label + ".zip.sha256")).write_text("%s  %s\n" % (digest, zip_path.name))

    raw = sum(p.stat().st_size for p in root.rglob("*") if p.is_file())
    print("fixture: %s (%d files, %.1f MB raw)" % (root, len(manifest["files"]) + 1, raw / 1e6))
    print("zip: %s (%.1f MB), sha256 %s" % (zip_path, zip_path.stat().st_size / 1e6, digest))
    print("tracks: rekordbox %d, Engine %d, paired %d (%d Engine rows match several rekordbox rows)" % (
        len(rb_tracks), len(en_rows), len(paired), ambiguous))


if __name__ == "__main__":
    main()
