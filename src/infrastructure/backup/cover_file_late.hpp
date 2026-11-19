// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::backup
{

// A file a command-line run is about to write or remove that its backup
// before the first byte did not foresee (an overview file the check did not
// name, a kept Engine analysis): added to the run's backup record
// (`recordId`) when it is there, noted absent in it when it is not, so that
// restoring the one backup puts back or takes away what the run did. With
// no record yet, a file that is there is backed up in one of its own. A
// file whether there or not cannot be told (exists() fails), or one absent
// with no record to note it in, is a refusal: it throws, and the caller
// writes nothing it could not put back. The record the file went into,
// when it was copied.
inline std::optional<application::BackupRecord> coverFileLate(FilesystemBackupStore &store, const std::string &recordId,
                                                              const std::string &file, const std::string &label)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(pathFromUtf8(file), ec);
    if (ec) {
        throw std::runtime_error("not written: whether " + file + " is there cannot be told (" + ec.message()
                                 + "), so it cannot be backed up first");
    }
    if (exists) {
        return recordId.empty() ? store.backup({file}, label) : store.addToArchive(recordId, {file});
    }
    if (recordId.empty()) {
        throw std::runtime_error("not written: Engine was not backed up first, so " + file
                                 + " could not be noted as new in the backup");
    }
    store.recordAbsent(recordId, {file});
    return std::nullopt;
}

}  // namespace seabass::infrastructure::backup
