// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace seabass::infrastructure::engine
{

// Where Engine DJ keeps things on a stick. The string literals for these
// paths are still spelled out at many older call sites; new code goes
// through here, and stick_root_scan.cpp shows the pattern.
inline constexpr std::string_view EngineLibraryDir = "Engine Library";
inline constexpr std::string_view Database2Dir = "Database2";
inline constexpr std::string_view MainDbName = "m.db";      // tracks, cues, playlists (Engine 2.x/3.x)
inline constexpr std::string_view HistoryDbName = "hm.db";  // play history
inline constexpr std::string_view LegacyDbName = "p.db";    // Engine 1.x performance data

inline std::filesystem::path engineLibraryPath(const std::filesystem::path &stickRoot)
{
    return stickRoot / EngineLibraryDir;
}

inline std::filesystem::path engineMainDatabasePath(const std::filesystem::path &stickRoot)
{
    return stickRoot / EngineLibraryDir / Database2Dir / MainDbName;
}

// SQLite keeps a database's committed state across up to three files:
// the main file, a write-ahead log (`X.db-wal`) and a rollback journal
// (`X.db-journal`). They only make sense together -- see
// docs/stick-backup-plan.md, "Database capture". `X.db-shm` is a
// scratch index SQLite regenerates and must never be copied.
inline bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool isSqliteDatabaseFile(std::string_view filename)
{
    return endsWith(filename, ".db");
}

inline bool isSqliteShmFile(std::string_view filename)
{
    return endsWith(filename, ".db-shm");
}

// The main database a file belongs to: itself for `X.db`, `X.db` for the
// `-wal` / `-journal` sidecars, nothing for anything else (including
// `-shm`).
inline std::optional<std::filesystem::path> dbSetMainFile(const std::filesystem::path &path)
{
    // On the path itself, never through a narrow string: "m.db-wal" has
    // the extension ".db-wal" and the stem "m", so the main file is the
    // stem with ".db" back on it.
    const std::filesystem::path extension = path.extension();
    if (extension == ".db") {
        return path;
    }
    if (extension == ".db-wal" || extension == ".db-journal") {
        std::filesystem::path main = path.stem();
        main += ".db";
        return path.parent_path() / main;
    }
    return std::nullopt;
}

}  // namespace seabass::infrastructure::engine
