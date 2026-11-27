// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The rules around rekordbox's kept original analysis (Analyse again keeps
// it, Restore original analysis puts it back), on a copy of the anonymised
// fixture stick with the real edit session, synthetic WAVs and no real
// stick. Each track is imported by Seabass, so its analysis is Seabass's;
// "an export" then stands for rekordbox exporting over the stick: the
// track's analysis files get a grid of rekordbox's (Seabass's moved by a
// few milliseconds) and waveforms of their own, and both rekordbox
// libraries rekordbox's tempo, while the stick's Seabass folder (the
// ledgers, the kept analyses) stays as it was.
//
//  1. Sequences of export, Analyse again (two tempo ranges) and Restore in
//     many orders, the ledger lost or damaged between, table driven: after
//     each step the grid on the stick, the kept original and its record
//     are what the rule says, a kept original is never replaced, and no
//     step of Seabass's loses a grid that is not Seabass's.
//  2. A OneLibrary renumbered by a new export (content ids swapped with
//     another track's): its rows are left alone and the restore says so;
//     the other track keeps its tempo.
//  3. Keys by name: export.pdb's kept key id names another key, and
//     exportLibrary.db's key table is renumbered as a new export does it;
//     each library gets the kept key by its name (the renumbered id). A row that was not there when
//     the original was kept gets the kept tempo and key too, and the log
//     counts it.
//  4. A kept key that cannot be written: the track is refused with a plain
//     reason before anything is written, and the save does not fail.
//  5. Library Health's relink carries the kept original, the analysis
//     ledger's entry and the beat grid ledger's line to the new name;
//     Free Up Space sees the track on the stick; Undo takes it all back;
//     relinked again, Analyse again keeps the original and Restore puts
//     it back at the new name.
//  6. A Seabass grid known only by another track's ledger entry, or only
//     by another kept entry's head, is not kept as an original.
//  7. The audio file's size: changed with a grid Seabass knows, the kept
//     entry stays byte for byte and Restore refuses the file of another
//     size; changed since the analysis, or not to be read, the track is
//     left alone with a reason.
//  8. A cue point set in the .EXT after the keep survives the restore.
//  9. No room on the stick for what is to be kept: the track is left alone
//     before anything is written, and the save's summary says so; free
//     space that cannot be read is not "no room".
// 10. Restore staged, then Analyse again of the same track: left out, with
//     the reason in the result.
// 11. Two changes of one save that record the same track in a ledger: the
//     one applied later wins, whatever their ids.
// 12. Free Up Space: every group unticked; a held lock is reported for the
//     library the removal asked it of, and nothing is removed.
// 13. Restore moves back only the cue points Analyse again moved, to where
//     they were, and only those still where it put them: declined at
//     Analyse again, nothing moves (the cue lists byte for byte); moved,
//     both go back; moved and one set by hand since, that one stays; one
//     nudged by 20 ms stays; of two memory cues 30 ms apart the one moved
//     goes back; one whose old place holds another cue stays, and the
//     review says so. Each time OneLibrary's cues are DeviceLibrary's.
//     Moved in Engine too, whose row had nothing kept: rekordbox's grid
//     and cues go back, Engine keeps Seabass's grid and its cues on it,
//     and the summary says so.
// 14. The player's analysis kept alone (Beat Grids & Waveforms): not
//     counted, not restored (the restore is refused), not listed.
//
// 15. A relink with a kept original under both names lands; the new
//     name's stays, the old one stays where it is; the ledger line goes
//     with the file; an original with no record stays where it is.
// 16. A ledger moved aside: what is kept has its origin unknown, and the
//     record, Free Up Space and Restore say so.
// 17. A damaged ledger moved aside and no new one written: true words, and
//     the stick's ledger is incomplete from then; a track Seabass cannot
//     tell about is named in the summary, which says where the command to
//     move the record aside is, as an unreadable ledger's warning does.
// 18. A kept original with no record: left alone and refused.
// 19. The player's analysis Copy Beat Grids kept is not Restore's: Engine,
//     on Seabass's grid, keeps it and its cues; rekordbox's go back.
// 20. The playlist menu's count right at the first opening after a save;
//     with something kept a warm reads the catalogs once. (Before any
//     case: with nothing kept, no library is read, no kept file parsed.)
// 21. A relink carries only the kinds whose library it points.
// 22. A relink carries the ledgers' lines as the same save left them.
//
// 23. Analyse again decides on every analysis file it writes: a grid
//     changed outside Seabass in OneLibrary's own analysis file leaves the
//     track alone, and so do rows that name other files than the ones the
//     original was kept from; Restore leaves both out, each with its
//     reason.
// 24. A relink whose record cannot be written under the new name takes
//     the half-carried entry away again; a relink with a damaged analysis
//     ledger and an unreadable beat grid ledger leaves both as they are.
// 25. The summary's words on the cue points are what the save did, one
//     note per outcome with the number of tracks; a track whose cue
//     points were to go back and cannot is refused whole, and the review
//     shows it when it can know.
//
// KEPT_RULES_CASE=<n>[,<n>...] runs those cases alone, after the import of
// their playlists only. ctest runs the cases in several entries
// (CMakeLists.txt).

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "application/path_key.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/analysis_ledger_record.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
#include "gui/edit/changes/moved_file_changes.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"
#include "gui/edit/changes/restore_analysis_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/analysis_settings.hpp"
#include "gui/edit/analysis_restore_prepare.hpp"
#include "gui/original_analysis_controller.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/track_import_controller.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
namespace local = infrastructure::local;
using infrastructure::rekordbox::AnlzFile;

namespace
{

fs::path &stickLog()
{
    static fs::path log;
    return log;
}

// A failure shows the stick's last lines of Analyse again and restore: the
// scratch tree goes with the process.
void require(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << std::endl;
        std::ifstream in(stickLog());
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            if (line.find("track-reanalysis") != std::string::npos || line.find("moved-files") != std::string::npos) {
                lines.push_back(line);
            }
        }
        for (std::size_t i = lines.size() > 10 ? lines.size() - 10 : 0; i < lines.size(); ++i) {
            std::cerr << "  log: " << lines[i].substr(0, 900) << std::endl;
        }
        std::exit(1);
    }
}

void writeBeatWav(const fs::path &file, int seconds, double bpm)
{
    fs::create_directories(file.parent_path());
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * 44100;
    std::vector<std::int16_t> samples(frames, 0);
    const double beat = 60.0 / bpm * 44100.0;
    for (double at = 0.25 * 44100.0; at < frames; at += beat) {
        for (std::uint32_t i = 0; i < 6615 && static_cast<std::uint32_t>(at) + i < frames; ++i) {
            const double t = i / 44100.0;
            samples[static_cast<std::uint32_t>(at) + i] =
                static_cast<std::int16_t>(24000.0 * std::exp(-t * 30.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * t));
        }
    }
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    out << "RIFF";
    u32(36 + frames * 2);
    out << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out << "data";
    u32(frames * 2);
    out.write(reinterpret_cast<const char *>(samples.data()), frames * 2);
}

template <typename Object, typename Signal>
bool waitFor(Object *object, Signal signal, const std::function<bool()> &done)
{
    QSignalSpy spy(object, signal);
    for (int i = 0; i < 24 && !done(); ++i) {
        spy.wait(5000);
    }
    return done();
}

std::string bytesOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path &file, const std::string &bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::uint32_t fourcc(const char (&text)[5])
{
    return (std::uint32_t(std::uint8_t(text[0])) << 24) | (std::uint32_t(std::uint8_t(text[1])) << 16)
           | (std::uint32_t(std::uint8_t(text[2])) << 8) | std::uint32_t(std::uint8_t(text[3]));
}

std::uint32_t readBE(const std::string &bytes, std::size_t at)
{
    return (std::uint32_t(std::uint8_t(bytes[at])) << 24) | (std::uint32_t(std::uint8_t(bytes[at + 1])) << 16)
           | (std::uint32_t(std::uint8_t(bytes[at + 2])) << 8) | std::uint32_t(std::uint8_t(bytes[at + 3]));
}

void writeBE(std::string &bytes, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xff);
    }
}

void shiftGrid(AnlzFile &dat, int shiftMs)
{
    for (auto &section : dat.sections) {
        if (section.fourcc == fourcc("PQTZ")) {
            const std::uint32_t lenHeader = readBE(section.rawBytes, 4);
            const std::uint32_t count = readBE(section.rawBytes, 20);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t at = lenHeader + i * 8 + 4;
                writeBE(section.rawBytes, at, readBE(section.rawBytes, at) + static_cast<std::uint32_t>(shiftMs));
            }
        }
    }
}

void touchWaveforms(AnlzFile &file)
{
    for (auto &section : file.sections) {
        if (infrastructure::rekordbox::isReplacedAnalysisSection(section.fourcc) && section.fourcc != fourcc("PQTZ")
            && section.fourcc != fourcc("PQT2") && section.rawBytes.size() > 40) {
            section.rawBytes[section.rawBytes.size() - 1] ^= 0x5a;
        }
    }
}

std::vector<std::string> cueListsOf(const AnlzFile &file)
{
    std::vector<std::string> lists;
    for (const auto &section : file.sections) {
        if (infrastructure::rekordbox::isCueListSection(section.fourcc)) {
            lists.push_back(section.rawBytes);
        }
    }
    return lists;
}

// SQL on exportLibrary.db, keyed as the writer keys it.
struct OneLibrarySql
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db;
    explicit OneLibrarySql(const fs::path &pioneer)
        : db(lib, infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer)), false)
    {
        db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    }
    std::vector<std::string> tablesWith(const std::string &column)
    {
        std::vector<std::string> tables;
        infrastructure::onelibrary::SqlCipherStatement all(db, "SELECT name FROM sqlite_master WHERE type = 'table'");
        std::vector<std::string> names;
        while (all.step()) {
            names.push_back(all.columnText(0));
        }
        for (const std::string &table : names) {
            infrastructure::onelibrary::SqlCipherStatement info(db, "PRAGMA table_info(\"" + table + "\")");
            while (info.step()) {
                if (info.columnText(1) == column) {
                    tables.push_back(table);
                }
            }
        }
        return tables;
    }
};

// A save change that only notes ledger lines after the commit, for the
// order rule (case 11).
class LedgerNoteChange : public PendingChange
{
public:
    LedgerNoteChange(QString id, std::string stickRoot, std::function<void(SaveContext &, const QString &)> note)
        : m_id(std::move(id)), m_stickRoot(std::move(stickRoot)), m_note(std::move(note))
    {
    }
    QString id() const override { return m_id; }
    QString owner() const override { return QStringLiteral("ledger-order"); }
    QString description() const override { return m_id; }
    QString unit() const override { return QStringLiteral("notes"); }
    QStringList formatsTouched() const override { return {}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &) const override
    {
        std::vector<BackupTarget> targets{{beatGridLedgerFileOf(m_stickRoot), "ledger-order", true}};
        if (const auto ledger = analysisLedgerBackupTarget(m_stickRoot, "ledger-order")) {
            targets.push_back(*ledger);
        }
        return targets;
    }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        m_note(ctx, m_id);
        return ChangeOutcome::success();
    }

