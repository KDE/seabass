// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <map>

#include "application/use_cases/unreferenced_tracks.hpp"

using namespace seabass;
using namespace seabass::application;

namespace
{

// Answers from a table, and counts reads so a test can prove the cache
// spared the file rather than merely produced the same answer.
class FakeProbe : public TrackMetadataProbe
{
public:
    std::map<std::string, FileMetadata> answers;
    int reads = 0;

    std::optional<FileMetadata> read(const std::string &path) override
    {
        ++reads;
        auto it = answers.find(path);
        if (it == answers.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

class FakeCache : public MetadataCachePort
{
public:
    std::map<std::string, FileMetadata> entries;
    int stores = 0;

    std::optional<FileMetadata> lookup(const std::string &path) const override
    {
        auto it = entries.find(path);
        if (it == entries.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void store(const std::string &path, const FileMetadata &metadata) override
    {
        ++stores;
        entries[path] = metadata;
    }
};

FileMetadata tagged(const std::string &title, const std::string &artist, double duration, int bitrate)
{
    FileMetadata m;
    m.title = title;
    m.artist = artist;
    m.durationSeconds = duration;
    m.bitrate = bitrate;
    return m;
}

}  // namespace

int main()
{
    // Every field a stray file can honestly fill in, filled in -- and
    // the ones that live in a catalog row left empty rather than guessed.
    {
        FakeProbe probe;
        probe.answers["/stick/Contents/Techno/track.mp3"] = tagged("Busy Ants", "Moritz Hofbauer", 402.0, 320);
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/Techno/track.mp3", 9'000'000}};

        auto tracks = unreferencedFilesAsTracks(files, probe, nullptr, CancellationToken::none());
        assert(tracks.size() == 1);
        const auto &t = tracks[0];
        assert(t.isUnreferenced);
        assert(t.format == "disk");
        assert(t.sourceId == "/stick/Contents/Techno/track.mp3");
        assert(t.filePath == "/stick/Contents/Techno/track.mp3");
        assert(t.filename == "track.mp3");
        assert(t.title == "Busy Ants" && t.artist == "Moritz Hofbauer");
        assert(t.durationSeconds == 402.0 && t.bitrate == 320);
        assert(t.fileSizeBytes == 9'000'000);
        assert(!t.durationIsEstimated);
        // No row, so nothing that lives in one.
        assert(!t.rating.has_value() && t.comment.empty());
        assert(!t.playCount.has_value() && !t.lastPlayedAt.has_value());
        assert(t.cues.empty() && t.key.empty());
        std::cout << "case 1 (a stray file becomes an ordinary Track, catalog-only fields left empty) OK\n";
    }

    // A Windows-written path read on Linux: the filename must still be
    // the filename, not the whole path.
    {
        FakeProbe probe;
        probe.answers["D:\\Contents\\Techno\\track.mp3"] = tagged("t", "a", 200.0, 320);
        std::vector<AudioFileOnDisk> files = {{"D:\\Contents\\Techno\\track.mp3", 100}};

        auto tracks = unreferencedFilesAsTracks(files, probe, nullptr, CancellationToken::none());
        assert(tracks.size() == 1 && tracks[0].filename == "track.mp3");
        std::cout << "case 2 (backslash paths still yield a filename) OK\n";
    }

    // The estimate flag is the whole reason the probe reports one --
    // it has to survive the trip into the domain, where the planner
    // refuses to propose deleting anything from a group holding one.
    {
        FakeProbe probe;
        FileMetadata guessed = tagged("t", "a", 200.0, 128);
        guessed.durationIsEstimated = true;
        probe.answers["/stick/Contents/vbr.mp3"] = guessed;
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/vbr.mp3", 100}};

        auto tracks = unreferencedFilesAsTracks(files, probe, nullptr, CancellationToken::none());
        assert(tracks.size() == 1 && tracks[0].durationIsEstimated);
        std::cout << "case 3 (durationIsEstimated survives into the domain Track) OK\n";
    }

    // A file the probe cannot read is dropped, not carried with a zero
    // duration: with nothing to group on it could only ever end up in
    // the wrong group, and the outcome of that is a deleted file.
    {
        FakeProbe probe;
        probe.answers["/stick/Contents/good.mp3"] = tagged("t", "a", 200.0, 320);
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/good.mp3", 100},
                                               {"/stick/Contents/corrupt.mp3", 100}};

        auto tracks = unreferencedFilesAsTracks(files, probe, nullptr, CancellationToken::none());
        assert(tracks.size() == 1 && tracks[0].filename == "good.mp3");
        std::cout << "case 4 (an unreadable file is dropped, never grouped on a zero duration) OK\n";
    }

    // Cache hit spares the read entirely; a miss is read once and stored.
    {
        FakeProbe probe;
        probe.answers["/stick/Contents/a.mp3"] = tagged("a", "x", 200.0, 320);
        probe.answers["/stick/Contents/b.mp3"] = tagged("b", "x", 300.0, 320);
        FakeCache cache;
        cache.entries["/stick/Contents/a.mp3"] = tagged("cached", "x", 111.0, 128);
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/a.mp3", 100}, {"/stick/Contents/b.mp3", 200}};

        auto tracks = unreferencedFilesAsTracks(files, probe, &cache, CancellationToken::none());
        assert(tracks.size() == 2);
        assert(tracks[0].title == "cached" && tracks[0].durationSeconds == 111.0);
        assert(probe.reads == 1);   // only the miss
        assert(cache.stores == 1);  // and only the miss was stored
        assert(cache.entries.count("/stick/Contents/b.mp3") == 1);
        std::cout << "case 5 (cache hit spares the read; a miss is read once and stored) OK\n";
    }

    // An unreadable file is not cached as an answer -- the next run
    // should ask again rather than remember a failure forever.
    {
        FakeProbe probe;
        FakeCache cache;
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/corrupt.mp3", 100}};

        auto tracks = unreferencedFilesAsTracks(files, probe, &cache, CancellationToken::none());
        assert(tracks.empty());
        assert(cache.stores == 0);
        std::cout << "case 6 (an unreadable file is not cached as an answer) OK\n";
    }

    // Cancellation stops the walk between files, the contract the
    // token's own header promises.
    {
        FakeProbe probe;
        probe.answers["/stick/Contents/a.mp3"] = tagged("a", "x", 200.0, 320);
        std::vector<AudioFileOnDisk> files = {{"/stick/Contents/a.mp3", 100}};
        CancellationToken cancel;
        cancel.cancel();

        bool threw = false;
        try {
            unreferencedFilesAsTracks(files, probe, nullptr, cancel);
        } catch (const OperationCancelled &) {
            threw = true;
        }
        assert(threw);
        assert(probe.reads == 0);
        std::cout << "case 7 (a cancelled token stops before reading a file) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
