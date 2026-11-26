// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/analysis_ledger_record.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;
using infrastructure::local::AnalysisLedger;
using infrastructure::local::AnalysisLedgerEntry;

// What this save's changes wrote, by change id, for the record after the
// commit. One per save and stick.
struct AnalysisLedgerRecord
{
    std::map<QString, AnalysisLedgerEntry> byChange;
};

std::string recordKeyFor(const std::string &stickRoot)
{
    return "analysis-ledger:" + application::normalizedPathKey(stickRoot);
}

std::string ledgerFileOf(const std::string &stickRoot)
{
    return pathToUtf8(infrastructure::paths::stickAnalysisLedger(pathFromUtf8(stickRoot)));
}

}  // namespace

BackupTarget analysisLedgerBackupTarget(const std::string &stickRoot, const std::string &label)
{
    BackupTarget target;
    target.file = ledgerFileOf(stickRoot);
    target.label = label;
    target.removeOnRestoreIfAbsent = true;
    return target;
}

std::string analysisLedgerKeyOf(const std::string &audioFile, const std::string &stickRoot)
{
    const std::string relative = application::stickRelativePathOf(audioFile, stickRoot);
    return relative.empty() ? std::string() : application::normalizedPathKey(relative);
}

AnalysisLedgerEntry analysisLedgerEntry(const std::string &pathKey, const ImportAnalysis::Provenance &provenance,
                                        std::uint64_t fileSize, std::int64_t fileMtimeUnix,
                                        const std::string &rekordboxGrid, const std::string &engineGrid,
                                        const std::string &key, bool keyWritten)
{
    AnalysisLedgerEntry entry;
    entry.pathKey = pathKey;
    entry.fileSize = fileSize;
    entry.fileMtimeUnix = fileMtimeUnix;
    entry.inputRevision = provenance.inputRevision;
    entry.gridRevision = provenance.gridRevision;
    entry.keyRevision = provenance.keyRevision;
    entry.waveformRevision = provenance.waveformRevision;
    entry.libraryVersion = provenance.libraryVersion;
    entry.tempoMin = provenance.tempoMin;
    entry.tempoMax = provenance.tempoMax;
    entry.rekordboxGrid = rekordboxGrid;
    entry.engineGrid = engineGrid;
    entry.key = key;
    entry.keyWritten = keyWritten && !key.empty();
    return entry;
}

namespace
{

std::function<void(const std::string &)> &beforeLedgerGridRead()
{
    static std::function<void(const std::string &)> hook;
    return hook;
}

}  // namespace

std::string ledgerGridOfAnalysisFile(const std::string &datPath, std::string *problem)
{
    problem->clear();
    try {
        if (beforeLedgerGridRead()) {
            beforeLedgerGridRead()(datPath);
        }
        return infrastructure::rekordbox::pqtzGridFingerprint(infrastructure::rekordbox::AnlzFile::readRaw(datPath));
    } catch (const std::exception &e) {
        *problem = e.what();
        return {};
    }
}

void setBeforeLedgerGridReadForTesting(std::function<void(const std::string &datPath)> hook)
{
    beforeLedgerGridRead() = std::move(hook);
}

std::optional<std::int64_t> fileMtimeUnixOf(const std::string &file)
{
    std::error_code ec;
    const auto written = fs::last_write_time(pathFromUtf8(file), ec);
    if (ec) {
        return std::nullopt;
    }
    return infrastructure::stick_backup::toUnixSeconds(written);
}

void recordAnalysisAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                               AnalysisLedgerEntry entry, const std::string &label)
{
    if (stickRoot.empty() || entry.pathKey.empty()) {
        return;
    }
    const std::string key = recordKeyFor(stickRoot);
    bool created = false;
    AnalysisLedgerRecord &record = ctx.sharedForWholeSave<AnalysisLedgerRecord>(key, [&created]() {
        created = true;
        return std::make_unique<AnalysisLedgerRecord>();
    });
    // A change applied again in the same save (a retry) notes its entry
    // again: the last one is what it left.
    record.byChange[changeId] = std::move(entry);
    if (!created) {
        return;
    }
    ctx.onAfterCommit([&ctx, key, stickRoot, label](const SaveContext::AfterCommit &after) {
        const AnalysisLedgerRecord *record = ctx.sharedForWholeSaveIfPresent<AnalysisLedgerRecord>(key);
        if (record == nullptr || after.appliedIds.isEmpty()) {
            // Nothing landed, or the save went back: the ledger stays as
            // it was, and so does the stick it describes.
            return;
        }
        std::map<std::string, AnalysisLedgerEntry> landed;
        for (const auto &[changeId, entry] : record->byChange) {
            if (after.appliedIds.contains(changeId) && !after.skippedIds.contains(changeId)) {
                landed[entry.pathKey] = entry;
            }
        }
        if (landed.empty()) {
            return;
        }
        const fs::path root = pathFromUtf8(stickRoot);
        std::string readError;
        infrastructure::local::AnalysisLedgerFault fault = infrastructure::local::AnalysisLedgerFault::None;
        const AnalysisLedger previous = infrastructure::local::readAnalysisLedger(root, &readError, &fault);
        if (fault == infrastructure::local::AnalysisLedgerFault::Unreadable) {
            // What it holds is not known, so it is not replaced: this
            // save's entries go unrecorded, and the save says so.
            ctx.log().record("analysis ledger: " + std::to_string(landed.size())
                             + " track(s) not recorded, the ledger left as it is: " + readError);
            throw std::runtime_error("Your changes are saved, but Seabass could not record which tracks it analysed ("
                                     + readError + "), so it will not know them for its own analyses later.");
        }
        const std::int64_t now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        AnalysisLedger next = previous;
        for (auto &[pathKey, entry] : landed) {
            // An entry that says what the ledger says already keeps its
            // time: the same analysis written again is not a new one.
            const auto old = previous.find(pathKey);
            entry.analysedAtUnix = now;
            if (old != previous.end()) {
                AnalysisLedgerEntry same = entry;
                same.analysedAtUnix = old->second.analysedAtUnix;
                if (same == old->second) {
                    entry = same;
                }
            }
            next[pathKey] = entry;
        }
        // An entry that cannot be written as it is stays off the stick,
        // and keeps no other entry off it.
        std::vector<std::string> dropped;
        next = infrastructure::local::writableAnalysisLedger(next, &dropped);
        for (const std::string &why : dropped) {
            ctx.log().record("analysis ledger: an entry left out: " + why);
        }
        // Written only when its text changes, or to replace a damaged file
        // (nothing in it was believed anyway).
        std::string problem;
        const auto previousText = infrastructure::local::serializeAnalysisLedger(previous, &problem);
        const auto nextText = infrastructure::local::serializeAnalysisLedger(next, &problem);
        if (!nextText) {
            throw std::runtime_error("Your changes are saved, but Seabass could not record which tracks it analysed ("
                                     + problem + "), so it will not know them for its own analyses later.");
        }
        if (readError.empty() && previousText && *previousText == *nextText) {
            return;
        }
        std::string writeError;
        const bool written = infrastructure::local::writeAnalysisLedger(
            root, next,
            [&ctx, &label](const std::string &file) {
                BackupTarget target;
                target.file = file;
                target.label = label;
                target.removeOnRestoreIfAbsent = true;
                ctx.backupAllNow({target});
            },
            &writeError);
        if (!written) {
            ctx.log().record("analysis ledger: not written: " + writeError);
            throw std::runtime_error("Your changes are saved, but Seabass could not record which tracks it analysed ("
                                     + writeError + "), so it will not know them for its own analyses later.");
        }
        ctx.log().record("analysis ledger: recorded " + std::to_string(landed.size()) + " track(s), "
                         + std::to_string(next.size()) + " in all"
                         + (readError.empty() ? std::string()
                                              : ", replacing a damaged ledger (" + readError + ")"));
    });
}

}  // namespace seabass::gui
