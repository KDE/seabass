#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Plant the damage that can only exist in a catalog, on an exported stick.

make_test_library.py can damage files; it cannot damage databases, because the
databases do not exist until rekordbox has exported the playlist and
make_engine_library has built the Engine side from it. This is the step after
that, and it is the one that reproduces the defects that actually cost this
project debugging time -- every count below was measured on a real stick.

Never run against a stick you care about. It writes, on purpose, and what it
writes is broken on purpose.
"""

import argparse
import os
import shutil
import sqlite3
import sys
import unicodedata


def engine_db(stick):
    """The Engine database on a stick, wherever the stick root was given."""
    for candidate in (
        os.path.join(stick, "Engine Library", "Database2", "m.db"),
        os.path.join(stick, "Database2", "m.db"),
        stick,
    ):
        if os.path.isfile(candidate):
            return candidate
    sys.exit(f"no Engine database under {stick}")


def plant(db_path, limit_per_defect=5, dry_run=False):
    planted = []
    connection = sqlite3.connect(db_path)
    connection.isolation_level = None
    cursor = connection.cursor()

    # Eight defects take limit_per_defect rows each and one takes a single
    # row; the spare is what the NFD case draws from, since it can only use a
    # path with something to decompose and may find none.
    needed = limit_per_defect * 8 + 1
    tracks = cursor.execute(
        "select id, filename from Track where filename is not null order by id limit ?",
        (needed + limit_per_defect * 2,),
    ).fetchall()
    if len(tracks) < needed:
        sys.exit(
            f"{db_path} has too few tracks ({len(tracks)}) to plant against: "
            f"{needed} needed at --count {limit_per_defect}. Use a smaller --count."
        )

    def take(count):
        # Never silently plant fewer than asked: a defect that quietly did
        # not get planted is a test that quietly passes.
        if len(tracks) < count:
            sys.exit(f"ran out of tracks with {len(tracks)} left, needed {count}")
        return [tracks.pop(0) for _ in range(count)]

    # 1. Sample rate that will not decompress. The real stick throws 283 of
    #    these -- "Track data blob doesn't have expected decompressed length of
    #    28 bytes" -- and anything that divides by a rate, or turns a stored
    #    sample offset into a time, meets a zero.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute(
                "update PerformanceData set trackData = ? where trackId = ?",
                (sqlite3.Binary(b"\x00\x01\x02"), track_id),
            )
        planted.append(("unreadable-sample-rate", track_id, filename))

    # 2. A row pointing at a file that is not there. rekordbox shows these as
    #    exclamation marks; Seabass has to notice them without deleting
    #    anything.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute(
                "update Track set path = path || '.missing', filename = filename || '.missing' where id = ?",
                (track_id,),
            )
        planted.append(("dangling-row", track_id, filename))

    # 3. A streaming row: its path names a cache on some other machine, so it
    #    must never be treated as a local file. 187 of these on the real stick.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute(
                "update Track set streamingSource = 'TIDAL', path = ? where id = ?",
                (f"/Users/somebody/Library/Caches/TIDAL/{track_id}.mp4", track_id),
            )
        planted.append(("streaming-row", track_id, filename))

    # 4. Artwork a player cannot find: the row keeps its albumArtId, the art is
    #    gone. Engine's own image://fileart bug had 1174 of 1271 tracks in this
    #    state.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute("update Track set albumArtId = 999999 where id = ?", (track_id,))
        planted.append(("missing-artwork", track_id, filename))

    # 5. Two catalog rows for one file, under two spellings of its path.
    #
    #    Engine will not take the same path twice -- Track.path is UNIQUE, so
    #    a literal duplicate row is impossible and the schema says so. What is
    #    possible, and what happens in the wild, is the same file written under
    #    two path strings: export.pdb pads its paths with trailing spaces, and
    #    "/Contents/a.mp3   " and "/Contents/a.mp3" are one file and two keys.
    #    That is the case path_key_test calls "THE ONE THAT BIT", and it is the
    #    only form this defect can take here.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            columns = [row[1] for row in cursor.execute("PRAGMA table_info(Track)")]
            copied = [c for c in columns if c != "id"]
            # Engine also keys rows by where they came from
            # (originDatabaseUuid, originTrackId is UNIQUE), so the copy has to
            # arrive as a locally-added track rather than as the same import
            # twice.
            def column_expression(column):
                if column == "path":
                    return "path || ' '"
                if column in ("originTrackId", "originDatabaseUuid"):
                    return "NULL"
                return column

            selected = ", ".join(column_expression(c) for c in copied)
            cursor.execute(
                f"insert into Track ({','.join(copied)}) select {selected} from Track where id = ?",
                (track_id,),
            )
        planted.append(("same-file-two-paths", track_id, filename))

    # -- paths that CAN be recovered, and must never be reported missing ----
    #
    #    application::path_key exists to fold exactly these, and the header
    #    says which way the error runs: normalising too little is the
    #    dangerous direction, because two spellings of one file get different
    #    keys, the file reads as unreferenced, and unreferenced files are what
    #    Seabass offers to delete. Every case below is a spelling the catalog
    #    really produces, with the file itself untouched on disk.
    #
    #    These are the opposite of defect 2. A dangling row is a file that is
    #    gone; these are files that are right there, under a name written
    #    differently. Nothing here should ever appear in a "missing files"
    #    list, and a fixture that only carried genuinely-absent files would
    #    never catch a normalisation that quietly stopped working.

    # 6. Case. exFAT and NTFS are case-insensitive, so this is one file.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute("update Track set path = upper(path) where id = ?", (track_id,))
        planted.append(("repairable-case", track_id, filename))

    # 7. Backslashes, from a stick written on Windows and read anywhere else.
    #    std::filesystem only treats '\' as a separator on Windows, so leaving
    #    it to the platform makes the same stick compare differently by OS.
    for track_id, filename in take(limit_per_defect):
        if not dry_run:
            cursor.execute("update Track set path = replace(path, '/', '\\') where id = ?", (track_id,))
        planted.append(("repairable-backslashes", track_id, filename))

    # 8. A decomposed filename. macOS stores NFD on FAT and exFAT, so a stick
    #    restored there spells every accented track differently from the
    #    catalog that wrote it; before this was folded, Clean Up called those
    #    files unreferenced. Only plantable on a path that actually has
    #    something to decompose.
    accented = [
        (track_id, name)
        for track_id, name in tracks
        if any(ord(character) > 127 for character in (name or ""))
    ]
    planted_nfd = 0
    for track_id, filename in accented[:limit_per_defect]:
        row = cursor.execute("select path from Track where id = ?", (track_id,)).fetchone()
        if not row or not row[0]:
            continue
        decomposed = unicodedata.normalize("NFD", row[0])
        if decomposed == row[0]:
            continue  # already decomposed: nothing to plant
        if not dry_run:
            cursor.execute("update Track set path = ? where id = ?", (decomposed, track_id))
        tracks[:] = [entry for entry in tracks if entry[0] != track_id]
        planted.append(("repairable-nfd", track_id, filename))
        planted_nfd += 1
    if planted_nfd == 0:
        # Say so rather than quietly plant nothing. This defect needs a
        # path with a composed accent in it, and a catalog of hashed or
        # plain-ASCII filenames has none -- the anonymized fixture is
        # exactly that. A silently unplanted defect is a test that passes
        # for the wrong reason, which is the one outcome worth refusing.
        planted.append(("repairable-nfd", None, "NOT PLANTED: no path here has a composed accent to decompose"))

    # -- and one that genuinely cannot be recovered -------------------------
    #
    # 9. A path naming a directory rather than a file. It exists, so an
    #    existence check passes; it is not audio, so everything after that
    #    fails. Distinct from defect 2, where nothing is there at all, and
    #    from 6 to 8, where the file is real and only the spelling differs.
    for track_id, filename in take(1):
        if not dry_run:
            cursor.execute(
                "update Track set path = ?, filename = ? where id = ?",
                ("Contents", "Contents", track_id),
            )
        planted.append(("unrepairable-path-is-a-directory", track_id, filename))

    if not dry_run:
        connection.commit()
    connection.close()
    return planted


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stick", help="stick root, Engine Library folder, or m.db itself")
    parser.add_argument("--count", type=int, default=5, help="rows per defect (default 5)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--backup", action="store_true", help="copy the database beside itself first")
    args = parser.parse_args()

    db_path = engine_db(os.path.expanduser(args.stick))
    if args.backup and not args.dry_run:
        shutil.copy2(db_path, db_path + ".before-defects")
        print(f"backed up -> {db_path}.before-defects")

    planted = plant(db_path, args.count, args.dry_run)

    by_kind = {}
    for kind, track_id, filename in planted:
        by_kind.setdefault(kind, []).append((track_id, filename))
    for kind, rows in by_kind.items():
        print(f"{kind}: {len(rows)} row(s)")
        for track_id, filename in rows[:2]:
            print(f"    id={track_id} {filename[:60]}")
    if args.dry_run:
        print("\nnothing written -- dry run")


if __name__ == "__main__":
    main()
