import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The one way changes reach the stick: a floating Save button in the
// bottom-right corner of an edit page, prominent on purpose (user
// decision) and disabled until there is something to save. The badge
// shows how many changes are staged; the tooltip lists them.
Item {
    id: root
    required property var session
    property string label: "Save"
    // Where this button's save actually lands. Every edit page writes to
    // the stick and takes the default; Metadata Backup writes to this
    // computer and never touches a stick, and its tooltip was the one
    // string on the page still saying otherwise.
    property string destinationPhrase: "written to the stick"

    readonly property bool hasSession: root.session !== null && root.session !== undefined
    readonly property int pendingCount: root.hasSession ? root.session.pendingCount : 0
    readonly property bool canSave: root.hasSession && root.session.dirty === true && root.session.state !== "writing"

    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight
    // Only when there is something to save. It used to sit there
    // permanently on any page with an edit session, which made a
    // floating button that does nothing the most prominent thing on a
    // page the user was only reading. Stays up while a save is running,
    // because that is when its progress matters most.
    visible: root.hasSession && (root.pendingCount > 0 || root.session.state === "writing")

    Button {
        id: button
        objectName: "saveButton"
        anchors.fill: parent
        enabled: root.canSave
        hoverEnabled: true
        leftPadding: 28
        rightPadding: 28
        topPadding: 14
        bottomPadding: 14
        onClicked: root.session.save()

        background: Rectangle {
            radius: height / 2
            color: !button.enabled ? Theme.surface
                : button.down ? Qt.darker(Theme.accent, 1.25)
                : button.hovered ? Qt.lighter(Theme.accent, 1.1)
                : Theme.accent
            border.color: button.enabled ? Qt.darker(Theme.accent, 1.3) : Theme.border
            border.width: 1
        }
        contentItem: RowLayout {
            spacing: 10
            Label {
                text: root.label
                font.family: Theme.titleFamily
                font.weight: Font.Bold
                font.pointSize: Theme.fontLarge
                color: button.enabled ? "#ffffff" : Theme.textMuted
            }
            Rectangle {
                objectName: "pendingBadge"
                visible: root.pendingCount > 0
                radius: height / 2
                color: button.enabled ? "#ffffff" : Theme.border
                implicitWidth: Math.max(badgeText.implicitWidth + 12, implicitHeight)
                implicitHeight: badgeText.implicitHeight + 6
                Label {
                    id: badgeText
                    anchors.centerIn: parent
                    text: root.pendingCount
                    font.bold: true
                    font.pointSize: Theme.fontSmall
                    color: button.enabled ? Theme.accent : Theme.textMuted
                }
            }
        }

        ToolTip.visible: hovered
        ToolTip.delay: 400
        // Three lines, each trimmed. Eight full descriptions was a wall
        // of text hanging off a button -- and a cleanup description is a
        // long sentence naming a track, a filename and a cue count, so
        // eight of them covered a good part of the window and told you
        // less than the count already did. The tooltip's job is "what am
        // I about to save"; the page itself lists the detail.
        ToolTip.text: {
            if (!root.hasSession) return "";
            if (root.pendingCount === 0) return "No unsaved changes";
            var shown = 3;
            var lines = root.session.pendingDescriptions.slice(0, shown).map(function (line) {
                return "- " + (line.length > 64 ? line.substring(0, 63) + "\u2026" : line);
            });
            if (root.pendingCount > shown) {
                lines.push("- and " + (root.pendingCount - shown) + " more");
            }
            return root.label + ": " + root.pendingCount + " change(s) " + root.destinationPhrase + "\n"
                 + lines.join("\n");
        }
    }
}
