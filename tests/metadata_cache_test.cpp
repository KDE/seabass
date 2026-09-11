// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <chrono>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/local/metadata_cache.hpp"

#include "scratch_path.hpp"

using seabass::application::FileMetadata;
using seabass::infrastructure::local::MetadataCache;
namespace fs = std::filesystem;

namespace
{

std::string writeFile(const fs::path &p, const std::string &data)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << data;
    return p.string();
}

// The same two values MetadataCache itself stats, so a hand-written
// cache line can be made to match a real file and actually hit.
std::string currentSizeBytes(const std::string &path)
{
    return std::to_string(static_cast<long long>(fs::file_size(path)));
}

std::string currentMtimeSeconds(const std::string &path)
{
    auto mtime = fs::last_write_time(path);
    return std::to_string(static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count()));
}

FileMetadata sample()
{
    FileMetadata m;
    m.title = "Una Hora M\xc3\xa1s";
    m.artist = "The Rocketman";
    m.album = "Prototype EP";
    m.durationSeconds = 266.376;
    m.bitrate = 320;
    m.sampleRate = 44100;
    m.durationIsEstimated = false;
    return m;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_metadata_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string audio = writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file");

    // Case 1: an empty cache misses, then round-trips every field
    // through save/load. The non-ASCII title is deliberate -- it has to
    // survive the hand-written JSON escaping both ways.
    {
        MetadataCache cache(root.string());
        assert(!cache.lookup(audio).has_value());
        cache.store(audio, sample());
        assert(cache.dirty());
        assert(cache.save());
        assert(fs::exists(root / "Seabass" / "caches" / "metadata.jsonl"));

        MetadataCache reloaded(root.string());
        auto got = reloaded.lookup(audio);
        assert(got.has_value());
        assert(got->title == "Una Hora M\xc3\xa1s");
        assert(got->artist == "The Rocketman");
        assert(got->album == "Prototype EP");
        assert(std::abs(got->durationSeconds - 266.376) < 0.001);
        assert(got->bitrate == 320);
        assert(got->sampleRate == 44100);
        assert(!got->durationIsEstimated);
        std::cout << "case 1 (store -> save -> reload -> hit, all fields) OK\n";
    }

    // Case 2: the estimated flag survives too. It is the one field whose
    // loss would be dangerous rather than merely annoying, since a
    // caller that deletes files refuses to act on an estimate.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        FileMetadata estimated = sample();
        estimated.durationIsEstimated = true;

        MetadataCache cache(root.string());
        cache.store(audio, estimated);
        assert(cache.save());

        MetadataCache reloaded(root.string());
        auto got = reloaded.lookup(audio);
        assert(got.has_value());
        assert(got->durationIsEstimated);
        std::cout << "case 2 (estimated flag round-trips) OK\n";
    }

    // Case 3: a line missing the estimated field reads back as
    // estimated, never as exact. A hand-edited line, or one written by
    // some future version that dropped the field, must not be able to
    // promote a guess into a fact that a caller then deletes a file
    // over. Written with the file's real size/mtime so the lookup
    // actually hits and the flag can be inspected.
    {
        writeFile(root / "Seabass" / "caches" / "metadata.jsonl",
                  "{\"artist\":\"A\",\"duration\":\"120.000000\",\"mtime\":\"" + currentMtimeSeconds(audio)
                      + "\",\"path\":\"Contents/a/track.mp3\",\"size\":\"" + currentSizeBytes(audio)
                      + "\",\"title\":\"T\"}\n");
        MetadataCache cache(root.string());
        auto got = cache.lookup(audio);
        assert(got.has_value());
        assert(got->title == "T");
        assert(got->durationIsEstimated);
        // An explicit "0" is the only thing that means exact.
        std::cout << "case 3 (missing estimated field loads as estimated) OK\n";
    }

    // Case 4: an entry is only trusted while size AND mtime both still
    // match, same strictness as DurationCache -- a re-tag that preserved
    // one of the two must not return stale artist/title.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        MetadataCache cache(root.string());
        cache.store(audio, sample());
        assert(cache.save());

        writeFile(root / "Contents" / "a" / "track.mp3", "the file changed underneath us, and got longer");
        MetadataCache reloaded(root.string());
        assert(!reloaded.lookup(audio).has_value());
        std::cout << "case 4 (size/mtime change invalidates the entry) OK\n";
    }

    // Case 5: paths outside the stick root belong to no stick and are
    // never cached, and a zero duration is never stored (it is what an
    // unreadable file yields, and caching it would make the failure
    // permanent).
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        MetadataCache cache(root.string());
        cache.store("/somewhere/else/track.mp3", sample());
        assert(!cache.dirty());

        FileMetadata noDuration = sample();
        noDuration.durationSeconds = 0.0;
        cache.store(audio, noDuration);
        assert(!cache.dirty());
        std::cout << "case 5 (foreign path and zero duration are not cached) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all metadata_cache_test cases passed\n";
    return 0;
}
