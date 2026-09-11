// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/advise_stick_backup.hpp"

namespace seabass::application
{

using domain::FingerprintSimilarity;
using domain::LibraryFingerprint;
using PeerStick = StickBackupAdviceInput::PeerStick;
using SourceRef = StickBackupAdvice::SourceRef;

namespace
{

// FAT stores mtimes at 2 s resolution, and two sticks were written by
// the same clock only in the best case: closer than this is "the same
// time", never an ordering.
constexpr std::int64_t MtimeToleranceSeconds = 2;
// Matches RestoreOptions::freeSpaceMarginBytes: a copy has to fit with
// that much to spare.
constexpr std::uint64_t CloneSpaceMarginBytes = 64u << 20;

bool newerThan(std::int64_t a, std::int64_t b)
{
    return a > 0 && b > 0 && a > b + MtimeToleranceSeconds;
}

struct Candidate
{
    const StickBackupDescription *backup = nullptr;
    StickBackupAdvice::MatchedBy matchedBy = StickBackupAdvice::MatchedBy::None;
    FingerprintSimilarity similarity;
};

bool verdictIsSame(FingerprintSimilarity::Verdict verdict)
{
    return verdict == FingerprintSimilarity::Verdict::Same
           || verdict == FingerprintSimilarity::Verdict::SameCollectionDifferentState;
}

// The backup whose content matches best: a plain "same" beats "same
// tracks, different cues", then the higher track overlap.
Candidate bestByFingerprint(const StickBackupAdviceInput &input, const std::vector<const StickBackupDescription *> &readable)
{
    Candidate best;
    if (!input.liveFingerprint) {
        return best;
    }
    for (const StickBackupDescription *backup : readable) {
        const std::optional<LibraryFingerprint> stored = LibraryFingerprint::parse(backup->libraryFingerprint);
        if (!stored) {
            continue;
        }
        const FingerprintSimilarity similarity = domain::compareFingerprints(*input.liveFingerprint, *stored);
        if (!verdictIsSame(similarity.verdict)) {
            continue;
        }
        const bool better = best.backup == nullptr
                            || (similarity.verdict == FingerprintSimilarity::Verdict::Same
                                && best.similarity.verdict != FingerprintSimilarity::Verdict::Same)
                            || (similarity.verdict == best.similarity.verdict
                                && similarity.trackOverlap > best.similarity.trackOverlap);
        if (better) {
            best = {backup, StickBackupAdvice::MatchedBy::Fingerprint, similarity};
        }
    }
    return best;
}

const StickBackupDescription *firstByIdentifier(const StickBackupAdviceInput &input,
                                                const std::vector<const StickBackupDescription *> &readable)
{
    if (input.stickIdentifier.empty()) {
        return nullptr;
    }
    for (const StickBackupDescription *backup : readable) {
        if (backup->stickIdentifier == input.stickIdentifier) {
            return backup;
        }
    }
    return nullptr;
}

const StickBackupDescription *firstByLabel(const StickBackupAdviceInput &input,
                                           const std::vector<const StickBackupDescription *> &readable)
{
    if (input.stickLabel.empty()) {
        return nullptr;
    }
    for (const StickBackupDescription *backup : readable) {
        if (backup->stickLabel == input.stickLabel) {
            return backup;
        }
    }
    return nullptr;
}

// Exact when the backup captured databases: every one of them must still
// be on the stick with the same DbSetFingerprint. Otherwise the content
// fingerprint has to be identical. Shared between "is the backup current
// for this stick" and "is it current for that peer".
bool backupMatchesCopy(const std::map<std::string, std::string> &databaseFingerprints,
                       const std::optional<LibraryFingerprint> &fingerprint, const StickBackupDescription &backup)
{
    if (!backup.databaseFingerprints.empty()) {
        for (const auto &[path, hex] : backup.databaseFingerprints) {
            const auto live = databaseFingerprints.find(path);
            if (live == databaseFingerprints.end() || live->second != hex) {
                return false;
            }
        }
        return true;
    }
    const std::optional<LibraryFingerprint> stored = LibraryFingerprint::parse(backup.libraryFingerprint);
    return stored && fingerprint && *stored == *fingerprint;
}

bool backupIsCurrent(const StickBackupAdviceInput &input, const StickBackupDescription &backup)
{
    return backupMatchesCopy(input.liveDatabaseFingerprints, input.liveFingerprint, backup);
}

// Two sticks hold the same copy when their database sets carry the same
// fingerprints (Engine) or, without any database fingerprint on either
// side (rekordbox only), the content fingerprints are identical.
bool peerInSync(const StickBackupAdviceInput &input, const PeerStick &peer)
{
    if (input.liveDatabaseFingerprints.empty() != peer.databaseFingerprints.empty()) {
        return false;
    }
    if (!input.liveDatabaseFingerprints.empty()) {
        return input.liveDatabaseFingerprints == peer.databaseFingerprints;
    }
    return input.liveFingerprint && peer.fingerprint && *input.liveFingerprint == *peer.fingerprint;
}

bool peerHasSameLibrary(const StickBackupAdviceInput &input, const PeerStick &peer)
{
    if (!input.liveFingerprint || !peer.fingerprint) {
        return false;
    }
    return verdictIsSame(domain::compareFingerprints(*input.liveFingerprint, *peer.fingerprint).verdict);
}

// Best effort: unknown sizes never block; the clone page measures again
// before writing.
bool fitsOn(std::uint64_t availableBytes, std::uint64_t neededBytes)
{
    return availableBytes == 0 || neededBytes == 0 || availableBytes >= neededBytes + CloneSpaceMarginBytes;
}

SourceRef peerSource(const PeerStick &peer, bool enoughSpace)
{
    SourceRef source;
    source.kind = SourceRef::Kind::Stick;
    source.label = peer.label;
    source.mountPoint = peer.mountPoint;
    source.modifiedAtUnix = peer.catalogModifiedAtUnix;
    source.enoughSpace = enoughSpace;
    return source;
}

SourceRef diskSource(const StickBackupDescription &backup, bool enoughSpace)
{
    SourceRef source;
    source.kind = SourceRef::Kind::DiskBackup;
    source.label = backup.stickLabel;
    source.backupPath = backup.archivePath;
    source.modifiedAtUnix = backup.createdAtUnix;
    source.enoughSpace = enoughSpace;
    return source;
}

void fillBackup(StickBackupAdvice &advice, const StickBackupDescription &backup)
{
    advice.backupPath = backup.archivePath;
    advice.backupLabel = backup.stickLabel;
    advice.backupCreatedAtUnix = backup.createdAtUnix;
}

// The stick against the disk backups only: the original advice.
StickBackupAdvice adviseDiskBackup(const StickBackupAdviceInput &input, const StickBackupDescription **matched)
{
    StickBackupAdvice advice;
    *matched = nullptr;
    std::vector<const StickBackupDescription *> readable;
    for (const StickBackupDescription &backup : input.backups) {
        if (backup.error.empty()) {
            readable.push_back(&backup);
        }
    }
    if (readable.empty()) {
        advice.state = StickBackupAdvice::State::NoBackups;
        advice.detail = input.hasLibrary ? "No backup of this library yet." : "No backups to restore yet.";
        return advice;
    }

    if (!input.hasLibrary) {
        advice.state = StickBackupAdvice::State::Restore;
        if (const StickBackupDescription *backup = firstByIdentifier(input, readable)) {
            fillBackup(advice, *backup);
            advice.matchedBy = StickBackupAdvice::MatchedBy::Identifier;
            advice.detail = "This stick's own backup can be restored onto it.";
        } else if (const StickBackupDescription *byLabel = firstByLabel(input, readable)) {
            fillBackup(advice, *byLabel);
            advice.matchedBy = StickBackupAdvice::MatchedBy::Label;
            advice.detail = "A backup with this stick's name can be restored onto it.";
        } else {
            fillBackup(advice, *readable.front());
            advice.matchedBy = StickBackupAdvice::MatchedBy::Newest;
            advice.detail = "The newest backup can be restored onto this empty stick.";
        }
        return advice;
    }

    Candidate candidate = bestByFingerprint(input, readable);
    const StickBackupDescription *byIdentifier = firstByIdentifier(input, readable);
    if (candidate.backup == nullptr && byIdentifier != nullptr) {
        candidate.backup = byIdentifier;
        candidate.matchedBy = StickBackupAdvice::MatchedBy::Identifier;
        const std::optional<LibraryFingerprint> stored = LibraryFingerprint::parse(byIdentifier->libraryFingerprint);
        if (stored && input.liveFingerprint) {
            candidate.similarity = domain::compareFingerprints(*input.liveFingerprint, *stored);
            if (candidate.similarity.verdict == FingerprintSimilarity::Verdict::Different) {
                fillBackup(advice, *byIdentifier);
                advice.state = StickBackupAdvice::State::DifferentLibrary;
                advice.matchedBy = StickBackupAdvice::MatchedBy::Identifier;
                advice.trackOverlap = candidate.similarity.trackOverlap;
                advice.cueOverlap = candidate.similarity.cueOverlap;
                advice.detail = "This stick's previous backup holds a different library.";
                return advice;
            }
        }
    }
    if (candidate.backup == nullptr) {
        advice.state = StickBackupAdvice::State::BackUpNew;
        advice.detail = "No backup of this library yet.";
        return advice;
    }

    *matched = candidate.backup;
    fillBackup(advice, *candidate.backup);
    advice.matchedBy = candidate.matchedBy;
    advice.trackOverlap = candidate.similarity.trackOverlap;
    advice.cueOverlap = candidate.similarity.cueOverlap;
    if (backupIsCurrent(input, *candidate.backup)) {
        advice.state = StickBackupAdvice::State::Current;
        advice.detail = "Backup is up to date.";
    } else if (newerThan(candidate.backup->createdAtUnix, input.catalogModifiedAtUnix)) {
        advice.state = StickBackupAdvice::State::BehindBackup;
        advice.detail = "The backup holds a newer copy of this library than the stick.";
    } else {
        advice.state = StickBackupAdvice::State::Outdated;
        advice.detail = candidate.similarity.verdict == FingerprintSimilarity::Verdict::SameCollectionDifferentState
                            ? "Same tracks as the backup, but the cues differ: update it to keep them."
                            : "The library has changed since its last backup.";
    }
    return advice;
}

// Empty stick: the newest peer library is what a backup stick would be
// created from.
void adviseClone(const StickBackupAdviceInput &input, StickBackupAdvice &advice)
{
    const PeerStick *best = nullptr;
    for (const PeerStick &peer : input.peers) {
        if (best == nullptr || peer.catalogModifiedAtUnix > best->catalogModifiedAtUnix) {
            best = &peer;
        }
    }
    if (best == nullptr) {
        return;
    }
    const bool enoughSpace = fitsOn(input.freeBytes, best->usedBytes);
    advice.cloneSource = peerSource(*best, enoughSpace);
    advice.cloneSource.detail = enoughSpace ? "Copy " + best->label + "'s library onto this stick."
                                            : "Not enough space on this stick for " + best->label + "'s library.";
}

// Library stick: among the copies of the same library that are newer
// than this one, the newest. A peer wins a tie against the disk backup:
// it is live, the backup is a snapshot.
void adviseUpdate(const StickBackupAdviceInput &input, const StickBackupDescription *matched, StickBackupAdvice &advice)
{
    const std::uint64_t capacityBytes = input.usedBytes + input.freeBytes;
    const PeerStick *newestPeer = nullptr;
    for (const PeerStick &peer : input.peers) {
        if (!peerHasSameLibrary(input, peer) || peerInSync(input, peer)
            || !newerThan(peer.catalogModifiedAtUnix, input.catalogModifiedAtUnix)) {
            continue;
        }
        if (newestPeer == nullptr || peer.catalogModifiedAtUnix > newestPeer->catalogModifiedAtUnix) {
            newestPeer = &peer;
        }
    }
    const bool diskIsNewer = advice.state == StickBackupAdvice::State::BehindBackup && matched != nullptr;

    if (newestPeer != nullptr && (!diskIsNewer || newestPeer->catalogModifiedAtUnix >= matched->createdAtUnix)) {
        advice.updateSource = peerSource(*newestPeer, fitsOn(capacityBytes, newestPeer->usedBytes));
        // Diverged: the disk backup is the common ancestor, and both
        // copies moved away from it.
        if (matched != nullptr && !backupIsCurrent(input, *matched)
            && !backupMatchesCopy(newestPeer->databaseFingerprints, newestPeer->fingerprint, *matched)
            && newerThan(input.catalogModifiedAtUnix, matched->createdAtUnix)
            && newerThan(newestPeer->catalogModifiedAtUnix, matched->createdAtUnix)) {
            advice.diverged = true;
        }
        advice.updateSource.detail =
            advice.diverged ? "Both this stick and " + newestPeer->label + " changed since the last backup: updating from "
                                  + newestPeer->label + " discards this stick's own changes."
                            : newestPeer->label + " holds a newer copy of this library.";
    } else if (diskIsNewer) {
        advice.updateSource = diskSource(*matched, fitsOn(capacityBytes, matched->archiveBytes));
        advice.updateSource.detail = "The backup holds a newer copy of this library than this stick.";
    }
    if (!advice.updateSource.enoughSpace) {
        advice.updateSource.detail = "Not enough space on this stick for " + advice.updateSource.label + "'s library.";
    }
}

}  // namespace

StickBackupAdvice adviseStickBackup(const StickBackupAdviceInput &input)
{
    const StickBackupDescription *matched = nullptr;
    StickBackupAdvice advice = adviseDiskBackup(input, &matched);
    if (input.hasLibrary) {
        adviseUpdate(input, matched, advice);
    } else {
        adviseClone(input, advice);
    }
    return advice;
}

std::string_view toString(StickBackupAdvice::State state)
{
    switch (state) {
    case StickBackupAdvice::State::NoBackups: return "no-backups";
    case StickBackupAdvice::State::Restore: return "restore";
    case StickBackupAdvice::State::BackUpNew: return "back-up-new";
    case StickBackupAdvice::State::Current: return "current";
    case StickBackupAdvice::State::Outdated: return "outdated";
    case StickBackupAdvice::State::BehindBackup: return "behind-backup";
    case StickBackupAdvice::State::DifferentLibrary: return "different-library";
    }
    return "no-backups";
}

std::string_view toString(StickBackupAdvice::MatchedBy matchedBy)
{
    switch (matchedBy) {
    case StickBackupAdvice::MatchedBy::None: return "none";
    case StickBackupAdvice::MatchedBy::Fingerprint: return "fingerprint";
    case StickBackupAdvice::MatchedBy::Identifier: return "identifier";
    case StickBackupAdvice::MatchedBy::Label: return "label";
    case StickBackupAdvice::MatchedBy::Newest: return "newest";
    }
    return "none";
}

std::string_view toString(StickBackupAdvice::SourceRef::Kind kind)
{
    switch (kind) {
    case StickBackupAdvice::SourceRef::Kind::None: return "none";
    case StickBackupAdvice::SourceRef::Kind::DiskBackup: return "disk-backup";
    case StickBackupAdvice::SourceRef::Kind::Stick: return "stick";
    }
    return "none";
}

}  // namespace seabass::application
