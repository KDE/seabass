// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// readEmbeddedArtwork() against audio files built byte by byte: FLAC with
// PICTURE blocks, MP4 with a covr atom, MP3/AIFF/WAV with an ID3v2 APIC
// frame, and Ogg Vorbis/Opus with a METADATA_BLOCK_PICTURE comment. The
// cover inside a track's own tags is the last source of art a stick
// still has when both libraries lost theirs (see embedded_artwork.hpp),
// and until this test only the MP3 path had ever run.
//
// Every fixture is a few hundred bytes, written here with each field
// named, so what the reader is handed is visible in this file rather
// than in a binary blob. The planted picture is a real 1x1 PNG; the
// reader must hand back exactly those bytes.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/audio/embedded_artwork.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "audio_file_fixture.hpp"
#include "scratch_path.hpp"

using seabass::infrastructure::audio::hasEmbeddedArtwork;
using seabass::infrastructure::audio::readEmbeddedArtwork;
using namespace seabass::test_fixture::audio;
namespace fs = std::filesystem;

namespace
{

// The smallest valid PNG: 1x1, RGBA, one transparent pixel.
const std::string Png1x1 = std::string(
    "\x89PNG\r\n\x1a\n"                                  // signature
    "\x00\x00\x00\x0d" "IHDR"                             // IHDR, 13 bytes
    "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00" // 1x1, 8 bit, RGBA
    "\x1f\x15\xc4\x89"                                    // CRC
    "\x00\x00\x00\x0a" "IDAT"                             // IDAT, 10 bytes
    "\x78\x9c\x63\x00\x01\x00\x00\x05\x00\x01"            // zlib stream
    "\x0d\x0a\x2d\xb4"                                    // CRC
    "\x00\x00\x00\x00" "IEND" "\xae\x42\x60\x82",         // IEND + CRC
    67);

// A second, different picture, to tell "front cover" from "the other
// one". The reader never decodes images, so these bytes need not be one.
const std::string BackCover = "back cover, not the one a player shows";

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_embedded_artwork_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto at = [&root](const std::string &name) { return seabass::pathToUtf8(root / seabass::pathFromUtf8(name)); };

    // Case 1: FLAC with one front-cover PICTURE block.
    {
        writeFile(root / "front.flac", flacFile({{6, flacPictureBody(3, Png1x1)}}));
        assert(readEmbeddedArtwork(at("front.flac")) == Png1x1);
        assert(hasEmbeddedArtwork(at("front.flac")));
        std::cout << "case 1 (FLAC PICTURE block, front cover) OK\n";
    }

    // Case 2: a back cover first, then the front: the front wins, the way
    // a player picks. Upper-case extension, which the reader lowers.
    {
        writeFile(root / "both.FLAC",
                  flacFile({{6, flacPictureBody(4, BackCover)}, {6, flacPictureBody(3, Png1x1)}}));
        assert(readEmbeddedArtwork(at("both.FLAC")) == Png1x1);
        std::cout << "case 2 (FLAC back then front -> front) OK\n";
    }

    // Case 3: only a back cover: taken, since it is all there is.
    {
        writeFile(root / "back.flac", flacFile({{6, flacPictureBody(4, BackCover)}}));
        assert(readEmbeddedArtwork(at("back.flac")) == BackCover);
        std::cout << "case 3 (FLAC back cover only -> taken) OK\n";
    }

    // Case 4: a FLAC with a PADDING block and no picture: nothing.
    {
        writeFile(root / "none.flac", flacFile({{1, std::string(16, '\0')}}));
        assert(readEmbeddedArtwork(at("none.flac")).empty());
        assert(!hasEmbeddedArtwork(at("none.flac")));
        std::cout << "case 4 (FLAC without a picture -> none) OK\n";
    }

    // Case 5: a FLAC with no PICTURE block but an ID3v2 tag in front of
    // "fLaC", as some taggers write: the APIC there is the fallback.
    {
        writeFile(root / "id3.flac", id3Tag({apicFrame(3, Png1x1)}) + flacFile({}));
        assert(readEmbeddedArtwork(at("id3.flac")) == Png1x1);
        std::cout << "case 5 (FLAC with the cover only in ID3v2 -> found) OK\n";
    }

    // Case 6: not a FLAC stream at all behind the extension: none, no crash.
    {
        writeFile(root / "garbage.flac", "this is not a FLAC stream, only its name says so");
        assert(readEmbeddedArtwork(at("garbage.flac")).empty());
        std::cout << "case 6 (garbage named .flac -> none) OK\n";
    }

    // Case 7: MP4 with a PNG in moov/udta/meta/ilst/covr.
    {
        writeFile(root / "cover.m4a", mp4File(atom("covr", dataAtom(14, Png1x1))));
        assert(readEmbeddedArtwork(at("cover.m4a")) == Png1x1);
        std::cout << "case 7 (MP4 covr atom) OK\n";
    }

