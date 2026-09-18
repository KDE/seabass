// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Every case here is one where a wrong answer produces a file rekordbox
// imports without complaining and then quietly gets wrong: a path that
// resolves to the wrong file, a rating that reads as unrated, a hot cue
// that lands in the wrong slot.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/rekordbox_xml.hpp"
#include "domain/track.hpp"
#include "infrastructure/rekordbox/rekordbox_xml_writer.hpp"

using seabass::domain::CuePoint;
using seabass::domain::PlaylistMembership;
using seabass::domain::Track;
using seabass::domain::buildPlaylistTree;
using seabass::infrastructure::rekordbox::escapeXmlText;
using seabass::infrastructure::rekordbox::toRekordboxLocation;
using seabass::infrastructure::rekordbox::writeRekordboxXml;

namespace
{

Track track(std::string title, std::string artist, std::string path)
{
    Track t;
    t.format = "rekordbox";
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.filePath = std::move(path);
    t.durationSeconds = 349.0;
    t.bitrate = 320;
    return t;
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

size_t countOf(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

void testEscaping()
{
    assert(escapeXmlText("AT&T") == "AT&amp;T");
    assert(escapeXmlText("a<b>c") == "a&lt;b&gt;c");
    assert(escapeXmlText("say \"hi\"") == "say &quot;hi&quot;");
    // Real playlist names on this stick: "Amelie Lens' track IDs".
    assert(escapeXmlText("Amelie Lens' track IDs") == "Amelie Lens&apos; track IDs");
    // UTF-8 is not escaped -- "Einaudi 🌻" and "Tiësto" are valid XML text
    // as they stand, and escaping them would change the name rekordbox shows.
    assert(escapeXmlText("Einaudi 🌻") == "Einaudi 🌻");
    assert(escapeXmlText("Tiësto") == "Tiësto");

    // The case that actually broke a real export: a form feed inside an
    // ID3 artist frame ("A\xEF\xBF\xBD\x0C" on the WHALESHARK stick).
    // XML 1.0 cannot represent it at all, and one occurrence makes the
    // whole document unparseable -- rekordbox rejects the file with no
    // clue which of 1471 tracks caused it. Dropped, not escaped: there is
    // no legal escape for it.
    assert(escapeXmlText(std::string("A\x0C")) == "A");
    assert(escapeXmlText(std::string("a\x01" "\x02" "b")) == "ab");
    assert(escapeXmlText(std::string("a\x7F" "b")) == "ab");

    // Tab/LF/CR are legal, but attribute-value normalization would turn
    // them into spaces on parse. As character references they survive.
    assert(escapeXmlText("a\tb") == "a&#x9;b");
    assert(escapeXmlText("a\nb") == "a&#xA;b");

    // Malformed UTF-8 becomes U+FFFD rather than passing through and
    // breaking the parse. Tags read off real sticks do contain it.
    assert(escapeXmlText(std::string("a\xFF\xFE" "b")) == "a\xEF\xBF\xBD\xEF\xBF\xBD" "b");
    assert(escapeXmlText(std::string("\xC3")) == "\xEF\xBF\xBD");           // truncated sequence
    assert(escapeXmlText(std::string("\xC0\xAF")) == "\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong '/'
    // A valid 4-byte sequence (an emoji) is left exactly as it is.
    assert(escapeXmlText("🌻") == "🌻");
}

void testLocationEncoding()
{
    assert(toRekordboxLocation("/Users/s/Music/a.mp3") == "file://localhost/Users/s/Music/a.mp3");
    // A space is the common case and must be %20, not '+'.
    assert(toRekordboxLocation("/m/My Track.mp3") == "file://localhost/m/My%20Track.mp3");
    // '#' and '?' would otherwise start a fragment/query and truncate the
    // path -- rekordbox would look for a file that does not exist.
    assert(toRekordboxLocation("/m/A#1.mp3") == "file://localhost/m/A%231.mp3");
    assert(toRekordboxLocation("/m/What?.mp3") == "file://localhost/m/What%3F.mp3");
    // Non-ASCII goes out as percent-encoded UTF-8 bytes, byte for byte,
    // which is what preserves an NFD-decomposed macOS filename exactly as
    // the filesystem spells it.
    assert(toRekordboxLocation("/m/Tiësto.mp3") == "file://localhost/m/Ti%C3%ABsto.mp3");
    // Separators stay separators.
    assert(countOf(toRekordboxLocation("/a/b/c.mp3"), "/") == 5);
    // Windows: backslashes become slashes and the drive letter keeps a
    // leading slash, so the URL has its three.
    assert(toRekordboxLocation("C:\\Music\\a.mp3") == "file://localhost/C%3A/Music/a.mp3");
    assert(toRekordboxLocation("").empty());
}

void testCueNumberingAndColor()
{
    Track t = track("God In You", "Tinlicker", "/m/god.mp3");
    t.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 39634.0, "#FF0017", "", false, 0.0});
    t.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 9904.0, "", "", false, 0.0});
    t.cues.push_back(CuePoint{CuePoint::Kind::Hot, 2, 75259.0, "#00C4FF", "intro", true, 79259.0});

    const std::string xml = writeRekordboxXml({t}, buildPlaylistTree({t}));

    // Hot cue keeps its slot; position is seconds with millisecond precision.
    assert(contains(xml, "Type=\"0\" Start=\"39.634\" Num=\"1\" Red=\"255\" Green=\"0\" Blue=\"23\""));
    // A memory cue is Num="-1" -- writing 0 would make it hot cue A.
    assert(contains(xml, "Start=\"9.904\" Num=\"-1\""));
    // A loop is Type="4" and carries End.
    assert(contains(xml, "Type=\"4\" Start=\"75.259\" End=\"79.259\" Num=\"2\""));
    // An uncolored cue gets no color attributes at all, rather than black.
    assert(!contains(xml, "Red=\"0\" Green=\"0\" Blue=\"0\""));
}

