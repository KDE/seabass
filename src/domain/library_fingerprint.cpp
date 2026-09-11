// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/library_fingerprint.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace seabass::domain
{

namespace
{

constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
constexpr std::uint64_t FnvPrime = 1099511628211ull;
constexpr std::size_t MinTracksForVerdict = 5;
constexpr double SameTrackThreshold = 0.85;
constexpr double SameCueThreshold = 0.6;

std::uint64_t fnv1a(std::string_view text, std::uint64_t seed = FnvOffset)
{
    std::uint64_t hash = seed;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= FnvPrime;
    }
    return hash;
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    hash *= FnvPrime;
    return hash;
}

// Lowercase ASCII, trimmed, inner whitespace runs collapsed to one space:
// enough to survive the casing and spacing differences between two
// exports of the same tag without pretending to be Unicode-aware.
std::string normalize(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
    }
    return out;
}

std::vector<std::uint64_t> bottomK(std::set<std::uint64_t> &&hashes)
{
    std::vector<std::uint64_t> sample;
    sample.reserve(std::min(hashes.size(), LibraryFingerprint::SampleSize));
    for (const std::uint64_t hash : hashes) {
        if (sample.size() == LibraryFingerprint::SampleSize) {
            break;
        }
        sample.push_back(hash);
    }
    return sample;
}

void appendHex(std::string &out, std::uint64_t value)
{
    static constexpr char Alphabet[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(Alphabet[(value >> shift) & 0xf]);
    }
}

bool parseHashes(std::string_view text, std::vector<std::uint64_t> &out)
{
    out.clear();
    if (text.empty()) {
        return true;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view item = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        std::uint64_t value = 0;
        const auto result = std::from_chars(item.data(), item.data() + item.size(), value, 16);
        if (item.empty() || result.ec != std::errc() || result.ptr != item.data() + item.size()) {
            return false;
        }
        out.push_back(value);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return std::is_sorted(out.begin(), out.end());
}

bool parseCount(std::string_view text, std::size_t &out)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return !text.empty() && result.ec == std::errc() && result.ptr == text.data() + text.size();
}

// Bottom-k containment estimate: among the k smallest hashes of the union
// (k = the smaller sketch, the range both sketches cover reliably), the
// share of the smaller side's members that the other side also has.
double containment(const std::vector<std::uint64_t> &a, const std::vector<std::uint64_t> &b)
{
    if (a.empty() || b.empty()) {
        return -1.0;
    }
    const std::size_t k = std::min(a.size(), b.size());
    std::vector<std::uint64_t> unionSketch;
    unionSketch.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(unionSketch));
    unionSketch.resize(std::min(k, unionSketch.size()));

    std::size_t inBoth = 0;
    std::size_t inA = 0;
    std::size_t inB = 0;
    for (const std::uint64_t hash : unionSketch) {
        const bool hasA = std::binary_search(a.begin(), a.end(), hash);
        const bool hasB = std::binary_search(b.begin(), b.end(), hash);
        inBoth += hasA && hasB;
        inA += hasA;
        inB += hasB;
    }
    const std::size_t smaller = std::min(inA, inB);
    return smaller == 0 ? 0.0 : static_cast<double>(inBoth) / static_cast<double>(smaller);
}

}  // namespace

std::uint64_t trackIdentityHash(const Track &track)
{
    std::string key = normalize(track.title);
    key.push_back('\x1f');
    key += normalize(track.artist);
    key.push_back('\x1f');
    key += std::to_string(static_cast<long long>(std::llround(track.durationSeconds)));
    return fnv1a(key);
}

