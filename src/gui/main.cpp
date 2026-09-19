// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSettings>
#include <QStyleHints>
#include <QThreadPool>

#include "gui/seabass_settings.hpp"

namespace
{

// Qt Quick Controls' Material style resolves its palette from these
// environment variables exactly once, the first time a Material-styled
// item is instantiated -- and that includes every Popup-based control
// (Dialog, ComboBox's dropdown, Menu, ToolTip), which does NOT reliably
// inherit Material.theme/accent/etc set as QML attached properties on the
// ApplicationWindow (Popups attach to the window's overlay, not its item
// tree, so that inheritance chain doesn't reach them). Setting the style
// globally here, before QGuiApplication exists, is what actually makes
// every Material-styled surface in the app -- not just the ones directly
// under the window -- consistently follow either "Kelp" (this app's own
// always-dark palette) or the system's own light/dark choice.
//
// Deliberately only sets THEME + ACCENT/PRIMARY, not BACKGROUND/
// FOREGROUND -- overriding those too caused widespread dark-on-dark and
// light-on-light contrast bugs (disabled-state colors, hint text, row
// alternation and more all broke at once), because Material's style
// implementation derives several other colors *from* its own default
// background/foreground relationship, not just from the two colors
// directly. Standard Material Dark's own background/foreground are close
// enough to "Kelp" for Popups (the only place this env var actually
// matters -- everything else uses Theme.qml's exact palette directly),
// and staying internally consistent matters far more there than an exact
// color match.
//
// Must run before the QGuiApplication is constructed -- Qt Quick
// Controls reads these at first use, effectively at process startup.
// Returns the useSystemTheme setting it read, so main() can reuse it
// without opening QSettings a second time.
bool exportMaterialPalette()
{
    // Same store AppSettingsController's own QSettings resolves to, and
    // constructed the same way ("seabass", "seabass") instead of as a
    // literal path: the two-argument constructor takes the organization
    // and application names directly, so it does NOT depend on
    // organizationName/applicationName having been set up by the time
    // this runs (they haven't -- this executes before QGuiApplication
    // exists at all), which was the original reason for hardcoding a
    // path here.
    //
    // The hardcoded "~/.config/seabass/seabass.conf" it replaces was
    // correct on Linux but wrong on Windows, where QSettings' native
    // format is the registry (HKCU\Software\seabass\seabass), not an INI
    // file under $HOME. That file therefore never existed on Windows, so
    // this read always fell through to the default, pinning
    // useSystemTheme to false forever and making the Settings toggle a
    // no-op for everything driven from here.
    QSettings settings = seabass::gui::openSeabassSettings();
    const bool useSystemTheme = settings.value("useSystemTheme", false).toBool();

    // "Current"/"Abyss" -- this app's own brand colors (see Theme.qml),
    // applied regardless of theme, same as the QML-level Material.accent/
    // primary bindings in Main.qml.
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#3daee9");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#123a52");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", useSystemTheme ? "System" : "Dark");

    // Deliberately NOT setting QT_QUICK_CONTROLS_STYLE on Linux: tried
    // forcing it to "Material" (plus a matching qtquickcontrols2.conf) to
    // fix a separate, narrower issue -- ProgressBar loading KDE's own
    // "org.kde.breeze" style and throwing harmless-but-noisy TypeErrors
    // (breeze's ProgressBar.qml assumes an anchoring context Material's
    // equivalent doesn't set up) -- but forcing the style did NOT fix
    // that (KDE's platform theme integration still overrode it for
    // ProgressBar specifically) and DID visibly break every other
    // control: Buttons rendered as Material's default pill-shaped
    // outlined buttons instead of this app's flat rectangular cards.
    // Net negative, reverted. The ProgressBar warning is left as a
    // known, cosmetic-only, unresolved issue -- see project memory.
    //
    // Windows has no equivalent of KDE's platform-theme integration to
    // auto-select a native-looking style, so with QT_QUICK_CONTROLS_STYLE
    // unset it silently falls back to Qt's plain "Basic" style for every
    // standard control this app uses directly (Button, ComboBox, Dialog,
    // Menu, ProgressBar, ...) -- flat, colorless, looks nothing like the
    // Linux build. Explicitly opt into "FluentWinUI3" (Qt 6.8+, native
    // Windows 11 look, ships in MSYS2's qt6-declarative and already gets
    // bundled by deploy-windows.ps1/windeployqt) there only; Linux keeps
    // its existing KDE-driven auto-selection untouched. Respect an
    // explicit override (e.g. a developer testing a different style) by
    // only setting this if nothing already has.
    //
    // macOS's native "macOS" style refuses the background/contentItem
    // overrides this app's controls are built on (it logs "does not
    // support customization" for each and draws its own). Material takes
    // them and honours the palette exported above; Fusion was tried and
    // drew light pages from the system palette around the app's dark
    // cards. Material is also what tests/qml's screenshot mode renders.
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "FluentWinUI3");
    }
