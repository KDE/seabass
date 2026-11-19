// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// readWaveformSections(): the rekordbox waveform sections a Denon
// overview is made from, out of a track's .EXT and .2EX.
//
// What the committed fixtures can show, and what they cannot:
//
//   - anonymized_library has .2EX files, but anonymising stripped every
//     waveform section (PWV3 to PWV7) from them and from the .EXT, so it
//     shows only that a file without the sections reads as "not there";
//   - testshark_t73 is a stick exactly as rekordbox 7.2 wrote it, and its
//     .EXT files carry PWV3, PWV4 and PWV5; it has no .2EX at all, so it
//     shows the PWV4 fallback and the scroll count from the .EXT.
//
// No committed fixture has a PWV6 or PWV7: the three-band preview is
// read here only from a hand-built file (case 4), and from real files
// only by the measuring scripts.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace fs = std::filesystem;
namespace rekordbox = seabass::infrastructure::rekordbox;

namespace
{

fs::path fixture(const std::string &relative)
{
    return seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / seabass::pathFromUtf8(relative);
}

// Serves files from a map, and remembers what it was asked for: the
// proof that the reader goes through the source, the way a browsed
// backup serves analysis files, and asks for the siblings by name.
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

// A section: fourcc, header length, tag length, the header's own fields,
// the body.
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

}  // namespace

int main()
{
    // ---- nothing there ---------------------------------------------------
    {
        const auto none = rekordbox::readWaveformSections("/no/such/stick", "1");
        assert(!none.threeBandPreview && !none.colourPreview && !none.scrollEntries);
        const auto notANumber = rekordbox::readWaveformSections(
            seabass::pathToUtf8(fixture("testshark_t73/stick/PIONEER")), "not-a-number");
        assert(!notANumber.threeBandPreview && !notANumber.colourPreview && !notANumber.scrollEntries);
        MapSource empty;
        const auto noFiles = rekordbox::readWaveformSections(empty, "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT");
        assert(!noFiles.threeBandPreview && !noFiles.colourPreview && !noFiles.scrollEntries);
        // Both siblings asked for by name, through the source.
        assert(empty.asked.size() == 2);
        assert(empty.asked[0] == "USBANLZ/P001/00000001/ANLZ0000.2EX");
        assert(empty.asked[1] == "USBANLZ/P001/00000001/ANLZ0000.EXT");
        std::cout << "case 1 (no stick, no number, no files: all absent, siblings asked by name) OK\n";
    }

    // ---- files without the sections -------------------------------------
    {
        const std::string root = seabass::pathToUtf8(fixture("anonymized_library/rekordbox"));
        rekordbox::KaitaiRekordboxReader reader(root);
        const auto tracks = reader.readAll();
        const rekordbox::AnlzPathIndex index(root);
        std::size_t looked = 0;
        std::size_t with2ex = 0;
        for (const auto &track : tracks) {
            if (looked >= 40) {
                break;
            }
            looked++;
            const auto sections = rekordbox::readWaveformSections(root, track.sourceId, nullptr, &index);
            assert(!sections.threeBandPreview && !sections.colourPreview && !sections.scrollEntries);
            if (fs::exists(fs::path(seabass::pathFromUtf8(root))
                           / seabass::pathFromUtf8(rekordbox::anlzSiblingRelativePath(track.analysisFile, ".2EX")))) {
                with2ex++;
            }
        }
        // The files are there; only their waveform sections are not.
        assert(with2ex > 0 && "the fixture has no .2EX any more: this case no longer shows what it says");
        std::cout << "case 2 (anonymized_library: " << with2ex << " of " << looked
                  << " tracks have a .2EX, none a waveform section: all absent) OK\n";
    }

    // ---- a stick as rekordbox wrote it: the .EXT ---------------------------
    {
        const std::string root = seabass::pathToUtf8(fixture("testshark_t73/stick/PIONEER"));
        rekordbox::KaitaiRekordboxReader reader(root);
        const auto tracks = reader.readAll();
        assert(!tracks.empty());
        std::size_t withColour = 0;
        for (const auto &track : tracks) {
            const auto sections = rekordbox::readWaveformSections(root, track.sourceId);
            assert(!sections.threeBandPreview);  // no .2EX on this stick
            if (!sections.colourPreview) {
                assert(!sections.scrollEntries);
                continue;
            }
            withColour++;
            // 1200 entries of six bytes, as every export measured had.
            assert(sections.colourPreview->size() == 1200 * 6);
            // The scroll count from PWV5 or PWV3: 150 entries a second,
            // so it agrees with the track's length to within a second.
            assert(sections.scrollEntries.has_value());
            assert(std::fabs(*sections.scrollEntries / 150.0 - track.durationSeconds) <= 1.0);
        }
        assert(withColour > 0 && "no track of testshark_t73 has a PWV4 any more");
        std::cout << "case 3 (testshark_t73: " << withColour << " of " << tracks.size()
                  << " tracks have PWV4 and a scroll count matching their length) OK\n";
    }

    // ---- a .2EX, built by hand -----------------------------------------
    //
    // PWV6's header has no field past the entry count; PWV7's has one
    // more, as PWV5's and PWV3's do.
    {
        std::string pwv6;
        for (int i = 0; i < 1200; ++i) {
            pwv6 += std::string{static_cast<char>(i % 256), 20, 30};
        }
        const std::string pwv7Body(3 * 4500, '\x01');
        const std::string pwv5Body(2 * 4400, '\x02');
        const std::string pwv4Body(6 * 1200, '\x03');
        MapSource source;
        source.files["USBANLZ/P001/00000001/ANLZ0000.2EX"] =
            anlzFile({section("PWV7", {3, 4500, 0x960000}, pwv7Body), section("PWV6", {3, 1200}, pwv6)});
        source.files["USBANLZ/P001/00000001/ANLZ0000.EXT"] =
            anlzFile({section("PWV5", {2, 4400, 0x960305}, pwv5Body), section("PWV4", {6, 1200, 0}, pwv4Body)});
        const auto both = rekordbox::readWaveformSections(source, "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT");
        assert(both.threeBandPreview && *both.threeBandPreview == pwv6);
        assert(both.colourPreview && *both.colourPreview == pwv4Body);
        assert(both.scrollEntries && *both.scrollEntries == 4500);  // PWV7 first

        // A .2EX that is not an analysis file costs the three-band
        // preview, not the .EXT's fallback.
        source.files["USBANLZ/P001/00000001/ANLZ0000.2EX"] = "not an analysis file";
        const auto broken = rekordbox::readWaveformSections(source, "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT");
        assert(!broken.threeBandPreview);
        assert(broken.colourPreview && *broken.colourPreview == pwv4Body);
        assert(broken.scrollEntries && *broken.scrollEntries == 4400);  // PWV5 next

        // A PWV6 whose entries are not three bytes is one this code does
        // not understand: not there.
        source.files["USBANLZ/P001/00000001/ANLZ0000.2EX"] =
            anlzFile({section("PWV6", {4, 900}, std::string(3600, '\x05'))});
        assert(!rekordbox::readWaveformSections(source, "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT").threeBandPreview);
        std::cout << "case 4 (hand-built .2EX and .EXT: PWV6, PWV4, PWV7 before PWV5; a broken .2EX costs only itself) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
