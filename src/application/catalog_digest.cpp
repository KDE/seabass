// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/catalog_digest.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "infrastructure/hashing/sha256.hpp"

namespace seabass::application
{

namespace
{

// Milliseconds, as an integer, so a value that round-trips through a
// format's own units compares equal. See the header on why this is a
// tolerance and why it is this one.
long long ms(double value)
{
    return static_cast<long long>(std::llround(value));
}

// Two decimals: BPM is authored to one or two, and the formats store it
// in fixed point, so full double precision would report a difference
// that no DJ set has.
std::string bpm(double value)
{
    std::ostringstream out;
    out.precision(2);
    out << std::fixed << value;
    return out.str();
}

std::string cueLine(const domain::CuePoint &cue)
{
    std::ostringstream out;
    out << "  cue\t" << static_cast<int>(cue.kind) << '\t' << cue.hotCueNumber << '\t' << ms(cue.positionMs) << '\t'
        << cue.color << '\t' << cue.comment << '\t' << (cue.isLoop ? 1 : 0) << '\t'
        << (cue.isLoop ? ms(cue.loopEndMs) : 0);
    return out.str();
}

}  // namespace

std::vector<std::string> catalogDigestLines(const std::vector<domain::Track> &tracks)
{
    // Sorted, because "the same library" must not depend on the order a
    // reader happened to walk its rows in -- and a rewrite is exactly the
    // thing that can change that order.
    std::vector<const domain::Track *> ordered;
    ordered.reserve(tracks.size());
    for (const domain::Track &track : tracks) {
        ordered.push_back(&track);
    }
    std::sort(ordered.begin(), ordered.end(), [](const domain::Track *a, const domain::Track *b) {
        if (a->filename != b->filename) {
            return a->filename < b->filename;
        }
        // Two rows for one filename is the duplicate case this project
        // exists to clean up, so it has to be a stable tie-break rather
        // than an assumption that it cannot happen.
        return a->sourceId < b->sourceId;
    });

    std::vector<std::string> lines;
    lines.reserve(tracks.size() * 2);
    lines.push_back("tracks\t" + std::to_string(tracks.size()));
    for (const domain::Track *track : ordered) {
        std::ostringstream out;
        out << "track\t" << track->sourceId << '\t' << track->filename << '\t' << track->title << '\t'
            << track->artist << '\t' << track->album << '\t' << bpm(track->bpm) << '\t' << track->key << '\t'
            << (track->rating ? std::to_string(*track->rating) : "-") << '\t' << track->comment;
        lines.push_back(out.str());

        // Cues sorted too, and for the same reason: a writer that
        // replaces a whole set is free to lay it out differently.
        std::vector<domain::CuePoint> cues = track->cues;
        std::sort(cues.begin(), cues.end(), [](const domain::CuePoint &a, const domain::CuePoint &b) {
            if (a.kind != b.kind) {
                return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            }
            if (a.hotCueNumber != b.hotCueNumber) {
                return a.hotCueNumber < b.hotCueNumber;
            }
            return ms(a.positionMs) < ms(b.positionMs);
        });
        for (const domain::CuePoint &cue : cues) {
            lines.push_back(cueLine(cue));
        }
    }
    return lines;
}

std::string catalogDigest(const std::vector<domain::Track> &tracks)
{
    infrastructure::hashing::Sha256 hasher;
    bool first = true;
    for (const std::string &line : catalogDigestLines(tracks)) {
        if (!first) {
            hasher.update(std::string_view("\n"));
        }
        first = false;
        hasher.update(std::string_view(line));
    }
    return infrastructure::hashing::toHex(hasher.finish());
}

}  // namespace seabass::application
