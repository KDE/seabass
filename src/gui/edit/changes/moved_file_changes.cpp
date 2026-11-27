// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/moved_file_changes.hpp"

#include <algorithm>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "gui/edit/changes/analysis_ledger_record.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr const char *MoveLabel = "moved-files";
constexpr const char *RelinkLabel = "moved-files-relink";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

std::string basenameOf(const std::string &path)
{
    return pathToUtf8(pathFromUtf8(path).filename());
}

QString nameOf(const domain::MovedFileFinding &finding)
{
    const std::string title = finding.title();
    return QString::fromStdString(title.empty() ? basenameOf(finding.expectedPath) : title);
}

// The catalogs this finding has rows in, as formatsTouched() lists them.
QStringList formatsOf(const domain::MovedFileFinding &finding)
{
    QStringList formats;
    for (const auto &row : finding.rows) {
        const QString format = QString::fromStdString(row.format);
        if (!formats.contains(format)) {
            formats << format;
        }
    }
    return formats;
}

// The ledgers' and kept analyses' key of a file on the stick (its path
// from the stick's root, normalized), empty when it has none.
std::string trackKeyOf(const std::string &file, const std::string &stickRoot)
{
    return stickRoot.empty() ? std::string() : analysisLedgerKeyOf(file, stickRoot);
}

bool existsAt(const std::string &file)
{
    std::error_code ec;
    return !file.empty() && fs::exists(pathFromUtf8(file), ec);
}

// The save's store of the player's kept analyses, the one Beat Grids &
// Waveforms and Analyse again write through (the same key, so one store
// for the save): every file it creates, replaces or removes backed up
// first, or noted absent, and protected for the change.
infrastructure::local::StickKeptEngineAnalysisStore &saveKeptEngineStore(SaveContext &ctx, const std::string &stickRoot)
{
    return ctx.shared<infrastructure::local::StickKeptEngineAnalysisStore>(
        "beat-grids:kept:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<infrastructure::local::StickKeptEngineAnalysisStore>(pathFromUtf8(stickRoot));
            made->setBeforeChange([&ctx](const std::string &file) {
                BackupTarget target;
                target.file = file;
                target.label = RelinkLabel;
                target.removeOnRestoreIfAbsent = true;
                ctx.backupAllNow({target});
                ctx.protectForThisChange(file);
            });
            return made;
        });
}

}  // namespace

QString movedFileChangeId(const std::string &expectedPath)
{
    return "moved-file:" + QString::fromStdString(application::normalizedPathKey(expectedPath));
}

std::string rekordboxPathOnStick(const std::string &pioneerRoot, const std::string &filePath)
{
    std::error_code ec;
    const auto relative = fs::relative(pathFromUtf8(filePath), pathFromUtf8(pioneerRoot).parent_path(), ec);
    return ec || relative.empty() ? filePath : "/" + pathToGenericUtf8(relative);
}

std::vector<std::string> rekordboxLookupSpellings(const std::string &pioneerRoot, const std::string &filePath)
{
    std::vector<std::string> spellings;
    const auto own = pathFromUtf8(filePath).lexically_normal().lexically_relative(
        pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(pioneerRoot)));
    const std::string text = pathToGenericUtf8(own);
    if (!text.empty() && text != "." && text.rfind("..", 0) != 0) {
        spellings.push_back("/" + text);
    }
    const std::string disk = rekordboxPathOnStick(pioneerRoot, filePath);
    if (spellings.empty() || spellings.front() != disk) {
        spellings.push_back(disk);
    }
    return spellings;
}

