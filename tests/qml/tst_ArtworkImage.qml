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

    function fileUrl(path) {
        return "file://" + path;
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
        compare(image.visible, true);
        compare(image.status, Image.Ready);
    }

    // The case the readers leave to it: an Engine row naming art that is
    // not on the stick, and the rekordbox copy's art that is.
    function test_aMissingSourceShowsTheFallback() {
        const art = make(fileUrl(browseFixture.missingArtwork()), fileUrl(browseFixture.presentArtwork()));
        tryCompare(art, "showing", "fallback");
        const image = findChild(art, "artworkImage");
        compare(image.visible, true);
        compare(image.status, Image.Ready);
        compare(image.source.toString(), fileUrl(browseFixture.presentArtwork()));
    }

    function test_anEmptySourceShowsTheFallback() {
        const art = make("", fileUrl(browseFixture.presentArtwork()));
        tryCompare(art, "showing", "fallback");
        compare(findChild(art, "artworkImage").visible, true);
    }

    // Nothing drawn at all, not a broken-image frame: the row's own
    // placeholder behind it is what shows.
    function test_bothMissingShowNothing() {
        const art = make(fileUrl(browseFixture.missingArtwork()), fileUrl(browseFixture.missingArtwork() + ".png"));
        const image = findChild(art, "artworkImage");
        tryCompare(art, "sourceFailed", true, 5000, "the source was tried first");
        tryCompare(image, "status", Image.Error);
        compare(image.source.toString(), fileUrl(browseFixture.missingArtwork() + ".png"), "then the fallback");
        compare(image.visible, false);
        compare(art.showing, "");
    }

    function test_aNewSourceIsTriedAgain() {
        const art = make(fileUrl(browseFixture.missingArtwork()), "");
        tryCompare(art, "sourceFailed", true);
        compare(art.showing, "");
        art.source = fileUrl(browseFixture.presentArtwork());
        tryCompare(art, "showing", "source");
    }
}
