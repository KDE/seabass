// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/track_import_staging.hpp"

namespace seabass::gui
{

// What a person offered for import, and where it should go.
struct ImportRequest
{
    // Files and folders on the computer, absolute. A folder stands for
    // every audio file in it and below it, by path.
    std::vector<std::string> offered;
    // The stick, as a page knows it: its PIONEER folder and its Engine
    // Library folder; either may be empty.
    QString rekordboxPath;
    QString enginePath;
    // None: the tracks go into the libraries and into no playlist.
    std::optional<ImportPlaylist> playlist;
    // Source files the plan called a possible duplicate, to import all
    // the same.
    std::set<std::string> importAnyway;
    // Source files the person took back out of the batch one by one
    // ("Don't import"): not offered again when the batch is prepared
    // again (more files joined to it, its possible duplicates asked for),
    // a folder's among them.
    std::set<std::string> leftOutByPerson;
    // The day of the import, "YYYY-MM-DD".
    std::string date;
    // Reads a file's cover (ImportTrackChange::setCover). Unset: no
    // covers.
    CoverReader coverReader;
    // Analyses each file that is staged (ImportTrackChange::setAnalysis),
    // once everything else is settled. Unset: the
    // tracks go in unanalysed.
    ImportAnalyser analyser;
    // How many files are analysed at once. Each is read piece by piece
    // and never held whole: about 50 MB and 3.5 MB a minute of music while
    // it is analysed (analysis_resources.hpp), and the memory gate holds a
    // file back while the computer is short.
    int analysisJobs = 2;
};

struct PreparedImport
{
    // Why nothing can be imported onto this stick at all, in words for
    // the person. Empty otherwise.
    std::string refusal;
    // The changes of the save, and the files they carry.
    StagedImport staged;
    // Every offered file that is not staged, with its reason, in the
    // order offered.
    std::vector<LeftOut> notImported;
    // Those among them the plan called a possible duplicate: the person
    // may ask for them all the same (ImportRequest::importAnyway).
    std::vector<std::string> possibleDuplicates;
    std::uint64_t bytesToCopy = 0;
    // Of the staged tracks, how many have an analysis and how many a
    // beat grid in it.
    int analysed = 0;
    int withGrid = 0;
    // The staged files whose grid follows a moving tempo, by name: the
    // ones to look at.
    std::vector<std::string> followedGrids;
    // The cover of each staged track that has one, by its change's id:
    // the smallest of CoverPictures (80 pixels a side, JPEG), for the
    // track list to show the row before Save. Read with the request's
    // coverReader, as the save reads it.
    std::map<QString, std::string> covers;
};

// How far the preparing of an import is, for a bar that is not in the
// way: which step, and how many of its files are done. `total` 0 for a
// step that cannot count ahead.
struct ImportProgress
{
    enum class Step {
        // The stick's libraries and files are read.
        Stick,
        // The offered files are found and their tags read.
        Files,
        // The plan is made and rehearsed against the stick's libraries.
        Check,
        // The staged files are decoded and analysed: beat grid and
        // waveform.
        Analysis,
    };
    Step step = Step::Stick;
    std::size_t done = 0;
    std::size_t total = 0;
    // The file in hand, by its name; empty between files. In the
    // analysis, where several are in hand at once: the one last done.
    std::string file;
    // Every file in hand now, by name: one while files are read, as many
    // as there are workers in the analysis.
    std::vector<std::string> inHand;
    // In the analysis: the bytes of the files done and of them all, so
    // that an hour long mix weighs as much on the bar as it takes.
    std::uint64_t doneBytes = 0;
    std::uint64_t totalBytes = 0;
    // Where the whole preparing is on its one bar (ImportBar), in planned
    // units: never less than the report before, barTotal at the end of
    // the work. Both 0 until the work is planned.
    std::uint64_t barDone = 0;
    std::uint64_t barTotal = 0;
};

// Called on the thread that prepares, often, and never from two threads
// at once: a sink for another thread passes it on itself.
using ImportProgressSink = std::function<void(const ImportProgress &)>;

// One operation, one bar (application::PhasedProgress, #58), for the
// preparing of an import and of Analyse again: from the offer to what is
// staged, each step a stretch of the bar as long as the work planned for
// it, end to end. The steps' own counts start at 0 each; the bar does not
// (ImportProgress::barDone). The plan is made once the work can be counted
// (the stick read, the offered folders walked, the files to analyse
// known); until then the bar stays at its start. A stretch moves by its
// step's count (the files read), by bytes in the analysis, or not at all
// before it ends (reading the stick, the check); a later step reported
// means the ones before it are done. The bar never goes back, and only
// end() takes it to its end.
class ImportBar
{
public:
    // Planned units: about a millisecond of work each on a computer of
    // today. Reading a stick's libraries and its files takes seconds, a
    // file's tags a few milliseconds, the check half a second (more when
    // a possible duplicate's bytes are compared), and the analysis about
    // 150 ms for each MiB of audio.
    static constexpr std::uint64_t StickUnits = 2000;
    static constexpr std::uint64_t UnitsPerFileRead = 10;
    static constexpr std::uint64_t CheckUnits = 500;
    static constexpr std::uint64_t AnalysisUnitsPerMiB = 150;
    // The analysis of `bytes` of audio, at least a unit per file.
    static std::uint64_t analysisUnits(std::uint64_t bytes, std::size_t files);

    // Passes each report on to `sink`, the bar filled in.
    explicit ImportBar(ImportProgressSink sink);

    // The stretches in their order, each step with its planned units; a
    // step not named takes none. Made once: a second plan is ignored.
    void plan(const std::vector<std::pair<ImportProgress::Step, std::uint64_t>> &stretches);
    void report(ImportProgress at);
    // The work has ended: the bar at its end, reported as `step`.
    void end(ImportProgress::Step step);
    // A sink that reports through this, for analyseFiles(). It holds this
    // bar by reference.
    ImportProgressSink sink();

    std::uint64_t total() const { return m_total; }
    std::uint64_t reached() const { return m_reached; }

private:
    std::uint64_t positionOf(const ImportProgress &at) const;

    ImportProgressSink m_sink;
    std::vector<std::pair<ImportProgress::Step, std::uint64_t>> m_stretches;
    std::uint64_t m_total = 0;
    std::uint64_t m_reached = 0;
};

// Analyses each of `files` with `analyser`, `jobs` at once, each worker a
// thread of its own (the decoder runs an event loop in the thread that
// reads it), reporting Step::Analysis with every file in hand by name.
// One result per file, in order, null where the analyser made none. The
// first analyser that throws stops the rest from starting and is thrown
// again here; a cancel throws application::OperationCancelled. Shared by
// the import and Analyse again.
std::vector<std::shared_ptr<const ImportAnalysis>> analyseFiles(const std::vector<std::string> &files,
                                                                const ImportAnalyser &analyser, int jobs,
                                                                const application::CancellationToken &cancel,
                                                                const ImportProgressSink &progress);

// From what was dropped to the changes of one save: the offered folders
// are walked, each file's tags read, the stick's libraries and audio
// files read, the plan made (domain::planTrackImport) and its imports
// staged (stageTrackImport). Reads the computer and the stick and writes
// neither: not for the GUI thread.
//
// A stick with a library that is there and cannot be read takes no
// import: a track belongs in every library of a stick, and one that
// cannot be read cannot be written with any confidence either.
//
// Throws application::OperationCancelled on a cancel.
PreparedImport prepareTrackImport(const ImportRequest &request, const application::CancellationToken &cancel,
                                  const ImportProgressSink &progress = {});

}  // namespace seabass::gui
