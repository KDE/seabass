// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QQmlContext>
#include <QQmlEngine>
#include <QDir>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <filesystem>
#include "../scratch_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/sync_controller.hpp"
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QQuickStyle>
#include <QtQuickTest/quicktest.h>

#include <cstdlib>

// Runs every tst_*.qml file found under the directory passed via -input
// (see the add_test() call in CMakeLists.txt) against a real QQmlEngine --
// TestCase, SignalSpy, mouseClick() etc. all work exactly as they would
// driving the real app, just against QML components in isolation rather
// than the full running application.
//
// `screenshotDir` is exposed to the tests from SEABASS_SCREENSHOT_DIR:
// when set, tests that render a whole page also save it as a PNG there
// (grabImage(page).save(...)), so a layout can be looked at for real
// rather than only asserted about. Empty (the default, and under ctest)
// means no files are written.
// Fills a Sync Cue Points page's own controller with a small fixed
// analysis, so tst_SyncPage.qml can measure the page and screenshot it
// without a stick: two decisions (one side of the first carries a 0:00
// memory cue) and five tracks ready to sync, one of which already has one
// of the cues it is about to receive. The page builds its controller
// itself, so the test finds it by objectName and hands it here.
class SyncPageFixture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE bool fill(QObject *controller)
    {
        auto *sync = qobject_cast<seabass::gui::SyncController *>(controller);
        if (sync == nullptr) {
            return false;
        }
        using namespace seabass::domain;
        const auto hot = [](int pad, double seconds, const char *color) {
            CuePoint cue;
            cue.kind = CuePoint::Kind::Hot;
            cue.hotCueNumber = pad;
            cue.positionMs = seconds * 1000.0;
            cue.color = color;
            return cue;
        };
        const auto memory = [](double seconds) {
            CuePoint cue;
            cue.kind = CuePoint::Kind::Memory;
            cue.positionMs = seconds * 1000.0;
            return cue;
        };
        const auto track = [](const char *format, const char *id, const char *title, const char *artist,
                              const char *filename, double seconds, double bpm, const char *key,
                              std::vector<CuePoint> cues) {
            Track t;
            t.format = format;
            t.sourceId = id;
            t.title = title;
            t.artist = artist;
            t.filename = filename;
            // Never read: it only makes Play look the way it does for a
            // real track, which is what the screenshots are for.
            t.filePath = std::string("/nonexistent/TESTSTICK/Contents/") + filename;
            t.durationSeconds = seconds;
            t.bpm = bpm;
            t.key = key;
            t.cues = std::move(cues);
            return t;
        };
        const auto readyPlan = [](Track source, Track target) {
            SyncPlan plan;
            plan.kind = SyncPlan::Kind::AOnly;
            plan.direction = SyncPlan::Direction::ToB;
            plan.cuesToApply = source.cues;
            plan.match.trackA = std::move(source);
            plan.match.trackB = std::move(target);
            return plan;
        };
        const auto decision = [](Track a, Track b, bool junkOnB) {
            CrossSourceSyncConflict conflict;
            conflict.samePair = true;
            conflict.target = b;
            conflict.cuesFromA = a.cues;
            conflict.cuesFromB = b.cues;
            conflict.sourceBHasJunkCue = junkOnB;
            conflict.sourceA = std::move(a);
            conflict.sourceB = std::move(b);
            return conflict;
        };

        std::vector<CrossSourceSyncConflict> conflicts;
        conflicts.push_back(decision(
            track("rekordbox", "10", "Flaschenpost", "Kollektiv Turmstrasse",
                  "05_Kollektiv Turmstrasse-Flaschenpost.mp3", 432, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 120, "#ff9b1a"), hot(3, 200, "#ffe13b"), hot(4, 260, "#39d353"),
                   hot(5, 330, "#2ec4f0")}),
            track("engine", "110", "Flaschenpost", "Kollektiv Turmstrasse",
                  "05_Kollektiv Turmstrasse-Flaschenpost.mp3", 432, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 150, "#ff9b1a"), hot(3, 200, "#ffe13b"), hot(4, 300, "#39d353"),
                   memory(0)}),
            true));
        conflicts.push_back(decision(
            track("engine", "111", "Diary of a Lost Girl", "Roman Fl\u00fcgel",
                  "17_Roman Flugel-Diary of a Lost Girl.mp3", 418, 122, "5A",
                  {hot(1, 16, "#e03c3c"), hot(2, 140, "#ffe13b"), hot(3, 290, "#2ec4f0")}),
            track("rekordbox", "11", "Diary of a Lost Girl", "Roman Fl\u00fcgel",
                  "17_Roman Flugel-Diary of a Lost Girl.mp3", 418, 122, "5A",
                  {hot(1, 16, "#e03c3c"), hot(2, 132, "#ffe13b"), hot(3, 290, "#2ec4f0")}),
            false));

        std::vector<SyncPlan> plans;
        plans.push_back(readyPlan(
            track("engine", "112", "Rej", "\u00c2me", "12_Ame-Rej.mp3", 521, 124, "11B",
                  {hot(1, 20, "#e03c3c"), hot(2, 150, "#ff9b1a"), hot(3, 290, "#39d353"), hot(4, 430, "#2ec4f0")}),
            track("rekordbox", "12", "Rej", "\u00c2me", "12_Ame-Rej.mp3", 521, 124, "11B",
                  {hot(1, 20, "#e03c3c")})));
        plans.push_back(readyPlan(
            track("rekordbox", "13", "Bloom", "Nils Hoffmann", "02_Nils Hoffmann-Bloom.mp3", 389, 120, "3A",
                  {hot(1, 4, "#e03c3c"), hot(2, 64, "#ff9b1a"), hot(3, 128, "#ffe13b"), hot(4, 192, "#39d353"),
                   hot(5, 256, "#2ec4f0"), hot(6, 320, "#7b61ff"), memory(4), memory(256)}),
            track("engine", "113", "Bloom", "Nils Hoffmann", "02_Nils Hoffmann-Bloom.mp3", 389, 120, "3A", {})));
        plans.push_back(readyPlan(
            track("rekordbox", "14", "Loop In Loop", "Sven V\u00e4th", "09_Sven Vath-Loop In Loop.mp3", 468, 127,
                  "6A", {hot(1, 32, "#e03c3c"), hot(2, 180, "#ffe13b"), hot(3, 360, "#2ec4f0")}),
            track("engine", "114", "Loop In Loop", "Sven V\u00e4th", "09_Sven Vath-Loop In Loop.mp3", 468, 127,
                  "6A", {})));
        plans.push_back(readyPlan(
            track("onelibrary", "15", "Sisters", "Recondite", "21_Recondite-Sisters.mp3", 402, 123, "9A",
                  {hot(1, 12, "#e03c3c"), hot(2, 90, "#ff9b1a"), hot(3, 180, "#ffe13b"), hot(4, 270, "#39d353"),
                   hot(5, 350, "#2ec4f0"), memory(12)}),
            track("engine", "115", "Sisters", "Recondite", "21_Recondite-Sisters.mp3", 402, 123, "9A", {})));
        plans.push_back(readyPlan(
            track("rekordbox", "16", "Flaschenpost (Edit)", "Kollektiv Turmstrasse",
                  "33_Kollektiv Turmstrasse-Flaschenpost Edit.mp3", 245, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 120, "#ff9b1a")}),
            track("engine", "116", "Flaschenpost (Edit)", "Kollektiv Turmstrasse",
                  "33_Kollektiv Turmstrasse-Flaschenpost Edit.mp3", 245, 124, "8A", {})));

        sync->plansModel()->setAnalysis(std::move(plans), std::move(conflicts));
        return true;
    }
};

