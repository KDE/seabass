// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// TagLibTagReader against audio files built byte by byte: an MP3, a WAV
// and an AIFF with ID3v2.3 frames, a FLAC with Vorbis comments, an MP4
// with its atoms. Each tag is planted under the name its format gives
// it, and the reader must hand back what was planted: the frame and atom
// names are the format's, written out here, not TagLib's word for them.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "audio_file_fixture.hpp"
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#include "infrastructure/audio/taglib_tag_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::domain::bpmFromTag;
using seabass::domain::TrackTags;
using seabass::infrastructure::audio::TagLibTagReader;
using namespace seabass::test_fixture::audio;
namespace fs = std::filesystem;

namespace
{

// An ID3v2.3 text frame in Latin-1: the four-character id, a 32-bit
// big-endian body size, two flag bytes, the encoding byte (0), the text.
std::string textFrame(const std::string &id, const std::string &text)
{
    const std::string body = std::string(1, '\0') + text;
    return id + be32(body.size()) + std::string(2, '\0') + body;
}

// The same in UTF-16 with a byte order mark (encoding 1), as taggers
// write a title Latin-1 cannot spell. `utf16le` is the text's code units,
// little-endian.
std::string textFrameUtf16(const std::string &id, const std::string &utf16le)
{
    const std::string body = std::string(1, '\x01') + "\xff\xfe" + utf16le;
    return id + be32(body.size()) + std::string(2, '\0') + body;
}

// A COMM frame: encoding (0), a three-letter language, a NUL-terminated
// description, the comment. A comment a person typed has no description;
// iTunes files its own data under one ("iTunNORM").
std::string commentFrame(const std::string &description, const std::string &text)
{
    const std::string body = std::string(1, '\0') + "eng" + description + '\0' + text;
    return "COMM" + be32(body.size()) + std::string(2, '\0') + body;
}

// An MP4 text item: the atom named for the tag, holding a data atom of
// well-known type 1 (UTF-8).
std::string mp4Text(const std::string &name, const std::string &value)
{
    return atom(name, dataAtom(1, value));
}

bool nothingIn(const TrackTags &tags)
{
    return tags.title.empty() && tags.artist.empty() && tags.album.empty() && tags.albumArtist.empty()
        && tags.genre.empty() && tags.comment.empty() && tags.composer.empty() && tags.label.empty()
        && tags.remixer.empty() && tags.key.empty() && tags.year == 0 && tags.trackNumber == 0 && tags.bpm == 0.0;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_taglib_tag_reader_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto at = [&root](const std::string &name) {
        return seabass::pathToUtf8(root / seabass::pathFromUtf8(name));
    };
    TagLibTagReader reader;

    // 1. An MP3 with every ID3v2.3 frame the import reads. The track
    //    number is the part before the slash, the comment is the one
    //    without a description, and spaces at either end are not part of
    //    a tag.
    {
        writeFile(root / "full.mp3",
                  id3Tag({textFrame("TIT2", "Song Title "), textFrame("TPE1", "Some Artist"),
                          textFrame("TALB", "The Album"), textFrame("TPE2", "Various Artists"),
                          textFrame("TCON", "Tech House"), textFrame("TYER", "2019"), textFrame("TRCK", "3/12"),
                          commentFrame("iTunNORM", " 0000 0000"), commentFrame("", "bought in Rotterdam"),
                          textFrame("TCOM", "A Composer"), textFrame("TPUB", "A Label"),
                          textFrame("TPE4", "A Remixer"), textFrame("TKEY", "8A"), textFrame("TBPM", "127.97")})
                      + mp3Frames(4));
        const auto tags = reader.read(at("full.mp3"));
        assert(tags.has_value());
        assert(tags->title == "Song Title");
        assert(tags->artist == "Some Artist");
        assert(tags->album == "The Album");
        assert(tags->albumArtist == "Various Artists");
        assert(tags->genre == "Tech House");
        assert(tags->year == 2019);
        assert(tags->trackNumber == 3);
        assert(tags->comment == "bought in Rotterdam");
        assert(tags->composer == "A Composer");
        assert(tags->label == "A Label");
        assert(tags->remixer == "A Remixer");
        assert(tags->key == "8A");
        assert(std::abs(tags->bpm - 127.97) < 1e-9);
        std::cout << "case 1 (MP3, ID3v2.3: every frame the import reads) OK\n";
    }

    // 2. Text Latin-1 cannot spell arrives as UTF-8: "Kölsch" from
    //    UTF-16 code units.
    {
        const std::string koelsch("K\0\xf6\0l\0s\0c\0h\0", 12);
        writeFile(root / "utf16.mp3", id3Tag({textFrameUtf16("TPE1", koelsch)}) + mp3Frames(4));
        const auto tags = reader.read(at("utf16.mp3"));
        assert(tags.has_value());
        assert(tags->artist == "K\xc3\xb6lsch");
        std::cout << "case 2 (UTF-16 text comes back as UTF-8) OK\n";
    }

    // 3. A FLAC's Vorbis comments, under the names taggers use: a full
    //    date gives its year, KEY and ORGANIZATION are read where
    //    INITIALKEY and LABEL are absent, and the first name wins where
    //    both are there.
    {
        writeFile(root / "full.flac",
                  flacFile({{4, vorbisComments({"TITLE=Flac Title", "ARTIST=Flac Artist", "ALBUM=Flac Album",
                                                "ALBUMARTIST=Flac Album Artist", "GENRE=Drum & Bass",
                                                "DATE=2021-03-05", "TRACKNUMBER=07", "COMMENT=a comment",
                                                "COMPOSER=Flac Composer", "ORGANIZATION=Flac Label",
                                                "REMIXER=Flac Remixer", "KEY=F#m", "BPM=174"})}}));
        auto tags = reader.read(at("full.flac"));
        assert(tags.has_value());
        assert(tags->title == "Flac Title" && tags->artist == "Flac Artist" && tags->album == "Flac Album");
        assert(tags->albumArtist == "Flac Album Artist");
        assert(tags->genre == "Drum & Bass");
        assert(tags->year == 2021);
        assert(tags->trackNumber == 7);
        assert(tags->comment == "a comment");
        assert(tags->composer == "Flac Composer");
        assert(tags->label == "Flac Label");
        assert(tags->remixer == "Flac Remixer");
        assert(tags->key == "F#m");
        assert(tags->bpm == 174.0);

        writeFile(root / "both.flac",
                  flacFile({{4, vorbisComments({"KEY=wrong", "INITIALKEY=11B", "ORGANIZATION=wrong", "LABEL=Right",
                                                "BPM=fast"})}}));
        tags = reader.read(at("both.flac"));
        assert(tags.has_value());
        assert(tags->key == "11B");
        assert(tags->label == "Right");
        assert(tags->bpm == 0.0);  // not a number: no tempo, not a guess
        std::cout << "case 3 (FLAC, Vorbis comments) OK\n";
    }

    // 4. An MP4's atoms: the track number and the tempo are binary there
    //    (trkn: two reserved bytes, the number, the total; tmpo: a 16-bit
    //    integer, data type 21).
    {
        const std::string trkn = atom("trkn", dataAtom(0, be16(0) + be16(5) + be16(11) + be16(0)));
        const std::string tmpo = atom("tmpo", dataAtom(21, be16(126)));
        writeFile(root / "full.m4a",
                  mp4File(mp4Text("\xa9nam", "Mp4 Title") + mp4Text("\xa9" "ART", "Mp4 Artist")
                          + mp4Text("\xa9" "alb", "Mp4 Album") + mp4Text("aART", "Mp4 Album Artist")
                          + mp4Text("\xa9gen", "House") + mp4Text("\xa9" "day", "2017")
                          + mp4Text("\xa9" "cmt", "mp4 comment") + mp4Text("\xa9wrt", "Mp4 Composer") + trkn + tmpo));
        const auto tags = reader.read(at("full.m4a"));
        assert(tags.has_value());
        assert(tags->title == "Mp4 Title" && tags->artist == "Mp4 Artist" && tags->album == "Mp4 Album");
        assert(tags->albumArtist == "Mp4 Album Artist");
        assert(tags->genre == "House");
        assert(tags->year == 2017);
        assert(tags->comment == "mp4 comment");
        assert(tags->composer == "Mp4 Composer");
        assert(tags->trackNumber == 5);
        assert(tags->bpm == 126.0);
        std::cout << "case 4 (MP4 atoms) OK\n";
    }

    // 5. WAV and AIFF keep an ID3v2 tag in a chunk of their own.
    {
        const std::string id3 = id3Tag({textFrame("TIT2", "Chunk Title"), textFrame("TCON", "Techno"),
                                        textFrame("TBPM", "132"), textFrame("TKEY", "Am")});
        writeFile(root / "tagged.wav", wavFile(id3));
        writeFile(root / "tagged.aiff", aiffFile(id3));
        for (const char *name : {"tagged.wav", "tagged.aiff"}) {
            const auto tags = reader.read(at(name));
            assert(tags.has_value());
            assert(tags->title == "Chunk Title");
            assert(tags->genre == "Techno");
            assert(tags->bpm == 132.0);
            assert(tags->key == "Am");
        }
        // The sample size is the container's: the fixture's 16 bits, and
        // 24 once its format chunk says so.
        assert(seabass::infrastructure::audio::bitsPerSampleOf(at("tagged.wav")) == 16);
        std::string deep = wavFile(id3);
        const std::size_t format = deep.find("fmt ");
        assert(format != std::string::npos && static_cast<unsigned char>(deep[format + 22]) == 16);
        deep[format + 22] = 24;
        writeFile(root / "deep.wav", deep);
        assert(seabass::infrastructure::audio::bitsPerSampleOf(at("deep.wav")) == 24);
        assert(seabass::infrastructure::audio::bitsPerSampleOf(at("full.mp3")) == 0);
        std::cout << "case 5 (WAV and AIFF, ID3v2 in a chunk; the sample size of a WAV) OK\n";
    }

    // 6. A file with no tags reads as a track nothing is known about; a
    //    file that is not there, or is not audio at all, does not read.
    {
        writeFile(root / "untagged.mp3", mp3Frames(4));
        const auto tags = reader.read(at("untagged.mp3"));
        assert(tags.has_value() && nothingIn(*tags));

        assert(!reader.read(at("missing.mp3")).has_value());
        writeFile(root / "notes.txt", "not audio");
        assert(!reader.read(at("notes.txt")).has_value());
        // Audio by its name only: TagLib opens it and finds no stream.
        writeFile(root / "garbage.flac", "this is not a FLAC stream, only its name says so");
        assert(!reader.read(at("garbage.flac")).has_value());
        std::cout << "case 6 (no tags is an empty answer, no file is no answer) OK\n";
    }

    // 6b. A file that has only
    // iTunes's own volume data under COMM has no comment a person wrote.
    {
        writeFile(root / "itunes.mp3",
                  id3Tag({textFrame("TIT2", "From iTunes"), commentFrame("iTunNORM", " 00000A2B 00000A2B")})
                      + mp3Frames(4));
        const auto tags = reader.read(at("itunes.mp3"));
        assert(tags.has_value());
        assert(tags->title == "From iTunes");
        assert(tags->comment.empty());
        std::cout << "case 6b (iTunes's volume data is not a comment) OK\n";
    }

    // 7. What counts as a tempo.
    {
        assert(bpmFromTag("128") == 128.0);
        assert(bpmFromTag(" 128.00 ") == 128.0);
        assert(std::abs(bpmFromTag("127,97") - 127.97) < 1e-9);
        assert(bpmFromTag(std::string("140\0", 4)) == 140.0);
        assert(bpmFromTag("") == 0.0);
        assert(bpmFromTag("fast") == 0.0);
        assert(bpmFromTag("120-125") == 0.0);
        assert(bpmFromTag("128.") == 0.0);
        assert(bpmFromTag(".5") == 0.0);
        assert(bpmFromTag("-128") == 0.0);
        assert(bpmFromTag("128 bpm") == 0.0);
        assert(bpmFromTag("1.2.3") == 0.0);
        assert(bpmFromTag("0") == 0.0);
        assert(bpmFromTag("19.99") == 0.0);
        assert(bpmFromTag("20") == 20.0);
        assert(bpmFromTag("999.9") == 999.9);
        assert(bpmFromTag("1000") == 0.0);
        assert(bpmFromTag("12800") == 0.0);
        std::cout << "case 7 (what counts as a tempo) OK\n";
    }

    fs::remove_all(root);
    std::cout << "ALL TAG READER CASES PASSED\n";
    return 0;
}
