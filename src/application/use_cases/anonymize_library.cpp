#include "application/use_cases/anonymize_library.hpp"

#include "application/stick_path_match.hpp"
#include <algorithm>
#include "infrastructure/anonymization_placeholder.hpp"
#include "infrastructure/cleanup/audio_file_walk.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "infrastructure/anonymization_verifier.hpp"
#include "infrastructure/engine/libdjinterop_engine_anonymizer.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/rekordbox/rekordbox_library_anonymizer.hpp"
#include "infrastructure/zip_archive_writer.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;

namespace
{

// No real version plumbing exists in this codebase yet (see
// BRAINSTORM.md's own separate "Add version, show it in --help and on
// About page" item) -- this is a stopgap literal, not a build-system
// integration, and should be replaced with the real thing once that
// item lands.
constexpr const char *SeabassVersionStopgap = "0.6 (beta)";

std::string hostOsName()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown OS";
#endif
}


// Blended, catalog-specific compression ratios measured with `gzip -9`
// against real rekordbox/Engine library files (see the plan this
// implements) -- an *estimate*, not a guarantee, of what a zip archive
// of the actual output would come to; printed alongside the real raw
// size so a caller (or the CLI's own printed summary) can judge whether
// to re-run with a smaller AnonymizationOptions::maxTracks before
// attaching this to an email.

std::string humanSize(std::uintmax_t bytes)
{
    constexpr double Kib = 1024.0;
    constexpr double Mib = Kib * 1024.0;
    std::ostringstream oss;
    if (bytes >= static_cast<std::uintmax_t>(Mib)) {
        oss.precision(1);
        oss << std::fixed << (static_cast<double>(bytes) / Mib) << " MB";
    } else {
        oss.precision(1);
        oss << std::fixed << (static_cast<double>(bytes) / Kib) << " KB";
    }
    return oss.str();
}

// The stick's audio files, listed rather than shipped.
//
// An export carries three catalogs and no audio, which means the whole
// files-versus-catalog half of Seabass -- unreferenced files, orphan
// detection, the cleanup that decides a file is safe to delete -- has
// nothing to run against on shared data. The files themselves cannot be
// shipped: they are gigabytes and they are not the submitter's to
// distribute.
//
// A listing costs kilobytes and is enough. Names go through the same
// placeholder as the catalog rows, so a manifest entry and the row that
// points at it still match, and the sizes are real, which is what the
// duplicate and orphan heuristics actually compare. A consumer can
// materialise the tree as empty files and exercise every one of those
// paths at no bytes.
//
// Written as TSV rather than prose: this one is read by programs.
void writeFileListing(const fs::path &listingPath, const fs::path &stickRoot, int &filesListed)
{
    std::error_code ec;
    std::ofstream out(listingPath, std::ios::trunc);
    out << "# relative path\tsize in bytes\n";
    if (!fs::is_directory(stickRoot, ec)) {
        return;
    }
    std::vector<std::pair<std::string, std::uintmax_t>> rows;
    for (const auto &entry : fs::recursive_directory_iterator(
             stickRoot, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!infrastructure::cleanup::isAudioExtension(name)) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), stickRoot, ec);
        if (ec) {
            continue;
        }
        // Directory names are a DJ's own filing -- artist and album --
        // so only the depth survives, never the words.
        std::string anonymized;
        const std::size_t depth = std::distance(relative.begin(), relative.end());
        for (std::size_t i = 0; i + 1 < depth; ++i) {
            anonymized += "d" + std::to_string(i) + "/";
        }
        anonymized += infrastructure::anonymizationFilenamePlaceholder(name);
        const std::uintmax_t size = entry.file_size(ec);
        rows.emplace_back(anonymized, ec ? 0 : size);
    }
    // Sorted so two exports of the same stick produce the same file.
    std::sort(rows.begin(), rows.end());
    for (const auto &[path, size] : rows) {
        out << path << '\t' << size << '\n';
    }
    filesListed = static_cast<int>(rows.size());
}

