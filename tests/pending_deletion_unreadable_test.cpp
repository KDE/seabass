// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A file Seabass cannot examine is not a file that is gone.
//
// applyPendingDeletions() is, in its own header's words, "the one place
// in the app that permanently destroys real audio file content". It
// asked std::filesystem::exists() whether each file was still there, and
// exists() answers false for BOTH "it is gone" and "I could not look" --
// only the error code tells them apart, and it was not read.
//
// So an entry Seabass could not examine took the AlreadyAbsent branch,
// which this file's own header defines as "gone already ... still
// cleared from the manifest". The person was told the file had been
// dealt with, the file stayed on the stick taking the space they were
// trying to reclaim, and the entry was dropped from the manifest so no
// later pass would ever retry it.
//
// That is the shape this codebase keeps finding: an answer that means
// two things, only one of which is true. removeEntry() exists one line
// below for exactly the same reason, and audio_file_walk.cpp already
// writes "|| ec".
//
// The unreadable case is made here by taking search permission off the
// parent directory, which is what a stick with a failing cell or a
// directory Seabass cannot enter looks like from inside exists().

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/cleanup/pending_deletion_applier.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::cleanup;
namespace fs = std::filesystem;

namespace
{

struct Stick
{
    fs::path root;
    fs::path locked;   // the directory whose permissions get taken away
    fs::path hidden;   // a real file inside it
    fs::path plain;    // an ordinary deletable file beside it
};

Stick makeStick(const std::string &name)
{
    Stick s;
    s.root = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(s.root, ec);
    fs::create_directories(s.root / "Contents" / "locked");
    s.locked = s.root / "Contents" / "locked";
    s.hidden = s.locked / "unreachable.mp3";
    s.plain = s.root / "Contents" / "ordinary.mp3";
    std::ofstream(s.hidden) << "audio";
    std::ofstream(s.plain) << "audio";
    return s;
}

PendingDeletion entryFor(const fs::path &file)
{
    PendingDeletion e;
    e.format = "rekordbox";
    e.filePath = file.string();
    e.title = file.filename().string();
    e.artist = "rig";
    return e;
}

}  // namespace

int main()
{
    Stick stick = makeStick("seabass_pending_unreadable");
    const fs::path manifestPath = stick.root / "pending.jsonl";

    PendingDeletionManifest manifest(manifestPath.string());
    manifest.append(entryFor(stick.hidden));
    manifest.append(entryFor(stick.plain));
    assert(manifest.list().size() == 2);

    // No search permission: exists() on anything inside now fails with a
    // real error rather than answering "not there".
    std::error_code ec;
    fs::permissions(stick.locked, fs::perms::none, fs::perm_options::replace, ec);
    assert(!ec);
    {
        std::error_code probe;
        const bool seen = fs::exists(stick.hidden, probe);
        if (!probe) {
            // Running as root, or a filesystem that ignores the mode.
            // Say so and stop rather than "pass" having tested nothing.
            fs::permissions(stick.locked, fs::perms::owner_all, fs::perm_options::replace, ec);
            std::cerr << "this environment still resolves a file inside a mode-000 directory (exists="
                      << seen << "), so the unreadable case cannot be built here and this test would\n"
                         "prove nothing. Not reporting a pass.\n";
            return 77;
        }
    }

    const std::vector<PendingDeletion> safeToDelete = {entryFor(stick.hidden), entryFor(stick.plain)};
    const auto outcomes = applyPendingDeletions(safeToDelete, stick.root.string(), manifest);
    fs::permissions(stick.locked, fs::perms::owner_all, fs::perm_options::replace, ec);

    assert(outcomes.size() == 2);

    const auto &unreadable = outcomes[0];
    if (unreadable.status != PendingDeletionOutcome::Status::Failed) {
        std::cerr << "a file that could not be examined was reported as status "
                  << static_cast<int>(unreadable.status) << " (AlreadyAbsent is 1), not Failed\n";
    }
    assert(unreadable.status == PendingDeletionOutcome::Status::Failed
           && "a file Seabass could not look at has not been dealt with");
    assert(!unreadable.failureReason.empty() && "and the person is told why");
    std::cout << "  reported: " << unreadable.failureReason << "\n";

    // The one beside it is ordinary and must still go, or the fix would
    // be "refuse everything" rather than "tell the two apart".
    assert(outcomes[1].status == PendingDeletionOutcome::Status::Deleted);
    assert(!fs::exists(stick.plain));

    // The point of the whole thing: it is still on the list, so a later
    // pass can retry it. Before the fix the manifest was left empty.
    const auto left = manifest.list();
    if (left.size() != 1 || left[0].filePath != stick.hidden.string()) {
        std::cerr << "manifest holds " << left.size() << " entr(ies) after the run; expected only the "
                  << "unreadable one\n";
        for (const auto &e : left) {
            std::cerr << "    " << e.filePath << "\n";
        }
    }
    assert(left.size() == 1 && "the deleted file is cleared and the unexaminable one is kept");
    assert(left[0].filePath == stick.hidden.string());
    assert(fs::exists(stick.hidden) && "and it is still on the stick, which is why it must stay listed");

    fs::remove_all(stick.root, ec);
    std::cout << "pending_deletion_unreadable_test passed\n";
    return 0;
}
