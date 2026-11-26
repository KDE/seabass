// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

constexpr std::uint32_t fourccOf(const char (&text)[5])
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8)
           | static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
}

constexpr std::uint32_t Pssi = fourccOf("PSSI");
constexpr std::uint32_t Pqtz = fourccOf("PQTZ");

constexpr std::uint32_t Replaced[] = {fourccOf("PQTZ"), fourccOf("PQT2"), fourccOf("PWAV"), fourccOf("PWV2"),
                                      fourccOf("PWV3"), fourccOf("PWV4"), fourccOf("PWV5"), fourccOf("PWV6"),
                                      fourccOf("PWV7"), fourccOf("PWVC")};

// The last section of `fourcc` in `sections`, or none.
std::optional<std::size_t> lastOf(const std::vector<AnlzRawSection> &sections, std::uint32_t fourcc)
{
    for (std::size_t i = sections.size(); i > 0; --i) {
        if (sections[i - 1].fourcc == fourcc) {
            return i - 1;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> firstOf(const std::vector<AnlzRawSection> &sections, std::uint32_t fourcc)
{
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].fourcc == fourcc) {
            return i;
        }
    }
    return std::nullopt;
}

// The spacing of the grid at beat `i`: to the next beat, or from the one
// before for the last.
double spacingAt(const std::vector<double> &beats, std::size_t i)
{
    return i + 1 < beats.size() ? beats[i + 1] - beats[i] : beats[i] - beats[i - 1];
}

}  // namespace

bool isReplacedAnalysisSection(std::uint32_t fourcc)
{
    for (const std::uint32_t replaced : Replaced) {
        if (fourcc == replaced) {
            return true;
        }
    }
    return false;
}

AnlzFile mergeAnlzAnalysis(const AnlzFile &old, const AnlzFile &built, bool keepPhrases)
{
    AnlzFile merged = old;
    merged.sections.clear();
    std::set<std::uint32_t> placed;
    for (const AnlzRawSection &section : old.sections) {
        if (section.fourcc == Pssi && !keepPhrases) {
            continue;
        }
        if (!isReplacedAnalysisSection(section.fourcc)) {
            merged.sections.push_back(section);
            continue;
        }
        // In the old one's place, once: a second old section of the kind
        // would be a second, older analysis.
        if (placed.count(section.fourcc) > 0) {
            continue;
        }
        if (const auto at = firstOf(built.sections, section.fourcc)) {
            merged.sections.push_back(built.sections[*at]);
            placed.insert(section.fourcc);
        }
    }
    // What the old file lacked, in the built file's order: each beside a
    // neighbour it has there, the ones inserted before it included.
    for (std::size_t j = 0; j < built.sections.size(); ++j) {
        const AnlzRawSection &section = built.sections[j];
        if (!isReplacedAnalysisSection(section.fourcc) || placed.count(section.fourcc) > 0) {
            continue;
        }
        std::optional<std::size_t> insertAt;
        for (std::size_t k = j; k > 0 && !insertAt; --k) {
            if (const auto at = lastOf(merged.sections, built.sections[k - 1].fourcc)) {
                insertAt = *at + 1;
            }
        }
        for (std::size_t k = j + 1; k < built.sections.size() && !insertAt; ++k) {
            if (const auto at = firstOf(merged.sections, built.sections[k].fourcc)) {
                insertAt = *at;
            }
        }
        merged.sections.insert(merged.sections.begin()
                                   + static_cast<std::ptrdiff_t>(insertAt.value_or(merged.sections.size())),
                               section);
        placed.insert(section.fourcc);
    }
    return merged;
}

std::vector<double> pqtzBeatTimes(const AnlzFile &dat)
{
    const auto at = firstOf(dat.sections, Pqtz);
    if (!at) {
        return {};
    }
    const std::string &bytes = dat.sections[*at].rawBytes;
    try {
        // Header: fourcc, len_header, len_tag, then two fields and the
        // beat count; each beat 8 bytes: beat in bar, tempo, time in ms.
        const std::uint32_t lenHeader = readU32BE(bytes, 4);
        if (lenHeader < 24 || lenHeader > bytes.size()) {
            return {};
        }
        const std::uint32_t count = readU32BE(bytes, 20);
        if (static_cast<std::uint64_t>(count) * 8 > bytes.size() - lenHeader) {
            return {};
        }
        std::vector<double> times;
        times.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            times.push_back(static_cast<double>(readU32BE(bytes, lenHeader + i * 8 + 4)));
        }
        return times;
    } catch (const std::out_of_range &) {
        return {};
    }
}

std::string pqtzGridFingerprint(const AnlzFile &dat)
{
    const auto at = firstOf(dat.sections, Pqtz);
    if (!at) {
        return {};
    }
    const std::string &bytes = dat.sections[*at].rawBytes;
    try {
        // As pqtzBeatTimes() reads it: the header's length, then the beat
        // count, then 8 bytes a beat.
        const std::uint32_t lenHeader = readU32BE(bytes, 4);
        if (lenHeader < 24 || lenHeader > bytes.size()) {
            return {};
        }
        const std::uint32_t count = readU32BE(bytes, 20);
        if (count == 0 || static_cast<std::uint64_t>(count) * 8 > bytes.size() - lenHeader) {
            return {};
        }
        const std::string_view beats(bytes.data() + lenHeader, static_cast<std::size_t>(count) * 8);
        return hashing::toHex(hashing::Sha256::of(beats));
    } catch (const std::out_of_range &) {
        return {};
    }
}

bool phrasesFitNewGrid(const std::vector<double> &oldBeats, const std::vector<double> &newBeats)
{
    if (oldBeats.size() < 2 || newBeats.size() < 2) {
        return false;
    }
    const std::size_t last = std::min(oldBeats.size(), newBeats.size()) - 1;
    for (const std::size_t i : {std::size_t{0}, last}) {
        if (std::abs(newBeats[i] - oldBeats[i]) > spacingAt(oldBeats, i) / 4.0) {
            return false;
        }
    }
    return true;
}

std::optional<MergedTrackAnlzFiles> mergeTrackAnlzFiles(const TrackAnlzFiles &old, const std::string &builtDat,
                                                        const std::string &builtExt, const std::string &builtTwoEx)
{
    if (!old.dat) {
        return std::nullopt;
    }
    const AnlzFile dat = AnlzFile::fromBytes(builtDat, "the new .DAT");
    const AnlzFile ext = AnlzFile::fromBytes(builtExt, "the new .EXT");
    const AnlzFile twoEx = AnlzFile::fromBytes(builtTwoEx, "the new .2EX");
    MergedTrackAnlzFiles merged;
    const bool keepPhrases = phrasesFitNewGrid(pqtzBeatTimes(*old.dat), pqtzBeatTimes(dat));
    merged.files.dat = mergeAnlzAnalysis(*old.dat, dat, keepPhrases);
    if (old.ext) {
        merged.files.ext = mergeAnlzAnalysis(*old.ext, ext, keepPhrases);
        if (firstOf(old.ext->sections, Pssi)) {
            (keepPhrases ? merged.phrasesKept : merged.phrasesRemoved) = true;
        }
    }
    merged.files.twoEx = old.twoEx ? mergeAnlzAnalysis(*old.twoEx, twoEx, keepPhrases) : twoEx;
    return merged;
}

}  // namespace seabass::infrastructure::rekordbox
