// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"

#include <stdexcept>

#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::infrastructure::rekordbox
{

RekordboxCleanupWriter::RekordboxCleanupWriter(std::string pioneerRoot) : m_pioneerRoot(std::move(pioneerRoot)) {}

void RekordboxCleanupWriter::removeTrackReplacingWith(const std::string &doomedTrackId,
                                                        const std::string &survivorTrackId)
{
    uint32_t doomedId = static_cast<uint32_t>(std::stoul(doomedTrackId));
    uint32_t survivorId = static_cast<uint32_t>(std::stoul(survivorTrackId));
    std::string pdbPath = m_pioneerRoot + "/rekordbox/export.pdb";

    PdbRowWriter writer(pdbPath);
    if (!writer.trackExists(doomedId)) {
        throw std::runtime_error("no rekordbox track with id=" + doomedTrackId);
    }
    if (!writer.trackExists(survivorId)) {
        throw std::runtime_error("no rekordbox track with id=" + survivorTrackId);
    }
    writer.reassignPlaylistMemberships(doomedId, survivorId);
    writer.removeTrack(doomedId);
    if (!writer.commit()) {
        throw std::runtime_error("failed to write " + pdbPath);
    }
}

}  // namespace seabass::infrastructure::rekordbox
