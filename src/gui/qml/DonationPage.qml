import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Opened from the heart button in StickListPage.qml's header. Content
// below is a placeholder -- the real text/links are still being written.
Page {
    id: root

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                title: "Support Seabass"
                onHomeRequested: root.StackView.view.pop(null)
            }
            Item { Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12

        Label {
            text: "❤️"
            font.pointSize: Theme.titleLarge
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            text: "Content coming soon."
            color: Theme.textMuted
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
