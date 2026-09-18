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
    // How many tracks on side `a` share each key, so an unknown length can
    // tell a key that names one track from one that names two (see below).
    std::map<std::string, int> aCountByTitleArtist;
    std::map<std::string, int> aCountByFilename;
    for (const auto &track : a) {
        if (auto key = titleArtistKey(track)) {
            aCountByTitleArtist[*key]++;
        }
        aCountByFilename[normalizeFilename(track.filename)]++;
    }
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
        // The key names exactly one track on each side. Only then may a
        // length that cannot be read stand aside -- see the loop below.
        bool unambiguous = false;

        if (auto key = titleArtistKey(trackA)) {
            auto it = bByTitleArtist.find(*key);
            if (it != bByTitleArtist.end()) {
                candidates = &it->second;
                unambiguous = it->second.size() == 1 && aCountByTitleArtist[*key] == 1;
            }
        }
        if (!candidates) {
            const std::string name = normalizeFilename(trackA.filename);
            auto it = bByFilename.find(name);
            if (it != bByFilename.end()) {
                candidates = &it->second;
                unambiguous = it->second.size() == 1 && aCountByFilename[name] == 1;
            }
        }
        if (!candidates) {
            continue;
        }

        for (const auto *trackB : *candidates) {
            // durationSeconds == 0 means "unreadable": the same convention
            // used throughout Track's own fields, not a real zero-length
            // track.
            //
            // Two real lengths have to agree. That is what tells a radio
            // edit from an extended mix filed under one artist and title.
            //
            // A length that cannot be read cannot be compared, so it only
            // stands aside when there is nothing to tell apart: the key
            // names exactly one track on each side. Under a key that names
            // two -- the radio edit and the extended mix, on either side --
            // an unknown length matches nothing, because matching anyway
            // would hand one of them the other's cues.
            //
            // Not stricter than that, although every read now fills
            // missing lengths from the audio file itself
            // (infrastructure::audio::fillTrackDurations, run by
            // LibraryCatalogCache and the CLI). The fill cannot always
            // run: a build without QtMultimedia probes nothing, and a
            // stick missing its audio has nothing to probe. On the
            // committed fixture, which has catalogs and no audio, refusing
            // every unknown length took matching from 1161 tracks to 188.
            //
            // The exact-path branch above is untouched: two rows naming
            // the same file on the same stick are one track with nothing
            // to compare.
            bool bothDurationsKnown = trackA.durationSeconds > 0.0 && trackB->durationSeconds > 0.0;
            bool agree = bothDurationsKnown
                ? std::abs(trackA.durationSeconds - trackB->durationSeconds) <= DurationToleranceSeconds
                : unambiguous;
            if (agree) {
                matches.emplace_back(&trackA, trackB);
                break;  // one match per `a` track is enough for propagating cues
            }
        }
    }
    return matches;
}

std::vector<CuePoint> keepExistingColours(std::vector<CuePoint> incoming, const std::vector<CuePoint> &existing)
{
    for (CuePoint &cue : incoming) {
        if (!cue.color.empty()) {
            continue;  // it brought its own
        }
        for (const CuePoint &had : existing) {
            if (had.color.empty() || had.kind != cue.kind) {
                continue;
            }
            // Engine reads hot cues and hot loops into the same slot
            // numbers, so the slot alone would let a loop's colour land
            // on the cue that shares its number.
            const bool sameCue = had.isLoop == cue.isLoop
                && (cue.kind == CuePoint::Kind::Hot
                        ? had.hotCueNumber == cue.hotCueNumber
                        : std::abs(had.positionMs - cue.positionMs) <= PositionToleranceMs);
            if (sameCue) {
                cue.color = had.color;
                break;
            }
        }
    }
    return incoming;
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
        // Two shades of the same cue are the same cue. Comparing color
        // outright made a cue in the right slot at the right position,
        // differing only in shade, count as a set that disagrees: a
        // conflict to resolve, an offer to write, a row that never
        // settled. Color is extra information about a cue, not what the
        // cue IS.
        //
        // One shade is different in kind from the others, though, and
        // that is no shade at all. A side with no color for a hot cue is
        // a side that has not been told one, so the two sets are not
        // equal and the sync that follows carries the color across. Two
        // real colors that disagree is a DJ recoloring on one side, and
        // it stays where it was put rather than starting an argument
        // nothing can settle.
        //
        // Memory cues are exempt from even that: Engine's single main cue
        // has no color at all, so "one side has none" is its permanent
        // state and would be a mismatch that no writer could ever fix.
        if (x.kind != y.kind || x.hotCueNumber != y.hotCueNumber) {
            return false;
        }
        if (x.kind == CuePoint::Kind::Hot && x.color.empty() != y.color.empty()) {
            return false;
        }
        if (std::abs(x.positionMs - y.positionMs) > PositionToleranceMs) {
            return false;
        }
    }
    return true;
}

}  // namespace seabass::domain
