// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A OneLibrary cue write lands in the track's analysis file first (#59),
// and that file is not the exportLibrary.db a change declares for its
// backup. The save's shared OneLibrary writer backs each analysis file up
// just before writing it; this checks that through a whole save:
//
// - a change that writes a OneLibrary-only track's analysis file and then
//   fails on the cue table is rolled back, the analysis file included,
//   byte for byte;
// - the same change, not sabotaged, lands its cue in the analysis file.
//
// Without the backup, the failed change would leave the analysis file
// rewritten while the save reported the change as not applied.

#include <QString>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using seabass::domain::Track;
using namespace seabass::infrastructure;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::string bytesOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path freshStick(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(scratch, ec);
    const fs::path pioneer = scratch / "PIONEER";
    seabass::testing::copyPioneerFixture(
        seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox", pioneer,
        ec);
    if (ec) {
        std::cerr << "could not copy the fixture: " << ec.message() << "\n";
        std::exit(2);
    }
    return pioneer;
}

struct Target
{
    Track track;
    int freeSlot = 0;
};

// A row only OneLibrary holds, whose analysis file is there and has a free
// hot cue slot: its analysis file is written by the OneLibrary writer
// alone, so nothing else in the save backs it up.
Target findTarget(const std::string &root)
{
    std::set<std::string> deviceLibraryFiles;
    for (const auto &t : rekordbox::KaitaiRekordboxReader(root).readTracks()) {
        deviceLibraryFiles.insert(t.analysisFile);
    }
    for (const auto &t : onelibrary::OneLibraryReader(root).readAll()) {
        if (t.analysisFile.empty() || t.filePath.empty() || deviceLibraryFiles.count(t.analysisFile)
            || !fs::exists(seabass::pathFromUtf8(rekordbox::extAnlzPath(root, t.analysisFile)))) {
            continue;
        }
        std::set<int> used;
        for (const auto &cue : t.cues) {
            if (cue.kind == CuePoint::Kind::Hot) {
                used.insert(cue.hotCueNumber);
            }
        }
        for (int slot = 1; slot <= 8; ++slot) {
            if (!used.count(slot)) {
                return {t, slot};
            }
        }
    }
    return {};
}

seabass::gui::SaveLoopResult addCue(const fs::path &pioneer, const Target &target)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    seabass::application::CancellationToken token;
    const QString root = seabass::gui::pathToQString(pioneer);
    seabass::gui::SaveContext ctx(token, noProgress, {}, root, {});
    std::vector<std::shared_ptr<seabass::gui::PendingChange>> changes = {
        std::make_shared<seabass::gui::AddCueChange>(QStringLiteral("onelibrary"), root,
                                                    QString::fromStdString(target.track.sourceId), 4321.0,
                                                    QStringLiteral("hot"), target.freeSlot, QString(), QString(),
                                                    false, 0.0, QString::fromStdString(target.track.title))};
    return seabass::gui::runSaveLoop(changes, ctx);
}

}  // namespace

int main()
{
    seabass::testing::sandboxSeabassHome(seabass::testing::scratchRoot() / "seabass_onelibrary_cue_file_backup_home");
    // 1. The cue table fails after the analysis file was written: the save
    //    puts the analysis file back.
    {
        const fs::path pioneer = freshStick("seabass_onelibrary_cue_file_backup_fail");
        const std::string root = seabass::pathToUtf8(pioneer);
        const Target target = findTarget(root);
        if (target.freeSlot == 0) {
            std::cerr << "the fixture has no OneLibrary-only row with an analysis file and a free slot\n";
            return 2;
        }
        const fs::path ext = seabass::pathFromUtf8(rekordbox::extAnlzPath(root, target.track.analysisFile));
        const fs::path dat = seabass::pathFromUtf8(rekordbox::datAnlzPath(root, target.track.analysisFile));
        const std::string extBefore = bytesOf(ext);
        const std::string datBefore = bytesOf(dat);

        // Right after the .EXT is written and read back, take the cue table
        // away, so the table half of the same write fails.
        bool sabotaged = false;
        rekordbox::RekordboxCueWriter::setAfterWriteForTesting([&](const std::string &path) {
            if (sabotaged || seabass::pathFromUtf8(path) != ext) {
                return;
            }
            sabotaged = true;
            onelibrary::SqlCipherLibrary lib;
            onelibrary::SqlCipherDb db(lib, onelibrary::OneLibraryCueWriter::dbPathFor(root), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + onelibrary::deriveOneLibraryKey() + "';");
            db.exec("DROP TABLE cue;");
        });
        const auto result = addCue(pioneer, target);
        rekordbox::RekordboxCueWriter::setAfterWriteForTesting({});

        check(sabotaged, "the analysis file was written before the cue table");
        check(!result.error.isEmpty(), "the change failed on the cue table");
        check(bytesOf(ext) == extBefore, "the failed change put the .EXT back as it was");
        check(bytesOf(dat) == datBefore, "the failed change left the .DAT as it was");
        seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(pioneer.parent_path()));
        std::cout << "a failed OneLibrary cue write puts its analysis file back\n";
    }

    // 2. The same change, not sabotaged, lands in the analysis file.
    {
        const fs::path pioneer = freshStick("seabass_onelibrary_cue_file_backup_ok");
        const std::string root = seabass::pathToUtf8(pioneer);
        const Target target = findTarget(root);
        const auto result = addCue(pioneer, target);
        check(result.error.isEmpty(), "the save succeeds: " + result.error.toStdString());
        rekordbox::FilesystemAnlzSource source(root);
        const auto cues = rekordbox::readAnalysisFileCues(source, target.track.analysisFile);
        bool found = false;
        for (const auto &cue : cues ? *cues : std::vector<CuePoint>{}) {
            found = found || (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == target.freeSlot
                              && cue.positionMs == 4321.0);
        }
        check(found, "the added cue is in the analysis file a OneLibrary player reads");
        seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(pioneer.parent_path()));
        std::cout << "a OneLibrary cue write lands in the analysis file\n";
    }

    std::error_code ec;
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_onelibrary_cue_file_backup_fail", ec);
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_onelibrary_cue_file_backup_ok", ec);
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "onelibrary_cue_file_backup_test OK\n";
    return 0;
}
