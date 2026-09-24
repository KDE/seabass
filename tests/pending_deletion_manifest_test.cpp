// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "infrastructure/cleanup/pending_deletion_manifest.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::cleanup;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_pending_deletion_manifest_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path manifestPath = root / ".seabass-pending-deletions.jsonl";

    // A fresh manifest that doesn't exist yet on disk lists as empty,
    // not an error.
    {
        PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));
        assert(manifest.list().empty());
        std::cout << "case 1 (missing file -> empty list) OK\n";
    }

    // Append/list round trip, including a value with characters that
    // need JSON escaping (quotes, backslash, a real path separator).
    {
        PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));

        PendingDeletion a;
        a.format = "rekordbox";
        a.filePath = "/Volumes/STICK/Contents/track \"one\".mp3";
        a.title = "Voices In My Head";
        a.artist = "Artist A";
        a.backupId = "20260101T000000-duplicate-file-cleanup";
        manifest.append(a);

        PendingDeletion b;
        b.format = "engine";
        b.filePath = "C:\\Music\\track two.flac";
        b.title = "Track Two";
        b.artist = "Artist B";
        b.backupId = "20260101T000001-duplicate-file-cleanup";
        manifest.append(b);

        auto entries = manifest.list();
        assert(entries.size() == 2);

        assert(entries[0].format == "rekordbox");
        assert(entries[0].filePath == "/Volumes/STICK/Contents/track \"one\".mp3");
        assert(entries[0].title == "Voices In My Head");
        assert(entries[0].artist == "Artist A");
        assert(entries[0].backupId == "20260101T000000-duplicate-file-cleanup");
        assert(!entries[0].timestampUtc.empty());

        assert(entries[1].format == "engine");
        assert(entries[1].filePath == "C:\\Music\\track two.flac");
        assert(entries[1].title == "Track Two");

        std::cout << "case 2 (append/list round trip, JSON escaping) OK\n";
    }

    // A second manifest instance opened on the same path sees prior
    // entries plus its own new append -- confirms append-only, not
    // truncate-on-open.
    {
        PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));
        PendingDeletion c;
        c.format = "rekordbox";
        c.filePath = "/Volumes/STICK/Contents/track three.mp3";
        c.title = "Track Three";
        c.artist = "Artist C";
        c.backupId = "20260101T000002-duplicate-file-cleanup";
        manifest.append(c);

        assert(manifest.list().size() == 3);
        std::cout << "case 3 (append-only across instances) OK\n";
    }

    // removeProcessed() drops only the matching entries, keeps every
    // other entry's original timestamp untouched, and is a no-op when
    // nothing matches.
    {
        PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));
        auto before = manifest.list();
        assert(before.size() == 3);
        std::string keptTimestamp = before[2].timestampUtc;

        manifest.removeProcessed({"/Volumes/STICK/Contents/track \"one\".mp3"});
        auto after = manifest.list();
        assert(after.size() == 2);
        assert(after[0].filePath == "C:\\Music\\track two.flac");
        assert(after[1].filePath == "/Volumes/STICK/Contents/track three.mp3");
        assert(after[1].timestampUtc == keptTimestamp);

        manifest.removeProcessed({"/no/such/path"});
        assert(manifest.list().size() == 2);
        std::cout << "case 4 (removeProcessed drops matches, preserves timestamps, no-ops otherwise) OK\n";
    }

    // A line that names no file is dropped on read, and a rewrite takes
    // it off the disk. Without this, one unreadable line became an empty
    // entry that every rewrite wrote back: a real stick carried 1707 of
    // them, 141 KB of records naming nothing, which can never be matched
    // or cleared.
    {
        {
            std::ofstream ofs(manifestPath, std::ofstream::app);
            ofs << R"({"timestampUtc":"","format":"","filePath":"","title":"","artist":"","backupId":""})" << "\n";
            ofs << "not json at all\n";
            ofs << R"({"timestampUtc":"2026-09-17T10:00:00Z","format":"rekordbox","filePath":"/Volumes/STICK/ok.mp3",)"
                << R"("title":"Kept","artist":"Someone","backupId":"b1"})" << "\n";
        }
        PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));
        auto listed = manifest.list();
        assert(listed.size() == 3);  // the two real ones from case 4, plus the good line just added
        for (const auto &entry : listed) {
            assert(!entry.filePath.empty());
        }

        // The rewrite keeps only what list() returned, so the blank line
        // is gone from the file as well, not merely ignored.
        manifest.removeProcessed({"/Volumes/STICK/ok.mp3"});
        std::ifstream ifs(manifestPath);
        std::string line;
        int lines = 0;
        while (std::getline(ifs, line)) {
            assert(line.find(R"("filePath":"")") == std::string::npos);
            ++lines;
        }
        assert(lines == 2);
        assert(manifest.list().size() == 2);
        std::cout << "case 5 (a record naming no file is dropped, and a rewrite removes it) OK\n";
    }

    // A write that does not land is not a write. Both halves of this
    // file used to say nothing at all when the stick refused them: the
    // append dropped the only record that a file had been orphaned, and
    // the rewrite destroyed the list before writing the new one.
    //
    // Read-only directories are the lever here, so root would sail
    // through both. Checked rather than skipped: as root the calls must
    // still succeed, which is a different assertion, not no assertion.
