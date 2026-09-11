// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
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
    void removeProcessed(const std::set<std::string> &processedFilePaths);

private:
    std::string m_manifestPath;
};

}  // namespace seabass::infrastructure::cleanup
