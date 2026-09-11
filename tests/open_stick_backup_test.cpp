// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Browsing a full stick backup end to end: extract only the catalogs,
// leave the analysis files in the archive, and read the library back.
//
// The assertion that matters is the last one -- a library scanned out of
// the extracted cache reports exactly the cues the original folder does,
// even though its USBANLZ directory was never written. That is the whole
// feature, and it works because the cache carries a marker naming the
// archive (see rekordbox::anlzSourceForPioneerRoot).
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "scratch_path.hpp"
#include "application/use_cases/open_stick_backup.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace fs = std::filesystem;
using seabass::application::OpenStickBackup;
using seabass::infrastructure::rekordbox::KaitaiRekordboxReader;
using namespace seabass::infrastructure::stick_backup;

namespace
{

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A backup of a stick whose PIONEER folder is `pioneerRoot`, plus a
// plausible audio file, so the "skipped everything that is not a
// catalog" counting is exercised on something real.
void writeBackupOf(const fs::path &archivePath, const fs::path &pioneerRoot)
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
        Zip64Writer::EntrySink sink = writer.beginFile(name, 1, Compression::Deflate);
        sink.write(zip::bytesOf(std::string_view(content)));
        sink.finish();
    }
    const std::string audio(64 * 1024, '\x01');
    Zip64Writer::EntrySink sink = writer.beginFile("Contents/track.mp3", 1, Compression::Store);
    sink.write(zip::bytesOf(std::string_view(audio)));
    sink.finish();

    BackupManifest manifest;
    manifest.stickIdentifier = "uuid-open-test";
    manifest.stickLabel = "TOURSTICK";
    manifest.status = BackupStatus::Complete;
    manifest.createdAtUnix = 1'757'000'000;
    writer.finish(manifest.serialize(), std::string(ManifestEntryName), 1);
}

