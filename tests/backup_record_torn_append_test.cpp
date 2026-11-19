// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A save record's archive grows in place: every addToArchive() appends new
// entries, a new central directory and a new end record after the old
// ones, and only then rewrites the manifest. A process killed (or a stick
// pulled) part-way through that append leaves the old, complete archive
// followed by a torn tail, and the old manifest. Round 12's rig check F6
// hit exactly that: the whole record read as damaged ("no
// end-of-central-directory record"), so Undo Last Save could not put back
// the m.db an earlier step of the same save had already backed up and
// written.
//
// Here the torn append is made the way the kill makes it: a real append,
// then the archive cut back to the old bytes plus a prefix of what the
// append wrote, at every structural point of it, and the manifest put
// back to the one from before the append. Each time the entries recorded
// before the append restore byte for byte, the tear is reported, and the
// next append works and gives a record of one more file. A record whose
// RECORDED part is damaged is still refused.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::backup;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
namespace fs = std::filesystem;
namespace sb = seabass::infrastructure::stick_backup;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    assert(out.good());
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

constexpr std::size_t RecordedFiles = 3;

// A stick with RecordedFiles files, one save record holding all of them
// (one backup() and then an addToArchive() per further file, as a save of
// several steps makes it), and one more file the next step would add.
struct Stick
{
    fs::path root;
    FilesystemBackupStore store;
    std::string id;
    fs::path dir;
    fs::path archive;
    fs::path manifest;
    std::vector<fs::path> files;
    std::vector<std::string> originals;
    fs::path next;
    std::string nextOriginal;

    explicit Stick(const fs::path &at) : root(at), store(pathToUtf8(at / "Seabass" / "backups"))
    {
        fs::remove_all(root);
        for (std::size_t i = 0; i < RecordedFiles; ++i) {
            files.push_back(root / "Engine Library" / "Database2" / pathFromUtf8("db" + std::to_string(i) + ".db"));
            originals.push_back(seabass::testing::incompressible(6000 + 2500 * i, 11 + i));
            writeFile(files.back(), originals.back());
        }
        next = root / "PIONEER" / "rekordbox" / "export.pdb";
        nextOriginal = seabass::testing::incompressible(20000, 99);
        writeFile(next, nextOriginal);

        id = store.backup({pathToUtf8(files[0])}, "sync").id;
        for (std::size_t i = 1; i < RecordedFiles; ++i) {
            store.addToArchive(id, {pathToUtf8(files[i])});
        }
        dir = root / "Seabass" / "backups" / pathFromUtf8(id);
        archive = dir / "backup.zip";
        manifest = dir / ".manifest";
        assert(fs::is_regular_file(archive) && fs::is_regular_file(manifest));
    }

    // What the save went on to write over the recorded files.
    void changeEverything()
    {
        for (const fs::path &file : files) {
            writeFile(file, "written by the save");
        }
        writeFile(next, "written by the save");
    }

    void assertRecordedFilesBack() const
    {
        for (std::size_t i = 0; i < files.size(); ++i) {
            assert(readFile(files[i]) == originals[i] && "a recorded file comes back byte for byte");
        }
    }
};

// Where the append's structures start, relative to the end of the old
// archive: what the cut points below are measured from.
struct AppendLayout
{
    std::uint64_t oldLength = 0;
    std::uint64_t newDataStart = 0;      // first data byte of the new entry
    std::uint64_t newCentralDirectory = 0;
    std::uint64_t newEndRecord = 0;      // the 22-byte end-of-central-directory record
    std::uint64_t newLength = 0;
};

