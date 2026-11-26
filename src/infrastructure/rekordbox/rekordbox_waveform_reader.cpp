// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

using Anlz = rekordbox_anlz_t;

domain::TrackAnalysis readTrackAnalysis(const std::string &pioneerRoot, const std::string &trackSourceId,
                                        std::shared_ptr<AnlzByteSource> anlzSource,
                                        const AnlzPathIndex *pathIndex, bool threeBandOverview)
{
    try {
        const auto trackId = static_cast<uint32_t>(std::stoul(trackSourceId));
        auto analyzePath = pathIndex != nullptr ? pathIndex->pathFor(trackId)
                                                : findAnlzPathForTrackId(pioneerRoot, trackId);
        if (!analyzePath) {
            return {};
        }

        if (!anlzSource) {
            anlzSource = anlzSourceForPioneerRoot(pioneerRoot);
        }
        return readTrackAnalysis(*anlzSource, *analyzePath, threeBandOverview);
    } catch (const std::exception &) {
        return {};
    }
}

namespace
{

std::uint32_t sectionU32(const std::string &section, std::size_t at)
{
    const auto *b = reinterpret_cast<const unsigned char *>(section.data() + at);
    return static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16
           | static_cast<std::uint32_t>(b[2]) << 8 | static_cast<std::uint32_t>(b[3]);
}

// rekordbox's three-band bytes, low, mid and high, run to 127.
constexpr double ThreeBandFull = 127.0;

// PWV6 from the .2EX next to the analysis file, read alone: the
// overview rekordbox draws in 3Band, 1,200 entries of low, mid and high.
// Empty where there is no .2EX, no PWV6, or one whose header does not
// add up.
std::vector<domain::WaveformColumn> threeBandOverviewOf(AnlzByteSource &source, const std::string &analyzePath)
{
    std::vector<domain::WaveformColumn> columns;
    const auto section = source.readSection(anlzSiblingRelativePath(analyzePath, ".2EX"), "PWV6");
    if (!section || section->size() < 20) {
        return columns;
    }
    const std::uint32_t headerLength = sectionU32(*section, 4);
    const std::uint32_t entryBytes = sectionU32(*section, 12);
    const std::uint32_t entries = sectionU32(*section, 16);
    if (entryBytes != 3 || entries == 0 || headerLength < 20
        || static_cast<std::uint64_t>(headerLength) + static_cast<std::uint64_t>(entries) * 3 != section->size()) {
        return columns;
    }
    columns.reserve(entries);
    for (std::size_t i = headerLength; i + 2 < section->size(); i += 3) {
        const auto band = [&](std::size_t k) {
            return std::min(1.0, static_cast<unsigned char>((*section)[i + k]) / ThreeBandFull);
        };
        columns.push_back({band(0), band(1), band(2)});
    }
    return columns;
}

}  // namespace

domain::TrackAnalysis readTrackAnalysis(AnlzByteSource &anlzSource, const std::string &analyzePath,
                                        bool threeBandOverview)
{
    domain::TrackAnalysis analysis;
    try {
        auto bytes = anlzSource.read(anlzRelativePath(analyzePath, /*wantExt=*/false));
        if (!bytes || bytes->empty()) {
            return {};
        }
        std::istringstream ifs(*bytes, std::ios::binary);

        kaitai::kstream ks(&ifs);
        Anlz anlz(&ks);

        for (const auto &section : *anlz.sections()) {
            if (section->fourcc() == Anlz::SECTION_TAGS_WAVE_PREVIEW && analysis.waveform.empty()) {
                auto *tag = dynamic_cast<Anlz::wave_preview_tag_t *>(section->body());
                if (!tag || tag->_is_null_data()) {
                    continue;
                }
                std::string data = tag->data();
                analysis.waveform.reserve(data.size());
                for (unsigned char b : data) {
                    double height = (b & 0x1F) / 31.0;
                    double whiteness = ((b >> 5) & 0x07) / 7.0;
                    domain::WaveformColumn col;
                    col.low = height;
                    col.mid = height * (0.55 + 0.45 * whiteness);
                    col.high = height * whiteness;
                    analysis.waveform.push_back(col);
                }
                analysis.waveformLook = domain::WaveformLook::RekordboxBlue;
            } else if (section->fourcc() == Anlz::SECTION_TAGS_BEAT_GRID && analysis.beats.empty()) {
                // PQTZ: rekordbox writes every beat out, with its place in the bar.
                auto *tag = dynamic_cast<Anlz::beat_grid_tag_t *>(section->body());
                if (!tag || tag->beats() == nullptr) {
                    continue;
                }
                std::vector<domain::Beat> beats;
                beats.reserve(tag->beats()->size());
                for (const auto &beat : *tag->beats()) {
                    const int inBar = beat->beat_number();
                    beats.push_back({static_cast<double>(beat->time()), inBar >= 1 && inBar <= 4 ? inBar : 0});
                }
                analysis.beats = domain::beatsInOrder(std::move(beats));
            }
        }
    } catch (const std::exception &) {
        // Half an analysis is no use to anyone who asked for a whole one.
        return {};
    }
    if (threeBandOverview) {
        try {
            if (auto threeBand = threeBandOverviewOf(anlzSource, analyzePath); !threeBand.empty()) {
                analysis.waveform = std::move(threeBand);
                analysis.waveformLook = domain::WaveformLook::RekordboxThreeBand;
            }
        } catch (const std::exception &) {
            // The .DAT's overview stands.
        }
    }
    return analysis;
}

