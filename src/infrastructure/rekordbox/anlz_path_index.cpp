// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_path_index.hpp"

#include <fstream>
#include <stdexcept>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/work_counters.hpp"

namespace seabass::infrastructure::rekordbox
{

using Pdb = rekordbox_pdb_t;

AnlzPathIndex::AnlzPathIndex(const std::string &pioneerRoot)
{
    const std::string pdbPath = pioneerRoot + "/rekordbox/export.pdb";
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }

    WorkCounters::instance().noteTrackDatabaseParse();
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);

    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowTrack = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (!rowTrack) {
                        continue;
                    }
                    std::string analyzePath = sqlText(rowTrack->analyze_path());
                    if (!analyzePath.empty()) {
                        m_pathById.emplace(rowTrack->id(), std::move(analyzePath));
                    }
                }
            }
        });
    }
}

std::optional<std::string> AnlzPathIndex::pathFor(uint32_t trackId) const
{
    auto it = m_pathById.find(trackId);
    if (it == m_pathById.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace seabass::infrastructure::rekordbox
