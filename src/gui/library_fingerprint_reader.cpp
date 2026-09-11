// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "library_fingerprint_reader.hpp"

#include <exception>
#include <utility>
#include <vector>

#include "domain/track.hpp"
#include "gui/library_catalog_cache.hpp"

namespace seabass::gui
{

std::optional<domain::LibraryFingerprint> readLibraryFingerprint(const QString &rekordboxPath, const QString &enginePath)
{
    std::vector<domain::Track> tracks;
    bool anyRead = false;
    for (const auto &[format, path] : {std::pair{"rekordbox", rekordboxPath}, std::pair{"engine", enginePath}}) {
        if (path.isEmpty()) {
            continue;
        }
        try {
            std::vector<domain::Track> read = LibraryCatalogCache::instance().tracksFor(format, path.toStdString());
            tracks.insert(tracks.end(), read.begin(), read.end());
            anyRead = true;
        } catch (const std::exception &) {
            // Unreadable catalog: the other one may still do.
        }
    }
    if (!anyRead) {
        return std::nullopt;
    }
    return domain::fingerprintLibrary(tracks);
}

}  // namespace seabass::gui
