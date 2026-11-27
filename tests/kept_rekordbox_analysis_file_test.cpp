// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// rekordbox's analysis kept on the stick before Analyse again replaces it
// (infrastructure/local/kept_rekordbox_analysis_file.hpp), the rule that
// puts it back (rekordbox::restoreAnlzAnalysis()) and the rule that
// decides whether to keep it (gui::keepOriginalDecision()):
//
// 1. A round trip keeps every byte of every file (each byte value, a file
//    that is not there), every row value (a NULL tempo, a key id), a key
//    and a path with a tab, newline and backslash.
// 2. A damaged entry is Damaged, never believed: one byte changed, cut
//    short, another version, not gzip.
// 3. keep() calls the hook first, writes durably and reads the entry back;
//    a hook that throws leaves nothing; a folder where the entry goes makes
//    keep() throw and leaves the folder; drop() of nothing is no error.
// 4. The listing reads the stick path from the first line, a damaged
//    entry listed as unreadable; the Engine listing reads the row's path.
// 5. The restore rule: the original byte for byte with the cue lists as
//    Analyse again left them; the current cue lists in the original's
//    places when they changed; one more cue list after the last of its
//    kind; one fewer left out.
// 6. The keep decision: Seabass's grid (the ledger's, or one a record
//    lists) keeps nothing new; someone else's is kept; a kept original
//    stays against Seabass's grids and its own, and is never replaced: a
//    grid changed outside Seabass leaves the track alone; without a record
//    Seabass cannot tell.
// 7. The head: read without the kept bytes; every record's Seabass grids.
// 8. The record: a round trip; damaged is Damaged; keep() writes the entry
//    and then its record, keepRecord() the record alone; drop() takes both
//    away; the listing counts the record's size, names it and its origin.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

#include "application/ports/kept_engine_analysis.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
#include "infrastructure/local/gzip_text.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::local::KeptRekordboxAnalysis;
using infrastructure::local::StickKeptRekordboxAnalysisStore;
using Status = infrastructure::local::KeptRekordboxAnalysisLookup::Status;
using infrastructure::rekordbox::AnlzFile;
using infrastructure::rekordbox::AnlzRawSection;

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

std::string every256()
{
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes += static_cast<char>(i);
    }
    return bytes;
}

std::string bytesOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path &file, const std::string &bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string gz(const std::string &text)
{
    std::string problem;
    return infrastructure::local::gzipBytes(text, &problem).value();
}

KeptRekordboxAnalysis sample()
{
    KeptRekordboxAnalysis e;
    e.pathKey = "contents/a\tb\nc\\d.wav";
    e.stickPath = "/Contents/a\tb\nc\\d.wav";
    e.audioFileSize = 123456789;
    e.audioFileMtimeUnix = 1764100000;
    e.originalGrid = std::string(64, 'a');
    e.files.push_back({"/PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.DAT", every256(), every256() + "x", std::nullopt});
    e.files.push_back({"/PIONEER/USBANLZ/P002/0000ABCE/ANLZ0000.DAT", std::string(), std::nullopt, every256()});
    e.rows.push_back({"rekordbox", 17, 12800, 0, std::nullopt});
    e.rows.push_back({"onelibrary", 9000000001, std::nullopt, 42, std::string("A\tm\n\\")});
    e.rows.push_back({"rekordbox", 18, 12800, 7, std::string("none")});
    return e;
}

std::string be32(std::uint32_t v)
{
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xff);
    }
    return out;
}

infrastructure::local::KeptRekordboxRecord recordOf(const KeptRekordboxAnalysis &e)
{
    infrastructure::local::KeptRekordboxRecord r;
    r.pathKey = e.pathKey;
    return r;
}

AnlzRawSection section(const char (&fourcc)[5], const std::string &body)
{
    AnlzRawSection s;
    s.fourcc = (std::uint32_t(std::uint8_t(fourcc[0])) << 24) | (std::uint32_t(std::uint8_t(fourcc[1])) << 16)
               | (std::uint32_t(std::uint8_t(fourcc[2])) << 8) | std::uint32_t(std::uint8_t(fourcc[3]));
    s.rawBytes = std::string(fourcc, 4) + be32(12) + be32(static_cast<std::uint32_t>(12 + body.size())) + body;
    return s;
}

