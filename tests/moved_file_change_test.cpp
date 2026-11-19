// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The two repairs of a moved file (#103), through the real save loop.
//
//   moved_file_change_test <tests/fixtures/anonymized_library>
//
// Repair A moves the file back where the catalogs expect it: no catalog is
// written, Undo moves it back again, and a save that fails after it moves
// it back too. Repair B points every catalog's row at the file where it
// is, on a copy of the fixture, and its Undo puts the rows back.

#include <QString>
#include <QStringList>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "application/path_key.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/moved_files.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/moved_file_changes.hpp"
#include "gui/edit/changes/restore_backups_change.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "fixture_copy.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

std::string read(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

class ScriptedChange : public PendingChange
{
public:
    ScriptedChange(QString id, std::function<ChangeOutcome(SaveContext &)> run)
        : m_id(std::move(id)), m_run(std::move(run))
    {
    }
    QString id() const override { return m_id; }
    QString description() const override { return "apply " + m_id; }
    QString unit() const override { return "tracks"; }
    QStringList formatsTouched() const override { return {"rekordbox"}; }
    ChangeOutcome apply(SaveContext &ctx) override { return m_run(ctx); }

private:
    QString m_id;
    std::function<ChangeOutcome(SaveContext &)> m_run;
};

struct ScratchStick
{
    fs::path path;
    explicit ScratchStick(fs::path p) : path(std::move(p)) { fs::remove_all(path); }
    ~ScratchStick()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchStick(const ScratchStick &) = delete;
    ScratchStick &operator=(const ScratchStick &) = delete;
};

domain::Track row(std::string format, std::string id, std::string path, std::string title)
{
    domain::Track t;
    t.format = std::move(format);
    t.sourceId = std::move(id);
    t.filePath = std::move(path);
    t.title = std::move(title);
    return t;
}

domain::Track fileAt(const fs::path &path)
{
    domain::Track t;
    t.format = "disk";
    t.isUnreferenced = true;
    t.sourceId = pathToUtf8(path);
    t.filePath = pathToUtf8(path);
    t.filename = pathToUtf8(path.filename());
    std::error_code ec;
    t.fileSizeBytes = fs::file_size(path, ec);
    return t;
}

SaveLoopResult save(const std::vector<std::shared_ptr<PendingChange>> &changes, const QString &pioneer,
                    const QString &engine)
{
    CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), {}, pioneer, engine);
    return runSaveLoop(changes, ctx);
}

