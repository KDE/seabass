// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// StickBackupController's Name field against real archives in scratch.
// A name chooses which archive the next run writes and never moves one:
// a new name starts a new full backup beside the old one, this stick's
// old name goes back to the old backup, and another stick's name is the
// collision it always was. Every archive is hashed before and after, so
// "nothing moved" is checked on the bytes, not on a path string.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QThread>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/manage_stick_backups.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_backup_controller.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::application;
using seabass::gui::pathToQString;
using seabass::gui::StickBackupController;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

QByteArray sha(const fs::path &path)
{
    QFile file(pathToQString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
}

// Until the controller has settled: no preview running, no run going.
// A collision steps through names with a preview each, and only the last
// one emits previewChanged, so the count says the chain has ended.
void settle(StickBackupController &controller, int &previews)
{
    const int before = previews;
    for (int i = 0; i < 3000; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QThread::msleep(10);
        if (previews > before && !controller.previewing() && !controller.busy()) {
            return;
        }
    }
    assert(false && "the controller never settled");
}

bool endsWith(const QString &path, const char *fileName)
{
    return path.endsWith(QLatin1Char('/') + QString::fromUtf8(fileName));
}

}  // namespace

int main(int argc, char **argv)
{
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path scratch = seabass::testing::scratchRoot() / "stick_backup_name_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    seabass::testing::sandboxSeabassHome(scratch / "home");
    seabass::testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    const fs::path backups = scratch / "Backups";
    const fs::path stick = scratch / "MYSTICK";
    writeFile(stick / "Contents" / "a.mp3", std::string(20'000, 'a'));
    writeFile(stick / "Contents" / "b.mp3", std::string(30'000, 'b'));
    writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", std::string(4096, 'p'));

    // A third stick's backup, planted under the name THIRD.
    const fs::path other = scratch / "OTHER";
    writeFile(other / "Contents" / "z.mp3", std::string(10'000, 'z'));
    const fs::path third = backups / "THIRD.zip";
    {
        BackupStickOptions options;
        options.stickRoot = other;
        options.archivePath = third;
        options.stickIdentifier = "uuid-OTHER";
        options.stickLabel = "OTHER";
        options.userName = "THIRD";
        assert(BackupStick::execute(options).status == BackupOutcomeStatus::Complete);
    }
    const QByteArray thirdSha = sha(third);

    StickBackupController controller;
    int previews = 0;
    QObject::connect(&controller, &StickBackupController::previewChanged, [&previews] { ++previews; });

    controller.configure(QStringLiteral("MYSTICK"), pathToQString(stick / "PIONEER"), QString(),
                         pathToQString(backups));
    settle(controller, previews);
    assert(endsWith(controller.archivePath(), "MYSTICK.zip"));
    assert(!controller.lastBackup().value("exists").toBool());

    // ---- the first backup, under the default name ----
    controller.backUp();
    settle(controller, previews);
    const fs::path a = backups / "MYSTICK.zip";
    assert(fs::exists(a));
    const QByteArray aSha = sha(a);
    assert(!aSha.isEmpty());
    assert(controller.lastBackup().value("exists").toBool());
    std::cout << "ok: the first backup is MYSTICK.zip\n";

    // ---- a new name: nothing moves, the next run is a new full backup ----
    controller.setBackupName(QStringLiteral("B"));
    // Straight away as well as after the preview: a move and a move back
    // by the preview that follows would leave the end state looking fine.
    assert(fs::exists(a) && "typing a name must not move the stick's backup");
    settle(controller, previews);
    assert(fs::exists(a));
    assert(sha(a) == aSha);
    assert(endsWith(controller.archivePath(), "B.zip"));
    assert(!fs::exists(backups / "B.zip") && "nothing is written before Back Up Now");
    {
        const QVariantMap last = controller.lastBackup();
        const QVariantMap since = controller.sinceLastBackup();
        assert(!last.value("exists").toBool());
        assert(last.value("keptArchiveName").toString() == QStringLiteral("MYSTICK.zip")
               && "the page names the backup that stays");
        assert(since.value("added").toLongLong() == since.value("entriesOnStick").toLongLong()
               && since.value("added").toLongLong() > 0 && "every file is to copy");
        assert(since.value("unchanged").toLongLong() == 0);
    }
    std::cout << "ok: a new name leaves MYSTICK.zip alone and previews a full backup\n";

    controller.backUp();
    settle(controller, previews);
    const fs::path b = backups / "B.zip";
    assert(fs::exists(b));
    assert(sha(a) == aSha && "the old backup is byte for byte what it was");
    assert(sha(third) == thirdSha);
    {
        int mine = 0;
        for (const ManagedStickBackup &backup : ManageStickBackups::list(backups, {})) {
            if (backup.description.stickLabel == "MYSTICK") {
                ++mine;
                const fs::path file = backup.description.archivePath.filename();
                assert((file == "MYSTICK.zip" && backup.description.userName == "MYSTICK")
                       || (file == "B.zip" && backup.description.userName == "B"));
            }
        }
        assert(mine == 2 && "Manage Backups lists both of this stick's backups");
    }
    const QByteArray bSha = sha(b);
    std::cout << "ok: Back Up Now wrote B.zip beside MYSTICK.zip, both listed\n";

    // ---- a name typed while a run writes: it takes effect when the run ends ----
    // The field is disabled while a run is in flight, but the rule lives in
    // the controller: the run writes the archive it started with (B.zip),
    // and the moment it ends the page targets the typed name, so the next
    // Back Up Now cannot update B.zip under the name C.
    controller.backUp();
    assert(controller.busy() && "the run is in flight");
    controller.setBackupName(QStringLiteral("C"));
    assert(endsWith(controller.archivePath(), "B.zip") && "the running update keeps its archive");
    settle(controller, previews);
    assert(endsWith(controller.archivePath(), "C.zip") && "the typed name took effect when the run ended");
    assert(!fs::exists(backups / "C.zip") && "and nothing was written under it yet");
    assert(fs::exists(b) && "B.zip is the one the run wrote");
    assert(sha(a) == aSha);
    {
        const QVariantMap last = controller.lastBackup();
        assert(!last.value("exists").toBool());
        assert(!last.value("keptArchiveName").toString().isEmpty() && "the page names a backup that stays");
    }
    std::cout << "ok: a name typed during a run targets a new backup once the run ends\n";

    // ---- the old name again: back to the old backup, as an update ----
    controller.setBackupName(QStringLiteral("MYSTICK"));
    settle(controller, previews);
    assert(endsWith(controller.archivePath(), "MYSTICK.zip"));
    {
        const QVariantMap last = controller.lastBackup();
        const QVariantMap since = controller.sinceLastBackup();
        assert(last.value("exists").toBool());
        assert(last.value("keptArchiveName").toString().isEmpty());
        assert(since.value("added").toLongLong() == 0 && since.value("changed").toLongLong() == 0
               && since.value("unchanged").toLongLong() > 0 && "an update, nothing to copy");
    }
    assert(sha(a) == aSha && sha(b) == bSha);
    std::cout << "ok: the old name targets MYSTICK.zip again and previews an update\n";

    // ---- another stick's name: the collision, and nothing moved ----
    controller.setBackupName(QStringLiteral("THIRD"));
    settle(controller, previews);
    assert(controller.nameCollidedWith() == QStringLiteral("OTHER"));
    assert(endsWith(controller.archivePath(), "THIRD (2).zip"));
    assert(!fs::exists(backups / "THIRD (2).zip"));
    assert(sha(third) == thirdSha && sha(a) == aSha && sha(b) == bSha);
    std::cout << "ok: another stick's name collides, every archive untouched\n";

    fs::remove_all(scratch, ec);
    std::cout << "stick_backup_name_test passed\n";
    return 0;
}
