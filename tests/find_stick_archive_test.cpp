// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A named backup has to be findable again.
//
// The Back Up page builds its archive path from the stick's LABEL, and a
// backup that was given a name is renamed to that name. So the page went
// looking for <label>.zip, found nothing, said "no backup of this stick
// yet", and the obvious next action wrote a SECOND full archive under
// the label while the named one sat beside it, invisible to the page
// that made it.
//
// The default name is the stick's own label, which maps to the same
// filename, so no rename happens and every test written against the
// default found its archive again. Only someone who typed a real name --
// the person who cared enough to -- ever met this.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "application/find_stick_archive.hpp"
#include "application/use_cases/backup_stick.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::application;
namespace fs = std::filesystem;

namespace
{

fs::path makeStick(const fs::path &root, const std::string &name)
{
    const fs::path stick = root / ("stick-" + name);
    fs::create_directories(stick / "Contents");
    std::ofstream(stick / "Contents" / "a.mp3", std::ios::binary) << std::string(4096, 'a') << name;
    return stick;
}

// A real archive, written by the real backup, under whatever file name
// is asked for. The point of the check is that the FILE NAME and the
// stick it belongs to have come apart, so a fake would prove nothing.
fs::path backUp(const fs::path &stick, const fs::path &archivePath, const std::string &identifier,
                const std::string &label)
{
    BackupStickOptions options;
    options.stickRoot = stick;
    options.archivePath = archivePath;
    options.stickIdentifier = identifier;
    options.stickLabel = label;
    const BackupStickOutcome outcome = BackupStick::execute(options);
    assert(outcome.status == BackupOutcomeStatus::Complete);
    return archivePath;
}

void aBackupUnderItsOwnNameIsStillFound(const fs::path &root)
{
    const fs::path stick = makeStick(root, "one");
    const fs::path dir = root / "backups";
    fs::create_directories(dir);
    // Named "Whaleshark live set", so the file is not <label>.zip -- the
    // exact case the page could not find.
    const fs::path named = backUp(stick, dir / "Whaleshark live set.zip", "uuid-one", "WHALESHARK");

    const fs::path found = findStickArchive(dir, "uuid-one", "WHALESHARK");
    std::cout << "  looking for uuid-one, found " << seabass::pathToUtf8(found.filename()) << "\n";
    assert(found == named);
}

void theRightStickWhenTwoShareALabel(const fs::path &root)
{
    const fs::path dir = root / "backups";
    fs::create_directories(dir);
    // Two sticks, the same label, different identifiers. This is the
    // case the label cannot answer and the identifier can: two sticks
    // called SANDISK are ordinary, and picking either would hand the
    // page the wrong stick's backup to update.
    backUp(makeStick(root, "a"), dir / "first.zip", "uuid-a", "SANDISK");
    const fs::path second = backUp(makeStick(root, "b"), dir / "second.zip", "uuid-b", "SANDISK");

    const fs::path found = findStickArchive(dir, "uuid-b", "SANDISK");
    std::cout << "  two sticks labelled SANDISK, uuid-b found " << seabass::pathToUtf8(found.filename()) << "\n";
    assert(found == second);
}

void nothingForAStickThatHasNoBackup(const fs::path &root)
{
    const fs::path dir = root / "backups";
    fs::create_directories(dir);
    backUp(makeStick(root, "c"), dir / "someone-else.zip", "uuid-c", "OTHER");

    // The ordinary case: a stick nobody has backed up. An empty answer,
    // not somebody else's archive.
    const fs::path found = findStickArchive(dir, "uuid-never-seen", "NEVER");
    std::cout << "  a stick with no backup finds nothing: " << (found.empty() ? "yes" : "no") << "\n";
    assert(found.empty());
}

void anEmptyOrMissingFolderIsNotAnError(const fs::path &root)
{
    assert(findStickArchive(root / "not-there", "uuid", "LABEL").empty());
    const fs::path empty = root / "empty";
    fs::create_directories(empty);
    assert(findStickArchive(empty, "uuid", "LABEL").empty());
    std::cout << "  a missing folder and an empty one both answer empty\n";
}

// Before Linux read a label with a space properly, "Stick 1" was
// identified as "<label>-<size>". Its backups carry that identifier and
// must stay this stick's once it is known by its filesystem UUID.
void aBackupUnderTheLegacyIdentifierIsStillFound(const fs::path &root)
{
    const fs::path dir = root / "backups";
    fs::create_directories(dir);
    backUp(makeStick(root, "other"), dir / "another Stick 1.zip", "Stick 1-2000", "Stick 1");
    const fs::path mine = backUp(makeStick(root, "mine"), dir / "Gig.zip", "Stick 1-1000", "Stick 1");

    const fs::path found = findStickArchive(dir, "ABCD-1234", "Stick 1", "Stick 1-1000");
    std::cout << "  recorded as Stick 1-1000, now ABCD-1234: found " << seabass::pathToUtf8(found.filename()) << "\n";
    assert(found == mine);

    // One already recorded under the new identifier wins over the old.
    const fs::path current = backUp(makeStick(root, "mine2"), dir / "current.zip", "ABCD-1234", "Stick 1");
    assert(findStickArchive(dir, "ABCD-1234", "Stick 1", "Stick 1-1000") == current);
}

// The update of that backup: not refused as another stick's, and it
// records the new identifier.
void theLegacyBackupIsUpdatedInPlace(const fs::path &root)
{
    const fs::path stick = makeStick(root, "space");
    const fs::path archive = backUp(stick, root / "Stick 1.zip", "Stick 1-1000", "Stick 1");
    std::ofstream(stick / "Contents" / "b.mp3", std::ios::binary) << std::string(4096, 'b');

    BackupStickOptions options;
    options.stickRoot = stick;
    options.archivePath = archive;
    options.stickIdentifier = "ABCD-1234";
    options.legacyStickIdentifier = "Stick 1-1000";
    options.stickLabel = "Stick 1";
    const BackupPreview preview = BackupStick::preview(options);
    std::cout << "  preview says another stick: " << (preview.identifierMismatch ? "yes" : "no") << "\n";
    assert(!preview.identifierMismatch);
    const BackupStickOutcome outcome = BackupStick::execute(options);
    std::cout << "  update: " << outcome.message << "\n";
    assert(outcome.status == BackupOutcomeStatus::Complete);
    assert(BackupStick::preview(options).previousIdentifier == "ABCD-1234");

    // A stick that merely shares the label is still refused.
    BackupStickOptions other = options;
    other.stickRoot = makeStick(root, "stranger");
    other.stickIdentifier = "EEEE-0000";
    other.legacyStickIdentifier = "Stick 1-2000";
    assert(BackupStick::preview(other).identifierMismatch);
    assert(BackupStick::execute(other).status != BackupOutcomeStatus::Complete);
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "find-stick-archive";
    fs::remove_all(scratch);
    struct Case
    {
        const char *name;
        void (*run)(const fs::path &);
    };
    const Case cases[] = {
        {"a backup under its own name is still found", aBackupUnderItsOwnNameIsStillFound},
        {"the right stick when two share a label", theRightStickWhenTwoShareALabel},
        {"nothing for a stick that has no backup", nothingForAStickThatHasNoBackup},
        {"an empty or missing folder is not an error", anEmptyOrMissingFolderIsNotAnError},
        {"a backup under the legacy identifier is still found", aBackupUnderTheLegacyIdentifierIsStillFound},
        {"the legacy backup is updated in place", theLegacyBackupIsUpdatedInPlace},
    };
    for (const Case &c : cases) {
        const fs::path here = scratch / c.name;
        fs::create_directories(here);
        std::cout << c.name << "\n";
        c.run(here);
    }
    fs::remove_all(scratch);
    std::cout << "find_stick_archive_test: ok\n";
    return 0;
}
