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
//
// The website has two channels, "testing" and "stable", while a build's
// own channel is the finer alpha/beta/stable the tag carries. The two are
// not the same word for the same thing, so `channel` here is always the
// website's and `build` is what the package called itself.
struct ReleaseInfo
{
    QString version;   // "0.2.1"
    QString channel;   // testing or stable: the list this entry came from
    QString build;     // alpha, beta or stable: what the package reports
    QString date;      // ISO date it was published
    QString note;
    QString noteLevel;  // info or warning
    // False until a person has installed this build and run it. An entry
    // exists from the moment its packages are uploaded, which is before
    // anybody has checked they start, so it is not offered to anyone yet.
    bool released = false;
    bool withdrawn = false;
    QString withdrawnReason;
};

// -1, 0, 1, comparing X.Y.Z numerically. A part that is not a number
// counts as 0 rather than throwing: a malformed version on the website
// must not be able to stop the app.
int compareVersions(const QString &left, const QString &right);

// Which website channel a build of this channel is published on. An
// alpha and a beta are both "testing"; only stable is stable. Empty for
// "dev", which is not published at all.
QString feedChannelFor(const QString &buildChannel);

// Which channels a build of this channel should be offered, steadiest
// first. A stable build is offered stables only: moving to a test build
// is not an update. A testing build is offered both, and within testing
// the version number decides, because the numbers only ever go up. A
// "dev" build -- anything not built from a release tag -- is offered
// nothing, because it is not any published version and comparing it to
// one is meaningless.
QVector<QString> channelsFor(const QString &buildChannel);

// The release this build should be told about, or nothing.
//
// Newest version across the channels it follows, skipping withdrawn ones
// and ones not yet released: a release that was pulled is never offered,
// whatever its number, and one nobody has smoke-tested is not offered
// either.
std::optional<ReleaseInfo> chooseUpdate(const QString &currentVersion, const QString &currentChannel,
                                        const QVector<ReleaseInfo> &releases);

// The running build's own entry, if the feed has one. Its purpose is the
// withdrawn flag: a user running a release that was pulled for losing
// data has to be told that, and it is the one thing worth saying even
// when there is no newer version to move to. So this one does look at
// entries that are not released yet: somebody handed a build to
// smoke-test is exactly who needs to hear it was withdrawn.
std::optional<ReleaseInfo> findRunning(const QString &currentVersion, const QString &currentChannel,
                                       const QVector<ReleaseInfo> &releases);

}  // namespace seabass::gui
