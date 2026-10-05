// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Library Health's cue list check (#55, #60) on synthetic analysis files:
// one .DAT/.EXT pair per shape the check names, built from the layouts
// the codecs' surveys establish (anlz_legacy_cue_codec.hpp,
// anlz_cue_codec.hpp), and one clean pair. No real track's file is in
// here; the cue positions are the ones the #33 and #60 reports quote.
//
//   a  the legacy memory list a Seabass between 5282555e and 6e0f1c09
//      wrote: header 0xFFFFFFFF, entries unlinked
//   b  the XDJ-RX2's rewrite of such a list: sized for one more entry
//      than it holds, the extra slot zero
//   c  legacy and modern lists that disagree: WHALESHARK2's Too Little
//      Too Late, one pad on the player and five in Seabass
//   d  the skeleton ANLZ0001.DAT a hung RX2 leaves beside the track
//
// Then the counts, and every repair: both generations equal afterwards,
// each list decodable by its codec, RekordboxCueWriter finding nothing
// to change and its read-back accepting the files, and a clean pair not
// touched at all.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include "domain/cue_list_count.hpp"
#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
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

// Whether a path the check reports is this file, compared as paths: the
// audit joins the row's "/PIONEER/..." onto the stick's root as text, so
// on Windows its separators are mixed where the test's are not.
bool samePath(const std::string &reported, const fs::path &expected)
{
    return pathFromUtf8(reported).lexically_normal() == expected.lexically_normal();
}

constexpr uint32_t Pcob = 0x50434f42;
constexpr uint32_t Pco2 = 0x50434f32;

std::string u32(uint32_t v)
{
    std::string s;
    appendU32BE(s, v);
    return s;
}

void putU32(std::string &s, size_t at, uint32_t v)
{
    writeU32BE(s, at, v);
}

void putU16(std::string &s, size_t at, uint16_t v)
{
    s[at] = static_cast<char>(v >> 8);
    s[at + 1] = static_cast<char>(v & 0xFF);
}

// The 28-byte PMAI header every surveyed file opens with.
std::string pmaiHeader()
{
    return "PMAI" + u32(28) + u32(0) + u32(1) + u32(0x00010000) + u32(0x00010000) + u32(0);
}

// The file's path section, as rekordbox writes it: UTF-16BE, NUL ended.
std::string ppth(const std::string &path)
{
    std::string body;
    for (char c : path + '\0') {
        body.push_back('\0');
        body.push_back(c);
    }
    return "PPTH" + u32(16) + u32(static_cast<uint32_t>(16 + body.size())) + u32(static_cast<uint32_t>(body.size()))
           + body;
}

// A beat grid with one beat, so the file is plainly an analysis and not a
// skeleton.
std::string pqtz()
{
    return "PQTZ" + u32(24) + u32(32) + u32(0) + u32(0x00080000) + u32(1) + std::string("\x00\x01\x30\x39", 4)
           + u32(0);
}

LegacyCueEntry legacy(uint32_t pad, uint32_t timeMs)
{
    LegacyCueEntry e;
    e.hotCueNumber = pad;
    e.timeMs = timeMs;
    return e;
}

RawHotCueEntry modern(uint32_t pad, uint32_t timeMs,
                      std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> color = std::nullopt)
{
    RawHotCueEntry e;
    e.hotCueNumber = pad;
    e.timeMs = timeMs;
    e.color = color;
    return e;
}

// What a track's pair holds: the legacy hot list split as the survey
// found it (1-3 in the .DAT, 4-8 in the .EXT), the .DAT's memory list as
// raw section bytes so a test can damage it, and the PCO2 lists.
struct Pair
{
    std::vector<LegacyCueEntry> legacyHot;
    std::string datMemoryList;  // a whole PCOB section
    std::vector<RawHotCueEntry> modernHot;
    std::vector<RawHotCueEntry> modernMemory;
};

std::string memoryList(const std::vector<uint32_t> &times)
{
    std::vector<LegacyCueEntry> entries;
    for (uint32_t t : times) {
        entries.push_back(legacy(0, t));
    }
    return AnlzLegacyCueCodec::encodeCues(entries, CueListTypeMemory);
}

struct Track
{
    fs::path pioneer;
    fs::path dir;
    std::string analyzePath;
    fs::path dat() const { return dir / "ANLZ0000.DAT"; }
    fs::path ext() const { return dir / "ANLZ0000.EXT"; }
};

void writeAnlz(const fs::path &to, std::vector<std::string> sections)
{
    AnlzFile file;
    file.headerBytes = pmaiHeader();
    for (auto &bytes : sections) {
        file.sections.push_back({readU32BE(bytes, 0), std::move(bytes)});
    }
    file.writeRaw(pathToUtf8(to));
}

