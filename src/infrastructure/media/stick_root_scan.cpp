// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/stick_root_scan.hpp"

#include <filesystem>

#include "infrastructure/engine/engine_library_layout.hpp"

namespace seabass::infrastructure::media
{

namespace fs = std::filesystem;
using application::DetectedStick;

void scanMountedRoot(const std::string &mountPoint, DetectedStick &stick)
{
    std::error_code ec;
    fs::path root(mountPoint);
    fs::path pdbPath = root / "PIONEER" / "rekordbox" / "export.pdb";
    fs::path engineDbPath = engine::engineMainDatabasePath(root);

    if (fs::exists(pdbPath, ec)) {
        stick.rekordboxPath = (root / "PIONEER").string();
    }
    if (fs::exists(engineDbPath, ec)) {
        stick.enginePath = engine::engineLibraryPath(root).string();
    }

    // A cheap, top-level-only peek at what's already on the drive -- for
    // the Format USB Stick feature's confirmation step, not a library
    // scan. Capped so a drive with thousands of loose files doesn't turn
    // this into real work; the point is giving a human something
    // recognizable to check against, not an inventory. See
    // application::DetectedStick::rootEntries's own comment for why this
    // exists (a raw disk index/size alone isn't something a person can
    // reliably recognize across reboots and replugs).
    constexpr size_t MaxRootEntries = 8;
    for (const auto &entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (stick.rootEntries.size() >= MaxRootEntries) {
            break;
        }
        std::error_code entryEc;
        bool isDir = entry.is_directory(entryEc);
        std::string name = entry.path().filename().string();
        stick.rootEntries.push_back(isDir ? name + "/" : name);
    }
}

}  // namespace seabass::infrastructure::media
