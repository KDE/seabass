// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Qt's incremental JavaScript collector frees live objects on 6.8.0 to
// 6.8.8, 6.9, 6.10 and 6.11.0 to 6.11.1 (QTBUG-138621, QTBUG-134687; the
// storm lane crashed on it in Chunk::sweep and in a QQmlType destructor,
// 7 runs in 10 on the KDE desktop style, 0 in 11 with the collector
// running each step whole). This pins which versions the app treats as
// affected and that the switch is thrown exactly then, before any engine
// exists, and never over a value somebody set themselves.

#include <QVersionNumber>
#include <QtGlobal>

#include <iostream>
#include <string>

#include "gui/qt_workarounds.hpp"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void affected(int major, int minor, int patch, bool expected)
{
    const QVersionNumber v(major, minor, patch);
    check(seabass::gui::jsCollectorFreesLiveObjects(v) == expected,
          "Qt " + v.toString().toStdString() + (expected ? " frees live objects" : " is fine"));
}

}  // namespace

int main()
{
    // Before the incremental collector: nothing to work around.
    affected(6, 7, 3, false);
    // The incremental collector, before the fix.
    affected(6, 8, 0, true);
    affected(6, 8, 3, true);   // the macOS package's Qt
    affected(6, 8, 8, true);
    affected(6, 8, 9, false);  // the fix in the 6.8 line
    affected(6, 9, 2, true);
    affected(6, 10, 2, true);  // Ubuntu 26.04
    affected(6, 11, 0, true);
    affected(6, 11, 1, true);  // KDE neon
    affected(6, 11, 2, false);
    affected(6, 12, 0, false); // the Craft packages' Qt
    affected(7, 0, 0, false);

    // The switch follows the running Qt, and only when nobody set it.
    const QVersionNumber running = QVersionNumber::fromString(QLatin1StringView(qVersion()));
    const bool expectedToAct = seabass::gui::jsCollectorFreesLiveObjects(running);
    qunsetenv("QV4_GC_TIMELIMIT");
    const bool acted = seabass::gui::keepJsCollectorInOneStep();
    check(acted == expectedToAct, "acts on Qt " + running.toString().toStdString() + " iff it is affected");
    check(qEnvironmentVariableIsSet("QV4_GC_TIMELIMIT") == expectedToAct, "QV4_GC_TIMELIMIT set iff affected");
    if (expectedToAct) {
        check(qgetenv("QV4_GC_TIMELIMIT") == "0", "QV4_GC_TIMELIMIT is 0: the collector runs each step whole");
    }

    qputenv("QV4_GC_TIMELIMIT", "5");
    check(!seabass::gui::keepJsCollectorInOneStep(), "a value already set is left alone");
    check(qgetenv("QV4_GC_TIMELIMIT") == "5", "and keeps its value");
    qunsetenv("QV4_GC_TIMELIMIT");

    if (failures == 0) {
        std::cout << "qt_workarounds_test: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
