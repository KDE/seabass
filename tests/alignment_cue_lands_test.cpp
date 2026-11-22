// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A cue moved between copies of one song lands on the sound it marked.
//
// End to end, for Cues on Duplicate Copies and for Clean Up: an Engine
// library made in a scratch directory with two copies of one song (same
// title, artist and length within the matching window), the files a pair
// from tests/fixtures/alignment, a hot cue on one copy exactly on a marker
// burst. Then what the app does: the library read, the duplicates found
// (DuplicateTrackFinder), the shifts measured with the app's probe and
// cue timeline (measureCueShifts), and
//
//   * Copy: DuplicateCueConsolidator's plan staged as a CopyCuesChange
//     and applied;
//   * Clean Up: DuplicateCleanupPlanner's plan, the uncued copy kept (it
//     has the higher bitrate), applied as a CleanupGroupChange through the
//     save loop.
//
// The written cue is read back as Seabass reads it (rekordbox's
// timeline), put on the decoder's with cueTimelineOffsetMs(), and checked
// against the target file itself, twice over: the marker's time in it as
// ffmpeg decodes it (files.tsv, measured by the generator), and the Qt
// decode of both files, where the 80 ms from the cue must be the 80 ms
// the source's cue marked. Within 1 ms.
//
// A pair that does not line up (another signal) copies nothing, and Clean
// Up flags the group (hasCuesOnUnalignedCopy, which leaves it unticked)
// and merges none of its cues.
//
// usage: alignment_cue_lands_test FIXTURE_DIR

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/audio_alignment.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/duplicate_cue_shift.hpp"
#include "domain/matching_policy.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"
#include "gui/edit/changes/copy_cues_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/audio/alignment_probe_factory.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using seabass::domain::Track;

namespace
{

constexpr double LandsWithinMs = 1.0;

std::vector<std::string> split(const std::string &line, char separator)
{
    std::vector<std::string> fields;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, separator)) {
        fields.push_back(field);
    }
    return fields;
}

// Not std::stod: QCoreApplication sets the C locale from the environment.
double number(const std::string &text)
{
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    assert(error == std::errc() && end == text.data() + text.size() && "a number in files.tsv");
    return value;
}

struct FileInfo
{
    int rate = 0;
    double seconds = 0.0;
    std::map<std::string, double> markersMs;
};

std::map<std::string, FileInfo> readFiles(const fs::path &dir)
{
    std::ifstream in(dir / "files.tsv");
    assert(in && "files.tsv is there");
    std::map<std::string, FileInfo> files;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto row = split(line, '\t');
        assert(row.size() == 8);
        FileInfo info;
        info.rate = static_cast<int>(number(row[1]));
        info.seconds = number(row[3]);
        if (row[7] != "-") {
            for (const std::string &marker : split(row[7], ',')) {
                const auto colon = marker.find(':');
                info.markersMs[marker.substr(0, colon)] = number(marker.substr(colon + 1));
            }
        }
        files[row[0]] = info;
    }
    return files;
}

struct Case
{
    std::string name;
    std::string source;  // carries the cue
    std::string target;  // carries none
    bool linesUp = true;
};

// Where the source's sound at `sourceMs` is in the target near
// `targetMs`, as Qt decodes both: the lag within `searchMs` at which the
// 80 ms from each correlate best, as an offset from targetMs, and the
// correlation coefficient there.
struct Found
{
    double offsetMs = 0.0;
    double normalised = 0.0;
};

