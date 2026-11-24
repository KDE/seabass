// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// How far apart beat grids are (docs/track-analysis-plan.md, phase 1).
//
//   analysis_benchmark libraries <stick root> [--tsv out.tsv] [--audio-root DIR] [--scratch DIR]
//
// For every track a stick's rekordbox export and its Engine library both
// hold (paired as Copy Beat Grids pairs them) and both have a grid for:
// the rekordbox grid is put on Engine's timeline with the player's
// lead-in code (playback_timeline.hpp) and the Engine grid is compared
// with it (domain/beat_grid_comparison.hpp), Engine the candidate and
// rekordbox the reference, so an offset is Engine's beat minus
// rekordbox's. One TSV line per track, a summary per file format and per
// tempo band on stdout. The "agreed" tracks are those where the two
// analysers agree well enough to trust either: median offset within
// 3 ms, the same tempo, both grids constant.
//
// Reads only. The stick's SQLite databases are copied to a scratch
// folder first and opened there, never where they are: opening one in
// place can delete its journal. Everything else (the analysis files,
// the audio) is reached through links from the scratch folder. The lead-in
// of an MP3 is read from the audio file under --audio-root (the stick
// root by default); where the file is not there, the row is compared on
// rekordbox's own timeline and says so.
//
//   analysis_benchmark synth [--preset NAME|all] [--shift-ms X] [--check] [--beats DIR]
//
// Renders synthetic tracks (tools/synth_track) and runs every registered
// analyser over them through the GridAnalyser port, scored against the
// true grid. No real analyser exists yet; the one registered is an
// oracle that answers the true grid moved by --shift-ms, which checks
// this tool's own arithmetic: every line must show exactly that shift
// with no scatter, on constant and moving grids alike. --check makes that
// the exit code.
//
// With -DSEABASS_ANALYSIS_BASELINE=ON the baseline analysers of phase 2
// (tools/analysis_baseline: Mixxx's method on qm-dsp) are registered as
// well: qm-raw, mixxx-constant and mixxx-constant-unrounded for the grid,
// qm-key for the key, which synth mode scores against the preset's key.
// --analyser picks some of them; the default is all, the oracle included.
// --tempo-range LOW-HIGH (in BPM, in the synth and the audio mode) is
// the range Seabass's own analyser gives its tempos in, rekordbox's
// "BPM range": the default is rekordbox's default, 70-180.
//
//   analysis_benchmark audio <stick root> --analyser NAME[,NAME]|all [--audio-root DIR] [--limit N]
//                      [--half tuning|heldback|all] [--tsv out.tsv] [--jobs N] [--scratch DIR]
//                      [--beats DIR]
//
// --beats DIR, in both: every grid that was compared is written there as
// <track>.<analyser>.tsv, and the one it was compared against as
// <track>.reference.tsv, a beat a line (ms, place in the bar), for
// looking at two grids side by side. A real track's grid is private data
// like its audio: DIR is a place outside the repository.
//
// Built with the baseline where Qt Multimedia is there. Every rekordbox
// track with a grid whose audio file is under --audio-root (the stick
// root by default) is decoded (QtMultimediaPcmSource, Engine's timeline)
// and analysed, and each grid is compared with rekordbox's, put on
// Engine's timeline as the libraries mode puts it. The catalogs are read
// from a scratch copy, as there. A track whose Engine row has a grid is
// in the "agreed subset" when Engine and rekordbox agree by the libraries
// mode's rule. Key analysers are compared with rekordbox's and Engine's
// stored keys and classed (same, relative, fifth up or down, parallel,
// other).
//
// The library is split once into a tuning half and a held-back half by
// a hash of each track's path as rekordbox's export names it (pathHash
// below): an analyser is tuned on the first and judged on the second, so
// --half defaults to tuning. --limit takes the first N tracks in hash
// order, a stable sample. --jobs tracks are worked on at once (three is
// the most a 32 GB machine has stood). The TSV (one line per track and analyser) names files; keep
// it out of the repository. The summary names none.
//
// In place of a stick root the audio mode takes a list, a tab-separated
// file with one track a line: an id, the audio file, the track's ANLZ
// .DAT file, rekordbox's key for it. That is how a rekordbox collection
// on a computer is measured, which has no export: its tracks, analysis
// files and keys are in its own database (list it with any tool that
// reads that). No Engine side then, so no agreed subset.
//
//   analysis_benchmark grids <dir> <name> <reference dir> --tsv out.tsv [--reference NAME]
//
// Beat lists against beat lists, with no audio: every <track>.<name>.tsv
// in <dir> is compared with <track>.reference.tsv (or .NAME.tsv) in
// <reference dir>, by the same measure and into the same TSV as the audio
// mode (its columns about the file and the decode are "-" or 0, and
// `half` says nothing: take it from an audio run's TSV). The lists are
// what --beats writes. For grids that were not made here (Engine DJ's
// own, another tracker's) and for one reference against another.
//
// The TSV is written as the tracks finish, in the order they finish, so
// a run that is stopped keeps what it had. Next to the offset each track
// has a drift (domain/beat_grid_comparison.hpp), and the summary counts
// the tracks half a beat out.
//
// Exit code 0 on success, 1 when something could not be read or a check
// failed, 2 for a usage error.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/use_cases/copy_beat_grids.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/beat_grid.hpp"
#include "domain/beat_grid_comparison.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "domain/grid_analyser.hpp"
#include "domain/mp3_lead_in.hpp"
#include "domain/pcm_source.hpp"
#include "domain/track.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "synth_track/synth_track.hpp"

#ifdef SEABASS_ANALYSIS_BASELINE
#include "analysis_baseline/baseline_analysers.hpp"
#include "domain/camelot_key.hpp"
#endif
#ifdef SEABASS_ANALYSIS_AUDIO
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QThread>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numeric>

#include "application/use_cases/plan_engine_update.hpp"
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#endif

#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using domain::BeatGridComparison;

namespace
{

int usage()
{
    std::fprintf(stderr,
                 "usage: analysis_benchmark libraries <stick root> [--tsv out.tsv] [--audio-root DIR] [--scratch DIR]\n"
                 "       analysis_benchmark synth [--preset NAME|all] [--analyser NAME[,NAME]|all] [--shift-ms X] [--check]\n"
                 "                          [--beats DIR] [--tempo-range LOW-HIGH]\n"
#ifdef SEABASS_ANALYSIS_AUDIO
                 "       analysis_benchmark audio <stick root|track list.tsv> --analyser NAME[,NAME]|all [--audio-root DIR]\n"
                 "                          [--limit N] [--half tuning|heldback|all] [--tsv out.tsv] [--jobs N]\n"
                 "                          [--scratch DIR] [--beats DIR] [--tempo-range LOW-HIGH]\n"
                 "       analysis_benchmark grids <dir> <name> <reference dir> --tsv out.tsv [--reference NAME]\n"
#endif
                 );
    return 2;
}

// --- The scratch root ---

// A folder that looks like the stick to the readers: the SQLite files of
// both catalogs copied in, everything else linked. Removed on
// destruction.
class ScratchRoot
{
public:
    ScratchRoot(const fs::path &stick, const fs::path &parent)
    {
        std::random_device seed;
        for (int attempt = 0; attempt < 100 && m_root.empty(); ++attempt) {
            const fs::path candidate = parent / ("analysis-benchmark-" + std::to_string(seed()));
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                m_root = candidate;
            }
        }
        if (m_root.empty()) {
            throw std::runtime_error("cannot make a scratch folder in " + pathToUtf8(parent));
        }
        mirror(stick, m_root, 0);
    }
    ~ScratchRoot()
    {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }
    ScratchRoot(const ScratchRoot &) = delete;
    ScratchRoot &operator=(const ScratchRoot &) = delete;

    const fs::path &root() const { return m_root; }
    int copiedFiles() const { return m_copied; }

private:
    // The catalogs' own folders are walked (to copy their files); any
    // other entry is a link to the original.
    static bool isCatalogFolder(const fs::path &relative)
    {
        const std::string r = relative.generic_string();
        return r == "PIONEER" || r == "PIONEER/rekordbox" || r == "Engine Library" || r == "Engine Library/Database2";
    }

    void mirror(const fs::path &from, const fs::path &to, int depth)
    {
        for (const fs::directory_entry &entry : fs::directory_iterator(from)) {
            const fs::path name = entry.path().filename();
            const fs::path relative = (to / name).lexically_relative(m_root);
            if (entry.is_directory() && isCatalogFolder(relative)) {
                fs::create_directory(to / name);
                mirror(entry.path(), to / name, depth + 1);
            } else if (entry.is_regular_file() && depth > 0 && isCatalogFolder(relative.parent_path())) {
                // A catalog file: copied, SQLite or not, journals and all,
                // so that what is opened is the copy.
                fs::copy_file(entry.path(), to / name);
                ++m_copied;
            } else {
                fs::create_symlink(entry.path(), to / name);
            }
        }
    }

