#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Checks that a fixture made from a real stick holds none of its text.

    tools/check-fixture-deidentified.py --source <stick> --candidate <dir or zip>
        [--extra TEXT]... [--allow FILE:TEXT]... [--min-length 4] [--scratch DIR] [--max-report 40]

<stick> is the real, private stick the candidate was made from: the
directory holding PIONEER/ and/or "Engine Library/". The candidate is the
de-identified tree or its zip. Nothing under either is written; databases
are copied into a scratch directory before they are opened.

Every identifying string is collected from the SOURCE:

  * export.pdb and exportExt.pdb: every DeviceSQL string in the file,
    live rows and the leftovers in free space alike (titles, artists,
    albums, genres, labels, comments, file names, paths, playlists, My
    Tags, ISRCs, mix names);
  * exportLibrary.db (decrypted the way the app does), m.db, hm.db, sm.db,
    stm.db: every text value of every column that is not a format field
    (see STRUCTURAL_COLUMNS), and the cue labels inside Engine's
    compressed quickCues and loops blobs;
  * every ANLZ file: the PATH section and the cue comments;
  * the stick's own file tree: every path component, file name and stem;
  * the user name and full name of whoever runs this, and every --extra.

Then every file of the CANDIDATE is searched, case-insensitively, for
each of them encoded as UTF-8, UTF-16LE and UTF-16BE: the raw bytes, every
string of a pdb on its own, every text and blob value of each SQLite
database (decrypted first if it is the OneLibrary mirror), the
zlib-compressed Engine blobs inflated, and the candidate's own file names.

The threshold. A string of --min-length characters (4) or more that is
not format vocabulary fails the check wherever it is found. Shorter
strings, and very common words, are still searched for (two-character
ones are not: they are everywhere), but their hits are only counted and
listed apart: "Sun" or "Ice" turn up by chance in any few megabytes of
binary data, and a check that fails on them would be switched off rather
than read. Four characters is where chance mostly stops: a given
four-letter string, any case, turns up about once per 270 MB of random
bytes, and a fixture is tens of MB.

Waveform bytes are not random, though: amplitudes and colours one byte
per column spell names now and then. The sections of an analysis file
whose layout has no text field (waveforms, grids, the VBR index, the song
structure) are therefore searched as a view of their own, and a hit
there is listed apart. So are hits in SQLite schema pages, which pack
table and index names with no gap between them ("indexindex_Track..."
holds "Dexi"). In files of text for people (README, manifests) a hit
must be a whole word: "Over" is not found in "covered". Elsewhere a hit
inside a longer word counts, unless that word is format vocabulary or an
anonymizer placeholder ("artwork/removed.jpg").