Track plant(const fs::path &pioneer, const std::string &slot, const Pair &pair)
{
    Track t;
    t.pioneer = pioneer;
    t.dir = pioneer / "USBANLZ" / "P001" / slot;
    t.analyzePath = "/PIONEER/USBANLZ/P001/" + slot + "/ANLZ0000.DAT";
    fs::create_directories(t.dir);
    std::vector<LegacyCueEntry> low;
    std::vector<LegacyCueEntry> high;
    for (const auto &e : pair.legacyHot) {
        (e.hotCueNumber <= 3 ? low : high).push_back(e);
    }
    const std::string path = "/Contents/" + slot + ".mp3";
    writeAnlz(t.dat(), {ppth(path), pqtz(), AnlzLegacyCueCodec::encodeCues(low, CueListTypeHot),
                        pair.datMemoryList.empty() ? memoryList({}) : pair.datMemoryList});
    writeAnlz(t.ext(), {ppth(path), AnlzLegacyCueCodec::encodeCues(high, CueListTypeHot), memoryList({}),
                        AnlzCueCodec::encodeHotCues(pair.modernHot, CueListTypeHot),
                        AnlzCueCodec::encodeHotCues(pair.modernMemory, CueListTypeMemory)});
    return t;
}

// The RX2's skeleton: its path, empty grid and waveforms, empty lists.
void writeSkeleton(const fs::path &to)
{
    std::string grid = "PQTZ" + u32(24) + u32(24) + u32(0) + u32(0x00080000) + u32(0);
    std::string wave = "PWAV" + u32(20) + u32(20) + u32(0) + u32(0x00010000);
    std::string wave2 = "PWV2" + u32(20) + u32(20) + u32(0) + u32(0x00010000);
    writeAnlz(to, {ppth("/Contents/skeleton.mp3"), grid, wave, wave2, AnlzLegacyCueCodec::encodeCues({}, CueListTypeHot),
                   memoryList({})});
}

std::string bytesOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Shape a: 5282555e's two-entry memory list, header "empty", unlinked.
std::string staleTwoUnlinked(uint32_t first, uint32_t second)
{
    std::string s = memoryList({first, second});
    putU32(s, 20, 0xFFFFFFFFu);
    for (size_t i = 0; i < 2; ++i) {
        putU16(s, 24 + 56 * i + 24, 0xFFFF);
        putU16(s, 24 + 56 * i + 26, 0xFFFF);
    }
    return s;
}

// Shape b: the RX2's own entry (status 1, the player's writer
// generation) in a list sized for two, the second slot zero.
std::string playerZeroSlot(uint32_t timeMs)
{
    std::string s = memoryList({timeMs});
    putU32(s, 24 + 16, 1);  // status 1, unknown1 0, loop_time 0: the
    putU32(s, 24 + 20, 0);  // older generation the survey found,
    putU32(s, 24 + 36, 0);  // which is what the RX2 writes
    s += std::string(56, '\0');
    putU32(s, 8, static_cast<uint32_t>(s.size()));
    return s;
}

// Every list in the pair decodes with its own codec, strictly.
bool decodesStrictly(const Track &t, std::string &why)
{
    try {
        for (const fs::path &file : {t.dat(), t.ext()}) {
            for (const auto &section : AnlzFile::readRaw(pathToUtf8(file)).sections) {
                if (section.fourcc == Pcob) {
                    AnlzLegacyCueCodec::decodeCues(section.rawBytes);
                } else if (section.fourcc == Pco2) {
                    AnlzCueCodec::decodeHotCues(section.rawBytes, readU32BE(section.rawBytes, 12));
                }
            }
        }
        return true;
    } catch (const std::exception &e) {
        why = e.what();
        return false;
    }
}

std::vector<seabass::domain::CuePoint> cuesOf(const std::vector<ListedCue> &hot, const std::vector<ListedCue> &memory)
{
    std::vector<seabass::domain::CuePoint> cues;
    for (const auto *list : {&hot, &memory}) {
        for (const auto &c : *list) {
            seabass::domain::CuePoint cue;
            cue.kind = list == &hot ? seabass::domain::CuePoint::Kind::Hot : seabass::domain::CuePoint::Kind::Memory;
            cue.hotCueNumber = static_cast<int>(c.pad);
            cue.positionMs = c.timeMs;
            cue.isLoop = c.isLoop;
            cue.loopEndMs = c.loopEndMs;
            cues.push_back(cue);
        }
    }
    return cues;
}

