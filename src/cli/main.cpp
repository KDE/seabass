#include "infrastructure/paths/seabass_paths.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/backup_store.hpp"
#include "application/ports/cue_writer.hpp"
#include "application/ports/library_reader.hpp"
#include "application/ports/operation_log.hpp"
#include "application/ports/removable_media_locator.hpp"
#include "application/use_cases/anonymize_library.hpp"
#include "application/use_cases/consolidate_duplicate_cues.hpp"
#include "application/use_cases/scan_library.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "cli/console.hpp"
#include "cli/terminal_progress_reporter.hpp"
#include "domain/fuzzy_matcher.hpp"
#include "domain/track_queries.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/local/file_library_edit_lock_store.hpp"
#include "infrastructure/logging/file_operation_log.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

#include "infrastructure/audio/duration_fill.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include <QCoreApplication>
#endif

using seabass::application::AnonymizeLibrary;
using seabass::application::AnonymizationOptions;
using seabass::application::BackupStore;
using seabass::application::ConsolidateDuplicateCues;
using seabass::application::CueWriter;
using seabass::application::DetectedStick;
using seabass::application::LibraryReader;
using seabass::application::OperationLog;
using seabass::application::RemovableMediaLocator;
using seabass::application::ScanLibrary;
using seabass::cli::Color;
using seabass::cli::Console;
using seabass::domain::ConsolidationPlan;
using seabass::domain::FuzzyMatcher;
using seabass::domain::Track;
using seabass::domain::TrackPrioritizer;

namespace fs = std::filesystem;

namespace
{

constexpr size_t DefaultKeepBackups = 10;
constexpr size_t DefaultNeedsCuesLimit = 20;

void printUsage()
{
    Console::heading("Seabass -- sync rekordbox and Denon Engine DJ libraries");
    Console::info("");
    Console::info("Seabass reads DJ track libraries off USB sticks prepared by rekordbox or");
    Console::info("Engine DJ (the format Denon's Prime-series gear uses), so cue points can");
    Console::info("eventually be kept in sync between the two. Right now, scanning (read-only");
    Console::info("reporting) and duplicate-track cue consolidation are implemented; syncing");
    Console::info("between the two formats is still in development.");
    Console::info("");
    Console::heading("Usage");
    Console::info("  seabass-cli scan [--rekordbox [PATH]] [--engine [PATH]] [--verbose] [--auto]");
    Console::info("                 [--track NAME] [--needs-cues [N]]");
    Console::info("  seabass-cli sync --rekordbox [PATH] --engine [PATH] [--dry-run] [--auto]");
    Console::info("  seabass-cli backups [--rekordbox [PATH]] [--engine [PATH]] [--clean] [--keep N]");
    Console::info("  seabass-cli anonymize [--rekordbox [PATH]] [--engine [PATH]] --out DIR");
    Console::info("                        [--max-tracks N] [--hardware TEXT] [--notes TEXT]");
    Console::info("  seabass-cli --help");
    Console::info("");
    Console::heading("Commands");
    Console::info("  scan     Reports every track and cue point found in a library, then checks");
    Console::info("           for duplicate copies of the same track with inconsistent cues (see");
    Console::info("           \"Duplicate-track cue consolidation\" below). Scanning itself never");
    Console::info("           modifies anything; consolidating duplicates does write, but only");
    Console::info("           after backing up the file(s) it's about to change, and only with");
    Console::info("           your confirmation unless --auto is given.");
    Console::info("  sync     Matches tracks between one rekordbox source and one Engine source by");
    Console::info("           filename and duration, and syncs their cues (see \"Sync\" below).");
    Console::info("           Two phases, always: first it analyzes both libraries and prints the");
    Console::info("           full proposal -- what would move where, and what can't be applied");
    Console::info("           yet -- without touching anything; only then does it ask you to");
    Console::info("           confirm (unless --auto or --dry-run) before writing anything.");
    Console::info("  backups  Lists the backups seabass-cli has made on a stick (see \"Backups\"");
    Console::info("           below). With --clean, deletes the oldest ones so at most --keep");
    Console::info("           N remain (default " + std::to_string(DefaultKeepBackups) +
                   "), freeing space on the stick.");
    Console::info("  anonymize  Writes a de-identified, structurally-real copy of one or both");
    Console::info("           libraries to --out DIR: every real track is kept by default (pass");
    Console::info("           --max-tracks to cap it), titles/artists/comments/filenames/playlist");
    Console::info("           names are replaced with placeholders, artwork and detailed color");
    Console::info("           waveform data are dropped, everything else (BPM/key/cues/ratings/");
    Console::info("           play counts/playlist structure) is kept as-is. Never sends anything");
    Console::info("           anywhere -- only ever writes to DIR. Useful for building a realistic,");
    Console::info("           privacy-safe test dataset, whether for this project's own test suite");
    Console::info("           or to send to its maintainer (see the notice this command prints).");
    Console::info("");
    Console::heading("Options");
    Console::info("  --rekordbox [PATH]  Scan a rekordbox USB export. PATH is the folder that");
    Console::info("                      contains \"rekordbox/\" and \"USBANLZ/\" (i.e. the");
    Console::info("                      \"PIONEER\" folder itself). If PATH is omitted, an");
    Console::info("                      inserted USB stick is auto-detected.");
    Console::info("  --engine [PATH]     Scan an Engine Library. PATH is the folder that");
    Console::info("                      contains \"Database2/\" (i.e. the \"Engine Library\"");
    Console::info("                      folder itself). If PATH is omitted, an inserted USB");
    Console::info("                      stick is auto-detected.");
    Console::info("  --auto              Skip the confirmation prompt (scan: for consolidating an");
    Console::info("                      unambiguous duplicate group; sync: for applying the");
    Console::info("                      analyzed plan). Never changes *what* gets decided --");
    Console::info("                      conflicts are still resolved the same way, and anything");
    Console::info("                      unwritable is still only reported, never guessed at.");
    Console::info("  --dry-run           sync only: run the analysis and print the full proposal,");
    Console::info("                      then stop -- never prompts, never writes.");
    Console::info("  --force             Write even while a Seabass window on this machine is");
    Console::info("                      editing the same library (its unsaved changes may then");
    Console::info("                      overwrite yours, or yours theirs). Off by default.");
    Console::info("  --track NAME        Show only tracks whose title, artist, or filename");
    Console::info("                      fuzzy-matches NAME (typo-tolerant, case-insensitive).");
    Console::info("                      Shows every match, including ones with no cues yet --");
    Console::info("                      unlike the default report, which only samples tracks");
    Console::info("                      that already have cues.");
    Console::info("  --needs-cues [N]    List cue-less tracks worth setting cues on, most");
    Console::info("                      \"worth it\" first: highest rekordbox play count, or");
    Console::info("                      most recently played on Engine (Engine has no play");
    Console::info("                      count, only a last-played timestamp). Defaults to the");
    Console::info("                      top " + std::to_string(DefaultNeedsCuesLimit) +
                   "; pass N to change that, or 0 for no limit.");
    Console::info("  --out DIR           anonymize only: where to write the anonymized library/ies");
    Console::info("                      and MANIFEST.txt. Required.");
    Console::info("  --slim              anonymize only: the three catalogs and the cues, without");
    Console::info("                      the binary bulk -- unreferenced analysis files, Engine's");
    Console::info("                      waveform blobs and .rgb previews all go. For building a");
    Console::info("                      test fixture; leave off for a library you are submitting.");
    Console::info("  --max-tracks N      anonymize only: include at most N real tracks (dropped");
    Console::info("                      ones are pruned, not just hidden). Omit to keep every");
    Console::info("                      real track -- see \"anonymize\" above for why that's the");
    Console::info("                      default rather than a sampled subset.");
    Console::info("  --hardware TEXT     anonymize only: what hardware you use, saved into");
    Console::info("                      MANIFEST.txt verbatim. Optional.");
    Console::info("  --notes TEXT        anonymize only: anything you'd like tested, saved into");
    Console::info("                      MANIFEST.txt verbatim. Optional.");
    Console::info("  --verbose           Print extra detail (resolved paths, per-item");
    Console::info("                      diagnostics). Default output is already");
    Console::info("                      informational, not silent -- this adds more on top.");
    Console::info("  --help, -h          Show this help and exit.");
    Console::info("");
    Console::heading("Auto-detection");
    Console::info("  Run \"seabass-cli scan\" with no --rekordbox/--engine PATH to auto-detect");
    Console::info("  inserted USB stick(s). A single stick often carries both a \"PIONEER\" folder");
    Console::info("  and an \"Engine Library\" folder side by side -- if so, both are scanned and");
    Console::info("  reported. Scanning is read-only, so if more than one stick is mounted, ALL of");
    Console::info("  them get scanned (each report labeled with the stick it came from); pass");
    Console::info("  --rekordbox/--engine with a PATH to scan just one specific stick instead.");
    Console::info("  \"seabass-cli backups\", which can delete old backups, is stricter: it always");
    Console::info("  requires a single unambiguous stick and will ask you to specify one.");
    Console::info("");
    Console::heading("Duplicate-track cue consolidation");
    Console::info("  A stick can hold the same track more than once (re-imports, duplicate");
    Console::info("  rows). If exactly one copy has hot cues and the others have none, Seabass");
    Console::info("  offers to copy those cues onto the cue-less copies -- this runs as the last");
    Console::info("  step of a scan, after all reporting is done, for both rekordbox and Engine.");
    Console::info("  If copies have cues that actually *differ*, that's reported as a conflict and");
    Console::info("  never touched -- you decide by hand which copy is right.");
    Console::info("");
    Console::heading("Sync");
    Console::info("  Matches tracks between a rekordbox source and an Engine source by filename +");
    Console::info("  duration, then for each matched pair: if only one side has cues, they'd move");
    Console::info("  to the other side; if both have the *same* cues, nothing happens; if both");
    Console::info("  have *different* cues, that's a conflict, resolved by which side's underlying");
    Console::info("  file was modified more recently (a heuristic, not a true edit timestamp --");
    Console::info("  documented as such, not treated as certain). Both directions can be written");
    Console::info("  now. Writing to rekordbox (rewriting the target track's ANLZ .EXT file) is the");
    Console::info("  least-proven part of Seabass -- validated by round-tripping real files and");
    Console::info("  cross-checking with an independent reader, but not yet confirmed against real");
    Console::info("  rekordbox software or real CDJ/XDJ hardware. Verify on your own gear before");
    Console::info("  trusting it for a gig. The full proposal is always printed before anything is");
    Console::info("  written -- --auto only skips the confirmation prompt afterwards, it never");
    Console::info("  skips the analysis step.");
    Console::info("");
    Console::heading("Backups");
    Console::info("  Every write Seabass makes (duplicate-cue consolidation, sync) backs up the");
    Console::info("  file(s) it's about to touch first, under");
    Console::info("  \"<stick root>/Seabass/backups/<timestamp>-<label>/\" -- shared across");
    Console::info("  rekordbox and Engine, since both live under the same stick root. Nothing");
    Console::info("  is ever deleted automatically; run \"seabass-cli backups --clean\" yourself to");
    Console::info("  prune old ones once they've accumulated (default: keep the " +
                   std::to_string(DefaultKeepBackups) + " most recent).");
    Console::info("  Every write is also appended to a plain-text log at");
    Console::info("  \"<stick root>/Seabass/seabass.log\" (what was copied, to/from which track id,");
    Console::info("  and which backup covers it) -- kept forever, since it's just text.");
    Console::info("");
    Console::heading("Examples");
    Console::info("  seabass-cli scan                              # auto-detect an inserted stick");
    Console::info("  seabass-cli scan --rekordbox                  # auto-detect, rekordbox side only");
    Console::info("  seabass-cli scan --rekordbox /media/me/NEXUS  # scan a specific rekordbox stick");
    Console::info("  seabass-cli scan --engine /media/me/NEXUS --verbose");
    Console::info("  seabass-cli scan --engine --auto              # also auto-fix unambiguous duplicates");
    Console::info("  seabass-cli backups                           # list backups on an inserted stick");
    Console::info("  seabass-cli backups --clean --keep 3          # keep only the 3 most recent");
    Console::info("  seabass-cli scan --engine --track concorde    # look up a track by (fuzzy) name");
    Console::info("  seabass-cli scan --rekordbox --needs-cues 10  # top 10 most-played tracks with no cues");
    Console::info("  seabass-cli sync --rekordbox --engine --dry-run  # see the proposal, change nothing");
    Console::info("  seabass-cli sync --rekordbox --engine            # analyze, then confirm before writing");
}

// Which tracks printReport shows detail for, beyond the always-printed
// summary counts. With neither set, it falls back to the original
// behavior: up to 5 tracks that have cues, as a representative sample.
struct ReportOptions
{
    std::optional<std::string> trackFilter;  // fuzzy match against title/artist/filename
    bool needsCues = false;
    size_t needsCuesLimit = 20;
};

constexpr double MinFuzzyMatchScore = 0.4;

std::string formatTimestamp(std::chrono::system_clock::time_point tp)
{
    return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::seconds>(tp));
}