    fs::path m_root;
    int m_copied = 0;
};

// --- Summaries ---

double medianOf(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double quantileOf(std::vector<double> values, double q)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(rank, 1) - 1];
}

bool bothConstant(const BeatGridComparison &c)
{
    return c.candidateShape.kind == domain::GridShapeKind::Constant
        && c.referenceShape.kind == domain::GridShapeKind::Constant;
}

bool agreed(const BeatGridComparison &c)
{
    return std::abs(c.offset.medianMs) <= 3.0 && c.tempo == domain::TempoRelation::Same && bothConstant(c);
}

// Where a rekordbox grid lands on Engine's timeline: the shift the player
// gives it for the file's lead-in (an MP3's, read from the audio under
// `audioRoot`), and which case that was: mp3-corrected,
// mp3-lead-in-unknown (the grid stays on rekordbox's own timeline) or
// not-mp3.
struct TimelineShift
{
    std::string timeline;
    double shiftMs = 0.0;
};

TimelineShift rekordboxOnEngineTimeline(const std::string &filePath, const std::string &stickRelativePath,
                                        const fs::path &audioRoot)
{
    TimelineShift shift;
    if (!domain::hasMp3Extension(filePath)) {
        shift.timeline = "not-mp3";
        return shift;
    }
    const fs::path audio = audioRoot / pathFromUtf8(stickRelativePath);
    const auto leadIn = stickRelativePath.empty() ? std::optional<domain::Mp3LeadIn>{}
                                                  : infrastructure::audio::readMp3LeadIn(pathToUtf8(audio));
    if (leadIn && leadIn->sampleRate > 0) {
        shift.timeline = "mp3-corrected";
        shift.shiftMs =
            infrastructure::audio::playbackShiftFor(leadIn, true, infrastructure::audio::GridTimeline::Rekordbox).beatsMs;
    } else {
        shift.timeline = "mp3-lead-in-unknown";
    }
    return shift;
}

std::vector<domain::Beat> shifted(std::vector<domain::Beat> beats, double shiftMs)
{
    for (domain::Beat &beat : beats) {
        beat.timeMs += shiftMs;
    }
    return beats;
}

struct Row
{
    std::string rekordboxId;
    std::string engineId;
    std::string file;
    std::string format;
    // The Engine row carries the mark of Engine's rekordbox import
    // (thirdPartySourceId 1). Reported, not acted on: measured on a real
    // library, rows with the mark and a grid read like the player's own
    // analysis, not like a copy of rekordbox's grid.
    bool importMark = false;
    // mp3-corrected, mp3-lead-in-unknown or not-mp3.
    std::string timeline;
    double shiftMs = 0.0;
    BeatGridComparison comparison;
};

void printSummaryHeader(const char *by)
{
    std::printf("%-26s %5s %8s %7s %7s %6s %6s %6s %6s %7s %6s %6s %6s %6s\n", by, "n", "median", "p25", "p75",
                "<=1ms", "<=3ms", "<=8ms", "<=20ms", "scatter", "same", "bar0", "const", "agreed");
}

void printSummaryLine(const std::string &label, const std::vector<const Row *> &rows)
{
    if (rows.empty()) {
        return;
    }
    std::vector<double> medians;
    std::vector<double> scatters;
    int within1 = 0, within3 = 0, within8 = 0, within20 = 0, same = 0, bar0 = 0, constant = 0, agree = 0;
    for (const Row *row : rows) {
        const BeatGridComparison &c = row->comparison;
        medians.push_back(c.offset.medianMs);
        scatters.push_back(c.offset.madMs);
        const double m = std::abs(c.offset.medianMs);
        within1 += m <= 1.0 ? 1 : 0;
        within3 += m <= 3.0 ? 1 : 0;
        within8 += m <= 8.0 ? 1 : 0;
        within20 += m <= 20.0 ? 1 : 0;
        same += c.tempo == domain::TempoRelation::Same ? 1 : 0;
        bar0 += c.bar.known && c.bar.beats == 0 ? 1 : 0;
        constant += bothConstant(c) ? 1 : 0;
        agree += agreed(c) ? 1 : 0;
    }
    const double n = static_cast<double>(rows.size());
    const auto pct = [n](int count) { return 100.0 * count / n; };
    std::printf("%-26s %5zu %+8.2f %+7.2f %+7.2f %5.1f%% %5.1f%% %5.1f%% %5.1f%% %7.2f %5.1f%% %5.1f%% %5.1f%% %6d\n",
                label.c_str(), rows.size(), medianOf(medians), quantileOf(medians, 0.25), quantileOf(medians, 0.75),
                pct(within1), pct(within3), pct(within8), pct(within20), medianOf(scatters), pct(same), pct(bar0),
                pct(constant), agree);
}

std::string bpmBand(double bpm)
{
    if (bpm <= 0.0) {
        return "no tempo";
    }
    static constexpr double Edges[] = {90.0, 110.0, 120.0, 125.0, 130.0, 140.0, 160.0};
    double low = 0.0;
    for (const double edge : Edges) {
        if (bpm < edge) {
            char text[32];
            std::snprintf(text, sizeof text, "%3.0f to %3.0f BPM", low, edge);
            return text;
        }
        low = edge;
    }
    return "160 BPM and up";
}

std::string formatOf(const std::string &filePath)
{
    std::string ext = pathToUtf8(pathFromUtf8(filePath).extension());
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext.empty() ? "none" : ext;
}

void writeTsv(const std::string &path, const std::vector<Row> &rows)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
    out << "rekordbox_id\tengine_id\tformat\timport_mark\ttimeline\tshift_ms\trekordbox_beats\tengine_beats\t"
           "matched\tmedian_ms\tscatter_ms\tp95_abs_ms\tmax_abs_ms\thit_1ms\thit_3ms\thit_8ms\thit_20ms\ttempo\t"
           "tempo_ratio\tbar_turn\tbar_share\tcover\trekordbox_shape\trekordbox_bpm\trekordbox_runs\tengine_shape\t"
           "engine_bpm\tengine_runs\tagreed\tfile\n";
    char line[1024];
    for (const Row &row : rows) {
        const BeatGridComparison &c = row.comparison;
        std::snprintf(line, sizeof line,
                      "%s\t%s\t%s\t%s\t%s\t%.3f\t%zu\t%zu\t%zu\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%.5f\t"
                      "%s\t%.3f\t%.4f\t%s\t%.3f\t%d\t%s\t%.3f\t%d\t%s\t",
                      row.rekordboxId.c_str(), row.engineId.c_str(), row.format.c_str(), row.importMark ? "yes" : "no",
                      row.timeline.c_str(), row.shiftMs, c.referenceBeats, c.candidateBeats, c.matchedBeats,
                      c.offset.medianMs, c.offset.madMs, c.offset.p95AbsMs, c.offset.maxAbsMs, c.hits.within1Ms,
                      c.hits.within3Ms, c.hits.within8Ms, c.hits.within20Ms, domain::tempoRelationName(c.tempo),
                      c.tempoRatio, c.bar.known ? std::to_string(c.bar.beats).c_str() : "-", c.bar.share, c.cover,
                      domain::gridShapeKindName(c.referenceShape.kind), c.referenceShape.bpm,
                      c.referenceShape.straightRuns, domain::gridShapeKindName(c.candidateShape.kind),
                      c.candidateShape.bpm, c.candidateShape.straightRuns, agreed(c) ? "yes" : "no");
        out << line << row.file << "\n";
    }
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
}

