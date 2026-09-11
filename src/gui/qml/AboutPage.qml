import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

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
                title: "About"
                onHomeRequested: root.StackView.view.pop(null)
            }
            Item { Layout.fillWidth: true }
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 64
        clip: true
        // A Flickable is draggable by default even when there's nothing
        // to scroll -- content shorter than the viewport still let you
        // click-drag it into an elastic overshoot and snap back, which
        // reads as a bug (dragging a page that visibly has no scrollbar
        // and doesn't move). Only interactive once there's real overflow.
        interactive: contentHeight > height

        ColumnLayout {
            id: content
            x: Math.max(32, (parent.width - width) / 2)
            width: Math.min(parent.width - 64, 640)
            y: 32
            spacing: 20

            Image {
                source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                Layout.preferredWidth: 96
                Layout.preferredHeight: 96
                Layout.alignment: Qt.AlignHCenter
                fillMode: Image.PreserveAspectFit
            }

            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 2
                Label {
                    text: "Seabass"
                    font.family: Theme.titleFamily
                    font.weight: Theme.titleWeight
                    font.pointSize: Theme.titleLarge
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: "Your DJ Toolbox"
                    font.pointSize: Theme.baseFontPointSize * 1.3
                    color: Qt.lighter(Theme.accent, 1.3)
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            Label {
                text: "Seabass reads and writes the DJ data already on a Rekordbox or Denon Engine "
                    + "USB stick - cue points, playlists, and the metadata your DJ software already "
                    + "computed - so you can inspect it, fix it up, and keep the two formats in sync."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true
                Subtitle { text: "What Seabass does" }
                Label {
                    text: "• Browses tracks, playlists and cue points across every catalog on a stick\n"
                        + "• Finds duplicate tracks and consolidates their cue points onto every copy\n"
                        + "• Syncs cues between every pair of catalogs present on the same stick\n"
                        + "• Lists and cleans up the automatic backups made before every write\n"
                        + "• Backs up cues to this computer and restores them if a stick's cues are lost\n"
                        + "• Shows a stick's saved Rekordbox player settings"
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true
                Subtitle { text: "The three library catalogs" }
                Label {
                    text: "A stick can carry up to three separate, independently-maintained catalogs of "
                        + "the same tracks. Browse Library can switch between whichever ones are present:"
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
                Label {
                    text: "• Engine OS - Denon Engine DJ's own library (m.db). What Denon/inMusic "
                        + "hardware (SC5000, Prime series, ...) reads directly.\n"
                        + "• DeviceLibrary - Rekordbox's classic per-stick export (export.pdb). What "
                        + "CDJs and XDJs read directly; every rekordbox export has this.\n"
                        + "• OneLibrary - Rekordbox 7's newer unified library (exportLibrary.db, also "
                        + "called \"Device Library Plus\"). Mirrors DeviceLibrary's tracks in a richer "
                        + "schema, kept in sync by Rekordbox itself; not present on every export."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
                Label {
                    text: "OneLibrary supports adding cues directly, same as DeviceLibrary and Engine OS. "
                        + "Merging duplicates isn't supported on this catalog yet."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
                Label {
                    text: "Engine OS can also link tracks to a streaming service (e.g. TIDAL) instead of a "
                        + "local file. Seabass shows these (badged with the service name) for visibility, "
                        + "but never plays, merges, syncs, or cleans them up. It only ever operates on "
                        + "files actually present on the stick. Preferences has an option to hide them from "
                        + "Browse Library entirely instead."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true
                Subtitle { text: "What Seabass doesn't do" }
                Label {
                    text: "Seabass never analyzes audio. Beatgridding, BPM/key detection, and waveform "
                        + "analysis all have to happen in Rekordbox or Engine DJ software first. "
                        + "Seabass only ever reads and moves around the results of that analysis, "
                        + "never recomputes it."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            Label {
                text: "Rekordbox is a trademark of AlphaTheta Corporation. Engine DJ is a trademark of "
                    + "inMusic Brands, Inc. TIDAL is a trademark of TIDAL Music AS. Seabass is an "
                    + "independent, unofficial tool and is not affiliated with, endorsed by, or "
                    + "sponsored by any of them."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.fontTiny
                color: Theme.textMuted
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
        }
    }
}