std::string relinkRefusalFor(const QString &pioneerRoot, const domain::MovedFileFinding &finding,
                             const std::string &newPath)
{
    bool rekordbox = false;
    for (const auto &row : finding.rows) {
        rekordbox = rekordbox || row.format == "rekordbox";
    }
    if (!rekordbox) {
        return {};  // Engine and OneLibrary take any path
    }
    const std::string pioneer = pioneerRoot.toStdString();
    const std::string stored = rekordboxPathOnStick(pioneer, newPath);
    const std::string name = basenameOf(newPath);
    for (unsigned char c : stored) {
        if (c >= 0x80) {
            return "export.pdb takes only plain ASCII in a track's path, and \"" + stored + "\" is not";
        }
    }
    try {
        const infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(pioneer));
        for (const auto &row : finding.rows) {
            if (row.format != "rekordbox") {
                continue;
            }
            const auto capacity = pdb.trackPathCapacity(static_cast<uint32_t>(std::stoul(row.sourceId)));
            if (!capacity) {
                return "this track's path fields in export.pdb cannot be written in place";
            }
            // Exactly, as setTrackFilePath() writes it: see its comment.
            if (stored.size() != capacity->filePath) {
                return infrastructure::rekordbox::trackPathLengthRefusal("path", stored.size(), capacity->filePath);
            }
            if (name.size() != capacity->filename) {
                return infrastructure::rekordbox::trackPathLengthRefusal("file name", name.size(),
                                                                         capacity->filename);
            }
        }
    } catch (const std::exception &e) {
        return std::string("export.pdb could not be read: ") + e.what();
    }
    return {};
}

// ---- Repair A ----------------------------------------------------------

RelocateMovedFileChange::RelocateMovedFileChange(domain::MovedFileFinding finding, domain::Track file)
    : m_finding(std::move(finding)), m_file(std::move(file))
{
}

QString RelocateMovedFileChange::id() const
{
    return movedFileChangeId(m_finding.expectedPath);
}

QString RelocateMovedFileChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RelocateMovedFileChange::description() const
{
    return QStringLiteral("Move \"%1\" back to %2")
        .arg(nameOf(m_finding), QString::fromStdString(m_finding.expectedPath));
}

QString RelocateMovedFileChange::subject() const
{
    return nameOf(m_finding);
}

QString RelocateMovedFileChange::unit() const
{
    return QStringLiteral("files");
}

QString RelocateMovedFileChange::verb() const
{
    return QStringLiteral("moved back");
}

// No catalog is written: the three stay level because the file goes
// where they all already point.
QStringList RelocateMovedFileChange::formatsTouched() const
{
    return {};
}

ChangeOutcome RelocateMovedFileChange::apply(SaveContext &ctx)
{
    const fs::path from = pathFromUtf8(m_file.filePath);
    const fs::path to = pathFromUtf8(m_finding.expectedPath);
    const std::string label = MoveLabel;
    std::error_code ec;
    if (fs::is_regular_file(to, ec)) {
        // Something is there again: the catalogs resolve, whatever it is,
        // and the next scan says whether it is the file.
        ctx.log().record(label + ": a file is at " + m_finding.expectedPath + " again; nothing moved");
        return ChangeOutcome::skip();
    }
    if (fs::exists(to, ec) || ec) {
        return ChangeOutcome::failure(
            QStringLiteral("Something that is not a file is at %1, so the file cannot be moved back there.")
                .arg(QString::fromStdString(m_finding.expectedPath)));
    }
    if (!fs::is_regular_file(from, ec)) {
        ctx.log().record(label + ": " + m_file.filePath + " is no longer there; nothing moved");
        return ChangeOutcome::skip();
    }
    // The file the scan matched, not another put at its path since: the
    // size is what made the match.
    const std::uintmax_t size = fs::file_size(from, ec);
    if (!ec && m_file.fileSizeBytes > 0 && size != m_file.fileSizeBytes) {
        return ChangeOutcome::failure(QStringLiteral("%1 has changed since the scan (%2 bytes, the scan saw %3), "
                                                     "so it is not moved. Run the check again.")
                                          .arg(QString::fromStdString(m_file.filePath))
                                          .arg(static_cast<qulonglong>(size))
                                          .arg(static_cast<qulonglong>(m_file.fileSizeBytes)));
    }
    // The folder the catalogs expect may be the thing that was renamed.
    // Making it again is part of putting the file back; Undo leaves it
    // standing, empty, which no catalog minds.
    if (!fs::is_directory(to.parent_path(), ec)) {
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return ChangeOutcome::failure(QStringLiteral("Could not make the folder %1: %2")
                                              .arg(pathToQString(to.parent_path()),
                                                   QString::fromStdString(ec.message())));
        }
        ctx.log().record(label + ": made the folder " + pathToUtf8(to.parent_path()));
    }
    // Noted before the move (see SaveContext::noteMoved), so Undo and a
    // failed save know to move it back.
    ctx.noteMoved(m_file.filePath, m_finding.expectedPath, label);
    fs::rename(from, to, ec);
    if (ec) {
        return ChangeOutcome::failure(QStringLiteral("Could not move %1 to %2: %3")
                                          .arg(QString::fromStdString(m_file.filePath),
                                               QString::fromStdString(m_finding.expectedPath),
                                               QString::fromStdString(ec.message())));
    }
    ctx.log().record(label + ": moved " + m_file.filePath + " back to " + m_finding.expectedPath + " (\""
                     + m_finding.title() + "\")");
    return ChangeOutcome::success();
}

