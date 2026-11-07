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
#include <iterator>

#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/metadata_cache.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::application::FileMetadata;
using seabass::domain::Mp3LeadIn;
using seabass::infrastructure::local::CachedSampleRates;
using seabass::infrastructure::local::MetadataCache;
namespace fs = std::filesystem;

namespace
{

std::string writeFile(const fs::path &p, const std::string &data)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << data;
    return seabass::pathToUtf8(p);
}

// The same two values MetadataCache itself stats, so a hand-written
// cache line can be made to match a real file and actually hit.
std::string currentSizeBytes(const std::string &path)
{
    return std::to_string(static_cast<long long>(fs::file_size(seabass::pathFromUtf8(path))));
}

std::string currentMtimeSeconds(const std::string &path)
{
    auto mtime = fs::last_write_time(seabass::pathFromUtf8(path));
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

Mp3LeadIn lameLeadIn()
{
    Mp3LeadIn l;
    l.infoFrame = true;
    l.tag = Mp3LeadIn::Tag::Lame;
    l.lameCrcValid = true;
    l.encoderDelay = 576;
    l.encoderPadding = 1404;
    l.infoFrameModeMatchesAudio = true;
    l.sampleRate = 44100;
    l.samplesPerFrame = 1152;
    return l;
}

bool sameLeadIn(const Mp3LeadIn &a, const Mp3LeadIn &b)
{
    return a.infoFrame == b.infoFrame && a.tag == b.tag && a.lameCrcValid == b.lameCrcValid
        && a.encoderDelay == b.encoderDelay && a.encoderPadding == b.encoderPadding
        && a.infoFrameModeMatchesAudio == b.infoFrameModeMatchesAudio && a.vbri == b.vbri
        && a.sampleRate == b.sampleRate && a.samplesPerFrame == b.samplesPerFrame;
}

// Answers what it is told and counts the files it was handed.
class CountingProbe : public seabass::application::TrackMetadataProbe
{
public:
    std::optional<FileMetadata> answer;
    int calls = 0;
    std::optional<FileMetadata> read(const std::string &) override
    {
        ++calls;
        return answer;
    }
};

}  // namespace

