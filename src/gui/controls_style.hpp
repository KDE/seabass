// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace seabass::gui
{

// Picks the Qt Quick Controls style this app runs under, where the
// platform's own default is not one it can be themed in.
//
// Windows falls back to "Basic" with nothing set, which looks nothing
// like the rest of the app, so it takes FluentWinUI3. macOS's native
// style refuses the background and contentItem overrides every control
// here is built on -- it logs "does not support customization" and
// draws its own -- so it takes Material, which honours the palette.
// Linux keeps its KDE-driven auto-selection.
//
// An explicit QT_QUICK_CONTROLS_STYLE always wins, so a developer, and
// ctest, can still pin a style deliberately.
//
// The test harness calls this too, and that is the point: a suite that
// runs under a style the app never uses is testing a different program.
// macOS found this the expensive way -- rows reported as painting
// nothing, on a fresh profile, under the native style the shipped app
// replaces before it draws anything.
void applyDefaultControlsStyle();

}  // namespace seabass::gui