void printTrackDetail(const Track &track)
{
    Console::info("");
    Console::info(Console::colorize(track.title, Color::Bold) + " (" + track.artist + ") [" +
                   std::to_string(static_cast<int>(track.durationSeconds)) + "s]");
    Console::verbose("  id=" + track.sourceId + " file=" + track.filename);
    if (track.playCount) {
        Console::info("  plays: " + std::to_string(*track.playCount));
    }
    if (track.lastPlayedAt) {
        Console::info("  last played: " + formatTimestamp(*track.lastPlayedAt));
    }
    for (const auto &playlist : track.playlists) {
        Console::info("  playlist: " + playlist.name + " (position " + std::to_string(playlist.position) + ")");
    }

    if (track.cues.empty()) {
        Console::info(Console::colorize("  no cues yet", Color::Gray));
        return;
    }
    for (const auto &cue : track.cues) {
        bool isHot = cue.kind == seabass::domain::CuePoint::Kind::Hot;
        std::string line = std::string("  ") + (isHot ? "hot" : "mem") + " " + std::to_string(cue.hotCueNumber) +
                            " @ " + std::to_string(static_cast<long>(cue.positionMs)) + "ms";
        if (cue.isLoop) {
            line += "-" + std::to_string(static_cast<long>(cue.loopEndMs)) + "ms (loop)";
        }
        Console::info(Console::colorize(line, isHot ? Color::Cyan : Color::Gray) + " " + cue.color +
                       (cue.comment.empty() ? "" : (" \"" + cue.comment + "\"")));
    }
}