int librariesMode(int argc, char **argv)
{
    if (argc < 3) {
        return usage();
    }
    const fs::path stick = pathFromUtf8(argv[2]);
    std::string tsvPath;
    fs::path audioRoot = stick;
    fs::path scratchParent = fs::temp_directory_path();
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage();
        }
        if (arg == "--tsv") {
            tsvPath = argv[++i];
        } else if (arg == "--audio-root") {
            audioRoot = pathFromUtf8(argv[++i]);
        } else if (arg == "--scratch") {
            scratchParent = pathFromUtf8(argv[++i]);
        } else {
            return usage();
        }
    }
    if (!fs::exists(stick / "PIONEER" / "rekordbox" / "export.pdb") || !fs::exists(stick / "Engine Library")) {
        std::fprintf(stderr, "%s holds no rekordbox export and Engine library\n", argv[2]);
        return 1;
    }

    std::vector<Row> rows;
    int rekordboxTracks = 0, engineTracks = 0, paired = 0, rekordboxOnly = 0, ambiguous = 0;
    int noRekordboxGrid = 0, noEngineGrid = 0, unreadableRow = 0;
    try {
        const ScratchRoot scratch(stick, scratchParent);
        const fs::path root = scratch.root();
        const fs::path pioneer = root / "PIONEER";
        const fs::path engineLibrary = root / "Engine Library";
        std::printf("scratch copy: %d catalog file(s) copied\n", scratch.copiedFiles());

        infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pathToUtf8(pioneer));
        const std::vector<domain::Track> rekordbox = application::ScanLibrary(rekordboxReader).execute();
        rig::EngineReaderAtFileRates engineReader(engineLibrary);
        const std::vector<domain::Track> engine = application::ScanLibrary(engineReader.reader()).execute();
        rekordboxTracks = static_cast<int>(rekordbox.size());
        engineTracks = static_cast<int>(engine.size());

        const application::BeatGridCopyPlan plan = application::planBeatGridCopy(rekordbox, engine, pathToUtf8(root));
        paired = static_cast<int>(plan.items.size());
        rekordboxOnly = plan.rekordboxOnly;
        ambiguous = plan.ambiguous;

        djinterop::database engineDb = djinterop::engine::load_database(pathToUtf8(engineLibrary));
        fs::path databaseFile = engineLibrary / "Database2" / "m.db";
        if (!fs::exists(databaseFile)) {
            databaseFile = engineLibrary / "m.db";
        }
        infrastructure::engine::EngineAnalysisRowAccess analysisRows(pathToUtf8(databaseFile));

        for (const application::BeatGridCopyItem &item : plan.items) {
            const std::vector<domain::Beat> rekordboxGrid =
                infrastructure::rekordbox::readBeatGrid(pathToUtf8(pioneer), item.rekordboxTrackId);
            std::vector<domain::Beat> engineGrid;
            std::string markError;
            std::optional<infrastructure::engine::EngineCueBlobs> blobs;
            try {
                const std::int64_t id = std::stoll(item.engineTrackId);
                if (const auto row = engineDb.track_by_id(id)) {
                    engineGrid = infrastructure::engine::beatGridOf(*row);
                }
                blobs = analysisRows.readCueBlobs(id, &markError);
            } catch (const std::exception &) {
                ++unreadableRow;
                continue;
            }
            if (rekordboxGrid.empty()) {
                ++noRekordboxGrid;
                continue;
            }
            if (engineGrid.empty()) {
                ++noEngineGrid;
                continue;
            }
            Row row;
            row.rekordboxId = item.rekordboxTrackId;
            row.engineId = item.engineTrackId;
            row.file = item.stickRelativePath;
            row.format = formatOf(item.filePath);
            row.importMark = blobs && blobs->rekordboxImportMark;

            // rekordbox's grid onto Engine's timeline, as the player puts
            // it there: the file's lead-in, read from the audio.
            const TimelineShift shift = rekordboxOnEngineTimeline(item.filePath, item.stickRelativePath, audioRoot);
            row.timeline = shift.timeline;
            row.shiftMs = shift.shiftMs;
            const std::vector<domain::Beat> reference = shifted(rekordboxGrid, row.shiftMs);
            row.comparison = domain::compareBeatGrids(engineGrid, reference);
            rows.push_back(std::move(row));
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::printf("rekordbox tracks %d, Engine tracks %d, paired %d (rekordbox only %d, ambiguous %d)\n",
                rekordboxTracks, engineTracks, paired, rekordboxOnly, ambiguous);
    std::printf("paired but not compared: no rekordbox grid %d, no Engine grid %d, Engine row unreadable %d\n",
                noRekordboxGrid, noEngineGrid, unreadableRow);
    std::printf("compared %zu: offsets are Engine minus rekordbox (on Engine's timeline where the lead-in is known), "
                "in ms\n\n",
                rows.size());

    // Engine's analysis against rekordbox's, by file format and
    // timeline, by tempo, and by the import's mark.
    std::map<std::string, std::vector<const Row *>> byFormat;
    std::map<std::string, std::vector<const Row *>> byBand;
    std::map<std::string, std::vector<const Row *>> byMark;
    std::vector<const Row *> all;
    for (const Row &row : rows) {
        all.push_back(&row);
        byFormat[row.format + " " + row.timeline].push_back(&row);
        byBand[bpmBand(row.comparison.referenceShape.kind == domain::GridShapeKind::Constant
                           ? row.comparison.referenceShape.bpm
                           : row.comparison.referenceShape.highestBpm)]
            .push_back(&row);
        byMark[row.importMark ? "import mark" : "no import mark"].push_back(&row);
    }
    std::printf("per track; median, p25, p75: of the per-track median offset; <=Nms: tracks whose median offset\n"
                "is within N ms; scatter: median of the per-track scatter; same: same tempo; bar0: bar not\n"
                "turned; const: both grids constant; agreed: within 3 ms, same tempo, both constant\n\n");
    printSummaryHeader("format, timeline");
    for (const auto &[label, group] : byFormat) {
        printSummaryLine(label, group);
    }
    printSummaryLine("all", all);
    std::printf("\n");
    printSummaryHeader("rekordbox tempo");
    for (const auto &[label, group] : byBand) {
        printSummaryLine(label, group);
    }
    std::printf("\n");
    printSummaryHeader("Engine row");
    for (const auto &[label, group] : byMark) {
        printSummaryLine(label, group);
    }

    std::map<long, int> histogram;
    int movingRekordbox = 0;
    int movingEngine = 0;
    for (const Row *row : all) {
        ++histogram[std::lround(row->comparison.offset.medianMs)];
        movingRekordbox += row->comparison.referenceShape.kind == domain::GridShapeKind::Moving ? 1 : 0;
        movingEngine += row->comparison.candidateShape.kind == domain::GridShapeKind::Moving ? 1 : 0;
    }
    std::printf("\nmoving grids: rekordbox %d, Engine %d\n", movingRekordbox, movingEngine);
    std::printf("per-track median offset, rounded to the ms:\n");
    for (const auto &[ms, count] : histogram) {
        std::printf("  %+5ld ms  %d\n", ms, count);
    }

    if (!tsvPath.empty()) {
        try {
            writeTsv(tsvPath, rows);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
        std::printf("\n%zu line(s) written to %s\n", rows.size(), tsvPath.c_str());
    }
    return 0;
}

// --- Synthetic tracks ---

// The true grid, moved by a set amount: what a perfect analyser with a
// constant bias would answer. It reads the whole source, as an analyser
// would, so that the port is exercised and a source that fails is
// reported.
class OracleAnalyser : public domain::GridAnalyser
{
public:
    OracleAnalyser(std::vector<domain::Beat> truth, double shiftMs) : m_truth(std::move(truth)), m_shiftMs(shiftMs) {}
    std::string name() const override { return "oracle"; }
    domain::GridAnalysis analyse(domain::PcmSource &source) override
    {
        domain::GridAnalysis analysis;
        const std::vector<float> samples = domain::readAllSamples(source);
        if (!source.error().empty() || samples.empty()) {
            analysis.error = source.error().empty() ? "no samples" : source.error();
            return analysis;
        }
        analysis.beats = m_truth;
        for (domain::Beat &beat : analysis.beats) {
            beat.timeMs += m_shiftMs;
        }
        analysis.confidence = 1.0;
        return analysis;
    }

private:
    std::vector<domain::Beat> m_truth;
    double m_shiftMs;
};

// The analysers a run asked for (--analyser a,b or "all"), in that
// order. The oracle needs the truth, so only synth mode offers it.
std::optional<std::vector<std::string>> parseAnalyserList(const std::string &list, bool withOracle)
{
    std::vector<std::string> known;
    if (withOracle) {
        known.push_back("oracle");
    }
    known.push_back("seabass");
#ifdef SEABASS_ANALYSIS_BASELINE
    for (const std::string &name : baseline::baselineGridAnalyserNames()) {
        known.push_back(name);
    }
    for (const std::string &name : baseline::baselineKeyAnalyserNames()) {
        known.push_back(name);
    }
#endif
    if (list == "all") {
        return known;
    }
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (std::find(known.begin(), known.end(), name) == known.end()) {
            std::fprintf(stderr, "no analyser %s; this build has:", name.c_str());
            for (const std::string &k : known) {
                std::fprintf(stderr, " %s", k.c_str());
            }
            std::fprintf(stderr, "\n");
            return std::nullopt;
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

// The tempo range Seabass's own analyser is run with (--tempo-range).
domain::TempoRange seabassTempoRange;

// "88-175" as a range. False for anything else.
bool parseTempoRange(const std::string &text, domain::TempoRange &range)
{
    const std::size_t dash = text.find('-');
    if (dash == std::string::npos || dash == 0) {
        return false;
    }
    char *end = nullptr;
    const std::string low = text.substr(0, dash);
    const std::string high = text.substr(dash + 1);
    const double lowBpm = std::strtod(low.c_str(), &end);
    if (end == low.c_str() || *end != '\0') {
        return false;
    }
    const double highBpm = std::strtod(high.c_str(), &end);
    if (end == high.c_str() || *end != '\0' || !(lowBpm > 0.0) || !(highBpm > lowBpm) || !std::isfinite(highBpm)) {
        return false;
    }
    range = domain::TempoRange{lowBpm, highBpm};
    return true;
}

// Says which range Seabass's own analyser runs with, as the analyser made
// it valid: a run's numbers are that range's numbers.
void printTempoRange(const std::vector<std::string> &analyserNames)
{
    if (std::find(analyserNames.begin(), analyserNames.end(), "seabass") == analyserNames.end()) {
        return;
    }
    const domain::TempoRange range = domain::SeabassGridAnalyser(seabassTempoRange).tempoRange();
    std::printf("seabass: tempo range %g to %g BPM\n", range.lowBpm, range.highBpm);
}

// A grid analyser by its name: Seabass's own, or one of the baseline's
// where the build has them. Null for a name that is neither.
std::unique_ptr<domain::GridAnalyser> makeGridAnalyser(const std::string &name)
{
    if (name == "seabass") {
        return std::make_unique<domain::SeabassGridAnalyser>(seabassTempoRange);
    }
#ifdef SEABASS_ANALYSIS_BASELINE
    return baseline::makeBaselineGridAnalyser(name);
#else
    return nullptr;
#endif
}

bool isKeyAnalyser(const std::string &name)
{
#ifdef SEABASS_ANALYSIS_BASELINE
    const std::vector<std::string> keys = baseline::baselineKeyAnalyserNames();
    return std::find(keys.begin(), keys.end(), name) != keys.end();
#else
    (void)name;
    return false;
#endif
}

#ifdef SEABASS_ANALYSIS_BASELINE
// How a key an analyser found relates to a reference key, by tonic and
// mode: the same key, its relative (same notes, other mode), a fifth up
// or down in the same mode (one step around the Camelot wheel), its
// parallel (same tonic, other mode), something else, or unknown when
// either key does not parse.
const char *keyClass(const std::string &found, const std::string &reference)
{
    const auto a = domain::CamelotKey::parse(found);
    const auto b = domain::CamelotKey::parse(reference);
    if (!a || !b) {
        return "unknown";
    }
    const int up = ((a->number - b->number) % 12 + 12) % 12;
    if (a->isMinor == b->isMinor) {
        return up == 0 ? "same" : up == 1 ? "fifth-up" : up == 11 ? "fifth-down" : "other";
    }
    if (up == 0) {
        return "relative";
    }
    // A minor key sits three steps anticlockwise of its parallel major
    // (C major 8B, C minor 5A).
    if ((a->isMinor && up == 9) || (!a->isMinor && up == 3)) {
        return "parallel";
    }
    return "other";
}
#endif

// The mean tempo of a beat list, first beat to last; 0 with fewer than
// two beats.
double meanBpm(const std::vector<domain::Beat> &beats)
{
    if (beats.size() < 2 || beats.back().timeMs <= beats.front().timeMs) {
        return 0.0;
    }
    return 60000.0 * static_cast<double>(beats.size() - 1) / (beats.back().timeMs - beats.front().timeMs);
}

#ifdef SEABASS_ANALYSIS_BASELINE
// A preset's key ("A minor", "D major") as CamelotKey::parse reads keys.
std::string synthKeyName(const synth::SynthTrackSpec &spec)
{
    const std::size_t space = spec.keyName.find(' ');
    const std::string tonic = spec.keyName.substr(0, space);
    return spec.minor ? tonic + "m" : tonic;
}
#endif

// One grid as a file of --beats: a beat a line, its time in ms and its
// place in the bar. Nothing when no directory was given.
void writeBeats(const fs::path &dir, const std::string &track, const std::string &grid,
                const std::vector<domain::Beat> &beats)
{
    if (dir.empty()) {
        return;
    }
    std::ofstream out(dir / pathFromUtf8(track + "." + grid + ".tsv"));
    char line[64];
    for (const domain::Beat &beat : beats) {
        std::snprintf(line, sizeof line, "%.3f\t%d\n", beat.timeMs, beat.beatInBar);
        out << line;
    }
}

// The directory of --beats, made if it is not there. False, with a line
// on stderr, when it cannot be.
bool makeBeatsDir(const fs::path &dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "cannot make %s\n", pathToUtf8(dir).c_str());
        return false;
    }
    return true;
}

int synthMode(int argc, char **argv)
{
    std::string preset = "all";
    double shiftMs = 0.0;
    bool checkMode = false;
    std::string analyserList = "all";
    fs::path beatsDir;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") {
            checkMode = true;
        } else if (arg == "--beats" && i + 1 < argc) {
            beatsDir = pathFromUtf8(argv[++i]);
            if (!makeBeatsDir(beatsDir)) {
                return 1;
            }
        } else if (arg == "--analyser" && i + 1 < argc) {
            analyserList = argv[++i];
        } else if (arg == "--tempo-range" && i + 1 < argc) {
            if (!parseTempoRange(argv[++i], seabassTempoRange)) {
                return usage();
            }
        } else if (arg == "--preset" && i + 1 < argc) {
            preset = argv[++i];
        } else if (arg == "--shift-ms" && i + 1 < argc) {
            char *end = nullptr;
            shiftMs = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !std::isfinite(shiftMs)) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    const auto analyserNames = parseAnalyserList(analyserList, true);
    if (!analyserNames) {
        return 2;
    }
    printTempoRange(*analyserNames);
    std::vector<std::string> names;
    if (preset == "all") {
        names = synth::synthPresetNames();
    } else if (synth::synthPreset(preset)) {
        names.push_back(preset);
    } else {
        std::fprintf(stderr, "no preset %s (synth_track --list names them)\n", preset.c_str());
        return 2;
    }

    std::printf("%-24s %-24s %-8s %5s %9s %8s %8s %8s %6s %6s %6s %6s %-6s %4s %5s %-9s %-9s %5s %8s %8s %9s\n",
                "preset", "analyser", "conf", "beats", "median", "scatter", "p95", "max", "<=1ms", "<=3ms", "<=8ms",
                "<=20ms", "tempo", "bar", "share", "truth", "found", "cover", "trueBPM", "foundBPM", "drift");
    std::printf("(trueBPM, foundBPM: the mean tempo, first beat to last; drift: how far the offset moves from\n"
                "the start of the track to its end, in ms, where the tempo is the same)\n");
    int failures = 0;
    for (const std::string &name : names) {
        const synth::SynthTrackSpec spec = *synth::synthPreset(name);
        const synth::SynthTrack track = synth::renderSynthTrack(spec);
        writeBeats(beatsDir, name, "reference", track.beats);
        std::vector<std::unique_ptr<domain::GridAnalyser>> analysers;
        for (const std::string &analyserName : *analyserNames) {
            if (analyserName == "oracle") {
                analysers.push_back(std::make_unique<OracleAnalyser>(track.beats, shiftMs));
            } else if (auto analyser = makeGridAnalyser(analyserName)) {
                analysers.push_back(std::move(analyser));
            }
#ifdef SEABASS_ANALYSIS_BASELINE
            else if (auto keyAnalyser = baseline::makeBaselineKeyAnalyser(analyserName)) {
                domain::InMemoryPcmSource source(track.samples, track.sampleRate);
                const baseline::KeyAnalysis key = keyAnalyser->analyse(source);
                if (!key.error.empty()) {
                    std::printf("%-24s %-24s failed: %s\n", name.c_str(), analyserName.c_str(), key.error.c_str());
                    ++failures;
                } else {
                    std::printf("%-24s %-24s key %s, the preset's %s: %s\n", name.c_str(), analyserName.c_str(),
                                key.key.empty() ? "none" : key.key.c_str(), spec.keyName.c_str(),
                                keyClass(key.key, synthKeyName(spec)));
                }
            }
#endif
        }
        for (const auto &analyser : analysers) {
            domain::InMemoryPcmSource source(track.samples, track.sampleRate);
            const domain::GridAnalysis analysis = analyser->analyse(source);
            if (!analysis.error.empty()) {
                std::printf("%-24s %-24s failed: %s\n", name.c_str(), analyser->name().c_str(), analysis.error.c_str());
                ++failures;
                continue;
            }
            const BeatGridComparison c = domain::compareBeatGrids(analysis.beats, track.beats);
            writeBeats(beatsDir, name, analyser->name(), analysis.beats);
            char drift[32] = "-";
            if (c.drift.known) {
                std::snprintf(drift, sizeof drift, "%+.3f", c.drift.ms);
            }
            std::printf("%-24s %-24s %8.3f %5zu %+9.4f %8.4f %8.4f %8.4f %6.3f %6.3f %6.3f %6.3f %-6s %4s %5.3f %-9s "
                        "%-9s %5.3f %8.3f %8.3f %9s\n",
                        name.c_str(), analyser->name().c_str(), analysis.confidence, c.candidateBeats,
                        c.offset.medianMs, c.offset.madMs, c.offset.p95AbsMs, c.offset.maxAbsMs, c.hits.within1Ms,
                        c.hits.within3Ms, c.hits.within8Ms, c.hits.within20Ms, domain::tempoRelationName(c.tempo),
                        c.bar.known ? std::to_string(c.bar.beats).c_str() : "-", c.bar.share,
                        domain::gridShapeKindName(c.referenceShape.kind),
                        domain::gridShapeKindName(c.candidateShape.kind), c.cover, meanBpm(track.beats),
                        meanBpm(analysis.beats), drift);
            if (checkMode && analyser->name() == "oracle") {
                // The oracle's answer is the truth moved by the shift: the
                // arithmetic must give back exactly that.
                const bool ok = std::abs(c.offset.medianMs - shiftMs) < 1e-6 && c.offset.madMs < 1e-6
                    && std::abs(c.offset.maxAbsMs - std::abs(shiftMs)) < 1e-6 && c.tempo == domain::TempoRelation::Same
                    && c.bar.known && c.bar.beats == 0 && c.bar.share == 1.0
                    && c.candidateShape.kind == c.referenceShape.kind && c.candidateBeats == track.beats.size()
                    && (std::abs(shiftMs) > 1.0 || c.hits.within1Ms == 1.0)
                    // A shift is no drift, on a constant grid and on a moving one.
                    && c.drift.known && std::abs(c.drift.ms) < 1e-6;
                if (!ok) {
                    std::printf("CHECK FAILED: %s: the oracle moved by %.4f ms does not come back as that\n",
                                name.c_str(), shiftMs);
                    ++failures;
                }
            }
        }
    }
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    if (checkMode) {
        std::printf("check passed: every preset's oracle came back as a shift of exactly %.4f ms\n", shiftMs);
    }
    return 0;
}

