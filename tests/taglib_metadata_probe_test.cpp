// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Exercises TagLibMetadataProbe against MP3s built byte by byte (see
// tests/mp3_fixture.hpp) rather than checked-in fixtures: the
// interesting cases are precisely the ones a normal encoder never
// produces on demand, above all a VBR stream with no Xing header.
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include "infrastructure/audio/taglib_metadata_probe.hpp"
#include "mp3_fixture.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::audio::TagLibMetadataProbe;
using namespace seabass::test_fixture::mp3;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_taglib_metadata_probe_test";
    fs::remove_all(root);
    fs::create_directories(root);

    TagLibMetadataProbe probe;

    // Case 1: a file with a Xing header reports a real length, and must
    // NOT be flagged estimated -- this is the case the unreferenced-file
    // cleanup is allowed to act on.
    {
        const int frames = 40;
        const fs::path path = root / "xing.mp3";
        writeMp3(path, frames, true);

        auto metadata = probe.read(path.string());
        assert(metadata.has_value());
        assert(!metadata->durationIsEstimated);
        assert(std::abs(metadata->durationSeconds - expectedSeconds(frames)) < 0.01);
        assert(metadata->bitrate == 128);
        assert(metadata->sampleRate == SampleRate);
        std::cout << "case 1 (Xing header -> exact duration, not estimated) OK\n";
    }

    // Case 2: the same stream without a Xing header. TagLib falls back to
    // size/bitrate, which happens to be close here because every frame is
    // the same size -- the point is not the number but the flag, since a
    // real VBR file would be seconds out and would then be excluded from
    // deletion. See FileMetadata::durationIsEstimated.
    {
        const int frames = 40;
        const fs::path path = root / "plain.mp3";
        writeMp3(path, frames, false);

        auto metadata = probe.read(path.string());
        assert(metadata.has_value());
        assert(metadata->durationIsEstimated);
        assert(metadata->durationSeconds > 0.0);
        assert(metadata->bitrate == 128);
        std::cout << "case 2 (no Xing header -> duration flagged estimated) OK\n";
    }

    // Case 3: tags come back. Written with TagLib itself, so this tests
    // our own plumbing (field mapping, UTF-8 conversion) rather than
    // re-testing TagLib's ID3 writer. The non-ASCII title is deliberate:
    // TagLib::String::to8Bit() defaults to Latin-1 and would mangle it.
    {
        const fs::path path = root / "tagged.mp3";
        writeMp3(path, 40, true);
        {
            TagLib::FileRef file(path.string().c_str());
            assert(!file.isNull());
            file.tag()->setTitle(TagLib::String("Una Hora M\xc3\xa1s", TagLib::String::UTF8));
            file.tag()->setArtist("The Rocketman");
            file.tag()->setAlbum("Prototype EP");
            assert(file.save());
        }

        auto metadata = probe.read(path.string());
        assert(metadata.has_value());
        assert(metadata->title == "Una Hora M\xc3\xa1s");
        assert(metadata->artist == "The Rocketman");
        assert(metadata->album == "Prototype EP");
        // Tagging must not have cost us the audio properties.
        assert(metadata->durationSeconds > 0.0);
        assert(metadata->bitrate == 128);
        std::cout << "case 3 (title/artist/album round-trip, UTF-8 intact) OK\n";
    }

    // Case 4: an untagged file is a successful read with empty strings,
    // not a failure -- callers still want its duration and bitrate.
    {
        const fs::path path = root / "untagged.mp3";
        writeMp3(path, 40, true);

        auto metadata = probe.read(path.string());
        assert(metadata.has_value());
        assert(metadata->title.empty());
        assert(metadata->artist.empty());
        assert(metadata->durationSeconds > 0.0);
        std::cout << "case 4 (no tags -> empty strings, properties still read) OK\n";
    }

    // Case 5: the two ways a real stick disappoints us -- a file that is
    // not audio at all, and a path that is not there. Both are ordinary
    // answers (nullopt), never exceptions, because a catalog row pointing
    // at a deleted file is normal.
    {
        writeBytes(root / "notaudio.mp3", std::vector<unsigned char>{'n', 'o', 'p', 'e'});
        assert(!probe.read((root / "notaudio.mp3").string()).has_value());
        assert(!probe.read((root / "does-not-exist.mp3").string()).has_value());
        std::cout << "case 5 (garbage file and missing file -> nullopt) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all taglib_metadata_probe_test cases passed\n";
    return 0;
}
