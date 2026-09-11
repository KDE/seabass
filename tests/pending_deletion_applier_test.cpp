#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

#include "infrastructure/cleanup/pending_deletion_applier.hpp"
#include "infrastructure/long_paths.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::cleanup;
namespace fs = std::filesystem;

namespace
{

void touch(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "audio data";
}

void writeLongPathFile(const fs::path &path, const std::string &content)
{
    // std::ofstream takes the path as given and cannot reach one past
    // MAX_PATH, so this goes through the prefixed path.
    fs::path full = seabass::infrastructure::longPathSafe(path);
#if defined(_WIN32)
    FILE *f = _wfopen(full.c_str(), L"wb");
#else
    FILE *f = std::fopen(full.c_str(), "wb");
#endif
    assert(f != nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

// Makes an existing file genuinely undeletable for as long as it is
// alive, and releases it on destruction.
//
// The mechanism has to differ by platform, because what stops a delete
// differs. On POSIX, removing a name is a write to the parent directory,
// so clearing that directory's write bit is enough -- and holding the
// file open would not help, since unlink() happily removes an open file.
// On Windows the opposite holds: a directory's permissions do not gate
// deleting the files inside it (and std::filesystem::permissions there
// only reaches the read-only attribute anyway, which fs::remove is free
// to clear), while a handle opened without FILE_SHARE_DELETE does block
// it. That is also the case this code path exists to survive in the
// first place -- a track still held open by rekordbox or a player.
class UndeletableFile
{
public:
    explicit UndeletableFile(const fs::path &filePath) : m_filePath(filePath)
    {
#if defined(_WIN32)
        m_handle = ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(m_handle != INVALID_HANDLE_VALUE);
#else
        fs::permissions(filePath.parent_path(), fs::perms::owner_read | fs::perms::owner_exec);
#endif
    }

    ~UndeletableFile()
    {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
        }
#else
        std::error_code ec;
        fs::permissions(m_filePath.parent_path(), fs::perms::owner_all, ec);
#endif
    }

    UndeletableFile(const UndeletableFile &) = delete;
    UndeletableFile &operator=(const UndeletableFile &) = delete;

private:
    fs::path m_filePath;
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#endif
};

PendingDeletion makeEntry(const std::string &filePath, const std::string &backupId = "backup-1")
{
    PendingDeletion e;
    e.format = "rekordbox";
    e.filePath = filePath;
    e.title = "Some Track";
    e.artist = "Some Artist";
    e.backupId = backupId;
    return e;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_pending_deletion_applier_test";
    // Not fs::remove_all: case 4 builds a tree past MAX_PATH, and
    // remove_all never returns on one (see infrastructure/long_paths.hpp),
    // so one interrupted run would hang every later run of this file.
    seabass::infrastructure::removeTreeDeepestFirst(root);
    fs::create_directories(root);
    fs::path manifestPath = root / "Seabass" / "orphaned" / "pending-deletions.jsonl";

    // A real file on disk is actually deleted, and cleared from the
    // manifest -- this is the one place in the app that permanently
    // destroys real audio content, so this is the test that matters
    // most: the file must genuinely be gone afterward, not just
    // reported as gone.
    {
        fs::remove(manifestPath);
        fs::path filePath = root / "orphaned.mp3";
        touch(filePath);
        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(filePath.string()));

        auto outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest);

        assert(outcomes.size() == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Deleted);
        assert(!fs::exists(filePath));  // genuinely gone from disk
        assert(manifest.list().empty());  // and cleared from the manifest
        std::cout << "case 1 (real file: actually deleted from disk, cleared from manifest) OK\n";
    }

    // A file already gone from disk (e.g. removed by hand since the
    // manifest entry was recorded) is treated as done, not an error --
    // still cleared from the manifest, since there's nothing left to
    // delete.
    {
        fs::remove(manifestPath);
        fs::path filePath = root / "already_gone.mp3";
        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(filePath.string()));
        assert(!fs::exists(filePath));  // never created

