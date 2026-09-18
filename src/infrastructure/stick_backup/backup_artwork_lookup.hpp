// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace seabass::infrastructure::stick_backup
{

// Lifts a lost Engine artwork file out of this computer's stick backups.
//
// Engine names an artwork file after the hash of its own bytes, so a name
// is a checksum: a file found under that name in any backup is provably
// the image the database is asking for, whichever stick the backup was
// made from. That makes this an exact source rather than a guess -- worth
// reaching for when neither the stick's rekordbox art nor the track's own
// tags still have a copy.
//
// Opens each archive at most once and only when something is actually
// asked for, since a scan of a healthy library asks nothing.
class BackupArtworkLookup
{
public:
    explicit BackupArtworkLookup(std::vector<std::filesystem::path> archives);
    ~BackupArtworkLookup();
    BackupArtworkLookup(const BackupArtworkLookup &) = delete;
    BackupArtworkLookup &operator=(const BackupArtworkLookup &) = delete;

    // `name` is the artwork file's base name, i.e. what artworkFileName()
    // spells from the row's hash, without an extension. Returns the image
    // bytes, or empty when no backup holds it.
    std::string find(const std::string &name);

    // Whether find() would return something. Same cost -- the bytes are
    // kept, so asking and then taking them reads the archive once.
    bool has(const std::string &name);

    // The other way round, for a row that has lost even its hash: find
    // the same audio file in a backed-up library and take whatever cover
    // that library holds for it. `trackPath` is the file's path inside
    // the archive, i.e. relative to the stick root ("Contents/x.mp3").
    //
    // This is the last resort it sounds like: it costs reading a backup's
    // whole Engine database out of the archive, and the art it finds is
    // another library's idea of this track's cover rather than a checksum
    // match. Only asked when nothing closer has an answer.
    std::string findForTrack(const std::string &trackPath);

private:
    // The Engine database of one backup, pulled out of the archive once
    // and kept for as long as this object lives.
    struct ExtractedDatabase
    {
        std::filesystem::path file;  // empty: this archive has no Engine library
        bool tried = false;
    };

    std::string artworkNameForTrack(const std::filesystem::path &archive, const std::string &trackPath);

    std::vector<std::filesystem::path> m_archives;
    std::map<std::string, std::string> m_found;       // name -> bytes, "" when looked for and not there
    std::map<std::string, std::string> m_foundByTrack;  // archive-relative track path -> bytes
    std::map<std::string, ExtractedDatabase> m_databases;
    std::filesystem::path m_scratch;  // holds the extracted databases, removed with this object
};

}  // namespace seabass::infrastructure::stick_backup
