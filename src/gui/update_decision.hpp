// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QVector>
#include <optional>

namespace seabass::gui
{

// One release as the website's releases.json describes it. Deliberately
// separate from the fetching: what to do about a list of releases is
// decided here, by code a test can hand any list at all, and the network
// only supplies the list.
struct ReleaseInfo
{
    QString version;   // "0.2.1"
    QString channel;   // alpha, beta, stable
    QString released;  // ISO date
    QString note;
    QString noteLevel;  // info or warning
    bool withdrawn = false;
    QString withdrawnReason;
};

// -1, 0, 1, comparing X.Y.Z numerically. A part that is not a number
// counts as 0 rather than throwing: a malformed version on the website
// must not be able to stop the app.
int compareVersions(const QString &left, const QString &right);

// Which channels a build of this channel should be offered, steadiest
// first. A beta build is offered betas and stables, never alphas: moving
// to a less finished build is not an update. A "dev" build -- anything
// not built from a release tag -- is offered nothing, because it is not
// any published version and comparing it to one is meaningless.
QVector<QString> channelsFor(const QString &channel);

// The release this build should be told about, or nothing.
//
// Newest version across the channels it follows, skipping withdrawn
// ones: a release that was pulled is never offered, whatever its number.
// Equal or older than what is running means nothing to say.
std::optional<ReleaseInfo> chooseUpdate(const QString &currentVersion, const QString &currentChannel,
                                        const QVector<ReleaseInfo> &releases);

// The running build's own entry, if the feed has one. Its purpose is the
// withdrawn flag: a user running a release that was pulled for losing
// data has to be told that, and it is the one thing worth saying even
// when there is no newer version to move to.
std::optional<ReleaseInfo> findRunning(const QString &currentVersion, const QString &currentChannel,
                                       const QVector<ReleaseInfo> &releases);

}  // namespace seabass::gui
