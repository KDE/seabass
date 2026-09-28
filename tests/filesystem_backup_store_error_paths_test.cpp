// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FilesystemBackupStore's refusals: a record whose manifest is missing or
// damaged, an archive that is not a file or not there, and writes the
// filesystem refuses. Each case asserts the store's own answer (the
// message, the return) and that nothing half-written is left behind:
// the record is what it was, and the file a restore would have put back
// is untouched.

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::backup;
using seabass::application::BackupOrigin;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Runs `call` and returns what it threw, or an empty string.
template<typename F>
std::string thrownBy(F &&call)
{
    try {
        call();
    } catch (const std::exception &e) {
        return e.what();
    }
    return {};
}

size_t entriesIn(const fs::path &dir)
{
    size_t count = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        ++count;
    }
    return count;
}

// A stick with one file on it and one automatic record of that file,
// after which the file is changed: what a restore would overwrite.
struct Stick
{
    fs::path root;
    fs::path backups;
    fs::path file;
    FilesystemBackupStore store;
    seabass::application::BackupRecord record;
    fs::path dir;
    fs::path manifest;
    fs::path archive;

    explicit Stick(const fs::path &at)
        : root(at), backups(at / "Seabass" / "backups"), file(at / "PIONEER" / "rekordbox" / "export.pdb"),
          store(pathToUtf8(backups))
    {
        fs::remove_all(root);
        writeFile(file, "what was backed up");
        record = store.backup({pathToUtf8(file)}, "sync");
        dir = pathFromUtf8(record.path);
        manifest = dir / ".manifest";
        archive = dir / "backup.zip";
        assert(fs::is_regular_file(manifest) && fs::is_regular_file(archive) && "the precondition: a whole record");
        writeFile(file, "what is there now");
    }
};

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_filesystem_backup_store_error_paths_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // The manifest is gone. The record is not listed, not restorable, and
    // not appended to; the file stays as it is.
    {
        Stick s(root / "no-manifest");
        fs::remove(s.manifest);
        assert(s.store.list().empty() && "a directory without its manifest is not a record");
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restore(s.record.id));
        assert(s.store.lastRestoreError() == "backup " + s.record.id + " lists no files");
        assert(readFile(s.file) == "what is there now");
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(s.file)}); });
        assert(refusal == "backup " + s.record.id + " is not a record this build wrote");
        assert(!fs::exists(s.manifest) && "a refused append writes no manifest");
        std::cout << "case 1 (a record whose manifest is missing is refused everywhere) OK\n";
    }

    // Not a record at all: ids that name nothing.
    {
        Stick s(root / "no-record");
        const std::string ghost = "20000101T000000-ghost";
        assert(!s.store.restore(ghost));
        assert(s.store.lastRestoreError() == "backup " + ghost + " is not on the stick");
        assert(!s.store.isRestorable(ghost));
        assert(!s.store.remove(ghost));
        assert(!s.store.restoreSpaceNeeded(ghost).has_value());
        const std::string refusal = thrownBy([&] { s.store.addToArchive(ghost, {pathToUtf8(s.file)}); });
        assert(refusal == "no backup with id " + ghost + " to add to");
        s.store.setDescription(ghost, "never lands anywhere");
        assert(!fs::exists(s.backups / ghost) && "a description for no record makes no directory");
        assert(entriesIn(s.backups) == 1);
        std::cout << "case 2 (an id that names no record is refused by every call) OK\n";
    }

    // A manifest from a newer build. Refused for restore and for append,
    // and the append leaves both the manifest and the archive as they
    // were.
    {
        Stick s(root / "wrong-version");
        const std::string original = readFile(s.manifest);
        const std::string newer = "MANIFEST-VERSION\t5" + original.substr(original.find('\n'));
        writeFile(s.manifest, newer);
        const auto archiveBefore = readFile(s.archive);
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restore(s.record.id));
        assert(s.store.lastRestoreError() == "backup " + s.record.id + " is not a record this build wrote");
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(s.file)}); });
        assert(refusal == "backup " + s.record.id + " is not a record this build wrote");
        assert(readFile(s.manifest) == newer);
        assert(readFile(s.archive) == archiveBefore);
        assert(readFile(s.file) == "what is there now");
        std::cout << "case 3 (a manifest from another version is refused, record untouched) OK\n";
    }

    // A manifest cut short in the middle of its entry line: the recorded
    // path is a prefix of the real one. Restoring from it would put the
    // file back under the wrong name and call that a success.
    {
        Stick s(root / "truncated-manifest");
        const std::string original = readFile(s.manifest);
        assert(original.back() == '\n');
        const std::string cut = original.substr(0, original.size() - 5);
        writeFile(s.manifest, cut);
        assert(s.store.list().empty() && "a manifest cut short says nothing about whose record this is");
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restore(s.record.id));
        assert(contains(s.store.lastRestoreError(), "manifest") && contains(s.store.lastRestoreError(), "damaged"));
        assert(readFile(s.file) == "what is there now");
        size_t filesUnderStick = 0;
        for (const auto &entry : fs::recursive_directory_iterator(s.root / "PIONEER")) {
            filesUnderStick += entry.is_regular_file() ? 1 : 0;
        }
        assert(filesUnderStick == 1 && "nothing was written under a truncated name");
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(s.file)}); });
        assert(contains(refusal, "manifest") && contains(refusal, "damaged"));
        assert(readFile(s.manifest) == cut && "a refused append does not rewrite the manifest");
        std::cout << "case 4 (a manifest cut short is damaged: not listed, not restored, not appended to) OK\n";
    }

    // A manifest holding a line this build never writes (no tab): the
    // record is refused rather than restored from whatever lines parse.
    {
        Stick s(root / "garbage-line");
        const std::string original = readFile(s.manifest);
        const size_t afterHeader = original.find('\n', original.find('\n') + 1) + 1;
        const std::string damaged = original.substr(0, afterHeader) + "this line has no tab\n" + original.substr(afterHeader);
        writeFile(s.manifest, damaged);
        assert(s.store.list().empty());
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restore(s.record.id));
        assert(contains(s.store.lastRestoreError(), "manifest") && contains(s.store.lastRestoreError(), "damaged"));
        assert(readFile(s.file) == "what is there now");
        std::cout << "case 5 (a manifest with an unparsable line is damaged and refused) OK\n";
    }

    // The archive is gone. Refused for restore with the reason, no space
    // figure is made up for it, and an append does not start a fresh
    // archive under a manifest that names entries it would not hold.
    {
        Stick s(root / "archive-removed");
        fs::remove(s.archive);
        const std::string manifestBefore = readFile(s.manifest);
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restoreSpaceNeeded(s.record.id).has_value());
        assert(!s.store.restore(s.record.id));
        assert(s.store.lastRestoreError() == "the backup's archive is missing");
        assert(s.store.list().size() == 1 && "no pre-restore copy was made for a refused restore");
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(s.file)}); });
        assert(contains(refusal, "archive") && contains(refusal, "missing"));
        assert(!fs::exists(s.archive) && "no archive was started that lacks the manifest's entries");
        assert(readFile(s.manifest) == manifestBefore);
        assert(readFile(s.file) == "what is there now");
        std::cout << "case 6 (a record whose archive is gone is refused, nothing started in its place) OK\n";
    }

    // A directory where the archive should be: its size cannot be read,
    // so an append is refused before anything is written, and a restore
    // says the archive cannot be used.
    {
        Stick s(root / "archive-is-a-directory");
        fs::remove(s.archive);
        fs::create_directories(s.archive / "inside");
        const std::string manifestBefore = readFile(s.manifest);
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(s.file)}); });
        assert(contains(refusal, "could not read the size of ") && contains(refusal, "backup.zip"));
        assert(fs::is_directory(s.archive / "inside") && "the directory in its place is left alone");
        assert(readFile(s.manifest) == manifestBefore);
        assert(!s.store.isRestorable(s.record.id));
        assert(!s.store.restore(s.record.id));
        assert(contains(s.store.lastRestoreError(), "the backup's archive "));
        assert(readFile(s.file) == "what is there now");
        std::cout << "case 7 (a directory in the archive's place: its size is unreadable, append refused) OK\n";
    }

    // A manifest that names an entry the archive does not hold: refused
    // before anything is copied, naming the entry.
    {
        Stick s(root / "entry-missing");
        const std::string original = readFile(s.manifest);
        const std::string entry = "PIONEER/rekordbox/export.pdb\t";
        const size_t at = original.find(entry);
        assert(at != std::string::npos && "the precondition: the entry line is where this test looks");
        std::string renamed = original;
        renamed.replace(at, entry.size(), "PIONEER/rekordbox/export.pdX\t");
        writeFile(s.manifest, renamed);
        assert(!s.store.restore(s.record.id));
        assert(s.store.lastRestoreError() == "the backup's archive lacks PIONEER/rekordbox/export.pdX");
        assert(s.store.list().size() == 1 && "no pre-restore copy was made");
        assert(readFile(s.file) == "what is there now");
        std::cout << "case 8 (a manifest entry the archive lacks is refused by name) OK\n";
    }

    // A path that is not there is left out of the record rather than
    // recorded as a file it does not hold.
    {
        Stick s(root / "absent-source");
        const fs::path absent = s.root / "PIONEER" / "rekordbox" / "never-written.pdb";
        const auto record = s.store.backup({pathToUtf8(absent), pathToUtf8(s.file)}, "sync");
        assert(record.filePaths.size() == 1 && record.filePaths[0] == "PIONEER/rekordbox/export.pdb");
        assert(!contains(readFile(pathFromUtf8(record.path) / ".manifest"), "never-written"));
        std::cout << "case 9 (a file that is not there is left out of the record) OK\n";
    }

    // A description is kept beside the record and read back by list(),
    // replaced whole by the next one.
    {
        Stick s(root / "description");
        s.store.setDescription(s.record.id, "before the Berlin gig");
        assert(s.store.list().at(0).description == "before the Berlin gig");
        s.store.setDescription(s.record.id, "after");
        assert(s.store.list().at(0).description == "after");
        std::cout << "case 10 (a description is stored, listed and replaced) OK\n";
    }

    // Writes the filesystem refuses. POSIX only, and not as root: root
    // writes into a 0500 directory anyway, so the case would pass
    // without the refusal ever happening.
