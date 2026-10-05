// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest

import "PixelScale.js" as PixelScale
import SeabassGui

// The catalog picker, since it became a combo box. What matters here is
// what a three-button row gave away for free and a combo box has to be
// asked for: that a catalog which is not on this stick still explains
// itself, and that picking one asks the page rather than switching the
// display on its own.
TestCase {
    id: testCase
    name: "LibrarySourceToggle"
    width: 400
    height: 300
    visible: true
    when: windowShown

    // How far the glyph's ink may sit from the name's, in pixels. The
    // translate that lines them up is computed from unhinted metrics and
    // painted hinted, so it is an approximation by construction: measured
    // here it leaves 0.84 px at worst, and the UI font is the machine's,
    // so another machine's hinting will not leave exactly that. The
    // regression this is here to catch -- the centring gone altogether --
    // is 3 px. The bound goes in the gap, near enough to the measurement
    // to mean something and far enough from it not to fail on a font.
    readonly property real allowedOffset: 1.25

    // grabImage() returns DEVICE pixels; mapToItem() returns logical
    // ones. On a Retina Mac those differ by two, so every band this file
    // samples landed at half its true position -- a row's band fell
    // across the boundary into its neighbour, one row read as empty, and
    // the backgrounds came back attributed to the wrong rows. It looked
    // exactly like a glyph that would not paint. Set from the grab
    // itself rather than assumed, because the only honest source for it
    // is the image that came back.
    property real grabScale: 1

    Component {
        id: toggleComponent
        LibrarySourceToggle {}
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // A single glyph, large, white on black, with nothing else in the
    // item: the smallest thing that can answer "does the bundled face
    // paint at all on this machine".
    Component {
        id: glyphProbeComponent
        Rectangle {
            property alias glyphText: probeLabel.text
            width: 80
            height: 80
            color: "black"
            Text {
                id: probeLabel
                anchors.centerIn: parent
                font.family: Theme.symbolFamily
                font.pixelSize: 48
                color: "white"
            }
        }
    }

    function test_showsTheCatalogItIsOn() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {current: "engine", width: 240});
        waitForRendering(toggle);
        compare(toggle.currentIndex, 0);
        compare(toggle.currentValue, "engine");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(toggle).save(screenshotDir + "/library-source-toggle.png");
        }
    }

    function test_everyCatalogCarriesItsOwnTooltip() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {});
        waitForRendering(toggle);
        compare(toggle.entries.length, 3);
        for (var i = 0; i < toggle.entries.length; ++i) {
            verify(toggle.entries[i].tooltip.length > 0,
                   toggle.entries[i].label + " has no tooltip");
        }
    }

    // The point of the rewording: a reader choosing between the two
    // Rekordbox catalogs is told which is which, and is not handed a file
    // name to do it with.
    function test_tooltipsNameVendorsAndAgeNotFiles() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {});
        waitForRendering(toggle);
        var byValue = {};
        for (var i = 0; i < toggle.entries.length; ++i) {
            byValue[toggle.entries[i].value] = toggle.entries[i].tooltip;
        }
        verify(byValue["engine"].indexOf("Denon") >= 0);
        verify(byValue["rekordbox"].indexOf("Pioneer") >= 0);
        verify(byValue["rekordbox"].indexOf("CDJ") >= 0);
        verify(byValue["rekordbox"].indexOf("older") >= 0);
        verify(byValue["onelibrary"].indexOf("newer") >= 0);
        var files = ["m.db", "export.pdb", "exportLibrary.db"];
        for (var v in byValue) {
            for (var f = 0; f < files.length; ++f) {
                verify(byValue[v].indexOf(files[f]) < 0,
                       v + "'s tooltip still quotes " + files[f]);
            }
        }
    }

    // A catalog that is not on this export is the one a reader most needs
    // a sentence about, so it keeps its tooltip instead of going inert.
    function test_absentCatalogStillSaysWhyItIsUnavailable() {
        var toggle = createTemporaryObject(toggleComponent, testCase,
                                           {hasOneLibrary: false});
        waitForRendering(toggle);
        var entry = toggle.entries[2];
        compare(entry.value, "onelibrary");
        compare(entry.selectable, false);
        verify(entry.tooltip.indexOf("Not present") >= 0);
    }

    function test_unsupportedCatalogUsesThePagesOwnReason() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {
            hasOneLibrary: true,
            oneLibrarySupported: false,
            oneLibraryUnsupportedReason: "Not supported for this operation yet"
        });
        waitForRendering(toggle);
        compare(toggle.entries[2].tooltip, "Not supported for this operation yet");
    }

    function test_pickingACatalogAsksThePage() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {current: "rekordbox"});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.selectEntry(0);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "engine");
        // And the page still owns what is displayed: nothing moved until
        // `current` came back changed.
        compare(toggle.currentValue, "rekordbox");
    }

    // A ComboBox handles arrow keys in C++ and writes currentIndex
    // itself, which is not a selection: it relabelled the header while
    // the page went on showing the old catalog, and it would do it for a
    // catalog the stick does not even have. The keys are routed through
    // the same path a click takes.
    function test_arrowKeyAsksThePageRatherThanRelabellingItself() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {current: "rekordbox"});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.forceActiveFocus();
        keyClick(Qt.Key_Down);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "onelibrary");
        // Still showing what the page still has: `current` has not come
        // back changed, so nothing about the display may have moved.
        compare(toggle.currentValue, "rekordbox");
        compare(toggle.currentIndex, 1);
    }

    function test_arrowKeyWalksBackwardsToo() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {current: "rekordbox"});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.forceActiveFocus();
        keyClick(Qt.Key_Up);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "engine");
    }

    function test_arrowKeyWillNotLandOnACatalogTheStickLacks() {
        var toggle = createTemporaryObject(toggleComponent, testCase,
                                           {current: "rekordbox", hasOneLibrary: false});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.forceActiveFocus();
        keyClick(Qt.Key_Down);
        // Nothing below it is selectable, so nothing is asked for and
        // nothing is displayed that is not there.
        compare(spy.count, 0);
        compare(toggle.currentValue, "rekordbox");
        compare(toggle.currentIndex, 1);
    }

    // What taking the keys over actually buys, over routing `activated`:
    // an arrow STEPS OVER a catalog this export lacks and lands on the
    // next real one. Left to the ComboBox, Down from Engine OS would
    // "select" the absent DeviceLibrary, be refused, and do nothing at
    // all -- a key that visibly does nothing on a picker with another
    // catalog still to offer.
    function test_arrowKeyStepsOverAnAbsentCatalogToTheNextRealOne() {
        var toggle = createTemporaryObject(toggleComponent, testCase,
                                           {current: "engine", hasRekordbox: false});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.forceActiveFocus();
        keyClick(Qt.Key_Down);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "onelibrary");
    }

    function test_pickingAnUnavailableCatalogAsksNothing() {
        var toggle = createTemporaryObject(toggleComponent, testCase,
                                           {current: "rekordbox", hasOneLibrary: false});
        waitForRendering(toggle);
        var spy = createTemporaryObject(spyComponent, testCase,
                                        {target: toggle, signalName: "sourceRequested"});
        toggle.selectEntry(2);
        compare(spy.count, 0);
        // ...but the list does not stay open pretending nothing happened.
        compare(toggle.popup.visible, false);
    }

    // Where the ink sits vertically, to better than a pixel: every row of
    // the band contributes how much it differs from the background, and
    // the answer is the weighted mean of those rows. Taking the first and
    // last inked row instead quantises the answer to half a pixel, which
    // is the same size as the errors worth catching here.
    function inkCentroid(image, background, x0, x1, y0, y1) {
        let weight = 0, moment = 0;
        for (let y = y0; y < y1; ++y) {
            for (let x = x0; x < x1; ++x) {
                const c = image.pixel(x, y);
                const d = Math.abs(c.r - background.r) + Math.abs(c.g - background.g)
                        + Math.abs(c.b - background.b);
                // Below this a pixel is the background plus rounding, not
                // ink; anti-aliased edges are well above it and are what
                // make this measurement subpixel in the first place.
                if (d > 0.02) {
                    weight += d;
                    moment += d * y;
                }
            }
        }
        return weight > 0 ? moment / weight : -1;
    }

    // Rows where anything differs from the background, inside a band of
    // columns.
    function inkRows(image, background, x0, x1, y0, y1) {
        var top = -1, bottom = -1;
        for (var y = y0; y < y1; ++y) {
            for (var x = x0; x < x1; ++x) {
                var c = image.pixel(x, y);
                if (Math.abs(c.r - background.r) + Math.abs(c.g - background.g) + Math.abs(c.b - background.b) > 0.15) {
                    if (top < 0) top = y;
                    bottom = y;
                    break;
                }
            }
        }
        return {top: top, bottom: bottom, centre: (top + bottom) / 2};
    }

    // Before any question about where a glyph sits: does it paint.
    //
    // The three catalog glyphs are U+2B21, U+25CE and U+25C8, and most
    // fonts have none of them. Theme bundles a three-glyph subset as a
    // QML resource precisely so the toggle looks the same on a machine
    // without Noto installed, and falls back to the real Noto family
    // when that resource does not load. Two environments have now been
    // found where nothing paints: a CI container carrying only DejaVu,
    // which has none of the three, and macOS, where the family resolves
    // to the bundled name and still inks nothing.
    //
    // Every other case in this file measures where the ink is, so all of
    // them fail together and none of them says why. This one says why,
    // in one line, with the loader's own status beside it.
    function test_theBundledSymbolFontPaintsItsGlyphs() {
        const glyphs = ["\u2B21", "\u25CE", "\u25C8"];
        const where = "family \"" + Theme.symbolFamily + "\", loader status " + Theme.symbolFont.status
                      + " (" + FontLoader.Ready + " is Ready), source " + Theme.symbolFont.source;
        // Two assertions, because either one alone is fooled by a
        // different machine. The loader being Ready is the only evidence
        // that the BUNDLED subset is available at all: on a machine that
        // has Noto Sans Symbols2 installed, the ink check below passes
        // through the system font whatever the resource did -- verified
        // by pointing the probe at a family name that does not exist,
        // which still painted here. And the ink check is the only
        // evidence that a loaded face draws anything: macOS resolves the
        // bundled family by name and still inks nothing.
        verify(Theme.symbolFont.status === FontLoader.Ready,
               "the bundled symbol subset did not load, so the toggle is relying on whatever the system has: "
                   + where);
        for (let i = 0; i < glyphs.length; ++i) {
            const probe = createTemporaryObject(glyphProbeComponent, testCase, {glyphText: glyphs[i]});
            verify(probe !== null);
            waitForRendering(probe);
            const image = grabImage(probe);
            let lit = 0;
            for (let y = 0; y < probe.height; ++y) {
                for (let x = 0; x < probe.width; ++x) {
                    const c = image.pixel(x, y);
                    if (c.r + c.g + c.b > 0.3) {
                        ++lit;
                    }
                }
            }
            verify(lit > 20,
                   "the catalog glyph " + glyphs[i] + " painted " + lit + " lit pixels of 6400 at 48px, white on "
                       + "black. Either the bundled subset is not loading and the fallback family is absent "
                       + "here, or the face loads and paints nothing. " + where);
        }
    }

    // The ink of one glyph/name pair, out of a grabbed image, in
    // testCase coordinates: measured from the pixels rather than from
    // the metrics the component itself used, which is the whole point.
    // TextMetrics reports UNHINTED metrics and NativeRendering hints
    // each glyph as it paints, so a sum that looks right on paper can
    // paint out of line.
    //
    // Both measurements are returned. The centroid is what the
    // assertions use; the extent (first and last row over a contrast
    // threshold) is kept only for "was anything painted at all". The
    // extent is what this file used to assert on, and on a faint
    // outline glyph it lies: rows of the hexagon that fall under the
    // threshold are simply not seen, so the band shrinks to a sliver at
    // one end and the centre it reports moves by whole pixels that
    // nothing on screen moved by. The centroid weighs every pixel by how
    // far it is from the background, which makes it subpixel and makes
    // it immune to that.
    //
    // The name is measured over its first letter only: a capital, no
    // descender.
    function pairInk(image, background, glyph, name, y0, y1) {
        const s = testCase.grabScale;
        const g = glyph.mapToItem(testCase, 0, 0);
        const n = name.mapToItem(testCase, 0, 0);
        const glyphBand = [Math.floor((g.x + glyph.leftPadding) * s), Math.ceil((g.x + glyph.width) * s)];
        const capitalBand = [Math.floor(n.x * s), Math.floor(n.x * s) + Math.round(7 * s)];
        return {
            glyph: inkRows(image, background, glyphBand[0], glyphBand[1], y0, y1),
            capital: inkRows(image, background, capitalBand[0], capitalBand[1], y0, y1),
            glyphCentroid: inkCentroid(image, background, glyphBand[0], glyphBand[1], y0, y1),
            capitalCentroid: inkCentroid(image, background, capitalBand[0], capitalBand[1], y0, y1)
        };
    }

    // The same pair again, in the open list. CatalogGlyph is used twice
    // -- once in the closed control, once per row of the popup -- and
    // the test above sees only the first of them, so a translate that
    // came out right in the header and wrong in the rows would have kept
    // every case here green. The rows are also where a reader compares
    // the three catalogs against each other, which is where a glyph
    // sitting high is most visible.
    // The most common colour in the right-hand quarter of an item, which
    // is past the end of its text in every style seen so far. One probe
    // pixel is not enough: the current catalog's row is highlighted, so
    // the three rows do not share a background, and a single probe can
    // land on a border, a gradient or a focus ring and turn the whole
    // row into "ink".
    // The rows of `image`, within y0..y1, where `item`'s own background
    // colour really is the background -- and nothing outside them.
    //
    // A delegate is not the same shape as the rectangle the style paints
    // behind it. Under org.kde.desktop the rows are separated by the
    // popup's own light background, so a band taken from the delegate's
    // height runs off the dark row into the light gap, and every gap
    // pixel differs from the row colour and counts as ink. That put the
    // measured ink of the Engine OS row at rows 39..65 when its text is
    // at 54..65, and dragged the glyph and capital centroids by
    // different amounts, because the two bands are different widths.
    //
    // The result was a glyph reported 2.04 px off its name on a row that
    // is aligned to within a third of a pixel. See seabass#45.
    function backgroundBand(image, item, y0, y1, background) {
        const s = testCase.grabScale;
        const p = item.mapToItem(testCase, 0, 0);
        const x0 = Math.floor((p.x + item.width * 0.75) * s);
        const x1 = Math.ceil((p.x + item.width - 2) * s);
        const isBackground = function(y) {
            let same = 0;
            let total = 0;
            for (let x = x0; x < x1; ++x) {
                const c = image.pixel(x, y);
                ++total;
                if (Math.abs(c.r - background.r) < 0.02 && Math.abs(c.g - background.g) < 0.02
                        && Math.abs(c.b - background.b) < 0.02) {
                    ++same;
                }
            }
            return total > 0 && same * 2 > total;
        };
        // The longest run, not the first: a row can have a border line
        // of its own at the top, which is one row of not-background
        // before the rectangle proper.
        let bestTop = -1;
        let bestBottom = -1;
        let runTop = -1;
        for (let y = y0; y <= y1; ++y) {
            const inside = y < y1 && isBackground(y);
            if (inside && runTop < 0) {
                runTop = y;
            } else if (!inside && runTop >= 0) {
                if (y - runTop > bestBottom - bestTop) {
                    bestTop = runTop;
                    bestBottom = y;
                }
                runTop = -1;
            }
        }
        return bestTop < 0 ? [y0, y1] : [bestTop, bestBottom];
    }

    function modalColour(image, item, y0, y1) {
        const s = testCase.grabScale;
        const p = item.mapToItem(testCase, 0, 0);
        const x0 = Math.floor((p.x + item.width * 0.75) * s);
        const x1 = Math.ceil((p.x + item.width - 2) * s);
        const counts = {};
        let best = null;
        let bestCount = 0;
        for (let y = y0; y < y1; ++y) {
            for (let x = x0; x < x1; ++x) {
                const c = image.pixel(x, y);
                const key = Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255);
                counts[key] = (counts[key] || 0) + 1;
                if (counts[key] > bestCount) {
                    bestCount = counts[key];
                    best = c;
                }
            }
        }
        return best;
    }

    // What a text item IS, for a failure message: its size, and the font
    // the platform actually resolved rather than the one asked for.
    // font.family is the request; fontInfo.family is what got used, and a
    // glyph that paints nothing is often a bundled family that did not
    // load, with the fallback lacking the code point.
    function describeText(item) {
        const resolved = item.fontInfo !== undefined ? item.fontInfo.family : "(no fontInfo)";
        // The application font is printed beside the item's own, because
        // the two answer different questions. QGuiApplication::setFont()
        // decides the first; a STYLE can decide the second, and where a
        // style supplies its own font the app's choice never reaches the
        // label. Windows reports names asking for the generic "Sans
        // Serif" and being handed the bundled symbol subset even after
        // the app font was named explicitly, and this line is what tells
        // us whether the app font took and the style overrode it, or the
        // app font never took at all.
        return "\"" + item.text + "\" " + Math.round(item.width) + "x" + Math.round(item.height)
               + ", font asked \"" + item.font.family + "\" got \"" + resolved + "\" at "
               + item.font.pixelSize + "px (application font \"" + Qt.application.font.family + "\")";
    }

    function describeRect(item) {
        const p = item.mapToItem(testCase, 0, 0);
        return "x " + Math.round(p.x) + " y " + Math.round(p.y) + " " + Math.round(item.width) + "x"
               + Math.round(item.height) + " in a " + testCase.width + "x" + testCase.height + " window";
    }

    // The same pair again, in the open list. CatalogGlyph is used twice
    // -- once in the closed control, once per row of the popup -- and
    // the test above sees only the first of them, so a translate that
    // came out right in the header and wrong in the rows would have kept
    // every case here green. The rows are also where a reader compares
    // the three catalogs against each other, which is where a glyph
    // sitting high is most visible.
    // One row's measurements, as a line. Every assertion below prints
    // the whole table rather than only the row it failed on: which rows
    // pass is half the evidence, and reading "this row inked nothing"
    // beside "the two either side of it inked normally" is a different
    // conclusion from reading it alone.
    function describeRow(m) {
        return m.text + " [" + m.rect + "] bg rgb(" + m.bg + ")"
               + " glyph ink " + m.glyphInk + " name ink " + m.nameInk
               + " off " + (m.off === null ? "n/a" : m.off.toFixed(2))
               + " colours glyph " + m.glyphColour + " name " + m.nameColour;
    }

}
