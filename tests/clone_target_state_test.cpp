// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the stick list says about a clone's target, end to end: a real
// clone onto a stick with another label (whose covers are made
// label-proof after the copy, #101), then the same advice the stick list
// computes, read off both sticks with the app's own reader.
//
// Round 12 of the rig (C1, C2): right after a clone the target read as
// behind its own backup ("1 file to write", "update from disk-backup"),
// because the cover step changed the m.db the backup's fingerprint names,
// and every further clone wrote the backup's m.db back and copied the
// covers again.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/advise_stick_backup.hpp"
#include "application/use_cases/clone_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "domain/library_fingerprint.hpp"
#include "infrastructure/stick_backup/library_catalog_mtime.hpp"
#include "infrastructure/stick_backup/restored_databases.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "scratch_path.hpp"
#include "stick_fixture.hpp"

using namespace seabass::application;
using namespace seabass::test_fixture;
namespace fs = std::filesystem;
namespace sb = seabass::infrastructure::stick_backup;
using State = StickBackupAdvice::State;
using Kind = StickBackupAdvice::SourceRef::Kind;

namespace
{

// The library's content fingerprint, the same on both sticks: tracks and
// cues are not what this is about, the Engine database's bytes are.
std::string libraryFingerprint()
{
    std::vector<seabass::domain::Track> tracks;
    for (int i = 0; i < 5; ++i) {
        seabass::domain::Track track;
        track.title = "Track " + std::to_string(i);
        track.artist = "Artist";
        track.durationSeconds = 200.0 + i;
        tracks.push_back(track);
    }
    return seabass::domain::fingerprintLibrary(tracks).serialize();
}

std::int64_t now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void setMtime(const fs::path &file, std::int64_t unix)
{
    fs::last_write_time(file, sb::fromUnixSeconds(unix));
}

struct Fixture
{
    fs::path root;
    fs::path source;
    fs::path backups;
    fs::path target;
    CloneStickOptions options;
    std::vector<std::int64_t> linkedTracks;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_clone_target_state_test_" + name)), source(root / "SOURCE"),
          backups(root / "Seabass Backups"), target(root / "TARGET")
    {
        fs::remove_all(root);
        writeFile(source / "Contents" / "a.mp3", pseudoRandom(90'000, 1), 1'700'000'000);
        writeFile(source / "PIONEER" / "rekordbox" / "export.pdb", pseudoRandom(4096, 4), 1'700'000'003);
        createEngineDb(mdb(source));
        // Four covers on a player's links to the source's name, three
        // with their image on the stick.
        linkedTracks = plantPlayerCoverLinks(source, "SOURCE", 4, 3);
        setMtime(mdb(source), 1'700'000'300);
        fs::create_directories(target);
        fs::create_directories(backups);
        options.backup.stickRoot = source;
        options.backup.archivePath = backups / "SOURCE.zip";
        options.backup.stickIdentifier = "uuid-SOURCE";
        options.backup.stickLabel = "SOURCE";
        const std::string fingerprint = libraryFingerprint();
        options.backup.readLibraryFingerprint = [fingerprint] { return fingerprint; };
        options.targetRoot = target;
        options.targetLabel = "TARGET";
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static fs::path mdb(const fs::path &stick) { return stick / "Engine Library" / "Database2" / "m.db"; }

    CloneStickOutcome clone()
    {
        const CloneStickOutcome outcome = CloneStick::execute(options);
        if (outcome.status != CloneStickOutcome::Status::Cloned) {
            std::cerr << "clone: " << toString(outcome.status) << ": " << outcome.message << "\n";
            for (const std::string &warning : outcome.restore.warnings) {
                std::cerr << "  " << warning << "\n";
            }
        }
        return outcome;
    }

    // What the stick list says about `stick`, with `other` plugged in
    // beside it: the inputs read exactly as BackupAdvisorController and
    // rig_advise read them.
    StickBackupAdvice advise(const fs::path &stick, const fs::path &other, const std::string &label = {}) const
    {
        const std::vector<StickBackupDescription> described = RestoreStickBackup::describeAll(backups);
        const auto fingerprint = seabass::domain::LibraryFingerprint::parse(libraryFingerprint());
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.stickLabel = label.empty() ? seabass::pathToUtf8(stick.filename()) : label;
        input.stickIdentifier = "uuid-" + input.stickLabel;
        input.liveFingerprint = fingerprint;
        StickDatabaseState mine = readStickDatabaseState(stick, described, true, input.stickLabel);
        input.liveDatabaseFingerprints = mine.fingerprints;
        input.restoredDatabases = mine.restoredDatabases;
        input.catalogModifiedAtUnix = sb::libraryCatalogModifiedAt(stick);
        input.backups = described;
        StickBackupAdviceInput::PeerStick peer;
        peer.mountPoint = seabass::pathToUtf8(other);
        peer.label = seabass::pathToUtf8(other.filename());
        peer.stickIdentifier = "uuid-" + peer.label;
        peer.fingerprint = fingerprint;
        StickDatabaseState theirs = readStickDatabaseState(other, described, true, peer.label);
        peer.databaseFingerprints = theirs.fingerprints;
        peer.restoredDatabases = theirs.restoredDatabases;
        peer.catalogModifiedAtUnix = sb::libraryCatalogModifiedAt(other);
        input.peers.push_back(peer);
        return adviseStickBackup(input);
    }

    void expect(const fs::path &stick, const fs::path &other, State state, Kind update, bool diverged,
                const char *what, const std::string &label = {}) const
    {
        const StickBackupAdvice advice = advise(stick, other, label);
        const bool ok = advice.state == state && advice.updateSource.kind == update && advice.diverged == diverged;
        if (!ok) {
            std::cerr << what << ": " << seabass::pathToUtf8(stick.filename()) << " reads " << toString(advice.state)
                      << ", update from " << toString(advice.updateSource.kind) << " "
                      << advice.updateSource.label << (advice.diverged ? ", diverged" : ", not diverged") << " ("
                      << advice.detail << ")\n";
        }
        assert(ok);
    }

    void expectCoversStored() const
    {
        for (int i = 0; i < 3; ++i) {
            assert(isStoredCoverFile(target, coverOf(target, linkedTracks[i])) && "a stored cover, file present");
        }
        assert(coverOf(target, linkedTracks[3]).hash == "image://fileart//media/TARGET/PIONEER/Artwork/00001/cover3.jpg");
    }
};

}  // namespace

int main()
{
    // The record of what a restore wrote stays on its stick: never in a
    // backup, never an extra.
    assert(sb::isExcludedFromBackup(sb::RestoredDatabasesPath, false));

    // ---- A clone reads as current at once, and a second run writes nothing ----
    {
        Fixture f("current");
        CloneStickOutcome outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.coversCopied == 3 && outcome.restore.coverLinksRenamed == 1);
        f.expectCoversStored();
        // The covers made the target's m.db differ from the backup's: the
        // reason this test exists.
        assert(readFile(Fixture::mdb(f.target)) != readFile(Fixture::mdb(f.source)));

        f.expect(f.target, f.source, State::Current, Kind::None, false, "right after the clone");
        f.expect(f.source, f.target, State::Current, Kind::None, false, "the source right after the clone");

        CloneStickPreview preview = CloneStick::preview(f.options);
        assert(preview.error.empty());
        assert(preview.archiveCurrent);
        assert(preview.restore.has_value());
        if (preview.restore->filesToWrite != 0) {
            std::cerr << "a clone run again would write " << preview.restore->filesToWrite << " file(s)\n";
        }
        assert(preview.restore->filesToWrite == 0);
        assert(preview.bytesToTarget == 0);

        const std::string written = readFile(Fixture::mdb(f.target));
        outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.backupStatus == BackupOutcomeStatus::NothingToDo);
        assert(outcome.restore.filesWritten == 0);
        assert(outcome.restore.coversCopied == 0 && outcome.restore.coverLinksRenamed == 0);
        assert(readFile(Fixture::mdb(f.target)) == written && "the second run left the database alone");
        f.expectCoversStored();
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after a second run");
        std::cout << "case 1 (clone reads current, a second run writes nothing) OK\n";

        // ---- The source really changes: the update is offered, applied, and current again ----
        appendEngineDbRow(Fixture::mdb(f.source), "Contents/b.mp3");
        setMtime(Fixture::mdb(f.source), now() + 60);
        f.expect(f.target, f.source, State::Current, Kind::Stick, false, "after the source changed");
        preview = CloneStick::preview(f.options);
        assert(!preview.archiveCurrent);

        outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.backupStatus == BackupOutcomeStatus::Complete);
        assert(outcome.restore.filesWritten >= 1);
        assert(outcome.restore.coversCopied == 3 && outcome.restore.coverLinksRenamed == 1);
        f.expectCoversStored();
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after the update");
        f.expect(f.source, f.target, State::Current, Kind::None, false, "the source after the update");
        preview = CloneStick::preview(f.options);
        assert(preview.archiveCurrent && preview.restore && preview.restore->filesToWrite == 0);
        std::cout << "case 2 (source changed: update offered, applied, current again) OK\n";

        // ---- Something else writes the target's m.db: changed, as before ----
        appendEngineDbRow(Fixture::mdb(f.target), "Contents/by-hand.mp3");
        setMtime(Fixture::mdb(f.target), now() + 120);
        f.expect(f.target, f.source, State::Outdated, Kind::None, false, "after a change on the target");
        preview = CloneStick::preview(f.options);
        assert(preview.restore && preview.restore->filesToWrite >= 1 && "an update would write the database again");

        // And with the source changed since the backup too: both moved away
        // from it, and updating the target would discard its own change.
        appendEngineDbRow(Fixture::mdb(f.source), "Contents/c.mp3");
        setMtime(Fixture::mdb(f.source), now() + 180);
        f.expect(f.target, f.source, State::Outdated, Kind::Stick, true, "both changed");
        std::cout << "case 3 (target's m.db changed by hand: changed, and diverged once the source moved too) OK\n";
    }