Format vocabulary is what rekordbox and Engine write into every library
(menu names, colour names, keys, "HISTORY 001", analysis-file paths under
/PIONEER/, schema words of the source's databases): listed apart as well.
Numbers, dates, UUIDs and hex digests are not text and are not searched,
except a streaming service's id for a recording, which names it as a
title does.

--allow FILE:TEXT moves a hit of TEXT in candidate file FILE to its own
list: for a word of the anonymizer's own MANIFEST.txt prose that is also
a name in the library. Each one is printed; read it before allowing it.

Exit status: 0 when nothing identifying was found and every file could
be read; 1 when a hit was found or a file could not be checked (an
encrypted database that does not open is a failure, not a pass); 2 on a
usage error.
"""

import argparse
import base64
import ctypes
import ctypes.util
import getpass
import os
import pwd
import re
import shutil
import sqlite3
import sys
import tempfile
import zipfile
import zlib
from collections import defaultdict
from pathlib import Path

SQLITE_MAGIC = b"SQLite format 3\x00"
NUMERIC_VIEW = "numeric ANLZ sections"
PROSE_VIEW = "text"
SCHEMA_VIEW = "SQLite schema pages"

# The OneLibrary key, derived exactly as src/infrastructure/onelibrary/
# onelibrary_key.cpp does it (the same constants pyrekordbox publishes).
ONELIBRARY_BLOB = "PN_1dH8$oLJY)16j_RvM6qphWw`476>;C1cWmI#se(PG`j}~xAjlufj?`#0i{;=glh(SkW)y0>n?YEiD`l%t("
ONELIBRARY_XOR = b"657f48f84c437cc1"

# Columns whose values the formats themselves fill in: never a person's
# text. Everything else in a source database counts as identifying.
STRUCTURAL_COLUMNS = {
    # Engine m.db and its siblings
    ("Information", "uuid"),
    ("Track", "fileType"),
    ("Track", "streamingSource"),
    ("Track", "originDatabaseUuid"),
    ("Track", "dateCreated"),
    ("Track", "dateAdded"),
    ("Track", "timeLastPlayed"),
    ("Track", "lastEditTime"),
    ("PlaylistEntity", "databaseUuid"),
    ("Pack", "packId"),
    ("Pack", "changeLogDatabaseUuid"),
    ("Smartlist", "listUuid"),
    ("Smartlist", "nextListUuid"),
    ("sqlite_sequence", "name"),
    # OneLibrary (exportLibrary.db)
    ("content", "releaseDate"),
    ("content", "dateCreated"),
    ("content", "dateAdded"),
    ("content", "analysisDataFilePath"),
    ("content", "cueUpdateCount"),
    ("key", "name"),
    ("color", "name"),
    ("menuItem", "name"),
    ("property", "dbVersion"),
    ("property", "createdDate"),
    ("image", "path"),
}

# The blobs that hold text a person typed: Engine's cue and loop labels.
TEXT_BLOB_COLUMNS = {("PerformanceData", "quickCues"), ("PerformanceData", "loops")}

# Written by rekordbox or Engine into every library, or by the anonymizer
# itself. Found in a candidate, they are reported apart, never failed on.
FORMAT_VOCABULARY = {
    "genre", "artist", "album", "track", "bpm", "rating", "year", "remixer", "label",
    "original artist", "key", "cue", "color", "time", "bitrate", "file name", "playlist",
    "hot cue bank", "history", "search", "comments", "date added", "dj play count", "folder",
    "default", "alphabet", "matching", "pink", "red", "orange", "yellow", "green", "aqua",
    "blue", "purple", "comment", "contents", "pioneer", "rekordbox", "engine library",
    "database2", "usbanlz", "artwork", "tidal", "mp3", "m4a", "mp4", "flac", "wav", "aiff",
    # The files and directories every stick has.
    "export.pdb", "export", "exportext.pdb", "exportext", "exportlibrary.db", "exportlibrary",
    "playlists3.sync", "playlists3", "playlists3plus.sync", "playlists3plus", "rbfltr.dat", "rbfltr",
    "m.db", "hm.db", "sm.db", "stm.db", "overviewdata", "pioneer/rekordbox", "engine", "library",
    # Engine's cover links: "image://fileart//media/<label>/PIONEER/...",
    # and its streaming ones: "streaming://TIDAL/Track/<id>".
    "image:", "fileart", "media", "streaming", "streaming:", "images",
}
FORMAT_PATTERNS = [
    re.compile(r"^￺.*￻$"),          # rekordbox menu item names
    re.compile(r"^HISTORY \d+$"),
    re.compile(r"^/PIONEER/(USBANLZ|Artwork)/"),
    re.compile(r"^ANLZ\d{4}(\.(DAT|EXT|2EX))?$"),
    re.compile(r"^P[0-9A-F]{3}$"),               # USBANLZ directory names
    re.compile(r"^a\d+(_m)?(\.jpg)?$"),          # rekordbox artwork files
    re.compile(r"^(Cue|Loop|Artist|Album|Genre|Label|Playlist|Tag|Track|Comment) ?\d*$", re.I),
    re.compile(r"^streaming://"),            # the scheme only; see split_streaming_uri
]

# Words the anonymizer writes in place of what it removes. A short title
# found inside one of them ("Emo" in "removed") is the placeholder, and
# is reported with the format hits.
PLACEHOLDER_WORDS = {"removed", "track", "artist", "album", "genre", "label", "comment", "playlist",
                     "contents", "tag", "cue", "loop", "history"}

# Words common enough that a hit says nothing about the library.
COMMON_WORDS = {
    "the", "and", "for", "with", "from", "this", "that", "original", "mix", "remix", "edit",
    "extended", "radio", "version", "club", "dub", "feat", "featuring", "vocal", "instrumental",
    "intro", "outro", "love", "life", "time", "night", "music", "dance", "house", "techno",
    "deep", "unknown", "none", "true", "false", "null", "data", "file", "files", "audio",
}

NOT_TEXT = [
    re.compile(r"^[\d\s.,:;/+\-_()\[\]#]*$"),                       # numbers, dates, times
    re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", re.I),
    re.compile(r"^[0-9a-f]{8,}$", re.I),                              # digests, ANLZ directories
    re.compile(r"^[0-9a-f]{8,}\.rgb$", re.I),
    re.compile(r"^\d+\.rgb$"),                                         # OverviewData files
]


def onelibrary_key():
    decoded = base64.b85decode(ONELIBRARY_BLOB)
    xored = bytes(b ^ ONELIBRARY_XOR[i % len(ONELIBRARY_XOR)] for i, b in enumerate(decoded))
    return zlib.decompress(xored).decode("ascii")


def decrypt_sqlcipher(path, plain_path):
    """Writes a plaintext copy of an SQLCipher database, or returns why not."""
    name = ctypes.util.find_library("sqlcipher") or "libsqlcipher.so.1"
    try:
        lib = ctypes.CDLL(name)
    except OSError as e:
        return "libsqlcipher could not be loaded: %s" % e
    lib.sqlite3_open_v2.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_int, ctypes.c_char_p]
    lib.sqlite3_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_char_p)]
    lib.sqlite3_close.argtypes = [ctypes.c_void_p]
    db = ctypes.c_void_p()
    # Read-write and create: ATTACH creates the plaintext copy with the
    # flags of the main connection. The file opened is a scratch copy.
    if lib.sqlite3_open_v2(str(path).encode(), ctypes.byref(db), 0x2 | 0x4, None) != 0:
        return "it could not be opened"
    error = ctypes.c_char_p()
    target = str(plain_path).replace("'", "''")
    sql = ("PRAGMA key = '%s'; ATTACH DATABASE '%s' AS plaintext KEY ''; "
           "SELECT sqlcipher_export('plaintext'); DETACH DATABASE plaintext;" % (onelibrary_key(), target))
    rc = lib.sqlite3_exec(db, sql.encode(), None, None, ctypes.byref(error))
    lib.sqlite3_close(db)
    if rc != 0:
        return "it did not decrypt with the OneLibrary key: %s" % (error.value or b"").decode(errors="replace")
    return None


class Scratch:
    def __init__(self, parent):
        self.dir = Path(tempfile.mkdtemp(prefix="fixture-check-", dir=parent))
        self.n = 0

    def path(self, suffix):
        self.n += 1
        return self.dir / ("%d%s" % (self.n, suffix))

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def sqlite_copy(path, scratch):
    """A copy of a database, plain or decrypted, or (None, why)."""
    copy = scratch.path(".db")
    shutil.copyfile(path, copy)
    with open(copy, "rb") as f:
        head = f.read(16)
    if head == SQLITE_MAGIC:
        return copy, None
    plain = scratch.path(".db")
    why = decrypt_sqlcipher(copy, plain)
    if why:
        return None, why
    return plain, None


def sqlite_values(db_path):
    """(table, column, value) for every text and blob value in every table."""
    conn = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    conn.text_factory = bytes
    try:
        tables = [r[0].decode() for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        for table in tables:
            columns = [r[1].decode() for r in conn.execute('PRAGMA table_info("%s")' % table)]
            for column in columns:
                query = 'SELECT "%s" FROM "%s" WHERE typeof("%s") IN (\'text\', \'blob\')' % (column, table, column)
                for (value,) in conn.execute(query):
                    yield table, column, bytes(value)
    finally:
        conn.close()


def engine_cue_labels(column, blob):
    """The labels of Engine's hot cues (quickCues: zlib, big-endian) or
    loops (loops: plain, little-endian), as libdjinterop lays them out."""
    data = inflate_engine_blob(blob) if column == "quickCues" else blob
    if not data or len(data) < 8:
        return []
    big = column == "quickCues"
    count = int.from_bytes(data[:8], "big" if big else "little", signed=True)
    after_label = 12 if big else 22
    labels = []
    at = 8
    for _ in range(max(0, min(count, 64))):
        if at >= len(data):
            break
        size = data[at]
        labels.append(data[at + 1:at + 1 + size].decode("utf-8", errors="replace"))
        at += 1 + size + after_label
    return [label for label in labels if label]


def sqlite_schema_words(db_path):
    """Every word of a database's schema: its table and column names and
    the SQL keywords of its triggers. A title that is one of them ("Index",
    "Create") cannot be told from the schema and is reported apart."""
    conn = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        words = set()
        for (sql,) in conn.execute("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL"):
            words.update(re.findall(r"[A-Za-z_][A-Za-z_0-9]*", sql))
        return words
    finally:
        conn.close()


def inflate_engine_blob(blob):
    """Engine's blobs: a 4-byte big-endian length, then a zlib stream."""
    for start in (4, 0):
        try:
            return zlib.decompress(blob[start:])
        except zlib.error:
            continue
    return None


# --- source side -----------------------------------------------------------

def device_sql_string_at(data, at):
    """The DeviceSQL string at `at`, or None when there is none there."""
    if at < 0 or at >= len(data):
        return None
    kind = data[at]
    if kind in (0x40, 0x90):
        if at + 4 > len(data):
            return None
        length = data[at + 1] | (data[at + 2] << 8)
        if length < 4 or at + length > len(data):
            return None
        body = data[at + 4:at + length]
        if kind == 0x90 and len(body) % 2 == 0:
            try:
                return body.decode("utf-16-le").strip("\x00\x03 ")
            except UnicodeDecodeError:
                pass
        # ISRCs are 0x90 strings holding ASCII behind a 0x03 byte.
        return body.decode("latin-1").strip("\x00\x03 ")
    if kind & 1:
        length = kind >> 1
        if length < 1 or at + length > len(data):
            return None
        return data[at + 1:at + length].decode("latin-1").strip("\x00 ")
    return None


# Page types of export.pdb (and of exportExt.pdb, where 3 holds the tags),
# and where each row keeps its strings: (fixed offset of the string) or a
# callable giving every string offset of the row.
def _u1(d, a):
    return d[a]


def _u2(d, a):
    return int.from_bytes(d[a:a + 2], "little")


def _near_far(near_at, far_at):
    def offsets(d, row):
        subtype = _u2(d, row)
        return [row + (_u2(d, row + far_at) if subtype & 0x04 else _u1(d, row + near_at))]
    return offsets


PDB_ROWS = {
    0: lambda d, row: [row + _u2(d, row + 0x5E + 2 * i) for i in range(21)],   # tracks
    1: lambda d, row: [row + 4],                                                # genres
    2: _near_far(0x09, 0x0A),                                                   # artists
    3: _near_far(0x15, 0x16),                                                   # albums
    4: lambda d, row: [row + 4],                                                # labels
    5: lambda d, row: [row + 8],                                                # keys
    6: lambda d, row: [row + 8],                                                # colours
    7: lambda d, row: [row + 0x14],                                             # playlist tree
    11: lambda d, row: [row + 4],                                               # history playlists
    13: lambda d, row: [row + 4],                                               # artwork
}
PDB_EXT_ROWS = {3: _near_far(0x1D, 0x1E)}                                       # My Tags


def pdb_strings(data, ext):
    """Every string of every row a page indexes, present or deleted, and
    every long-form string anywhere else in the file (free space)."""
    found = set()
    if len(data) < 16:
        return found
    page_len = int.from_bytes(data[4:8], "little")
    if page_len < 0x100:
        return found
    layouts = PDB_EXT_ROWS if ext else PDB_ROWS
    for page in range(page_len, len(data) - page_len + 1, page_len):
        ptype = int.from_bytes(data[page + 8:page + 12], "little")
        flags = data[page + 0x1B]
        if flags & 0x40 or ptype not in layouts:
            continue
        bits = int.from_bytes(data[page + 0x18:page + 0x1B], "little")
        offsets_count = bits & 0x1FFF
        for index in range(min(offsets_count, 0x1FFF)):
            group, r = divmod(index, 16)
            slot = page + page_len - group * 0x24 - (6 + 2 * r)
            if slot < page + 0x28:
                break
            row = page + 0x28 + _u2(data, slot)
            if row >= page + page_len:
                continue
            try:
                for at in layouts[ptype](data, row):
                    text = device_sql_string_at(data, at)
                    if text:
                        found.add(text)
            except IndexError:
                continue
    # Long-form strings anywhere: a renamed row leaves its old text in
    # the heap where no index points at it any more.
    for m in re.finditer(rb"[\x40\x90]", data):
        at = m.start()
        if at + 4 > len(data) or data[at + 3] != 0:
            continue
        text = device_sql_string_at(data, at)
        if text and len(text) >= 4 and text.isprintable():
            found.add(text)
    return found


def anlz_text(data):
    """The PATH of one ANLZ file and the comments of its extended cues."""
    out = set()
    if len(data) < 12 or data[:4] != b"PMAI":
        return out
    at = int.from_bytes(data[4:8], "big")
    while at + 12 <= len(data):
        tag = data[at:at + 4]
        header = int.from_bytes(data[at + 4:at + 8], "big")
        length = int.from_bytes(data[at + 8:at + 12], "big")
        if length < 12 or at + length > len(data):
            break
        section = data[at:at + length]
        if tag == b"PPTH" and len(section) >= 16:
            size = int.from_bytes(section[12:16], "big")
            out.add(section[header:header + size].decode("utf-16-be", errors="replace"))
        elif tag == b"PCO2" and len(section) >= 20:
            count = int.from_bytes(section[16:18], "big")
            entry = header
            for _ in range(count):
                if entry + 44 > len(section):
                    break
                entry_length = int.from_bytes(section[entry + 8:entry + 12], "big")
                if entry_length > 43:
                    size = int.from_bytes(section[entry + 40:entry + 44], "big")
                    out.add(section[entry + 44:entry + 44 + size].decode("utf-16-be", errors="replace"))
                if entry_length < 44:
                    break
                entry += entry_length
        at += length
    return {t.strip("\x00 ") for t in out if t.strip("\x00 ")}


def path_strings(text):
    """A path, every component of it, and each file name without its extension."""
    out = {text}
    for part in re.split(r"[\\/]", text):
        if part:
            out.add(part)
            stem, dot, _ = part.rpartition(".")
            if dot and stem:
                out.add(stem)
    return out


def split_streaming_uri(text):
    """streaming://TIDAL/Track/123456789: the id is what names the recording."""
    m = re.match(r"^streaming://[^/]+/[^/]+/(.+)$", text)
    return {m.group(1)} if m else set()


def collect_source(source, scratch, extras):
    """{string: set of origins}"""
    strings = defaultdict(set)

    def add_path(text, origin):
        # The parts of a path the format itself writes (an artwork file
        # under /PIONEER/Artwork) are not split: "a273" says nothing.
        if is_format(text):
            add(text, origin)
            return
        for part in path_strings(text):
            add(part, origin)

    def add(text, origin):
        if isinstance(text, bytes):
            text = text.decode("utf-8", errors="replace")
        text = text.strip().strip("\x00").strip()
        if text:
            strings[text].add(origin)

    unreadable = []
    schema_words = set()
    for path in sorted(source.rglob("*")):
        rel = path.relative_to(source).as_posix()
        add_path(rel, "source file name")
        if not path.is_file():
            continue
        name = path.name
        with open(path, "rb") as f:
            head = f.read(16)
        if name.lower().endswith(".pdb"):
            for s in pdb_strings(path.read_bytes(), name.lower().startswith("exportext")):
                add(s, name)
                if "/" in s:
                    add_path(s, name + " path")
        elif head[:4] == b"PMAI":
            for s in anlz_text(path.read_bytes()):
                add_path(s, "ANLZ PATH or cue comment")
        elif head == SQLITE_MAGIC or name.endswith(".db"):
            db, why = sqlite_copy(path, scratch)
            if db is None:
                unreadable.append("%s: %s" % (rel, why))
                continue
            schema_words.update(sqlite_schema_words(db))
            for table, column, value in sqlite_values(db):
                origin = "%s %s.%s" % (name, table, column)
                if (table, column) in TEXT_BLOB_COLUMNS:
                    for label in engine_cue_labels(column, value):
                        add(label, origin)
                    continue
                if (table, column) in STRUCTURAL_COLUMNS:
                    continue
                try:
                    text = value.decode("utf-8")
                except UnicodeDecodeError:
                    continue  # a binary blob, not text
                if not text.isprintable():
                    continue
                add(text, origin)
                for s in split_streaming_uri(text):
                    add(s, origin + " (streaming id)")
                    STREAMING_IDS.add(s.strip())
                if "/" in text:
                    add_path(text, origin + " path")
    try:
        user = getpass.getuser()
        add(user, "user name")
        for part in re.split(r"[ ,]+", pwd.getpwnam(user).pw_gecos):
            add(part, "user full name")
        add(str(Path.home()), "home directory")
    except (KeyError, OSError):
        pass
    for e in extras:
        add(e, "--extra")
    FORMAT_VOCABULARY.update(w.lower() for w in schema_words)
    return strings, unreadable


def is_format(text):
    if text.lower() in FORMAT_VOCABULARY or any(p.match(text) for p in FORMAT_PATTERNS):
        return True
    # A path made of nothing but format names: PIONEER/USBANLZ/P03B.
    parts = [p for p in re.split(r"[\\/]", text) if p]
    return len(parts) > 1 and all(is_format(p) or any(n.match(p) for n in NOT_TEXT) for p in parts)


STREAMING_IDS = set()


def classify(text, min_length):
    if text in STREAMING_IDS:
        # A streaming service's id names one recording, as a title does.
        return "identifying"
    if any(p.match(text) for p in NOT_TEXT):
        return "not text"
    low = text.lower()
    if is_format(text):
        return "format"
    if len(text) < min_length:
        return "short"
    if low in COMMON_WORDS:
        return "common"
    return "identifying"


# --- candidate side --------------------------------------------------------

class Matcher:
    """Finds many needles in bytes at once: a trie compiled to one regex.

    Python's re has no multi-pattern search, and an alternation of
    thousands of literals is tried one by one at every position. A trie
    turns that into one branch per byte. Only a prefix of each needle goes
    into the regex, which keeps it shallow; the rest is compared at the
    hit."""

    PREFIX = 24

    def __init__(self, needles):
        self.full = defaultdict(list)  # lowered prefix -> [(lowered needle, needle text, codec)]
        trie = {}
        for encoded, text, codec in needles:
            low = encoded.lower()
            prefix = low[:self.PREFIX]
            self.full[prefix].append((low, text, codec))
            node = trie
            for b in prefix:
                node = node.setdefault(b, {})
            node[None] = True
        self.regex = re.compile(b"(?=(" + self._pattern(trie) + b"))", re.S) if trie else None

    def _pattern(self, node):
        ends = None in node
        branches = [re.escape(bytes([b])) + self._pattern(child) for b, child in sorted(
            (k, v) for k, v in node.items() if k is not None)]
        if not branches:
            return b""
        body = branches[0] if len(branches) == 1 else b"(?:" + b"|".join(branches) + b")"
        if ends:
            body = b"(?:" + body + b")?"
        return body

    def find(self, data):
        """(start, end, text, codec) of every needle in data."""
        if self.regex is None:
            return
        low = data.lower()
        seen = set()
        for m in self.regex.finditer(low):
            start = m.start()
            match = m.group(1)
            # Every needle whose prefix is a prefix of what matched here.
            for k in range(1, len(match) + 1):
                for needle, text, codec in self.full.get(match[:k], ()):
                    if (start, text) in seen or not low.startswith(needle, start):
                        continue
                    seen.add((start, text))
                    yield start, start + len(needle), text, codec


def unit_at(data, at, codec):
    """The ASCII letter or digit at byte `at`, lowered, or None."""
    if codec == "utf-8":
        c = chr(data[at]) if 0 <= at < len(data) else ""
    elif at < 0 or at + 2 > len(data):
        c = ""
    else:
        lo, hi = (data[at], data[at + 1]) if codec == "utf-16-le" else (data[at + 1], data[at])
        c = chr(lo) if hi == 0 else ""
    return c.lower() if c.isascii() and c.isalnum() else None


def enclosing_word(data, start, end, codec, text):
    """The run of ASCII letters and digits a hit sits inside, when it is
    the inside of a longer one ("over" in "covered"), else None."""
    width = 1 if codec == "utf-8" else 2
    before, after = "", ""
    if text[0].isalnum():
        at = start - width
        while unit_at(data, at, codec):
            before = unit_at(data, at, codec) + before
            at -= width
    if text[-1].isalnum():
        at = end
        while unit_at(data, at, codec):
            after += unit_at(data, at, codec)
            at += width
    if not before and not after:
        return None
    return before + text.lower() + after


def encodings(text):
    out = set()
    for variant in {text, text.lower(), text.upper()}:
        for codec in ("utf-8", "utf-16-le", "utf-16-be"):
            try:
                out.add((variant.encode(codec), codec))
            except UnicodeEncodeError:
                pass
    return out


def candidate_files(candidate, scratch):
    if candidate.is_file() and zipfile.is_zipfile(candidate):
        root = scratch.dir / "candidate"
        with zipfile.ZipFile(candidate) as z:
            for info in z.infolist():
                target = (root / info.filename).resolve()
                if not str(target).startswith(str(root.resolve())):
                    raise SystemExit("refusing a zip entry outside its root: %s" % info.filename)
            z.extractall(root)
        return root
    return candidate


# ANLZ sections whose layout has no text field: waveforms, beat grids,
# the VBR index and the (masked) song structure. Their bytes are numbers,
# and a few megabytes of numbers spell a given four-letter word by chance
# (it happened: two artist names, in two .EXT files). They are searched
# as their own view, and what is found there is counted apart.
NUMERIC_ANLZ_SECTIONS = {b"PWAV", b"PWV2", b"PWV3", b"PWV4", b"PWV5", b"PWV6", b"PWV7", b"PWVC",
                         b"PQTZ", b"PQT2", b"PVBR", b"PSSI"}


def split_anlz(data):
    """(file with its numeric section bodies blanked, those bodies), or
    (data, b"") when it is not an ANLZ file."""
    if len(data) < 12 or data[:4] != b"PMAI":
        return data, b""
    text = bytearray(data)
    numeric = bytearray()
    at = int.from_bytes(data[4:8], "big")
    while at + 12 <= len(data):
        header = int.from_bytes(data[at + 4:at + 8], "big")
        length = int.from_bytes(data[at + 8:at + 12], "big")
        if length < 12 or header > length or at + length > len(data):
            break
        if data[at:at + 4] in NUMERIC_ANLZ_SECTIONS:
            numeric += data[at + header:at + length] + b"\x00"
            text[at + header:at + length] = bytes(length - header)
        at += length
    return bytes(text), bytes(numeric)


def is_prose(data):
    """A file that is text for people (a README, a manifest, a listing)."""
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return data[:16] != SQLITE_MAGIC


def schema_pages(db_path):
    """The page numbers that hold a database's schema, from dbstat, or
    None where this SQLite has no dbstat."""
    conn = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        return {r[0] for r in conn.execute(
            "SELECT pageno FROM dbstat WHERE name IN ('sqlite_schema', 'sqlite_master')")}, \
            conn.execute("PRAGMA page_size").fetchone()[0]
    except sqlite3.Error:
        return None, None
    finally:
        conn.close()


def without_pages(data, pages, page_size):
    out = bytearray(data)
    for page in pages:
        start = (page - 1) * page_size
        out[start:start + page_size] = bytes(len(out[start:start + page_size]))
    return bytes(out)


def views(path, rel, scratch, problems):
    """(label, bytes) pairs to search for one candidate file."""
    data = path.read_bytes()
    text, numeric = split_anlz(data)
    db = None
    if data[:16] == SQLITE_MAGIC or path.suffix == ".db":
        db, why = sqlite_copy(path, scratch)
        if db is None:
            problems.append("%s could not be checked by content: %s" % (rel, why))
    if db is not None:
        # The schema pages apart: a schema packs its names with no gap
        # ("indexindex_Track_artist"), and a title like "Dexi" turns up in
        # it. Nothing a person typed is in them.
        plain = Path(db).read_bytes()
        pages, page_size = schema_pages(db)
        if pages:
            yield SCHEMA_VIEW, b"\x00".join(plain[(p - 1) * page_size:p * page_size] for p in sorted(pages))
            plain = without_pages(plain, pages, page_size)
        yield ("raw bytes" if data[:16] == SQLITE_MAGIC else "decrypted database file"), plain
    else:
        yield (PROSE_VIEW if is_prose(data) else "raw bytes"), text
    if path.suffix.lower() == ".pdb":
        # Each string on its own as well: in the raw page a string sits
        # right after its length byte, and that can be a letter.
        strings = pdb_strings(data, path.name.lower().startswith("exportext"))
        yield "pdb strings", "\x00".join(sorted(strings)).encode("utf-8", errors="replace")
    if numeric:
        yield NUMERIC_VIEW, numeric
    if db is None:
        return
    for table, column, value in sqlite_values(db):
        label = "%s.%s" % (table, column)
        yield label, value
        inflated = inflate_engine_blob(value) if len(value) > 4 else None
        if inflated:
            yield label + " (inflated)", inflated


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, type=Path, help="the private stick the candidate was made from")
    ap.add_argument("--candidate", required=True, type=Path, help="the de-identified tree, or its zip")
    ap.add_argument("--extra", action="append", default=[], help="another string to look for (volume names)")
    ap.add_argument("--allow", action="append", default=[], metavar="FILE:TEXT",
                    help="a hit of TEXT in candidate file FILE (path inside the candidate) that a person has "
                         "read and found harmless, such as a word of the anonymizer's own MANIFEST.txt; "
                         "listed apart, not failed on")
    ap.add_argument("--min-length", type=int, default=4)
    ap.add_argument("--scratch", type=Path, default=None, help="where copies are made (default: the temp dir)")
    ap.add_argument("--max-report", type=int, default=40, help="hits printed per class")
    args = ap.parse_args()
    if not args.source.is_dir() or not args.candidate.exists():
        print("--source must be a directory and --candidate must exist", file=sys.stderr)
        return 2

    scratch = Scratch(args.scratch)
    try:
        return run(args, scratch)
    finally:
        scratch.close()