#elif defined(Q_OS_MACOS)
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
    }
#endif

    return useSystemTheme;
}

}  // namespace

int main(int argc, char **argv)
{
    const bool useSystemTheme = exportMaterialPalette();
    QGuiApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg")));

#ifdef Q_OS_LINUX
    // Qt Quick's default text renderer draws glyphs from a distance
    // field: resolution independent, but unhinted and not snapped to the
    // pixel grid, so on a fractionally scaled display every other line's
    // baseline lands mid-pixel and the bottom row of every stem is drawn
    // at partial coverage. On screen the letters look shaved off along
    // the baseline (reported on a 2256x1504 panel at Plasma's 1.5
    // scaling, Noto Sans at 11pt).
    //
    // Native rendering rasterises through FreeType instead, with the
    // desktop's own hinting and subpixel antialiasing, and snaps to whole
    // pixels -- the same thing KDE's own Qt Quick apps do, and it makes
    // Seabass's text match every other app on the desktop rather than
    // being the one that looks soft.
    //
    // Linux only for now: this is the platform the problem was seen and
    // measured on, and the distance field renderer has its own reasons to
    // stay the default elsewhere (it is what keeps text sharp under the
    // scaling and animation macOS and Windows do). Widen it when there is
    // a real screenshot from one of those to judge by, not before.
    //
    // Static, and read when a QQuickWindow is constructed, so it has to
    // run before the engine loads Main.qml.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
#endif

#ifdef Q_OS_WIN
    // FluentWinUI3 (opted into above) draws native Windows 11 controls and
    // deliberately ignores the Material attached properties, so Main.qml's
    // `Material.theme: ... : Material.Dark` -- the binding that gives the
    // Linux build its always-dark "Kelp" look -- has no effect on any of
    // them. Left alone, FluentWinUI3 simply follows the Windows system
    // colour scheme, so on a light-mode machine the entire app renders
    // light no matter what this app asked for.
    //
    // Qt 6.8+ exposes the colour scheme as a settable style hint, which is
    // what makes a native-looking style honour the app's own preference
    // rather than the OS setting. Only forced when useSystemTheme is off:
    // leaving it unset (Qt::ColorScheme::Unknown) is exactly what "follow
    // the system" means, so the true branch needs no code.
    //
    // Read once at startup, deliberately matching the palette env vars
    // above -- toggling this in Settings needs an app restart to take
    // effect here. Doing it live would mean reaching a C++ hook out of
    // AppSettingsController, which is a QML_ELEMENT instantiated by
    // Main.qml rather than something main() holds a handle to.
    if (!useSystemTheme) {
        app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    }
#endif

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("SeabassGui", "Main");

    int result = app.exec();

    // A write task is a detached QtConcurrent::run() -- it keeps running
    // on the thread pool independent of any window, so give it a real
    // chance to finish (backup + write are already-fast, small-file
    // operations; a stuck one past this timeout is not worth hanging
    // process exit over) rather than let the process tear down mid-write
    // to a stick.
    QThreadPool::globalInstance()->waitForDone(15000);
    return result;
}