void printReport(const std::string &heading, const std::vector<Track> &tracks, const ReportOptions &options)
{
    int tracksWithCues = 0;
    int totalCues = 0;
    for (const auto &track : tracks) {
        if (!track.cues.empty()) {
            tracksWithCues++;
            totalCues += static_cast<int>(track.cues.size());
        }
    }

    Console::info("");
    Console::heading(heading);
    Console::info("  tracks:           " + std::to_string(tracks.size()));
    Console::info("  tracks with cues: " + std::to_string(tracksWithCues));
    Console::info("  total cues:       " + std::to_string(totalCues));

    if (!options.trackFilter && !options.needsCues) {
        int shown = 0;
        for (const auto &track : tracks) {
            if (track.cues.empty() || shown >= 5) {
                continue;
            }
            shown++;
            printTrackDetail(track);
        }
        return;
    }

    std::vector<Track> candidates = tracks;

    if (options.trackFilter) {
        struct ScoredTrack
        {
            Track track;
            double score;
        };
        std::vector<ScoredTrack> scored;
        for (const auto &track : candidates) {
            double score = std::max({FuzzyMatcher::score(*options.trackFilter, track.title),
                                      FuzzyMatcher::score(*options.trackFilter, track.artist),
                                      FuzzyMatcher::score(*options.trackFilter, track.filename)});
            if (score >= MinFuzzyMatchScore) {
                scored.push_back({track, score});
            }
        }
        std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.score > b.score; });
        candidates.clear();
        for (auto &s : scored) {
            candidates.push_back(std::move(s.track));
        }
    }

    if (options.needsCues) {
        candidates = TrackPrioritizer::tracksNeedingCues(candidates, options.needsCuesLimit);
    }

    if (candidates.empty()) {
        Console::info("");
        Console::info(options.trackFilter ? "no tracks matching \"" + *options.trackFilter + "\""
                                           : "no tracks need cues");
        return;
    }

    for (const auto &track : candidates) {
        printTrackDetail(track);
    }
}

// Fills in the lengths neither catalog recorded, so duplicate detection
// has the one field that can tell a radio edit from an extended mix.
// Engine leaves `length` NULL on every track it has not analyzed (77.6%
// of rows on a real stick), and without a length DuplicateTrackFinder
// refuses to group at all -- see its header.
void fillDurations(std::vector<Track> &tracks, const std::string &libraryPath)
{
    const auto result = seabass::infrastructure::audio::fillTrackDurations(tracks, libraryPath);
    if (result.probed > 0 || result.fromCache > 0) {
        Console::verbose("durations: " + std::to_string(result.alreadyKnown) + " from catalog, " +
                          std::to_string(result.fromCache) + " cached, " + std::to_string(result.probed) +
                          " probed, " + std::to_string(result.unreadable) + " still unknown");
    }
}

std::vector<Track> scanPath(std::unique_ptr<LibraryReader> reader, const std::string &libraryPath)
{
    seabass::cli::TerminalProgressReporter progress;
    reader->setProgressReporter(progress);

    ScanLibrary useCase(*reader);
    auto tracks = useCase.execute();
    fillDurations(tracks, libraryPath);
    return tracks;
}

// Last step of a scan: find duplicate tracks and, where an unambiguous
// consolidation is possible and a CueWriter exists for this format, offer
// to apply it (backing up first). writer/backupStore/log may be null when
// the format has no write path yet -- such duplicates are still detected
// and reported, just never modified.
//
// filesToBackUpFor resolves which underlying file(s) a given track's data
// lives in -- for Engine that's always the same m.db regardless of track,
// but for rekordbox each track's cues live in its own ANLZ .EXT file, so
// the backup target genuinely depends on which track is being written.
void handleDuplicates(const std::string &formatName, const std::vector<Track> &tracks, CueWriter *writer,
                       BackupStore *backupStore, OperationLog *log,
                       const std::function<std::vector<std::string>(const std::string &)> &filesToBackUpFor,
                       bool autoMode)
{
    auto plans = ConsolidateDuplicateCues().execute(tracks);
    if (plans.empty()) {
        return;
    }

    bool printedHeading = false;
    std::set<std::string> backedUpFiles;
    size_t groupsConsolidated = 0;
    size_t totalCuesCopied = 0;
    size_t targetsWritten = 0;
    auto ensureHeading = [&]() {
        if (!printedHeading) {
            Console::info("");
            Console::heading(formatName + ": duplicate tracks");
            printedHeading = true;
        }
    };

    for (const auto &plan : plans) {
        if (plan.kind == ConsolidationPlan::Kind::NoCues || plan.kind == ConsolidationPlan::Kind::AlreadyConsistent) {
            continue;
        }

        ensureHeading();

        if (plan.kind == ConsolidationPlan::Kind::Conflict) {
            Console::warn("\"" + plan.group.tracks.front().filename +
                           "\" has duplicate copies with different cues -- not touching them:");
            for (const auto &t : plan.group.tracks) {
                Console::info("  id=" + t.sourceId + "  cues=" + std::to_string(t.cues.size()));
            }
            continue;
        }

        // Kind::Unambiguous
        Console::info("\"" + plan.source->filename + "\": " + std::to_string(plan.source->cues.size()) +
                       " cue(s) on one copy (id=" + plan.source->sourceId + "), missing on " +
                       std::to_string(plan.targets.size()) + " other copy/copies.");

        if (!writer) {
            Console::info("  " + formatName + " writing isn't supported yet -- cues not copied.");
            continue;
        }

        bool apply = autoMode || Console::confirm("  copy these cues onto the other copy/copies?");
        if (!apply) {
            Console::info("  skipped.");
            continue;
        }

        for (const auto &target : plan.targets) {
            if (backupStore) {
                auto files = filesToBackUpFor(target.sourceId);
                files.erase(std::remove_if(files.begin(), files.end(),
                                            [&](const std::string &f) { return backedUpFiles.contains(f); }),
                            files.end());
                if (!files.empty()) {
                    auto record = backupStore->backup(files, "duplicate-cue-consolidation");
                    Console::info("  backed up to " + record.path);
                    if (log) {
                        log->record(formatName + ": backed up before duplicate-cue consolidation -> " + record.path);
                    }
                    for (const auto &f : files) {
                        backedUpFiles.insert(f);
                    }
                }
            }

            writer->writeHotCues(target.sourceId, plan.source->cues);
            Console::info("  copied " + std::to_string(plan.source->cues.size()) + " cue(s) from \"" +
                           plan.source->filename + "\" (id=" + plan.source->sourceId + ") to id=" + target.sourceId);
            if (log) {
                log->record(formatName + ": copied " + std::to_string(plan.source->cues.size()) +
                             " cue(s) from track id=" + plan.source->sourceId + " (\"" + plan.source->title +
                             "\") to track id=" + target.sourceId);
            }
            totalCuesCopied += plan.source->cues.size();
            targetsWritten++;
        }
        groupsConsolidated++;
    }

    if (groupsConsolidated > 0) {
        Console::info("");
        Console::heading(formatName + ": summary");
        Console::info("  consolidated " + std::to_string(groupsConsolidated) + " duplicate track(s): copied " +
                       std::to_string(totalCuesCopied) + " cue(s) total onto " + std::to_string(targetsWritten) +
                       " track(s)");
    }
}

// Resolves the path to scan for one format: the explicit path if the user
// gave one, otherwise the unique auto-detected stick carrying that format.
// Returns an empty optional (after printing an explanatory message) when
// resolution isn't possible -- no path found, or more than one candidate.
std::optional<std::string> resolvePath(const std::optional<std::string> &explicitPath,
                                        const std::vector<DetectedStick> &detected,
                                        const std::string &formatName,
                                        std::optional<std::string> DetectedStick::*member)
{
    if (explicitPath) {
        return explicitPath;
    }

    std::vector<const DetectedStick *> candidates;
    for (const auto &stick : detected) {
        if (stick.*member) {
            candidates.push_back(&stick);
        }
    }

    if (candidates.empty()) {
        Console::error("no " + formatName + " USB stick found. Mount one, or pass a path explicitly.");
        return std::nullopt;
    }
    if (candidates.size() > 1) {
        Console::warn("multiple " + formatName + " USB sticks found -- specify one explicitly:");
        for (const auto *stick : candidates) {
            Console::info("  " + stick->label + "  (" + stick->mountPoint + ")");
        }
        return std::nullopt;
    }

    Console::verbose("auto-detected " + formatName + " stick: " + candidates[0]->label);
    return *(candidates[0]->*member);
}

struct ResolvedLibraryPaths
{
    std::optional<std::string> rekordboxPath;
    std::optional<std::string> enginePath;
    bool ok = true;
};

