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
// - the whole analysis (copyRekordboxAnalysis): the plan kept to a set of
//   rekordbox ids; every waveform outcome in its own bucket and every grid
//   outcome in its, both adding up; completed rows and the overview files
//   counted; the file's facts asked only for a row without a length; a
//   preview's counts kept apart from work done; and a writer that loses
//   an item in either part is caught.

#include <functional>
#include <iostream>
#include <map>
#include <set>
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

// Answers importAnalysis() with the outcomes set for each row.
class FakeAnalysisWriter : public AnalysisWriter
{
public:
    struct Answer
    {
        Outcome grid = Outcome::Written;
        AnalysisImportResult::Waveform waveform = AnalysisImportResult::Waveform::Written;
        bool completes = false;
        // Answered only once the file's rate is given; NoSampleCount until.
        bool needsRate = false;
    };
    std::map<std::string, Answer> answers;
    std::map<std::string, std::string> throwing;
    std::vector<std::pair<std::string, AnalysisImportOptions>> calls;

    AnalysisImportResult importAnalysis(const std::string &id, const RekordboxAnalysis &,
                                        const AnalysisImportOptions &options) override
    {
        calls.push_back({id, options});
        if (const auto t = throwing.find(id); t != throwing.end()) {
            throw std::runtime_error(t->second);
        }
        const Answer answer = answers.contains(id) ? answers.at(id) : Answer{};
        AnalysisImportResult result;
        if (answer.needsRate && !options.fileSampleRateHz) {
            result.grid.outcome = Outcome::RowNotAnalysed;
            result.waveform = AnalysisImportResult::Waveform::NoSampleCount;
            return result;
        }
        result.grid.outcome = options.grids ? answer.grid : Outcome::NotRequested;
        result.waveform = options.waveforms ? answer.waveform : AnalysisImportResult::Waveform::NotRequested;
        result.completedRow = answer.completes;
        if (result.grid.outcome == Outcome::Written || result.grid.outcome == Outcome::AlreadySame) {
            result.grid.gridFingerprint = std::string(64, id.back());
        }
        if (result.waveform == AnalysisImportResult::Waveform::Written) {
            result.overviewFileRelativePath = "Database2/OverviewData/u/" + id + ".rgb";
        }
        return result;
    }

    // The ways back, answered per row; NothingKept and NotAnalysed when
    // not set.
    std::map<std::string, EngineAnalysisRestoreResult::Outcome> restoreAnswers;
    std::map<std::string, LetPlayerAnalyseResult::Outcome> markAnswers;
    std::vector<std::pair<std::string, EngineAnalysisRestoreOptions>> restoreCalls;

    EngineAnalysisRestoreResult restoreEngineAnalysis(const std::string &id, const std::string &,
                                                      const EngineAnalysisRestoreOptions &options) override
    {
        restoreCalls.push_back({id, options});
        if (const auto t = throwing.find(id); t != throwing.end()) {
            throw std::runtime_error(t->second);
        }
        EngineAnalysisRestoreResult result;
        result.outcome = restoreAnswers.contains(id) ? restoreAnswers.at(id)
                                                     : EngineAnalysisRestoreResult::Outcome::NothingKept;
        if (result.outcome == EngineAnalysisRestoreResult::Outcome::ChangedSince) {
            result.reason = "changed on the player";
        }
        if (result.outcome == EngineAnalysisRestoreResult::Outcome::Restored) {
            result.overviewFileRelativePath = "Database2/OverviewData/u/" + id + ".rgb";
        }
        return result;
    }
    LetPlayerAnalyseResult letPlayerAnalyseAgain(const std::string &id, const std::string &, bool) override
    {
        if (const auto t = throwing.find(id); t != throwing.end()) {
            throw std::runtime_error(t->second);
        }
        LetPlayerAnalyseResult result;
        result.outcome = markAnswers.contains(id) ? markAnswers.at(id) : LetPlayerAnalyseResult::Outcome::NotAnalysed;
        return result;
    }
    EngineAnalysisFacts engineAnalysisFacts(const std::string &, const std::string &) override { return {}; }
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

