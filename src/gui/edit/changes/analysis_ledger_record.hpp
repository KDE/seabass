// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/pending_change.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"

namespace seabass::gui
{

class SaveContext;

// The stick's analysis ledger (<stick>/Seabass/analysis.tsv.gz,
// infrastructure/local/analysis_ledger_file.hpp) as a save writes it: the
// track import and Analyse again each note, per change, the entry of the
// track whose analysis they wrote, and the save records the entries of
// the changes that landed once, after the commit.
//
// The file is a backup target of every change that may add to it
// (analysisLedgerBackupTarget(), in the change's filesToBackup()), noted
// absent when the stick has none yet, so the save's backup holds the
// ledger as it was before the save, and Undo Last Save puts it back, or
// takes away the first.

// The ledger's file on the stick at `stickRoot`, to be backed up before a
// save that may write it.
BackupTarget analysisLedgerBackupTarget(const std::string &stickRoot, const std::string &label);

// The ledger's key for an audio file under `stickRoot` (its stick-relative
// path through application::normalizedPathKey), as Beat Grids keys its
// own ledger; empty when the file is not under the root.
std::string analysisLedgerKeyOf(const std::string &audioFile, const std::string &stickRoot);

// The entry for a track whose analysis a change wrote: what made it
// (`provenance`), the audio file on the stick (`fileSize`,
// `fileMtimeUnix`), the grids each library was left with (empty: "-"),
// the key heard and whether a row took it. analysedAtUnix is set when the
// save records it.
infrastructure::local::AnalysisLedgerEntry analysisLedgerEntry(const std::string &pathKey,
                                                               const ImportAnalysis::Provenance &provenance,
                                                               std::uint64_t fileSize, std::int64_t fileMtimeUnix,
                                                               const std::string &rekordboxGrid,
                                                               const std::string &engineGrid, const std::string &key,
                                                               bool keyWritten);

// Notes `entry` as what the change `changeId` wrote on the stick at
// `stickRoot`, and registers, once per save and stick, the record after
// the commit. That record reads the ledger on the stick, sets the entries
// of the changes that landed (in SaveContext::AfterCommit::appliedIds, and
// not skipped), and writes the file only when its text changes. Nothing
// is written when nothing landed: a save that went back leaves the ledger
// as it was. A damaged ledger is replaced, its fault in the save's log. A
// ledger that cannot be opened or read is left as it is and this save's
// entries are not recorded; that, and a ledger that cannot be written, is
// a warning on the save in plain words, never a failure: the libraries
// are committed by then. An entry that cannot be written as it is is left
// out, named in the log, and the others are written.
void recordAnalysisAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                               infrastructure::local::AnalysisLedgerEntry entry, const std::string &label);

// The fingerprint of the grid in the rekordbox analysis file at
// `datPath` as it is on the stick now (rekordbox::pqtzGridFingerprint()),
// for an entry. Never throws: the ledger never costs a save, so a file
// that cannot be read gives an empty fingerprint ("-") and `problem` says
// why; `problem` is cleared otherwise.
std::string ledgerGridOfAnalysisFile(const std::string &datPath, std::string *problem);
// For a test: called with the path before ledgerGridOfAnalysisFile()
// reads it, so a test can stand in for a file that cannot be read by
// throwing. Empty: nothing is called.
void setBeforeLedgerGridReadForTesting(std::function<void(const std::string &datPath)> hook);

// The modification time of a file, seconds since 1970; nullopt when it
// cannot be read.
std::optional<std::int64_t> fileMtimeUnixOf(const std::string &file);

}  // namespace seabass::gui
