// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_recovery.hpp"

#include <string>

#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

RecoveryOutcome recoverOnOpen(ArchiveFile &archive, ArchiveFile &journalFile)
{
    RecoveryOutcome outcome;
    JournalState state = journal::read(journalFile);
    if (state.kind == JournalState::Kind::Absent) {
        return outcome;
    }
    if (state.kind == JournalState::Kind::Corrupt) {
        // begin() writes the record and barriers it before the first
        // append, so a half-written journal means nothing was appended.
        journal::clear(journalFile);
        outcome.action = RecoveryOutcome::Action::ClearedCorruptJournal;
        return outcome;
    }

    const JournalRecord &record = state.record;
    if (archive.size() < record.preLength) {
        throw ArchiveFormatError("archive is shorter than its journaled pre-update length");
    }

    std::string error;
    if (verifyArchiveTail(archive, record.preLength, &error)) {
        journal::clear(journalFile);
        outcome.action = RecoveryOutcome::Action::ConfirmedComplete;
        return outcome;
    }

    outcome.truncatedFrom = archive.size();
    outcome.truncatedTo = record.preLength;
    outcome.detail = error;
    archive.truncate(record.preLength);
    archive.barrier();

    if (record.preLength > 0) {
        std::string oldError;
        std::optional<Zip64Reader> old = Zip64Reader::tryOpen(archive, &oldError);
        if (!old || old->layout().endOfCentralDirectoryOffset != record.preEocdOffset) {
            throw ArchiveFormatError("archive does not read back after rolling the update back: "
                                     + (old ? std::string("EOCD not where the journal recorded it") : oldError));
        }
    }
    journal::clear(journalFile);
    outcome.action = RecoveryOutcome::Action::RolledBack;
    return outcome;
}

}  // namespace seabass::infrastructure::stick_backup
