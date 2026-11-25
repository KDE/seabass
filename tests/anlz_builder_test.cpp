// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// buildAnlzFiles(): a track's .DAT, .EXT and .2EX built from nothing.
//
// Read back with the app's own readers (AnlzFile's validation, the Kaitai
// parser with no tolerance, the beat grid and waveform readers, the cue
// codecs), and held against the real files of testshark_t73, a stick as
// rekordbox 7.2 wrote it: the same sections in the same order, the same
// header lengths, the same sizes where a section's size is fixed. The
// fixture has no .2EX; that file's order is held against the one every
// measured .2EX without the unknown PVDI has (see anlz_builder.hpp).
//
// The audio is generated: a kick, a bass line and hats, synthetic, no
// real track anywhere.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <numbers>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <kaitai/kaitaistream.h>

#include "anlz_strict_check.hpp"
#include "domain/waveform_from_audio.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
namespace rekordbox = seabass::infrastructure::rekordbox;
namespace domain = seabass::domain;
using Anlz = rekordbox_anlz_t;

namespace
{

constexpr int Rate = 44100;
const std::string AnalyzePath = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
// Non-ASCII and outside the BMP on purpose: PPTH is UTF-16BE, and a
// character above U+FFFF takes a surrogate pair there.
const std::string TrackPath = "/Contents/Synthetic Artist/Test Album \xC3\x98/01 Kick \xF0\x9F\x8E\xB5 Bass.wav";

fs::path fixture(const std::string &relative)
{
    return seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / seabass::pathFromUtf8(relative);
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::uint32_t be32(const std::string &s, std::size_t at)
{
    return anlz_strict::be32(s, at);
}

std::uint16_t be16(const std::string &s, std::size_t at)
{
    return anlz_strict::be16(s, at);
}

class MapSource : public rekordbox::AnlzByteSource
{
public:
    std::map<std::string, std::string> files;
    std::optional<std::string> read(const std::string &relativePath) override
    {
        auto it = files.find(relativePath);
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

MapSource sourceOf(const rekordbox::NewAnlzFiles &files)
{
    MapSource source;
    source.files[rekordbox::anlzRelativePath(AnalyzePath, false)] = files.dat;
    source.files[rekordbox::anlzSiblingRelativePath(AnalyzePath, ".EXT")] = files.ext;
    source.files[rekordbox::anlzSiblingRelativePath(AnalyzePath, ".2EX")] = files.twoEx;
    return source;
}

std::vector<std::string> fourccs(const std::string &file)
{
    std::vector<std::string> out;
    for (const auto &s : anlz_strict::sectionsOf(file)) {
        out.push_back(s.fourcc);
    }
    return out;
}

std::optional<std::string> sectionBytes(const std::string &file, const std::string &fourcc, int nth = 0)
{
    for (const auto &s : anlz_strict::sectionsOf(file)) {
        if (s.fourcc == fourcc && nth-- == 0) {
            return s.bytes;
        }
    }
    return std::nullopt;
}

std::string join(const std::vector<std::string> &v)
{
    std::string out;
    for (const auto &s : v) {
        out += (out.empty() ? "" : " ") + s;
    }
    return out;
}

// A synthetic track: a four-on-the-floor kick at 125 BPM, a bass note
// under each kick, hats on the offbeats.
std::vector<float> syntheticTrack(double seconds)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * Rate), 0.0f);
    const double beat = 0.48;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        const double inBeat = std::fmod(t, beat);
        const double kick = 0.6 * std::exp(-inBeat * 25.0) * std::sin(2.0 * std::numbers::pi * 55.0 * inBeat);
        const double bass = 0.2 * std::sin(2.0 * std::numbers::pi * 110.0 * t) * (inBeat < 0.2 ? 1.0 : 0.3);
        const double offbeat = std::fmod(t + beat / 2.0, beat);
        // A short burst of a high tone stands in for a hat.
        const double hat = 0.15 * std::exp(-offbeat * 60.0) * std::sin(2.0 * std::numbers::pi * 7919.0 * t);
        out[i] = static_cast<float>(kick + bass + hat);
    }
    return out;
}