void writeManifest(const fs::path &manifestPath, const AnonymizationSummary &summary,
                    const AnonymizationOptions &options)
{
    std::ostringstream m;
    m << "Seabass anonymized library export\n";
    m << "==================================\n\n";
    m << "Seabass version: " << SeabassVersionStopgap << "\n";
    m << "Host OS: " << hostOsName() << "\n";
    m << "Hardware (as entered by the submitter): " << (options.hardware.empty() ? "(none given)" : options.hardware)
      << "\n";
    m << "Notes (as entered by the submitter): " << (options.notes.empty() ? "(none given)" : options.notes) << "\n\n";

    if (summary.rekordboxAttempted) {
        m << "rekordbox:\n";
        if (summary.rekordboxError.empty()) {
            m << "Tracks kept: " << summary.rekordboxTracksKept << "\n";
            m << "Tracks dropped (--max-tracks): " << summary.rekordboxTracksDropped << "\n";
            m << "Distinct artists renamed: " << summary.rekordboxArtistsRenamed << "\n";
            m << "Playlists/folders renamed: " << summary.rekordboxPlaylistsRenamed << "\n";
        } else {
            m << "FAILED: " << summary.rekordboxError << "\n";
        }
        m << "\n";
    }
    if (summary.engineAttempted) {
        m << "Engine:\n";
        if (summary.engineError.empty()) {
            m << "Tracks kept: " << summary.engineTracksKept << "\n";
            m << "Tracks dropped (--max-tracks): " << summary.engineTracksDropped << "\n";
            m << "Playlists/folders renamed: " << summary.enginePlaylistsRenamed << "\n";
            if (summary.engineTracksRefused > 0) {
                m << "\n*** WARNING: " << summary.engineTracksRefused
                  << " track(s) could NOT be anonymized and still hold their real\n"
                     "    title, artist and file path. Reason: " << summary.engineFirstRefusalReason << "\n"
                     "    DO NOT SHARE THIS EXPORT.\n";
            }
        } else {
            m << "FAILED: " << summary.engineError << "\n";
        }
        m << "\n";
    }

    m << "What's included vs. stripped, on every track:\n"
         "  KEPT as-is: format, file size, bitrate, duration, BPM, key,\n"
         "    hot/memory cue positions and colors, rating, play count,\n"
         "    last-played date, whether it's a streaming-service track,\n"
         "    playlist membership/position, the low-resolution monochrome\n"
         "    waveform preview actually used by this app, beatgrid.\n"
         "  REPLACED with placeholder text (e.g. \"Track 014\"): title,\n"
         "    artist, comment, cue comments, filename/file path,\n"
         "    playlist/folder names.\n"
         "  REPLACED inside the analysis files too: the file path each\n"
         "    one embeds, which is where the artist, album and title\n"
         "    would otherwise still be readable.\n"
         "  ALSO INCLUDED: your player preference files (MYSETTING.DAT\n"
         "    and friends). They hold settings like LCD brightness and\n"
         "    jog feel, nothing about you or your music, and this app's\n"
         "    Device Profile feature cannot be tested without them.\n"
         "  ALSO SCRUBBED AND INCLUDED: your Device Library Plus\n"
         "    database (exportLibrary.db), the mirror rekordbox keeps\n"
         "    beside export.pdb. Its titles, artists, albums, genres,\n"
         "    labels, playlist names, cue comments and file paths are\n"
         "    replaced the same way, and the same real track gets the\n"
         "    same placeholder in all three catalogs.\n"
         "  REMOVED entirely: artwork images, the detailed color and\n"
         "    scrolling waveform data rekordbox's own UI uses during\n"
         "    playback (not read by this app), original file paths, and\n"
         "    your My Tag vocabulary (exportExt.pdb), which has no\n"
         "    anonymizer yet and so is left out rather than sent as it is.\n\n"
         "Everything above is CHECKED, not just intended: this export was\n"
         "read back through the app's own readers and every analysis file\n"
         "was inspected before the zip was written. Had anything still\n"
         "held real data, no file would have been produced.\n\n";

    // Raw only. This file is written before the zip exists and ends up
    // inside it, so the real compressed size cannot be stated here --
    // and the fixed-ratio guess that used to stand in its place was off
    // by more than double on a slimmed export (2.5 MB predicted, 1.1 MB
    // actual). A number that wrong is worse than no number; the app and
    // the CLI both report the real one once the file exists.
    m << "Output size: " << humanSize(summary.outputSizeBytes) << " before compression ("
      << summary.filesWritten << " files).\n\n";

    m << "Nothing has been sent anywhere; this only wrote a single zip\n"
         "file. Review its contents, then attach that zip to an email to\n"
         "sebas@kde.org if you'd like to help test against your\n"
         "hardware/library. This dataset may be published as part of the\n"
         "project's test suite. If there's anything in the hardware or\n"
         "notes text above you'd rather not have published, leave it out\n"
         "here and mention it directly in your email instead. If this is\n"
         "too large to attach, re-run with --max-tracks to include a\n"
         "smaller sample.\n";

    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    out << m.str();
}

}  // namespace

