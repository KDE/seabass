import QtQuick
import QtTest
import SeabassGui

// Spike harness: scans a library, picks tracks with art and a waveform,
// and saves a sheet of rings to screenshotDir. Not part of the suite.
TestCase {
    id: testCase
    name: "TrackRingSpike"
    width: 1500
    height: 980
    visible: true
    when: windowShown

    property string libraryPath: "/home/sebas/Seabass/builds/track-ring-sample/PIONEER"
    ScanController { id: scanner }
    PlaybackController { id: player }

    Rectangle {
        id: sheet
        anchors.fill: parent
        color: Theme.background

        property var picks: []

        Row {
            anchors.fill: parent
            anchors.margins: 40
            spacing: 40

            // The showpiece: an expanded "now playing".
            Column {
                width: 620
                spacing: 18
                Loader {
                    width: 620; height: 620
                    active: sheet.picks.length > 0
                    sourceComponent: TrackRing {
                        waveformData: sheet.picks[0].waveform
                        cueData: sheet.picks[0].cues
                        trackDurationMs: sheet.picks[0].durationSeconds * 1000
                        artworkSource: sheet.picks[0].artworkPath.replace(/\.jpg$/, "_m.jpg")
                        progress: 0.37
                        fallbackColor: Theme.accent
                    }
                }
                Text {
                    width: 620; horizontalAlignment: Text.AlignHCenter
                    text: sheet.picks.length > 0 ? sheet.picks[0].title : ""
                    color: Theme.text; font.pointSize: Theme.fontLarge; elide: Text.ElideRight
                }
                Text {
                    width: 620; horizontalAlignment: Text.AlignHCenter
                    text: sheet.picks.length > 0 ? sheet.picks[0].artist : ""
                    color: Theme.textMuted; font.pointSize: Theme.fontNormal; elide: Text.ElideRight
                }
            }

            Column {
              spacing: 36
              Row {
                spacing: 28
                Repeater {
                    model: sheet.picks.length > 6 ? [{s: 112, b: 96}, {s: 84, b: 72}, {s: 60, b: 48}] : []
                    TrackRing {
                        required property var modelData
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: modelData.s; height: modelData.s
                        bars: modelData.b
                        waveformData: sheet.picks[6].waveform
                        trackDurationMs: sheet.picks[6].durationSeconds * 1000
                        artworkSource: sheet.picks[6].artworkPath.replace(/\.jpg$/, "_m.jpg")
                        progress: 0.55
                    }
                }
              }
            Grid {
                columns: 3
                spacing: 24
                Repeater {
                    model: Math.max(0, sheet.picks.length - 1)
                    TrackRing {
                        required property int index
                        readonly property var pick: sheet.picks[index + 1]
                        width: 240; height: 240
                        bars: 160
                        waveformData: pick.waveform
                        cueData: pick.cues
                        trackDurationMs: pick.durationSeconds * 1000
                        artworkSource: index === 7 ? "" : pick.artworkPath.replace(/\.jpg$/, "_m.jpg")
                        progress: index === 4 ? -1 : [0.08, 0.62, 0.91, 0.25, 0, 0.5, 0.75, 0.4, 0.15][index]
                        fallbackColor: Theme.accent
                    }
                }
            }
            }
        }
    }

    function test_renderSheet() {
        scanner.scan("rekordbox", libraryPath);
        tryVerify(function() { return !scanner.busy && scanner.tracks.trackCount() > 0; }, 120000);
        var n = scanner.tracks.trackCount();
        console.log("tracks:", n, scanner.errorMessage);
        var picks = [];
        var withoutArt = null;
        var step = Math.max(1, Math.floor(n / 60));
        for (var i = 0; i < n && picks.length < 9; i += step) {
            var t = scanner.tracks.trackAt(i);
            var w = player.waveformFor("rekordbox", libraryPath, t.sourceId);
            if (!w || w.length === 0) continue;
            t.waveform = w;
            if (t.artworkPath.length === 0) { if (!withoutArt) withoutArt = t; continue; }
            picks.push(t);
        }
        if (withoutArt) picks.push(withoutArt);
        console.log("picked:", picks.length, "columns:", picks.length ? picks[0].waveform.length : 0,
                    "sample:", JSON.stringify(picks[0].waveform[300]), "cues:", JSON.stringify(picks[0].cues).slice(0, 300));
        sheet.picks = picks;
        wait(1500);
        grabImage(sheet).save(screenshotDir + "/track-ring.png");
    }
}
