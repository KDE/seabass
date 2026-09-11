// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/paths/seabass_paths.hpp"

#include <cstdlib>

namespace seabass::infrastructure::paths
{

namespace
{
constexpr const char *StickDirName = "Seabass";
constexpr const char *BackupsSubdir = "backups";
constexpr const char *CachesSubdir = "caches";
constexpr const char *OrphanedSubdir = "orphaned";
constexpr const char *MetadataSubdir = "metadata";
constexpr const char *FullSubdir = "full";

fs::path homeDirectory()
{
#if defined(_WIN32)
    const char *profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0') {
        return fs::path(profile);
    }
    const char *drive = std::getenv("HOMEDRIVE");
    const char *path = std::getenv("HOMEPATH");
    if (drive != nullptr && path != nullptr && *drive != '\0') {
        return fs::path(std::string(drive) + path);
    }
#else
    const char *home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return fs::path(home);
    }
#endif
    // No home is not a situation to invent a path for -- returning "."
    // would scatter Seabass data through whatever directory the process
    // happened to start in.
    return fs::path(".");
}
}  // namespace

std::string stickRootForCatalogPath(const std::string &catalogPath)
{
    return fs::path(catalogPath).parent_path().string();
}

fs::path stickDir(const fs::path &stickRoot)
{
    return stickRoot / StickDirName;
}

fs::path stickBackupsDir(const fs::path &stickRoot)
{
    return stickDir(stickRoot) / BackupsSubdir;
}

fs::path stickCachesDir(const fs::path &stickRoot)
{
    return stickDir(stickRoot) / CachesSubdir;
}

fs::path stickOrphanedDir(const fs::path &stickRoot)
{
    return stickDir(stickRoot) / OrphanedSubdir;
}

fs::path stickOperationLog(const fs::path &stickRoot)
{
    return stickDir(stickRoot) / "seabass.log";
}

fs::path stickPendingDeletions(const fs::path &stickRoot)
{
    return stickOrphanedDir(stickRoot) / "pending-deletions.jsonl";
}

fs::path stickMetadataCache(const fs::path &stickRoot)
{
    return stickCachesDir(stickRoot) / "metadata.jsonl";
}

fs::path stickDurationCache(const fs::path &stickRoot)
{
    return stickCachesDir(stickRoot) / "durations.jsonl";
}

namespace
{
fs::path &localRootOverride()
{
    static fs::path value;
    return value;
}
}  // namespace

void setLocalRootOverride(const fs::path &root)
{
    localRootOverride() = root;
}

fs::path localRoot()
{
    if (!localRootOverride().empty()) {
        return localRootOverride();
    }
    // Read every call rather than cached: a test sets it after this
    // translation unit is already loaded.
    const char *fromEnv = std::getenv("SEABASS_HOME");
    if (fromEnv != nullptr && *fromEnv != '\0') {
        return fs::path(fromEnv);
    }
    return homeDirectory() / "Seabass";
}

fs::path localBackupsDir()
{
    return localRoot() / BackupsSubdir;
}

fs::path localFullBackupsDir()
{
    return localBackupsDir() / FullSubdir;
}

fs::path localMetadataDir()
{
    return localRoot() / MetadataSubdir;
}

fs::path localBrowsedBackupsDir()
{
    return localMetadataDir() / "browsed-backups";
}

}  // namespace seabass::infrastructure::paths
