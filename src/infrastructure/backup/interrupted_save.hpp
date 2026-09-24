// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::backup
{

// A save names its backup records here, in the stick's backups folder,
// before it writes a single catalog file, and the note is removed once the
// save has finished. A note still there when a session opens means the last
// save never finished -- the stick was pulled, or Seabass stopped -- and
// those records are what Undo Last Save puts back.
//
// Undo used to know only the backups of a save made by the same session,
// in memory. Found in the macOS round 8 manual check P3 (issue #48): after
// a pull mid-sync, Discard and a fresh look at the page, the stick was
// half-written, its backup was complete on the stick, and Undo was not
// offered anywhere.
inline std::filesystem::path interruptedSaveNote(const std::string &backupDir)
{
    return pathFromUtf8(backupDir) / ".save-in-progress";
}

// One record id per line. Rewritten whole, durably, each time the save
// makes another record, so the note never names fewer records than exist.
inline bool noteSaveInProgress(const std::string &backupDir, const std::vector<std::string> &recordIds)
{
    std::string text;
    for (const std::string &id : recordIds) {
        text += id;
        text += '\n';
    }
    return writeFileDurablyAtomic(pathToUtf8(interruptedSaveNote(backupDir)), text);
}

inline std::vector<std::string> interruptedSaveRecords(const std::string &backupDir)
{
    std::vector<std::string> ids;
    std::ifstream in(interruptedSaveNote(backupDir));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            ids.push_back(line);
        }
    }
    return ids;
}

inline void clearSaveInProgress(const std::string &backupDir)
{
    std::error_code ec;
    std::filesystem::remove(interruptedSaveNote(backupDir), ec);
}

}  // namespace seabass::infrastructure::backup
