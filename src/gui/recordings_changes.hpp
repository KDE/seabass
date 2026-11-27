// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QString>

namespace seabass::gui
{

// Says that the recordings on a stick may be other than they were: a
// delete on the Recordings page ended, or a keep did. Browse's Own
// Recordings row lists them too, from a page of its own, and reads them
// again when told. The app's one, used on the GUI thread only.
class RecordingsChanges : public QObject
{
    Q_OBJECT

public:
    static RecordingsChanges &instance();

    void announce(const QString &stickRoot) { emit changed(stickRoot); }

    // Whether two spellings of a stick's root name one stick.
    static bool sameStick(const QString &a, const QString &b);

signals:
    void changed(const QString &stickRoot);

private:
    using QObject::QObject;
};

}  // namespace seabass::gui
