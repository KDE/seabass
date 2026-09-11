#include <QQmlContext>
#include <QQmlEngine>
#include <QDir>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <filesystem>
#include "../scratch_path.hpp"
#include <QSettings>
#include <QString>
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
        {
            const QSettings probe("seabass", "seabass");
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
    }
};

QUICK_TEST_MAIN_WITH_SETUP(SeabassGuiQmlTests, Setup)

#include "qml_test_main.moc"
