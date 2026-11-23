// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Hardware probe for Copy Beat Grids: puts a grid of the caller's making
// on one Engine row, through the same writer the feature uses, so that a
// player can be asked a question rekordbox's own grids do not ask.
//
// A copied grid on a click track looks like Denon's own analysis: both
// found the same clicks. Whether the player plays by the grid Seabass
// wrote shows only when that grid is visibly somewhere else, half a beat
// late say, or changing tempo halfway through a track that does not.
//
// usage: beat_grid_probe <Engine Library dir> <track id> --bpm B --first-ms F --until-ms U
//            [--bpm2 B2 --change-at-ms T]         a tempo change at T
//            [--unanalysed fill|fill-lock --rate HZ --duration-ms D]
//            [--overwrite]
//        beat_grid_probe <Engine Library dir> <track id> --anlz DIR --audio FILE [--waveform] [--dry-run]
//
// Times are on Engine's timeline: no MP3 lead-in shift is applied. The
// caller backs the library up; this writes straight to it.
//
// With --anlz, rekordbox's own analysis of one track (ANLZ0000.DAT, .EXT
// and .2EX in DIR, as rekordbox keeps them in its library, no export
// needed) goes onto the row the way Beat Grids & Waveforms takes a row
// the player has not analysed: read by the page's reader
// (rekordboxAnalysisAt), with --waveform through importAnalysisItem()
// with the page's default options, which completes the row; without it
// the grid alone through writeBeatGrid(), filled as copyBeatGrids() fills
// it. FILE gives the sample rate and count, and the MP3 lead-in shift the
// writer takes off (none for any other kind of file). No kept-analysis
// store and no ledger: the row is taken as one the player has not
// analysed. --dry-run makes the same calls as a preview.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include <djinterop/engine/v2/beat_data_blob.hpp>

#include "application/use_cases/copy_beat_grids.hpp"
#include "domain/beat_grid.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

using namespace seabass;

namespace
{
using Anlz = rekordbox_anlz_t;

const char *words(application::BeatGridWriteResult::Outcome outcome)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    switch (outcome) {
    case Outcome::Written:
        return "written";
    case Outcome::AlreadySame:
        return "already the same";
    case Outcome::NoSourceGrid:
        return "no source grid";
    case Outcome::RowNotAnalysed:
        return "row not analysed";
    case Outcome::RowLocked:
        return "row locked";
    case Outcome::RowEditedOnEngine:
        return "row edited on Engine";
    case Outcome::NoSampleRate:
        return "no sample rate";
    case Outcome::UnsupportedSchema:
        return "unsupported schema";
    case Outcome::NotRequested:
        return "not requested";
    case Outcome::CannotKeepEngines:
        return "Engine's analysis cannot be kept";
    case Outcome::RowUnreadable:
        return "row unreadable";
    }
    return "unknown";
}

const char *words(application::AnalysisImportResult::Waveform waveform)
{
    using Waveform = application::AnalysisImportResult::Waveform;
    switch (waveform) {
    case Waveform::Written:
        return "written";
    case Waveform::AlreadySame:
        return "already the same";
    case Waveform::NotRequested:
        return "not requested";
    case Waveform::NoSourceWaveform:
        return "no rekordbox waveform";
    case Waveform::RowNotAnalysed:
        return "row not analysed, not completed";
    case Waveform::NoSampleCount:
        return "no sample count";
    case Waveform::UnsupportedSchema:
        return "unsupported schema";
    case Waveform::CannotKeepEngines:
        return "Engine's analysis cannot be kept";
    case Waveform::RowUnreadable:
        return "row unreadable";
    case Waveform::PlayersWaveform:
        return "the player's waveform, left alone";
    }
    return "unknown";
}

// The tempos rekordbox wrote with the beats (PQTZ), read with the same
// generated parser the reader uses, which drops them: how many differ.
std::set<int> rekordboxTempos(const std::string &datFile)
{
    std::ifstream in(pathFromUtf8(datFile), std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    std::istringstream stream(bytes.str(), std::ios::binary);
    kaitai::kstream ks(&stream);
    Anlz anlz(&ks);
    std::set<int> tempos;
    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() != Anlz::SECTION_TAGS_BEAT_GRID) {
            continue;
        }
        if (auto *tag = dynamic_cast<Anlz::beat_grid_tag_t *>(section->body()); tag && tag->beats()) {
            for (const auto &beat : *tag->beats()) {
                tempos.insert(beat->tempo());
            }
            break;
        }
    }
    return tempos;
}

