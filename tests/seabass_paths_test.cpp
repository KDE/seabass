// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The layout itself. Every location Seabass writes to is decided here, so
// a change to any of them should have to be deliberate rather than a
// side effect -- these assertions are the record of what was chosen.
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "infrastructure/paths/seabass_paths.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::paths;

int main()
{
    const fs::path stick = "/media/dj/RV2";

    // Everything on the stick lives under one visible directory. It used
    // to be five hidden dotfiles at the stick root, which a DJ could
    // neither find nor reason about.
    assert(stickDir(stick) == stick / "Seabass");
    assert(stickBackupsDir(stick) == stick / "Seabass" / "backups");
    assert(stickCachesDir(stick) == stick / "Seabass" / "caches");
    assert(stickOrphanedDir(stick) == stick / "Seabass" / "orphaned");
    assert(stickOperationLog(stick) == stick / "Seabass" / "seabass.log");
    assert(stickPendingDeletions(stick) == stick / "Seabass" / "orphaned" / "pending-deletions.jsonl");
    assert(stickMetadataCache(stick) == stick / "Seabass" / "caches" / "metadata.jsonl");
    assert(stickDurationCache(stick) == stick / "Seabass" / "caches" / "durations.jsonl");
    std::cout << "case 1 (the on-stick layout) OK\n";

    // The backups directory is now two levels below the stick root, and
    // FilesystemBackupStore derives the stick root by walking back up.
    // Pinned here because getting it wrong is silent: recorded paths
    // would be stored relative to <stick>/Seabass and every restore would
    // resolve inside the Seabass directory instead of back to the file.
    assert(stickBackupsDir(stick).parent_path().parent_path() == stick);
    std::cout << "case 2 (the stick root is two levels above the backups dir) OK\n";

    // A catalog path is PIONEER or Engine Library; the stick root is its
    // parent, and both catalogs on one stick must agree on it.
    assert(stickRootForCatalogPath((stick / "PIONEER").string()) == stick.string());
    assert(stickRootForCatalogPath((stick / "Engine Library").string()) == stick.string());
    assert(stickBackupsDir(stickRootForCatalogPath((stick / "PIONEER").string()))
           == stickBackupsDir(stickRootForCatalogPath((stick / "Engine Library").string())));
    std::cout << "case 3 (both catalogs on a stick resolve to one backups dir) OK\n";

    // SEABASS_HOME exists so a test run can never write into the real
    // ~/Seabass. Read every call, not cached at load time.
#if defined(_WIN32)
    _putenv_s("SEABASS_HOME", "C:\\tmp\\sb");
    const fs::path fake = "C:\\tmp\\sb";
#else
    setenv("SEABASS_HOME", "/tmp/seabass-paths-test", 1);
    const fs::path fake = "/tmp/seabass-paths-test";
#endif
    assert(localRoot() == fake);
    assert(localBackupsDir() == fake / "backups");
    assert(localFullBackupsDir() == fake / "backups" / "full");
    assert(localMetadataDir() == fake / "metadata");
    std::cout << "case 4 (the local layout, and SEABASS_HOME overrides it) OK\n";

    // Without the override it is ~/Seabass -- never a bare "." scattering
    // Seabass data through whatever directory the process started in,
    // unless there is genuinely no home to find.
#if defined(_WIN32)
    _putenv_s("SEABASS_HOME", "");
#else
    unsetenv("SEABASS_HOME");
#endif
    assert(localRoot().filename() == "Seabass");
    assert(localRoot().is_absolute() || localRoot().begin()->string() == ".");
    std::cout << "case 5 (the default local root is <home>/Seabass) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
