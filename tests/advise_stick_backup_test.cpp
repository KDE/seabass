// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/advise_stick_backup.hpp"

using namespace seabass::application;
using namespace seabass::domain;

namespace
{

std::vector<Track> makeLibrary(std::size_t count, int seed)
{
    std::vector<Track> tracks;
    for (std::size_t i = 0; i < count; ++i) {
        Track track;
        track.title = "Track " + std::to_string(i + seed * 100000);
        track.artist = "Artist " + std::to_string((i + seed) % 7);
        track.durationSeconds = 180.0 + static_cast<double>(i % 200);
        CuePoint cue;
        cue.positionMs = 1000.0 * static_cast<double>(1 + i % 5);
        track.cues.push_back(cue);
        tracks.push_back(track);
    }
    return tracks;
}

StickBackupDescription makeBackup(const std::string &path, const std::string &identifier, const std::string &label,
                                  std::int64_t createdAt, const std::vector<Track> &library)
{
    StickBackupDescription backup;
    backup.archivePath = path;
    backup.stickIdentifier = identifier;
    backup.stickLabel = label;
    backup.createdAtUnix = createdAt;
    backup.libraryFingerprint = fingerprintLibrary(library).serialize();
    backup.databaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-" + path}};
    return backup;
}

using Peer = StickBackupAdviceInput::PeerStick;
using Kind = StickBackupAdvice::SourceRef::Kind;

Peer makePeer(const std::string &mount, const std::string &label, const std::vector<Track> &library,
              const std::string &dbHex, std::int64_t modifiedAt, std::uint64_t usedBytes)
{
    Peer peer;
    peer.mountPoint = mount;
    peer.label = label;
    peer.stickIdentifier = "uuid-" + label;
    peer.fingerprint = fingerprintLibrary(library);
    peer.databaseFingerprints = {{"PIONEER/rekordbox/export.pdb", dbHex}};
    peer.catalogModifiedAtUnix = modifiedAt;
    peer.usedBytes = usedBytes;
    return peer;
}

constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;

}  // namespace

