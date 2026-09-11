// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/find_unreferenced_files.hpp"

#include "application/path_key.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace seabass::application
{

namespace
{

namespace fs = std::filesystem;

void collect(const std::optional<std::vector<domain::Track>> &catalog, std::set<std::string> &referenced)
{
    if (!catalog) {
        return;
    }
    for (const auto &track : *catalog) {
        if (!track.filePath.empty()) {
            referenced.insert(normalizedPathKey(track.filePath));
        }
    }
}

}  // namespace

std::vector<std::string> CatalogTracks::present() const
{
    std::vector<std::string> names;
    if (rekordbox) {
        names.push_back("rekordbox");
    }
    if (engine) {
        names.push_back("engine");
    }
    if (oneLibrary) {
        names.push_back("onelibrary");
    }
    return names;
}

UnreferencedFileScan findUnreferencedFiles(const std::vector<AudioFileOnDisk> &filesOnDisk,
                                             const CatalogTracks &catalogs)
{
    UnreferencedFileScan result;
    result.catalogsConsulted = catalogs.present();
    result.audioFilesSeen = filesOnDisk.size();

    if (result.catalogsConsulted.empty()) {
        // No catalog at all: every file would look unreferenced, which
        // would propose deleting the entire stick. Report "unusable" and
        // nothing else.
        return result;
    }
    result.usable = true;

    std::set<std::string> referenced;
    collect(catalogs.rekordbox, referenced);
    collect(catalogs.engine, referenced);
    collect(catalogs.oneLibrary, referenced);
    result.referencedPathsSeen = referenced.size();

    for (const auto &file : filesOnDisk) {
        if (file.filePath.empty()) {
            continue;
        }
        if (!referenced.contains(normalizedPathKey(file.filePath))) {
            result.unreferenced.push_back(file);
        }
    }
    return result;
}

}  // namespace seabass::application
