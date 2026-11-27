// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/recordings_changes.hpp"

#include <QDir>

namespace seabass::gui
{

RecordingsChanges &RecordingsChanges::instance()
{
    // Never destroyed: pages still connected to it at exit go first.
    static auto *changes = new RecordingsChanges;
    return *changes;
}

bool RecordingsChanges::sameStick(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty()
        && QDir::cleanPath(QDir::fromNativeSeparators(a)) == QDir::cleanPath(QDir::fromNativeSeparators(b));
}

}  // namespace seabass::gui
