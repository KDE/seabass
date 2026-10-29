// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Where Metadata Backup finds a stored row's audio file to play it: on the
// stick it was backed up from first, then on any other, and nowhere else.
// Two stick-shaped directories under the test's own scratch tree; no real
// stick is looked at.

#include <QTest>

#include <filesystem>
#include <fstream>

#include "gui/metadata_row_text.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{
const QString Track = QStringLiteral("Contents/Artist/Album/track.mp3");

QVariantMap library(const QString &label, const fs::path &stickRoot)
{
    return {{QStringLiteral("label"), label}, {QStringLiteral("catalogPath"), pathToQString(stickRoot / "PIONEER")}};
}

void plant(const fs::path &stickRoot, const QString &relative)
{
    const fs::path file = stickRoot / pathFromQString(relative);
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "audio";
}
}  // namespace

class StoredTrackFileTest : public QObject
{
    Q_OBJECT

private:
    fs::path m_root;
    fs::path m_home;   // the stick the row was backed up from
    fs::path m_other;  // a different stick with the same export

private slots:
    void init()
    {
        m_root = seabass::testing::scratchRoot() / "stored_track_file_test";
        fs::remove_all(m_root);
        m_home = m_root / "HOME";
        m_other = m_root / "OTHER";
        fs::create_directories(m_home / "PIONEER");
        fs::create_directories(m_other / "PIONEER");
    }

    void cleanup() { fs::remove_all(m_root); }

    // Both sticks have it: the one the row names wins, wherever it is
    // listed.
    void foundOnTheStickItWasBackedUpFrom()
    {
        plant(m_home, Track);
        plant(m_other, Track);
        const QVariantList sticks{library(QStringLiteral("OTHER"), m_other), library(QStringLiteral("HOME"), m_home)};
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("HOME"), sticks),
                 pathToQString(m_home / "Contents/Artist/Album/track.mp3"));
    }

    // Its own stick is plugged in without the file (or not plugged in at
    // all): another stick that has it plays it.
    void foundOnAnotherStick()
    {
        plant(m_other, Track);
        const QVariantList sticks{library(QStringLiteral("HOME"), m_home), library(QStringLiteral("OTHER"), m_other)};
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("HOME"), sticks),
                 pathToQString(m_other / "Contents/Artist/Album/track.mp3"));
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("GONE"), {library(QStringLiteral("OTHER"), m_other)}),
                 pathToQString(m_other / "Contents/Artist/Album/track.mp3"));
    }

    void notFound()
    {
        const QVariantList sticks{library(QStringLiteral("HOME"), m_home), library(QStringLiteral("OTHER"), m_other)};
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("HOME"), sticks), QString());
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("HOME"), {}), QString());
        // A folder of that name is not a file to play.
        fs::create_directories(m_home / pathFromQString(Track));
        QCOMPARE(storedTrackFileOnSticks(Track, QStringLiteral("HOME"), sticks), QString());
    }

    // A stored path that is not stick-relative names nothing on a stick,
    // even when a file is there.
    void onlyStickRelativePaths()
    {
        plant(m_root, QStringLiteral("outside.mp3"));
        const QVariantList sticks{library(QStringLiteral("HOME"), m_home)};
        QCOMPARE(storedTrackFileOnSticks(QStringLiteral("../outside.mp3"), QStringLiteral("HOME"), sticks), QString());
        QCOMPARE(storedTrackFileOnSticks(pathToQString(m_root / "outside.mp3"), QStringLiteral("HOME"), sticks),
                 QString());
        QCOMPARE(storedTrackFileOnSticks(QString(), QStringLiteral("HOME"), sticks), QString());
    }
};

QTEST_GUILESS_MAIN(StoredTrackFileTest)
#include "stored_track_file_test.moc"
