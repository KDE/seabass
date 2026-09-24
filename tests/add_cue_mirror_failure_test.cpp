// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A rekordbox cue whose Device Library Plus mirror cannot be written is not
// a success, and does not leave half a cue behind.
//
// export.pdb and exportLibrary.db are one library in two formats. Add Cue
// used to catch a failed mirror write, log it, and report the change as
// applied -- so the page told the DJ the cue was on the stick while a
// player reading Device Library Plus would not show it. The change now
// fails, and because the save loop rolls a failed change back, the
// DeviceLibrary half it had already written goes back too.
//
// Driven through runSaveLoop rather than apply(), because the rollback is
// the save loop's job: calling apply() directly would pass with the cue
// still in the ANLZ file, which is exactly the half-written state this
// guards against.
//
// Two runs over copies of the fixture's rekordbox catalog. The first, with
// a writable exportLibrary.db, must succeed AND change the stick: without
// that baseline, "the stick is unchanged" in the second run could hold
// because nothing was ever going to be written. The second makes the
// database read-only and must fail with every file back as it was.

#include <QString>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

struct Target
{
    std::string sourceId;
    std::string title;
    int freeSlot = 0;
};

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

// A track that already carries a hot cue (so its ANLZ file exists and
// round-trips), has a free slot to add to, and has a file path -- the mirror
// is only attempted for a track with one.
Target findTarget(const fs::path &pioneerRoot)
{
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneerRoot));
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty()) {
            continue;
        }
        std::set<int> used;
        for (const CuePoint &cue : track.cues) {
            if (cue.kind == CuePoint::Kind::Hot) {
                used.insert(cue.hotCueNumber);
            }
        }
        if (used.empty()) {
            continue;
        }
        for (int slot = 1; slot <= 8; ++slot) {
            if (!used.count(slot)) {
                return {track.sourceId, track.title, slot};
            }
        }
    }
    return {};
}

// Every file under the PIONEER folder, by relative path, with its bytes.
//
// -shm files are left out on purpose: a rollback deletes the -shm beside a
// -wal it put back, because the shared-memory index describes the WAL that
// was just replaced and SQLite rebuilds it. That is the rollback working,
// not the stick changing.
std::map<std::string, std::string> snapshot(const fs::path &pioneerRoot)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(pioneerRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = seabass::pathToUtf8(entry.path().filename());
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-shm") == 0) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files[seabass::pathToGenericUtf8(fs::relative(entry.path(), pioneerRoot))] =
            std::string(std::istreambuf_iterator<char>(in), {});
    }
    return files;
}

SaveLoopResult addCueThroughSave(const fs::path &pioneerRoot, const Target &target)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    // (cancel, progress, status sink, rekordbox path, engine path)
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
        QStringLiteral("rekordbox"), root, QString::fromStdString(target.sourceId), 1234.0, QStringLiteral("hot"),
        target.freeSlot, QStringLiteral("#FF0000"), QString(), false, 0.0, QString::fromStdString(target.title))};
    return runSaveLoop(changes, ctx);
}

}  // namespace

int main()
{
    // ---- baseline: a writable mirror, so the save succeeds and writes ------
    {
        const fs::path pioneerRoot = freshCopy("seabass_add_cue_mirror_ok");
        const Target target = findTarget(pioneerRoot);
        assert(target.freeSlot != 0 && "the fixture must have a rekordbox track with a cue, a file path and a free slot");
        const auto before = snapshot(pioneerRoot);
        const SaveLoopResult result = addCueThroughSave(pioneerRoot, target);
        if (!result.error.isEmpty()) {
            std::cerr << "baseline failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "with a writable Device Library Plus the cue must go through");
        assert(result.appliedIds.size() == 1);
        // And it actually wrote something, or case 2's "nothing changed"
        // would prove nothing.
        assert(snapshot(pioneerRoot) != before && "a successful Add Cue must change the stick");
        std::cout << "case 1 (a writable mirror: the save succeeds and the stick changes) OK\n";
    }

    // ---- the mirror cannot be written: a failure, and nothing left behind --
    {
        const fs::path pioneerRoot = freshCopy("seabass_add_cue_mirror_readonly");
        const Target target = findTarget(pioneerRoot);
        assert(target.freeSlot != 0);
        const fs::path db = pioneerRoot / "rekordbox" / "exportLibrary.db";
        const std::vector<fs::path> dbFiles = {db, fs::path(db).concat("-wal"), fs::path(db).concat("-shm")};
        for (const fs::path &file : dbFiles) {
            std::error_code ec;
            if (fs::exists(file, ec)) {
                fs::permissions(file, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                                fs::perm_options::replace, ec);
                assert(!ec);
            }
        }
        const auto before = snapshot(pioneerRoot);

        const SaveLoopResult result = addCueThroughSave(pioneerRoot, target);
        assert(!result.error.isEmpty() && "a cue that did not reach Device Library Plus must not be reported as applied");
        assert(result.appliedIds.isEmpty());
        const std::string error = result.error.toStdString();
        assert(error.find("Device Library Plus") != std::string::npos);
        // No "-- and putting back what it had already written failed": the
        // rollback itself must have worked.
        assert(error.find("putting back") == std::string::npos);

        // The point of the whole change: the ANLZ file the cue went into is
        // back as it was, and so is everything else in PIONEER.
        const auto after = snapshot(pioneerRoot);
        for (const auto &[path, bytes] : before) {
            const auto found = after.find(path);
            if (found == after.end() || found->second != bytes) {
                std::cerr << "left changed after a failed save: " << path << "\n";
            }
        }
        assert(after == before && "a failed Add Cue must leave every file as it was");
        std::cout << "case 2 (a mirror that cannot be written fails, and DeviceLibrary is put back) OK\n";

        // Put the permissions back so the scratch directory can be removed.
        for (const fs::path &file : dbFiles) {
            std::error_code ec;
            fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
        }
    }

    std::error_code ec;
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_add_cue_mirror_ok", ec);
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_add_cue_mirror_readonly", ec);
    std::cout << "add_cue_mirror_failure_test passed\n";
    return 0;
}