struct Marker
{
    double sample = 0.0;
    long long beat = 0;
};

void printMarkers(const std::string &heading, const std::vector<Marker> &markers)
{
    std::cout << heading << ": " << markers.size() << " marker(s)\n";
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (i == 3 && markers.size() > 6) {
            std::cout << "  ...\n";
            i = markers.size() - 3;
        }
        std::printf("  beat %6lld at sample %.3f\n", markers[i].beat, markers[i].sample);
    }
}

// The row as stored, after the write: flags, blob lengths, the mark, and
// the adjusted grid decoded. Engine 3.x keeps the blobs in
// PerformanceData, 2.x in Track.
bool printRow(const std::string &library, const std::string &trackId)
{
    const std::string file = pathToUtf8(pathFromUtf8(library) / "Database2" / "m.db");
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::cerr << "beat_grid_probe: cannot open " << file << "\n";
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    const char *v3 = "SELECT t.isAnalyzed, p.beatData, length(p.overviewWaveFormData), length(p.trackData), "
                     "p.thirdPartySourceId FROM Track t JOIN PerformanceData p ON p.trackId = t.id WHERE t.id = ?1";
    const char *v2 = "SELECT isAnalyzed, beatData, length(overviewWaveFormData), length(trackData), "
                     "thirdPartySourceId FROM Track WHERE id = ?1";
    if (sqlite3_prepare_v2(db, v3, -1, &stmt, nullptr) != SQLITE_OK
        && sqlite3_prepare_v2(db, v2, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "beat_grid_probe: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, std::atoll(trackId.c_str()));
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = true;
        const auto text = [stmt](int column) {
            const unsigned char *t = sqlite3_column_text(stmt, column);
            return t ? std::string(reinterpret_cast<const char *>(t)) : std::string("NULL");
        };
        const int beatBytes = sqlite3_column_bytes(stmt, 1);
        std::vector<std::byte> beatData(beatBytes);
        if (beatBytes > 0) {
            std::memcpy(beatData.data(), sqlite3_column_blob(stmt, 1), beatBytes);
        }
        std::cout << "row " << trackId << " read back: isAnalyzed " << text(0) << ", beatData " << beatBytes
                  << " byte(s), overviewWaveFormData " << text(2) << ", trackData " << text(3)
                  << ", thirdPartySourceId " << text(4) << "\n";
        if (!beatData.empty()) {
            try {
                const auto blob = djinterop::engine::v2::beat_data_blob::from_blob(beatData);
                std::vector<Marker> markers;
                for (const auto &m : blob.adjusted_beat_grid) {
                    markers.push_back({m.sample_offset, static_cast<long long>(m.beat_number)});
                }
                std::printf("beatData: rate %.1f, samples %.0f, default grid %zu marker(s)\n", blob.sample_rate,
                            blob.samples, blob.default_beat_grid.size());
                printMarkers("adjusted grid as stored", markers);
            } catch (const std::exception &e) {
                std::cout << "beatData does not decode: " << e.what() << "\n";
            }
        }
    } else {
        std::cerr << "beat_grid_probe: no row " << trackId << "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

int anlzMode(const std::string &library, const std::string &trackId, const std::string &anlzDir,
             const std::string &audioFile, bool waveform, bool dryRun)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    // The item as the page makes one, of what is known without an
    // export: the row, and the file. No ledger key, no key, no tempo.
    application::BeatGridCopyItem item;
    item.engineTrackId = trackId;
    item.filePath = audioFile;
    item.title = audioFile;

    const application::RekordboxAnalysis analysis =
        infrastructure::local::rekordboxAnalysisAt(anlzDir, "ANLZ0000.DAT", item, true, waveform);
    const std::set<int> tempos = rekordboxTempos(pathToUtf8(pathFromUtf8(anlzDir) / "ANLZ0000.DAT"));
    std::cout << "rekordbox: " << analysis.beats.size() << " beat(s), " << tempos.size() << " distinct tempo(s)";
    if (!tempos.empty()) {
        std::printf(" %.2f to %.2f BPM", *tempos.begin() / 100.0, *tempos.rbegin() / 100.0);
    }
    if (!analysis.beats.empty()) {
        std::printf(", first beat %.0f ms, last %.0f ms", analysis.beats.front().timeMs, analysis.beats.back().timeMs);
    }
    std::cout << "\n";
    if (waveform) {
        std::cout << "rekordbox waveform: three-band preview "
                  << (analysis.threeBandPreview ? std::to_string(analysis.threeBandPreview->size()) + " byte(s)" : "none")
                  << ", colour preview "
                  << (analysis.colourPreview ? std::to_string(analysis.colourPreview->size()) + " byte(s)" : "none")
                  << ", scrolling entries "
                  << (analysis.scrollEntries ? std::to_string(*analysis.scrollEntries) : "none") << "\n";
    }

#ifdef SEABASS_HAVE_TAGLIB
    infrastructure::audio::TagLibMetadataProbe probe;
#else
    application::NullTrackMetadataProbe probe;
#endif
    // Rooted at the file's folder; read only, the cache is never saved.
    infrastructure::local::CachedSampleRates rates(pathToUtf8(pathFromUtf8(audioFile).parent_path()), probe);
    const application::AnalysisFileFactsSource fileFacts = [&rates](const application::BeatGridCopyItem &of) {
        return infrastructure::local::analysisFileFactsOf(of, rates);
    };
    const application::AnalysisFileFacts facts = fileFacts(item);
    std::cout << "file: rate " << (facts.sampleRateHz ? std::to_string(*facts.sampleRateHz) : "unknown")
              << ", samples " << (facts.sampleCount ? std::to_string(*facts.sampleCount) : "unknown")
              << (facts.isMp3 ? ", MP3" : "") << "\n";

    try {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(library);
        // The writer reads the lead-in of the row's own file; here of the
        // file given, through the same reader. It asks only for an MP3 row.
        writer.setLeadInSource([audioFile](const std::string &) { return infrastructure::audio::readMp3LeadIn(audioFile); });
        const double shiftMs = writer.cueTimelineShiftMs(trackId);
        std::printf("lead-in shift taken off: %.3f ms\n", shiftMs);

        application::BeatGridWriteResult grid;
        if (waveform) {
            // The page's defaults: both parts, nothing of the player's
            // written over.
            application::AnalysisCopyOptions options;
            options.grids = true;
            options.waveforms = true;
            options.overwriteEngineEdits = false;
            options.overwritePlayerWaveforms = false;
            options.previewOnly = dryRun;
            const application::AnalysisItemOutcome out =
                application::importAnalysisItem(item, analysis, writer, {}, options, fileFacts);
            grid = out.result.grid;
            std::cout << "outcome: grid " << words(grid.outcome) << ", waveform " << words(out.result.waveform)
                      << (out.result.completedRow ? (dryRun ? ", row would be completed" : ", row completed") : "")
                      << (out.result.noSampleCountReason.empty() ? "" : " (" + out.result.noSampleCountReason + ")")
                      << "\n";
        } else {
            // As copyBeatGrids() takes an item, filled where the player
            // has not analysed the row.
            application::BeatGridWriteOptions options;
            options.unanalysed = application::UnanalysedRowGrid::Fill;
            options.previewOnly = dryRun;
            grid = writer.writeBeatGrid(trackId, analysis.beats, options);
            if (grid.outcome == Outcome::NoSampleRate && facts.sampleRateHz && *facts.sampleRateHz > 0.0) {
                options.fileSampleRateHz = *facts.sampleRateHz;
                if (facts.sampleCount) {
                    options.fileDurationMs = static_cast<double>(*facts.sampleCount) / *facts.sampleRateHz * 1000.0;
                }
                grid = writer.writeBeatGrid(trackId, analysis.beats, options);
            }
            std::cout << "outcome: grid " << words(grid.outcome) << "\n";
        }
        std::cout << "grid: " << grid.markers << " marker(s), " << grid.barBreaks << " bar break(s)"
                  << (grid.gridFingerprint.empty() ? "" : ", fingerprint " + grid.gridFingerprint) << "\n";

        if (dryRun) {
            // The markers the writer would make, by the conversion it
            // makes them with (its convertedGrid()), at the file's rate.
            if (facts.sampleRateHz && facts.sampleCount) {
                std::vector<domain::Beat> beats = analysis.beats;
                for (domain::Beat &beat : beats) {
                    beat.timeMs -= shiftMs;
                }
                const double rate = *facts.sampleRateHz;
                const double durationMs = static_cast<double>(*facts.sampleCount) / rate * 1000.0;
                std::vector<Marker> markers;
                for (const auto &m : domain::engineNumbered(domain::markersFromBeats(beats, durationMs).markers)) {
                    markers.push_back({m.timeMs / 1000.0 * rate, m.index});
                }
                printMarkers("markers that would be written", markers);
            }
            std::cout << "--dry-run: nothing written\n";
            return 0;
        }
    } catch (const std::exception &e) {
        std::cerr << "beat_grid_probe: " << e.what() << "\n";
        return 1;
    }
    // The writer closed: what is on disk now.
    return printRow(library, trackId) ? 0 : 1;
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: beat_grid_probe <Engine Library dir> <track id> --bpm B --first-ms F --until-ms U\n"
                     "           [--bpm2 B2 --change-at-ms T] [--unanalysed fill|fill-lock --rate HZ --duration-ms D]\n"
                     "           [--overwrite]\n"
                     "       beat_grid_probe <Engine Library dir> <track id> --anlz DIR --audio FILE\n"
                     "           [--waveform] [--dry-run]\n";
        return 2;
    }
    const std::string library = argv[1];
    const std::string trackId = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) != "--anlz") {
            continue;
        }
        std::string anlzDir;
        std::string audioFile;
        bool waveform = false;
        bool dryRun = false;
        for (int j = 3; j < argc; ++j) {
            const std::string arg = argv[j];
            if (arg == "--waveform") {
                waveform = true;
            } else if (arg == "--dry-run") {
                dryRun = true;
            } else if (arg == "--anlz" && j + 1 < argc) {
                anlzDir = argv[++j];
            } else if (arg == "--audio" && j + 1 < argc) {
                audioFile = argv[++j];
            } else {
                std::cerr << "beat_grid_probe: what is \"" << arg << "\" with --anlz?\n";
                return 2;
            }
        }
        if (anlzDir.empty() || audioFile.empty()) {
            std::cerr << "beat_grid_probe: --anlz needs --audio\n";
            return 2;
        }
        return anlzMode(library, trackId, anlzDir, audioFile, waveform, dryRun);
    }
    std::map<std::string, std::string> values;
    bool overwrite = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--overwrite") {
            overwrite = true;
        } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
            values[arg] = argv[++i];
        } else {
            std::cerr << "beat_grid_probe: what is \"" << arg << "\"?\n";
            return 2;
        }
    }
    const auto number = [&values](const std::string &name, double otherwise) {
        const auto found = values.find(name);
        return found == values.end() ? otherwise : std::atof(found->second.c_str());
    };
    const double bpm = number("--bpm", 0.0);
    const double firstMs = number("--first-ms", 0.0);
    const double untilMs = number("--until-ms", 0.0);
    const double bpm2 = number("--bpm2", 0.0);
    const double changeAtMs = number("--change-at-ms", 0.0);
    if (!(bpm > 0.0) || !(untilMs > firstMs)) {
        std::cerr << "beat_grid_probe: --bpm and --until-ms are needed\n";
        return 2;
    }

    // Every beat written out, as rekordbox would, the bar starting on the
    // first one.
    std::vector<domain::Beat> beats;
    double at = firstMs;
    for (int i = 0; at <= untilMs; ++i) {
        beats.push_back({at, i % 4 + 1});
        at += 60000.0 / (bpm2 > 0.0 && at >= changeAtMs ? bpm2 : bpm);
    }

    application::BeatGridWriteOptions options;
    options.overwriteEngineEdits = overwrite;
    const std::string unanalysed = values.count("--unanalysed") ? values["--unanalysed"] : "skip";
    if (unanalysed == "fill") {
        options.unanalysed = application::UnanalysedRowGrid::Fill;
    } else if (unanalysed == "fill-lock") {
        options.unanalysed = application::UnanalysedRowGrid::FillAndLock;
    } else if (unanalysed != "skip") {
        std::cerr << "beat_grid_probe: --unanalysed takes fill or fill-lock\n";
        return 2;
    }
    if (values.count("--rate")) {
        options.fileSampleRateHz = number("--rate", 0.0);
    }
    if (values.count("--duration-ms")) {
        options.fileDurationMs = number("--duration-ms", 0.0);
    }

    try {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(library);
        // The times given are Engine's already.
        writer.setLeadInSource({});
        const application::BeatGridWriteResult result = writer.writeBeatGrid(trackId, beats, options);
        std::cout << "track " << trackId << ": " << words(result.outcome) << ", " << beats.size() << " beat(s) given, "
                  << result.markers << " marker(s), fingerprint " << result.gridFingerprint << "\n";
        return result.outcome == application::BeatGridWriteResult::Outcome::Written
                || result.outcome == application::BeatGridWriteResult::Outcome::AlreadySame
            ? 0
            : 1;
    } catch (const std::exception &e) {
        std::cerr << "beat_grid_probe: " << e.what() << "\n";
        return 1;
    }
}
