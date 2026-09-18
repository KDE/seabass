// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

#include <fstream>
#include <sstream>

#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

using Anlz = rekordbox_anlz_t;

domain::TrackAnalysis readTrackAnalysis(const std::string &pioneerRoot, const std::string &trackSourceId,
                                        std::shared_ptr<AnlzByteSource> anlzSource)
{
    domain::TrackAnalysis analysis;
    try {
        auto analyzePath = findAnlzPathForTrackId(pioneerRoot, static_cast<uint32_t>(std::stoul(trackSourceId)));
        if (!analyzePath) {
            return {};
        }

        if (!anlzSource) {
            anlzSource = anlzSourceForPioneerRoot(pioneerRoot);
        }
        auto bytes = anlzSource->read(anlzRelativePath(*analyzePath, /*wantExt=*/false));
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
    return analysis;
}

std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &pioneerRoot,
                                                          const std::string &trackSourceId,
                                                          std::shared_ptr<AnlzByteSource> anlzSource)
{
    return readTrackAnalysis(pioneerRoot, trackSourceId, std::move(anlzSource)).waveform;
}

}  // namespace seabass::infrastructure::rekordbox
