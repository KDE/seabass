// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// UpdateChecker end to end, minus the website: a releases.json written
// into the scratch tree and fetched through file://, by the checker's own
// QNetworkAccessManager, so the reply handling, the parsing and the
// decision all run exactly as they do against vizzzion.org.
//
// What to offer given a list of releases is update_decision_test's
// business. This is about what the checker does with the bytes: which
// entries it keeps, what it says, what it remembers, and that a feed it
// cannot read is reported rather than acted on. The running build is
// named per case (RunningBuild), because the test binary itself is a
// development build from a working tree, which rightly never fetches
// anything.

#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/update_checker.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::gui::pathToQString;
using seabass::gui::RunningBuild;
using seabass::gui::UpdateChecker;

namespace
{

fs::path g_scratch;
int g_feedCount = 0;

// Every case starts from a machine that has never checked: the checker
// reads its settings in the constructor.
void forgetSettings()
{
    QSettings settings = seabass::gui::openSeabassSettings();
    settings.clear();
    settings.sync();
}

// Writes body as a feed file of its own and returns its file:// URL.
QString feed(const std::string &body)
{
    const fs::path file = g_scratch / "feeds" / ("releases-" + std::to_string(++g_feedCount) + ".json");
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << body;
    out.close();
    assert(out.good());
    return QUrl::fromLocalFile(pathToQString(file)).toString();
}

// One entry of releases.json, shaped as the website writes it.
std::string entry(const std::string &version, const std::string &build, bool released, const std::string &extra = {})
{
    return R"({"version": ")" + version + R"(", "build": ")" + build + R"(", "date": "2026-09-26", "released": )"
        + (released ? "true" : "false") + extra + "}";
}

std::string channels(const std::string &stable, const std::string &testing)
{
    return R"({"updated": "2026-09-26T05:34:27+02:00", "channels": {"stable": [)" + stable + R"(], "testing": [)"
        + testing + "]}}";
}

// Runs one check against the feed at url and waits for its answer.
void check(UpdateChecker &checker, const QString &url)
{
    checker.setFeedUrl(url);
    checker.checkNow();
    assert(checker.state() == QStringLiteral("checking"));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&checker, &UpdateChecker::stateChanged, &loop, [&] {
        if (checker.state() != QStringLiteral("checking")) {
            loop.quit();
        }
    });
    timeout.start(10000);
    loop.exec();
    assert(checker.state() != QStringLiteral("checking") && "the check never finished");
}

