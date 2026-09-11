// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/stick_backup/archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::infrastructure::stick_backup
{

// Serves a browsed stick backup's analysis files straight out of the
// archive: one findEntry() plus one read (inflating if the entry was
// deflated), no extraction. See rekordbox::AnlzByteSource for why ANLZ is
// worth reading this way when the databases are not -- it is ~99% of a
// library's metadata bytes and is wanted one track at a time.
//
// Owns both the Zip64Reader and the ArchiveFile it reads through.
// Zip64Reader keeps only a raw `const ArchiveFile *`, so whoever holds a
// reader must outlive the file or the next entry read is a dangling
// pointer -- taking a share of the file here is what makes this source
// safe to hand to a reader and forget about. The reader has already
// validated every offset and count against the file size, so lookups are
// arithmetic on an already-checked central directory.
//
// One instance is shared by every thread that reads the same backup (see
// anlzSourceForPioneerRoot's cache): a scan on a worker and a waveform
// lookup on the UI thread can ask for entries at once. Zip64Reader fills
// its data-offset table lazily from a const method with no lock of its
// own, so read() fills that slot under m_readMutex and does the actual
// read outside it -- every access to the reader goes through here.
class ArchiveAnlzSource : public rekordbox::AnlzByteSource
{
public:
    // `entryPrefix` is what the stick's PIONEER folder is called inside
    // the archive -- "PIONEER/" for a backup taken from a stick root.
    // Passed in rather than assumed so a backup of a differently-nested
    // tree stays readable.
    //
    // `file` is the ArchiveFile `reader` was opened over. Optional only so
    // a caller that keeps the file alive itself (a test) need not pass it;
    // anything longer-lived must.
    ArchiveAnlzSource(std::shared_ptr<const Zip64Reader> reader, std::string entryPrefix,
                       std::shared_ptr<const ArchiveFile> file = nullptr);

    std::optional<std::string> read(const std::string &relativePath) override;

private:
    // Declared before m_reader so it is destroyed after it: the reader
    // must never outlive the file it points at.
    std::shared_ptr<const ArchiveFile> m_file;
    std::shared_ptr<const Zip64Reader> m_reader;
    std::string m_entryPrefix;
    mutable std::mutex m_readMutex;
};

}  // namespace seabass::infrastructure::stick_backup
