// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// engine_import_probe's --compare, on two records built by hand: one case
// for each verdict, so a classifier that confuses any two of them fails
// here before it misreports a real player's import in a talk.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine_import_probe_compare.hpp"

using namespace seabass::probe;

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

std::string set(std::vector<std::string> elements)
{
    return joinSet(elements);
}

}  // namespace

int main()
{
    // The classifier alone.
    check(classify(std::string("1000 ms"), std::string("1000 ms"), std::nullopt, false) == Verdict::Kept,
          "an unchanged value is KEPT");
    check(classify(std::string("1000 ms"), std::string("2000 ms"), std::string("2000 ms"), false)
              == Verdict::Overwritten,
          "a value replaced by rekordbox's is OVERWRITTEN");
    check(classify(std::string("1000 ms"), std::nullopt, std::nullopt, false) == Verdict::Deleted,
          "a value that is gone is DELETED");
    check(classify(std::nullopt, std::string("1000 ms"), std::string("1000 ms"), false) == Verdict::Added,
          "a value that appears is ADDED");
    check(classify(set({"x", "y"}), set({"x", "y", "z"}), set({"x", "z"}), true) == Verdict::Merged,
          "a set holding both sides is MERGED");
    check(classify(set({"x", "y"}), set({"x", "z"}), set({"x", "z"}), true) == Verdict::Overwritten,
          "a set replaced by rekordbox's is OVERWRITTEN");
    check(classify(std::nullopt, std::nullopt, std::string("1000 ms"), false) == Verdict::Kept,
          "absent before and after is KEPT");

    // Two records and a case list, through the file formats.
    Record before;
    Record after;
    before.set("M", "-", "pdb.sequence", "14205");
    after.set("M", "-", "pdb.sequence", "14205");
    before.set("M", "-", "engine.counter", "14204");
    after.set("M", "-", "engine.counter", "14205");
    for (Record *r : {&before, &after}) {
        r->set("RT", "Contents/a.mp3", "cue.hot.1", "10000 ms");
        r->set("RT", "Contents/c.mp3", "cue.hot.7", "45000 ms");
        r->set("RT", "Contents/b.mp3", "cue.hot.8", "30000 ms");
        r->set("RP", "Seabass test A", "entries", set({"Contents/a.mp3", "Contents/b.mp3"}));
        r->set("ET", "Contents/a.mp3", "cue.hot.1", "10000 ms");
        r->set("ET", "Contents/n.mp3", "title", "Untouched");
    }
    // kept: (a) hot 1; deleted: (a) hot 8; overwritten: (c) hot 7;
    // added: (b) hot 8; merged: the shared playlist.
    before.set("ET", "Contents/a.mp3", "cue.hot.8", "30000 ms");
    before.set("ET", "Contents/c.mp3", "cue.hot.7", "30000 ms");
    after.set("ET", "Contents/c.mp3", "cue.hot.7", "45000 ms");
    after.set("ET", "Contents/b.mp3", "cue.hot.8", "30000 ms");
    before.set("EP", "Seabass test A", "entries", set({"Contents/a.mp3", "Contents/x.mp3"}));
    after.set("EP", "Seabass test A", "entries", set({"Contents/a.mp3", "Contents/x.mp3", "Contents/b.mp3"}));
    // Something outside the matrix: an unplanted track's title.
    after.set("ET", "Contents/n.mp3", "title", "Renamed");
    // A file the import added.
    after.set("EF", "Database2/hm.db", "size", "4096");

    std::vector<Case> cases = {
        {"a1", "planted", "hot cue only in Engine", "Contents/a.mp3", "", "", "", {Item{"ET", "Contents/a.mp3", "cue.hot.8", "value"}}},
        {"a2", "planted", "the common cue", "Contents/a.mp3", "", "", "", {Item{"ET", "Contents/a.mp3", "cue.hot.1", "value"}}},
        {"b", "planted", "hot cue only in rekordbox", "Contents/b.mp3", "", "", "", {Item{"ET", "Contents/b.mp3", "cue.hot.8", "value"}}},
        {"c", "planted", "pad 7 apart", "Contents/c.mp3", "", "", "", {Item{"ET", "Contents/c.mp3", "cue.hot.7", "value"}}},
        {"h", "planted", "playlist", "Seabass test A", "", "", "", {Item{"EP", "Seabass test A", "entries", "set"}}},
        {"h3", "planted", "member", "Contents/b.mp3", "", "", "",
         {Item{"EP", "Seabass test A", "entries", "member:Contents/b.mp3"}}},
        {"z", "not planted", "no such track", "", "", "", "nothing to plant on", {}},
    };

    std::stringstream recordFile;
    writeRecord(recordFile, before);
    Record reread;
    std::string error;
    check(readRecord(recordFile, reread, &error) && reread.values == before.values,
          "a record reads back as written (" + error + ")");
    std::stringstream casesFile;
    writeCases(casesFile, cases);
    std::vector<Case> rereadCases;
    check(readCases(casesFile, rereadCases, &error) && rereadCases.size() == cases.size()
              && rereadCases[5].items.size() == 1 && rereadCases[5].items[0].mode == "member:Contents/b.mp3",
          "a case list reads back as written (" + error + ")");

    const CompareResult result = compareRecords(before, after, rereadCases);
    const auto verdictOf = [&result](const std::string &id) {
        for (const auto &c : result.cases) {
            if (c.planted.id == id) {
                return std::string(verdictName(c.verdict()));
            }
        }
        return std::string("missing");
    };
    check(verdictOf("a1") == "DELETED", "(a1) Engine-only hot cue: " + verdictOf("a1"));
    check(verdictOf("a2") == "KEPT", "(a2) common cue: " + verdictOf("a2"));
    check(verdictOf("b") == "ADDED", "(b) rekordbox-only hot cue: " + verdictOf("b"));
    check(verdictOf("c") == "OVERWRITTEN", "(c) pad apart: " + verdictOf("c"));
    check(verdictOf("h") == "MERGED", "(h) playlist: " + verdictOf("h"));
    check(verdictOf("h3") == "ADDED", "(h3) member: " + verdictOf("h3"));
    check(result.notPlanted == 1, "one case not planted");

    std::ostringstream printed;
    printCompare(printed, result);
    const std::string text = printed.str();
    const auto contains = [&text](const std::string &needle) { return text.find(needle) != std::string::npos; };
    check(contains("PROBE RESULT: 6 cases, 1 overwritten, 1 kept, 1 deleted, 2 added, 1 merged, 1 not planted"),
          "the result line counts every verdict");
    check(contains("30000 ms -> 45000 ms") && contains("Engine now matches it"),
          "an overwrite names old and new and says it now matches rekordbox");
    check(contains("engine.counter: 14204 -> 14205"), "the import counter's move is reported");
    check(contains("title: changed on 1 unplanted tracks"), "a change outside the matrix is reported");
    check(contains("added: Database2/hm.db"), "a file the import added is reported");
    check(contains("rekordbox side: unchanged"), "an untouched rekordbox side says so");

    if (failures > 0) {
        std::cerr << text;
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "engine_import_probe compare: every verdict classified as built\n";
    return 0;
}
