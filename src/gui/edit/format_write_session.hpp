// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "gui/edit/save_context.hpp"
#include "infrastructure/scratch_dir_guard.hpp"

namespace seabass::gui
{

// The per-format, per-save write target: backs the catalog's database
// file up once, and -- when many items are about to hit one shared
// database on a slow stick (see infrastructure::shouldUseWholeFileReplace)
// -- redirects the writes to a local scratch copy that is committed back
// onto the stick in one durable atomic replace when the save finishes.
//
// Extracted from the three near-identical blocks the sync, clean-up and
// repair tasks used to carry. Obtained through SaveContext::shared() so
// every change of one save that writes the same catalog uses the same
// copy.
//
// Commit rule on finish: the scratch copy is committed when the save
// succeeded, or when it was cancelled/failed after at least one item was
// applied to it (those items are complete; the summary counts them).
// With zero items applied the scratch is simply discarded.
class FormatWriteSession
{
public:
    // format: "rekordbox" | "engine" | "onelibrary". catalogPath: the
    // PIONEER or Engine Library folder. itemCountHint: how many writes
    // this save is expected to make against the database (drives the
    // scratch decision). label: backup/log label, e.g. "sync".
    FormatWriteSession(std::string format, std::string catalogPath, int itemCountHint, std::string label,
                       SaveContext &ctx);

    const std::string &format() const { return m_format; }
    // Where writers should be pointed: the scratch copy's root (same
    // layout as the catalog folder) or the real catalog path.
    const std::string &writeRoot() const { return m_writeRoot; }
    const std::string &realRoot() const { return m_catalogPath; }
    const std::string &databaseFile() const { return m_dbFile; }
    bool usesScratch() const { return m_scratch.has_value(); }
    int itemsApplied() const { return m_itemsApplied; }

    // Call after each item that wrote through writeRoot().
    void noteItemApplied() { ++m_itemsApplied; }

    // The database file a format keeps everything in, for any caller.
    static std::string databaseFileFor(const std::string &format, const std::string &catalogPath);

private:
    void commit(bool ok);

    std::string m_format;
    std::string m_catalogPath;
    std::string m_dbFile;
    std::string m_scratchSubdir;
    std::string m_scratchFilename;
    std::string m_writeRoot;
    std::string m_label;
    std::uintmax_t m_existingBytes = 0;
    std::optional<infrastructure::ScratchDirGuard> m_scratch;
    int m_itemsApplied = 0;
    SaveContext &m_ctx;
};

}  // namespace seabass::gui
