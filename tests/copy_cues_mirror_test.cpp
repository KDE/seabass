// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Copy Cues on the Duplicates page writes both Pioneer formats.
//
// export.pdb and exportLibrary.db are one library in two formats. A
// rekordbox Copy Cues used to write only the analysis files rekordbox
// reads, so after every such save a player reading Device Library Plus
// showed the copy's old cues while one reading DeviceLibrary showed the
// new ones. Add Cue, Merge Cues and stray-cue removal already wrote both.
//
// Driven through runSaveLoop, as the page's save is. Two runs over copies
// of the fixture's rekordbox catalog:
//
// 1. The copy lands in Device Library Plus too: the target's cue table
//    there holds the source's cues afterwards, and did not before (so the
//    check could fail). Library Health compares that table with the
//    analysis file, so a copy that skipped it left a finding behind.
// 2. A Device Library Plus that cannot be written fails the change, and
//    the save puts back the DeviceLibrary half it had already written.

#include <QString>

#include <cassert>
#include <cmath>
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
#include "domain/track.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_cues_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
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

std::vector<Track> readOneLibrary(const fs::path &pioneerRoot)
{
    seabass::infrastructure::onelibrary::OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
    return reader.readAll();
}

std::optional<Track> byPath(const std::vector<Track> &tracks, const std::string &filePath)
{
    for (const Track &track : tracks) {
        if (track.filePath == filePath) {
            return track;
        }
    }
    return std::nullopt;
}

// The hot cues, by slot and position: what Copy Cues copies.
std::map<int, double> hotCues(const std::vector<CuePoint> &cues)
{
    std::map<int, double> bySlot;
    for (const CuePoint &cue : cues) {
        if (cue.kind == CuePoint::Kind::Hot) {
            bySlot[cue.hotCueNumber] = cue.positionMs;
        }
    }
    return bySlot;
}

bool sameHotCues(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b)
{
    const auto x = hotCues(a);
    const auto y = hotCues(b);
    if (x.size() != y.size()) {
        return false;
    }
    for (const auto &[slot, position] : x) {
        const auto found = y.find(slot);
        if (found == y.end() || std::abs(found->second - position) > 1.0) {
            return false;
        }
    }
    return true;
}

// The hot cues in the content row's cue table: what Library Health holds
// against the analysis file (#57). The analysis file alone cannot show the
// missing mirror: in this fixture, as on a rekordbox export, both formats
// name the same one, so the DeviceLibrary write already changes it.
std::vector<CuePoint> cueTableOf(const fs::path &pioneerRoot, const std::string &contentId)
{
    using namespace seabass::infrastructure::onelibrary;
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(seabass::pathToUtf8(pioneerRoot)), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    std::vector<CuePoint> cues;
    for (const CueTableEntry &entry : readCueTable(db, std::stoll(contentId))) {
        if (entry.understood()) {
            cues.push_back(entry.cue);
        }
    }
    return cues;
}

// A source with hot cues and a target whose Device Library Plus hot cues
// differ from them, both listed in Device Library Plus.
DuplicatesCopyOp pickOp(const fs::path &pioneerRoot)
{
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneerRoot));
    const std::vector<Track> rekordbox = reader.readAll();
    const std::vector<Track> oneLibrary = readOneLibrary(pioneerRoot);
    for (const Track &source : rekordbox) {
        if (source.filePath.empty() || hotCues(source.cues).empty() || !byPath(oneLibrary, source.filePath)) {
            continue;
        }
        for (const Track &target : rekordbox) {
            if (target.sourceId == source.sourceId || target.filePath.empty()) {
                continue;
            }
            const auto mirrored = byPath(oneLibrary, target.filePath);
            if (mirrored && !sameHotCues(cueTableOf(pioneerRoot, mirrored->sourceId), source.cues)) {
                DuplicatesCopyOp op;
                op.source = source;
                op.targets = {target};
                return op;
            }
        }
    }
    return {};
}

std::map<std::string, std::string> snapshot(const fs::path &pioneerRoot)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(pioneerRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = seabass::pathToUtf8(entry.path().filename());
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-shm") == 0) {
            continue;  // rebuilt by SQLite after a rollback puts its -wal back
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files[seabass::pathToGenericUtf8(fs::relative(entry.path(), pioneerRoot))] =
            std::string(std::istreambuf_iterator<char>(in), {});
    }
    return files;
}

SaveLoopResult copyThroughSave(const fs::path &pioneerRoot, const DuplicatesCopyOp &op)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<PendingChange>> changes = {
        std::make_shared<CopyCuesChange>(QStringLiteral("rekordbox"), root, QStringLiteral("group"), op)};
    return runSaveLoop(changes, ctx);
}

}  // namespace

