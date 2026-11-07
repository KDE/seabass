// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #50, layer 3: where rekordbox and Engine each put the same instants of
// an MP3, after each analysed it on its own.
//
//   click_positions <stick root>
//
// For every MP3 both the rekordbox export (PIONEER/rekordbox/export.pdb)
// and the Engine Library hold, matched by file path, it prints:
//   - the file's mp3guessenc class and the model's offset
//     (engineToRekordboxOffsetMs: Engine position + offset = rekordbox's);
//   - the first beat at or after 0 of each beat grid, Engine's raw and
//     moved by the offset;
//   - each hot cue (loops too), Engine's raw (what the catalog stores)
//     and corrected (as the reader hands it on, Track::cueTimelineShiftMs
//     added), and per pad both catalogs hold the residual: corrected
//     Engine minus rekordbox, in ms.
// Then a histogram of the cue residuals rounded to the ms, and one of the
// first-beat residuals. On the click files (tests/fixtures/mp3-click)
// analysed by each app, a model that is right gives residuals of 0.
//
// It only reads: the catalogs, the analysis files and the first
// kilobytes of each MP3. A file whose lead-in cannot be read is
// "unreadable", offset 0, as the app treats it.
//
// Exit code 0 when the catalogs were read, 1 when one could not be, 2 for
// a usage error.

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/use_cases/scan_library.hpp"
#include "domain/beat_grid.hpp"
#include "domain/mp3_lead_in.hpp"
#include "domain/track.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using domain::CuePoint;
using domain::Track;

