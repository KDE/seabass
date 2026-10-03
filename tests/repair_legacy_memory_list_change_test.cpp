// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RepairLegacyMemoryListChange through the real save loop (#55): a
// track's .DAT with the stale memory list of 5282555e and a hung
// player's ANLZ0001.DAT beside it comes out with the list in shape, its
// entry kept, the debris gone, and both files backed up for Undo.

#include <QString>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/repair_legacy_memory_list_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "application/path_key.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"
#include <fstream>
#include <iterator>
#include <set>

using namespace seabass;
using namespace seabass::gui;
using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

std::string fromHex(const std::string &hex)
{
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// after-seabass/P025/0000297B/ANLZ0000.DAT of the #33 evidence: one entry
// at 149 ms under a header that says "empty".
const std::string StaleHeaderOneEntry = fromHex(
    "50434f4200000018000000500000000000000001ffffffff504350540000001c00000038000000000000000000010000ffffffff"
    "010003e800000095ffffffff00000000000000000000000000000000");

std::string memoryListOf(const fs::path &dat)
{
    for (const auto &section : AnlzFile::readRaw(pathToUtf8(dat)).sections) {
        if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
            return section.rawBytes;
        }
    }
    return {};
}

// One content row's cue table, as (kind, inUsec).
std::multiset<std::pair<long long, long long>> tableCues(const std::string &pioneerRoot, const std::string &contentId)
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db(lib, infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot),
                                               /*readOnly=*/true);
    db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    infrastructure::onelibrary::SqlCipherStatement rows(db, "SELECT kind, inUsec FROM cue WHERE content_id = ?");
    rows.bindInt64(1, std::stoll(contentId));
    std::multiset<std::pair<long long, long long>> out;
    while (rows.step()) {
        out.insert({rows.columnInt64(0), rows.columnInt64(1)});
    }
    return out;
}

// Rows no player reads, drifted from the file.
void plantTableCues(const std::string &pioneerRoot, const std::string &contentId)
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db(lib, infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot),
                                               /*readOnly=*/false);
    db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    db.exec("DELETE FROM cue WHERE content_id = " + contentId + ";");
    db.exec("INSERT INTO cue (content_id, kind, colorTableIndex, isActiveLoop, inUsec, outUsec) VALUES (" + contentId
            + ", 7, 0, 0, 4242000, 4242000);");
}

// The analysis file's cues in the table's terms.
std::multiset<std::pair<long long, long long>> asTable(const std::vector<domain::CuePoint> &cues)
{
    std::multiset<std::pair<long long, long long>> out;
    for (const auto &cue : cues) {
        out.insert({cue.kind == domain::CuePoint::Kind::Hot ? cue.hotCueNumber : 0,
                    static_cast<long long>(cue.positionMs * 1000.0)});
    }
    return out;
}

