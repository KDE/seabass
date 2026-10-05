// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The #59 hardware probe (tools/onelibrary_cue_source_probe) tested without
// a stick, against a copy of tests/fixtures/anonymized_library:
//
// - an unplanted copy fails the probe's own read-back for all five tracks,
//   so that read-back can go red;
// - after plant(), the read-back passes, and Seabass's own readers agree
//   independently: the OneLibrary reader shows T1 with no cue (its file is
//   empty), T3's loop, T4's pad B and T5's 0:55; the analysis-file reader
//   calls T2's two lists in disagreement; the cue table holds T1's and
//   T5's rows and T4's from Add Cue;
// - a T2 file whose legacy list agrees again fails the read-back, naming T2;
// - readback() of an untouched stick finds nothing, and of a stick a
//   "deck" then wrote finds the changed analysis file and the new file.

#include <QJsonObject>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "fixture_copy.hpp"
#include "onelibrary_cue_source_probe_lib.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using namespace seabass;
using namespace seabass::infrastructure;

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

bool mentions(const std::vector<std::string> &problems, const std::string &text)
{
    return std::any_of(problems.begin(), problems.end(),
                       [&](const std::string &p) { return p.find(text) != std::string::npos; });
}

fs::path freshStick(const std::string &name)
{
    const fs::path stick = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(stick, ec);
    seabass::testing::copyPioneerFixture(
        pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox",
        stick / "PIONEER", ec);
    if (ec) {
        std::cerr << "could not copy the fixture: " << ec.message() << "\n";
        std::exit(2);
    }
    return stick;
}

const domain::Track *oneLibraryRow(const std::vector<domain::Track> &tracks, const std::string &contentId)
{
    for (const auto &t : tracks) {
        if (t.sourceId == contentId) {
            return &t;
        }
    }
    return nullptr;
}

bool hasHot(const domain::Track &t, int slot, double ms, bool loop = false, double endMs = 0.0)
{
    return std::any_of(t.cues.begin(), t.cues.end(), [&](const CuePoint &c) {
        return c.kind == CuePoint::Kind::Hot && c.hotCueNumber == slot && c.positionMs == ms && c.isLoop == loop
            && (!loop || c.loopEndMs == endMs);
    });
}

}  // namespace

