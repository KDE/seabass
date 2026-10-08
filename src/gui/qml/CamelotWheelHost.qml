// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma Singleton

import QtQuick

// The one Camelot wheel every key badge opens. Each badge used to carry a
// wheel of its own, made with the badge whether anybody opened it or not,
// so a list of tracks built a whole wheel per row (24 segments, 1440
// shapes for a screenful) and built them all again whenever the list was
// rebuilt: most of a playlist switch went on wheels nobody had asked for
// (qmlprofiler, Release, Windows). Only one can be open at a time anyway.
//
// Made the first time a badge asks for it, and kept, one per window: the
// wheel lies on that window's overlay. A badge in another window (a test's
// own, say) gets a wheel of its own there.
QtObject {
    id: host

    // The badge that opened the wheel last, which hears its hover signals.
    property Item opener: null

    property var wheel: null
    property Item wheelWindowItem: null

    property Component wheelComponent: Component {
        CamelotWheelPopup {
            onKeyHovered: (number, isMinor, hovering) => {
                if (host.opener) {
                    host.opener.keyHovered(number, isMinor, hovering);
                }
            }
            onRelationHovered: (relationLabel, hovering) => {
                if (host.opener) {
                    host.opener.relationHovered(relationLabel, hovering);
                }
            }
            onClosed: host.opener = null
        }
    }

    // `badge` is a KeyBadge (it has the two signals), `windowItem` the
    // content item of the window it is in, `key` what to select ("" for
    // none), `notation` the badge's own.
    function open(badge, windowItem, key, notation) {
        if (host.wheel === null || host.wheelWindowItem !== windowItem) {
            if (host.wheel !== null) {
                host.wheel.destroy();
            }
            host.wheel = host.wheelComponent.createObject(windowItem);
            host.wheelWindowItem = windowItem;
        }
        host.opener = badge;
        host.wheel.notation = notation;
        host.wheel.openAt(key);
    }
}