// After a repair: both generations agree, each list decodes strictly,
// and RekordboxCueWriter, handed the cues the files now hold, has
// nothing to change; then, made to write them anyway, its own read-back
// accepts both files.
void checkRepaired(const Track &t, const std::string &name)
{
    std::string why;
    check(decodesStrictly(t, why), name + ": every list decodes strictly after the repair (" + why + ")");
    TrackCueLists lists;
    try {
        lists = readTrackCueLists(pathToUtf8(t.pioneer), t.analyzePath);
    } catch (const std::exception &e) {
        check(false, name + ": the lists read after the repair: " + e.what());
        return;
    }
    check(!compareCueLists(lists).has_value(), name + ": the two generations agree after the repair");
    RekordboxCueWriter writer(pathToUtf8(t.pioneer));
    try {
        const bool changed = writer.writeCuesToAnalysisFile(t.analyzePath, cuesOf(lists.modernHot, lists.modernMemory),
                                                            RekordboxCueWriter::Rewrite::OnlyIfChanged);
        check(!changed, name + ": the writer has nothing to change in the repaired files");
        writer.writeCuesToAnalysisFile(t.analyzePath, cuesOf(lists.modernHot, lists.modernMemory),
                                       RekordboxCueWriter::Rewrite::Always);
    } catch (const std::exception &e) {
        check(false, name + ": the writer's read-back accepts the repaired files: " + e.what());
    }
}

constexpr size_t FixtureExamined = 1161;
constexpr size_t FixtureDisagree = 203;

const auto Red = std::make_tuple(uint8_t(0xFF), uint8_t(0x00), uint8_t(0x00));
const auto Blue = std::make_tuple(uint8_t(0x00), uint8_t(0x00), uint8_t(0xFF));

// The #60 shape: the player shows pad A at 0:30.765 and B at 1:07.751;
// Seabass wrote five pads, A half a second earlier (a different cue) and
// B a millisecond later (the same cue, coloured red).
Pair disagreeingPair()
{
    Pair p;
    p.legacyHot = {legacy(2, 67751), legacy(1, 30765)};
    p.modernHot = {modern(1, 30251, Blue), modern(2, 67752, Red), modern(3, 75251), modern(4, 120251),
                   modern(5, 135251)};
    return p;
}

// Agreeing lists, pad B a millisecond apart in the two as rekordbox
// itself leaves them: the same cue, nothing to repair, nothing to write.
Pair cleanPair()
{
    Pair p;
    p.legacyHot = {legacy(5, 90000), legacy(4, 60000), legacy(2, 30000), legacy(1, 453)};
    p.datMemoryList = memoryList({5662, 240027});
    p.modernHot = {modern(1, 453, Red), modern(2, 30001), modern(4, 60000), modern(5, 90000, Blue)};
    p.modernMemory = {modern(0, 5662), modern(0, 240027)};
    return p;
}

// The same to the millisecond, for the repairs after which the writer
// must find nothing to change: it writes both generations from one
// value, so it would even out the clean pair's millisecond.
Pair exactPair()
{
    Pair p = cleanPair();
    p.modernHot[1].timeMs = 30000;
    return p;
}

