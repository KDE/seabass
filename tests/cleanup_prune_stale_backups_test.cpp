// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Free Up Space's old backups through CleanupController, against a stick
// built in scratch with records FilesystemBackupStore itself made: the
// staleBackups listing, pruneStaleBackups() run end to end on the worker,
// its cancel between two records, a record the filesystem will not
// delete, the summary the page shows for each, and the stick's log.
//
// The live session is a real LibraryEditSession whose last save is two
// records: opened over an interrupted save's note naming them, which it
// adopts the way it does after a pull, and the note then cleared, as a
// save that finishes clears it. From there only the session keeps them,
// which is the state after any finished save.
//
// - case 1 (listing): every automatic record of an earlier save, oldest
//   first; not the user's own, not the live session's.
// - case 2 (removal): exactly the listed ones go, one log line each,
//   none for anything else, and the list is empty afterwards.
// - case 3 (cancel, POSIX only): the worker is held inside the first log
//   line, a FIFO whose buffer the test filled, until the cancel has been
//   asked for; the first record goes and the rest stay whole.
// - case 4 (refusal): a record the filesystem will not delete is counted
//   failed, logged, and left listed; the record after it still goes. On
//   POSIX the record's directory is read-only; on Windows its files are
//   held open with no sharing, the sharing violation a real stick gives
//   when an archiver or a preview pane has the zip open.
//
// Windows runs cases 1, 2 and 4. Holding the worker between two records
// without a sleep needs the FIFO, which Windows has no equivalent of.

#include <QCoreApplication>
#include <QFileSystemWatcher>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "application/ports/backup_store.hpp"
#include "gui/cleanup_controller.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::application::BackupOrigin;
using seabass::gui::CleanupController;
using seabass::gui::EditSessionRegistry;
using seabass::gui::pathFromQString;
using seabass::gui::pathToQString;
using seabass::infrastructure::backup::FilesystemBackupStore;
namespace fs = std::filesystem;

namespace
{

const std::string DeletedLine = "cleanup: deleted backup from an earlier save, no longer needed for undo -> ";
const std::string FailedLine = "cleanup: failed to delete backup from an earlier save -> ";

[[noreturn]] void fail(const std::string &why)
{
    std::cerr << "FAIL: " << why << "\n";
    std::exit(1);
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The messages of the log lines in `text`, timestamp taken off.
std::vector<std::string> messages(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // The log is written in text mode, so on Windows its lines end in
        // CRLF, and the file is read here as bytes: the '\r' would otherwise
        // stay on every message and no exact comparison could match.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t gap = line.find("  ");
        if (gap != std::string::npos) {
            out.push_back(line.substr(gap + 2));
        }
    }
    return out;
}

int countOf(const std::vector<std::string> &lines, const std::string &message)
{
    int n = 0;
    for (const auto &line : lines) {
        n += line == message ? 1 : 0;
    }
    return n;
}

// Any line at all that names this record.
bool mentions(const std::vector<std::string> &lines, const std::string &id)
{
    for (const auto &line : lines) {
        if (line.size() >= id.size() && line.compare(line.size() - id.size(), id.size(), id) == 0) {
            return true;
        }
    }
    return false;
}

bool whole(const fs::path &backups, const std::string &id)
{
    std::error_code ec;
    const fs::path dir = backups / seabass::pathFromUtf8(id);
    return fs::is_regular_file(dir / ".manifest", ec) && fs::is_regular_file(dir / "backup.zip", ec);
}

bool gone(const fs::path &backups, const std::string &id)
{
    std::error_code ec;
    return !fs::exists(backups / seabass::pathFromUtf8(id), ec);
}

std::vector<std::string> listedIds(const CleanupController &controller)
{
    std::vector<std::string> ids;
    for (const QVariant &entry : controller.staleBackups()) {
        ids.push_back(entry.toMap().value(QStringLiteral("id")).toString().toStdString());
    }
    return ids;
}

void waitForListing(CleanupController &controller, QSignalSpy &listed)
{
    for (int i = 0; i < 300 && controller.listingStaleBackups(); ++i) {
        listed.wait(100);
    }
    if (controller.listingStaleBackups()) {
        fail("the stale-backup listing never landed");
    }
}

QVariantMap waitForPrune(QSignalSpy &finished)
{
    for (int i = 0; i < 300 && finished.count() == 0; ++i) {
        finished.wait(100);
    }
    if (finished.count() != 1) {
        fail("pruneStaleBackups() never finished");
    }
    return finished.takeFirst().at(0).toMap();
}

std::string join(const std::vector<std::string> &ids)
{
    std::string out;
    for (const auto &id : ids) {
        out += (out.empty() ? "" : ", ") + id;
    }
    return "[" + out + "]";
}

void expectList(const CleanupController &controller, const std::vector<std::string> &expected, const char *what)
{
    const auto ids = listedIds(controller);
    if (ids != expected) {
        fail(std::string(what) + ": listed " + join(ids) + ", expected " + join(expected));
    }
    if (controller.staleBackupCount() != static_cast<int>(expected.size())) {
        fail(std::string(what) + ": staleBackupCount disagrees with the list");
    }
}

}  // namespace