int main()
{
    // ---- 1. the copy reaches Device Library Plus ---------------------------
    {
        const fs::path pioneerRoot = freshCopy("seabass_copy_cues_mirror_ok");
        const DuplicatesCopyOp op = pickOp(pioneerRoot);
        assert(!op.targets.empty() && "the fixture must have two tracks whose Device Library Plus hot cues differ");
        const std::string targetPath = op.targets.front().filePath;

        const SaveLoopResult result = copyThroughSave(pioneerRoot, op);
        if (!result.error.isEmpty()) {
            std::cerr << "save failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty());
        assert(result.appliedIds.size() == 1);

        seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneerRoot));
        const auto rekordboxTarget = [&]() -> std::optional<Track> {
            for (const Track &track : reader.readAll()) {
                if (track.sourceId == op.targets.front().sourceId) return track;
            }
            return std::nullopt;
        }();
        assert(rekordboxTarget && sameHotCues(rekordboxTarget->cues, op.source.cues)
               && "DeviceLibrary has the copied cues");
        const auto mirrored = byPath(readOneLibrary(pioneerRoot), targetPath);
        assert(mirrored);
        assert(sameHotCues(mirrored->cues, op.source.cues) && "the analysis file Device Library Plus names has them");
        const std::vector<CuePoint> table = cueTableOf(pioneerRoot, mirrored->sourceId);
        if (!sameHotCues(table, op.source.cues)) {
            std::cerr << "Device Library Plus's cue table still has " << hotCues(table).size() << " hot cue(s) on "
                      << targetPath << ", the source has " << hotCues(op.source.cues).size() << "\n";
        }
        assert(sameHotCues(table, op.source.cues) && "Device Library Plus's cue table has the copied cues too");
        std::cout << "case 1 (a rekordbox copy lands in Device Library Plus too) OK\n";
    }

    // ---- 2. a mirror that cannot be written fails, and nothing is left -----
    {
        const fs::path pioneerRoot = freshCopy("seabass_copy_cues_mirror_readonly");
        const DuplicatesCopyOp op = pickOp(pioneerRoot);
        assert(!op.targets.empty());
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

        const SaveLoopResult result = copyThroughSave(pioneerRoot, op);
        assert(!result.error.isEmpty() && "a copy that did not reach Device Library Plus is not applied");
        assert(result.appliedIds.isEmpty());
        assert(result.error.toStdString().find("Device Library Plus") != std::string::npos);
        assert(result.error.toStdString().find("putting back") == std::string::npos);
        const auto after = snapshot(pioneerRoot);
        for (const auto &[path, bytes] : before) {
            const auto found = after.find(path);
            if (found == after.end() || found->second != bytes) {
                std::cerr << "left changed after a failed save: " << path << "\n";
            }
        }
        assert(after == before && "a failed Copy Cues leaves every file as it was");
        std::cout << "case 2 (a mirror that cannot be written fails, and DeviceLibrary is put back) OK\n";

        for (const fs::path &file : dbFiles) {
            std::error_code ec;
            fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
        }
    }

    // ---- 3. an Engine copy survives a save that has m.db on a scratch copy --
    // Clean Up, Sync and the repairs put m.db behind a scratch copy for a
    // large enough save. A copy written straight to the stick's m.db then
    // was overwritten when the scratch was committed back: reported as
    // applied, and gone. merge_cues_change_test holds the same for Merge.
    {
        const fs::path scratch = seabass::testing::scratchRoot() / "seabass_copy_cues_engine_scratch";
        std::error_code cleared;
        fs::remove_all(scratch, cleared);
        fs::create_directories(scratch);
        const fs::path engineRoot = scratch / "Engine Library";
        fs::copy(seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine",
                 engineRoot, fs::copy_options::recursive);

        DuplicatesCopyOp op;
        {
            seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(engineRoot));
            const std::vector<Track> tracks = reader.readAll();
            for (const Track &track : tracks) {
                if (!hotCues(track.cues).empty()) {
                    op.source = track;
                    break;
                }
            }
            for (const Track &track : tracks) {
                if (track.cues.empty() && track.sourceId != op.source.sourceId) {
                    op.targets = {track};
                    break;
                }
            }
        }
        assert(!op.source.sourceId.empty() && !op.targets.empty());

        auto &noProgress = seabass::application::NullProgressReporter::instance();
        CancellationToken token;
        const QString root = seabass::gui::pathToQString(engineRoot);
        {
            SaveContext ctx(token, noProgress, {}, {}, root);
            // Stand-in for a Clean Up or Sync change staged into the same
            // save, asked for first with a hint that takes a scratch copy.
            auto &other = sharedFormatWriteSession(ctx, "engine", seabass::pathToUtf8(engineRoot), 5000, "test-other-feature");
            assert(other.usesScratch());  // otherwise this case proves nothing

            CopyCuesChange change(QStringLiteral("engine"), root, QStringLiteral("group"), op);
            assert(change.apply(ctx).ok);
            assert(!ctx.runFinishHooks(true).error);
        }

        seabass::infrastructure::engine::LibdjinteropEngineReader after(seabass::pathToUtf8(engineRoot));
        bool found = false;
        for (const Track &track : after.readAll()) {
            if (track.sourceId == op.targets.front().sourceId) {
                found = sameHotCues(track.cues, op.source.cues);
            }
        }
        assert(found && "the copied cues must survive a save that also scratches m.db");
        std::cout << "case 3 (an Engine copy survives a save that has m.db on a scratch copy) OK\n";
        fs::remove_all(scratch, cleared);
    }

    std::error_code ec;
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_copy_cues_mirror_ok", ec);
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_copy_cues_mirror_readonly", ec);
    std::cout << "copy_cues_mirror_test passed\n";
    return 0;
}