int main()
{
    seabass::testing::sandboxSeabassHome(seabass::testing::scratchRoot() / "seabass_omnis59_probe_home");
    const fs::path stick = freshStick("seabass_omnis59_probe_stick");
    const std::string root = pathToUtf8(stick);
    const std::string pioneer = pathToUtf8(stick / "PIONEER");

    std::ostringstream log;
    const auto candidates = omnis59::findCandidates(root, /*requireAudio=*/false, log);
    const auto five = omnis59::pickFive(candidates);
    if (five.size() != 5) {
        std::cerr << "the fixture offers " << candidates.size() << " candidate(s), not enough for five\n" << log.str();
        return 2;
    }
    for (const auto &c : five) {
        check(c.durationSeconds >= 150.0, c.title + " is long enough for T2's 1:40 pad");
    }

    // The read-back can fail: before planting, every track is wrong.
    {
        const auto problems = omnis59::verifyPlants(root, five);
        for (int t = 1; t <= 5; ++t) {
            check(mentions(problems, "T" + std::to_string(t) + " ("),
                  "an unplanted T" + std::to_string(t) + " fails the read-back");
        }
    }

    omnis59::plant(root, five, log);
    const auto problems = omnis59::verifyPlants(root, five);
    for (const auto &p : problems) {
        std::cerr << "  " << p << "\n";
    }
    check(problems.empty(), "every plant reads back exactly as planted");

    // Seabass's own readers, independently of the probe's decoding.
    {
        const auto tracks = onelibrary::OneLibraryReader(pioneer).readAll();
        const auto *t1 = oneLibraryRow(tracks, five[0].contentId);
        const auto *t3 = oneLibraryRow(tracks, five[2].contentId);
        const auto *t4 = oneLibraryRow(tracks, five[3].contentId);
        const auto *t5 = oneLibraryRow(tracks, five[4].contentId);
        check(t1 && t3 && t4 && t5, "the OneLibrary reader still lists the planted rows");
        if (t1 && t3 && t4 && t5) {
            check(t1->cues.empty(), "T1's analysis file holds no cue (its pad is in the table only)");
            check(t3->cues.size() == 1 && hasHot(*t3, 1, 30000.0, true, 38000.0), "T3 is one loop, A 0:30-0:38");
            check(t4->cues.size() == 1 && hasHot(*t4, 2, 75000.0), "T4 is pad B at 1:15");
            check(t5->cues.size() == 1 && hasHot(*t5, 1, 55000.0), "T5's file holds pad A at 0:55");
        }
        rekordbox::FilesystemAnlzSource source(pioneer);
        domain::Track::CueListsCheck lists = domain::Track::CueListsCheck::NotChecked;
        rekordbox::readAnalysisFileCues(source, five[1].analysisFile, &lists);
        check(lists == domain::Track::CueListsCheck::Disagree, "Seabass's reader finds T2's PCOB and PCO2 in disagreement");
        lists = domain::Track::CueListsCheck::NotChecked;
        rekordbox::readAnalysisFileCues(source, five[4].analysisFile, &lists);
        check(lists == domain::Track::CueListsCheck::Examined, "T5's two lists agree");
    }
    {
        onelibrary::SqlCipherLibrary lib;
        onelibrary::SqlCipherDb db(lib, onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + onelibrary::deriveOneLibraryKey() + "';");
        const auto rows = [&](const std::string &contentId) {
            std::vector<std::string> out;
            onelibrary::SqlCipherStatement select(
                db, "SELECT kind || ' ' || inUsec || ' ' || ifnull(cueComment, '') FROM cue WHERE content_id = ?");
            select.bindInt64(1, std::stoll(contentId));
            while (select.step()) {
                out.push_back(select.columnText(0));
            }
            return out;
        };
        check(rows(five[0].contentId) == std::vector<std::string>{"1 45000000 T1 TABLE"}, "T1's table row");
        check(rows(five[1].contentId).empty(), "T2 has no table row");
        check(rows(five[2].contentId).empty(), "T3 has no table row");
        check(rows(five[3].contentId) == std::vector<std::string>{"2 75000000 T4 SEABASS"}, "T4's row from Add Cue");
        check(rows(five[4].contentId) == std::vector<std::string>{"1 50000000 T5 TABLE"}, "T5's table row at 0:50");
    }
    {
        const std::string text = omnis59::card(five, "TEST");
        for (size_t i = 0; i < five.size(); ++i) {
            check(text.find("T" + std::to_string(i + 1) + "  \"" + five[i].title + "\"") != std::string::npos,
                  "the card names T" + std::to_string(i + 1));
        }
    }

    // Snapshot, then readback: nothing, then what a deck wrote.
    const QJsonObject snap = omnis59::snapshot(root, five);
    {
        std::ostringstream out;
        check(omnis59::readback(root, snap, out) == 0, "an untouched stick reads back with no difference:\n" + out.str());
    }
    {
        rekordbox::RekordboxCueWriter(pioneer).writeCuesToAnalysisFile(
            five[2].analysisFile, {CuePoint{CuePoint::Kind::Hot, 3, 90000.0, "", "", false, 0.0}});
        std::ofstream(stick / "PIONEER" / "DECK-WROTE-THIS.DAT") << "x";
        std::ostringstream out;
        const int differences = omnis59::readback(root, snap, out);
        const std::string report = out.str();
        check(differences >= 4, "a deck's writes are counted (" + std::to_string(differences) + ")");
        check(report.find("CHANGED pco2Hot: was pad A 0:30-0:38 loop, now pad C 1:30") != std::string::npos,
              "readback decodes the changed PCO2 list:\n" + report);
        check(report.find("ADDED   PIONEER/DECK-WROTE-THIS.DAT") != std::string::npos, "readback lists the new file");
    }

    // A T2 whose legacy list agrees with the modern one again is caught.
    {
        const std::vector<CuePoint> three{CuePoint{CuePoint::Kind::Hot, 1, 20000.0, "", "", false, 0.0},
                                          CuePoint{CuePoint::Kind::Hot, 2, 60000.0, "", "", false, 0.0},
                                          CuePoint{CuePoint::Kind::Hot, 3, 100000.0, "", "", false, 0.0}};
        rekordbox::RekordboxCueWriter(pioneer).writeCuesToAnalysisFile(five[1].analysisFile, three);
        const auto after = omnis59::verifyPlants(root, five);
        check(mentions(after, "T2 (") && mentions(after, ".DAT PCOB hot"), "an agreeing T2 fails the read-back");
    }

    std::error_code ec;
    fs::remove_all(stick, ec);
    if (failures) {
        std::cerr << failures << " check(s) failed\n" << log.str();
        return 1;
    }
    std::cout << "onelibrary_cue_source_probe_test OK\n";
    return 0;
}
