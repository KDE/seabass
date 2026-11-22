// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The real probe against pairs whose answer is known without it.
//
// tests/fixtures/alignment/ holds copies of a few signals with silence put
// in front, the start trimmed, another tail, a section cut out, another
// gain, the other polarity, another rate, another codec, and loops that
// repeat bar for bar. tools/make_alignment_fixtures.py made them and
// established each pair's truth by construction and by finding marker
// bursts in ffmpeg's decode of every file (pairs.tsv, files.tsv; the
// directory's README.md says how). This asks the probe the app uses
// (makeAudioAlignmentProbe()) about every pair of one shard, both ways
// round, and holds the answer to the truth:
//
//   * a pair with a true shift: within 0.5 ms of it;
//   * a pair with none (a cut, another signal): unknown;
//   * a pair where more than one answer is right, or where unknown is
//     acceptable (a loop that repeats exactly, an inverted copy): one of
//     those. A wrong value is never acceptable: it moves cues.
//
// Two more checks, so a failure says where it is:
//
//   * decoder agreement: around every marker the two files share, the
//     Qt decoder's samples (the ones the probe correlates) line up by the
//     amount ffmpeg's do. A pair failing this and the shift check is a
//     timeline the decoder and ffmpeg disagree on, not the correlation.
//   * cue timeline, for every pair with an MP3 in it: measureCueShifts()
//     with cueTimelineOffsetMs() gives the decoder's shift plus the two
//     files' rekordbox offsets, which mp3_lead_in_dump read into
//     files.tsv; and cueTimelineOffsetMs() reads each MP3 as recorded.
//
// usage: alignment_fixture_test FIXTURE_DIR SHARD   (SHARD "all" for every pair)

#include <QCoreApplication>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "domain/audio_alignment.hpp"
#include "domain/duplicate_cue_shift.hpp"
#include "domain/matching_policy.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/alignment_probe_factory.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace
{

constexpr double ShiftToleranceMs = 0.5;
// The two decodes are each brought to 22.05 kHz by a box filter; a
// marker's position in them is good to a small part of a sample.
constexpr double AgreementToleranceMs = 0.25;

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

// Not std::stod: QCoreApplication sets the C locale from the environment,
// and a decimal comma would read "20.9977" as 20.
double number(const std::string &text)
{
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        std::cerr << "not a number in the manifest: \"" << text << "\"\n";
        std::abort();
    }
    return value;
}

std::vector<std::vector<std::string>> readTsv(const std::string &path)
{
    std::ifstream in(seabass::pathFromUtf8(path));
    if (!in) {
        std::cerr << "cannot read " << path << "\n";
        std::abort();
    }
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        rows.push_back(split(line, '\t'));
    }
    return rows;
}

struct FileInfo
{
    int rate = 0;
    std::string format;
    double seconds = 0.0;
    std::string leadInClass;
    double cueTimelineOffsetMs = 0.0;
    std::map<std::string, double> markersMs;  // decoder time, as ffmpeg decodes
};

// One answer the truth allows: a shift, or unknown.
struct Answer
{
    bool unknown = false;
    double shiftMs = 0.0;
};

struct Pair
{
    std::string id, shard, a, b, why;
    std::vector<Answer> accepted;  // a to b
};

std::vector<Answer> reversed(const std::vector<Answer> &answers)
{
    std::vector<Answer> out = answers;
    for (Answer &answer : out) {
        answer.shiftMs = -answer.shiftMs;
    }
    return out;
}

bool accepts(const std::vector<Answer> &accepted, const std::optional<double> &got)
{
    for (const Answer &answer : accepted) {
        if (answer.unknown ? !got : (got && std::abs(*got - answer.shiftMs) <= ShiftToleranceMs)) {
            return true;
        }
    }
    return false;
}

std::string describe(const std::vector<Answer> &accepted)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    for (size_t i = 0; i < accepted.size(); ++i) {
        out << (i ? " or " : "");
        if (accepted[i].unknown) {
            out << "unknown";
        } else {
            out << accepted[i].shiftMs;
        }
    }
    return out.str();
}

