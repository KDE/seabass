// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QVersionNumber>

namespace seabass::gui
{

// Whether this Qt's JavaScript garbage collector can free objects that are
// still in use (QTBUG-138621, QTBUG-134687). The incremental collector
// arrived in 6.8.0; the fix is in 6.8.9, 6.11.2 and 6.12.0. Every release
// in between is affected, on every platform.
bool jsCollectorFreesLiveObjects(const QVersionNumber &qt);

// On an affected Qt, makes the collector finish each run in one step
// instead of interleaving it with the application: that is the mode in
// which the bug does not show. Reads the running Qt, not the one this
// was built against, and leaves QV4_GC_TIMELIMIT alone when it is already
// set, so a hunt or a user can still choose. Must run before the first
// QQmlEngine exists: the collector reads the variable when it is made.
// Returns whether it set anything.
bool keepJsCollectorInOneStep();

}  // namespace seabass::gui
