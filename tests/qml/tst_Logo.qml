import QtQuick
import QtTest
import SeabassGui

// Renders the brand SVG the way the app does (PreserveAspectFit in a
// square, then in the About page's 96 px box) and saves it when
// SEABASS_SCREENSHOT_DIR is set -- the only way to look at the icon
// without a desktop.
//
// The URL has to be the one the app itself asks for. The icon is
// registered with a QT_RESOURCE_ALIAS (see CMakeLists.txt) so it resolves
// without the src/gui/ prefix the other test QML files carry; this test
// kept the unaliased spelling for a while and quietly grabbed three empty
// boxes, which is exactly the failure a screenshot test exists to catch.
// Hence the status check below: a URL that resolves to nothing must fail
// the test, not produce a blank PNG that looks like a rendering bug.
TestCase {
    id: testCase
    name: "Logo"
    width: 480
    height: 240
    visible: true
    when: windowShown

    Component {
        id: logoComponent
        Row {
            spacing: 16
            Rectangle {
                width: 200; height: 200; color: "#1b1e22"; border.color: "#3daee9"
                Image {
                    id: large
                    objectName: "logo200"
                    anchors.fill: parent
                    source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(200, 200)
                }
            }
            Rectangle {
                width: 96; height: 96; color: "#1b1e22"; border.color: "#3daee9"
                Image {
                    objectName: "logo96"
                    anchors.fill: parent
                    source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(96, 96)
                }
            }
            Rectangle {
                width: 32; height: 32; color: "#1b1e22"; border.color: "#3daee9"
                Image {
                    objectName: "logo32"
                    anchors.fill: parent
                    source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(32, 32)
                }
            }
        }
    }

    function test_renders() {
        var row = createTemporaryObject(logoComponent, testCase, {});
        verify(row !== null);
        waitForRendering(row);
        for (var i = 0; i < row.children.length; ++i) {
            var image = row.children[i].children[0];
            tryVerify(function() { return image.status === Image.Ready; });
            compare(image.status, Image.Ready,
                "the brand SVG did not load: " + image.source);
        }
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(row).save(screenshotDir + "/logo.png");
        }
    }
}