// Makes the append for real, reads where everything landed, and puts the
// record back to how a kill before the manifest write leaves it: old
// manifest, and the archive left to the caller to tear.
std::string realAppend(Stick &s, AppendLayout &layout)
{
    const std::string before = readFile(s.archive);
    const std::string manifestBefore = readFile(s.manifest);
    s.store.addToArchive(s.id, {pathToUtf8(s.next)});
    const std::string after = readFile(s.archive);
    assert(after.size() > before.size() && after.compare(0, before.size(), before) == 0
           && "an append leaves every old byte where it was");
    {
        sb::PosixArchiveFile file(s.archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
        const sb::Zip64Reader reader = sb::Zip64Reader::open(file);
        const auto index = reader.findEntry("PIONEER/rekordbox/export.pdb");
        assert(index.has_value());
        layout.oldLength = before.size();
        layout.newDataStart = reader.dataOffset(*index);
        layout.newCentralDirectory = reader.layout().centralDirectoryOffset;
        layout.newEndRecord = reader.layout().endOfCentralDirectoryOffset;
        layout.newLength = after.size();
        assert(layout.oldLength < layout.newDataStart && layout.newDataStart < layout.newCentralDirectory
               && layout.newCentralDirectory < layout.newEndRecord && layout.newEndRecord + 22 == layout.newLength);
    }
    writeFile(s.manifest, manifestBefore);
    return after;
}

void tornAt(const fs::path &root, const std::string &name, std::uint64_t (*cut)(const AppendLayout &))
{
    Stick s(root / pathFromUtf8(name));
    AppendLayout layout;
    const std::string after = realAppend(s, layout);
    const std::uint64_t length = cut(layout);
    assert(length > layout.oldLength && length < layout.newLength);
    writeFile(s.archive, after.substr(0, length));

    // Undo: the recorded files come back, and the tear is said.
    s.changeEverything();
    const bool restored = s.store.restore(s.id);
    if (!restored) {
        std::cerr << name << ": " << s.store.lastRestoreError() << "\n";
    }
    assert(restored && "the entries recorded before the torn append restore");
    s.assertRecordedFilesBack();
    assert(readFile(s.next) == "written by the save" && "the torn entry was never recorded, so nothing of it comes back");
    assert(contains(s.store.lastArchiveRepair(), "the last addition to backup " + s.id + " was incomplete"));

    // The next step of a save: the append goes onto the recorded part,
    // and the record holds one more file, read without any repair.
    writeFile(s.next, s.nextOriginal);
    const auto record = s.store.addToArchive(s.id, {pathToUtf8(s.next)});
    assert(record.filePaths.size() == RecordedFiles + 1);
    assert(contains(s.store.lastArchiveRepair(), "was incomplete"));
    {
        sb::PosixArchiveFile file(s.archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
        std::string error;
        assert(sb::Zip64Reader::tryOpen(file, &error).has_value() && "the archive reads whole again after the append");
    }
    s.changeEverything();
    assert(s.store.restore(s.id));
    assert(s.store.lastArchiveRepair().empty() && "a whole archive needs no repair");
    s.assertRecordedFilesBack();
    assert(readFile(s.next) == s.nextOriginal);
    std::cout << "torn append (" << name << "): " << RecordedFiles << " recorded files restore, the next append makes "
              << RecordedFiles + 1 << " OK\n";
}

// Flips one byte of the archive.
void damageByte(const fs::path &archive, std::uint64_t offset)
{
    std::string bytes = readFile(archive);
    assert(offset < bytes.size());
    bytes[offset] = static_cast<char>(bytes[offset] ^ 0x5a);
    writeFile(archive, bytes);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_backup_record_torn_append_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Cut points: one byte of the new local header, the middle of it, the
    // middle of the new entry's data, the middle of the new central
    // directory, everything but the final end record, and half of that.
    tornAt(root, "one-byte", [](const AppendLayout &l) { return l.oldLength + 1; });
    tornAt(root, "mid-local-header", [](const AppendLayout &l) { return l.oldLength + 17; });
    tornAt(root, "mid-data", [](const AppendLayout &l) { return l.newDataStart + (l.newCentralDirectory - l.newDataStart) / 2; });
    tornAt(root, "mid-central-directory", [](const AppendLayout &l) { return l.newCentralDirectory + 30; });
    tornAt(root, "all-but-the-end-record", [](const AppendLayout &l) { return l.newEndRecord; });
    tornAt(root, "half-the-end-record", [](const AppendLayout &l) { return l.newEndRecord + 11; });

    // Garbage rather than a prefix of a real append: what a FAT stick can
    // hand back after a pull, a file grown into clusters never written.
    {
        Stick s(root / "garbage");
        const std::string before = readFile(s.archive);
        writeFile(s.archive, before + seabass::testing::incompressible(70000, 5));
        s.changeEverything();
        assert(s.store.restore(s.id));
        s.assertRecordedFilesBack();
        assert(contains(s.store.lastArchiveRepair(), "was incomplete"));
        writeFile(s.next, s.nextOriginal);
        const auto record = s.store.addToArchive(s.id, {pathToUtf8(s.next)});
        assert(record.filePaths.size() == RecordedFiles + 1);
        assert(readFile(s.archive).compare(0, before.size(), before) == 0 && "the recorded part is never rewritten");
        s.changeEverything();
        assert(s.store.restore(s.id));
        s.assertRecordedFilesBack();
        assert(readFile(s.next) == s.nextOriginal);
        std::cout << "torn append (garbage after the end record): recorded files restore, the next append works OK\n";
    }

    // The recorded part damaged: the newest end record's central directory
    // is not what was written, and no tear explains it. The older end
    // records lack the file the last append recorded, so the archive is
    // refused, for restore and append alike, and nothing is cut.
    {
        Stick s(root / "recorded-directory-damaged");
        std::uint64_t centralDirectory = 0;
        {
            sb::PosixArchiveFile file(s.archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
            centralDirectory = sb::Zip64Reader::open(file).layout().centralDirectoryOffset;
        }
        damageByte(s.archive, centralDirectory);
        const std::string damaged = readFile(s.archive);
        s.changeEverything();
        assert(!s.store.restore(s.id));
        assert(contains(s.store.lastRestoreError(), "the backup's archive is damaged"));
        assert(readFile(s.files[0]) == "written by the save" && "a refused restore puts nothing back");
        const std::string refusal = thrownBy([&] { s.store.addToArchive(s.id, {pathToUtf8(s.next)}); });
        assert(contains(refusal, "is unreadable, so nothing is added to it"));
        assert(readFile(s.archive) == damaged && "a refused archive is not cut");
        std::cout << "damaged central directory of the recorded part: refused, nothing cut OK\n";
    }

    // The newest end record itself damaged, no tear: the same.
    {
        Stick s(root / "recorded-end-record-damaged");
        damageByte(s.archive, fs::file_size(s.archive) - 22);
        s.changeEverything();
        assert(!s.store.restore(s.id));
        assert(contains(s.store.lastRestoreError(), "the backup's archive is damaged"));
        assert(!thrownBy([&] { s.store.addToArchive(s.id, {pathToUtf8(s.next)}); }).empty());
        std::cout << "damaged end record of the recorded part: refused OK\n";
    }

    // A torn append AND the recorded part damaged under it: the end record
    // before the tear does not read, the older ones lack a recorded file.
    {
        Stick s(root / "torn-and-recorded-damaged");
        AppendLayout layout;
        const std::string after = realAppend(s, layout);
        writeFile(s.archive, after.substr(0, layout.newDataStart + 100));
        std::uint64_t oldCentralDirectory = 0;
        {
            // The old archive, as it was before the append.
            const fs::path copy = root / "old-archive.zip";
            writeFile(copy, after.substr(0, layout.oldLength));
            sb::PosixArchiveFile file(copy, sb::PosixArchiveFile::OpenMode::ReadOnly);
            oldCentralDirectory = sb::Zip64Reader::open(file).layout().centralDirectoryOffset;
        }
        damageByte(s.archive, oldCentralDirectory);
        const std::string damaged = readFile(s.archive);
        s.changeEverything();
        assert(!s.store.restore(s.id));
        assert(contains(s.store.lastRestoreError(), "the backup's archive is damaged"));
        assert(!thrownBy([&] { s.store.addToArchive(s.id, {pathToUtf8(s.next)}); }).empty());
        assert(readFile(s.archive) == damaged && "a refused archive is not cut");
        std::cout << "torn append over a damaged recorded part: refused, nothing cut OK\n";
    }

    // A torn append over a recorded entry whose data is damaged: the
    // structure reads, the checksum does not, and the restore is refused.
    {
        Stick s(root / "torn-and-recorded-data-damaged");
        AppendLayout layout;
        const std::string after = realAppend(s, layout);
        writeFile(s.archive, after.substr(0, layout.newCentralDirectory + 30));
        std::uint64_t firstData = 0;
        {
            const fs::path copy = root / "old-archive-2.zip";
            writeFile(copy, after.substr(0, layout.oldLength));
            sb::PosixArchiveFile file(copy, sb::PosixArchiveFile::OpenMode::ReadOnly);
            const sb::Zip64Reader reader = sb::Zip64Reader::open(file);
            firstData = reader.dataOffset(0);
        }
        damageByte(s.archive, firstData + 40);
        s.changeEverything();
        assert(!s.store.restore(s.id));
        assert(contains(s.store.lastRestoreError(), "damaged (checksum)"));
        assert(readFile(s.files[1]) == "written by the save" && "a refused restore puts nothing back");
        std::cout << "torn append over a recorded entry with damaged data: refused OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
