import QtQuick
import QtTest
import SeabassGui

// The background watermark has to sit just inside the window's
// bottom-right corner: fully visible, and close enough to the corner to
// read as part of the background rather than a logo floating in the
// middle of nowhere.
//
// It is easy to get wrong because the brand SVG is a square viewBox with
// a much wider-than-tall fish centred in it, so the artwork's bounding
// box is nowhere near the ink. WatermarkLayer compensates with per-axis
// margins, and those numbers only hold while the mark keeps roughly its
// current proportions. When tools/flatten_logo.py reshaped the fish to
// span almost the full width of its viewBox, the old single margin
// sliced 5% off the nose and nobody noticed, because nothing rendered
// this component at all.
//
// So this measures the rendered pixels rather than trusting the numbers:
// no ink may touch the window edge, and ink must reach close to it.
TestCase {
    id: testCase
    name: "Watermark"
    // Must be the whole window, not a corner of it: grabImage() captures
    // the test case's own window, so a smaller one here quietly returns
    // an all-black image with the watermark outside it, and every "is it
    // clipped" check then passes by seeing nothing at all.
    width: 1100
    height: 720
    visible: true
    when: windowShown

    readonly property string brandMark:
        "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"

    Component {
        id: windowLike
        Rectangle {
            // Stands in for the main window: same default size, and it
            // clips exactly the way ApplicationWindow does.
            width: 1100
            height: 720
            color: "black"
            clip: true

            property alias watermark: mark

            WatermarkLayer {
                id: mark
                objectName: "watermark"
                // The real one sits at 0.18 so it reads as background.
                // Full opacity here only so a pixel is unambiguously ink
                // or unambiguously the window behind it.
                opacity: 1.0
            }
        }
    }

    function isInk(image, x, y) {
        var c = image.pixel(x, y);
        return c.r > 0.02 || c.g > 0.02 || c.b > 0.02;
    }

    function columnHasInk(image, x) {
        for (var y = 0; y < image.height; ++y) {
            if (isInk(image, x, y)) {
                return true;
            }
        }
        return false;
    }

    function rowHasInk(image, y) {
        for (var x = 0; x < image.width; ++x) {
            if (isInk(image, x, y)) {
                return true;
            }
        }
        return false;
    }

    function test_theMarkSitsInsideTheCornerWithoutBeingClipped() {
        var host = createTemporaryObject(windowLike, testCase, {});
        verify(host !== null);
        host.watermark.source = brandMark;
        tryVerify(function() { return host.watermark.width > 0; });
        waitForRendering(host);

        var image = grabImage(host);
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/watermark.png");
        }

        // Nothing may be cut off. The window clips, so ink in the very
        // last column or row means the mark runs off the edge.
        verify(!columnHasInk(image, image.width - 1),
            "the watermark is clipped by the window's right edge");
        verify(!rowHasInk(image, image.height - 1),
            "the watermark is clipped by the window's bottom edge");

        // ...and it still has to hug the corner. A mark that drifted into
        // open space would pass the checks above while looking wrong.
        var nearRight = Math.round(image.width * 0.94);
        var nearBottom = Math.round(image.height * 0.94);
        var reachesRight = false;
        for (var x = nearRight; x < image.width && !reachesRight; ++x) {
            reachesRight = columnHasInk(image, x);
        }
        var reachesBottom = false;
        for (var y = nearBottom; y < image.height && !reachesBottom; ++y) {
            reachesBottom = rowHasInk(image, y);
        }
        verify(reachesRight,
            "the watermark does not reach the window's right edge any more");
        verify(reachesBottom,
            "the watermark does not reach the window's bottom edge any more");
    }
}
