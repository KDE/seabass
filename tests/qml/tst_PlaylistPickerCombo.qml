// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

TestCase {
    id: testCase
    name: "PlaylistPickerCombo"
    width: 400
    height: 300
    visible: true
    when: windowShown

    property var testModel: [
        { name: "All tracks", count: 42 },
        { name: "Techno", count: 12 },
        { name: "House", count: 8 },
    ]

    Component {
        id: comboComponent
        PlaylistPickerCombo {}
    }

    // No static target -- each test that needs it points this at whatever
    // combo it just created (createTemporaryObject()'d instances don't
    // exist yet when this component tree is built).
    SignalSpy {
        id: pickedSpy
        signalName: "playlistPicked"
    }

    // A regression guard for exactly today's bug: redeclaring a property
    // ComboBox already owns (`model` is FINAL in Qt 6's QQC2) fails object
    // creation outright. This alone would have caught it -- no click
    // needed, just instantiation.
    function test_instantiatesWithModel() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel });
        verify(combo !== null);
        compare(combo.model.length, 3);
    }

    function test_currentIndexReflectsExternalSelection() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel, currentIndex: 1 });
        verify(combo !== null);
        compare(combo.currentIndex, 1);
        compare(combo.model[combo.currentIndex].name, "Techno");
    }

    function test_playlistPickedOnRowClick() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel });
        verify(combo !== null);

        pickedSpy.target = combo;
        pickedSpy.clear();

        mouseClick(combo);
        tryVerify(function() { return combo.popup.visible; });
        // The ListView's delegates aren't necessarily instantiated the
        // instant the popup becomes visible -- poll rather than assume
        // one synchronous check after popup.visible is enough.
        tryVerify(function() { return combo.popup.contentItem.itemAtIndex(2) !== null; });

        var row = combo.popup.contentItem.itemAtIndex(2);
        verify(row !== null);
        mouseClick(row);

        tryVerify(function() { return pickedSpy.count === 1; });
        compare(pickedSpy.signalArguments[0][0], 2);
        compare(pickedSpy.signalArguments[0][1].name, "House");
        tryVerify(function() { return !combo.popup.visible; });
    }
}
