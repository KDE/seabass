// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A track import's export.pdb rows through the save loop, on a copy of the
// anonymized fixture's rekordbox side (1,000 tracks and more) as a stick
// with that one library:
// 1. a save that imports tracks into a new playlist and puts some of them
//    at its top, staged as the import stages it, reads export.pdb once and
//    writes it once (WorkCounters), and every row and entry is there in
//    the order asked;
// 2. a change that opens export.pdb itself, between two imports, sees the
//    rows of the import before it, and the import after it sees its edit:
//    the held rows are written before it and read again after;
// 3. a change that fails after some imports takes the save back: the file
//    is its own bytes again and was never written;
// 4. a file replaced on the computer after its analysis is imported as
//    one nobody analysed: its row names no analysis file, no analysis
//    file is written, its tempo and key are its tags', and the log says
//    why. The same file not replaced gets all of it.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "domain/imported_track.hpp"
#include "gui/edit/changes/analysis_ledger_record.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/work_counters.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

const QStringList RekordboxOnly{QStringLiteral("rekordbox")};

// A stick with the fixture's rekordbox library and nothing else.
testing::EngineChangeStick rekordboxStick(const fs::path &fixture, const std::string &name)
{
    testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, name);
    fs::remove(stick.pioneer / "rekordbox" / "exportLibrary.db");
    fs::remove_all(stick.engine);
    return stick;
}

// A file on "the computer": `bytes` long, its own bytes.
fs::path makeSource(const fs::path &folder, const std::string &name, int seed, int bytes = 4096)
{
    fs::create_directories(folder);
    const fs::path file = folder / name;
    std::string content;
    for (int i = 0; i < bytes; ++i) {
        content.push_back(static_cast<char>((i * 31 + seed * 7 + i / 251) & 0xFF));
    }
    std::ofstream(file, std::ios::binary) << content;
    return file;
}

domain::ImportedTrack describe(const std::string &stickPath, int n, const std::string &key = "Am")
{
    domain::TrackTags tags;
    tags.title = "Held Row " + std::to_string(n);
    tags.artist = n % 3 == 0 ? std::string("Hand Built") : "Held Artist " + std::to_string(n % 17);
    tags.album = "Held Album " + std::to_string(n % 11);
    tags.genre = "Techno";
    tags.label = "A Label";
    tags.key = key;
    tags.bpm = 126.5;
    tags.year = 2024;
    tags.trackNumber = n % 20;
    domain::AudioFormat audio;
    audio.durationSeconds = 10.0;
    audio.bitrate = 1411;
    audio.sampleRateHz = 44100;
    audio.bitsPerSample = 16;
    return domain::describeImportedTrack(tags, audio, stickPath, "2026-11-26");
}

std::string stickPathOf(int n)
{
    return "/Contents/Held Rows/" + std::to_string(n) + " Held Row.flac";
}

struct PdbRow
{
    std::uint32_t id = 0;
    std::string analyzePath;
    std::uint32_t tempo = 0;
    std::uint32_t keyId = 0;
};

// The present track row whose file_path is `path`, read with the app's
// parser.
std::optional<PdbRow> pdbRowOf(const fs::path &pdb, const std::string &path)
{
    const std::string bytes = testing::fileBytes(pdb);
    std::istringstream in(bytes);
    kaitai::kstream ks(&in);
    rekordbox_pdb_t file(false, &ks);
    std::optional<PdbRow> found;
    for (const auto &table : *file.tables()) {
        if (table->type() != rekordbox_pdb_t::PAGE_TYPE_TRACKS) {
            continue;
        }
        infrastructure::rekordbox::forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    auto *t = row->present() ? dynamic_cast<rekordbox_pdb_t::track_row_t *>(row->body()) : nullptr;
                    if (t && infrastructure::rekordbox::sqlText(t->file_path()) == path) {
                        found = PdbRow{t->id(), infrastructure::rekordbox::sqlText(t->analyze_path()), t->tempo(), t->key_id()};
                    }
                }
            }
        });
    }
    return found;
}

std::vector<domain::Track> libraryOf(const testing::EngineChangeStick &stick)
{
    return infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
}

