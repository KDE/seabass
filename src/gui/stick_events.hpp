// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QString>

namespace seabass::gui
{

// One process-wide place that says a stick went away. MediaController
// tells it; every AsyncRequest listens, so a page reading a stick that
// was pulled ends its read with an error instead of waiting for I/O on a
// device that is gone. Pages are QML-instantiated and never see the
// MediaController, which is why this is a singleton and not a pointer
// handed around. See docs/async-requests.md, rule 6.
class StickEvents : public QObject
{
    Q_OBJECT

public:
    static StickEvents &instance();

    void announceStickGone(const QString &mountPoint) { emit stickGone(mountPoint); }
    // A write rewrote what is on a stick after the page that made it had
    // gone, so nobody was left to ask for a re-detect. MediaController
    // listens.
    void announceStickContentsChanged(const QString &stickRoot) { emit stickContentsChanged(stickRoot); }

signals:
    void stickGone(const QString &mountPoint);
    void stickContentsChanged(const QString &stickRoot);

private:
    StickEvents() = default;
};

}  // namespace seabass::gui
