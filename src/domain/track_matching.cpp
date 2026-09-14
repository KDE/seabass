// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/track_matching.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace seabass::domain
{

namespace
{

constexpr double PositionToleranceMs = 1000.0;
constexpr double DurationToleranceSeconds = 2.0;

std::vector<CuePoint> sortedCues(std::vector<CuePoint> cues)
{
    std::sort(cues.begin(), cues.end(), [](const CuePoint &a, const CuePoint &b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.hotCueNumber != b.hotCueNumber) {
            return a.hotCueNumber < b.hotCueNumber;
        }
        return a.positionMs < b.positionMs;
    });
    return cues;
}

}  // namespace

std::string normalizeFilename(const std::string &filename)
{
    std::string result;
    result.reserve(filename.size());
    for (unsigned char c : filename) {
        if (!std::isspace(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

std::optional<std::string> titleArtistKey(const Track &track)
{
    if (track.title.empty() || track.artist.empty()) {
        return std::nullopt;
    }
    return normalizeFilename(track.title + "|" + track.artist);
}

std::vector<std::pair<const Track *, const Track *>> matchTracks(const std::vector<Track> &a,
                                                                   const std::vector<Track> &b)
{
    std::map<std::string, const Track *> bByFilePath;
    std::map<std::string, std::vector<const Track *>> bByTitleArtist;
    std::map<std::string, std::vector<const Track *>> bByFilename;
    for (const auto &track : b) {
        if (!track.filePath.empty()) {
            // first-wins if somehow more than one row on this side
            // resolves to the same path -- not expected in practice, and
            // any candidate is as good as another for propagating cues.
            bByFilePath.emplace(track.filePath, &track);
        }
        if (auto key = titleArtistKey(track)) {
            bByTitleArtist[*key].push_back(&track);
        }
        bByFilename[normalizeFilename(track.filename)].push_back(&track);
    }

    std::vector<std::pair<const Track *, const Track *>> matches;
    for (const auto &trackA : a) {
        // Exact resolved file path is the strongest possible signal --
        // the two rows describe the same physical file on the same
        // stick, regardless of which catalog's database is doing the
        // describing, so no metadata/duration check is needed at all.
        // Confirmed on real data to be dramatically more complete than
        // the metadata heuristic below: 100% of rekordbox tracks matched
        // their Engine counterpart by path, vs. ~94% by title+artist+
        // duration even after fixing that heuristic's own duration-0 bug.
        if (!trackA.filePath.empty()) {
            auto pathIt = bByFilePath.find(trackA.filePath);
            if (pathIt != bByFilePath.end()) {
                matches.emplace_back(&trackA, pathIt->second);
                continue;
            }
        }

        const std::vector<const Track *> *candidates = nullptr;

        if (auto key = titleArtistKey(trackA)) {
            auto it = bByTitleArtist.find(*key);
            if (it != bByTitleArtist.end()) {
                candidates = &it->second;
            }
        }
        if (!candidates) {
            auto it = bByFilename.find(normalizeFilename(trackA.filename));
            if (it != bByFilename.end()) {
                candidates = &it->second;
            }
        }
        if (!candidates) {
            continue;
        }

        for (const auto *trackB : *candidates) {
            // durationSeconds == 0 means "unreadable": the same convention
            // used throughout Track's own fields, not a real zero-length
            // track. A track whose length cannot be read cannot be
            // compared on length, so it does not match on title and artist
            // at all -- a radio edit and an extended mix share both, and
            // length is the only thing that tells them apart. Matching
            // anyway would hand one of them the other's cues.
            //
            // This used to be the other way round, and the reason no
            // longer holds. On RV2, Engine reports no length for 1214 of
            // its 1469 local tracks, which once made most of them
            // unmatchable -- so unknown lengths were let through. Every
            // read now fills a missing length from the audio file itself
            // (infrastructure::audio::fillTrackDurations, run by
            // LibraryCatalogCache and the CLI), and on RV2 that leaves
            // exactly one track without a length. What is still zero after
            // that is a file that is broken or missing, and not something
            // a match should guess about.
            //
            // The exact-path branch above is untouched: two rows naming
            // the same file on the same stick are one track with nothing
            // to compare.
            bool bothDurationsKnown = trackA.durationSeconds > 0.0 && trackB->durationSeconds > 0.0;
            if (bothDurationsKnown &&
                std::abs(trackA.durationSeconds - trackB->durationSeconds) <= DurationToleranceSeconds) {
                matches.emplace_back(&trackA, trackB);
                break;  // one match per `a` track is enough for propagating cues
            }
        }
    }
    return matches;
}

bool cueSetsEqual(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    auto sortedA = sortedCues(a);
    auto sortedB = sortedCues(b);
    for (size_t i = 0; i < sortedA.size(); ++i) {
        const auto &x = sortedA[i];
        const auto &y = sortedB[i];
        // comment is deliberately excluded: it's cosmetic label metadata,
        // not core cue data, and RekordboxCueWriter can't write it at all
        // (anlz_cue_codec.cpp always encodes len_comment=0) -- treating a
        // comment difference as a real mismatch made every Engine cue with
        // a label permanently reappear as "needs sync" after a ToRekordbox
        // apply, since no writer could ever make the comment fields agree.
        //
        // color is excluded too, but only for memory cues: Engine's single
        // memory-style cue point ("Cue"/main_cue) has no color at all, so
        // an Engine-read memory CuePoint always has color == "" -- comparing
        // it against a colored rekordbox memory cue would create the exact
        // same unresolvable-forever mismatch the comment exclusion above
        // fixed. Hot cues DO have color on both sides, so it's still
        // compared there.
        if (x.kind != y.kind || x.hotCueNumber != y.hotCueNumber) {
            return false;
        }
        if (x.kind == CuePoint::Kind::Hot && x.color != y.color) {
            return false;
        }
        if (std::abs(x.positionMs - y.positionMs) > PositionToleranceMs) {
            return false;
        }
    }
    return true;
}

}  // namespace seabass::domain
