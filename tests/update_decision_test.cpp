// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the update check decides, without a network anywhere near it.
// The rules it encodes are promises: a withdrawn release is never
// offered, a release nobody has smoke-tested is never offered, a stable
// build is never sent to a test build, and a development build is offered
// nothing at all.

#include <QTest>

#include "gui/update_decision.hpp"

using namespace seabass::gui;

namespace
{
// Released by default, because that is what an entry on the website looks
// like by the time anyone is meant to see it. The one test about the gate
// clears it deliberately -- everywhere else it would only be noise, and
// leaving it false would quietly turn every other assertion here into a
// test of the gate instead of the thing it names.
ReleaseInfo make(const QString &version, const QString &channel, bool withdrawn = false,
                 const QString &build = {})
{
    ReleaseInfo release;
    release.version = version;
    release.channel = channel;
    release.build = build.isEmpty() ? channel : build;
    release.released = true;
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

    void alphaAndBetaAreBothTesting()
    {
        QCOMPARE(feedChannelFor(QStringLiteral("alpha")), QStringLiteral("testing"));
        QCOMPARE(feedChannelFor(QStringLiteral("beta")), QStringLiteral("testing"));
        QCOMPARE(feedChannelFor(QStringLiteral("stable")), QStringLiteral("stable"));
        // Not published, so it has no channel to be on.
        QVERIFY(feedChannelFor(QStringLiteral("dev")).isEmpty());
    }

    void aStableBuildStaysOnStable()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.3.0"), QStringLiteral("testing")),
                                        make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto update = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), feed);
        QVERIFY(update.has_value());
        QCOMPARE(update->version, QStringLiteral("0.2.0"));
    }

    void aTestBuildTakesStableToo()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.4.0"), QStringLiteral("stable")),
                                        make(QStringLiteral("0.3.0"), QStringLiteral("testing"))};
        // Both from an alpha and from a beta: they are one channel here,
        // and the number decides which of the two on offer is newer.
        for (const auto &running : {QStringLiteral("alpha"), QStringLiteral("beta")}) {
            const auto update = chooseUpdate(QStringLiteral("0.2.0"), running, feed);
            QVERIFY(update.has_value());
            QCOMPARE(update->version, QStringLiteral("0.4.0"));
        }
    }

    void aWithdrawnReleaseIsNeverOffered()
    {
        const QVector<ReleaseInfo> feed{make(QStringLiteral("0.3.0"), QStringLiteral("stable"), true),
                                        make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto update = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), feed);
        QVERIFY(update.has_value());
        QCOMPARE(update->version, QStringLiteral("0.2.0"));
    }

    // The packages are uploaded and the entry is written before anybody
    // has installed the build; only then is it marked released. Until it
    // is, the app must not send people to it, or a build that does not
    // start would reach everyone running the version before it.
    void aReleaseNobodyHasTestedIsNeverOffered()
    {
        ReleaseInfo fresh = make(QStringLiteral("0.3.0"), QStringLiteral("stable"));
        fresh.released = false;
        const QVector<ReleaseInfo> feed{fresh, make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto update = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), feed);
        QVERIFY(update.has_value());
        QCOMPARE(update->version, QStringLiteral("0.2.0"));

        // And it is offered the moment it is marked released, so what is
        // being tested here is the flag and not the version.
        fresh.released = true;
        const QVector<ReleaseInfo> after{fresh, make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto now = chooseUpdate(QStringLiteral("0.1.0"), QStringLiteral("stable"), after);
        QVERIFY(now.has_value());
        QCOMPARE(now->version, QStringLiteral("0.3.0"));
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

    void theRunningBuildIsFoundByVersionAndBuildChannel()
    {
        const QVector<ReleaseInfo> feed{
            make(QStringLiteral("0.2.0"), QStringLiteral("testing"), true, QStringLiteral("alpha")),
            make(QStringLiteral("0.2.0"), QStringLiteral("testing"), false, QStringLiteral("beta")),
            make(QStringLiteral("0.2.0"), QStringLiteral("stable"))};
        const auto alpha = findRunning(QStringLiteral("0.2.0"), QStringLiteral("alpha"), feed);
        QVERIFY(alpha.has_value());
        QVERIFY(alpha->withdrawn);
        // The same number built as a beta shares the testing list with
        // it, and must not inherit its withdrawal.
        const auto beta = findRunning(QStringLiteral("0.2.0"), QStringLiteral("beta"), feed);
        QVERIFY(beta.has_value());
        QVERIFY(!beta->withdrawn);
        const auto stable = findRunning(QStringLiteral("0.2.0"), QStringLiteral("stable"), feed);
        QVERIFY(stable.has_value());
        QVERIFY(!stable->withdrawn);
    }

    // Somebody handed a build to smoke-test is running an entry that is
    // not released yet, and is exactly who needs to hear it was pulled.
    void anUntestedBuildStillHearsItWasWithdrawn()
    {
        ReleaseInfo fresh = make(QStringLiteral("0.3.0"), QStringLiteral("testing"), true,
                                 QStringLiteral("alpha"));
        fresh.released = false;
        const auto running = findRunning(QStringLiteral("0.3.0"), QStringLiteral("alpha"), {fresh});
        QVERIFY(running.has_value());
        QVERIFY(running->withdrawn);
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
