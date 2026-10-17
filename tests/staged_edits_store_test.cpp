// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// StagedEditsStore on its own, over a directory of its own: Browse's
// staged playlist edits written and read back as they were staged, the
// file gone once nothing is staged, and a track's path re-rooted on the
// mount point the stick has on the day it is read back. The controller
// and the page that offer them back are tst_ScanPage's.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cassert>
#include <filesystem>
#include <iostream>

#include "gui/edit/staged_edits_store.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::gui::StagedEditRequest;
using seabass::gui::StagedEdits;
using seabass::gui::StagedEditsStore;
namespace fs = std::filesystem;

namespace
{

StagedEdits batch(const QString &mountPoint)
{
    StagedEdits edits;
    edits.libraryId = QStringLiteral("uuid:1234-ABCD/odd:name");
    edits.stickLabel = QStringLiteral("WHALESHARK3");
    edits.savedAtUtc = QStringLiteral("2026-10-17T02:40:00Z");
    edits.appVersion = QStringLiteral("0.8.8");
    edits.requests = {
        {QStringLiteral("create"), QStringLiteral("Fresh Picks"), {}, {}, -1, {}, false},
        {QStringLiteral("add"), QStringLiteral("Fresh Picks"), mountPoint + QStringLiteral("/Contents/a b/1.mp3"),
         QStringLiteral("One"), -1, {}, true},
        {QStringLiteral("move"), QStringLiteral("Peak"), mountPoint + QStringLiteral("/Contents/2.mp3"),
         QStringLiteral("Two"), 3, {}, false},
        {QStringLiteral("rename"), QStringLiteral("Peak"), {}, {}, -1, QStringLiteral("Peak Time"), false},
        {QStringLiteral("remove"), QStringLiteral("Peak Time"), mountPoint + QStringLiteral("/Contents/3.mp3"),
         QStringLiteral("Three"), -1, {}, false},
        {QStringLiteral("delete"), QStringLiteral("Old"), {}, {}, -1, {}, false},
    };
    return edits;
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "staged-edits-store";
    fs::remove_all(scratch);
    const QString directory = QString::fromStdString(seabass::pathToGenericUtf8(scratch / "staged-edits"));
    const StagedEditsStore store(directory);
    const QString mount = QStringLiteral("/media/sebas/WHALESHARK3");

    {
        // Nothing staged, nothing written: not even the directory.
        StagedEdits empty = batch(mount);
        empty.requests.clear();
        assert(store.write(empty, mount));
        assert(!fs::exists(scratch / "staged-edits"));
        assert(!store.read(empty.libraryId, mount).has_value());
        std::cout << "case 1 (an empty batch writes nothing) OK\n";
    }

    {
        const StagedEdits written = batch(mount);
        assert(store.write(written, mount));
        const QString path = store.pathFor(written.libraryId);
        assert(QFile::exists(path));
        assert(path.startsWith(directory + QLatin1Char('/')));
        assert(!path.mid(directory.size() + 1).contains(QLatin1Char('/')));
        assert(!path.contains(QLatin1Char(':')));
        const auto read = store.read(written.libraryId, mount);
        assert(read.has_value());
        assert(read->libraryId == written.libraryId);
        assert(read->stickLabel == written.stickLabel);
        assert(read->savedAtUtc == written.savedAtUtc);
        assert(read->appVersion == written.appVersion);
        assert(read->requests.size() == written.requests.size());
        for (std::size_t i = 0; i < written.requests.size(); ++i) {
            if (!(read->requests[i] == written.requests[i])) {
                std::cerr << "request " << i << " came back as " << read->requests[i].kind.toStdString() << " "
                          << read->requests[i].file.toStdString() << "\n";
                assert(false);
            }
        }
        std::cout << "case 2 (the batch round-trips, in order) OK\n";

        // On disk the files are the stick's, not this mount's.
        QFile file(path);
        assert(file.open(QIODevice::ReadOnly));
        const QByteArray text = file.readAll();
        assert(!text.contains("/media/sebas"));
        assert(text.contains("Contents/a b/1.mp3"));
        std::cout << "case 3 (files are kept relative to the mount point) OK\n";

        // The same stick mounted elsewhere tomorrow.
        const QString elsewhere = QStringLiteral("/run/media/someone/WHALESHARK3");
        const auto moved = store.read(written.libraryId, elsewhere);
        assert(moved.has_value());
        assert(moved->requests[1].file == QDir::toNativeSeparators(elsewhere + QStringLiteral("/Contents/a b/1.mp3")));
        assert(moved->requests[2].file == QDir::toNativeSeparators(elsewhere + QStringLiteral("/Contents/2.mp3")));
        assert(moved->requests[0].file.isEmpty());
        assert(StagedEditsStore::relativeTo(mount, mount + QStringLiteral("/Contents/x.mp3"))
               == QStringLiteral("Contents/x.mp3"));
        assert(StagedEditsStore::relativeTo(mount + QLatin1Char('/'), mount + QStringLiteral("/Contents/x.mp3"))
               == QStringLiteral("Contents/x.mp3"));
        // A path outside the mount point is kept whole, and comes back
        // whole: never glued onto another root.
        assert(StagedEditsStore::relativeTo(mount, QStringLiteral("/elsewhere/x.mp3")) == QStringLiteral("/elsewhere/x.mp3"));
        assert(StagedEditsStore::reroot(elsewhere, QStringLiteral("/elsewhere/x.mp3")) == QStringLiteral("/elsewhere/x.mp3"));
        std::cout << "case 4 (a path re-roots on another mount point) OK\n";

        // Shrunk, then emptied: the file follows, and goes.
        StagedEdits shrunk = written;
        shrunk.requests.resize(2);
        assert(store.write(shrunk, mount));
        assert(store.read(written.libraryId, mount)->requests.size() == 2);
        shrunk.requests.clear();
        assert(store.write(shrunk, mount));
        assert(!QFile::exists(path));
        assert(!store.read(written.libraryId, mount).has_value());
        std::cout << "case 5 (the file goes when the batch is empty) OK\n";
    }

    {
        // Another library's edits, or a torn file, are never offered.
        StagedEdits other = batch(mount);
        assert(store.write(other, mount));
        // Sanitised to the same file name as `other`'s id.
        const QString lookalike = QStringLiteral("uuid:1234-ABCD/odd_name");
        assert(store.pathFor(lookalike) == store.pathFor(other.libraryId));
        assert(!store.read(lookalike, mount).has_value());
        assert(store.read(other.libraryId, mount).has_value());
        QFile torn(store.pathFor(other.libraryId));
        assert(torn.open(QIODevice::WriteOnly | QIODevice::Truncate));
        torn.write("{\"libraryId\": \"uuid:12");
        torn.close();
        assert(!store.read(other.libraryId, mount).has_value());
        store.remove(other.libraryId);
        assert(!QFile::exists(store.pathFor(other.libraryId)));
        std::cout << "case 6 (a torn or foreign file is not offered) OK\n";
    }

    fs::remove_all(scratch);
    std::cout << "staged_edits_store_test: all cases passed\n";
    return 0;
}
