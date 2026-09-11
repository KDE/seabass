// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

namespace seabass::gui
{

// Call at the very top of every background write task, before acquiring
// the stick write lock: a non-empty result is the refusal message to hand
// back (rekordbox or Engine DJ appears to be running, and either may be
// writing the very files this task is about to touch). Advisory, like
// the process detection it wraps -- never instead of the write lock.
QString refuseIfDjSoftwareRunning();

}  // namespace seabass::gui