int main(int argc, char **argv)
{
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path scratch = seabass::testing::scratchRoot() / "cleanup_prune_stale_backups_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    seabass::testing::sandboxSeabassHome(scratch / "home");
    seabass::testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    QTemporaryDir stickDir(pathToQString(scratch / "STICK-XXXXXX"));
    assert(stickDir.isValid());
    const fs::path stick = pathFromQString(stickDir.path());
    const fs::path pioneer = stick / "PIONEER";
    const fs::path engine = stick / "Engine Library";
    fs::create_directories(pioneer);
    fs::create_directories(engine / "Database2");
    const fs::path backups = stick / "Seabass" / "backups";
    const fs::path logPath = seabass::infrastructure::paths::stickOperationLog(stick);
    const fs::path covered = stick / "backup-fixture.dat";
    std::ofstream(covered, std::ios::binary) << std::string(4096, 'b');

    FilesystemBackupStore store(seabass::pathToUtf8(backups));
    // Labels rise with the order the records are made in, so two made in
    // the same second still sort oldest first.
    auto automatic = [&](const std::string &label) {
        return store.backup({seabass::pathToUtf8(covered)}, label).id;
    };

    // Case 1: the listing.
    // Arrange: three automatic records from earlier saves, the user's own
    // between them, and the live session's last save of two.
    const std::string a1 = automatic("earlier-a");
    const std::string a2 = automatic("earlier-b");
    const std::string mine =
        store.backup({seabass::pathToUtf8(covered)}, "earlier-mine", BackupOrigin::UserRequested).id;
    const std::string a3 = automatic("earlier-c");
    const std::string l1 = automatic("live-a");
    const std::string l2 = automatic("live-b");
    assert(read(backups / seabass::pathFromUtf8(mine) / ".manifest").find("ORIGIN\tuser\n") != std::string::npos);

    auto *registry = EditSessionRegistry::instance();
    const QString libraryId = registry->libraryIdForPath(pathToQString(engine));
    if (libraryId.isEmpty()) {
        fail("the scratch stick has no library id, so no session can hold its records");
    }
    if (!seabass::infrastructure::backup::noteSaveInProgress(seabass::pathToUtf8(backups), {l1, l2})) {
        fail("the save note could not be written");
    }
    auto *session = registry->openSession(libraryId, QStringLiteral("STICK"), pathToQString(pioneer),
                                          pathToQString(engine));
    seabass::infrastructure::backup::clearSaveInProgress(seabass::pathToUtf8(backups));
    if (session->lastSaveBackupIds() != QStringList{QString::fromStdString(l1), QString::fromStdString(l2)}) {
        fail("the live session does not hold its last save's two records");
    }
    if (!seabass::infrastructure::backup::interruptedSaveRecords(seabass::pathToUtf8(backups)).empty()) {
        fail("the note is still there, so it and not the session would keep the live records");
    }

    CleanupController controller;
    QSignalSpy listed(&controller, &CleanupController::staleBackupsChanged);
    QSignalSpy finished(&controller, &CleanupController::staleBackupsPruneFinished);

    // Act
    controller.loadPendingDeletionsOnly(QStringLiteral("engine"), pathToQString(engine));
    waitForListing(controller, listed);

    // Assert
    expectList(controller, {a1, a2, a3}, "case 1");
    assert(controller.staleBackups().at(0).toMap().value("label").toString() == "earlier-a");
    std::cout << "case 1 (staleBackups lists the earlier saves' automatic records, oldest first; "
                 "not the user's, not the live session's) OK\n";

    // Case 2: the removal.
    // Arrange
    const std::string logBefore = read(logPath);

    // Act
    controller.pruneStaleBackups();
    assert(controller.pruningBackups());
    const QVariantMap removed = waitForPrune(finished);

    // Assert
    assert(removed.value("error").toString().isEmpty());
    assert(removed.value("written").toInt() == 3);
    assert(removed.value("total").toInt() == 3);
    assert(removed.value("unit").toString() == "backups");
    assert(removed.value("verb").toString() == "deleted");
    assert(!removed.value("cancelled").toBool());
    assert(!removed.contains("warning"));
    for (const auto &id : {a1, a2, a3}) {
        if (!gone(backups, id)) {
            fail("case 2: " + id + " is still on the stick");
        }
    }
    for (const auto &id : {mine, l1, l2}) {
        if (!whole(backups, id)) {
            fail("case 2: " + id + " was touched");
        }
    }
    const std::string logAfter = read(logPath);
    assert(logAfter.compare(0, logBefore.size(), logBefore) == 0);
    const auto lines = messages(logAfter.substr(logBefore.size()));
    for (const auto &id : {a1, a2, a3}) {
        if (countOf(lines, DeletedLine + id) != 1) {
            fail("case 2: the log does not say once that " + id + " was deleted");
        }
    }
    for (const auto &id : {mine, l1, l2}) {
        if (mentions(lines, id)) {
            fail("case 2: the log names " + id + ", which was not deleted");
        }
    }
    assert(lines.size() == 3);
    waitForListing(controller, listed);
    expectList(controller, {}, "case 2, afterwards");
    std::cout << "case 2 (pruneStaleBackups removes exactly the listed records, one log line each) OK\n";

    // Case 3: cancelled after the first record. POSIX only, see the top.
    // Arrange: three more, listed (two on Windows, which goes straight to
    // case 4 with them).
#if !defined(_WIN32)
    const std::string c1 = automatic("later-a");
#endif
    const std::string c2 = automatic("later-b");
    const std::string c3 = automatic("later-c");
    controller.refreshStaleBackups();
    waitForListing(controller, listed);
#if defined(_WIN32)
    expectList(controller, {c2, c3}, "case 4, before");
#else
    expectList(controller, {c1, c2, c3}, "case 3, before");

    // The stick's log becomes a FIFO whose buffer is full: the worker's
    // first line, written after the first record is removed and before
    // the cancel is read for the second, blocks until the test drains it.
    const fs::path logAside = logPath.parent_path() / "seabass.log.case2";
    fs::rename(logPath, logAside);
    if (::mkfifo(logPath.c_str(), 0600) != 0) {
        fail("case 3: no FIFO for the stick log");
    }
    const int fifo = ::open(logPath.c_str(), O_RDWR | O_NONBLOCK);
    if (fifo < 0) {
        fail("case 3: the FIFO did not open");
    }
    {
        const std::string filler(4096, '\n');
        while (::write(fifo, filler.data(), filler.size()) > 0) {
        }
        while (::write(fifo, "\n", 1) > 0) {
        }
        if (errno != EAGAIN) {
            fail("case 3: the FIFO did not fill");
        }
    }
    std::string piped;
    auto drain = [&]() {
        char buffer[65536];
        ssize_t n = 0;
        while ((n = ::read(fifo, buffer, sizeof buffer)) > 0) {
            piped.append(buffer, static_cast<size_t>(n));
        }
    };
    QFileSystemWatcher watcher({pathToQString(backups)});
    QSignalSpy backupsChanged(&watcher, &QFileSystemWatcher::directoryChanged);

    // Act
    controller.pruneStaleBackups();
    for (int i = 0; i < 300 && !gone(backups, c1); ++i) {
        backupsChanged.wait(100);
    }
    if (!gone(backups, c1)) {
        fail("case 3: the first record was never removed");
    }
    assert(controller.writeCancellable());
    controller.cancelWrite();
    for (int i = 0; i < 300 && finished.count() == 0; ++i) {
        drain();
        finished.wait(20);
    }
    drain();
    const QVariantMap stopped = waitForPrune(finished);
    ::close(fifo);
    fs::remove(logPath);
    fs::rename(logAside, logPath);

    // Assert
    assert(stopped.value("error").toString().isEmpty());
    if (!stopped.value("cancelled").toBool()) {
        fail("case 3: the summary does not say cancelled");
    }
    assert(stopped.value("written").toInt() == 1);
    assert(stopped.value("total").toInt() == 3);
    assert(!stopped.contains("warning"));
    for (const auto &id : {c2, c3}) {
        if (!whole(backups, id)) {
            fail("case 3: " + id + " is not whole after the cancel");
        }
    }
    const auto pipedLines = messages(piped);
    assert(pipedLines.size() == 1);
    assert(countOf(pipedLines, DeletedLine + c1) == 1);
    waitForListing(controller, listed);
    expectList(controller, {c2, c3}, "case 3, afterwards");
    std::cout << "case 3 (cancelled after the first record: 1 of 3 deleted, the rest whole and still listed) OK\n";
#endif

    // Case 4: a record that will not delete.
    // Arrange: nothing inside c2 can be unlinked, so remove_all() destroys
    // none of it. Both of its files must be refused: remove_all() takes
    // them in directory order, and one deletable file would leave a
    // half-emptied record behind the refusal.
    const fs::path lockedDir = backups / seabass::pathFromUtf8(c2);
#if defined(_WIN32)
    // Each file held open with no sharing: the sharing violation a real
    // stick gives when an archiver or a preview pane has the zip open,
    // which is the Windows case of #10.
    const fs::path heldFiles[2] = {lockedDir / ".manifest", lockedDir / "backup.zip"};
    HANDLE held[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    for (int i = 0; i < 2; ++i) {
        held[i] = CreateFileW(heldFiles[i].c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (held[i] == INVALID_HANDLE_VALUE) {
            fail("case 4: could not hold " + seabass::pathToUtf8(heldFiles[i].filename()) + " open");
        }
    }
    auto releaseHold = [&]() {
        for (HANDLE handle : held) {
            CloseHandle(handle);
        }
    };
#else
    // The record's directory read-only (case 8 of
    // filesystem_backup_store_test). Root ignores the bits, and a run
    // that cannot refuse proves nothing: a failure, not a skip.
    if (::geteuid() == 0) {
        fail("case 4: running as root, a read-only record cannot refuse a delete");
    }
    const fs::perms originalPerms = fs::status(lockedDir).permissions();
    fs::permissions(lockedDir, fs::perms::owner_read | fs::perms::owner_exec);
    auto releaseHold = [&]() { fs::permissions(lockedDir, originalPerms); };
#endif
    const std::string logBeforeRefusal = read(logPath);

    // Act
    controller.pruneStaleBackups();
    const QVariantMap refused = waitForPrune(finished);
    waitForListing(controller, listed);
    releaseHold();

    // Assert
    assert(refused.value("error").toString().isEmpty());
    assert(!refused.value("cancelled").toBool());
    assert(refused.value("written").toInt() == 1);
    assert(refused.value("total").toInt() == 2);
    if (refused.value("warning").toString()
        != QStringLiteral("1 backup(s) could not be deleted and are still on the stick.")) {
        fail("case 4: the summary does not count the one that failed");
    }
    if (!whole(backups, c2)) {
        fail("case 4: the refused record is not whole");
    }
    if (!gone(backups, c3)) {
        fail("case 4: the record after the refused one was not deleted");
    }
    const auto refusalLines = messages(read(logPath).substr(logBeforeRefusal.size()));
    assert(refusalLines.size() == 2);
    assert(countOf(refusalLines, FailedLine + c2) == 1);
    assert(countOf(refusalLines, DeletedLine + c3) == 1);
    expectList(controller, {c2}, "case 4, afterwards");
    std::cout << "case 4 (a record the filesystem refuses is counted failed, logged, and stays listed) OK\n";

    registry->closeSession(libraryId);
    std::cout << "cleanup_prune_stale_backups_test: all passed\n";
    return 0;
}