void findingsAndRepairs(const fs::path &stick)
{
    const fs::path pioneer = stick / "PIONEER";

    const Track clean = plant(pioneer, "00000001", cleanPair());

    Pair a = exactPair();
    a.datMemoryList = staleTwoUnlinked(5662, 240027);
    const Track stale = plant(pioneer, "00000002", a);

    Pair b = exactPair();
    b.datMemoryList = playerZeroSlot(16640);
    b.modernMemory = {modern(0, 16640)};
    const Track zeroSlot = plant(pioneer, "00000003", b);

    const Track disagree = plant(pioneer, "00000004", disagreeingPair());

    const Track debris = plant(pioneer, "00000005", cleanPair());
    writeSkeleton(debris.dir / "ANLZ0001.DAT");

    const std::string root = pathToUtf8(pioneer);
    const auto names = [&](const std::string &p) {
        for (const Track *t : {&clean, &stale, &zeroSlot, &disagree, &debris}) {
            if (p == t->analyzePath) {
                return true;
            }
        }
        return false;
    };

    // Each shape is named, and only its own kind; the clean pair is not.
    {
        const auto f = examineTrackAnalysis(root, clean.analyzePath, names);
        check(f.examined, "clean: examined");
        check(!f.anything(), "clean: nothing to report");
    }
    {
        const auto f = examineTrackAnalysis(root, stale.analyzePath, names);
        check(f.examined && f.shape.headerStale && f.shape.unlinked && f.shape.repairable(),
              "a: the stale header and the unlinked entries are found");
        check(!f.disagreement && f.debris.empty(), "a: nothing else, its lists agree");
    }
    {
        const auto f = examineTrackAnalysis(root, zeroSlot.analyzePath, names);
        check(f.examined && f.shape.zeroSlots && f.shape.entries == 1 && f.shape.repairable(),
              "b: the player's zero slot is found, one real entry");
        check(!f.disagreement, "b: read leniently, its one entry agrees with PCO2's");
    }
    {
        const auto f = examineTrackAnalysis(root, disagree.analyzePath, names);
        check(f.examined && f.disagreement && f.disagreement->hot && !f.disagreement->memory,
              "c: the hot lists disagree and the memory lists do not");
        if (f.disagreement) {
            check(f.disagreement->lists.legacyHot.size() == 2 && f.disagreement->lists.modernHot.size() == 5,
                  "c: both lists are kept for the report, 2 pads and 5");
        }
        check(!f.shape.damaged() && f.debris.empty(), "c: nothing else");
        check(f.listsFixable(), "c: repairable");
    }
    {
        const auto f = examineTrackAnalysis(root, debris.analyzePath, names);
        check(f.debris.size() == 1 && samePath(f.debris[0], debris.dir / "ANLZ0001.DAT"), "d: the skeleton is found");
        check(!f.shape.damaged() && !f.disagreement, "d: nothing else");
    }

    // The counts. Six paths given, one of them twice and one missing.
    {
        const std::string missing = "/PIONEER/USBANLZ/P001/0000FFFF/ANLZ0000.DAT";
        const std::vector<std::string> paths{clean.analyzePath, stale.analyzePath, zeroSlot.analyzePath,
                                             disagree.analyzePath, debris.analyzePath, stale.analyzePath, missing};
        size_t calls = 0;
        const CueListScan scan = scanCueLists(root, paths, names, [&calls] { ++calls; });
        check(calls == paths.size(), "each path given is reported to the progress callback");
        check(scan.tally.examined == 5, "five files examined, the one named twice once: "
                                            + std::to_string(scan.tally.examined));
        check(scan.tally.unreadable == 1, "the missing one is counted as unreadable: "
                                              + std::to_string(scan.tally.unreadable));
        check(scan.tally.legacyHeader == 1, "one legacy memory list header: " + std::to_string(scan.tally.legacyHeader));
        check(scan.tally.playerRewritten == 1,
              "one player-rewritten memory list: " + std::to_string(scan.tally.playerRewritten));
        check(scan.tally.disagree == 1, "one pair of lists that disagree: " + std::to_string(scan.tally.disagree));
        check(scan.tally.strayFiles == 1, "one stray analysis file: " + std::to_string(scan.tally.strayFiles));
        // Four findings and the unreadable one, which is listed so a page
        // can name it.
        check(scan.findings.size() == 5, "four findings and the missing file, the clean pair not among them: "
                                             + std::to_string(scan.findings.size()));
        size_t unreadableListed = 0;
        for (const auto &f : scan.findings) {
            check(f.analyzePath != clean.analyzePath, "the clean pair is not a finding");
            unreadableListed += f.unreadable.empty() ? 0 : 1;
        }
        check(unreadableListed == 1, "the missing file is listed with why");
    }
    // The same files read the way the readers' cue pass reads them
    // (readAnalysisFileCues): each row's cue lists are compared in the
    // bytes read for its cues, with no second pass, and the stick summary
    // counts what the Cue lists check counts (#60).
    {
        const std::string missing = "/PIONEER/USBANLZ/P001/0000FFFF/ANLZ0000.DAT";
        FilesystemAnlzSource source(root);
        std::vector<seabass::domain::Track> rows;
        for (const std::string &path : {clean.analyzePath, stale.analyzePath, zeroSlot.analyzePath,
                                        disagree.analyzePath, debris.analyzePath, stale.analyzePath, missing}) {
            seabass::domain::Track row;
            row.analysisFile = path;
            try {
                readAnalysisFileCues(source, path, &row.cueLists);
            } catch (const std::exception &) {
                // A file the cue reader cannot parse is that track's
                // problem; its lists were looked at first.
            }
            rows.push_back(row);
        }
        using Check = seabass::domain::Track::CueListsCheck;
        check(rows[3].cueLists == Check::Disagree, "the disagreeing pair reads as Disagree");
        check(rows[0].cueLists == Check::Examined && rows[1].cueLists == Check::Examined
                  && rows[2].cueLists == Check::Examined && rows[4].cueLists == Check::Examined,
              "the others as Examined, a damaged memory list included");
        check(rows[6].cueLists == Check::Unreadable, "the missing file as Unreadable");
        const seabass::domain::CueListCount count = seabass::domain::countCueLists(rows);
        check(count.files == 6 && count.examined == 5 && count.disagree == 1 && count.unreadable == 1,
              "the summary counts each file once, as the scan does: " + std::to_string(count.files) + " files, "
                  + std::to_string(count.examined) + " examined, " + std::to_string(count.disagree) + " disagree, "
                  + std::to_string(count.unreadable) + " unreadable");
        check(count.examined == static_cast<int>(scanCueLists(root, {clean.analyzePath, stale.analyzePath,
                                                                     zeroSlot.analyzePath, disagree.analyzePath,
                                                                     debris.analyzePath, stale.analyzePath, missing},
                                                              names).tally.examined),
              "and examines exactly the files the Cue lists check examines");
        check(seabass::domain::describeCueListCount(count) == "1 analysis file could not be read",
              "with a file unread, the summary says so and gives no count of disagreements: "
                  + seabass::domain::describeCueListCount(count));
        rows.pop_back();
        check(seabass::domain::describeCueListCount(seabass::domain::countCueLists(rows)) == "1 cue list disagrees",
              "every file examined: the count of disagreements");
        rows.erase(rows.begin() + 3);
        check(seabass::domain::describeCueListCount(seabass::domain::countCueLists(rows)).empty()
                  && seabass::domain::describeCueListCount(seabass::domain::countCueLists(rows), true)
                         == "Cue lists agree in all 4 analysis files",
              "all agreeing: nothing on the card, the statistics page says so");
        // A memory list the check cannot read (bytes past its last entry
        // that are not zero) is not a list that agrees: Unreadable, as
        // the check reports it malformed.
        Pair junk = exactPair();
        junk.datMemoryList = playerZeroSlot(16640);
        junk.datMemoryList.back() = 0x7f;
        const Track malformed = plant(pioneer, "00000020", junk);
        seabass::domain::Track junkRow;
        junkRow.analysisFile = malformed.analyzePath;
        try {
            readAnalysisFileCues(source, malformed.analyzePath, &junkRow.cueLists);
        } catch (const std::exception &) {
        }
        check(!examineTrackAnalysis(root, malformed.analyzePath, names).shape.malformed.empty(),
              "the check calls that memory list malformed");
        check(junkRow.cueLists == Check::Unreadable, "and the summary does not count it as agreeing");
        fs::remove_all(malformed.dir);
        seabass::domain::Track notRead;
        notRead.analysisFile = clean.analyzePath;
        check(seabass::domain::describeCueListCount(seabass::domain::countCueLists({notRead})).empty(),
              "cues not read yet: nothing said");
    }

    // Nothing there: nothing examined, and never a clean bill.
    {
        const CueListScan none = scanCueLists(pathToUtf8(stick / "NOT-MOUNTED" / "PIONEER"),
                                              {clean.analyzePath, stale.analyzePath}, names);
        check(none.tally.examined == 0 && none.tally.unreadable == 2,
              "a scan of a path that is not there examines 0 and counts 2 unreadable: "
                  + std::to_string(none.tally.examined) + "/" + std::to_string(none.tally.unreadable));
        check(none.findings.size() == 2 && !none.findings[0].fixable() && !none.findings[0].anything(),
              "and finds nothing it could act on, naming both files it could not read");
        const CueListScan empty = scanCueLists(root, {}, names);
        check(empty.tally.examined == 0 && empty.tally.unreadable == 0, "no rows: 0 examined");
    }

    // The repairs.
    {
        const std::string datBefore = bytesOf(clean.dat());
        const std::string extBefore = bytesOf(clean.ext());
        const auto f = examineTrackAnalysis(root, clean.analyzePath, names);
        const std::string account = repairTrackAnalysis(f, KeepCueList::Player);
        check(account.empty(), "clean: the repair has nothing to say: " + account);
        check(bytesOf(clean.dat()) == datBefore && bytesOf(clean.ext()) == extBefore,
              "clean: both files byte-identical after a repair pass");
    }
    {
        const auto f = examineTrackAnalysis(root, stale.analyzePath, names);
        repairTrackAnalysis(f, KeepCueList::Player);
        const auto after = examineTrackAnalysis(root, stale.analyzePath, names);
        check(!after.anything(), "a: clean after the repair");
        check(readTrackCueLists(root, stale.analyzePath).legacyMemory.size() == 2, "a: both entries kept");
        checkRepaired(stale, "a");
    }
    {
        const std::string entryBefore = b.datMemoryList.substr(24, 56);
        const auto f = examineTrackAnalysis(root, zeroSlot.analyzePath, names);
        repairTrackAnalysis(f, KeepCueList::Seabass);
        const auto after = examineTrackAnalysis(root, zeroSlot.analyzePath, names);
        check(!after.anything(), "b: clean after the repair");
        for (const auto &section : AnlzFile::readRaw(pathToUtf8(zeroSlot.dat())).sections) {
            if (section.fourcc == Pcob && readU32BE(section.rawBytes, 12) == CueListTypeMemory) {
                check(section.rawBytes.size() == 80, "b: the zero slot is gone");
                check(section.rawBytes.substr(24, 56) == entryBefore, "b: the player's entry kept byte for byte");
            }
        }
        checkRepaired(zeroSlot, "b");
    }
    {
        // c, keeping what the player shows: PCO2 becomes the two pads,
        // B keeping the red of the PCO2 entry that is the same cue.
        const Track keepPlayer = plant(pioneer, "00000006", disagreeingPair());
        const auto f = examineTrackAnalysis(root, keepPlayer.analyzePath, names);
        const std::string account = repairTrackAnalysis(f, KeepCueList::Player);
        check(account.find("from the player's list") != std::string::npos, "c/player: the account says so: " + account);
        const TrackCueLists lists = readTrackCueLists(root, keepPlayer.analyzePath);
        check(lists.modernHot.size() == 2 && lists.legacyHot.size() == 2, "c/player: two pads in both generations");
        for (const auto &cue : lists.modernHot) {
            if (cue.pad == 1) {
                check(cue.timeMs == 30765 && !cue.color, "c/player: A where the player had it, no colour carried "
                                                         "from a different cue");
            } else if (cue.pad == 2) {
                check(cue.timeMs == 67751 && cue.color == Red, "c/player: B where the player had it, red carried over");
            }
        }
        checkRepaired(keepPlayer, "c/player");
    }
    {
        // c, keeping what Seabass wrote: the legacy lists become the five
        // pads, split across the two files, and PCO2 is not touched.
        const Track keepSeabass = plant(pioneer, "00000007", disagreeingPair());
        const std::string pco2Before = [&] {
            std::string joined;
            for (const auto &s : AnlzFile::readRaw(pathToUtf8(keepSeabass.ext())).sections) {
                if (s.fourcc == Pco2) {
                    joined += s.rawBytes;
                }
            }
            return joined;
        }();
        const auto f = examineTrackAnalysis(root, keepSeabass.analyzePath, names);
        repairTrackAnalysis(f, KeepCueList::Seabass);
        const TrackCueLists lists = readTrackCueLists(root, keepSeabass.analyzePath);
        check(lists.legacyHot.size() == 5, "c/seabass: five pads on the player now");
        std::string pco2After;
        for (const auto &s : AnlzFile::readRaw(pathToUtf8(keepSeabass.ext())).sections) {
            if (s.fourcc == Pco2) {
                pco2After += s.rawBytes;
            }
        }
        check(pco2After == pco2Before, "c/seabass: the modern lists byte for byte as they were");
        checkRepaired(keepSeabass, "c/seabass");
    }
    {
        // b and c at once, as the RX2 left a real track: the player's
        // entry in the zero-slot list, and PCO2 holding ours and its own.
        for (const KeepCueList keep : {KeepCueList::Player, KeepCueList::Seabass}) {
            Pair both = exactPair();
            both.datMemoryList = playerZeroSlot(16640);
            both.modernMemory = {modern(0, 149), modern(0, 16640)};
            const std::string slot = keep == KeepCueList::Player ? "00000008" : "00000009";
            const Track t = plant(pioneer, slot, both);
            const auto f = examineTrackAnalysis(root, t.analyzePath, names);
            check(f.shape.zeroSlots && f.disagreement && f.disagreement->memory, "b+c: both found on " + slot);
            repairTrackAnalysis(f, keep);
            const TrackCueLists lists = readTrackCueLists(root, t.analyzePath);
            const size_t want = keep == KeepCueList::Player ? 1 : 2;
            check(lists.legacyMemory.size() == want && lists.modernMemory.size() == want,
                  "b+c: " + std::to_string(want) + " memory cues in both generations on " + slot);
            check(!examineTrackAnalysis(root, t.analyzePath, names).anything(), "b+c: clean afterwards on " + slot);
            checkRepaired(t, "b+c " + slot);
        }
    }
    {
        const auto f = examineTrackAnalysis(root, debris.analyzePath, names);
        const std::string datBefore = bytesOf(debris.dat());
        repairTrackAnalysis(f, KeepCueList::Player);
        check(!fs::exists(debris.dir / "ANLZ0001.DAT"), "d: the skeleton is removed");
        check(bytesOf(debris.dat()) == datBefore, "d: the track's own .DAT is not touched");
    }
    {
        // A pad only the legacy list has: the player shows it, and so
        // does Seabass (its reader adds legacy cues PCO2 lacks). The
        // lists still disagree, and keeping what Seabass wrote keeps the
        // pad rather than deleting a cue every reader showed.
        Pair onlyLegacy = exactPair();
        onlyLegacy.legacyHot.push_back(legacy(6, 150000));
        const Track t = plant(pioneer, "0000000B", onlyLegacy);
        const auto f = examineTrackAnalysis(root, t.analyzePath, names);
        check(f.disagreement && f.disagreement->hot, "a pad only the legacy list has is a disagreement");
        if (f.disagreement) {
            check(f.disagreement->seabassHot.size() == 5, "and Seabass shows it: "
                                                              + std::to_string(f.disagreement->seabassHot.size()));
        }
        repairTrackAnalysis(f, KeepCueList::Seabass);
        const TrackCueLists lists = readTrackCueLists(root, t.analyzePath);
        bool padF = false;
        for (const auto &cue : lists.modernHot) {
            padF = padF || (cue.pad == 6 && cue.timeMs == 150000);
        }
        check(padF && lists.legacyHot.size() == 5, "c/seabass: pad F kept, now in both generations");
        checkRepaired(t, "c/seabass legacy-only pad");
    }
    {
        // Paired in order of position, not first come first served:
        // legacy 100 and 700 against PCO2 550 and 50 pair as 100/50 and
        // 700/550, both within the tolerance, so nothing disagrees.
        Pair crossed = exactPair();
        crossed.datMemoryList = memoryList({100, 700});
        crossed.modernMemory = {modern(0, 550), modern(0, 50)};
        const Track t = plant(pioneer, "0000000C", crossed);
        const auto f = examineTrackAnalysis(root, t.analyzePath, names);
        check(f.examined && !f.disagreement, "memory cues are paired by position, not greedily");
    }
    {
        // A cue and a loop at one position, listed in opposite orders:
        // the loop pairs with the loop, nothing disagrees.
        Pair mixed = exactPair();
        std::vector<LegacyCueEntry> entries{legacy(0, 100), legacy(0, 100)};
        entries[1].isLoop = true;
        entries[1].loopEndMs = 5000;
        mixed.datMemoryList = AnlzLegacyCueCodec::encodeCues(entries, CueListTypeMemory);
        RawHotCueEntry loop = modern(0, 100);
        loop.isLoop = true;
        loop.loopEndMs = 5000;
        mixed.modernMemory = {loop, modern(0, 100)};
        const Track t = plant(pioneer, "0000000F", mixed);
        const auto f = examineTrackAnalysis(root, t.analyzePath, names);
        check(f.examined && !f.disagreement, "a loop pairs with a loop, not with the cue at its position");
    }
    {
        // What Seabass shows merges as its reader does: a legacy loop at a
        // position PCO2 already has a cue for is not shown, so keeping
        // Seabass's list does not write it.
        Pair hidden = exactPair();
        std::vector<LegacyCueEntry> entries{legacy(0, 30000)};
        entries[0].isLoop = true;
        entries[0].loopEndMs = 34000;
        hidden.datMemoryList = AnlzLegacyCueCodec::encodeCues(entries, CueListTypeMemory);
        hidden.modernMemory = {modern(0, 30000)};
        const Track t = plant(pioneer, "00000010", hidden);
        const auto f = examineTrackAnalysis(root, t.analyzePath, names);
        check(f.disagreement && f.disagreement->memory, "a loop against a cue is a disagreement");
        if (f.disagreement) {
            check(f.disagreement->seabassMemory.size() == 1 && !f.disagreement->seabassMemory[0].isLoop,
                  "and Seabass shows the cue alone, as its reader does");
        }
        repairTrackAnalysis(f, KeepCueList::Seabass);
        const TrackCueLists lists = readTrackCueLists(root, t.analyzePath);
        check(lists.legacyMemory.size() == 1 && !lists.legacyMemory[0].isLoop, "c/seabass writes what Seabass showed");
    }
    {
        // The .EXT gone: the stale memory list is still found from the
        // .DAT alone (an RX2 hangs on it all the same), and the track is
        // counted unreadable, not examined.
        Pair stale = exactPair();
        stale.datMemoryList = staleTwoUnlinked(5662, 240027);
        const Track t = plant(pioneer, "0000000D", stale);
        fs::remove(t.ext());
        const auto f = auditTrackAnalysis(root, t.analyzePath, names);
        check(f && f->shape.headerStale && f->shape.repairable(), "a stale list beside a missing .EXT is reported");
        check(f && !f->examined && !f->unreadable.empty(), "and the track is not counted as examined");
        CueListTally tally;
        if (f) {
            tally.add(*f);
            repairTrackAnalysis(*f, KeepCueList::Player);
            check(!examineTrackAnalysis(root, t.analyzePath, names).shape.damaged(), "and it is repaired");
        }
        check(tally.examined == 0 && tally.unreadable == 1 && tally.legacyHeader == 1, "counted unreadable");
    }
    {
        // Two tracks analysed into one directory, a stray file beside
        // them: one stray file, reported once.
        const Track first = plant(pioneer, "0000000E", exactPair());
        const fs::path secondDat = first.dir / "ANLZ0001.DAT";
        fs::copy_file(first.dat(), secondDat);
        fs::copy_file(first.ext(), first.dir / "ANLZ0001.EXT");
        writeSkeleton(first.dir / "ANLZ0002.DAT");
        const std::string second = "/PIONEER/USBANLZ/P001/0000000E/ANLZ0001.DAT";
        const auto bothNamed = [&](const std::string &p) { return p == first.analyzePath || p == second; };
        const CueListScan scan = scanCueLists(root, {first.analyzePath, second}, bothNamed);
        check(scan.tally.examined == 2 && scan.tally.strayFiles == 1 && scan.findings.size() == 1,
              "one stray file in a shared directory is one finding: " + std::to_string(scan.tally.strayFiles) + " / "
                  + std::to_string(scan.findings.size()));
    }
    {
        // Left alone: a hot cue on a pad no list can hold in both, and a
        // memory cue in the .EXT's legacy list.
        Pair odd = disagreeingPair();
        odd.modernHot.push_back(modern(9, 150000));
        const Track t = plant(pioneer, "0000000A", odd);
        const auto f = examineTrackAnalysis(root, t.analyzePath, names);
        check(f.disagreement && !f.disagreement->unrepairable.empty() && !f.listsFixable(),
              "a pad 9 makes the disagreement one the repair leaves alone");
    }
}