// Used by "backups": resolves which format(s) to act on and their paths,
// auto-detecting a USB stick where a PATH wasn't given. Requires a single
// unambiguous stick, since pruning targets one specific backup directory --
// unlike "scan" (see resolveScanTargets below), which happily scans every
// detected stick since reading is never destructive. wantRekordbox/
// wantEngine are updated in place when neither was requested (a bare
// invocation infers both from whatever the detected stick carries).
ResolvedLibraryPaths resolveLibraryPaths(bool &wantRekordbox, bool &wantEngine,
                                          const std::optional<std::string> &rekordboxPathArg,
                                          const std::optional<std::string> &enginePathArg)
{
    auto locator = seabass::infrastructure::media::createRemovableMediaLocator();
    bool needsDetection = (wantRekordbox && !rekordboxPathArg) || (wantEngine && !enginePathArg) ||
                          (!wantRekordbox && !wantEngine);
    std::vector<DetectedStick> detected;
    if (needsDetection) {
        detected = locator->detect();
    }

    if (!wantRekordbox && !wantEngine) {
        if (detected.empty()) {
            Console::error("no rekordbox or Engine USB stick found. Mount one, or pass a path explicitly.");
            return {.ok = false};
        }
        if (detected.size() > 1) {
            Console::warn("multiple USB sticks found -- specify one explicitly with --rekordbox/--engine:");
            for (const auto &stick : detected) {
                std::string formats = (stick.rekordboxPath ? "rekordbox" : "");
                if (stick.enginePath) {
                    formats += (formats.empty() ? "" : "+") + std::string("engine");
                }
                Console::info("  " + stick.label + "  (" + stick.mountPoint + ")  [" + formats + "]");
            }
            return {.ok = false};
        }
        wantRekordbox = static_cast<bool>(detected[0].rekordboxPath);
        wantEngine = static_cast<bool>(detected[0].enginePath);
    }

    ResolvedLibraryPaths result;
    if (wantRekordbox) {
        result.rekordboxPath = resolvePath(rekordboxPathArg, detected, "rekordbox", &DetectedStick::rekordboxPath);
        result.ok = result.ok && result.rekordboxPath.has_value();
    }
    if (wantEngine) {
        result.enginePath = resolvePath(enginePathArg, detected, "engine", &DetectedStick::enginePath);
        result.ok = result.ok && result.enginePath.has_value();
    }
    return result;
}

// One thing to scan: a path plus a human label for its report heading
// (the stick's label when auto-detected, or the path itself when given
// explicitly -- only shown when there's more than one target, to avoid
// cluttering the common single-target case).
struct ScanTarget
{
    std::string label;
    std::string path;
};

struct ResolvedScanTargets
{
    std::vector<ScanTarget> rekordboxTargets;
    std::vector<ScanTarget> engineTargets;
};

// Used by "scan": resolves every target to scan for each requested format.
// Unlike resolveLibraryPaths (used by "backups"), multiple detected sticks
// are never an error here -- scanning is read-only, so there's no harm in
// just scanning all of them. An explicit PATH for a format always means
// exactly one target for that format, no detection needed.
ResolvedScanTargets resolveScanTargets(bool wantRekordbox, bool wantEngine,
                                        const std::optional<std::string> &rekordboxPathArg,
                                        const std::optional<std::string> &enginePathArg, bool &ok)
{
    ResolvedScanTargets result;
    ok = true;
    bool bareInvocation = !wantRekordbox && !wantEngine;

    std::vector<DetectedStick> detected;
    if ((!rekordboxPathArg && (bareInvocation || wantRekordbox)) ||
        (!enginePathArg && (bareInvocation || wantEngine))) {
        auto locator = seabass::infrastructure::media::createRemovableMediaLocator();
        detected = locator->detect();
    }

    if (rekordboxPathArg) {
        result.rekordboxTargets.push_back({*rekordboxPathArg, *rekordboxPathArg});
    } else if (bareInvocation || wantRekordbox) {
        for (const auto &stick : detected) {
            if (stick.rekordboxPath) {
                result.rekordboxTargets.push_back({stick.label, *stick.rekordboxPath});
            }
        }
        if (wantRekordbox && result.rekordboxTargets.empty()) {
            Console::error("no rekordbox USB stick found. Mount one, or pass a path explicitly.");
            ok = false;
        }
    }

    if (enginePathArg) {
        result.engineTargets.push_back({*enginePathArg, *enginePathArg});
    } else if (bareInvocation || wantEngine) {
        for (const auto &stick : detected) {
            if (stick.enginePath) {
                result.engineTargets.push_back({stick.label, *stick.enginePath});
            }
        }
        if (wantEngine && result.engineTargets.empty()) {
            Console::error("no engine USB stick found. Mount one, or pass a path explicitly.");
            ok = false;
        }
    }

    if (bareInvocation && result.rekordboxTargets.empty() && result.engineTargets.empty()) {
        Console::error("no rekordbox or Engine USB stick found. Mount one, or pass a path explicitly.");
        ok = false;
    }

    return result;
}

std::string humanSize(std::uint64_t bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        unit++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

// Backups live under <stick root>/Seabass/backups, shared across formats
// (the rekordbox and Engine folders on one stick share the same parent).
std::string backupDirFor(const ResolvedLibraryPaths &resolved)
{
    fs::path anyPath = resolved.enginePath ? *resolved.enginePath : *resolved.rekordboxPath;
    return seabass::infrastructure::paths::stickBackupsDir(anyPath.parent_path()).string();
}

// The GUI's per-library edit lock (docs/edit-mode-and-cancel.md): a
// Seabass window on this machine has unsaved changes for, or is writing
// to, the library at `catalogPath`. Writing underneath it would race
// that save. Returns true when the write must be skipped; --force only
// warns. Read-only commands never call this.
bool refuseIfLockedByGui(const std::string &catalogPath, bool force)
{
    namespace local = seabass::infrastructure::local;
    fs::path stickRoot = fs::path(catalogPath).parent_path();
    auto info = seabass::infrastructure::system::readStickHardwareInfo(stickRoot.string(),
                                                                       stickRoot.filename().string());
    if (info.stickIdentifier.empty()) {
        return false;
    }
    local::FileLibraryEditLockStore store(local::FileLibraryEditLockStore::defaultDirectory());
    auto probe = store.probe(info.stickIdentifier, "seabass-cli");
    if (probe.status != seabass::application::EditLockStatus::HeldByOther) {
        return false;
    }
    std::string holder = "another Seabass instance";
    if (probe.holder) {
        holder = "Seabass on " + probe.holder->hostname + " (process " + std::to_string(probe.holder->pid) + ")";
    }
    if (force) {
        Console::warn("the library under " + stickRoot.string() + " is being edited by " + holder +
                      "; writing anyway because of --force");
        return false;
    }
    Console::error("the library under " + stickRoot.string() + " is being edited by " + holder +
                   ". Save or discard those changes there first, or pass --force to write anyway.");
    return true;
}

int runBackupsCommand(bool wantRekordbox, bool wantEngine, const std::optional<std::string> &rekordboxPath,
                       const std::optional<std::string> &enginePath, bool clean, size_t keepCount, bool force)
{
    auto resolved = resolveLibraryPaths(wantRekordbox, wantEngine, rekordboxPath, enginePath);
    if (!resolved.rekordboxPath && !resolved.enginePath) {
        return 1;
    }

    seabass::infrastructure::backup::FilesystemBackupStore store(backupDirFor(resolved));

    if (clean) {
        if (refuseIfLockedByGui(resolved.enginePath ? *resolved.enginePath : *resolved.rekordboxPath, force)) {
            return 1;
        }
        // The same on-stick write lock the GUI's writes hold.
        auto locks = seabass::infrastructure::backup::acquireStickLocks({backupDirFor(resolved)});
        auto freed = store.prune(keepCount);
        Console::info("freed " + humanSize(freed) + " (kept up to " + std::to_string(keepCount) +
                       " most recent backup(s))");
        return resolved.ok ? 0 : 1;
    }

    auto records = store.list();
    if (records.empty()) {
        Console::info("no backups found under " + backupDirFor(resolved));
        return resolved.ok ? 0 : 1;
    }

    Console::heading("Backups (" + backupDirFor(resolved) + ")");
    std::uint64_t total = 0;
    for (const auto &record : records) {
        Console::info("  " + record.id + "  " + humanSize(record.sizeBytes));
        total += record.sizeBytes;
    }
    Console::info("  total: " + humanSize(total) + " across " + std::to_string(records.size()) + " backup(s)");
    Console::info("run with --clean [--keep N] to prune old backups (default keep " +
                   std::to_string(DefaultKeepBackups) + ")");
    return resolved.ok ? 0 : 1;
}

std::chrono::system_clock::time_point fileMtime(const std::string &path)
{
    return std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(path));
}

