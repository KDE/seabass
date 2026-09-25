// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// Theme's colours as the palette of `target`, the app's window, and so of
// everything under it that inherits its palette: popups included, live
// as Theme changes.
//
// Qt's Basic and Fusion styles ink a Label nobody gave a colour with
// palette.windowText, and ground a Page, a button or a field with
// palette.window, button and base. Left alone that palette is the
// style's or the platform's: Basic's own fixed light one, or whatever
// the platform theme hands Fusion, which is Qt's light palette on any
// desktop but Plasma (GNOME, Xfce, a bare X server). Under Kelp that
// drew page titles, overlay headings and report counts in near-black on
// Kelp's near-black, and grounded every page light around Theme's dark
// cards.
//
// QGuiApplication::setPalette() cannot do this job: a Qt Quick window
// takes the application palette only when it changes after the window
// exists, so one set at startup never reaches the controls, and a
// window made later starts from the style's palette again.
//
// Only under those two (ControlsStyle.inksFromPalette,
// gui/controls_style.hpp). KDE's style takes its ink from KDE's colour
// scheme (gui/app_color_scheme.hpp), Material from Material.theme and
// FluentWinUI3 from the colour scheme hint, all of which already follow
// Theme; their other palette roles are their own look, and stay.
QtObject {
    id: root

    // An Item or a Window: anything with a `palette`.
    property var target: null

    readonly property QtObject palette: target && ControlsStyle.inksFromPalette ? target.palette : null

    property list<QtObject> bindings: [
        Binding { target: root.palette; property: "window"; value: Theme.background },
        Binding { target: root.palette; property: "windowText"; value: Theme.text },
        Binding { target: root.palette; property: "base"; value: Theme.surface },
        Binding { target: root.palette; property: "alternateBase"; value: Theme.rowOdd },
        Binding { target: root.palette; property: "text"; value: Theme.text },
        Binding { target: root.palette; property: "placeholderText"; value: Theme.textMuted },
        Binding { target: root.palette; property: "button"; value: Theme.rowHover },
        Binding { target: root.palette; property: "buttonText"; value: Theme.text },
        Binding { target: root.palette; property: "toolTipBase"; value: Theme.surface },
        Binding { target: root.palette; property: "toolTipText"; value: Theme.text },
        Binding { target: root.palette; property: "highlight"; value: Theme.accent },
        Binding { target: root.palette; property: "highlightedText"; value: Theme.accentInk },
        Binding { target: root.palette; property: "accent"; value: Theme.accent },
        Binding { target: root.palette; property: "link"; value: Theme.info },
        Binding { target: root.palette ? root.palette.disabled : null; property: "windowText"; value: Theme.textMuted },
        Binding { target: root.palette ? root.palette.disabled : null; property: "text"; value: Theme.textMuted },
        Binding { target: root.palette ? root.palette.disabled : null; property: "buttonText"; value: Theme.textMuted }
    ]
}