// #60 with OneLibrary: a OneLibrary-only row whose analysis file's two
// lists disagree (PCO2 holds a pad the legacy list lacks) and whose cue
// table has drifted. After the repair, keeping what the player shows, the
// file's lists agree and the table holds what the file holds.
bool oneLibraryTableFollowsTheFile(const fs::path &fixture)
{
    const fs::path scratch = testing::scratchRoot() / "seabass_repair_cue_lists_onelibrary_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    const fs::path pioneer = scratch / "PIONEER";
    testing::copyPioneerFixture(fixture / "rekordbox", pioneer, ec);
    if (ec) {
        std::cerr << "could not copy the fixture: " << ec.message() << "\n";
        return false;
    }
    const std::string root = pathToUtf8(pioneer);
    const infrastructure::rekordbox::AnlzPathIndex index(root);
    std::set<std::string> deviceLibrary;
    for (const auto &p : index.paths()) {
        deviceLibrary.insert(application::normalizedPathKey(p));
    }
    const auto named = [&](const std::string &p) { return index.names(p); };
    std::optional<domain::Track> row;
    for (const auto &t : infrastructure::onelibrary::OneLibraryReader(root).readTracks()) {
        if (t.analysisFile.empty() || t.filePath.empty() || deviceLibrary.count(application::normalizedPathKey(t.analysisFile))) {
            continue;
        }
        const auto f = examineTrackAnalysis(root, t.analysisFile, named);
        infrastructure::rekordbox::FilesystemAnlzSource cuesOf(root);
        const auto has = infrastructure::rekordbox::readAnalysisFileCues(cuesOf, t.analysisFile);
        // Agreeing lists holding cues, so the table has something to
        // match after the repair.
        if (f.examined && !f.anything() && has && !has->empty()) {
            row = t;
            break;
        }
    }
    if (!row) {
        std::cerr << "FAIL: the fixture has no OneLibrary-only row with agreeing, nonempty lists to plant on\n";
        return false;
    }
    // A pad only PCO2 has: what a Seabass before 5282555e left.
    {
        const std::string ext = extAnlzPath(root, row->analysisFile);
        AnlzFile file = AnlzFile::readRaw(ext);
        for (auto &section : file.sections) {
            if (section.fourcc == 0x50434f32 && readU32BE(section.rawBytes, 12) == CueListTypeHot) {
                auto entries = AnlzCueCodec::decodeHotCues(section.rawBytes, CueListTypeHot);
                RawHotCueEntry extra;
                extra.hotCueNumber = 8;
                extra.timeMs = 200000;
                entries.push_back(extra);
                section.rawBytes = AnlzCueCodec::encodeHotCues(entries, CueListTypeHot);
            }
        }
        file.writeRaw(ext);
    }
    plantTableCues(root, row->sourceId);
    auto finding = examineTrackAnalysis(root, row->analysisFile, named);
    if (!finding.listsFixable()) {
        std::cerr << "FAIL: the planted disagreement is found and fixable\n";
        return false;
    }

    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pathToQString(pioneer), {});
    std::vector<std::shared_ptr<PendingChange>> changes;
    changes.push_back(std::make_shared<RepairLegacyMemoryListChange>(*row, finding, KeepCueList::Player));
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    // Backed up by the save already: a second backupOnce() makes none.
    const bool backsUpTheDatabase
        = !ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "legacy-memory-list");
    bool ok = true;
    if (!result.error.isEmpty() || result.appliedIds.size() != 1) {
        std::cerr << "FAIL: the repair saves: " << result.error.toStdString() << "\n";
        ok = false;
    }
    if (!backsUpTheDatabase) {
        std::cerr << "FAIL: exportLibrary.db is backed up before its table is refreshed\n";
        ok = false;
    }
    if (examineTrackAnalysis(root, row->analysisFile, named).disagreement) {
        std::cerr << "FAIL: the file's lists agree after the repair\n";
        ok = false;
    }
    infrastructure::rekordbox::FilesystemAnlzSource source(root);
    const auto inFile = infrastructure::rekordbox::readAnalysisFileCues(source, row->analysisFile);
    const auto table = tableCues(root, row->sourceId);
    if (!inFile || table != asTable(*inFile)) {
        std::cerr << "FAIL: the cue table holds what the file holds after the repair (table " << table.size()
                  << " rows, file " << (inFile ? inFile->size() : 0) << " cues)\n";
        ok = false;
    }
    for (const auto &[kind, in] : table) {
        if (kind == 8 || in == 4242000) {
            std::cerr << "FAIL: neither the dropped pad nor the drifted row is left in the table\n";
            ok = false;
        }
    }

    // The same audio file, but the repaired analysis file is another one
    // than its OneLibrary rows name: the refresh would write this file's
    // cues into theirs, so it is left out, and their file and table stay.
    {
        std::optional<std::string> otherFile;
        for (const auto &p : index.paths()) {
            const auto f = examineTrackAnalysis(root, p, named);
            if (f.examined && !f.anything() && application::normalizedPathKey(p) != application::normalizedPathKey(row->analysisFile)) {
                otherFile = p;
                break;
            }
        }
        if (!otherFile) {
            std::cerr << "FAIL: the fixture has a DeviceLibrary file to plant on\n";
            return false;
        }
        const std::string ext = extAnlzPath(root, *otherFile);
        AnlzFile file = AnlzFile::readRaw(ext);
        for (auto &section : file.sections) {
            if (section.fourcc == 0x50434f32 && readU32BE(section.rawBytes, 12) == CueListTypeHot) {
                auto entries = AnlzCueCodec::decodeHotCues(section.rawBytes, CueListTypeHot);
                RawHotCueEntry extra;
                extra.hotCueNumber = 8;
                extra.timeMs = 210000;
                entries.push_back(extra);
                section.rawBytes = AnlzCueCodec::encodeHotCues(entries, CueListTypeHot);
            }
        }
        file.writeRaw(ext);
        plantTableCues(root, row->sourceId);
        const std::string rowExtBefore = [&] {
            std::ifstream in(pathFromUtf8(extAnlzPath(root, row->analysisFile)), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();
        const auto otherFinding = examineTrackAnalysis(root, *otherFile, named);
        application::CancellationToken token2;
        SaveContext ctx2(token2, application::NullProgressReporter::instance(), nullptr, pathToQString(pioneer), {});
        std::vector<std::shared_ptr<PendingChange>> other;
        other.push_back(std::make_shared<RepairLegacyMemoryListChange>(*row, otherFinding, KeepCueList::Player));
        const SaveLoopResult saved = runSaveLoop(other, ctx2);
        const std::string rowExtAfter = [&] {
            std::ifstream in(pathFromUtf8(extAnlzPath(root, row->analysisFile)), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();
        if (!saved.error.isEmpty() || examineTrackAnalysis(root, *otherFile, named).disagreement) {
            std::cerr << "FAIL: the other file is repaired: " << saved.error.toStdString() << "\n";
            ok = false;
        }
        if (rowExtAfter != rowExtBefore) {
            std::cerr << "FAIL: the analysis file the OneLibrary row names is not written with another file's cues\n";
            ok = false;
        }
        const auto drifted = tableCues(root, row->sourceId);
        if (drifted.size() != 1 || drifted.begin()->second != 4242000) {
            std::cerr << "FAIL: and its table is left as it was\n";
            ok = false;
        }
    }
    fs::remove_all(scratch, ec);
    return ok;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: repair_legacy_memory_list_change_test <anonymized_library dir>\n";
        return 2;
    }
    const fs::path fixtureDat = pathFromUtf8(argv[1]) / "rekordbox" / "USBANLZ" / "P017" / "0002435D" / "ANLZ0000.DAT";
    assert(fs::exists(fixtureDat));

    const fs::path stick = testing::scratchRoot() / "seabass_repair_legacy_memory_list_change_test";
    fs::remove_all(stick);
    const fs::path pioneer = stick / "PIONEER";
    const fs::path dir = pioneer / "USBANLZ" / "P001" / "00000001";
    fs::create_directories(dir);
    {
        AnlzFile file = AnlzFile::readRaw(pathToUtf8(fixtureDat));
        for (auto &section : file.sections) {
            if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
                section.rawBytes = StaleHeaderOneEntry;
            }
        }
        file.writeRaw(pathToUtf8(dir / "ANLZ0000.DAT"));
    }
    {
        // The fixture's .EXT, its modern memory list holding the cue the
        // stale legacy one holds, as that writer left both.
        AnlzFile ext = AnlzFile::readRaw(pathToUtf8(fixtureDat.parent_path() / "ANLZ0000.EXT"));
        RawHotCueEntry entry;
        entry.timeMs = 149;
        for (auto &section : ext.sections) {
            if (section.fourcc == 0x50434f32 && readU32BE(section.rawBytes, 12) == CueListTypeMemory) {
                section.rawBytes = AnlzCueCodec::encodeHotCues({entry}, CueListTypeMemory);
            }
        }
        ext.writeRaw(pathToUtf8(dir / "ANLZ0000.EXT"));
    }
    {
        // The RX2's skeleton: empty beat grid, waveforms and cue lists.
        AnlzFile skeleton = AnlzFile::readRaw(pathToUtf8(fixtureDat));
        for (auto &section : skeleton.sections) {
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
            default:
                break;
            }
        }
        skeleton.writeRaw(pathToUtf8(dir / "ANLZ0001.DAT"));
    }

    const std::string analyzePath = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
    const auto named = [&analyzePath](const std::string &p) { return p == analyzePath; };
    auto finding = auditTrackAnalysis(pathToUtf8(pioneer), analyzePath, named);
    assert(finding && finding->shape.headerStale && finding->debris.size() == 1);

    domain::Track track;
    track.format = "rekordbox";
    track.sourceId = "7";
    track.title = "Codec";
    track.artist = "Aender";

    // Through the save loop, as Library Health stages it.
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pathToQString(pioneer), {});
    std::vector<std::shared_ptr<PendingChange>> changes;
    changes.push_back(std::make_shared<RepairLegacyMemoryListChange>(track, *finding));
    assert(changes[0]->id() == RepairLegacyMemoryListChange::idFor(analyzePath));
    assert(changes[0]->filesToBackup(ctx).size() == 2 && "the .DAT and the debris are both backed up");
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    assert(result.error.isEmpty() && !result.cancelled);
    assert(result.appliedIds.size() == 1);

    // The list is in shape with its entry, the debris is gone, the
    // sibling is not, and the track audits clean.
    const std::string after = memoryListOf(dir / "ANLZ0000.DAT");
    AnlzLegacyCueCodec::checkSection(after);
    assert(readU32BE(after, 20) == 0 && "the header names the last entry");
    assert(after.substr(24) == StaleHeaderOneEntry.substr(24) && "the entry is kept byte for byte");
    assert(!fs::exists(dir / "ANLZ0001.DAT") && "the debris is gone");
    assert(fs::exists(dir / "ANLZ0000.EXT") && "the .EXT is not");
    assert(!auditTrackAnalysis(pathToUtf8(pioneer), analyzePath, named).has_value());
    std::cout << "case 1 (stale list rebuilt, entry kept, debris removed) OK\n";

    fs::remove_all(stick);

    if (!oneLibraryTableFollowsTheFile(pathFromUtf8(argv[1]))) {
        return 1;
    }
    std::cout << "case 2 (lists rewritten, OneLibrary's cue table refreshed from the file) OK\n";
    std::cout << "all cases passed\n";
    return 0;
}