namespace
{

std::string pathKey(const std::string &filePath)
{
    return pathToUtf8(pathFromUtf8(filePath).lexically_normal());
}

std::string relativeToStick(const std::string &filePath, const fs::path &root)
{
    const fs::path relative = pathFromUtf8(filePath).lexically_normal().lexically_relative(root.lexically_normal());
    return relative.empty() ? filePath : pathToUtf8(relative);
}

const char *caseName(domain::Mp3Case c)
{
    switch (c) {
    case domain::Mp3Case::A:
        return "A";
    case domain::Mp3Case::B:
        return "B";
    case domain::Mp3Case::C:
        return "C";
    case domain::Mp3Case::D:
        return "D";
    }
    return "?";
}

std::optional<double> firstBeatMs(const std::vector<domain::Beat> &beats)
{
    for (const domain::Beat &beat : beats) {
        if (beat.timeMs >= 0.0) {
            return beat.timeMs;
        }
    }
    return std::nullopt;
}

std::map<int, const CuePoint *> hotCuesByPad(const Track &track)
{
    std::map<int, const CuePoint *> pads;
    for (const CuePoint &cue : track.cues) {
        if (cue.kind == CuePoint::Kind::Hot) {
            pads.emplace(cue.hotCueNumber, &cue);
        }
    }
    return pads;
}

std::string padName(int number)
{
    return number >= 1 && number <= 26 ? std::string(1, static_cast<char>('A' + number - 1)) : std::to_string(number);
}

void printHistogram(const char *title, const std::map<long, int> &histogram)
{
    int total = 0;
    for (const auto &[ms, count] : histogram) {
        total += count;
    }
    std::printf("%s: %d\n", title, total);
    for (const auto &[ms, count] : histogram) {
        std::printf("  %+5ld ms  %d\n", ms, count);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: click_positions <stick root>\n");
        return 2;
    }
    const fs::path root = pathFromUtf8(argv[1]);
    const fs::path pioneer = root / "PIONEER";
    const fs::path engineLibrary = root / "Engine Library";

    std::vector<Track> rekordboxTracks;
    std::vector<Track> engineTracks;
    std::optional<djinterop::database> engineDb;
    try {
        infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(pathToUtf8(pioneer));
        rekordboxTracks = application::ScanLibrary(rekordbox).execute();
        // The reader's own lead-in source: the cues come out corrected,
        // with what it added recorded in cueTimelineShiftMs.
        rig::EngineReaderAtFileRates engine(engineLibrary);
        engineTracks = application::ScanLibrary(engine.reader()).execute();
        engineDb = djinterop::engine::load_database(pathToUtf8(engineLibrary));
    } catch (const std::exception &e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }

    // A click file goes in as an MP3 or, for the analyser-bias baseline,
    // as a WAV: the WAV has no lead-in, so its offset is 0 and whatever
    // its residual shows is the analysers' own difference.
    const auto isProbeFile = [](const std::string &filePath) {
        const std::string ext = pathToUtf8(pathFromUtf8(filePath).extension());
        return domain::hasMp3Extension(filePath) || ext == ".wav" || ext == ".WAV";
    };
    std::map<std::string, const Track *> engineByPath;
    // Each app may have copied the same file to its own folder (rekordbox
    // under Contents/, Engine under Engine Library/Music/), so a path that
    // matches nowhere falls back to the bare file name, when it is unique.
    std::map<std::string, const Track *> engineByName;
    std::map<std::string, int> engineNameCount;
    for (const Track &track : engineTracks) {
        if (track.streamingSource.empty() && isProbeFile(track.filePath)) {
            engineByPath.emplace(pathKey(track.filePath), &track);
            const std::string name = pathToUtf8(pathFromUtf8(track.filePath).filename());
            engineByName[name] = &track;
            ++engineNameCount[name];
        }
    }

    std::map<long, int> cueResiduals;
    std::map<long, int> beatResiduals;
    int files = 0;
    for (const Track &rb : rekordboxTracks) {
        if (!rb.streamingSource.empty() || !isProbeFile(rb.filePath)) {
            continue;
        }
        const Track *matched = nullptr;
        bool byName = false;
        if (const auto match = engineByPath.find(pathKey(rb.filePath)); match != engineByPath.end()) {
            matched = match->second;
        } else {
            const std::string name = pathToUtf8(pathFromUtf8(rb.filePath).filename());
            const auto count = engineNameCount.find(name);
            if (count != engineNameCount.end() && count->second == 1) {
                matched = engineByName.at(name);
                byName = true;
            }
        }
        if (matched == nullptr) {
            continue;
        }
        const Track &en = *matched;
        ++files;

        std::printf("%s  (rekordbox id %s, Engine id %s%s)\n", relativeToStick(rb.filePath, root).c_str(),
                    rb.sourceId.c_str(), en.sourceId.c_str(),
                    byName ? (", matched by file name to " + relativeToStick(en.filePath, root)).c_str() : "");
        const bool isMp3 = domain::hasMp3Extension(rb.filePath);
        const auto leadIn = isMp3 ? infrastructure::audio::readMp3LeadIn(rb.filePath) : std::optional<domain::Mp3LeadIn>{};
        const double offsetMs = leadIn ? domain::engineToRekordboxOffsetMs(*leadIn) : 0.0;
        if (leadIn) {
            std::printf("  case %s, predicted offset %.3f ms\n", caseName(domain::classify(*leadIn)), offsetMs);
        } else if (!isMp3) {
            std::printf("  WAV baseline, no lead-in, offset 0.000 ms\n");
        } else {
            std::printf("  case unreadable, predicted offset 0.000 ms\n");
        }
        if (std::abs(en.cueTimelineShiftMs - offsetMs) > 1e-9) {
            std::printf("  note: the Engine reader moved this row's cues by %.3f ms, not by the prediction\n",
                        en.cueTimelineShiftMs);
        }

        const auto rbBeat = firstBeatMs(infrastructure::rekordbox::readBeatGrid(pathToUtf8(pioneer), rb.sourceId));
        std::optional<double> enBeat;
        try {
            if (const auto row = engineDb->track_by_id(std::stoll(en.sourceId))) {
                enBeat = firstBeatMs(infrastructure::engine::beatGridOf(*row));
            }
        } catch (const std::exception &) {
        }
        const auto show = [](const std::optional<double> &ms) {
            if (!ms) {
                return std::string("none");
            }
            char text[32];
            std::snprintf(text, sizeof text, "%.3f", *ms + 0.0);
            return std::string(text);
        };
        std::printf("  first beat: rekordbox %s, Engine raw %s, corrected %s", show(rbBeat).c_str(),
                    show(enBeat).c_str(), show(enBeat ? std::optional<double>(*enBeat + offsetMs) : std::nullopt).c_str());
        if (rbBeat && enBeat) {
            const double residual = *enBeat + offsetMs - *rbBeat;
            std::printf(", residual %+.3f ms", residual);
            ++beatResiduals[std::lround(residual)];
        }
        std::printf("\n");

        const auto rbPads = hotCuesByPad(rb);
        const auto enPads = hotCuesByPad(en);
        std::map<int, bool> pads;
        for (const auto &[pad, cue] : rbPads) {
            pads[pad] = true;
        }
        for (const auto &[pad, cue] : enPads) {
            pads[pad] = true;
        }
        for (const auto &[pad, unused] : pads) {
            const auto r = rbPads.find(pad);
            const auto e = enPads.find(pad);
            std::printf("  pad %s:", padName(pad).c_str());
            if (r != rbPads.end()) {
                std::printf(" rekordbox %.3f%s", r->second->positionMs, r->second->isLoop ? " (loop)" : "");
            } else {
                std::printf(" rekordbox none");
            }
            if (e != enPads.end()) {
                const double corrected = e->second->positionMs;
                const double raw = corrected - en.cueTimelineShiftMs;
                std::printf(", Engine raw %.3f, corrected %.3f%s", raw, corrected, e->second->isLoop ? " (loop)" : "");
            } else {
                std::printf(", Engine none");
            }
            if (r != rbPads.end() && e != enPads.end()) {
                const double residual = e->second->positionMs - r->second->positionMs;
                std::printf(", residual %+.3f ms", residual);
                ++cueResiduals[std::lround(residual)];
            }
            std::printf("\n");
        }
    }
    std::printf("%d file(s) in both catalogs\n", files);
    printHistogram("hot cue residuals (corrected Engine minus rekordbox, ms)", cueResiduals);
    printHistogram("first beat residuals (Engine + offset minus rekordbox, ms)", beatResiduals);
    return 0;
}
