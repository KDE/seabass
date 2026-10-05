// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The legacy memory list shapes of #33 and #55, as the evidence snapshots
// in project/evidence/issue33-shakedown8b-2026-09-30 hold them: the
// sections below are those files' memory PCOBs byte for byte, so the
// audit is checked against what a Seabass between 5282555e and 6e0f1c09
// and an XDJ-RX2 (firmware 1.43) really wrote, not against a reading of
// the spec.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::rekordbox;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;

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

// Whether a path the audit reports is this file. Compared as paths, not
// as strings: the audit joins the row's "/PIONEER/..." onto the stick's
// root as text, so on Windows it reports the stick's own backslashes
// followed by forward ones -- the same file as the test's t.dir / "...",
// spelled differently.
bool samePath(const std::string &reported, const fs::path &expected)
{
    return pathFromUtf8(reported).lexically_normal() == expected.lexically_normal();
}

std::string fromHex(const std::string &hex)
{
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// after-seabass/P025/0000297B/ANLZ0000.DAT: the writer of 5282555e, one
// entry (at 149 ms) under a header that says 0xFFFFFFFF, "empty".
const std::string StaleHeaderOneEntry = fromHex(
    "50434f4200000018000000500000000000000001ffffffff504350540000001c00000038000000000000000000010000ffffffff"
    "010003e800000095ffffffff00000000000000000000000000000000");

// after-rx2/P025/0000297B/ANLZ0000.DAT: the RX2's rewrite of that list.
// Our cue is gone, its own (at 16640 ms, status 1) is there, the header
// is right, and the section is sized for two with the second slot zero.
const std::string PlayerZeroSlot = fromHex(
    "50434f420000001800000088000000000000000100000000504350540000001c00000038000000000000000100000000ffffffff"
    "010003e8000041000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000");

// after-rx2-fixed/P04F/000158DF/ANLZ0000.DAT: three entries the RX2 wrote
// over a 6e0f1c09 list, linked 0-1-2, header 2. Healthy.
const std::string PlayerThreeLinked = fromHex(
    "50434f4200000018000000c0000000000000000300000002504350540000001c00000038000000000000000100000000ffff0001"
    "020003e80000c3440000d1b100000000000000000000000000000000504350540000001c00000038000000000000000100000000"
    "00000002010003e80000b4d80000000000000000000000000000000000000000504350540000001c0000003800000000000000000001"
    "00000001ffff010003e80000161effffffff00000000000000000000000000000000");

// after-rx2-smile/P06B/00012097/ANLZ0000.DAT: one memory loop the RX2
// saved on its own. Healthy.
const std::string PlayerOneLoop = fromHex(
    "50434f420000001800000050000000000000000100000000504350540000001c00000038000000000000000100000000ffffffff"
    "020003e8000075cc0000856c00000000000000000000000000000000");

// after-seabass-fixed/P04F/000158DF/ANLZ0000.DAT: 6e0f1c09's one-entry
// list. Healthy.
const std::string SeabassFixedOne = fromHex(
    "50434f420000001800000050000000000000000100000000504350540000001c00000038000000000000000000010000ffffffff"
    "010003e80000161effffffff00000000000000000000000000000000");

void putU32(std::string &s, size_t offset, uint32_t value)
{
    s[offset] = static_cast<char>((value >> 24) & 0xFF);
    s[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    s[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    s[offset + 3] = static_cast<char>(value & 0xFF);
}

void putU16(std::string &s, size_t offset, uint16_t value)
{
    s[offset] = static_cast<char>((value >> 8) & 0xFF);
    s[offset + 1] = static_cast<char>(value & 0xFF);
}

// What 5282555e wrote for two memory cues: the first two of the RX2's
// three entries, links 0xFFFF, header 0xFFFFFFFF. (Only one-entry lists
// of that writer were snapshotted; the two-entry shape is the ticket's
// description of the same code path, so it is built here.)
std::string staleTwoUnlinked()
{
    std::string s = PlayerThreeLinked.substr(0, 24 + 56 * 2);
    putU32(s, 8, static_cast<uint32_t>(s.size()));
    putU16(s, 18, 2);
    putU32(s, 20, 0xFFFFFFFFu);
    for (size_t entry : {size_t(0), size_t(1)}) {
        putU16(s, 24 + 56 * entry + 24, 0xFFFF);
        putU16(s, 24 + 56 * entry + 26, 0xFFFF);
    }
    return s;
}

void shapes()
{
    {
        const auto shape = auditLegacyMemoryList(StaleHeaderOneEntry);
        check(shape.malformed.empty(), "stale one-entry list is readable");
        check(shape.entries == 1, "stale one-entry list has one entry");
        check(shape.headerStale, "stale one-entry list: header says empty");
        check(!shape.unlinked, "one entry has nothing to link to");
        check(!shape.zeroSlots, "stale one-entry list has no zero slot");
        check(shape.repairable(), "stale one-entry list is repairable");
    }
    {
        const auto shape = auditLegacyMemoryList(PlayerZeroSlot);
        check(shape.malformed.empty(), "player's zero-slot list is readable");
        check(shape.entries == 1 && shape.zeroSlots, "player's zero-slot list: one entry and the slot");
        check(!shape.headerStale && !shape.unlinked, "player's zero-slot list has header and links right");
        check(shape.repairable(), "player's zero-slot list is repairable");
    }
    {
        const auto shape = auditLegacyMemoryList(staleTwoUnlinked());
        check(shape.entries == 2 && shape.headerStale && shape.unlinked && !shape.zeroSlots,
              "stale two-entry list: header and links both wrong");
    }
    for (const auto &[name, bytes] : {std::pair{"RX2 three linked", PlayerThreeLinked},
                                      std::pair{"RX2 one loop", PlayerOneLoop},
                                      std::pair{"6e0f1c09 one entry", SeabassFixedOne}}) {
        const auto shape = auditLegacyMemoryList(bytes);
        check(shape.malformed.empty() && !shape.damaged(), std::string(name) + " is healthy");
    }
    {
        const auto shape = auditLegacyMemoryList(AnlzLegacyCueCodec::encodeCues({}, 0));
        check(shape.malformed.empty() && !shape.damaged() && shape.entries == 0, "an empty list is healthy");
    }
    {
        // Each written shape must come out healthy, or the check would
        // flag what the writer just wrote.
        for (int n : {1, 2, 5}) {
            std::vector<LegacyCueEntry> cues;
            for (int i = 0; i < n; ++i) {
                LegacyCueEntry e;
                e.timeMs = static_cast<uint32_t>(1000 * (i + 1));
                cues.push_back(e);
            }
            const auto shape = auditLegacyMemoryList(AnlzLegacyCueCodec::encodeCues(cues, 0));
            check(shape.malformed.empty() && !shape.damaged() && shape.entries == n,
                  "the current writer's " + std::to_string(n) + "-entry list is healthy");
        }
    }
    {
        std::string trailingJunk = PlayerZeroSlot;
        trailingJunk[100] = 0x01;
        check(!auditLegacyMemoryList(trailingJunk).malformed.empty(), "nonzero bytes past the entries: malformed");
        std::string hotList = SeabassFixedOne;
        putU32(hotList, 12, 1);
        check(!auditLegacyMemoryList(hotList).malformed.empty(), "a hot list is not this check's");
        check(!auditLegacyMemoryList(SeabassFixedOne.substr(0, 60)).malformed.empty(), "a truncated list: malformed");
        std::string badEntry = SeabassFixedOne;
        badEntry[24] = 'X';
        check(!auditLegacyMemoryList(badEntry).malformed.empty(), "an entry that is not PCPT: malformed");
        check(!auditLegacyMemoryList("").malformed.empty(), "nothing: malformed");
    }
}

void repairs()
{
    {
        const std::string repaired = repairLegacyMemoryList(StaleHeaderOneEntry);
        AnlzLegacyCueCodec::checkSection(repaired);
        check(repaired.size() == 80, "stale one-entry list repaired is 80 bytes");
        check(readU32BE(repaired, 20) == 0, "stale one-entry list repaired: header names the last entry");
        check(repaired.substr(24) == StaleHeaderOneEntry.substr(24), "the entry is kept byte for byte");
        check(!auditLegacyMemoryList(repaired).damaged(), "stale one-entry list repaired is healthy");
    }
    {
        const std::string repaired = repairLegacyMemoryList(PlayerZeroSlot);
        AnlzLegacyCueCodec::checkSection(repaired);
        check(repaired.size() == 80, "zero slot dropped");
        check(readU32BE(repaired, 20) == 0, "player's list repaired: header right");
        check(repaired.substr(24, 56) == PlayerZeroSlot.substr(24, 56),
              "the player's own entry is kept byte for byte, its writer generation included");
        check(readU32BE(repaired, 24 + 32) == 16640, "and it is still the cue at 16640 ms");
        check(!auditLegacyMemoryList(repaired).damaged(), "player's list repaired is healthy");
    }
    {
        const std::string repaired = repairLegacyMemoryList(staleTwoUnlinked());
        AnlzLegacyCueCodec::checkSection(repaired);
        check(readU32BE(repaired, 20) == 1, "two entries: header names entry 1");
        check(readU16BE(repaired, 24 + 24) == 0xFFFF && readU16BE(repaired, 24 + 26) == 1, "entry 0 links to 1");
        check(readU16BE(repaired, 24 + 56 + 24) == 0 && readU16BE(repaired, 24 + 56 + 26) == 0xFFFF,
              "entry 1 links back to 0");
        // Everything but the links: the RX2's entries as they were.
        auto withoutLinks = [](std::string entry) {
            putU16(entry, 24, 0);
            putU16(entry, 26, 0);
            return entry;
        };
        for (size_t i = 0; i < 2; ++i) {
            check(withoutLinks(repaired.substr(24 + 56 * i, 56)) == withoutLinks(PlayerThreeLinked.substr(24 + 56 * i, 56)),
                  "entry " + std::to_string(i) + " kept apart from its links");
        }
        check(!auditLegacyMemoryList(repaired).damaged(), "two-entry list repaired is healthy");
    }
    for (const auto &[name, bytes] : {std::pair{"a healthy list", PlayerThreeLinked},
                                      std::pair{"a hot list", [] { std::string s = SeabassFixedOne; putU32(s, 12, 1); return s; }()}}) {
        bool threw = false;
        try {
            repairLegacyMemoryList(bytes);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw, std::string("repairing ") + name + " is refused");
    }
}

// A track directory on a scratch stick, from the fixture's own analysis
// files with the memory list swapped for one of the shapes above.
struct TrackDir
{
    fs::path pioneer;
    fs::path dir;
    std::string analyzePath;
};

TrackDir plant(const fs::path &stick, const fs::path &fixtureDat, const std::string &memoryList, const std::string &slot)
{
    TrackDir t;
    t.pioneer = stick / "PIONEER";
    t.dir = t.pioneer / "USBANLZ" / "P001" / slot;
    t.analyzePath = "/PIONEER/USBANLZ/P001/" + slot + "/ANLZ0000.DAT";
    fs::create_directories(t.dir);
    AnlzFile file = AnlzFile::readRaw(pathToUtf8(fixtureDat));
    for (auto &section : file.sections) {
        if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
            section.rawBytes = memoryList;
        }
    }
    file.writeRaw(pathToUtf8(t.dir / "ANLZ0000.DAT"));
    // The siblings a track has, so the audit must tell them from debris:
    // the fixture's own .EXT, its modern memory list made to hold what
    // the planted legacy one holds. A Seabass of that window wrote both
    // from the same cues, and the RX2 adds its own entry to both, so
    // the two generations agree here and only the memory list's shape
    // is wrong (the #60 disagreement has a test of its own).
    AnlzFile ext = AnlzFile::readRaw(pathToUtf8(fixtureDat.parent_path() / "ANLZ0000.EXT"));
    std::vector<RawHotCueEntry> modern;
    if (memoryList.size() >= 24 && memoryList.compare(0, 4, "PCOB") == 0) {
        const uint16_t count = readU16BE(memoryList, 18);
        for (uint16_t i = 0; i < count && 24 + 56 * (i + 1) <= memoryList.size(); ++i) {
            const size_t at = 24 + 56 * i;
            RawHotCueEntry entry;
            entry.timeMs = readU32BE(memoryList, at + 32);
            entry.isLoop = static_cast<unsigned char>(memoryList[at + 28]) == 2;
            entry.loopEndMs = entry.isLoop ? readU32BE(memoryList, at + 36) : 0;
            modern.push_back(entry);
        }
    }
    for (auto &section : ext.sections) {
        if (section.fourcc == 0x50434f32 && readU32BE(section.rawBytes, 12) == CueListTypeMemory) {
            section.rawBytes = AnlzCueCodec::encodeHotCues(modern, CueListTypeMemory);
        }
    }
    ext.writeRaw(pathToUtf8(t.dir / "ANLZ0000.EXT"));
    fs::copy_file(fixtureDat, t.dir / "ANLZ0000.2EX");
    return t;
}

// What the RX2 leaves: the track's PPTH and PVBR, then the empty beat
// grid and waveform sections exactly as after-rx2-freeze/P029/00007983/
// ANLZ0001.DAT holds them, and two empty cue lists.
void writeSkeleton(const fs::path &fixtureDat, const fs::path &to)
{
    AnlzFile file = AnlzFile::readRaw(pathToUtf8(fixtureDat));
    for (auto &section : file.sections) {
        switch (section.fourcc) {
        case 0x5051545A:
            section.rawBytes = fromHex("5051545a0000001800000018000000000008000000000000");
            break;
        case 0x50574156:
            section.rawBytes = fromHex("5057415600000014000000140000000000010000");
            break;
        case 0x50575632:
            section.rawBytes = fromHex("5057563200000014000000140000000000010000");
            break;
        case 0x50434f42:
            section.rawBytes = AnlzLegacyCueCodec::encodeCues({}, readU32BE(section.rawBytes, 12));
            break;
        default:
            break;
        }
    }
    file.writeRaw(pathToUtf8(to));
}

// The rows name this track's own file and nothing else, unless a test
// says otherwise.
std::function<bool(const std::string &)> namesOnly(const std::string &analyzePath)
{
    return [analyzePath](const std::string &p) { return p == analyzePath; };
}

std::string memoryListOf(const fs::path &dat)
{
    const AnlzFile file = AnlzFile::readRaw(pathToUtf8(dat));
    for (const auto &section : file.sections) {
        if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
            return section.rawBytes;
        }
    }
    return {};
}

void files(const fs::path &fixtureDat)
{
    const fs::path stick = seabass::testing::scratchRoot() / "seabass_legacy_memory_list_audit_test";
    fs::remove_all(stick);

    {
        // Healthy, and alone in its directory: nothing to report.
        const TrackDir t = plant(stick, fixtureDat, PlayerThreeLinked, "00000001");
        check(!auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath)).has_value(), "a healthy track has no finding");
    }
    {
        // The frozen track: our stale list, and the RX2's skeleton beside it.
        const TrackDir t = plant(stick, fixtureDat, StaleHeaderOneEntry, "00000002");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0001.DAT");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding.has_value(), "the frozen track is found");
        if (!finding) {
            return;
        }
        check(samePath(finding->datPath, t.dir / "ANLZ0000.DAT"), "the finding names the .DAT");
        check(finding->shape.headerStale && finding->shape.repairable(), "its list is the stale one");
        check(finding->debris.size() == 1 && samePath(finding->debris[0], t.dir / "ANLZ0001.DAT"),
              "and ANLZ0001.DAT is the debris, the .EXT and .2EX are not");

        const std::string account = repairTrackAnalysis(*finding);
        check(account.find("rebuilt") != std::string::npos && account.find("removed") != std::string::npos,
              "the account says both: " + account);
        check(!fs::exists(t.dir / "ANLZ0001.DAT"), "the debris is gone");
        check(fs::exists(t.dir / "ANLZ0000.EXT") && fs::exists(t.dir / "ANLZ0000.2EX"), "the siblings are not");
        const std::string after = memoryListOf(t.dir / "ANLZ0000.DAT");
        check(after == repairLegacyMemoryList(StaleHeaderOneEntry), "the .DAT holds the repaired list");
        check(!auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath)).has_value(), "and audits clean afterwards");
        // The file as a whole is still one AnlzFile::readRaw accepts: the
        // write went through its validation and len_file is right.
        AnlzFile::readRaw(pathToUtf8(t.dir / "ANLZ0000.DAT"));
    }
    {
        // The player's rewrite, no debris: repaired alone.
        const TrackDir t = plant(stick, fixtureDat, PlayerZeroSlot, "00000003");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding && finding->shape.zeroSlots && finding->debris.empty(), "the zero-slot track is found");
        if (finding) {
            const std::string account = repairTrackAnalysis(*finding);
            check(account.find("removed") == std::string::npos, "nothing removed: " + account);
            check(memoryListOf(t.dir / "ANLZ0000.DAT") == repairLegacyMemoryList(PlayerZeroSlot), "the list is repaired");
        }
    }
    {
        // Debris beside a healthy list: reported and removed, the list
        // untouched.
        const TrackDir t = plant(stick, fixtureDat, SeabassFixedOne, "00000004");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0001.DAT");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0002.DAT");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding && !finding->shape.damaged() && finding->debris.size() == 2, "two debris files beside a healthy list");
        if (finding) {
            const auto before = fs::last_write_time(t.dir / "ANLZ0000.DAT");
            repairTrackAnalysis(*finding);
            check(fs::last_write_time(t.dir / "ANLZ0000.DAT") == before, "a healthy .DAT is not rewritten");
            check(!fs::exists(t.dir / "ANLZ0001.DAT") && !fs::exists(t.dir / "ANLZ0002.DAT"), "both removed");
        }
    }
    {
        // Not debris: rekordbox's own second analysis in the directory
        // (a full set, named from another track's row), a full set no
        // row names, and a real analysis without its .EXT. Only the
        // unnamed skeleton without an .EXT is.
        const TrackDir t = plant(stick, fixtureDat, SeabassFixedOne, "00000008");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0001.DAT");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0001.EXT");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0002.DAT");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0002.EXT");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0003.DAT");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0004.DAT");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0005.DAT");
        fs::copy_file(fixtureDat, t.dir / "ANLZ0005.EXT");
        const auto named = [&t](const std::string &p) {
            return p == t.analyzePath || p == "/PIONEER/USBANLZ/P001/00000008/ANLZ0001.DAT";
        };
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, named);
        check(finding.has_value(), "the one skeleton is found");
        if (finding) {
            check(finding->debris.size() == 1 && samePath(finding->debris[0], t.dir / "ANLZ0004.DAT"),
                  "only the unnamed skeleton without an .EXT is debris");
        }
        // And a named skeleton is not: it is some row's analysis, however
        // empty.
        auto namedSkeleton = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, [&t](const std::string &p) {
            return p == t.analyzePath || p.ends_with("ANLZ0004.DAT") || p == "/PIONEER/USBANLZ/P001/00000008/ANLZ0001.DAT";
        });
        check(!namedSkeleton.has_value(), "a skeleton some row names is left alone");
    }
    {
        // A list this cannot read: named, not repaired, the file untouched.
        std::string junk = PlayerZeroSlot;
        junk[100] = 0x7f;
        const TrackDir t = plant(stick, fixtureDat, junk, "00000005");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding && !finding->shape.malformed.empty() && !finding->shape.repairable(), "a malformed list is reported");
        if (finding) {
            const std::string account = repairTrackAnalysis(*finding);
            check(account.empty(), "and nothing is done about it: " + account);
            check(memoryListOf(t.dir / "ANLZ0000.DAT") == junk, "the file is as it was");
        }
    }
    {
        // A save in between (a sync rewrote the list) leaves nothing to
        // do, and that is not an error.
        const TrackDir t = plant(stick, fixtureDat, StaleHeaderOneEntry, "00000006");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding.has_value(), "found before the sync");
        fs::remove_all(t.dir);
        plant(stick, fixtureDat, SeabassFixedOne, "00000006");
        if (finding) {
            const std::string account = repairTrackAnalysis(*finding);
            check(account.find("already in shape") != std::string::npos, "a list fixed since is left alone: " + account);
        }
    }
    {
        // No .DAT at all (a track whose analysis is gone) but debris: the
        // debris is still reported.
        TrackDir t;
        t.pioneer = stick / "PIONEER";
        t.dir = t.pioneer / "USBANLZ" / "P001" / "00000007";
        t.analyzePath = "/PIONEER/USBANLZ/P001/00000007/ANLZ0000.DAT";
        fs::create_directories(t.dir);
        writeSkeleton(fixtureDat, t.dir / "ANLZ0001.DAT");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding && finding->datPath.empty() && finding->debris.size() == 1, "debris without a .DAT is reported");
        check(!auditTrackAnalysis(pathToUtf8(t.pioneer), "/PIONEER/USBANLZ/P001/00000099/ANLZ0000.DAT", namesOnly("")).has_value(),
              "a directory that does not exist is nothing");
    }
    {
        // export.pdb pads its strings with spaces. A row's analyze_path
        // that arrives padded names the same files: examined, its own
        // .DAT not taken for a stranger, and the debris rule's directory
        // cut from the trimmed path.
        const TrackDir t = plant(stick, fixtureDat, SeabassFixedOne, "00000009");
        writeSkeleton(fixtureDat, t.dir / "ANLZ0001.DAT");
        const std::string padded = t.analyzePath + "   ";
        const auto finding = examineTrackAnalysis(pathToUtf8(t.pioneer), padded, namesOnly(t.analyzePath));
        check(finding.examined && finding.unreadable.empty(),
              "a padded analyze_path is examined: " + finding.unreadable);
        check(finding.debris.size() == 1 && samePath(finding.debris[0], t.dir / "ANLZ0001.DAT"),
              "and only the skeleton is debris");
    }
    {
        // The in-place repair is read back with the writer's own check
        // (analysisFileReadBackProblem), the hook a test damages a file
        // through included: a file that does not read back as written is
        // put back as it was, and the repair fails.
        const TrackDir t = plant(stick, fixtureDat, StaleHeaderOneEntry, "0000000A");
        auto finding = auditTrackAnalysis(pathToUtf8(t.pioneer), t.analyzePath, namesOnly(t.analyzePath));
        check(finding.has_value(), "the stale list is found");
        if (finding) {
            RekordboxCueWriter::setAfterWriteForTesting([](const std::string &path) {
                std::ofstream(pathFromUtf8(path), std::ios::binary | std::ios::app) << '\0';
            });
            bool threw = false;
            try {
                repairTrackAnalysis(*finding);
            } catch (const std::exception &e) {
                threw = std::string(e.what()).find("failed its check after writing") != std::string::npos;
            }
            RekordboxCueWriter::setAfterWriteForTesting({});
            check(threw, "a repair that does not read back fails");
            check(memoryListOf(t.dir / "ANLZ0000.DAT") == StaleHeaderOneEntry, "and the file is put back as it was");
        }
    }
    fs::remove_all(stick);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: legacy_memory_list_audit_test <anonymized_library dir>\n";
        return 2;
    }
    const fs::path fixtureDat = pathFromUtf8(argv[1]) / "rekordbox" / "USBANLZ" / "P017" / "0002435D" / "ANLZ0000.DAT";
    if (!fs::exists(fixtureDat)) {
        std::cerr << "fixture missing: " << fixtureDat << "\n";
        return 2;
    }
    shapes();
    repairs();
    files(fixtureDat);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "legacy_memory_list_audit_test: all cases passed\n";
    return 0;
}