namespace
{

// The sections of one analysis file, or none where it is not there or
// does not parse. Kaitai reads every section it knows; the generated
// waveform bodies hand the entries over as raw bytes.
std::unique_ptr<Anlz> parsedSibling(AnlzByteSource &source, const std::string &analyzePath,
                                    const std::string &extension, std::unique_ptr<std::istringstream> &stream,
                                    std::unique_ptr<kaitai::kstream> &kaitaiStream)
{
    auto bytes = source.read(anlzSiblingRelativePath(analyzePath, extension));
    if (!bytes || bytes->empty()) {
        return nullptr;
    }
    // Kaitai keeps pointers to its stream, so the stream outlives this.
    stream = std::make_unique<std::istringstream>(std::move(*bytes), std::ios::binary);
    kaitaiStream = std::make_unique<kaitai::kstream>(stream.get());
    return std::make_unique<Anlz>(kaitaiStream.get());
}

template<typename Tag>
const Tag *sectionBody(Anlz &anlz, Anlz::section_tags_t fourcc)
{
    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() == fourcc) {
            if (auto *tag = dynamic_cast<const Tag *>(section->body())) {
                return tag;
            }
        }
    }
    return nullptr;
}

// The entries of a preview section, if they are whole entries of the
// expected width: anything else is a section this code does not
// understand, so it is treated as not there.
template<typename Tag>
std::optional<std::string> previewEntries(Anlz &anlz, Anlz::section_tags_t fourcc, std::uint32_t entryBytes)
{
    const Tag *tag = sectionBody<Tag>(anlz, fourcc);
    if (tag == nullptr || tag->len_entry_bytes() != entryBytes || tag->len_entries() == 0) {
        return std::nullopt;
    }
    std::string entries = tag->entries();
    if (entries.size() != static_cast<std::size_t>(tag->len_entries()) * entryBytes) {
        return std::nullopt;
    }
    return entries;
}

template<typename Tag>
std::optional<std::uint32_t> scrollCount(Anlz &anlz, Anlz::section_tags_t fourcc)
{
    const Tag *tag = sectionBody<Tag>(anlz, fourcc);
    if (tag == nullptr || tag->len_entries() == 0) {
        return std::nullopt;
    }
    return tag->len_entries();
}

}  // namespace

RekordboxWaveformSections readWaveformSections(AnlzByteSource &source, const std::string &analyzePath)
{
    RekordboxWaveformSections sections;
    // Each file on its own: a .2EX that does not parse should not cost
    // the .EXT's fallback, nor the other way round.
    try {
        std::unique_ptr<std::istringstream> stream;
        std::unique_ptr<kaitai::kstream> kaitaiStream;
        if (auto anlz = parsedSibling(source, analyzePath, ".2EX", stream, kaitaiStream)) {
            sections.threeBandPreview = previewEntries<Anlz::wave_3band_preview_tag_t>(
                *anlz, Anlz::SECTION_TAGS_WAVE_3BAND_PREVIEW, 3);
            sections.scrollEntries = scrollCount<Anlz::wave_3band_scroll_tag_t>(*anlz,
                                                                               Anlz::SECTION_TAGS_WAVE_3BAND_SCROLL);
        }
    } catch (const std::exception &) {
        sections.threeBandPreview.reset();
        sections.scrollEntries.reset();
    }
    try {
        std::unique_ptr<std::istringstream> stream;
        std::unique_ptr<kaitai::kstream> kaitaiStream;
        if (auto anlz = parsedSibling(source, analyzePath, ".EXT", stream, kaitaiStream)) {
            sections.colourPreview = previewEntries<Anlz::wave_color_preview_tag_t>(
                *anlz, Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW, 6);
            if (!sections.scrollEntries) {
                sections.scrollEntries = scrollCount<Anlz::wave_color_scroll_tag_t>(*anlz,
                                                                                  Anlz::SECTION_TAGS_WAVE_COLOR_SCROLL);
            }
            if (!sections.scrollEntries) {
                sections.scrollEntries = scrollCount<Anlz::wave_scroll_tag_t>(*anlz, Anlz::SECTION_TAGS_WAVE_SCROLL);
            }
        }
    } catch (const std::exception &) {
        sections.colourPreview.reset();
    }
    return sections;
}

