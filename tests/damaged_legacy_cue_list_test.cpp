// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// One analysis file whose legacy cue list does not parse must not take
// the whole rekordbox library with it. A real stick (WHALESHARK,
// 2026-09-26) had two .DAT files whose memory PCOB claimed one entry that
// did not start with PCPT; kaitai threw on the first of them, the reader
// let it through, and Seabass could not open the stick at all. The
// modern PCO2 list of that track, and every other track, were fine.
//
// Reproduced here on a copy of the fixture: the same shape of damage is
// planted in one .DAT whose .EXT carries cues, and the library must read
// as before -- same tracks, same cues -- since a legacy list only ever
// adds what PCO2 does not already hold.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;

namespace
{

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t be32(const std::string &s, size_t at)
{
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8)
        | uint32_t(uint8_t(s[at + 3]));
}

void putBe32(std::string &s, size_t at, uint32_t v)
{
    s[at] = char(v >> 24);
    s[at + 1] = char(v >> 16);
    s[at + 2] = char(v >> 8);
    s[at + 3] = char(v);
}

// Whether an ANLZ file holds a cue list of the given fourcc with at
// least one entry.
bool hasCues(const std::string &bytes, const std::string &fourcc)
{
    if (bytes.size() < 12 || bytes.compare(0, 4, "PMAI") != 0) {
        return false;
    }
    size_t off = be32(bytes, 4);
    while (off + 12 <= bytes.size()) {
        const uint32_t len = be32(bytes, off + 8);
        if (len < 12) {
            return false;
        }
        if (bytes.compare(off, 4, fourcc) == 0 && off + 18 <= bytes.size()) {
            const uint32_t num = (uint32_t(uint8_t(bytes[off + 16])) << 8) | uint8_t(bytes[off + 17]);
            if (num > 0) {
                return true;
            }
        }
        off += len;
    }
    return false;
}

// Plants the real damage: the last PCOB section claims one entry and is
// grown by 56 bytes that are not a PCPT entry (the bytes as found on
// the stick, a PCPT entry's tail from order_first on, preceded by
// 00 00 ff ff).
void damageLastLegacyList(const fs::path &dat)
{
    std::string bytes = readFile(dat);
    assert(bytes.compare(0, 4, "PMAI") == 0);
    size_t off = be32(bytes, 4);
    size_t last = std::string::npos;
    while (off + 12 <= bytes.size()) {
        const uint32_t len = be32(bytes, off + 8);
        assert(len >= 12);
        if (bytes.compare(off, 4, "PCOB") == 0) {
            last = off;
        }
        off += len;
    }
    assert(last != std::string::npos && "the fixture's .DAT has a PCOB to damage");
    const uint32_t len = be32(bytes, last + 8);
    assert(last + len == bytes.size() && "the last PCOB ends the file, as in every real .DAT seen");
    static const unsigned char junk[56] = {0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x03, 0xe8,
                                           0x00, 0x01, 0x45, 0x4d};
    bytes.append(reinterpret_cast<const char *>(junk), sizeof(junk));
    putBe32(bytes, last + 8, len + 56);
    // num_cues sits at +18 in a PCOB (after type and a 2-byte field),
    // not at +16 as in a PCO2: written at +16 the file parses as an empty
    // list and the damage is not there to detect.
    bytes[last + 18] = 0;
    bytes[last + 19] = 1;
    putBe32(bytes, 8, static_cast<uint32_t>(bytes.size()));  // len_file
    std::ofstream out(dat, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

size_t totalCues(const std::vector<seabass::domain::Track> &tracks)
{
    size_t n = 0;
    for (const auto &t : tracks) {
        n += t.cues.size();
    }
    return n;
}

}  // namespace

int main()
{
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    const fs::path scratch = seabass::testing::scratchRoot() / "damaged-legacy-cue-list";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    fs::copy(source, scratch, fs::copy_options::recursive);

    seabass::infrastructure::rekordbox::KaitaiRekordboxReader before(seabass::pathToUtf8(scratch));
    const auto tracksBefore = before.readAll();
    assert(!tracksBefore.empty());
    const size_t cuesBefore = totalCues(tracksBefore);
    assert(cuesBefore > 0);

    // The .DAT of a track the catalog references and that has cues: the
    // one the reader parses for that track, whose cues would be lost if
    // the damage were allowed to take them. Chosen through the catalog,
    // not by walking USBANLZ: an analysis file no row points at is never
    // read, and damaging one of those proves nothing.
    fs::path victim;
    for (const auto &track : tracksBefore) {
        if (track.cues.empty() || track.sourceId.empty()) {
            continue;
        }
        const auto analyzePath = seabass::infrastructure::rekordbox::findAnlzPathForTrackId(
            seabass::pathToUtf8(scratch), static_cast<uint32_t>(std::stoul(track.sourceId)));
        if (!analyzePath) {
            continue;
        }
        const fs::path dat = seabass::pathFromUtf8(
            seabass::infrastructure::rekordbox::datAnlzPath(seabass::pathToUtf8(scratch), *analyzePath));
        fs::path ext = dat;
        ext.replace_extension(".EXT");
        if (fs::exists(dat) && fs::exists(ext) && hasCues(readFile(ext), "PCO2")) {
            victim = dat;
            break;
        }
    }
    assert(!victim.empty() && "the fixture has a referenced track with cues in its .EXT and a .DAT");
    damageLastLegacyList(victim);
    std::cout << "damaged " << seabass::pathToUtf8(fs::relative(victim, scratch)) << "\n";

    // The library reads as before: every track, every cue -- the damaged
    // list was a legacy one, and PCO2 holds the cues that matter.
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader after(seabass::pathToUtf8(scratch));
    const auto tracksAfter = after.readAll();
    assert(tracksAfter.size() == tracksBefore.size() && "one unreadable analysis file must not cost the library");
    assert(totalCues(tracksAfter) == cuesBefore && "the modern list's cues survive a damaged legacy list");

    fs::remove_all(scratch, ec);
    std::cout << "damaged_legacy_cue_list_test: " << tracksAfter.size() << " tracks, " << cuesBefore
              << " cues, all read past the damage\n";
    return 0;
}
