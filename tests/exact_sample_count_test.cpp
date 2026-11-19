// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// audio::readExactSampleCount(): a file's own sample count, as a Denon
// player stores it for the row it analyses.
//
// 1. Files made here with known counts, odd ones so that no rounding can
//    hide: WAV (16-bit stereo and 24-bit mono), AIFF, a FLAC with only its
//    STREAMINFO, M4A (mdhd version 0 and 1, a video track first, the moov
//    after a large mdat, a timescale other than the rate).
// 2. Nothing for a file that is not there, another kind, a FLAC whose
//    STREAMINFO says 0 (not known), an M4A with no sound track or cut
//    short, a count that would not come out whole.
// 3. Measured truth: tests/fixtures/testshark_t73's Engine database
//    (m-after-R1b.db, copied to scratch first) holds rows a Prime 4
//    analysed from FLAC 44.1/48 kHz, AIFF 48 kHz and an MP3; for each, the
//    player's stored sample count is the file's (the audio heads keep the
//    headers whole). The table is printed.
//
// usage: exact_sample_count_test <tests/fixtures> [folder to print counts of]

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>

#include "infrastructure/audio/exact_sample_count.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::audio::ExactSampleCount;
using infrastructure::audio::readExactSampleCount;
using Source = ExactSampleCount::Source;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

std::string le(std::uint64_t v, int width)
{
    std::string out;
    for (int i = 0; i < width; ++i) {
        out += static_cast<char>((v >> (8 * i)) & 0xff);
    }
    return out;
}

std::string be(std::uint64_t v, int width)
{
    std::string out;
    for (int i = width - 1; i >= 0; --i) {
        out += static_cast<char>((v >> (8 * i)) & 0xff);
    }
    return out;
}