void testRatingScale()
{
    Track t = track("Doppler", "Charlotte de Witte", "/m/doppler.mp3");
    t.rating = 4;
    const std::string xml = writeRekordboxXml({t}, buildPlaylistTree({t}));
    // rekordbox stores stars * 51. Writing "4" here reads back as barely
    // rated at all.
    assert(contains(xml, "Rating=\"204\""));
}

void testKindFollowsExtension()
{
    Track mp3 = track("a", "b", "/m/a.mp3");
    Track flac = track("c", "d", "/m/c.FLAC");
    Track m4a = track("e", "f", "/m/e.m4a");
    const std::string xml = writeRekordboxXml({mp3, flac, m4a}, buildPlaylistTree({mp3, flac, m4a}));
    assert(contains(xml, "Kind=\"MP3 File\""));
    assert(contains(xml, "Kind=\"FLAC File\""));  // case-insensitive
    assert(contains(xml, "Kind=\"M4A File\""));
}

void testPlaylistTreeNesting()
{
    Track a = track("a", "x", "/m/a.mp3");
    a.playlists.push_back(PlaylistMembership{"Techno/Peak Time", 1});
    Track b = track("b", "x", "/m/b.mp3");
    b.playlists.push_back(PlaylistMembership{"Techno/Peak Time", 0});

    const std::vector<Track> tracks{a, b};
    const std::string xml = writeRekordboxXml(tracks, buildPlaylistTree(tracks));

    assert(contains(xml, "<NODE Name=\"ROOT\" Type=\"0\""));
    assert(contains(xml, "<NODE Name=\"Techno\" Type=\"0\" Count=\"1\">"));
    assert(contains(xml, "<NODE Name=\"Peak Time\" Type=\"1\" KeyType=\"0\" Entries=\"2\">"));
    // Stored order wins over the order the tracks arrived in: b is at
    // position 0, so its key (2) comes first.
    const size_t first = xml.find("<TRACK Key=\"2\"/>");
    const size_t second = xml.find("<TRACK Key=\"1\"/>");
    assert(first != std::string::npos && second != std::string::npos);
    assert(first < second);
}

void testPositionMinusOneSortsLast()
{
    // -1 means the reader could not tell, which is weaker than any real
    // position. Sorting it as a number would put it before track 0.
    Track known = track("known", "x", "/m/known.mp3");
    known.playlists.push_back(PlaylistMembership{"Set", 0});
    Track unknown = track("unknown", "x", "/m/unknown.mp3");
    unknown.playlists.push_back(PlaylistMembership{"Set", -1});

    const std::vector<Track> tracks{unknown, known};
    const std::string xml = writeRekordboxXml(tracks, buildPlaylistTree(tracks));
    assert(xml.find("<TRACK Key=\"2\"/>") < xml.find("<TRACK Key=\"1\"/>"));
}

void testFolderThatAlsoHoldsTracks()
{
    // rekordbox has no node that is both. A path used as both has to come
    // out as a folder with a same-named playlist inside it, or the tracks
    // sitting directly in "Techno" are silently lost.
    Track inFolder = track("direct", "x", "/m/direct.mp3");
    inFolder.playlists.push_back(PlaylistMembership{"Techno", 0});
    Track nested = track("nested", "x", "/m/nested.mp3");
    nested.playlists.push_back(PlaylistMembership{"Techno/Peak Time", 0});

    const std::vector<Track> tracks{inFolder, nested};
    const std::string xml = writeRekordboxXml(tracks, buildPlaylistTree(tracks));

    assert(contains(xml, "<NODE Name=\"Techno\" Type=\"0\" Count=\"2\">"));
    assert(contains(xml, "<NODE Name=\"Techno\" Type=\"1\" KeyType=\"0\" Entries=\"1\">"));
    assert(contains(xml, "<NODE Name=\"Peak Time\" Type=\"1\" KeyType=\"0\" Entries=\"1\">"));
}

void testEmptyCollectionStillValid()
{
    const std::string xml = writeRekordboxXml({}, buildPlaylistTree({}));
    assert(contains(xml, "<COLLECTION Entries=\"0\">"));
    // The root must stay a folder even with nothing in it, or rekordbox
    // shows no playlist section at all.
    assert(contains(xml, "<NODE Name=\"ROOT\" Type=\"0\" Count=\"0\">"));
    assert(contains(xml, "</DJ_PLAYLISTS>"));
}

void testDeterministicOutput()
{
    Track a = track("a", "x", "/m/a.mp3");
    a.playlists.push_back(PlaylistMembership{"B/Two", 0});
    a.playlists.push_back(PlaylistMembership{"A/One", 0});
    const std::vector<Track> tracks{a};
    // Byte-identical across runs is what makes the output diffable and
    // these assertions meaningful at all.
    assert(writeRekordboxXml(tracks, buildPlaylistTree(tracks)) ==
           writeRekordboxXml(tracks, buildPlaylistTree(tracks)));
}

}  // namespace

int main()
{
    testEscaping();
    testLocationEncoding();
    testCueNumberingAndColor();
    testRatingScale();
    testKindFollowsExtension();
    testPlaylistTreeNesting();
    testPositionMinusOneSortsLast();
    testFolderThatAlsoHoldsTracks();
    testEmptyCollectionStillValid();
    testDeterministicOutput();
    std::cout << "rekordbox_xml_writer_test passed\n";
    return 0;
}
