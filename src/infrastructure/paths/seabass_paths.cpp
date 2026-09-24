// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <cstdlib>
#include <cstring>

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

// An environment variable as a path, or empty when unset or blank. On
// Windows the narrow environment is in the ANSI code page, so a profile
// directory named for an account outside it (a Japanese user name) would
// not survive std::getenv(); the wide copy the CRT keeps alongside has
// the real characters. The variable names themselves are ASCII.
fs::path envPath(const char *name)
{
#if defined(_WIN32)
    const std::wstring wideName(name, name + std::strlen(name));
    const wchar_t *value = _wgetenv(wideName.c_str());
    if (value != nullptr && *value != L'\0') {
        return fs::path(value);
    }
#else
    const char *value = std::getenv(name);
    if (value != nullptr && *value != '\0') {
        return fs::path(value);
    }
#endif
    return {};
}

fs::path homeDirectory()
{
#if defined(_WIN32)
    if (const fs::path profile = envPath("USERPROFILE"); !profile.empty()) {
        return profile;
    }
    const fs::path drive = envPath("HOMEDRIVE");
    const fs::path path = envPath("HOMEPATH");
    if (!drive.empty() && !path.empty()) {
        // "C:" + "\Users\x": concatenation, not operator/, which would
        // put a separator after the drive letter's colon.
        fs::path home = drive;
        home += path;
        return home;
    }
#else
    if (const fs::path home = envPath("HOME"); !home.empty()) {
        return home;
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
    return pathToUtf8(pathFromUtf8(catalogPath).parent_path());
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

fs::path stickSilenceCache(const fs::path &stickRoot)
{
    return stickCachesDir(stickRoot) / "silence.jsonl";
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
    if (const fs::path fromEnv = envPath("SEABASS_HOME"); !fromEnv.empty()) {
        return fromEnv;
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
