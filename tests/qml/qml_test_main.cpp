#include <QQmlContext>
#include <QQmlEngine>
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
        if (const char *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
            xdgConfigHome != nullptr && *xdgConfigHome != '\0') {
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromLocal8Bit(xdgConfigHome));
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