std::string describeCues(const std::vector<seabass::domain::CuePoint> &cues)
{
    int hot = 0;
    int memory = 0;
    for (const auto &cue : cues) {
        (cue.kind == seabass::domain::CuePoint::Kind::Hot ? hot : memory)++;
    }
    std::string result = std::to_string(hot) + " hot";
    if (memory > 0) {
        result += ", " + std::to_string(memory) + " memory (not written -- Engine writer only handles hot cues)";
    }
    return result;
}

// "sync": two phases. First, analyze both libraries and print the full
// proposal -- what would move which direction, and what can't be applied
// because rekordbox has no write path yet -- without touching anything.
// Only after that does it ask for confirmation (unless --auto or
// --dry-run) before actually writing.
int runSyncCommand(bool wantRekordbox, bool wantEngine, const std::optional<std::string> &rekordboxPath,
                    const std::optional<std::string> &enginePath, bool autoMode, bool dryRun, bool force)
{
    auto resolved = resolveLibraryPaths(wantRekordbox, wantEngine, rekordboxPath, enginePath);
    using seabass::domain::SyncPlan;

    // A stick can carry three catalogs, and sync needs any two of them.
    // OneLibrary lives beside export.pdb under the same PIONEER root.
    const bool hasRekordbox = resolved.rekordboxPath.has_value();
    const bool hasEngine = resolved.enginePath.has_value();
    const bool hasOneLibrary =
        hasRekordbox && seabass::infrastructure::onelibrary::OneLibraryCueWriter::existsFor(*resolved.rekordboxPath);

    if (static_cast<int>(hasRekordbox) + static_cast<int>(hasEngine) + static_cast<int>(hasOneLibrary) < 2) {
        Console::error("sync needs at least two catalogs -- found: " +
                        std::string(hasRekordbox ? "rekordbox" : "no rekordbox") + ", " +
                        std::string(hasEngine ? "engine" : "no engine") + ", " +
                        std::string(hasOneLibrary ? "onelibrary" : "no onelibrary"));
        return 1;
    }

    std::vector<seabass::domain::Track> rekordboxTracks;
    std::vector<seabass::domain::Track> engineTracks;
    std::vector<seabass::domain::Track> oneLibraryTracks;
    std::string rekordboxDbFile;
    std::string engineDbFile;
    std::string oneLibraryDbFile;

    try {
        if (hasRekordbox) {
            rekordboxDbFile = (fs::path(*resolved.rekordboxPath) / "rekordbox" / "export.pdb").string();
            rekordboxTracks = scanPath(
                std::make_unique<seabass::infrastructure::rekordbox::KaitaiRekordboxReader>(*resolved.rekordboxPath),
                *resolved.rekordboxPath);
        }
        if (hasEngine) {
            engineDbFile = (fs::path(*resolved.enginePath) / "Database2" / "m.db").string();
            engineTracks = scanPath(
                std::make_unique<seabass::infrastructure::engine::LibdjinteropEngineReader>(*resolved.enginePath),
                *resolved.enginePath);
        }
        if (hasOneLibrary) {
            oneLibraryDbFile =
                seabass::infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(*resolved.rekordboxPath);
            oneLibraryTracks = scanPath(
                std::make_unique<seabass::infrastructure::onelibrary::OneLibraryReader>(*resolved.rekordboxPath),
                *resolved.rekordboxPath);
        }
    } catch (const std::exception &e) {
        Console::error(e.what());
        return 1;
    }

    // Only for catalogs that are actually present: fileMtime() throws on a
    // path that is not there, and an absent catalog has no mtime to compare.
    auto mtimeOrEpoch = [](const std::string &path) {
        return path.empty() ? std::chrono::system_clock::time_point{} : fileMtime(path);
    };
    auto rekordboxMtime = mtimeOrEpoch(rekordboxDbFile);
    auto engineMtime = mtimeOrEpoch(engineDbFile);
    auto oneLibraryMtime = mtimeOrEpoch(oneLibraryDbFile);

    // rekordbox <-> OneLibrary is deliberately NOT a pair: they are one
    // library written in two formats, and every rekordbox write below
    // mirrors into OneLibrary, so they cannot drift apart. See
    // sync_controller.cpp, which makes the same choice for the same reason.
    std::vector<SyncPlan> plans;
    auto addPair = [&plans](const std::vector<seabass::domain::Track> &a,
                            const std::vector<seabass::domain::Track> &b,
                            std::chrono::system_clock::time_point mtimeA,
                            std::chrono::system_clock::time_point mtimeB) {
        for (auto &plan : seabass::application::SyncLibraries().execute(a, b, mtimeA, mtimeB)) {
            plans.push_back(std::move(plan));
        }
    };
    if (hasRekordbox && hasEngine) {
        addPair(rekordboxTracks, engineTracks, rekordboxMtime, engineMtime);
    }
    if (hasEngine && hasOneLibrary) {
        addPair(engineTracks, oneLibraryTracks, engineMtime, oneLibraryMtime);
    }
    if (!hasEngine) {
        // The only two catalogs here are rekordbox and OneLibrary, which is
        // one library in two formats. They are kept level by mirroring on
        // every write, not by syncing, so there is no pair to plan -- and
        // saying "already consistent" would imply this checked something.
        Console::info("");
        Console::info("rekordbox and OneLibrary are one library in two formats, kept in step whenever");
        Console::info("Seabass writes either of them -- there is nothing for sync to reconcile between");
        Console::info("them. Add an Engine library to have something to sync with.");
        return 0;
    }

    // --- Phase 1: analyze and propose, no writes yet ---
    Console::info("");
    Console::heading("sync analysis");
    if (hasRekordbox) {
        Console::info("  rekordbox tracks:  " + std::to_string(rekordboxTracks.size()));
    }
    if (hasEngine) {
        Console::info("  engine tracks:     " + std::to_string(engineTracks.size()));
    }
    if (hasOneLibrary) {
        Console::info("  onelibrary tracks: " + std::to_string(oneLibraryTracks.size()));
    }
    Console::info("  matched tracks:    " + std::to_string(plans.size()));

    // Bucketed by the TARGET's own format rather than by which side of the
    // pair it sits on. With more than one pair, trackA is no longer always
    // rekordbox, and position would silently write to the wrong catalog.
    auto targetOf = [](const SyncPlan &plan) {
        return plan.direction == SyncPlan::Direction::ToB ? &plan.match.trackB : &plan.match.trackA;
    };
    std::vector<const SyncPlan *> toEngine;
    std::vector<const SyncPlan *> toRekordbox;
    std::vector<const SyncPlan *> toOneLibrary;
    for (const auto &plan : plans) {
        if (plan.direction == SyncPlan::Direction::None) {
            continue;
        }
        const std::string &format = targetOf(plan)->format;
        if (format == "engine") {
            toEngine.push_back(&plan);
        } else if (format == "rekordbox") {
            toRekordbox.push_back(&plan);
        } else if (format == "onelibrary") {
            toOneLibrary.push_back(&plan);
        }
    }

    if (toEngine.empty() && toRekordbox.empty() && toOneLibrary.empty()) {
        Console::info("  nothing to sync -- matched tracks' cues are already consistent (or empty on both sides).");
        return 0;
    }

    if (!toEngine.empty()) {
        Console::info("");
        Console::info("Would copy cues to Engine for " + std::to_string(toEngine.size()) + " track(s):");
        for (const auto *plan : toEngine) {
            bool conflict = plan->kind == SyncPlan::Kind::Conflict;
            Console::info("  \"" + targetOf(*plan)->filename + "\": " + describeCues(plan->cuesToApply) +
                           (conflict ? "  [conflict resolved: the source catalog is newer]" : ""));
        }
    }

    if (!toRekordbox.empty()) {
        Console::info("");
        Console::info("Would copy cues to rekordbox for " + std::to_string(toRekordbox.size()) + " track(s):");
        for (const auto *plan : toRekordbox) {
            bool conflict = plan->kind == SyncPlan::Kind::Conflict;
            Console::info("  \"" + targetOf(*plan)->filename + "\": " + describeCues(plan->cuesToApply) +
                           (conflict ? "  [conflict resolved: the source catalog is newer]" : ""));
        }
        Console::warn("rekordbox writing is the least-proven part of Seabass -- verify the result in rekordbox");
        Console::warn("and on real hardware before trusting it for a gig (see --help's Sync section).");
    }

    if (!toOneLibrary.empty()) {
        Console::info("");
        Console::info("Would copy cues to OneLibrary for " + std::to_string(toOneLibrary.size()) + " track(s):");
        for (const auto *plan : toOneLibrary) {
            bool conflict = plan->kind == SyncPlan::Kind::Conflict;
            Console::info("  \"" + targetOf(*plan)->filename + "\": " + describeCues(plan->cuesToApply) +
                           (conflict ? "  [conflict resolved: the source catalog is newer]" : ""));
        }
    }

    if (dryRun) {
        Console::info("");
        Console::info("--dry-run: no changes made.");
        return 0;
    }

    // --- Phase 2: single confirmation gate ---
    size_t totalChanges = toEngine.size() + toRekordbox.size() + toOneLibrary.size();
    bool apply = autoMode || Console::confirm("\nApply the " + std::to_string(totalChanges) + " change(s) above?");
    if (!apply) {
        Console::info("skipped -- no changes made.");
        return 0;
    }
    if ((hasEngine && refuseIfLockedByGui(*resolved.enginePath, force))
        || (hasRekordbox && refuseIfLockedByGui(*resolved.rekordboxPath, force))) {
        return 1;
    }

    // --- Phase 3: apply ---
    try {
        // The same on-stick write locks the GUI's writes hold, for every
        // catalog present (one lock when they share a stick). OneLibrary
        // lives under the rekordbox root, so it needs no lock of its own.
        std::vector<std::string> lockDirs;
        if (hasEngine) {
            lockDirs.push_back(seabass::infrastructure::backup::backupDirForCatalogPath(*resolved.enginePath));
        }
        if (hasRekordbox) {
            lockDirs.push_back(seabass::infrastructure::backup::backupDirForCatalogPath(*resolved.rekordboxPath));
        }
        auto stickLocks = seabass::infrastructure::backup::acquireStickLocks(lockDirs);

        // A store and a log per stick root, created only where there is a
        // catalog to write. rekordbox and OneLibrary share both.
        const std::string engineRootDir =
            hasEngine ? fs::path(*resolved.enginePath).parent_path().string() : std::string();
        const std::string rekordboxRootDir =
            hasRekordbox ? fs::path(*resolved.rekordboxPath).parent_path().string() : std::string();

        std::optional<seabass::infrastructure::backup::FilesystemBackupStore> engineBackupStoreOpt;
        std::optional<seabass::infrastructure::logging::FileOperationLog> engineLogOpt;
        if (hasEngine) {
            engineBackupStoreOpt.emplace(seabass::infrastructure::paths::stickBackupsDir(fs::path(engineRootDir)).string());
            engineLogOpt.emplace(seabass::infrastructure::paths::stickOperationLog(fs::path(engineRootDir)).string());
        }
        std::optional<seabass::infrastructure::backup::FilesystemBackupStore> rekordboxBackupStoreOpt;
        std::optional<seabass::infrastructure::logging::FileOperationLog> rekordboxLogOpt;
        if (hasRekordbox) {
            rekordboxBackupStoreOpt.emplace(seabass::infrastructure::paths::stickBackupsDir(fs::path(rekordboxRootDir)).string());
            rekordboxLogOpt.emplace(seabass::infrastructure::paths::stickOperationLog(fs::path(rekordboxRootDir)).string());
        }

        // Everything this sync will overwrite, resolved before a single
        // byte is written, and backed up as ONE record per stick rather
        // than one per file. The CLI used to call backup() inside the write
        // loop: a 939-track sync left 942 separate records, which Manage
        // Backups lists individually and Undo cannot revert as a unit.
        // Same shape as the GUI's save loop -- declare, back up once, write.
        std::set<std::string> rekordboxFiles;
        for (const auto *plan : toRekordbox) {
            auto analyzePath = seabass::infrastructure::rekordbox::findAnlzPathForTrackId(
                *resolved.rekordboxPath, static_cast<uint32_t>(std::stoul(targetOf(*plan)->sourceId)));
            if (analyzePath) {
                rekordboxFiles.insert(
                    seabass::infrastructure::rekordbox::extAnlzPath(*resolved.rekordboxPath, *analyzePath));
            }
        }
        if (hasOneLibrary && (!toOneLibrary.empty() || !toRekordbox.empty())) {
            rekordboxFiles.insert(oneLibraryDbFile);  // written directly, or mirrored into
        }

        if (!rekordboxFiles.empty()) {
            auto record = rekordboxBackupStoreOpt->backup(
                std::vector<std::string>(rekordboxFiles.begin(), rekordboxFiles.end()), "sync");
            Console::info("");
            Console::info("backed up " + std::to_string(rekordboxFiles.size()) + " file(s) to " + record.path);
            rekordboxLogOpt->record("sync: backed up " + std::to_string(rekordboxFiles.size())
                                    + " file(s) before cross-format sync -> " + record.path);
        }

        size_t engineCuesCopied = 0;
        if (!toEngine.empty()) {
            seabass::infrastructure::engine::LibdjinteropEngineCueWriter writer(*resolved.enginePath);
            auto record = engineBackupStoreOpt->backup({engineDbFile}, "sync");
            Console::info("");
            Console::info("backed up Engine to " + record.path);
            engineLogOpt->record("sync: backed up before cross-format sync -> " + record.path);

            for (const auto *plan : toEngine) {
                writer.writeHotCues(targetOf(*plan)->sourceId, plan->cuesToApply);
                engineCuesCopied += plan->cuesToApply.size();
                engineLogOpt->record("sync: copied cues (" + describeCues(plan->cuesToApply) +
                                  ") from rekordbox track \"" + plan->match.trackA.title +
                                  "\" (id=" + plan->match.trackA.sourceId +
                                  ") to engine track id=" + plan->match.trackB.sourceId);
            }
        }

        size_t rekordboxCuesCopied = 0;
        if (!toRekordbox.empty()) {
            seabass::infrastructure::rekordbox::RekordboxCueWriter writer(*resolved.rekordboxPath);

            for (const auto *plan : toRekordbox) {
                const auto *target = targetOf(*plan);
                const auto &source =
                    plan->direction == SyncPlan::Direction::ToB ? plan->match.trackA : plan->match.trackB;
                writer.writeHotCues(target->sourceId, plan->cuesToApply);
                rekordboxCuesCopied += plan->cuesToApply.size();
                rekordboxLogOpt->record("sync: copied cues (" + describeCues(plan->cuesToApply) + ") from "
                                        + source.format + " track \"" + source.title + "\" (id="
                                        + source.sourceId + ") to rekordbox track id=" + target->sourceId);

                // export.pdb and exportLibrary.db are one library in two
                // formats, so a cue written to one belongs in the other.
                // This is why sync does not plan them as a pair.
                if (hasOneLibrary && !target->filePath.empty()) {
                    try {
                        seabass::infrastructure::onelibrary::OneLibraryCueWriter mirror(*resolved.rekordboxPath);
                        mirror.writeCuesForPath(target->filePath, plan->cuesToApply);
                        rekordboxLogOpt->record("sync: also wrote cues to the OneLibrary copy of track id="
                                                + target->sourceId);
                    } catch (const std::exception &e) {
                        // Best-effort, matching the GUI: the rekordbox write
                        // already landed, and failing the whole sync over the
                        // mirror would leave the user worse off than a stale
                        // copy they can re-run.
                        Console::warn(std::string("could not mirror into OneLibrary: ") + e.what());
                        rekordboxLogOpt->record(std::string("sync: OneLibrary cue mirror failed: ") + e.what());
                    }
                }
            }
        }

        size_t oneLibraryCuesCopied = 0;
        if (!toOneLibrary.empty()) {
            seabass::infrastructure::onelibrary::OneLibraryCueWriter writer(*resolved.rekordboxPath);
            for (const auto *plan : toOneLibrary) {
                const auto *target = targetOf(*plan);
                const auto &source =
                    plan->direction == SyncPlan::Direction::ToB ? plan->match.trackA : plan->match.trackB;
                if (target->filePath.empty()) {
                    continue;  // OneLibrary resolves a row by path; without one there is nothing to write
                }
                writer.writeCuesForPath(target->filePath, plan->cuesToApply);
                oneLibraryCuesCopied += plan->cuesToApply.size();
                rekordboxLogOpt->record("sync: copied cues (" + describeCues(plan->cuesToApply) + ") from "
                                        + source.format + " track \"" + source.title + "\" (id="
                                        + source.sourceId + ") to onelibrary track \"" + target->filename + "\"");
            }
        }

        Console::info("");
        Console::heading("sync summary");
        if (!toEngine.empty()) {
            Console::info("  synced " + std::to_string(toEngine.size()) + " track(s) to Engine: copied " +
                           std::to_string(engineCuesCopied) + " cue(s) total");
        }
        if (!toRekordbox.empty()) {
            Console::info("  synced " + std::to_string(toRekordbox.size()) + " track(s) to rekordbox: copied " +
                           std::to_string(rekordboxCuesCopied) + " cue(s) total");
        }
        if (!toOneLibrary.empty()) {
            Console::info("  synced " + std::to_string(toOneLibrary.size()) + " track(s) to OneLibrary: copied " +
                           std::to_string(oneLibraryCuesCopied) + " cue(s) total");
        }
    } catch (const std::exception &e) {
        Console::error(e.what());
        return 1;
    }

    return 0;
}

