// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/rekordbox_baseline_ledger.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "seabass_version.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

std::string baselineStickRelativePath(const std::string &stickRoot, const std::string &filePath)
{
    return application::stickRelativePathOf(filePath, stickRoot);
}

std::string baselinePathKey(const std::string &stickRelativePath)
{
    return stickRelativePath.empty() ? std::string() : application::normalizedPathKey(stickRelativePath);
}

std::vector<domain::SeabassWrite> seabassWritesFor(const std::string &stickRoot,
                                                   const std::vector<RekordboxWrite> &writes)
{
    std::vector<domain::SeabassWrite> out;
    for (const auto &write : writes) {
        const std::string key = baselinePathKey(baselineStickRelativePath(stickRoot, write.filePath));
        if (!key.empty()) {
            out.push_back({key, write.cues, write.rating});
        }
    }
    return out;
}

BaselineAtSaveStart readBaselineAtSaveStart(const SaveContext &ctx)
{
    BaselineAtSaveStart start;
    if (ctx.rekordboxPath().isEmpty()) {
        return start;  // a save that cannot write the rekordbox side keeps out of it
    }
    const std::string stickRoot = ctx.stickRoot();
    const fs::path file = infrastructure::paths::stickRekordboxBaseline(pathFromUtf8(stickRoot));
    std::error_code ec;
    if (!fs::exists(file, ec) || ec) {
        return start;
    }
    start.exists = true;
    start.file = pathToUtf8(file);
    start.baselineSequence =
        infrastructure::local::readRekordboxBaselineSequence(pathFromUtf8(stickRoot), &start.error);
    const auto pdb = infrastructure::engine::readRekordboxImportState({}, ctx.rekordboxPath().toStdString());
    if (pdb.hasRekordboxLibrary) {
        start.pdbSequence = pdb.librarySequence;
    }
    return start;
}

std::string baselineWriter()
{
    return std::string("Seabass ") + version::Number;
}

QString recordOriginLedger(SaveContext &ctx, const BaselineAtSaveStart &start, const SaveContext::AfterCommit &after,
                           const std::string &label)
{
    if (!start.exists || after.rekordboxWrites.empty()) {
        return {};
    }
    const QString failed = QStringLiteral(
        "This save's cue and rating writes onto rekordbox could not be recorded in the stick's rekordbox baseline "
        "(%1). Sync after Rekordbox Export will not know Seabass wrote them, and asks rather than restores.");
    const std::string stickRoot = ctx.stickRoot();
    std::string error;
    auto baseline = infrastructure::local::readRekordboxBaseline(pathFromUtf8(stickRoot), &error);
    if (!error.empty()) {
        return failed.arg(QString::fromStdString(error));
    }
    if (!baseline) {
        return {};  // gone since the save started; nothing to merge into
    }
    const auto writes = seabassWritesFor(stickRoot, after.rekordboxWrites);
    const auto unlisted = domain::recordSeabassWrites(*baseline, writes);

    const bool current = start.baselineSequence && start.pdbSequence && *start.baselineSequence == *start.pdbSequence;
    std::string sequenceNote = "sequence left at " + std::to_string(baseline->pdbSequence) + " (not current at save start)";
    if (current) {
        const auto now = infrastructure::engine::readRekordboxImportState({}, ctx.rekordboxPath().toStdString());
        if (!now.hasRekordboxLibrary) {
            return failed.arg(QStringLiteral("export.pdb could not be read after the save"));
        }
        baseline->pdbSequence = now.librarySequence;
        sequenceNote = "sequence now " + std::to_string(now.librarySequence);
    }
    baseline->recordedAtUnix =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    baseline->writer = baselineWriter();
    if (!infrastructure::local::writeRekordboxBaseline(
            pathFromUtf8(stickRoot), *baseline, [&](const std::string &path) { ctx.backupOnce(path, label); },
            &error)) {
        return failed.arg(QString::fromStdString(error));
    }
    ctx.log().record("rekordbox baseline: recorded " + std::to_string(writes.size() - unlisted.size())
                     + " of Seabass's own write(s), " + std::to_string(unlisted.size()) + " for tracks it does not list; "
                     + sequenceNote);
    return {};
}

}  // namespace seabass::gui