#if !defined(_WIN32)
    {
        const bool permissionsBind = ::geteuid() != 0;

        // ---- append into a folder nothing can write -----------------
        const fs::path lockedDir = root / "locked-append";
        fs::create_directories(lockedDir);
        const fs::path lockedManifest = lockedDir / "pending.jsonl";
        fs::permissions(lockedDir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

        PendingDeletion entry;
        entry.format = "rekordbox";
        entry.filePath = "/Volumes/STICK/Contents/orphan.mp3";
        entry.title = "Orphan";
        bool threw = false;
        try {
            PendingDeletionManifest(seabass::pathToUtf8(lockedManifest)).append(entry);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        fs::permissions(lockedDir, fs::perms::owner_all, fs::perm_options::replace);
        if (permissionsBind) {
            assert(threw && "an append that reached no file must say so, not report success");
            assert(!fs::exists(lockedManifest));
        } else {
            assert(!threw && "as root the write goes through, and must not be reported as a failure");
            assert(fs::exists(lockedManifest));
        }

        // ---- a rewrite that cannot happen keeps the whole old list ---
        const fs::path keptDir = root / "locked-rewrite";
        fs::create_directories(keptDir);
        const fs::path keptManifest = keptDir / "pending.jsonl";
        {
            PendingDeletionManifest manifest(seabass::pathToUtf8(keptManifest));
            PendingDeletion a;
            a.filePath = "/Volumes/STICK/Contents/gone.mp3";
            a.title = "Deleted just now";
            manifest.append(a);
            PendingDeletion b;
            b.filePath = "/Volumes/STICK/Contents/still-waiting.mp3";
            b.title = "Still orphaned";
            manifest.append(b);
        }
        const std::string before = [&] {
            std::ifstream ifs(keptManifest);
            return std::string(std::istreambuf_iterator<char>(ifs), {});
        }();
        assert(!before.empty());

        // The file AND the folder: a folder alone would still let a
        // rewrite open the existing file and truncate it, and a file
        // alone would still let a temp-file-and-rename replace it. Both
        // shut means nothing can be written either way, which is the
        // situation worth asking about -- what does the caller get told.
        fs::permissions(keptManifest, fs::perms::owner_read, fs::perm_options::replace);
        fs::permissions(keptDir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        PendingDeletionManifest manifest(seabass::pathToUtf8(keptManifest));
        const bool rewritten = manifest.removeProcessed({"/Volumes/STICK/Contents/gone.mp3"});
        fs::permissions(keptDir, fs::perms::owner_all, fs::perm_options::replace);
        fs::permissions(keptManifest, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);

        const std::string after = [&] {
            std::ifstream ifs(keptManifest);
            return std::string(std::istreambuf_iterator<char>(ifs), {});
        }();
        if (permissionsBind) {
            // Asserted before the return value: the entry that was being
            // KEPT is what a rewrite that truncates first destroys, and
            // that is the loss, whatever it reports.
            assert(after == before && "the old list must survive a failed rewrite in full");
            assert(manifest.list().size() == 2);
            assert(!rewritten && "a rewrite that could not happen must not report that it did");
        } else {
            assert(rewritten);
            assert(manifest.list().size() == 1);
        }
        std::cout << "case 6 (a write that cannot land is reported, and loses nothing) OK\n";

        // ---- a manifest that is there and cannot be READ -------------
        // list() answers "no entries" for that, same as for a stick that
        // never had one, so a rewrite found nothing to remove and said
        // it had brought the file in line. The files were already
        // deleted by then and every entry stayed on disk.
        const fs::path unreadableDir = root / "unreadable";
        fs::create_directories(unreadableDir);
        const fs::path unreadableManifest = unreadableDir / "pending.jsonl";
        {
            PendingDeletionManifest manifest(seabass::pathToUtf8(unreadableManifest));
            PendingDeletion a;
            a.filePath = "/Volumes/STICK/Contents/gone.mp3";
            manifest.append(a);
        }
        fs::permissions(unreadableManifest, fs::perms::none, fs::perm_options::replace);
        PendingDeletionManifest unreadable(seabass::pathToUtf8(unreadableManifest));
        const bool claimed = unreadable.removeProcessed({"/Volumes/STICK/Contents/gone.mp3"});
        fs::permissions(unreadableManifest, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace);
        if (permissionsBind) {
            assert(!claimed && "a manifest that could not be read is not a manifest with nothing to remove");
            assert(unreadable.list().size() == 1 && "and it still holds what it held");
        } else {
            assert(claimed);
        }
        std::cout << "case 7 (a manifest that cannot be read is not an empty one) OK\n";
    }
#endif

    std::cout << "all cases passed\n";
    return 0;
}