RekordboxWaveformSections readWaveformSections(const std::string &pioneerRoot, const std::string &trackSourceId,
                                               std::shared_ptr<AnlzByteSource> anlzSource,
                                               const AnlzPathIndex *pathIndex)
{
    try {
        const auto trackId = static_cast<uint32_t>(std::stoul(trackSourceId));
        auto analyzePath = pathIndex != nullptr ? pathIndex->pathFor(trackId)
                                                : findAnlzPathForTrackId(pioneerRoot, trackId);
        if (!analyzePath) {
            return {};
        }
        if (!anlzSource) {
            anlzSource = anlzSourceForPioneerRoot(pioneerRoot);
        }
        return readWaveformSections(*anlzSource, *analyzePath);
    } catch (const std::exception &) {
        return {};
    }
}

namespace
{

// rekordbox's scrolling waveforms have 150 entries a second.
constexpr double ScrollMsPerEntry = 1000.0 / 150.0;

// A PWAV or PWV3 byte: a 5-bit height and a 3-bit whiteness, all three
// bands the same height, the whiteness tinting it (see readTrackAnalysis()).
domain::WaveformColumn monochromeColumn(unsigned char b)
{
    const double height = (b & 0x1F) / 31.0;
    const double whiteness = ((b >> 5) & 0x07) / 7.0;
    return {height, height * (0.55 + 0.45 * whiteness), height * whiteness};
}

}  // namespace

domain::DetailWaveform readDetailWaveform(AnlzByteSource &source, const std::string &analyzePath)
{
    domain::DetailWaveform detail;
    try {
        std::unique_ptr<std::istringstream> stream;
        std::unique_ptr<kaitai::kstream> kaitaiStream;
        if (auto anlz = parsedSibling(source, analyzePath, ".2EX", stream, kaitaiStream)) {
            if (const auto entries = previewEntries<Anlz::wave_3band_scroll_tag_t>(
                    *anlz, Anlz::SECTION_TAGS_WAVE_3BAND_SCROLL, 3)) {
                detail.columns.reserve(entries->size() / 3);
                for (std::size_t i = 0; i + 2 < entries->size(); i += 3) {
                    const auto band = [&](std::size_t k) {
                        return std::min(1.0, static_cast<unsigned char>((*entries)[i + k]) / ThreeBandFull);
                    };
                    detail.columns.push_back({band(0), band(1), band(2)});
                }
                detail.look = domain::WaveformLook::RekordboxThreeBand;
            }
        }
    } catch (const std::exception &) {
        detail.columns.clear();
    }
    if (detail.columns.empty()) {
        try {
            std::unique_ptr<std::istringstream> stream;
            std::unique_ptr<kaitai::kstream> kaitaiStream;
            if (auto anlz = parsedSibling(source, analyzePath, ".EXT", stream, kaitaiStream)) {
                if (const auto entries = previewEntries<Anlz::wave_scroll_tag_t>(
                        *anlz, Anlz::SECTION_TAGS_WAVE_SCROLL, 1)) {
                    detail.columns.reserve(entries->size());
                    for (unsigned char b : *entries) {
                        detail.columns.push_back(monochromeColumn(b));
                    }
                    detail.look = domain::WaveformLook::RekordboxBlue;
                }
            }
        } catch (const std::exception &) {
            detail.columns.clear();
        }
    }
    detail.msPerColumn = detail.columns.empty() ? 0.0 : ScrollMsPerEntry;
    return detail;
}

domain::DetailWaveform readDetailWaveform(const std::string &pioneerRoot, const std::string &trackSourceId,
                                          std::shared_ptr<AnlzByteSource> anlzSource,
                                          const AnlzPathIndex *pathIndex)
{
    try {
        const auto trackId = static_cast<uint32_t>(std::stoul(trackSourceId));
        auto analyzePath = pathIndex != nullptr ? pathIndex->pathFor(trackId)
                                                : findAnlzPathForTrackId(pioneerRoot, trackId);
        if (!analyzePath) {
            return {};
        }
        if (!anlzSource) {
            anlzSource = anlzSourceForPioneerRoot(pioneerRoot);
        }
        return readDetailWaveform(*anlzSource, *analyzePath);
    } catch (const std::exception &) {
        return {};
    }
}

std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &pioneerRoot,
                                                          const std::string &trackSourceId,
                                                          std::shared_ptr<AnlzByteSource> anlzSource)
{
    return readTrackAnalysis(pioneerRoot, trackSourceId, std::move(anlzSource)).waveform;
}

}  // namespace seabass::infrastructure::rekordbox