// 125 BPM from 52.925 ms for 32 beats, then 10 beats at 126 BPM: a grid
// whose spacing changes, with fractions of a millisecond.
std::vector<domain::Beat> syntheticBeats()
{
    std::vector<domain::Beat> beats;
    double t = 52.925;
    for (int i = 0; i < 42; ++i) {
        beats.push_back({t, i % 4 + 1});
        t += i < 32 ? 480.0 : 60000.0 / 126.0;
    }
    return beats;
}

rekordbox::NewAnlzInput syntheticInput()
{
    rekordbox::NewAnlzInput input;
    input.trackPath = TrackPath;
    input.beats = syntheticBeats();
    input.waveform = domain::waveformFromSamples(syntheticTrack(21.0), Rate);
    input.durationMs = 21000.0;
    input.sampleRateHz = Rate;
    return input;
}

template<typename F>
bool throwsInvalid(F &&f)
{
    try {
        f();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

// Every .DAT and .EXT of testshark_t73, read.
std::vector<std::pair<fs::path, std::string>> fixtureFiles(const std::string &extension)
{
    std::vector<std::pair<fs::path, std::string>> out;
    for (const auto &entry : fs::recursive_directory_iterator(fixture("testshark_t73/stick/PIONEER/USBANLZ"))) {
        if (entry.is_regular_file() && entry.path().extension() == seabass::pathFromUtf8(extension)) {
            out.emplace_back(entry.path(), readFile(entry.path()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main()
{
    const rekordbox::NewAnlzInput input = syntheticInput();
    const rekordbox::NewAnlzFiles files = rekordbox::buildAnlzFiles(input);
    const fs::path scratch = seabass::testing::scratchRoot() / "anlz_builder_test";
    fs::create_directories(scratch);

    // ---- the files parse: AnlzFile's validation and strict Kaitai -----------
    {
        for (const auto &[name, bytes] : std::vector<std::pair<std::string, std::string>>{
                 {"ANLZ0000.DAT", files.dat}, {"ANLZ0000.EXT", files.ext}, {"ANLZ0000.2EX", files.twoEx}}) {
            const fs::path path = scratch / name;
            {
                std::ofstream out(path, std::ios::binary);
                out << bytes;
            }
            const auto raw = rekordbox::AnlzFile::readRaw(seabass::pathToUtf8(path));
            assert(raw.toBytes() == bytes);
            assert(raw.headerBytes.size() == 28);
            const auto problems = anlz_strict::strictProblems(path);
            for (const auto &p : problems) {
                std::cerr << p << "\n";
            }
            assert(problems.empty());

            // Every section Kaitai knows comes out as its own type, not
            // as an unknown body, and the path reads back as given.
            std::istringstream in(bytes, std::ios::binary);
            kaitai::kstream ks(&in);
            Anlz anlz(&ks);
            std::size_t unknown = 0;
            for (const auto &section : *anlz.sections()) {
                if (dynamic_cast<Anlz::unknown_tag_t *>(section->body()) != nullptr) {
                    ++unknown;
                }
                if (auto *p = dynamic_cast<Anlz::path_tag_t *>(section->body())) {
                    assert(p->path() == TrackPath);
                }
            }
            // PQT2 and PWVC are not in the vendored spec; nothing else
            // may fall through to unknown.
            const std::size_t expectedUnknown = name == "ANLZ0000.EXT" ? 1 : (name == "ANLZ0000.2EX" ? 1 : 0);
            assert(unknown == expectedUnknown);
        }
        std::cout << "case 1 (all three files pass AnlzFile::readRaw and the strict Kaitai check; path reads back) OK\n";
    }

    // ---- section order and layout against rekordbox's own files ------------
    {
        const auto dats = fixtureFiles(".DAT");
        const auto exts = fixtureFiles(".EXT");
        assert(!dats.empty() && !exts.empty());
        for (const auto &[path, bytes] : dats) {
            if (fourccs(bytes) != fourccs(files.dat)) {
                std::cerr << seabass::pathToUtf8(path) << ": " << join(fourccs(bytes)) << "\nbuilt: "
                          << join(fourccs(files.dat)) << "\n";
                assert(false && "the .DAT's sections differ from rekordbox's");
            }
        }
        // PSSI and PVB2 are left out on purpose (anlz_builder.hpp); with
        // them taken away, every real .EXT has the built one's order.
        for (const auto &[path, bytes] : exts) {
            auto real = fourccs(bytes);
            std::erase_if(real, [](const std::string &f) { return f == "PSSI" || f == "PVB2"; });
            if (real != fourccs(files.ext)) {
                std::cerr << seabass::pathToUtf8(path) << ": " << join(real) << "\nbuilt: " << join(fourccs(files.ext))
                          << "\n";
                assert(false && "the .EXT's sections differ from rekordbox's");
            }
        }
        assert(fourccs(files.twoEx) == (std::vector<std::string>{"PPTH", "PWV7", "PWV6", "PWVC"}));

        // Header lengths: every section the built files share with a real
        // one has the real one's len_header.
        std::map<std::string, std::uint32_t> realHeader;
        for (const auto *set : {&dats, &exts}) {
            for (const auto &[path, bytes] : *set) {
                for (const auto &s : anlz_strict::sectionsOf(bytes)) {
                    const auto lenHeader = be32(s.bytes, 4);
                    auto [it, fresh] = realHeader.emplace(s.fourcc, lenHeader);
                    assert(it->second == lenHeader);  // the fixture agrees with itself
                }
            }
        }
        std::size_t compared = 0;
        for (const auto *file : {&files.dat, &files.ext}) {
            for (const auto &s : anlz_strict::sectionsOf(*file)) {
                assert(realHeader.count(s.fourcc) == 1);
                assert(be32(s.bytes, 4) == realHeader[s.fourcc]);
                ++compared;
            }
        }
        // The .2EX's, as measured (no committed fixture has one).
        const std::map<std::string, std::uint32_t> twoExHeader{{"PPTH", 16}, {"PWV7", 24}, {"PWV6", 20}, {"PWVC", 14}};
        for (const auto &s : anlz_strict::sectionsOf(files.twoEx)) {
            assert(be32(s.bytes, 4) == twoExHeader.at(s.fourcc));
        }

        // The file header after len_file, and the fixed-size sections
        // byte for byte in their headers and in size.
        const std::string &realDat = dats.front().second;
        const std::string &realExt = exts.front().second;
        assert(files.dat.substr(12, 16) == realDat.substr(12, 16));
        assert(files.ext.substr(12, 16) == realExt.substr(12, 16));
        assert(files.twoEx.substr(12, 16) == realDat.substr(12, 16));
        for (const char *fixed : {"PVBR", "PWAV", "PWV2"}) {
            const auto built = *sectionBytes(files.dat, fixed);
            const auto real = *sectionBytes(realDat, fixed);
            assert(built.size() == real.size());
            assert(built.substr(0, be32(real, 4)) == real.substr(0, be32(real, 4)));
        }
        {
            const auto built = *sectionBytes(files.ext, "PWV4");
            const auto real = *sectionBytes(realExt, "PWV4");
            assert(built.size() == real.size() && built.substr(0, 24) == real.substr(0, 24));
        }
        // The empty cue lists of the real .EXT, byte for byte (the .DAT's
        // hot lists hold cues on this stick, its memory lists do not).
        for (int nth : {0, 1}) {
            assert(*sectionBytes(files.ext, "PCOB", nth) == *sectionBytes(realExt, "PCOB", nth));
        }
        assert(*sectionBytes(files.dat, "PCOB", 1) == *sectionBytes(realDat, "PCOB", 1));
        assert(*sectionBytes(files.ext, "PCO2", 1) == *sectionBytes(realExt, "PCO2", 1));
        // Scroll headers: the same fields after the entry count.
        for (const char *scroll : {"PWV3", "PWV5"}) {
            const auto built = *sectionBytes(files.ext, scroll);
            const auto real = *sectionBytes(realExt, scroll);
            assert(built.substr(12, 4) == real.substr(12, 4));   // entry size
            assert(built.substr(20, 4) == real.substr(20, 4));   // 0x0096 and the last field
        }
        {
            const auto real = *sectionBytes(realExt, "PQT2");
            const auto built = *sectionBytes(files.ext, "PQT2");
            assert(built.substr(12, 12) == real.substr(12, 12));
            assert(built.substr(48, 8) == real.substr(48, 8));
        }
        std::cout << "case 2 (section order of all " << dats.size() << " .DAT and " << exts.size()
                  << " .EXT of testshark_t73; " << compared
                  << " header lengths; fixed sections and empty cue lists byte for byte) OK\n";
    }

    // ---- PPTH spelled the way rekordbox spells it ---------------------------
    //
    // Each real file's own path, built again, gives its PPTH byte for byte.
    {
        std::size_t checked = 0;
        for (const auto &[path, bytes] : fixtureFiles(".DAT")) {
            const std::string real = *sectionBytes(bytes, "PPTH");
            std::istringstream in(bytes, std::ios::binary);
            kaitai::kstream ks(&in);
            Anlz anlz(&ks);
            std::string trackPath;
            for (const auto &section : *anlz.sections()) {
                if (auto *p = dynamic_cast<Anlz::path_tag_t *>(section->body())) {
                    trackPath = p->path();
                }
            }
            assert(!trackPath.empty());
            rekordbox::NewAnlzInput again;
            again.trackPath = trackPath;
            again.durationMs = 1000.0;
            again.sampleRateHz = Rate;
            const auto built = rekordbox::buildAnlzFiles(again);
            assert(*sectionBytes(built.dat, "PPTH") == real);
            assert(*sectionBytes(built.ext, "PPTH") == real);
            ++checked;
        }
        // And the surrogate pair: U+1F3B5 is D83C DFB5.
        const auto ppth = *sectionBytes(files.dat, "PPTH");
        assert(ppth.find(std::string("\xD8\x3C\xDF\xB5", 4)) != std::string::npos);
        assert(be32(ppth, 12) == ppth.size() - 16);
        assert(ppth.substr(ppth.size() - 2) == std::string(2, '\0'));
        std::cout << "case 3 (PPTH of " << checked << " real files rebuilt byte for byte; surrogate pair) OK\n";
    }

    // ---- the beat grid reads back ---------------------------------------------
    {
        MapSource source = sourceOf(files);
        const auto analysis = rekordbox::readTrackAnalysis(source, AnalyzePath);
        assert(analysis.beats.size() == input.beats.size());
        for (std::size_t i = 0; i < input.beats.size(); ++i) {
            assert(analysis.beats[i].timeMs == std::floor(input.beats[i].timeMs));
            assert(analysis.beats[i].beatInBar == input.beats[i].beatInBar);
        }
        assert(analysis.waveform.size() == 400);

        // Tempo per beat, from the gap to the next (the last from the gap
        // before): 125 BPM, then 126.
        const std::string pqtz = *sectionBytes(files.dat, "PQTZ");
        assert(be32(pqtz, 20) == input.beats.size());
        for (std::size_t i = 0; i < input.beats.size(); ++i) {
            const std::uint16_t tempo = be16(pqtz, 24 + 8 * i + 2);
            assert(tempo == (i < 32 ? 12500 : 12600));
        }

        // PQT2: the first and last beat, the count, the sum of every PQTZ
        // field, and each beat's microseconds.
        const std::string pqt2 = *sectionBytes(files.ext, "PQT2");
        assert(be32(pqt2, 4) == 56 && pqt2.size() == 56 + 2 * input.beats.size());
        assert(pqt2.substr(24, 8) == pqtz.substr(24, 8));
        assert(pqt2.substr(32, 8) == pqtz.substr(24 + 8 * (input.beats.size() - 1), 8));
        assert(be32(pqt2, 40) == input.beats.size());
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < input.beats.size(); ++i) {
            sum += be16(pqtz, 24 + 8 * i) + be16(pqtz, 24 + 8 * i + 2) + be32(pqtz, 24 + 8 * i + 4);
        }
        assert(be32(pqt2, 44) == sum);
        for (std::size_t i = 0; i < input.beats.size(); ++i) {
            const double t = input.beats[i].timeMs;
            const auto micro = static_cast<std::uint16_t>(std::lround((t - std::floor(t)) * 1000.0));
            assert(be16(pqt2, 56 + 2 * i) == micro);
        }
        assert(be16(pqt2, 56) == 925);  // 52.925 ms

        // No beats: an empty PQTZ, and no PQT2 at all.
        rekordbox::NewAnlzInput noGrid = input;
        noGrid.beats.clear();
        const auto plain = rekordbox::buildAnlzFiles(noGrid);
        assert(be32(*sectionBytes(plain.dat, "PQTZ"), 20) == 0 && sectionBytes(plain.dat, "PQTZ")->size() == 24);
        assert(!sectionBytes(plain.ext, "PQT2"));
        MapSource plainSource = sourceOf(plain);
        assert(rekordbox::readTrackAnalysis(plainSource, AnalyzePath).beats.empty());

        // Beats without their place in the bar count on from the one before.
        rekordbox::NewAnlzInput counted = input;
        for (auto &b : counted.beats) {
            b.beatInBar = 0;
        }
        counted.beats[2].beatInBar = 4;
        const auto countedPqtz = *sectionBytes(rekordbox::buildAnlzFiles(counted).dat, "PQTZ");
        assert(be16(countedPqtz, 24) == 1 && be16(countedPqtz, 32) == 2 && be16(countedPqtz, 40) == 4 &&
               be16(countedPqtz, 48) == 1);
        std::cout << "case 4 (" << analysis.beats.size()
                  << " beats back to the ms, tempo per beat, PQT2 sum and microseconds; no grid, no PQT2) OK\n";
    }

    // ---- the waveforms read back ---------------------------------------------
    {
        MapSource source = sourceOf(files);
        const auto sections = rekordbox::readWaveformSections(source, AnalyzePath);
        assert(sections.threeBandPreview && *sections.threeBandPreview == files.threeBandPreview);
        assert(sections.colourPreview && *sections.colourPreview == files.colourPreview);
        assert(files.threeBandPreview.size() == 1200 * 3 && files.colourPreview.size() == 1200 * 6);
        assert(sections.scrollEntries && *sections.scrollEntries == files.scrollEntries);
        assert(files.scrollEntries == input.waveform.entries.size());
        assert(files.scrollEntries == domain::waveformEntryCount(21 * Rate, Rate));

        const auto detail = rekordbox::readDetailWaveform(source, AnalyzePath);
        assert(detail.columns.size() == files.scrollEntries);
        assert(std::fabs(detail.msPerColumn - 1000.0 / 150.0) < 1e-9);

        // Sizes of the scrolling sections follow the count.
        assert(sectionBytes(files.ext, "PWV3")->size() == 24 + files.scrollEntries);
        assert(sectionBytes(files.ext, "PWV5")->size() == 24 + 2 * files.scrollEntries);
        assert(sectionBytes(files.twoEx, "PWV7")->size() == 24 + 3 * files.scrollEntries);

        // The synthetic track is all kick and bass with quieter hats: the
        // low band is the tallest on average, in PWV7 and in PWV6.
        double low = 0, mid = 0, high = 0;
        for (const auto &c : detail.columns) {
            low += c.low;
            mid += c.mid;
            high += c.high;
        }
        assert(low > mid && low > high && high > 0.0);
        double previewLow = 0, previewMid = 0;
        for (std::size_t j = 0; j < 1200; ++j) {
            previewLow += static_cast<unsigned char>(files.threeBandPreview[3 * j]);
            previewMid += static_cast<unsigned char>(files.threeBandPreview[3 * j + 1]);
        }
        assert(previewLow > previewMid);

        // PWAV scaled to its loudest column, 25, as rekordbox's are.
        const std::string pwav = *sectionBytes(files.dat, "PWAV");
        int tallest = 0;
        for (std::size_t j = 20; j < pwav.size(); ++j) {
            tallest = std::max(tallest, static_cast<unsigned char>(pwav[j]) & 0x1F);
        }
        assert(tallest == 25);
        const std::string pwv2 = *sectionBytes(files.dat, "PWV2");
        int tallestTiny = 0;
        for (std::size_t j = 20; j < pwv2.size(); ++j) {
            assert((static_cast<unsigned char>(pwv2[j]) & 0xF0) == 0);
            tallestTiny = std::max(tallestTiny, static_cast<int>(static_cast<unsigned char>(pwv2[j])));
        }
        assert(tallestTiny == 15);

        // No audio, only a length: flat waveforms of the right length.
        rekordbox::NewAnlzInput flat;
        flat.trackPath = TrackPath;
        flat.durationMs = 2000.0;
        flat.sampleRateHz = Rate;
        const auto flatFiles = rekordbox::buildAnlzFiles(flat);
        assert(flatFiles.scrollEntries == 300);
        assert(std::all_of(flatFiles.threeBandPreview.begin(), flatFiles.threeBandPreview.end(),
                           [](char c) { return c == 0; }));
        std::cout << "case 5 (PWV6, PWV4, scroll count and PWV7 read back; sizes; bands in the right places) OK\n";
    }

    // ---- the empty cue lists ----------------------------------------------------
    {
        for (const std::string *file : {&files.dat, &files.ext}) {
            const auto hot = *sectionBytes(*file, "PCOB", 0);
            const auto memory = *sectionBytes(*file, "PCOB", 1);
            rekordbox::AnlzLegacyCueCodec::checkSection(hot);
            rekordbox::AnlzLegacyCueCodec::checkSection(memory);
            assert(be32(hot, 12) == rekordbox::CueListTypeHot);
            assert(be32(memory, 12) == rekordbox::CueListTypeMemory);
            assert(rekordbox::AnlzLegacyCueCodec::decodeCues(hot).empty());
            assert(rekordbox::AnlzLegacyCueCodec::decodeCues(memory).empty());
            // The memory list's end marker: "no last entry".
            assert(rekordbox::AnlzLegacyCueCodec::memoryCountOf(memory) == 0xFFFFFFFFu);
            assert(rekordbox::AnlzLegacyCueCodec::memoryCountOf(hot) == 0xFFFFFFFFu);
        }
        const auto hot2 = *sectionBytes(files.ext, "PCO2", 0);
        const auto memory2 = *sectionBytes(files.ext, "PCO2", 1);
        assert(be32(hot2, 12) == rekordbox::CueListTypeHot && be32(memory2, 12) == rekordbox::CueListTypeMemory);
        assert(rekordbox::AnlzCueCodec::decodeHotCues(hot2, rekordbox::CueListTypeHot).empty());
        assert(rekordbox::AnlzCueCodec::decodeHotCues(memory2, rekordbox::CueListTypeMemory).empty());
        std::cout << "case 6 (empty PCOB pairs with the memory end marker, empty PCO2 pair) OK\n";
    }

    // ---- PVBR -----------------------------------------------------------------
    {
        const auto pvbr = *sectionBytes(files.dat, "PVBR");
        assert(pvbr.size() == 1620 && pvbr.find_first_not_of('\0', 16) == std::string::npos);
        rekordbox::NewAnlzInput mp3 = input;
        mp3.mp3SampleCount = 11869 * 1152;
        const auto cbr = *sectionBytes(rekordbox::buildAnlzFiles(mp3).dat, "PVBR");
        assert(be32(cbr, 16 + 4 * 400) == 11869u * 1152u);
        assert(cbr.find_first_not_of('\0', 16) >= 16 + 4 * 400);
        mp3.mp3SeekOffsets.resize(400);
        for (std::uint32_t i = 0; i < 400; ++i) {
            mp3.mp3SeekOffsets[i] = 1954 + i * 1000;
        }
        const auto vbr = *sectionBytes(rekordbox::buildAnlzFiles(mp3).dat, "PVBR");
        assert(be32(vbr, 16) == 1954 && be32(vbr, 16 + 4 * 399) == 1954 + 399000);
        mp3.mp3SeekOffsets.resize(399);
        assert(throwsInvalid([&] { rekordbox::buildAnlzFiles(mp3); }));
        std::cout << "case 7 (PVBR: zeros, the MP3 sample count last, a 400-entry seek table) OK\n";
    }

    // ---- inputs that cannot make a file ------------------------------------------
    {
        auto with = [&](auto change) {
            rekordbox::NewAnlzInput bad = input;
            change(bad);
            return throwsInvalid([&] { rekordbox::buildAnlzFiles(bad); });
        };
        assert(with([](auto &b) { b.trackPath = ""; }));
        assert(with([](auto &b) { b.trackPath = "Contents/a.wav"; }));
        assert(with([](auto &b) { b.trackPath = "/Contents/\xC3("; }));
        assert(with([](auto &b) { b.trackPath = "/Contents/\xED\xA0\x80.wav"; }));  // a surrogate in UTF-8
        assert(with([](auto &b) { std::swap(b.beats[3], b.beats[4]); }));
        assert(with([](auto &b) { b.beats[0].timeMs = -1.0; }));
        assert(with([](auto &b) { b.beats[5].timeMs = std::nan(""); }));
        assert(with([](auto &b) { b.beats[6].timeMs = b.beats[5].timeMs + 0.05; }));
        assert(with([](auto &b) {
            b.waveform = {};
            b.durationMs = 0.0;
        }));
        assert(with([](auto &b) { b.sampleRateHz = 48000; }));
        std::cout << "case 8 (bad path, unordered or impossible beats, no length, rate mismatch: refused) OK\n";
    }

    // ---- where the files go -------------------------------------------------------
    {
        const std::regex shape("/PIONEER/USBANLZ/P0[0-7][0-9A-F]/000[0-2][0-9A-F]{4}/ANLZ0000\\.DAT");
        const std::string first = rekordbox::newAnlzPathFor(TrackPath, {});
        assert(std::regex_match(first, shape));
        assert(rekordbox::newAnlzPathFor(TrackPath, {}) == first);
        assert(rekordbox::newAnlzPathFor("/Contents/Other/track.wav", {}) != first);

        // A folder with any file named in it is taken, however cased.
        const std::string folder = first.substr(0, first.rfind('/'));
        std::string lower = folder + "/ANLZ0001.DAT";
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        const std::unordered_set<std::string> taken{lower};
        const std::string second = rekordbox::newAnlzPathFor(TrackPath, taken);
        assert(std::regex_match(second, shape));
        assert(second.substr(0, second.rfind('/')) != folder);

        // Many tracks, no two in one folder.
        std::unordered_set<std::string> used;
        for (int i = 0; i < 2000; ++i) {
            const std::string p = rekordbox::newAnlzPathFor("/Contents/A/" + std::to_string(i % 50) + ".wav", used);
            assert(std::regex_match(p, shape));
            assert(used.insert(p).second);
        }
        std::cout << "case 9 (a free folder of rekordbox's shape, stable per path, never one in use) OK\n";
    }

    std::cout << "anlz_builder_test: all cases passed\n";
    return 0;
}