// The tracks of playlist `name` in its order, by file name.
std::vector<std::string> playlistOrder(const std::vector<domain::Track> &tracks, const std::string &name)
{
    std::vector<std::pair<int, std::string>> at;
    for (const auto &t : tracks) {
        for (const auto &p : t.playlists) {
            if (p.name == name) {
                at.emplace_back(p.position, pathToUtf8(pathFromUtf8(t.filePath).filename()));
            }
        }
    }
    std::sort(at.begin(), at.end());
    std::vector<std::string> order;
    for (const auto &[position, file] : at) {
        order.push_back(file);
    }
    return order;
}

// A change that opens export.pdb itself, as most changes other than the
// import's do: it must find the row of the file named, and gives it a
// rating.
class RateOnItsOwn : public PendingChange
{
public:
    RateOnItsOwn(QString pioneer, std::string stickPath)
        : m_pioneer(std::move(pioneer)), m_stickPath(std::move(stickPath))
    {
    }
    QString id() const override { return QStringLiteral("test:rate-on-its-own"); }
    QString description() const override { return QStringLiteral("Rate a track through a writer of its own"); }
    QString unit() const override { return QStringLiteral("ratings"); }
    QStringList formatsTouched() const override { return RekordboxOnly; }
    std::vector<BackupTarget> filesToBackup(SaveContext &) const override
    {
        return {{pathToUtf8(pathFromUtf8(m_pioneer.toStdString()) / "rekordbox" / "export.pdb"), "test-rate"}};
    }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", m_pioneer.toStdString(), 1, "test-rate");
        infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pathFromUtf8(session.writeRoot()) / "rekordbox" / "export.pdb"));
        const auto ids = rows.trackIdsWithFilePath(m_stickPath);
        if (ids.size() != 1) {
            return ChangeOutcome::failure(QStringLiteral("the row of the track imported before is not in the file"));
        }
        if (!rows.setTrackRating(ids.front(), 5) || !rows.commit()) {
            return ChangeOutcome::failure(QStringLiteral("could not rate"));
        }
        session.noteItemApplied();
        return ChangeOutcome::success();
    }

private:
    QString m_pioneer;
    std::string m_stickPath;
};