#if !defined(_WIN32)
    if (::geteuid() == 0) {
        std::cout << "skipped the read-only cases: running as root, which ignores directory permissions\n";
    } else {
        const auto readOnly = [](const fs::path &dir) {
            fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        };
        const auto writable = [](const fs::path &dir) {
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        };

        // A new record in a backups folder nothing can write to.
        {
            Stick s(root / "read-only-backups");
            readOnly(s.backups);
            const std::string refusal = thrownBy([&] { s.store.backup({pathToUtf8(s.file)}, "add-cue"); });
            writable(s.backups);
            assert(!refusal.empty() && "a backup that cannot be written says so");
            assert(entriesIn(s.backups) == 1 && "and leaves no directory of its own behind");
            std::cout << "case 11 (a backup into a read-only folder is refused, nothing left) OK\n";
        }

        // An append whose archive grows but whose manifest cannot be
        // replaced: the archive is cut back to its old length, the
        // manifest stays, and the record still restores.
        {
            Stick s(root / "read-only-record");
            const auto archiveBefore = readFile(s.archive);
            const auto manifestBefore = readFile(s.manifest);
            const fs::path second = s.root / "PIONEER" / "USBANLZ" / "ANLZ0000.DAT";
            writeFile(second, seabass::testing::incompressible(8192, 7));
            readOnly(s.dir);
            const std::string refusal = thrownBy([&] { s.store.addToArchive(s.record.id, {pathToUtf8(second)}); });
            writable(s.dir);
            assert(contains(refusal, "could not write the manifest of backup " + s.record.id));
            assert(readFile(s.archive) == archiveBefore && "the archive is cut back to what it was");
            assert(readFile(s.manifest) == manifestBefore);
            assert(s.store.restore(s.record.id) && "the record still restores");
            assert(readFile(s.file) == "what was backed up");
            std::cout << "case 12 (an append whose manifest cannot be written is rolled back) OK\n";
        }

        // A restore that cannot keep a copy of what it would overwrite
        // does not overwrite it.
        {
            Stick s(root / "no-room-for-pre-restore");
            readOnly(s.backups);
            const bool restored = s.store.restore(s.record.id);
            writable(s.backups);
            assert(!restored);
            assert(contains(s.store.lastRestoreError(), "could not keep a copy of what the restore would overwrite: "));
            assert(!s.store.lastPreRestoreId().has_value());
            assert(readFile(s.file) == "what is there now" && "nothing was put back without its undo copy");
            assert(entriesIn(s.backups) == 1);
            std::cout << "case 13 (a restore that cannot copy what it overwrites changes nothing) OK\n";
        }
    }
#endif

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