#ifdef SEABASS_ANALYSIS_AUDIO
// --- Real audio ---

// Which half of a library a track is in, so that an analyser is tuned on
// one half and judged on the other. Decided by the track's path as
// rekordbox's export names it (from the stick root, '/'-separated),
// hashed with 64-bit FNV-1a: it depends on nothing that changes between
// runs or machines (not the audio, not a database id, not the mount
// point), and the hash's lowest bit splits about evenly. Even is the
// tuning half, odd the held-back half.
std::uint64_t pathHash(const std::string &rekordboxPath)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : rekordboxPath) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

const char *halfOf(std::uint64_t hash)
{
    return (hash & 1u) == 0 ? "tuning" : "heldback";
}

// The beat grid of one ANLZ .DAT file, read by its path: the PQTZ
// section's beats, each with its place in the bar and its time in whole
// ms (all big-endian: the section's header length and beat count, then
// eight bytes a beat after the 24-byte header). Empty when the file is
// not there or holds no grid.
std::vector<domain::Beat> beatGridOfAnlzFile(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u16 = [&bytes](std::size_t at) { return static_cast<std::uint32_t>(bytes[at] << 8 | bytes[at + 1]); };
    const auto u32 = [&u16](std::size_t at) { return u16(at) << 16 | u16(at + 2); };
    std::vector<domain::Beat> beats;
    if (bytes.size() < 28 || std::string(bytes.begin(), bytes.begin() + 4) != "PMAI") {
        return beats;
    }
    std::size_t at = u32(4);
    while (at + 12 <= bytes.size()) {
        const std::uint32_t headerLength = u32(at + 4);
        const std::uint32_t sectionLength = u32(at + 8);
        if (sectionLength < 12 || at + sectionLength > bytes.size()) {
            break;
        }
        if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                        bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4) == "PQTZ" && headerLength >= 24) {
            const std::uint32_t count = u32(at + 20);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t entry = at + headerLength + 8u * i;
                if (entry + 8 > at + sectionLength) {
                    break;
                }
                domain::Beat beat;
                beat.beatInBar = static_cast<int>(u16(entry));
                beat.timeMs = static_cast<double>(u32(entry + 4));
                beats.push_back(beat);
            }
            break;
        }
        at += sectionLength;
    }
    return beats;
}