// "anonymize": produces a de-identified, structurally-real copy of one
// or both libraries at --out DIR, for the maintainer's own committed
// test fixtures or for a user who wants to submit their library to help
// test against hardware/library shapes the maintainer doesn't have --
// see AnonymizeLibrary's own doc comment for exactly what's kept vs.
// replaced vs. removed. Never sends anything anywhere itself; only ever
// writes to DIR.
int runAnonymizeCommand(bool wantRekordbox, bool wantEngine, const std::optional<std::string> &rekordboxPath,
                         const std::optional<std::string> &enginePath, const std::optional<std::string> &outDir,
                         std::optional<size_t> maxTracks, bool slim, const std::string &hardware,
                         const std::string &notes)
{
    if (!outDir) {
        Console::error("anonymize requires --out DIR");
        return 1;
    }

    auto resolved = resolveLibraryPaths(wantRekordbox, wantEngine, rekordboxPath, enginePath);
    if (!resolved.rekordboxPath && !resolved.enginePath) {
        return 1;
    }

    AnonymizationOptions options;
    options.maxTracks = maxTracks;
    options.slimForTesting = slim;
    options.hardware = hardware;
    options.notes = notes;

    seabass::cli::TerminalProgressReporter progress;
    AnonymizeLibrary useCase;
    auto summary = useCase.execute(resolved.rekordboxPath, resolved.enginePath, *outDir, options, progress);

    if (!summary.outputError.empty()) {
        Console::error(summary.outputError);
    }
    if (summary.rekordboxAttempted && !summary.rekordboxError.empty()) {
        Console::error("rekordbox: " + summary.rekordboxError);
    }
    if (summary.engineAttempted && !summary.engineError.empty()) {
        Console::error("engine: " + summary.engineError);
    }
    if (summary.verificationFailed) {
        // The export was built, checked against the promise its own
        // manifest makes, found to still hold real data, and thrown away.
        // Saying so plainly matters more than usual here: the whole point
        // of the check is that nobody ships a leaking export believing it
        // is clean.
        Console::error("");
        Console::error("This export still held real data, so it was NOT written.");
        Console::error("Nothing was left on disk. Please report this with the detail below.");
        Console::error("");
        Console::error(summary.verificationReport);
        return 1;
    }
    if (!summary.succeeded()) {
        return 1;
    }

    Console::info("");
    Console::heading("Anonymized library written to " + summary.outputZipPath);
    if (summary.rekordboxAttempted) {
        std::string line = "  rekordbox: kept " + std::to_string(summary.rekordboxTracksKept) + " track(s)";
        if (summary.rekordboxTracksDropped > 0) {
            line += ", dropped " + std::to_string(summary.rekordboxTracksDropped);
        }
        line += "; renamed " + std::to_string(summary.rekordboxArtistsRenamed) + " artist(s), " +
                std::to_string(summary.rekordboxPlaylistsRenamed) + " playlist(s)/folder(s)";
        Console::info(line);
    }
    if (summary.engineAttempted) {
        std::string line = "  engine: kept " + std::to_string(summary.engineTracksKept) + " track(s)";
        if (summary.engineTracksDropped > 0) {
            line += ", dropped " + std::to_string(summary.engineTracksDropped);
        }
        line += "; renamed " + std::to_string(summary.enginePlaylistsRenamed) + " playlist(s)/folder(s)";
        Console::info(line);
    }
    Console::info("  " + humanSize(static_cast<std::uint64_t>(summary.finalZipBytes)) + " zipped, " +
                   std::to_string(summary.filesWritten) + " file(s) (" +
                   humanSize(static_cast<std::uint64_t>(summary.outputSizeBytes)) + " before compression)");
    Console::info("");
    Console::info("MANIFEST.txt (inside the zip) documents exactly what's included and excluded:");
    Console::info("");
    Console::info(summary.manifestText);
    Console::heading("Nothing has been sent anywhere");
    Console::info("This only wrote " + summary.outputZipPath +
                   ". Review its contents, then attach it to an");
    Console::info("email to sebas@kde.org if you'd like to help test against your hardware/library.");
    Console::info("This dataset may be published as part of the project's test suite. If there's");
    Console::info("anything in --hardware/--notes you'd rather not have published, leave it out and");
    Console::info("mention it directly in your email instead.");
    if (!maxTracks) {
        Console::info("If this is too large to attach, re-run with --max-tracks to include a smaller");
        Console::info("sample.");
    }

    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
#ifdef SEABASS_HAVE_QT_AUDIO
    // QtMultimediaDurationProbe runs a nested QEventLoop, which needs an
    // application object to exist. QCoreApplication specifically -- no
    // GUI, no QGuiApplication, no QPA platform plugin (verified headless
    // with DISPLAY and WAYLAND_DISPLAY unset), so this stays a console
    // program.
    QCoreApplication app(argc, argv);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);

    bool verbose = false;
    bool help = false;
    bool autoMode = false;
    bool dryRun = false;
    bool clean = false;
    bool force = false;
    bool wantRekordbox = false;
    bool wantEngine = false;
    bool needsCues = false;
    size_t keepCount = DefaultKeepBackups;
    size_t needsCuesLimit = DefaultNeedsCuesLimit;
    std::optional<std::string> rekordboxPath;
    std::optional<std::string> enginePath;
    std::optional<std::string> trackFilter;
    std::optional<std::string> outDir;
    std::optional<size_t> maxTracks;
    bool slim = false;
    std::string hardware;
    std::string notes;
    std::vector<std::string> commands;

    auto looksLikeFlag = [](const std::string &s) { return s.size() >= 2 && s[0] == '-' && s[1] == '-'; };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--auto") {
            autoMode = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--clean") {
            clean = true;
        } else if (arg == "--keep") {
            if (i + 1 >= args.size()) {
                Console::error("--keep requires a number");
                return 1;
            }
            ++i;
            try {
                keepCount = std::stoul(args[i]);
            } catch (const std::exception &) {
                Console::error("--keep requires a number, got \"" + args[i] + "\"");
                return 1;
            }
        } else if (arg == "--track") {
            if (i + 1 >= args.size()) {
                Console::error("--track requires a name to search for");
                return 1;
            }
            trackFilter = args[++i];
        } else if (arg == "--needs-cues") {
            needsCues = true;
            if (i + 1 < args.size() && !looksLikeFlag(args[i + 1])) {
                try {
                    needsCuesLimit = std::stoul(args[i + 1]);
                    ++i;
                } catch (const std::exception &) {
                    // Not a number -- leave it for the next flag/command to consume.
                }
            }
        } else if (arg == "--rekordbox") {
            wantRekordbox = true;
            if (i + 1 < args.size() && !looksLikeFlag(args[i + 1])) {
                rekordboxPath = args[++i];
            }
        } else if (arg == "--engine") {
            wantEngine = true;
            if (i + 1 < args.size() && !looksLikeFlag(args[i + 1])) {
                enginePath = args[++i];
            }
        } else if (arg == "--out") {
            if (i + 1 >= args.size()) {
                Console::error("--out requires a directory");
                return 1;
            }
            outDir = args[++i];
        } else if (arg == "--slim") {
            slim = true;
        } else if (arg == "--max-tracks") {
            if (i + 1 >= args.size()) {
                Console::error("--max-tracks requires a number");
                return 1;
            }
            ++i;
            try {
                maxTracks = std::stoul(args[i]);
            } catch (const std::exception &) {
                Console::error("--max-tracks requires a number, got \"" + args[i] + "\"");
                return 1;
            }
        } else if (arg == "--hardware") {
            if (i + 1 >= args.size()) {
                Console::error("--hardware requires a description");
                return 1;
            }
            hardware = args[++i];
        } else if (arg == "--notes") {
            if (i + 1 >= args.size()) {
                Console::error("--notes requires text");
                return 1;
            }
            notes = args[++i];
        } else if (looksLikeFlag(arg)) {
            Console::error("unknown option: " + arg);
            printUsage();
            return 1;
        } else {
            commands.push_back(arg);
        }
    }

    Console::setVerbose(verbose);

    if (help || commands.empty()) {
        printUsage();
        return help ? 0 : 1;
    }
    if (commands.size() != 1 ||
        (commands[0] != "scan" && commands[0] != "backups" && commands[0] != "sync" && commands[0] != "anonymize")) {
        Console::error("unknown command: " + commands[0]);
        printUsage();
        return 1;
    }

    if (commands[0] == "backups") {
        return runBackupsCommand(wantRekordbox, wantEngine, rekordboxPath, enginePath, clean, keepCount, force);
    }

    if (commands[0] == "sync") {
        return runSyncCommand(wantRekordbox, wantEngine, rekordboxPath, enginePath, autoMode, dryRun, force);
    }

    if (commands[0] == "anonymize") {
        return runAnonymizeCommand(wantRekordbox, wantEngine, rekordboxPath, enginePath, outDir, maxTracks,
                                    slim, hardware, notes);
    }

    // commands[0] == "scan". Every detected stick carrying a requested
    // format gets scanned -- reading is never destructive, so unlike
    // "backups" there's no reason to force picking just one when several
    // sticks are plugged in.
    bool ok = true;
    auto scanTargets = resolveScanTargets(wantRekordbox, wantEngine, rekordboxPath, enginePath, ok);
    ReportOptions reportOptions{.trackFilter = trackFilter, .needsCues = needsCues, .needsCuesLimit = needsCuesLimit};
    try {
        bool multipleRekordbox = scanTargets.rekordboxTargets.size() > 1;
        for (const auto &target : scanTargets.rekordboxTargets) {
            std::string heading = multipleRekordbox ? "rekordbox (" + target.label + ")" : "rekordbox";
            auto tracks = scanPath(
                std::make_unique<seabass::infrastructure::rekordbox::KaitaiRekordboxReader>(target.path),
                target.path);
            printReport(heading, tracks, reportOptions);
            if (refuseIfLockedByGui(target.path, force)) {
                continue;  // the report is out; only the consolidation offer is skipped
            }
            auto stickLocks = seabass::infrastructure::backup::acquireStickLocks(
                {seabass::infrastructure::backup::backupDirForCatalogPath(target.path)});

            seabass::infrastructure::rekordbox::RekordboxCueWriter writer(target.path);
            fs::path stickRoot = fs::path(target.path).parent_path();
            seabass::infrastructure::backup::FilesystemBackupStore backupStore(
                seabass::infrastructure::paths::stickBackupsDir(stickRoot).string());
            seabass::infrastructure::logging::FileOperationLog log(seabass::infrastructure::paths::stickOperationLog(stickRoot).string());
            std::string pioneerRoot = target.path;
            auto filesToBackUpFor = [pioneerRoot](const std::string &trackSourceId) -> std::vector<std::string> {
                auto analyzePath = seabass::infrastructure::rekordbox::findAnlzPathForTrackId(
                    pioneerRoot, static_cast<uint32_t>(std::stoul(trackSourceId)));
                if (!analyzePath) {
                    return {};
                }
                return {seabass::infrastructure::rekordbox::extAnlzPath(pioneerRoot, *analyzePath)};
            };
            handleDuplicates(heading, tracks, &writer, &backupStore, &log, filesToBackUpFor, autoMode);
        }

        bool multipleEngine = scanTargets.engineTargets.size() > 1;
        for (const auto &target : scanTargets.engineTargets) {
            std::string heading = multipleEngine ? "engine (" + target.label + ")" : "engine";
            auto tracks = scanPath(
                std::make_unique<seabass::infrastructure::engine::LibdjinteropEngineReader>(target.path),
                target.path);
            printReport(heading, tracks, reportOptions);
            if (refuseIfLockedByGui(target.path, force)) {
                continue;
            }
            auto stickLocks = seabass::infrastructure::backup::acquireStickLocks(
                {seabass::infrastructure::backup::backupDirForCatalogPath(target.path)});

            seabass::infrastructure::engine::LibdjinteropEngineCueWriter writer(target.path);
            fs::path stickRoot = fs::path(target.path).parent_path();
            seabass::infrastructure::backup::FilesystemBackupStore backupStore(
                seabass::infrastructure::paths::stickBackupsDir(stickRoot).string());
            seabass::infrastructure::logging::FileOperationLog log(seabass::infrastructure::paths::stickOperationLog(stickRoot).string());
            std::string engineDbFile = (fs::path(target.path) / "Database2" / "m.db").string();
            auto filesToBackUpFor = [engineDbFile](const std::string &) -> std::vector<std::string> {
                return {engineDbFile};
            };
            handleDuplicates(heading, tracks, &writer, &backupStore, &log, filesToBackUpFor, autoMode);
        }
    } catch (const std::exception &e) {
        Console::error(e.what());
        return 1;
    }

    return ok ? 0 : 1;
}
