// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::describeImportedTrack: the one description of an imported
// track that the three library writers are filled from. The numbers it
// derives are rekordbox 7.2.18's, read off the T73 export's rows for
// files whose own properties are known (180 s and 240 s clicks, FLAC
// 24 bit, AIFF 48 kHz 16 bit, MP3 320).

#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "domain/import_field_checks.hpp"
#include "domain/imported_track.hpp"

using namespace seabass::domain;

namespace
{

template <typename Call>
bool refuses(Call &&call)
{
    try {
        call();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

}  // namespace

int main()
{
    // 1. A tagged file: every text as the tag has it, trimmed.
    {
        TrackTags tags;
        tags.title = "  A Title \r\n";
        tags.artist = "DJ X ";
        tags.album = "\tAn Album";
        tags.albumArtist = "Various";
        tags.genre = "Techno";
        tags.label = "A Label";
        tags.remixer = "A Remixer";
        tags.composer = "A Composer";
        tags.key = " 8A ";
        tags.comment = "line one\nline two\n";
        tags.bpm = 127.97;
        tags.year = 1997;
        tags.trackNumber = 7;
        AudioFormat audio;
        audio.durationSeconds = 301.4;
        audio.bitrate = 320;
        audio.sampleRateHz = 44100;
        audio.fileSizeBytes = 12345678;
        const ImportedTrack t = describeImportedTrack(tags, audio, "/Contents/DJ X/An Album/07 A Title.mp3", "2026-11-24");
        assert(t.stickPath == "/Contents/DJ X/An Album/07 A Title.mp3" && t.date == "2026-11-24");
        assert(t.title == "A Title" && t.artist == "DJ X" && t.album == "An Album" && t.albumArtist == "Various");
        assert(t.genre == "Techno" && t.label == "A Label" && t.remixer == "A Remixer" && t.composer == "A Composer");
        assert(t.key == "8A" && "as the tag spells it");
        assert(t.comment == "line one\nline two" && "a line end inside stays, the one at the end goes");
        assert(t.bpm == 127.97 && t.year == 1997 && t.trackNumber == 7);
        assert(t.lengthSeconds == 301 && t.durationSeconds == 301.4);
        assert(t.bitrate == 320 && t.bitDepth == 16 && t.sampleRateHz == 44100 && t.fileSizeBytes == 12345678);
        for (const std::string *text : {&t.title, &t.artist, &t.album, &t.albumArtist, &t.genre, &t.label, &t.remixer,
                                        &t.composer, &t.key, &t.comment, &t.stickPath}) {
            assert(isStorableText(*text) && "what the row writers ask of a text");
        }
    }
    std::cout << "case 1 (a tagged file: the tag's texts, trimmed) OK\n";

    // 2. rekordbox's numbers for the T73 files.
    {
        const auto described = [](const char *path, double seconds, int bitrate, int bits, int rate) {
            AudioFormat audio;
            audio.durationSeconds = seconds;
            audio.bitrate = bitrate;
            audio.bitsPerSample = bits;
            audio.sampleRateHz = rate;
            return describeImportedTrack({}, audio, path, "2026-11-13");
        };
        // B07: FLAC, exactly 180 s, 24 bit. rekordbox: length 179, bitrate 0, depth 24.
        const ImportedTrack flac = described("/Contents/Seabass Test/T73 Base/T73-B07-click-44k-114bpm.flac", 180.0, 1411, 24, 44100);
        assert(flac.lengthSeconds == 179 && flac.bitrate == 0 && flac.bitDepth == 24 && flac.sampleRateHz == 44100);
        assert(flac.title == "T73-B07-click-44k-114bpm" && "an untagged file is titled by its name, as rekordbox does");
        // B04: MP3 320, 180.036 s. rekordbox: length 180, bitrate 320, depth 16.
        const ImportedTrack mp3 = described("/Contents/Seabass Test/T73 Base/T73-B04-click-44k-108bpm.mp3", 180.035918, 320, 0, 44100);
        assert(mp3.lengthSeconds == 180 && mp3.bitrate == 320 && mp3.bitDepth == 16);
        // B11: AIFF 48 kHz 16 bit, 180 s. rekordbox: length 179, bitrate 1536, depth 16.
        const ImportedTrack aiff = described("/Contents/Seabass Test/T73 Base/T73-B11-click-48k-122bpm.aiff", 180.0, 1536, 16, 48000);
        assert(aiff.lengthSeconds == 179 && aiff.bitrate == 1536 && aiff.bitDepth == 16 && aiff.sampleRateHz == 48000);
        // N1: FLAC 48 kHz, 240 s: 239.
        assert(described("/Contents/Seabass Test/T73/T73-N1-click-48k-120bpm.flac", 240.0, 0, 24, 48000).lengthSeconds == 239);
        // A lossy file is 16 bit whatever a reader made of it; a FLAC in
        // capitals is still a FLAC.
        assert(described("/Contents/a.m4a", 10.5, 256, 24, 44100).bitDepth == 16);
        assert(described("/Contents/a.FLAC", 10.5, 900, 16, 44100).bitrate == 0);
        // A lossless file whose depth was not read: 16, rather than 0.
        assert(described("/Contents/a.wav", 10.5, 1411, 0, 44100).bitDepth == 16);
    }
    std::cout << "case 2 (length, bit rate and depth as rekordbox wrote them for the T73 files) OK\n";

    // 3. What the file does not say, or says in a way no row can hold.
    {
        TrackTags tags;
        tags.title = "   ";
        tags.artist = "caf\xc3";               // cut in the middle of a character
        tags.album = std::string("a\0b", 3);  // a NUL inside
        tags.genre = "\xff\xfe";
        tags.bpm = 5000.0;
        tags.year = 20260;
        tags.trackNumber = -3;
        AudioFormat audio;
        audio.durationSeconds = std::numeric_limits<double>::quiet_NaN();
        audio.bitrate = -1;
        audio.sampleRateHz = -44100;
        const ImportedTrack t = describeImportedTrack(tags, audio, "/Contents/UnknownArtist/UnknownAlbum/caf\xc3\xa9.wav", "2026-11-24");
        assert(t.title == "caf\xc3\xa9" && "a blank title is no title: the file's name");
        assert(t.artist.empty() && t.album.empty() && t.genre.empty() && "broken text is absent, not stored broken");
        assert(t.bpm == 0.0 && t.year == 0 && t.trackNumber == 0);
        assert(t.lengthSeconds == 0 && t.durationSeconds == 0.0 && t.bitrate == 0 && t.sampleRateHz == 0);
        // Names with more or fewer dots than one.
        assert(describeImportedTrack({}, {}, "/Contents/a.b.c.mp3", "2026-11-24").title == "a.b.c");
        assert(describeImportedTrack({}, {}, "/Contents/noextension", "2026-11-24").title == "noextension");
        assert(describeImportedTrack({}, {}, "/Contents/.flac", "2026-11-24").title == ".flac");
        // A length just above a whole second, and a sliver.
        AudioFormat tiny;
        tiny.durationSeconds = 0.25;
        assert(describeImportedTrack({}, tiny, "/Contents/a.wav", "2026-11-24").lengthSeconds == 0);
        tiny.durationSeconds = 1.0;
        assert(describeImportedTrack({}, tiny, "/Contents/a.wav", "2026-11-24").lengthSeconds == 0);
        tiny.durationSeconds = 1.001;
        assert(describeImportedTrack({}, tiny, "/Contents/a.wav", "2026-11-24").lengthSeconds == 1);
        tiny.durationSeconds = std::numeric_limits<double>::infinity();
        const ImportedTrack endless = describeImportedTrack({}, tiny, "/Contents/a.wav", "2026-11-24");
        assert(endless.lengthSeconds == 0 && endless.durationSeconds == 0.0 && "no length is not an endless one");
        // The edges of what is kept.
        tags = {};
        tags.bpm = 999.99;
        tags.year = 9999;
        tags.trackNumber = 9999;
        const ImportedTrack edge = describeImportedTrack(tags, {}, "/Contents/a.wav", "2026-11-24");
        assert(edge.bpm == 999.99 && edge.year == 9999 && edge.trackNumber == 9999);
        tags.bpm = 1000.0;
        tags.year = 10000;
        tags.trackNumber = 10000;
        const ImportedTrack past = describeImportedTrack(tags, {}, "/Contents/a.wav", "2026-11-24");
        assert(past.bpm == 0.0 && past.year == 0 && past.trackNumber == 0);
    }
    std::cout << "case 3 (blank, broken and impossible values are absent; the title falls back to the file's name) OK\n";

    // 4. The caller's own values are refused when wrong, not repaired.
    {
        for (const char *path : {"", "/", "Contents/a.mp3", "/Contents/", "/Contents/a.mp3 ", "/Contents/caf\xc3.mp3",
                                 "/Contents/../a.mp3", "/../a.mp3", "//Contents/a.mp3", "/Contents//a.mp3",
                                 "/Contents/./a.mp3", "/Contents\\a.mp3", "/Contents/.."}) {
            assert(refuses([&] { (void)describeImportedTrack({}, {}, path, "2026-11-24"); }));
        }
        for (const char *date : {"", "24-11-2026", "2026-02-30", "2026-11-24T10:00"}) {
            assert(refuses([&] { (void)describeImportedTrack({}, {}, "/Contents/a.mp3", date); }));
        }
    }
    std::cout << "case 4 (a path that is not one, a date that is not a day: refused) OK\n";

    // 5. storableText on its own.
    assert(storableText(" \t a b \r\n") == "a b" && storableText("") == "" && storableText(" \n ") == "");
    assert(storableText(std::string("\0x\0", 3)) == "x" && storableText(std::string("x\0y", 3)) == "");
    assert(storableText("\xc3\xa9 ") == "\xc3\xa9" && storableText("\xc3") == "");
    std::cout << "imported_track_test: all cases passed\n";
    return 0;
}
