// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/analysis_ledger_record.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/long_paths.hpp"
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
namespace paths = infrastructure::paths;

// What this save's changes wrote, by change id, for the record after the
// commit. One per save and stick.
struct AnalysisLedgerRecord
{
    std::map<QString, AnalysisLedgerEntry> byChange;
    // The keys whose entries a change takes out (Restore original
    // analysis: the track is no longer Seabass's analysis).
    std::map<QString, std::string> forgetByChange;
    // The entries a change moves from one key to another (a relink), as
    // the ledger has them when the change's turn comes after the commit,
    // so an entry an earlier change of the same save recorded moves too.
    std::map<QString, std::vector<std::pair<std::string, std::string>>> renameByChange;
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

std::optional<BackupTarget> analysisLedgerBackupTarget(const std::string &stickRoot, const std::string &label)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(pathFromUtf8(ledgerFileOf(stickRoot)), ec);
    if (status.type() != fs::file_type::not_found && status.type() != fs::file_type::regular) {
        return std::nullopt;
    }
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
    entry.gridConfidence = provenance.gridConfidence;
    entry.gridFollowsTempo = provenance.gridFollowsTempo;
    entry.keyMargin = provenance.keyMargin;
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
    const auto written = fs::last_write_time(infrastructure::longPathSafe(pathFromUtf8(file)), ec);
    if (ec) {
        return std::nullopt;
    }
    return infrastructure::stick_backup::toUnixSeconds(written);
}

namespace
{

void registerLedgerRecord(SaveContext &ctx, const std::string &stickRoot, const std::string &label);

AnalysisLedgerRecord &ledgerRecordOf(SaveContext &ctx, const std::string &stickRoot, const std::string &label)
{
    bool created = false;
    AnalysisLedgerRecord &record =
        ctx.sharedForWholeSave<AnalysisLedgerRecord>(recordKeyFor(stickRoot), [&created]() {
            created = true;
            return std::make_unique<AnalysisLedgerRecord>();
        });
    if (created) {
        registerLedgerRecord(ctx, stickRoot, label);
    }
    return record;
}

}  // namespace

void recordAnalysisAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                               AnalysisLedgerEntry entry, const std::string &label)
{
    if (stickRoot.empty() || entry.pathKey.empty()) {
        return;
    }
    // A change applied again in the same save (a retry) notes its entry
    // again: the last one is what it left.
    ledgerRecordOf(ctx, stickRoot, label).byChange[changeId] = std::move(entry);
}

void forgetAnalysisAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                               const std::string &pathKey, const std::string &label)
{
    if (stickRoot.empty() || pathKey.empty()) {
        return;
    }
    ledgerRecordOf(ctx, stickRoot, label).forgetByChange[changeId] = pathKey;
}

void renameAnalysisAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                               const std::string &fromKey, const std::string &toKey, const std::string &label)
{
    if (stickRoot.empty() || fromKey.empty() || toKey.empty() || fromKey == toKey) {
        return;
    }
    ledgerRecordOf(ctx, stickRoot, label).renameByChange[changeId].emplace_back(fromKey, toKey);
}

