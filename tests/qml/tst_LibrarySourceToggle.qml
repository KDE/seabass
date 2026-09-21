// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
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

    Component {
        id: toggleComponent
        LibrarySourceToggle {}
    }

    Component {
        id: spyComponent
        SignalSpy {}
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
        const g = glyph.mapToItem(testCase, 0, 0);
        const n = name.mapToItem(testCase, 0, 0);
        const glyphBand = [Math.floor(g.x + glyph.leftPadding), Math.ceil(g.x + glyph.width)];
        const capitalBand = [Math.floor(n.x), Math.floor(n.x) + 7];
        return {
            glyph: inkRows(image, background, glyphBand[0], glyphBand[1], y0, y1),
            capital: inkRows(image, background, capitalBand[0], capitalBand[1], y0, y1),
            glyphCentroid: inkCentroid(image, background, glyphBand[0], glyphBand[1], y0, y1),
            capitalCentroid: inkCentroid(image, background, capitalBand[0], capitalBand[1], y0, y1)
        };
    }

    // Each catalog's glyph is centred on its name, by the ink rather than
    // by the text boxes: the glyph font's metrics differ from the name's,
    // and centring the boxes put the glyph a couple of pixels high.
    function test_glyphIsCentredOnTheName() {
        var toggle = createTemporaryObject(toggleComponent, testCase, {width: 220});
        verify(toggle !== null);
        var values = ["engine", "rekordbox", "onelibrary"];
        for (var i = 0; i < values.length; ++i) {
            toggle.current = values[i];
            waitForRendering(toggle);
            var glyph = findChild(toggle.contentItem, "catalogGlyph");
            var name = findChild(toggle.contentItem, "catalogName");
            verify(glyph !== null && name !== null);
            var image = grabImage(testCase);
            var origin = toggle.mapToItem(testCase, 0, 0);
            var background = image.pixel(Math.round(origin.x + toggle.width / 2), Math.round(origin.y + 3));
            var y0 = Math.floor(origin.y + 3), y1 = Math.ceil(origin.y + toggle.height - 3);
            const ink = pairInk(image, background, glyph, name, y0, y1);
            const capitalInk = ink.capital;
            verify(ink.glyph.top >= 0 && capitalInk.top >= 0, values[i] + ": nothing was painted");
            const off = ink.glyphCentroid - ink.capitalCentroid;
            verify(Math.abs(off) <= testCase.allowedOffset,
                   values[i] + ": the glyph's centre is " + off + " px off the name's. glyph "
                       + describeText(glyph) + "; name " + describeText(name));
            // And the name stays centred in the box: lining the two up by
            // their baseline lifted the whole text about 2 px.
            var boxCentre = origin.y + (toggle.height - 1) / 2;
            verify(Math.abs(capitalInk.centre - boxCentre) <= 1.5,
                   values[i] + ": the name sits " + (capitalInk.centre - boxCentre) + " px off the box's centre");
        }
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
    function modalColour(image, item, y0, y1) {
        const p = item.mapToItem(testCase, 0, 0);
        const x0 = Math.floor(p.x + item.width * 0.75);
        const x1 = Math.ceil(p.x + item.width - 2);
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
        return "\"" + item.text + "\" " + Math.round(item.width) + "x" + Math.round(item.height)
               + ", font asked \"" + item.font.family + "\" got \"" + resolved + "\" at "
               + item.font.pixelSize + "px";
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
    function test_everyRowInTheListLinesUpToo() {
        const toggle = createTemporaryObject(toggleComponent, testCase,
                                             {width: 220, current: "rekordbox"});
        verify(toggle !== null);
        waitForRendering(toggle);
        toggle.popup.open();
        tryVerify(function() { return toggle.popup.visible; });
        const view = toggle.popup.contentItem;
        tryVerify(function() { return view.count === 3 && view.itemAtIndex(2) !== null; });
        waitForRendering(view);
        const image = grabImage(testCase);
        for (let i = 0; i < 3; ++i) {
            const row = view.itemAtIndex(i);
            verify(row !== null, "row " + i + " is not there");
            const glyph = findChild(row, "entryGlyph");
            const name = findChild(row, "entryName");
            verify(glyph !== null && name !== null, "row " + i + " has no glyph/name pair");
            const origin = row.mapToItem(testCase, 0, 0);
            // Measuring a row the grab does not contain is measuring
            // nothing, and it is not the same fault as a glyph out of
            // line, so it is not reported as one. Styles put a combo's
            // popup in different places -- below the field, or over it
            // with the current row on top of it -- and its own window
            // when the style says so, which this scene's grab never
            // sees. Whichever it is, say where the row actually was.
            verify(origin.y >= 0 && origin.y + row.height <= testCase.height && origin.x >= 0
                       && origin.x + row.width <= testCase.width,
                   name.text + ": the row is not inside the grabbed scene (" + describeRect(row)
                       + "). Either the popup is a separate window in this style, or it is placed over the "
                       + "field rather than under it; ctest pins QT_QUICK_CONTROLS_STYLE=Basic, and a direct "
                       + "run of this binary inherits the desktop's style");
            // A band with no width measures nothing, whatever the
            // renderer did, so it is not the same finding as an empty
            // band and is not reported as one. macOS sees rows whose
            // glyph inks nothing in every configuration, including one
            // where the name beside it inks cleanly, and a glyph item
            // sized 0 wide would produce exactly that.
            verify(glyph.width > 0 && glyph.height > 0 && name.width > 0,
                   name.text + ": the pair has no area to measure. glyph " + describeText(glyph)
                       + "; name " + describeText(name));
            const y0 = Math.floor(origin.y + 2), y1 = Math.ceil(origin.y + row.height - 2);
            const background = modalColour(image, row, y0, y1);
            const ink = pairInk(image, background, glyph, name, y0, y1);
            verify(ink.glyph.top >= 0 && ink.capital.top >= 0,
                   name.text + ": nothing was painted in the row, although it is inside the scene ("
                       + describeRect(row) + "). Background read as rgb(" + Math.round(background.r * 255) + ","
                       + Math.round(background.g * 255) + "," + Math.round(background.b * 255) + "); glyph ink "
                       + ink.glyph.top + ".." + ink.glyph.bottom + ", name ink " + ink.capital.top + ".."
                       + ink.capital.bottom + ". Items: glyph " + describeText(glyph) + "; name "
                       + describeText(name));
            const off = ink.glyphCentroid - ink.capitalCentroid;
            verify(Math.abs(off) <= testCase.allowedOffset,
                   name.text + ": the row's glyph is " + off + " px off its name. glyph " + describeText(glyph)
                       + "; name " + describeText(name));
        }
        toggle.popup.close();
    }
}
