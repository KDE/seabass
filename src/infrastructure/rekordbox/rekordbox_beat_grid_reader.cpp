// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

#include <sstream>

#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

using Anlz = rekordbox_anlz_t;

std::vector<domain::Beat> readBeatGrid(const std::string &pioneerRoot, const std::string &trackSourceId,
                                       std::shared_ptr<AnlzByteSource> anlzSource)
{
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
        std::istringstream stream(*bytes, std::ios::binary);
        kaitai::kstream ks(&stream);
        Anlz anlz(&ks);

        for (const auto &section : *anlz.sections()) {
            if (section->fourcc() != Anlz::SECTION_TAGS_BEAT_GRID) {
                continue;
            }
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
            return beats;
        }
    } catch (const std::exception &) {
        return {};
    }
    return {};
}

}  // namespace seabass::infrastructure::rekordbox