    // 6. The plan kept to a set of rekordbox ids.
    {
        BeatGridCopyPlan plan;
        for (const char *id : {"1", "2", "3"}) {
            BeatGridCopyItem item;
            item.rekordboxTrackId = std::string("r") + id;
            item.engineTrackId = id;
            plan.items.push_back(item);
        }
        BeatGridCopyPlan kept = plan;
        check(keepBeatGridItemsWithIds(kept, {"r1", "r3", "r9"}) == 1 && kept.items.size() == 2
                  && kept.items[0].engineTrackId == "1" && kept.items[1].engineTrackId == "3",
              "the ids named are kept, in plan order");
        kept = plan;
        check(keepBeatGridItemsWithIds(kept, {}) == 3 && kept.items.empty(), "no ids keep nothing");
        std::cout << "case 6 (selection by ids) done\n";
    }

    // 7. The whole analysis: every outcome of each part in its bucket.
    {
        using Waveform = AnalysisImportResult::Waveform;
        using Answer = FakeAnalysisWriter::Answer;
        BeatGridCopyPlan plan;
        for (const std::string id : {"1", "2", "3", "4", "5", "6", "7", "8", "9"}) {
            BeatGridCopyItem item;
            item.rekordboxTrackId = "r" + id;
            item.engineTrackId = id;
            item.ledgerKey = "contents/" + id;
            item.filePath = "/stick/Contents/" + id + (id == "7" ? ".MP3" : ".flac");
            plan.items.push_back(item);
        }
        FakeAnalysisWriter writer;
        writer.answers = {
            {"1", {Outcome::Written, Waveform::Written, false, false}},
            {"2", {Outcome::AlreadySame, Waveform::AlreadySame, false, false}},
            {"3", {Outcome::RowLocked, Waveform::Written, false, false}},
            {"4", {Outcome::Written, Waveform::NoSourceWaveform, false, false}},
            {"5", {Outcome::NoSourceGrid, Waveform::RowNotAnalysed, false, false}},
            {"6", {Outcome::UnsupportedSchema, Waveform::UnsupportedSchema, false, false}},
            {"7", {Outcome::Written, Waveform::Written, true, true}},
            {"8", {Outcome::Written, Waveform::Written, false, true}},
        };
        writer.throwing = {{"9", "the row is gone"}};
        std::vector<std::string> asked;
        const AnalysisFileFactsSource facts = [&](const BeatGridCopyItem &item) {
            asked.push_back(item.engineTrackId);
            AnalysisFileFacts f;
            if (item.engineTrackId == "7") {
                f.sampleRateHz = 44100.0;
                f.isMp3 = true;
            }
            return f;
        };
        const AnalysisSource source = [](const BeatGridCopyItem &) { return RekordboxAnalysis{}; };
        AnalysisCopyOptions options;
        options.skipOverviewFile = true;
        options.zeroLoudness = true;
        const AnalysisCopyResult result = copyRekordboxAnalysis(plan, source, writer, {}, options, facts);
        const BeatGridCopyResult &g = result.grids;
        const WaveformCopyCounts &w = result.waveforms;
        check(g.written == 3 && g.alreadySame == 1 && g.lockedOnEngine == 1 && g.noRekordboxGrid == 1
                  && g.unsupportedLibrary == 1 && g.notAnalysed == 1 && g.failed == 1 && g.total() == 9,
              "every grid outcome in its bucket: written " + std::to_string(g.written) + ", not analysed "
                  + std::to_string(g.notAnalysed));
        check(w.written == 3 && w.alreadySame == 1 && w.noRekordboxWaveform == 1 && w.notAnalysed == 1
                  && w.unsupportedLibrary == 1 && w.noSampleCount == 1 && w.failed == 1 && w.total() == 9,
              "every waveform outcome in its bucket: written " + std::to_string(w.written) + ", no length "
                  + std::to_string(w.noSampleCount));
        check(w.wouldWrite == 0 && g.wouldWrite == 0 && result.wouldCompleteRows == 0, "nothing counted as a preview");
        check(result.completedRows == 1, "one row completed (7, once its rate was known)");
        check(result.rowsWritten == 4, "four rows with something written (1, 3, 4 and 7), got "
                                           + std::to_string(result.rowsWritten));
        check((asked == std::vector<std::string>{"7", "8"}), "the file's facts asked only for the rows without a length");
        check(writer.calls.size() == 10, "7 asked again with its rate, 8 not: 10 calls, got "
                                             + std::to_string(writer.calls.size()));
        bool sawRate = false;
        for (const auto &[id, call] : writer.calls) {
            check(call.grids && call.waveforms && call.skipOverviewFile && call.zeroLoudness && !call.previewOnly,
                  "the options passed on to row " + id);
            if (id == "7" && call.fileSampleRateHz) {
                sawRate = *call.fileSampleRateHz == 44100.0 && call.fileIsMp3;
            }
        }
        check(sawRate, "7's second try carries the file's rate and that it is an MP3");
        check((result.overviewFiles
               == std::vector<std::string>{"Database2/OverviewData/u/1.rgb", "Database2/OverviewData/u/3.rgb",
                                           "Database2/OverviewData/u/7.rgb"}),
              "the overview files written, in plan order");
        check(g.ledgerEntries.size() == 4 && !g.ledgerEntries.contains("contents/3"),
              "ledger entries for the grids written or already there");
        check(g.failures.size() == 1 && g.failures[0].message == "the row is gone", "the failure with its message");

        // Waveforms only: every grid is NotRequested, still adding up.
        AnalysisCopyOptions waveformsOnly;
        waveformsOnly.grids = false;
        FakeAnalysisWriter second;
        const AnalysisCopyResult onlyWaveforms = copyRekordboxAnalysis(plan, source, second, {}, waveformsOnly);
        check(onlyWaveforms.grids.notRequested == 9 && onlyWaveforms.grids.total() == 9
                  && onlyWaveforms.grids.ledgerEntries.empty(),
              "grids not asked for are their own bucket, and nothing for the ledger");
        std::cout << "case 7 (whole analysis) done\n";
    }

