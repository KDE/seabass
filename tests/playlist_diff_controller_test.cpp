// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PlaylistDiffController over the committed anonymized Engine library:
// the playlists it lists, the pair it opens on, the counts of the diff
// and the folded rows. Every number here was read off the fixture's m.db
// by hand (see the comments), so a controller that dropped an entry, or
// folded one away, fails against the fixture rather than against itself.
//
// argv[1]: tests/fixtures/anonymized_library. The Engine catalog is
// copied into scratch first: opening m.db may roll a journal back, and
// the fixture itself is never written.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gui/playlist_diff_controller.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::gui::PlaylistDiffController;
using seabass::gui::PlaylistDiffRowModel;
namespace fs = std::filesystem;

namespace
{

void waitUntilIdle(PlaylistDiffController &controller)
{
    QSignalSpy busy(&controller, &PlaylistDiffController::busyChanged);
    for (int i = 0; i < 600 && controller.busy(); ++i) {
        busy.wait(100);
    }
    assert(!controller.busy());
}

int foldRows(const PlaylistDiffRowModel &rows)
{
    int n = 0;
    for (const auto &row : rows.rows()) {
        if (row.kind == QLatin1String("fold")) {
            ++n;
        }
    }
    return n;
}

// Tracks named by id, each a member of the playlists that list it, at the
// positions they list it: a playlist naming "x" twice is one track with
// two memberships, the way a catalog with a duplicate entry reads.
std::vector<seabass::domain::Track> tracksOf(const std::vector<std::pair<std::string, std::vector<std::string>>> &playlists)
{
    std::map<std::string, seabass::domain::Track> byId;
    for (const auto &[name, ids] : playlists) {
        for (int position = 0; position < static_cast<int>(ids.size()); ++position) {
            auto &track = byId[ids[static_cast<std::size_t>(position)]];
            track.sourceId = ids[static_cast<std::size_t>(position)];
            track.title = "Title " + track.sourceId;
            track.artist = "Artist " + track.sourceId;
            track.playlists.push_back({name, position + 1});
        }
    }
    std::vector<seabass::domain::Track> out;
    for (auto &[id, track] : byId) {
        out.push_back(std::move(track));
    }
    return out;
}

// Duplicate entries, common in rekordbox playlists: an extra copy on B's
// side is a track only B has, never a silent move.
void testExtraCopyInB()
{
    PlaylistDiffController controller;
    controller.setTracksForTesting(tracksOf({{"A", {"x"}}, {"B", {"x", "x"}}}));
    controller.setPlaylistA(QStringLiteral("A"));
    controller.setPlaylistB(QStringLiteral("B"));
    assert(controller.onlyBCount() == 1);
    assert(controller.movedCount() == 0);
    assert(controller.sharedCount() == 1);
    assert(controller.verdict() == QStringLiteral("B is A plus 1 extra copy."));
    assert(controller.onlyInBText() == QStringLiteral("Artist x - Title x"));
    for (const auto &row : controller.rows()->rows()) {
        assert(row.leftKind != QLatin1String("moved") && row.rightKind != QLatin1String("moved"));
    }
}

// An extra copy on A's side is only in A, and every moved row's partner
// is a row on screen (the folded-partner stub is for folds alone).
void testExtraCopyInAAndPartners()
{
    PlaylistDiffController controller;
    controller.setTracksForTesting(tracksOf({{"A", {"x", "x"}}, {"B", {"x"}}, {"C", {"x", "y", "x"}}, {"D", {"y", "x", "x"}}}));
    controller.setFoldIdentical(false);
    controller.setPlaylistA(QStringLiteral("A"));
    controller.setPlaylistB(QStringLiteral("B"));
    assert(controller.onlyACount() == 1 && controller.movedCount() == 0 && controller.sharedCount() == 1);
    assert(controller.verdict() == QStringLiteral("B is A minus 1 extra copy."));
    assert(controller.onlyInAText() == QStringLiteral("Artist x - Title x"));

    controller.setPlaylistA(QStringLiteral("C"));
    controller.setPlaylistB(QStringLiteral("D"));
    assert(controller.movedCount() == 1 && controller.onlyACount() == 0 && controller.onlyBCount() == 0);
    int moved = 0;
    for (const auto &row : controller.rows()->rows()) {
        if (row.leftKind == QLatin1String("moved") || row.rightKind == QLatin1String("moved")) {
            ++moved;
            assert(row.partnerRow >= 0);
        }
    }
    assert(moved == 2);
}

// Two long playlists diff on a worker, the page told so meanwhile, and
// an answer for a pair no longer shown never lands.
void testLongPlaylistsDiffOffTheGuiThread()
{
    // 2500 x 2500 entries, past the synchronous bound; Q is P with every
    // neighbouring pair swapped, so half the entries are moved.
    std::vector<std::string> p;
    std::vector<std::string> q;
    for (int i = 0; i < 2500; ++i) {
        p.push_back("t" + std::to_string(i));
        q.push_back("t" + std::to_string(i ^ 1));
    }
    PlaylistDiffController controller;
    controller.setTracksForTesting(tracksOf({{"P", p}, {"Q", q}, {"S", {"t0", "t1"}}}));
    controller.setPlaylistA(QStringLiteral("P"));
    controller.setPlaylistB(QStringLiteral("Q"));
    assert(controller.busy());
    assert(controller.rows()->rowCount() == 0);
    assert(controller.verdict() == QStringLiteral("Comparing two long playlists: 2500 and 2500 entries."));
    waitUntilIdle(controller);
    assert(controller.sharedCount() == 2500 && controller.movedCount() == 1250);
    assert(controller.onlyACount() == 0 && controller.onlyBCount() == 0);
    assert(controller.rows()->rowCount() > 0);

    // Superseded: the long diff starts, then a short pair is picked
    // before it answers. The short pair's diff stands; the long one's
    // answer, whenever it comes, is dropped.
    controller.setPlaylistB(QStringLiteral("S"));
    controller.setPlaylistB(QStringLiteral("Q"));
    assert(controller.busy());
    controller.setPlaylistB(QStringLiteral("S"));
    const QString shortVerdict = controller.verdict();
    assert(controller.sharedCount() == 2 && controller.onlyACount() == 2498);
    QSignalSpy diffChanged(&controller, &PlaylistDiffController::diffChanged);
    for (int i = 0; i < 20; ++i) {
        QTest::qWait(100);
    }
    assert(!controller.busy());
    assert(diffChanged.isEmpty());
    assert(controller.verdict() == shortVerdict && controller.sharedCount() == 2 && controller.onlyACount() == 2498);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: playlist_diff_controller_test <anonymized_library fixture>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path scratch = seabass::testing::scratchRoot() / "playlist_diff_controller";
    seabass::testing::sandboxSeabassHome(scratch / "home");
    const fs::path engineLibrary = scratch / "stick" / "Engine Library";
    fs::remove_all(scratch / "stick");
    fs::create_directories(scratch / "stick");
    fs::copy(fixture / "engine", engineLibrary, fs::copy_options::recursive);

    PlaylistDiffController controller;
    controller.scan(QStringLiteral("engine"), seabass::gui::pathToQString(engineLibrary));
    assert(controller.busy());
    waitUntilIdle(controller);
    assert(controller.errorMessage().isEmpty());

    // The fixture's Engine library names 33 playlists; three are empty,
    // and an empty playlist has no track to name it.
    const QStringList names = controller.playlistNames();
    assert(names.size() == 30);
    assert(names.first() == QStringLiteral("Playlist 000"));
    assert(controller.playlistTrackCounts().value(QStringLiteral("Playlist 000")).toInt() == 100);
    assert(controller.playlistTrackCounts().value(QStringLiteral("Playlist 019")).toInt() == 110);

    // Opens on the first playlist and its nearest relative: Playlist 019
    // is Playlist 000 with ten more at the end (the WHALESHARK2 shape).
    assert(controller.playlistA() == QStringLiteral("Playlist 000"));
    assert(controller.playlistB() == QStringLiteral("Playlist 019"));
    assert(controller.onlyACount() == 0);
    assert(controller.onlyBCount() == 10);
    assert(controller.sharedCount() == 100);
    assert(controller.movedCount() == 0);
    assert(controller.entriesA() == 100);
    assert(controller.entriesB() == 110);
    assert(controller.verdict() == QStringLiteral("Playlist 019 is Playlist 000 plus 10 tracks."));
    assert(controller.onlyInAText().isEmpty());
    assert(controller.onlyInBText().count(QLatin1Char('\n')) == 9);

    // Folded: the hundred identical rows keep their last two as context
    // (none at the top, the run starts the list), one fold row stands
    // for the other 98, then the ten added rows.
    const PlaylistDiffRowModel *rows = controller.rows();
    assert(rows->rowCount() == 13);
    assert(foldRows(*rows) == 1);
    assert(rows->rows()[0].kind == QLatin1String("fold"));
    assert(rows->rows()[0].foldCount == 98);
    assert(rows->rows()[2].kind == QLatin1String("same"));
    assert(rows->rows()[2].leftPosition == 100 && rows->rows()[2].rightPosition == 100);
    assert(rows->rows()[3].kind == QLatin1String("onlyB"));
    assert(rows->rows()[3].leftKind.isEmpty() && rows->rows()[3].rightKind == QLatin1String("only"));
    assert(rows->rows()[3].rightPosition == 101);
    assert(!rows->rows()[3].rightTitle.isEmpty());

    // Opening the fold puts the 98 rows in its place.
    controller.expandFold(0);
    assert(rows->rowCount() == 110);
    assert(foldRows(*rows) == 0);
    assert(rows->rows()[0].kind == QLatin1String("same") && rows->rows()[0].leftPosition == 1);

    // Folding off: the same, from the setting.
    controller.setFoldIdentical(false);
    assert(rows->rowCount() == 110);
    controller.setFoldIdentical(true);
    assert(rows->rowCount() == 13);

    // The relatives of A, most alike first: 019 holds all of A plus ten;
    // 004 is the first thirty of A.
    const QVariantList relatives = controller.relatives();
    assert(!relatives.isEmpty());
    const QVariantMap first = relatives.first().toMap();
    assert(first.value("name") == QStringLiteral("Playlist 019"));
    assert(first.value("relation") == QStringLiteral("superset"));
    assert(first.value("onlyB").toInt() == 10);
    const QVariantMap second = relatives.at(1).toMap();
    assert(second.value("name") == QStringLiteral("Playlist 004"));
    assert(second.value("relation") == QStringLiteral("subset"));
    assert(second.value("shared").toInt() == 30);

    // Swapping turns the plus into a minus.
    QSignalSpy diffChanged(&controller, &PlaylistDiffController::diffChanged);
    controller.swapPlaylists();
    assert(diffChanged.count() == 1);
    assert(controller.playlistA() == QStringLiteral("Playlist 019"));
    assert(controller.onlyACount() == 10 && controller.onlyBCount() == 0);
    assert(controller.verdict() == QStringLiteral("Playlist 000 is Playlist 019 minus 10 tracks."));

    // A reordered pair: 025 is 003 plus one track, 36 of the shared
    // ninety out of sequence (LCS 54 of 90). Moved rows show on both
    // sides and point at each other.
    controller.setPlaylistA(QStringLiteral("Playlist 003"));
    controller.setPlaylistB(QStringLiteral("Playlist 025"));
    assert(controller.sharedCount() == 90);
    assert(controller.onlyACount() == 0 && controller.onlyBCount() == 1);
    assert(controller.movedCount() == 36);
    controller.setFoldIdentical(false);
    int movedLeft = 0;
    int movedRight = 0;
    int partnered = 0;
    for (int i = 0; i < static_cast<int>(rows->rows().size()); ++i) {
        const auto &row = rows->rows()[static_cast<std::size_t>(i)];
        if (row.leftKind == QLatin1String("moved")) {
            ++movedLeft;
            assert(row.leftPartnerPosition > 0);
            if (row.partnerRow >= 0) {
                const auto &other = rows->rows()[static_cast<std::size_t>(row.partnerRow)];
                assert(other.rightKind == QLatin1String("moved"));
                assert(other.rightPosition == row.leftPartnerPosition);
                ++partnered;
            }
        }
        if (row.rightKind == QLatin1String("moved")) {
            ++movedRight;
        }
    }
    assert(movedLeft == 36 && movedRight == 36);
    assert(partnered == 36);

    // The same playlist on both sides, said so.
    controller.setPlaylistB(QStringLiteral("Playlist 003"));
    assert(controller.verdict() == QStringLiteral("The same playlist on both sides."));
    assert(controller.onlyACount() == 0 && controller.onlyBCount() == 0 && controller.movedCount() == 0);

    testExtraCopyInB();
    testExtraCopyInAAndPartners();
    testLongPlaylistsDiffOffTheGuiThread();

    std::cout << "playlist_diff_controller_test: ok\n";
    return 0;
}
