// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Issue #21: a library-shaped stick through a full backup and a restore,
// the far end compared with the near end through the app's own readers.
//
// Every part of the backup path has a test; what none of them did was
// take a real library, back it up, restore it somewhere else, and ask
// whether what came out is the library that went in. The release rig
// does that on real sticks (FB1, FB8, X2); this does it in ctest with the
// anonymized fixture, which carries real rekordbox, OneLibrary and Engine
// catalogs, laid out as a stick.
//
// What has to hold, in this order:
//   1. the backup completes and a preview against the stick then wants
//      nothing (every file is in the archive, sizes and times as on the
//      stick);
//   2. an exact restore onto an empty directory completes without a write
//      error, and a preview against it then wants nothing either;
//   3. the restored tree is byte-for-byte the stick's, as the backup's
//      own walker sees both;
//   4. the app's readers see the same library on both sides: the same
//      tracks with the same cues, the same playlists, the same fingerprint;
//   5. after a change on the stick (a file added, one changed, one
//      removed) the second backup carries exactly that, and restores to
//      the changed stick, while the first restore still holds the
//      original.

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"
#include "stick_fixture.hpp"

using namespace seabass::application;
using namespace seabass::test_fixture;
namespace fs = std::filesystem;

namespace
{

// One catalog as the app reads it: the tracks, and per track the things
// a lossy restore would lose first.
struct Catalog
{
    std::string name;
    std::vector<seabass::domain::Track> tracks;
};

std::vector<Catalog> readCatalogs(const fs::path &root)
{
    std::vector<Catalog> catalogs;
    const fs::path pioneer = root / "PIONEER";
    const fs::path engine = root / "Engine Library";
    if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
        seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneer));
        catalogs.push_back({"rekordbox", ScanLibrary(reader).execute()});
    }
    if (fs::exists(pioneer / "rekordbox" / "exportLibrary.db")) {
        seabass::infrastructure::onelibrary::OneLibraryReader reader(seabass::pathToUtf8(pioneer));
        catalogs.push_back({"onelibrary", ScanLibrary(reader).execute()});
    }
    if (fs::exists(engine / "Database2" / "m.db")) {
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(engine));
        catalogs.push_back({"engine", ScanLibrary(reader).execute()});
    }
    return catalogs;
}

// What a restore must carry for a track: its identity, its cues (count
// and positions), its playlists.
std::string trackSignature(const seabass::domain::Track &t)
{
    std::string s = t.sourceId + "|" + t.artist + "|" + t.title + "|" + std::to_string(t.cues.size());
    for (const auto &cue : t.cues) {
        s += "|" + std::to_string(static_cast<long long>(cue.positionMs));
    }
    for (const auto &membership : t.playlists) {
        s += "|" + membership.name;
    }
    return s;
}