    // ---- Exact mode: the record is not an extra, and a rerun is clean ----
    {
        Fixture f("exact");
        f.options.exact = true;
        writeFile(f.target / "holiday-photos.txt", "not in the backup", 1'700'000'100);
        CloneStickOutcome outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.extrasRemoved == 1);
        assert(fs::exists(f.target / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath))));
        const CloneStickPreview preview = CloneStick::preview(f.options);
        assert(preview.restore.has_value());
        if (preview.restore->filesToWrite != 0 || preview.restore->extras != 0) {
            std::cerr << "an exact clone run again would write " << preview.restore->filesToWrite << " and remove "
                      << preview.restore->extras << "\n";
        }
        assert(preview.restore->filesToWrite == 0 && preview.restore->extras == 0);
        outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.filesWritten == 0 && outcome.restore.extrasRemoved == 0);
        assert(outcome.restore.warnings.empty());
        f.expectCoversStored();
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after an exact clone");

        // An exact update after the source changed: the covers are copied
        // again and stay, and the target is current again.
        appendEngineDbRow(Fixture::mdb(f.source), "Contents/b.mp3");
        setMtime(Fixture::mdb(f.source), now() + 60);
        outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.coversCopied == 3);
        f.expectCoversStored();
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after an exact update");
        outcome = f.clone();
        assert(outcome.restore.filesWritten == 0 && outcome.restore.extrasRemoved == 0);
        f.expectCoversStored();
        std::cout << "case 4 (exact: the record is no extra, a rerun writes and removes nothing) OK\n";
    }

    // ---- Onto a stick with the source's own label: no covers, no record ----
    {
        Fixture f("samelabel");
        f.options.targetLabel = "SOURCE";
        const CloneStickOutcome outcome = f.clone();
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(readFile(Fixture::mdb(f.target)) == readFile(Fixture::mdb(f.source)));
        assert(!fs::exists(f.target / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath))));
        assert(sb::readRestoredDatabases(f.target).empty());
        std::cout << "case 5 (same label: the database as backed up, nothing recorded) OK\n";
    }

    // ---- The backup restored onto the clone under the source's own name: the record goes, folder and all ----
    {
        Fixture f("recordgoes");
        assert(f.clone().status == CloneStickOutcome::Status::Cloned);
        const fs::path record = f.target / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath));
        assert(fs::exists(record));
        RestoreOptions restore;
        restore.archivePath = f.options.backup.archivePath;
        restore.targetRoot = f.target;
        restore.exact = true;
        restore.targetLabel = "SOURCE";
        const RestoreSummary summary = RestoreStickBackup::execute(restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(readFile(Fixture::mdb(f.target)) == readFile(Fixture::mdb(f.source)));
        assert(!fs::exists(record) && !fs::exists(record.parent_path()));
        const RestorePreview after = RestoreStickBackup::preview(restore);
        assert(after.filesToWrite == 0 && after.extras == 0);
        std::cout << "case 6 (restored as backed up: no record, no folder left for it) OK\n";
    }

    // ---- m.db changed in place, same size, header as it was: not the clone's any more ----
    // The fingerprint reads the header's change counter and schema cookie,
    // the sizes and the WAL's salts. A commit checkpointed back into a main
    // file of the same page count can leave all of them as they were, so
    // the record vouches only when the content's hash matches as well.
    {
        Fixture f("samesize");
        assert(f.clone().status == CloneStickOutcome::Status::Cloned);
        f.expect(f.target, f.source, State::Current, Kind::None, false, "right after the clone");
        const fs::path db = Fixture::mdb(f.target);
        const std::optional<sb::DbSetFingerprint> before = sb::fingerprintDbSet(db);
        const std::string bytesBefore = readFile(db);
        const fs::file_time_type mtimeBefore = fs::last_write_time(db);
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(db).c_str(), &handle) == SQLITE_OK);
        assert(sqlite3_exec(handle, "UPDATE Track SET path = 'Contents/A.mp3' WHERE path = 'Contents/a.mp3'", nullptr,
                            nullptr, nullptr)
               == SQLITE_OK);
        assert(sqlite3_changes(handle) == 1);
        sqlite3_close(handle);
        std::string bytes = readFile(db);
        assert(bytes.size() == bytesBefore.size() && bytes != bytesBefore);
        bytes.replace(0, 100, bytesBefore, 0, 100);  // the header as it was
        {
            std::ofstream out(db, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        fs::last_write_time(db, mtimeBefore);
        assert(sb::fingerprintDbSet(db) == before && "the fingerprint cannot see this change");
        f.expect(f.target, f.source, State::BehindBackup, Kind::DiskBackup, false, "after a same-size change in place");
        const CloneStickPreview preview = CloneStick::preview(f.options);
        assert(preview.restore && preview.restore->filesToWrite >= 1 && "an update writes the database again");
        std::cout << "case 7 (same-size change in place, header restored: not current) OK\n";
    }

    // ---- A record vouches only for the stick under the label it was written for ----
    {
        Fixture f("relabelled");
        assert(f.clone().status == CloneStickOutcome::Status::Cloned);
        RestoreOptions restore;
        restore.archivePath = f.options.backup.archivePath;
        restore.targetRoot = f.target;
        restore.targetLabel = "TARGET";
        assert(RestoreStickBackup::preview(restore).filesToWrite == 0);
        // Relabelled to the source's own name, and to a third one: the
        // links are TARGET's, so neither the stick list nor the planner
        // takes the database for the backup's.
        for (const std::string label : {"SOURCE", "THIRD"}) {
            f.expect(f.target, f.source, State::BehindBackup, Kind::DiskBackup, false, "relabelled", label);
            restore.targetLabel = label;
            const RestorePreview preview = RestoreStickBackup::preview(restore);
            assert(preview.filesToWrite >= 1 && "and a restore under that label writes m.db");
        }
        // Copied file by file onto another stick, record and all: its links
        // name TARGET, not COPY.
        const fs::path copy = f.root / "COPY";
        fs::copy(f.target, copy, fs::copy_options::recursive);
        assert(fs::exists(copy / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath))));
        // (The copy has its own mtimes, newer than the backup.)
        f.expect(copy, f.source, State::Outdated, Kind::None, false, "a file manager copy");
        restore.targetRoot = copy;
        restore.targetLabel = "COPY";
        assert(RestoreStickBackup::preview(restore).filesToWrite >= 1);
        // The clone itself, under its own name, is still the backup's.
        f.expect(f.target, f.source, State::Current, Kind::None, false, "the clone under its own name");
        std::cout << "case 8 (relabelled, or copied with its record: not vouched for, by either) OK\n";
    }

    // ---- A record naming files outside the Artwork folder is not trusted at all ----
    {
        Fixture f("foreignfile");
        f.options.exact = true;
        assert(f.clone().status == CloneStickOutcome::Status::Cloned);
        RestoreOptions restore;
        restore.archivePath = f.options.backup.archivePath;
        restore.targetRoot = f.target;
        restore.targetLabel = "TARGET";
        restore.exact = true;
        RestorePreview preview = RestoreStickBackup::preview(restore);
        assert(preview.filesToWrite == 0 && preview.extras == 0);
        const fs::path recordFile = f.target / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath));
        const std::string record = readFile(recordFile);
        for (const std::string foreign :
             {"Contents/old.mp3", "Engine Library/Artwork/../../Contents/old.mp3", "Engine Library/Artwork/./x.jpg",
              "/Contents/old.mp3", "engine library/database2/m.db"}) {
            std::ofstream(recordFile, std::ios::binary | std::ios::trunc)
                << record << "file\tEngine Library/Database2/m.db\t" << foreign << "\n";
            assert(sb::readRestoredDatabases(f.target).empty() && "one foreign line and the record says nothing");
        }
        // Contents/old.mp3 is an extra like any other, not kept.
        std::ofstream(recordFile, std::ios::binary | std::ios::trunc)
            << record << "file\tEngine Library/Database2/m.db\tContents/old.mp3\n";
        const RestorePreview without = RestoreStickBackup::preview(restore);
        assert(without.filesToWrite >= 1 && "the database is written again");
        writeFile(f.target / "Contents" / "old.mp3", "not in the backup", 1'700'000'100);
        preview = RestoreStickBackup::preview(restore);
        if (preview.extras != without.extras + 1) {
            std::cerr << "extras with Contents/old.mp3 on the stick: " << preview.extras << ", without: " << without.extras
                      << "\n";
        }
        assert(preview.extras == without.extras + 1);
        const RestoreSummary summary = RestoreStickBackup::execute(restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(!fs::exists(f.target / "Contents" / "old.mp3"));
        f.expectCoversStored();
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after the exact restore");
        std::cout << "case 9 (a record listing Contents/old.mp3: untrusted, the file an extra) OK\n";
    }

    // ---- Bytes that are not UTF-8 in the record: ignored, and nothing throws ----
    {
        Fixture f("badutf8");
        assert(f.clone().status == CloneStickOutcome::Status::Cloned);
        const fs::path recordFile = f.target / seabass::pathFromUtf8(std::string(sb::RestoredDatabasesPath));
        const std::string record = readFile(recordFile);
        assert(!sb::readRestoredDatabases(f.target).empty());
        for (const std::string bad : {std::string("Engine Library/Artwork/\xff\xfe.jpg"),
                                      std::string("Engine Library/Artwork/\xc0\xaf.jpg"),
                                      std::string("Engine Library/Artwork/\xed\xa0\x80.jpg")}) {
            std::ofstream(recordFile, std::ios::binary | std::ios::trunc)
                << record << "file\tEngine Library/Database2/m.db\t" << bad << "\n";
            assert(sb::readRestoredDatabases(f.target).empty());
        }
        RestoreOptions restore;
        restore.archivePath = f.options.backup.archivePath;
        restore.targetRoot = f.target;
        restore.targetLabel = "TARGET";
        const RestoreSummary summary = RestoreStickBackup::execute(restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(sb::readRestoredDatabases(f.target).size() == 1 && "the run wrote a record that reads");
        f.expect(f.target, f.source, State::Current, Kind::None, false, "after a restore over the bad record");
        std::cout << "case 10 (a record with bytes that are not UTF-8: ignored, replaced) OK\n";
    }

    std::cout << "All clone_target_state tests passed." << std::endl;
    return 0;
}