int main()
{
    const std::vector<Track> libraryA = makeLibrary(300, 1);
    const std::vector<Track> libraryB = makeLibrary(300, 2);
    const StickBackupDescription backupA = makeBackup("/b/A.zip", "uuid-a", "STICKA", 200, libraryA);
    const StickBackupDescription backupB = makeBackup("/b/B.zip", "uuid-b", "STICKB", 100, libraryB);

    // Nothing in the folder.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        assert(adviseStickBackup(input).state == StickBackupAdvice::State::NoBackups);
        StickBackupDescription broken;
        broken.error = "unreadable";
        input.backups = {broken};
        assert(adviseStickBackup(input).state == StickBackupAdvice::State::NoBackups);
    }

    // Empty stick: its own backup by identifier, else by label, else the newest.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = false;
        input.backups = {backupA, backupB};
        input.stickIdentifier = "uuid-b";
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Restore);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Identifier);
        assert(advice.backupPath == "/b/B.zip");

        input.stickIdentifier = "uuid-fresh-after-format";
        input.stickLabel = "STICKB";
        advice = adviseStickBackup(input);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Label);
        assert(advice.backupPath == "/b/B.zip");

        input.stickLabel = "NEW";
        advice = adviseStickBackup(input);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Newest);
        assert(advice.backupPath == "/b/A.zip");
    }

    // The same library on a different stick (re-imported, new UUID): found
    // by content, current when the database fingerprint still matches.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.stickIdentifier = "uuid-other";
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-/b/B.zip"}};
        input.backups = {backupA, backupB};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Current);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Fingerprint);
        assert(advice.backupPath == "/b/B.zip");
        assert(advice.trackOverlap == 1.0);

        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-changed"}};
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);
        assert(advice.backupPath == "/b/B.zip");
    }

    // Grown since the backup: still that backup, outdated.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(makeLibrary(400, 2));
        input.backups = {backupA, backupB};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Fingerprint);
        assert(advice.backupPath == "/b/B.zip");
    }

    // A library nobody has backed up.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(makeLibrary(300, 3));
        input.backups = {backupA, backupB};
        assert(adviseStickBackup(input).state == StickBackupAdvice::State::BackUpNew);
    }

    // The stick that backup A came from now carries library B: a different
    // library, but B's own backup wins over the identifier.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.stickIdentifier = "uuid-a";
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.backups = {backupA, backupB};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Fingerprint);
        assert(advice.backupPath == "/b/B.zip");

        input.liveFingerprint = fingerprintLibrary(makeLibrary(300, 4));
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::DifferentLibrary);
        assert(advice.backupPath == "/b/A.zip");
    }

    // An old backup without a content fingerprint: the identifier decides,
    // and the database fingerprint says whether it is current.
    {
        StickBackupDescription old = backupA;
        old.libraryFingerprint.clear();
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.stickIdentifier = "uuid-a";
        input.liveFingerprint = fingerprintLibrary(libraryA);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-/b/A.zip"}};
        input.backups = {old};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Current);
        assert(advice.matchedBy == StickBackupAdvice::MatchedBy::Identifier);
    }

    // Same tracks but the cues were lost: outdated with a cue-specific hint.
    {
        std::vector<Track> noCues = libraryA;
        for (Track &track : noCues) {
            track.cues.clear();
        }
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(noCues);
        input.backups = {backupA};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);
        assert(advice.detail.find("cues") != std::string::npos);
    }

    // ---- Peer sticks: creating a backup stick from another stick ----

    // An empty stick next to a stick with a library: clone from it, even
    // when the backup folder is empty. The newest peer wins; the clone
    // must fit.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = false;
        input.freeBytes = 64 * GiB;
        input.peers = {makePeer("/m/old", "OLD", libraryA, "db-old", 100, 20 * GiB),
                       makePeer("/m/new", "NEW", libraryB, "db-new", 200, 20 * GiB)};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::NoBackups);
        assert(advice.cloneSource.kind == Kind::Stick);
        assert(advice.cloneSource.label == "NEW");
        assert(advice.cloneSource.mountPoint == "/m/new");
        assert(advice.cloneSource.modifiedAtUnix == 200);
        assert(advice.cloneSource.enoughSpace);
        assert(advice.updateSource.kind == Kind::None);

        input.freeBytes = 20 * GiB;  // no room for the 64 MiB margin
        advice = adviseStickBackup(input);
        assert(advice.cloneSource.kind == Kind::Stick);
        assert(!advice.cloneSource.enoughSpace);
        assert(advice.cloneSource.detail.find("Not enough space") != std::string::npos);

        input.freeBytes = 0;  // unknown never blocks
        advice = adviseStickBackup(input);
        assert(advice.cloneSource.enoughSpace);

        // With backups on disk too, the restore advice stays as it was.
        input.backups = {backupA, backupB};
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Restore);
        assert(advice.cloneSource.label == "NEW");
    }

    // No peers: nothing to clone from.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = false;
        assert(adviseStickBackup(input).cloneSource.kind == Kind::None);
    }

    // ---- Peer sticks: updating this stick from a newer copy ----

    // Same library on a peer, peer's catalog newer and its database
    // different: update from the peer. Older or equal (within the 2 s FAT
    // tolerance) or in sync: nothing.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.catalogModifiedAtUnix = 500;
        input.usedBytes = 20 * GiB;
        input.freeBytes = 40 * GiB;
        input.peers = {makePeer("/m/peer", "PEER", libraryB, "db-peer", 1000, 20 * GiB)};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::NoBackups);
        assert(advice.updateSource.kind == Kind::Stick);
        assert(advice.updateSource.label == "PEER");
        assert(advice.updateSource.mountPoint == "/m/peer");
        assert(advice.updateSource.enoughSpace);
        assert(!advice.diverged);
        assert(advice.cloneSource.kind == Kind::None);

        input.catalogModifiedAtUnix = 1500;  // this stick is the newest copy
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);

        input.catalogModifiedAtUnix = 999;  // within tolerance: the same time
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);

        input.catalogModifiedAtUnix = 500;
        input.peers[0].databaseFingerprints = input.liveDatabaseFingerprints;  // in sync despite the mtime
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);

        input.peers[0].databaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-peer"}};
        input.catalogModifiedAtUnix = 0;  // unknown: no ordering, no offer
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);
    }

    // The library must fit on this stick's whole volume for an update.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.catalogModifiedAtUnix = 500;
        input.usedBytes = 10 * GiB;
        input.freeBytes = 5 * GiB;
        input.peers = {makePeer("/m/peer", "PEER", libraryB, "db-peer", 1000, 20 * GiB)};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.updateSource.kind == Kind::Stick);
        assert(!advice.updateSource.enoughSpace);
    }

    // A peer with a different library is never an update source (but an
    // empty stick would still clone it).
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.catalogModifiedAtUnix = 500;
        input.peers = {makePeer("/m/peer", "PEER", libraryA, "db-peer", 1000, 20 * GiB)};
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);
    }

    // Rekordbox-only sticks carry no database fingerprint: the content
    // fingerprint decides "in sync".
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.catalogModifiedAtUnix = 500;
        Peer peer = makePeer("/m/peer", "PEER", libraryB, "", 1000, 0);
        peer.databaseFingerprints.clear();
        input.peers = {peer};
        assert(adviseStickBackup(input).updateSource.kind == Kind::None);  // identical content

        input.peers[0].fingerprint = fingerprintLibrary(makeLibrary(310, 2));  // grown on the peer
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.updateSource.kind == Kind::Stick);
    }

    // ---- The disk backup as the newer copy ----

    // The backup was taken from another stick after this one was last
    // written: behind the backup, update the stick from it. Written
    // after the backup: outdated as before.
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.backups = {backupA, backupB};  // B.zip created at 100
        input.catalogModifiedAtUnix = 50;
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::BehindBackup);
        assert(advice.backupPath == "/b/B.zip");
        assert(advice.updateSource.kind == Kind::DiskBackup);
        assert(advice.updateSource.backupPath == "/b/B.zip");
        assert(advice.updateSource.label == "STICKB");
        assert(advice.updateSource.modifiedAtUnix == 100);
        assert(!advice.diverged);

        input.catalogModifiedAtUnix = 500;
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);
        assert(advice.updateSource.kind == Kind::None);

        input.catalogModifiedAtUnix = 0;  // unknown: the old behaviour
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);

        // In sync with the backup: current, whatever the clocks say.
        input.catalogModifiedAtUnix = 50;
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-/b/B.zip"}};
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Current);
        assert(advice.updateSource.kind == Kind::None);
    }

    // ---- Newest copy wins between the backup and a peer; peers win ties ----
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.backups = {backupB};  // created at 100
        input.catalogModifiedAtUnix = 50;
        input.peers = {makePeer("/m/peer", "PEER", libraryB, "db-peer", 200, 0)};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.updateSource.kind == Kind::Stick);
        assert(advice.state == StickBackupAdvice::State::BehindBackup);

        input.peers[0].catalogModifiedAtUnix = 100;  // tie
        assert(adviseStickBackup(input).updateSource.kind == Kind::Stick);

        input.peers[0].catalogModifiedAtUnix = 80;  // backup newer than the peer
        advice = adviseStickBackup(input);
        assert(advice.updateSource.kind == Kind::DiskBackup);
        assert(advice.updateSource.backupPath == "/b/B.zip");
    }

    // ---- Divergence: both copies moved away from the common backup ----
    {
        StickBackupAdviceInput input;
        input.hasLibrary = true;
        input.liveFingerprint = fingerprintLibrary(libraryB);
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.backups = {backupB};  // created at 100, db-/b/B.zip
        input.catalogModifiedAtUnix = 200;
        input.peers = {makePeer("/m/peer", "PEER", libraryB, "db-peer", 300, 0)};
        StickBackupAdvice advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Outdated);
        assert(advice.updateSource.kind == Kind::Stick);
        assert(advice.diverged);
        assert(advice.updateSource.detail.find("discards") != std::string::npos);

        // This stick still matches the backup: a plain fast-forward.
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-/b/B.zip"}};
        advice = adviseStickBackup(input);
        assert(advice.state == StickBackupAdvice::State::Current);
        assert(advice.updateSource.kind == Kind::Stick);
        assert(!advice.diverged);

        // The peer still matches the backup, this stick moved on: the
        // peer is not newer, nothing to update from.
        input.liveDatabaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-mine"}};
        input.peers[0].databaseFingerprints = {{"PIONEER/rekordbox/export.pdb", "db-/b/B.zip"}};
        input.peers[0].catalogModifiedAtUnix = 100;
        advice = adviseStickBackup(input);
        assert(advice.updateSource.kind == Kind::None);
        assert(!advice.diverged);
    }

    // ---- toString round trips for the new values ----
    assert(toString(StickBackupAdvice::State::BehindBackup) == "behind-backup");
    assert(toString(Kind::None) == "none");
    assert(toString(Kind::DiskBackup) == "disk-backup");
    assert(toString(Kind::Stick) == "stick");

    std::cout << "All advise_stick_backup tests passed." << std::endl;
    return 0;
}
