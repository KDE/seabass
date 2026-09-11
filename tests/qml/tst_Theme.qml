// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// A regression guard for a real gotcha hit setting this test module up:
// Theme.qml's QT_QML_SINGLETON_TYPE source property has to be set BEFORE
// qt_add_qml_module() registers it (see CMakeLists.txt's own comment on
// the exact ordering) -- get that backwards and Theme.qml compiles as a
// plain, non-singleton type instead. Every bare `Theme.xxx` reference
// elsewhere then silently evaluates to undefined (no error at all,
// nothing to grep for) rather than the QML engine complaining that
// "Theme" isn't defined, which took real effort to track down the first
// time. This test would have caught it in under a second.
TestCase {
    id: testCase
    name: "Theme"
    when: true

    function test_singletonResolvesToRealValues() {
        // kelpBackground-derived colors, for the default (non-system-theme)
        // palette -- plain literals, no external dependency, so any of
        // these being undefined means the singleton itself isn't wired up
        // correctly, not that some specific color computation is wrong.
        verify(Theme.surface !== undefined);
        verify(Theme.accent !== undefined);
        verify(Theme.rowEven !== undefined);
        // fontSmall depends on the QML_SINGLETON C++ SystemFontMetrics
        // type also being registered on this module -- a separate, but
        // similarly silent, failure mode if it's ever missing.
        verify(Theme.fontSmall !== undefined);
        verify(Theme.fontSmall > 0);
    }
}