private:
    QString m_id;
    std::string m_stickRoot;
    std::function<void(SaveContext &, const QString &)> m_note;
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: kept_analysis_rules_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "kept_analysis_rules_home");
    testing::sandboxSettings(testing::scratchRoot() / "kept_analysis_rules_config");
    QCoreApplication app(argc, argv);
    const auto runs = [](int n) {
        const char *only = std::getenv("KEPT_RULES_CASE");
        if (only == nullptr) {
            return true;
        }
        std::stringstream list(only);
        for (std::string one; std::getline(list, one, ',');) {
            if (std::atoi(one.c_str()) == n) {
                return true;
            }
        }
        return false;
    };
    const fs::path computer = testing::scratchRoot() / "kept_analysis_rules_computer";
    fs::remove_all(computer);
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "kept_analysis_rules_stick");
    testing::levelImportCounter(stick);
    stickLog() = stick.root / "Seabass" / "seabass.log";
    const QString pioneer = stick.pioneerPath();
    const QString engine = stick.enginePath();
    const std::string stickRoot = pathToUtf8(stick.root);
    const fs::path onStick = stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum";
    const std::string pdbFile = pathToUtf8(stick.pioneer / "rekordbox" / "export.pdb");

    TrackImportController importer;
    importer.setExperimentalEnabled(true);
    OriginalAnalysisController restorer;
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(pioneer));
    require(session != nullptr, "the stick has an edit session");
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    const auto saveNow = [&]() {
        finished.clear();
        session->save();
        require(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }), "save ends");
        return session->lastSummary();
    };
    const auto savedClean = [&](const std::string &what) {
        const QVariantMap summary = saveNow();
        require(summary.value("error").toString().isEmpty(),
                what + ": the save fails: " + summary.value("error").toString().toStdString());
        return summary;
    };
    const auto undoNow = [&]() {
        finished.clear();
        session->undoLastSave();
        require(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }), "undo ends");
    };
    const auto invalidate = [&]() { LibraryCatalogCache::instance().invalidateEveryCatalogOn(stickRoot); };
    const auto logText = [&]() { return bytesOf(stickLog()); };
    const auto keyOf = [](const std::string &name) {
        return application::normalizedPathKey("Contents/UnknownArtist/UnknownAlbum/" + name);
    };
    const auto rowOf = [](auto &&reader, const std::string &name) {
        for (const auto &track : reader.readAll()) {
            if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                return track;
            }
        }
        require(false, name + " is in the library");
        return domain::Track{};
    };
    const auto pdbRow = [&](const std::string &name) {
        return rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), name);
    };
    const auto pdbId = [&](const std::string &name) { return static_cast<std::uint32_t>(std::stoul(pdbRow(name).sourceId)); };
    const auto anlzOf = [&](const std::string &name, const char *extension) {
        const std::string dat = pdbRow(name).analysisFile;
        return stick.root / pathFromUtf8(dat.substr(1, dat.size() - 4) + extension);
    };
    const auto gridOf = [&](const std::string &name) {
        return infrastructure::rekordbox::pqtzGridFingerprint(AnlzFile::readRaw(pathToUtf8(anlzOf(name, "DAT"))));
    };
    const auto keptFile = [&](const std::string &key) { return local::keptRekordboxAnalysisFile(stick.root, key); };
    const auto keptOf = [&](const std::string &key) { return local::StickKeptRekordboxAnalysisStore(stick.root).find(key); };
    const auto recordOf = [&](const std::string &key) {
        return local::StickKeptRekordboxAnalysisStore(stick.root).findRecord(key);
    };
    const auto recordFile = [&](const std::string &key) { return local::keptRekordboxRecordFile(stick.root, key); };
    // An entry written again by hand, its record as it is.
    const auto rekeep = [&](const local::KeptRekordboxAnalysis &entry) {
        local::StickKeptRekordboxAnalysisStore store(stick.root);
        const auto record = store.findRecord(entry.pathKey);
        require(record.status == local::KeptRekordboxRecordLookup::Status::Found, "the record of a kept entry reads");
        store.keep(entry, record.record);
    };
    // A record of nothing written, for an entry planted by hand.
    const auto plainRecord = [](const std::string &key) {
        local::KeptRekordboxRecord record;
        record.pathKey = key;
        return record;
    };
    const auto ledger = [&]() {
        std::string error;
        const auto read = local::readAnalysisLedger(stick.root, &error);
        require(error.empty(), "the analysis ledger reads: " + error);
        return read;
    };
    const auto writeLedger = [&](const local::AnalysisLedger &next) {
        std::string error;
        require(local::writeAnalysisLedger(stick.root, next, [](const std::string &) {}, &error), error);
    };
    // The ledger as a case found it, put back with what was recorded since
    // (a case that loses or damages the ledger leaves the others theirs),
    // and the ledgers moved aside taken away.
    const auto ledgerAsBefore = [&](const local::AnalysisLedger &before) {
        std::string error;
        local::AnalysisLedger merged = before;
        for (const auto &[key, entry] : local::readAnalysisLedger(stick.root, &error)) {
            merged[key] = entry;
        }
        for (const auto &entry : fs::directory_iterator(stick.root / "Seabass")) {
            if (pathToUtf8(entry.path().filename()).rfind("analysis.tsv.gz.", 0) == 0) {
                fs::remove_all(entry.path());
            }
        }
        writeLedger(merged);
    };
    const auto gridLedger = [&]() {
        std::string error;
        const auto read = local::readBeatGridLedger(stick.root, &error);
        require(error.empty(), "the beat grid ledger reads: " + error);
        return read;
    };
    const auto oneLibraryRows = [&](const std::string &name) {
        return infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(stick.pioneer))
            .tempoAndKeyIdsForPath(pathToUtf8(onStick / name));
    };
    const auto analyseAgain = [&](const QString &playlist) {
        require(importer.analyseAgain(pioneer, engine, playlist), "Analyse again starts");
        require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }),
                "Analyse again ends");
        require(importer.errorMessage().isEmpty(), "Analyse again: " + importer.errorMessage().toStdString());
        const QVariantMap result = importer.lastReanalysis();
        if (result.value("analysed").toInt() == 0) {
            for (const QVariant &left : result.value("leftAloneTracks").toList()) {
                std::cerr << "  Analyse again left " << left.toMap().value("name").toString().toStdString() << ": "
                          << left.toMap().value("why").toString().toStdString() << std::endl;
            }
        }
        return result;
    };
    // Analyse again that has to stage the playlist's track.
    const auto analyseAgainStaged = [&](const QString &playlist, const std::string &what) {
        const QVariantMap result = analyseAgain(playlist);
        require(result.value("analysed").toInt() > 0, what + ": Analyse again stages the track");
        return result;
    };
    const auto restore = [&](const QString &playlist) {
        require(restorer.restoreOriginalAnalysis(pioneer, engine, playlist),
                "the restore starts: " + restorer.errorMessage().toStdString());
        require(waitFor(&restorer, &OriginalAnalysisController::busyChanged, [&] { return !restorer.busy(); }),
                "the restore is prepared");
        require(restorer.errorMessage().isEmpty(), "restore: " + restorer.errorMessage().toStdString());
        return restorer.lastRestore();
    };

    // The import: one playlist per case, Seabass's analysis.
    const std::vector<std::pair<std::string, std::vector<std::string>>> playlists{
        {"Seq1", {"s one.wav"}},  {"Seq2", {"s two.wav"}},  {"Seq3", {"s three.wav"}}, {"Seq4", {"s four.wav"}},
        {"Rows", {"r one.wav", "r two.wav"}},                {"Keys", {"r three.wav"}}, {"Refuse", {"r four.wav"}},
        {"Move", {"m one.wav"}},  {"Elsewhere", {"m two.wav"}}, {"Size", {"z one.wav"}}, {"Cue", {"c one.wav"}},
        {"Room", {"q one.wav"}},  {"Staged", {"g one.wav"}}, {"CueNo", {"u one.wav"}}, {"CueYes", {"u two.wav"}},
        {"CueEdit", {"u three.wav"}}, {"EngineAlone", {"e one.wav"}}, {"Seq5", {"s five.wav"}},
        {"Seq6", {"s six.wav"}},  {"Seq7", {"s seven.wav"}}, {"Seq8", {"s eight.wav"}}, {"Lost", {"l one.wav"}},
        {"Broken", {"b one.wav", "b two.wav"}}, {"NoRecord", {"n one.wav"}}, {"CueNudge", {"u four.wav"}},
        {"CueMem", {"u five.wav"}}, {"CueHit", {"u six.wav"}}, {"EngineCopied", {"e two.wav"}},
        {"Relink", {"k one.wav"}}, {"RelinkRb", {"k two.wav"}}, {"Menu", {"w one.wav"}},
        {"RelinkSave", {"k three.wav"}}, {"TwoFiles", {"t one.wav"}}, {"RelinkHalf", {"k four.wav"}},
        {"CueMoved", {"u seven.wav", "u eight.wav", "u nine.wav"}}, {"CueSplit", {"v one.wav"}}};
    // The playlists each case works on: only those of the cases run are
    // imported (the import is most of a case's time).
    const std::map<std::string, std::vector<int>> casesOf{
        {"Seq1", {1, 12}}, {"Seq2", {1}}, {"Seq3", {1}}, {"Seq4", {1}}, {"Seq5", {1}}, {"Seq6", {1}}, {"Seq7", {1}},
        {"Seq8", {1}}, {"Rows", {2}}, {"Keys", {3}}, {"Refuse", {4}}, {"Move", {5, 11}}, {"Elsewhere", {6}},
        {"Size", {7}}, {"Cue", {8}}, {"Room", {9}}, {"Staged", {10}}, {"CueNo", {13}}, {"CueYes", {13}},
        {"CueEdit", {13}}, {"CueNudge", {13}}, {"CueMem", {13}}, {"CueHit", {13}}, {"EngineAlone", {14}},
        {"Relink", {15}}, {"Lost", {16}}, {"Broken", {17}}, {"NoRecord", {18}}, {"EngineCopied", {19}},
        {"Menu", {20}}, {"RelinkRb", {21}}, {"RelinkSave", {22}}, {"TwoFiles", {23}}, {"RelinkHalf", {24}}, {"CueMoved", {25}}, {"CueSplit", {25}}};
    const auto wanted = [&](const std::string &playlist) {
        const auto of = casesOf.find(playlist);
        require(of != casesOf.end(), playlist + " belongs to a case");
        return std::any_of(of->second.begin(), of->second.end(), runs);
    };
    int seconds = 16;
    for (const auto &[playlist, names] : playlists) {
        if (!wanted(playlist)) {
            seconds += static_cast<int>(names.size());
            continue;
        }
        QVariantList files;
        for (const std::string &name : names) {
            writeBeatWav(computer / name, seconds++, 120.0);
            files << pathToQString(computer / name);
        }
        require(importer.importFiles(pioneer, engine, files, QString::fromStdString(playlist), true), "the import starts");
        require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }), "import ends");
        savedClean("the import of " + playlist);
    }
    // Seabass's analysis files of each track as the import left them, and
    // its grid: what an export is made from, and what Analyse again writes.
    std::map<std::string, std::map<std::string, std::string>> imported;
    std::map<std::string, std::string> seabassGrid;
    for (const auto &[playlist, names] : playlists) {
        if (!wanted(playlist)) {
            continue;
        }
        for (const std::string &name : names) {
            for (const char *extension : {"DAT", "EXT"}) {
                imported[name][extension] = bytesOf(anlzOf(name, extension));
            }
            seabassGrid[name] = gridOf(name);
            require(!seabassGrid[name].empty() && ledger().count(keyOf(name)) == 1, name + " is Seabass's");
        }
    }

    // Nothing kept yet: the playlist menu's warm and count, and Free Up
    // Space's listing, read no library and parse no kept file. A player's
    // analysis Copy Beat Grids kept alone is not opened either.
    {
        require(!fs::exists(infrastructure::paths::stickKeptRekordboxAnalysisDir(stick.root))
                    || fs::is_empty(infrastructure::paths::stickKeptRekordboxAnalysisDir(stick.root)),
                "nothing of rekordbox's is kept after the import");
        application::KeptEngineAnalysis alone;
        alone.pathKey = keyOf("e one.wav");
        alone.trackFile = "../Contents/UnknownArtist/UnknownAlbum/e one.wav";
        alone.libraryUuid = "copied alone";
        local::StickKeptEngineAnalysisStore(stick.root).keep(alone);
        AnalysisRestoreRequest warm;
        warm.rekordboxPath = pioneer;
        warm.enginePath = engine;
        warm.stickId = registry->libraryIdForPath(pioneer).toStdString();
        warm.playlist = "Seq1";
        const int readsBefore = keptCountCatalogReadsForTesting();
        const int parsedBefore = local::keptEngineEntriesParsedForTesting();
        warmKeptCounts(warm, application::CancellationToken());
        const int counted = countKeptOriginals(warm, application::CancellationToken());
        const auto listed = KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8());
        require(counted == 0 && restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Seq1")) == 0
                    && listed.groups.empty(),
                "nothing kept: every count is 0 and Free Up Space lists nothing");
        require(keptCountCatalogReadsForTesting() == readsBefore,
                "nothing kept: the warm and the count read no library");
        require(local::keptEngineEntriesParsedForTesting() == parsedBefore,
                "nothing kept: Free Up Space parses no kept file of the player's");
        fs::remove(local::keptEngineAnalysisFile(stick.root, alone.pathKey));
        forgetKeptAnalysisNames();
        std::cout << "nothing kept: no library read, no kept file parsed OK\n";
    }

    // rekordbox exporting over the stick: the track's analysis files with
    // Seabass's grid `shiftMs` later and waveforms of rekordbox's, and
    // rekordbox's tempo (with `key` where a row names none) in both
    // libraries. Seabass/ is left as it is. The grid's fingerprint.
    const auto exportOver = [&](const std::string &name, int shiftMs, const std::string &key = std::string()) {
        AnlzFile dat = AnlzFile::fromBytes(imported[name]["DAT"], "the imported .DAT");
        shiftGrid(dat, shiftMs);
        touchWaveforms(dat);
        writeBytes(anlzOf(name, "DAT"), dat.toBytes());
        AnlzFile ext = AnlzFile::fromBytes(imported[name]["EXT"], "the imported .EXT");
        touchWaveforms(ext);
        writeBytes(anlzOf(name, "EXT"), ext.toBytes());
        infrastructure::rekordbox::PdbRowWriter rows(pdbFile);
        rows.setTrackTempoAndKey(pdbId(name), 119.5, key);
        require(rows.commit(), "export.pdb takes rekordbox's tempo");
        infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(stick.pioneer))
            .setTempoAndKeyForPath(pathToUtf8(onStick / name), 119.5, key);
        invalidate();
        return gridOf(name);
    };

    // 1. Sequences. "E<n>": an export with grid n (each its own shift);
    //    "A": Analyse again, saved; "T": Analyse again with the tempo range
    //    58 to 115, which gives Seabass's grid at half the tempo (a grid
    //    of Seabass's that is not its first); "R": Restore original
    //    analysis, saved; "L": the analysis ledger moved aside (the
    //    command line's --reset); "D": the analysis ledger damaged.
    //    The rule: a kept original is never replaced. Analyse again keeps
    //    the grid on the stick when nothing is kept and it is not
    //    Seabass's; with an original kept it writes only over the kept
    //    grid or one of Seabass's as the kept entry's record lists them,
    //    whatever became of the ledger, and otherwise leaves the track
    //    alone. A restore puts the kept one back over the same grids and
    //    refuses any other. No step of Seabass's ever loses a grid that is
    //    not Seabass's.
    if (runs(1)) {
        const local::AnalysisLedger ledgerBefore = ledger();
        const std::vector<std::pair<std::string, std::vector<std::string>>> sequences{
            {"s one.wav", {"E1", "A", "A", "R"}},
            {"s two.wav", {"E1", "A", "E2", "A", "R"}},
            {"s three.wav", {"E1", "A", "R", "A", "R"}},
            {"s four.wav", {"E1", "A", "E2", "R", "A", "A", "R"}},
            {"s five.wav", {"A", "E1", "A", "T", "L", "A", "R"}},
            {"s six.wav", {"E1", "A", "T", "D", "A", "R"}},
            {"s seven.wav", {"E1", "A", "T", "E2", "A", "T", "R"}},
            {"s eight.wav", {"E1", "R", "A", "T", "L", "T", "E2", "R", "A"}},
        };
        const std::map<std::string, QString> playlistOf{
            {"s one.wav", "Seq1"}, {"s two.wav", "Seq2"},   {"s three.wav", "Seq3"}, {"s four.wav", "Seq4"},
            {"s five.wav", "Seq5"}, {"s six.wav", "Seq6"}, {"s seven.wav", "Seq7"}, {"s eight.wav", "Seq8"}};
        const auto withRange = [&](const QString &range) {
            QSettings settings = openSeabassSettings();
            settings.setValue(QLatin1String(AnalysisTempoRangeKey), range);
            settings.sync();
        };
        for (const auto &[name, steps] : sequences) {
            const QString playlist = playlistOf.at(name);
            std::string onTheStick = seabassGrid[name];
            std::string kept;
            // Seabass's grids since the original was kept, as the rule
            // knows them: the record's.
            std::set<std::string> seabass;
            std::set<std::string> everSeabass{seabassGrid[name]};
            std::string done;
            for (const std::string &step : steps) {
                done += (done.empty() ? "" : " ") + step;
                const std::string at = name + " after " + done;
                // The grids not Seabass's before the step: a step of
                // Seabass's keeps each on the stick or kept.
                std::set<std::string> theirs;
                for (const std::string &grid : {onTheStick, kept}) {
                    if (!grid.empty() && everSeabass.count(grid) == 0) {
                        theirs.insert(grid);
                    }
                }
                const std::string keptBefore = kept;
                if (step[0] == 'E') {
                    onTheStick = exportOver(name, 7 * std::stoi(step.substr(1)));
                    require(everSeabass.count(onTheStick) == 0, at + ": the export's grid is not Seabass's");
                } else if (step == "L") {
                    std::string error;
                    require(local::moveAnalysisLedgerAside(stick.root, std::chrono::system_clock::now(), &error)
                                .has_value(),
                            at + ": the ledger is moved aside: " + error);
                } else if (step == "D") {
                    writeBytes(infrastructure::paths::stickAnalysisLedger(stick.root), "damaged");
                } else if (step == "A" || step == "T") {
                    if (step == "T") {
                        withRange(QStringLiteral("58-115"));
                    }
                    analyseAgainStaged(playlist, at);
                    const QVariantMap summary = savedClean(at);
                    withRange(defaultAnalysisTempoRange());
                    invalidate();
                    const bool writes = kept.empty() || onTheStick == kept || seabass.count(onTheStick) > 0;
                    if (!writes) {
                        // Changed outside Seabass: the note says what can be
                        // done, and what it costs.
                        require(summary.value("notes").toString().contains(QStringLiteral(
                                    "its analysis was changed outside Seabass since Seabass kept its original, and "
                                    "Seabass leaves it alone. Removing the kept original in Free Up Space lets Seabass "
                                    "analyse it again; that discards the original Seabass kept."))
                                    && !summary.value("notes").toString().contains(QStringLiteral("Restore the original")),
                                at + ": the summary says what can be done: " + summary.value("notes").toString().toStdString());
                    }
                    if (writes) {
                        if (kept.empty() && everSeabass.count(onTheStick) == 0) {
                            kept = onTheStick;
                        }
                        onTheStick = gridOf(name);
                        everSeabass.insert(onTheStick);
                        if (!kept.empty()) {
                            seabass.insert(onTheStick);
                        }
                        require(step == "A" ? onTheStick == seabassGrid[name] : onTheStick != seabassGrid[name],
                                at + ": Analyse again wrote Seabass's grid of that tempo range");
                    }
                } else if (kept.empty()) {
                    // Nothing kept: the restore is refused before it stages.
                    require(restorer.restoreOriginalAnalysis(pioneer, engine, playlist), at + ": the restore starts");
                    require(waitFor(&restorer, &OriginalAnalysisController::busyChanged, [&] { return !restorer.busy(); }),
                            at + ": the restore is prepared");
                    require(!restorer.errorMessage().isEmpty() && session->pendingCount() == 0,
                            at + ": with nothing kept the restore is refused");
                } else {
                    const QVariantMap staged = restore(playlist);
                    if (onTheStick != kept && seabass.count(onTheStick) == 0) {
                        // Changed outside Seabass: left out when the restore
                        // is prepared, with the true reason, never staged.
                        const QVariantList left = staged.value("leftAlone").toList();
                        require(staged.value("staged").toInt() == 0 && session->pendingCount() == 0 && left.size() == 1
                                    && left[0].toMap().value("why").toString().contains(
                                        QStringLiteral("was changed outside Seabass since its original analysis was kept")),
                                at + ": a grid changed outside Seabass is left out of the restore, saying why");
                    }
                    if (staged.value("staged").toInt() > 0) {
                        savedClean(at);
                    }
                    if (!kept.empty() && (onTheStick == kept || seabass.count(onTheStick) > 0)) {
                        onTheStick = kept;
                        kept.clear();
                        seabass.clear();
                    }
                }
                invalidate();
                require(gridOf(name) == onTheStick, at + ": the grid on the stick is the one the rule says");
                const auto found = keptOf(keyOf(name));
                require(kept.empty() ? found.status == local::KeptRekordboxAnalysisLookup::Status::None
                                     : found.status == local::KeptRekordboxAnalysisLookup::Status::Found
                                           && found.entry.originalGrid == kept,
                        at + ": the kept original is the one the rule says");
                require(keptBefore.empty() || kept.empty() || kept == keptBefore,
                        at + ": a kept original is never replaced");
                if (step[0] != 'E') {
                    for (const std::string &grid : theirs) {
                        require(gridOf(name) == grid
                                    || (found.status == local::KeptRekordboxAnalysisLookup::Status::Found
                                        && found.entry.originalGrid == grid),
                                at + ": a grid that is not Seabass's is on the stick or kept");
                    }
                }
                if (!kept.empty()) {
                    const auto record = recordOf(keyOf(name));
                    require(record.status == local::KeptRekordboxRecordLookup::Status::Found
                                && std::set<std::string>(record.record.seabassGrids.begin(),
                                                         record.record.seabassGrids.end())
                                       == seabass,
                            at + ": the record lists every grid Seabass wrote since the keep");
                }
            }
            std::cout << "  " << name << ": " << done << " OK\n";
        }
        ledgerAsBefore(ledgerBefore);
        std::cout << "case 1 (export, Analyse again and Restore in any order, the ledger lost or damaged: a kept "
                     "original is never replaced and no grid of rekordbox's is lost) OK\n";
    }

    // 2. A OneLibrary renumbered by a new export.
    if (runs(2)) {
        exportOver("r one.wav", 9);
        analyseAgainStaged(QStringLiteral("Rows"), "Analyse again of " + std::string("Rows"));
        savedClean("2: Analyse again");
        const auto keptRows = keptOf(keyOf("r one.wav")).entry.rows;
        const auto oneRows = oneLibraryRows("r one.wav");
        const auto twoRows = oneLibraryRows("r two.wav");
        require(oneRows.size() == 1 && twoRows.size() == 1, "2: one OneLibrary row each");
        const std::int64_t one = oneRows[0].contentId;
        const std::int64_t two = twoRows[0].contentId;
        // r two's tempo, which no restore of r one may touch.
        const auto twoBefore = twoRows[0];
        {
            OneLibrarySql sql(stick.pioneer);
            for (const std::string &table : sql.tablesWith("content_id")) {
                sql.db.exec("UPDATE \"" + table + "\" SET content_id = -1 WHERE content_id = " + std::to_string(one));
                sql.db.exec("UPDATE \"" + table + "\" SET content_id = " + std::to_string(one)
                            + " WHERE content_id = " + std::to_string(two));
                sql.db.exec("UPDATE \"" + table + "\" SET content_id = " + std::to_string(two)
                            + " WHERE content_id = -1");
            }
        }
        invalidate();
        require(oneLibraryRows("r one.wav")[0].contentId == two && oneLibraryRows("r two.wav")[0].contentId == one,
                "2: the content ids are swapped");
        const std::uint32_t pdbBefore = infrastructure::rekordbox::PdbRowWriter(pdbFile).trackTempoAndKeyId(pdbId("r one.wav"))->tempo;
        require(pdbBefore != 11950, "2: export.pdb holds Seabass's tempo before the restore");
        const std::size_t logBefore = logText().size();
        require(restore(QStringLiteral("Rows")).value("staged").toInt() == 1, "2: the restore of r one is staged");
        savedClean("2: the restore");
        invalidate();
        const auto twoAfter = oneLibraryRows("r two.wav")[0];
        require(twoAfter.bpmx100 == twoBefore.bpmx100 && twoAfter.keyId == twoBefore.keyId,
                "2: r two, whose row has r one's old id now, keeps its tempo and key");
        require(oneLibraryRows("r one.wav")[0].bpmx100 != std::optional<std::int64_t>(11950),
                "2: r one's OneLibrary row is left alone");
        require(infrastructure::rekordbox::PdbRowWriter(pdbFile).trackTempoAndKeyId(pdbId("r one.wav"))->tempo == 11950,
                "2: export.pdb, not renumbered, gets the kept tempo");
        require(logText().substr(logBefore).find("none of its rows for the file is one that was kept") != std::string::npos,
                "2: the restore says OneLibrary's rows were left alone");
        (void)keptRows;
        std::cout << "case 2 (a renumbered OneLibrary is left alone, the other track untouched) OK\n";
    }

    // 3. Keys by name, and a row added since the keep.
    if (runs(3)) {
        exportOver("r three.wav", 11, "Fm");
        analyseAgainStaged(QStringLiteral("Keys"), "Analyse again of " + std::string("Keys"));
        savedClean("3: Analyse again");
        auto entry = keptOf(keyOf("r three.wav")).entry;
        std::string pdbName;
        std::string oneName;
        for (auto &row : entry.rows) {
            require(row.keyName.has_value() && row.keyId && *row.keyId != 0, "3: each kept row names its key by name");
            (row.library == "rekordbox" ? pdbName : oneName) = *row.keyName;
        }
        require(!pdbName.empty() && !oneName.empty(), "3: a key kept from each library");
        // export.pdb's key table as a new export leaves it: the kept id
        // names another key now. Another track's key id, of another name.
        std::uint32_t otherId = 0;
        {
            const infrastructure::rekordbox::PdbRowWriter pdb(pdbFile);
            for (const auto &row : pdb.trackPathRows()) {
                const auto values = pdb.trackTempoAndKeyId(row.id);
                if (values && values->keyName && *values->keyName != pdbName) {
                    otherId = values->keyId;
                }
            }
        }
        require(otherId != 0, "3: export.pdb has a key of another name");
        for (auto &row : entry.rows) {
            if (row.library == "rekordbox") {
                row.keyId = otherId;
            }
        }
        rekeep(entry);
        // exportLibrary.db's as a new export leaves it: the key table
        // renumbered (every key's id, in every table that names one), so
        // the kept id names no key or another one; and a second row for the
        // file, which was not there when the original was kept.
        std::int64_t added = 0;
        std::int64_t keptOneKeyId = 0;
        constexpr std::int64_t Renumbered = 1000;
        {
            OneLibrarySql sql(stick.pioneer);
            for (const std::string &table : sql.tablesWith("key_id")) {
                sql.db.exec("UPDATE \"" + table + "\" SET key_id = key_id + " + std::to_string(Renumbered)
                            + " WHERE key_id IS NOT NULL AND key_id != 0");
            }
            for (const auto &row : entry.rows) {
                if (row.library == "onelibrary") {
                    keptOneKeyId = *row.keyId;
                    infrastructure::onelibrary::SqlCipherStatement top(sql.db, "SELECT max(content_id) FROM content");
                    require(top.step(), "3: the content table has rows");
                    added = top.columnInt64(0) + 1;
                    sql.db.exec("CREATE TEMP TABLE copied AS SELECT * FROM content WHERE content_id = "
                                + std::to_string(row.rowId));
                    sql.db.exec("UPDATE copied SET content_id = " + std::to_string(added));
                    sql.db.exec("INSERT INTO content SELECT * FROM copied");
                }
            }
        }
        invalidate();
        require(oneLibraryRows("r three.wav").size() == 2, "3: OneLibrary has two rows for the file now");
        const std::size_t logBefore = logText().size();
        require(restore(QStringLiteral("Keys")).value("staged").toInt() == 1, "3: the restore is staged");
        savedClean("3: the restore");
        const auto pdbNow = infrastructure::rekordbox::PdbRowWriter(pdbFile).trackTempoAndKeyId(pdbId("r three.wav"));
        require(pdbNow && pdbNow->tempo == 11950 && pdbNow->keyName == pdbName && pdbNow->keyId != otherId,
                "3: export.pdb gets the kept key by its name, not the kept id");
        const auto oneNow = oneLibraryRows("r three.wav");
        require(oneNow.size() == 2, "3: still two OneLibrary rows");
        for (const auto &row : oneNow) {
            require(row.bpmx100 == std::optional<std::int64_t>(11950) && row.keyName == oneName
                        && row.keyId == std::optional<std::int64_t>(keptOneKeyId + Renumbered),
                    "3: every OneLibrary row of the file, the added one too, gets the kept key by its name: the id the "
                    "renumbered table gives that name, not the kept id");
        }
        require(logText().substr(logBefore).find("1 of them not there when the original was kept") != std::string::npos,
                "3: the log counts the row that was not there when the original was kept");
        std::cout << "case 3 (keys put back by name in both libraries; a row added since gets them too) OK\n";
    }

    // 4. A kept key that cannot be written.
    if (runs(4)) {
        exportOver("r four.wav", 13, "Fm");
        analyseAgainStaged(QStringLiteral("Refuse"), "Analyse again of " + std::string("Refuse"));
        savedClean("4: Analyse again");
        auto entry = keptOf(keyOf("r four.wav")).entry;
        for (auto &row : entry.rows) {
            if (row.library == "rekordbox") {
                row.keyId = 3;
                row.keyName = "Fm ";  // ends in a blank: no row may hold it
            }
        }
        rekeep(entry);
        const std::string keptBytes = bytesOf(keptFile(keyOf("r four.wav")));
        const std::string datBefore = bytesOf(anlzOf("r four.wav", "DAT"));
        const std::size_t logBefore = logText().size();
        require(restore(QStringLiteral("Refuse")).value("staged").toInt() == 1, "4: the restore is staged");
        const QVariantMap summary = saveNow();
        require(summary.value("error").toString().isEmpty(), "4: the save does not fail");
        require(logText().substr(logBefore).find("the tempo and key kept for it cannot be put back") != std::string::npos,
                "4: the track is refused, saying why");
        require(bytesOf(anlzOf("r four.wav", "DAT")) == datBefore && bytesOf(keptFile(keyOf("r four.wav"))) == keptBytes,
                "4: the track is left whole: its analysis files and the kept entry as they were");
        std::cout << "case 4 (a kept key that cannot be written refuses the track before anything is written) OK\n";
    }

    // 5. Relink.
    if (runs(5)) {
        const int theirs = 15;
        exportOver("m one.wav", theirs);
        const std::string original = gridOf("m one.wav");
        analyseAgainStaged(QStringLiteral("Move"), "Analyse again of " + std::string("Move"));
        savedClean("5: Analyse again");
        const std::string oldKey = keyOf("m one.wav");
        const std::string newKey = keyOf("m won.wav");
        require(fs::exists(keptFile(oldKey)) && ledger().count(oldKey) == 1 && gridLedger().count(oldKey) == 1,
                "5: kept, and in both ledgers, under the old name");
        // The file moved outside Seabass; Library Health relinks it.
        domain::MovedFileFinding finding;
        const auto add = [&](domain::Track row, const char *format) {
            row.format = format;
            finding.rows.push_back(row);
        };
        add(pdbRow("m one.wav"), "rekordbox");
        add(rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "m one.wav"), "onelibrary");
        add(rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "m one.wav"), "engine");
        finding.expectedPath = finding.rows.front().filePath;
        fs::rename(onStick / "m one.wav", onStick / "m won.wav");
        domain::Track file;
        file.filePath = pathToUtf8(onStick / "m won.wav");
        file.fileSizeBytes = fs::file_size(onStick / "m won.wav");
        require(relinkRefusalFor(pioneer, finding, file.filePath).empty(),
                "5: the relink can be offered: " + relinkRefusalFor(pioneer, finding, file.filePath));
        const std::string keptBytes = bytesOf(keptFile(oldKey));
        const auto relink = [&](const std::string &what) {
            require(session->stage(std::make_unique<RelinkMovedFileChange>(pioneer, engine, finding, file)),
                    "5: the relink is staged");
            savedClean(what);
            invalidate();
        };
        relink("5: the relink");
        const auto carried = keptOf(newKey);
        require(!fs::exists(keptFile(oldKey)) && carried.status == local::KeptRekordboxAnalysisLookup::Status::Found
                    && carried.entry.stickPath == "/Contents/UnknownArtist/UnknownAlbum/m won.wav"
                    && carried.entry.originalGrid == original,
                "5: rekordbox's kept original is carried to the new name");
        require(recordOf(newKey).status == local::KeptRekordboxRecordLookup::Status::Found
                    && !fs::exists(recordFile(oldKey)),
                "5: its record goes with it");
        require(ledger().count(oldKey) == 0 && ledger().count(newKey) == 1 && ledger().at(newKey).pathKey == newKey,
                "5: the analysis ledger's entry is carried");
        require(gridLedger().count(oldKey) == 0 && gridLedger().count(newKey) == 1, "5: the beat grid line is carried");
        const auto listing = KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8());
        for (const auto &group : listing.groups) {
            require(!group.gone, "5: Free Up Space sees the kept original's track on the stick");
        }
        undoNow();
        invalidate();
        require(bytesOf(keptFile(oldKey)) == keptBytes && !fs::exists(keptFile(newKey)) && ledger().count(oldKey) == 1
                    && ledger().count(newKey) == 0 && gridLedger().count(oldKey) == 1 && gridLedger().count(newKey) == 0,
                "5: Undo of the relink puts the kept original and the ledgers back under the old name");
        relink("5: the relink again");
        // Analyse again at the new name: Seabass's grid, the original stays.
        const std::string carriedBytes = bytesOf(keptFile(newKey));
        analyseAgainStaged(QStringLiteral("Move"), "Analyse again of " + std::string("Move"));
        savedClean("5: Analyse again at the new name");
        require(bytesOf(keptFile(newKey)) == carriedBytes, "5: Analyse again at the new name keeps the original as it is");
        require(restore(QStringLiteral("Move")).value("staged").toInt() == 1, "5: the restore is staged");
        savedClean("5: the restore at the new name");
        invalidate();
        require(gridOf("m won.wav") == original, "5: the original grid is back at the new name");
        std::cout << "case 5 (a relink carries the kept original and both ledgers; Undo takes them back) OK\n";
    }

    // 6. Seabass's grid known by another track's records only.
    if (runs(6)) {
        const std::string key = keyOf("m two.wav");
        // Its ledger entry under another name, as a move nobody carried
        // would leave it.
        local::AnalysisLedger moved = ledger();
        local::AnalysisLedgerEntry entry = moved.at(key);
        moved.erase(key);
        entry.pathKey = "contents/elsewhere/m two.wav";
        moved[entry.pathKey] = entry;
        writeLedger(moved);
        analyseAgainStaged(QStringLiteral("Elsewhere"), "Analyse again of " + std::string("Elsewhere"));
        savedClean("6: Analyse again");
        require(!fs::exists(keptFile(key)), "6: a grid another ledger entry has is Seabass's: nothing kept");
        // No ledger entry at all; the grid in another kept entry's head.
        local::AnalysisLedger none = ledger();
        none.erase(key);
        none.erase("contents/elsewhere/m two.wav");
        writeLedger(none);
        local::KeptRekordboxAnalysis other;
        other.pathKey = "contents/elsewhere/other.wav";
        other.stickPath = "/Contents/elsewhere/other.wav";
        other.originalGrid = std::string(64, 'a');
        other.files.push_back({"/PIONEER/USBANLZ/P000/00000001/ANLZ0000.DAT", std::string("x"), std::nullopt, std::nullopt});
        local::KeptRekordboxRecord otherRecord = plainRecord(other.pathKey);
        otherRecord.seabassGrids = {seabassGrid["m two.wav"]};
        local::StickKeptRekordboxAnalysisStore(stick.root).keep(other, otherRecord);
        analyseAgainStaged(QStringLiteral("Elsewhere"), "Analyse again of " + std::string("Elsewhere"));
        savedClean("6: Analyse again with no ledger entry");
        require(!fs::exists(keptFile(key)), "6: a grid another kept entry records as Seabass's: nothing kept");
        fs::remove(keptFile(other.pathKey));
        std::cout << "case 6 (Seabass's grid known by another track's ledger entry or kept entry is never kept as an "
                     "original) OK\n";
    }

    // 7. The audio file's size.
    if (runs(7)) {
        exportOver("z one.wav", 17);
        analyseAgainStaged(QStringLiteral("Size"), "Analyse again of " + std::string("Size"));
        savedClean("7: Analyse again");
        const std::string key = keyOf("z one.wav");
        const std::string keptBytes = bytesOf(keptFile(key));
        const fs::path audio = onStick / "z one.wav";
        const std::string audioBytes = bytesOf(audio);
        // The file of another size than the one kept for (the rows follow
        // the file, so Analyse again takes it), with Seabass's grid on the
        // stick: the kept entry stays as it is, and a restore would refuse
        // the file of another size.
        {
            auto entry = keptOf(key).entry;
            entry.audioFileSize += 64;
            rekeep(entry);
        }
        const std::string otherSize = bytesOf(keptFile(key));
        analyseAgainStaged(QStringLiteral("Size"), "7: Analyse again of a file of another size than kept for");
        savedClean("7: Analyse again of a file of another size");
        require(bytesOf(keptFile(key)) == otherSize, "7: another size, a grid Seabass knows: the kept entry as it was");
        {
            const std::string datBeforeRestore = bytesOf(anlzOf("z one.wav", "DAT"));
            const std::size_t logBeforeRestore = logText().size();
            require(restore(QStringLiteral("Size")).value("staged").toInt() == 1, "7: the restore is staged");
            savedClean("7: the restore of a file of another size than kept for");
            require(logText().substr(logBeforeRestore).find("its audio file changed since its analysis was kept")
                            != std::string::npos
                        && bytesOf(anlzOf("z one.wav", "DAT")) == datBeforeRestore
                        && bytesOf(keptFile(key)) == otherSize,
                    "7: Restore refuses a file of another size than kept for, saying why, and writes nothing");
        }
        writeBytes(keptFile(key), keptBytes);
        // Changed after the analysis: left alone.
        analyseAgainStaged(QStringLiteral("Size"), "Analyse again of " + std::string("Size"));
        {
            std::ofstream out(audio, std::ios::binary | std::ios::app);
            out << std::string(64, '\0');
        }
        std::size_t logBefore = logText().size();
        std::string datBefore = bytesOf(anlzOf("z one.wav", "DAT"));
        savedClean("7: Analyse again of a file changed since its analysis");
        require(logText().substr(logBefore).find("its audio file changed since it was analysed") != std::string::npos
                    && bytesOf(anlzOf("z one.wav", "DAT")) == datBefore,
                "7: a file changed since it was analysed is left alone, saying why");
        // A size that cannot be read: a folder where the file was.
        writeBytes(audio, audioBytes);
        analyseAgainStaged(QStringLiteral("Size"), "Analyse again of " + std::string("Size"));
        fs::remove(audio);
        fs::create_directory(audio);
        logBefore = logText().size();
        datBefore = bytesOf(anlzOf("z one.wav", "DAT"));
        savedClean("7: Analyse again of a file whose size cannot be read");
        require(logText().substr(logBefore).find("the size of its audio file cannot be read") != std::string::npos
                    && bytesOf(anlzOf("z one.wav", "DAT")) == datBefore && bytesOf(keptFile(key)) == keptBytes,
                "7: a size that cannot be read leaves the track alone, saying why");
        fs::remove(audio);
        writeBytes(audio, audioBytes);
        invalidate();
        std::cout << "case 7 (the audio file's size: the kept entry stays; changed or unreadable, the track is left "
                     "alone) OK\n";
    }

    // 8. A cue set in the .EXT after the keep.
    if (runs(8)) {
        exportOver("c one.wav", 19);
        analyseAgainStaged(QStringLiteral("Cue"), "Analyse again of " + std::string("Cue"));
        savedClean("8: Analyse again");
        const auto kept = keptOf(keyOf("c one.wav")).entry;
        const AnlzFile keptExt = AnlzFile::fromBytes(*kept.files.at(0).ext, "the kept .EXT");
        require(session->stage(std::make_unique<AddCueChange>(QStringLiteral("rekordbox"), pioneer,
                                                              QString::fromStdString(pdbRow("c one.wav").sourceId),
                                                              3000.0, QStringLiteral("hot"), 3, QStringLiteral("#00FF00"),
                                                              QStringLiteral("set since"), false, 0.0,
                                                              QStringLiteral("c one"))),
                "8: the cue is staged");
        savedClean("8: the cue");
        const auto extListsNow = cueListsOf(AnlzFile::readRaw(pathToUtf8(anlzOf("c one.wav", "EXT"))));
        require(extListsNow != cueListsOf(keptExt), "8: the .EXT's cue lists changed after the keep");
        require(restore(QStringLiteral("Cue")).value("staged").toInt() == 1, "8: the restore is staged");
        savedClean("8: the restore");
        invalidate();
        require(cueListsOf(AnlzFile::readRaw(pathToUtf8(anlzOf("c one.wav", "EXT")))) == extListsNow,
                "8: the .EXT's cue lists after the restore are the ones set since the keep");
        bool found = false;
        for (const auto &cue : pdbRow("c one.wav").cues) {
            found = found || (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == 3
                              && std::abs(cue.positionMs - 3000.0) < 1.0);
        }
        require(found, "8: the cue set since the keep is there after the restore");
        std::cout << "case 8 (a cue point set in the .EXT after the keep survives the restore) OK\n";
    }

    // 9. No room for what is to be kept.
    if (runs(9)) {
        const std::string datTheirs = [&] {
            exportOver("q one.wav", 21);
            return bytesOf(anlzOf("q one.wav", "DAT"));
        }();
        setStickBytesAvailableForTesting([](const std::string &) { return std::uint64_t(500 * 1024); });
        analyseAgainStaged(QStringLiteral("Room"), "Analyse again of " + std::string("Room"));
        const std::size_t logBefore = logText().size();
        const QVariantMap summary = saveNow();
        setStickBytesAvailableForTesting({});
        require(summary.value("error").toString().isEmpty(), "9: the save does not fail");
        require(!fs::exists(keptFile(keyOf("q one.wav"))) && bytesOf(anlzOf("q one.wav", "DAT")) == datTheirs,
                "9: nothing kept and nothing written");
        require(logText().substr(logBefore).find("the stick has no room to keep rekordbox's analysis") != std::string::npos,
                "9: the log says why");
        const bool noted =
            summary.value("notes").toString().contains(QStringLiteral("no room to keep the analysis rekordbox made"));
        require(noted, "9: the save's summary says so");
        // Free space that cannot be read is not "no room".
        setStickBytesAvailableForTesting([](const std::string &) { return std::optional<std::uint64_t>(); });
        analyseAgainStaged(QStringLiteral("Room"), "9: Analyse again, free space unknown");
        savedClean("9: Analyse again, free space unknown");
        setStickBytesAvailableForTesting({});
        require(fs::exists(keptFile(keyOf("q one.wav"))) && bytesOf(anlzOf("q one.wav", "DAT")) != datTheirs,
                "9: free space that cannot be read: kept and written");
        std::cout << "case 9 (no room to keep the original: the track is left alone before anything is written) OK\n";
    }

    // 10. Restore staged, then Analyse again.
    if (runs(10)) {
        exportOver("g one.wav", 23);
        analyseAgainStaged(QStringLiteral("Staged"), "Analyse again of " + std::string("Staged"));
        savedClean("10: Analyse again");
        require(restore(QStringLiteral("Staged")).value("staged").toInt() == 1, "10: the restore is staged");
        const QVariantMap again = analyseAgain(QStringLiteral("Staged"));
        require(again.value("analysed").toInt() == 0 && again.value("leftAlone").toInt() == 1,
                "10: Analyse again leaves the track out");
        const QVariantList left = again.value("leftAloneTracks").toList();
        require(left.size() == 1
                    && left[0].toMap().value("why").toString().contains(QStringLiteral("Restore original analysis")),
                "10: and says why");
        require(!session->hasChange(ReanalyseTrackChange::idFor("/Contents/UnknownArtist/UnknownAlbum/g one.wav"))
                    && session->hasChange(RestoreAnalysisChange::idFor("/Contents/UnknownArtist/UnknownAlbum/g one.wav")),
                "10: the restore waits, no Analyse again beside it");
        restorer.dropStaged();
        require(session->pendingCount() == 0, "10: nothing waits");
        std::cout << "case 10 (Analyse again leaves out a track whose restore waits) OK\n";
    }

    // 11. The ledgers' order: the change applied later wins.
    if (runs(11)) {
        const std::string key = "contents/ledger/order.wav";
        const std::string first(64, '1');
        const std::string later(64, '2');
        local::AnalysisLedgerEntry entry = ledger().begin()->second;
        entry.pathKey = key;
        // Applied first, its id sorting last.
        auto applied1 = std::make_shared<LedgerNoteChange>(
            QStringLiteral("ledger-order:zzz"), stickRoot, [&](SaveContext &ctx, const QString &id) {
                recordGridInLedgerAfterCommit(ctx, stickRoot, id, key, first);
                recordAnalysisAfterCommit(ctx, stickRoot, id, entry, "ledger-order");
            });
        auto applied2 = std::make_shared<LedgerNoteChange>(
            QStringLiteral("ledger-order:aaa"), stickRoot, [&](SaveContext &ctx, const QString &id) {
                recordGridInLedgerAfterCommit(ctx, stickRoot, id, key, later);
                forgetAnalysisAfterCommit(ctx, stickRoot, id, key, "ledger-order");
            });
        const auto saved = testing::saveChanges({applied1, applied2}, pioneer, engine);
        require(saved.error.isEmpty(), "11: the save lands");
        require(gridLedger().count(key) == 1 && gridLedger().at(key) == later,
                "11: the beat grid ledger holds the grid of the change applied later");
        require(ledger().count(key) == 0, "11: the analysis ledger: the later change took the entry out");
        std::cout << "case 11 (two changes of one save on the same ledger line: the one applied later wins) OK\n";
    }

    // A copy of a track's analysis files in a folder of their own, as an
    // export that gives OneLibrary's row its own files makes it: the .DAT
    // path as a row names it, and the .DAT on the stick.
    const auto copyAnalysisOf = [&](const std::string &name, const std::string &folder) {
        const std::string dat = "/PIONEER/USBANLZ/P0TW/" + folder + "/ANLZ0000.DAT";
        const fs::path to = stick.root / pathFromUtf8(dat.substr(1));
        fs::create_directories(to.parent_path());
        for (const char *extension : {"DAT", "EXT", "2EX"}) {
            const fs::path from = anlzOf(name, extension);
            if (fs::exists(from)) {
                fs::copy_file(from, to.parent_path() / ("ANLZ0000." + std::string(extension)),
                              fs::copy_options::overwrite_existing);
            }
        }
        return std::pair{dat, to};
    };
    // OneLibrary's row of the track naming the analysis file `dat`.
    const auto pointOneLibraryOf = [&](const std::string &name, const std::string &dat) {
        const std::int64_t content = oneLibraryRows(name).at(0).contentId;
        {
            OneLibrarySql sql(stick.pioneer);
            sql.db.exec("UPDATE content SET analysisDataFilePath = '" + dat + "' WHERE content_id = "
                        + std::to_string(content));
        }
        invalidate();
        require(rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name).analysisFile == dat,
                "OneLibrary's row of " + name + " names " + dat);
    };

    // A hot cue's place in each library (-1: none), the cue lists of a
    // track's analysis files, a hot cue set in rekordbox's libraries or
    // Engine, and a track made ready for case 13 and 19: rekordbox's grid
    // 4 ms from Seabass's (so a cue on one is on a beat of the other, and
    // a fresh plan would move it the 4 ms the wrong way), two hot cues on
    // its beats 8 and 16 (in Engine too, when asked), then Analyse again,
    // its cue switch as given.
    const auto hotIn = [](const std::vector<domain::CuePoint> &cues, int pad) {
        for (const auto &cue : cues) {
            if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == pad && !cue.isLoop) {
                return cue.positionMs;
            }
        }
        return -1.0;
    };
    const auto hotCue = [&](const std::string &name, int pad) { return hotIn(pdbRow(name).cues, pad); };
    const auto oneLibraryHotCue = [&](const std::string &name, int pad) {
        return hotIn(rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name).cues, pad);
    };
    const auto engineRowOf = [&](const std::string &name) {
        return rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name);
    };
    const auto engineHotCue = [&](const std::string &name, int pad) { return hotIn(engineRowOf(name).cues, pad); };
    const auto cueListsNow = [&](const std::string &name) {
        std::vector<std::string> lists = cueListsOf(AnlzFile::readRaw(pathToUtf8(anlzOf(name, "DAT"))));
        for (const std::string &one : cueListsOf(AnlzFile::readRaw(pathToUtf8(anlzOf(name, "EXT"))))) {
            lists.push_back(one);
        }
        return lists;
    };
    const auto setCueIn = [&](const QString &format, const std::string &name, int pad, double at) {
        const bool inEngine = format == QLatin1String("engine");
        require(session->stage(std::make_unique<AddCueChange>(
                    format, inEngine ? engine : pioneer,
                    QString::fromStdString(inEngine ? engineRowOf(name).sourceId : pdbRow(name).sourceId), at,
                    QStringLiteral("hot"), pad, QStringLiteral("#00FF00"), QStringLiteral("cue"), false, 0.0,
                    QString::fromStdString(name))),
                "a cue is staged");
        savedClean("the cue on " + name);
        invalidate();
    };
    const auto setCue = [&](const std::string &name, int pad, double at) {
        setCueIn(QStringLiteral("rekordbox"), name, pad, at);
    };
    const auto prepareTrack = [&](const std::string &name, const QString &playlist, bool moveCues,
                                  bool engineToo = false) {
        exportOver(name, 4);
        const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), pdbRow(name).sourceId);
        require(grid.size() > 20, name + " has rekordbox's grid");
        const std::pair<double, double> onBeats{std::round(grid[8].timeMs), std::round(grid[16].timeMs)};
        setCue(name, 1, onBeats.first);
        setCue(name, 2, onBeats.second);
        if (engineToo) {
            setCueIn(QStringLiteral("engine"), name, 1, onBeats.first);
            setCueIn(QStringLiteral("engine"), name, 2, onBeats.second);
        }
        analyseAgainStaged(playlist, "Analyse again of " + name);
        importer.setMoveCuesForAll(moveCues);
        savedClean("Analyse again of " + name);
        invalidate();
        return onBeats;
    };

    // 13. The cue points on Restore: only those Analyse again moved, back
    //     to where they were, and only those still where it put them.
    if (runs(13)) {
        // Each of rekordbox's libraries holds the same hot cues (OneLibrary
        // mirrors DeviceLibrary), and Engine's as they were before the
        // restore (Engine had none set, or keeps Seabass's grid).
        const auto librariesAgree = [&](const std::string &name, const std::vector<domain::CuePoint> &engineBefore,
                                        const std::string &what) {
            for (int pad = 1; pad <= 3; ++pad) {
                require(std::abs(oneLibraryHotCue(name, pad) - hotCue(name, pad)) < 1.0,
                        what + ": OneLibrary's hot cue " + std::to_string(pad) + " is DeviceLibrary's");
                require(std::abs(engineHotCue(name, pad) - hotIn(engineBefore, pad)) < 1.0,
                        what + ": Engine's hot cue " + std::to_string(pad) + " is as before the restore");
            }
        };
        // Declined at Analyse again: Restore offers no move and the cue
        // lists stay byte for byte.
        {
            const auto [one, two] = prepareTrack("u one.wav", QStringLiteral("CueNo"), false);
            require(std::abs(hotCue("u one.wav", 1) - one) < 1.0 && std::abs(hotCue("u one.wav", 2) - two) < 1.0,
                    "13: declined, the cues stay on rekordbox's beats");
            require(keptOf(keyOf("u one.wav")).entry.cueMoves.empty(), "13: declined, the kept entry records no move");
            const auto listsBefore = cueListsNow("u one.wav");
            const QVariantMap staged = restore(QStringLiteral("CueNo"));
            require(staged.value("cueTracks").toInt() == 0 && restorer.cueReview().isEmpty(),
                    "13: no cue move is offered when Analyse again moved none");
            restorer.setMoveCuesBackForAll(true);
            const auto engineBefore = engineRowOf("u one.wav").cues;
            savedClean("13: the restore of u one");
            invalidate();
            librariesAgree("u one.wav", engineBefore, "13: u one");
            require(cueListsNow("u one.wav") == listsBefore, "13: the cue lists after the restore, byte for byte");
        }
        // Moved at Analyse again, in Engine too, whose row had nothing of
        // the player's to keep (its grid was Seabass's from the import, as
        // a library made by Create Engine Library with its cues synced has
        // it): rekordbox's grid goes back and its cues with it, Engine
        // keeps Seabass's grid and the cues Analyse again moved onto it.
        {
            const auto [one, two] = prepareTrack("u two.wav", QStringLiteral("CueYes"), true, true);
            require(std::abs(hotCue("u two.wav", 1) - one) >= 3.0, "13: moved with the grid at Analyse again");
            const double engineMoved1 = engineHotCue("u two.wav", 1);
            const double engineMoved2 = engineHotCue("u two.wav", 2);
            require(std::abs(engineMoved1 - one) >= 3.0 && std::abs(engineMoved1 - hotCue("u two.wav", 1)) < 1.0
                        && std::abs(engineMoved2 - hotCue("u two.wav", 2)) < 1.0,
                    "13: Engine's cues moved with Seabass's grid at Analyse again, as rekordbox's did");
            const auto moves = keptOf(keyOf("u two.wav")).entry.cueMoves;
            require(moves.size() == 4, "13: the kept entry records the four moves, two in each library");
            require(recordOf(keyOf("u two.wav")).record.engineKept.empty()
                        && local::StickKeptEngineAnalysisStore(stick.root).find(keyOf("u two.wav")).status
                               == application::KeptEngineAnalysisLookup::Status::None,
                    "13: nothing of the player's analysis was kept");
            const std::string engineGrid = ledger().at(keyOf("u two.wav")).engineGrid;
            require(!engineGrid.empty(), "13: Engine holds Seabass's grid");
            const QVariantMap staged = restore(QStringLiteral("CueYes"));
            require(staged.value("cueTracks").toInt() == 1, "13: the move back is offered");
            restorer.setMoveCuesBackForAll(true);
            const QVariantMap summary = savedClean("13: the restore of u two");
            invalidate();
            require(std::abs(hotCue("u two.wav", 1) - one) < 1.0 && std::abs(hotCue("u two.wav", 2) - two) < 1.0,
                    "13: both of rekordbox's cues back where they were before Analyse again");
            require(std::abs(oneLibraryHotCue("u two.wav", 1) - one) < 1.0
                        && std::abs(oneLibraryHotCue("u two.wav", 2) - two) < 1.0,
                    "13: OneLibrary's too");
            require(std::abs(engineHotCue("u two.wav", 1) - engineMoved1) < 1.0
                        && std::abs(engineHotCue("u two.wav", 2) - engineMoved2) < 1.0,
                    "13: Engine's stay on the grid Engine keeps");
            require(gridOf("u two.wav") != seabassGrid["u two.wav"], "13: rekordbox's grid went back");
            require(ledger().count(keyOf("u two.wav")) == 1 && ledger().at(keyOf("u two.wav")).engineGrid == engineGrid
                        && ledger().at(keyOf("u two.wav")).rekordboxGrid.empty(),
                    "13: the ledger knows Engine keeps Seabass's grid, rekordbox's libraries their own");
            require(summary.value("notes").toString().contains(QStringLiteral(
                        "On one restored track, rekordbox's libraries have the kept analysis back and Engine keeps the "
                        "grid it has, as nothing of the player's analysis was kept. In rekordbox's libraries, the cue "
                        "points Analyse again moved that were still where it put them went back with the grid. In "
                        "Engine, the cue points Analyse again moved stay where they are, on the grid Engine keeps.")),
                    "13: the summary says which library went back, which stayed, and where the cue points are: "
                        + summary.value("notes").toString().toStdString());
            require(!fs::exists(keptFile(keyOf("u two.wav"))), "13: rekordbox's kept original has done its work");
        }
        // Moved, then one set by hand since: that one stays.
        {
            const auto [one, two] = prepareTrack("u three.wav", QStringLiteral("CueEdit"), true);
            const double byHand = hotCue("u three.wav", 1) + 777.0;
            setCue("u three.wav", 1, byHand);
            require(std::abs(hotCue("u three.wav", 1) - byHand) < 1.0, "13: the cue set by hand");
            require(restore(QStringLiteral("CueEdit")).value("cueTracks").toInt() == 1, "13: the move back is offered");
            restorer.setMoveCuesBackForAll(true);
            const auto engineBefore = engineRowOf("u three.wav").cues;
            savedClean("13: the restore of u three");
            invalidate();
            librariesAgree("u three.wav", engineBefore, "13: u three");
            require(std::abs(hotCue("u three.wav", 1) - byHand) < 1.0, "13: the cue set by hand since stays");
            require(std::abs(hotCue("u three.wav", 2) - two) < 1.0, "13: the other goes back where it was");
            (void)one;
        }
        // Nudged by the DJ by 20 ms since: it is not where Analyse again
        // put it any more, and stays.
        {
            const auto [one, two] = prepareTrack("u four.wav", QStringLiteral("CueNudge"), true);
            const double nudged = hotCue("u four.wav", 1) + 20.0;
            setCue("u four.wav", 1, nudged);
            require(restore(QStringLiteral("CueNudge")).value("cueTracks").toInt() == 1, "13: the move back is offered");
            restorer.setMoveCuesBackForAll(true);
            const auto engineBefore = engineRowOf("u four.wav").cues;
            savedClean("13: the restore of u four");
            invalidate();
            librariesAgree("u four.wav", engineBefore, "13: u four");
            require(std::abs(hotCue("u four.wav", 1) - nudged) < 1.0, "13: a cue nudged by 20 ms since stays");
            require(std::abs(hotCue("u four.wav", 2) - two) < 1.0, "13: the other goes back where it was");
            (void)one;
        }
        // Two memory cues 30 ms apart, the later one on a beat (it moves
        // with the grid), the earlier off the grid (it stays): the one that
        // moved goes back, the other is not touched.
        {
            const auto memoryCues = [&](const std::string &name) {
                std::vector<double> at;
                for (const auto &cue : pdbRow(name).cues) {
                    if (cue.kind == domain::CuePoint::Kind::Memory && !cue.isLoop) {
                        at.push_back(cue.positionMs);
                    }
                }
                std::sort(at.begin(), at.end());
                return at;
            };
            const auto setMemory = [&](const std::string &name, double at) {
                require(session->stage(std::make_unique<AddCueChange>(
                            QStringLiteral("rekordbox"), pioneer, QString::fromStdString(pdbRow(name).sourceId), at,
                            QStringLiteral("memory"), 0, QStringLiteral("#FF0000"), QStringLiteral("memory"), false,
                            0.0, QString::fromStdString(name))),
                        "13: a memory cue is staged");
                savedClean("13: the memory cue on " + name);
                invalidate();
            };
            exportOver("u five.wav", 4);
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), pdbRow("u five.wav").sourceId);
            require(grid.size() > 20, "13: u five has rekordbox's grid");
            const double onBeat = std::round(grid[8].timeMs);
            setMemory("u five.wav", onBeat - 30.0);
            setMemory("u five.wav", onBeat);
            analyseAgainStaged(QStringLiteral("CueMem"), "13: Analyse again of u five");
            importer.setMoveCuesForAll(true);
            savedClean("13: Analyse again of u five");
            invalidate();
            const auto moved = memoryCues("u five.wav");
            require(moved.size() == 2 && std::abs(moved[0] - (onBeat - 30.0)) < 1.0 && std::abs(moved[1] - onBeat) >= 3.0,
                    "13: the memory cue on the beat moved with the grid, the other did not");
            require(restore(QStringLiteral("CueMem")).value("cueTracks").toInt() == 1, "13: the move back is offered");
            restorer.setMoveCuesBackForAll(true);
            const auto engineBefore = engineRowOf("u five.wav").cues;
            savedClean("13: the restore of u five");
            invalidate();
            librariesAgree("u five.wav", engineBefore, "13: u five");
            std::vector<double> oneLibraryMemory;
            for (const auto &cue : rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)),
                                         "u five.wav")
                                       .cues) {
                if (cue.kind == domain::CuePoint::Kind::Memory && !cue.isLoop) {
                    oneLibraryMemory.push_back(cue.positionMs);
                }
            }
            std::sort(oneLibraryMemory.begin(), oneLibraryMemory.end());
            const auto back = memoryCues("u five.wav");
            require(oneLibraryMemory.size() == back.size()
                        && std::equal(back.begin(), back.end(), oneLibraryMemory.begin(),
                                      [](double a, double b) { return std::abs(a - b) < 1.0; }),
                    "13: OneLibrary's memory cues are DeviceLibrary's");
            require(back.size() == 2 && std::abs(back[0] - (onBeat - 30.0)) < 1.0 && std::abs(back[1] - onBeat) < 1.0,
                    "13: the memory cue that moved goes back, the one 30 ms from it stays");
        }
        // Another cue set since at the place a moved cue would go back to:
        // that move is not made, and the review says so.
        {
            const auto [one, two] = prepareTrack("u six.wav", QStringLiteral("CueHit"), true);
            const double movedTwo = hotCue("u six.wav", 2);
            setCue("u six.wav", 3, two);
            const QVariantMap staged = restore(QStringLiteral("CueHit"));
            require(staged.value("cueTracks").toInt() == 1 && staged.value("blockedCueTracks").toInt() == 1,
                    "13: the review offers the move that is free and says one is blocked");
            restorer.setMoveCuesBackForAll(true);
            const auto engineBefore = engineRowOf("u six.wav").cues;
            savedClean("13: the restore of u six");
            invalidate();
            librariesAgree("u six.wav", engineBefore, "13: u six");
            require(std::abs(hotCue("u six.wav", 2) - movedTwo) < 1.0,
                    "13: the cue whose old place holds another stays where it is");
            require(std::abs(hotCue("u six.wav", 3) - two) < 1.0, "13: the cue at the old place stays");
            require(std::abs(hotCue("u six.wav", 1) - one) < 1.0, "13: the free one goes back");
        }
        std::cout << "case 13 (Restore moves back only the cues Analyse again moved, to where they were) OK\n";
    }

    // 14. The player's analysis kept alone (Beat Grids & Waveforms): not
    //     Restore original analysis's, nor Free Up Space's.
    if (runs(14)) {
        application::KeptEngineAnalysis alone;
        alone.pathKey = keyOf("e one.wav");
        alone.trackFile = "../Contents/UnknownArtist/UnknownAlbum/e one.wav";
        alone.trackId = std::stoll(rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "e one.wav")
                                       .sourceId);
        alone.libraryUuid = "planted";
        local::StickKeptEngineAnalysisStore(stick.root).keep(alone);
        const fs::path file = local::keptEngineAnalysisFile(stick.root, alone.pathKey);
        forgetKeptAnalysisNames();
        restorer.refreshKeptCount(pioneer, engine, QStringLiteral("EngineAlone"));
        require(waitFor(&restorer, &OriginalAnalysisController::keptCountChanged,
                        [&] { return restorer.keptCountPlaylist() == QLatin1String("EngineAlone"); }),
                "14: counted");
        require(restorer.keptCount() == 0, "14: not counted");
        require(restorer.restoreOriginalAnalysis(pioneer, engine, QStringLiteral("EngineAlone")), "14: asked");
        require(waitFor(&restorer, &OriginalAnalysisController::busyChanged, [&] { return !restorer.busy(); }),
                "14: prepared");
        require(restorer.errorMessage().contains(QStringLiteral("Analyse again kept")) && session->pendingCount() == 0,
                "14: the restore is refused, nothing staged");
        for (const auto &group : KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8()).groups) {
            for (const auto &info : group.files) {
                require(info.file != pathToUtf8(file), "14: Free Up Space does not list it");
            }
        }
        require(fs::exists(file), "14: it stays on the stick");
        fs::remove(file);
        std::cout << "case 14 (the player's analysis kept alone is neither counted, restored nor listed) OK\n";
    }


    // The rows of a track as Library Health's moved-file finding lists
    // them, of the formats given.
    const auto findingOf = [&](const std::string &name, const std::set<std::string> &formats) {
        domain::MovedFileFinding finding;
        const auto add = [&](domain::Track row, const char *format) {
            if (formats.count(format) > 0) {
                row.format = format;
                finding.rows.push_back(row);
            }
        };
        add(pdbRow(name), "rekordbox");
        add(rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name), "onelibrary");
        add(rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name), "engine");
        finding.expectedPath = finding.rows.front().filePath;
        return finding;
    };
    // The file moved outside Seabass from `from` to `to`, and relinked.
    const auto moveAndRelink = [&](const std::string &from, const std::string &to, const std::set<std::string> &formats,
                                   const std::string &what) {
        const domain::MovedFileFinding finding = findingOf(from, formats);
        fs::rename(onStick / from, onStick / to);
        domain::Track file;
        file.filePath = pathToUtf8(onStick / to);
        file.fileSizeBytes = fs::file_size(onStick / to);
        require(relinkRefusalFor(pioneer, finding, file.filePath).empty(), what + ": the relink can be offered");
        require(session->stage(std::make_unique<RelinkMovedFileChange>(pioneer, engine, finding, file)),
                what + ": the relink is staged");
        const QVariantMap summary = savedClean(what);
        invalidate();
        return summary;
    };
    const auto engineIdOf = [&](const std::string &name) {
        return std::stoll(rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name).sourceId);
    };
    const auto plantEngineKept = [&](const std::string &name, const std::string &mark) {
        application::KeptEngineAnalysis kept;
        kept.pathKey = keyOf(name);
        kept.trackFile = "../Contents/UnknownArtist/UnknownAlbum/" + name;
        kept.libraryUuid = mark;
        return kept;
    };

    // 15. The released relink never fails for what Seabass keeps: Copy
    //     Beat Grids kept the player's analysis at /B, the file moved to
    //     /A and was relinked (as on master, the kept entry stayed under
    //     B), Copy Beat Grids kept again at /A, the file moved back to /B
    //     and is relinked: it lands, both kept entries are still there, the
    //     one under the name it gets stays, and the log says so; the beat
    //     grid ledger's line goes with the file. A kept original with no
    //     record cannot be told about, and stays where it is.
    if (runs(15)) {
        const std::string b = "k one.wav";
        const std::string a = "k won.wav";
        const std::set<std::string> all{"rekordbox", "onelibrary", "engine"};
        moveAndRelink(b, a, all, "15: the first relink");
        auto atB = plantEngineKept(b, "kept at B");
        atB.trackId = engineIdOf(a);
        local::StickKeptEngineAnalysisStore(stick.root).keep(atB);
        auto atA = plantEngineKept(a, "kept at A");
        atA.trackId = engineIdOf(a);
        local::StickKeptEngineAnalysisStore(stick.root).keep(atA);
        {
            auto lines = gridLedger();
            lines[keyOf(b)] = std::string(64, 'b');
            lines[keyOf(a)] = std::string(64, 'a');
            std::string error;
            require(local::writeBeatGridLedger(stick.root, lines, &error), "15: the grid ledger is planted: " + error);
        }
        const std::size_t logBefore = logText().size();
        const QVariantMap summary = moveAndRelink(a, b, all, "15: the relink back");
        require(summary.value("warning").toString().isEmpty(), "15: the relink back lands without a warning");
        require(pdbRow(b).filePath.find(b) != std::string::npos, "15: the rows point at the file again");
        const auto keptB = local::StickKeptEngineAnalysisStore(stick.root).find(keyOf(b));
        const auto keptA = local::StickKeptEngineAnalysisStore(stick.root).find(keyOf(a));
        require(keptB.status == application::KeptEngineAnalysisLookup::Status::Found
                    && keptB.entry.libraryUuid == "kept at B"
                    && keptA.status == application::KeptEngineAnalysisLookup::Status::Found
                    && keptA.entry.libraryUuid == "kept at A",
                "15: both kept entries are still there, the one under the name it gets in its place");
        require(logText().substr(logBefore).find("the player's analysis is kept under the new name already, and that one "
                                                 "stays; the one kept under the old name stays where it is")
                    != std::string::npos,
                "15: the log says which one stays");
        require(gridLedger().count(keyOf(a)) == 0 && gridLedger().at(keyOf(b)) == std::string(64, 'a'),
                "15: the grid ledger's line of the name the file had goes with it");
        // An original kept with no record beside it: the pair stays where
        // it is, the relink lands.
        exportOver(b, 29);
        analyseAgainStaged(QStringLiteral("Relink"), "15: Analyse again");
        savedClean("15: Analyse again");
        fs::remove(recordFile(keyOf(b)));
        // And the beat grid ledger cannot be written (a folder where its
        // temporary file goes): the relink lands, no warning of copied
        // beat grids, a line in the log.
        const fs::path gridsInTheWay = stick.root / "Seabass" / "beat-grids.tsv.tmp-seabass-write";
        fs::create_directories(gridsInTheWay / "in the way");
        const std::size_t logBefore2 = logText().size();
        const QVariantMap noRecord =
            moveAndRelink(b, a, {"rekordbox", "onelibrary", "engine"}, "15: a relink of a kept original with no record");
        fs::remove_all(gridsInTheWay);
        require(fs::exists(keptFile(keyOf(b))) && !fs::exists(keptFile(keyOf(a)))
                    && logText().substr(logBefore2).find("is missing, so both stay there") != std::string::npos,
                "15: an original with no record stays where it is, and the log says so");
        require(noRecord.value("warning").toString().isEmpty()
                    && logText().substr(logBefore2).find("beat grids: the ledger could not be written") != std::string::npos,
                "15: a beat grid ledger a relink cannot write is a line in the log, not a warning about copied grids: "
                    + noRecord.value("warning").toString().toStdString());
        std::cout << "case 15 (a relink lands whatever is kept under either name; the new name's stays) OK\n";
    }

    // 21. Only the kinds whose library is pointed are carried: a relink of
    //     rekordbox's rows alone carries rekordbox's kept original, not
    //     the player's.
    if (runs(21)) {
        const std::string from = "k two.wav";
        const std::string to = "k too.wav";
        exportOver(from, 31);
        analyseAgainStaged(QStringLiteral("RelinkRb"), "21: Analyse again");
        savedClean("21: Analyse again");
        auto player = plantEngineKept(from, "the player's");
        player.trackId = engineIdOf(from);
        local::StickKeptEngineAnalysisStore(stick.root).keep(player);
        moveAndRelink(from, to, {"rekordbox", "onelibrary"}, "21: a relink of rekordbox's rows alone");
        require(fs::exists(keptFile(keyOf(to))) && recordOf(keyOf(to)).status == local::KeptRekordboxRecordLookup::Status::Found
                    && !fs::exists(keptFile(keyOf(from))),
                "21: rekordbox's kept original and its record are carried");
        require(local::StickKeptEngineAnalysisStore(stick.root).find(keyOf(from)).status
                        == application::KeptEngineAnalysisLookup::Status::Found
                    && local::StickKeptEngineAnalysisStore(stick.root).find(keyOf(to)).status
                           == application::KeptEngineAnalysisLookup::Status::None,
                "21: the player's, whose library was not pointed, stays under its name");
        std::cout << "case 21 (a relink carries only the kinds whose library it points) OK\n";
    }

    // 22. A relink carries the ledgers' lines as the save left them: a
    //     change earlier in the same save records a line and an entry under
    //     the old name, and the relink after it carries those, not what the
    //     ledgers held when the save began.
    if (runs(22)) {
        const std::string from = "k three.wav";
        const std::string to = "k thre3.wav";
        const std::string fresh(64, 'c');
        local::AnalysisLedgerEntry entry = ledger().at(keyOf(from));
        entry.rekordboxGrid = fresh;
        auto note = std::make_shared<LedgerNoteChange>(
            QStringLiteral("ledger-order:before-relink"), stickRoot, [&](SaveContext &ctx, const QString &id) {
                recordGridInLedgerAfterCommit(ctx, stickRoot, id, keyOf(from), fresh);
                recordAnalysisAfterCommit(ctx, stickRoot, id, entry, "ledger-order");
            });
        const domain::MovedFileFinding finding = findingOf(from, {"rekordbox", "onelibrary", "engine"});
        fs::rename(onStick / from, onStick / to);
        domain::Track file;
        file.filePath = pathToUtf8(onStick / to);
        file.fileSizeBytes = fs::file_size(onStick / to);
        auto relink = std::make_shared<RelinkMovedFileChange>(pioneer, engine, finding, file);
        const auto saved = testing::saveChanges({note, relink}, pioneer, engine);
        invalidate();
        require(saved.error.isEmpty(), "22: the save lands: " + saved.error.toStdString());
        require(gridLedger().count(keyOf(from)) == 0 && gridLedger().count(keyOf(to)) == 1
                    && gridLedger().at(keyOf(to)) == fresh,
                "22: the beat grid line the save recorded first goes with the file");
        require(ledger().count(keyOf(from)) == 0 && ledger().count(keyOf(to)) == 1
                    && ledger().at(keyOf(to)).rekordboxGrid == fresh,
                "22: the analysis ledger's entry the save recorded first goes with the file");
        std::cout << "case 22 (a relink carries the ledgers' lines as the same save left them) OK\n";
    }

    // 16. A ledger moved aside: a track with no ledger entry and nothing
    //     kept has its analysis kept, as the analysis that was on the stick
    //     before (it is Seabass's own here, which the lost ledger knew):
    //     its record, Free Up Space and Restore say who made it is not
    //     known. The new ledger records that it is incomplete.
    if (runs(16)) {
        const local::AnalysisLedger before = ledger();
        std::string error;
        require(local::moveAnalysisLedgerAside(stick.root, std::chrono::system_clock::now(), &error).has_value(),
                "16: the ledger is moved aside: " + error);
        analyseAgainStaged(QStringLiteral("Lost"), "16: Analyse again");
        savedClean("16: Analyse again");
        const auto record = recordOf(keyOf("l one.wav"));
        require(fs::exists(keptFile(keyOf("l one.wav"))) && record.status == local::KeptRekordboxRecordLookup::Status::Found
                    && !record.record.originKnown,
                "16: kept, its origin unknown");
        std::string since;
        local::readAnalysisLedger(stick.root, &error, nullptr, &since);
        require(!since.empty(), "16: the new ledger says it is incomplete");
        bool earlier = false;
        for (const auto &group : KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8()).groups) {
            for (const auto &info : group.files) {
                earlier = earlier || (group.kind == QLatin1String("earlier") && info.trackFile.find("l one") != std::string::npos);
            }
        }
        require(earlier, "16: Free Up Space lists it as the analysis that was on the stick before");
        const QVariantMap staged = restore(QStringLiteral("Lost"));
        require(staged.value("unknownOriginTracks").toInt() == 1, "16: the restore says who made it is not known");
        const std::size_t logBefore = logText().size();
        savedClean("16: the restore");
        require(logText().substr(logBefore).find("the analysis files that were on the stick before put back")
                    != std::string::npos,
                "16: and so does its log");
        ledgerAsBefore(before);
        std::cout << "case 16 (a ledger moved aside: what is kept is the analysis that was there, its maker unknown) OK\n";
    }

    // 17. A damaged ledger moved aside and no new one written (a folder
    //     where its temporary file goes): the warning says what is true,
    //     and the next save treats the stick's ledger as incomplete. A
    //     track with nothing kept on the damaged ledger is left alone, and
    //     the summary says so and what to do.
    if (runs(17)) {
        const local::AnalysisLedger before = ledger();
        exportOver("b one.wav", 33);
        exportOver("b two.wav", 35);
        {
            // b one's original kept; b two's not.
            const fs::path two = onStick / "b two.wav";
            const fs::path aside = onStick / "b two.aside";
            fs::rename(two, aside);
            invalidate();
            importer.analyseAgain(pioneer, engine, QStringLiteral("Broken"));
            require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }),
                    "17: Analyse again ends");
            fs::rename(aside, two);
            savedClean("17: Analyse again of b one");
            invalidate();
        }
        require(fs::exists(keptFile(keyOf("b one.wav"))) && !fs::exists(keptFile(keyOf("b two.wav"))),
                "17: b one's original kept, b two's not");
        writeBytes(infrastructure::paths::stickAnalysisLedger(stick.root), "damaged");
        const fs::path inTheWay = stick.root / "Seabass" / "analysis.tsv.gz.tmp-seabass-write";
        fs::create_directories(inTheWay / "in the way");
        analyseAgainStaged(QStringLiteral("Broken"), "17: Analyse again on a damaged ledger");
        const QVariantMap summary = savedClean("17: Analyse again on a damaged ledger");
        const QString warning = summary.value("warning").toString();
        require(warning.contains(QStringLiteral("moved it aside as analysis.tsv.gz.damaged-"))
                    && warning.contains(QStringLiteral("no new record could be written")),
                "17: the warning says the damaged record was moved aside and no new one written: "
                    + warning.toStdString());
        require(summary.value("notes").toString().contains(
                    QStringLiteral("Seabass cannot tell whether its beat grid is its own, as its record of the tracks it "
                                   "analysed cannot be read"))
                    && summary.value("notes").toString().contains(
                        QStringLiteral("the command to run is in Seabass's documentation on experimental features.")),
                "17: the summary names the track left alone and what to do: "
                    + summary.value("notes").toString().toStdString());
        require(!fs::exists(keptFile(keyOf("b two.wav"))), "17: b two is left alone");
        fs::remove_all(inTheWay);
        std::string error;
        std::string since;
        require(local::readAnalysisLedger(stick.root, &error, nullptr, &since).empty() && error.empty() && !since.empty(),
                "17: no ledger beside the damaged one moved aside is incomplete");
        analyseAgainStaged(QStringLiteral("Broken"), "17: Analyse again on no ledger");
        savedClean("17: Analyse again on no ledger");
        const auto record = recordOf(keyOf("b two.wav"));
        require(record.status == local::KeptRekordboxRecordLookup::Status::Found && !record.record.originKnown,
                "17: then b two's analysis is kept, its origin unknown");
        // A ledger that cannot be read (a folder where it belongs): an
        // import lands, and the warning points to where the command is.
        {
            const fs::path ledgerFile = infrastructure::paths::stickAnalysisLedger(stick.root);
            fs::remove(ledgerFile);
            fs::create_directories(ledgerFile / "in the way");
            writeBeatWav(computer / "b three.wav", 91, 120.0);
            require(importer.importFiles(pioneer, engine, QVariantList{pathToQString(computer / "b three.wav")},
                                         QStringLiteral("Broken Import"), true),
                    "17: the import starts");
            require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }),
                    "17: the import ends");
            const QString unreadable = savedClean("17: an import on an unreadable ledger").value("warning").toString();
            require(unreadable.contains(QStringLiteral("Seabass's command line tool, seabass-cli, can move the record "
                                                       "aside"))
                        && unreadable.contains(QStringLiteral(
                            "the command to run is in Seabass's documentation on experimental features.")),
                    "17: the warning for an unreadable ledger says where the command is: " + unreadable.toStdString());
            fs::remove_all(ledgerFile);
        }
        ledgerAsBefore(before);
        std::cout << "case 17 (a damaged ledger moved aside and not written again: true words, and incomplete) OK\n";
    }

    // 18. A kept original whose record is missing: Analyse again leaves the
    //     track alone and the summary says why and what to do; Restore
    //     leaves it out, saying why, and refuses it at the save.
    if (runs(18)) {
        exportOver("n one.wav", 37);
        analyseAgainStaged(QStringLiteral("NoRecord"), "18: Analyse again");
        savedClean("18: Analyse again");
        const std::string record = bytesOf(recordFile(keyOf("n one.wav")));
        // Staged with the record there, the record gone by the save.
        require(restore(QStringLiteral("NoRecord")).value("staged").toInt() == 1, "18: the restore is staged");
        fs::remove(recordFile(keyOf("n one.wav")));
        const std::string datBefore = bytesOf(anlzOf("n one.wav", "DAT"));
        std::size_t logBefore = logText().size();
        savedClean("18: the restore with no record");
        require(logText().substr(logBefore).find("Seabass cannot tell whether the beat grid on the stick is its own") != std::string::npos
                    && bytesOf(anlzOf("n one.wav", "DAT")) == datBefore,
                "18: Restore refuses at the save, saying why");
        const QVariantMap staged = restore(QStringLiteral("NoRecord"));
        require(staged.value("staged").toInt() == 0 && staged.value("leftAlone").toList().size() == 1
                    && staged.value("leftAlone").toList()[0].toMap().value("why").toString().contains(
                           QStringLiteral("record of the grids Seabass wrote")),
                "18: Restore leaves it out, saying why");
        analyseAgainStaged(QStringLiteral("NoRecord"), "18: Analyse again with no record");
        logBefore = logText().size();
        const QVariantMap summary = savedClean("18: Analyse again with no record");
        require(bytesOf(anlzOf("n one.wav", "DAT")) == datBefore
                    && logText().substr(logBefore).find("Seabass cannot tell whether its beat grid is its own") != std::string::npos,
                "18: Analyse again leaves it alone");
        require(summary.value("notes").toString().contains(QStringLiteral("Remove the kept original in Free Up Space")),
                "18: the summary says what to do: " + summary.value("notes").toString().toStdString());
        writeBytes(recordFile(keyOf("n one.wav")), record);
        std::cout << "case 18 (a kept original with no record: left alone and refused, with true reasons) OK\n";
    }

    // 19. The player's analysis kept by Copy Beat Grids, then Analyse again
    //     (Engine takes Seabass's grid and its cues move with it) and
    //     Restore: rekordbox's grid and cues go back; Engine keeps Seabass's
    //     grid and the cues on it, its kept analysis is left for Beat Grids
    //     & Waveforms, and the summary says which library went back, which
    //     stayed, and where.
    if (runs(19)) {
        auto copied = plantEngineKept("e two.wav", "kept by Copy Beat Grids");
        copied.trackId = engineIdOf("e two.wav");
        local::StickKeptEngineAnalysisStore(stick.root).keep(copied);
        const fs::path file = local::keptEngineAnalysisFile(stick.root, copied.pathKey);
        const std::string copiedBytes = bytesOf(file);
        const auto [one, two] = prepareTrack("e two.wav", QStringLiteral("EngineCopied"), true, true);
        require(recordOf(keyOf("e two.wav")).record.engineKept.empty(), "19: the record says Analyse again kept no Engine analysis");
        const std::string engineGrid = ledger().at(keyOf("e two.wav")).engineGrid;
        require(!engineGrid.empty(), "19: Engine holds Seabass's grid after Analyse again");
        const double engineMoved1 = engineHotCue("e two.wav", 1);
        const double engineMoved2 = engineHotCue("e two.wav", 2);
        require(std::abs(engineMoved1 - one) >= 3.0 && std::abs(engineMoved1 - hotCue("e two.wav", 1)) < 1.0
                    && std::abs(engineMoved2 - hotCue("e two.wav", 2)) < 1.0,
                "19: Engine's cues moved with Seabass's grid, as rekordbox's did");
        require(restore(QStringLiteral("EngineCopied")).value("staged").toInt() == 1, "19: the restore is staged");
        restorer.setMoveCuesBackForAll(true);
        const std::size_t logBefore = logText().size();
        const QVariantMap summary = savedClean("19: the restore");
        invalidate();
        const QString expected = QStringLiteral(
            "On one restored track, rekordbox's libraries have the kept analysis back and Engine keeps the grid it "
            "has: the player's analysis that Copy Beat Grids kept goes back on the Beat Grids & Waveforms page, with "
            "Restore Engine's Analysis. The cue points Analyse again moved in Engine do not follow that restore: the "
            "record of their moves is not kept for it. In rekordbox's libraries, the cue points Analyse again moved "
            "that were still where it put them went back with the grid. In Engine, the cue points Analyse again moved "
            "stay where they are, on the grid Engine keeps.");
        require(summary.value("notes").toString().contains(expected),
                "19: the summary says which library went back, which stayed, where the cue points are, and that "
                "Engine's do not follow Restore Engine's Analysis: "
                    + summary.value("notes").toString().toStdString());
        require(logText().substr(logBefore).find("e two") != std::string::npos
                    && logText().substr(logBefore).find("for the summary: " + expected.mid(23).toStdString())
                           != std::string::npos,
                "19: the log names the track with its words");
        require(gridOf("e two.wav") != seabassGrid["e two.wav"], "19: rekordbox's grid went back");
        require(std::abs(hotCue("e two.wav", 1) - one) < 1.0 && std::abs(hotCue("e two.wav", 2) - two) < 1.0
                    && std::abs(oneLibraryHotCue("e two.wav", 1) - one) < 1.0
                    && std::abs(oneLibraryHotCue("e two.wav", 2) - two) < 1.0,
                "19: rekordbox's cues, in both its libraries, back where they were");
        require(std::abs(engineHotCue("e two.wav", 1) - engineMoved1) < 1.0
                    && std::abs(engineHotCue("e two.wav", 2) - engineMoved2) < 1.0,
                "19: Engine's cues stay on the grid Engine keeps");
        require(ledger().count(keyOf("e two.wav")) == 1 && ledger().at(keyOf("e two.wav")).engineGrid == engineGrid,
                "19: Engine keeps Seabass's grid, as the ledger says");
        require(bytesOf(file) == copiedBytes && gridLedger().count(keyOf("e two.wav")) == 1,
                "19: the player's kept analysis and its beat grid line stay as they were, for Restore Engine's Analysis");
        fs::remove(file);
        std::cout << "case 19 (the player's analysis Copy Beat Grids kept is not Restore's; each library's cues follow "
                     "its own grid) OK\n";
    }

    // 25. What the summary says of the cue points is what the save did,
    //     one note per outcome, and a track whose cue points were to go
    //     back and cannot is refused. (a) Three tracks, their cue points
    //     moved by Analyse again in rekordbox's libraries and Engine (which
    //     keeps Seabass's grid: nothing of the player's was kept), the move
    //     back switched on; then the DJ moves the hot cues in rekordbox's
    //     libraries, on one track in Engine too: no cue goes back anywhere,
    //     rekordbox's grid does, and the summary has two notes, one for two
    //     tracks. (b) OneLibrary's row with an analysis file of its own:
    //     its cue points made other than DeviceLibrary's after the restore
    //     was staged with the switch on, the track is refused, nothing
    //     written, what is kept kept; restored again, the review shows why
    //     and keeps the switch off, and the save puts the grid back and says
    //     the cue points no longer sit on it.
    if (runs(25)) {
        const auto movedBy = [](std::vector<domain::CuePoint> cues, double ms) {
            for (auto &cue : cues) {
                if (cue.kind == domain::CuePoint::Kind::Hot && !cue.isLoop
                    && (cue.hotCueNumber == 1 || cue.hotCueNumber == 2)) {
                    cue.positionMs += ms;
                }
            }
            return cues;
        };
        // The DJ's edit in rekordbox, exported over the stick: the hot cues
        // of the track's OneLibrary row and the analysis files it names.
        const auto rekordboxEdit = [&](const std::string &name, const std::vector<domain::CuePoint> &cues) {
            infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(stick.pioneer))
                .writeCuesForPath(pathToUtf8(onStick / name), cues);
            invalidate();
        };
        // (a)
        {
            const std::vector<std::string> names{"u seven.wav", "u eight.wav", "u nine.wav"};
            std::map<std::string, std::pair<double, double>> before;
            for (const std::string &name : names) {
                exportOver(name, 4);
                const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), pdbRow(name).sourceId);
                require(grid.size() > 20, "25: " + name + " has rekordbox's grid");
                before[name] = {std::round(grid[8].timeMs), std::round(grid[16].timeMs)};
                for (const QString format : {QStringLiteral("rekordbox"), QStringLiteral("engine")}) {
                    setCueIn(format, name, 1, before[name].first);
                    setCueIn(format, name, 2, before[name].second);
                }
            }
            analyseAgainStaged(QStringLiteral("CueMoved"), "25: Analyse again of three tracks");
            importer.setMoveCuesForAll(true);
            savedClean("25: Analyse again of three tracks");
            invalidate();
            for (const std::string &name : names) {
                require(keptOf(keyOf(name)).entry.cueMoves.size() == 4,
                        "25: " + name + ": the kept entry records two moves in each library");
                require(std::abs(hotCue(name, 1) - before[name].first) >= 3.0
                            && std::abs(engineHotCue(name, 1) - hotCue(name, 1)) < 1.0,
                        "25: " + name + ": its cues moved with Seabass's grid in both");
            }
            // Engine's of u eight edited before the review.
            const double eightEngine1 = engineHotCue("u eight.wav", 1) + 600.0;
            const double eightEngine2 = engineHotCue("u eight.wav", 2) + 600.0;
            setCueIn(QStringLiteral("engine"), "u eight.wav", 1, eightEngine1);
            setCueIn(QStringLiteral("engine"), "u eight.wav", 2, eightEngine2);
            const QVariantMap staged = restore(QStringLiteral("CueMoved"));
            require(staged.value("staged").toInt() == 3 && staged.value("cueTracks").toInt() == 3,
                    "25: three restores staged, the move back offered for each");
            restorer.setMoveCuesBackForAll(true);
            // The DJ moves the hot cues in rekordbox after the review.
            std::map<std::string, std::vector<domain::CuePoint>> rekordboxNow;
            std::map<std::string, std::vector<domain::CuePoint>> engineNow;
            for (const std::string &name : names) {
                rekordboxNow[name] = movedBy(pdbRow(name).cues, 500.0);
                rekordboxEdit(name, rekordboxNow[name]);
                require(std::abs(hotCue(name, 1) - hotIn(rekordboxNow[name], 1)) < 1.0
                            && std::abs(oneLibraryHotCue(name, 1) - hotIn(rekordboxNow[name], 1)) < 1.0,
                        "25: " + name + ": the DJ's hot cues are in both of rekordbox's libraries");
                engineNow[name] = engineRowOf(name).cues;
            }
            const QVariantMap summary = savedClean("25: the restore of three tracks");
            invalidate();
            const QString notes = summary.value("notes").toString();
            for (const std::string &name : names) {
                require(gridOf(name) != seabassGrid[name], "25: " + name + ": rekordbox's grid went back");
                require(ledger().count(keyOf(name)) == 1 && !ledger().at(keyOf(name)).engineGrid.empty(),
                        "25: " + name + ": Engine keeps Seabass's grid");
                for (int pad = 1; pad <= 2; ++pad) {
                    require(std::abs(hotCue(name, pad) - hotIn(rekordboxNow[name], pad)) < 1.0
                                && std::abs(oneLibraryHotCue(name, pad) - hotIn(rekordboxNow[name], pad)) < 1.0,
                            "25: " + name + ": the DJ's hot cue " + std::to_string(pad)
                                + " stays in both of rekordbox's libraries");
                    require(std::abs(engineHotCue(name, pad) - hotIn(engineNow[name], pad)) < 1.0,
                            "25: " + name + ": Engine's hot cue " + std::to_string(pad) + " stays where it was");
                }
                require(!fs::exists(keptFile(keyOf(name))), "25: " + name + ": the kept original has done its work");
            }
            require(std::abs(engineHotCue("u eight.wav", 1) - eightEngine1) < 1.0,
                    "25: u eight: Engine's edited cue stays where the DJ put it");
            const QString head = QStringLiteral(
                "rekordbox's libraries have the kept analysis back and Engine keeps the grid it has, as nothing of the "
                "player's analysis was kept. In rekordbox's libraries, none of the cue points Analyse again moved is "
                "where it put them any more, so none went back.");
            require(notes.contains(QStringLiteral("On 2 restored tracks, ") + head
                                   + QStringLiteral(" In Engine, the cue points Analyse again moved stay where they "
                                                    "are, on the grid Engine keeps.")),
                    "25: one note for the two tracks whose Engine cues stayed on Engine's grid: " + notes.toStdString());
            require(notes.contains(QStringLiteral("On one restored track, ") + head
                                   + QStringLiteral(" In Engine, none of the cue points Analyse again moved is where "
                                                    "it put them any more, so none went back.")),
                    "25: one note for the track whose Engine cues the DJ edited too: " + notes.toStdString());
            require(!notes.contains(QStringLiteral("went back with the grid")),
                    "25: no note says a cue point went back: " + notes.toStdString());
            require(notes.count(QStringLiteral("restored track")) == 2, "25: two notes, not one per track: "
                                                                            + notes.toStdString());
        }
        // (b)
        {
            const std::string name = "v one.wav";
            exportOver(name, 4);
            const auto [bPath, b] = copyAnalysisOf(name, "00000025");
            pointOneLibraryOf(name, bPath);
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), pdbRow(name).sourceId);
            require(grid.size() > 20, "25: v one has rekordbox's grid");
            const std::pair<double, double> onBeats{std::round(grid[8].timeMs), std::round(grid[16].timeMs)};
            setCue(name, 1, onBeats.first);
            setCue(name, 2, onBeats.second);
            require(std::abs(oneLibraryHotCue(name, 1) - onBeats.first) < 1.0, "25: v one: OneLibrary has the cues");
            analyseAgainStaged(QStringLiteral("CueSplit"), "25: Analyse again of v one");
            importer.setMoveCuesForAll(true);
            savedClean("25: Analyse again of v one");
            invalidate();
            require(keptOf(keyOf(name)).entry.files.size() == 2 && !keptOf(keyOf(name)).entry.cueMoves.empty(),
                    "25: v one: both analysis files kept, the cue moves recorded");
            require(std::abs(hotCue(name, 1) - onBeats.first) >= 3.0
                        && std::abs(oneLibraryHotCue(name, 1) - hotCue(name, 1)) < 1.0,
                    "25: v one: its cues moved with the grid, in both of rekordbox's libraries");
            const fs::path a = anlzOf(name, "DAT");
            // (b1) Staged with the switch on, then OneLibrary's cue points
            // made other than DeviceLibrary's: refused, nothing written.
            {
                const QVariantMap staged = restore(QStringLiteral("CueSplit"));
                require(staged.value("staged").toInt() == 1 && staged.value("cuesCannotMoveTracks").toInt() == 0
                            && restorer.cueReview().size() == 1,
                        "25: v one: staged, the move back offered");
                restorer.setMoveCuesBackForAll(true);
                rekordboxEdit(name, movedBy(rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)),
                                                  name)
                                                .cues,
                                            300.0));
                require(std::abs(oneLibraryHotCue(name, 1) - hotCue(name, 1)) >= 3.0,
                        "25: v one: OneLibrary's cue points are not DeviceLibrary's");
                const std::string keptBytes = bytesOf(keptFile(keyOf(name)));
                const std::string aBytes = bytesOf(a);
                const std::string bBytes = bytesOf(b);
                const std::string aExt = bytesOf(anlzOf(name, "EXT"));
                const std::string pdbBytes = bytesOf(pdbFile);
                const QVariantMap summary = savedClean("25: v one, the switch on and the cues cannot go back");
                invalidate();
                require(bytesOf(a) == aBytes && bytesOf(b) == bBytes && bytesOf(anlzOf(name, "EXT")) == aExt
                            && bytesOf(pdbFile) == pdbBytes,
                        "25: v one: refused, nothing written (grids, cues, export.pdb)");
                require(bytesOf(keptFile(keyOf(name))) == keptBytes, "25: v one: what is kept stays, byte for byte");
                const QString notes = summary.value("notes").toString();
                require(notes.contains(QStringLiteral(
                            "One track kept the analysis it has: its cue points were to move back with the grid, and "
                            "they cannot, as DeviceLibrary and Device Library Plus hold different cue points. To give "
                            "it its original analysis back without moving its cue points, restore it again and leave "
                            "its cue point switch off.")),
                        "25: v one: the summary says why and what to do: " + notes.toStdString());
                require(!notes.contains(QStringLiteral("changed since Seabass analysed")),
                        "25: v one: not counted under the other refusals: " + notes.toStdString());
            }
            // (b2) Restored again: the review knows, the switch stays off;
            // the save puts the grid back and says where the cues are.
            {
                const QVariantMap staged = restore(QStringLiteral("CueSplit"));
                const QVariantList review = restorer.cueReview();
                require(staged.value("staged").toInt() == 1 && staged.value("cuesCannotMoveTracks").toInt() == 1
                            && staged.value("askedCueTracks").toInt() == 0 && review.size() == 1
                            && review[0].toMap().value("cannotMove").toString()
                                   == QStringLiteral("DeviceLibrary and Device Library Plus hold different cue points")
                            && !review[0].toMap().value("moveCues").toBool(),
                        "25: v one: the review says why its cue points cannot go back, the switch off");
                restorer.setMoveCuesBackForAll(true);
                restorer.setMoveCuesBack(review[0].toMap().value("changeId").toString(), true);
                require(!restorer.cueReview()[0].toMap().value("moveCues").toBool(),
                        "25: v one: its switch cannot be turned on");
                const double deviceLibrary1 = hotCue(name, 1);
                const double oneLibrary1 = oneLibraryHotCue(name, 1);
                const QVariantMap summary = savedClean("25: v one, restored with the switch off");
                invalidate();
                require(gridOf(name) != seabassGrid[name]
                            && infrastructure::rekordbox::pqtzGridFingerprint(AnlzFile::readRaw(pathToUtf8(b)))
                                   != seabassGrid[name],
                        "25: v one: rekordbox's grid went back in both files");
                require(std::abs(hotCue(name, 1) - deviceLibrary1) < 1.0
                            && std::abs(oneLibraryHotCue(name, 1) - oneLibrary1) < 1.0,
                        "25: v one: the cue points stay where they are in both libraries");
                require(!fs::exists(keptFile(keyOf(name))), "25: v one: the kept original has done its work");
                const QString notes = summary.value("notes").toString();
                require(notes.contains(QStringLiteral(
                            "On one restored track, rekordbox's libraries have the kept analysis back and Engine keeps "
                            "the grid it has, as nothing of the player's analysis was kept. In rekordbox's libraries, "
                            "the cue points Analyse again moved were not moved back: they stay where they are and no "
                            "longer sit on the restored grid.")),
                        "25: v one: the summary says the cue points no longer sit on the restored grid: "
                            + notes.toStdString());
            }
            pointOneLibraryOf(name, pdbRow(name).analysisFile);
        }
        std::cout << "case 25 (the summary says what the save did to the cue points, one note per outcome; a track "
                     "whose cue points were to go back and cannot is refused) OK\n";
    }

    // 20. The playlist menu: warmed when Browse loads the stick, it knows
    //     the count right after a save that kept or put back an original,
    //     with no new count asked for.
    if (runs(20)) {
        AnalysisRestoreRequest warm;
        warm.rekordboxPath = pioneer;
        warm.enginePath = engine;
        warm.stickId = registry->libraryIdForPath(pioneer).toStdString();
        warmKeptCounts(warm, application::CancellationToken());
        require(restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Menu")) == 0, "20: warmed, nothing kept");
        exportOver("w one.wav", 41);
        analyseAgainStaged(QStringLiteral("Menu"), "20: Analyse again");
        savedClean("20: Analyse again");
        // As the controller does after every save: with something kept,
        // the catalogs are read once, at their first stage.
        const int readsBefore = keptCountCatalogReadsForTesting();
        warmKeptCounts(warm, application::CancellationToken());
        require(keptCountCatalogReadsForTesting() == readsBefore + 1, "20: with something kept the warm reads once");
        require(restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Menu")) == 1,
                "20: right after the save the menu knows the original kept");
        require(restore(QStringLiteral("Menu")).value("staged").toInt() == 1, "20: the restore is staged");
        savedClean("20: the restore");
        require(restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Menu")) == 0,
                "20: right after the restore it knows none is kept");
        std::cout << "case 20 (the playlist menu's count is right at the first opening after a save) OK\n";
    }

    // 23. Analyse again decides on every file it writes. OneLibrary's row
    //     names an analysis file of its own (B, beside DeviceLibrary's A),
    //     each with a grid of rekordbox's: both are kept. Then B's grid is
    //     changed outside Seabass while A holds Seabass's: the track is left
    //     alone, B untouched, and Restore leaves it out. Then OneLibrary's
    //     row names a third file (C, a re-export): left alone again, since
    //     the kept original would have no way back from C.
    if (runs(23)) {
        const std::string name = "t one.wav";
        const std::string key = keyOf(name);
        exportOver(name, 43);
        const fs::path a = anlzOf(name, "DAT");
        const auto copyAnalysis = [&](const std::string &folder) { return copyAnalysisOf(name, folder); };
        const auto pointOneLibraryAt = [&](const std::string &dat) { pointOneLibraryOf(name, dat); };
        const auto shiftFile = [&](const fs::path &dat, int ms) {
            AnlzFile file = AnlzFile::readRaw(pathToUtf8(dat));
            shiftGrid(file, ms);
            writeBytes(dat, file.toBytes());
            invalidate();
            return infrastructure::rekordbox::pqtzGridFingerprint(AnlzFile::readRaw(pathToUtf8(dat)));
        };
        const auto [bPath, b] = copyAnalysis("00000002");
        const std::string bTheirs = shiftFile(b, 7);
        pointOneLibraryAt(bPath);
        analyseAgainStaged(QStringLiteral("TwoFiles"), "23: Analyse again over two files");
        savedClean("23: Analyse again over two files");
        invalidate();
        const auto kept = keptOf(key);
        require(kept.status == local::KeptRekordboxAnalysisLookup::Status::Found && kept.entry.files.size() == 2,
                "23: both analysis files are kept");
        const std::string seabassNow = infrastructure::rekordbox::pqtzGridFingerprint(AnlzFile::readRaw(pathToUtf8(a)));
        require(infrastructure::rekordbox::pqtzGridFingerprint(AnlzFile::readRaw(pathToUtf8(b))) == seabassNow
                    && bTheirs != seabassNow,
                "23: both hold Seabass's grid");
        const std::string bSeabass = bytesOf(b);
        // B changed outside Seabass; A still Seabass's.
        shiftFile(b, 5);
        const std::string bChanged = bytesOf(b);
        const std::string aBefore = bytesOf(a);
        analyseAgainStaged(QStringLiteral("TwoFiles"), "23: Analyse again, B changed outside");
        QVariantMap summary = savedClean("23: Analyse again, B changed outside");
        invalidate();
        require(bytesOf(b) == bChanged && bytesOf(a) == aBefore,
                "23: a grid changed outside Seabass in OneLibrary's file leaves the whole track alone");
        require(summary.value("notes").toString().contains(QStringLiteral("its analysis was changed outside Seabass")),
                "23: and the summary says so: " + summary.value("notes").toString().toStdString());
        const QVariantMap staged = restore(QStringLiteral("TwoFiles"));
        require(staged.value("staged").toInt() == 0 && staged.value("leftAlone").toList().size() == 1
                    && staged.value("leftAlone").toList()[0].toMap().value("why").toString().contains(
                        QString::fromStdString(bPath)),
                "23: Restore leaves it out, naming the file changed outside Seabass");
        // B back to Seabass's grid, and OneLibrary's row naming C now.
        writeBytes(b, bSeabass);
        // C: B's grid (Seabass's) with waveforms of its own, so a write
        // shows in its bytes.
        const auto [cPath, c] = copyAnalysis("00000003");
        {
            AnlzFile file = AnlzFile::readRaw(pathToUtf8(c));
            touchWaveforms(file);
            writeBytes(c, file.toBytes());
        }
        pointOneLibraryAt(cPath);
        const std::string cBefore = bytesOf(c);
        analyseAgainStaged(QStringLiteral("TwoFiles"), "23: Analyse again, OneLibrary names another file");
        summary = savedClean("23: Analyse again, OneLibrary names another file");
        invalidate();
        require(bytesOf(c) == cBefore && bytesOf(a) == aBefore,
                "23: rows naming files the original was not kept from: nothing written");
        require(summary.value("notes").toString().contains(QStringLiteral(
                    "its rows name other analysis files than the ones Seabass kept its original from")),
                "23: and the summary says why: " + summary.value("notes").toString().toStdString());
        // Restore leaves it out before it is staged, with the true reason.
        {
            const QVariantMap left = restore(QStringLiteral("TwoFiles"));
            const QVariantList leftAlone = left.value("leftAlone").toList();
            const QString why = leftAlone.isEmpty() ? QString() : leftAlone[0].toMap().value("why").toString();
            require(left.value("staged").toInt() == 0 && leftAlone.size() == 1
                        && why.contains(QStringLiteral("its rows name other analysis files ("))
                        && why.contains(QString::fromStdString(cPath))
                        && why.contains(QStringLiteral("than the ones its original was kept from, so the kept original "
                                                       "cannot be put back; it can only be removed in Free Up Space")),
                    "23: Restore leaves out rows that name other files than the kept ones, saying so: "
                        + why.toStdString());
        }
        pointOneLibraryAt(bPath);
        std::cout << "case 23 (Analyse again decides on every file it writes, and only on the files kept) OK\n";
    }

    // 24. The released relink. (1) The record cannot be written under the
    //     new name (a folder where its temporary file goes): the entry written there is taken
    //     away again, the old pair stays, the relink lands. (2) A relink on a
    //     stick whose analysis ledger is damaged and whose beat grid ledger
    //     cannot be read: both stay exactly as they are, nothing is moved
    //     aside, no warning, a line in the log.
    if (runs(24)) {
        const std::string from = "k four.wav";
        const std::string to = "k f0ur.wav";
        exportOver(from, 45);
        analyseAgainStaged(QStringLiteral("RelinkHalf"), "24: Analyse again");
        savedClean("24: Analyse again");
        const std::string keptBytes = bytesOf(keptFile(keyOf(from)));
        const std::string recordBytes = bytesOf(recordFile(keyOf(from)));
        // A folder where the record's temporary file goes.
        fs::path inTheWay = recordFile(keyOf(to));
        inTheWay += ".tmp-seabass-write";
        fs::create_directories(inTheWay / "in the way");
        std::size_t logBefore = logText().size();
        QVariantMap summary = moveAndRelink(from, to, {"rekordbox", "onelibrary", "engine"}, "24: the relink");
        fs::remove_all(inTheWay);
        require(pdbRow(to).filePath.find(to) != std::string::npos, "24: the relink lands");
        require(!fs::exists(keptFile(keyOf(to))), "24: the entry written under the new name is taken away again");
        require(bytesOf(keptFile(keyOf(from))) == keptBytes && bytesOf(recordFile(keyOf(from))) == recordBytes,
                "24: the old pair stays as it was");
        require(logText().substr(logBefore).find("rekordbox's kept analysis could not be carried and stays under the old "
                                                 "name")
                    != std::string::npos,
                "24: the log says so");
        // (2): both ledgers that cannot be believed, and a relink back.
        const local::AnalysisLedger before = ledger();
        const fs::path analysisLedger = infrastructure::paths::stickAnalysisLedger(stick.root);
        const fs::path gridLedgerFile = pathFromUtf8(beatGridLedgerFileOf(stickRoot));
        const std::string gridLedgerBytes = bytesOf(gridLedgerFile);
        writeBytes(analysisLedger, "damaged");
        writeBytes(gridLedgerFile, "unreadable");
        logBefore = logText().size();
        summary = moveAndRelink(to, from, {"rekordbox", "onelibrary", "engine"}, "24: the relink back");
        require(bytesOf(analysisLedger) == "damaged" && bytesOf(gridLedgerFile) == "unreadable",
                "24: a rename-only save leaves both ledgers exactly as they are");
        bool movedAside = false;
        for (const auto &entry : fs::directory_iterator(stick.root / "Seabass")) {
            movedAside = movedAside || pathToUtf8(entry.path().filename()).rfind("analysis.tsv.gz.", 0) == 0;
        }
        require(!movedAside, "24: nothing is moved aside");
        require(summary.value("warning").toString().isEmpty(),
                "24: no warning: " + summary.value("warning").toString().toStdString());
        const std::string logged = logText().substr(logBefore);
        require(logged.find("analysis ledger: damaged") != std::string::npos
                    && logged.find("so its entries were not moved and it is left as it is") != std::string::npos
                    && logged.find("beat grids: the ledger cannot be read") != std::string::npos
                    && logged.find("track(s) not recorded") == std::string::npos,
                "24: a line in the log for each, and no track counted as analysed");
        writeBytes(gridLedgerFile, gridLedgerBytes);
        fs::remove(analysisLedger);
        ledgerAsBefore(before);
        std::cout << "case 24 (a half-carried relink is taken back; a rename-only save never replaces a ledger it "
                     "cannot believe) OK\n";
    }

    // 12. Free Up Space: unticked; the lock. Last: it takes the stick's
    //     lock away from the session.
    if (runs(12)) {
        exportOver("s one.wav", 25);
        analyseAgainStaged(QStringLiteral("Seq1"), "Analyse again of " + std::string("Seq1"));
        savedClean("12: Analyse again");
        local::KeptRekordboxAnalysis gone = keptOf(keyOf("s one.wav")).entry;
        gone.pathKey = "contents/unknownartist/unknownalbum/not here.wav";
        gone.stickPath = "/Contents/UnknownArtist/UnknownAlbum/not here.wav";
        local::StickKeptRekordboxAnalysisStore(stick.root).keep(gone, plainRecord(gone.pathKey));
        KeptAnalysisSpaceController space;
        space.refresh(pioneer, engine);
        require(waitFor(&space, &KeptAnalysisSpaceController::busyChanged, [&] { return !space.busy(); }), "12: listed");
        bool sawGone = false;
        for (const QVariant &g : space.groups()) {
            sawGone = sawGone || g.toMap().value("gone").toBool();
            require(!g.toMap().value("included").toBool(), "12: every group starts unticked");
        }
        require(sawGone && space.includedCount() == 0, "12: a group of a track not on the stick, unticked");
        for (int i = 0; i < space.groups().size(); ++i) {
            space.setIncluded(i, true);
        }
        // Another instance holds the stick's lock.
        const QString libraryId = registry->libraryIdForPath(engine);
        application::LibraryEditLock other =
            registry->lockTemplate(libraryId, QStringLiteral("TEST"), pathToQString(stick.root));
        other.instanceId = "another-seabass";
        other.pid = static_cast<std::int64_t>(::getpid());
        registry->lockStore().forceRemove(libraryId.toStdString());
        require(registry->lockStore().tryAcquire(other), "12: the other instance takes the lock");
        QSignalSpy refused(&space, &KeptAnalysisSpaceController::lockRefused);
        const auto filesBefore = KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8()).groups.size();
        space.removeIncluded();
        require(refused.count() == 1 && refused.at(0).at(1).toString() == libraryId,
                "12: the held lock is reported for the library the removal asked it of");
        require(!space.removing() && fs::exists(keptFile(gone.pathKey))
                    && KeptAnalysisSpaceController::listKept(stickRoot, stick.engineUtf8()).groups.size() == filesBefore,
                "12: nothing removed");
        registry->lockStore().forceRemove(libraryId.toStdString());
        std::cout << "case 12 (Free Up Space: every group unticked; a held lock reported for its library) OK\n";
    }

    std::cout << "kept_analysis_rules_test: all cases passed\n";
    return 0;
}
