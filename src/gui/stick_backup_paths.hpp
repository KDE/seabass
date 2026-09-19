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
