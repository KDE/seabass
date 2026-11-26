// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The tier 2 fixture (real analysis data, de-identified, no audio) read
// through the app's own readers. See "Test fixtures in three tiers" in
// docs/testing.md.
//
// The fixture is not in the repository. cmake/SeabassFixtures.cmake hands
// its directory over as SEABASS_REAL_ANALYSIS_FIXTURE in this test's
// environment, and registers the test only when there is one: a build
// without the fixture has no such test, rather than one that does not
// run. Run by hand without a directory it fails, saying how to get one,
// and so does a directory that is not a whole fixture: a broken path or
// a truncated download cannot pass for "no fixture".
//
// What it checks:
//  * every file MANIFEST.json lists is there, with its size and SHA-256,
//    and nothing else is;
//  * both catalogs read, with the track counts the manifest records, and
//    the tracks they pair on are the manifest's pairs;
//  * for every pair, rekordbox's beat grid, PWAV preview, PWV4 and PWV6
//    previews, scrolling waveform and detail waveform, and Engine's
//    overview and beat grid, read with the counts the manifest records.
//    The manifest's counts come from the files' own headers, written by
//    tools/make-real-analysis-fixture.py, not from these readers.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure;

namespace
{

int failures = 0;

void fail(const std::string &what)
{
    ++failures;
    if (failures <= 40) {
        std::cerr << "FAIL: " << what << "\n";
    }
}

void expectEqual(const std::string &what, long long actual, long long expected)
{
    if (actual != expected) {
        fail(what + ": read " + std::to_string(actual) + ", MANIFEST.json says " + std::to_string(expected));
    }
}

std::string sha256Of(const fs::path &path)
{
    QFile file(QString::fromStdString(seabass::pathToUtf8(path)));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result().toHex().toStdString();
}

// Every file listed is there with its size and hash, and no other is.
// MANIFEST.json itself is the one file it cannot list.
void checkTree(const fs::path &root, const QJsonObject &files)
{
    std::set<std::string> listed;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const std::string rel = it.key().toStdString();
        listed.insert(rel);
        const fs::path path = root / seabass::pathFromUtf8(rel);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            fail(rel + " is listed in MANIFEST.json and missing");
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        const auto size = static_cast<long long>(fs::file_size(path, ec));
        if (size != entry.value("size").toInteger()) {
            fail(rel + " is " + std::to_string(size) + " bytes, MANIFEST.json says "
                 + std::to_string(entry.value("size").toInteger()));
            continue;
        }
        if (sha256Of(path) != entry.value("sha256").toString().toStdString()) {
            fail(rel + " does not have the SHA-256 MANIFEST.json gives");
        }
    }
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string rel = seabass::pathToGenericUtf8(fs::relative(entry.path(), root));
        if (rel != "MANIFEST.json" && listed.count(rel) == 0) {
            fail(rel + " is in the fixture and not in MANIFEST.json");
        }
    }
}

// The scrolling waveform the readers report: PWV7 from the .2EX, else
// PWV5, else PWV3 (see readWaveformSections()).
long long scrollEntriesOf(const QJsonObject &rekordbox)
{
    for (const char *tag : {"pwv7", "pwv5", "pwv3"}) {
        if (rekordbox.value(tag).toInteger() > 0) {
            return rekordbox.value(tag).toInteger();
        }
    }
    return 0;
}

}  // namespace