std::vector<std::string> tabFields(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

// One track to analyse: where its audio is, rekordbox's grid on Engine's
// timeline, and what Engine holds for it.
struct AudioJob
{
    std::string rekordboxId;
    std::string engineId;
    std::string file;
    std::string format;
    fs::path audio;
    std::uint64_t hash = 0;
    TimelineShift shift;
    std::vector<domain::Beat> reference;
    std::string rekordboxKey;
    std::string engineKey;
    // "yes" or "no" when Engine has a grid for the track (the libraries
    // mode's rule on Engine against rekordbox), "no-engine-grid"
    // otherwise.
    std::string agreed = "no-engine-grid";
};

struct AudioResult
{
    const AudioJob *job = nullptr;
    std::string analyser;
    bool key = false;
    std::string error;
    double decodeMs = 0.0;
    double analyseMs = 0.0;
    double durationS = 0.0;
    int sampleRate = 0;
    BeatGridComparison comparison;
    std::string keyFound;
};

double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Decodes one track and runs every analyser on it. The decoded samples
// are held once per track and each analyser reads its own copy, so the
// decode is not repeated and no analyser sees what another consumed.
std::vector<AudioResult> analyseJob(const AudioJob &job, const std::vector<std::string> &analyserNames,
                                    const fs::path &beatsDir)
{
    std::vector<AudioResult> results;
    writeBeats(beatsDir, job.rekordboxId, "reference", job.reference);
    const auto decodeStart = std::chrono::steady_clock::now();
    infrastructure::audio::QtMultimediaPcmSource decoder(pathToUtf8(job.audio));
    const int rate = decoder.sampleRate();
    std::vector<float> samples;
    if (rate > 0) {
        samples = domain::readAllSamples(decoder);
    }
    const double decodeMs = msSince(decodeStart);
    std::string decodeError = decoder.error();
    if (decodeError.empty() && samples.empty()) {
        decodeError = "no samples";
    }
    for (const std::string &name : analyserNames) {
        AudioResult result;
        result.job = &job;
        result.analyser = name;
        result.key = isKeyAnalyser(name);
        result.decodeMs = decodeMs;
        result.sampleRate = rate;
        result.durationS = rate > 0 ? static_cast<double>(samples.size()) / rate : 0.0;
        if (!decodeError.empty()) {
            result.error = "decode: " + decodeError;
            results.push_back(std::move(result));
            continue;
        }
        domain::InMemoryPcmSource source(samples, rate);
        const auto analyseStart = std::chrono::steady_clock::now();
        if (result.key) {
            const baseline::KeyAnalysis key = baseline::makeBaselineKeyAnalyser(name)->analyse(source);
            result.analyseMs = msSince(analyseStart);
            result.error = key.error;
            result.keyFound = key.key;
        } else {
            const domain::GridAnalysis grid = makeGridAnalyser(name)->analyse(source);
            result.analyseMs = msSince(analyseStart);
            result.error = grid.error;
            if (grid.error.empty()) {
                result.comparison = domain::compareBeatGrids(grid.beats, job.reference);
                writeBeats(beatsDir, job.rekordboxId, name, grid.beats);
            }
        }
        results.push_back(std::move(result));
    }
    return results;
}

// A per-track median offset means nothing where no reference beat met a
// candidate beat; such a track ranks as worst and counts as a miss.
double rankOffset(const BeatGridComparison &c)
{
    return c.offset.count == 0 ? std::numeric_limits<double>::infinity() : std::abs(c.offset.medianMs);
}

// The same tempo as rekordbox but the beats between rekordbox's: the
// track's median offset is more than three tenths of a beat.
bool halfABeatOut(const AudioResult &row)
{
    const BeatGridComparison &c = row.comparison;
    const double bpm = meanBpm(row.job->reference);
    return c.tempo == domain::TempoRelation::Same && c.offset.count > 0 && bpm > 0.0 &&
           std::abs(c.offset.medianMs) > 0.3 * 60000.0 / bpm;
}

void printAudioSummaryHeader(const char *by)
{
    std::printf("%-28s %5s %8s %7s %7s %6s %6s %6s %6s %7s %6s %5s %5s %5s %5s %5s %5s %6s %5s %7s %6s %6s\n", by, "n", "median",
                "p25", "p75", "<=1ms", "<=3ms", "<=8ms", "<=20ms", "scatter", "same", "half", "dbl", "2/3", "3/2",
                "other", "unkn", "shape", "nogrid", "|drift|", "dr<=10", "halfbt");
}

void printAudioSummaryLine(const std::string &label, const std::vector<const AudioResult *> &rows)
{
    if (rows.empty()) {
        return;
    }
    std::vector<double> medians;
    std::vector<double> scatters;
    std::vector<double> drifts;
    int within1 = 0, within3 = 0, within8 = 0, within20 = 0, shapeMatch = 0, noGrid = 0, driftWithin10 = 0;
    int halfBeat = 0;
    std::map<domain::TempoRelation, int> tempo;
    for (const AudioResult *row : rows) {
        const BeatGridComparison &c = row->comparison;
        ++tempo[c.tempo];
        noGrid += c.candidateBeats == 0 ? 1 : 0;
        shapeMatch += c.candidateShape.kind == c.referenceShape.kind ? 1 : 0;
        if (c.offset.count > 0) {
            medians.push_back(c.offset.medianMs);
            scatters.push_back(c.offset.madMs);
        }
        if (c.drift.known) {
            drifts.push_back(std::abs(c.drift.ms));
            driftWithin10 += std::abs(c.drift.ms) <= 10.0 ? 1 : 0;
        }
        halfBeat += halfABeatOut(*row) ? 1 : 0;
        const double m = rankOffset(c);
        within1 += m <= 1.0 ? 1 : 0;
        within3 += m <= 3.0 ? 1 : 0;
        within8 += m <= 8.0 ? 1 : 0;
        within20 += m <= 20.0 ? 1 : 0;
    }
    const double n = static_cast<double>(rows.size());
    const auto pct = [n](int count) { return 100.0 * count / n; };
    std::printf("%-28s %5zu %+8.2f %+7.2f %+7.2f %5.1f%% %5.1f%% %5.1f%% %5.1f%% %7.2f %5.1f%% %5d %5d %5d %5d %5d %5d "
                "%5.1f%% %5d %7.2f %5.1f%% %6d\n",
                label.c_str(), rows.size(), medianOf(medians), quantileOf(medians, 0.25), quantileOf(medians, 0.75),
                pct(within1), pct(within3), pct(within8), pct(within20), medianOf(scatters),
                pct(tempo[domain::TempoRelation::Same]), tempo[domain::TempoRelation::Half],
                tempo[domain::TempoRelation::Double], tempo[domain::TempoRelation::TwoThirds],
                tempo[domain::TempoRelation::ThreeHalves], tempo[domain::TempoRelation::Other],
                tempo[domain::TempoRelation::Unknown], pct(shapeMatch), noGrid, medianOf(drifts),
                drifts.empty() ? 0.0 : 100.0 * driftWithin10 / static_cast<double>(drifts.size()), halfBeat);
}

void writeAudioTsvHeader(std::ofstream &out)
{
    out << "rekordbox_id\tengine_id\tformat\thalf\tanalyser\terror\ttimeline\tshift_ms\tsample_rate\tduration_s\t"
           "decode_ms\tanalyse_ms\trekordbox_beats\tcandidate_beats\tmatched\tmedian_ms\tscatter_ms\tp95_abs_ms\t"
           "max_abs_ms\thit_1ms\thit_3ms\thit_8ms\thit_20ms\ttempo\ttempo_ratio\tcover\trekordbox_shape\t"
           "rekordbox_bpm\tcandidate_shape\tcandidate_bpm\tcandidate_runs\tdrift_ms\tdrift_start_ms\t"
           "drift_end_ms\tagreed_subset\tkey_found\tkey_rekordbox\tkey_engine\tkey_vs_rekordbox\tkey_vs_engine\t"
           "file\n";
}

// The lines of one track, one per analyser. drift_ms and its two ends
// are "-" where the drift is not known (another tempo, too few beats).
void writeAudioTsvRows(std::ofstream &out, const std::vector<AudioResult> &results)
{
    char line[2048];
    for (const AudioResult &r : results) {
        const AudioJob &job = *r.job;
        const BeatGridComparison &c = r.comparison;
        const bool grid = !r.key && r.error.empty();
        std::snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.3f\t%d\t%.3f\t%.1f\t%.1f\t", job.rekordboxId.c_str(),
                      job.engineId.c_str(), job.format.c_str(), halfOf(job.hash), r.analyser.c_str(),
                      r.error.empty() ? "-" : r.error.c_str(), job.shift.timeline.c_str(), job.shift.shiftMs,
                      r.sampleRate, r.durationS, r.decodeMs, r.analyseMs);
        out << line;
        if (grid) {
            std::snprintf(line, sizeof line,
                          "%zu\t%zu\t%zu\t%.3f\t%.3f\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%.5f\t%.4f\t%s\t%.3f\t%s\t"
                          "%.3f\t%d\t",
                          c.referenceBeats, c.candidateBeats, c.matchedBeats, c.offset.medianMs, c.offset.madMs,
                          c.offset.p95AbsMs, c.offset.maxAbsMs, c.hits.within1Ms, c.hits.within3Ms, c.hits.within8Ms,
                          c.hits.within20Ms, domain::tempoRelationName(c.tempo), c.tempoRatio, c.cover,
                          domain::gridShapeKindName(c.referenceShape.kind), c.referenceShape.bpm,
                          domain::gridShapeKindName(c.candidateShape.kind), c.candidateShape.bpm,
                          c.candidateShape.straightRuns);
            out << line;
            if (c.drift.known) {
                std::snprintf(line, sizeof line, "%.3f\t%.3f\t%.3f\t", c.drift.ms, c.drift.startMs, c.drift.endMs);
                out << line;
            } else {
                out << "-\t-\t-\t";
            }
        } else {
            out << "-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t";
        }
        out << job.agreed << "\t";
        if (r.key) {
            out << (r.keyFound.empty() ? "-" : r.keyFound) << "\t" << (job.rekordboxKey.empty() ? "-" : job.rekordboxKey)
                << "\t" << (job.engineKey.empty() ? "-" : job.engineKey) << "\t"
                << (r.error.empty() ? keyClass(r.keyFound, job.rekordboxKey) : "-") << "\t"
                << (r.error.empty() ? keyClass(r.keyFound, job.engineKey) : "-") << "\t";
        } else {
            out << "-\t-\t-\t-\t-\t";
        }
        out << job.file << "\n";
    }
    out.flush();
}

