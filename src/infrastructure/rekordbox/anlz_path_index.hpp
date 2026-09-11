// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace seabass::infrastructure::rekordbox
{

// Every track's analysis-file path, from one pass over export.pdb.
//
// findAnlzPathForTrackId() parses the whole 1.4 MB database to answer one
// question, and a save asks it twice per item: once to find the file to
// back up, once inside the cue writer to find the file to write. A
// 200-item save therefore parsed the same database 400 times. Building
// this index costs one pass and answers every lookup from memory.
//
// Deliberately not a field on domain::Track. It is a rekordbox-only
// detail on a type shared by three catalogs, and the catalog cache hands
// out copies that outlive an export.pdb rewrite, so a field would go
// stale silently. An index is built for one save and discarded with it.
class AnlzPathIndex
{
public:
    // One pass over pioneerRoot/rekordbox/export.pdb. Throws if it cannot
    // be opened, the same as the single-id lookup.
    explicit AnlzPathIndex(const std::string &pioneerRoot);

    // The analyze_path as stored in the row, or nothing when the track has
    // none or is not in the database.
    std::optional<std::string> pathFor(uint32_t trackId) const;

    size_t size() const { return m_pathById.size(); }

private:
    std::unordered_map<uint32_t, std::string> m_pathById;
};

}  // namespace seabass::infrastructure::rekordbox
