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
// The unreadable case is made here by taking away the right to look --
// search permission off the parent directory on POSIX, a deny ACE on the
// directory and the file on Windows (see lockOut()) -- which is what a
// stick with a failing cell or a directory Seabass cannot enter looks
// like from inside exists().

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/cleanup/pending_deletion_applier.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/paths/utf8_path.hpp"
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
    s.root = seabass::testing::scratchRoot() / seabass::pathFromUtf8(name);
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

// Takes away, and gives back, the right to look at `hidden` from inside
// `locked`.
//
// POSIX: search permission off the directory is enough. Windows ignores
// the mode (fs::permissions only flips the read-only attribute), and a
// deny on the directory alone is not enough either: every account holds
// "bypass traverse checking", so a file is still examined by its full
// path, and NTFS grants read-attributes on a file to anyone who may list
// its directory. Denying read on BOTH is what makes GetFileAttributesEx,
// and so exists(), fail with ERROR_ACCESS_DENIED -- measured on Windows 11
// with MSVC's std::filesystem, where either deny alone still answers
// exists=true.
#ifdef _WIN32
int icacls(const std::wstring &args)
{
    return _wsystem((L"icacls " + args + L" >nul").c_str());
}

std::wstring quoted(const fs::path &p)
{
    return L"\"" + p.wstring() + L"\"";
}
#endif

void lockOut(const Stick &s)
{
#ifdef _WIN32
    // *S-1-1-0 is Everyone, named by SID so a localized Windows finds it.
    icacls(quoted(s.locked) + L" /deny *S-1-1-0:(RX)");
    icacls(quoted(s.hidden) + L" /deny *S-1-1-0:(R)");
#else
    std::error_code ec;
    fs::permissions(s.locked, fs::perms::none, fs::perm_options::replace, ec);
    assert(!ec);
#endif
}

void letIn(const Stick &s)
{
#ifdef _WIN32
    icacls(quoted(s.locked) + L" /remove:d *S-1-1-0");
    icacls(quoted(s.hidden) + L" /remove:d *S-1-1-0");
#else
    std::error_code ec;
    fs::permissions(s.locked, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
}

PendingDeletion entryFor(const fs::path &file)
{
    PendingDeletion e;
    e.format = "rekordbox";
    e.filePath = seabass::pathToUtf8(file);
    e.title = seabass::pathToUtf8(file.filename());
    e.artist = "rig";
    return e;
}

}  // namespace

int main()
{
    Stick stick = makeStick("seabass_pending_unreadable");
    const fs::path manifestPath = stick.root / "pending.jsonl";

    PendingDeletionManifest manifest(seabass::pathToUtf8(manifestPath));
    manifest.append(entryFor(stick.hidden));
    manifest.append(entryFor(stick.plain));
    assert(manifest.list().size() == 2);

    // No permission to look: exists() on the file inside now fails with a
    // real error rather than answering "not there".
    std::error_code ec;
    lockOut(stick);
    {
        std::error_code probe;
        const bool seen = fs::exists(stick.hidden, probe);
        if (!probe) {
            // Running as root, or a filesystem that ignores the mode.
            // Say so and stop rather than "pass" having tested nothing.
            letIn(stick);
            std::cerr << "this environment still resolves a file it was denied access to (exists="
                      << seen << "), so the unreadable case cannot be built here and this test would\n"
                         "prove nothing. Not reporting a pass.\n";
            return 77;
        }
    }

    const std::vector<PendingDeletion> safeToDelete = {entryFor(stick.hidden), entryFor(stick.plain)};
    const auto outcomes = applyPendingDeletions(safeToDelete, seabass::pathToUtf8(stick.root), manifest);
    letIn(stick);

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
    if (left.size() != 1 || left[0].filePath != seabass::pathToUtf8(stick.hidden)) {
        std::cerr << "manifest holds " << left.size() << " entr(ies) after the run; expected only the "
                  << "unreadable one\n";
        for (const auto &e : left) {
            std::cerr << "    " << e.filePath << "\n";
        }
    }
    assert(left.size() == 1 && "the deleted file is cleared and the unexaminable one is kept");
    assert(left[0].filePath == seabass::pathToUtf8(stick.hidden));
    assert(fs::exists(stick.hidden) && "and it is still on the stick, which is why it must stay listed");

    fs::remove_all(stick.root, ec);
    std::cout << "pending_deletion_unreadable_test passed\n";
    return 0;
}
