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
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
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
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "engine_change_fixture.hpp"
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

// A track row's file_path and filename as export.pdb stores them, read
// through the generated parser WITHOUT the readers' trimming: what a
// player reads, padding and all.
struct RawPdbPath
{
    std::string filename;
    std::string filePath;
};

std::string rawText(rekordbox_pdb_t::device_sql_string_t *s)
{
    auto *body = s->body();
    if (auto *a = dynamic_cast<rekordbox_pdb_t::device_sql_short_ascii_t *>(body)) {
        return a->text();
    }
    if (auto *a = dynamic_cast<rekordbox_pdb_t::device_sql_long_ascii_t *>(body)) {
        return a->text();
    }
    if (auto *a = dynamic_cast<rekordbox_pdb_t::device_sql_long_utf16le_t *>(body)) {
        return a->text();
    }
    return "";
}

std::optional<RawPdbPath> rawPdbPath(const fs::path &pdb, uint32_t trackId)
{
    std::ifstream in(pdb, std::ios::binary);
    kaitai::kstream ks(&in);
    rekordbox_pdb_t parsed(false, &ks);
    std::optional<RawPdbPath> out;
    for (const auto &table : *parsed.tables()) {
        if (table->type() != rekordbox_pdb_t::PAGE_TYPE_TRACKS) {
            continue;
        }
        infrastructure::rekordbox::forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *t = dynamic_cast<rekordbox_pdb_t::track_row_t *>(row->body());
                    if (t != nullptr && t->id() == trackId) {
                        out = RawPdbPath{rawText(t->filename()), rawText(t->file_path())};
                    }
                }
            }
        });
    }
    return out;
}

// `path` with its folder spelled in another letter case, as another
// catalog may spell the same file on a FAT or exFAT stick.
std::string withFolderCase(const std::string &path, bool upper)
{
    const fs::path p = pathFromUtf8(path);
    std::string folder = pathToUtf8(p.parent_path().filename());
    for (char &c : folder) {
        c = static_cast<char>(upper ? std::toupper(static_cast<unsigned char>(c))
                                    : std::tolower(static_cast<unsigned char>(c)));
    }
    return pathToUtf8(p.parent_path().parent_path() / folder / p.filename());
}

// A catalog row's path written as given, with no filesystem in between:
// the writers spell a path the way the disk does once its folder exists,
// and a test of other spellings must not depend on whether it does yet.
void spellEngineRow(const fs::path &engine, const std::string &id, const std::string &relative)
{
    const std::string db = pathToUtf8(engine / "Database2" / "m.db");
    const std::string name = pathToUtf8(pathFromUtf8(relative).filename());
    assert(relative.find('\'') == std::string::npos && name.find('\'') == std::string::npos);
    assert(seabass::testing::sqlExec(db, "UPDATE Track SET path = '" + relative + "', filename = '" + name
                                             + "' WHERE id = " + id + ";"));
    assert(seabass::testing::sqlScalar(db, "SELECT count(*) FROM Track WHERE id = " + id + " AND path = '" + relative
                                               + "';")
           == 1);
}

// The OneLibrary rows at `stickPath` ("/Contents/...") moved to `newPath`,
// as given. Returns how many.
int spellOneLibraryRows(const fs::path &pioneer, const std::string &stickPath, const std::string &newPath)
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db(
        lib, infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer)), false);
    db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    int rows = 0;
    {
        infrastructure::onelibrary::SqlCipherStatement count(db, "SELECT count(*) FROM content WHERE path = ?");
        count.bindText(1, stickPath);
        if (count.step()) {
            rows = static_cast<int>(count.columnInt64(0));
        }
    }
    infrastructure::onelibrary::SqlCipherStatement update(db, "UPDATE content SET path = ? WHERE path = ?");
    update.bindText(1, newPath);
    update.bindText(2, stickPath);
    update.run();
    return rows;
}