std::optional<Found> findNear(const std::vector<float> &source, double sourceMs, const std::vector<float> &target,
                              double targetMs, double searchMs)
{
    const double rate = seabass::domain::alignment::AnalysisRate;
    const long long is = std::llround(sourceMs * rate / 1000.0);
    const long long it = std::llround(targetMs * rate / 1000.0);
    const long long length = std::llround(0.08 * rate);
    const long long search = std::llround(searchMs * rate / 1000.0);
    if (is < 0 || is + length > static_cast<long long>(source.size()) || it - search < 0
        || it + search + length > static_cast<long long>(target.size())) {
        return std::nullopt;
    }
    long long bestLag = 0;
    double best = -1.0;
    double bestNormalised = 0.0;
    std::vector<double> corr;
    for (long long j = -search; j <= search; ++j) {
        double cross = 0.0;
        double es = 0.0;
        double et = 0.0;
        for (long long i = 0; i < length; ++i) {
            const double x = source[static_cast<size_t>(is + i)];
            const double y = target[static_cast<size_t>(it + j + i)];
            cross += x * y;
            es += x * x;
            et += y * y;
        }
        corr.push_back(cross);
        if (cross > best) {
            best = cross;
            bestLag = j;
            bestNormalised = es > 0.0 && et > 0.0 ? cross / std::sqrt(es * et) : 0.0;
        }
    }
    const size_t k = static_cast<size_t>(bestLag + search);
    double frac = 0.0;
    if (k > 0 && k + 1 < corr.size()) {
        const double denominator = corr[k - 1] - 2 * corr[k] + corr[k + 1];
        if (denominator != 0.0) {
            frac = 0.5 * (corr[k - 1] - corr[k + 1]) / denominator;
        }
    }
    const double offset = (static_cast<double>(bestLag) + frac) * 1000.0 / rate
        + (static_cast<double>(it) * 1000.0 / rate - targetMs) - (static_cast<double>(is) * 1000.0 / rate - sourceMs);
    return Found{offset, bestNormalised};
}

// A scratch Engine library holding the two copies, the source with one hot
// cue at `cueDecoderMs` on its decoder's timeline (which is Engine's: the
// player's timeline, qt_decoder_timeline_test). The target has the higher
// bitrate, so Clean Up keeps it.
fs::path makeLibrary(const fs::path &fixtures, const std::map<std::string, FileInfo> &files, const Case &c,
                     const std::string &label, double cueDecoderMs)
{
    const fs::path scratch = seabass::testing::scratchRoot() / seabass::pathFromUtf8("seabass_cue_lands_" + c.name + "_" + label);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "Music");
    const fs::path library = scratch / "Engine Library";
    fs::create_directories(library);
    auto db = djinterop::engine::create_database(seabass::pathToUtf8(library));
    for (const std::string *name : {&c.source, &c.target}) {
        fs::copy_file(fixtures / seabass::pathFromUtf8(*name), scratch / "Music" / seabass::pathFromUtf8(*name));
        const FileInfo &info = files.at(*name);
        djinterop::track_snapshot row;
        row.title = "Song " + c.name;
        row.artist = "Alignment Fixture";
        row.relative_path = "../Music/" + *name;
        row.sample_rate = static_cast<double>(info.rate);
        row.sample_count = static_cast<unsigned long long>(std::llround(info.seconds * info.rate));
        row.duration = std::chrono::milliseconds(std::llround(info.seconds * 1000.0));
        row.bitrate = name == &c.source ? 128 : 320;
        if (name == &c.source) {
            row.hot_cues.resize(8);
            row.hot_cues[0] = djinterop::hot_cue{"burst", cueDecoderMs / 1000.0 * info.rate,
                                                 djinterop::engine::standard_pad_colors::pad_1};
        }
        db.create_track(row);
    }
    return library;
}

std::vector<Track> readLibrary(const fs::path &library, bool correctMp3 = true)
{
    seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
    reader.setCorrectMp3CueTiming(correctMp3);
    return reader.readAll();
}

const Track *byFile(const std::vector<Track> &tracks, const std::string &name)
{
    for (const Track &track : tracks) {
        if (seabass::pathToUtf8(seabass::pathFromUtf8(track.filePath).filename()) == name) {
            return &track;
        }
    }
    return nullptr;
}

std::optional<double> hotCueOne(const std::vector<CuePoint> &cues)
{
    for (const CuePoint &cue : cues) {
        if (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == 1) {
            return cue.positionMs;
        }
    }
    return std::nullopt;
}

// The library's one duplicate group, measured as the app measures it.
std::vector<seabass::domain::DuplicateGroup> measuredGroups(const std::vector<Track> &tracks,
                                                            seabass::domain::AudioAlignmentProbe &probe)
{
    auto groups = seabass::domain::DuplicateTrackFinder::find(tracks);
    assert(groups.size() == 1 && groups[0].tracks.size() == 2 && "the two copies are one duplicate group");
    seabass::domain::measureCueShifts(groups, &probe, seabass::infrastructure::audio::cueTimelineOffsetMs);
    return groups;
}

class Checker
{
public:
    Checker(const fs::path &fixtures, const std::map<std::string, FileInfo> &files) : m_fixtures(fixtures), m_files(files) {}