int main(int argc, char **argv)
{
    // The MP3 lead-in fixtures (tests/fixtures/mp3-lead-in), for cases 8
    // and 9. Required: a missing directory is a failure, not a skip.
    if (argc < 2 || !fs::is_directory(argv[1])) {
        std::cerr << "usage: metadata_cache_test FIXTURE_DIR (tests/fixtures/mp3-lead-in)\n";
        return 2;
    }
    const std::string fixtures = argv[1];
    fs::path root = seabass::testing::scratchRoot() / "seabass_metadata_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string audio = writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file");

    // Case 1: an empty cache misses, then round-trips every field
    // through save/load. The non-ASCII title is deliberate -- it has to
    // survive the hand-written JSON escaping both ways.
    {
        MetadataCache cache(seabass::pathToUtf8(root));
        assert(!cache.lookup(audio).has_value());
        cache.store(audio, sample());
        assert(cache.dirty());
        assert(cache.save());
        assert(fs::exists(root / "Seabass" / "caches" / "metadata.jsonl"));

        MetadataCache reloaded(seabass::pathToUtf8(root));
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

        MetadataCache cache(seabass::pathToUtf8(root));
        cache.store(audio, estimated);
        assert(cache.save());

        MetadataCache reloaded(seabass::pathToUtf8(root));
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
        MetadataCache cache(seabass::pathToUtf8(root));
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
        MetadataCache cache(seabass::pathToUtf8(root));
        cache.store(audio, sample());
        assert(cache.save());

        writeFile(root / "Contents" / "a" / "track.mp3", "the file changed underneath us, and got longer");
        MetadataCache reloaded(seabass::pathToUtf8(root));
        assert(!reloaded.lookup(audio).has_value());
        std::cout << "case 4 (size/mtime change invalidates the entry) OK\n";
    }

    // Case 5: paths outside the stick root belong to no stick and are
    // never cached, and a zero duration is never stored (it is what an
    // unreadable file yields, and caching it would make the failure
    // permanent).
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        MetadataCache cache(seabass::pathToUtf8(root));
        cache.store("/somewhere/else/track.mp3", sample());
        assert(!cache.dirty());

        FileMetadata noDuration = sample();
        noDuration.durationSeconds = 0.0;
        cache.store(audio, noDuration);
        assert(!cache.dirty());
        std::cout << "case 5 (foreign path and zero duration are not cached) OK\n";
    }

    // Case 6: an MP3's lead-in (#50) round-trips, and so does "looked,
    // found none". A line written before the field existed reads as not
    // looked, so the file is probed again, not taken to have no lead-in.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        const std::string other = writeFile(root / "Contents" / "a" / "other.mp3", "another file");
        FileMetadata withLeadIn = sample();
        withLeadIn.mp3LeadInRead = true;
        withLeadIn.mp3LeadIn = lameLeadIn();
        FileMetadata withNone = sample();
        withNone.mp3LeadInRead = true;

        MetadataCache cache(seabass::pathToUtf8(root));
        cache.store(audio, withLeadIn);
        cache.store(other, withNone);
        assert(cache.save());

        MetadataCache reloaded(seabass::pathToUtf8(root));
        auto got = reloaded.lookup(audio);
        assert(got && got->mp3LeadInRead && got->mp3LeadIn);
        assert(sameLeadIn(*got->mp3LeadIn, lameLeadIn()));
        auto none = reloaded.lookup(other);
        assert(none && none->mp3LeadInRead && !none->mp3LeadIn);

        writeFile(root / "Seabass" / "caches" / "metadata.jsonl",
                  "{\"duration\":\"120.000000\",\"estimated\":\"0\",\"mtime\":\"" + currentMtimeSeconds(audio)
                      + "\",\"path\":\"Contents/a/track.mp3\",\"size\":\"" + currentSizeBytes(audio) + "\"}\n"
                      + "{\"duration\":\"120.000000\",\"mp3leadin\":\"9 garbage\",\"mtime\":\"" + currentMtimeSeconds(other)
                      + "\",\"path\":\"Contents/a/other.mp3\",\"size\":\"" + currentSizeBytes(other) + "\"}\n");
        MetadataCache old(seabass::pathToUtf8(root));
        auto before = old.lookup(audio);
        assert(before && !before->mp3LeadInRead && !before->mp3LeadIn);
        auto unknown = old.lookup(other);
        assert(unknown && !unknown->mp3LeadInRead);
        std::cout << "case 6 (lead-in round-trips; an entry without one is not looked) OK\n";
    }

    // Case 7: lookupByPathOnly takes an entry without the stat, so it
    // still answers for a file that changed; lookup does not.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        const std::string changing = writeFile(root / "Contents" / "b" / "changing.mp3", "first");
        MetadataCache cache(seabass::pathToUtf8(root));
        cache.store(changing, sample());
        assert(cache.save());
        writeFile(root / "Contents" / "b" / "changing.mp3", "second, and longer");
        MetadataCache reloaded(seabass::pathToUtf8(root));
        assert(!reloaded.lookup(changing).has_value());
        assert(reloaded.lookupByPathOnly(changing).has_value());
        assert(!reloaded.lookupByPathOnly(seabass::pathToUtf8(root / "nowhere.mp3")).has_value());
        std::cout << "case 7 (lookupByPathOnly skips the stat) OK\n";
    }

    // Case 8: CachedSampleRates::leadInOf probes once and caches; an entry
    // without a lead-in is probed again; a probe with nothing to say (no
    // TagLib) falls back to reading the file's start directly.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        const fs::path mp3 = root / "Contents" / "c" / "lame.mp3";
        fs::create_directories(mp3.parent_path());
        fs::copy_file(seabass::pathFromUtf8(fixtures) / "lame-cbr-44100.mp3", mp3,
                      fs::copy_options::overwrite_existing);
        const std::string file = seabass::pathToUtf8(mp3);

        CountingProbe probe;
        FileMetadata probed = sample();
        probed.mp3LeadInRead = true;
        probed.mp3LeadIn = lameLeadIn();
        probed.mp3LeadIn->encoderDelay = 1000;  // not the file's: proves the probe's answer is the one used
        probe.answer = probed;
        {
            CachedSampleRates rates(seabass::pathToUtf8(root), probe);
            auto first = rates.leadInOf(file);
            assert(first && first->encoderDelay == 1000 && probe.calls == 1);
            auto second = rates.leadInOf(file);
            assert(second && second->encoderDelay == 1000 && probe.calls == 1);
            assert(rates.save());
        }
        {
            // A cache entry the probe stored without looking at the lead-in.
            fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
            CountingProbe old;
            old.answer = sample();
            CachedSampleRates rates(seabass::pathToUtf8(root), old);
            assert(rates.rateOf(file).has_value() && old.calls == 1);
            // No lead-in from the probe: read from the file itself.
            auto direct = rates.leadInOf(file);
            assert(old.calls == 2);
            assert(direct && direct->tag == Mp3LeadIn::Tag::Lame && direct->encoderDelay == 576);
            // And by path, without any probe at all.
            CountingProbe none;
            CachedSampleRates byPath(seabass::pathToUtf8(root), none);
            auto taken = byPath.leadInByPathOf(file);
            assert(taken && taken->encoderDelay == 576 && none.calls == 0);
        }
        std::cout << "case 8 (CachedSampleRates lead-ins: probe once, fall back to the file) OK\n";
    }

    // Case 9: a probe with nothing to say (a build without TagLib, or a
    // file TagLib finds no length in). The lead-in read from the file's
    // start is cached all the same, as an entry with no duration, so the
    // next Cues pass does not read the file again. Such an entry is no
    // answer to a question about the file's tags or length: lookup()
    // misses, and rateOf() probes. Stored over an entry the probe made
    // earlier, the lead-in joins it and the length stays.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        const fs::path mp3 = root / "Contents" / "d" / "lame.mp3";
        fs::create_directories(mp3.parent_path());
        fs::copy_file(seabass::pathFromUtf8(fixtures) / "lame-cbr-44100.mp3", mp3,
                      fs::copy_options::overwrite_existing);
        const std::string file = seabass::pathToUtf8(mp3);
        {
            seabass::application::NullTrackMetadataProbe nothing;
            CachedSampleRates rates(seabass::pathToUtf8(root), nothing);
            auto first = rates.leadInOf(file);
            assert(first && first->encoderDelay == 576 && rates.probes() == 1);
            auto second = rates.leadInOf(file);
            assert(second && second->encoderDelay == 576);
            assert(rates.probes() == 1 && "the second ask is answered from the cache, not the file");
            assert(rates.save());
        }
        {
            MetadataCache reloaded(seabass::pathToUtf8(root));
            assert(!reloaded.lookup(file).has_value() && "a lead-in alone is no metadata");
            assert(!reloaded.lookupByPathOnly(file).has_value());
            CountingProbe none;
            CachedSampleRates rates(seabass::pathToUtf8(root), none);
            auto again = rates.leadInOf(file);
            assert(again && again->encoderDelay == 576 && none.calls == 0 && "survives a save");
            auto taken = rates.leadInByPathOf(file);
            assert(taken && taken->encoderDelay == 576 && none.calls == 0);
        }
        {
            fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
            CountingProbe old;
            old.answer = sample();
            CachedSampleRates rates(seabass::pathToUtf8(root), old);
            assert(rates.rateOf(file).has_value() && old.calls == 1);
            assert(rates.leadInOf(file).has_value() && old.calls == 2);
            assert(rates.leadInOf(file).has_value() && old.calls == 2 && "cached with the probe's entry");
            assert(rates.rateOf(file) == 44100.0 && old.calls == 2 && "and the length and rate stay");
            assert(rates.save());
            MetadataCache reloaded(seabass::pathToUtf8(root));
            const auto entry = reloaded.lookup(file);
            assert(entry && entry->durationSeconds == 266.376 && entry->mp3LeadInRead && entry->mp3LeadIn);
        }
        std::cout << "case 9 (a lead-in read without the probe is cached on its own) OK\n";
    }

    // Case 10: the lead-in token carries the version of the parser that
    // read it (#50). An entry the parser of an earlier build wrote may
    // say what that parser got wrong, so it reads as not looked and the
    // file is read again; one this build wrote reads back.
    {
        fs::remove(root / "Seabass" / "caches" / "metadata.jsonl");
        writeFile(root / "Seabass" / "caches" / "metadata.jsonl",
                  "{\"duration\":\"120.000000\",\"estimated\":\"0\",\"mp3leadin\":\"1 1 L 1 576 1404 1 0 44100 1152\","
                  "\"mtime\":\"" + currentMtimeSeconds(audio)
                      + "\",\"path\":\"Contents/a/track.mp3\",\"size\":\"" + currentSizeBytes(audio) + "\"}\n");
        MetadataCache old(seabass::pathToUtf8(root));
        auto stale = old.lookup(audio);
        assert(stale && !stale->mp3LeadInRead && !stale->mp3LeadIn && "a version 1 token is not this parser's");
        assert(!old.lookupLeadIn(audio).has_value() || !old.lookupLeadIn(audio)->mp3LeadInRead);

        FileMetadata withLeadIn = sample();
        withLeadIn.mp3LeadInRead = true;
        withLeadIn.mp3LeadIn = lameLeadIn();
        old.store(audio, withLeadIn);
        assert(old.save());
        std::ifstream written(root / "Seabass" / "caches" / "metadata.jsonl", std::ios::binary);
        const std::string line((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
        const std::string token =
            "\"mp3leadin\":\"" + std::to_string(seabass::infrastructure::audio::Mp3LeadInParserVersion) + " ";
        assert(line.find(token) != std::string::npos && "written with this parser's version");
        MetadataCache reloaded(seabass::pathToUtf8(root));
        auto fresh = reloaded.lookup(audio);
        assert(fresh && fresh->mp3LeadInRead && fresh->mp3LeadIn && sameLeadIn(*fresh->mp3LeadIn, lameLeadIn()));
        std::cout << "case 10 (a lead-in from another parser version is read again) OK\n";
    }

    // Case 11: a lead-in whose numbers no MP3 can have reads as not
    // looked (a hand-edited or damaged line): a delay or padding outside
    // the 12-bit field, a rate that is not one of the nine MPEG rates, a
    // frame that is neither 576 nor 1152 samples. The same line with
    // sound numbers reads back, so the rejections are the numbers.
    {
        const std::string v = std::to_string(seabass::infrastructure::audio::Mp3LeadInParserVersion);
        auto read = [&](const std::string &fields) {
            writeFile(root / "Seabass" / "caches" / "metadata.jsonl",
                      "{\"duration\":\"120.000000\",\"estimated\":\"0\",\"mp3leadin\":\"" + v + " " + fields
                          + "\",\"mtime\":\"" + currentMtimeSeconds(audio) + "\",\"path\":\"Contents/a/track.mp3\","
                          "\"size\":\"" + currentSizeBytes(audio) + "\"}\n");
            MetadataCache cache(seabass::pathToUtf8(root));
            auto got = cache.lookup(audio);
            assert(got.has_value());
            return got->mp3LeadInRead;
        };
        assert(read("1 L 1 576 1404 1 0 44100 1152"));
        assert(read("1 L 1 0 4095 1 0 8000 576"));
        assert(!read("1 L 1 4096 1404 1 0 44100 1152") && "delay beyond 12 bits");
        assert(!read("1 L 1 -1 1404 1 0 44100 1152") && "negative delay");
        assert(!read("1 L 1 576 4096 1 0 44100 1152") && "padding beyond 12 bits");
        assert(!read("1 L 1 576 -5 1 0 44100 1152") && "negative padding");
        assert(!read("1 L 1 576 1404 1 0 44000 1152") && "not an MPEG rate");
        assert(!read("1 L 1 576 1404 1 0 0 1152") && "no rate");
        assert(!read("1 L 1 576 1404 1 0 96000 1152") && "not an MPEG rate");
        assert(!read("1 L 1 576 1404 1 0 44100 1024") && "not a Layer III frame");
        assert(!read("1 L 1 576 1404 1 0 44100 0") && "no frame length");
        std::cout << "case 11 (a lead-in with impossible numbers is read again) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all metadata_cache_test cases passed\n";
    return 0;
}
