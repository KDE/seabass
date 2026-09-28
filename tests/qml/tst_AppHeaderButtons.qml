// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The About, Preferences and Support buttons, on the home and on those
// three pages: the button for the page one is on is off, the others lead
// to their page.
TestCase {
    id: testCase
    name: "AppHeaderButtons"
    width: 400
    height: 100
    visible: true
    when: windowShown

    Component { id: buttonsComponent; AppHeaderButtons {} }
    Component { id: spyComponent; SignalSpy {} }

    function test_theCurrentPagesButtonIsOffAndTheOthersLead_data() {
        return [
            {tag: "home", current: "", off: ""},
            {tag: "about", current: "about", off: "aboutButton"},
            {tag: "preferences", current: "preferences", off: "preferencesButton"},
            {tag: "support", current: "support", off: "donateButton"},
        ];
    }

    function test_theCurrentPagesButtonIsOffAndTheOthersLead(data) {
        const row = createTemporaryObject(buttonsComponent, testCase, {current: data.current});
        waitForRendering(row);
        const names = ["aboutButton", "preferencesButton", "donateButton"];
        const signals = ["aboutRequested", "preferencesRequested", "supportRequested"];
        for (let i = 0; i < names.length; ++i) {
            const button = findChild(row, names[i]);
            verify(button !== null, names[i] + " exists");
            compare(button.enabled, names[i] !== data.off, names[i] + " on " + data.tag);
            const spy = createTemporaryObject(spyComponent, testCase, {target: row, signalName: signals[i]});
            mouseClick(button);
            compare(spy.count, button.enabled ? 1 : 0, names[i] + " leads to its page, or is off");
        }
    }
}
