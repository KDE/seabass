// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"

namespace seabass::infrastructure::rekordbox
{

// Shared helpers for working with export.pdb, used by both the reader and
// the cue writer.

// Extracts the text from a device_sql_string_t regardless of which of the
// three on-disk string encodings it happens to use.
std::string sqlText(rekordbox_pdb_t::device_sql_string_t *s);

// Path from track_row::analyze_path() is like
// "/PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.DAT"; extended hot-cue data
// (colors/comments, tag "PCO2") lives in the sibling .EXT file, not .DAT.
std::string extAnlzPath(const std::string &pioneerRoot, const std::string &analyzePath);

// Same resolution as extAnlzPath(), but keeps the ".DAT" extension -- for
// tags (like the waveform preview) that live in the base file rather than
// the ".EXT" sibling.
std::string datAnlzPath(const std::string &pioneerRoot, const std::string &analyzePath);

// Walks every page of a table, invoking visitDataPage(page) for each page
// that actually holds row data.
template<typename Visitor>
void forEachDataPage(rekordbox_pdb_t::table_t &table, Visitor visitDataPage)
{
    uint32_t lastIndex = table.last_page()->index();
    rekordbox_pdb_t::page_ref_t *pageRef = table.first_page();
    while (true) {
        rekordbox_pdb_t::page_t *page = pageRef->body();
        if (page->is_data_page()) {
            visitDataPage(page);
        }
        if (page->page_index() == lastIndex) {
            break;
        }
        pageRef = page->next_page();
    }
}

// Looks up a single track's ANLZ .EXT path by its export.pdb track id.
// Returns nullopt if no track with that id exists, or it has no
// analyze_path recorded.
std::optional<std::string> findAnlzPathForTrackId(const std::string &pioneerRoot, uint32_t trackId);

}  // namespace seabass::infrastructure::rekordbox
