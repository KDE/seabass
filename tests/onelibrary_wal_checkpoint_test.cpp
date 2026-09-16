// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// exportLibrary.db runs in WAL mode, and until this test nothing in
// Seabass ever checkpointed it: the rows landed in the database only
// because SQLite folds the log when the last connection closes. That is
// someone else's guarantee, not ours -- a future writer that keeps a
// connection open past the save would start stranding committed rows in
// a sidecar nobody reads, silently, with every catalog still matching.
//
// So: after a save that succeeded, the library is one self-contained
// file. No frames left in a write-ahead log, and no empty -wal/-shm
// beside it for a player or a later reader to trip over.

#include <QString>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using seabass::domain::Track;
using seabass::gui::SaveContext;
using seabass::gui::SaveLoopResult;
using seabass::gui::runSaveLoop;
using seabass::gui::PendingChange;
using seabass::gui::AddCueChange;
using seabass::application::CancellationToken;

namespace {

struct Target
{
    std::string sourceId;
    std::string title;
    int freeSlot = 0;
};

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    assert(fs::exists(source / "rekordbox" / "exportLibrary.db"));
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

// Only a track Device Library Plus also lists. Add Cue mirrors into
// exportLibrary.db solely when hasTrackAtPath() finds a row for the file
// (635 of 1118 tracks on a real stick have none), and a target without
// one would take the "nothing to mirror" branch: the database would never
// be opened, no write-ahead log would ever exist, and every assertion
// below would hold for the wrong reason.
Target findTarget(const fs::path &pioneerRoot)
{
    seabass::infrastructure::onelibrary::OneLibraryReader mirror(pioneerRoot.string());
    std::set<std::string> mirrored;
    for (const Track &track : mirror.readAll()) {
        if (!track.filePath.empty()) {
            mirrored.insert(track.filePath);
        }
    }
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneerRoot.string());
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty() || !mirrored.count(track.filePath)) {
            continue;
        }
        std::set<int> used;
        for (const CuePoint &cue : track.cues) {
            if (cue.kind == CuePoint::Kind::Hot) {
                used.insert(cue.hotCueNumber);
            }
        }
        if (used.empty()) {
            continue;
        }
        for (int slot = 1; slot <= 8; ++slot) {
            if (!used.count(slot)) {
                return {track.sourceId, track.title, slot};
            }
        }
    }
    return {};
}

SaveLoopResult addCueThroughSave(const fs::path &pioneerRoot, const Target &target)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = QString::fromStdString(pioneerRoot.string());
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
        QStringLiteral("rekordbox"), root, QString::fromStdString(target.sourceId), 1234.0, QStringLiteral("hot"),
        target.freeSlot, QStringLiteral("#FF0000"), QString(), false, 0.0, QString::fromStdString(target.title))};
    return runSaveLoop(changes, ctx);
}

// Cue rows in Device Library Plus, read through its own connection. The
// point is to prove the mirror wrote, so this deliberately opens the
// database rather than trusting the save's own report.
int mirroredCueCount(const fs::path &db)
{
    using namespace seabass::infrastructure::onelibrary;
    SqlCipherLibrary lib;
    SqlCipherDb conn(lib, db.string(), /*readOnly=*/true);
    conn.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement count(conn, "SELECT COUNT(*) FROM cue;");
    return count.step() ? static_cast<int>(count.columnInt64(0)) : -1;
}

std::uintmax_t sizeOf(const fs::path &file)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    return ec ? 0 : size;
}

}  // namespace

int main()
{
    const fs::path pioneerRoot = freshCopy("seabass_onelibrary_wal_checkpoint");
    const Target target = findTarget(pioneerRoot);
    assert(target.freeSlot != 0 && "the fixture must have a rekordbox track with a cue, a path and a free slot");

    const fs::path db = pioneerRoot / "rekordbox" / "exportLibrary.db";
    const fs::path wal = fs::path(db.string() + "-wal");
    const fs::path shm = fs::path(db.string() + "-shm");

    const int cuesBefore = mirroredCueCount(db);
    const SaveLoopResult result = addCueThroughSave(pioneerRoot, target);
    assert(result.error.isEmpty() && "the save itself must succeed");
    assert(result.appliedIds.size() == 1);

    // Sidecars first, cue count second, and the order matters: counting
    // opens a connection to a WAL database, which creates -wal and -shm
    // again. Reading them before asserting on them measured this test's
    // own probe rather than the save.
    const bool walAfterSave = fs::exists(wal);
    const std::uintmax_t walBytesAfterSave = sizeOf(wal);
    const bool shmAfterSave = fs::exists(shm);

    // The save really did go through Device Library Plus. Without this the
    // -wal assertions below would pass on a save that never opened the
    // database at all, which is the way this check would rot silently.
    const int cuesAfter = mirroredCueCount(db);
    if (cuesAfter <= cuesBefore) {
        std::cerr << "the mirror did not write: cue rows " << cuesBefore << " -> " << cuesAfter << "\n";
    }
    assert(cuesAfter > cuesBefore && "Add Cue must have written into exportLibrary.db");

    // The point: a save that reported success left the library in one
    // file. A -wal with frames in it means rows that every reader of the
    // database alone cannot see.
    if (walAfterSave && walBytesAfterSave > 0) {
        std::cerr << "a successful save left " << walBytesAfterSave << " bytes in " << wal.filename().string() << "\n";
    }
    assert(!(walAfterSave && walBytesAfterSave > 0) && "a successful save must leave no frames in the write-ahead log");

    // And the sidecars themselves are gone, not merely empty: an empty
    // -wal beside a -shm is what the sticks carry today, and it is what
    // "make sure it cannot get in the way" asks us to stop leaving.
    if (walAfterSave) {
        std::cerr << "left behind: " << wal.filename().string() << " (" << walBytesAfterSave << " bytes)\n";
    }
    if (shmAfterSave) {
        std::cerr << "left behind: " << shm.filename().string() << "\n";
    }
    assert(!walAfterSave && "a checkpointed database keeps no -wal beside it");
    assert(!shmAfterSave && "a checkpointed database keeps no -shm beside it");

    // The cue really is in the database that remains, not only in a log
    // that happened to be folded by something else.
    assert(sizeOf(db) > 0);

    std::error_code ec;
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_onelibrary_wal_checkpoint", ec);
    std::cout << "onelibrary_wal_checkpoint_test passed\n";
    return 0;
}