bool AnonymizationSummary::succeeded() const
{
    bool anyAttempted = rekordboxAttempted || engineAttempted;
    bool anyFailed = (rekordboxAttempted && !rekordboxError.empty()) || (engineAttempted && !engineError.empty());
    // A failed verification is a failed export: no zip was written.
    return anyAttempted && !anyFailed && !verificationFailed && outputError.empty();
}

AnonymizationSummary AnonymizeLibrary::execute(const std::optional<std::string> &rekordboxRoot,
                                                const std::optional<std::string> &engineRoot,
                                                const std::string &outputDir, const AnonymizationOptions &options,
                                                ProgressReporter &reporter)
{
    AnonymizationSummary summary;

    // The staging directory is created here and removed at the end, so it
    // must be ours from the start. A user who typed an existing folder as
    // the output ("/home/me/Music", with the .zip stripped by the GUI)
    // would otherwise get that folder zipped and deleted. Refuse anything
    // that already holds content, anything inside a library being read,
    // and anything that would swallow one.
    {
        std::error_code ec;
        const auto normalized = [&ec](const fs::path &p) { return fs::absolute(p, ec).lexically_normal().string(); };
        const auto contains = [](const std::string &outer, const std::string &inner) {
            return outer == inner || pathIsUnder(inner, outer);
        };
        const std::string out = normalized(fs::path(outputDir));
        for (const auto &source : {rekordboxRoot, engineRoot}) {
            if (!source) {
                continue;
            }
            const std::string catalog = normalized(fs::path(*source));
            if (contains(catalog, out) || contains(out, catalog)) {
                summary.outputError = "the output location " + outputDir + " overlaps the library being read (" + *source
                                      + "); pick a folder of its own";
                return summary;
            }
        }
        if (fs::exists(out, ec)) {
            if (!fs::is_directory(out, ec)) {
                summary.outputError = "the output location " + outputDir + " exists and is not a directory";
                return summary;
            }
            if (!fs::is_empty(out, ec)) {
                summary.outputError = "the output directory " + outputDir + " already exists and is not empty; refusing to "
                                      "use it, since it would be removed when the zip is written";
                return summary;
            }
        }
    }

    fs::create_directories(outputDir);

    if (rekordboxRoot) {
        summary.rekordboxAttempted = true;
        auto result = infrastructure::rekordbox::anonymizeRekordboxLibrary(
            *rekordboxRoot, (fs::path(outputDir) / "rekordbox").string(), options.maxTracks,
            options.slimForTesting, reporter);
        summary.rekordboxTracksKept = result.tracksKept;
        summary.rekordboxTracksDropped = result.tracksDropped;
        summary.rekordboxArtistsRenamed = result.artistsRenamed;
        summary.rekordboxPlaylistsRenamed = result.playlistsRenamed;
        summary.rekordboxError = result.errorMessage;
    }

    if (engineRoot) {
        summary.engineAttempted = true;
        auto result = infrastructure::engine::anonymizeEngineLibrary(
            *engineRoot, (fs::path(outputDir) / "engine").string(), options.maxTracks, options.slimForTesting,
            reporter);
        summary.engineTracksKept = result.tracksKept;
        summary.engineTracksDropped = result.tracksDropped;
        summary.enginePlaylistsRenamed = result.playlistsRenamed;
        summary.engineTracksRefused = result.tracksRefused;
        summary.engineFirstRefusalReason = result.firstRefusalReason;
        summary.engineError = result.errorMessage;
    }

    std::uintmax_t rekordboxBytes = infrastructure::directoryTreeSizeBytes(fs::path(outputDir) / "rekordbox");
    std::uintmax_t engineBytes = infrastructure::directoryTreeSizeBytes(fs::path(outputDir) / "engine");
    summary.outputSizeBytes = rekordboxBytes + engineBytes;
    std::error_code countEc;
    for (const auto &entry : fs::recursive_directory_iterator(outputDir, countEc)) {
        if (entry.is_regular_file(countEc)) {
            ++summary.filesWritten;
        }
    }

    // The stick root is the parent of whichever catalog directory was
    // given; both live directly under it.
    if (rekordboxRoot || engineRoot) {
        const fs::path anyCatalog(rekordboxRoot ? *rekordboxRoot : *engineRoot);
        writeFileListing(fs::path(outputDir) / "files.tsv", anyCatalog.parent_path(), summary.audioFilesListed);
    }

    summary.manifestPath = (fs::path(outputDir) / "MANIFEST.txt").string();
    writeManifest(summary.manifestPath, summary, options);

    // Captured before the staging directory is removed below -- once
    // execute() returns, MANIFEST.txt only exists inside outputZipPath,
    // not as a standalone file a caller could read back off disk.
    {
        std::ifstream manifestIn(summary.manifestPath, std::ios::binary);
        std::ostringstream manifestContent;
        manifestContent << manifestIn.rdbuf();
        summary.manifestText = manifestContent.str();
    }

    // outputDir was only ever a staging area -- the actual deliverable
    // is one zip file, not a directory tree the user has to remember to
    // zip themselves before emailing it. fs::path's own manipulation
    // (rather than raw string/separator surgery, which would need a
    // separate code path for Windows' wide path::value_type) keeps this
    // portable: a trailing separator makes filename() report empty, so
    // that case falls back to parent_path() first, then += appends
    // ".zip" without inserting a fresh separator.
    // The last thing before the zip exists: check that this export is
    // what the manifest above just promised it is. Everything the
    // anonymizers were supposed to scrub gets sampled back through the
    // app's own readers, and every analysis file's embedded path is read
    // in full, because that is where the leak was -- all 2744 of them
    // still held the real artist, album and title while the manifest
    // said original file paths were removed entirely.
    //
    // A failure refuses to write the zip at all. Producing the file and
    // describing the problem afterwards would leave a leaking export on
    // disk with a manifest inside it saying otherwise, which is exactly
    // the thing to avoid.
    auto verification = infrastructure::verifyAnonymizedExport(outputDir);
    if (!verification.ok) {
        summary.verificationFailed = true;
        summary.verificationReport = verification.describe();
        infrastructure::removeTreeDeepestFirst(outputDir);
        return summary;
    }

    // Last thing before zipping: SQLite's side files, recreated by the
    // verification above when it read the OneLibrary mirror back. Nothing
    // opens the database after this point, so nothing recreates them.
    for (const char *sideFile : {"exportLibrary.db-shm", "exportLibrary.db-wal"}) {
        std::error_code sideEc;
        fs::remove(fs::path(outputDir) / "rekordbox" / "rekordbox" / sideFile, sideEc);
    }

    fs::path zipPath(outputDir);
    if (zipPath.filename().empty()) {
        zipPath = zipPath.parent_path();
    }
    zipPath += ".zip";
    infrastructure::writeZipArchive(outputDir, zipPath);
    summary.outputZipPath = zipPath.string();
    std::error_code sizeEc;
    summary.finalZipBytes = fs::file_size(zipPath, sizeEc);

    // Not fs::remove_all: outputDir mirrors a real rekordbox/Engine
    // library, so it can hold a path past MAX_PATH, and remove_all never
    // returns on one -- it spins instead of reporting that it is stuck.
    // See infrastructure/long_paths.hpp.
    infrastructure::removeTreeDeepestFirst(outputDir);

    return summary;
}

}  // namespace seabass::application