std::string cueDigest(const fs::path &pioneerRoot)
{
    KaitaiRekordboxReader reader(pioneerRoot.string());
    std::string out;
    for (const auto &track : reader.readAll()) {
        out += track.sourceId + "{";
        for (const auto &cue : track.cues) {
            out += (cue.kind == seabass::domain::CuePoint::Kind::Hot ? "hot" : "mem");
            out += ":" + std::to_string(cue.hotCueNumber) + "@" + std::to_string(cue.positionMs) + ":" + cue.color
                   + (cue.isLoop ? ":loop" : "") + ";";
        }
        out += "}";
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: open_stick_backup_test <anonymized_library dir>\n";
        return 2;
    }
    const fs::path fixturePioneer = fs::path(argv[1]) / "rekordbox";
    assert(fs::exists(fixturePioneer / "rekordbox" / "export.pdb"));

    const fs::path scratch = seabass::testing::scratchRoot() / "open-backup-test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    // The cache lives where the app would put it. ctest sandboxes
    // SEABASS_HOME per test; a manual run gets the same sandbox rather
    // than ~/Seabass.
    seabass::testing::sandboxSeabassHome(scratch / "home");
    const fs::path browseCache = seabass::infrastructure::paths::localBrowsedBackupsDir();
    fs::create_directories(browseCache);
    const fs::path archivePath = scratch / "TOURSTICK.zip";
    writeBackupOf(archivePath, fixturePioneer);

    // The predicate that carries the size argument.
    {
        assert(OpenStickBackup::isCatalogEntry("PIONEER/rekordbox/export.pdb"));
        assert(OpenStickBackup::isCatalogEntry("PIONEER/rekordbox/exportLibrary.db"));
        assert(OpenStickBackup::isCatalogEntry("Engine Library/Database2/m.db"));
        assert(OpenStickBackup::isAnalysisEntry("PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.EXT"));
        assert(!OpenStickBackup::isCatalogEntry("PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.EXT"));
        assert(!OpenStickBackup::isCatalogEntry("Contents/track.mp3"));
        // Per-track Engine data that nothing reads: waveform overviews
        // come out of m.db through libdjinterop, not from these.
        assert(!OpenStickBackup::isCatalogEntry(
            "Engine Library/Database2/OverviewData/20e9f3a8-b5e1-424c-bed3-95cfcbab6655/1050.rgb"));
        assert(!OpenStickBackup::isCatalogEntry("PIONEER/rekordbox/share/anything.bin"));
        std::cout << "case 1 (what counts as a catalog) OK\n";
    }

    const fs::path cache = browseCache / "open-backup-test";
    const auto opened = OpenStickBackup::execute(archivePath, cache);

    // Only the catalogs came out, and the analysis files were left alone.
    {
        assert(opened.error.empty());
        assert(opened.libraryRoot == cache);
        assert(opened.stickLabel == "TOURSTICK");
        assert(opened.filesExtracted > 0);
        assert(opened.analysisFilesLeftInArchive > 0);
        assert(opened.otherEntriesSkipped >= 1);  // the audio file
        assert(fs::exists(cache / "PIONEER" / "rekordbox" / "export.pdb"));
        assert(!fs::exists(cache / "PIONEER" / "USBANLZ"));
        std::cout << "case 2 (only catalogs extracted) OK -- " << opened.filesExtracted << " files, "
                  << opened.bytesExtracted / 1024 << " KiB, " << opened.analysisFilesLeftInArchive
                  << " analysis files left in the archive\n";
    }

    // What was left behind dominates: the point of not extracting it.
    {
        std::uint64_t anlzBytes = 0;
        for (const auto &entry : fs::recursive_directory_iterator(fixturePioneer / "USBANLZ")) {
            if (entry.is_regular_file()) {
                anlzBytes += entry.file_size();
            }
        }
        assert(anlzBytes > opened.bytesExtracted);
        std::cout << "         analysis files not extracted: " << anlzBytes / 1024 << " KiB vs "
                  << opened.bytesExtracted / 1024 << " KiB of catalog\n";
    }

    // The marker is what makes the cache self-describing.
    {
        const fs::path marker = cache / seabass::infrastructure::local::BrowsedBackupMarkerName;
        assert(fs::exists(marker));
        std::string line;
        std::string rootLine;
        std::ifstream in(marker);
        std::getline(in, line);
        std::getline(in, rootLine);
        assert(line == fs::absolute(archivePath).string());
        assert(fs::path(rootLine) == fs::weakly_canonical(cache));
        std::cout << "case 3 (cache names its archive) OK\n";
    }

    // The whole feature: the same cues, off a directory that has no
    // analysis files in it at all.
    {
        const std::string fromFolder = cueDigest(fixturePioneer);
        const std::string fromBackup = cueDigest(cache / "PIONEER");
        assert(!fromFolder.empty());
        if (fromFolder != fromBackup) {
            std::cerr << "cue digests differ between the folder and the browsed backup\n";
            return 1;
        }
        std::cout << "case 4 (browsed backup reports identical cues) OK\n";
    }

    // A cache whose archive has gone away still browses the catalogs it
    // extracted -- just without cues -- rather than failing the scan.
    {
        const fs::path movedAside = scratch / "moved-away.zip";
        fs::rename(archivePath, movedAside);
        KaitaiRekordboxReader reader((cache / "PIONEER").string());
        const auto tracks = reader.readAll();
        assert(!tracks.empty());
        std::size_t cues = 0;
        for (const auto &track : tracks) {
            cues += track.cues.size();
        }
        assert(cues == 0);
        fs::rename(movedAside, archivePath);
        std::cout << "case 5 (archive gone: catalogs still browse, no cues) OK\n";
    }

    // A failed re-open leaves yesterday's cache exactly as it was. There
    // are two ways for an open to fail and only the second one puts the
    // cache in danger, so the case has to cover it: refusing a file that
    // is not an archive happens before anything is written, while an
    // archive that reads fine and only *then* turns out to hold no
    // catalog fails at the end -- past the point where an extraction
    // straight into the cache would already have emptied it.
    {
        assert(fs::exists(cache / "PIONEER" / "rekordbox" / "export.pdb"));
        const fs::path movedAside = scratch / "good.zip";
        fs::rename(archivePath, movedAside);

        // Not an archive at all: refused before the cache is touched.
        std::ofstream(archivePath) << "this replaced the backup and is not a zip";
        {
            const auto failed = OpenStickBackup::execute(archivePath, cache);
            assert(!failed.error.empty());
            assert(fs::exists(cache / "PIONEER" / "rekordbox" / "export.pdb"));
        }

        // A readable archive holding nothing to extract: the open gets
        // all the way to "no catalog in there" and must still not have
        // touched the cache on its way.
        fs::remove(archivePath);
        {
            PosixArchiveFile file(archivePath, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            const std::string audio(1024, '\x02');
            Zip64Writer::EntrySink sink = writer.beginFile("Contents/track.mp3", 1, Compression::Store);
            sink.write(zip::bytesOf(std::string_view(audio)));
            sink.finish();
            writer.finish("", std::string(ManifestEntryName), 1);
        }
        const auto failed = OpenStickBackup::execute(archivePath, cache);
        assert(!failed.error.empty());
        assert(fs::exists(cache / "PIONEER" / "rekordbox" / "export.pdb"));
        assert(fs::exists(cache / seabass::infrastructure::local::BrowsedBackupMarkerName));
        assert(!fs::exists(fs::path(cache.string() + ".partial")));

        fs::remove(archivePath);
        fs::rename(movedAside, archivePath);
        // Intact, not merely present: with its archive back, the cache
        // reports the cues it did before the two failed opens.
        assert(cueDigest(cache / "PIONEER") == cueDigest(fixturePioneer));
        std::cout << "case 5b (failed re-open keeps the previous cache) OK\n";
    }

    // An entry name that would resolve outside the cache is refused, and
    // the open fails rather than skipping it: a backup with malformed
    // catalog entries is not one to browse. Backslash segments have no
    // '/' after the prefix, so isCatalogEntry alone would let them in.
    {
        const fs::path evil = scratch / "evil.zip";
        {
            PosixArchiveFile file(evil, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            const std::string content = "not really a database";
            Zip64Writer::EntrySink sink =
                writer.beginFile("PIONEER/rekordbox/..\\..\\..\\evil.pdb", 1, Compression::Store);
            sink.write(zip::bytesOf(std::string_view(content)));
            sink.finish();
            writer.finish("", std::string(ManifestEntryName), 1);
        }
        const auto refused = OpenStickBackup::execute(evil, browseCache / "cache-evil");
        assert(!refused.error.empty());
        assert(refused.error.find("evil.pdb") != std::string::npos);
        assert(!fs::exists(browseCache / "evil.pdb"));
        assert(!fs::exists(browseCache.parent_path() / "evil.pdb"));
        assert(!fs::exists(browseCache / "cache-evil"));
        std::cout << "case 5c (entry escaping the cache refused) OK\n";
    }

    // After the double failure at the end of a swap, yesterday's cache
    // survives only as <cache>.old. Opening again must put it back before
    // anything else -- and if that open then fails, it is still there.
    {
        const fs::path retired = fs::path(cache.string() + ".old");
        fs::rename(cache, retired);
        assert(!fs::exists(cache) && fs::exists(retired));
        const fs::path movedAside = scratch / "good2.zip";
        fs::rename(archivePath, movedAside);
        std::ofstream(archivePath) << "not a zip either";
        const auto failed = OpenStickBackup::execute(archivePath, cache);
        assert(!failed.error.empty());
        assert(fs::exists(cache / "PIONEER" / "rekordbox" / "export.pdb"));
        assert(seabass::infrastructure::local::isBrowsedBackupRoot(cache));
        assert(!fs::exists(retired));
        fs::remove(archivePath);
        fs::rename(movedAside, archivePath);
        std::cout << "case 5e (retired copy put back when it is all there is) OK\n";
    }

    // The marker names the directory it was written for. Copy the cache
    // somewhere else, marker and all, and it is an ordinary folder: the
    // reader looks for USBANLZ beside the databases, finds none, and the
    // tracks read as having no cues -- never as a browsed backup.
    {
        const fs::path copied = scratch / "copied-elsewhere";
        fs::copy(cache, copied, fs::copy_options::recursive);
        assert(fs::exists(copied / seabass::infrastructure::local::BrowsedBackupMarkerName));
        assert(!seabass::infrastructure::local::isBrowsedBackupRoot(copied));
        KaitaiRekordboxReader reader((copied / "PIONEER").string());
        std::size_t cues = 0;
        for (const auto &track : reader.readAll()) {
            cues += track.cues.size();
        }
        assert(cues == 0);
        std::cout << "case 5d (marker copied elsewhere is ignored) OK\n";
    }

    // A file that is not an archive at all is refused with a message.
    {
        const fs::path notAZip = scratch / "notes.txt";
        std::ofstream(notAZip) << "definitely not a zip";
        const auto failed = OpenStickBackup::execute(notAZip, scratch / "cache2");
        assert(!failed.error.empty());
        assert(failed.libraryRoot.empty());
        std::cout << "case 6 (not an archive refused) OK\n";
    }

    fs::remove_all(scratch);
    std::cout << "open_stick_backup_test: all cases passed\n";
    return 0;
}
