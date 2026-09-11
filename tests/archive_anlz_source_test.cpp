// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Browsing a stick backup in place: the cues a library reports must be
// identical whether its analysis files come off a real PIONEER folder or
// out of the ZIP, with nothing extracted.
//
// Runs against the committed anonymized library fixture, so this is real
// rekordbox data (real ANLZ .EXT sections, real cue colors), not
// synthesised bytes -- the point is that the parse is unchanged, and only
// a synthetic archive of the real thing can show that.
//
// Both compression methods are covered: a backup of a stick may store or
// deflate an entry, and the reader inflates raw deflate streams itself.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "scratch_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/archive_anlz_source.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::stick_backup;
using seabass::infrastructure::rekordbox::AnlzByteSource;
using seabass::infrastructure::rekordbox::anlzRelativePath;
using seabass::infrastructure::stick_backup::ArchiveAnlzSource;
using seabass::infrastructure::rekordbox::FilesystemAnlzSource;
using seabass::infrastructure::rekordbox::KaitaiRekordboxReader;

namespace
{

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A stick backup of `pioneerRoot`, entries named the way a real backup of
// a stick root names them ("PIONEER/...").
void writeArchiveOf(const fs::path &archivePath, const fs::path &pioneerRoot, Compression compression)
{
    PosixArchiveFile file(archivePath, PosixArchiveFile::OpenMode::ReadWrite);
    Zip64Writer writer(file, {});
    for (const auto &entry : fs::recursive_directory_iterator(pioneerRoot)) {
        const std::string name = "PIONEER/" + fs::relative(entry.path(), pioneerRoot).generic_string();
        if (entry.is_directory()) {
            writer.addDirectory(name + "/", 1);
            continue;
        }
        const std::string content = readWholeFile(entry.path());
        Zip64Writer::EntrySink sink = writer.beginFile(name, 1, compression);
        sink.write(zip::bytesOf(std::string_view(content)));
        sink.finish();
    }
    writer.finish("{}", "MANIFEST.json", 1);
}

struct CueDigest
{
    std::string trackSourceId;
    std::size_t cueCount = 0;
    std::string detail;  // every cue, so a difference names itself
};

std::vector<CueDigest> digestOf(KaitaiRekordboxReader &reader)
{
    std::vector<CueDigest> out;
    for (const auto &track : reader.readAll()) {
        CueDigest d;
        d.trackSourceId = track.sourceId;
        d.cueCount = track.cues.size();
        for (const auto &cue : track.cues) {
            d.detail += (cue.kind == seabass::domain::CuePoint::Kind::Hot ? "hot" : "mem");
            d.detail += ":" + std::to_string(cue.hotCueNumber) + "@" + std::to_string(cue.positionMs) + ":"
                        + cue.color + (cue.isLoop ? ":loop" : "") + ";";
        }
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: archive_anlz_source_test <anonymized_library dir>\n";
        return 2;
    }
    const fs::path fixture = fs::path(argv[1]) / "rekordbox";
    if (!fs::exists(fixture / "rekordbox" / "export.pdb")) {
        std::cerr << "fixture missing: " << fixture << "\n";
        return 1;
    }

    const fs::path scratch = seabass::testing::scratchRoot() / "archive-anlz-test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    // Baseline: the library as read off a real folder.
    KaitaiRekordboxReader onDisk(fixture.string());
    const std::vector<CueDigest> expected = digestOf(onDisk);
    assert(!expected.empty());
    std::size_t totalCues = 0;
    for (const auto &d : expected) {
        totalCues += d.cueCount;
    }
    assert(totalCues > 0 && "fixture has no cues, so this test would prove nothing");
    std::cout << "baseline: " << expected.size() << " tracks, " << totalCues << " cues from the folder\n";

    for (const auto compression : {Compression::Store, Compression::Deflate}) {
        const bool deflated = compression == Compression::Deflate;
        const fs::path archivePath = scratch / (deflated ? "deflate.zip" : "store.zip");
        writeArchiveOf(archivePath, fixture, compression);

        // export.pdb still comes from a real file (the pdb parser seeks
        // all over it); only the analysis files come from the archive.
        auto archiveFile = std::make_shared<PosixArchiveFile>(archivePath, PosixArchiveFile::OpenMode::ReadOnly);
        auto reader = std::make_shared<const Zip64Reader>(Zip64Reader::open(*archiveFile));
        auto source = std::make_shared<ArchiveAnlzSource>(reader, "PIONEER/");

        KaitaiRekordboxReader fromArchive(fixture.string(), source);
        const std::vector<CueDigest> actual = digestOf(fromArchive);

        assert(actual.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            assert(actual[i].trackSourceId == expected[i].trackSourceId);
            if (actual[i].detail != expected[i].detail) {
                std::cerr << "cue mismatch on track " << expected[i].trackSourceId << "\n  folder:  "
                          << expected[i].detail << "\n  archive: " << actual[i].detail << "\n";
                return 1;
            }
        }
        std::cout << "case " << (deflated ? "2 (deflated" : "1 (stored") << " archive) OK -- " << totalCues
                  << " cues identical, nothing extracted\n";

        // Nothing was written next to the archive: the whole point is
        // that ANLZ never lands on disk.
        std::size_t filesInScratch = 0;
        for (const auto &entry : fs::directory_iterator(scratch)) {
            filesInScratch += entry.is_regular_file() ? 1 : 0;
        }
        assert(filesInScratch <= 2);  // the two archives themselves, nothing more
    }

    // A track whose analysis file is not in the backup reads as "no
    // cues", exactly as a missing file off a real stick does.
    {
        const fs::path archivePath = scratch / "store.zip";
        auto archiveFile = std::make_shared<PosixArchiveFile>(archivePath, PosixArchiveFile::OpenMode::ReadOnly);
        auto reader = std::make_shared<const Zip64Reader>(Zip64Reader::open(*archiveFile));
        ArchiveAnlzSource source(reader, "PIONEER/");
        assert(!source.read("USBANLZ/NOPE/00000000/ANLZ0000.EXT").has_value());
        std::cout << "case 3 (analysis file absent from the backup) OK\n";
    }

    // The relative path is what both a folder and an archive entry are
    // built from, so its two forms have to be right.
    {
        const std::string stored = "/PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.DAT";
        assert(anlzRelativePath(stored, /*wantExt=*/true) == "USBANLZ/P05D/000117F3/ANLZ0000.EXT");
        assert(anlzRelativePath(stored, /*wantExt=*/false) == "USBANLZ/P05D/000117F3/ANLZ0000.DAT");
        // Already relative, or oddly rooted: left alone rather than mangled.
        assert(anlzRelativePath("USBANLZ/X/Y/ANLZ0000.DAT", false) == "USBANLZ/X/Y/ANLZ0000.DAT");
        std::cout << "case 4 (relative path forms) OK\n";
    }

    fs::remove_all(scratch);
    std::cout << "archive_anlz_source_test: all cases passed\n";
    return 0;
}
