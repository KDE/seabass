// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FinishCleanupChange through the real save loop, on a copy of the
// committed fixture's OneLibrary: the leftover's rows go, its playlist
// entries land on the survivor, and a leftover that cannot be finished is
// a skip -- never a removal that drops entries with nowhere to go.

#include <QString>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/finish_cleanup_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"

#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::gui;

namespace
{

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / seabass::pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

std::vector<domain::Track> oneLibrary(const fs::path &root)
{
    return infrastructure::onelibrary::OneLibraryReader(seabass::pathToUtf8(root)).readAll();
}

std::vector<const domain::Track *> rowsAt(const std::vector<domain::Track> &rows, const std::string &path)
{
    std::vector<const domain::Track *> found;
    for (const auto &row : rows) {
        if (row.filePath == path) {
            found.push_back(&row);
        }
    }
    return found;
}

std::set<std::string> playlistsOf(const std::vector<domain::Track> &rows, const std::string &path)
{
    std::set<std::string> names;
    for (const auto *row : rowsAt(rows, path)) {
        for (const auto &membership : row->playlists) {
            names.insert(membership.name);
        }
    }
    return names;
}

SaveLoopResult save(const fs::path &root, std::vector<std::shared_ptr<PendingChange>> changes)
{
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, seabass::gui::pathToQString(root),
                    {});
    return runSaveLoop(changes, ctx);
}

domain::CleanupLeftover leftover(const domain::Track &row, std::string survivorPath)
{
    domain::CleanupLeftover l;
    l.kind = domain::CleanupLeftover::Kind::Repairable;
    l.row = row;
    l.rowIds = {row.sourceId};
    domain::Track survivor;
    survivor.filePath = std::move(survivorPath);
    l.survivor = survivor;
    return l;
}

}  // namespace

int main()
{
    const fs::path root = freshCopy("seabass_finish_cleanup_change_test");
    const auto before = oneLibrary(root);

    // A leftover in at least one playlist the survivor is not in, so the
    // entries have to MOVE -- a pair already sharing every playlist would
    // pass with the entries simply dropped. Both files listed once, so
    // the counts below are exact.
    const domain::Track *doomed = nullptr;
    const domain::Track *kept = nullptr;
    for (const auto &a : before) {
        if (a.filePath.empty() || a.playlists.empty() || rowsAt(before, a.filePath).size() != 1) {
            continue;
        }
        for (const auto &b : before) {
            if (b.filePath.empty() || b.filePath == a.filePath || rowsAt(before, b.filePath).size() != 1) {
                continue;
            }
            const auto theirs = playlistsOf(before, b.filePath);
            bool movesSomewhere = false;
            for (const auto &membership : a.playlists) {
                movesSomewhere = movesSomewhere || !theirs.count(membership.name);
            }
            if (movesSomewhere) {
                doomed = &a;
                kept = &b;
                break;
            }
        }
        if (doomed) {
            break;
        }
    }
    assert(doomed && kept && "the fixture must offer such a pair, or this proves nothing");
    const std::string doomedPath = doomed->filePath;
    const std::string keptPath = kept->filePath;
    const std::set<std::string> doomedPlaylists = playlistsOf(before, doomedPath);
    const std::set<std::string> keptPlaylistsBefore = playlistsOf(before, keptPath);

    // 1. Finished: the leftover's row is gone, the survivor is in every
    //    playlist the leftover was in, and nothing else lost a row.
    {
        auto change = std::make_shared<FinishCleanupChange>(seabass::gui::pathToQString(root),
                                                            leftover(*doomed, keptPath), true);
        const QString id = change->id();
        const SaveLoopResult result = save(root, {change});
        assert(result.error.isEmpty());
        assert(result.appliedIds == QStringList{id});
        assert(result.skippedIds.isEmpty());
        assert(!result.backups.empty() && "exportLibrary.db is backed up before the first removal");

        const auto after = oneLibrary(root);
        assert(rowsAt(after, doomedPath).empty());
        assert(rowsAt(after, keptPath).size() == 1);
        assert(after.size() == before.size() - 1);
        const auto keptPlaylistsAfter = playlistsOf(after, keptPath);
        for (const auto &name : doomedPlaylists) {
            assert(keptPlaylistsAfter.count(name) && "every playlist the leftover was in keeps the song");
        }
        for (const auto &name : keptPlaylistsBefore) {
            assert(keptPlaylistsAfter.count(name));
        }
        std::cout << "case 1 (the leftover goes, its " << doomedPlaylists.size()
                  << " playlist(s) keep the song through the survivor) OK\n";
    }

    // 2. The same change again finds nothing to finish: a skip, counted
    //    apart, not a failure that stops the rest of the save.
    {
        auto change = std::make_shared<FinishCleanupChange>(seabass::gui::pathToQString(root),
                                                            leftover(*doomed, keptPath), true);
        const SaveLoopResult result = save(root, {change});
        assert(result.error.isEmpty());
        assert(result.skippedIds == QStringList{change->id()});
        std::cout << "case 2 (already finished: skipped) OK\n";
    }

    // 3. A survivor that has gone since the scan: the leftover stays, with
    //    its entries, because removing it would drop them.
    {
        const auto now = oneLibrary(root);
        const domain::Track *another = nullptr;
        for (const auto &row : now) {
            if (!row.filePath.empty() && !row.playlists.empty() && row.filePath != keptPath
                && rowsAt(now, row.filePath).size() == 1) {
                another = &row;
                break;
            }
        }
        assert(another);
        const std::string path = another->filePath;
        auto change = std::make_shared<FinishCleanupChange>(
            seabass::gui::pathToQString(root), leftover(*another, seabass::pathToUtf8(root / "no-such-file.mp3")), true);
        const SaveLoopResult result = save(root, {change});
        assert(result.error.isEmpty());
        assert(result.skippedIds == QStringList{change->id()});
        assert(rowsAt(oneLibrary(root), path).size() == 1);
        assert(playlistsOf(oneLibrary(root), path) == playlistsOf(now, path));
        std::cout << "case 3 (survivor gone: the leftover and its entries stay) OK\n";
    }

    fs::remove_all(root.parent_path());
    std::cout << "finish_cleanup_change_test: all cases passed\n";
    return 0;
}
