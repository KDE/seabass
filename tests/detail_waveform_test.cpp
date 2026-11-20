// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The scrolling waveform the zoomed popup draws (#104), read from
// rekordbox's analysis files: PWV7 from the .2EX (three bands), else
// PWV3 from the .EXT (one byte, as PWAV), 150 columns a second, and
// nothing where neither is there. The committed fixtures carry no
// waveform sections (the anonymiser drops them), so the files are made
// here, byte by byte, in the layout the generated parser reads.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "domain/waveform.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

using namespace seabass;
namespace rb = seabass::infrastructure::rekordbox;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

void u32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

// One waveform section: fourcc, header and tag lengths, then entry
// width, entry count, a word nobody reads, and the entries.
std::string section(const std::string &fourcc, std::uint32_t entryBytes, const std::string &entries)
{
    std::string out = fourcc;
    u32(out, 24);
    u32(out, static_cast<std::uint32_t>(24 + entries.size()));
    u32(out, entryBytes);
    u32(out, static_cast<std::uint32_t>(entries.size() / entryBytes));
    u32(out, 0);
    return out + entries;
}

// A whole analysis file: the PMAI header and its sections.
std::string anlzFile(const std::string &sections)
{
    std::string out = "PMAI";
    u32(out, 28);
    u32(out, static_cast<std::uint32_t>(28 + sections.size()));
    out.append(16, '\0');
    return out + sections;
}

class MemorySource : public rb::AnlzByteSource
{
public:
    std::map<std::string, std::string> files;
    std::optional<std::string> read(const std::string &relativePath) override
    {
        const auto it = files.find(relativePath);
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

const std::string AnalyzePath = "/PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.DAT";

std::string bytes(std::initializer_list<int> values)
{
    std::string out;
    for (int v : values) {
        out.push_back(static_cast<char>(v));
    }
    return out;
}

}  // namespace

int main()
{
    const std::string twoEx = rb::anlzSiblingRelativePath(AnalyzePath, ".2EX");
    const std::string ext = rb::anlzSiblingRelativePath(AnalyzePath, ".EXT");
    // PWV7: low, mid, high, 0 to 127. PWV3: a 5-bit height, 3 bits of
    // whiteness: 0xFF is full height, all white; 0x10 half height, none.
    const std::string pwv7 = section("PWV7", 3, bytes({127, 0, 0, 0, 127, 0, 0, 0, 127, 254, 64, 0}));
    const std::string pwv3 = section("PWV3", 1, bytes({0xFF, 0x10}));

    {
        MemorySource source;
        source.files[twoEx] = anlzFile(pwv7);
        source.files[ext] = anlzFile(pwv3);
        const domain::DetailWaveform detail = rb::readDetailWaveform(source, AnalyzePath);
        check(detail.columns.size() == 4, "PWV7's four columns, not PWV3's two: " + std::to_string(detail.columns.size()));
        if (detail.columns.size() == 4) {
            check(near(detail.columns[0].low, 1.0) && near(detail.columns[0].mid, 0.0) && near(detail.columns[0].high, 0.0),
                  "the first byte is the low band");
            check(near(detail.columns[1].mid, 1.0) && near(detail.columns[1].low, 0.0), "the second the mid");
            check(near(detail.columns[2].high, 1.0) && near(detail.columns[2].mid, 0.0), "the third the high");
            check(near(detail.columns[3].low, 1.0), "past 127 is still full height");
            check(near(detail.columns[3].mid, 64.0 / 127.0), "each band out of 127");
        }
        check(near(detail.msPerColumn, 1000.0 / 150.0), "150 columns a second");
    }
    {
        MemorySource source;
        source.files[ext] = anlzFile(pwv3);
        const domain::DetailWaveform detail = rb::readDetailWaveform(source, AnalyzePath);
        check(detail.columns.size() == 2, "without a .2EX, PWV3's columns");
        if (detail.columns.size() == 2) {
            check(near(detail.columns[0].low, 1.0) && near(detail.columns[0].high, 1.0), "full height, all white");
            check(near(detail.columns[1].low, 16.0 / 31.0) && near(detail.columns[1].high, 0.0), "half height, no white");
        }
        check(near(detail.msPerColumn, 1000.0 / 150.0), "PWV3 is 150 a second too");
    }
    {
        // A .2EX that does not parse, and one whose PWV7 is not three
        // bytes an entry: neither costs the .EXT's fallback.
        for (const std::string &bad : {std::string("not an analysis file"),
                                       anlzFile(section("PWV7", 2, bytes({1, 2, 3, 4})))}) {
            MemorySource source;
            source.files[twoEx] = bad;
            source.files[ext] = anlzFile(pwv3);
            const domain::DetailWaveform detail = rb::readDetailWaveform(source, AnalyzePath);
            check(detail.columns.size() == 2, "a bad .2EX falls back to PWV3");
        }
    }
    {
        // An .EXT without PWV3 (the anonymised fixtures' kind), or no
        // files at all: nothing, and no spacing to go with it.
        MemorySource source;
        source.files[ext] = anlzFile(section("PWV4", 6, std::string(12, '\x10')));
        const domain::DetailWaveform none = rb::readDetailWaveform(source, AnalyzePath);
        check(none.columns.empty() && none.msPerColumn == 0.0, "no scrolling waveform, nothing");
        MemorySource empty;
        const domain::DetailWaveform missing = rb::readDetailWaveform(empty, AnalyzePath);
        check(missing.columns.empty() && missing.msPerColumn == 0.0, "no files, nothing");
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "detail_waveform_test: all checks passed\n";
    return 0;
}
