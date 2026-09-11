// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/library_catalog_mtime.hpp"

#include <algorithm>
#include <system_error>

#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

std::int64_t libraryCatalogModifiedAt(const fs::path &stickRoot)
{
    const fs::path engineMain = engine::engineMainDatabasePath(stickRoot);
    const fs::path candidates[] = {
        stickRoot / "PIONEER" / "rekordbox" / "export.pdb",
        engineMain,
        fs::path(engineMain.string() + "-wal"),
        engineMain.parent_path() / engine::HistoryDbName,
    };
    std::int64_t newest = 0;
    for (const fs::path &candidate : candidates) {
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(candidate, ec);
        if (ec) {
            continue;
        }
        newest = std::max(newest, toUnixSeconds(time));
    }
    return newest;
}

}  // namespace seabass::infrastructure::stick_backup
