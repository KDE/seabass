// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/update_decision.hpp"

#include <algorithm>

namespace seabass::gui
{

int compareVersions(const QString &left, const QString &right)
{
    const QStringList leftParts = left.split(QLatin1Char('.'));
    const QStringList rightParts = right.split(QLatin1Char('.'));
    const int parts = qMax(leftParts.size(), rightParts.size());
    for (int i = 0; i < parts; ++i) {
        // A missing part is 0, so 1.2 and 1.2.0 are the same release.
        const int a = i < leftParts.size() ? leftParts.at(i).toInt() : 0;
        const int b = i < rightParts.size() ? rightParts.at(i).toInt() : 0;
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return 0;
}

QString feedChannelFor(const QString &buildChannel)
{
    if (buildChannel == QLatin1String("stable")) {
        return QStringLiteral("stable");
    }
    if (buildChannel == QLatin1String("alpha") || buildChannel == QLatin1String("beta")) {
        return QStringLiteral("testing");
    }
    // dev, or anything unrecognised. Not published anywhere.
    return {};
}

bool isPreReleaseChannel(const QString &buildChannel)
{
    return feedChannelFor(buildChannel) == QLatin1String("testing");
}

QVector<QString> channelsFor(const QString &buildChannel, bool includeTesting)
{
    const QString own = feedChannelFor(buildChannel);
    if (own.isEmpty()) {
        return {};
    }
    if (own == QLatin1String("stable") && !includeTesting) {
        return {QStringLiteral("stable")};
    }
    // A test build follows both: the newest stable is an update for it
    // just as much as the next alpha is, and which of the two is newer is
    // the version number's business, not the channel's. So does a stable
    // build on a machine that has asked for test builds.
    return {QStringLiteral("stable"), QStringLiteral("testing")};
}

std::optional<ReleaseInfo> chooseUpdate(const QString &currentVersion, const QString &currentChannel,
                                        const QVector<ReleaseInfo> &releases, bool includeTesting)
{
    const QVector<QString> follow = channelsFor(currentChannel, includeTesting);
    if (follow.isEmpty()) {
        return std::nullopt;
    }
    std::optional<ReleaseInfo> best;
    for (const ReleaseInfo &release : releases) {
        if (release.withdrawn) {
            continue;
        }
        // Uploaded but not yet smoke-tested. The website hides these too;
        // this is the same gate on the app's side, so a release that
        // turns out not to start was never offered to anybody.
        if (!release.released) {
            continue;
        }
        if (!follow.contains(release.channel)) {
            continue;
        }
        if (compareVersions(release.version, currentVersion) <= 0) {
            continue;
        }
        if (!best || compareVersions(release.version, best->version) > 0) {
            best = release;
        }
    }
    return best;
}

std::optional<ReleaseInfo> findRunning(const QString &currentVersion, const QString &currentChannel,
                                       const QVector<ReleaseInfo> &releases)
{
    // By the build's own channel where the entry records one, and by the
    // website channel it maps to otherwise: an alpha and a beta of the
    // same number are different builds and must not inherit each other's
    // withdrawal, but they share one list.
    const QString feed = feedChannelFor(currentChannel);
    for (const ReleaseInfo &release : releases) {
        if (release.version != currentVersion) {
            continue;
        }
        const QString mine = release.build.isEmpty() ? currentChannel : release.build;
        if (mine == currentChannel && release.channel == feed) {
            return release;
        }
    }
    return std::nullopt;
}

bool TapSequence::tap(qint64 nowMs)
{
    m_taps.erase(std::remove_if(m_taps.begin(), m_taps.end(),
                                [nowMs](qint64 then) { return nowMs - then > WindowMs || then > nowMs; }),
                 m_taps.end());
    m_taps.append(nowMs);
    if (m_taps.size() < TapsNeeded) {
        return false;
    }
    m_taps.clear();
    return true;
}

}  // namespace seabass::gui