std::vector<std::string> signatures(const Catalog &catalog)
{
    std::vector<std::string> out;
    for (const auto &t : catalog.tracks) {
        out.push_back(trackSignature(t));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The fixture holds PIONEER's content under rekordbox/ and Engine
// Library's under engine/; a stick has them at its root.
void layOutStick(const fs::path &stick)
{
    const fs::path fixture = fs::path("tests") / "fixtures" / "anonymized_library";
    fs::create_directories(stick);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    // A few audio files, so the tree is stick-shaped rather than catalogs alone.
    writeFile(stick / "Contents" / "a.mp3", pseudoRandom(90'000, 1), 1'700'000'000);
    writeFile(stick / "Contents" / "Sub" / "b.mp3", pseudoRandom(40'000, 2), 1'700'000'001);
    writeFile(stick / "Contents" / "c.mp3", pseudoRandom(3'000, 3), 1'700'000'002);
}

bool restoredCleanly(const RestoreSummary &summary)
{
    // Restored, and nothing else: no library check is asked for, so any
    // "problem" the restore could report would be its own (an extra it
    // could not remove, a file left in place).
    return summary.status == RestoreSummary::Status::Restored && summary.rejected.empty() && summary.writeErrors.empty();
}

// The scratch tree, removed however the test ends: four copies of the
// fixture in RAM-backed /tmp are not something a failed check may leave.
// So the checks return from main() rather than abort(), which would skip
// this destructor.
struct Scratch
{
    fs::path root;
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

// A failed check reports and returns, so the scratch tree is removed.
#define CHECK(condition)                                                                                   \
    do {                                                                                                   \
        if (!(condition)) {                                                                                \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n";                              \
            return 1;                                                                                      \
        }                                                                                                  \
    } while (false)

int main()
{
    const Scratch scratch{seabass::testing::scratchRoot() / "seabass_library_backup_roundtrip_test"};
    const fs::path &root = scratch.root;
    fs::remove_all(root);
    const fs::path stick = root / "stick";
    const fs::path archive = root / "Seabass Backups" / "FIXTURE.zip";
    const fs::path target = root / "restored";
    const fs::path target2 = root / "restored-2";
    layOutStick(stick);
    fs::create_directories(target);
    fs::create_directories(target2);

    BackupStickOptions backup;
    backup.stickRoot = stick;
    backup.archivePath = archive;
    backup.stickIdentifier = "fixture-0001";
    backup.stickLabel = "FIXTURE";

    RestoreOptions restore;
    restore.archivePath = archive;
    restore.targetRoot = target;
    restore.exact = true;

    // 1. Backup; nothing left to back up afterwards.
    {
        const BackupStickOutcome outcome = BackupStick::execute(backup);
        CHECK(outcome.status == BackupOutcomeStatus::Complete && "the first backup completes");
        std::cout << "backup: added " << outcome.added << ", changed " << outcome.changed << ", removed "
                  << outcome.removed << '\n';
        RestoreOptions againstStick = restore;
        againstStick.targetRoot = stick;
        const RestorePreview preview = RestoreStickBackup::preview(againstStick);
        CHECK(preview.error.empty());
        CHECK(preview.filesToWrite == 0 && preview.extras == 0 && "the archive holds every file as it is on the stick");
        std::cout << "case 1: the backup holds the stick (" << preview.filesUnchanged << " files unchanged) OK\n";
    }

    // 2. Exact restore onto an empty directory; nothing to restore afterwards.
    {
        const RestoreSummary summary = RestoreStickBackup::execute(restore);
        std::cout << "restore: " << summary.filesWritten << " written, " << summary.message << '\n';
        CHECK(restoredCleanly(summary));
        CHECK(summary.filesWritten > 0);
        const RestorePreview after = RestoreStickBackup::preview(restore);
        CHECK(after.error.empty());
        CHECK(after.filesToWrite == 0 && after.extras == 0 && "a preview against the restored tree wants nothing");
        std::cout << "case 2: the restore is complete and settled OK\n";
    }

    // 3. The trees are the same, byte for byte, as the walker sees them.
    //    (Kept: case 5 compares against it, as the original.)
    const auto original = snapshot(stick);
    {
        const auto far = snapshot(target);
        CHECK(!original.empty());
        CHECK(original == far && "the restored tree is the stick's, byte for byte");
        std::cout << "case 3: " << original.size() << " entries identical on both sides OK\n";
    }

    // 4. The app's readers see the same library on both sides.
    {
        const std::vector<Catalog> near = readCatalogs(stick);
        const std::vector<Catalog> far = readCatalogs(target);
        CHECK(near.size() == 3 && "the fixture carries rekordbox, OneLibrary and Engine catalogs");
        CHECK(far.size() == near.size());
        std::vector<seabass::domain::Track> nearAll;
        std::vector<seabass::domain::Track> farAll;
        for (std::size_t i = 0; i < near.size(); ++i) {
            CHECK(near[i].name == far[i].name);
            CHECK(!near[i].tracks.empty() && "the fixture's catalogs hold tracks");
            CHECK(near[i].tracks.size() == far[i].tracks.size());
            CHECK(signatures(near[i]) == signatures(far[i]) && "every track, with its cues and playlists, reads the same");
            std::cout << "  " << near[i].name << ": " << near[i].tracks.size() << " tracks read the same on both sides\n";
            nearAll.insert(nearAll.end(), near[i].tracks.begin(), near[i].tracks.end());
            farAll.insert(farAll.end(), far[i].tracks.begin(), far[i].tracks.end());
        }
        const auto nearPrint = seabass::domain::fingerprintLibrary(nearAll);
        const auto farPrint = seabass::domain::fingerprintLibrary(farAll);
        CHECK(nearPrint == farPrint && "the library fingerprint is the same on both sides");
        std::cout << "case 4: the readers see the same library on both sides (" << nearPrint.cuedTrackCount
                  << " cued tracks) OK\n";
    }

    // 5. A change on the stick: the second generation carries exactly it,
    //    and restores to the changed stick; the first restore keeps the original.
    {
        writeFile(stick / "Contents" / "d.mp3", pseudoRandom(5'000, 4), 1'700'000'010);      // added
        writeFile(stick / "Contents" / "c.mp3", pseudoRandom(3'000, 5), 1'700'000'011);      // changed
        fs::remove(stick / "Contents" / "Sub" / "b.mp3");                                     // removed
        const BackupStickOutcome second = BackupStick::execute(backup);
        CHECK(second.status == BackupOutcomeStatus::Complete);
        CHECK(second.added == 1 && second.changed == 1 && second.removed == 1
               && "the second backup carries exactly the change");
        RestoreOptions gen2 = restore;
        gen2.targetRoot = target2;
        const RestoreSummary summary = RestoreStickBackup::execute(gen2);
        CHECK(restoredCleanly(summary));
        CHECK(snapshot(target2) == snapshot(stick) && "the second generation restores to the changed stick");
        CHECK(snapshot(target) == original && "the first restore still holds the original");
        CHECK(!fs::exists(target2 / "Contents" / "Sub" / "b.mp3") && fs::exists(target / "Contents" / "Sub" / "b.mp3"));
        std::cout << "case 5: a change goes through the second backup and not the first restore OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
