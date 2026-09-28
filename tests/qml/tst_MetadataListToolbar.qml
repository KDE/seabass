// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import SeabassGui

// The shared list toolbar (Sync, Metadata Restore, ...) shares its row with
// the page's own buttons. When those take room (Sync's Undo Last Save
// appearing after an interrupted save, rig check F6, 2026-09-28), the
// toolbar has to give: its summary label ran under the buttons instead.
TestCase {
    id: testCase
    name: "MetadataListToolbar"
    width: 900
    height: 200
    visible: true
    when: windowShown

    Component {
        id: rowComponent
        RowLayout {
            width: 700
            spacing: 8
            MetadataListToolbar {
                objectName: "toolbar"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                summary: "96 tracks · 96 selected, and a longer note than the row has room for"
            }
            Rectangle {
                objectName: "buttons"
                Layout.preferredWidth: 300
                Layout.minimumWidth: 300
                height: 30
                color: "transparent"
            }
        }
    }

    function test_theSummaryGivesWayBeforeTheRowOverflows() {
        const row = createTemporaryObject(rowComponent, testCase);
        waitForRendering(row);
        const toolbar = findChild(row, "toolbar");
        const buttons = findChild(row, "buttons");
        const summary = findChild(toolbar, "listSummary");
        const search = findChild(toolbar, "searchField");
        verify(summary !== null && search !== null && buttons !== null);
        const summaryRight = summary.mapToItem(row, summary.width, 0).x;
        const buttonsLeft = buttons.mapToItem(row, 0, 0).x;
        verify(summaryRight <= buttonsLeft + 0.5,
               "the summary ends before the buttons start: " + summaryRight + " vs " + buttonsLeft);
        verify(toolbar.width <= row.width - buttons.width - row.spacing + 0.5,
               "the toolbar fits beside the buttons: " + toolbar.width);
        verify(summary.truncated, "it is the summary that gave way");
        verify(search.width >= 110, "the search field keeps its floor: " + search.width);
    }
}
