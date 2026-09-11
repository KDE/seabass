// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_restore_check.hpp"

#include <system_error>

#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

std::optional<std::vector<std::string>> checkRestoredEngineLibrary(const fs::path &targetRoot)
{
    std::error_code ec;
    if (!fs::exists(engineMainDatabasePath(targetRoot), ec)) {
        return std::nullopt;
    }
    LibdjinteropEngineReader reader(engineLibraryPath(targetRoot).string());
    std::vector<std::string> missing;
    for (const domain::Track &track : reader.readAll()) {
        if (!track.streamingSource.empty()) {
            continue;  // no local file by design
        }
        if (track.filePath.empty()) {
            missing.push_back(track.title.empty() ? track.filename : track.title + " (path unresolved)");
            continue;
        }
        if (!fs::exists(fs::path(track.filePath), ec)) {
            missing.push_back(track.filePath);
        }
    }
    return missing;
}

}  // namespace seabass::infrastructure::engine
