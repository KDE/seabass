// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A new analysis merged into a track's own analysis files (Analyse
// again), on a pair rekordbox wrote (tests/fixtures/testshark_t73, with
// phrases and an unknown PVB2 section in its .EXT):
//
// 1. A grid a few ms off the old one: grid and waveform sections are the
//    built file's, every other section of the old file is there byte for
//    byte in its order (cue lists, PPTH, PVBR, PVB2), the phrases kept.
// 2. A section the old file lacks goes where the built file has it.
// 3. A grid at another tempo, and one half a beat off: the phrases go.
// 4. The quarter-beat rule at its edge.
// 5. A missing .2EX is the built one whole; a missing .EXT stays
//    missing; a missing .DAT is no merge at all.
// 6. A new analysis without a grid takes the old grid out with it.
// 7. The merged files write back over the copies they were read from.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

std::uint32_t fourcc(const char *text)
{
    return readU32BE(std::string(text, 4), 0);
}

std::vector<std::string> namesOf(const AnlzFile &file)
{
    std::vector<std::string> names;
    for (const auto &section : file.sections) {
        names.push_back(section.rawBytes.substr(0, 4));
    }
    return names;
}

const AnlzRawSection *find(const AnlzFile &file, const char *name)
{
    for (const auto &section : file.sections) {
        if (section.fourcc == fourcc(name)) {
            return &section;
        }
    }
    return nullptr;
}

// The sections that are not replaced, in order, as bytes.
std::vector<std::string> keptOf(const AnlzFile &file, bool withPhrases)
{
    std::vector<std::string> kept;
    for (const auto &section : file.sections) {
        if (!isReplacedAnalysisSection(section.fourcc) && (withPhrases || section.fourcc != fourcc("PSSI"))) {
            kept.push_back(section.rawBytes);
        }
    }
    return kept;
}

// The built files for `beats` (ms), with a flat waveform as long as the
// old track.
NewAnlzFiles builtFor(const std::vector<double> &beats, double durationMs)
{
    NewAnlzInput input;
    input.trackPath = "/Contents/Somebody/Something.mp3";
    for (std::size_t i = 0; i < beats.size(); ++i) {
        if (beats[i] >= 0.0 && beats[i] <= durationMs) {
            input.beats.push_back({beats[i], static_cast<int>(i % 4) + 1});
        }
    }
    input.durationMs = durationMs;
    input.sampleRateHz = 44100;
    return buildAnlzFiles(input);
}

}  // namespace

