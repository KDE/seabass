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
}
