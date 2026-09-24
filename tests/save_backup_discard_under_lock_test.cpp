// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A save that fails and is fully rolled back leaves no backup record
// behind -- including when it runs the way the GUI runs it, under the
// stick's write lock.
//
// Round 4 filled a stick, watched the save be refused correctly, and found
// a complete backup record left on it afterwards: a copy of a stick that
// never changed, taking the space the save had just been refused for.
// SaveContext::discardBackupsTakenThisSave() exists for exactly that, and
// it did run -- it then asked for the stick's write lock, which
// LibraryEditSession::save() was already holding around the whole save
// loop. StickWriteLock is flock() on an open file description and is
// deliberately not reentrant, so the request threw StickBusyError every
// single time and the records always stayed. The only trace was a line in
// the stick's own log.
//
// Every test that drove runSaveLoop directly passed throughout, because
// none of them held the outer lock: the fixture agreed with the bug. So
// this runs both ways, and the locked case is the one that used to fail.

#include <QString>

#include <cassert>
#include <filesystem>
#include <fstream>
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
#include "gui/qt_path.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/backup/stick_space.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

struct Target
{
    std::string sourceId;
    std::string title;
    int freeSlot = 0;
};

// The stick root is PIONEER's parent, so the copy goes one level down and
// Seabass/backups lands beside it exactly as it does on a real stick.
fs::path freshStick(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / seabass::pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    assert(fs::exists(source / "rekordbox" / "export.pdb"));
    fs::copy(source, scratch / "PIONEER", fs::copy_options::recursive);
    return scratch;
}

Target findTarget(const fs::path &pioneerRoot)
{
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneerRoot));
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty()) {
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

// One directory per backup record, which is what Manage Backups lists and
// what the rig counts on the stick.
int recordCount(const fs::path &stickRoot)
{
    const fs::path backups = stickRoot / "Seabass" / "backups";
    std::error_code ec;
    if (!fs::is_directory(backups, ec)) {
        return 0;
    }
    int count = 0;
    for (const auto &entry : fs::directory_iterator(backups, ec)) {
        if (entry.is_directory(ec)) {
            ++count;
        }
    }
    return count;
}

std::string stickLog(const fs::path &stickRoot)
{
    std::ifstream in(stickRoot / "Seabass" / "seabass.log");
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Makes the Device Library Plus mirror unwritable, which is the failure
// add_cue_mirror_failure_test already pins: the change fails, the save
// loop rolls it back whole, and nothing is applied -- the one case in
// which the backup this save took is a copy of a stick that never changed.
void makeMirrorReadOnly(const fs::path &pioneerRoot, bool readOnly)
{
    const fs::path db = pioneerRoot / "rekordbox" / "exportLibrary.db";
    for (const fs::path &file : {db, fs::path(db).concat("-wal"), fs::path(db).concat("-shm")}) {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            continue;
        }
        if (readOnly) {
            fs::permissions(file, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                            fs::perm_options::replace, ec);
        } else {
            fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
        }
    }
}

SaveLoopResult failingSave(const fs::path &pioneerRoot, const Target &target)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
        QStringLiteral("rekordbox"), root, QString::fromStdString(target.sourceId), 1234.0, QStringLiteral("hot"),
        target.freeSlot, QStringLiteral("#FF0000"), QString(), false, 0.0, QString::fromStdString(target.title))};
    return runSaveLoop(changes, ctx);
}

// Both cases, told apart only by whether the caller holds the stick lock.
void runCase(const std::string &name, bool holdStickLock)
{
    const fs::path stickRoot = freshStick(name);
    const fs::path pioneerRoot = stickRoot / "PIONEER";
    const Target target = findTarget(pioneerRoot);
    assert(target.freeSlot != 0 && "the fixture must have a track with a cue, a file path and a free slot");
    assert(recordCount(stickRoot) == 0 && "a fresh stick starts with no backup records");
    makeMirrorReadOnly(pioneerRoot, true);

    SaveLoopResult result;
    {
        // Exactly what LibraryEditSession::save() does, and for the same
        // scope: held across the whole save loop.
        std::vector<std::unique_ptr<seabass::infrastructure::backup::StickWriteLock>> locks;
        if (holdStickLock) {
            locks = seabass::infrastructure::backup::acquireStickLocks(
                {seabass::infrastructure::backup::backupDirForCatalogPath(seabass::pathToUtf8(pioneerRoot))});
        }
        result = failingSave(pioneerRoot, target);
    }
    makeMirrorReadOnly(pioneerRoot, false);

    assert(!result.error.isEmpty() && "the save must fail: this whole test is about what a failed save leaves");
    assert(result.appliedIds.isEmpty() && "and apply nothing, or its backup is the undo and must be kept");
    assert(result.error.toStdString().find("putting back") == std::string::npos
           && "the rollback itself must have worked, or the backup is rightly kept");

    const std::string log = stickLog(stickRoot);
    // Pinned rather than inferred: "0 records left" would also hold if no
    // backup had ever been taken, which is the shape of bug this guards.
    assert(log.find("backed up") != std::string::npos && "the save did take a backup before it failed");
    assert(log.find("backup record(s) this save had taken were removed") != std::string::npos
           && "and said it removed them again");
    assert(log.find("could not be cleared up") == std::string::npos
           && "the clean-up must not have failed on a lock its own caller holds");
    assert(log.find("could NOT be removed") == std::string::npos);

    const int left = recordCount(stickRoot);
    if (left != 0) {
        std::cerr << name << ": " << left << " backup record(s) left on the stick\n" << log;
    }
    assert(left == 0 && "a failed, fully rolled back save leaves no backup record behind");
    // Nor the note that would offer an undo of it (#48): it named the
    // records while the save ran, and goes with them.
    assert(!fs::exists(stickRoot / "Seabass" / "backups" / ".save-in-progress")
           && "no note of a save in progress survives a save that changed nothing");

    std::error_code ec;
    fs::remove_all(stickRoot, ec);
    std::cout << "case (" << (holdStickLock ? "under the stick write lock, as the GUI saves" : "no outer lock")
              << ") OK\n";
}