AnlzFile anlz(std::vector<AnlzRawSection> sections, const std::string &headerExtra)
{
    AnlzFile file;
    file.headerBytes = "PMAI" + be32(28) + be32(0) + std::string(16, '\0');
    file.headerBytes.replace(12, headerExtra.size(), headerExtra);
    file.sections = std::move(sections);
    return file;
}

}  // namespace

int main()
{
    const fs::path stick = testing::scratchRoot() / "kept_rekordbox_analysis_file_test";
    fs::remove_all(stick);
    fs::create_directories(stick);

    // 1.
    {
        const KeptRekordboxAnalysis e = sample();
        const auto back = infrastructure::local::parseKeptRekordboxAnalysis(
            infrastructure::local::serializeKeptRekordboxAnalysis(e));
        check(back.status == Status::Found && back.entry == e, "1: a round trip keeps every value");
        KeptRekordboxAnalysis none = e;
        none.originalGrid.clear();
        none.rows.clear();
        const auto back2 = infrastructure::local::parseKeptRekordboxAnalysis(
            infrastructure::local::serializeKeptRekordboxAnalysis(none));
        check(back2.status == Status::Found && back2.entry == none, "1: no grids and no rows round trip too");
        std::cout << "case 1 (a round trip keeps every byte and value) OK\n";
    }

    // 2.
    {
        const std::string text = infrastructure::local::serializeKeptRekordboxAnalysis(sample());
        std::string flipped = text;
        flipped[text.find("audioFile") + 10] ^= 1;
        check(infrastructure::local::parseKeptRekordboxAnalysis(flipped).status == Status::Damaged,
              "2: one byte changed is Damaged");
        check(infrastructure::local::parseKeptRekordboxAnalysis(text.substr(0, text.size() - 1)).status
                  == Status::Damaged,
              "2: cut short is Damaged");
        std::string other = text;
        other.replace(other.find("\t1\t"), 3, "\t2\t");
        check(infrastructure::local::parseKeptRekordboxAnalysis(other).status == Status::Damaged,
              "2: another version is Damaged");
        const KeptRekordboxAnalysis e = sample();
        const fs::path dir = infrastructure::paths::stickKeptRekordboxAnalysisDir(stick);
        fs::create_directories(dir);
        StickKeptRekordboxAnalysisStore store(stick);
        writeBytes(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey), "not gzip at all");
        check(store.find(e.pathKey).status == Status::Damaged, "2: a file that is not gzip is Damaged");
        writeBytes(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey), gz(flipped));
        const auto damaged = store.find(e.pathKey);
        check(damaged.status == Status::Damaged && damaged.error.find("sha256") != std::string::npos,
              "2: a gzip of a changed entry is Damaged, and says why");
        fs::remove(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey));
        std::cout << "case 2 (a damaged entry is never believed) OK\n";
    }

    // 3.
    {
        StickKeptRekordboxAnalysisStore store(stick);
        std::vector<std::string> told;
        store.setBeforeChange([&](const std::string &file) { told.push_back(file); });
        const KeptRekordboxAnalysis e = sample();
        check(store.find(e.pathKey).status == Status::None, "3: nothing kept is None");
        store.keep(e, recordOf(e));
        const fs::path file = infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey);
        check(told.size() == 2 && told[0] == pathToUtf8(file)
                  && told[1] == pathToUtf8(infrastructure::local::keptRekordboxRecordFile(stick, e.pathKey)),
              "3: the hook is told of the entry first, then of its record");
        check(file.parent_path() == stick / "Seabass" / "rekordbox-analysis", "3: the file is in its folder");
        const auto found = store.find(e.pathKey);
        check(found.status == Status::Found && found.entry == e, "3: what was kept is found");
        std::cout << "  a kept entry of two 256-byte file sets: " << fs::file_size(file) << " bytes\n";
        store.setBeforeChange([](const std::string &) { throw std::runtime_error("refused"); });
        bool threw = false;
        try {
            store.drop(e.pathKey);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw && fs::exists(file), "3: a hook that throws leaves the file");
        store.setBeforeChange({});
        store.drop(e.pathKey);
        check(!fs::exists(file) && store.find(e.pathKey).status == Status::None, "3: drop takes it away");
        store.drop(e.pathKey);  // nothing: no error
        // A folder where the entry goes: keep() throws, the folder stays.
        fs::create_directories(file / "in the way");
        threw = false;
        try {
            store.keep(e, recordOf(e));
        } catch (const std::exception &) {
            threw = true;
        }
        check(threw && fs::is_directory(file / "in the way"), "3: a folder in the way makes keep() throw");
        fs::remove_all(file);
        std::cout << "case 3 (keep, find, drop; the hook first; a folder in the way) OK\n";
    }

    // 4.
    {
        StickKeptRekordboxAnalysisStore store(stick);
        KeptRekordboxAnalysis e = sample();
        store.keep(e, recordOf(e));
        KeptRekordboxAnalysis other = sample();
        other.pathKey = "contents/other.wav";
        other.stickPath = "/Contents/other.wav";
        store.keep(other, recordOf(other));
        writeBytes(infrastructure::paths::stickKeptRekordboxAnalysisDir(stick) / "0123456789abcdef0123456789abcdef.tsv.gz",
                   "damaged");
        std::string error;
        const auto listed = infrastructure::local::listKeptRekordboxAnalyses(stick, &error);
        int readable = 0;
        bool sawPath = false;
        for (const auto &info : listed) {
            readable += info.readable ? 1 : 0;
            sawPath = sawPath || info.trackFile == e.stickPath;
            const std::uintmax_t record = info.record.empty() ? 0 : fs::file_size(pathFromUtf8(info.record));
            check(info.bytes == fs::file_size(pathFromUtf8(info.file)) + record,
                  "4: each listed with its size and its record's");
        }
        check(error.empty() && listed.size() == 3 && readable == 2 && sawPath,
              "4: the listing reads the stick path of each, a damaged one unreadable");
        // Engine's listing.
        application::KeptEngineAnalysis engine;
        engine.pathKey = "contents/x.wav";
        engine.trackFile = "../Contents/x.wav";
        engine.trackId = 3;
        engine.libraryUuid = "u";
        infrastructure::local::StickKeptEngineAnalysisStore(stick).keep(engine);
        const auto engineListed = infrastructure::local::listKeptEngineAnalyses(stick, &error);
        check(error.empty() && engineListed.size() == 1 && engineListed[0].readable
                  && engineListed[0].trackFile == "../Contents/x.wav",
              "4: the Engine listing reads the row's path");
        fs::remove_all(stick / "Seabass");
        std::cout << "case 4 (the listings) OK\n";
    }

    // 5.
    {
        const AnlzFile original =
            anlz({section("PPTH", "path"), section("PQTZ", "rekordbox grid"), section("PCOB", "hot cues"),
                  section("PCOB", "memory cues"), section("PSSI", "phrases"), section("PWAV", "rekordbox wave")},
                 "orig");
        // Analyse again: the grid and the waveform replaced, the phrases
        // out, the cue lists kept.
        const AnlzFile analysed =
            anlz({section("PPTH", "path"), section("PQTZ", "seabass grid"), section("PCOB", "hot cues"),
                  section("PCOB", "memory cues"), section("PWAV", "seabass wave"), section("PWV2", "seabass tiny")},
                 "seab");
        const AnlzFile same = infrastructure::rekordbox::restoreAnlzAnalysis(original, analysed);
        check(same.toBytes() == original.toBytes(), "5: with the cue lists as they were, the original byte for byte");
        // A hot cue set since: the current hot cue list, everything else
        // the original's.
        AnlzFile edited = analysed;
        edited.sections[2] = section("PCOB", "hot cues edited");
        const AnlzFile withEdit = infrastructure::rekordbox::restoreAnlzAnalysis(original, edited);
        AnlzFile expected = original;
        expected.sections[2] = section("PCOB", "hot cues edited");
        check(withEdit.toBytes() == expected.toBytes(), "5: a changed cue list is the current one in its place");
        // A PCO2 only the current file has: after the last of its kind,
        // or at the end.
        AnlzFile extra = analysed;
        extra.sections.push_back(section("PCO2", "extended"));
        const AnlzFile withExtra = infrastructure::rekordbox::restoreAnlzAnalysis(original, extra);
        AnlzFile expectedExtra = original;
        expectedExtra.sections.push_back(section("PCO2", "extended"));
        check(withExtra.toBytes() == expectedExtra.toBytes(), "5: a cue list only the current file has goes last");
        // One PCOB fewer in the current file: the original's second one is
        // left out.
        AnlzFile fewer = analysed;
        fewer.sections.erase(fewer.sections.begin() + 3);
        const AnlzFile withFewer = infrastructure::rekordbox::restoreAnlzAnalysis(original, fewer);
        AnlzFile expectedFewer = original;
        expectedFewer.sections.erase(expectedFewer.sections.begin() + 3);
        check(withFewer.toBytes() == expectedFewer.toBytes(), "5: a cue list the current file lost stays lost");
        std::cout << "case 5 (the restore rule) OK\n";
    }

    // 6. The keep decision: (grid now, whether it is Seabass's, whether
    //    the ledger is believed, what is kept).
    {
        using gui::KeepOriginal;
        const std::string a(64, 'a');
        const std::string b(64, 'b');
        const std::string d(64, 'd');
        check(gui::keepOriginalDecision(b, true, true, nullptr, nullptr) == KeepOriginal::Nothing,
              "6: Seabass's grid and nothing kept: nothing to keep");
        check(gui::keepOriginalDecision(a, false, true, nullptr, nullptr) == KeepOriginal::KeepNow,
              "6: someone else's grid: kept now");
        check(gui::keepOriginalDecision(a, false, false, nullptr, nullptr) == KeepOriginal::CannotTell,
              "6: nothing kept and the ledger damaged: Seabass cannot tell whose grid it is, left alone");
        check(gui::keepOriginalDecision(b, true, false, nullptr, nullptr) == KeepOriginal::Nothing,
              "6: nothing kept, the ledger damaged, the grid known from a kept entry: nothing to keep");
        check(gui::keepOriginalDecision("", false, true, nullptr, nullptr) == KeepOriginal::KeepNow,
              "6: no grid at all, nothing kept: rekordbox's waveforms kept now");
        KeptRekordboxAnalysis kept;
        kept.originalGrid = a;
        infrastructure::local::KeptRekordboxRecord record;
        record.seabassGrids = {b};
        check(gui::keepOriginalDecision(b, true, true, &kept, &record) == KeepOriginal::KeptStays,
              "6: Seabass's grid: the kept original stays");
        check(gui::keepOriginalDecision(b, true, false, &kept, &record) == KeepOriginal::KeptStays,
              "6: Seabass's grid, the ledger lost: the kept original stays");
        check(gui::keepOriginalDecision(a, false, true, &kept, &record) == KeepOriginal::KeptStays,
              "6: the kept original's grid itself: it stays");
        check(gui::keepOriginalDecision("", false, true, &kept, &record) == KeepOriginal::KeptStays,
              "6: no grid in the file: no evidence of a newer analysis, the kept one stays");
        check(gui::keepOriginalDecision(d, false, true, &kept, &record) == KeepOriginal::ChangedOutside,
              "6: a grid nobody knows: changed outside Seabass, the kept original is not replaced");
        check(gui::keepOriginalDecision(d, false, false, &kept, &record) == KeepOriginal::ChangedOutside,
              "6: a grid nobody knows, the ledger lost: the record alone decides, the track is left alone");
        check(gui::keepOriginalDecision(b, false, true, &kept, nullptr) == KeepOriginal::CannotTell,
              "6: kept, and no record: Seabass cannot tell, whatever the ledger says");
        check(gui::keepOriginalDecision(a, false, true, &kept, nullptr) == KeepOriginal::CannotTell,
              "6: kept, no record, the kept grid: still cannot tell, the track is left alone");
        std::cout << "case 6 (the keep decision) OK\n";
    }

    // 7. The head: read without the kept bytes; every head's seabassGrids.
    {
        StickKeptRekordboxAnalysisStore store(stick);
        KeptRekordboxAnalysis e = sample();
        e.files[0].dat = std::string(200000, 'x');  // the head is read without it
        auto mine = recordOf(e);
        mine.seabassGrids = {std::string(64, 'b'), std::string(64, 'c')};
        store.keep(e, mine);
        KeptRekordboxAnalysis other = sample();
        other.pathKey = "contents/other.wav";
        other.stickPath = "/Contents/other.wav";
        auto theirs = recordOf(other);
        theirs.seabassGrids = {std::string(64, 'e')};
        store.keep(other, theirs);
        writeBytes(infrastructure::paths::stickKeptRekordboxAnalysisDir(stick) / "0123456789abcdef0123456789abcdef.tsv.gz",
                   gz("damaged\n"));
        const auto head = infrastructure::local::readKeptRekordboxAnalysisHead(
            infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey));
        check(head && head->pathKey == e.pathKey && head->stickPath == e.stickPath && head->originalGrid == e.originalGrid,
              "7: the head reads back");
        writeBytes(infrastructure::paths::stickKeptRekordboxAnalysisDir(stick) / "0123456789abcdef0123456789abcdef.seabass.tsv",
                   "damaged\n");
        std::string error;
        const auto grids = infrastructure::local::keptRekordboxSeabassGrids(stick, &error);
        check(error.empty()
                  && grids == std::vector<std::string>{std::string(64, 'b'), std::string(64, 'c'), std::string(64, 'e')},
              "7: every record's Seabass grids, a damaged one adding none");
        fs::remove_all(stick / "Seabass");
        std::cout << "case 7 (the head, and the Seabass grids of every record) OK\n";
    }

    // 8. The record.
    {
        using RecordStatus = infrastructure::local::KeptRekordboxRecordLookup::Status;
        infrastructure::local::KeptRekordboxRecord r;
        r.pathKey = "contents/a\tb.wav";
        r.originKnown = false;
        r.seabassGrids = {std::string(64, 'b'), std::string(64, 'c')};
        r.engineKept = std::string(64, 'f');
        const std::string text = infrastructure::local::serializeKeptRekordboxRecord(r);
        const auto back = infrastructure::local::parseKeptRekordboxRecord(text);
        check(back.status == RecordStatus::Found && back.record == r, "8: a record round trips");
        infrastructure::local::KeptRekordboxRecord plain;
        plain.pathKey = "k";
        const auto plainBack =
            infrastructure::local::parseKeptRekordboxRecord(infrastructure::local::serializeKeptRekordboxRecord(plain));
        check(plainBack.status == RecordStatus::Found && plainBack.record == plain, "8: an empty record round trips");
        std::string flipped = text;
        flipped[text.find("origin") + 8] ^= 1;
        check(infrastructure::local::parseKeptRekordboxRecord(flipped).status == RecordStatus::Damaged,
              "8: a changed byte is Damaged");
        check(infrastructure::local::parseKeptRekordboxRecord(text.substr(0, text.size() - 1)).status
                  == RecordStatus::Damaged,
              "8: cut short is Damaged");
        StickKeptRekordboxAnalysisStore store(stick);
        KeptRekordboxAnalysis e = sample();
        auto record = recordOf(e);
        record.seabassGrids = {std::string(64, 'b')};
        store.keep(e, record);
        check(store.findRecord(e.pathKey).status == RecordStatus::Found
                  && store.findRecord(e.pathKey).record == record,
              "8: keep() writes the record beside the entry");
        record.seabassGrids.push_back(std::string(64, 'c'));
        const std::string entryBytes = bytesOf(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey));
        store.keepRecord(record);
        check(store.findRecord(e.pathKey).record == record
                  && bytesOf(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey)) == entryBytes,
              "8: keepRecord() writes the record alone");
        const fs::path recordFile = infrastructure::local::keptRekordboxRecordFile(stick, e.pathKey);
        check(recordFile.parent_path() == infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey).parent_path()
                  && pathToUtf8(recordFile.filename()).find(".seabass.tsv") != std::string::npos,
              "8: the record sits beside the entry, the same name and another suffix");
        writeBytes(recordFile, "damaged\n");
        check(store.findRecord(e.pathKey).status == RecordStatus::Damaged, "8: a damaged record is Damaged");
        auto unknown = recordOf(e);
        unknown.originKnown = false;
        store.keepRecord(unknown);
        std::string error;
        const auto listed = infrastructure::local::listKeptRekordboxAnalyses(stick, &error);
        check(listed.size() == 1 && listed[0].record == pathToUtf8(recordFile) && !listed[0].originKnown,
              "8: the listing names the record and its origin");
        store.drop(e.pathKey);
        check(!fs::exists(recordFile) && !fs::exists(infrastructure::local::keptRekordboxAnalysisFile(stick, e.pathKey)),
              "8: drop() takes both away");
        fs::remove_all(stick / "Seabass");
        std::cout << "case 8 (the record beside the entry) OK\n";
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "kept_rekordbox_analysis_file_test: all cases passed\n";
    return 0;
}