    // Case 8: a covr whose first data atom is empty and whose second
    // holds the picture: the empty one is passed over. .mp4 and .m4b go
    // the same way as .m4a.
    {
        const std::string covr = atom("covr", dataAtom(14, "") + dataAtom(14, Png1x1));
        writeFile(root / "second.mp4", mp4File(covr));
        writeFile(root / "second.m4b", mp4File(covr));
        assert(readEmbeddedArtwork(at("second.mp4")) == Png1x1);
        assert(readEmbeddedArtwork(at("second.m4b")) == Png1x1);
        std::cout << "case 8 (MP4 empty cover skipped, .mp4 and .m4b) OK\n";
    }

    // Case 9: an MP4 with tags but no covr item: none.
    {
        writeFile(root / "nocover.m4a", mp4File(atom("\xa9nam", dataAtom(1, "A title"))));
        assert(readEmbeddedArtwork(at("nocover.m4a")).empty());
        std::cout << "case 9 (MP4 without covr -> none) OK\n";
    }

    // Case 10: an only-empty covr: none.
    {
        writeFile(root / "emptycover.m4a", mp4File(atom("covr", dataAtom(14, ""))));
        assert(readEmbeddedArtwork(at("emptycover.m4a")).empty());
        std::cout << "case 10 (MP4 covr with no bytes -> none) OK\n";
    }

    // Case 11: an atom inside moov whose size (4) is smaller than its own
    // header: the file is refused, none, no crash.
    {
        std::string bytes = mp4File(atom("covr", dataAtom(14, Png1x1)));
        const std::size_t udta = bytes.find("udta");
        assert(udta != std::string::npos);
        bytes.replace(udta - 4, 4, be32(4));
        writeFile(root / "malformed.m4a", bytes);
        assert(readEmbeddedArtwork(at("malformed.m4a")).empty());
        std::cout << "case 11 (MP4 atom size below its header -> none) OK\n";
    }

    // Case 12: MP3 with a back cover, then the front: the front wins; and
    // one with only the back cover still gives it.
    {
        writeFile(root / "both.mp3", id3Tag({apicFrame(4, BackCover), apicFrame(3, Png1x1)}) + mp3Frames(4));
        writeFile(root / "back.mp3", id3Tag({apicFrame(4, BackCover)}) + mp3Frames(4));
        writeFile(root / "untagged.mp3", mp3Frames(4));
        assert(readEmbeddedArtwork(at("both.mp3")) == Png1x1);
        assert(readEmbeddedArtwork(at("back.mp3")) == BackCover);
        assert(readEmbeddedArtwork(at("untagged.mp3")).empty());
        std::cout << "case 12 (MP3 APIC front preferred, back as fallback, none untagged) OK\n";
    }

    // Case 13: AIFF and WAV keep their ID3v2 tag in a chunk, not at the
    // start of the file.
    {
        const std::string tag = id3Tag({apicFrame(3, Png1x1)});
        writeFile(root / "cover.aiff", aiffFile(tag));
        writeFile(root / "cover.aif", aiffFile(tag));
        writeFile(root / "cover.wav", wavFile(tag));
        assert(readEmbeddedArtwork(at("cover.aiff")) == Png1x1);
        assert(readEmbeddedArtwork(at("cover.aif")) == Png1x1);
        assert(readEmbeddedArtwork(at("cover.wav")) == Png1x1);
        std::cout << "case 13 (AIFF and WAV ID3 chunk) OK\n";
    }

    // Case 14: Ogg Vorbis and Opus carry the cover as a base64
    // METADATA_BLOCK_PICTURE comment.
    {
        writeFile(root / "cover.ogg", oggVorbisFile({"TITLE=A title", pictureComment(3, Png1x1)}));
        writeFile(root / "cover.opus", oggOpusFile({pictureComment(3, Png1x1)}));
        writeFile(root / "nocover.ogg", oggVorbisFile({"TITLE=A title"}));
        assert(readEmbeddedArtwork(at("cover.ogg")) == Png1x1);
        assert(readEmbeddedArtwork(at("cover.opus")) == Png1x1);
        assert(readEmbeddedArtwork(at("nocover.ogg")).empty());
        std::cout << "case 14 (Ogg Vorbis and Opus picture comment) OK\n";
    }

    // Case 15: what is not asked of it: an empty path, a missing file, and
    // an extension it does not read.
    {
        writeFile(root / "cover.txt", Png1x1);
        assert(readEmbeddedArtwork("").empty());
        assert(readEmbeddedArtwork(at("missing.flac")).empty());
        assert(readEmbeddedArtwork(at("cover.txt")).empty());
        std::cout << "case 15 (empty path, missing file, unknown extension -> none) OK\n";
    }

    std::cout << "embedded_artwork_test: all cases passed\n";
    return 0;
}
