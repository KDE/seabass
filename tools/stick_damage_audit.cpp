// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Read-only: does a stick (or a copy of one) carry what Seabass builds
// before 6e0f1c09 and 05d71bbd left behind? Legacy memory cue lists a
// player hangs on (#55) were written by builds up to 0.7.12, including the
// published 0.7.11 and 0.7.12 alphas; Engine cues without a colour, which
// the player hides (#56), by builds before 05d71bbd, including all three
// alphas 0.7.11 to 0.7.13. Seabass ships no repair for either (Library
// Health's checks for them were removed on 2026-10-04); users of those
// alphas are advised to re-export the stick from rekordbox. Runs the same
// checks and prints the counts. Point it at a copy, not a mounted stick,
// to be sure nothing is ever opened for writing.
//
// #57: Sync Cue Points in builds before afa3dbdb could write a OneLibrary
// row's cue table on its own, with another copy's cues. The players read
// the analysis file (#59), and OneLibraryCueWriter writes the table and
// the file alike, so a table holding cues the file does not is what those
// builds left. An empty table next to a file with cues is not: rekordbox
// and the OMNIS-DUO leave the table alone. Each differing row is listed
// with both cue sets.
//
//   stick_damage_audit <root holding PIONEER/ and Engine Library/>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/hidden_engine_cues.hpp"
#include "domain/cue_tolerance.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

namespace fs = std::filesystem;

