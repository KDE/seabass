// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Copy Beat Grids (application/use_cases/copy_beat_grids), with a fake
// writer that answers each Engine row with the outcome the test gives it:
//
// - the plan pairs tracks by file, keys each by its stick-relative path,
//   and counts every rekordbox track once (an item, rekordbox only, or
//   left out as ambiguous: two rekordbox rows on one Engine row);
// - every outcome lands in its own bucket, a throwing source or writer
//   is a failure with its message, bar breaks add up over the rows
//   written, and the buckets add up to the plan;
// - the ledger's fingerprint reaches the writer for that track and no
//   other, and only Written and AlreadySame rows come back as ledger
//   entries;
// - the Fill modes ask for the file's facts only for a row the writer
//   turned down for want of them, and write it again with them;
// - cancelled midway, the run stops between rows and says how many it
//   did not reach, still adding up.

#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/copy_beat_grids.hpp"

using namespace seabass;
using namespace seabass::application;
using Outcome = BeatGridWriteResult::Outcome;

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

domain::Track track(const std::string &format, const std::string &id, const std::string &relative)
{
    domain::Track t;
    t.format = format;
    t.sourceId = id;
    t.title = "Title " + relative;
    t.artist = "Artist";
    t.filename = relative.substr(relative.rfind('/') + 1);
    t.filePath = "/stick/" + relative;
    t.durationSeconds = 200.0;
    return t;
}

struct Call
{
    std::string engineTrackId;
    std::size_t beats = 0;
    BeatGridWriteOptions options;
};

// Answers each row with the outcome set for it; Written by default.
class FakeWriter : public BeatGridWriter
{
public:
    std::map<std::string, Outcome> outcomes;
    std::map<std::string, std::string> throwing;
    std::vector<Call> calls;
    std::function<void()> afterCall;

    BeatGridWriteResult writeBeatGrid(const std::string &id, const std::vector<domain::Beat> &beats,
                                      const BeatGridWriteOptions &options) override
    {
        calls.push_back({id, beats.size(), options});
        if (afterCall) {
            afterCall();
        }
        if (const auto t = throwing.find(id); t != throwing.end()) {
            throw std::runtime_error(t->second);
        }
        BeatGridWriteResult result;
        const auto o = outcomes.find(id);
        result.outcome = o == outcomes.end() ? Outcome::Written : o->second;
        // The writer's NoSampleRate turns into a write once it has the facts.
        if (result.outcome == Outcome::NoSampleRate && options.fileSampleRateHz && options.fileDurationMs) {
            result.outcome = Outcome::Written;
        }
        if (result.outcome == Outcome::Written) {
            result.barBreaks = 2;
        }
        if (result.outcome == Outcome::Written || result.outcome == Outcome::AlreadySame) {
            result.gridFingerprint = std::string(64, id.back());
        }
        return result;
    }
};

std::vector<domain::Beat> twoBeats(const BeatGridCopyItem &)
{
    return {{100.0, 1}, {600.0, 2}};
}

}  // namespace

