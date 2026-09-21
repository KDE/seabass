// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/update_decision.hpp"

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

QVector<QString> channelsFor(const QString &channel)
{
    if (channel == QLatin1String("stable")) {
        return {QStringLiteral("stable")};
    }
    if (channel == QLatin1String("beta")) {
        return {QStringLiteral("stable"), QStringLiteral("beta")};
    }
    if (channel == QLatin1String("alpha")) {
        return {QStringLiteral("stable"), QStringLiteral("beta"), QStringLiteral("alpha")};
    }
    // dev, or anything unrecognised.
    return {};
}

std::optional<ReleaseInfo> chooseUpdate(const QString &currentVersion, const QString &currentChannel,
                                        const QVector<ReleaseInfo> &releases)
{
    const QVector<QString> follow = channelsFor(currentChannel);
    if (follow.isEmpty()) {
        return std::nullopt;
    }
    std::optional<ReleaseInfo> best;
    for (const ReleaseInfo &release : releases) {
        if (release.withdrawn) {
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
    for (const ReleaseInfo &release : releases) {
        if (release.version == currentVersion && release.channel == currentChannel) {
            return release;
        }
    }
    return std::nullopt;
}

}  // namespace seabass::gui