class Setup : public QObject
{
    Q_OBJECT
public slots:
    // Screenshot mode only: the app runs under the desktop's own Qt Quick
    // style with Material's dark palette exported for popups (see
    // gui/main.cpp); offscreen there is no desktop, so without this the
    // pages render in the light Basic style over Theme.qml's dark palette
    // and every contrast judgement is wrong. Material Dark is the closest
    // stand-in that needs no platform theme. The plain test run (no
    // screenshot dir) is left exactly as it was.
    void applicationAvailable()
    {
        // AppSettingsController, main.cpp's exportMaterialPalette() and
        // media_controller.cpp's opened-folders store all construct their
        // QSettings the same way -- QSettings("seabass", "seabass"),
        // i.e. QSettings::defaultFormat() at QSettings::UserScope -- and
        // several QML tests build the real controller rather than a fake
        // one (see tst_AppSettingsPage.qml's own comment). CMakeLists.txt
        // sets XDG_CONFIG_HOME to a build-local directory specifically so
        // those tests read and write there instead of the developer's
        // real settings, but that redirection is a Linux/XDG convention:
        // QSettings::NativeFormat (the default) ignores XDG_CONFIG_HOME
        // entirely on Windows and always resolves to the registry
        // (HKCU\Software\seabass\seabass) regardless of it. On Windows
        // this comment's whole reason for existing silently did nothing
        // -- every run of this binary wrote real test fixture values
        // (a fake stickBackupDirectory among them) straight into the
        // real registry, which the real app then read back as if a user
        // had set them. Forcing IniFormat and pointing UserScope at the
        // same XDG_CONFIG_HOME directory makes the redirect actually
        // apply, identically, on every platform -- the registry (or
        // equivalent) is never touched by a test run again.
        //
        // Unconditional, and it trusts nothing it was handed. The
        // redirect used to happen only when XDG_CONFIG_HOME was ALREADY
        // set -- true under ctest, which sets it, and false for the
        // command docs/testing.md tells you to run:
        //
        //     SEABASS_SCREENSHOT_DIR=<dir> QT_QPA_PLATFORM=offscreen \
        //         build/seabass_qml_tests -input tests/qml
        //
        // Plasma does not export XDG_CONFIG_HOME (it is a default, not a
        // setting), so on a normal KDE desktop that guard fell straight
        // through and every direct run wrote the suite's own fixtures
        // into the real ~/.config/seabass/seabass.conf -- including
        // tst_AppSettingsPage's fake "/home/somebody/Music/..." backup
        // directory, which the app then read back and showed as the
        // user's own choice.
        //
        // Honouring the variable when it IS set would leave the same
        // hole open from the other side: plenty of setups export
        // XDG_CONFIG_HOME="$HOME/.config" from a dotfile or an
        // environment.d drop-in, and then the "sandbox" is the real
        // store and every check below passes while the fixtures land in
        // it. So this makes its own directory every run and points the
        // platform's own variables at it -- via the same helpers the C++
        // tests use, which also covers APPDATA for Windows, where the
        // native store is the registry and no XDG variable is read.
        //
        // SEABASS_HOME too: sandboxSeabassHome() leaves an inherited one
        // alone (ctest sets one per test), but on a direct run there is
        // none, and AppSettingsController would otherwise point the
        // local root at the developer's real ~/Seabass -- which is what
        // tst_StickListPage's home-backup probes would then be reading.
        static QTemporaryDir sandbox;
        if (!sandbox.isValid()) {
            qCritical("seabass_qml_tests: could not create a settings sandbox (%s) -- refusing "
                      "to run rather than fall back to the real store.",
                      qPrintable(sandbox.errorString()));
            std::abort();
        }
        const std::filesystem::path sandboxRoot(sandbox.path().toStdString());
        // Named "Seabass" rather than "home": SEABASS_HOME stands in for
        // the real ~/Seabass, and pages that show the user where they
        // write show this path. tst_MetadataBackupPage asserts the label
        // names a Seabass location, which under ctest passed only
        // because the build directory happens to sit under ~/Seabass --
        // an accident this would otherwise have turned into a failure.
        seabass::testing::sandboxSeabassHome(sandboxRoot / "Seabass");
        seabass::testing::sandboxSettings(sandboxRoot / "config");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QString::fromStdString((sandboxRoot / "config").string()));