    // 8. A preview of the whole analysis, and a cancelled run.
    {
        BeatGridCopyPlan plan;
        for (const char *id : {"1", "2", "3", "4"}) {
            BeatGridCopyItem item;
            item.engineTrackId = id;
            item.ledgerKey = std::string("contents/") + id;
            plan.items.push_back(item);
        }
        FakeAnalysisWriter writer;
        writer.answers["2"] = {Outcome::Written, AnalysisImportResult::Waveform::Written, true, false};
        writer.answers["3"] = {Outcome::AlreadySame, AnalysisImportResult::Waveform::AlreadySame, false, false};
        const AnalysisSource source = [](const BeatGridCopyItem &) { return RekordboxAnalysis{}; };
        AnalysisCopyOptions options;
        options.previewOnly = true;
        const AnalysisCopyResult preview = copyRekordboxAnalysis(plan, source, writer, {}, options);
        check(preview.grids.written == 0 && preview.waveforms.written == 0 && preview.completedRows == 0
                  && preview.rowsWritten == 0,
              "a preview counts nothing as done");
        check(preview.grids.wouldWrite == 3 && preview.waveforms.wouldWrite == 3 && preview.wouldCompleteRows == 1
                  && preview.rowsWouldWrite == 3,
              "and what would be done in buckets of its own");
        check(preview.overviewFiles.size() == 3, "with the overview files that would be written");
        check(preview.grids.ledgerEntries.empty(), "nothing for the ledger");
        bool allPreviews = true;
        for (const auto &[id, call] : writer.calls) {
            allPreviews = allPreviews && call.previewOnly;
        }
        check(allPreviews, "every row asked as a preview");

        FakeAnalysisWriter cancelled;
        CancellationToken cancel;
        const AnalysisSource cancelling = [&](const BeatGridCopyItem &item) {
            if (item.engineTrackId == "2") {
                cancel.cancel();
            }
            return RekordboxAnalysis{};
        };
        const AnalysisCopyResult stopped = copyRekordboxAnalysis(plan, cancelling, cancelled, {}, {}, {}, cancel);
        check(stopped.grids.cancelled && stopped.grids.notReached == 2 && stopped.waveforms.notReached == 2,
              "cancelled after the row in flight: two not reached in each part");
        check(stopped.grids.total() == 4 && stopped.waveforms.total() == 4, "both parts still adding up");
        std::cout << "case 8 (analysis preview, cancelled) done\n";
    }

    // 9. A writer whose answer has no bucket is caught, not miscounted:
    // writeBeatGrid() answering NotRequested is a failure of that row.
    {
        BeatGridCopyPlan plan;
        BeatGridCopyItem item;
        item.engineTrackId = "1";
        plan.items.push_back(item);
        FakeWriter writer;
        writer.outcomes["1"] = Outcome::NotRequested;
        const BeatGridCopyResult result = copyBeatGrids(plan, twoBeats, writer, {}, {});
        check(result.failed == 1 && result.notRequested == 0 && result.total() == 1,
              "a grid write that says it was not asked for fails its row");
        std::cout << "case 9 (an answer with no bucket) done\n";
    }

