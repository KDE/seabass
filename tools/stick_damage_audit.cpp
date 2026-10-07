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
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/hidden_engine_cues.hpp"
#include "domain/onelibrary_cue_table.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

namespace fs = std::filesystem;

namespace
{

using seabass::domain::CuePoint;

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
        const ol::OneLibraryCueTables tables(pioneer);
        const auto audit = seabass::domain::auditCueTables(
            tracks, [&tables](const seabass::domain::Track &t) { return tables.of(std::stoll(t.sourceId)); });
        std::vector<std::string> differ;
        for (const auto &e : audit.excess) {
            differ.push_back("  content_id " + e.row.sourceId + " " + e.row.filePath + "\n    cue table:     " + describe(e.table)
                             + "\n    analysis file: " + describe(e.row.cues));
        }
        std::cout << "#57 OneLibrary cue table against the analysis file: " << audit.rowsRead << " rows read; "
                  << audit.emptyTable << " with an empty table, " << audit.withinFile << " within the file, " << differ.size()
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