// A save that goes through, so the stick ends up with an automatic backup
// record the next save is allowed to release.
void successfulSave(const fs::path &pioneerRoot, const Target &target, int slot)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
        QStringLiteral("rekordbox"), root, QString::fromStdString(target.sourceId), 1000.0 * slot,
        QStringLiteral("hot"), slot, QStringLiteral("#FF0000"), QString(), false, 0.0,
        QString::fromStdString(target.title))};
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    if (!result.error.isEmpty()) {
        std::cerr << "setup save failed: " << result.error.toStdString() << "\n";
    }
    assert(result.error.isEmpty() && "the setup saves must go through, or there are no records to release");
}

// The other half of the same bug. releaseAutomaticBackupsIfTight() took
// the same lock from inside the same save, threw the same StickBusyError,
// and swallowed it -- returning 0, which is also what a stick with room
// returns. So it has never released a byte in the GUI, and no test could
// see that: the only way to reach it was to fill a real volume below its
// headroom, and nothing did. The space is handed in here instead.
void runReleaseCase()
{
    const fs::path stickRoot = freshStick("seabass_release_locked");
    const fs::path pioneerRoot = stickRoot / "PIONEER";
    const Target target = findTarget(pioneerRoot);
    assert(target.freeSlot != 0);

    // Two automatic records, because the newest is always kept: with one
    // record there is nothing to release and a pass would prove nothing.
    successfulSave(pioneerRoot, target, target.freeSlot);
    const Target next = findTarget(pioneerRoot);
    assert(next.freeSlot != 0 && next.freeSlot != target.freeSlot);
    successfulSave(pioneerRoot, next, next.freeSlot);
    const int before = recordCount(stickRoot);
    assert(before >= 2 && "the setup must leave at least two automatic records");

    // A stick well below its headroom, as measureStickSpace would report
    // one. Nothing this save took is spared, because this context took
    // nothing: the records are the earlier saves'.
    seabass::infrastructure::backup::StickSpace tight;
    tight.capacityBytes = 2ULL * 1024 * 1024 * 1024;
    tight.freeBytes = 1024;

    std::uint64_t released = 0;
    {
        std::vector<std::unique_ptr<seabass::infrastructure::backup::StickWriteLock>> locks =
            seabass::infrastructure::backup::acquireStickLocks(
                {seabass::infrastructure::backup::backupDirForCatalogPath(seabass::pathToUtf8(pioneerRoot))});
        auto &noProgress = seabass::application::NullProgressReporter::instance();
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, seabass::gui::pathToQString(pioneerRoot), {});
        released = ctx.releaseAutomaticBackupsIfTight(tight);
    }

    const int after = recordCount(stickRoot);
    if (released == 0 || after >= before) {
        std::cerr << "released " << released << " bytes, " << before << " -> " << after << " records\n"
                  << stickLog(stickRoot);
    }
    assert(released > 0 && "a stick below its headroom must actually give space back");
    assert(after < before && "and a released record is a record that is gone");
    assert(after >= 1 && "the newest automatic record is never released: it is the undo");
    assert(stickLog(stickRoot).find("could not be released") == std::string::npos
           && "and it must not have failed on a lock its own caller holds");

    std::error_code ec;
    fs::remove_all(stickRoot, ec);
    std::cout << "case (release under the stick write lock) OK: " << released << " bytes, " << before << " -> "
              << after << " records\n";
}

}  // namespace

int main()
{
    runCase("seabass_discard_unlocked", false);
    runCase("seabass_discard_locked", true);
    runReleaseCase();
    std::cout << "save_backup_discard_under_lock_test passed\n";
    return 0;
}
