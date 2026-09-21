// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/controls_style.hpp"

// QByteArray explicitly, not transitively: qputenv() takes a
// QByteArrayView for its value, and nothing in <QtGlobal> provides the
// conversion from a string literal. This compiled in main.cpp only
// because its neighbours -- QGuiApplication and friends -- dragged
// QByteArray in, so the call was leaning on them rather than standing
// on its own. Moving it into a file of its own is what exposed that,
// on macOS, where the error is "no matching function for call to
// qputenv". The Windows arm has the same shape and simply is not
// compiled on any machine that has built this so far.
#include <QByteArray>
#include <QtGlobal>

namespace seabass::gui
{

void applyDefaultControlsStyle()
{
    if (!qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        return;
    }
#if defined(Q_OS_WIN)
    qputenv("QT_QUICK_CONTROLS_STYLE", "FluentWinUI3");
#elif defined(Q_OS_MACOS)
    qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
#endif
}

}  // namespace seabass::gui