namespace
{

using seabass::domain::CuePoint;

// A cue table row as OneLibraryCueWriter writes one: kind 0 is a memory
// cue, otherwise the hot cue slot; a loop has its out point past its in.
std::vector<CuePoint> cueTableOf(const seabass::infrastructure::onelibrary::SqlCipherDb &db, const std::string &contentId)
{
    namespace ol = seabass::infrastructure::onelibrary;
    std::vector<CuePoint> cues;
    ol::SqlCipherStatement select(db, "SELECT kind, inUsec, outUsec, isActiveLoop FROM cue WHERE content_id = ?");
    select.bindInt64(1, std::stoll(contentId));
    while (select.step()) {
        CuePoint cue;
        const std::int64_t kind = select.columnInt64(0);
        cue.kind = kind == 0 ? CuePoint::Kind::Memory : CuePoint::Kind::Hot;
        cue.hotCueNumber = static_cast<int>(kind);
        cue.positionMs = static_cast<double>(select.columnInt64(1)) / 1000.0;
        const std::int64_t out = select.columnInt64(2);
        cue.isLoop = (!select.columnIsNull(3) && select.columnInt64(3) != 0) || out > select.columnInt64(1);
        cue.loopEndMs = cue.isLoop ? static_cast<double>(out) / 1000.0 : 0.0;
        cues.push_back(cue);
    }
    return cues;
}

// True if every cue the table holds is in the file too: a hot cue on the
// same pad at the same place, a memory cue at the same place. The file
// holding more (rekordbox's memory cue at 0:00, which the table never
// carries) is not what #57 is about.
bool tableWithinFile(const std::vector<CuePoint> &table, const std::vector<CuePoint> &file, double toleranceMs)
{
    for (const auto &t : table) {
        bool found = false;
        for (const auto &f : file) {
            if (f.kind == t.kind && (t.kind == CuePoint::Kind::Memory || f.hotCueNumber == t.hotCueNumber)
                && seabass::domain::sameCuePlace(t, f, toleranceMs)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

std::string describe(const std::vector<CuePoint> &cues)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    for (const auto &c : cues) {
        out << (c.kind == CuePoint::Kind::Hot ? std::string(1, static_cast<char>('A' + c.hotCueNumber - 1)) : std::string("M"))
            << "@" << c.positionMs / 1000.0;
        if (c.isLoop) {
            out << "-" << c.loopEndMs / 1000.0;
        }
        out << " ";
    }
    return cues.empty() ? std::string("(none)") : out.str();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: stick_damage_audit <stick root or copy>\n";
        return 2;
    }
    const fs::path root = seabass::pathFromUtf8(argv[1]);
    const std::string pioneer = seabass::pathToUtf8(root / "PIONEER");
    if (fs::exists(root / "PIONEER" / "rekordbox" / "export.pdb")) {
        namespace rb = seabass::infrastructure::rekordbox;
        const rb::AnlzPathIndex index(pioneer);
        const std::vector<std::string> paths(index.paths().begin(), index.paths().end());
        const auto scan = rb::scanCueLists(pioneer, paths, [&index](const std::string &p) { return index.names(p); });
        const auto &t = scan.tally;
        std::cout << "#55 memory cue lists: " << t.examined << " analysis files examined, " << t.unreadable
                  << " unreadable; stale header or unlinked (pre-6e0f1c09): " << t.legacyHeader
                  << "; rewritten by an RX2 (zero slot): " << t.playerRewritten << "; RX2 freeze debris files: " << t.strayFiles
                  << "; legacy and modern lists disagree (#60): " << t.disagree << "\n";
        if (std::getenv("SEABASS_AUDIT_DISAGREEMENTS")) {
            for (const auto &f : scan.findings) {
                if (!f.disagreement || !f.disagreement->any()) {
                    continue;
                }
                const auto &l = f.disagreement->lists;
                std::cout << "DISAGREE\t" << f.analyzePath << "\t" << l.legacyHot.size() << "\t" << l.modernHot.size() << "\t"
                          << l.legacyMemory.size() << "\t" << l.modernMemory.size() << "\t" << f.disagreement->viewsAgree << "\n";
            }
        }
        int shown = 0;
        for (const auto &f : scan.findings) {
            if (f.memoryListFinding() && shown++ < 10) {
                std::cout << "  " << f.analyzePath << (f.debris.empty() ? "" : " (+debris)") << "\n";
            }
        }
    }
    if (seabass::infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        namespace ol = seabass::infrastructure::onelibrary;
        // Cues from each row's analysis file, the half the players read.
        const auto tracks = ol::OneLibraryReader(pioneer).readAll();
        ol::SqlCipherLibrary lib;
        ol::SqlCipherDb db(lib, ol::OneLibraryCueWriter::dbPathFor(pioneer), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + ol::deriveOneLibraryKey() + "';");
        int emptyTable = 0;
        int agree = 0;
        std::vector<std::string> differ;
        for (const auto &track : tracks) {
            const auto table = cueTableOf(db, track.sourceId);
            if (table.empty()) {
                ++emptyTable;
                continue;
            }
            if (tableWithinFile(table, track.cues, seabass::domain::cueToleranceMsFor(track.bpm, track.bpm))) {
                ++agree;
                continue;
            }
            differ.push_back("  content_id " + track.sourceId + " " + track.filePath + "\n    cue table:     " + describe(table)
                             + "\n    analysis file: " + describe(track.cues));
        }
        std::cout << "#57 OneLibrary cue table against the analysis file: " << tracks.size() << " rows read; "
                  << emptyTable << " with an empty table, " << agree << " within the file, " << differ.size()
                  << " hold cues the analysis file does not\n";
        for (const auto &line : differ) {
            std::cout << line << "\n";
        }
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db")) {
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(engine));
        const auto tracks = reader.readAll();
        const auto hidden = seabass::domain::HiddenEngineCueFinder::find(tracks);
        int hotCues = 0;
        int loops = 0;
        for (const auto &h : hidden) {
            hotCues += h.hotCues;
            loops += h.loops;
        }
        std::cout << "#56 Engine cues the player hides: " << tracks.size() << " tracks read; " << hidden.size()
                  << " tracks with colourless pads: " << hotCues << " hot cues, " << loops << " loops\n";
        for (const auto &h : hidden) {
            std::cout << "  " << h.track.title << " / " << h.track.artist << ": " << h.hotCues << " hot cues, " << h.loops << " loops\n";
        }
    }
    return 0;
}