        // And proof -- against the real store, computed independently,
        // rather than against the string just handed to setPath(), which
        // would agree with itself whatever it pointed at. What has to be
        // true is that nothing lands under the user's own config
        // directory; the sandbox lives in the temp tree, so it cannot.
        //
        // Probed via openSeabassSettings(), the same call production
        // code makes: a bare QSettings("seabass","seabass") here would
        // check a different, uninteresting thing on Windows, where that
        // two-argument constructor keeps resolving to NativeFormat (the
        // registry) regardless of the setDefaultFormat()/setPath() calls
        // above -- confirmed directly, not merely suspected -- so the
        // probe would find nothing wrong while AppSettingsController's
        // real four-argument-constructed QSettings still opened the
        // registry underneath it.
        {
            QSettings probe = seabass::gui::openSeabassSettings();
            const QString realConfigRoot = QDir::homePath() + QStringLiteral("/.config");
            if (probe.fileName().startsWith(realConfigRoot)
                || !probe.fileName().startsWith(sandbox.path())) {
                qCritical("seabass_qml_tests: settings would be written to %s, outside the "
                          "sandbox at %s -- refusing to run rather than touch the real store.",
                          qPrintable(probe.fileName()), qPrintable(sandbox.path()));
                std::abort();
            }
        }