std::string key(const std::string &path)
{
    return application::normalizedPathKey(path);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: moved_file_change_test <tests/fixtures/anonymized_library>\n";
        return 1;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);

    // 1. Repair A: the file goes back, no catalog is touched, the save's
    //    record notes the move, and Undo (the record restored) moves it
    //    back again; undoing the Undo moves it back once more.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_relocate");
        const fs::path &stick = scratch.path;
        const fs::path pioneer = stick / "PIONEER";
        const fs::path pdb = pioneer / "rekordbox" / "export.pdb";
        const fs::path expected = stick / "Contents" / "Old" / "Track.mp3";
        const fs::path moved = stick / "Contents" / "New" / "Track.mp3";
        write(pdb, "pdb-original");
        write(moved, "audio bytes");

        domain::MovedFileFinding finding;
        finding.expectedPath = pathToUtf8(expected);
        finding.rows = {row("rekordbox", "7", pathToUtf8(expected), "Track"),
                        row("engine", "70", pathToUtf8(expected), "Track")};
        finding.candidates.push_back({fileAt(moved), domain::MovedFileCandidate::Confidence::High, "the same"});
        assert(finding.oneClick());

        auto change = std::make_shared<RelocateMovedFileChange>(finding, finding.candidates[0].file);
        assert(change->id() == movedFileChangeId(pathToUtf8(expected)));
        assert(change->formatsTouched().isEmpty() && "no catalog is written");
        const auto result = save({change}, pathToQString(pioneer), {});
        assert(result.error.isEmpty() && result.appliedIds == QStringList{change->id()});
        assert(result.skippedIds.isEmpty());
        assert(fs::is_regular_file(expected) && !fs::exists(moved));
        assert(read(expected) == "audio bytes");
        assert(read(pdb) == "pdb-original");
        assert(result.backups.size() == 1 && "one record, holding the move");

        infrastructure::backup::FilesystemBackupStore store(result.backups[0].backupDir.toStdString());
        const auto records = store.list();
        assert(records.size() == 1 && records[0].id == result.backups[0].id.toStdString());
        assert(records[0].filePaths.empty() && records[0].movedPaths.size() == 1);
        assert(key(records[0].movedPaths[0].first) == key(pathToUtf8(moved)));
        assert(key(records[0].movedPaths[0].second) == key(pathToUtf8(expected)));
        assert(store.isRestorable(records[0].id));

        // Undo, as the session runs it.
        const auto undone = save({std::make_shared<RestoreBackupsChange>(result.backups)}, pathToQString(pioneer), {});
        assert(undone.error.isEmpty());
        assert(fs::is_regular_file(moved) && !fs::exists(expected) && "undo moved the file back");
        assert(read(moved) == "audio bytes");
        // The restore kept a record of its own move (the undo's own store
        // made it), so the restore can itself be undone.
        std::string preRestore;
        for (const auto &record : store.list()) {
            if (record.label == "pre-restore") {
                assert(record.movedPaths.size() == 1 && record.filePaths.empty());
                assert(key(record.movedPaths[0].first) == key(pathToUtf8(expected)));
                assert(key(record.movedPaths[0].second) == key(pathToUtf8(moved)));
                preRestore = record.id;
            }
        }
        assert(!preRestore.empty() && "the undo noted its move the other way round");
        assert(store.restore(preRestore));
        assert(fs::is_regular_file(expected) && !fs::exists(moved) && "undoing the undo moved it back again");
        std::cout << "case 1 (repair A moves the file back, writes no catalog, and undoes as a move) OK\n";
    }

    // 2. A save that fails after the move puts the file back where it
    //    was: the save lands whole or not at all.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_rollback");
        const fs::path &stick = scratch.path;
        const fs::path pioneer = stick / "PIONEER";
        const fs::path expected = stick / "Contents" / "Old" / "Track.mp3";
        const fs::path moved = stick / "Contents" / "New" / "Track.mp3";
        write(pioneer / "rekordbox" / "export.pdb", "pdb");
        write(moved, "audio bytes");
        domain::MovedFileFinding finding;
        finding.expectedPath = pathToUtf8(expected);
        finding.rows = {row("rekordbox", "7", pathToUtf8(expected), "Track")};
        finding.candidates.push_back({fileAt(moved), domain::MovedFileCandidate::Confidence::High, "the same"});
        bool movedWhenTheNextChangeRan = false;
        const auto result =
            save({std::make_shared<RelocateMovedFileChange>(finding, finding.candidates[0].file),
                  std::make_shared<ScriptedChange>("b", [&](SaveContext &) {
                      movedWhenTheNextChangeRan = fs::is_regular_file(expected);
                      return ChangeOutcome::failure("the next change refused");
                  })},
                 pathToQString(pioneer), {});
        assert(movedWhenTheNextChangeRan && "the move had landed when the next change ran");
        assert(result.rolledBack && result.failedId == "b");
        assert(fs::is_regular_file(moved) && !fs::exists(expected) && "the failed save moved the file back");
        std::cout << "case 2 (a save that fails after the move moves the file back) OK\n";
    }

    // 3. The stick changed since the scan: the file gone from where the
    //    scan found it is a skip; a file of another size there is a
    //    failure, since the size is what made the match.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_changed");
        const fs::path &stick = scratch.path;
        const fs::path pioneer = stick / "PIONEER";
        const fs::path expected = stick / "Contents" / "Old" / "Track.mp3";
        const fs::path moved = stick / "Contents" / "New" / "Track.mp3";
        write(pioneer / "rekordbox" / "export.pdb", "pdb");
        write(moved, "audio bytes");
        domain::MovedFileFinding finding;
        finding.expectedPath = pathToUtf8(expected);
        finding.rows = {row("rekordbox", "7", pathToUtf8(expected), "Track")};
        finding.candidates.push_back({fileAt(moved), domain::MovedFileCandidate::Confidence::High, "the same"});
        const domain::Track file = finding.candidates[0].file;

        write(moved, "audio bytes, and more");
        auto result = save({std::make_shared<RelocateMovedFileChange>(finding, file)}, pathToQString(pioneer), {});
        assert(!result.error.isEmpty() && result.error.contains("changed since the scan"));
        assert(fs::is_regular_file(moved) && !fs::exists(expected));

        fs::remove(moved);
        result = save({std::make_shared<RelocateMovedFileChange>(finding, file)}, pathToQString(pioneer), {});
        assert(result.error.isEmpty() && result.skippedIds.size() == 1);
        std::cout << "case 3 (a file changed since the scan is refused; one gone is a skip) OK\n";
    }

    // 4. Repair B on a copy of the fixture: every catalog's row for a
    //    track pointed at the file where it is now, read back through the
    //    readers, and Undo puts every row back. The fixture's Engine
    //    library names no file its rekordbox export names (its paths were
    //    anonymized apart), so the Engine row is a finding of its own in
    //    the same save.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_relink");
        const fs::path &stick = scratch.path;
        const fs::path pioneer = stick / "PIONEER";
        const fs::path engine = stick / "Engine Library";
        std::error_code ec;
        seabass::testing::copyPioneerFixture(fixture / "rekordbox", pioneer, ec);
        assert(!ec && "the rekordbox side of the fixture copies");
        fs::copy(fixture / "engine", engine, fs::copy_options::recursive, ec);
        assert(!ec && "the Engine side of the fixture copies");

        const auto readAll = [&]() {
            std::vector<domain::Track> rows;
            infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(pathToUtf8(pioneer));
            for (auto &t : application::ScanLibrary(rekordbox).execute()) {
                rows.push_back(std::move(t));
            }
            infrastructure::onelibrary::OneLibraryReader oneLibrary(pathToUtf8(pioneer));
            for (auto &t : application::ScanLibrary(oneLibrary).execute()) {
                rows.push_back(std::move(t));
            }
            infrastructure::engine::LibdjinteropEngineReader engineReader(pathToUtf8(engine));
            for (auto &t : application::ScanLibrary(engineReader).execute()) {
                rows.push_back(std::move(t));
            }
            return rows;
        };
        const std::vector<domain::Track> before = readAll();

        // The finding for one path: every row naming it, from whichever
        // catalogs do.
        const auto findingFor = [&](const std::string &path) {
            domain::MovedFileFinding finding;
            finding.expectedPath = path;
            for (const auto &r : before) {
                if (!r.filePath.empty() && key(r.filePath) == key(path)) {
                    finding.rows.push_back(r);
                }
            }
            return finding;
        };
        const auto formatsOf = [](const domain::MovedFileFinding &finding) {
            std::set<std::string> formats;
            for (const auto &r : finding.rows) {
                formats.insert(r.format);
            }
            return formats;
        };

        // A track both rekordbox catalogs list, whose path has room for a
        // shorter one: the relink keeps export.pdb's span, so the new path
        // must fit in it, and a path with the stick's own folders in it
        // does. And an Engine row, whatever its path.
        domain::MovedFileFinding rekordboxFinding;
        domain::MovedFileFinding engineFinding;
        fs::path rekordboxFile;
        fs::path engineFile;
        for (const auto &candidate : before) {
            if (candidate.filePath.empty()) {
                continue;
            }
            if (candidate.format == "rekordbox" && rekordboxFinding.rows.empty()) {
                domain::MovedFileFinding attempt = findingFor(candidate.filePath);
                const auto formats = formatsOf(attempt);
                const fs::path at = stick / "Contents" / "M" / pathFromUtf8(candidate.filePath).filename();
                if (formats.contains("rekordbox") && formats.contains("onelibrary")
                    && relinkRefusalFor(pathToQString(pioneer), attempt, pathToUtf8(at)).empty()) {
                    rekordboxFinding = std::move(attempt);
                    rekordboxFile = at;
                }
            } else if (candidate.format == "engine" && engineFinding.rows.empty()) {
                engineFinding = findingFor(candidate.filePath);
                engineFile = stick / "Contents" / "E" / pathFromUtf8(candidate.filePath).filename();
            }
        }
        assert(rekordboxFinding.rows.size() == 2 && "a track both rekordbox catalogs list, with room");
        assert(engineFinding.rows.size() == 1 && "an Engine row");
        write(rekordboxFile, "audio bytes");
        write(engineFile, "other audio bytes");
        rekordboxFinding.candidates.push_back(
            {fileAt(rekordboxFile), domain::MovedFileCandidate::Confidence::High, "the same"});
        engineFinding.candidates.push_back({fileAt(engineFile), domain::MovedFileCandidate::Confidence::High, "the same"});
        const std::string oldRekordboxPath = rekordboxFinding.expectedPath;
        const std::string newRekordboxPath = pathToUtf8(rekordboxFile);
        const std::string oldEnginePath = engineFinding.expectedPath;
        const std::string newEnginePath = pathToUtf8(engineFile);

        // A longer path, or one that is not ASCII, is refused before
        // anything is staged; the Engine row takes any path.
        {
            const fs::path tooLong = stick / "Contents" / std::string(300, 'x') / rekordboxFile.filename();
            const std::string refusal =
                relinkRefusalFor(pathToQString(pioneer), rekordboxFinding, pathToUtf8(tooLong));
            assert(refusal.find("characters") != std::string::npos && "a longer path is refused, with the reason");
            const std::string notAscii = relinkRefusalFor(pathToQString(pioneer), rekordboxFinding,
                                                          pathToUtf8(stick / "Contents" / "Caf\xC3\xA9.mp3"));
            assert(notAscii.find("ASCII") != std::string::npos);
            assert(relinkRefusalFor(pathToQString(pioneer), engineFinding, pathToUtf8(tooLong)).empty());
        }

        auto rekordboxChange = std::make_shared<RelinkMovedFileChange>(
            pathToQString(pioneer), pathToQString(engine), rekordboxFinding, rekordboxFinding.candidates[0].file);
        auto engineChange = std::make_shared<RelinkMovedFileChange>(pathToQString(pioneer), pathToQString(engine),
                                                                    engineFinding, engineFinding.candidates[0].file);
        assert(rekordboxChange->id() == movedFileChangeId(oldRekordboxPath)
               && "the same id as repair A, so one replaces the other");
        assert(rekordboxChange->formatsTouched() == (QStringList{"rekordbox", "onelibrary"}));
        assert(engineChange->formatsTouched() == QStringList{"engine"});
        const auto result =
            save({rekordboxChange, engineChange}, pathToQString(pioneer), pathToQString(engine));
        if (!result.error.isEmpty()) {
            std::cerr << "save failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && result.skippedIds.isEmpty());
        assert(result.appliedIds == (QStringList{rekordboxChange->id(), engineChange->id()}));

        const std::vector<domain::Track> after = readAll();
        int pointed = 0;
        int enginePointed = 0;
        for (const auto &r : after) {
            if (key(r.filePath) == key(newRekordboxPath)) {
                ++pointed;
                assert(r.filename == pathToUtf8(rekordboxFile.filename()) && "the filename follows the path");
            }
            if (key(r.filePath) == key(newEnginePath)) {
                ++enginePointed;
                assert(r.format == "engine");
            }
            assert(key(r.filePath) != key(oldRekordboxPath) && "no row names the old path any more");
            assert(key(r.filePath) != key(oldEnginePath));
        }
        assert(pointed == 2 && "both rekordbox catalogs' rows point at the new path");
        assert(enginePointed == 1 && "and Engine's at its own");
        assert(fs::is_regular_file(rekordboxFile) && fs::is_regular_file(engineFile) && "the files were not moved");

        const auto undone = save({std::make_shared<RestoreBackupsChange>(result.backups)}, pathToQString(pioneer),
                                 pathToQString(engine));
        assert(undone.error.isEmpty());
        int restored = 0;
        int engineRestored = 0;
        for (const auto &r : readAll()) {
            restored += key(r.filePath) == key(oldRekordboxPath) ? 1 : 0;
            engineRestored += key(r.filePath) == key(oldEnginePath) ? 1 : 0;
            assert(key(r.filePath) != key(newRekordboxPath) && key(r.filePath) != key(newEnginePath));
        }
        assert(restored == 2 && engineRestored == 1 && "undo put every row back");
        std::cout << "case 4 (repair B points every catalog at the file, and undoes) OK\n";
    }

    std::cout << "moved_file_change_test: all cases OK\n";
    return 0;
}