std::string describe(const std::optional<double> &got)
{
    if (!got) {
        return "unknown";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << *got;
    return out.str();
}

// Where the marker that starts at `atMs` in `b` lines up with the one at
// `atMsA` in `a`, as the probe's own decodes have them: the lag, within a
// few ms of the one the manifest gives, at which the 80 ms from each
// marker's onset correlate best (either polarity), to a fraction of a
// sample. Returned as that lag minus the manifest's, in ms.
// Over what the probe kept of each file (alignment::KeptSamples): a
// marker in a stretch that was not kept is not checked.
std::optional<double> agreementErrorMs(const seabass::domain::alignment::KeptSamples &keptA,
                                       const seabass::domain::alignment::KeptSamples &keptB, double atMsA,
                                       double atMsB)
{
    const double rate = seabass::domain::alignment::AnalysisRate;
    const long long ia = std::llround(atMsA * rate / 1000.0);
    const long long ib = std::llround(atMsB * rate / 1000.0);
    const long long length = std::llround(0.08 * rate);
    const long long search = std::llround(0.003 * rate);
    if (ia < 0 || ib - search < 0) {
        return std::nullopt;
    }
    const auto a = keptA.slice(static_cast<size_t>(ia), static_cast<size_t>(length));
    const auto b = keptB.slice(static_cast<size_t>(ib - search), static_cast<size_t>(2 * search + length));
    if (!a || !b) {
        return std::nullopt;
    }
    std::vector<double> corr;
    for (long long j = -search; j <= search; ++j) {
        double cross = 0.0;
        for (long long i = 0; i < length; ++i) {
            cross += static_cast<double>((*a)[static_cast<size_t>(i)]) * (*b)[static_cast<size_t>(search + j + i)];
        }
        corr.push_back(cross);
    }
    size_t best = 0;
    for (size_t k = 1; k < corr.size(); ++k) {
        if (std::abs(corr[k]) > std::abs(corr[best])) {
            best = k;
        }
    }
    const double sign = corr[best] >= 0 ? 1.0 : -1.0;
    double frac = 0.0;
    if (best > 0 && best + 1 < corr.size()) {
        const double y0 = sign * corr[best - 1];
        const double y1 = sign * corr[best];
        const double y2 = sign * corr[best + 1];
        const double denominator = y0 - 2 * y1 + y2;
        if (denominator != 0.0) {
            frac = 0.5 * (y0 - y2) / denominator;
        }
    }
    const double lag = static_cast<double>(ib - ia) + static_cast<double>(best) - static_cast<double>(search) + frac;
    return lag * 1000.0 / rate - (atMsB - atMsA);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: alignment_fixture_test FIXTURE_DIR SHARD\n";
        return 2;
    }
    const std::filesystem::path dir = seabass::pathFromUtf8(argv[1]);
    const std::string shard = argv[2];
    auto at = [&](const std::string &name) { return seabass::pathToUtf8(dir / seabass::pathFromUtf8(name)); };

    assert(seabass::infrastructure::audio::QtMultimediaAlignmentProbe::decodingAvailable()
           && "this build's decoder must load for this test to mean anything");
    seabass::domain::MatchingPolicy::reset();
    assert(seabass::domain::MatchingPolicy::correctMp3CueTiming());

    std::map<std::string, FileInfo> files;
    for (const auto &row : readTsv(at("files.tsv"))) {
        assert(row.size() == 8);
        FileInfo info;
        info.rate = static_cast<int>(number(row[1]));
        info.format = row[2];
        info.seconds = number(row[3]);
        info.leadInClass = row[4];
        info.cueTimelineOffsetMs = number(row[5]);
        if (row[7] != "-") {
            for (const std::string &marker : split(row[7], ',')) {
                const auto colon = marker.find(':');
                assert(colon != std::string::npos);
                info.markersMs[marker.substr(0, colon)] = number(marker.substr(colon + 1));
            }
        }
        files[row[0]] = info;
    }
    std::vector<Pair> pairs;
    for (const auto &row : readTsv(at("pairs.tsv"))) {
        assert(row.size() == 7);
        if (shard != "all" && row[1] != shard) {
            continue;
        }
        Pair pair{row[0], row[1], row[2], row[3], row[6], {}};
        assert(files.contains(pair.a) && files.contains(pair.b) && "every pair names files in files.tsv");
        pair.accepted.push_back(row[4] == "unknown" ? Answer{true, 0.0} : Answer{false, number(row[4])});
        if (row[5] != "-") {
            for (const std::string &also : split(row[5], '|')) {
                pair.accepted.push_back(also == "unknown" ? Answer{true, 0.0} : Answer{false, number(also)});
            }
        }
        pairs.push_back(pair);
    }
    assert(!pairs.empty() && "the shard names at least one pair");

    // No stick root, so no cache: every pair is measured.
    auto probe = seabass::infrastructure::audio::makeAudioAlignmentProbe({});
    assert(probe->hasDecoder());
    // The same decoder again, for the samples the probe correlates: the
    // agreement check reads them.
    seabass::infrastructure::audio::QtMultimediaAlignmentProbe samples;

    int failures = 0;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "pair                 direction  truth                           probe        ms     verdict\n";

    for (const Pair &pair : pairs) {
        for (int direction = 0; direction < 2; ++direction) {
            const std::string &from = direction == 0 ? pair.a : pair.b;
            const std::string &to = direction == 0 ? pair.b : pair.a;
            const std::vector<Answer> accepted = direction == 0 ? pair.accepted : reversed(pair.accepted);
            const auto start = std::chrono::steady_clock::now();
            const std::optional<double> got = probe->shiftMs(at(from), at(to));
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            const bool ok = accepts(accepted, got);
            if (!ok) {
                ++failures;
            }
            std::cout << std::left << std::setw(21) << pair.id << std::setw(11) << (direction == 0 ? "a->b" : "b->a")
                      << std::setw(32) << describe(accepted) << std::setw(13) << describe(got) << std::setw(7)
                      << took.count() << (ok ? "ok" : "WRONG") << std::right << "\n";
            if (!ok) {
                std::cout << "    " << from << " -> " << to << ": " << pair.why << "\n";
            }
        }
    }

    // Decoder agreement: only where the truth is a shift and the two files
    // share markers.
    std::cout << "\ndecoder agreement (Qt's decode against ffmpeg's, per shared marker):\n";
    for (const Pair &pair : pairs) {
        if (pair.accepted.front().unknown) {
            continue;
        }
        const FileInfo &fa = files[pair.a];
        const FileInfo &fb = files[pair.b];
        const auto a = samples.decoded(at(pair.a));
        const auto b = samples.decoded(at(pair.b));
        assert(a && b && "a fixture file decodes");
        double worst = 0.0;
        int checked = 0;
        for (const auto &[id, msA] : fa.markersMs) {
            const auto inB = fb.markersMs.find(id);
            if (inB == fb.markersMs.end()) {
                continue;
            }
            const auto error = agreementErrorMs(*a, *b, msA, inB->second);
            if (!error) {
                continue;
            }
            ++checked;
            worst = std::max(worst, std::abs(*error));
        }
        if (checked == 0) {
            // A pair with markers in both files that checked none of them
            // checked nothing: a fail, not a pass.
            bool shared = false;
            for (const auto &entry : fa.markersMs) {
                shared = shared || fb.markersMs.contains(entry.first);
            }
            if (shared) {
                ++failures;
                std::cout << "  " << pair.id << ": no shared marker could be checked  FAIL\n";
            }
            continue;
        }
        const bool ok = worst <= AgreementToleranceMs;
        if (!ok) {
            ++failures;
        }
        std::cout << "  " << std::left << std::setw(21) << pair.id << std::right << checked
                  << " marker(s), worst " << std::setprecision(4) << worst << std::setprecision(3) << " ms  " << (ok ? "ok" : "DISAGREE") << "\n";
    }

    // The cue timeline: every pair with an MP3 in it, both ways round.
    std::cout << "\ncue timeline (decoder shift + offset(b) - offset(a)):\n";
    for (const auto &[name, info] : files) {
        if (!seabass::domain::hasMp3Extension(name)) {
            continue;
        }
        bool needed = false;
        for (const Pair &pair : pairs) {
            needed = needed || pair.a == name || pair.b == name;
        }
        if (!needed) {
            continue;
        }
        const std::optional<double> offset = seabass::infrastructure::audio::cueTimelineOffsetMs(at(name));
        const bool ok = offset && std::abs(*offset - info.cueTimelineOffsetMs) < 0.001;
        if (!ok) {
            ++failures;
        }
        std::cout << "  " << name << ": class " << info.leadInClass << ", offset " << describe(offset)
                  << " ms, files.tsv " << info.cueTimelineOffsetMs << " ms  " << (ok ? "ok" : "WRONG") << "\n";
    }
    for (const Pair &pair : pairs) {
        if (!seabass::domain::hasMp3Extension(pair.a) && !seabass::domain::hasMp3Extension(pair.b)) {
            continue;
        }
        for (int direction = 0; direction < 2; ++direction) {
            const std::string &from = direction == 0 ? pair.a : pair.b;
            const std::string &to = direction == 0 ? pair.b : pair.a;
            std::vector<Answer> accepted = direction == 0 ? pair.accepted : reversed(pair.accepted);
            for (Answer &answer : accepted) {
                answer.shiftMs += files[to].cueTimelineOffsetMs - files[from].cueTimelineOffsetMs;
            }
            seabass::domain::Track first;
            first.filePath = at(from);
            first.cues.push_back({seabass::domain::CuePoint::Kind::Hot, 1, 2000.0, "", ""});
            seabass::domain::Track copy;
            copy.filePath = at(to);
            std::vector<seabass::domain::DuplicateGroup> groups(1);
            groups[0].tracks = {first, copy};
            seabass::domain::measureCueShifts(groups, probe.get(), seabass::infrastructure::audio::cueTimelineOffsetMs);
            assert(groups[0].cueShiftsMeasured);
            const std::optional<double> got = seabass::domain::cueShiftBetween(groups[0], 0, 1);
            const bool ok = accepts(accepted, got);
            if (!ok) {
                ++failures;
            }
            std::cout << "  " << std::left << std::setw(21) << pair.id << std::setw(11)
                      << (direction == 0 ? "a->b" : "b->a") << std::setw(32) << describe(accepted)
                      << std::setw(13) << describe(got) << std::right << (ok ? "ok" : "WRONG") << "\n";
        }
    }

    if (failures > 0) {
        std::cout << "\nalignment_fixture_test (" << shard << "): " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "\nalignment_fixture_test (" << shard << "): all checks passed\n";
    return 0;
}
