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
//   analysis_benchmark synth [--preset NAME|all] [--shift-ms X] [--check]
//
// Renders synthetic tracks (tools/synth_track) and runs every registered
// analyser over them through the GridAnalyser port, scored against the
// true grid. No real analyser exists yet; the one registered is an
// oracle that answers the true grid moved by --shift-ms, which checks
// this tool's own arithmetic: every line must show exactly that shift
// with no scatter, on constant and moving grids alike. --check makes that
// the exit code.
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
                 "       analysis_benchmark synth [--preset NAME|all] [--shift-ms X] [--check]\n");
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
            std::vector<domain::Beat> reference = rekordboxGrid;
            if (!domain::hasMp3Extension(item.filePath)) {
                row.timeline = "not-mp3";
            } else {
                const fs::path audio = audioRoot / pathFromUtf8(item.stickRelativePath);
                const auto leadIn = item.stickRelativePath.empty()
                    ? std::optional<domain::Mp3LeadIn>{}
                    : infrastructure::audio::readMp3LeadIn(pathToUtf8(audio));
                if (leadIn && leadIn->sampleRate > 0) {
                    row.timeline = "mp3-corrected";
                    row.shiftMs = infrastructure::audio::playbackShiftFor(
                                      leadIn, true, infrastructure::audio::GridTimeline::Rekordbox)
                                      .beatsMs;
                } else {
                    row.timeline = "mp3-lead-in-unknown";
                }
            }
            for (domain::Beat &beat : reference) {
                beat.timeMs += row.shiftMs;
            }
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

int synthMode(int argc, char **argv)
{
    std::string preset = "all";
    double shiftMs = 0.0;
    bool checkMode = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") {
            checkMode = true;
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
    std::vector<std::string> names;
    if (preset == "all") {
        names = synth::synthPresetNames();
    } else if (synth::synthPreset(preset)) {
        names.push_back(preset);
    } else {
        std::fprintf(stderr, "no preset %s (synth_track --list names them)\n", preset.c_str());
        return 2;
    }

    std::printf("%-24s %-8s %-8s %5s %9s %8s %8s %8s %6s %6s %6s %6s %-6s %4s %5s %-9s %-9s %5s\n", "preset",
                "analyser", "conf", "beats", "median", "scatter", "p95", "max", "<=1ms", "<=3ms", "<=8ms", "<=20ms",
                "tempo", "bar", "share", "truth", "found", "cover");
    int failures = 0;
    for (const std::string &name : names) {
        const synth::SynthTrackSpec spec = *synth::synthPreset(name);
        const synth::SynthTrack track = synth::renderSynthTrack(spec);
        std::vector<std::unique_ptr<domain::GridAnalyser>> analysers;
        analysers.push_back(std::make_unique<OracleAnalyser>(track.beats, shiftMs));
        for (const auto &analyser : analysers) {
            domain::InMemoryPcmSource source(track.samples, track.sampleRate);
            const domain::GridAnalysis analysis = analyser->analyse(source);
            if (!analysis.error.empty()) {
                std::printf("%-24s %-8s failed: %s\n", name.c_str(), analyser->name().c_str(), analysis.error.c_str());
                ++failures;
                continue;
            }
            const BeatGridComparison c = domain::compareBeatGrids(analysis.beats, track.beats);
            std::printf("%-24s %-8s %8.3f %5zu %+9.4f %8.4f %8.4f %8.4f %6.3f %6.3f %6.3f %6.3f %-6s %4s %5.3f %-9s "
                        "%-9s %5.3f\n",
                        name.c_str(), analyser->name().c_str(), analysis.confidence, c.candidateBeats,
                        c.offset.medianMs, c.offset.madMs, c.offset.p95AbsMs, c.offset.maxAbsMs, c.hits.within1Ms,
                        c.hits.within3Ms, c.hits.within8Ms, c.hits.within20Ms, domain::tempoRelationName(c.tempo),
                        c.bar.known ? std::to_string(c.bar.beats).c_str() : "-", c.bar.share,
                        domain::gridShapeKindName(c.referenceShape.kind),
                        domain::gridShapeKindName(c.candidateShape.kind), c.cover);
            if (checkMode && analyser->name() == "oracle") {
                // The oracle's answer is the truth moved by the shift: the
                // arithmetic must give back exactly that.
                const bool ok = std::abs(c.offset.medianMs - shiftMs) < 1e-6 && c.offset.madMs < 1e-6
                    && std::abs(c.offset.maxAbsMs - std::abs(shiftMs)) < 1e-6 && c.tempo == domain::TempoRelation::Same
                    && c.bar.known && c.bar.beats == 0 && c.bar.share == 1.0
                    && c.candidateShape.kind == c.referenceShape.kind && c.candidateBeats == track.beats.size()
                    && (std::abs(shiftMs) > 1.0 || c.hits.within1Ms == 1.0);
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
    return usage();
}
