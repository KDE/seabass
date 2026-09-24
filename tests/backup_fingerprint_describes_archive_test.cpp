// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The number in a backup's manifest header must describe the library
// stored in that same backup.
//
// Nothing checked this, and it was not true. TESTRIG_ABC.zip records
// "v1;143;151;4" in its header while the catalogs sitting beside it in the
// same archive fingerprint as 156 tracks -- verified by pulling those
// catalogs out of the zip and fingerprinting them with nothing else
// involved. Both sides say "v1", so no version check notices, and the
// advisor then reports a stick restored from that archive as a DIFFERENT
// library from the archive it came out of. That is the worst answer it
// has: it tells someone their stick diverged when it never changed.
//
// The shape of the bug is one this project keeps finding: a stored number
// that nothing ever compares against the thing it claims to describe. So
// this test compares them, over a real backup of real catalogs, through
// the restore path an actual recovery uses.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/scan_library.hpp"
#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "domain/library_fingerprint.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::application;
namespace fs = std::filesystem;

namespace
{

// The stick's library as the app fingerprints it: rekordbox and Engine
// together. Deliberately NOT the cached reader -- this has to answer for
// the bytes on disk, which is the entire point of the check.
std::optional<domain::LibraryFingerprint> fingerprintOf(const fs::path &root)
{
    std::vector<domain::Track> tracks;
    bool anyRead = false;
    const fs::path pioneer = root / "PIONEER";
    if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneer));
        std::vector<domain::Track> read = ScanLibrary(reader).execute();
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(engine));
        std::vector<domain::Track> read = ScanLibrary(reader).execute();
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    if (!anyRead) {
        return std::nullopt;
    }
    return domain::fingerprintLibrary(tracks);
}

// A stick carrying the committed fixture's two catalogs, which are real
// rekordbox and Engine databases rather than the filler bytes the other
// backup tests use. A fingerprint over filler would be empty and would
// agree with itself no matter what this code did.
fs::path buildStick(const fs::path &root)
{
    const fs::path fixture = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library";
    const fs::path stick = root / "stick";
    fs::create_directories(stick);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    return stick;
}

void theHeaderDescribesTheArchiveItSitsIn(const fs::path &root)
{
    const fs::path stick = buildStick(root);
    const auto live = fingerprintOf(stick);
    assert(live && "the fixture's catalogs must fingerprint, or this test proves nothing");
    assert(live->trackCount > 0);

    // WHEN the fingerprint is taken is the whole fix, so the test watches
    // for it rather than trusting it. Comparing the header with the
    // archive alone would pass even if the number were read before the
    // first byte was copied -- a caller handing in a value it read
    // earlier is exactly the bug, and on a stick nobody else is writing
    // to, early and late agree. These two count the copied files at the
    // moment the callback fires.
    std::size_t filesCopiedWhenAsked = 0;
    int timesAsked = 0;
    std::size_t filesCopied = 0;
    BackupStickOptions options;
    options.stickRoot = stick;
    options.archivePath = root / "backup.zip";
    options.stickIdentifier = "uuid-fixture";
    options.stickLabel = "FIXTURE";
    options.onProgress = [&filesCopied](const BackupProgress &progress) {
        if (progress.phase == BackupProgress::Phase::Reading || progress.phase == BackupProgress::Phase::Database) {
            filesCopied = std::max(filesCopied, progress.filesDone);
        }
    };
    options.readLibraryFingerprint = [stick, &filesCopied, &filesCopiedWhenAsked, &timesAsked] {
        ++timesAsked;
        filesCopiedWhenAsked = filesCopied;
        const auto fingerprint = fingerprintOf(stick);
        return fingerprint ? fingerprint->serialize() : std::string();
    };
    const BackupStickOutcome outcome = BackupStick::execute(options);
    assert(outcome.status == BackupOutcomeStatus::Complete);
    assert(timesAsked == 1 && "the fingerprint is taken once per backup, not per file and not never");
    assert(filesCopiedWhenAsked > 0 && "the fingerprint was taken before anything had been copied");
    std::cout << "  asked once, after " << filesCopiedWhenAsked << " file(s) were copied\n";

    const StickBackupDescription described = RestoreStickBackup::describe(seabass::pathToUtf8(options.archivePath));
    const auto recorded = domain::LibraryFingerprint::parse(described.libraryFingerprint);
    assert(recorded && "a completed backup of a readable library must record a fingerprint");

    // Restored rather than read off the source stick: the question is what
    // the ARCHIVE holds. Reading the stick again would compare the source
    // with itself and pass however wrong the header was -- which is
    // exactly how this went unnoticed.
    const fs::path restored = root / "restored";
    fs::create_directories(restored);
    RestoreOptions restoreOptions;
    restoreOptions.archivePath = options.archivePath;
    restoreOptions.targetRoot = restored;
    const RestoreSummary restoreSummary = RestoreStickBackup::execute(restoreOptions);
    assert(restoreSummary.status == RestoreSummary::Status::Restored);

    const auto inTheArchive = fingerprintOf(restored);
    assert(inTheArchive && "the archive must hold catalogs that still read");

    // Counts, not the sampled hashes: the counts are what the advisor
    // shows a person and what "different library" is decided on, and a
    // mismatch here is the failure that shipped.
    std::cout << "  header: " << recorded->trackCount << " tracks, " << recorded->cuedTrackCount << " cued, "
              << recorded->playlistCount << " playlists\n"
              << "  archive: " << inTheArchive->trackCount << " tracks, " << inTheArchive->cuedTrackCount << " cued, "
              << inTheArchive->playlistCount << " playlists\n";
    assert(recorded->trackCount == inTheArchive->trackCount);
    assert(recorded->cuedTrackCount == inTheArchive->cuedTrackCount);
    assert(recorded->playlistCount == inTheArchive->playlistCount);
}