// ---- Repair B ----------------------------------------------------------

RelinkMovedFileChange::RelinkMovedFileChange(QString pioneerRoot, QString enginePath,
                                             domain::MovedFileFinding finding, domain::Track file)
    : m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)),
      m_finding(std::move(finding)),
      m_file(std::move(file))
{
}

QString RelinkMovedFileChange::id() const
{
    return movedFileChangeId(m_finding.expectedPath);
}

QString RelinkMovedFileChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RelinkMovedFileChange::description() const
{
    return QStringLiteral("Point the %1 entries for \"%2\" at %3")
        .arg(formatsOf(m_finding).join(QStringLiteral(", ")), nameOf(m_finding),
             QString::fromStdString(m_file.filePath));
}

QString RelinkMovedFileChange::subject() const
{
    return nameOf(m_finding);
}

QString RelinkMovedFileChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RelinkMovedFileChange::verb() const
{
    return QStringLiteral("relinked");
}

QStringList RelinkMovedFileChange::formatsTouched() const
{
    return formatsOf(m_finding);
}

bool RelinkMovedFileChange::has(const std::string &format) const
{
    for (const auto &row : m_finding.rows) {
        if (row.format == format) {
            return true;
        }
    }
    return false;
}

std::vector<BackupTarget> RelinkMovedFileChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (has("rekordbox") && !pioneer.empty()) {
        targets.push_back({exportPdbOf(pioneer), RelinkLabel});
    }
    if (has("onelibrary") && !pioneer.empty()) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), RelinkLabel});
    }
    if (has("engine") && !m_enginePath.isEmpty()) {
        targets.push_back({engineDbOf(m_enginePath.toStdString()), RelinkLabel});
    }
    // What Seabass keeps about the track by its path: the kept analyses
    // and the two ledgers' lines, carried to its new name in apply(). Each
    // kept file noted absent where it is not there, so Undo takes a new
    // one away; the ledgers only where they are on the stick.
    const std::string root = stickRoot();
    const std::string newKey = trackKeyOf(m_file.filePath, root);
    if (!newKey.empty()) {
        for (const std::string &oldKey : oldKeys()) {
            for (const std::string &kept :
                 {keptRekordboxAnalysisFileOf(root, oldKey), keptRekordboxAnalysisFileOf(root, newKey),
                  keptRekordboxRecordFileOf(root, oldKey), keptRekordboxRecordFileOf(root, newKey),
                  keptEngineAnalysisFileOf(root, oldKey), keptEngineAnalysisFileOf(root, newKey)}) {
                if (!kept.empty()) {
                    targets.push_back({kept, RelinkLabel, true});
                }
            }
        }
        if (const auto ledger = analysisLedgerBackupTarget(root, RelinkLabel); ledger && existsAt(ledger->file)) {
            targets.push_back(*ledger);
        }
        if (const std::string grids = beatGridLedgerFileOf(root); existsAt(grids)) {
            targets.push_back({grids, RelinkLabel, true});
        }
    }
    return targets;
}

