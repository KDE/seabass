// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

#include "infrastructure/stick_backup/archive_file.hpp"

namespace seabass::infrastructure::stick_backup
{

struct RecoveryOutcome
{
    enum class Action
    {
        Nothing,             // no journal: the archive was left in a settled state
        ConfirmedComplete,   // journal outlived a finished update; only the journal was cleared
        RolledBack,          // archive truncated back to its pre-update length
        ClearedCorruptJournal,  // journal never finished being written; archive untouched
    };
    Action action = Action::Nothing;
    std::uint64_t truncatedFrom = 0;
    std::uint64_t truncatedTo = 0;
    std::string detail;
};

// Run before anything else touches an archive. Applies the rule from
// docs/stick-backup-plan.md: a present journal means an update did not
// confirm, but it must not blindly truncate -- the crash may have landed
// after the new trailer was durable and before the journal's clear was.
// So: if the archive parses at its current length, every entry written
// beyond the journaled pre-update length verifies and the manifest is
// intact, the update is complete and only the journal is cleared;
// otherwise the archive is truncated back and the old end-of-central-
// directory record is checked for being where the journal said.
//
// After a successful return the archive either parses, or is empty (a
// rolled-back first backup -- the caller removes the file). Throws
// ArchiveFormatError if neither state can be reached, which means the
// bytes before the journaled length were damaged too and the backup
// needs a human.
RecoveryOutcome recoverOnOpen(ArchiveFile &archive, ArchiveFile &journalFile);

}  // namespace seabass::infrastructure::stick_backup