        const char *dir = std::getenv("SEABASS_SCREENSHOT_DIR");
        if (dir == nullptr || *dir == '\0') {
            return;
        }
        if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE")) {
            qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
        }
        qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#3daee9");
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#123a52");
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        const char *dir = std::getenv("SEABASS_SCREENSHOT_DIR");
        engine->rootContext()->setContextProperty(QStringLiteral("screenshotDir"),
                                                  dir != nullptr ? QString::fromLocal8Bit(dir) : QString());
        // Whether this run ended up under a Qt Quick Controls style other
        // than "Basic", the one the pixel-measuring tests are calibrated
        // against: the screenshot mode forces Material, a run under the
        // desktop's own style forces that, and -- the case an env-var
        // check alone missed -- a plain ctest run on Windows still lands
        // on "Windows" with nothing forced at all, because that style is
        // this platform's native default rather than something QML asks
        // for. Comparing the resolved name instead of the env var catches
        // every one of those the same way.
        engine->rootContext()->setContextProperty(QStringLiteral("controlsStyleForced"),
                                                  QQuickStyle::name() != QStringLiteral("Basic"));
        // tests/qml-live/: the mount point of a real (scratch) stick to
        // drive the real pages and controllers against. Empty under
        // ctest, and every live test skips itself then.
        const char *stick = std::getenv("SEABASS_LIVE_STICK");
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickRoot"),
                                                  stick != nullptr ? QString::fromLocal8Bit(stick) : QString());
        // Which of the orchestrated live scenarios this run is (see
        // tests/qml-live/run-live.sh); each file skips itself otherwise.
        engine->rootContext()->setContextProperty(QStringLiteral("liveLockPlanted"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_LOCKED"));
        engine->rootContext()->setContextProperty(QStringLiteral("liveGuardRun"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_GUARD"));
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickPullRun"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_STICK_PULL"));
        // tools/rig-clones.sh: where tst_LiveEditMode::test_11_rigKeepCue
        // adds a memory cue it keeps, in ms (0: the test skips itself).
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigKeepCueMs"),
                                                  qEnvironmentVariableIntValue("SEABASS_RIG_KEEP_CUE_MS"));
        // tools/rig-edits.sh, after tools/rig_plant_repairable planted an
        // issue: test_10 must find something to repair. A skip exits 0, so
        // without this a plant Library Health did not see still read PASS.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigRequireRepairable"),
                                                  qEnvironmentVariableIsSet("SEABASS_RIG_REQUIRE_REPAIRABLE"));
        // A second (scratch) stick, for the flows that read one stick and
        // write another -- metadata backed up from it, restored onto
        // liveStickRoot. Empty: those tests skip themselves.
        const char *secondStick = std::getenv("SEABASS_LIVE_SECOND_STICK");
        engine->rootContext()->setContextProperty(QStringLiteral("liveSecondStickRoot"),
                                                  secondStick != nullptr ? QString::fromLocal8Bit(secondStick) : QString());
        // The Breeze icons compiled into this binary, by name, read from
        // the resources rather than listed: tst_SeabassIcon loads each one
        // through Theme.iconUrl(), so a file registered under a different
        // path than the one QML asks for fails there.
        QStringList bundledIcons;
        for (const QString &file : QDir(QStringLiteral(":/qt/qml/SeabassGui/qml/icons/breeze"))
                                       .entryList({QStringLiteral("*.svg")}, QDir::Files)) {
            bundledIcons.append(file.chopped(4));
        }
        engine->rootContext()->setContextProperty(QStringLiteral("bundledIcons"), bundledIcons);
        engine->rootContext()->setContextProperty(QStringLiteral("syncPageFixture"), new SyncPageFixture(engine));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(SeabassGuiQmlTests, Setup)

#include "qml_test_main.moc"
