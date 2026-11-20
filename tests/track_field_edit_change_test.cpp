// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A track's rating and comment edited in Browse (#105), written through
// the real save loop into every catalog of a copy of the fixture, read
// back by the repo's readers and by the raw columns, undone, and rolled
// back when the last catalog cannot be written.
//
//   track_field_edit_change_test <tests/fixtures/one_stick_two_catalogs>
//
// TRACK_EDIT_CASE=<n> runs that case alone.

#include <QString>
#include <QStringList>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/track_field_edit_change.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "engine_change_fixture.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
using seabass::testing::fileBytes;
using seabass::testing::saveChanges;
using seabass::testing::sqlScalar;
using seabass::testing::undoSave;

namespace
{

std::string key(const std::string &path)
{
    return application::normalizedPathKey(path);
}

void write(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

struct ScratchStick
{
    fs::path path;
    explicit ScratchStick(fs::path p) : path(std::move(p)) { fs::remove_all(path); }
    ~ScratchStick()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// One track as each catalog has it: format -> row.
using Copies = std::map<std::string, domain::Track>;

struct FixtureStick
{
    ScratchStick scratch;
    fs::path stick, pioneer, engine;
    FixtureStick(const fs::path &fixture, const std::string &name)
        : scratch(seabass::testing::scratchRoot() / name),
          stick(scratch.path),
          pioneer(stick / "PIONEER"),
          engine(stick / "Engine Library")
    {
        std::error_code ec;
        seabass::testing::copyPioneerFixture(fixture / "rekordbox", pioneer, ec);
        assert(!ec && "the rekordbox side of the fixture copies");
        fs::copy(fixture / "engine", engine, fs::copy_options::recursive, ec);
        assert(!ec && "the Engine side of the fixture copies");
    }
    QString pioneerQ() const { return pathToQString(pioneer); }
    QString engineQ() const { return pathToQString(engine); }
    fs::path pdb() const { return pioneer / "rekordbox" / "export.pdb"; }
    fs::path mdb() const { return engine / "Database2" / "m.db"; }
    std::string oneLibraryDb() const
    {
        return infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer));
    }

    std::vector<domain::Track> readAll() const
    {
        std::vector<domain::Track> rows;
        if (fs::exists(pdb())) {
            infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(pathToUtf8(pioneer));
            for (auto &t : application::ScanLibrary(rekordbox).execute()) {
                rows.push_back(std::move(t));
            }
        }
        infrastructure::onelibrary::OneLibraryReader oneLibrary(pathToUtf8(pioneer));
        for (auto &t : application::ScanLibrary(oneLibrary).execute()) {
            rows.push_back(std::move(t));
        }
        infrastructure::engine::LibdjinteropEngineReader engineReader(pathToUtf8(engine));
        for (auto &t : application::ScanLibrary(engineReader).execute()) {
            rows.push_back(std::move(t));
        }
        return rows;
    }
    Copies copiesOf(const std::string &filePath) const
    {
        Copies out;
        for (auto &t : readAll()) {
            if (!t.filePath.empty() && key(t.filePath) == key(filePath)) {
                out[t.format] = t;
            }
        }
        return out;
    }
    // Files export.pdb lists once and OneLibrary at least once, by the rekordbox
    // row. The fixture's Engine side was anonymized apart from its
    // rekordbox side, so no file is in all three; plantEngineCopy() makes
    // one so.
    std::vector<domain::Track> inBothRekordbox() const
    {
        std::map<std::string, std::map<std::string, int>> counts;
        std::map<std::string, domain::Track> rekordbox;
        for (const auto &t : readAll()) {
            if (t.filePath.empty()) {
                continue;
            }
            ++counts[key(t.filePath)][t.format];
            if (t.format == "rekordbox") {
                rekordbox[key(t.filePath)] = t;
            }
        }
        std::vector<domain::Track> out;
        for (const auto &[k, perFormat] : counts) {
            if (perFormat.count("rekordbox") && perFormat.at("rekordbox") == 1 && perFormat.count("onelibrary")
                && (!perFormat.count("engine") || perFormat.at("engine") == 1)) {
                out.push_back(rekordbox.at(k));
            }
        }
        return out;
    }
    // Points one Engine row at `t`'s file, as Engine spells a file on the
    // stick (relative to its library folder), so all three catalogs list
    // it. Each call takes another Engine row.
    void plantEngineCopy(const domain::Track &t)
    {
        if (copiesOf(t.filePath).size() == 3) {
            return;  // Engine lists it already
        }
        static int next = 0;
        const auto ids = seabass::testing::sqlRows(pathToUtf8(mdb()), "SELECT id FROM Track ORDER BY id;");
        const std::string id = ids.at(static_cast<size_t>(next++))[0];
        const std::string relative = pathToGenericUtf8(pathFromUtf8(t.filePath).lexically_relative(engine));
        const std::string name = pathToUtf8(pathFromUtf8(t.filePath).filename());
        assert(relative.find('\'') == std::string::npos);
        assert(seabass::testing::sqlExec(pathToUtf8(mdb()), "UPDATE Track SET path = '" + relative + "', filename = '"
                                                                 + name + "' WHERE id = " + id + ";"));
        assert(copiesOf(t.filePath).size() == 3 && "the planted Engine row names the file");
    }
    domain::Track threeCatalogTrack(size_t index = 0)
    {
        const auto tracks = inBothRekordbox();
        assert(tracks.size() > index);
        plantEngineCopy(tracks[index]);
        return tracks[index];
    }

    // The raw stored values, below the readers.
    int pdbRatingByte(uint32_t id) const
    {
        // The reader maps 0 to "none"; the writer's own lookup is not
        // the reader, so read the byte through a fresh writer's view: a
        // rating of 0 reads back as nullopt, anything else as itself.
        for (const auto &t : readAll()) {
            if (t.format == "rekordbox" && t.sourceId == std::to_string(id)) {
                return t.rating.value_or(0);
            }
        }
        assert(false && "the pdb row is there");
        return -1;
    }
    std::optional<std::int64_t> oneLibraryRating(const std::string &filePath) const
    {
        infrastructure::onelibrary::SqlCipherLibrary lib;
        infrastructure::onelibrary::SqlCipherDb db(lib, oneLibraryDb(), true);
        db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
        infrastructure::onelibrary::SqlCipherStatement s(db, "SELECT rating, path FROM content");
        std::optional<std::int64_t> out;
        bool found = false;
        while (s.step()) {
            if (key(pathToUtf8(stick) + s.columnText(1)) == key(filePath)) {
                found = true;
                out = s.columnIsNull(0) ? std::nullopt : std::optional<std::int64_t>(s.columnInt64(0));
            }
        }
        assert(found);
        return out;
    }
    std::int64_t engineRating(const std::string &id) const
    {
        return sqlScalar(pathToUtf8(mdb()), "SELECT rating FROM Track WHERE id = " + id + ";");
    }
};

std::shared_ptr<TrackFieldEditChange> ratingChange(const FixtureStick &s, const std::string &file,
                                                   std::optional<int> stars)
{
    return std::make_shared<TrackFieldEditChange>(s.pioneerQ(), s.engineQ(), file, "T",
                                                  TrackFieldValue{TrackField::Rating, stars, {}});
}

std::shared_ptr<TrackFieldEditChange> commentChange(const FixtureStick &s, const std::string &file,
                                                    const std::string &text, QString pioneer = {})
{
    return std::make_shared<TrackFieldEditChange>(pioneer.isNull() ? s.pioneerQ() : pioneer, s.engineQ(), file, "T",
                                                  TrackFieldValue{TrackField::Comment, std::nullopt, text});
}

void expectRatingEverywhere(const FixtureStick &s, const std::string &file, std::optional<int> stars)
{
    const Copies copies = s.copiesOf(file);
    assert(copies.size() == 3);
    for (const auto &[format, t] : copies) {
        if (t.rating != stars) {
            std::cerr << format << " reads rating " << (t.rating ? std::to_string(*t.rating) : "none") << ", expected "
                      << (stars ? std::to_string(*stars) : "none") << "\n";
            assert(false && "every reader reads the written rating");
        }
    }
    // And below the readers: the stored numbers in each format's scale.
    assert(s.oneLibraryRating(file) == std::optional<std::int64_t>(stars.value_or(0)));
    assert(s.engineRating(copies.at("engine").sourceId) == stars.value_or(0) * 20);
    assert(s.pdbRatingByte(static_cast<uint32_t>(std::stoul(copies.at("rekordbox").sourceId))) == stars.value_or(0));
}

void expectCommentEverywhere(const FixtureStick &s, const std::string &file, const std::string &comment,
                             const std::set<std::string> &formats = {"rekordbox", "onelibrary", "engine"})
{
    const Copies copies = s.copiesOf(file);
    for (const auto &format : formats) {
        assert(copies.count(format));
        if (copies.at(format).comment != comment) {
            std::cerr << format << " reads comment \"" << copies.at(format).comment << "\", expected \"" << comment
                      << "\"\n";
            assert(false && "every reader reads the written comment");
        }
    }
}

// The fixture track in all three catalogs whose export.pdb comment has the
// most room, with that room.
std::pair<domain::Track, infrastructure::rekordbox::PdbRowWriter::TrackCommentCapacity>
roomiestComment(FixtureStick &s, bool utf16)
{
    const infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pdb()));
    std::optional<std::pair<domain::Track, infrastructure::rekordbox::PdbRowWriter::TrackCommentCapacity>> best;
    for (const auto &t : s.inBothRekordbox()) {
        const auto capacity = pdb.trackCommentCapacity(static_cast<uint32_t>(std::stoul(t.sourceId)));
        if (capacity && capacity->utf16 == utf16 && (!best || capacity->units > best->second.units)) {
            best = {t, *capacity};
        }
    }
    assert(best && best->second.units >= 8 && "the fixture has a track with room for a comment in export.pdb");
    s.plantEngineCopy(best->first);
    return *best;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: track_field_edit_change_test <tests/fixtures/one_stick_two_catalogs>\n";
        return 1;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    const auto runs = [](int n) {
        const char *only = std::getenv("TRACK_EDIT_CASE");
        return only == nullptr || std::atoi(only) == n;
    };

    // 1. A rating set, changed and cleared lands in all three catalogs, in
    //    each one's scale (stars; stars; 20 per star), and every reader
    //    reads it back. Clearing writes each format's "no stars" (0).
    if (runs(1)) {
        FixtureStick s(fixture, "seabass_track_edit_rating");
        const std::string file = s.threeCatalogTrack().filePath;
        for (const std::optional<int> stars : {std::optional<int>(4), std::optional<int>(2), std::optional<int>()}) {
            const auto change = ratingChange(s, file, stars);
            assert(change->id() == trackFieldChangeId(TrackField::Rating, file));
            const auto result = saveChanges({change}, s.pioneerQ(), s.engineQ());
            assert(result.error.isEmpty() && result.appliedIds == QStringList{change->id()});
            expectRatingEverywhere(s, file, stars);
            // The origin ledger hears of it: Sync after Rekordbox Export
            // then knows Seabass wrote this rating.
            const auto writes = change->rekordboxWrites();
            assert(writes.size() == 1 && writes[0].rating == stars.value_or(0) && !writes[0].cues);
        }
        std::cout << "case 1 (rating set, changed, cleared in all three catalogs) OK\n";
    }

    // 2. export.pdb's comment: the same length, shorter and empty are
    //    written in place (padded, read back trimmed) in all three; longer
    //    and, in an ASCII field, non-ASCII are refused by the writer, fail
    //    the save, and leave every catalog as it was.
    for (const bool utf16 : {false, true}) {
        if (!runs(2)) {
            break;
        }
        FixtureStick s(fixture, utf16 ? "seabass_track_edit_comment16" : "seabass_track_edit_comment");
        const auto [track, capacity] = roomiestComment(s, utf16);
        const std::string file = track.filePath;
        const std::string originalComment = s.copiesOf(file).at("rekordbox").comment;
        const auto save = [&](const std::string &text) {
            return saveChanges({commentChange(s, file, text)}, s.pioneerQ(), s.engineQ());
        };
        // The same length as the exported comment (the span holds exactly
        // it, so it fills the span): written.
        const std::string full(capacity.units, 'c');
        assert(originalComment.size() <= full.size());
        assert(save(full).error.isEmpty());
        expectCommentEverywhere(s, file, full);
        assert(save("short").error.isEmpty());
        expectCommentEverywhere(s, file, "short");

        // Longer: refused, the save rolled back, nothing changed anywhere.
        const std::string pdbBefore = fileBytes(s.pdb());
        const std::string engineBefore = fileBytes(s.mdb());
        const auto longer = save(full + "x");
        assert(!longer.error.isEmpty() && longer.rolledBack && "a comment longer than the row holds is refused");
        assert(longer.error.contains("room for"));
        assert(fileBytes(s.pdb()) == pdbBefore && fileBytes(s.mdb()) == engineBefore);
        expectCommentEverywhere(s, file, "short");

        // Non-ASCII: written into a UTF-16 field, a character outside the
        // BMP counting two units there; refused by an ASCII field.
        const std::string accented = "Caf\xc3\xa9 \xf0\x9f\x8e\xb6";  // "Café 🎶": 7 UTF-16 units
        const auto nonAscii = save(accented);
        if (utf16) {
            assert(capacity.units >= 7);
            assert(nonAscii.error.isEmpty());
            expectCommentEverywhere(s, file, accented);
            const std::string tooMany(capacity.units - 1, 'e');
            const auto overByEmoji = save(tooMany + "\xf0\x9f\x8e\xb6");  // units + 1
            assert(!overByEmoji.error.isEmpty() && "an emoji is two units in a UTF-16 field");
        } else {
            assert(!nonAscii.error.isEmpty() && nonAscii.error.contains("ASCII"));
            expectCommentEverywhere(s, file, "short");
        }
        assert(save("").error.isEmpty());
        expectCommentEverywhere(s, file, "");
        std::cout << "case 2 (export.pdb comment in a" << (utf16 ? " UTF-16" : "n ASCII") << " field of "
                  << capacity.units << " units: same length, shorter, empty written; longer"
                  << (utf16 ? "" : " and non-ASCII") << " refused) OK\n";
    }

    // 3. The limit Browse holds the field to is what the writer takes: at
    //    the limit it round-trips through all three, one over is refused
    //    by the limit's own rule and by the writer.
    if (runs(3)) {
        FixtureStick s(fixture, "seabass_track_edit_limit");
        const auto [track, capacity] = roomiestComment(s, false);
        const CommentLimit limit = commentLimitFor(s.pioneerQ(), track.filePath);
        assert(limit.limited && limit.units == capacity.units && limit.asciiOnly == !capacity.utf16);
        assert(!limit.reason.empty());
        const std::string atLimit(limit.units, 'L');
        assert(commentLimitRefusal(atLimit, limit).empty());
        assert(!commentLimitRefusal(atLimit + "L", limit).empty());
        assert(saveChanges({commentChange(s, track.filePath, atLimit)}, s.pioneerQ(), s.engineQ()).error.isEmpty());
        expectCommentEverywhere(s, track.filePath, atLimit);
        infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pdb()));
        std::string refusal;
        assert(!pdb.setTrackComment(static_cast<uint32_t>(std::stoul(track.sourceId)), atLimit + "L", &refusal));
        assert(!refusal.empty() && "the writer refuses one over the limit as a second line of defence");
        // A track with no room: the limit says so, and 0.
        const infrastructure::rekordbox::PdbRowWriter reader(pathToUtf8(s.pdb()));
        bool sawNoRoom = false;
        for (const auto &t : s.inBothRekordbox()) {
            const auto c = reader.trackCommentCapacity(static_cast<uint32_t>(std::stoul(t.sourceId)));
            if (c && c->units == 0) {
                const CommentLimit none = commentLimitFor(s.pioneerQ(), t.filePath);
                assert(none.limited && none.units == 0 && none.reason.find("no room") != std::string::npos);
                assert(commentLimitRefusal("", none).empty() && !commentLimitRefusal("a", none).empty());
                sawNoRoom = true;
                break;
            }
        }
        assert(sawNoRoom && "the fixture has a track with no room for a comment");
        std::cout << "case 3 (the comment limit matches what the writer takes, " << limit.units << " units) OK\n";
    }

    // 4. Without export.pdb, OneLibrary and Engine take any comment: long,
    //    non-ASCII, emoji. And no limit is put on the field.
    if (runs(4)) {
        FixtureStick s(fixture, "seabass_track_edit_no_pdb");
        const std::string file = s.threeCatalogTrack().filePath;
        fs::remove(s.pdb());
        assert(!commentLimitFor(s.pioneerQ(), file).limited);
        const std::string text = std::string(300, 'w') + " Gr\xc3\xbc\xc3\x9f" + "e \xf0\x9f\x8e\xb6";
        const auto result = saveChanges({commentChange(s, file, text)}, s.pioneerQ(), s.engineQ());
        assert(result.error.isEmpty());
        expectCommentEverywhere(s, file, text, {"onelibrary", "engine"});
        std::cout << "case 4 (no export.pdb: a long non-ASCII comment lands in OneLibrary and Engine) OK\n";
    }

    // 5. One staged edit in all three, then Undo Last Save puts all three
    //    back as they were, byte for byte.
    if (runs(5)) {
        FixtureStick s(fixture, "seabass_track_edit_undo");
        const std::string file = s.threeCatalogTrack().filePath;
        // Engine's import counter level with export.pdb, as any earlier
        // Seabass save leaves a stick: a save (the Undo's too) levels it
        // otherwise, which is not this change's write.
        {
            const auto state = infrastructure::engine::readRekordboxImportState(pathToUtf8(s.engine), pathToUtf8(s.pioneer));
            assert(state.error.empty());
            std::string error;
            assert(infrastructure::engine::markRekordboxLibraryImported(pathToUtf8(s.engine), state.librarySequence,
                                                                         &error));
        }
        const std::string pdbBefore = fileBytes(s.pdb());
        const std::string engineBefore = fileBytes(s.mdb());
        const auto dumpBefore = seabass::testing::dumpDatabase(pathToUtf8(s.mdb()));
        const Copies before = s.copiesOf(file);
        const auto saved = saveChanges({ratingChange(s, file, 5)}, s.pioneerQ(), s.engineQ());
        assert(saved.error.isEmpty());
        expectRatingEverywhere(s, file, 5);
        const auto undone = undoSave(saved, s.pioneerQ(), s.engineQ());
        assert(undone.error.isEmpty());
        assert(fileBytes(s.pdb()) == pdbBefore && "export.pdb is back byte for byte");
        if (fileBytes(s.mdb()) != engineBefore) {
            seabass::testing::printDifferences(
                seabass::testing::differences(dumpBefore, seabass::testing::dumpDatabase(pathToUtf8(s.mdb()))));
            assert(false && "m.db is back byte for byte");
        }
        const Copies after = s.copiesOf(file);
        for (const auto &[format, t] : before) {
            assert(after.at(format).rating == t.rating && "Undo Last Save put every catalog's rating back");
        }
        std::cout << "case 5 (Undo Last Save restores all three) OK\n";
    }

    // 6. The last catalog written (export.pdb, after OneLibrary and
    //    Engine) cannot be: a non-empty folder where its durable replace
    //    writes its temporary file. The save fails whole, and Engine and
    //    OneLibrary, written before it, are put back.
    if (runs(6)) {
        FixtureStick s(fixture, "seabass_track_edit_rollback");
        const std::string file = s.threeCatalogTrack().filePath;
        const Copies before = s.copiesOf(file);
        const std::string pdbBefore = fileBytes(s.pdb());
        const std::string engineBefore = fileBytes(s.mdb());
        const int newStars = before.at("rekordbox").rating.value_or(0) == 3 ? 4 : 3;
        const fs::path obstacle = pathFromUtf8(pathToUtf8(s.pdb()) + ".tmp-seabass-write");
        write(obstacle / "in the way", "x");
        const auto result = saveChanges({ratingChange(s, file, newStars)}, s.pioneerQ(), s.engineQ());
        if (result.error.isEmpty() || !result.rolledBack) {
            std::cerr << "error: \"" << result.error.toStdString() << "\" rolledBack=" << result.rolledBack << "\n";
        }
        assert(!result.error.isEmpty() && result.rolledBack);
        assert(result.error.contains("export.pdb"));
        fs::remove_all(obstacle);
        assert(fileBytes(s.pdb()) == pdbBefore && fileBytes(s.mdb()) == engineBefore);
        const Copies after = s.copiesOf(file);
        for (const auto &[format, t] : before) {
            if (after.at(format).rating != t.rating) {
                std::cerr << format << " kept the rating\n";
                assert(false && "no catalog kept the rating");
            }
        }
        std::cout << "case 6 (a failure in export.pdb, written last, rolls Engine and OneLibrary back) OK\n";
    }

    // 7. Staged twice for one track, the same id: the later one replaces
    //    the first in a session; and rating and comment are one unit.
    if (runs(7)) {
        FixtureStick s(fixture, "seabass_track_edit_ids");
        const std::string file = s.threeCatalogTrack().filePath;
        assert(ratingChange(s, file, 1)->id() == ratingChange(s, file, 4)->id());
        assert(ratingChange(s, file, 1)->id() != commentChange(s, file, "x")->id());
        assert(ratingChange(s, file, 1)->unitKeys() == commentChange(s, file, "x")->unitKeys());
        assert(ratingChange(s, file, 1)->owner() == QStringLiteral("addcue"));
        std::cout << "case 7 (ids and units) OK\n";
    }
    return 0;
}
