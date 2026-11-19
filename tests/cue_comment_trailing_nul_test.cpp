// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Issue #100: rekordbox stores a cue comment with a trailing NUL (its
// automatic hot cues say "1.1Bars\0"), Seabass read it that way, and Create
// Engine Library and the sync wrote it into Engine's quickCues labels.
//
// 1. An ANLZ cue entry with rekordbox's padded comment reads as "1.1Bars".
// 2. An Engine label "1.1Bars\0", as Seabass 1.0.x wrote it, reads as
//    "1.1Bars", and against rekordbox's "1.1Bars" neither Sync after
//    Rekordbox Export nor Sync Cue Points plans anything.
// 3. Create Engine Library and the Engine cue writer store no trailing NUL
//    in a label, whether the cue came through the reader or carries one
//    (an older backup): checked in the stored quickCues blob itself.
//
// The XML export needs nothing of its own: escapeXmlText drops a NUL like
// every other control character (rekordbox_xml_writer_test).

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v2/quick_cues_blob.hpp>

#include "application/use_cases/scan_library.hpp"
#include "domain/engine_update_planning.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::domain;
using namespace seabass::infrastructure;
using seabass::pathToUtf8;

namespace
{

int failures = 0;
int reported = 0;

// "case ... OK" only when the case added no failure since the last one.
void caseDone(const std::string &what)
{
    std::cout << "case " << what << (failures == reported ? " OK\n" : " FAILED\n");
    reported = failures;
}

const std::string Padded("1.1Bars\0", 8);
const std::string Clean = "1.1Bars";
const std::string AnalyzePath = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";

// "1.1Bars\0" as rekordbox spells it in an ANLZ file: UTF-16BE, the NUL
// included and counted in len_comment.
std::string utf16be(const std::string &ascii)
{
    std::string out;
    for (char c : ascii) {
        out.push_back('\0');
        out.push_back(c);
    }
    return out;
}

// A .EXT holding one PCO2 hot-cue list with one hot cue on pad 1 at
// 1000 ms whose comment is rekordbox's padded "1.1Bars". The entry layout
// is specs/rekordbox_anlz.ksy's cue_extended_entry: the 44-byte fixed part
// up to len_comment, the comment, then colour code and RGB.
std::string extWithPaddedComment()
{
    const std::string comment = utf16be(Padded);
    std::string entry = "PCP2";
    rekordbox::appendU32BE(entry, 0x10);                                           // len_header
    rekordbox::appendU32BE(entry, static_cast<uint32_t>(44 + comment.size() + 4));  // len_entry
    rekordbox::appendU32BE(entry, 1);                                              // hot_cue: pad 1
    entry.push_back('\x01');                                                       // type: a cue point
    entry += std::string("\x00\x03\xe8", 3);
    rekordbox::appendU32BE(entry, 1000);        // time
    rekordbox::appendU32BE(entry, 0xFFFFFFFF);  // loop_time: none
    entry.push_back('\0');                      // color_id
    entry += std::string(7, '\0');
    rekordbox::appendU16BE(entry, 0);  // loop_numerator
    rekordbox::appendU16BE(entry, 0);  // loop_denominator
    assert(entry.size() == 44 - 4);
    rekordbox::appendU32BE(entry, static_cast<uint32_t>(comment.size()));  // len_comment
    entry += comment;
    entry += std::string("\x01\xff\x00\x00", 4);  // color_code, red, green, blue

    std::string section = "PCO2";
    rekordbox::appendU32BE(section, 20);                                         // len_header
    rekordbox::appendU32BE(section, static_cast<uint32_t>(20 + entry.size()));  // len_tag
    rekordbox::appendU32BE(section, 1);                                          // type: hot cues
    rekordbox::appendU16BE(section, 1);                                          // num_cues
    rekordbox::appendU16BE(section, 0);
    section += entry;

    std::string data = "PMAI";
    rekordbox::appendU32BE(data, 12);  // len_header
    rekordbox::appendU32BE(data, 0);   // len_file, patched below
    data += section;
    rekordbox::writeU32BE(data, 8, static_cast<uint32_t>(data.size()));
    return data;
}

// Serves the .EXT above, and no .DAT.
class OneExtSource : public rekordbox::AnlzByteSource
{
public:
    std::optional<std::string> read(const std::string &relativePath) override
    {
        if (relativePath.size() > 4 && relativePath.compare(relativePath.size() - 4, 4, ".EXT") == 0) {
            return extWithPaddedComment();
        }
        return std::nullopt;
    }
};

// The labels of the quickCues blob Engine stores for `trackId`, decoded
// from the database row itself rather than through any Seabass reader.
std::vector<std::string> storedLabels(const fs::path &engineLibrary, std::int64_t trackId)
{
    sqlite3 *db = nullptr;
    const std::string dbPath = pathToUtf8(engineLibrary / "Database2" / "m.db");
    assert(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT quickCues FROM PerformanceData WHERE trackId = ?", -1, &stmt, nullptr)
           == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, trackId);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const auto *bytes = static_cast<const std::byte *>(sqlite3_column_blob(stmt, 0));
    const std::vector<std::byte> blob(bytes, bytes + sqlite3_column_bytes(stmt, 0));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    std::vector<std::string> labels;
    for (const auto &cue : djinterop::engine::v2::quick_cues_blob::from_blob(blob).quick_cues) {
        if (cue.sample_offset >= 0) {
            labels.push_back(cue.label);
        }
    }
    return labels;
}

Track rekordboxTrack(const fs::path &root, std::vector<CuePoint> cues)
{
    Track t;
    t.format = "rekordbox";
    t.sourceId = "1";
    t.title = "Song";
    t.artist = "Artist";
    t.filePath = pathToUtf8(root / "Contents" / "song.mp3");
    t.bpm = 120.0;
    t.durationSeconds = 300.0;
    t.analysisFile = AnalyzePath;
    t.cues = std::move(cues);
    return t;
}

std::vector<Track> readEngine(const fs::path &engineLibrary)
{
    engine::LibdjinteropEngineReader reader(pathToUtf8(engineLibrary));
    return seabass::application::ScanLibrary(reader).execute();
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_cue_comment_trailing_nul_test";
    fs::remove_all(root);
    fs::create_directories(root / "Contents");

    // 1. The ANLZ reader.
    std::vector<CuePoint> fromAnlz;
    {
        OneExtSource source;
        const auto cues = rekordbox::readAnalysisFileCues(source, AnalyzePath);
        assert(cues && cues->size() == 1);
        const CuePoint &cue = cues->front();
        assert(cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == 1 && cue.positionMs == 1000.0);
        if (cue.comment != Clean) {
            std::cerr << "FAIL: the ANLZ comment reads as " << cue.comment.size() << " bytes, not \"1.1Bars\"\n";
            ++failures;
        }
        fromAnlz = *cues;
        caseDone("1 (rekordbox's padded ANLZ comment reads as \"1.1Bars\")");
    }

    // 3a. Create Engine Library from those cues: the label Engine stores is
    // clean. And from a cue that still carries the NUL, as a metadata
    // backup an older Seabass made can.
    const fs::path created = root / "Engine Library";
    std::int64_t trackId = 0;
    {
        CuePoint carried = fromAnlz.front();
        carried.hotCueNumber = 2;
        carried.positionMs = 2000.0;
        carried.comment = Padded;
        std::vector<CuePoint> cues = fromAnlz;
        cues.push_back(carried);
        const auto result = engine::EngineLibraryCreator::create(pathToUtf8(created), {rekordboxTrack(root, cues)},
                                                                 engine::EngineSchemaGeneration::V2);
        assert(result.errorMessage.empty() && result.tracksCreated == 1);
        const auto engineTracks = readEngine(created);
        assert(engineTracks.size() == 1);
        trackId = std::stoll(engineTracks.front().sourceId);
        const auto labels = storedLabels(created, trackId);
        assert(labels.size() == 2);
        for (const auto &label : labels) {
            if (label != Clean) {
                std::cerr << "FAIL: Create Engine Library stored a " << label.size() << "-byte label\n";
                ++failures;
            }
        }
        caseDone("3a (Create Engine Library stores \"1.1Bars\" without the NUL)");
    }

    // 2. A stick Seabass 1.0.x wrote: plant "1.1Bars\0" through libdjinterop
    // itself, then read it back through Seabass.
    {
        auto db = djinterop::engine::load_database(pathToUtf8(created));
        auto track = db.track_by_id(trackId);
        assert(track);
        std::vector<std::optional<djinterop::hot_cue>> slots(8);
        slots[0] = djinterop::hot_cue{Padded, 44100.0, djinterop::pad_color{0xFF, 0, 0, 0xFF}};
        track->set_hot_cues(slots);
    }
    {
        const auto labels = storedLabels(created, trackId);
        assert(labels.size() == 1 && labels.front() == Padded);  // planted as 1.0.x wrote it
    }
    {
        const auto engineTracks = readEngine(created);
        assert(engineTracks.size() == 1);
        const Track &engineTrack = engineTracks.front();
        assert(engineTrack.cues.size() == 1);
        if (engineTrack.cues.front().comment != Clean) {
            std::cerr << "FAIL: Engine's label reads as " << engineTrack.cues.front().comment.size()
                      << " bytes, not \"1.1Bars\"\n";
            ++failures;
        }
        caseDone("2a (Engine's \"1.1Bars\\0\" reads as \"1.1Bars\")");

        // rekordbox's side: the same track and the same cue, as case 1 read it.
        Track rb = engineTrack;
        rb.format = "rekordbox";
        rb.sourceId = "1";
        rb.cues = fromAnlz;
        rb.cues.front().positionMs = engineTrack.cues.front().positionMs;
        rb.cues.front().color = engineTrack.cues.front().color;

        const std::string stickRoot = pathToUtf8(root) + "/";
        EngineUpdateInput in;
        in.rekordbox = {rb};
        in.engine = engineTracks;
        in.currentSequence = 2;
        in.enginePdbImportKey = {{engineTrack.sourceId, 1}};
        in.stickRelativeOf = [stickRoot](const std::string &path) {
            return path.rfind(stickRoot, 0) == 0 ? path.substr(stickRoot.size()) : std::string();
        };
        in.pathKeyOf = [](const std::string &relative) { return relative; };
        in.fileExists = [](const std::string &) { return true; };
        // Without a record, and with one that equals rekordbox now.
        const auto withoutRecord = EngineUpdatePlanner::plan(in);
        in.baseline = baselineFrom(in.rekordbox, {}, 1, in.stickRelativeOf, in.pathKeyOf);
        const auto withRecord = EngineUpdatePlanner::plan(in);
        for (const auto *p : {&withoutRecord, &withRecord}) {
            assert(p->cuesToEngine.empty() && p->cuesToRekordbox.empty());
            assert(p->conflicts.empty());
            assert(p->empty());
        }

        const SyncPlan cuePlan = SyncPlanner::plan(SyncMatch{rb, engineTrack}, {}, {});
        assert(cuePlan.direction == SyncPlan::Direction::None);
        caseDone("2b (\"1.1Bars\\0\" on Engine against rekordbox's \"1.1Bars\": nothing planned, no "
                     "conflict)");
    }

    // 3b. The Engine cue writer a sync writes with: a cue that still
    // carries the NUL is stored without it.
    {
        engine::LibdjinteropEngineCueWriter writer(pathToUtf8(created));
        writer.setLeadInSource([](const std::string &) { return std::nullopt; });
        CuePoint cue = fromAnlz.front();
        cue.comment = Padded;
        CuePoint loop = cue;
        loop.hotCueNumber = 2;
        loop.isLoop = true;
        loop.positionMs = 3000.0;
        loop.loopEndMs = 4000.0;
        writer.writeHotCues(std::to_string(trackId), {cue, loop});
        const auto labels = storedLabels(created, trackId);
        assert(labels.size() == 1);
        if (labels.front() != Clean) {
            std::cerr << "FAIL: the Engine cue writer stored a " << labels.front().size() << "-byte label\n";
            ++failures;
        }
        const auto engineTracks = readEngine(created);
        bool sawLoop = false;
        for (const auto &c : engineTracks.front().cues) {
            if (c.isLoop) {
                sawLoop = true;
            }
        }
        assert(sawLoop);
        // The loop's label, from libdjinterop's own decode of the loops blob.
        auto db = djinterop::engine::load_database(pathToUtf8(created));
        const auto loops = db.track_by_id(trackId)->loops();
        assert(loops.size() >= 2 && loops[1]);
        if (loops[1]->label != Clean) {
            std::cerr << "FAIL: the Engine cue writer stored a " << loops[1]->label.size() << "-byte loop label\n";
            ++failures;
        }
        caseDone("3b (the Engine cue writer stores hot cue and loop labels without the NUL)");
    }

    fs::remove_all(root);
    return failures == 0 ? 0 : 1;
}