std::string RelinkMovedFileChange::stickRoot() const
{
    const QString any = m_pioneerRoot.isEmpty() ? m_enginePath : m_pioneerRoot;
    return any.isEmpty() ? std::string() : infrastructure::paths::normalStickRootForCatalogPath(any.toStdString());
}

std::vector<std::string> RelinkMovedFileChange::oldKeys() const
{
    const std::string root = stickRoot();
    const std::string newKey = trackKeyOf(m_file.filePath, root);
    std::set<std::string> keys;
    const auto add = [&](const std::string &file) {
        if (const std::string key = trackKeyOf(file, root); !key.empty() && key != newKey) {
            keys.insert(key);
        }
    };
    add(m_finding.expectedPath);
    for (const auto &row : m_finding.rows) {
        add(row.filePath);
    }
    return {keys.begin(), keys.end()};
}

void RelinkMovedFileChange::carryTrackRecords(SaveContext &ctx, bool rekordboxPointed, bool enginePointed,
                                              const std::string &engineWriteRoot, std::vector<std::string> &said)
{
    const std::string root = stickRoot();
    const std::string newKey = trackKeyOf(m_file.filePath, root);
    if (newKey.empty()) {
        return;
    }
    using RekordboxStatus = infrastructure::local::KeptRekordboxAnalysisLookup::Status;
    using RecordStatus = infrastructure::local::KeptRekordboxRecordLookup::Status;
    using EngineStatus = application::KeptEngineAnalysisLookup::Status;
    const std::string stickPath = "/" + application::stickRelativePathOf(m_file.filePath, root);
    // Best effort, each on its own: nothing here fails the relink, which
    // is the repair the person asked for. What cannot be carried stays
    // where it is, and the log says so.
    const auto tryTo = [&](const std::string &what, const std::function<void()> &step) {
        try {
            step();
        } catch (const std::exception &e) {
            said.push_back(what + " could not be carried and stays under the old name (" + e.what() + ")");
        }
    };
    for (const std::string &oldKey : oldKeys()) {
        if (rekordboxPointed) {
            tryTo("rekordbox's kept analysis", [&]() {
                auto &store = saveKeptRekordboxStore(ctx, root);
                const auto found = store.find(oldKey);
                if (found.status == RekordboxStatus::Damaged) {
                    said.push_back("rekordbox's kept analysis under the old name cannot be read and stays there ("
                                   + found.error + ")");
                    return;
                }
                if (found.status != RekordboxStatus::Found) {
                    return;
                }
                if (store.find(newKey).status != RekordboxStatus::None) {
                    // Two kept originals for one name: the one kept for
                    // the name stays, and the other is not deleted.
                    said.push_back("rekordbox's analysis is kept under the new name already, and that one stays; the "
                                   "one kept under the old name stays where it is");
                    return;
                }
                // The pair goes together or not at all: an entry without a
                // record that can be read stays where it is.
                const auto record = store.findRecord(oldKey);
                if (record.status != RecordStatus::Found) {
                    said.push_back("the record beside rekordbox's kept analysis under the old name "
                                   + (record.status == RecordStatus::None ? std::string("is missing")
                                                                          : "cannot be read (" + record.error + ")")
                                   + ", so both stay there");
                    return;
                }
                infrastructure::local::KeptRekordboxAnalysis entry = found.entry;
                entry.pathKey = newKey;
                entry.stickPath = stickPath;
                infrastructure::local::KeptRekordboxRecord next = record.record;
                next.pathKey = newKey;
                store.keep(entry, next);
                store.drop(oldKey);
                said.push_back("rekordbox's kept analysis carried to the new name");
            });
        }
        if (enginePointed) {
            tryTo("the player's kept analysis", [&]() {
                auto &store = saveKeptEngineStore(ctx, root);
                const auto found = store.find(oldKey);
                if (found.status == EngineStatus::Damaged) {
                    said.push_back("the player's kept analysis under the old name cannot be read and stays there ("
                                   + found.error + ")");
                    return;
                }
                if (found.status != EngineStatus::Found) {
                    return;
                }
                if (store.find(newKey).status != EngineStatus::None) {
                    said.push_back("the player's analysis is kept under the new name already, and that one stays; the "
                                   "one kept under the old name stays where it is");
                    return;
                }
                application::KeptEngineAnalysis entry = found.entry;
                entry.pathKey = newKey;
                // The row's Track.path as the relink left it, which the
                // entry is recognised by.
                for (const auto &[id, path] : infrastructure::engine::engineTrackPaths(engineWriteRoot)) {
                    if (id == entry.trackId) {
                        entry.trackFile = path;
                    }
                }
                store.keep(entry);
                store.drop(oldKey);
                said.push_back("the player's kept analysis carried to the new name");
            });
            // The line as the ledger has it when this change's turn comes
            // after the commit, a line this save recorded included.
            renameGridInLedgerAfterCommit(ctx, root, id(), oldKey, newKey);
        }
        renameAnalysisAfterCommit(ctx, root, id(), oldKey, newKey, RelinkLabel);
    }
}

