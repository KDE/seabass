// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>
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
// alpha, a beta and a dev build are all "testing"; only stable is stable.
// Empty for a channel nothing knows.
//
// "dev" throughout this file means a dev package CI made to hand out
// (SEABASS_PUBLISHED_BUILD): the signed packages from a Seabass/X.Y
// pipeline are dev builds, and the website lists them as test builds. A
// dev build from a working tree never gets here: UpdateChecker stops it
// before it fetches anything.
QString feedChannelFor(const QString &buildChannel);

// Whether a build channel is a pre-release one: alpha, beta or (a
// published) dev, the ones on the website's "testing" list.
bool isPreReleaseChannel(const QString &buildChannel);

// Which channels a build of this channel should be offered, steadiest
// first. A stable build is offered stables only: moving to a test build
// is not an update. A testing build is offered both, and within testing
// the version number decides, because the numbers only ever go up. A
// published "dev" build is a testing build here: the packages testers
// are handed are dev builds, and leaving them without updates would
// strand them. A channel nothing knows is offered nothing.
//
// includeTesting widens a stable build's list to testing too. It is the
// machine's memory of having run a pre-release (or of the hidden opt-in
// in Settings), so a tester who moved to the stable their beta became is
// still told about the next alpha. It changes nothing for other builds.
QVector<QString> channelsFor(const QString &buildChannel, bool includeTesting = false);

// The release this build should be told about, or nothing.
//
// Newest version across the channels it follows, skipping withdrawn ones
// and ones not yet released: a release that was pulled is never offered,
// whatever its number, and one nobody has smoke-tested is not offered
// either.
std::optional<ReleaseInfo> chooseUpdate(const QString &currentVersion, const QString &currentChannel,
                                        const QVector<ReleaseInfo> &releases, bool includeTesting = false);

// The running build's own entry, if the feed has one. Its purpose is the
// withdrawn flag: a user running a release that was pulled for losing
// data has to be told that, and it is the one thing worth saying even
// when there is no newer version to move to. So this one does look at
// entries that are not released yet: somebody handed a build to
// smoke-test is exactly who needs to hear it was withdrawn. A dev build
// is found by number alone on the testing list, because its entry on the
// website names the build it stands in for rather than "dev"; never on
// stable, whose entry of the same number is a different build.
std::optional<ReleaseInfo> findRunning(const QString &currentVersion, const QString &currentChannel,
                                       const QVector<ReleaseInfo> &releases);

// Whether a change to includeTesting may re-decide the checker's answer
// from the releases it already has, given the state it is in. Only over
// an answer that came from that feed: during a check, re-deciding would
// end "checking" early (and let a second request start), and after a
// failed one it would replace the error with an answer from an older feed
// that the failed check never gave.
bool mayRedecideFrom(const QString &state);

// Whether completing the tap sequence changes anything on a build of
// this channel with the setting as it is: only on a stable build, and
// only when test builds are not followed already. A pre-release or
// development build follows them whatever the setting says (a dev build
// from a working tree is offered nothing at all), and a build
// of a channel nothing knows is offered nothing at all, so for those the
// taps must neither claim a change nor pop anything up.
bool tapsWouldEnableTesting(const QString &buildChannel, bool includeTestingNow);

// The hidden switch for hearing about test builds on a stable build: ten
// taps on the version line within five seconds. Not in the user
// interface anywhere, on purpose: it is for people who know, and once it
// has been used the ordinary checkbox appears and stays. Pure arithmetic
// on the timestamps it is handed, so a test can tap at any pace it likes.
class TapSequence
{
public:
    static constexpr int TapsNeeded = 10;
    static constexpr qint64 WindowMs = 5000;

    // Records a tap at nowMs. True on the tap that completes the
    // sequence, which also starts over; taps older than the window are
    // forgotten first, so a slow tapper never gets there.
    bool tap(qint64 nowMs);

private:
    QVector<qint64> m_taps;
};

}  // namespace seabass::gui
