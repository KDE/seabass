// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The name a stick's backup lives under.
//
// It is not decoration: the archive is found by this name, the .journal,
// .lock and .compacting siblings are derived from it by appending a
// suffix, and the advisor falls back to matching a stick to a backup by
// label. One definition, used by the backup, restore and clone pages, so
// all three find the same file for the same stick.
//
// Untested until a real stick made the gap obvious: one carrying the
// label A1 was relabelled SANDISK_1, and two sticks sharing a label is
// the ordinary case on a desk with several of them.
#include <cassert>
#include <iostream>

#include <QCoreApplication>
#include <QDir>

#include "gui/stick_backup_paths.hpp"

using seabass::gui::archiveFileNameForLabel;
using seabass::gui::archiveFileNameFor;
using seabass::gui::archivePathFor;
using seabass::gui::archivePathForLabel;
using seabass::gui::backupNameFor;
using seabass::gui::shouldRecordBackupName;

namespace
{

void testTheNameIsTheLabel()
{
    assert(archiveFileNameForLabel(QStringLiteral("WHALESHARK")) == QStringLiteral("WHALESHARK.zip"));
    // Trimmed, because a label with a trailing space would otherwise make
    // a file whose name nobody can type back.
    assert(archiveFileNameForLabel(QStringLiteral("  MAIN  ")) == QStringLiteral("MAIN.zip"));
}

void testCharactersNoFilesystemTakesAreReplaced()
{
    assert(archiveFileNameForLabel(QStringLiteral("A/B:C*D?E\"F<G>H|I")) == QStringLiteral("A_B_C_D_E_F_G_H_I.zip"));
    // Never empty, and never a name that means something else to a
    // filesystem.
    assert(archiveFileNameForLabel(QString()) == QStringLiteral("stick.zip"));
    assert(archiveFileNameForLabel(QStringLiteral(".")) == QStringLiteral("stick.zip"));
    assert(archiveFileNameForLabel(QStringLiteral("..")) == QStringLiteral("stick.zip"));
}

void testCollisionsCountUpFromThePlainName()
{
    // 1 is the plain name, so a caller can loop from 1 without having to
    // special-case the first time round.
    assert(archiveFileNameForLabel(QStringLiteral("MAIN"), 1) == QStringLiteral("MAIN.zip"));
    assert(archiveFileNameForLabel(QStringLiteral("MAIN"), 2) == QStringLiteral("MAIN (2).zip"));
    assert(archiveFileNameForLabel(QStringLiteral("MAIN"), 3) == QStringLiteral("MAIN (3).zip"));
    // 0 and negatives are the plain name too, rather than something
    // absurd: a caller that miscounts should not produce "MAIN (0).zip".
    assert(archiveFileNameForLabel(QStringLiteral("MAIN"), 0) == QStringLiteral("MAIN.zip"));
    assert(archiveFileNameForLabel(QStringLiteral("MAIN"), -1) == QStringLiteral("MAIN.zip"));
}

void testOnlyTheExtensionComesOff()
{
    // A label with dots of its own. Splitting on '.' would turn this into
    // "DJ (2).zip" and lose most of the name.
    assert(archiveFileNameForLabel(QStringLiteral("DJ.set.2024"), 2) == QStringLiteral("DJ.set.2024 (2).zip"));
    // A label that already ends in .zip keeps its own: the stem is what
    // this function appended, not what the label happened to contain, so
    // "backup.zip" is a stick called backup.zip and its archive says so.
    assert(archiveFileNameForLabel(QStringLiteral("backup.zip")) == QStringLiteral("backup.zip.zip"));
    assert(archiveFileNameForLabel(QStringLiteral("backup.zip"), 2) == QStringLiteral("backup.zip (2).zip"));
}

void testPathsJoinTheDirectory()
{
    const QString dir = QStringLiteral("/tmp/backups");
    assert(archivePathForLabel(dir, QStringLiteral("MAIN")) == QDir(dir).filePath(QStringLiteral("MAIN.zip")));
    assert(archivePathForLabel(dir, QStringLiteral("MAIN"), 2) == QDir(dir).filePath(QStringLiteral("MAIN (2).zip")));
    // The one-argument form and attempt 1 must never disagree, or the
    // restore page would look for a file the backup page did not write.
    assert(archivePathForLabel(dir, QStringLiteral("MAIN")) == archivePathForLabel(dir, QStringLiteral("MAIN"), 1));
}

void testTheNameWinsOverTheLabel()
{
    // What a person recognises the backup by is what the file is called,
    // so a stick still carrying its factory label does not name the
    // archive after it.
    assert(archiveFileNameFor(QStringLiteral("TESTRIG_ABC"), QStringLiteral("SANDISK_1"))
           == QStringLiteral("TESTRIG_ABC.zip"));

    // No name, or nothing but spaces: the label, exactly as before.
    assert(archiveFileNameFor(QString(), QStringLiteral("SANDISK_1")) == QStringLiteral("SANDISK_1.zip"));
    assert(archiveFileNameFor(QStringLiteral("   "), QStringLiteral("SANDISK_1")) == QStringLiteral("SANDISK_1.zip"));

    // A name goes through the same sanitising as a label: it is free text
    // a person typed, so it can hold anything at all.
    assert(archiveFileNameFor(QStringLiteral("before/the gig"), QStringLiteral("MAIN"))
           == QStringLiteral("before_the gig.zip"));

    // Collisions count up on whichever of the two supplied the name.
    assert(archiveFileNameFor(QStringLiteral("TESTRIG_ABC"), QStringLiteral("SANDISK_1"), 2)
           == QStringLiteral("TESTRIG_ABC (2).zip"));
    assert(archiveFileNameFor(QString(), QStringLiteral("SANDISK_1"), 2) == QStringLiteral("SANDISK_1 (2).zip"));
}

void testTheFieldStartsOutHoldingTheStickName()
{
    // What the Name field on the Full Stick Backup page shows before
    // anybody types: the stick's own label, because an empty field made a
    // person guess what the backup would end up called.
    assert(backupNameFor(QString(), QStringLiteral("SANDISK_1")) == QStringLiteral("SANDISK_1"));
    // A backup that already carries a name keeps it: that name is the one
    // its owner chose, and the field is also how they rename it.
    assert(backupNameFor(QStringLiteral("Seabass test fixture A"), QStringLiteral("SANDISK_1"))
           == QStringLiteral("Seabass test fixture A"));
    // A stored name of nothing but spaces is no name at all.
    assert(backupNameFor(QStringLiteral("   "), QStringLiteral("SANDISK_1")) == QStringLiteral("SANDISK_1"));
    // Trimmed both ways, so the field never starts out with whitespace
    // that would be written into the manifest.
    assert(backupNameFor(QString(), QStringLiteral("  SANDISK_1 ")) == QStringLiteral("SANDISK_1"));
    assert(backupNameFor(QStringLiteral(" Before Berlin "), QStringLiteral("SANDISK_1"))
           == QStringLiteral("Before Berlin"));
    // The default changes no filename: the archive was already called
    // after the label when the name was empty, so a field defaulting to
    // the label lands on exactly the same file.
    assert(archiveFileNameFor(backupNameFor(QString(), QStringLiteral("SANDISK_1")), QStringLiteral("SANDISK_1"))
           == archiveFileNameFor(QString(), QStringLiteral("SANDISK_1")));
}

void testTheDefaultNameIsNotWrittenUntilAPreviewSaysItMay()
{
    const QString label = QStringLiteral("SANDISK_1");
    const QString stored = QStringLiteral("Seabass test fixture A");
    // The page opens, the field holds the label, no preview has come back:
    // starting a backup now must not record a name, because the archive
    // may already carry one this has not read yet. Back Up Now really is
    // live here -- busy() covers runs, not previews.
    assert(!shouldRecordBackupName(true, false, label, QString()));
    // The preview came back and said there is no name: now the default is
    // the name, and the backup is called after its stick.
    assert(shouldRecordBackupName(true, true, label, QString()));
    // The preview came back with a name, which the field adopted: nothing
    // changed, so nothing is written and the stored name survives.
    assert(!shouldRecordBackupName(false, true, stored, stored));
    // A typed name is written whether or not a preview ever answered --
    // including a preview that failed, which never settles.
    assert(shouldRecordBackupName(false, false, QStringLiteral("Before Berlin"), stored));
    // Clearing the box is a decision too: an empty name differs from the
    // stored one, so it is recorded as the clearing it is.
    assert(shouldRecordBackupName(false, false, QString(), stored));
}

void testNamingABackupAfterItsStickLandsOnTheCollidedName()
{
    // Why StickBackupController refuses to rename an archive back onto the
    // plain label once it has stepped off it: with the name defaulting to
    // the label, the file the name asks for IS the file the collision
    // stepped around, which belongs to the other stick.
    const QString dir = QStringLiteral("/b");
    const QString label = QStringLiteral("MAIN");
    assert(archivePathFor(dir, label, label, 1) == archivePathForLabel(dir, label, 1));
    assert(archivePathForLabel(dir, label, 2) != archivePathForLabel(dir, label, 1));
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testTheNameIsTheLabel();
    testCharactersNoFilesystemTakesAreReplaced();
    testCollisionsCountUpFromThePlainName();
    testOnlyTheExtensionComesOff();
    testPathsJoinTheDirectory();
    testTheNameWinsOverTheLabel();
    testTheFieldStartsOutHoldingTheStickName();
    testTheDefaultNameIsNotWrittenUntilAPreviewSaysItMay();
    testNamingABackupAfterItsStickLandsOnTheCollidedName();
    std::cout << "stick_backup_paths_test passed\n";
    return 0;
}
