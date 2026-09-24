// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/artwork_rescue_sources.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_artwork_lookup.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/embedded_artwork.hpp"
#endif

#include <algorithm>
#include <system_error>

namespace seabass::gui
{

namespace fs = std::filesystem;

std::vector<fs::path> stickBackupArchives(const std::string &backupDirectory)
{
    std::vector<fs::path> archives;
    if (backupDirectory.empty()) {
        return archives;
    }
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(pathFromUtf8(backupDirectory), ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() == ".zip") {
            archives.push_back(entry.path());
        }
    }
    // Newest first: the closest copy of the stick is the likeliest to
    // hold what it lost, and the search stops at the first hit.
    std::sort(archives.begin(), archives.end(), [](const fs::path &a, const fs::path &b) {
        std::error_code timeEc;
        return fs::last_write_time(a, timeEc) > fs::last_write_time(b, timeEc);
    });
    return archives;
}

struct ArtworkRescueSources::Impl
{
    explicit Impl(std::vector<fs::path> archives) : backups(std::move(archives)) {}
    infrastructure::stick_backup::BackupArtworkLookup backups;
};

ArtworkRescueSources::ArtworkRescueSources(const std::string &backupDirectory, const std::string &stickRoot)
    : m_impl(std::make_shared<Impl>(stickBackupArchives(backupDirectory)))
{
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(pathFromUtf8(stickRoot), ec);
    m_stickRoot = pathToUtf8(ec ? pathFromUtf8(stickRoot) : resolved);
}

std::string ArtworkRescueSources::archivePathFor(const std::string &trackFile) const
{
    if (m_stickRoot.empty() || trackFile.empty() || trackFile.rfind(m_stickRoot, 0) != 0) {
        return {};
    }
    std::string relative = trackFile.substr(m_stickRoot.size());
    while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.erase(relative.begin());
    }
    // Archive paths are written with forward slashes whatever wrote them.
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

ArtworkRescueSources::~ArtworkRescueSources() = default;

std::string ArtworkRescueSources::bytesFor(const infrastructure::engine::ArtworkEntry &entry)
{
#ifdef SEABASS_HAVE_TAGLIB
    if (!entry.trackFile.empty()) {
        std::string tagged = infrastructure::audio::readEmbeddedArtwork(entry.trackFile);
        if (!tagged.empty()) {
            return tagged;
        }
    }
#endif
    // Only a row that still knows its hash can be answered from a backup:
    // the name is the checksum, and without one there is nothing to ask
    // for. entry.reference carries it for exactly those two faults.
    if (entry.storage == infrastructure::engine::ArtworkStorage::CachedFileMissing
        || entry.storage == infrastructure::engine::ArtworkStorage::CachedFileUnreadable) {
        std::string exact = m_impl->backups.find(entry.reference);
        if (!exact.empty()) {
            return exact;
        }
    }
    // No checksum match, or no hash to match with: ask the backups
    // whether any of them knows this same audio file, and take whatever
    // cover that library kept for it. Another library's answer rather
    // than a provably identical file, but a cover is a cover, and this is
    // the last place left to look.
    return m_impl->backups.findForTrack(archivePathFor(entry.trackFile));
}

infrastructure::engine::ArtworkSourceProbe ArtworkRescueSources::probe()
{
    return [this](const infrastructure::engine::ArtworkEntry &entry) { return !bytesFor(entry).empty(); };
}

infrastructure::engine::ArtworkSourceReader ArtworkRescueSources::reader()
{
    return [this](const infrastructure::engine::ArtworkEntry &entry) { return bytesFor(entry); };
}

}  // namespace seabass::gui