// A copy of the fixture as a stick: rekordbox, OneLibrary and Engine.
struct FixtureStick
{
    ScratchStick scratch;
    fs::path stick;
    fs::path pioneer;
    fs::path engine;
    FixtureStick(const fs::path &fixture, const std::string &name)
        : scratch(seabass::testing::scratchRoot() / name),
          stick(scratch.path),
          pioneer(stick / "PIONEER"),
          engine(stick / "Engine Library")
    {
        std::error_code ec;
        seabass::testing::copyPioneerFixture(fixture / "rekordbox", pioneer, ec);
        assert(!ec && "the rekordbox side of the fixture copies");
        fs::copy(fixture / "engine", engine, fs::copy_options::recursive, ec);
        assert(!ec && "the Engine side of the fixture copies");
    }
    std::vector<domain::Track> readAll() const
    {
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
    }
    // Every row's format, id and path, to compare before and after.
    std::set<std::string> snapshot() const
    {
        std::set<std::string> out;
        for (const auto &r : readAll()) {
            out.insert(r.format + "|" + r.sourceId + "|" + r.filePath + "|" + r.filename);
        }
        return out;
    }
    // A track both rekordbox catalogs list, the finding for its path, and
    // a place for its file that export.pdb can take: a path and a file
    // name of exactly the lengths its fields hold, since a relink writes
    // them exactly or not at all. The fixture's fields are longer than
    // the paths in them (the anonymizer padded its placeholders), so the
    // new name and folder are drawn out to fill them.
    domain::MovedFileFinding relinkableRekordboxFinding(fs::path &newFile) const
    {
        const auto rows = readAll();
        const infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        for (const auto &candidate : rows) {
            if (candidate.format != "rekordbox" || candidate.filePath.empty()) {
                continue;
            }
            domain::MovedFileFinding finding;
            finding.expectedPath = candidate.filePath;
            std::set<std::string> formats;
            for (const auto &r : rows) {
                if (!r.filePath.empty() && key(r.filePath) == key(candidate.filePath)) {
                    finding.rows.push_back(r);
                    formats.insert(r.format);
                }
            }
            if (!formats.contains("onelibrary") || finding.rows.size() != 2) {
                continue;
            }
            const auto capacity = pdb.trackPathCapacity(static_cast<uint32_t>(std::stoul(candidate.sourceId)));
            const fs::path old = pathFromUtf8(candidate.filePath);
            const std::string stem = pathToUtf8(old.stem());
            const std::string extension = pathToUtf8(old.extension());
            if (!capacity || capacity->filename < stem.size() + extension.size()) {
                continue;
            }
            const std::string name =
                stem + std::string(capacity->filename - stem.size() - extension.size(), 'x') + extension;
            const size_t fixed = std::string("/Contents//").size() + name.size();
            if (capacity->filePath <= fixed) {
                continue;
            }
            const fs::path at = stick / "Contents" / std::string(capacity->filePath - fixed, 'M') / name;
            if (relinkRefusalFor(pathToQString(pioneer), finding, pathToUtf8(at)).empty()) {
                newFile = at;
                return finding;
            }
            if (std::getenv("MOVED_FILE_DEBUG") != nullptr) {
                std::cerr << candidate.filePath << " -> " << pathToUtf8(at) << ": "
                          << relinkRefusalFor(pathToQString(pioneer), finding, pathToUtf8(at)) << "\n";
            }
        }
        return {};
    }
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: moved_file_change_test <tests/fixtures/anonymized_library>\n";
        return 1;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    // MOVED_FILE_CASE=<n> runs that case alone, to watch one fail.
    const auto runs = [](int n) {
        const char *only = std::getenv("MOVED_FILE_CASE");
        return only == nullptr || std::atoi(only) == n;
    };

