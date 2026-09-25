// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <istream>
#include <set>
#include <string>
#include <vector>

namespace seabass::infrastructure::cleanup
{

struct PendingDeletion
{
    std::string timestampUtc;  // ISO 8601, set by append()
    std::string format;        // "rekordbox" or "engine"
    std::string filePath;
    std::string title;
    std::string artist;
    std::string backupId;  // the backup covering the DB edit that orphaned this file

    // NOT persisted in the manifest file itself (append()/list() never
    // read or write this) -- the manifest only ever recorded metadata
    // about the DB edit that orphaned a file, not a snapshot of its size
    // at that moment, which could go stale. Callers that want to show how
    // much disk space these entries represent (e.g. the GUI) populate
    // this with a fresh std::filesystem::file_size() stat right before
    // display, the same "never trust a static number, re-check live"
    // stance resolvePendingDeletions() already takes for reference-safety.
    std::uint64_t fileSizeBytes = 0;
};

// Append-only "garbage bin": a durable record of audio files a duplicate
// cleanup's database edit has orphaned (their track/playlist rows
// removed) but that haven't actually been deleted from disk -- so a
// later pass can review and act on them instead of that information
// being lost. One JSON object per line, human-inspectable like
// FileOperationLog but structured so it can be parsed back reliably.
class PendingDeletionManifest
{
public:
    explicit PendingDeletionManifest(std::string manifestPath);

    // Sets entry.timestampUtc to now and appends it as one line.
    //
    // Throws std::runtime_error if the line did not reach the file (a
    // full or write-protected stick, a folder that could not be made).
    // A change that cannot record what it orphaned must not report
    // success: the catalog rows would be gone and the file left behind
    // with nothing naming it, which Delete Orphaned Files reads this
    // file and only this file to find.
    void append(PendingDeletion entry);

    std::vector<PendingDeletion> list() const;

    // Rewrites the manifest, dropping every entry whose filePath is in
    // processedFilePaths -- call this once a pending deletion has
    // actually been acted on (the file deleted from disk). Every entry
    // that's kept retains its original timestampUtc exactly as first
    // appended; rewriting here never regenerates it, since it still
    // describes when the file was actually orphaned, not when this
    // rewrite happened to run. A no-op (file untouched) if none of
    // processedFilePaths actually match an existing entry.
    //
    // Returns false if the file could not be rewritten, in which case
    // the previous manifest is still there in full: the rewrite goes
    // through the same durable temp-file-and-rename as every other
    // replaced file on a stick. Callers have already deleted the files
    // by the time this runs, so this is something to report, not to
    // treat as "nothing happened".
    [[nodiscard]] bool removeProcessed(const std::set<std::string> &processedFilePaths);

    // Rewrites the manifest, dropping every entry the save behind one of
    // `backupIds` recorded -- for Undo Last Save, which puts those catalog
    // rows back, so their files are no longer orphaned and must not wait
    // in Delete Orphaned Files. Entries without a backup id (a stray file
    // no catalog ever named) are kept. A no-op when nothing matches.
    //
    // Returns false, with the previous manifest intact, if the file
    // could not be rewritten -- see removeProcessed().
    [[nodiscard]] bool removeForBackups(const std::set<std::string> &backupIds);

    // Every entry the file holds. False means it is there and could not
    // be read, which an empty list cannot say and which neither rewrite
    // may take for "nothing to remove". A reader whose answer depends on
    // the list being complete (the stray-file scan, which offers what is
    // not on it) asks here rather than through list().
    [[nodiscard]] bool readAll(std::vector<PendingDeletion> &entries) const;

    // readAll()'s parse and its verdict on the stream, apart from the
    // file: false when the stream stopped anywhere but a clean end.
    // Separate so that verdict can be tested with a stream that fails on
    // read, which a file on this machine's filesystem may not produce.
    [[nodiscard]] static bool readFrom(std::istream &in, std::vector<PendingDeletion> &entries);

private:
    // Replaces the whole file, durably and atomically. False means the
    // old contents are still there, untouched.
    bool rewrite(const std::string &contents) const;

    std::string m_manifestPath;
};

}  // namespace seabass::infrastructure::cleanup