def run(args, scratch):
    problems = []
    strings, unreadable = collect_source(args.source.resolve(), scratch, args.extra)
    for u in unreadable:
        problems.append("source " + u + " (its strings were not collected)")

    by_class = defaultdict(list)
    for text in strings:
        by_class[classify(text, args.min_length)].append(text)
    print("Source strings collected: %d (identifying %d, short %d, common %d, format %d, not text %d)" % (
        len(strings), len(by_class["identifying"]), len(by_class["short"]), len(by_class["common"]),
        len(by_class["format"]), len(by_class["not text"])))

    # Two characters are not searched at all: they match every few
    # kilobytes of any binary file, and no library is identified by one.
    needles = set()
    for cls in ("identifying", "short", "common", "format"):
        for text in by_class[cls]:
            if len(text) < 3:
                continue
            for encoded, codec in encodings(text):
                needles.add((encoded, text, codec))
    matcher = Matcher(needles)
    klass = {t: classify(t, args.min_length) for t in strings}

    allowed = defaultdict(set)
    for item in args.allow:
        where, _, text = item.partition(":")
        allowed[where].add(text)
    root = candidate_files(args.candidate.resolve(), scratch)
    hits = defaultdict(list)  # class -> [(file, view, offset, text)]
    files = sorted(p for p in root.rglob("*") if p.is_file())
    names = "\n".join(p.relative_to(root).as_posix() for p in sorted(root.rglob("*"))).encode()
    for start, end, text, codec in matcher.find(names):
        if enclosing_word(names, start, end, codec, text) is None:
            hits[klass[text]].append(("(file names)", "paths", start, text))
    swept = 0
    for path in files:
        rel = path.relative_to(root).as_posix()
        for label, data in views(path, rel, scratch, problems):
            swept += len(data)
            for offset, end, text, codec in matcher.find(data):
                cls = klass[text]
                word = enclosing_word(data, offset, end, codec, text) if cls == "identifying" else None
                if word is not None and label == PROSE_VIEW:
                    # Prose is made of words: "Over" is not in "covered".
                    continue
                if word is not None and (word in FORMAT_VOCABULARY or word in PLACEHOLDER_WORDS):
                    # Inside a format or placeholder word of the candidate's
                    # own: "artwork/removed.jpg" holds no title "Emo".
                    cls = "format"
                elif cls == "identifying" and label == NUMERIC_VIEW:
                    cls = "numeric"
                elif cls == "identifying" and label == SCHEMA_VIEW:
                    cls = "schema"
                elif cls == "identifying" and any(
                        text in texts and (where in ("*", rel) or rel.endswith("/" + where))
                        for where, texts in allowed.items()):
                    cls = "allowed"
                hits[cls].append((rel, label, offset, text))

    print("Candidate files searched: %d (%.1f MB across every view)" % (len(files), swept / 1e6))
    for cls in ("short", "common", "format", "numeric", "schema", "allowed"):
        found = hits[cls]
        distinct = sorted({h[3] for h in found})
        print("%s hits, not failing: %d (%d distinct)%s" % (
            cls.capitalize(), len(found), len(distinct),
            (": " + ", ".join(repr(d) for d in distinct[:args.max_report])) if distinct else ""))
        if cls in ("numeric", "schema", "allowed"):
            for rel, label, offset, text in found[:args.max_report]:
                print("      %r in %s [%s] at offset %d" % (text, rel, label, offset))
    found = hits["identifying"]
    print("IDENTIFYING hits: %d (%d distinct strings)" % (len(found), len({h[3] for h in found})))
    grouped = defaultdict(list)
    for hit in found:
        grouped[hit[3]].append(hit)
    for text in sorted(grouped, key=lambda t: (-len(grouped[t]), t))[:args.max_report]:
        where = grouped[text]
        print("  %r: %d hit(s), from %s" % (text, len(where), ", ".join(sorted(strings[text]))))
        for rel, label, offset, _ in where[:3]:
            print("      %s [%s] at offset %d" % (rel, label, offset))
    if len(grouped) > args.max_report:
        print("  ... and %d more strings" % (len(grouped) - args.max_report))
    for p in problems:
        print("NOT CHECKED: " + p)
    if found or problems:
        print("FAIL")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