        auto outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest);

        assert(outcomes.size() == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::AlreadyAbsent);
        assert(manifest.list().empty());
        std::cout << "case 2 (already-absent file: treated as done, cleared from manifest) OK\n";
    }

    // A file that exists but can't actually be deleted fails, and --
    // critically -- stays in the manifest so a later pass can retry it,
    // rather than being silently forgotten.
    {
        fs::remove(manifestPath);
        fs::path lockedDir = root / "locked";
        fs::create_directories(lockedDir);
        fs::path filePath = lockedDir / "cant_delete.mp3";
        touch(filePath);

        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(filePath.string()));

        std::vector<PendingDeletionOutcome> outcomes;
        {
            UndeletableFile blocked(filePath);
            outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest);
            assert(fs::exists(filePath));  // genuinely untouched, while still blocked
        }

        assert(outcomes.size() == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Failed);
        assert(!outcomes[0].failureReason.empty());
        // The reason has to say something. fs::remove returns false without
        // setting its error_code when there was simply nothing to remove,
        // and passing that through unchecked reported the failure as
        // "The operation completed successfully". Compared against a
        // default-constructed error_code rather than against that English
        // text, because the text is localised -- on this machine it comes
        // back as "De bewerking is voltooid".
        assert(outcomes[0].failureReason != std::error_code().message());
        assert(fs::exists(filePath));
        assert(manifest.list().size() == 1);  // stays in the manifest for a future retry
        std::cout << "case 3 (undeletable file: fails safely, stays in the manifest for retry) OK\n";
    }

    // A track under a path past MAX_PATH is really deleted, and reported
    // as Deleted rather than as never having been there.
    //
    // This is the one place in the app that destroys real audio, and the
    // failure here was the quiet kind: unprefixed, fs::exists() answers
    // false for a file that is plainly present, so the entry was recorded
    // AlreadyAbsent and cleared from the manifest. The user was told the
    // track had been cleaned up, the track stayed on disk, and because the
    // manifest entry was gone nothing would ever retry it.
    {
        fs::remove(manifestPath);
        fs::path deep = root / "deep";
        std::error_code ec;
        for (char c = 'a'; c < 'f'; ++c) {
            deep = deep / std::string(40, c);
            fs::create_directories(seabass::infrastructure::longPathSafe(deep), ec);
        }
        fs::path filePath = deep / "long_orphan.mp3";
        assert(filePath.native().size() > 260);
        writeLongPathFile(filePath, "audio data");
        assert(fs::exists(seabass::infrastructure::longPathSafe(filePath)));

        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(filePath.string()));

        auto outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest);

        assert(outcomes.size() == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Deleted);
        assert(!fs::exists(seabass::infrastructure::longPathSafe(filePath)));  // genuinely gone
        assert(manifest.list().empty());
        std::cout << "case 4 (track past MAX_PATH: really deleted, not reported already-absent) OK\n";
    }

    // A mixed batch: only the entries that actually get processed
    // (deleted or already-absent) are cleared -- an untouched entry
    // (e.g. one resolvePendingDeletions() would have excluded, deliberately
    // never passed in here) is left alone in the manifest.
    {
        fs::remove(manifestPath);
        fs::path deletableFile = root / "batch_deletable.mp3";
        touch(deletableFile);
        fs::path keepFile = root / "batch_still_referenced.mp3";
        touch(keepFile);

        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(deletableFile.string()));
        manifest.append(makeEntry(keepFile.string()));  // simulates an entry NOT passed to applyPendingDeletions

        std::vector<PendingDeletion> toDelete = {makeEntry(deletableFile.string())};
        auto outcomes = applyPendingDeletions(toDelete, root.string(), manifest);

        assert(outcomes.size() == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Deleted);
        assert(!fs::exists(deletableFile));
        assert(fs::exists(keepFile));  // untouched -- never in the deletion list

        auto remaining = manifest.list();
        assert(remaining.size() == 1);
        assert(remaining[0].filePath == keepFile.string());  // only the processed entry was cleared
        std::cout << "case 5 (mixed batch: only processed entries cleared, others left in the manifest) OK\n";
    }

    // Cancel between two files: the first is gone and cleared from the
    // manifest, the second is untouched on disk AND still listed, so a
    // later pass picks it up again.
    {
        fs::remove(manifestPath);
        fs::path first = root / "cancel_first.mp3";
        fs::path second = root / "cancel_second.mp3";
        touch(first);
        touch(second);

        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(first.string()));
        manifest.append(makeEntry(second.string()));

        seabass::application::CancellationToken cancel;
        size_t reported = 0;
        auto outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest, cancel, [&](size_t done) {
            reported = done;
            cancel.cancel();  // the user pressed Cancel while the first file was being deleted
        });

        assert(outcomes.size() == 1);
        assert(reported == 1);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Deleted);
        assert(!fs::exists(first));
        assert(fs::exists(second));
        auto remaining = manifest.list();
        assert(remaining.size() == 1);
        assert(remaining[0].filePath == second.string());
        std::cout << "case 6 (cancel between files: the rest stays on disk and in the manifest) OK\n";
    }

    // An entry whose path is not under this stick's root is never
    // deleted, whatever the list says: a mount point that moved (a clone
    // with the same label took the original's place) makes the manifest
    // name a file on some other drive. It fails, and stays for review.
    {
        fs::remove(manifestPath);
        fs::path elsewhere = root.parent_path() / "seabass_pending_deletion_applier_test_other" / "victim.mp3";
        fs::create_directories(elsewhere.parent_path());
        touch(elsewhere);
        fs::path own = root / "own.mp3";
        touch(own);
        PendingDeletionManifest manifest(manifestPath.string());
        manifest.append(makeEntry(elsewhere.string()));
        manifest.append(makeEntry(own.string()));

        auto outcomes = applyPendingDeletions(manifest.list(), root.string(), manifest);

        assert(outcomes.size() == 2);
        assert(outcomes[0].status == PendingDeletionOutcome::Status::Failed);
        assert(outcomes[0].failureReason.find("not on this stick") != std::string::npos);
        assert(fs::exists(elsewhere));
        assert(outcomes[1].status == PendingDeletionOutcome::Status::Deleted);
        assert(!fs::exists(own));
        auto remaining = manifest.list();
        assert(remaining.size() == 1 && remaining[0].filePath == elsewhere.string());
        fs::remove_all(elsewhere.parent_path());
        std::cout << "case: a path outside the stick root is refused and kept in the manifest OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