LibraryFingerprint fingerprintLibrary(const std::vector<Track> &tracks)
{
    std::set<std::uint64_t> trackHashes;
    std::set<std::uint64_t> cueHashes;
    std::set<std::uint64_t> playlistHashes;
    for (const Track &track : tracks) {
        const std::uint64_t identity = trackIdentityHash(track);
        trackHashes.insert(identity);
        if (!track.cues.empty()) {
            std::vector<long long> positions;
            positions.reserve(track.cues.size());
            for (const CuePoint &cue : track.cues) {
                positions.push_back(std::llround(cue.positionMs / 50.0));
            }
            std::sort(positions.begin(), positions.end());
            std::uint64_t hash = identity;
            for (const long long position : positions) {
                hash = mix(hash, static_cast<std::uint64_t>(position));
            }
            cueHashes.insert(hash);
        }
        for (const PlaylistMembership &playlist : track.playlists) {
            playlistHashes.insert(fnv1a(normalize(playlist.name)));
        }
    }
    LibraryFingerprint fingerprint;
    fingerprint.trackCount = trackHashes.size();
    fingerprint.cuedTrackCount = cueHashes.size();
    fingerprint.playlistCount = playlistHashes.size();
    fingerprint.trackHashes = bottomK(std::move(trackHashes));
    fingerprint.cueHashes = bottomK(std::move(cueHashes));
    fingerprint.playlistHashes = bottomK(std::move(playlistHashes));
    return fingerprint;
}

std::string LibraryFingerprint::serialize() const
{
    std::string out = "v" + std::to_string(Version);
    out += ';' + std::to_string(trackCount);
    out += ';' + std::to_string(cuedTrackCount);
    out += ';' + std::to_string(playlistCount);
    for (const std::vector<std::uint64_t> *hashes : {&trackHashes, &cueHashes, &playlistHashes}) {
        out.push_back(';');
        for (std::size_t i = 0; i < hashes->size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            appendHex(out, (*hashes)[i]);
        }
    }
    return out;
}

std::optional<LibraryFingerprint> LibraryFingerprint::parse(std::string_view text)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t semicolon = text.find(';', start);
        parts.push_back(text.substr(start, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - start));
        if (semicolon == std::string_view::npos) {
            break;
        }
        start = semicolon + 1;
    }
    if (parts.size() != 7 || parts[0] != "v" + std::to_string(Version)) {
        return std::nullopt;
    }
    LibraryFingerprint fingerprint;
    if (!parseCount(parts[1], fingerprint.trackCount) || !parseCount(parts[2], fingerprint.cuedTrackCount)
        || !parseCount(parts[3], fingerprint.playlistCount) || !parseHashes(parts[4], fingerprint.trackHashes)
        || !parseHashes(parts[5], fingerprint.cueHashes) || !parseHashes(parts[6], fingerprint.playlistHashes)) {
        return std::nullopt;
    }
    return fingerprint;
}

FingerprintSimilarity compareFingerprints(const LibraryFingerprint &a, const LibraryFingerprint &b)
{
    FingerprintSimilarity similarity;
    similarity.trackOverlap = containment(a.trackHashes, b.trackHashes);
    similarity.cueOverlap = containment(a.cueHashes, b.cueHashes);
    similarity.playlistOverlap = containment(a.playlistHashes, b.playlistHashes);

    if (std::min(a.trackCount, b.trackCount) < MinTracksForVerdict) {
        similarity.verdict = FingerprintSimilarity::Verdict::Unknown;
        return similarity;
    }
    if (similarity.trackOverlap < SameTrackThreshold) {
        similarity.verdict = FingerprintSimilarity::Verdict::Different;
        return similarity;
    }
    // Cue hashes embed the track hash, so a cue match is also a track
    // match; only the cue evidence decides between the two "same" verdicts.
    const bool aCued = a.cuedTrackCount > 0;
    const bool bCued = b.cuedTrackCount > 0;
    if (!aCued && !bCued) {
        similarity.verdict = FingerprintSimilarity::Verdict::Same;
    } else if (aCued != bCued || similarity.cueOverlap < SameCueThreshold) {
        similarity.verdict = FingerprintSimilarity::Verdict::SameCollectionDifferentState;
    } else {
        similarity.verdict = FingerprintSimilarity::Verdict::Same;
    }
    return similarity;
}

}  // namespace seabass::domain
