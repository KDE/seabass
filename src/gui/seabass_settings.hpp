#pragma once

#include <QSettings>
#include <QString>

namespace seabass::gui
{

// The one place "seabass"/"seabass" QSettings gets constructed, used by
// AppSettingsController, main.cpp's exportMaterialPalette() and
// MediaController's opened-folders store alike -- and by the QML and
// open_folder tests that sandbox it, so a test's redirect actually
// reaches every one of them the same way.
//
// QSettings(organization, application) -- the two-argument constructor
// every one of those used before this existed -- is documented to fall
// back to QSettings::defaultFormat() when no format is given. On this
// Qt6/Windows build it does not: measured directly (a small standalone
// probe, both before and after calling QSettings::setDefaultFormat(
// IniFormat)), that constructor's QSettings::format() reads back
// NativeFormat regardless, and QSettings::fileName() keeps resolving to
// the registry (\HKEY_CURRENT_USER\Software\seabass\seabass) even with
// defaultFormat() and setPath() both pointed elsewhere first. The
// four-argument constructor, given QSettings::defaultFormat() explicitly
// rather than left to resolve it internally, does honour that -- same
// probe, only the constructor call changed, correctly resolves under
// setPath()'s redirected directory. Whatever the four-argument overload
// does differently isn't established here; the difference in behaviour
// is.
//
// A caller that never redirects the format (every real run of the app)
// gets QSettings::defaultFormat()'s untouched value, NativeFormat, which
// is exactly what the two-argument constructor would have produced --
// this changes nothing about where a real user's settings live.
inline QSettings openSeabassSettings(QObject *parent = nullptr)
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("seabass"),
                      QStringLiteral("seabass"), parent);
}

}  // namespace seabass::gui