// A run that was cancelled holds part of a library, so it has no library
// identity of its own to write down. It must keep what the generation
// before it recorded rather than stamping the live library over a partial
// archive -- which would make the archive claim a completeness it does not
// have, the same lie in the other direction.
void aCancelledRunKeepsThePreviousFingerprint(const fs::path &root)
{
    const fs::path stick = buildStick(root);
    BackupStickOptions options;
    options.stickRoot = stick;
    options.archivePath = root / "backup.zip";
    options.stickIdentifier = "uuid-fixture";
    options.stickLabel = "FIXTURE";
    options.readLibraryFingerprint = [] { return std::string("v1;1;1;1;0000000000000001"); };
    assert(BackupStick::execute(options).status == BackupOutcomeStatus::Complete);
    const std::string afterFirst = RestoreStickBackup::describe(seabass::pathToUtf8(options.archivePath)).libraryFingerprint;
    assert(afterFirst == "v1;1;1;1;0000000000000001");

    // Second run over the same archive, cancelled, offering a different
    // number. The archive must still say what it said.
    CancellationToken cancel;
    options.cancel = cancel;
    options.readLibraryFingerprint = [] { return std::string("v1;999;999;9;000000000000ffff"); };
    cancel.cancel();
    const BackupStickOutcome second = BackupStick::execute(options);
    assert(second.status != BackupOutcomeStatus::Complete);
    const std::string afterCancel = RestoreStickBackup::describe(seabass::pathToUtf8(options.archivePath)).libraryFingerprint;
    std::cout << "  after a cancelled update the header still reads: " << afterCancel << "\n";
    assert(afterCancel == afterFirst);
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "backup-fingerprint";
    fs::remove_all(scratch);
    {
        const fs::path here = scratch / "describes-archive";
        fs::create_directories(here);
        std::cout << "the header describes the archive it sits in\n";
        theHeaderDescribesTheArchiveItSitsIn(here);
    }
    {
        const fs::path here = scratch / "cancelled";
        fs::create_directories(here);
        std::cout << "a cancelled run keeps the previous fingerprint\n";
        aCancelledRunKeepsThePreviousFingerprint(here);
    }
    fs::remove_all(scratch);
    std::cout << "backup_fingerprint_describes_archive_test: ok\n";
    return 0;
}
