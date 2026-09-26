// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/stick_events.hpp"

namespace seabass::gui
{

StickEvents &StickEvents::instance()
{
    static StickEvents events;
    return events;
}

}  // namespace seabass::gui
