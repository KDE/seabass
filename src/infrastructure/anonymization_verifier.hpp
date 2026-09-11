// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

namespace seabass::infrastructure
{

// Checks that an anonymized export is what its own manifest promises.
//
// The manifest tells a contributor their original file paths are removed
// entirely. That promise was not kept for a long time: every analysis file
// still embedded the real path, and the Engine filename, album, genre and
// label columns were never scrubbed at all. Those specific holes are now
// fixed, and this exists so the promise is *checked* rather than trusted,
// on every export, before anyone is asked to send one.
//
// It runs on the staging directory, before the zip is written, so a
// failure can refuse to produce the file at all rather than describing a
// leak that already exists on disk.
//
// It works two ways. It reads the catalogs back through this project's
// own readers and checks every value has the shape of a placeholder; and
// it sweeps the raw bytes of every exported file for readable text that
// cannot be accounted for. The second exists because the first is blind
// by construction: a reader returns live rows, which are exactly the rows
// the anonymizer just overwrote, so it can never see a value still legible
// in a SQLite freeblock, in a page on the freelist, or in the slack
// DeviceSQL leaves when a row is removed. The 0.6-era fixture committed to
// this repository passed the reader checks with 52 real artist names in
// engine/Database2/m.db and 403 in rekordbox/export.pdb.
//
// What it will not do is prove anonymity in general. A scrubber that wrote
// a convincing-looking constant would pass the first check, and a leak
// made entirely of schema words would pass the second. It is a tripwire
// for the failures this project has actually had -- a field or a file
// nobody remembered to scrub, and a scrubbed row whose original was still
// sitting in the same file -- not a proof.
struct AnonymizationVerification
{
    bool ok = false;
    // One line per problem, each naming the file or field and what was
    // found, in the order they were found. A non-empty list means
    // something real leaked.
    std::vector<std::string> problems;
    // Things that stopped a check from running rather than failing it --
    // most often a catalog the readers cannot parse, which a hand-built
    // minimal test fixture genuinely is. Kept separate from problems
    // because "we could not look" and "we looked and found real data" are
    // different facts and only the second must block an export. That a
    // real export reads back correctly is asserted by the corpus runner,
    // which scans both catalogs and checks their counts.
    std::vector<std::string> warnings;
    int analysisFilesChecked = 0;
    // Files whose raw bytes were swept for readable text the catalog
    // readers cannot reach.
    int filesSwept = 0;
    // Entries in files.tsv whose name was confirmed to be a placeholder.
    int audioFilesChecked = 0;
    int rekordboxTracksSampled = 0;
    int engineTracksSampled = 0;
    int oneLibraryTracksSampled = 0;

    // Everything, ready to print or to put in an error message.
    std::string describe() const;
};

// `exportRoot` is the directory holding MANIFEST.txt, rekordbox/ and
// engine/ -- the staging directory AnonymizeLibrary builds before zipping.
// Sampling is capped so this stays fast enough to run on every export;
// analysis files are checked in full, because they are where the leak was.
// trackSampleSize caps how many tracks per catalog are checked. The
// default, 0, means every one of them.
//
// It used to default to 200, which on a 1161-track library left 83% of
// the rows unexamined -- a real title planted on the last track passed
// the gate with zero problems reported. This is the check that decides
// whether a DJ's library metadata gets uploaded to a stranger, so it
// looks at all of it. It costs almost nothing: the tracks are already
// read in full to be checked at all, and the per-track work is string
// comparison. Measured on the committed fixture, sampling 200 against
// sweeping everything is 1.58 s against 1.60 s.
//
// The parameter stays so tests can force a small sample and show the
// difference. It caps only the reader-based sampling; the byte sweep
// always reads every file in full, because a cap on it would reintroduce
// exactly the gap it was added to close.
//
// The sweep is what this now mostly costs: on the committed 30 MB fixture
// (6,680 files) the whole check is 4.5 s against the 1.6 s it was before.
// That is a one-off on export, against the alternative of shipping a real
// library, so it buys its seconds.
AnonymizationVerification verifyAnonymizedExport(const std::string &exportRoot, int trackSampleSize = 0);

// True when `value` could be a placeholder of `kind` produced by
// anonymizationPlaceholder(), including one the rekordbox writer truncated
// to fit the real field's byte length. Exposed for the verifier's own test.
bool looksLikeHashPlaceholder(const std::string &value, const std::string &kind);

// True when `value` could be an anonymizationFilenamePlaceholder(), i.e. a
// short hex hash plus an extension, possibly truncated.
bool looksLikeFilenamePlaceholder(const std::string &value);

// True when `value` could be an indexed placeholder like "Artist 007",
// possibly truncated.
bool looksLikeIndexedPlaceholder(const std::string &value, const std::string &kind);

}  // namespace seabass::infrastructure
