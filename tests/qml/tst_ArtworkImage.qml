// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Cover art that finds out at display time whether its file is there:
// the readers name the art a catalog names without looking for it.
TestCase {
    id: testCase
    name: "ArtworkImage"
    width: 200
    height: 100
    visible: true
    when: windowShown

    Component {
        id: artComponent
        ArtworkImage {
            width: 40
            height: 40
        }
    }

    // The file:// URL of an absolute local path: file:///C:/... on
    // Windows, where "file://" + path gave file://C:/..., which Qt reads
    // as host "c".
    function fileUrl(path) {
        return path.startsWith("/") ? "file://" + path : "file:///" + path;
    }

    function make(source, fallbackSource) {
        const art = createTemporaryObject(artComponent, testCase, {source: source, fallbackSource: fallbackSource});
        verify(art !== null);
        return art;
    }

    function test_aPresentSourceIsShown() {
        const art = make(fileUrl(browseFixture.presentArtwork()), fileUrl(browseFixture.missingArtwork()));
        tryCompare(art, "showing", "source");
        const image = findChild(art, "artworkImage");
        compare(art.drawn, true);
        compare(image.status, Image.Ready);
        // It fades in rather than popping in, and ends fully there.
        tryCompare(art, "reveal", 1, 2000);
    }

    // Nothing loaded, nothing to fade: transparent, so no half-drawn frame
    // shows before the art arrives.
    function test_nothingLoadedIsTransparent() {
        const art = make("", "");
        compare(art.reveal, 0);
        compare(art.drawn, false);
    }

    // The case the readers leave to it: an Engine row naming art that is
    // not on the stick, and the rekordbox copy's art that is.
    function test_aMissingSourceShowsTheFallback() {
        const art = make(fileUrl(browseFixture.missingArtwork()), fileUrl(browseFixture.presentArtwork()));
        tryCompare(art, "showing", "fallback");
        const image = findChild(art, "artworkImage");
        compare(art.drawn, true);
        compare(image.status, Image.Ready);
        compare(art.chosenSource.toString(), fileUrl(browseFixture.presentArtwork()));
    }

    function test_anEmptySourceShowsTheFallback() {
        const art = make("", fileUrl(browseFixture.presentArtwork()));
        tryCompare(art, "showing", "fallback");
        compare(art.drawn, true);
    }

    // Nothing drawn at all, not a broken-image frame: the row's own
    // placeholder behind it is what shows.
    function test_bothMissingShowNothing() {
        const art = make(fileUrl(browseFixture.missingArtwork()), fileUrl(browseFixture.missingArtwork() + ".png"));
        const image = findChild(art, "artworkImage");
        tryCompare(art, "sourceFailed", true, 5000, "the source was tried first");
        tryCompare(image, "status", Image.Error);
        compare(art.chosenSource.toString(), fileUrl(browseFixture.missingArtwork() + ".png"), "then the fallback");
        compare(art.drawn, false);
        compare(findChild(art, "roundedArtwork").visible, false, "nor by the shader");
        compare(image.visible, false);
        compare(art.showing, "");
    }

    // A file on disk goes through the cover cache (gui/cover_cache.hpp),
    // decoded at the drawn size; it is the same picture either way.
    function test_aLocalFileLoadsThroughTheCoverCache() {
        const art = make(fileUrl(browseFixture.presentArtwork()), "");
        tryCompare(art, "showing", "source");
        const image = findChild(art, "artworkImage");
        verify(image.source.toString().startsWith("image://cover/"), image.source.toString());
        // Covers the 40 px square (at the screen's pixel ratio), never more.
        const side = Math.ceil(40 * Screen.devicePixelRatio);
        compare(Math.min(image.implicitWidth, image.implicitHeight) <= side, true);
        verify(image.implicitWidth > 0 && image.implicitHeight > 0);
    }

    // Anything that is not a local file loads as it is.
    function test_aResourceLoadsAsItIs() {
        const icon = "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg";
        const art = make(icon, "");
        tryCompare(art, "showing", "source");
        compare(findChild(art, "artworkImage").source.toString(), icon);
    }

    function test_aNewSourceIsTriedAgain() {
        const art = make(fileUrl(browseFixture.missingArtwork()), "");
        tryCompare(art, "sourceFailed", true);
        compare(art.showing, "");
        art.source = fileUrl(browseFixture.presentArtwork());
        tryCompare(art, "showing", "source");
    }
}