infrastructure::WorkCountersSnapshot countsNow()
{
    return infrastructure::WorkCounters::instance().snapshot();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: import_held_rows_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "import_held_rows_home");
    const fs::path computer = testing::scratchRoot() / "import_held_rows_computer";
    fs::remove_all(computer);

    // 1. Tracks into a new playlist, the last ten put at its top.
    {
        const testing::EngineChangeStick stick = rekordboxStick(fixture, "import_held_rows_one");
        const fs::path pdb = stick.pioneer / "rekordbox" / "export.pdb";
        const size_t before = libraryOf(stick).size();
        assert(before > 1000);
        const int count = 300;
        const std::string playlist = "Held Rows";
        std::vector<std::shared_ptr<PendingChange>> changes;
        auto made = std::make_shared<CreatePlaylistChange>(stick.pioneerPath(), QString(), playlist, RekordboxOnly, count);
        changes.push_back(made);
        std::vector<std::string> placed;
        std::vector<std::string> expected;
        for (int n = 0; n < count; ++n) {
            const fs::path source = makeSource(computer / "one", std::to_string(n) + ".flac", n);
            auto import = std::make_shared<ImportTrackChange>(pathToUtf8(source), pathToQString(stick.root), stick.pioneerPath(),
                                                              QString(), describe(stickPathOf(n), n), RekordboxOnly, count);
            changes.push_back(import);
            auto entry = std::make_shared<AddToPlaylistChange>(stick.pioneerPath(), QString(), playlist,
                                                               import->destinationFile(), import->subject(), RekordboxOnly);
            entry->tieTo(import->unitKeys().last());
            changes.push_back(entry);
            if (n >= count - 10) {
                placed.push_back(import->destinationFile());
            }
        }
        for (int n = count - 10; n < count; ++n) {
            expected.push_back(std::to_string(n) + " Held Row.flac");
        }
        for (int n = 0; n < count - 10; ++n) {
            expected.push_back(std::to_string(n) + " Held Row.flac");
        }
        changes.push_back(std::make_shared<PlaceInPlaylistChange>(stick.pioneerPath(), QString(), playlist, placed, 0, RekordboxOnly));

        const auto countsBefore = countsNow();
        const auto started = std::chrono::steady_clock::now();
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), QString());
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.appliedIds.size() == static_cast<int>(changes.size()));
        const auto countsAfter = countsNow();
        std::cerr << "  " << count << " imports, " << count << " entries and a move in " << seconds << " s ("
                  << 1000.0 * seconds / count << " ms a track): export.pdb opened "
                  << countsAfter.pdbWriterOpens - countsBefore.pdbWriterOpens << " and written "
                  << countsAfter.pdbWriterCommits - countsBefore.pdbWriterCommits << " times\n";
        assert(countsAfter.pdbWriterOpens - countsBefore.pdbWriterOpens == 1 && "export.pdb read once for the save");
        assert(countsAfter.pdbWriterCommits - countsBefore.pdbWriterCommits == 1 && "and written once");
        const auto tracks = libraryOf(stick);
        assert(tracks.size() == before + count);
        assert(playlistOrder(tracks, playlist) == expected && "every entry, the last ten at the top");
        std::cout << "case 1 (300 imports into a new playlist, ten put at its top: export.pdb read once and written "
                     "once, every row and entry there) OK\n";
    }

    // 2. A change with a writer of its own between imports.
    {
        const testing::EngineChangeStick stick = rekordboxStick(fixture, "import_held_rows_between");
        std::vector<std::shared_ptr<PendingChange>> changes;
        for (int n = 0; n < 6; ++n) {
            const fs::path source = makeSource(computer / "between", std::to_string(n) + ".flac", 100 + n);
            changes.push_back(std::make_shared<ImportTrackChange>(pathToUtf8(source), pathToQString(stick.root),
                                                                  stick.pioneerPath(), QString(), describe(stickPathOf(n), n),
                                                                  RekordboxOnly, 6));
            if (n == 2) {
                changes.push_back(std::make_shared<RateOnItsOwn>(stick.pioneerPath(), stickPathOf(1)));
            }
        }
        const auto countsBefore = countsNow();
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), QString());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty());
        const auto countsAfter = countsNow();
        // The held rows, the change's own writer, the held rows again.
        assert(countsAfter.pdbWriterOpens - countsBefore.pdbWriterOpens == 3);
        assert(countsAfter.pdbWriterCommits - countsBefore.pdbWriterCommits == 3);
        int found = 0;
        for (const auto &t : libraryOf(stick)) {
            for (int n = 0; n < 6; ++n) {
                if (pathToUtf8(pathFromUtf8(t.filePath).filename()) == std::to_string(n) + " Held Row.flac") {
                    ++found;
                    assert(t.rating.value_or(0) == (n == 1 ? 5 : 0) && "the rating made between the imports stays");
                }
            }
        }
        assert(found == 6);
        std::cout << "case 2 (a change with a writer of its own between imports: it sees the rows before it, the "
                     "imports after it see its rating) OK\n";
    }

    // 3. A change that fails after imports: the save goes back.
    {
        const testing::EngineChangeStick stick = rekordboxStick(fixture, "import_held_rows_fails");
        const fs::path pdb = stick.pioneer / "rekordbox" / "export.pdb";
        const std::string pdbBefore = testing::fileBytes(pdb);
        std::vector<std::shared_ptr<PendingChange>> changes;
        for (int n = 0; n < 4; ++n) {
            const fs::path source = makeSource(computer / "fails", std::to_string(n) + ".flac", 200 + n);
            changes.push_back(std::make_shared<ImportTrackChange>(pathToUtf8(source), pathToQString(stick.root),
                                                                  stick.pioneerPath(), QString(), describe(stickPathOf(n), n),
                                                                  RekordboxOnly, 5));
        }
        changes.push_back(std::make_shared<ImportTrackChange>(pathToUtf8(computer / "fails" / "not there.flac"),
                                                              pathToQString(stick.root), stick.pioneerPath(), QString(),
                                                              describe(stickPathOf(4), 4), RekordboxOnly, 5));
        const auto countsBefore = countsNow();
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), QString());
        assert(!saved.error.isEmpty() && saved.appliedIds.isEmpty());
        assert(countsNow().pdbWriterCommits == countsBefore.pdbWriterCommits && "the held rows were never written");
        assert(testing::fileBytes(pdb) == pdbBefore);
        assert(!fs::exists(stick.root / "Contents" / "Held Rows" / "0 Held Row.flac"));
        std::cout << "case 3 (a change that fails after four imports: export.pdb never written, the save back) OK\n";
    }

    // 4. A file replaced after its analysis, beside one that was not.
    {
        const testing::EngineChangeStick stick = rekordboxStick(fixture, "import_held_rows_replaced");
        const fs::path pdb = stick.pioneer / "rekordbox" / "export.pdb";
        const auto analysisOf = [](const fs::path &source, const std::string &key) {
            auto analysis = std::make_shared<ImportAnalysis>();
            analysis->waveform.sampleRateHz = 44100;
            analysis->waveform.sampleCount = 10 * 44100;
            analysis->waveform.entries.assign(1500, domain::WaveformEntry{0.5f, 0.5f, 0.5f, 0.5f});
            for (int i = 0; i < 20; ++i) {
                analysis->beats.push_back({i * 500.0, i % 4 + 1});
            }
            analysis->bpm = 120.0;
            analysis->key = key;
            ImportAnalysis::Provenance provenance;
            provenance.fileSize = fs::file_size(source);
            provenance.fileMtimeUnix = *fileMtimeUnixOf(pathToUtf8(source));
            analysis->provenance = provenance;
            return std::shared_ptr<const ImportAnalysis>(analysis);
        };
        struct Case
        {
            std::string name;
            bool replaced;
            std::string tagKey;
        };
        const std::vector<Case> cases{{"kept.wav", false, "Am"},
                                      {"kept keyless.wav", false, ""},
                                      {"replaced.wav", true, "Am"},
                                      {"replaced keyless.wav", true, ""}};
        std::vector<std::shared_ptr<PendingChange>> changes;
        for (const Case &c : cases) {
            const fs::path source = computer / "replaced" / c.name;
            testing::writeSilentWav(source, 44100, 10);
            auto change = std::make_shared<ImportTrackChange>(pathToUtf8(source), pathToQString(stick.root), stick.pioneerPath(),
                                                              QString(), describe("/Contents/Replaced/" + c.name, 7, c.tagKey),
                                                              RekordboxOnly, 4);
            change->setAnalysis(analysisOf(source, "F#m"));
            if (c.replaced) {
                testing::writeSilentWav(source, 44100, 11);  // another file in its place, of another size
            }
            changes.push_back(change);
        }
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), QString());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty());
        const std::string log = testing::fileBytes(stick.root / "Seabass" / "seabass.log");
        std::map<std::string, std::string> keys;
        for (const auto &t : libraryOf(stick)) {
            keys[pathToUtf8(pathFromUtf8(t.filePath).filename())] = t.key;
        }
        for (const Case &c : cases) {
            const auto row = pdbRowOf(pdb, "/Contents/Replaced/" + c.name);
            assert(row);
            const std::string said = "/Contents/Replaced/" + c.name + " is not recorded in the analysis ledger";
            if (c.replaced) {
                assert(row->analyzePath.empty() && "no analysis file named");
                assert(row->tempo == 12650 && "the tag's tempo, not the analysis's");
                assert(keys.at(c.name) == c.tagKey && "the tag's key, or none");
                assert(log.find(said) != std::string::npos
                       && log.find("It was replaced on the computer after its analysis, so it is imported without that analysis")
                              != std::string::npos);
            } else {
                assert(!row->analyzePath.empty() && "the analysis files named");
                assert(fs::exists(stick.root / pathFromUtf8(row->analyzePath.substr(1))));
                assert(row->tempo == 12000 && "the analysis's tempo");
                assert(keys.at(c.name) == (c.tagKey.empty() ? std::string("F#m") : c.tagKey));
                assert(log.find(said) == std::string::npos);
            }
        }
        std::cout << "case 4 (a file replaced after its analysis: no analysis file, the tags' tempo and key, the log says "
                     "why; one not replaced gets all of it) OK\n";
    }

    std::cout << "import_held_rows_test: all cases passed\n";
    return 0;
}
