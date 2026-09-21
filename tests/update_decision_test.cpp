// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the update check decides, without a network anywhere near it.
// The rules it encodes are promises: a withdrawn release is never
// offered, a build is never sent to a less finished channel than its
// own, and a development build is offered nothing at all.

#include <QTest>

#include "gui/update_decision.hpp"

using namespace seabass::gui;

namespace
{
ReleaseInfo make(const QString &version, const QString &channel, bool withdrawn = false)
{
    ReleaseInfo release;
    release.version = version;
    release.channel = channel;
    release.withdrawn = withdrawn;
    if (withdrawn) {
        release.withdrawnReason = QStringLiteral("it ate a library");
    }
    return release;
}
}  // namespace

class UpdateDecisionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void comparesNumerically()
    {
        QCOMPARE(compareVersions(QStringLiteral("0.2.0"), QStringLiteral("0.10.0")), -1);
        QCOMPARE(compareVersions(QStringLiteral("1.0.0"), QStringLiteral("0.99.99")), 1);
        QCOMPARE(compareVersions(QStringLiteral("0.2.1"), QStringLiteral("0.2.1")), 0);
        // A missing part is a zero, so these are the same release.
        QCOMPARE(compareVersions(QStringLiteral("1.2"), QStringLiteral("1.2.0")), 0);
        // Nonsense on the website must not throw or win.
        QCOMPARE(compareVersions(QStringLiteral("not.a.version"), QStringLiteral("0.0.1")), -1);
    }

    void aStableBuildStaysOnStable()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.3.0"), QStringLiteral("beta")),
                                        make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto update = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), feed);
        QVERIFY(update.has_value());
        QCOMPARE(update->version, QStringLiteral("0.2.0"));
    }

    void aBetaBuildTakesStableToo()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.4.0"), QStringLiteral("stable")),
                                        make(QStringLiteral("0.3.0"), QStringLiteral("beta")),
                                        make(QStringLiteral("0.9.0"), QStringLiteral("alpha"))};
        const auto update = chooseUpdate(QStringLiteral("0.2.0"), QStringLiteral("beta"), feed);
        QVERIFY(update.has_value());
        // The newest of the two it follows. Never the alpha, however high
        // its number: that is not an update, it is a downgrade in
        // finish.
        QCOMPARE(update->version, QStringLiteral("0.4.0"));
    }

    void aWithdrawnReleaseIsNeverOffered()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.3.0"), QStringLiteral("stable"), true),
                                        make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto update = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), feed);
        QVERIFY(update.has_value());
        QCOMPARE(update->version, QStringLiteral("0.2.0"));
    }

    void nothingNewerMeansNothingToSay()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        QVERIFY(!chooseUpdate(QStringLiteral("0.2.0"), QStringLiteral("stable"), feed).has_value());
        QVERIFY(!chooseUpdate(QStringLiteral("0.3.0"), QStringLiteral("stable"), feed).has_value());
    }

    void aDevelopmentBuildIsOfferedNothing()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("9.9.9"), QStringLiteral("stable"))};
        QVERIFY(channelsFor(QStringLiteral("dev")).isEmpty());
        QVERIFY(!chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("dev"), feed).has_value());
    }

    void theRunningBuildIsFoundByVersionAndChannel()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.2.0"), QStringLiteral("beta"), true),
                                        make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto beta = findRunning(QStringLiteral("0.2.0"), QStringLiteral("beta"), feed);
        QVERIFY(beta.has_value());
        QVERIFY(beta->withdrawn);
        // The same number on another channel is a different build and
        // must not inherit the other one's withdrawal.
        const auto stable = findRunning(QStringLiteral("0.2.0"), QStringLiteral("stable"), feed);
        QVERIFY(stable.has_value());
        QVERIFY(!stable->withdrawn);
    }

    void withdrawnAndNothingNewerIsStillWorthSaying()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.2.0"), QStringLiteral("stable"), true)};
        QVERIFY(!chooseUpdate(QStringLiteral("0.2.0"), QStringLiteral("stable"), feed).has_value());
        const auto running = findRunning(QStringLiteral("0.2.0"), QStringLiteral("stable"), feed);
        QVERIFY(running.has_value());
        QVERIFY(running->withdrawn);
        QCOMPARE(running->withdrawnReason, QStringLiteral("it ate a library"));
    }
};

QTEST_MAIN(UpdateDecisionTest)
#include "update_decision_test.moc"