ChangeOutcome RelinkMovedFileChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string engine = m_enginePath.toStdString();
    const std::string label = RelinkLabel;
    const std::string &oldPath = m_finding.expectedPath;
    const std::string &newPath = m_file.filePath;
    const fs::path file = pathFromUtf8(newPath);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        ctx.log().record(label + ": " + newPath + " is no longer there; nothing pointed at it");
        return ChangeOutcome::skip();
    }
    // The file the scan matched, as repair A checks it: the size is what
    // made the match, and a catalog pointed at another file is a track
    // that plays the wrong audio.
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return ChangeOutcome::failure(QStringLiteral("Could not tell whether %1 has changed since the scan (%2), so "
                                                     "no library is pointed at it.")
                                          .arg(QString::fromStdString(newPath), QString::fromStdString(ec.message())));
    }
    if (m_file.fileSizeBytes > 0 && size != m_file.fileSizeBytes) {
        return ChangeOutcome::failure(QStringLiteral("%1 has changed since the scan (%2 bytes, the scan saw %3), "
                                                     "so no library is pointed at it. Run the check again.")
                                          .arg(QString::fromStdString(newPath))
                                          .arg(static_cast<qulonglong>(size))
                                          .arg(static_cast<qulonglong>(m_file.fileSizeBytes)));
    }

    // Each catalog's rows by the spelling of THOSE rows: the finding groups
    // rows by normalizedPathKey, so its own expectedPath is only the first
    // row's spelling, and export.pdb and Engine look a path up byte for
    // byte. How many rows each spelling has, to find every one of them.
    std::map<std::string, std::map<std::string, size_t>> spellings;  // format -> spelling -> rows
    std::set<std::string> oldPaths{oldPath};
    for (const auto &row : m_finding.rows) {
        ++spellings[row.format][row.filePath];
        oldPaths.insert(row.filePath);
    }
    // And the file is still missing where they point: one back there
    // again resolves them already, and pointing them away from it would
    // break a track that works.
    for (const std::string &old : oldPaths) {
        const bool there = fs::exists(pathFromUtf8(old), ec);
        if (there || ec) {
            return ChangeOutcome::failure(QStringLiteral("%1 is there again, so the library is not pointed away from "
                                                         "it. Run the check again.")
                                              .arg(QString::fromStdString(old)));
        }
    }

    // A row the scan listed and the write does not find fails the change,
    // and the save puts back whatever it had written. Carrying on would
    // point some catalogs at the file and leave the others at the old
    // path: the catalogs disagree, and the file, named by a row now, is no
    // longer unreferenced, so the next scan cannot offer it again.
    const auto vanished = [&](const QString &catalog, const std::string &spelling, size_t found, size_t listed) {
        return ChangeOutcome::failure(
            QStringLiteral("%1 no longer has every row the check found for %2 (%3 of %4), so no library is "
                           "pointed at %5: they would disagree. Run the check again.")
                .arg(catalog, QString::fromStdString(spelling))
                .arg(static_cast<qulonglong>(found))
                .arg(static_cast<qulonglong>(listed))
                .arg(QString::fromStdString(newPath)));
    };
    size_t pointed = 0;
    std::vector<std::string> carried;
    std::string engineWriteRoot;
    try {
        if (const auto rows = spellings.find("rekordbox"); rows != spellings.end()) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, label);
            infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(session.writeRoot()));
            const std::string stored = rekordboxPathOnStick(pioneer, newPath);
            const std::string name = basenameOf(newPath);
            for (const auto &[spelling, listed] : rows->second) {
                // By the path, not the id from the scan: a re-export renumbers.
                // The row's own spelling first, then the disk's (see
                // rekordboxLookupSpellings).
                std::vector<uint32_t> ids;
                for (const std::string &onStick : rekordboxLookupSpellings(pioneer, spelling)) {
                    ids = pdb.trackIdsWithFilePath(onStick);
                    if (!ids.empty()) {
                        break;
                    }
                }
                if (ids.size() < listed) {
                    return vanished(QStringLiteral("export.pdb"), spelling, ids.size(), listed);
                }
                for (const uint32_t id : ids) {
                    std::string refusal;
                    if (!pdb.setTrackFilePath(id, name, stored, &refusal)) {
                        if (refusal.empty()) {
                            return vanished(QStringLiteral("export.pdb"), spelling, 0, listed);
                        }
                        return ChangeOutcome::failure(
                            QStringLiteral("export.pdb cannot be pointed at %1: %2. Moving the file back instead "
                                           "works for every catalog.")
                                .arg(QString::fromStdString(newPath), QString::fromStdString(refusal)));
                    }
                    ++pointed;
                }
            }
            if (!pdb.commit()) {
                return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
            }
            session.noteItemApplied();
        }
        if (const auto rows = spellings.find("onelibrary"); rows != spellings.end()) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            for (const auto &[spelling, listed] : rows->second) {
                size_t found = 0;
                try {
                    found = static_cast<size_t>(writer.setTrackPath(spelling, newPath));
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    found = 0;
                }
                if (found < listed) {
                    return vanished(QStringLiteral("OneLibrary"), spelling, found, listed);
                }
                pointed += found;
            }
        }
        if (const auto rows = spellings.find("engine"); rows != spellings.end()) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, label);
            for (const auto &[spelling, listed] : rows->second) {
                const auto found = static_cast<size_t>(std::max(
                    0, infrastructure::engine::setEngineTrackPath(session.writeRoot(), session.realRoot(), spelling,
                                                                  newPath)));
                if (found < listed) {
                    return vanished(QStringLiteral("Engine"), spelling, found, listed);
                }
                pointed += found;
            }
            session.noteItemApplied();
            engineWriteRoot = session.writeRoot();
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    if (pointed == 0) {
        return ChangeOutcome::skip();
    }
    // What Seabass keeps of the track by its path goes with it, as far as
    // it can: never at the cost of the relink.
    carryTrackRecords(ctx, spellings.count("rekordbox") > 0 || spellings.count("onelibrary") > 0,
                      spellings.count("engine") > 0, engineWriteRoot, carried);
    std::string carriedWords;
    for (const std::string &one : carried) {
        carriedWords += "; " + one;
    }
    ctx.log().record(label + ": pointed " + std::to_string(pointed) + " row(s) for \"" + m_finding.title() + "\" at "
                     + newPath + " (was " + oldPath + ")" + carriedWords);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
