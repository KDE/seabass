// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: what the stick list would offer for each mounted stick --
// rig checks C1 to C4.
//
//   rig_advise <backup dir> <stick root>... [--expect LABEL=STATE[,update=PEER|none][,clone=PEER|none][,diverged]]...
//
// Gathers the facts BackupAdvisorController gathers for every stick given
// (hardware identifier, library fingerprint, catalog modification time,
// database fingerprints, and every backup in <backup dir>), treats each
// stick as the others' peer, and runs the same adviseStickBackup(). Prints
// the state, the backup it matched, the source it would clone or update
// from, and whether the two copies diverged.
//
// With --expect it checks one stick's advice: STATE is the advice state as
// toString() spells it (no-backups, restore, back-up-new, current,
// outdated, behind-backup, different-library), update= the
// label of the stick it must offer to update from (or "none"), clone= the
// stick an empty stick is offered to be created from (or "none"), and
// "diverged" that the divergence warning is raised. Without "diverged" it
// must not be.
//
// Fingerprints are taken without the duration fill (see rig_catalog.hpp),
// so nothing is written to any stick.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "application/use_cases/advise_stick_backup.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/library_catalog_mtime.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::StickBackupAdvice;
using application::StickBackupAdviceInput;

namespace
{

struct Facts
{
    fs::path root;
    std::string label;
    std::string identifier;
    bool hasLibrary = false;
    std::optional<domain::LibraryFingerprint> fingerprint;
    std::map<std::string, std::string> databaseFingerprints;
    std::int64_t catalogModifiedAtUnix = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t freeBytes = 0;
};

struct Expectation
{
    std::string label;
    std::string state;
    std::optional<std::string> update;  // a peer label, or "none"
    std::optional<std::string> clone;   // a peer label, or "none"
    bool diverged = false;
};

Facts gather(const fs::path &root, const std::vector<application::StickBackupDescription> &backups)
{
    namespace sb = infrastructure::stick_backup;
    Facts facts;
    facts.root = root;
    facts.label = rig::stickLabelFor(root);
    const auto hardware = infrastructure::system::readStickHardwareInfo(pathToUtf8(root), facts.label);
    facts.identifier = hardware.stickIdentifier;
    facts.freeBytes = hardware.freeBytes;
    facts.usedBytes = hardware.totalBytes > hardware.freeBytes ? hardware.totalBytes - hardware.freeBytes : 0;
    std::error_code ec;
    facts.hasLibrary = fs::is_directory(root / "PIONEER", ec) || fs::is_directory(root / "Engine Library", ec);
    std::cout << "  " << facts.label << ":\n";
    if (facts.hasLibrary) {
        facts.fingerprint = rig::fingerprintStick(root);
        facts.catalogModifiedAtUnix = sb::libraryCatalogModifiedAt(root);
    }
    // The same databases the backups captured, plus the stick's own Engine
    // database -- exactly the set BackupAdvisorController fingerprints.
    std::set<std::string> databasePaths;
    for (const auto &backup : backups) {
        for (const auto &[path, hex] : backup.databaseFingerprints) {
            databasePaths.insert(path);
        }
    }
    if (facts.hasLibrary) {
        databasePaths.insert(pathToGenericUtf8(infrastructure::engine::engineMainDatabasePath(fs::path())));
    }
    for (const std::string &path : databasePaths) {
        if (const auto fingerprint = sb::fingerprintDbSet(root / pathFromUtf8(path))) {
            facts.databaseFingerprints[path] = fingerprint->toHex();
        }
    }
    return facts;
}

std::optional<Expectation> parseExpectation(const std::string &text)
{
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) {
        return std::nullopt;
    }
    Expectation e;
    e.label = text.substr(0, equals);
    std::stringstream parts(text.substr(equals + 1));
    std::string part;
    bool first = true;
    while (std::getline(parts, part, ',')) {
        if (first) {
            e.state = part;
            first = false;
        } else if (part.rfind("update=", 0) == 0) {
            e.update = part.substr(7);
        } else if (part.rfind("clone=", 0) == 0) {
            e.clone = part.substr(6);
        } else if (part == "diverged") {
            e.diverged = true;
        } else {
            return std::nullopt;
        }
    }
    return e.state.empty() ? std::nullopt : std::optional<Expectation>(e);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: rig_advise <backup dir> <stick root>... "
                     "[--expect LABEL=STATE[,update=PEER|none][,clone=PEER|none][,diverged]]...\n";
        return 2;
    }
    const fs::path backupDir = pathFromUtf8(argv[1]);
    std::vector<fs::path> roots;
    std::vector<Expectation> expectations;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--expect" && i + 1 < argc) {
            const auto e = parseExpectation(argv[++i]);
            if (!e) {
                std::cerr << "cannot read expectation: " << argv[i] << "\n";
                return 2;
            }
            expectations.push_back(*e);
        } else {
            roots.push_back(pathFromUtf8(arg));
        }
    }
    bool pass = true;

    try {
        const auto backups = application::RestoreStickBackup::describeAll(backupDir);
        std::cout << "backups in " << pathToUtf8(backupDir) << ": " << backups.size() << "\n";
        std::cout << "reading sticks:\n";
        std::vector<Facts> sticks;
        for (const fs::path &root : roots) {
            sticks.push_back(gather(root, backups));
        }

        std::map<std::string, StickBackupAdvice> adviceByLabel;
        for (const Facts &facts : sticks) {
            StickBackupAdviceInput input;
            input.hasLibrary = facts.hasLibrary;
            input.stickIdentifier = facts.identifier;
            input.stickLabel = facts.label;
            input.liveFingerprint = facts.fingerprint;
            input.liveDatabaseFingerprints = facts.databaseFingerprints;
            input.catalogModifiedAtUnix = facts.catalogModifiedAtUnix;
            input.usedBytes = facts.usedBytes;
            input.freeBytes = facts.freeBytes;
            input.backups = backups;
            for (const Facts &other : sticks) {
                if (&other == &facts || !other.hasLibrary) {
                    continue;
                }
                StickBackupAdviceInput::PeerStick peer;
                peer.mountPoint = pathToUtf8(other.root);
                peer.label = other.label;
                peer.stickIdentifier = other.identifier;
                peer.fingerprint = other.fingerprint;
                peer.databaseFingerprints = other.databaseFingerprints;
                peer.catalogModifiedAtUnix = other.catalogModifiedAtUnix;
                peer.usedBytes = other.usedBytes;
                input.peers.push_back(std::move(peer));
            }
            const StickBackupAdvice advice = application::adviseStickBackup(input);
            adviceByLabel[facts.label] = advice;
            // The two numbers every ordering here rests on. advise's
            // newerThan() ignores a difference of 2 seconds or less,
            // because FAT keeps mtimes at that resolution, so a round
            // that runs faster than the filesystem can distinguish gets
            // "not newer" for a catalog it has just written. Printed on
            // every advice so a round says which it met rather than
            // leaving the reader to guess from the verdict: Linux runs
            // C1-C5 in under a minute where Windows takes several.
            {
                std::int64_t backupAt = 0;
                for (const auto &backup : backups) {
                    if (backup.stickLabel == advice.backupLabel) {
                        backupAt = backup.createdAtUnix;
                        break;
                    }
                }
                std::cout << "  catalog mtime " << facts.catalogModifiedAtUnix << ", backup " << backupAt;
                if (backupAt > 0 && facts.catalogModifiedAtUnix > 0) {
                    std::cout << ", catalog is " << (facts.catalogModifiedAtUnix - backupAt)
                              << " s past it (2 s or less counts as the same time)";
                }
                std::cout << "\n";
            }
            std::cout << facts.label << ": " << application::toString(advice.state) << " (matched by "
                      << application::toString(advice.matchedBy) << ", backup " << advice.backupLabel << ")"
                      << "\n  " << advice.detail
                      << "\n  update from: " << application::toString(advice.updateSource.kind)
                      << (advice.updateSource.label.empty() ? "" : " " + advice.updateSource.label)
                      << (advice.updateSource.detail.empty() ? "" : " (" + advice.updateSource.detail + ")")
                      << "\n  clone from: " << application::toString(advice.cloneSource.kind)
                      << (advice.cloneSource.label.empty() ? "" : " " + advice.cloneSource.label)
                      << "\n  diverged: " << (advice.diverged ? "yes" : "no") << "\n";
        }

        for (const Expectation &e : expectations) {
            const auto it = adviceByLabel.find(e.label);
            if (it == adviceByLabel.end()) {
                std::cout << "EXPECTED " << e.label << ": no such stick\n";
                pass = false;
                continue;
            }
            const StickBackupAdvice &advice = it->second;
            bool ok = std::string(application::toString(advice.state)) == e.state && advice.diverged == e.diverged;
            if (e.update) {
                const bool noUpdate = advice.updateSource.kind == StickBackupAdvice::SourceRef::Kind::None;
                ok = ok && (*e.update == "none" ? noUpdate : (!noUpdate && advice.updateSource.label == *e.update));
            }
            if (e.clone) {
                const bool noClone = advice.cloneSource.kind == StickBackupAdvice::SourceRef::Kind::None;
                ok = ok && (*e.clone == "none" ? noClone : (!noClone && advice.cloneSource.label == *e.clone));
            }
            std::cout << (ok ? "as expected " : "NOT AS EXPECTED ") << e.label << ": wanted " << e.state
                      << (e.update ? ", update " + *e.update : "") << (e.clone ? ", clone " + *e.clone : "")
                      << (e.diverged ? ", diverged" : ", not diverged")
                      << "\n";
            pass = pass && ok;
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
