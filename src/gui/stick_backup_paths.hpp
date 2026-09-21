// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDir>
#include <QString>

namespace seabass::gui
{

// The file a stick's full backup lives in: named after the stick label
// with the characters no filesystem accepts replaced, never empty. One
// definition, so the backup page, the restore page and the clone page
// all find the same archive for the same stick.
inline QString archiveFileNameForLabel(const QString &stickLabel)
{
    QString name = stickLabel.trimmed();
    for (QChar &c : name) {
        if (QStringLiteral("/\\:*?\"<>|").contains(c) || c.unicode() < 0x20) {
            c = QLatin1Char('_');
        }
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        name = QStringLiteral("stick");
    }
    return name + QStringLiteral(".zip");
}

inline QString archivePathForLabel(const QString &backupDirectory, const QString &stickLabel)
{
    return QDir(backupDirectory).filePath(archiveFileNameForLabel(stickLabel));
}

// The same name with a number on it: "MAIN.zip", then "MAIN (2).zip".
//
// Two sticks can carry one label -- a spare bought from the same shelf,
// or a stick relabelled after it was first backed up -- and the archive
// is chosen by label, so they collide on one file. Updating one stick's
// backup with another stick's contents diffs the newcomer against it and
// records every file of the original as removed, which is the original's
// backup gone. So a collision takes the next free name by default, and
// replacing the other stick's archive is something to ask for.
//
// `attempt` is 1-based: 1 is the plain name, so a caller can count up
// from 1 without special-casing the first.
inline QString archiveFileNameForLabel(const QString &stickLabel, int attempt)
{
    const QString base = archiveFileNameForLabel(stickLabel);
    if (attempt <= 1) {
        return base;
    }
    // chopped() rather than a split on '.': a label may contain dots of
    // its own, and only the extension this function just added is meant
    // to come off.
    const QString stem = base.chopped(QStringLiteral(".zip").size());
    return stem + QStringLiteral(" (") + QString::number(attempt) + QStringLiteral(").zip");
}

// What the archive is called when the backup has been given a name: the
// name, falling back to the stick's label.
//
// The name is what a person recognises the backup by, so it is what they
// look for in a folder -- "TESTRIG_ABC.zip", not "SANDISK_1.zip" after a
// factory label nobody chose. The cost is that renaming a backup has to
// move the file, and with it the .journal and .lock siblings, which are
// derived from the archive path by appending a suffix. Nothing may rename
// an archive that is open for browsing: .seabass-backup-source stores the
// absolute path and would be left pointing at nothing.
inline QString archiveFileNameFor(const QString &backupName, const QString &stickLabel, int attempt = 1)
{
    const QString chosen = backupName.trimmed().isEmpty() ? stickLabel : backupName;
    return archiveFileNameForLabel(chosen, attempt);
}

// The name a backup's Name field starts out holding: the stick's own
// label, so the field a person looks at already says what the backup is
// of, and the archive and the manifest agree with it without anybody
// typing. A backup that was named before keeps that name -- the stored
// name is the one its owner chose, and adopting it is what lets the field
// double as "rename this backup".
//
// Deliberately NOT the same thing as archiveFileNameFor's fallback: that
// one decides a filename when the name is empty, which stays true; this
// one decides what to show, and an empty field is what a person sees when
// they have cleared the name on purpose.
inline QString backupNameFor(const QString &storedName, const QString &stickLabel)
{
    return storedName.trimmed().isEmpty() ? stickLabel.trimmed() : storedName.trimmed();
}

// Whether what is in the Name box is a name to write into the archive's
// manifest, or only the default nobody has confirmed yet.
//
// Leaving it unwritten is what tells BackupStick to keep the name the
// previous generation had. The field now starts out holding the stick's
// label, and that default must not count as a name until a preview has
// come back and said this stick's backup has none of its own: a backup
// can be started before that -- the page's Back Up Now is live while a
// preview runs -- and a preview that fails never says anything at all.
// Either way the stick's label would otherwise be stamped over a name its
// owner chose.
inline bool shouldRecordBackupName(bool isStillTheDefault, bool previewSettled, const QString &name,
                                    const QString &savedName)
{
    return (previewSettled || !isStillTheDefault) && name != savedName;
}

inline QString archivePathFor(const QString &backupDirectory, const QString &backupName, const QString &stickLabel,
                               int attempt = 1)
{
    return QDir(backupDirectory).filePath(archiveFileNameFor(backupName, stickLabel, attempt));
}

inline QString archivePathForLabel(const QString &backupDirectory, const QString &stickLabel, int attempt)
{
    return QDir(backupDirectory).filePath(archiveFileNameForLabel(stickLabel, attempt));
}

}  // namespace seabass::gui
