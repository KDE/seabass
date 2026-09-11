// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_anlz_source.hpp"

#include <mutex>
#include <utility>

namespace seabass::infrastructure::stick_backup
{

ArchiveAnlzSource::ArchiveAnlzSource(std::shared_ptr<const Zip64Reader> reader, std::string entryPrefix,
                                       std::shared_ptr<const ArchiveFile> file)
    : m_file(std::move(file)), m_reader(std::move(reader)), m_entryPrefix(std::move(entryPrefix))
{
}

std::optional<std::string> ArchiveAnlzSource::read(const std::string &relativePath)
{
    if (!m_reader) {
        return std::nullopt;
    }
    std::optional<std::size_t> index;
    {
        // Held only while the reader's lazily filled offset table can be
        // written: dataOffset() fills the slot for this entry under the
        // lock, so the read below finds it filled and only reads. The
        // read itself -- pread plus inflate into local buffers -- runs
        // unlocked, so a scan on a worker does not serialise every
        // waveform lookup on the UI thread behind it.
        std::lock_guard<std::mutex> lock(m_readMutex);
        index = m_reader->findEntry(m_entryPrefix + relativePath);
        if (index) {
            m_reader->dataOffset(*index);
        }
    }
    if (!index) {
        // A track whose analysis file is not in the backup reads as "no
        // cues", the same as one whose file is missing off a real stick.
        return std::nullopt;
    }
    return m_reader->readEntryToString(*index);
}

}  // namespace seabass::infrastructure::stick_backup