void printKeyClasses(const char *label, const std::vector<std::pair<std::string, std::string>> &pairs)
{
    std::map<std::string, int> classes;
    for (const auto &[found, reference] : pairs) {
        ++classes[keyClass(found, reference)];
    }
    std::printf("%-40s %5zu", label, pairs.size());
    for (const char *name : {"same", "relative", "fifth-up", "fifth-down", "parallel", "other", "unknown"}) {
        const int count = classes[name];
        std::printf("  %s %d (%.1f%%)", name, count, pairs.empty() ? 0.0 : 100.0 * count / pairs.size());
    }
    std::printf("\n");
}

int audioMode(int argc, char **argv)
{
    if (argc < 3) {
        return usage();
    }
    const fs::path stick = pathFromUtf8(argv[2]);
    std::string tsvPath;
    std::string analyserList;
    std::string half = "tuning";
    fs::path audioRoot = stick;
    fs::path scratchParent = fs::temp_directory_path();
    fs::path beatsDir;
    long limit = -1;
    int jobs = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage();
        }
        const std::string value = argv[++i];
        if (arg == "--tsv") {
            tsvPath = value;
        } else if (arg == "--analyser") {
            analyserList = value;
        } else if (arg == "--audio-root") {
            audioRoot = pathFromUtf8(value);
        } else if (arg == "--tempo-range") {
            if (!parseTempoRange(value, seabassTempoRange)) {
                return usage();
            }
        } else if (arg == "--scratch") {
            scratchParent = pathFromUtf8(value);
        } else if (arg == "--beats") {
            beatsDir = pathFromUtf8(value);
            if (!makeBeatsDir(beatsDir)) {
                return 1;
            }
        } else if (arg == "--half" && (value == "tuning" || value == "heldback" || value == "all")) {
            half = value;
        } else if (arg == "--limit") {
            limit = std::strtol(value.c_str(), nullptr, 10);
            if (limit <= 0) {
                return usage();
            }
        } else if (arg == "--jobs") {
            jobs = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
            // More than three decoders and analysers at once have run a
            // laptop with 32 GB out of memory; a decoded track is held
            // whole. Go past three only where the memory is there.
            if (jobs < 1 || jobs > 16) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (analyserList.empty()) {
        return usage();
    }
    const auto analyserNames = parseAnalyserList(analyserList, false);
    if (!analyserNames) {
        return 2;
    }
    printTempoRange(*analyserNames);
    std::error_code listEc;
    const bool fromList = fs::is_regular_file(stick, listEc);
    if (!fromList && !fs::exists(stick / "PIONEER" / "rekordbox" / "export.pdb")) {
        std::fprintf(stderr, "%s holds no rekordbox export\n", argv[2]);
        return 1;
    }
    if (!infrastructure::audio::QtMultimediaPcmSource::decodingAvailable()) {
        std::fprintf(stderr, "this build has no working audio decoder\n");
        return 1;
    }

    std::vector<AudioJob> allJobs;
    int rekordboxTracks = 0, noGrid = 0, noAudio = 0, otherHalf = 0;
    std::vector<AudioResult> results;
    if (fromList) {
        std::ifstream list(stick);
        std::string line;
        while (std::getline(list, line)) {
            const std::vector<std::string> fields = tabFields(line);
            if (fields.size() < 3 || fields[0].empty()) {
                continue;
            }
            ++rekordboxTracks;
            const std::uint64_t hash = pathHash(fields[1]);
            if (half != "all" && half != halfOf(hash)) {
                ++otherHalf;
                continue;
            }
            const std::vector<domain::Beat> rekordboxGrid = beatGridOfAnlzFile(pathFromUtf8(fields[2]));
            if (rekordboxGrid.empty()) {
                ++noGrid;
                continue;
            }
            AudioJob job;
            job.audio = pathFromUtf8(fields[1]);
            std::error_code ec;
            if (!fs::is_regular_file(job.audio, ec)) {
                ++noAudio;
                continue;
            }
            job.rekordboxId = fields[0];
            job.file = fields[1];
            job.format = formatOf(fields[1]);
            job.hash = hash;
            job.rekordboxKey = fields.size() > 3 ? fields[3] : std::string();
            // The list names the audio in full, so it is found from no root.
            job.shift = rekordboxOnEngineTimeline(fields[1], fields[1], fs::path());
            job.reference = shifted(rekordboxGrid, job.shift.shiftMs);
            allJobs.push_back(std::move(job));
        }
    } else
    try {
        const ScratchRoot scratch(stick, scratchParent);
        const fs::path root = scratch.root();
        const fs::path pioneer = root / "PIONEER";
        const fs::path engineLibrary = root / "Engine Library";
        std::printf("scratch copy: %d catalog file(s) copied\n", scratch.copiedFiles());

        infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pathToUtf8(pioneer));
        const std::vector<domain::Track> rekordbox = application::ScanLibrary(rekordboxReader).execute();
        rekordboxTracks = static_cast<int>(rekordbox.size());

        // Engine is optional: without it there is no agreed subset and no
        // Engine key, and every track is still measured against rekordbox.
        std::vector<domain::Track> engine;
        std::optional<djinterop::database> engineDb;
        if (fs::exists(engineLibrary)) {
            rig::EngineReaderAtFileRates engineReader(engineLibrary);
            engine = application::ScanLibrary(engineReader.reader()).execute();
            engineDb = djinterop::engine::load_database(pathToUtf8(engineLibrary));
        }
        const application::BeatGridCopyPlan plan = application::planBeatGridCopy(rekordbox, engine, pathToUtf8(root));
        std::map<std::string, const application::BeatGridCopyItem *> pairedByRekordboxId;
        for (const application::BeatGridCopyItem &item : plan.items) {
            pairedByRekordboxId[item.rekordboxTrackId] = &item;
        }
        std::map<std::string, const domain::Track *> engineById;
        for (const domain::Track &track : engine) {
            engineById[track.sourceId] = &track;
        }

        for (const domain::Track &track : rekordbox) {
            const std::string relative = application::stickRelativePathOf(track.filePath, pathToUtf8(root));
            if (relative.empty()) {
                ++noAudio;
                continue;
            }
            const std::uint64_t hash = pathHash(relative);
            if (half != "all" && half != halfOf(hash)) {
                ++otherHalf;
                continue;
            }
            const std::vector<domain::Beat> rekordboxGrid =
                infrastructure::rekordbox::readBeatGrid(pathToUtf8(pioneer), track.sourceId);
            if (rekordboxGrid.empty()) {
                ++noGrid;
                continue;
            }
            AudioJob job;
            job.audio = audioRoot / pathFromUtf8(relative);
            std::error_code ec;
            if (!fs::is_regular_file(job.audio, ec)) {
                ++noAudio;
                continue;
            }
            job.rekordboxId = track.sourceId;
            job.file = relative;
            job.format = formatOf(track.filePath);
            job.hash = hash;
            job.rekordboxKey = track.key;
            job.shift = rekordboxOnEngineTimeline(track.filePath, relative, audioRoot);
            job.reference = shifted(rekordboxGrid, job.shift.shiftMs);
            const auto paired = pairedByRekordboxId.find(track.sourceId);
            if (paired != pairedByRekordboxId.end() && engineDb) {
                job.engineId = paired->second->engineTrackId;
                if (const auto engineTrack = engineById.find(job.engineId); engineTrack != engineById.end()) {
                    job.engineKey = engineTrack->second->key;
                }
                try {
                    if (const auto row = engineDb->track_by_id(std::stoll(job.engineId))) {
                        const std::vector<domain::Beat> engineGrid = infrastructure::engine::beatGridOf(*row);
                        if (!engineGrid.empty()) {
                            job.agreed = agreed(domain::compareBeatGrids(engineGrid, job.reference)) ? "yes" : "no";
                        }
                    }
                } catch (const std::exception &) {
                    // An unreadable Engine row leaves the track out of the
                    // agreed subset, as one without a grid.
                }
            }
            allJobs.push_back(std::move(job));
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    // A stable order that mixes the library (by the hash), so that a
    // limit takes a sample rather than the first folders.
    std::sort(allJobs.begin(), allJobs.end(), [](const AudioJob &a, const AudioJob &b) {
        return a.hash != b.hash ? a.hash < b.hash : a.file < b.file;
    });
    const std::size_t eligible = allJobs.size();
    if (limit > 0 && allJobs.size() > static_cast<std::size_t>(limit)) {
        allJobs.resize(static_cast<std::size_t>(limit));
    }
    std::printf("rekordbox tracks %d; half %s: %zu with a grid and audio (other half %d, no grid %d, no audio %d)%s\n",
                rekordboxTracks, half.c_str(), eligible, otherHalf, noGrid, noAudio,
                allJobs.size() < eligible ? (", limited to " + std::to_string(allJobs.size())).c_str() : "");
    std::printf("analysing with %d job(s)\n", jobs);
    std::fflush(stdout);

    // Each worker is a QThread, so the decoder's nested event loop has an
    // event dispatcher in its thread.
    std::ofstream tsv;
    std::mutex tsvMutex;
    if (!tsvPath.empty()) {
        tsv.open(tsvPath, std::ios::trunc);
        if (!tsv) {
            std::fprintf(stderr, "error: cannot write %s\n", tsvPath.c_str());
            return 1;
        }
        writeAudioTsvHeader(tsv);
        tsv.flush();
    }
    std::vector<std::vector<AudioResult>> perJob(allJobs.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    const auto wallStart = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<QThread>> workers;
    for (int w = 0; w < jobs; ++w) {
        workers.emplace_back(QThread::create([&] {
            for (;;) {
                const std::size_t index = next.fetch_add(1);
                if (index >= allJobs.size()) {
                    return;
                }
                perJob[index] = analyseJob(allJobs[index], *analyserNames, beatsDir);
                if (tsv.is_open()) {
                    const std::lock_guard<std::mutex> lock(tsvMutex);
                    writeAudioTsvRows(tsv, perJob[index]);
                }
                const std::size_t finished = done.fetch_add(1) + 1;
                if (finished % 25 == 0 || finished == allJobs.size()) {
                    std::fprintf(stderr, "%zu/%zu tracks, %.0f s\n", finished, allJobs.size(),
                                 msSince(wallStart) / 1000.0);
                }
            }
        }));
        workers.back()->start();
    }
    for (const auto &worker : workers) {
        worker->wait();
    }
    const double wallMs = msSince(wallStart);
    for (std::vector<AudioResult> &trackResults : perJob) {
        for (AudioResult &result : trackResults) {
            results.push_back(std::move(result));
        }
    }

    std::printf("\n%zu track(s) in %.1f s wall clock\n", allJobs.size(), wallMs / 1000.0);
    std::printf("offsets are the analyser's beat minus rekordbox's, both on Engine's timeline where the lead-in is\n"
                "known, in ms. median, p25, p75: of the per-track median offset (tracks with a matched beat);\n"
                "<=Nms: share of tracks whose median offset is within N ms (no matched beat counts as a miss);\n"
                "scatter: median of the per-track scatter; same: share with the same tempo; half, dbl, 2/3, 3/2,\n"
                "other, unkn: track counts of the analyser's tempo against rekordbox's; shape: share whose shape\n"
                "(constant or moving) matches rekordbox's; nogrid: tracks the analyser gave no beats for;\n"
                "|drift|: median of how far the offset moves from the start of a track to its end (same tempo\n"
                "only); dr<=10: share of those within 10 ms; halfbt: tracks at the same tempo whose beats fall\n"
                "between rekordbox's\n");

    for (const std::string &name : *analyserNames) {
        if (isKeyAnalyser(name)) {
            continue;
        }
        std::vector<const AudioResult *> all;
        std::vector<const AudioResult *> agreedSubset;
        std::map<std::string, std::vector<const AudioResult *>> byFormat;
        std::map<std::string, std::vector<const AudioResult *>> byBand;
        std::vector<double> decodeMs;
        std::vector<double> analyseMs;
        int failed = 0;
        for (const AudioResult &r : results) {
            if (r.analyser != name) {
                continue;
            }
            if (!r.error.empty()) {
                ++failed;
                continue;
            }
            all.push_back(&r);
            decodeMs.push_back(r.decodeMs);
            analyseMs.push_back(r.analyseMs);
            if (r.job->agreed == "yes") {
                agreedSubset.push_back(&r);
            }
            byFormat[r.job->format + " " + r.job->shift.timeline].push_back(&r);
            const domain::GridShape &ref = r.comparison.referenceShape;
            byBand[bpmBand(ref.kind == domain::GridShapeKind::Constant ? ref.bpm : ref.highestBpm)].push_back(&r);
        }
        std::printf("\n=== %s: %zu track(s) analysed, %d failed; per track: decode median %.0f ms, analysis median "
                    "%.0f ms (mean %.0f ms)\n",
                    name.c_str(), all.size(), failed, medianOf(decodeMs), medianOf(analyseMs),
                    analyseMs.empty() ? 0.0
                                      : std::accumulate(analyseMs.begin(), analyseMs.end(), 0.0) / analyseMs.size());
        printAudioSummaryHeader("set");
        printAudioSummaryLine("all", all);
        printAudioSummaryLine("agreed subset", agreedSubset);
        std::printf("\n");
        printAudioSummaryHeader("format, timeline");
        for (const auto &[label, group] : byFormat) {
            printAudioSummaryLine(label, group);
        }
        std::printf("\n");
        printAudioSummaryHeader("rekordbox tempo");
        for (const auto &[label, group] : byBand) {
            printAudioSummaryLine(label, group);
        }

        std::vector<const AudioResult *> worst = all;
        std::stable_sort(worst.begin(), worst.end(), [](const AudioResult *a, const AudioResult *b) {
            return rankOffset(a->comparison) > rankOffset(b->comparison);
        });
        worst.resize(std::min<std::size_t>(worst.size(), 30));
        std::printf("\nthe %zu worst by median offset (the TSV has their files):\n", worst.size());
        std::printf("  %-12s %-6s %-20s %9s %8s %-6s %8s %8s %-9s %-7s\n", "rekordbox id", "format", "timeline",
                    "median", "scatter", "tempo", "rbBPM", "ratio", "shape", "agreed");
        for (const AudioResult *r : worst) {
            const BeatGridComparison &c = r->comparison;
            std::printf("  %-12s %-6s %-20s %+9.2f %8.2f %-6s %8.3f %8.4f %-9s %-7s\n", r->job->rekordboxId.c_str(),
                        r->job->format.c_str(), r->job->shift.timeline.c_str(),
                        c.offset.count == 0 ? std::nan("") : c.offset.medianMs, c.offset.madMs,
                        domain::tempoRelationName(c.tempo), meanBpm(r->job->reference),
                        c.tempoRatio,
                        domain::gridShapeKindName(c.candidateShape.kind), r->job->agreed.c_str());
        }
    }

    for (const std::string &name : *analyserNames) {
        if (!isKeyAnalyser(name)) {
            continue;
        }
        std::vector<std::pair<std::string, std::string>> againstRekordbox;
        std::vector<std::pair<std::string, std::string>> againstEngine;
        std::vector<std::pair<std::string, std::string>> engineAgainstRekordbox;
        std::vector<double> analyseMs;
        int failed = 0, noneFound = 0;
        for (const AudioResult &r : results) {
            if (r.analyser != name) {
                continue;
            }
            if (!r.error.empty()) {
                ++failed;
                continue;
            }
            analyseMs.push_back(r.analyseMs);
            if (r.keyFound.empty()) {
                ++noneFound;
            }
            if (!r.job->rekordboxKey.empty()) {
                againstRekordbox.emplace_back(r.keyFound, r.job->rekordboxKey);
            }
            if (!r.job->engineKey.empty()) {
                againstEngine.emplace_back(r.keyFound, r.job->engineKey);
            }
            if (!r.job->engineKey.empty() && !r.job->rekordboxKey.empty()) {
                engineAgainstRekordbox.emplace_back(r.job->engineKey, r.job->rekordboxKey);
            }
        }
        std::printf("\n=== %s: %zu track(s), %d failed, %d without a key; analysis median %.0f ms\n", name.c_str(),
                    analyseMs.size(), failed, noneFound, medianOf(analyseMs));
        std::printf("(a class names the first key's relation to the second: fifth-up means a fifth above it)\n");
        printKeyClasses((name + " against rekordbox").c_str(), againstRekordbox);
        printKeyClasses((name + " against Engine").c_str(), againstEngine);
        printKeyClasses("Engine against rekordbox (same tracks)", engineAgainstRekordbox);
    }

    if (!tsvPath.empty()) {
        if (!tsv) {
            std::fprintf(stderr, "error: cannot write %s\n", tsvPath.c_str());
            return 1;
        }
        std::printf("\n%zu line(s) written to %s\n", results.size(), tsvPath.c_str());
    }
    return 0;
}

// --- The grids mode ---

// A beat list as --beats writes it: a beat a line, the time in ms and the
// place in the bar. A line that does not start with a number is passed
// over.
std::vector<domain::Beat> readBeats(const fs::path &file)
{
    std::vector<domain::Beat> beats;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        char *end = nullptr;
        const double ms = std::strtod(line.c_str(), &end);
        if (end == line.c_str() || !std::isfinite(ms)) {
            continue;
        }
        beats.push_back({ms, static_cast<int>(std::strtol(end, nullptr, 10))});
    }
    return beats;
}

int gridsMode(int argc, char **argv)
{
    if (argc < 5) {
        return usage();
    }
    const fs::path candidateDir = pathFromUtf8(argv[2]);
    const std::string candidateName = argv[3];
    const fs::path referenceDir = pathFromUtf8(argv[4]);
    std::string referenceName = "reference";
    std::string tsvPath;
    for (int i = 5; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage();
        }
        const std::string value = argv[++i];
        if (arg == "--tsv") {
            tsvPath = value;
        } else if (arg == "--reference") {
            referenceName = value;
        } else {
            return usage();
        }
    }
    if (tsvPath.empty() || candidateName.empty() || referenceName.empty()) {
        return usage();
    }
    const std::string suffix = "." + candidateName + ".tsv";
    std::vector<std::string> tracks;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(candidateDir, ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            tracks.push_back(name.substr(0, name.size() - suffix.size()));
        }
    }
    if (ec) {
        std::fprintf(stderr, "cannot list %s: %s\n", pathToUtf8(candidateDir).c_str(), ec.message().c_str());
        return 1;
    }
    std::sort(tracks.begin(), tracks.end());
    std::ofstream tsv(pathFromUtf8(tsvPath), std::ios::trunc);
    if (!tsv) {
        std::fprintf(stderr, "cannot write %s\n", tsvPath.c_str());
        return 1;
    }
    writeAudioTsvHeader(tsv);
    std::size_t compared = 0;
    std::size_t withoutReference = 0;
    for (const std::string &track : tracks) {
        const fs::path reference = referenceDir / pathFromUtf8(track + "." + referenceName + ".tsv");
        if (!fs::is_regular_file(reference, ec)) {
            ++withoutReference;
            continue;
        }
        AudioJob job;
        job.rekordboxId = track;
        job.format = "-";
        job.shift.timeline = "as-given";
        job.reference = readBeats(reference);
        AudioResult result;
        result.job = &job;
        result.analyser = candidateName;
        result.comparison = domain::compareBeatGrids(readBeats(candidateDir / pathFromUtf8(track + suffix)), job.reference);
        writeAudioTsvRows(tsv, {result});
        ++compared;
    }
    std::printf("%zu grid(s) named %s compared with %s; %zu without one to compare with; written to %s\n", compared,
                candidateName.c_str(), referenceName.c_str(), withoutReference, tsvPath.c_str());
    return tsv ? 0 : 1;
}
#endif

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    const std::string mode = argv[1];
    if (mode == "libraries") {
        return librariesMode(argc, argv);
    }
    if (mode == "synth") {
        return synthMode(argc, argv);
    }
#ifdef SEABASS_ANALYSIS_AUDIO
    if (mode == "audio") {
        QCoreApplication app(argc, argv);
        // QCoreApplication takes the user's locale, which would print
        // decimal commas into the summary and the TSV.
        std::setlocale(LC_NUMERIC, "C");
        // The FFmpeg backend describes every file it opens; a decode that
        // fails says so through the source's error() instead.
        QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg*=false"));
        return audioMode(argc, argv);
    }
    if (mode == "grids") {
        return gridsMode(argc, argv);
    }
#endif
    return usage();
}