void write(const fs::path &file, const std::string &bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string wav(std::uint32_t rate, int channels, int bits, std::uint32_t frames)
{
    const std::uint32_t frameBytes = static_cast<std::uint32_t>(channels * bits / 8);
    const std::string data(static_cast<std::size_t>(frames) * frameBytes, '\x01');
    std::string fmt = le(1, 2) + le(static_cast<std::uint64_t>(channels), 2) + le(rate, 4) + le(rate * frameBytes, 4)
        + le(frameBytes, 2) + le(static_cast<std::uint64_t>(bits), 2);
    std::string body = "WAVEfmt " + le(fmt.size(), 4) + fmt + "data" + le(data.size(), 4) + data;
    return "RIFF" + le(body.size(), 4) + body;
}

// An 80-bit IEEE extended float of a whole rate, as AIFF's COMM keeps it.
std::string extended(std::uint32_t rate)
{
    int exponent = 31;
    std::uint64_t mantissa = rate;
    while ((mantissa & (1ull << 63)) == 0) {
        mantissa <<= 1;
    }
    for (std::uint32_t r = rate; r > 1; r >>= 1) {
        ++exponent;
    }
    exponent -= 31;
    return be(static_cast<std::uint64_t>(16383 + exponent), 2) + be(mantissa, 8);
}

std::string aiff(std::uint32_t rate, std::uint32_t frames)
{
    const std::string comm = be(2, 2) + be(frames, 4) + be(16, 2) + extended(rate);
    const std::string sound = be(0, 4) + be(0, 4) + std::string(static_cast<std::size_t>(frames) * 4, '\x02');
    const std::string body = "AIFFCOMM" + be(comm.size(), 4) + comm + "SSND" + be(sound.size(), 4) + sound;
    return "FORM" + be(body.size(), 4) + body;
}

std::string flacStreamInfoOnly(std::uint32_t rate, std::uint64_t total)
{
    std::string info = be(4096, 2) + be(4096, 2) + be(0, 3) + be(0, 3);
    // 20 bits rate, 3 bits channels-1, 5 bits bits-1, 36 bits total.
    const std::uint64_t packed = (static_cast<std::uint64_t>(rate) << 44) | (1ull << 41) | (15ull << 36) | total;
    info += be(packed, 8) + std::string(16, '\0');
    return std::string("fLaC") + be(0x80, 1) + be(info.size(), 3) + info;
}

std::string box(const std::string &type, const std::string &payload)
{
    return be(payload.size() + 8, 4) + type + payload;
}

std::string mp4Trak(const std::string &handler, std::uint32_t timescale, std::uint64_t duration, std::uint32_t rate,
                    bool version1)
{
    const std::string hdlr = box("hdlr", be(0, 4) + be(0, 4) + handler + std::string(12, '\0') + std::string(1, '\0'));
    const std::string mdhd = version1
        ? box("mdhd", be(0x01000000, 4) + be(0, 8) + be(0, 8) + be(timescale, 4) + be(duration, 8) + be(0, 4))
        : box("mdhd", be(0, 4) + be(0, 4) + be(0, 4) + be(timescale, 4) + be(duration, 4) + be(0, 4));
    const std::string entry = box("mp4a", std::string(6, '\0') + be(1, 2) + std::string(8, '\0') + be(2, 2) + be(16, 2)
                                              + be(0, 2) + be(0, 2) + be(static_cast<std::uint64_t>(rate) << 16, 4));
    const std::string stsd = box("stsd", be(0, 4) + be(1, 4) + entry);
    return box("trak", box("mdia", hdlr + mdhd + box("minf", box("stbl", stsd))));
}

std::string m4a(const std::string &traks, std::size_t mdatBytes)
{
    return box("ftyp", "M4A " + be(0, 4) + "M4A isom") + box("mdat", std::string(mdatBytes, '\x03'))
        + box("moov", box("mvhd", std::string(100, '\0')) + traks);
}

std::uint64_t storedSamples(const std::string &mdb, std::int64_t id)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(mdb.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    std::uint64_t samples = 0;
    if (sqlite3_prepare_v2(db, "SELECT trackData FROM PerformanceData WHERE trackId = ?1", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
            const auto *b = static_cast<const std::byte *>(sqlite3_column_blob(stmt, 0));
            const std::vector<std::byte> bytes(b, b + sqlite3_column_bytes(stmt, 0));
            samples = static_cast<std::uint64_t>(djinterop::engine::v3::track_data_blob::from_blob(bytes).samples);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return samples;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: exact_sample_count_test <tests/fixtures> [folder]\n";
        return 2;
    }
    const fs::path fixtures = pathFromUtf8(argv[1]);
    if (argc >= 3) {
        // Print mode, for a folder of audio by hand: the file's count.
        for (const auto &entry : fs::directory_iterator(pathFromUtf8(argv[2]))) {
            const auto count = readExactSampleCount(pathToUtf8(entry.path()));
            std::cout << pathToUtf8(entry.path().filename()) << "\t"
                      << (count ? std::to_string(count->samples) + "\t" + std::to_string(count->sampleRateHz) : "-")
                      << "\n";
        }
        return 0;
    }
    const fs::path dir = testing::scratchRoot() / "exact_sample_count_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto counted = [&](const std::string &name, const std::string &bytes) {
        write(dir / name, bytes);
        return readExactSampleCount(pathToUtf8(dir / name));
    };
    const auto expect = [&](const std::string &name, const std::string &bytes, std::uint64_t samples, double rate,
                            Source source) {
        const auto c = counted(name, bytes);
        check(c && c->samples == samples && c->sampleRateHz == rate && c->source == source,
              name + ": " + (c ? std::to_string(c->samples) + " at " + std::to_string(c->sampleRateHz) : "nothing")
                  + ", wanted " + std::to_string(samples));
    };

    // 1. Made here.
    expect("a.wav", wav(44100, 2, 16, 132301), 132301, 44100.0, Source::WavData);
    expect("b.WAV", wav(48000, 1, 24, 14407), 14407, 48000.0, Source::WavData);
    expect("c.aiff", aiff(48000, 144007), 144007, 48000.0, Source::AiffComm);
    expect("d.aif", aiff(44100, 1323), 1323, 44100.0, Source::AiffComm);
    expect("e.flac", flacStreamInfoOnly(44100, 7938001), 7938001, 44100.0, Source::FlacStreamInfo);
    expect("f.m4a", m4a(mp4Trak("soun", 44100, 132301, 44100, false), 16), 132301, 44100.0, Source::Mp4TrackDuration);
    expect("g.m4a",
           m4a(mp4Trak("vide", 90000, 270000, 0, false) + mp4Trak("soun", 48000, 144007, 48000, true), 200000),
           144007, 48000.0, Source::Mp4TrackDuration);
    // A timescale other than the rate: the count only where it is whole.
    expect("h.mp4", m4a(mp4Trak("soun", 1000, 3000, 44100, false), 8), 132300, 44100.0, Source::Mp4TrackDuration);

    // 2. Nothing.
    check(!readExactSampleCount(pathToUtf8(dir / "none.flac")), "a file that is not there");
    check(!counted("x.ogg", "OggS"), "another kind");
    check(!counted("x.flac", flacStreamInfoOnly(44100, 0)), "STREAMINFO's 0 means not known");
    check(!counted("x.m4a", m4a(mp4Trak("vide", 90000, 270000, 0, false), 8)), "no sound track");
    const std::string whole = m4a(mp4Trak("soun", 44100, 132301, 44100, false), 8);
    check(!counted("y.m4a", whole.substr(0, whole.size() - 30)), "a moov cut short");
    check(!counted("z.m4a", m4a(mp4Trak("soun", 1000, 3001, 44100, true), 8)), "a count that is not whole");
    check(!counted("w.wav", "RIFF"), "a WAV that does not parse");

    // 3. The player's counts against the files (heads with whole headers).
    const fs::path t73 = fixtures / "testshark_t73";
    const std::string mdb = pathToUtf8(dir / "m.db");
    fs::copy_file(t73 / "engine" / "m-after-R1b.db", mdb, fs::copy_options::overwrite_existing);
    const std::map<std::int64_t, std::string> analysed = {
        {5, "UnknownArtist/UnknownAlbum/T73-B11-click-48k-122bpm.aiff"},
        {12, "Seabass Test/T73 Base/T73-B04-click-44k-108bpm.mp3"},
        {13, "Seabass Test/T73/T73-N1-click-48k-120bpm.flac"},
        {14, "Seabass Test/T73/T73-N3-click-44k-128bpm.flac"},
        {15, "UnknownArtist/UnknownAlbum/T73-N2-click-48k-125bpm.aiff"},
    };
    std::cout << "row\tfile\tDenon samples\tfile samples\tsame\n";
    for (const auto &[id, file] : analysed) {
        const std::uint64_t denon = storedSamples(mdb, id);
        const auto own = readExactSampleCount(pathToUtf8(t73 / "stick" / "Contents" / pathFromUtf8(file)));
        std::cout << id << "\t" << pathToUtf8(pathFromUtf8(file).filename()) << "\t" << denon << "\t"
                  << (own ? std::to_string(own->samples) : "-") << "\t" << (own && own->samples == denon ? "yes" : "NO")
                  << "\n";
        check(denon > 0 && own && own->samples == denon, "row " + std::to_string(id) + ": the player's count is the file's");
    }
    // The B files are 180 s at 44.1 kHz: 7938000 in every FLAC header.
    const auto b01 = readExactSampleCount(pathToUtf8(t73 / "stick/Contents/Seabass Test/T73 Base/T73-B01-click-44k-102bpm.flac"));
    check(b01 && b01->samples == 7938000, "B01: 180 s at 44.1 kHz is 7938000 samples");
    // An MP3 with no Xing/Info frame: its frames are not counted here.
    check(!readExactSampleCount(pathToUtf8(fixtures / "mp3-click" / "click-A-notag-44100.mp3")),
          "an MP3 without an Info frame gives no count");
    const auto lame = readExactSampleCount(pathToUtf8(fixtures / "mp3-click" / "click-D-lame-44100.mp3"));
    check(lame && lame->source == Source::Mp3InfoFrames && lame->samples % 1152 == 0, "a LAME MP3: whole frames");

    fs::remove_all(dir);
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "exact_sample_count_test: all checks passed\n";
    return 0;
}
