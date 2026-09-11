// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_layout.hpp"

#include <filesystem>
#include <system_error>

#include "infrastructure/engine/engine_library_layout.hpp"

namespace fs = std::filesystem;

namespace seabass::infrastructure
{

std::string catalogPathFor(const std::string &format, const std::string &libraryPath)
{
    if (libraryPath.empty()) {
        return {};
    }
    const fs::path root = fs::path(libraryPath).parent_path();
    const fs::path pioneerRoot = root / "PIONEER";
    std::error_code ec;

    if (format == "rekordbox") {
        return fs::exists(pioneerRoot / "rekordbox" / "export.pdb", ec) ? pioneerRoot.string() : std::string();
    }
    if (format == "engine") {
        return fs::exists(engine::engineMainDatabasePath(root), ec)
                   ? engine::engineLibraryPath(root).string()
                   : std::string();
    }
    if (format == "onelibrary") {
        // The filename directly rather than OneLibraryCueWriter::
        // existsFor(): this file is pure on-disk layout, and reaching
        // for a *writer* to answer "does this exist" dragged SQLCipher
        // into every test that links part of the core rather than all
        // of it -- pending_deletion_resolver_test stopped building.
        // Kept identical to OneLibraryCueWriter::dbPathFor().
        return fs::is_regular_file(pioneerRoot / "rekordbox" / "exportLibrary.db", ec) ? pioneerRoot.string()
                                                                                       : std::string();
    }
    return {};
}

}  // namespace seabass::infrastructure
