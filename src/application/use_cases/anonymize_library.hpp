#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "application/ports/progress_reporter.hpp"

namespace seabass::application
{

struct AnonymizationOptions
{
    // Unset (default) keeps every real track; set to cap the output at
    // this many, dropping the rest -- see BRAINSTORM.md's "For
    // Developers, Maintainers and Testing" section and the plan this
    // implements for why this is an always-available user choice
    // rather than an on-by-default sampling step.
    std::optional<size_t> maxTracks;

    // Strips the export down to what a test suite needs: the three
    // catalogs and the cues, and none of the binary bulk.
    //
    //   - analysis files no kept track points at are removed rather than
    //     scrubbed and shipped (20.7 MB across 5976 files on a real
    //     stick, against 2 MB for all three catalogs)
    //   - Engine's overviewWaveFormData column is emptied, and its
    //     OverviewData directory of .rgb previews removed
    //   - m.db is vacuumed, so the pages freed by the two above are
    //     actually given back rather than left as slack
    //
    // What it never touches is the cue data. Engine keeps its cues in
    // PerformanceData.quickCues and .loops, right beside the waveform
    // column, and rekordbox keeps its in the analysis files rather than
    // in export.pdb -- so an export slimmed any harder than this could
    // not exercise a cue write at all, which is most of what the suite
    // is for.
    //
    // Off by default: a library someone submits is more useful whole,
    // and the unreferenced analysis files are a real on-stick condition
    // the cleanup features exist to find.
    bool slimForTesting = false;

    // Free text captured verbatim into MANIFEST.txt (and shown back to
    // the caller before anything is sent anywhere) -- what hardware the
    // submitter uses, and anything they'd like tested. Not validated or
    // interpreted; the caller is responsible for warning that this text
    // may be published alongside the rest of the export.
    std::string hardware;
    std::string notes;
};

struct AnonymizationSummary
{
    // Audio files listed in files.tsv. The files themselves are never
    // copied: they are gigabytes, and they are not the submitter's to
    // distribute. The listing is what lets the unreferenced-file and
    // orphan-cleanup paths run against shared data at all.
    int audioFilesListed = 0;
    bool rekordboxAttempted = false;
    int rekordboxTracksKept = 0;
    int rekordboxTracksDropped = 0;
    int rekordboxArtistsRenamed = 0;
    int rekordboxPlaylistsRenamed = 0;
    std::string rekordboxError;  // empty on success

    bool engineAttempted = false;
    int engineTracksKept = 0;
    int engineTracksDropped = 0;
    int enginePlaylistsRenamed = 0;
    // Tracks the Engine anonymizer could not touch: their real metadata is
    // still in the export, so it must not be shared. Surfaced in
    // MANIFEST.txt and by the CLI.
    int engineTracksRefused = 0;
    std::string engineFirstRefusalReason;
    std::string engineError;  // empty on success

    // Only valid while execute() is still running -- the staging
    // directory this pointed to is removed once the zip below is
    // written. Callers should use outputZipPath/manifestText instead;
    // kept only for code inside this use case that needs it mid-run.
    std::string manifestPath;
    std::uintmax_t outputSizeBytes = 0;  // raw, uncompressed, before zipping
    int filesWritten = 0;                // files in the export, before zipping
    // A ratio-based estimate (real compression measurements against
    // actual rekordbox/Engine files, see estimateZippedBytes() in the
    // .cpp) baked into MANIFEST.txt's own text -- written *before* the
    // real zip exists (the manifest is itself one of the zipped files,
    // so it can't know its own archive's final exact size). Prefer
    // finalZipBytes below for anything reported after execute() returns.

    // The single .zip file this run actually produced -- everything
    // execute() wrote ends up in here; no loose directory is left
    // behind. Empty if execute() never got far enough to zip anything.
    std::string outputZipPath;
    std::uintmax_t finalZipBytes = 0;  // real fs::file_size(outputZipPath), not an estimate

    // Full contents of MANIFEST.txt, captured before the staging
    // directory (and the standalone copy of this file in it) is
    // removed -- callers needing to show or print this shouldn't read
    // it back off disk themselves; it's already inside outputZipPath by
    // the time execute() returns.
    std::string manifestText;

    // True if at least one catalog was attempted and none of the
    // attempted ones failed.
    // Set when the finished export failed its own verification (see
    // infrastructure/anonymization_verifier.hpp). No zip was written and
    // the staging directory was removed: there is nothing to share, which
    // is the point.
    bool verificationFailed = false;
    std::string verificationReport;
    // Set when the output location was refused before anything was
    // written: it already existed with content, or it lies inside a
    // library being read. Nothing was created or removed.
    std::string outputError;

    bool succeeded() const;
};

// Produces a de-identified, structurally-real copy of one or both real
// libraries, zipped into a single outputZipPath (see
// AnonymizationSummary above) -- see
// infrastructure::rekordbox::anonymizeRekordboxLibrary and
// infrastructure::engine::anonymizeEngineLibrary for what each edit
// actually does to its catalog. Shared by both the maintainer fixture-
// regeneration workflow and the user-facing "export anonymized library"
// CLI command -- see seabass-cli's own `anonymize` subcommand.
//
// outputDir is used as a staging directory during the run (removed
// once the zip is written) -- the actual deliverable is
// outputDir + ".zip" alongside it, not outputDir itself.
class AnonymizeLibrary
{
public:
    AnonymizationSummary execute(const std::optional<std::string> &rekordboxRoot,
                                  const std::optional<std::string> &engineRoot, const std::string &outputDir,
                                  const AnonymizationOptions &options,
                                  ProgressReporter &reporter = NullProgressReporter::instance());
};

}  // namespace seabass::application
