// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A waveform says how it is drawn (domain::WaveformLook): rekordbox's
// 3Band where the analysis has PWV6 or PWV7, rekordbox's Blue where it has
// only PWAV or PWV3, Engine's for Engine's. And rekordbox's 3Band
// overview is read as a section alone (AnlzByteSource::readSection), not
// as the whole .2EX it ends.
//
// No committed fixture has a PWV6 or PWV7 (see
// rekordbox_waveform_sections_test.cpp), so the analysis files here are
// built by hand, section by section, as rekordbox lays them out: PWV7
// first, then PWV6.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/waveform.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
namespace rekordbox = seabass::infrastructure::rekordbox;
using seabass::domain::WaveformLook;

namespace
{

// Serves files from a map and remembers what it was asked for.
class MapSource : public rekordbox::AnlzByteSource
{
public:
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;

    std::optional<std::string> read(const std::string &relativePath) override
    {
        asked.push_back(relativePath);
        auto it = files.find(relativePath);
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

void putU32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

std::string section(const std::string &fourcc, const std::vector<std::uint32_t> &fields, const std::string &body)
{
    std::string out = fourcc;
    const auto headerLength = static_cast<std::uint32_t>(12 + 4 * fields.size());
    putU32(out, headerLength);
    putU32(out, headerLength + static_cast<std::uint32_t>(body.size()));
    for (auto f : fields) {
        putU32(out, f);
    }
    return out + body;
}

std::string anlzFile(const std::vector<std::string> &sections)
{
    std::string body;
    for (const auto &s : sections) {
        body += s;
    }
    std::string out = "PMAI";
    putU32(out, 28);
    putU32(out, static_cast<std::uint32_t>(28 + body.size()));
    out += std::string(16, '\0');
    return out + body;
}

// Three-band entries: low, mid, high per entry, `n` of them.
std::string threeBand(std::size_t n, std::uint8_t low, std::uint8_t mid, std::uint8_t high)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(low));
        out.push_back(static_cast<char>(mid));
        out.push_back(static_cast<char>(high));
    }
    return out;
}

const std::string AnalyzePath = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
const std::string DatPath = "USBANLZ/P001/00000001/ANLZ0000.DAT";
const std::string ExtPath = "USBANLZ/P001/00000001/ANLZ0000.EXT";
const std::string TwoExPath = "USBANLZ/P001/00000001/ANLZ0000.2EX";

// A .DAT with a PWAV of 400 columns, height 31, whiteness 7.
std::string datFile()
{
    return anlzFile({section("PWAV", {400, 0x10000}, std::string(400, static_cast<char>(0xFF)))});
}

// A .2EX as rekordbox writes one: path, PWV7 (150 a second), then PWV6
// (1,200 entries), each with its 3-byte entries.
std::string twoExFile(std::uint32_t pwv6EntryBytes = 3)
{
    return anlzFile({section("PPTH", {8}, std::string(8, 'p')),
                     section("PWV7", {3, 3000, 0x960000}, threeBand(3000, 100, 50, 20)),
                     section("PWV6", {pwv6EntryBytes, 1200}, threeBand(1200, 127, 63, 0))});
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

}  // namespace

