// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/restore_stick_backup.hpp"
#include "domain/library_fingerprint.hpp"

namespace seabass::application
{

// What the stick list knows about one mounted stick, every backup in the
// backup folder, and every *other* mounted stick that carries a library.
// Pure input: the GUI gathers it on a worker thread.
struct StickBackupAdviceInput
{
    // Another mounted stick with a library on it: a candidate to copy
    // from (onto an empty stick) or to update this stick from.
    struct PeerStick
    {
        std::string mountPoint;  // identity, handed back in the advice
        std::string label;
        std::string stickIdentifier;
        std::optional<domain::LibraryFingerprint> fingerprint;
        std::map<std::string, std::string> databaseFingerprints;  // same keys as liveDatabaseFingerprints
        std::int64_t catalogModifiedAtUnix = 0;  // 0: unknown
        std::uint64_t usedBytes = 0;             // 0: unknown
    };

    bool hasLibrary = false;
    std::string stickIdentifier;  // infrastructure::system::StickHardwareInfo::stickIdentifier
    std::string stickLabel;
    std::optional<domain::LibraryFingerprint> liveFingerprint;  // nullopt: no library, or unreadable
    // Archive-relative main-database path -> the DbSetFingerprint hex the
    // stick has right now (absent when that database is not on the stick).
    std::map<std::string, std::string> liveDatabaseFingerprints;
    // When the catalog was last written: the newest mtime among the
    // library databases (export.pdb, m.db and its WAL, hm.db). The only
    // signal that orders two copies of the same library; fingerprints
    // say "same" or "different", never "newer". 0: unknown.
    std::int64_t catalogModifiedAtUnix = 0;
    std::uint64_t usedBytes = 0;  // of the whole volume; 0: unknown
    std::uint64_t freeBytes = 0;  // available; 0: unknown
    std::vector<StickBackupDescription> backups;  // unreadable ones (error set) are ignored
    std::vector<PeerStick> peers;
};

// Which backup this stick relates to, and what to offer for it. Decided
// by the library's content fingerprint first, the stick's hardware
// identifier when there is no fingerprint to go on (an older backup, an
// empty stick), then the label, then plain recency. Independently, which
// other copy of the library (a peer stick or the disk backup) this stick
// could be created from or brought up to date from.
struct StickBackupAdvice
{
    enum class State
    {
        NoBackups,         // the folder has no readable backup at all
        Restore,           // no library on the stick: offer to restore `backupPath`
        BackUpNew,         // a library, but no backup of it: offer a fresh backup
        Current,           // `backupPath` holds this library as it is now
        Outdated,          // `backupPath` holds this library, but the stick has changed since: offer to update the backup
        BehindBackup,      // `backupPath` holds a newer copy of this library than the stick: offer to update the stick
        DifferentLibrary,  // `backupPath` was taken from this stick, but holds a different library
    };
    enum class MatchedBy
    {
        None,
        Fingerprint,
        Identifier,
        Label,
        Newest,
    };

    // A copy of a library somewhere else: another mounted stick, or a
    // backup on disk.
    struct SourceRef
    {
        enum class Kind
        {
            None,
            DiskBackup,
            Stick,
        };
        Kind kind = Kind::None;
        std::string label;
        std::string mountPoint;           // Stick only
        std::filesystem::path backupPath;  // DiskBackup only
        std::int64_t modifiedAtUnix = 0;   // the copy's catalog mtime, or the backup's creation time
        bool enoughSpace = true;           // this stick can hold that copy (best effort; false only when known not to)
        std::string detail;                // one sentence for the stick list
    };

    State state = State::NoBackups;
    MatchedBy matchedBy = MatchedBy::None;
    std::filesystem::path backupPath;
    std::string backupLabel;
    std::int64_t backupCreatedAtUnix = 0;
    double trackOverlap = -1.0;  // from the fingerprint comparison, -1 when none was possible
    double cueOverlap = -1.0;
    std::string detail;  // one sentence for the stick list

    // No library on this stick, and a peer stick has one: the newest peer
    // library, to create a backup stick from.
    SourceRef cloneSource;
    // A library on this stick, and a newer copy of the same library exists
    // (a peer stick, or the disk backup): the newest one, to update this
    // stick from. A stick that is itself the newest copy gets none.
    SourceRef updateSource;
    // Both this stick and the update source changed since they last
    // matched the disk backup (the only common ancestor there is): an
    // update would discard this stick's own changes.
    bool diverged = false;
};

StickBackupAdvice adviseStickBackup(const StickBackupAdviceInput &input);

std::string_view toString(StickBackupAdvice::State state);
std::string_view toString(StickBackupAdvice::MatchedBy matchedBy);
std::string_view toString(StickBackupAdvice::SourceRef::Kind kind);

}  // namespace seabass::application