int main()
{
    const char *configured = std::getenv("SEABASS_REAL_ANALYSIS_FIXTURE");
    if (configured == nullptr || std::string(configured).empty()) {
        std::cerr << "FAIL: no real-analysis fixture configured. Configure with -DSEABASS_FETCH_FIXTURES=ON to "
                     "download it, or with -DSEABASS_REAL_ANALYSIS_FIXTURE=<dir> to use an unpacked copy.\n";
        return 1;
    }
    const fs::path root = seabass::pathFromUtf8(configured);
    const fs::path manifestPath = root / "MANIFEST.json";
    QFile manifestFile(QString::fromStdString(seabass::pathToUtf8(manifestPath)));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        std::cerr << "FAIL: SEABASS_REAL_ANALYSIS_FIXTURE is " << configured
                  << " and there is no readable MANIFEST.json in it. Point it at an unpacked fixture.\n";
        return 1;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (!document.isObject()) {
        std::cerr << "FAIL: MANIFEST.json does not parse: " << parseError.errorString().toStdString() << "\n";
        return 1;
    }
    const QJsonObject manifest = document.object();
    const QJsonObject counts = manifest.value("counts").toObject();
    const QJsonArray paired = manifest.value("paired").toArray();
    const QJsonObject files = manifest.value("files").toObject();
    if (files.isEmpty() || paired.isEmpty()) {
        std::cerr << "FAIL: MANIFEST.json lists no files or no paired tracks\n";
        return 1;
    }

    checkTree(root, files);
    if (failures > 0) {
        std::cerr << failures << " problem(s): the fixture does not match its MANIFEST.json\n";
        return 1;
    }
    std::cout << "tree: " << files.size() << " files match MANIFEST.json\n";

    // On a copy: the Engine reader opens m.db through SQLite, which leaves
    // journal files beside it, and the fixture is checked file by file.
    const fs::path scratch = seabass::testing::scratchRoot() / "real_analysis_fixture_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const QJsonObject layout = manifest.value("layout").toObject();
    const std::string pioneerName = layout.value("pioneer").toString("PIONEER").toStdString();
    const std::string engineName = layout.value("engine").toString("Engine Library").toStdString();
    for (const std::string &name : {pioneerName, engineName}) {
        fs::copy(root / seabass::pathFromUtf8(name), scratch / seabass::pathFromUtf8(name), fs::copy_options::recursive);
    }
    const std::string pioneer = seabass::pathToUtf8(scratch / seabass::pathFromUtf8(pioneerName));
    const std::string engineLibrary = seabass::pathToUtf8(scratch / seabass::pathFromUtf8(engineName));

    rekordbox::KaitaiRekordboxReader rekordboxReader(pioneer);
    const std::vector<seabass::domain::Track> rekordboxTracks = rekordboxReader.readTracks();
    engine::LibdjinteropEngineReader engineReader(engineLibrary);
    const std::vector<seabass::domain::Track> engineTracks = engineReader.readTracks();
    expectEqual("rekordbox tracks", static_cast<long long>(rekordboxTracks.size()),
                counts.value("rekordbox_tracks").toInteger());
    expectEqual("Engine tracks", static_cast<long long>(engineTracks.size()),
                counts.value("engine_tracks").toInteger());

    // The pairs, by file name as the fixture builder pairs them: an Engine
    // row whose file name one rekordbox row with an analysis file shares.
    std::map<std::string, std::vector<const seabass::domain::Track *>> rekordboxByName;
    for (const auto &track : rekordboxTracks) {
        rekordboxByName[track.filename].push_back(&track);
    }
    std::set<std::pair<std::string, std::string>> readPairs;
    for (const auto &track : engineTracks) {
        const auto found = rekordboxByName.find(track.filename);
        if (found == rekordboxByName.end() || found->second.size() != 1) {
            continue;
        }
        const seabass::domain::Track &match = *found->second.front();
        std::error_code ec;
        if (match.analysisFile.empty()
            || !fs::is_regular_file(seabass::pathFromUtf8(pioneer)
                                        / seabass::pathFromUtf8(match.analysisFile.substr(
                                            match.analysisFile.find("/USBANLZ/") + 1)),
                                    ec)) {
            continue;
        }
        readPairs.emplace(match.sourceId, track.sourceId);
    }
    std::set<std::pair<std::string, std::string>> manifestPairs;
    for (const auto &value : paired) {
        const QJsonObject pair = value.toObject();
        manifestPairs.emplace(std::to_string(pair.value("rekordbox_id").toInteger()),
                              std::to_string(pair.value("engine_id").toInteger()));
    }
    expectEqual("paired tracks", static_cast<long long>(readPairs.size()), counts.value("paired_tracks").toInteger());
    if (readPairs != manifestPairs) {
        fail("the tracks the catalogs pair on are not the pairs MANIFEST.json lists");
    }

    long long beatsRead = 0;
    long long scrollEntriesRead = 0;
    long long overviewEntriesRead = 0;
    for (const auto &value : paired) {
        const QJsonObject pair = value.toObject();
        const std::string rid = std::to_string(pair.value("rekordbox_id").toInteger());
        const std::string eid = std::to_string(pair.value("engine_id").toInteger());
        const QJsonObject rb = pair.value("rekordbox").toObject();
        const QJsonObject en = pair.value("engine").toObject();
        const std::string which = "rekordbox " + rid + " / Engine " + eid;

        const auto beats = rekordbox::readBeatGrid(pioneer, rid);
        expectEqual(which + ": rekordbox beats", static_cast<long long>(beats.size()), rb.value("beats").toInteger());
        beatsRead += static_cast<long long>(beats.size());

        expectEqual(which + ": PWAV columns", static_cast<long long>(rekordbox::readWaveformPreview(pioneer, rid).size()),
                    rb.value("pwav").toInteger());

        const rekordbox::RekordboxWaveformSections sections = rekordbox::readWaveformSections(pioneer, rid);
        expectEqual(which + ": PWV6 bytes",
                    sections.threeBandPreview ? static_cast<long long>(sections.threeBandPreview->size()) : 0,
                    rb.value("pwv6").toInteger() * 3);
        expectEqual(which + ": PWV4 bytes",
                    sections.colourPreview ? static_cast<long long>(sections.colourPreview->size()) : 0,
                    rb.value("pwv4").toInteger() * 6);
        expectEqual(which + ": scrolling waveform entries",
                    sections.scrollEntries ? static_cast<long long>(*sections.scrollEntries) : 0, scrollEntriesOf(rb));
        scrollEntriesRead += sections.scrollEntries ? static_cast<long long>(*sections.scrollEntries) : 0;

        const long long detailExpected =
            rb.value("pwv7").toInteger() > 0 ? rb.value("pwv7").toInteger() : rb.value("pwv3").toInteger();
        expectEqual(which + ": detail waveform columns",
                    static_cast<long long>(rekordbox::readDetailWaveform(pioneer, rid).columns.size()), detailExpected);

        const seabass::domain::TrackAnalysis engineAnalysis =
            engine::readTrackAnalysis(engineLibrary, eid, std::numeric_limits<std::size_t>::max());
        expectEqual(which + ": Engine overview entries", static_cast<long long>(engineAnalysis.detail.columns.size()),
                    en.value("overview_entries").toInteger());
        expectEqual(which + ": Engine overview drawn", engineAnalysis.waveform.empty() ? 0 : 1,
                    en.value("overview_entries").toInteger() > 0 ? 1 : 0);
        overviewEntriesRead += static_cast<long long>(engineAnalysis.detail.columns.size());

        // Engine stores markers and the reader lays out every beat between
        // them, to the track's length: a count no file header holds. That
        // there is a grid when there are two markers or more is what the
        // manifest can say.
        expectEqual(which + ": Engine grid", engine::readBeatGrid(engineLibrary, eid).empty() ? 0 : 1,
                    en.value("grid_markers").toInteger() >= 2 ? 1 : 0);
    }

    if (failures > 0) {
        std::cerr << failures << " problem(s) reading the fixture\n";
        return 1;
    }
    std::cout << "read " << paired.size() << " paired tracks: " << beatsRead << " rekordbox beats, "
              << scrollEntriesRead << " scrolling waveform entries, " << overviewEntriesRead
              << " Engine overview entries, all as MANIFEST.json records them\n";
    return 0;
}
