// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Removing a junk cue that cannot reach Device Library Plus is not a
// success either.
//
// The sibling of add_cue_mirror_failure_test, and the same rule:
// export.pdb and exportLibrary.db are one library in two formats, so a
// cue removed from one and not the other is a library that disagrees
// with itself. AddCueChange was fixed for that; RemoveJunkCueChange kept
// the behaviour AddCueChange was fixed FROM -- it logged the failed
// mirror write and returned success, so the page said the junk cue was
// gone while a player reading Device Library Plus still showed it, and
// Undo offered nothing to go back to because nothing had failed.
//
// Driven through runSaveLoop rather than apply(), because the rollback is
// the save loop's job: calling apply() directly would pass with the cue
// already removed from the ANLZ file, which is the half-written state
// this guards against.
//
// Two runs over copies of the fixture. The first, writable, must succeed
// AND change the stick -- without that baseline, "the stick is unchanged"
// in the second could hold because nothing was ever going to be written.

#include <QString>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/junk_cue.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / seabass::pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    assert(fs::exists(source / "rekordbox" / "export.pdb"));
    assert(fs::exists(source / "rekordbox" / "exportLibrary.db"));
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

// A track the mirror actually lists, carrying a junk cue to remove. Both
// matter: a track OneLibrary does not list is deliberately NOT a failure
// (635 of 1118 on a real stick are in that position), so picking one
// would test the opposite of what this is for.
std::optional<Track> findMirroredTrackWithJunk(const fs::path &pioneerRoot)
{
    const std::string root = seabass::pathToUtf8(pioneerRoot);
    if (!seabass::infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root)) {
        return std::nullopt;
    }
    seabass::infrastructure::onelibrary::OneLibraryCueWriter mirror(root);
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(root);
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty()) {
            continue;
        }
        const bool carriesJunk = std::any_of(track.cues.begin(), track.cues.end(),
                                             [](const CuePoint &cue) { return isJunkCue(cue); });
        if (!carriesJunk) {
            continue;
        }
        if (mirror.hasTrackAtPath(track.filePath)) {
            return track;
        }
    }
    return std::nullopt;
}

std::map<std::string, std::string> snapshot(const fs::path &root)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = seabass::pathToUtf8(entry.path().filename());
        // -shm is rebuilt from the -wal a rollback puts back; that is the
        // rollback working, not the stick changing.
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-shm") == 0) {
            continue;
        }
        // An EMPTY -wal is the same state as no -wal: SQLite truncates it
        // on checkpoint and creates a zero-length one the moment the
        // database is opened, which this test does when it opens the
        // read-only copy to pick its track. Counting it as a new file
        // would report a failed save as having changed the stick when
        // nothing was committed. A -wal with bytes in it is a real
        // difference and is compared like any other file.
        std::error_code sizeEc;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-wal") == 0
            && fs::file_size(entry.path(), sizeEc) == 0 && !sizeEc) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files[seabass::pathToGenericUtf8(fs::relative(entry.path(), root))] =
            std::string(std::istreambuf_iterator<char>(in), {});
    }
    return files;
}

void setMirrorReadOnly(const fs::path &pioneerRoot, bool readOnly)
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

SaveLoopResult removeJunkThroughSave(const fs::path &pioneerRoot, const Track &track)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<RemoveJunkCueChange>(root, track)};
    return runSaveLoop(changes, ctx);
}

}  // namespace

int main()
{
    // ---- baseline: a writable mirror, so it goes through and writes ------
    {
        const fs::path pioneerRoot = freshCopy("seabass_junk_mirror_ok");
        const auto track = findMirroredTrackWithJunk(pioneerRoot);
        if (!track) {
            std::cerr << "the fixture has no rekordbox track that both carries a junk cue and is listed in "
                         "Device Library Plus: this test cannot prove anything without one\n";
            return 1;
        }
        const auto before = snapshot(pioneerRoot);
        const SaveLoopResult result = removeJunkThroughSave(pioneerRoot, *track);
        if (!result.error.isEmpty()) {
            std::cerr << "baseline failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "with a writable Device Library Plus the removal must go through");
        assert(result.appliedIds.size() == 1);
        assert(snapshot(pioneerRoot) != before && "a successful junk-cue removal must change the stick");
        std::cout << "case 1 (a writable mirror: the removal succeeds and the stick changes) OK\n";
        std::error_code ec;
        fs::remove_all(pioneerRoot.parent_path(), ec);
    }

    // ---- the mirror cannot be written: a failure, nothing left behind ----
    {
        const fs::path pioneerRoot = freshCopy("seabass_junk_mirror_readonly");
        const auto track = findMirroredTrackWithJunk(pioneerRoot);
        assert(track && "the same fixture must still offer the same track");
        setMirrorReadOnly(pioneerRoot, true);
        const auto before = snapshot(pioneerRoot);

        const SaveLoopResult result = removeJunkThroughSave(pioneerRoot, *track);
        setMirrorReadOnly(pioneerRoot, false);

        if (result.error.isEmpty()) {
            std::cerr << "the save reported success although Device Library Plus could not be written\n";
        }
        assert(!result.error.isEmpty()
               && "a junk cue that did not reach Device Library Plus must not be reported as removed");
        assert(result.appliedIds.isEmpty());
        const std::string error = result.error.toStdString();
        assert(error.find("Device Library Plus") != std::string::npos);
        // No "-- and putting back what it had already written failed".
        assert(error.find("putting back") == std::string::npos);

        const auto after = snapshot(pioneerRoot);
        for (const auto &[path, bytes] : before) {
            const auto found = after.find(path);
            if (found == after.end()) {
                std::cerr << "removed by a failed save: " << path << "\n";
            } else if (found->second != bytes) {
                std::cerr << "left changed after a failed save: " << path << " (" << bytes.size() << " -> "
                          << found->second.size() << " bytes)\n";
            }
        }
        for (const auto &[path, bytes] : after) {
            if (!before.count(path)) {
                std::cerr << "created by a failed save: " << path << " (" << bytes.size() << " bytes)\n";
            }
        }
        assert(after == before && "a failed removal must leave every file as it was, ANLZ included");
        std::cout << "case 2 (a mirror that cannot be written fails, and DeviceLibrary is put back) OK\n";

        std::error_code ec;
        fs::remove_all(pioneerRoot.parent_path(), ec);
    }

    std::cout << "junk_cue_mirror_failure_test passed\n";
    return 0;
}