    // 1. Repair A: the file goes back, no catalog is touched, the save's
    //    record notes the move, and Undo (the record restored) moves it
    //    back again; undoing the Undo moves it back once more.
    if (runs(1)) {
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
    if (runs(2)) {
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
    if (runs(3)) {
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
    //    readers and, in export.pdb, byte for byte, and Undo puts every
    //    row back. The fixture's Engine library names no file its
    //    rekordbox export names (its paths were anonymized apart), so the
    //    Engine row is a finding of its own in the same save.
    if (runs(4)) {
        const FixtureStick f(fixture, "seabass_moved_file_relink");
        const fs::path &stick = f.stick;
        const fs::path &pioneer = f.pioneer;
        const fs::path &engine = f.engine;
        const std::vector<domain::Track> before = f.readAll();

        // A track both rekordbox catalogs list, pointed at a path of
        // exactly its old length: export.pdb's fields keep their span, and
        // a path is written exactly or not at all. And an Engine row,
        // whatever its path.
        fs::path rekordboxFile;
        domain::MovedFileFinding rekordboxFinding = f.relinkableRekordboxFinding(rekordboxFile);
        assert(rekordboxFinding.rows.size() == 2 && "a track both rekordbox catalogs list, with a same-length path");
        domain::MovedFileFinding engineFinding;
        fs::path engineFile;
        for (const auto &candidate : before) {
            if (candidate.format == "engine" && !candidate.filePath.empty()) {
                engineFinding.expectedPath = candidate.filePath;
                engineFinding.rows = {candidate};
                engineFile = stick / "Contents" / "E" / pathFromUtf8(candidate.filePath).filename();
                break;
            }
        }
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

        // A longer path, a shorter one, or one that is not ASCII, is
        // refused before anything is staged; the Engine row takes any
        // path. Shorter too: export.pdb would store it padded with spaces,
        // a file name that ends in spaces.
        {
            const fs::path tooLong = stick / "Contents" / std::string(300, 'x') / rekordboxFile.filename();
            const std::string refusal =
                relinkRefusalFor(pathToQString(pioneer), rekordboxFinding, pathToUtf8(tooLong));
            assert(refusal.find("characters") != std::string::npos && "a longer path is refused, with the reason");
            const fs::path old = pathFromUtf8(oldRekordboxPath);
            const std::string folder = pathToUtf8(old.parent_path().filename());
            assert(folder.size() > 1);
            const fs::path shorter = old.parent_path().parent_path() / folder.substr(1) / old.filename();
            const std::string shorterRefusal =
                relinkRefusalFor(pathToQString(pioneer), rekordboxFinding, pathToUtf8(shorter));
            if (shorterRefusal.find("exactly") == std::string::npos) {
                std::cerr << "a path one character shorter: \"" << shorterRefusal << "\"\n";
            }
            assert(shorterRefusal.find("exactly") != std::string::npos
                   && "a shorter path is refused too, with the length export.pdb takes");
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

        const std::vector<domain::Track> after = f.readAll();
        int pointed = 0;
        int enginePointed = 0;
        for (const auto &r : after) {
            if (key(r.filePath) == key(newRekordboxPath)) {
                ++pointed;
                if (r.filename != pathToUtf8(rekordboxFile.filename())) {
                    std::cerr << r.format << " row's filename: \"" << r.filename << "\"\n";
                }
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
        // Byte for byte in export.pdb: the path and the name exactly as
        // given, with nothing after them.
        for (const auto &r : rekordboxFinding.rows) {
            if (r.format != "rekordbox") {
                continue;
            }
            const auto raw = rawPdbPath(pioneer / "rekordbox" / "export.pdb", static_cast<uint32_t>(std::stoul(r.sourceId)));
            assert(raw.has_value());
            if (raw->filePath != rekordboxPathOnStick(pathToUtf8(pioneer), newRekordboxPath)) {
                std::cerr << "stored file_path: \"" << raw->filePath << "\"\n";
            }
            assert(raw->filePath == rekordboxPathOnStick(pathToUtf8(pioneer), newRekordboxPath));
            assert(raw->filename == pathToUtf8(rekordboxFile.filename()));
        }

        const auto undone = save({std::make_shared<RestoreBackupsChange>(result.backups)}, pathToQString(pioneer),
                                 pathToQString(engine));
        assert(undone.error.isEmpty());
        int restored = 0;
        int engineRestored = 0;
        for (const auto &r : f.readAll()) {
            restored += key(r.filePath) == key(oldRekordboxPath) ? 1 : 0;
            engineRestored += key(r.filePath) == key(oldEnginePath) ? 1 : 0;
            assert(key(r.filePath) != key(newRekordboxPath) && key(r.filePath) != key(newEnginePath));
        }
        assert(restored == 2 && engineRestored == 1 && "undo put every row back");
        std::cout << "case 4 (repair B points every catalog at the file, exactly, and undoes) OK\n";
    }

    // 5. Repair B when the catalogs spell the file differently (#103
    //    review): rows are grouped into one finding by normalizedPathKey,
    //    so the finding's own spelling is one catalog's, and each catalog
    //    is looked up by the spelling of ITS row. Here OneLibrary spells
    //    the folder in capitals (the finding's spelling), export.pdb as it
    //    was, and an Engine row in small letters.
    if (runs(5)) {
        const FixtureStick f(fixture, "seabass_moved_file_spellings");
        fs::path newFile;
        const domain::MovedFileFinding found = f.relinkableRekordboxFinding(newFile);
        assert(found.rows.size() == 2);
        std::string rekordboxSpelling;
        std::string oneLibraryOld;
        for (const auto &r : found.rows) {
            (r.format == "rekordbox" ? rekordboxSpelling : oneLibraryOld) = r.filePath;
        }
        const std::string oneLibrarySpelling = withFolderCase(rekordboxSpelling, true);
        const std::string engineSpelling = withFolderCase(rekordboxSpelling, false);
        assert(oneLibrarySpelling != rekordboxSpelling && engineSpelling != rekordboxSpelling
               && oneLibrarySpelling != engineSpelling && "three spellings of one path");
        // Each spelling written into its catalog as it is, so that it
        // holds whether or not the folder is on disk yet.
        const auto onStick = [&](const std::string &path) {
            return "/" + pathToGenericUtf8(pathFromUtf8(path).lexically_relative(f.stick));
        };
        assert(spellOneLibraryRows(f.pioneer, onStick(oneLibraryOld), onStick(oneLibrarySpelling)) >= 1);
        std::string engineRowId;
        for (const auto &r : f.readAll()) {
            if (r.format == "engine" && !r.filePath.empty()) {
                engineRowId = r.sourceId;
                break;
            }
        }
        assert(!engineRowId.empty());
        spellEngineRow(f.engine, engineRowId, pathToGenericUtf8(pathFromUtf8(engineSpelling).lexically_relative(f.engine)));

        domain::MovedFileFinding finding;
        finding.expectedPath = oneLibrarySpelling;  // as the first row spells it
        for (const char *format : {"onelibrary", "rekordbox", "engine"}) {
            for (const auto &r : f.readAll()) {
                if (r.format == format && !r.filePath.empty() && key(r.filePath) == key(rekordboxSpelling)) {
                    finding.rows.push_back(r);
                }
            }
        }
        assert(finding.rows.size() == 3 && "one row in each catalog");
        for (const auto &r : finding.rows) {
            const std::string &spelled = r.format == "onelibrary" ? oneLibrarySpelling
                                         : r.format == "rekordbox" ? rekordboxSpelling
                                                                   : engineSpelling;
            if (r.filePath != spelled) {
                std::cerr << r.format << " spells it \"" << r.filePath << "\", expected \"" << spelled << "\"\n";
            }
            assert(r.filePath == spelled && "each catalog its own spelling");
        }
        write(newFile, "audio bytes");
        // The folder is on disk now, in the stick's own spelling, as it is
        // on a real stick: the new file is in it. A FAT or exFAT stick, and
        // macOS's own disk, find it by any spelling, and asking the
        // filesystem for the path answers in the disk's spelling, not the
        // row's. On a filesystem that tells case apart a link per other
        // spelling stands in for that, so the lookup meets it here too.
        const fs::path folder = pathFromUtf8(rekordboxSpelling).parent_path();
        assert(fs::is_directory(folder) && "the old file's folder is on disk");
        for (const std::string &other : {oneLibrarySpelling, engineSpelling}) {
            const fs::path otherFolder = pathFromUtf8(other).parent_path();
            if (!fs::exists(otherFolder)) {
                fs::create_directory_symlink(folder.filename(), otherFolder);
            }
            assert(fs::equivalent(otherFolder, folder) && "every spelling of the folder finds it");
        }
        // export.pdb's lookup tries a row's path as the row spells it
        // first, and the disk's spelling after it.
        const auto spellings = rekordboxLookupSpellings(pathToUtf8(f.pioneer), engineSpelling);
        if (spellings.empty() || spellings.front() != onStick(engineSpelling)) {
            std::cerr << "export.pdb would look \"" << engineSpelling << "\" up as \""
                      << (spellings.empty() ? std::string() : spellings.front()) << "\" first\n";
        }
        assert(spellings.size() == 2 && spellings.front() == onStick(engineSpelling)
               && spellings.back() == onStick(rekordboxSpelling) && "its own spelling first, then the disk's");
        assert(rekordboxLookupSpellings(pathToUtf8(f.pioneer), rekordboxSpelling)
                   == std::vector<std::string>{onStick(rekordboxSpelling)}
               && "once when the two agree");
        finding.candidates.push_back({fileAt(newFile), domain::MovedFileCandidate::Confidence::High, "the same"});
        assert(relinkRefusalFor(pathToQString(f.pioneer), finding, pathToUtf8(newFile)).empty());
        auto change = std::make_shared<RelinkMovedFileChange>(pathToQString(f.pioneer), pathToQString(f.engine),
                                                              finding, finding.candidates[0].file);
        const auto result = save({change}, pathToQString(f.pioneer), pathToQString(f.engine));
        if (!result.error.isEmpty()) {
            std::cerr << "save failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && result.skippedIds.isEmpty());
        std::set<std::string> pointedFormats;
        for (const auto &r : f.readAll()) {
            assert(key(r.filePath) != key(rekordboxSpelling) && "no catalog names the old path in any spelling");
            if (key(r.filePath) == key(pathToUtf8(newFile))) {
                pointedFormats.insert(r.format);
            }
        }
        if (pointedFormats.size() != 3) {
            std::cerr << "pointed at the file: " << pointedFormats.size() << " catalog(s)\n";
        }
        assert(pointedFormats == (std::set<std::string>{"rekordbox", "onelibrary", "engine"})
               && "every catalog's row points at the file");
        std::cout << "case 5 (repair B finds each catalog's row by its own spelling) OK\n";
    }

    // 6. A row the finding lists that the write no longer finds fails the
    //    change, and the save puts back what it had written: here the
    //    Engine row went between the scan and the save, after export.pdb
    //    and OneLibrary were written. Never a skip that reports success
    //    with the catalogs disagreeing.
    if (runs(6)) {
        const FixtureStick f(fixture, "seabass_moved_file_vanished");
        fs::path newFile;
        domain::MovedFileFinding finding = f.relinkableRekordboxFinding(newFile);
        assert(finding.rows.size() == 2);
        domain::Track gone = finding.rows[0];
        gone.format = "engine";
        gone.sourceId = "999999";
        finding.rows.push_back(gone);
        write(newFile, "audio bytes");
        finding.candidates.push_back({fileAt(newFile), domain::MovedFileCandidate::Confidence::High, "the same"});
        const fs::path pdb = f.pioneer / "rekordbox" / "export.pdb";
        const std::string pdbBefore = read(pdb);
        const std::set<std::string> before = f.snapshot();
        auto change = std::make_shared<RelinkMovedFileChange>(pathToQString(f.pioneer), pathToQString(f.engine),
                                                              finding, finding.candidates[0].file);
        const auto result = save({change}, pathToQString(f.pioneer), pathToQString(f.engine));
        if (result.error.isEmpty()) {
            std::cerr << "the save succeeded with a row missing\n";
        }
        assert(!result.error.isEmpty() && result.appliedIds.isEmpty());
        assert(result.error.contains("Engine") && "the failure names the catalog");
        assert(result.rolledBack && "and everything it had written went back");
        assert(read(pdb) == pdbBefore && "export.pdb as it was");
        assert(f.snapshot() == before && "every catalog as it was");
        std::cout << "case 6 (a row gone since the scan fails the relink, and nothing stays written) OK\n";
    }

    // 7. Repair B checks the file is still the one the scan matched, as
    //    repair A does: another size refuses it. And a file back at the
    //    old path refuses it too: the catalogs already resolve there.
    if (runs(7)) {
        const FixtureStick f(fixture, "seabass_moved_file_relink_changed");
        fs::path newFile;
        const domain::MovedFileFinding finding = f.relinkableRekordboxFinding(newFile);
        assert(finding.rows.size() == 2);
        write(newFile, "audio bytes");
        const domain::Track file = fileAt(newFile);
        const std::set<std::string> before = f.snapshot();
        const auto relink = [&]() {
            return save({std::make_shared<RelinkMovedFileChange>(pathToQString(f.pioneer), pathToQString(f.engine),
                                                                 finding, file)},
                        pathToQString(f.pioneer), pathToQString(f.engine));
        };

        write(newFile, "audio bytes, and more");
        auto result = relink();
        if (!result.error.contains("changed since the scan")) {
            std::cerr << "a changed file: \"" << result.error.toStdString() << "\"\n";
        }
        assert(result.error.contains("changed since the scan"));
        assert(f.snapshot() == before);

        write(newFile, "audio bytes");
        write(pathFromUtf8(finding.expectedPath), "back where the catalogs expect it");
        result = relink();
        if (!result.error.contains("there again")) {
            std::cerr << "a file back at the old path: \"" << result.error.toStdString() << "\"\n";
        }
        assert(result.error.contains("there again"));
        assert(f.snapshot() == before);
        std::cout << "case 7 (repair B refuses a file changed since the scan, or an old path found again) OK\n";
    }

    // Ten files moved back by one save, as Move All Back stages them: the
    // stick, where each one is now, and the save's record.
    struct TenMoved
    {
        std::unique_ptr<ScratchStick> scratch;
        fs::path pioneer;
        std::vector<fs::path> expected;
        std::vector<fs::path> moved;
        SaveLoopResult result;
    };
    const auto tenMovedBack = [](const std::string &name) {
        TenMoved t;
        t.scratch = std::make_unique<ScratchStick>(seabass::testing::scratchRoot() / name);
        const fs::path &stick = t.scratch->path;
        t.pioneer = stick / "PIONEER";
        write(t.pioneer / "rekordbox" / "export.pdb", "pdb");
        std::vector<std::shared_ptr<PendingChange>> changes;
        for (int i = 0; i < 10; ++i) {
            const std::string name = "Track" + std::to_string(i) + ".mp3";
            t.expected.push_back(stick / "Contents" / "Old" / name);
            t.moved.push_back(stick / "Contents" / "New" / name);
            write(t.moved.back(), "audio " + std::to_string(i));
            domain::MovedFileFinding finding;
            finding.expectedPath = pathToUtf8(t.expected.back());
            finding.rows = {row("rekordbox", std::to_string(i + 1), finding.expectedPath, name)};
            finding.candidates.push_back(
                {fileAt(t.moved.back()), domain::MovedFileCandidate::Confidence::High, "the same"});
            changes.push_back(std::make_shared<RelocateMovedFileChange>(finding, finding.candidates[0].file));
        }
        t.result = save(changes, pathToQString(t.pioneer), {});
        assert(t.result.error.isEmpty() && t.result.appliedIds.size() == 10);
        for (int i = 0; i < 10; ++i) {
            assert(fs::is_regular_file(t.expected[i]) && !fs::exists(t.moved[i]));
        }
        return t;
    };
    const auto allTenWhereTheSaveLeftThem = [](const TenMoved &t) {
        for (size_t i = 0; i < 10; ++i) {
            if (!fs::is_regular_file(t.expected[i]) || read(t.expected[i]) != "audio " + std::to_string(i)) {
                std::cerr << "file " << i << " is not where the save left it\n";
                return false;
            }
        }
        return true;
    };

    // 8. An undo of the ten that meets an obstacle at one of them (a file
    //    put where one of them goes back to) moves none of them: Undo is
    //    all or nothing for moves too, and says it failed.
    if (runs(8)) {
        TenMoved t = tenMovedBack("seabass_moved_file_undo_obstacle");
        write(t.moved[4], "someone else's file");
        const auto undone =
            save({std::make_shared<RestoreBackupsChange>(t.result.backups)}, pathToQString(t.pioneer), {});
        assert(!undone.error.isEmpty() && "the undo failed, and says so");
        assert(allTenWhereTheSaveLeftThem(t) && "all ten where they were before the undo");
        assert(read(t.moved[4]) == "someone else's file" && "the obstacle untouched");
        for (size_t i = 0; i < 10; ++i) {
            assert(i == 4 || !fs::exists(t.moved[i]));
        }
        std::cout << "case 8 (an undo that meets an obstacle at one of ten moves leaves all ten) OK\n";
    }

    // 9. An undo whose note of a move fails partway (the pre-restore
    //    record's manifest cannot be written, as on a full stick) moves
    //    back what it had moved and reports the failure: no file stays
    //    where half an undo left it, and no exception leaves restore().
    if (runs(9)) {
        TenMoved t = tenMovedBack("seabass_moved_file_undo_note_fails");
        // A folder, with a file in it, where the manifest's write goes
        // first (writeFileDurablyAtomic's temporary file): the write fails
        // as it would on a full stick, and the record stays removable.
        std::vector<fs::path> blocks;
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting(
            [&](const std::string &recordDir, std::size_t index) {
                if (index == 4) {
                    const fs::path block = pathFromUtf8(recordDir) / ".manifest.tmp-seabass-write";
                    write(block / "in the way", "x");
                    blocks.push_back(block);
                }
            });
        const auto unlock = [&]() {
            for (const auto &block : blocks) {
                std::error_code ec;
                fs::remove_all(block, ec);
            }
            blocks.clear();
        };
        const auto undone =
            save({std::make_shared<RestoreBackupsChange>(t.result.backups)}, pathToQString(t.pioneer), {});
        unlock();
        assert(!undone.error.isEmpty() && "the undo failed, and says so");
        if (!allTenWhereTheSaveLeftThem(t)) {
            std::cerr << "undo error: " << undone.error.toStdString() << "\n";
        }
        assert(allTenWhereTheSaveLeftThem(t) && "all ten where they were before the undo");
        for (size_t i = 0; i < 10; ++i) {
            assert(!fs::exists(t.moved[i]));
        }

        // The store itself, called directly: a bool, not an exception.
        infrastructure::backup::FilesystemBackupStore store(t.result.backups[0].backupDir.toStdString());
        bool threw = false;
        bool restored = true;
        try {
            restored = store.restore(t.result.backups[0].id.toStdString());
        } catch (const std::exception &e) {
            threw = true;
            std::cerr << "restore() threw: " << e.what() << "\n";
        }
        unlock();
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting({});
        assert(!threw && !restored && !store.lastRestoreError().empty());
        assert(allTenWhereTheSaveLeftThem(t) && "and again all ten where they were");
        std::cout << "case 9 (an undo whose move note fails partway moves back what it moved) OK\n";
    }

    // 10. A note of a move holds the file's size, and Undo refuses to move
    //     back a file of another size: the stick changed since (a
    //     rekordbox export rewrote it, someone replaced it), and moving
    //     that file away is not undoing this save.
    if (runs(10)) {
        TenMoved t = tenMovedBack("seabass_moved_file_undo_other_file");
        write(t.expected[7], "a different file, replaced since the save");
        const auto undone =
            save({std::make_shared<RestoreBackupsChange>(t.result.backups)}, pathToQString(t.pioneer), {});
        if (!undone.error.contains("bytes")) {
            std::cerr << "undo of a replaced file: \"" << undone.error.toStdString() << "\"\n";
        }
        assert(!undone.error.isEmpty() && undone.error.contains("bytes") && "refused, saying why");
        assert(read(t.expected[7]) == "a different file, replaced since the save" && !fs::exists(t.moved[7]));
        for (size_t i = 0; i < 10; ++i) {
            assert(i == 7 || (fs::is_regular_file(t.expected[i]) && !fs::exists(t.moved[i])));
        }
        std::cout << "case 10 (undo refuses to move back a file whose size changed) OK\n";
    }

    // 11. The same for a save that fails after the move: the file at the
    //     place it was moved to is no longer the one moved there, so the
    //     rollback leaves it and says it could not put the stick back.
    if (runs(11)) {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_rollback_other_file");
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
        const auto result =
            save({std::make_shared<RelocateMovedFileChange>(finding, finding.candidates[0].file),
                  std::make_shared<ScriptedChange>("b", [&](SaveContext &) {
                      write(expected, "replaced by something else meanwhile");
                      return ChangeOutcome::failure("the next change refused");
                  })},
                 pathToQString(pioneer), {});
        assert(result.failedId == "b");
        assert(!result.rolledBack && "the rollback could not put the stick back, and says so");
        assert(read(expected) == "replaced by something else meanwhile" && !fs::exists(moved)
               && "the other file was not moved away");
        std::cout << "case 11 (a failed save does not move back a file whose size changed) OK\n";
    }

    // 12. An undo of two saves whose second restore fails after the first
    //     moved its file back: the first one's move goes back too (the
    //     store reports it, SaveContext::noteMovedForRollback), so the
    //     undo is all or nothing across its records as well.
    if (runs(12)) {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_moved_file_undo_two_saves");
        const fs::path &stick = scratch.path;
        const fs::path pioneer = stick / "PIONEER";
        write(pioneer / "rekordbox" / "export.pdb", "pdb");
        std::vector<UndoableBackup> backups;
        std::vector<fs::path> expected;
        std::vector<fs::path> moved;
        for (int i = 0; i < 2; ++i) {
            const std::string name = "Track" + std::to_string(i) + ".mp3";
            expected.push_back(stick / "Contents" / "Old" / name);
            moved.push_back(stick / "Contents" / "New" / name);
            write(moved.back(), "audio " + std::to_string(i));
            domain::MovedFileFinding finding;
            finding.expectedPath = pathToUtf8(expected.back());
            finding.rows = {row("rekordbox", std::to_string(i + 1), finding.expectedPath, name)};
            finding.candidates.push_back(
                {fileAt(moved.back()), domain::MovedFileCandidate::Confidence::High, "the same"});
            const auto result = save({std::make_shared<RelocateMovedFileChange>(finding, finding.candidates[0].file)},
                                     pathToQString(pioneer), {});
            assert(result.error.isEmpty() && result.backups.size() == 1);
            backups.push_back(result.backups[0]);
        }
        // The undo restores the newer record first: Track1 moves back,
        // then the older one meets an obstacle.
        write(moved[0], "someone else's file");
        const auto undone = save({std::make_shared<RestoreBackupsChange>(backups)}, pathToQString(pioneer), {});
        assert(!undone.error.isEmpty() && "the undo failed, and says so");
        if (!fs::is_regular_file(expected[1])) {
            std::cerr << "undo error: " << undone.error.toStdString() << "\n";
        }
        assert(fs::is_regular_file(expected[1]) && !fs::exists(moved[1])
               && "the first restore's move went back with the failed undo");
        assert(fs::is_regular_file(expected[0]) && read(moved[0]) == "someone else's file");
        std::cout << "case 12 (an undo of two saves that fails at the second puts the first one's move back) OK\n";
    }

    // The pre-restore records a store holds, by id.
    const auto preRestoreRecords = [](const TenMoved &t) {
        std::vector<std::string> ids;
        infrastructure::backup::FilesystemBackupStore store(t.result.backups[0].backupDir.toStdString());
        for (const auto &record : store.list()) {
            if (record.label == "pre-restore") {
                ids.push_back(record.id);
            }
        }
        return ids;
    };

    // 13. A rename that fails partway through an undo (a folder put where
    //     the fifth file goes back to, after the check and before the
    //     move): the four already moved go back, the undo fails saying
    //     why, nothing it had restored was kept, and no pre-restore record
    //     is left for a restore that wrote nothing.
    if (runs(13)) {
        TenMoved t = tenMovedBack("seabass_moved_file_undo_rename_fails");
        fs::path planted;
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting(
            [&](const std::string &, std::size_t index) {
                if (index == 4) {
                    planted = t.moved[5];  // newest first: the fifth move is Track5
                    write(planted / "in the way", "x");
                }
            });
        const auto undone =
            save({std::make_shared<RestoreBackupsChange>(t.result.backups)}, pathToQString(t.pioneer), {});
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting({});
        if (!undone.error.contains("could not move")) {
            std::cerr << "undo error: " << undone.error.toStdString() << "\n";
        }
        assert(!planted.empty() && "the fifth move was reached");
        assert(undone.error.contains("could not move") && "the failed rename is named");
        assert(undone.error.contains("Nothing it had restored was kept") && "and it is true");
        std::error_code ec;
        fs::remove_all(planted, ec);
        assert(allTenWhereTheSaveLeftThem(t) && "the four moved went back");
        for (size_t i = 0; i < 10; ++i) {
            assert(!fs::exists(t.moved[i]));
        }
        assert(preRestoreRecords(t).empty() && "a restore that wrote nothing keeps no record");
        std::cout << "case 13 (a rename failing partway through an undo moves back what it had moved) OK\n";
    }

    // 14. The stuck path: the fifth rename fails and so do the moves back.
    //     Obstacles, not permissions (a CI runner as root ignores those):
    //     a non-empty folder where the fifth file goes, and one where each
    //     file already moved has to go back to. The files that could not
    //     be put back are named, the pre-restore record that notes them is
    //     kept, and the message does NOT say nothing was kept.
    if (runs(14)) {
        TenMoved t = tenMovedBack("seabass_moved_file_undo_stuck");
        bool locked = false;
        std::vector<fs::path> planted;
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting(
            [&](const std::string &, std::size_t index) {
                if (index == 4) {
                    // Newest first: Track9 to Track6 have moved, Track5 is next.
                    planted.push_back(t.moved[5]);
                    for (int i = 6; i < 10; ++i) {
                        planted.push_back(t.expected[i]);
                    }
                    for (const auto &folder : planted) {
                        write(folder / "in the way", "x");
                    }
                    locked = true;
                }
            });
        const auto undone =
            save({std::make_shared<RestoreBackupsChange>(t.result.backups)}, pathToQString(t.pioneer), {});
        infrastructure::backup::FilesystemBackupStore::setBeforeRestoreMoveForTesting({});
        std::error_code ec;
        for (const auto &folder : planted) {
            fs::remove_all(folder, ec);
        }
        std::cerr << "undo error: " << undone.error.toStdString() << "\n";
        assert(locked && "the fifth move was reached");
        assert(!undone.error.isEmpty());
        assert(!undone.error.contains("Nothing it had restored was kept") && "not said when it is not true");
        assert(undone.error.contains("could not be returned"));
        // Track9 to Track6 were moved and are stuck where the undo put
        // them: each named.
        for (int i = 6; i < 10; ++i) {
            assert(fs::is_regular_file(t.moved[i]) && !fs::exists(t.expected[i]));
            assert(undone.error.contains(pathToQString(t.moved[i])) && "each stuck file is named");
        }
        for (int i = 0; i < 5; ++i) {
            assert(fs::is_regular_file(t.expected[i]) && "the moves not reached were not made");
        }
        assert(preRestoreRecords(t).size() == 1 && "the record noting the stuck moves is kept");
        std::cout << "case 14 (moves that cannot be put back are named and their record kept) OK\n";
    }

    // 15. An Engine row is found by its own path, and by the disk's
    //     spelling of it when no row has that path (#103 review). Seabass
    //     writes a row it adds in the disk's spelling (createEngineTrack
    //     asks the filesystem), and its readback, playlist edits and copy
    //     analysis look it up by the rekordbox row's path, which may spell
    //     a folder otherwise: rekordbox made "ABBA", a later row says
    //     "Abba". And two rows that differ only in a folder's case are two
    //     rows: each is found by its own path, never the other's.
    if (runs(15)) {
        const FixtureStick f(fixture, "seabass_moved_file_engine_spellings");
        std::vector<std::string> ids;
        for (const auto &r : f.readAll()) {
            if (r.format == "engine" && !r.filePath.empty() && ids.size() < 3) {
                ids.push_back(r.sourceId);
            }
        }
        assert(ids.size() == 3 && "three Engine rows");
        // On disk the folder is "ABBA". A disk that does not tell case
        // apart finds it as "Abba" too; elsewhere a link stands in.
        const fs::path disk = f.stick / "Contents" / "ABBA";
        const fs::path other = f.stick / "Contents" / "Abba";
        fs::create_directories(disk);
        if (!fs::exists(other)) {
            fs::create_directory_symlink("ABBA", other);
        }
        assert(fs::equivalent(disk, other));
        write(disk / "added.mp3", "audio bytes");
        write(disk / "twice.mp3", "audio bytes");

        const std::string engine = pathToUtf8(f.engine);
        // A row in the disk's spelling, as Seabass adds one.
        spellEngineRow(f.engine, ids[0], "../Contents/ABBA/added.mp3");
        // Two rows for one file, its folder in two spellings.
        spellEngineRow(f.engine, ids[1], "../Contents/ABBA/twice.mp3");
        spellEngineRow(f.engine, ids[2], "../Contents/Abba/twice.mp3");
        const auto idsOf = [&](const fs::path &file) {
            std::vector<std::string> found;
            for (const std::int64_t id : infrastructure::engine::engineTrackIdsForFile(engine, engine, pathToUtf8(file))) {
                found.push_back(std::to_string(id));
            }
            return found;
        };
        const auto said = [](const std::vector<std::string> &found) {
            std::string out;
            for (const auto &id : found) {
                out += (out.empty() ? "" : ",") + id;
            }
            return "[" + out + "]";
        };

        const auto byOtherSpelling = idsOf(other / "added.mp3");
        if (byOtherSpelling != std::vector<std::string>{ids[0]}) {
            std::cerr << "Abba/added.mp3 found " << said(byOtherSpelling) << ", the row is " << ids[0] << "\n";
        }
        assert(byOtherSpelling == std::vector<std::string>{ids[0]} && "found through another spelling of its folder");
        assert(idsOf(disk / "added.mp3") == std::vector<std::string>{ids[0]} && "and through its own");

        const auto upper = idsOf(disk / "twice.mp3");
        const auto mixed = idsOf(other / "twice.mp3");
        if (upper != std::vector<std::string>{ids[1]} || mixed != std::vector<std::string>{ids[2]}) {
            std::cerr << "ABBA/twice.mp3 found " << said(upper) << " (row " << ids[1] << "), Abba/twice.mp3 found "
                      << said(mixed) << " (row " << ids[2] << ")\n";
        }
        assert(upper == std::vector<std::string>{ids[1]} && mixed == std::vector<std::string>{ids[2]}
               && "two rows that differ only in case: each found by its own path");

        // And pointed elsewhere the same way: through another spelling,
        // and only the row of the spelling given.
        assert(infrastructure::engine::setEngineTrackPath(engine, engine, pathToUtf8(other / "added.mp3"),
                                                          pathToUtf8(disk / "moved.mp3"))
               == 1);
        assert(seabass::testing::sqlScalar(pathToUtf8(f.engine / "Database2" / "m.db"),
                                           "SELECT count(*) FROM Track WHERE id = " + ids[0]
                                               + " AND path = '../Contents/ABBA/moved.mp3';")
               == 1);
        assert(infrastructure::engine::setEngineTrackPath(engine, engine, pathToUtf8(other / "twice.mp3"),
                                                          pathToUtf8(disk / "moved twice.mp3"))
               == 1);
        assert(seabass::testing::sqlScalar(pathToUtf8(f.engine / "Database2" / "m.db"),
                                           "SELECT count(*) FROM Track WHERE id = " + ids[1]
                                               + " AND path = '../Contents/ABBA/twice.mp3';")
               == 1 && "the other row is left alone");
        std::cout << "case 15 (an Engine row is found by its own spelling first, then by the disk's) OK\n";
    }

    std::cout << "moved_file_change_test: all cases OK\n";
    return 0;
}
