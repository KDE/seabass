// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "infrastructure/engine/engine_artwork.hpp"

namespace seabass::gui
{

// Everywhere a lost cover can still be found, past the rekordbox art on
// the stick itself, in the order this app is willing to promise them:
//
//   1. the track's own tags -- the copy both libraries were made from,
//      always with the stick, and free to read;
//   2. a stick backup on this computer -- exact, because an Engine
//      artwork file is named after the hash of its own bytes, so a name
//      found in any backup is the image the database is asking for.
//
// One object per scan: it opens a backup archive only once something is
// actually missing, and remembers what it found for the repair that
// follows the scan.
class ArtworkRescueSources
{
public:
    // `backupDirectory` empty, or holding no archives, simply leaves the
    // backups out. `stickRoot` is what an archive's paths are relative
    // to, so a track on this stick can be recognised in one.
    ArtworkRescueSources(const std::string &backupDirectory, const std::string &stickRoot);
    ~ArtworkRescueSources();

    infrastructure::engine::ArtworkSourceProbe probe();
    infrastructure::engine::ArtworkSourceReader reader();

private:
    std::string bytesFor(const infrastructure::engine::ArtworkEntry &entry);
    // The track's path as a backup archive spells it, or empty when this
    // track is not under the stick root.
    std::string archivePathFor(const std::string &trackFile) const;

    struct Impl;
    std::shared_ptr<Impl> m_impl;
    std::string m_stickRoot;
};

// Every full stick backup this computer holds, newest first.
std::vector<std::filesystem::path> stickBackupArchives(const std::string &backupDirectory);

}  // namespace seabass::gui