RunningBuild build(const char *version, const char *channel, const char *commit = "", bool published = false)
{
    return RunningBuild{QString::fromLatin1(version), QString::fromLatin1(channel), QString::fromLatin1(commit),
                        published};
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    g_scratch = seabass::testing::scratchRoot() / "update-checker-test";
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);
    // Both belts, as open_folder_test explains: the variable for Linux,
    // Ini plus a path for Windows. Before the first QSettings exists.
    seabass::testing::sandboxSettings(g_scratch / "config");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, pathToQString(g_scratch / "config"));
    {
        QSettings probe = seabass::gui::openSeabassSettings();
        assert(probe.fileName().toStdString().rfind(seabass::pathToGenericUtf8(g_scratch / "config"), 0) == 0
               && "QSettings must resolve inside the test's scratch tree");
    }

    // The checker the app constructs speaks for this binary, and says so.
    {
        forgetSettings();
        UpdateChecker checker;
        assert(checker.currentVersion() == RunningBuild::thisBuild().version);
        assert(checker.currentChannel() == RunningBuild::thisBuild().channel);
        assert(checker.downloadPage() == QStringLiteral("https://vizzzion.org/seabass/get-it.html"));
        assert(checker.state() == QStringLiteral("idle"));
        assert(checker.message() == QStringLiteral("Not checked yet."));
        assert(!checker.automatic());
        // The suite is configured without SEABASS_PUBLISHED_BUILD.
        assert(!RunningBuild::thisBuild().published);
        std::cout << "case 1 (the default checker is this build, idle, off) OK\n";
    }

    // A development build from a working tree is no published version:
    // no request, a message naming the commit (or the version where there
    // is none), and nothing written to this machine's settings, which the
    // installed Seabass shares.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.8", "dev", "1a391f9c-dirty"));
        assert(!checker.runningPreRelease());
        assert(!checker.includeTesting() && !checker.testingOptionRevealed());
        checker.setFeedUrl(feed(channels(entry("9.9.9", "stable", true), entry("9.9.9", "beta", true))));
        checker.checkNow();
        assert(checker.state() == QStringLiteral("notARelease"));
        assert(checker.message()
               == QStringLiteral("This is a development build (1a391f9c-dirty), not a released version, so there is "
                                 "nothing to compare it with."));
        assert(!checker.lastChecked().isValid() && "nothing was fetched, so nothing was checked");
        UpdateChecker bare(build("0.8.8", "dev"));
        bare.checkNow();
        assert(bare.message().contains(QStringLiteral("development build (0.8.8)")));
        // Ten taps switch nothing on: it is offered nothing whatever the
        // setting says.
        for (int i = 0; i < 12; ++i) {
            assert(!bare.versionTapped());
        }
        assert(!bare.includeTesting() && !bare.testingOptionRevealed());
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(settings.allKeys().isEmpty() && "a working-tree build writes no setting at all");
        }
        std::cout << "case 2a (a working-tree dev build never fetches and writes nothing) OK\n";
    }

    // A published development build checks like a test build: the signed
    // packages on the website come from a train branch's pipeline, which
    // builds them as dev, and the people running them are testers.
    // Running one opts the machine into test builds for good, like a beta
    // does.
    {
        forgetSettings();
        UpdateChecker checker(build("0.7.15", "dev", "v0.7.15-3-g1a391f9c", true));
        assert(checker.runningPreRelease());
        assert(checker.includeTesting() && checker.testingOptionRevealed());
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(settings.value(QStringLiteral("updates/includeTesting")).toBool()
                   && "written at once, so the stable a tester moves to still hears of the next test build");
        }
        checker.setIncludeTesting(false);
        assert(checker.includeTesting());
        // Ten taps switch nothing on and claim nothing: it follows test
        // builds already.
        for (int i = 0; i < 12; ++i) {
            assert(!checker.versionTapped());
        }

        // The website lists the dev packages under the build they stand
        // in for, so its own number, as a beta, is not an update; nor is
        // an untested or a pulled 0.7.16.
        const std::string pulled = R"(, "withdrawn": true, "withdrawnReason": "It can lose hot cues.")";
        check(checker, feed(channels(entry("0.7.14", "stable", true),
                                     entry("0.7.16", "beta", false) + ", " + entry("0.7.16", "beta", true, pulled)
                                         + ", " + entry("0.7.15", "beta", true))));
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(checker.lastChecked().isValid() && "it fetched");
        assert(checker.message() == QStringLiteral("Seabass 0.7.15 is the newest release on any channel."));

        // A released test build is offered, and a newer stable over it.
        check(checker, feed(channels("", entry("0.7.16", "beta", true))));
        assert(checker.state() == QStringLiteral("updateAvailable"));
        assert(checker.message() == QStringLiteral("Seabass 0.7.16 (beta) is available. You have 0.7.15."));
        check(checker, feed(channels(entry("0.8.0", "stable", true), entry("0.7.16", "beta", true))));
        assert(checker.latestVersion() == QStringLiteral("0.8.0"));

        // Its own number withdrawn: it is told, found by number alone.
        check(checker, feed(channels("", entry("0.7.15", "beta", true, pulled))));
        assert(checker.state() == QStringLiteral("withdrawn"));
        assert(checker.runningWithdrawnReason() == QStringLiteral("It can lose hot cues."));
        // But not by a stable of the same number: that is another build.
        check(checker, feed(channels(entry("0.7.15", "stable", true, pulled), "")));
        assert(!checker.runningWithdrawn());
        assert(checker.state() == QStringLiteral("upToDate"));

        // And the stable a tester moves to still hears of the next one.
        UpdateChecker after(build("0.8.0", "stable"));
        assert(after.includeTesting() && after.testingOptionRevealed());
        check(after, feed(channels(entry("0.8.0", "stable", true), entry("0.8.9", "alpha", true))));
        assert(after.latestVersion() == QStringLiteral("0.8.9"));
        std::cout << "case 2 (a published dev build checks like a test build) OK\n";
    }

    // A channel nothing knows is offered nothing, and changes nothing.
    {
        forgetSettings();
        UpdateChecker checker(build("0.7.15", "nightly"));
        assert(!checker.runningPreRelease());
        assert(!checker.includeTesting() && !checker.testingOptionRevealed());
        check(checker, feed(channels(entry("9.9.9", "stable", true), entry("9.9.9", "beta", true))));
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(checker.latestVersion().isEmpty());
        for (int i = 0; i < 12; ++i) {
            assert(!checker.versionTapped());
        }
        assert(!checker.includeTesting());
        std::cout << "case 2b (an unknown channel is offered nothing) OK\n";
    }

    // A stable build is offered the newer stable, and 0.7.10 is newer
    // than 0.7.9: numbers, not strings.
    {
        forgetSettings();
        UpdateChecker checker(build("0.7.9", "stable"));
        assert(!checker.runningPreRelease());
        assert(!checker.includeTesting() && !checker.testingOptionRevealed());
        check(checker, feed(channels(entry("0.7.10", "stable", true, R"(, "note": "Fixes lost cues.", "noteLevel": "warning")"),
                                     entry("0.8.1", "alpha", true))));
        assert(checker.state() == QStringLiteral("updateAvailable"));
        assert(checker.updateAvailable());
        assert(checker.latestVersion() == QStringLiteral("0.7.10"));
        assert(checker.latestChannel() == QStringLiteral("stable"));
        assert(checker.latestNote() == QStringLiteral("Fixes lost cues."));
        assert(checker.latestNoteLevel() == QStringLiteral("warning"));
        assert(checker.message()
               == QStringLiteral("Seabass 0.7.10 (stable) is available. You have 0.7.9. Fixes lost cues."));
        assert(checker.lastChecked().isValid());
        assert(!checker.runningWithdrawn());
        std::cout << "case 3 (a newer stable is offered; 0.7.10 > 0.7.9) OK\n";
    }

    // Equal and older are no update, and a stable build without the
    // opt-in says it has the newest stable.
    for (const char *offered : {"0.8.0", "0.7.12"}) {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        check(checker, feed(channels(entry(offered, "stable", true), "")));
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(!checker.updateAvailable());
        assert(checker.latestVersion().isEmpty());
        assert(checker.message() == QStringLiteral("Seabass 0.8.0 is the newest stable release."));
    }
    std::cout << "case 4 (equal and older are not offered) OK\n";

    // The testing opt-in: off, a newer alpha is not an update for a
    // stable build; ticking it re-decides from the feed already fetched,
    // with no second request; unticking goes back.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        const QString url = feed(channels(entry("0.8.0", "stable", true), entry("0.8.9", "alpha", true)));
        check(checker, url);
        assert(checker.state() == QStringLiteral("upToDate"));
        const QDateTime firstCheck = checker.lastChecked();

        // Pointed at a feed that no longer exists: had the change fetched
        // again, it would have failed.
        checker.setFeedUrl(QUrl::fromLocalFile(pathToQString(g_scratch / "gone.json")).toString());
        checker.setIncludeTesting(true);
        assert(checker.includeTesting() && checker.testingOptionRevealed());
        assert(checker.state() == QStringLiteral("updateAvailable") && "decided at once, not after a fetch");
        assert(checker.latestVersion() == QStringLiteral("0.8.9"));
        assert(checker.latestChannel() == QStringLiteral("testing"));
        // What the package calls itself, not the list it is on.
        assert(checker.message() == QStringLiteral("Seabass 0.8.9 (alpha) is available. You have 0.8.0."));
        assert(checker.lastChecked() == firstCheck);
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(settings.value(QStringLiteral("updates/includeTesting")).toBool());
            assert(settings.value(QStringLiteral("updates/testingOptionRevealed")).toBool());
        }

        checker.setIncludeTesting(false);
        assert(!checker.includeTesting());
        assert(checker.testingOptionRevealed() && "once found, the option stays in view");
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(checker.message() == QStringLiteral("Seabass 0.8.0 is the newest stable release."));

        // Same value again: nothing to change, nothing announced.
        int changes = 0;
        QObject::connect(&checker, &UpdateChecker::includeTestingChanged, [&] { ++changes; });
        checker.setIncludeTesting(false);
        assert(changes == 0);

        // With testing on and still nothing newer, the answer covers
        // every channel.
        forgetSettings();
        UpdateChecker level(build("0.8.9", "stable"));
        level.setIncludeTesting(true);
        check(level, url);
        assert(level.state() == QStringLiteral("upToDate"));
        assert(level.message() == QStringLiteral("Seabass 0.8.9 is the newest release on any channel."));
        std::cout << "case 5 (the testing opt-in, decided without a second fetch) OK\n";
    }

    // Not released yet, or no "released" field at all: never offered,
    // whatever the number.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        check(checker, feed(channels(entry("0.9.0", "stable", false) + R"(, {"version": "0.9.1", "build": "stable"})",
                                     "")));
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(checker.latestVersion().isEmpty());
        std::cout << "case 6 (released: false and a missing flag are never offered) OK\n";
    }

    // A pre-release build follows everything: it turns the opt-in on for
    // good, the box cannot turn it off, and a newer stable is an update
    // for it as much as a newer alpha.
    {
        forgetSettings();
        UpdateChecker checker(build("0.7.10", "beta"));
        assert(checker.runningPreRelease());
        assert(checker.includeTesting() && checker.testingOptionRevealed());
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(settings.value(QStringLiteral("updates/includeTesting")).toBool()
                   && "written at once, so the stable this beta becomes still hears of the next alpha");
        }
        checker.setIncludeTesting(false);
        assert(checker.includeTesting());
        checker.forgetTestingChoice();
        assert(checker.includeTesting() && checker.testingOptionRevealed());
        check(checker, feed(channels(entry("0.8.0", "stable", true), entry("0.7.12", "alpha", true))));
        assert(checker.state() == QStringLiteral("updateAvailable"));
        assert(checker.latestVersion() == QStringLiteral("0.8.0"));
        assert(checker.message() == QStringLiteral("Seabass 0.8.0 (stable) is available. You have 0.7.10."));

        // And the stable it became still hears about the next alpha.
        UpdateChecker after(build("0.8.0", "stable"));
        assert(after.includeTesting() && after.testingOptionRevealed());
        check(after, feed(channels(entry("0.8.0", "stable", true), entry("0.8.9", "alpha", true))));
        assert(after.latestVersion() == QStringLiteral("0.8.9"));
        // Until the choice is forgotten: back to a fresh stable install.
        after.forgetTestingChoice();
        assert(!after.includeTesting() && !after.testingOptionRevealed());
        assert(after.state() == QStringLiteral("upToDate"));
        std::cout << "case 7 (a pre-release build follows every channel, for good) OK\n";
    }

    // The running build withdrawn: said even with nothing newer, and
    // carried into the message when there is something newer. An alpha of
    // the same number does not pass its withdrawal to the beta.
    {
        forgetSettings();
        const std::string pulled = R"(, "withdrawn": true, "withdrawnReason": "It can lose hot cues.")";
        UpdateChecker checker(build("0.7.11", "alpha"));
        check(checker, feed(channels("", entry("0.7.11", "alpha", true, pulled))));
        assert(checker.state() == QStringLiteral("withdrawn"));
        assert(checker.runningWithdrawn());
        assert(checker.runningWithdrawnReason() == QStringLiteral("It can lose hot cues."));
        assert(!checker.updateAvailable());
        assert(checker.message()
               == QStringLiteral("Your version has been withdrawn: It can lose hot cues. There is no newer release yet."));

        check(checker, feed(channels("", entry("0.7.12", "alpha", true, R"(, "note": "Try it on a spare stick.")") + ", "
                                             + entry("0.7.11", "alpha", true, pulled))));
        assert(checker.state() == QStringLiteral("updateAvailable"));
        assert(checker.runningWithdrawn());
        // The withdrawal, not the new release's note: that is the thing
        // to say.
        assert(checker.message()
               == QStringLiteral("Seabass 0.7.12 (alpha) is available. You have 0.7.11. Your version has been withdrawn: "
                                 "It can lose hot cues."));

        UpdateChecker beta(build("0.7.11", "beta"));
        check(beta, feed(channels("", entry("0.7.11", "alpha", true, pulled))));
        assert(!beta.runningWithdrawn());
        assert(beta.state() == QStringLiteral("upToDate"));
        assert(beta.message() == QStringLiteral("Seabass 0.7.11 is the newest release on any channel."));
        std::cout << "case 8 (a withdrawn running build is told so) OK\n";
    }

    // Feeds it cannot use: refused and reported, the last good answer not
    // replaced by a guess, and nothing crashes.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        check(checker, feed("{\"channels\": {\"stable\": [ {\"version\": "));
        assert(checker.state() == QStringLiteral("failed"));
        assert(checker.message().startsWith(QStringLiteral("Could not check: ")));
        assert(checker.message().size() > QStringLiteral("Could not check: ").size() && "says what was wrong");
        assert(!checker.lastChecked().isValid() && "a feed that did not parse is not a check");

        check(checker, feed(R"([{"version": "9.9.9", "released": true}])"));
        assert(checker.state() == QStringLiteral("failed") && "JSON, but not the object the feed is");

        // Well formed but missing what matters: no channels at all, a
        // channel that is not a list, entries that are not objects or
        // have no version. Nothing offered, nothing crashed.
        check(checker, feed(R"({"updated": "2026-09-26"})"));
        assert(checker.state() == QStringLiteral("upToDate"));
        check(checker, feed(R"({"channels": {"stable": "0.9.0", "testing": [42, "0.9.1", {"build": "alpha", "released": true}, {"version": "", "released": true}]}})"));
        assert(checker.state() == QStringLiteral("upToDate"));
        assert(checker.latestVersion().isEmpty());
        assert(checker.lastChecked().isValid());

        check(checker, QUrl::fromLocalFile(pathToQString(g_scratch / "no-such-feed.json")).toString());
        assert(checker.state() == QStringLiteral("failed"));
        assert(checker.message().contains(QStringLiteral("no-such-feed.json")) && "the reply's own error");

        // A failed check is not re-decided when the opt-in changes, even
        // with an older feed in hand: the error is the answer the last
        // check gave, and a guess from the older feed would hide it.
        checker.setIncludeTesting(true);
        assert(checker.includeTesting());
        assert(checker.state() == QStringLiteral("failed"));
        std::cout << "case 9 (malformed and incomplete feeds are refused without a crash) OK\n";
    }

    // One check at a time: a second press while one is running starts
    // nothing and announces nothing.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        checker.setFeedUrl(feed(channels(entry("0.8.1", "stable", true), "")));
        int changes = 0;
        QObject::connect(&checker, &UpdateChecker::stateChanged, [&] { ++changes; });
        checker.checkNow();
        assert(changes == 1 && checker.state() == QStringLiteral("checking"));
        assert(checker.message() == QStringLiteral("Checking…"));
        checker.checkNow();
        assert(changes == 1);
        // Also no re-decide on a settings change mid-check: it would end
        // "checking" before the reply.
        checker.setIncludeTesting(true);
        assert(checker.state() == QStringLiteral("checking"));
        QEventLoop loop;
        QObject::connect(&checker, &UpdateChecker::stateChanged, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        assert(checker.state() == QStringLiteral("updateAvailable"));
        assert(changes == 2 && "one answer for the one request");
        std::cout << "case 10 (a check in progress is not started twice) OK\n";
    }

    // Automatic checks: off by default and silent; on, it checks at
    // once, remembers when, and at the next start is not due for a day.
    {
        forgetSettings();
        {
            UpdateChecker checker(build("0.8.0", "stable"));
            checker.setFeedUrl(feed(channels(entry("0.8.1", "stable", true), "")));
            checker.checkIfDue();
            assert(checker.state() == QStringLiteral("idle") && "off means no request at all");

            int toggles = 0;
            QObject::connect(&checker, &UpdateChecker::automaticChanged, [&] { ++toggles; });
            checker.setAutomatic(true);
            assert(checker.automatic() && toggles == 1);
            assert(checker.state() == QStringLiteral("checking") && "turned on means checked now");
            QEventLoop loop;
            QObject::connect(&checker, &UpdateChecker::stateChanged, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            assert(checker.state() == QStringLiteral("updateAvailable"));
            checker.setAutomatic(true);
            assert(toggles == 1 && "the same value is no change");
        }
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(settings.value(QStringLiteral("updates/automatic")).toBool());
            assert(settings.value(QStringLiteral("updates/lastChecked")).toDateTime().isValid());
        }
        {
            // Restarted within the day: remembered on, and not due.
            UpdateChecker checker(build("0.8.0", "stable"));
            assert(checker.automatic());
            assert(checker.lastChecked().isValid());
            checker.setFeedUrl(feed(channels(entry("0.8.1", "stable", true), "")));
            checker.checkIfDue();
            assert(checker.state() == QStringLiteral("idle"));
            assert(checker.message().startsWith(QStringLiteral("Last checked ")));
            checker.setAutomatic(false);
            assert(!checker.automatic());
        }
        {
            QSettings settings = seabass::gui::openSeabassSettings();
            assert(!settings.value(QStringLiteral("updates/automatic")).toBool());
            // A day and more ago: due again.
            settings.setValue(QStringLiteral("updates/automatic"), true);
            settings.setValue(QStringLiteral("updates/lastChecked"), QDateTime::currentDateTimeUtc().addDays(-2));
        }
        {
            UpdateChecker checker(build("0.8.0", "stable"));
            checker.setFeedUrl(feed(channels(entry("0.8.1", "stable", true), "")));
            checker.checkIfDue();
            assert(checker.state() == QStringLiteral("checking") && "a day has passed");
            QEventLoop loop;
            QObject::connect(&checker, &UpdateChecker::stateChanged, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            assert(checker.state() == QStringLiteral("updateAvailable"));
        }
        std::cout << "case 11 (automatic checks: off is silent, on is daily) OK\n";
    }

    // The hidden switch on a stable build: the tenth quick tap turns the
    // opt-in on and says so once; more taps claim nothing.
    {
        forgetSettings();
        UpdateChecker checker(build("0.8.0", "stable"));
        int changes = 0;
        QObject::connect(&checker, &UpdateChecker::includeTestingChanged, [&] { ++changes; });
        for (int i = 0; i < 9; ++i) {
            assert(!checker.versionTapped());
        }
        assert(!checker.includeTesting() && changes == 0);
        assert(checker.versionTapped());
        assert(checker.includeTesting() && checker.testingOptionRevealed() && changes == 1);
        for (int i = 0; i < 10; ++i) {
            assert(!checker.versionTapped() && "already following test builds");
        }
        checker.forgetTestingChoice();
        assert(!checker.includeTesting() && !checker.testingOptionRevealed() && changes == 2);
        std::cout << "case 12 (ten taps on a stable build reveal testing) OK\n";
    }

    std::cout << "update_checker_test: all cases passed\n";
    return 0;
}