    // The cue as read back, on the target's file: on the marker?
    // `raw` is the same cue as m.db holds it, read without the MP3
    // correction: Engine's timeline, which is the decoder's.
    bool landed(const Case &c, const std::string &path, const std::string &marker, double sourceDecoderMs,
                const Track &target, const std::optional<double> &raw)
    {
        const std::optional<double> written = hotCueOne(target.cues);
        if (!written) {
            std::cout << "    " << path << ": no hot cue 1 on the target\n";
            return false;
        }
        const std::optional<double> offset = seabass::infrastructure::audio::cueTimelineOffsetMs(target.filePath);
        assert(offset);
        const double onDecoder = *written - *offset;
        const double markerInTarget = m_files.at(c.target).markersMs.at(marker);
        const auto source = m_samples.decoded(seabass::pathToUtf8(m_fixtures / seabass::pathFromUtf8(c.source)));
        const auto decodedTarget = m_samples.decoded(target.filePath);
        assert(source && decodedTarget);
        const auto found = findNear(*source, sourceDecoderMs, *decodedTarget, onDecoder, 5.0);
        const double fromMarker = onDecoder - markerInTarget;
        const bool ok = std::abs(fromMarker) <= LandsWithinMs && found && std::abs(found->offsetMs) <= LandsWithinMs
            && found->normalised >= 0.97 && raw && std::abs(*raw - onDecoder) <= LandsWithinMs;
        std::cout << std::fixed << std::setprecision(3) << "    " << std::left << std::setw(6) << path << std::right
                  << " cue " << *written << " ms (rekordbox), " << onDecoder << " ms (decoder); marker " << marker
                  << " at " << markerInTarget << " ms: " << fromMarker << " ms off; Qt decode: ";
        if (found) {
            std::cout << found->offsetMs << " ms off, coefficient " << found->normalised;
        } else {
            std::cout << "out of range";
        }
        std::cout << "; m.db " << (raw ? *raw : -1.0) << " ms  " << (ok ? "ON THE SOUND" : "MISSED") << "\n";
        return ok;
    }

private:
    fs::path m_fixtures;
    const std::map<std::string, FileInfo> &m_files;
    seabass::infrastructure::audio::QtMultimediaAlignmentProbe m_samples;
};

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: alignment_cue_lands_test FIXTURE_DIR\n";
        return 2;
    }
    assert(seabass::infrastructure::audio::QtMultimediaAlignmentProbe::decodingAvailable());
    seabass::domain::MatchingPolicy::reset();
    assert(seabass::domain::MatchingPolicy::correctMp3CueTiming());

    const fs::path fixtures = seabass::pathFromUtf8(argv[1]);
    const auto files = readFiles(fixtures);
    Checker checker(fixtures, files);
    auto probe = seabass::infrastructure::audio::makeAudioAlignmentProbe();
    assert(probe->hasDecoder());

    const std::vector<Case> cases = {
        {"wav160", "a-ref.wav", "a-lead160.wav"},
        {"mp3-to-m4a", "a-lead21-D.mp3", "a-lead160.m4a"},
        {"m4a-to-mp3", "a-lead160.m4a", "a-ref-B.mp3"},
        {"44k-to-48k", "a-ref.flac", "a-48k-lead120.flac"},
        {"loop-1300", "c-ref.flac", "c-lead1300.flac"},
        {"loop-eq-1300", "c-ref.flac", "c-eq-1300.flac"},
        {"unaligned", "a-ref.flac", "a-other.flac", false},
    };

    int failures = 0;
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    for (const Case &c : cases) {
        std::cout << c.name << ": " << c.source << " -> " << c.target << "\n";
        const FileInfo &source = files.at(c.source);
        const FileInfo &target = files.at(c.target);
        // A marker both files have, past the first second and a half (a
        // cue inside the first second is a stray, and the planners leave
        // those out). For the pair that does not line up there is none in
        // common: any marker of the source will do.
        std::string marker;
        for (const auto &[id, ms] : source.markersMs) {
            const auto inTarget = target.markersMs.find(id);
            if (ms >= 1500.0 && (!c.linesUp || (inTarget != target.markersMs.end() && inTarget->second >= 1500.0))
                && (marker.empty() || ms < source.markersMs.at(marker))) {
                marker = id;
            }
        }
        assert(!marker.empty() && "the pair has a marker to put the cue on");
        const double sourceDecoderMs = source.markersMs.at(marker);

        // ---- Cues on Duplicate Copies ----
        {
            const fs::path library = makeLibrary(fixtures, files, c, "copy", sourceDecoderMs);
            const auto tracks = readLibrary(library);
            const auto groups = measuredGroups(tracks, *probe);
            const auto plan = seabass::domain::DuplicateCueConsolidator::plan(groups[0]);
            const Track *targetRow = byFile(tracks, c.target);
            assert(targetRow);
            if (!c.linesUp) {
                const bool held = plan.kind == seabass::domain::ConsolidationPlan::Kind::Unaligned && plan.targets.empty();
                std::cout << "    copy   plan " << (held ? "Unaligned, nothing to copy  OK" : "offers a copy  WRONG") << "\n";
                failures += held ? 0 : 1;
            } else if (plan.kind != seabass::domain::ConsolidationPlan::Kind::Unambiguous) {
                std::cout << "    copy   plan is not a copy (kind " << static_cast<int>(plan.kind) << ")  WRONG\n";
                ++failures;
            } else {
                seabass::gui::DuplicatesCopyOp op{*plan.source, plan.targets, plan.targetShiftMs};
                seabass::gui::CopyCuesChange change(QStringLiteral("engine"), seabass::gui::pathToQString(library),
                                                    QStringLiteral("group"), op);
                seabass::application::CancellationToken token;
                {
                    seabass::gui::SaveContext ctx(token, noProgress, {}, {}, seabass::gui::pathToQString(library));
                    assert(change.apply(ctx).ok);
                    assert(!ctx.runFinishHooks(true).error);
                }
                const auto after = readLibrary(library);
                const Track *written = byFile(after, c.target);
                assert(written);
                const auto rawAfter = readLibrary(library, false);
                const Track *raw = byFile(rawAfter, c.target);
                if (!checker.landed(c, "copy", marker, sourceDecoderMs, *written, raw ? hotCueOne(raw->cues) : std::nullopt)) {
                    ++failures;
                }
            }
        }

        // ---- Clean Up ----
        {
            const fs::path library = makeLibrary(fixtures, files, c, "cleanup", sourceDecoderMs);
            const auto tracks = readLibrary(library);
            const auto groups = measuredGroups(tracks, *probe);
            const auto plan = seabass::domain::DuplicateCleanupPlanner::plan(groups[0]);
            const Track *targetRow = byFile(tracks, c.target);
            assert(targetRow && plan.survivor.sourceId == targetRow->sourceId && "the copy without cues is kept");
            if (!c.linesUp) {
                const bool held = plan.hasCuesOnUnalignedCopy && plan.removedShiftMs.size() == 1
                    && !plan.removedShiftMs[0] && !hotCueOne(plan.mergedCuesForSurvivor);
                std::cout << "    clean  plan " << (held ? "flags the unaligned copy's cues, merges none  OK" : "merges  WRONG")
                          << "\n";
                failures += held ? 0 : 1;
            } else {
                seabass::application::CancellationToken token;
                seabass::gui::SaveContext ctx(token, noProgress, nullptr, {}, seabass::gui::pathToQString(library));
                std::vector<std::shared_ptr<seabass::gui::PendingChange>> changes = {
                    std::make_shared<seabass::gui::CleanupGroupChange>(QStringLiteral("engine"),
                                                                       seabass::gui::pathToQString(library), plan, 1)};
                const auto result = seabass::gui::runSaveLoop(changes, ctx);
                if (!result.error.isEmpty()) {
                    std::cout << "    clean  save failed: " << result.error.toStdString() << "\n";
                    ++failures;
                } else {
                    const auto after = readLibrary(library);
                    assert(after.size() == 1 && "the copy with the cue is removed");
                    const Track *kept = byFile(after, c.target);
                    assert(kept);
                    const auto rawAfter = readLibrary(library, false);
                    const Track *raw = byFile(rawAfter, c.target);
                    if (!checker.landed(c, "clean", marker, sourceDecoderMs, *kept,
                                        raw ? hotCueOne(raw->cues) : std::nullopt)) {
                        ++failures;
                    }
                }
            }
        }
    }

    if (failures > 0) {
        std::cout << "alignment_cue_lands_test: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "alignment_cue_lands_test: every cue landed on its sound\n";
    return 0;
}