int main()
{
    // ---- 1. the look's names --------------------------------------------
    {
        assert(std::string(seabass::domain::waveformLookName(WaveformLook::RekordboxThreeBand)) == "rekordbox-3band");
        assert(std::string(seabass::domain::waveformLookName(WaveformLook::RekordboxBlue)) == "rekordbox-blue");
        assert(std::string(seabass::domain::waveformLookName(WaveformLook::Engine)) == "engine");
        assert(std::string(seabass::domain::waveformLookName(WaveformLook::Unknown)).empty());
        std::cout << "case 1 (the looks' names, as QML takes them) OK\n";
    }

    // ---- 2. one section out of a file ----------------------------------
    {
        const std::string file = twoExFile();
        const auto pwv6 = rekordbox::anlzSectionOf(file, "PWV6");
        assert(pwv6 && pwv6->size() == 20 + 3600 && pwv6->compare(0, 4, "PWV6") == 0);
        assert(file.compare(file.size() - pwv6->size(), pwv6->size(), *pwv6) == 0 && "the last section, whole");
        assert(rekordbox::anlzSectionOf(file, "PWV7")->size() == 24 + 9000);
        assert(!rekordbox::anlzSectionOf(file, "PWV5") && "a section the file lacks");
        assert(!rekordbox::anlzSectionOf("not an analysis file at all", "PWV6"));
        std::string cut = file.substr(0, file.size() - 100);
        assert(!rekordbox::anlzSectionOf(cut, "PWV6") && "a section running past the end of the file");
        std::string lying = file;
        lying[28 + 8 + 3] = static_cast<char>(5);  // PPTH says it is 5 bytes long, less than a header
        assert(!rekordbox::anlzSectionOf(lying, "PWV6"));
        std::cout << "case 2 (one section out of a file; none from a short, cut or damaged one) OK\n";
    }

    // ---- 3. the same from a file on disk, by seeking -------------------
    {
        const fs::path root = seabass::testing::scratchRoot() / "seabass_waveform_looks_test" / "PIONEER";
        fs::remove_all(root.parent_path());
        fs::create_directories(root / "USBANLZ" / "P001" / "00000001");
        const std::string file = twoExFile();
        std::ofstream(root / seabass::pathFromUtf8(TwoExPath), std::ios::binary) << file;
        rekordbox::FilesystemAnlzSource source(seabass::pathToUtf8(root));
        assert(source.readSection(TwoExPath, "PWV6") == rekordbox::anlzSectionOf(file, "PWV6"));
        assert(source.readSection(TwoExPath, "PWV7") == rekordbox::anlzSectionOf(file, "PWV7"));
        assert(!source.readSection(TwoExPath, "PWV5"));
        assert(!source.readSection("USBANLZ/P001/00000001/ANLZ0001.2EX", "PWV6") && "no such file");
        // A PWV7 of 60 KB, as a real track's: PWV6 is found in the file's
        // last 16 KB, PWV7 only by the walk from the start; both whole.
        const std::string big = anlzFile({section("PPTH", {8}, std::string(8, 'p')),
                                          section("PWV7", {3, 20000, 0x960000}, threeBand(20000, 9, 8, 7)),
                                          section("PWV6", {3, 1200}, threeBand(1200, 127, 63, 0)),
                                          section("PWVC", {0, 0}, "")});
        std::ofstream(root / seabass::pathFromUtf8(TwoExPath), std::ios::binary | std::ios::trunc) << big;
        assert(source.readSection(TwoExPath, "PWV6") == rekordbox::anlzSectionOf(big, "PWV6"));
        assert(source.readSection(TwoExPath, "PWV7") == rekordbox::anlzSectionOf(big, "PWV7"));
        assert(source.readSection(TwoExPath, "PWVC") == rekordbox::anlzSectionOf(big, "PWVC"));
        // A "PWV6" inside a later section's bytes, the first one a search
        // from the end meets, is not taken for the section: it does not
        // chain to the end of the file.
        std::string decoy = anlzFile({section("PWV6", {3, 1200}, threeBand(1200, 1, 2, 3)),
                                      section("PWV7", {3, 20}, "xxPWV6" + std::string(54, '\x05'))});
        assert(decoy.rfind("PWV6") > decoy.find("PWV7") && "the decoy comes last");
        std::ofstream(root / seabass::pathFromUtf8(TwoExPath), std::ios::binary | std::ios::trunc) << decoy;
        const auto real = rekordbox::anlzSectionOf(decoy, "PWV6");
        assert(real && real->find(threeBand(1200, 1, 2, 3)) != std::string::npos);
        assert(source.readSection(TwoExPath, "PWV6") == real);
        fs::remove_all(root.parent_path());
        std::cout << "case 3 (a file on disk gives the same section, from its end or by the walk) OK\n";
    }

    // ---- 4. the overview's look -----------------------------------------
    {
        MapSource source;
        source.files[DatPath] = datFile();
        source.files[TwoExPath] = twoExFile();

        // Asked for the grid alone: PWAV, Blue, and no second file read.
        const auto plain = rekordbox::readTrackAnalysis(source, AnalyzePath);
        assert(plain.waveform.size() == 400 && plain.waveformLook == WaveformLook::RekordboxBlue);
        assert(source.asked == std::vector<std::string>{DatPath});

        // Asked for a waveform to draw: PWV6, 3Band, each band of 127.
        source.asked.clear();
        const auto drawn = rekordbox::readTrackAnalysis(source, AnalyzePath, /*threeBandOverview=*/true);
        assert(drawn.waveform.size() == 1200 && drawn.waveformLook == WaveformLook::RekordboxThreeBand);
        assert(near(drawn.waveform[0].low, 1.0) && near(drawn.waveform[0].mid, 63.0 / 127.0)
               && near(drawn.waveform[0].high, 0.0));
        assert(source.asked.size() == 2 && source.asked[1] == TwoExPath);

        // No .2EX: PWAV stands, Blue.
        source.files.erase(TwoExPath);
        const auto noTwoEx = rekordbox::readTrackAnalysis(source, AnalyzePath, true);
        assert(noTwoEx.waveform.size() == 400 && noTwoEx.waveformLook == WaveformLook::RekordboxBlue);

        // A PWV6 whose entries are not three bytes: not understood, Blue.
        source.files[TwoExPath] = twoExFile(2);
        const auto odd = rekordbox::readTrackAnalysis(source, AnalyzePath, true);
        assert(odd.waveform.size() == 400 && odd.waveformLook == WaveformLook::RekordboxBlue);
        std::cout << "case 4 (overview: 3Band from PWV6 where asked and there, Blue from PWAV otherwise) OK\n";
    }

    // ---- 5. the detail's look -------------------------------------------
    {
        MapSource source;
        source.files[TwoExPath] = twoExFile();
        const auto threeBandDetail = rekordbox::readDetailWaveform(source, AnalyzePath);
        assert(threeBandDetail.columns.size() == 3000 && threeBandDetail.look == WaveformLook::RekordboxThreeBand);

        MapSource older;
        older.files[ExtPath] = anlzFile({section("PWV3", {1, 1500, 0x960000}, std::string(1500, static_cast<char>(0x3F)))});
        const auto blueDetail = rekordbox::readDetailWaveform(older, AnalyzePath);
        assert(blueDetail.columns.size() == 1500 && blueDetail.look == WaveformLook::RekordboxBlue);

        MapSource none;
        assert(rekordbox::readDetailWaveform(none, AnalyzePath).look == WaveformLook::Unknown);
        std::cout << "case 5 (detail: 3Band from PWV7, Blue from PWV3, unknown from nothing) OK\n";
    }

    std::cout << "rekordbox_waveform_looks_test: all OK\n";
    return 0;
}