    // 10. The ways back, counted exactly; each item's key handed on; the
    // ledger lines to drop are the rows whose analysis is the player's
    // again; a preview drops none.
    {
        using R = EngineAnalysisRestoreResult::Outcome;
        using M = LetPlayerAnalyseResult::Outcome;
        BeatGridCopyPlan plan;
        for (const char *id : {"1", "2", "3", "4", "5", "6", "7", "8"}) {
            BeatGridCopyItem item;
            item.engineTrackId = id;
            item.ledgerKey = std::string("k") + id;
            plan.items.push_back(item);
        }
        FakeAnalysisWriter writer;
        writer.restoreAnswers = {{"1", R::Restored},    {"2", R::Restored},   {"3", R::AlreadyEngines},
                                 {"4", R::ChangedSince}, {"5", R::KeptDamaged}, {"6", R::Stale}};
        writer.throwing["8"] = "the stick went away";
        const EngineAnalysisRestoreCounts done = restoreEngineAnalysis(plan, writer, {});
        check(done.restored == 2 && done.alreadyEngines == 1 && done.changedSince == 1 && done.keptDamaged == 1
                  && done.stale == 1 && done.nothingKept == 1 && done.failed == 1 && done.total() == 8,
              "every restore outcome in its bucket");
        check(done.ledgerKeysToDrop == std::vector<std::string>{"k1", "k2", "k3"}, "ledger lines of the rows put back");
        check(done.refusals.size() == 1 && done.refusals[0].item.engineTrackId == "4", "the refusal with its reason");
        check(done.overviewFiles.size() == 2, "the overview files a restore changes");
        const EngineAnalysisRestoreCounts preview = restoreEngineAnalysis(plan, writer, {false, true});
        check(preview.wouldRestore == 2 && preview.restored == 0 && preview.ledgerKeysToDrop.empty(),
              "a preview restores nothing and drops nothing");
        check(writer.restoreCalls.back().second.previewOnly, "the preview asked the writer for one");

        writer.markAnswers = {{"1", M::Marked}, {"2", M::Marked}, {"3", M::UnsupportedSchema}};
        const LetPlayerAnalyseCounts marked = letPlayerAnalyseAgain(plan, writer, false);
        check(marked.marked == 2 && marked.unsupportedLibrary == 1 && marked.notAnalysed == 4 && marked.failed == 1
                  && marked.total() == 8,
              "every mark outcome in its bucket");
        check(marked.ledgerKeysToDrop == std::vector<std::string>{"k1", "k2"}, "the marked rows' ledger lines");
        std::cout << "case 10 (restore and let the player analyse, counted) done\n";
    }

    // 11. The file's own count reaches the writer, and the copy's options
    // carry the item's key for keeping the player's analysis.
    {
        BeatGridCopyPlan plan;
        BeatGridCopyItem item;
        item.engineTrackId = "1";
        item.ledgerKey = "k1";
        item.filePath = "/stick/a.flac";
        plan.items.push_back(item);
        FakeAnalysisWriter writer;
        writer.answers["1"].needsRate = true;
        const AnalysisFileFactsSource facts = [](const BeatGridCopyItem &) {
            AnalysisFileFacts f;
            f.sampleRateHz = 48000.0;
            f.sampleCount = 11520000;
            f.countRequired = true;
            return f;
        };
        copyRekordboxAnalysis(plan, [](const BeatGridCopyItem &) { return RekordboxAnalysis{}; }, writer, {}, {}, facts);
        const AnalysisImportOptions &last = writer.calls.back().second;
        check(last.fileSampleCount == std::optional<std::uint64_t>(11520000) && last.fileCountRequired
                  && last.fileSampleRateHz == std::optional<double>(48000.0),
              "the file's count and rate are handed on");
        check(writer.calls.front().second.keepKey == "k1", "the key to keep by is the item's ledger key");
        check(requiresOwnSampleCount("/x/A.FLAC") && requiresOwnSampleCount("b.m4a") && !requiresOwnSampleCount("c.mp3")
                  && isMp3File("C.MP3") && !requiresOwnSampleCount("d.ogg"),
              "the kinds whose count must come from the file");
        std::cout << "case 11 (the file's own count) done\n";
    }

    if (failures == 0) {
        std::cout << "copy_beat_grids_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