int main()
{
    // 1. The plan.
    {
        std::vector<domain::Track> rekordbox = {
            track("rekordbox", "r1", "Contents/A/one.mp3"),
            track("rekordbox", "r2", "Contents/A/two.mp3"),
            track("rekordbox", "r3", "Contents/B/only-rekordbox.mp3"),
        };
        std::vector<domain::Track> engine = {
            track("engine", "e1", "Contents/A/one.mp3"),
            track("engine", "e2", "Contents/A/two.mp3"),
            track("engine", "e9", "Contents/C/only-engine.mp3"),
        };
        const BeatGridCopyPlan plan = planBeatGridCopy(rekordbox, engine, "/stick");
        check(plan.items.size() == 2 && plan.rekordboxOnly == 1 && plan.ambiguous == 0,
              "two pairs and one rekordbox-only track, got " + std::to_string(plan.items.size()) + "/"
                  + std::to_string(plan.rekordboxOnly) + "/" + std::to_string(plan.ambiguous));
        if (plan.items.size() == 2) {
            const BeatGridCopyItem &first = plan.items[0];
            check(first.rekordboxTrackId == "r1" && first.engineTrackId == "e1", "r1 pairs with e1");
            check(first.stickRelativePath == "Contents/A/one.mp3", "keyed by its path from the stick root");
            check(first.ledgerKey == normalizedPathKey("Contents/A/one.mp3"), "through normalizedPathKey");
            check(first.durationMs == 200000.0 && first.filePath == "/stick/Contents/A/one.mp3",
                  "carrying rekordbox's file and length");
            check(first.title == "Title Contents/A/one.mp3" && first.artist == "Artist", "and its title and artist");
        }

        // Two rekordbox rows of one file: both match the one Engine row.
        rekordbox.push_back(track("rekordbox", "r4", "Contents/A/one.mp3"));
        const BeatGridCopyPlan twice = planBeatGridCopy(rekordbox, engine, "/stick");
        check(twice.items.size() == 1 && twice.ambiguous == 2 && twice.rekordboxOnly == 1,
              "an Engine row two rekordbox rows match is left out, both counted");
        check(static_cast<int>(twice.items.size()) + twice.rekordboxOnly + twice.ambiguous
                  == static_cast<int>(rekordbox.size()),
              "every rekordbox track counted once");

        // A file outside the stick root has no key.
        const BeatGridCopyPlan outside = planBeatGridCopy({track("rekordbox", "r1", "x.mp3")}, {track("engine", "e1", "x.mp3")},
                                                          "/elsewhere");
        check(outside.items.size() == 1 && outside.items[0].ledgerKey.empty() && outside.items[0].stickRelativePath.empty(),
              "a file not under the stick root has no ledger key");
        std::cout << "case 1 (the plan) done\n";
    }

    // 1b. --track: title or "artist - title", case ignored.
    {
        const auto item = [](const std::string &id, const std::string &artist, const std::string &title) {
            BeatGridCopyItem i;
            i.engineTrackId = id;
            i.artist = artist;
            i.title = title;
            return i;
        };
        const BeatGridCopyPlan all{{item("e1", "Ahmed Spins", "Moroccan Dream"), item("e2", "Röyksopp", "Eple"),
                                    item("e3", "Other", "Ahmed's Tune"), item("e4", "Someone", "Else")}};
        const auto ids = [](const BeatGridCopyPlan &p) {
            std::string out;
            for (const auto &i : p.items) {
                out += i.engineTrackId + " ";
            }
            return out;
        };
        BeatGridCopyPlan plan = all;
        check(keepBeatGridItemsMatching(plan, "ahmed") == 2 && ids(plan) == "e1 e3 ",
              "\"ahmed\" keeps the artist's track and the title's, got " + ids(plan));
        plan = all;
        check(keepBeatGridItemsMatching(plan, "spins - moroccan") == 3 && ids(plan) == "e1 ",
              "\"artist - title\" is searched as one, got " + ids(plan));
        plan = all;
        check(keepBeatGridItemsMatching(plan, "RÖYKSOPP") == 3 && ids(plan) == "e2 ",
              "case ignored beyond ASCII, got " + ids(plan));
        plan = all;
        check(keepBeatGridItemsMatching(plan, "") == 0 && plan.items.size() == 4, "an empty filter keeps every item");
        plan = all;
        check(keepBeatGridItemsMatching(plan, "nothing like it") == 4 && plan.items.empty(),
              "a filter matching nothing keeps none");
    }

    // 2. Every outcome in its bucket, the ledger in and out.
    {
        BeatGridCopyPlan plan;
        const std::vector<std::string> ids = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b"};
        for (const std::string &id : ids) {
            BeatGridCopyItem item;
            item.rekordboxTrackId = "r" + id;
            item.engineTrackId = id;
            item.stickRelativePath = "Contents/" + id + ".mp3";
            item.ledgerKey = normalizedPathKey(item.stickRelativePath);
            plan.items.push_back(item);
        }
        plan.items.back().ledgerKey.clear();  // "b": written, but no key

        FakeWriter writer;
        writer.outcomes = {{"1", Outcome::Written},        {"2", Outcome::AlreadySame},
                           {"3", Outcome::NoSourceGrid},   {"4", Outcome::RowNotAnalysed},
                           {"5", Outcome::RowLocked},      {"6", Outcome::RowEditedOnEngine},
                           {"7", Outcome::NoSampleRate},   {"8", Outcome::UnsupportedSchema},
                           {"a", Outcome::Written},        {"b", Outcome::Written}};
        writer.throwing = {{"9", "the row is gone"}};

        std::map<std::string, std::string> ledger = {
            {normalizedPathKey("Contents/6.mp3"), std::string(64, 'f')},
            {normalizedPathKey("Contents/elsewhere.mp3"), std::string(64, 'e')},
        };
        int sourceCalls = 0;
        const BeatGridSource source = [&](const BeatGridCopyItem &item) {
            ++sourceCalls;
            if (item.engineTrackId == "a") {
                throw std::runtime_error("no analysis file");
            }
            return twoBeats(item);
        };
        const BeatGridCopyResult result = copyBeatGrids(plan, source, writer, ledger, {});
        check(result.written == 2, "two written (1 and b), got " + std::to_string(result.written));
        check(result.alreadySame == 1 && result.noRekordboxGrid == 1 && result.notAnalysed == 1
                  && result.lockedOnEngine == 1 && result.editedOnEngine == 1 && result.noSampleRate == 1
                  && result.unsupportedLibrary == 1,
              "one in each refusal's bucket");
        check(result.failed == 2 && result.failures.size() == 2, "a throwing writer and a throwing source fail");
        if (result.failures.size() == 2) {
            check(result.failures[0].item.engineTrackId == "9" && result.failures[0].message == "the row is gone",
                  "the writer's message kept");
            check(result.failures[1].item.engineTrackId == "a" && result.failures[1].message == "no analysis file",
                  "the source's message kept");
        }
        check(result.notReached == 0 && !result.cancelled, "nothing left unreached");
        check(result.total() == static_cast<int>(plan.items.size()), "the buckets add up to the plan");
        check(result.barBreaks == 4, "bar breaks over the rows written");
        check(sourceCalls == static_cast<int>(plan.items.size()), "the source asked once per row");
        check(writer.calls.size() == plan.items.size() - 1, "the writer once per row the source answered");

        check(result.ledgerEntries.size() == 2, "ledger entries for Written and AlreadySame only, got "
                                                    + std::to_string(result.ledgerEntries.size()));
        check(result.ledgerEntries.count(normalizedPathKey("Contents/1.mp3")) == 1
                  && result.ledgerEntries.at(normalizedPathKey("Contents/1.mp3")) == std::string(64, '1'),
              "the written row's fingerprint");
        check(result.ledgerEntries.count(normalizedPathKey("Contents/2.mp3")) == 1, "and the one already the same");
        check(result.notRecorded == 1, "the written row with no key is counted as not recorded");

        int withFingerprint = 0;
        for (const Call &call : writer.calls) {
            if (call.options.ownGridFingerprint) {
                ++withFingerprint;
                check(call.engineTrackId == "6" && *call.options.ownGridFingerprint == std::string(64, 'f'),
                      "the ledger's fingerprint goes to its own track, row " + call.engineTrackId);
            }
            check(call.options.unanalysed == UnanalysedRowGrid::Skip && !call.options.overwriteEngineEdits,
                  "the default options passed on");
            check(!call.options.fileSampleRateHz, "no file facts asked for in Skip");
        }
        check(withFingerprint == 1, "one row had a fingerprint to hand on");
        std::cout << "case 2 (outcomes, ledger) done\n";
    }

    // 3. Fill: the file's facts only for the row that needs them.
    {
        BeatGridCopyPlan plan;
        for (const std::string id : {"1", "7", "8"}) {
            BeatGridCopyItem item;
            item.engineTrackId = id;
            item.ledgerKey = "contents/" + id;
            item.durationMs = 1234.0;
            plan.items.push_back(item);
        }
        FakeWriter writer;
        writer.outcomes = {{"7", Outcome::NoSampleRate}, {"8", Outcome::NoSampleRate}};
        std::vector<std::string> asked;
        const BeatGridFileFactsSource facts = [&](const BeatGridCopyItem &item) {
            asked.push_back(item.engineTrackId);
            BeatGridFileFacts f;
            if (item.engineTrackId == "7") {
                f.sampleRateHz = 48000.0;
                f.durationMs = item.durationMs;
            }
            return f;
        };
        BeatGridCopyOptions options;
        options.unanalysed = UnanalysedRowGrid::FillAndLock;
        options.overwriteEngineEdits = true;
        const BeatGridCopyResult result = copyBeatGrids(plan, twoBeats, writer, {}, options, facts);
        check(asked == std::vector<std::string>{"7", "8"}, "facts asked only for the rows that lacked them");
        check(result.written == 2 && result.noSampleRate == 1, "7 written with its facts, 8 still without");
        check(writer.calls.size() == 4, "7 tried again with its facts, 8 not: 4 calls, got "
                                            + std::to_string(writer.calls.size()));
        bool sawFacts = false;
        for (const Call &call : writer.calls) {
            check(call.options.unanalysed == UnanalysedRowGrid::FillAndLock && call.options.overwriteEngineEdits,
                  "the options passed on");
            if (call.engineTrackId == "7" && call.options.fileSampleRateHz) {
                sawFacts = *call.options.fileSampleRateHz == 48000.0 && call.options.fileDurationMs == 1234.0;
            }
        }
        check(sawFacts, "7's second try carries the file's rate and length");
        check(result.total() == 3, "and it all adds up");
        std::cout << "case 3 (fill facts) done\n";
    }

    // 4. Cancelled after the second row.
    {
        BeatGridCopyPlan plan;
        for (int i = 1; i <= 5; ++i) {
            BeatGridCopyItem item;
            item.engineTrackId = std::to_string(i);
            item.ledgerKey = "contents/" + item.engineTrackId;
            plan.items.push_back(item);
        }
        FakeWriter writer;
        CancellationToken cancel;
        writer.afterCall = [&] {
            if (writer.calls.size() == 2) {
                cancel.cancel();
            }
        };
        const BeatGridCopyResult result = copyBeatGrids(plan, twoBeats, writer, {}, {}, {}, cancel);
        check(result.cancelled, "the run says it was cancelled");
        check(writer.calls.size() == 2, "and stopped after the row in flight");
        check(result.written == 2 && result.notReached == 3, "two written, three not reached");
        check(result.total() == 5, "still adding up to the plan");
        check(result.ledgerEntries.size() == 2, "only the rows written are recorded");
        std::cout << "case 4 (cancellation) done\n";
    }

    // 5. A preview: every row asked as a preview, the rows a run would
    // write counted apart from written work and listed, nothing for the
    // ledger, and the buckets still adding up.
    {
        BeatGridCopyPlan plan;
        for (const char *id : {"1", "2", "3", "4"}) {
            BeatGridCopyItem item;
            item.engineTrackId = id;
            item.ledgerKey = std::string("contents/") + id + ".mp3";
            item.title = std::string("Title ") + id;
            plan.items.push_back(item);
        }
        FakeWriter writer;
        writer.outcomes["2"] = Outcome::AlreadySame;
        writer.outcomes["3"] = Outcome::RowLocked;
        BeatGridCopyOptions options;
        options.previewOnly = true;
        const BeatGridCopyResult result = copyBeatGrids(plan, twoBeats, writer, {}, options);
        bool allPreviews = writer.calls.size() == 4;
        for (const Call &call : writer.calls) {
            allPreviews = allPreviews && call.options.previewOnly;
        }
        check(allPreviews, "every row is asked as a preview");
        check(result.written == 0, "a preview writes nothing, and says so: written is " + std::to_string(result.written));
        check(result.wouldWrite == 2 && result.alreadySame == 1 && result.lockedOnEngine == 1,
              "the rows a run would write are their own bucket");
        check(result.wouldWriteItems.size() == 2 && result.wouldWriteItems[0].engineTrackId == "1"
                  && result.wouldWriteItems[1].engineTrackId == "4",
              "and listed, in plan order");
        check(result.ledgerEntries.empty() && result.notRecorded == 0, "nothing is recorded for the ledger");
        check(result.total() == 4, "the buckets add up to the plan");
        std::cout << "case 5 (preview) done\n";
    }

    if (failures == 0) {
        std::cout << "copy_beat_grids_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