int main()
{
    const fs::path fixture = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "testshark_t73" / "stick" / "PIONEER"
                             / "USBANLZ" / "P013" / "00020F27";
    const fs::path scratch = seabass::testing::scratchRoot() / "anlz_analysis_merge_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    fs::copy_file(fixture / "ANLZ0000.DAT", scratch / "ANLZ0000.DAT");
    fs::copy_file(fixture / "ANLZ0000.EXT", scratch / "ANLZ0000.EXT");
    const std::string datPath = seabass::pathToUtf8(scratch / "ANLZ0000.DAT");
    const std::string extPath = seabass::pathToUtf8(scratch / "ANLZ0000.EXT");
    TrackAnlzFiles old;
    old.dat = AnlzFile::readRaw(datPath);
    old.ext = AnlzFile::readRaw(extPath);
    assert(find(*old.ext, "PSSI") && find(*old.ext, "PVB2") && "the fixture has phrases and an unknown section");
    const std::vector<double> oldBeats = pqtzBeatTimes(*old.dat);
    assert(oldBeats.size() > 100);
    const double beat = oldBeats[1] - oldBeats[0];
    const double durationMs = oldBeats.back() + 2000.0;
    std::cerr << "  old grid: " << oldBeats.size() << " beats, " << beat << " ms apart" << std::endl;

    // 1.
    {
        std::vector<double> near = oldBeats;
        for (double &t : near) {
            t += 5.0;
        }
        const NewAnlzFiles built = builtFor(near, durationMs);
        const auto merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        assert(merged && merged->files.dat && merged->files.ext && merged->files.twoEx);
        assert(merged->phrasesKept && !merged->phrasesRemoved);
        assert(namesOf(*merged->files.dat) == namesOf(*old.dat));
        assert(namesOf(*merged->files.ext) == namesOf(*old.ext));
        assert(keptOf(*merged->files.dat, true) == keptOf(*old.dat, true));
        assert(keptOf(*merged->files.ext, true) == keptOf(*old.ext, true));
        assert(merged->files.dat->headerBytes == old.dat->headerBytes);
        const AnlzFile builtDat = AnlzFile::fromBytes(built.dat, "built");
        const AnlzFile builtExt = AnlzFile::fromBytes(built.ext, "built");
        for (const char *name : {"PQTZ", "PWAV", "PWV2"}) {
            assert(find(*merged->files.dat, name)->rawBytes == find(builtDat, name)->rawBytes);
            assert(find(*merged->files.dat, name)->rawBytes != find(*old.dat, name)->rawBytes);
        }
        for (const char *name : {"PWV3", "PQT2", "PWV5", "PWV4"}) {
            assert(find(*merged->files.ext, name)->rawBytes == find(builtExt, name)->rawBytes);
        }
        // The old path stays: the built one names another file.
        assert(find(*merged->files.dat, "PPTH")->rawBytes == find(*old.dat, "PPTH")->rawBytes);
        assert(find(builtDat, "PPTH")->rawBytes != find(*old.dat, "PPTH")->rawBytes);
        const std::vector<double> mergedBeats = pqtzBeatTimes(*merged->files.dat);
        assert(mergedBeats.size() == near.size() && std::abs(mergedBeats[10] - near[10]) < 1.0);
        std::cout << "case 1 (grid and waveforms replaced, every other section byte for byte, phrases kept) OK\n";
    }

    // 2.
    {
        TrackAnlzFiles lacking = old;
        auto &sections = lacking.ext->sections;
        sections.erase(std::find_if(sections.begin(), sections.end(),
                                    [](const AnlzRawSection &s) { return s.fourcc == fourcc("PWV3"); }));
        sections.erase(std::find_if(sections.begin(), sections.end(),
                                    [](const AnlzRawSection &s) { return s.fourcc == fourcc("PWV4"); }));
        const NewAnlzFiles built = builtFor(oldBeats, durationMs);
        const auto merged = mergeTrackAnlzFiles(lacking, built.dat, built.ext, built.twoEx);
        // Built: PPTH PWV3 PCOB PCOB PCO2 PCO2 PQT2 PWV5 PWV4.
        assert(namesOf(*merged->files.ext) == namesOf(*old.ext));
        std::cout << "case 2 (sections the old file lacks go where the built file has them) OK\n";
    }

    // 3.
    {
        std::vector<double> faster;
        for (std::size_t i = 0; i < oldBeats.size(); ++i) {
            faster.push_back(oldBeats[0] + static_cast<double>(i) * beat * 0.97);
        }
        NewAnlzFiles built = builtFor(faster, durationMs);
        auto merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        assert(merged->phrasesRemoved && !merged->phrasesKept && !find(*merged->files.ext, "PSSI"));
        assert(keptOf(*merged->files.ext, false) == keptOf(*old.ext, false));
        assert(find(*merged->files.ext, "PVB2")->rawBytes == find(*old.ext, "PVB2")->rawBytes);

        std::vector<double> halfOff = oldBeats;
        for (double &t : halfOff) {
            t += beat / 2.0;
        }
        built = builtFor(halfOff, durationMs);
        merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        assert(merged->phrasesRemoved && !find(*merged->files.ext, "PSSI"));
        std::cout << "case 3 (another tempo, or half a beat off: the phrases go, the rest stays) OK\n";
    }

    // 4.
    {
        const std::vector<double> grid{1000.0, 1500.0, 2000.0, 2500.0};
        const auto shifted = [&](double firstBy, double lastBy) {
            std::vector<double> moved = grid;
            moved.front() += firstBy;
            moved.back() += lastBy;
            return moved;
        };
        assert(phrasesFitNewGrid(grid, shifted(124.0, -124.0)));
        assert(!phrasesFitNewGrid(grid, shifted(126.0, 0.0)));
        assert(!phrasesFitNewGrid(grid, shifted(0.0, -126.0)));
        // The last beat both grids have: a shorter new grid is compared
        // where it ends.
        assert(phrasesFitNewGrid(grid, {1000.0, 1500.0, 2000.0}));
        assert(!phrasesFitNewGrid(grid, {1000.0, 1500.0, 2200.0}));
        assert(!phrasesFitNewGrid(grid, {1000.0}) && !phrasesFitNewGrid({}, grid));
        std::cout << "case 4 (a quarter of a beat, at the first beat and the last both grids have) OK\n";
    }

    // 5.
    {
        const NewAnlzFiles built = builtFor(oldBeats, durationMs);
        auto merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        assert(merged->files.twoEx && merged->files.twoEx->toBytes() == built.twoEx);
        TrackAnlzFiles noExt = old;
        noExt.ext.reset();
        merged = mergeTrackAnlzFiles(noExt, built.dat, built.ext, built.twoEx);
        assert(merged && merged->files.dat && !merged->files.ext && !merged->phrasesKept && !merged->phrasesRemoved);
        TrackAnlzFiles noDat = old;
        noDat.dat.reset();
        assert(!mergeTrackAnlzFiles(noDat, built.dat, built.ext, built.twoEx));
        std::cout << "case 5 (a missing .2EX is the built one, a missing .EXT stays missing, no .DAT no merge) OK\n";
    }

    // 6.
    {
        const NewAnlzFiles built = builtFor({}, durationMs);
        const auto merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        assert(!find(*merged->files.ext, "PQT2") && pqtzBeatTimes(*merged->files.dat).empty());
        assert(merged->phrasesRemoved);
        std::cout << "case 6 (no new grid: the old one goes with the old waveforms) OK\n";
    }

    // 7.
    {
        std::vector<double> near = oldBeats;
        for (double &t : near) {
            t -= 3.0;
        }
        const NewAnlzFiles built = builtFor(near, durationMs);
        const auto merged = mergeTrackAnlzFiles(old, built.dat, built.ext, built.twoEx);
        merged->files.dat->writeRaw(datPath);
        merged->files.ext->writeRaw(extPath);
        assert(AnlzFile::readRaw(datPath).toBytes() == merged->files.dat->toBytes());
        assert(AnlzFile::readRaw(extPath).toBytes() == merged->files.ext->toBytes());
        // A copy that changed since it was read is not written over.
        std::ofstream(scratch / "ANLZ0000.DAT", std::ios::binary | std::ios::app) << "x";
        bool refused = false;
        try {
            merged->files.dat->writeRaw(datPath);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
        std::cout << "case 7 (written back over the files read, refused over one that changed) OK\n";
    }

    std::cout << "anlz_analysis_merge_test: all cases passed\n";
    return 0;
}
