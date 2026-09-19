// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The backup history as a person reads it.
//
// The rows themselves are covered by backup_stick_test case 13; this is
// the other half, and the half that only the GUI can reach, which is
// exactly why it needs a test of its own. Nothing else would notice if
// the file came out empty, in the wrong order, or claiming an archive
// was damaged when it simply predates the log.
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTextStream>

#include "gui/backup_changelog_text.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::stick_backup;
using seabass::gui::writeChangelogFile;

namespace
{

// A real archive holding nothing but a manifest: enough for the renderer,
// which never looks at the entries.
fs::path writeArchive(const fs::path &path, const BackupManifest &manifest)
{
    fs::create_directories(path.parent_path());
    PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadWrite);
    Zip64Writer writer(file, {});
    writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
    return path;
}

BackupManifest manifestWith(std::vector<GenerationRow> generations)
{
    BackupManifest manifest;
    manifest.stickIdentifier = "uuid";
    manifest.stickLabel = "WHALESHARK";
    manifest.createdAtUnix = 1'700'000'000;
    manifest.generations = std::move(generations);
    return manifest;
}

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream in(&file);
    return in.readAll();
}

GenerationRow generation(std::int64_t at, std::size_t added, const std::string &name = {})
{
    GenerationRow row;
    row.createdAtUnix = at;
    row.added = added;
    row.bytesRead = added * 1000;
    row.userName = name;
    return row;
}

void testNewestFirstWithTheNamesInForceAtTheTime()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_changelog_text_test";
    fs::remove_all(root);
    const fs::path archive = writeArchive(root / "WHALESHARK.zip",
                                           manifestWith({generation(1'700'000'000, 7, "before the Berlin gig"),
                                                          generation(1'700'100'000, 3, "after the Berlin gig")}));

    QString error;
    const QString path = writeChangelogFile(archive, &error);
    assert(!path.isEmpty() && error.isEmpty());

    const QString text = readAll(path);
    assert(!text.isEmpty());
    assert(text.contains(QStringLiteral("WHALESHARK")));
    assert(text.contains(QStringLiteral("2 runs recorded")));

    // Newest first: the later run has to appear before the earlier one,
    // because the last thing that happened is what anyone opening this
    // wants to see.
    const int later = text.indexOf(QStringLiteral("after the Berlin gig"));
    const int earlier = text.indexOf(QStringLiteral("before the Berlin gig"));
    assert(later >= 0 && earlier >= 0);
    assert(later < earlier);

    // Each row keeps the name it had at the time, so renaming a backup
    // does not rewrite its history.
    assert(text.contains(QStringLiteral("called \"before the Berlin gig\" at the time")));
    fs::remove_all(root);
}

void testAnArchiveWithNoHistoryIsNotReportedAsDamaged()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_changelog_text_empty_test";
    fs::remove_all(root);
    const fs::path archive = writeArchive(root / "OLD.zip", manifestWith({}));

    QString error;
    const QString path = writeChangelogFile(archive, &error);
    assert(path.isEmpty());
    // Every backup written before the log existed lands here. "No history
    // yet" must not read as "this backup is broken", which is the whole
    // point of separating the two messages.
    assert(error.contains(QStringLiteral("no history recorded yet")));
    assert(!error.contains(QStringLiteral("damaged")));
    fs::remove_all(root);
}

void testAnArchiveThatWillNotOpenSaysSo()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_changelog_text_broken_test";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream out(root / "BROKEN.zip", std::ios::binary);
        out << "this is not a zip file at all";
    }

    QString error;
    const QString path = writeChangelogFile(root / "BROKEN.zip", &error);
    assert(path.isEmpty());
    assert(!error.isEmpty());
    assert(error.contains(QStringLiteral("could not be read")));
    fs::remove_all(root);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testNewestFirstWithTheNamesInForceAtTheTime();
    testAnArchiveWithNoHistoryIsNotReportedAsDamaged();
    testAnArchiveThatWillNotOpenSaysSo();
    std::cout << "backup_changelog_text_test passed\n";
    return 0;
}
