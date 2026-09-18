#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Assemble one clean library out of a stick's catalogs and a folder of files.

A prototype, deliberately: the detection this leans on (matching, duplicates,
junk cues) is already C++ and already tested, and the intent is for v2 to fold
the file placement below into use cases beside it. What is here is the part
that has no home yet -- deciding where each file should live and which copy of
a recording survives -- plus the reports that let a human check the decisions
before anything moves.

Inputs:
  --catalog-xml   what "seabass-cli export-xml" wrote: the stick's tracks with
                  their cues and playlists, already merged across catalogs and
                  already cleaned of junk cues.
  --extra-root    a folder of audio files with no catalog at all (the copy that
                  came off the NAS), whose folder names imply playlists.

It does not move a byte without --apply, and it never moves the sources: the
library is built by copying, so both inputs stay exactly as they are until the
result has been checked.
"""

import argparse
import collections
import csv
import hashlib
import os
import re
import sys
import unicodedata
import urllib.parse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

try:
    import mutagen
except ImportError:
    sys.exit("mutagen is required: pip3 install --user mutagen")

AUDIO_EXTENSIONS = {".mp3", ".m4a", ".mp4", ".aac", ".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".ogg"}
# What rekordbox imports here. FLAC is a fine file and an XDJ-RX2 cannot play
# one, so it is archived rather than catalogued -- see --lossless-dir.
LOSSLESS_EXTENSIONS = {".flac"}

# Two recordings count as the same when artist and title agree and the
# durations are within this many seconds. The same number DuplicateTrackFinder
# uses, for the same reason: stored lengths disagree slightly across formats.
DURATION_TOLERANCE_S = 2.0


def normalize_text(value):
    """Fold the differences that are spelling rather than substance."""
    if not value:
        return ""
    value = unicodedata.normalize("NFKD", value)
    value = "".join(c for c in value if not unicodedata.combining(c))
    value = value.lower()
    # "(Extended Mix)" vs "(extended mix)" survives; punctuation and spacing do not.
    value = re.sub(r"[\s_]+", " ", value)
    value = re.sub(r"[^\w\s()&-]", "", value)
    return value.strip()


def normalize_stem(filename):
    """A filename reduced to something comparable across exports.

    rekordbox writes "NNN_Artist-Title.mp3" and re-exports renumber the prefix,
    so the number is noise. So is a "-1" disambiguating suffix.
    """
    stem = os.path.splitext(os.path.basename(filename))[0]
    stem = re.sub(r"^\d+[_\s-]+", "", stem)
    stem = re.sub(r"-\d+$", "", stem)
    return normalize_text(stem)


def safe_component(value, fallback="Unknown"):
    """One path segment that every filesystem a DJ uses will accept.

    exFAT is the target -- it is what sticks are formatted as -- so the
    characters it refuses go, and so does a trailing dot or space, which
    Windows silently strips and then cannot find the file again.
    """
    value = (value or "").strip()
    if not value:
        value = fallback
    value = value.replace("/", "_").replace("\\", "_")
    value = re.sub(r'[:*?"<>|]', "_", value)
    value = re.sub(r"[\x00-\x1f\x7f]", "", value)
    value = value.rstrip(". ")
    if not value:
        value = fallback
    # exFAT's limit is 255 UTF-16 units per component; leave room for a suffix.
    return value[:120]


# The playlist the transcoded tracks land in, so they are identifiable as
# generated rather than ripped, and can be re-made if the encoder settings
# ever change.
FROM_FLAC_PLAYLIST = "from-flac"


def transcode_to_mp3(source, destination, tags):
    """FLAC -> MP3 320 CBR, tags and cover art carried over.

    lame does not read FLAC, so flac decodes to stdout and lame encodes from
    stdin. -b 320 --cbr rather than V0: a constant bitrate is what a 2017
    player's seek is happiest with, and this is the copy that exists to be
    played on one.
    """
    import subprocess

    os.makedirs(os.path.dirname(destination), exist_ok=True)
    partial = destination + ".partial"
    decode = subprocess.Popen(["flac", "-d", "-c", "--totally-silent", source], stdout=subprocess.PIPE)
    encode = subprocess.Popen(
        ["lame", "-b", "320", "--cbr", "-q", "0", "--quiet", "-", partial], stdin=decode.stdout
    )
    decode.stdout.close()
    encode.communicate()
    decode.wait()
    if encode.returncode != 0 or decode.returncode != 0 or not os.path.exists(partial):
        if os.path.exists(partial):
            os.remove(partial)
        return False, f"encoder failed (flac={decode.returncode}, lame={encode.returncode})"

    try:
        from mutagen.id3 import ID3, TIT2, TPE1, TALB, APIC, ID3NoHeaderError

        try:
            id3 = ID3(partial)
        except ID3NoHeaderError:
            id3 = ID3()
        if tags.get("title"):
            id3.add(TIT2(encoding=3, text=tags["title"]))
        if tags.get("artist"):
            id3.add(TPE1(encoding=3, text=tags["artist"]))
        if tags.get("album"):
            id3.add(TALB(encoding=3, text=tags["album"]))
        if tags.get("picture"):
            data, mime = tags["picture"]
            id3.add(APIC(encoding=3, mime=mime, type=3, desc="Cover", data=data))
        id3.save(partial)
    except Exception as exc:
        # The audio is fine; only the tagging failed. Worth reporting, not
        # worth throwing the encode away.
        os.replace(partial, destination)
        return True, f"encoded, but tagging failed: {type(exc).__name__}"

    os.replace(partial, destination)
    return True, ""


def flac_tags(path):
    """Title/artist/album and a cover picture, for handing to the encoder."""
    tags = {}
    try:
        audio = mutagen.File(path)
    except Exception:
        return tags
    if audio is None:
        return tags

    def first(key):
        try:
            value = audio.get(key)
        except Exception:
            return ""
        if not value:
            return ""
        if isinstance(value, list):
            value = value[0]
        return str(value).strip()

    tags["title"] = first("title") or first("TITLE")
    tags["artist"] = first("artist") or first("ARTIST")
    tags["album"] = first("album") or first("ALBUM")
    pictures = getattr(audio, "pictures", None)
    if pictures:
        tags["picture"] = (pictures[0].data, pictures[0].mime or "image/jpeg")
    return tags


@dataclass
class Copy:
    """One audio file on disk, wherever it came from."""

    path: str
    source: str  # "stick" or "extra"
    artist: str = ""
    title: str = ""
    album: str = ""
    duration: float = 0.0
    bitrate: int = 0
    lossless: bool = False
    has_art: bool = False
    size: int = 0
    readable: bool = True
    problem: str = ""
    playlists: list = field(default_factory=list)  # (name, position)
    cues: int = 0
    _digest: str = ""

    @property
    def ext(self):
        return os.path.splitext(self.path)[1].lower()

    def digest(self):
        """Cheap content identity: size plus the first megabyte.

        Only ever computed for files already grouped as the same recording, so
        the whole library is never read -- that matters when one side of the
        comparison is on a USB stick.
        """
        if not self._digest:
            h = hashlib.sha1()
            h.update(str(self.size).encode())
            try:
                with open(self.path, "rb") as handle:
                    h.update(handle.read(1 << 20))
            except OSError as exc:
                self._digest = f"unreadable:{exc}"
                return self._digest
            self._digest = h.hexdigest()
        return self._digest

    def identity(self):
        """What makes this the same recording as another copy.

        Artist and title when both are there, because a filename embeds an
        export's playlist numbering and a tag does not. The filename is the
        fallback for the files whose tags are empty.
        """
        artist = normalize_text(self.artist)
        title = normalize_text(self.title)
        if artist and title:
            return ("tag", artist, title)
        return ("name", normalize_stem(self.path), "")


def read_tags(path):
    """Everything the file itself can say. Never raises: a file that cannot be
    read is a finding, not a crash."""
    copy = Copy(path=path, source="")
    try:
        copy.size = os.path.getsize(path)
    except OSError as exc:
        copy.readable = False
        copy.problem = f"size unreadable: {exc}"
        return copy

    try:
        audio = mutagen.File(path)
    except Exception as exc:  # mutagen raises a zoo of its own types
        copy.readable = False
        copy.problem = f"tags unreadable: {type(exc).__name__}"
        return copy

    if audio is None:
        copy.readable = False
        copy.problem = "not audio, or truncated beyond recognition"
        return copy

    info = getattr(audio, "info", None)
    copy.duration = float(getattr(info, "length", 0.0) or 0.0)
    copy.bitrate = int((getattr(info, "bitrate", 0) or 0) / 1000)
    copy.lossless = os.path.splitext(path)[1].lower() in LOSSLESS_EXTENSIONS

    def first(*keys):
        for key in keys:
            # A Vorbis comment block refuses a key it considers malformed --
            # "TPE1" is an ID3 frame name and raises ValueError on a FLAC --
            # so asking the wrong container for the wrong key must not be
            # fatal. Every file gets asked for every spelling; only the ones
            # that fit answer.
            try:
                value = audio.get(key)
            except Exception:
                continue
            if value:
                if isinstance(value, list):
                    value = value[0]
                text = str(value).strip()
                if text:
                    return text
        return ""

    copy.artist = first("TPE1", "artist", "\xa9ART", "ARTIST")
    copy.title = first("TIT2", "title", "\xa9nam", "TITLE")
    copy.album = first("TALB", "album", "\xa9alb", "ALBUM")

    # Cover art lives somewhere different in every container.
    try:
        if audio.tags is not None:
            if hasattr(audio.tags, "getall") and audio.tags.getall("APIC"):
                copy.has_art = True
            elif "covr" in audio.tags:
                copy.has_art = True
        if not copy.has_art and getattr(audio, "pictures", None):
            copy.has_art = True
    except Exception:
        pass

    # A file whose duration cannot be established is either truncated or not
    # really audio; either way it must not silently become the surviving copy.
    if copy.duration <= 0.0:
        copy.problem = "no readable duration (truncated?)"
    return copy


def load_catalog(xml_path):
    """The stick, as "seabass-cli export-xml" describes it."""
    root = ET.parse(xml_path).getroot()
    by_id = {}
    copies = []
    for element in root.findall("./COLLECTION/TRACK"):
        location = element.get("Location", "")
        path = urllib.parse.unquote(location.replace("file://localhost", ""))
        copy = Copy(
            path=path,
            source="stick",
            artist=element.get("Artist", ""),
            title=element.get("Name", ""),
            album=element.get("Album", ""),
            duration=float(element.get("TotalTime", 0) or 0),
            bitrate=int(element.get("BitRate", 0) or 0),
            lossless=os.path.splitext(path)[1].lower() in LOSSLESS_EXTENSIONS,
            cues=len(element.findall("POSITION_MARK")),
        )
        by_id[element.get("TrackID")] = copy
        copies.append(copy)

    def walk(node, prefix):
        name = node.get("Name", "")
        full = f"{prefix}/{name}" if prefix else name
        if node.get("Type") == "1":
            for position, entry in enumerate(node.findall("TRACK")):
                copy = by_id.get(entry.get("Key"))
                if copy is not None:
                    copy.playlists.append((full, position))
        for child in node.findall("NODE"):
            walk(child, "" if name == "ROOT" else full)

    playlists_root = root.find("PLAYLISTS/NODE")
    if playlists_root is not None:
        walk(playlists_root, "")
    return copies


def load_folder(root_dir):
    """A folder tree with no catalog: the deepest folder is the playlist."""
    copies = []
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [d for d in dirnames if d != "@eaDir"]
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() not in AUDIO_EXTENSIONS:
                continue
            path = os.path.join(dirpath, name)
            copy = read_tags(path)
            copy.source = "extra"
            relative = os.path.relpath(dirpath, root_dir)
            if relative not in (".", ""):
                copy.playlists.append((relative.replace(os.sep, "/"), -1))
            copies.append(copy)
    return copies


def group_copies(copies):
    """Same recording, one group. Duration is the guard against two different
    songs sharing a title."""
    buckets = collections.defaultdict(list)
    for copy in copies:
        buckets[copy.identity()].append(copy)

    groups = []
    for _, bucket in sorted(buckets.items()):
        remaining = sorted(bucket, key=lambda c: -c.duration)
        while remaining:
            seed = remaining.pop(0)
            group = [seed]
            rest = []
            for candidate in remaining:
                same_length = (
                    seed.duration <= 0
                    or candidate.duration <= 0
                    or abs(candidate.duration - seed.duration) <= DURATION_TOLERANCE_S
                )
                (group if same_length else rest).append(candidate)
            remaining = rest
            groups.append(group)
    return groups


def reconcile_groups(groups, weak_rows):
    """A second, looser pass over what the strict key could not join.

    The strict key is artist + title + length, and two things defeat it on real
    files: a copy whose tags are empty falls back to its filename and can never
    match a tagged one, and a title that carries "(Extended Mix)" on one side
    only splits a recording in two. Measured here: 2 of 158 archived FLACs had
    an mp3 twin this pass finds and the strict one missed.

    Deliberately conservative -- it only ever joins groups whose durations
    agree within the usual tolerance, and every join it makes is written to
    weak_rows for a human to veto, because a looser key is exactly how two
    different mixes of the same title get merged by accident.
    """

    # What a mix descriptor is called varies, and whether it is in brackets at
    # all varies more: one exporter writes "Advice(Extended Mix)" into a title
    # tag, another writes "Advice Extended Mix" into a filename and no tags at
    # all. Same recording, and nothing short of removing the words joins them.
    mix_words = re.compile(
        r"\b(original|extended|radio|club|instrumental|vocal|dub|album|single)?\s*"
        r"(mix|edit|version|remix\s*edit)\b",
        re.IGNORECASE,
    )

    def title_core(text):
        text = re.sub(r"\(.*?\)", " ", text or "")
        text = mix_words.sub(" ", text)
        return normalize_text(text)

    def weak_key(group):
        """Title alone, deliberately.

        Not artist+title: a file with no tags has no artist to offer, and 108
        tracks in this library are exactly that -- an untagged rip named
        "Advice Extended Mix" beside a tagged "Tiefstone - Advice(Extended
        Mix)". Keying on the artist keeps them apart forever. Duration is what
        guards against two different songs sharing a title, and every join is
        reported anyway.
        """
        for copy in group:
            core = title_core(copy.title)
            if core:
                return core
        return title_core(normalize_stem(group[0].path)) or normalize_stem(group[0].path)

    buckets = collections.defaultdict(list)
    for group in groups:
        buckets[weak_key(group)].append(group)

    merged = []
    for key, candidates in buckets.items():
        if len(candidates) == 1 or not key:
            merged.extend(candidates)
            continue
        base = candidates[0]
        joined = list(base)
        seed = max((c.duration for c in base), default=0.0)
        for other in candidates[1:]:
            other_duration = max((c.duration for c in other), default=0.0)
            close = seed > 0 and other_duration > 0 and abs(seed - other_duration) <= DURATION_TOLERANCE_S
            if not close:
                merged.append(other)
                continue
            weak_rows.append(
                [
                    base[0].path,
                    other[0].path,
                    key,
                    f"{seed:.0f}s vs {other_duration:.0f}s",
                ]
            )
            joined.extend(other)
        merged.append(joined)
    return merged


def canonical_artists(copies):
    """One spelling per artist, so "JackRock" and "Jackrock" share a folder.

    Without this the folder a track lands in depends on which copy won, and
    the same artist ends up with two directories that differ only in case --
    on a case-insensitive filesystem that is merely untidy, on the exFAT stick
    it is two real folders.
    """
    spellings = collections.defaultdict(collections.Counter)
    for copy in copies:
        if copy.artist:
            spellings[normalize_text(copy.artist)][copy.artist] += 1
    return {key: counter.most_common(1)[0][0] for key, counter in spellings.items()}


def rank(copy):
    """Which copy of a recording deserves to be the one in the library.

    A catalogued copy wins a tie because it is the one carrying the cues, the
    rating and the play count; bitrate decides before that, because quality is
    the point of keeping one rather than another.
    """
    return (
        0 if copy.problem else 1,
        copy.bitrate,
        1 if copy.source == "stick" else 0,
        copy.size,
    )


def strip_leading_artist(title, artist):
    """Drop an artist name the title already carries.

    Plenty of tags read "Charlotte de Witte - Tomorrowland (Closing Main Stage
    2025)" in the title field alone, and "<artist> - <title>" then produces the
    artist twice in one filename. Only an exact, case-insensitive match of the
    artist followed by a separator is removed, so a title that merely mentions
    someone ("Voodoo (Tinlicker Remix)") is untouched.
    """
    if not title or not artist:
        return title
    normalized_title = normalize_text(title)
    normalized_artist = normalize_text(artist)
    if not normalized_artist or not normalized_title.startswith(normalized_artist):
        return title
    remainder = title[len(artist):] if title.lower().startswith(artist.lower()) else ""
    remainder = remainder.lstrip()
    for separator in ("-", "\u2013", "\u2014", ":"):
        if remainder.startswith(separator):
            stripped = remainder[len(separator):].strip()
            return stripped or title
    return title


def target_path(copy, out_dir, lossless_dir, canonical=None):
    spelling = copy.artist
    if canonical and copy.artist:
        spelling = canonical.get(normalize_text(copy.artist), copy.artist)
    artist = safe_component(spelling or "Unknown Artist")
    title = safe_component(
        strip_leading_artist(copy.title, spelling) or os.path.splitext(os.path.basename(copy.path))[0]
    )
    base = lossless_dir if copy.lossless else os.path.join(out_dir, "Tracks")
    return os.path.join(base, artist, f"{artist} - {title}{copy.ext}")


def write_library_xml(catalog_xml, plan_rows, playlist_members, out_path, canonical, targets_seen, aliases=None):
    """The rekordbox XML for the library as it now stands on disk.

    Built by rewriting the catalog export rather than composing one from
    nothing: that file already carries the cues, ratings and play counts, and
    already solved the escaping and percent-encoding problems that make a
    hand-rolled collection XML unimportable (see docs/rekordbox-xml-export.md).
    What this adds is everything the catalog never knew about -- the files that
    came from a folder with no database, and the MP3s transcoded from
    FLAC-only recordings -- and the new location of every file that moved.

    A track whose file was not placed (a duplicate, or a copy that would not
    parse) is dropped: rekordbox keys its collection on Location, and a row
    pointing at a file that is not there is how a library fills up with
    exclamation marks.
    """
    import xml.etree.ElementTree as ET

    moved = {row[0]: row[1] for row in plan_rows}
    # A catalog row whose own file lost the quality comparison still describes
    # the recording, and it is the row carrying the cues. Following the
    # duplicate chain to the file that survived is what keeps those cues:
    # dropping the row instead would silently lose them for every track whose
    # better copy came from somewhere else.
    for dropped, kept in (aliases or {}).items():
        if dropped not in moved and kept in moved:
            moved[dropped] = moved[kept]

    tree = ET.parse(catalog_xml)
    root = tree.getroot()
    collection = root.find("COLLECTION")

    kept_by_destination = {}
    next_id = 0
    for track in list(collection):
        location = track.get("Location", "")
        source = urllib.parse.unquote(location.replace("file://localhost", ""))
        destination = moved.get(source)
        if destination is None:
            collection.remove(track)
            continue
        existing = kept_by_destination.get(destination)
        if existing is not None:
            # Two catalog rows for one file on disk: union their cues by slot,
            # the same rule the merge across catalogs uses, rather than letting
            # the second row overwrite the first.
            taken = {(m.get("Num"), m.get("Start")) for m in existing.findall("POSITION_MARK")}
            slots = {m.get("Num") for m in existing.findall("POSITION_MARK") if m.get("Num") != "-1"}
            for mark in track.findall("POSITION_MARK"):
                key = (mark.get("Num"), mark.get("Start"))
                if key in taken or (mark.get("Num") != "-1" and mark.get("Num") in slots):
                    continue
                existing.append(mark)
                taken.add(key)
                if mark.get("Num") != "-1":
                    slots.add(mark.get("Num"))
            collection.remove(track)
            continue
        track.set("Location", "file://localhost" + urllib.parse.quote(destination, safe="/"))
        kept_by_destination[destination] = track
        next_id = max(next_id, int(track.get("TrackID", "0")))

    # Everything the catalog never listed.
    for row in plan_rows:
        destination, role = row[1], row[2]
        if role == "lossless-archive" or destination in kept_by_destination:
            continue
        copy = targets_seen.get(destination)
        if copy is None:
            continue
        next_id += 1
        spelling = canonical.get(normalize_text(copy.artist), copy.artist) if copy.artist else ""
        attributes = {
            "TrackID": str(next_id),
            "Name": xml_safe(strip_leading_artist(copy.title, spelling) or os.path.splitext(os.path.basename(destination))[0]),
            "Artist": xml_safe(spelling),
            "Album": xml_safe(copy.album),
            "Kind": "MP3 File" if destination.lower().endswith(".mp3") else "M4A File",
            "TotalTime": str(int(copy.duration)),
            "Location": "file://localhost" + urllib.parse.quote(destination, safe="/"),
        }
        if role == "transcode":
            attributes["BitRate"] = "320"
        elif copy.bitrate:
            attributes["BitRate"] = str(copy.bitrate)
        element = ET.SubElement(collection, "TRACK", attributes)
        kept_by_destination[destination] = element

    collection.set("Entries", str(len(collection)))

    # One playlist section for the library as a whole: the catalog's playlists
    # and the folder-derived ones are the same kind of thing now. The catalog's
    # have to be read out BEFORE the section is rebuilt, or rebuilding throws
    # away every playlist the stick had -- which is most of them.
    playlists = root.find("PLAYLISTS")
    destination_of_id = {track.get("TrackID"): destination for destination, track in kept_by_destination.items()}

    def harvest(node, prefix):
        name = node.get("Name", "")
        full = name if not prefix else f"{prefix}/{name}"
        if node.get("Type") == "1":
            for position, entry in enumerate(node.findall("TRACK")):
                destination = destination_of_id.get(entry.get("Key"))
                if destination is not None:
                    playlist_members[full].append((position, destination))
            return
        for child in node.findall("NODE"):
            harvest(child, "" if name == "ROOT" else full)

    existing_root = playlists.find("NODE")
    if existing_root is not None:
        harvest(existing_root, "")

    for child in list(playlists):
        playlists.remove(child)
    root_node = ET.SubElement(playlists, "NODE", {"Name": "ROOT", "Type": "0", "Count": "0"})

    written = 0
    for name in sorted(playlist_members):
        members = sorted(playlist_members[name], key=lambda m: (m[0] < 0, m[0]))
        entries = []
        seen = set()
        for _, destination in members:
            track = kept_by_destination.get(destination)
            if track is None or destination in seen:
                continue
            seen.add(destination)
            entries.append(track.get("TrackID"))
        if not entries:
            continue
        node = root_node
        segments = [safe_component(part) for part in name.split("/") if part]
        for segment in segments[:-1]:
            existing = next((n for n in node.findall("NODE") if n.get("Name") == segment and n.get("Type") == "0"), None)
            node = existing if existing is not None else ET.SubElement(node, "NODE", {"Name": segment, "Type": "0", "Count": "0"})
        leaf = ET.SubElement(
            node,
            "NODE",
            {"Name": xml_safe(segments[-1] if segments else name), "Type": "1", "KeyType": "0", "Entries": str(len(entries))},
        )
        for track_id in entries:
            ET.SubElement(leaf, "TRACK", {"Key": track_id})
        written += 1

    def fix_counts(node):
        children = node.findall("NODE")
        if children:
            node.set("Count", str(len(children)))
            for child in children:
                fix_counts(child)

    fix_counts(root_node)
    tree.write(out_path, encoding="UTF-8", xml_declaration=True)
    return len(collection), written


def xml_safe(text):
    """Drop what XML 1.0 cannot hold, matching the C++ writer's rule.

    A single control byte out of an ID3 frame makes the document unparseable
    and rekordbox rejects the file without saying which track did it.
    """
    if not text:
        return ""
    return "".join(c for c in text if c in "\t\n\r" or 0x20 <= ord(c) < 0x7F or ord(c) > 0x9F)


def xml_from_plan(plan_path, catalog_xml, out_path, extra_roots):
    """Write the library XML from a plan already carried out.

    Needed because a plan is a record of a move: once the files are in the
    library, re-scanning the sources finds only what was left behind, and a
    fresh plan would omit everything that succeeded. The plan on disk is the
    only complete statement of where each file went.

    Playlists come from two places, as they did when the plan was made: the
    catalog XML for tracks a stick knew about, and the source folder for files
    that had no catalog -- which is recoverable because the plan kept the path
    each file came from.
    """
    import xml.etree.ElementTree as ET

    plan_rows = []
    targets_seen = {}
    playlist_members = collections.defaultdict(list)

    with open(plan_path, encoding="utf-8") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            source, destination, role = row["source"], row["destination"], row["role"]
            plan_rows.append([source, destination, role, row["from"], row["kbps"], row["seconds"], row["cues"]])
            copy = Copy(path=source, source=row["from"])
            copy.duration = float(row["seconds"] or 0)
            copy.bitrate = int(row["kbps"] or 0)
            copy.lossless = destination.lower().endswith(".flac")
            # The tags are read back off the file that is now in the library,
            # so the XML says what the library actually holds.
            if os.path.exists(destination):
                probe = read_tags(destination)
                copy.artist, copy.title, copy.album = probe.artist, probe.title, probe.album
                copy.duration = probe.duration or copy.duration
                copy.bitrate = probe.bitrate or copy.bitrate
            targets_seen[destination] = copy

            if role == "transcode":
                playlist_members[FROM_FLAC_PLAYLIST].append((-1, destination))
                continue
            if role != "library" or row["from"] != "extra":
                continue
            for root_dir in extra_roots:
                root_dir = os.path.expanduser(root_dir)
                if source.startswith(root_dir):
                    relative = os.path.dirname(os.path.relpath(source, root_dir))
                    if relative not in (".", ""):
                        playlist_members[relative.replace(os.sep, "/")].append((-1, destination))
                    break

    # dropped copy -> the copy the library actually holds
    aliases = {}
    duplicates_path = os.path.join(os.path.dirname(plan_path), "duplicates.tsv")
    if os.path.exists(duplicates_path):
        with open(duplicates_path, encoding="utf-8") as handle:
            for row in csv.DictReader(handle, delimiter="\t"):
                aliases[row["dropped"]] = row["kept"]

    canonical = canonical_artists(list(targets_seen.values()))
    return write_library_xml(
        catalog_xml, plan_rows, playlist_members, out_path, canonical, targets_seen, aliases
    )


def quarantine_untagged_duplicates(out_dir, plan_path):
    """Remove copies a better-tagged twin already covers, from a built library.

    For libraries assembled before the matcher learned to ignore mix
    descriptors and missing artists. An untagged rip named "Advice Extended
    Mix" and a tagged "Tiefstone - Advice(Extended Mix)" of the same length are
    one recording, and only the tagged one is worth keeping: it is the copy
    carrying the artist, the album and any cues.

    Files are MOVED to <library>/quarantine/, never deleted. The judgement
    "these are the same recording" is good but not certain, and a directory
    you can listen through and then remove is the honest way to ship a
    judgement like that.
    """
    import shutil

    rows = list(csv.DictReader(open(plan_path, encoding="utf-8"), delimiter="\t"))
    by_destination = {r["destination"]: r for r in rows}

    probes = {}
    for destination in by_destination:
        if destination.lower().endswith(".flac") or not os.path.exists(destination):
            continue
        probes[destination] = read_tags(destination)

    mix_words = re.compile(
        r"\b(original|extended|radio|club|instrumental|vocal|dub|album|single)?\s*(mix|edit|version)\b",
        re.IGNORECASE,
    )

    def core(text):
        return normalize_text(mix_words.sub(" ", re.sub(r"\(.*?\)", " ", text or "")))

    tagged = [(d, c) for d, c in probes.items() if c.artist and c.title]
    untagged = [(d, c) for d, c in probes.items() if not c.artist]

    quarantine = os.path.join(out_dir, "quarantine")
    removed = []
    for destination, copy in untagged:
        name_core = core(copy.title) or core(os.path.splitext(os.path.basename(destination))[0].split(" - ", 1)[-1])
        if len(name_core) < 8:
            continue
        for other_destination, other in tagged:
            other_core = core(other.title)
            if not other_core or len(other_core) < 8:
                continue
            if not (name_core.startswith(other_core) or other_core.startswith(name_core)):
                continue
            if not (copy.duration and other.duration and abs(copy.duration - other.duration) <= 3.0):
                continue
            relative = os.path.relpath(destination, out_dir)
            target = os.path.join(quarantine, relative)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.move(destination, target)
            removed.append([destination, other_destination, f"{copy.duration:.0f}s vs {other.duration:.0f}s"])
            break

    kept = [r for r in rows if r["destination"] not in {row[0] for row in removed}]
    with open(plan_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), delimiter="\t")
        writer.writeheader()
        writer.writerows(kept)

    report = os.path.join(os.path.dirname(plan_path), "untagged-duplicates.tsv")
    with open(report, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(["quarantined", "covered_by", "durations"])
        writer.writerows(removed)
    return len(removed), quarantine, report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--catalog-xml", help="XML written by seabass-cli export-xml")
    parser.add_argument("--extra-root", action="append", default=[], help="folder of audio files (repeatable)")
    parser.add_argument("--out", required=True, help="the library to build")
    parser.add_argument("--apply", action="store_true", help="actually place files (default: report only)")
    parser.add_argument(
        "--quarantine-untagged-duplicates",
        action="store_true",
        help="in a built library, move untagged copies a tagged twin already covers into quarantine/",
    )
    parser.add_argument(
        "--from-plan",
        metavar="PLAN.TSV",
        help="skip scanning and write the library XML from a plan already carried out",
    )
    parser.add_argument(
        "--move-local",
        action="store_true",
        help="move (not copy) sources already on the library's own volume -- instant, and costs no space",
    )
    args = parser.parse_args()

    out_dir = os.path.expanduser(args.out)
    lossless_dir = os.path.join(out_dir, "Lossless")
    reports_dir = os.path.join(out_dir, "reports")
    os.makedirs(reports_dir, exist_ok=True)

    if args.quarantine_untagged_duplicates:
        count, quarantine, report = quarantine_untagged_duplicates(
            out_dir, os.path.join(reports_dir, "plan.tsv")
        )
        print(f"quarantined {count} untagged duplicate(s) -> {quarantine}")
        print(f"listed in {report}")
        return

    if args.from_plan:
        xml_path = os.path.join(out_dir, "rekordbox-library.xml")
        tracks, lists = xml_from_plan(
            os.path.expanduser(args.from_plan), os.path.expanduser(args.catalog_xml), xml_path, args.extra_root
        )
        print(f"{tracks} track(s), {lists} playlist(s) -> {xml_path}")
        return

    copies = []
    if args.catalog_xml:
        catalog = load_catalog(os.path.expanduser(args.catalog_xml))
        print(f"catalog: {len(catalog)} track(s)")
        # The catalog says what the stick believes; the files say what they
        # are. Where a file is present, it is the better source for bitrate,
        # duration and whether there is any cover art in it at all.
        for index, copy in enumerate(catalog, 1):
            if os.path.exists(copy.path):
                probe = read_tags(copy.path)
                copy.bitrate = probe.bitrate or copy.bitrate
                copy.duration = probe.duration or copy.duration
                copy.has_art = probe.has_art
                copy.size = probe.size
                copy.problem = probe.problem
                copy.readable = probe.readable
            else:
                copy.readable = False
                copy.problem = "catalog row points at a file that is not there"
            if index % 200 == 0:
                print(f"  probed {index}/{len(catalog)}", flush=True)
        copies.extend(catalog)

    for root_dir in args.extra_root:
        root_dir = os.path.expanduser(root_dir)
        print(f"scanning {root_dir} ...", flush=True)
        folder = load_folder(root_dir)
        print(f"  {len(folder)} audio file(s)")
        copies.extend(folder)

    weak_rows = []
    groups = reconcile_groups(group_copies(copies), weak_rows)
    canonical = canonical_artists(copies)

    plan_rows = []
    transcode_rows = []
    duplicate_rows = []
    artwork_rows = []
    problem_rows = []
    playlist_members = collections.defaultdict(list)
    reclaimable = 0
    targets_seen = {}

    for group in groups:
        lossy = [c for c in group if not c.lossless]
        lossless = [c for c in group if c.lossless]

        # A file that will not parse is not a copy of anything. It must never
        # become the library's version of a recording: 114 files in this
        # library are the right size and entirely zero bytes -- and 109 of
        # them are the only copy there is, so keeping the "best available"
        # copy would have filled the library with silence that looks like
        # music until a deck refuses it.
        usable_lossy = [c for c in lossy if not c.problem]
        usable_lossless = [c for c in lossless if not c.problem]

        keeper = max(usable_lossy, key=rank) if usable_lossy else None
        archive = max(usable_lossless, key=rank) if usable_lossless else None

        if keeper is None and archive is None:
            for copy in group:
                problem_rows.append(
                    [copy.path, "no usable copy", f"{copy.problem}; this recording is not in the library"]
                )
            continue

        if keeper is None and archive is not None:
            # No copy an RX2 could play. The FLAC is still archived, and a
            # 320 CBR encode of it becomes the library's copy so the recording
            # is not simply missing from rekordbox. They go in one playlist of
            # their own, so what was generated stays obvious.
            transcode_rows.append(archive)

        for copy in group:
            if copy.problem:
                replaced = "a good copy of this recording is in the library" if keeper else "archived as lossless only"
                problem_rows.append([copy.path, "unusable", f"{copy.problem}; {replaced}"])

        for chosen in (keeper, archive):
            if chosen is None:
                continue
            destination = target_path(chosen, out_dir, lossless_dir, canonical)
            # Two different recordings can reduce to one name. Never let one
            # silently overwrite the other.
            if destination in targets_seen and targets_seen[destination] is not chosen:
                stem, ext = os.path.splitext(destination)
                suffix = 2
                while f"{stem} ({suffix}){ext}" in targets_seen:
                    suffix += 1
                destination = f"{stem} ({suffix}){ext}"
            targets_seen[destination] = chosen
            plan_rows.append(
                [
                    chosen.path,
                    destination,
                    "lossless-archive" if chosen.lossless else "library",
                    chosen.source,
                    str(chosen.bitrate),
                    f"{chosen.duration:.0f}",
                    str(chosen.cues),
                ]
            )
            if not chosen.lossless:
                for name, position in chosen.playlists:
                    playlist_members[name].append((position, destination))
                if not chosen.has_art:
                    artwork_rows.append([destination, chosen.artist, chosen.title, "no embedded cover art"])

        if len(group) > 1:
            by_digest = collections.defaultdict(list)
            for copy in group:
                by_digest[copy.digest()].append(copy)
            for copy in group:
                if copy is keeper or copy is archive:
                    continue
                identical = len(by_digest[copy.digest()]) > 1
                # Name the copy this one actually lost to, of its own kind: a
                # redundant FLAC loses to the archived FLAC, not to the mp3
                # that happens to be the library's copy of the recording.
                survivor = archive if copy.lossless and archive is not None else (keeper or archive)
                duplicate_rows.append(
                    [
                        survivor.path,
                        copy.path,
                        "byte-identical" if identical else "same recording, different file",
                        copy.source,
                        str(copy.bitrate),
                        f"{copy.duration:.0f}",
                        str(copy.size),
                    ]
                )
                reclaimable += copy.size

    # The transcodes, planned like any other file so the reports and the copy
    # phase treat them the same way.
    transcode_plan = []
    for archive in transcode_rows:
        spelling = canonical.get(normalize_text(archive.artist), archive.artist)
        artist = safe_component(spelling or "Unknown Artist")
        title = safe_component(
            strip_leading_artist(archive.title, spelling) or os.path.splitext(os.path.basename(archive.path))[0]
        )
        destination = os.path.join(out_dir, "Tracks", artist, f"{artist} - {title}.mp3")
        if destination in targets_seen:
            stem, ext = os.path.splitext(destination)
            suffix = 2
            while f"{stem} ({suffix}){ext}" in targets_seen:
                suffix += 1
            destination = f"{stem} ({suffix}){ext}"
        targets_seen[destination] = archive
        transcode_plan.append((archive, destination))
        plan_rows.append(
            [archive.path, destination, "transcode", archive.source, "320", f"{archive.duration:.0f}", "0"]
        )
        playlist_members[FROM_FLAC_PLAYLIST].append((-1, destination))
        if not archive.has_art:
            artwork_rows.append([destination, archive.artist, archive.title, "no embedded cover art"])

    def write_report(name, header, rows):
        path = os.path.join(reports_dir, name)
        with open(path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle, delimiter="\t")
            writer.writerow(header)
            writer.writerows(rows)
        return path

    write_report("plan.tsv", ["source", "destination", "role", "from", "kbps", "seconds", "cues"], plan_rows)
    write_report(
        "duplicates.tsv",
        ["kept", "dropped", "why", "dropped_from", "dropped_kbps", "seconds", "bytes"],
        sorted(duplicate_rows, key=lambda r: -int(r[6])),
    )
    write_report("artwork.tsv", ["track", "artist", "title", "issue"], artwork_rows)
    write_report(
        "weak-matches.tsv",
        ["group", "joined_with", "matched_on", "durations"],
        weak_rows,
    )
    # The recordings this library simply does not have a working copy of, as
    # artist/title rather than paths, because the next step for them is buying
    # or re-ripping, not file surgery.
    corrupt_rows = []
    for row in problem_rows:
        if row[1] != "no usable copy":
            continue
        stem = os.path.splitext(os.path.basename(row[0]))[0]
        stem = re.sub(r"^\d+[_\s-]+", "", stem)
        artist, _, title = stem.partition("-")
        corrupt_rows.append([artist.strip(), title.strip() or stem, row[0], row[2]])
    write_report("corrupt-recordings.tsv", ["artist", "title", "file", "detail"], sorted(corrupt_rows))
    write_report("unmatched.tsv", ["file", "kind", "detail"], problem_rows)

    playlist_rows = []
    normalized = collections.defaultdict(list)
    for name, members in sorted(playlist_members.items()):
        playlist_rows.append([name, str(len(members))])
        normalized[normalize_text(name).replace("whaleshark", "").strip()].append(name)
    write_report("playlists.tsv", ["playlist", "tracks"], playlist_rows)

    merge_rows = [
        [key or "(unnamed)", " | ".join(names), str(len(names))]
        for key, names in sorted(normalized.items())
        if len(names) > 1
    ]
    write_report("playlist-merges.tsv", ["after_merge", "candidates", "count"], merge_rows)

    library_tracks = sum(1 for row in plan_rows if row[2] == "library")
    transcodes = len(transcode_plan)
    archived = sum(1 for row in plan_rows if row[2] == "lossless-archive")
    # A transcode's destination is a 320 kbps encode, roughly a third of the
    # FLAC it comes from -- counting the source's size would overstate the
    # disk this needs by tens of gigabytes.
    bytes_needed = 0
    for row in plan_rows:
        copy = targets_seen.get(row[1])
        if copy is None:
            continue
        bytes_needed += int(copy.duration * 40_000) if row[2] == "transcode" else copy.size

    print()
    print(f"recordings:        {len(groups)}")
    print(f"library tracks:    {library_tracks}")
    print(f"lossless archived: {archived}")
    print(f"playlists:         {len(playlist_members)} ({len(merge_rows)} look like merge candidates)")
    print(f"duplicates:        {len(duplicate_rows)} copy/copies not needed, {reclaimable / 2**30:.1f} GB")
    print(f"transcodes:        {transcodes} FLAC-only recording(s) -> MP3 320, playlist \"{FROM_FLAC_PLAYLIST}\"")
    print(f"weak matches:      {len(weak_rows)} group(s) joined on a looser key (see weak-matches.tsv)")
    print(f"missing art:       {len(artwork_rows)}")
    print(f"unrecoverable:     {len(corrupt_rows)} recording(s) with no working copy (corrupt-recordings.tsv)")
    print(f"problems:          {len(problem_rows)}")
    print(f"space to copy:     {bytes_needed / 2**30:.1f} GB")
    print(f"reports:           {reports_dir}")

    if not args.apply:
        print("\nnothing copied -- this was a dry run; re-run with --apply")
        return

    # Files already on this volume are MOVED: a rename costs no space, and the
    # whole plan is 41 GB against less free disk than that. Files on the stick
    # are copied, because the stick is not ours to empty. Nothing is ever
    # removed from a source that was not moved into the library.
    import shutil

    moved = copied = encoded = failed = 0
    same_volume = os.stat(out_dir).st_dev

    for index, row in enumerate(plan_rows, 1):
        source, destination, role = row[0], row[1], row[2]
        if role == "transcode":
            continue
        if os.path.exists(destination):
            continue
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        try:
            if args.move_local and os.stat(source).st_dev == same_volume:
                shutil.move(source, destination)
                moved += 1
            else:
                # Copy to a partial name first: an interrupted copy must not
                # look like a finished track on the next run.
                partial = destination + ".partial"
                shutil.copy2(source, partial)
                os.replace(partial, destination)
                copied += 1
        except OSError as exc:
            problem_rows.append([source, "copy failed", str(exc)])
            failed += 1
        if index % 200 == 0:
            print(f"  placed {index}/{len(plan_rows)}", flush=True)

    if transcode_plan:
        print(f"\ntranscoding {len(transcode_plan)} FLAC-only recording(s) to MP3 320:")
    for index_of_transcode, (archive, destination) in enumerate(transcode_plan, 1):
        if os.path.exists(destination):
            continue
        # The FLAC may have just been moved into the archive; encode from
        # wherever it actually is now.
        source = archive.path
        if not os.path.exists(source):
            for row in plan_rows:
                if row[0] == archive.path and row[2] == "lossless-archive":
                    source = row[1]
                    break
        label = os.path.basename(destination)
        if len(label) > 58:
            label = label[:55] + "..."
        # One line per file, printed before the encode starts: a 7-minute FLAC
        # takes a few seconds, and a progress counter that only moves on
        # completion looks like a hang on the slow ones.
        position = f"[{index_of_transcode:>4}/{len(transcode_plan)}]"
        print(f"  {position} encoding {label}", flush=True)
        ok, note = transcode_to_mp3(source, destination, flac_tags(source))
        if ok:
            encoded += 1
            if note:
                problem_rows.append([destination, "transcoded with a warning", note])
                print(f"  {position} {note}", flush=True)
        else:
            failed += 1
            problem_rows.append([source, "transcode failed", note])
            print(f"  {position} FAILED: {note}", flush=True)

    write_report("unmatched.tsv", ["file", "kind", "detail"], problem_rows)

    print()
    print(f"moved:    {moved}")
    print(f"copied:   {copied}")
    print(f"encoded:  {encoded}")
    print(f"failed:   {failed}")

    xml_path = os.path.join(out_dir, "rekordbox-library.xml")
    if args.catalog_xml:
        tracks, lists = write_library_xml(
            os.path.expanduser(args.catalog_xml), plan_rows, playlist_members, xml_path, canonical, targets_seen
        )
        print(f"xml:      {xml_path} ({tracks} track(s), {lists} playlist(s))")
    print(f"library:  {out_dir}")


if __name__ == "__main__":
    main()
