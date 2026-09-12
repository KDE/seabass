import QtQuick
import QtQuick.Effects

// One layer of the large background watermark in the bottom-right corner
// of the main window: normally the "Sound Bass" app mark, swapped for the
// playing track's own cover art whenever there is one.
//
// Main.qml keeps two of these and alternates which is "front". A plain
// source swap on a single Image is an instantaneous pixel replacement, so
// fading one layer out and back in reads as a dip to nothing between old
// and new art rather than a blend. Crossfading needs the old image still
// on screen, fading out, while the new one fades in over it, and that
// needs two separate Image/MultiEffect stacks.
//
// This used to be an inline `component WatermarkLayer` inside Main.qml,
// which meant nothing could instantiate it and no test could look at it.
// It has a history of failing silently -- it once anchored its effect to
// an illegal target, resolved to zero size, and vanished from the app
// entirely -- so it lives in its own file with tst_Watermark.qml holding
// it to its corner.
Item {
    id: layer

    property alias source: img.source
    property bool isArtwork: false

    // Cover art fills its square edge to edge, so it is simply pulled out
    // past the corner and allowed to bleed off it.
    readonly property real artworkBleed: 0.08

    // The brand mark cannot be placed that way, because its artwork is
    // mostly empty. The SVG is a square viewBox with a fish centred in
    // it that is far wider than it is tall, so the ink stops well short
    // of the box on one axis and almost fills it on the other. These are
    // where the ink actually starts and stops, as a fraction of the
    // square, and they come straight from tools/flatten_logo.py's fitted
    // bounds: 3% padding left and right, 17.5% above and below.
    readonly property real markPadX: 0.03
    readonly property real markPadY: 0.175

    // Where the mark's ink should end up relative to the window corner,
    // again as a fraction of this layer. Small, so the fish tucks into
    // the corner without touching the edges.
    readonly property real markInsetX: 0.04
    readonly property real markInsetY: 0.02

    // Placing the artwork's bounding box, rather than its ink, is what
    // broke this. A single -8% margin used to do both jobs only because
    // the old mark happened to carry ~10% padding on every side; when
    // the fish was redrawn to span its viewBox, that same margin sliced
    // 5% off its nose. tst_Watermark.qml now measures the rendered
    // pixels, so a reshaped mark fails a test rather than losing a nose.
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.rightMargin: isArtwork ? -width * artworkBleed
                                   : width * (markInsetX - markPadX)
    anchors.bottomMargin: isArtwork ? -height * artworkBleed
                                    : height * (markInsetY - markPadY)
    width: Math.min(parent.width, parent.height) * 0.75
    height: width
    opacity: 0

    Behavior on opacity {
        NumberAnimation { duration: 400; easing.type: Easing.InOutQuad }
    }

    Image {
        id: img
        // Drawn directly for the brand mark, and only handed to the
        // MultiEffect below when there is artwork to blur. The effect was
        // previously in the path for both, with its blur switched off for
        // the mark -- a shader pass that changed no pixels, and one the
        // software renderer cannot run at all, so the mark rendered as
        // nothing under QT_QPA_PLATFORM=offscreen. The two must never be
        // visible at once or the source is drawn twice.
        visible: !layer.isArtwork
        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        smooth: true
        // Without this, the vector brand SVG still gets rasterized
        // once at whatever small default size the source reports
        // (not "crisp at any size" as the comment below assumes),
        // then that raster is upscaled to ~0.75x the window's
        // shorter side -- soft/blurry despite being vector source.
        // Binding sourceSize to the actual on-screen size makes Qt's
        // SVG renderer rasterize at target resolution instead.
        // Harmless for the artwork-cover-art case too: that path
        // already relies on MultiEffect's blur, not sharpness.
        sourceSize: Qt.size(layer.width, layer.height)
    }

    // Cover art is a small source image (a rekordbox/Engine
    // thumbnail, often well under 300px) stretched to ~0.75x the
    // window's shorter side. Upscaled that far, its own pixel grid
    // becomes visible ("scaled up a lot... shows artifacts").
    // Blurred here rather than just relying on Image.smooth's
    // bilinear filtering, which softens edges slightly but doesn't
    // hide a real resolution mismatch at this scale factor. The
    // brand SVG watermark is vector and crisp at any size, so it
    // skips this entirely and draws itself above.
    MultiEffect {
        visible: layer.isArtwork
        // Fills this layer (its actual parent). Anchoring straight
        // to the Image sibling-of-a-different-item instead is not a
        // legal QML anchor target (only parent/sibling) and was
        // silently resolving to a zero-size effect in an earlier
        // version of this watermark, which is why it disappeared
        // entirely for a while.
        anchors.fill: parent
        source: img
        blurEnabled: true
        blur: 1.0
        blurMax: 64
    }
}