namespace
{

void registerLedgerRecord(SaveContext &ctx, const std::string &stickRoot, const std::string &label)
{
    const std::string key = recordKeyFor(stickRoot);
    ctx.onAfterCommit([&ctx, key, stickRoot, label](const SaveContext::AfterCommit &after) {
        const AnalysisLedgerRecord *record = ctx.sharedForWholeSaveIfPresent<AnalysisLedgerRecord>(key);
        if (record == nullptr || after.appliedIds.isEmpty()) {
            // Nothing landed, or the save went back: the ledger stays as
            // it was, and so does the stick it describes.
            return;
        }
        const auto landedChange = [&](const QString &changeId) {
            return after.appliedIds.contains(changeId) && !after.skippedIds.contains(changeId);
        };
        // The tracks this save would record or take out, and the entries
        // it would move (a relink): a move is no track analysed.
        std::size_t tracks = 0;
        std::size_t moves = 0;
        for (const QString &changeId : after.appliedIds) {
            if (landedChange(changeId)) {
                tracks += record->forgetByChange.count(changeId) + record->byChange.count(changeId);
                if (const auto renames = record->renameByChange.find(changeId); renames != record->renameByChange.end()) {
                    moves += renames->second.size();
                }
            }
        }
        if (tracks == 0 && moves == 0) {
            return;
        }
        const fs::path root = pathFromUtf8(stickRoot);
        std::string readError;
        infrastructure::local::AnalysisLedgerFault fault = infrastructure::local::AnalysisLedgerFault::None;
        std::string incompleteSince;
        const AnalysisLedger previous =
            infrastructure::local::readAnalysisLedger(root, &readError, &fault, &incompleteSince);
        // A save that only moves entries never replaces, moves aside or
        // warns about a ledger it cannot believe: the file stays exactly as
        // it is, and the log says so.
        if (tracks == 0 && fault != infrastructure::local::AnalysisLedgerFault::None) {
            ctx.log().record("analysis ledger: " + std::string(fault == infrastructure::local::AnalysisLedgerFault::Damaged
                                                                   ? "damaged"
                                                                   : "cannot be read")
                             + " (" + readError + "), so its entries were not moved and it is left as it is");
            return;
        }
        if (fault == infrastructure::local::AnalysisLedgerFault::Unreadable) {
            // What it holds is not known, so it is not replaced: this
            // save's entries go unrecorded, and the save says so.
            ctx.log().record("analysis ledger: " + std::to_string(tracks) + " track(s) not recorded, the ledger left as it is: "
                             + readError);
            throw std::runtime_error(
                "Your changes are saved, but Seabass could not record which tracks it analysed (" + readError
                + "), so it will not know them for its own analyses later. The record on the stick cannot be read, so "
                  "Seabass leaves it as it is and says this at every save. Check the stick for errors on a computer. "
                  "If it stays unreadable, Seabass's command line tool, seabass-cli, can move the record aside, "
                  "deleting nothing, and the next save starts a new one: the command to run is in Seabass's "
                  "documentation on experimental features.");
        }
        // A damaged ledger is never written over: whatever it still holds
        // stays on the stick, moved aside, and a new one starts with this
        // save's tracks, marked as incomplete from now. One that cannot be
        // moved is left as it is, as one that cannot be read.
        std::string movedAside;
        if (fault == infrastructure::local::AnalysisLedgerFault::Damaged) {
            std::string moveError;
            const auto now = std::chrono::system_clock::now();
            const auto aside = infrastructure::local::moveAnalysisLedgerAside(root, now, &moveError, "damaged");
            if (!aside) {
                ctx.log().record("analysis ledger: " + std::to_string(tracks)
                                 + " track(s) not recorded, the damaged ledger left as it is: " + readError + "; "
                                 + moveError);
                throw std::runtime_error(
                    "Your changes are saved, but Seabass could not record which tracks it analysed: the record on the "
                    "stick is damaged (" + readError + ") and could not be moved aside (" + moveError + "). Seabass "
                    "leaves it as it is. Check the stick for errors on a computer.");
            }
            movedAside = pathToUtf8(aside->filename());
            const std::string name = pathToUtf8(paths::stickAnalysisLedger(root).filename());
            incompleteSince = movedAside.rfind(name + ".damaged-", 0) == 0
                                  ? movedAside.substr(name.size() + std::string(".damaged-").size(), 16)
                                  : std::string("unknown");
            ctx.log().record("analysis ledger: the damaged ledger (" + readError + ") moved aside as " + movedAside
                             + "; a new one starts, incomplete since " + incompleteSince);
        }
        const std::int64_t now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        // In the order the changes were applied: where two speak of the
        // same track, the one applied later left it as it is. One change
        // may take one entry out and record another, or move one.
        AnalysisLedger next = previous;
        std::set<std::string> recordedKeys;
        std::size_t taken = 0;
        std::vector<std::string> renamed;
        for (const QString &changeId : after.appliedIds) {
            if (!landedChange(changeId)) {
                continue;
            }
            if (const auto forget = record->forgetByChange.find(changeId); forget != record->forgetByChange.end()) {
                taken += next.erase(forget->second);
                recordedKeys.erase(forget->second);
            }
            if (const auto moves = record->renameByChange.find(changeId); moves != record->renameByChange.end()) {
                for (const auto &[from, to] : moves->second) {
                    const auto entry = next.find(from);
                    if (entry == next.end()) {
                        continue;
                    }
                    // The entry of the name the file had describes the
                    // file: one under the new name is of what was there
                    // before, and gives way.
                    AnalysisLedgerEntry moved = entry->second;
                    moved.pathKey = to;
                    renamed.push_back(from + " to " + to + (next.count(to) > 0 ? ", in place of the entry there" : ""));
                    next.erase(entry);
                    next[to] = moved;
                    if (recordedKeys.erase(from) > 0) {
                        recordedKeys.insert(to);
                    }
                }
            }
            if (const auto entry = record->byChange.find(changeId); entry != record->byChange.end()) {
                next[entry->second.pathKey] = entry->second;
                recordedKeys.insert(entry->second.pathKey);
            }
        }
        for (const std::string &pathKey : recordedKeys) {
            // An entry that says what the ledger says already keeps its
            // time: the same analysis written again is not a new one.
            AnalysisLedgerEntry &entry = next.at(pathKey);
            entry.analysedAtUnix = now;
            if (const auto old = previous.find(pathKey); old != previous.end()) {
                AnalysisLedgerEntry same = entry;
                same.analysedAtUnix = old->second.analysedAtUnix;
                if (same == old->second) {
                    entry = same;
                }
            }
        }
        // An entry that cannot be written as it is stays off the stick,
        // and keeps no other entry off it.
        std::vector<std::string> dropped;
        next = infrastructure::local::writableAnalysisLedger(next, &dropped);
        for (const std::string &why : dropped) {
            ctx.log().record("analysis ledger: an entry left out: " + why);
        }
        // Written only when its text changes, or to replace a damaged file
        // (nothing in it was believed anyway), or to start one where one
        // was moved aside (it records that it is incomplete).
        std::string problem;
        const auto previousText = infrastructure::local::serializeAnalysisLedger(previous, &problem, incompleteSince);
        const auto nextText = infrastructure::local::serializeAnalysisLedger(next, &problem, incompleteSince);
        if (!nextText) {
            throw std::runtime_error("Your changes are saved, but Seabass could not record which tracks it analysed ("
                                     + problem + "), so it will not know them for its own analyses later.");
        }
        std::error_code ec;
        const bool there = fs::exists(fs::symlink_status(paths::stickAnalysisLedger(root), ec));
        if (readError.empty() && there && previousText && *previousText == *nextText) {
            return;
        }
        // No ledger, nothing to record and nothing moved aside: a stick
        // with nothing of Seabass's gets none.
        if (readError.empty() && !there && next.empty() && incompleteSince.empty()) {
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
            &writeError, incompleteSince);
        if (!written) {
            ctx.log().record("analysis ledger: not written: " + writeError);
            if (!movedAside.empty()) {
                throw std::runtime_error(
                    "Your changes are saved. The record of which tracks Seabass analysed was damaged, so Seabass moved "
                    "it aside as " + movedAside + ", deleting nothing, but no new record could be written ("
                    + writeError + "). Until one is, Seabass treats every track it has no record of as one whose "
                    "analysis it cannot tell. Check that the stick has free space and is not write-protected; the "
                    "next save that writes an analysis tries again.");
            }
            throw std::runtime_error(
                "Your changes are saved, but Seabass could not record which tracks it analysed (" + writeError
                + "), so it will not know them for its own analyses later. The record on the stick is as it was. "
                  "Check that the stick has free space and is not write-protected; the next save that writes an "
                  "analysis tries again.");
        }
        std::string renamedWords;
        for (const std::string &one : renamed) {
            renamedWords += "; moved the entry of " + one;
        }
        ctx.log().record("analysis ledger: recorded " + std::to_string(recordedKeys.size()) + " track(s), "
                         + (taken > 0 ? "took " + std::to_string(taken) + " out, " : std::string())
                         + std::to_string(next.size()) + " in all"
                         + (incompleteSince.empty() ? std::string() : ", incomplete since " + incompleteSince)
                         + (movedAside.empty() ? std::string() : ", in a new ledger; the damaged one is " + movedAside)
                         + renamedWords);
        if (!movedAside.empty()) {
            throw std::runtime_error("Your changes are saved. The record of which tracks Seabass analysed was damaged, "
                                     "so Seabass moved it aside as " + movedAside + ", deleting nothing, and started a "
                                     "new one with the tracks of this save. Tracks analysed before are not in the new "
                                     "record, so Seabass no longer tells their analysis for its own.");
        }
    });
}

}  // namespace

}  // namespace seabass::gui