// The committed fixture, read only: every file its export.pdb names,
// counted. Pinned, because a count that drifts is how a check that
// stopped looking shows up (0 examined would otherwise pass as "clean").
// The numbers were cross-checked with an independent Python reading of
// the same files (PCOB/PCO2 walked by hand, the same tolerance; 203 of
// the 1161, the same 203; 17 more such files on the fixture belong to
// rows export.pdb no longer holds, which only OneLibrary names): the
// fixture was exported by Seabass 0.6, after the PCO2-only writer, so
// its disagreements are #60's, mostly a memory cue at 0:00 Engine's main
// cue left in PCO2 alone.
void fixtureCounts(const fs::path &fixture)
{
    const std::string root = pathToUtf8(fixture / "rekordbox");
    const AnlzPathIndex index(root);
    std::vector<std::string> paths(index.paths().begin(), index.paths().end());
    const CueListScan scan = scanCueLists(root, paths, [&index](const std::string &p) { return index.names(p); });
    std::cout << "fixture: " << paths.size() << " paths, examined " << scan.tally.examined << ", unreadable "
              << scan.tally.unreadable << ", legacy header " << scan.tally.legacyHeader << ", player rewritten "
              << scan.tally.playerRewritten << ", disagree " << scan.tally.disagree << ", stray "
              << scan.tally.strayFiles << "\n";
    check(scan.tally.examined == FixtureExamined, "fixture: examined " + std::to_string(scan.tally.examined));
    check(scan.tally.unreadable == 0, "fixture: unreadable " + std::to_string(scan.tally.unreadable));
    check(scan.tally.legacyHeader == 0 && scan.tally.playerRewritten == 0 && scan.tally.strayFiles == 0,
          "fixture: no memory list damage and no debris");
    check(scan.tally.disagree == FixtureDisagree, "fixture: disagree " + std::to_string(scan.tally.disagree));
    if (const char *dump = std::getenv("CUE_LIST_HEALTH_DUMP")) {
        std::ofstream out(dump);
        for (const auto &f : scan.findings) {
            out << f.analyzePath << "\n";
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: cue_list_health_test <anonymized_library dir>\n";
        return 2;
    }
    fixtureCounts(pathFromUtf8(argv[1]));
    const fs::path stick = seabass::testing::scratchRoot() / "seabass_cue_list_health_test";
    fs::remove_all(stick);
    findingsAndRepairs(stick);
    fs::remove_all(stick);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "cue_list_health_test: all cases passed\n";
    return 0;
}
