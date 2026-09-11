// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/format_write_session.hpp"

#include <chrono>
#include <stdexcept>
#include <system_error>

#include "infrastructure/bulk_write_strategy.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

std::string FormatWriteSession::databaseFileFor(const std::string &format, const std::string &catalogPath)
{
    if (format == "engine") {
        return (fs::path(catalogPath) / "Database2" / "m.db").generic_string();
    }
    if (format == "onelibrary") {
        return infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(catalogPath);
    }
    return (fs::path(catalogPath) / "rekordbox" / "export.pdb").generic_string();
}

FormatWriteSession::FormatWriteSession(std::string format, std::string catalogPath, int itemCountHint,
                                       std::string label, SaveContext &ctx)
    : m_format(std::move(format)),
      m_catalogPath(std::move(catalogPath)),
      m_label(std::move(label)),
      m_ctx(ctx)
{
    m_dbFile = databaseFileFor(m_format, m_catalogPath);
    if (m_format == "engine") {
        m_scratchSubdir = "Database2";
        m_scratchFilename = "m.db";
    } else if (m_format == "onelibrary") {
        m_scratchSubdir = "rekordbox";
        m_scratchFilename = "exportLibrary.db";
    } else {
        m_scratchSubdir = "rekordbox";
        m_scratchFilename = "export.pdb";
    }
    m_writeRoot = m_catalogPath;

    // The one backup of the database file for this whole save, whichever
    // change asked first.
    m_ctx.backupOnce(m_dbFile, m_label);

    std::error_code sizeEc;
    m_existingBytes = fs::file_size(m_dbFile, sizeEc);
    infrastructure::BulkWriteStrategyInputs inputs;
    inputs.itemCount = itemCountHint;
    inputs.existingFileBytes = sizeEc ? 0 : m_existingBytes;
    bool useWholeFile = !sizeEc && infrastructure::shouldUseWholeFileReplace(inputs)
        && infrastructure::hasRoomForWholeFileReplace(fs::path(m_dbFile).parent_path(), m_existingBytes);

    if (useWholeFile) {
        fs::path scratchDir = fs::temp_directory_path()
            / ("seabass-" + m_label + "-scratch-" + m_format + "-"
               + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code cleanupEc;
        fs::remove_all(scratchDir, cleanupEc);
        fs::create_directories(scratchDir / m_scratchSubdir);
        fs::copy_file(m_dbFile, scratchDir / m_scratchSubdir / m_scratchFilename);
        m_scratch.emplace(scratchDir);
        m_writeRoot = scratchDir.string();
        m_ctx.log().record(m_label + ": applying up to " + std::to_string(itemCountHint) + " " + m_format
                           + " update(s) to a local scratch copy first (" + m_scratchFilename + " is "
                           + std::to_string(m_existingBytes) + " bytes)");
    }

    m_ctx.onFinish([this](bool ok) { commit(ok); });
}

void FormatWriteSession::commit(bool ok)
{
    if (!m_scratch) {
        return;
    }
    if (!ok && m_itemsApplied == 0) {
        m_ctx.log().record(m_label + ": nothing was applied to the " + m_format + " scratch copy; discarding it");
        return;
    }
    fs::path scratchFile = m_scratch->path / m_scratchSubdir / m_scratchFilename;
    if (!infrastructure::copyFileDurablyAtomic(scratchFile.string(), m_dbFile)) {
        m_ctx.log().record(m_label + ": FAILED to commit the " + m_format + " scratch copy back onto the stick");
        throw std::runtime_error(m_label + ": failed to commit the scratch-built " + m_format
                                 + " database back onto the stick");
    }
    m_ctx.log().record(m_label + ": committed the " + m_format + " scratch copy ("
                       + std::to_string(m_itemsApplied) + " update(s)) back onto the stick");
}

}  // namespace seabass::gui
