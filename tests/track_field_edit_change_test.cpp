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
#include <iterator>
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
#include "infrastructure/work_counters.hpp"
#include "infrastructure/media/stick_root_scan.hpp"
#include "infrastructure/database_read_copy.hpp"
#include "infrastructure/system/process_liveness.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include <chrono>
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include <djinterop/djinterop.hpp>

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

// Raw writes below the change, to set a stick up.
void oneLibraryExec(const FixtureStick &s, const std::string &sql, const std::vector<std::string> &texts = {})
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db(lib, s.oneLibraryDb(), false);
    db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    infrastructure::onelibrary::SqlCipherStatement st(db, sql);
    int i = 1;
    for (const auto &t : texts) {
        st.bindText(i++, t);
    }
    st.run();
}

// The catalog's own stored path for the file: OneLibrary's content.path.
std::string oneLibraryPathOf(const FixtureStick &s, const std::string &file)
{
    for (const auto &[id, path] : infrastructure::onelibrary::oneLibraryContentPaths(pathToUtf8(s.pioneer))) {
        if (key(pathToUtf8(s.stick) + path) == key(file)) {
            return path;
        }
    }
    assert(false && "OneLibrary lists the file");
    return {};
}

// The file's folder spelled in upper case: "/Contents/x.mp3" to
// "/CONTENTS/x.mp3", as another catalog may spell it on a FAT stick.
std::string upperFolder(const std::string &stored)
{
    const auto slash = stored.find_last_of('/');
    std::string out = stored;
    for (size_t i = 0; i < slash; ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

// Every catalog's rating for the file set to its own value, so a rollback
// is seen in each: export.pdb `pdb`, OneLibrary `one`, Engine `engine`.
void setRatings(const FixtureStick &s, const std::string &file, int pdbStars, int oneStars, int engineStars)
{
    const Copies copies = s.copiesOf(file);
    {
        infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pdb()));
        assert(pdb.setTrackRating(static_cast<uint32_t>(std::stoul(copies.at("rekordbox").sourceId)), pdbStars));
        assert(pdb.commit());
    }
    oneLibraryExec(s, "UPDATE content SET rating = " + std::to_string(oneStars) + " WHERE content_id = "
                          + copies.at("onelibrary").sourceId);
    assert(seabass::testing::sqlExec(pathToUtf8(s.mdb()), "UPDATE Track SET rating = " + std::to_string(engineStars * 20)
                                                               + " WHERE id = " + copies.at("engine").sourceId + ";"));
    const Copies after = s.copiesOf(file);
    assert(after.at("rekordbox").rating == std::optional<int>(pdbStars));
    assert(after.at("onelibrary").rating == std::optional<int>(oneStars));
    assert(after.at("engine").rating == std::optional<int>(engineStars));
}

void expectRatings(const FixtureStick &s, const std::string &file, int pdbStars, int oneStars, int engineStars)
{
    const Copies c = s.copiesOf(file);
    if (c.at("rekordbox").rating != std::optional<int>(pdbStars) || c.at("onelibrary").rating != std::optional<int>(oneStars)
        || c.at("engine").rating != std::optional<int>(engineStars)) {
        std::cerr << "ratings: export.pdb " << c.at("rekordbox").rating.value_or(0) << ", OneLibrary "
                  << c.at("onelibrary").rating.value_or(0) << ", Engine " << c.at("engine").rating.value_or(0) << "\n";
        assert(false && "every catalog kept its own rating");
    }
}

// Every file under `root`: path, size and modification time, to tell
// whether anything was created, removed or written.
std::map<std::string, std::pair<std::uintmax_t, long long>> listing(const fs::path &root)
{
    std::map<std::string, std::pair<std::uintmax_t, long long>> out;
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        out[pathToUtf8(entry.path())] = {entry.file_size(ec),
                                         static_cast<long long>(entry.last_write_time(ec).time_since_epoch().count())};
    }
    return out;
}

void printListing(const std::map<std::string, std::pair<std::uintmax_t, long long>> &l, const fs::path &root)
{
    for (const auto &[path, sm] : l) {
        std::cerr << "      " << pathToUtf8(pathFromUtf8(path).lexically_relative(root)) << "  " << sm.first << " bytes  mtime "
                  << sm.second << "\n";
    }
}

std::shared_ptr<TrackFieldEditChange> ratingChange(const FixtureStick &s, const std::string &file,
                                                   std::optional<int> stars)
{
    return std::make_shared<TrackFieldEditChange>(s.pioneerQ(), s.engineQ(), file, "T",
                                                  TrackFieldValue{TrackField::Rating, stars, {}},
                                                  findTrackRows(s.pioneerQ(), s.engineQ(), file));
}

std::shared_ptr<TrackFieldEditChange> commentChange(const FixtureStick &s, const std::string &file,
                                                    const std::string &text, QString pioneer = {})
{
    const QString root = pioneer.isNull() ? s.pioneerQ() : pioneer;
    return std::make_shared<TrackFieldEditChange>(root, s.engineQ(), file, "T",
                                                  TrackFieldValue{TrackField::Comment, std::nullopt, text},
                                                  findTrackRows(root, s.engineQ(), file));
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
        const CommentLimit limit = commentLimitFor(s.pioneerQ(), findTrackRows(s.pioneerQ(), s.engineQ(), track.filePath));
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
                const CommentLimit none = commentLimitFor(s.pioneerQ(), findTrackRows(s.pioneerQ(), s.engineQ(), t.filePath));
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
        assert(!commentLimitFor(s.pioneerQ(), findTrackRows(s.pioneerQ(), s.engineQ(), file)).limited);
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
    // 8. export.pdb spells the path in another case than OneLibrary and
    //    Engine (#103's FAT stick): an edit asked for by Engine's row
    //    reaches export.pdb too, and its comment limit comes from it.
    if (runs(8)) {
        FixtureStick s(fixture, "seabass_track_edit_case");
        const auto [track, capacity] = roomiestComment(s, false);
        const Copies copies = s.copiesOf(track.filePath);
        const std::string stored = oneLibraryPathOf(s, track.filePath);
        const std::string upper = upperFolder(stored);
        assert(upper != stored);
        oneLibraryExec(s, "UPDATE content SET path = ? WHERE path = ?", {upper, stored});
        const std::string engineRelative =
            pathToGenericUtf8(pathFromUtf8(pathToUtf8(s.stick) + upper).lexically_relative(s.engine));
        assert(seabass::testing::sqlExec(pathToUtf8(s.mdb()), "UPDATE Track SET path = '" + engineRelative
                                                                   + "' WHERE id = " + copies.at("engine").sourceId + ";"));
        // The file as Browse names it when it shows Engine's row.
        const std::string engineFile =
            pathToUtf8((s.engine / pathFromUtf8(engineRelative)).lexically_normal());
        const TrackRows rows = findTrackRows(s.pioneerQ(), s.engineQ(), engineFile);
        assert(rows.rekordbox.state == CatalogRows::State::Listed && "export.pdb's row is found, case folded");
        assert(rows.oneLibrary.state == CatalogRows::State::Listed && rows.engine.state == CatalogRows::State::Listed);
        assert(rows.refusal().empty());
        const CommentLimit limit = commentLimitFor(s.pioneerQ(), rows);
        assert(limit.limited && limit.units == capacity.units && "the limit is export.pdb's, not none");
        const int stars = copies.at("rekordbox").rating.value_or(0) == 4 ? 2 : 4;
        const auto result = saveChanges({ratingChange(s, engineFile, stars)}, s.pioneerQ(), s.engineQ());
        assert(result.error.isEmpty());
        for (const auto &[format, t] : s.copiesOf(engineFile)) {
            assert(t.rating == std::optional<int>(stars) && "every catalog took it, export.pdb included");
        }
        std::cout << "case 8 (export.pdb spelling the path in another case is found and written) OK\n";
    }

    // 9. OneLibrary lists the file twice, in two spellings: which row is
    //    the track cannot be known, so the edit is refused when it is
    //    looked up, and a save staged before that fails and writes nothing.
    if (runs(9)) {
        FixtureStick s(fixture, "seabass_track_edit_ambiguous");
        const std::string file = s.threeCatalogTrack().filePath;
        const auto staged = ratingChange(s, file, 5);
        setRatings(s, file, 1, 2, 3);
        const std::string stored = oneLibraryPathOf(s, file);
        // Another row of OneLibrary's pointed at the same file, its folder
        // in upper case.
        oneLibraryExec(s, "UPDATE content SET path = ? WHERE content_id = (SELECT min(content_id) FROM content WHERE "
                          "path <> ?)", {upperFolder(stored), stored});
        const TrackRows rows = findTrackRows(s.pioneerQ(), s.engineQ(), file);
        assert(rows.oneLibrary.state == CatalogRows::State::Ambiguous);
        assert(rows.refusal().find("more than one spelling") != std::string::npos);
        const auto result = saveChanges({staged}, s.pioneerQ(), s.engineQ());
        assert(!result.error.isEmpty() && result.error.contains("more than one spelling"));
        expectRatings(s, file, 1, 2, 3);
        std::cout << "case 9 (two OneLibrary rows in two spellings: refused, nothing written) OK\n";
    }

    // 10. A catalog that listed the track at staging and does not at Save
    //     fails the save whole, saying which; each catalog in turn, the
    //     first (OneLibrary), the second (Engine) and the last (export.pdb),
    //     and every catalog keeps its own, different, rating.
    if (runs(10)) {
        for (const std::string lost : {"onelibrary", "engine", "rekordbox"}) {
            FixtureStick s(fixture, "seabass_track_edit_lost_" + lost);
            const std::string file = s.threeCatalogTrack().filePath;
            const auto staged = ratingChange(s, file, 5);
            setRatings(s, file, 1, 2, 3);
            const Copies copies = s.copiesOf(file);
            if (lost == "onelibrary") {
                oneLibraryExec(s, "UPDATE content SET path = '/Contents/gone.mp3' WHERE content_id = "
                                  + copies.at("onelibrary").sourceId);
            } else if (lost == "engine") {
                assert(seabass::testing::sqlExec(pathToUtf8(s.mdb()), "UPDATE Track SET path = '../Contents/gone.mp3' "
                                                                       "WHERE id = " + copies.at("engine").sourceId + ";"));
            } else {
                infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pdb()));
                assert(pdb.removeTrack(static_cast<uint32_t>(std::stoul(copies.at("rekordbox").sourceId))));
                assert(pdb.commit());
            }
            const std::string name = lost == "onelibrary" ? "OneLibrary" : lost == "engine" ? "Engine" : "export.pdb";
            const auto result = saveChanges({staged}, s.pioneerQ(), s.engineQ());
            if (!result.error.contains(QString::fromStdString(name + " no longer lists"))) {
                std::cerr << lost << ": " << result.error.toStdString() << "\n";
                assert(false && "the save fails naming the catalog");
            }
            assert(result.rolledBack);
            const Copies after = s.copiesOf(file);
            if (lost != "rekordbox") {
                assert(after.at("rekordbox").rating == std::optional<int>(1));
            }
            if (lost != "onelibrary") {
                assert(after.at("onelibrary").rating == std::optional<int>(2) && "OneLibrary, written first, put back");
            }
            if (lost != "engine") {
                assert(after.at("engine").rating == std::optional<int>(3) && "Engine put back");
            }
        }
        std::cout << "case 10 (a catalog losing the row between staging and Save fails the save, first, second or last) "
                     "OK\n";
    }

    // 11. The last write fails (export.pdb's replace meets a folder) with
    //     every catalog holding a rating of its own: all three keep theirs.
    if (runs(11)) {
        FixtureStick s(fixture, "seabass_track_edit_rollback_distinct");
        const std::string file = s.threeCatalogTrack().filePath;
        setRatings(s, file, 1, 2, 3);
        const auto staged = ratingChange(s, file, 5);
        const fs::path obstacle = pathFromUtf8(pathToUtf8(s.pdb()) + ".tmp-seabass-write");
        write(obstacle / "in the way", "x");
        const auto result = saveChanges({staged}, s.pioneerQ(), s.engineQ());
        fs::remove_all(obstacle);
        assert(!result.error.isEmpty() && result.rolledBack);
        expectRatings(s, file, 1, 2, 3);
        std::cout << "case 11 (export.pdb failing last: OneLibrary and Engine, each with its own rating, put back) OK\n";
    }

    // 12. Rating and comment of one track in one save, read back in all
    //     three; and with a playlist created in the same save.
    if (runs(12)) {
        FixtureStick s(fixture, "seabass_track_edit_both");
        const auto [track, capacity] = roomiestComment(s, false);
        const std::string file = track.filePath;
        const int stars = track.rating.value_or(0) == 3 ? 4 : 3;
        const QStringList all{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};
        const auto result = saveChanges({ratingChange(s, file, stars), commentChange(s, file, "both"),
                                         std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.engineQ(), "Edited",
                                                                                all)},
                                        s.pioneerQ(), s.engineQ());
        if (!result.error.isEmpty()) {
            std::cerr << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && result.appliedIds.size() == 3);
        expectRatingEverywhere(s, file, stars);
        expectCommentEverywhere(s, file, "both");
        bool made = false;
        for (const auto &n : infrastructure::rekordbox::PdbRowWriter(pathToUtf8(s.pdb())).playlistTree()) {
            made = made || n.name == "Edited";
        }
        assert(made && "the playlist edit in the same save landed too");
        std::cout << "case 12 (rating, comment and a playlist edit in one save) OK\n";
    }

    // 13. Twenty tracks edited in one save replace export.pdb once, not
    //     twenty times (the session's scratch copy), and a failure on the
    //     last of them still rolls every one back.
    if (runs(13)) {
        const auto durableWrites = [&](int tracks, bool failLast) {
            FixtureStick s(fixture, "seabass_track_edit_batch_" + std::to_string(tracks) + (failLast ? "f" : ""));
            const auto candidates = s.inBothRekordbox();
            assert(candidates.size() >= static_cast<size_t>(tracks));
            std::vector<std::shared_ptr<PendingChange>> changes;
            std::vector<std::pair<std::string, std::optional<int>>> before;
            for (int i = 0; i < tracks; ++i) {
                const auto &t = candidates[static_cast<size_t>(i)];
                before.emplace_back(t.filePath, t.rating);
                changes.push_back(ratingChange(s, t.filePath, t.rating.value_or(0) == 5 ? 4 : 5));
            }
            if (failLast) {
                // The last track's export.pdb row goes before the save.
                infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pdb()));
                assert(pdb.removeTrack(static_cast<uint32_t>(std::stoul(candidates[static_cast<size_t>(tracks - 1)].sourceId))));
                assert(pdb.commit());
            }
            const std::string pdbBefore = fileBytes(s.pdb());
            infrastructure::WorkCounters::instance().reset();
            const auto result = saveChanges(changes, s.pioneerQ(), s.engineQ());
            const auto writes = infrastructure::WorkCounters::instance().snapshot().durableFileWrites;
            // The writes went to a local scratch copy of each database, put
            // onto the stick once at the end, when there were enough of them.
            std::ifstream logFile(s.stick / "Seabass" / "seabass.log");
            const std::string log((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
            const bool scratched = log.find("applying up to " + std::to_string(tracks)
                                            + " rekordbox update(s) to a local scratch copy")
                                   != std::string::npos;
            assert(scratched == (tracks >= 15) && "a batch goes through one scratch copy of export.pdb");
            if (failLast) {
                assert(!result.error.isEmpty() && result.rolledBack);
                assert(fileBytes(s.pdb()) == pdbBefore && "every edit before the failing one was put back");
            } else {
                assert(result.error.isEmpty() && result.appliedIds.size() == static_cast<qsizetype>(tracks));
            }
            return writes;
        };
        const auto one = durableWrites(1, false);
        const auto twenty = durableWrites(20, false);
        // Durable writes count the scratch copy's too (each change commits
        // into it, on this computer's disk): 20 there, and then one per
        // database onto the stick.
        std::cerr << "    durable writes: 1 track " << one << ", 20 tracks " << twenty << "\n";
        durableWrites(20, true);
        std::cout << "case 13 (twenty edits, one replace; a failure on the last rolls all back) OK\n";
    }

    // 14. The fixture's Engine library is schema 3.x; a 2.x one is written
    //     the same way, and a cleared rating reads back as none; an Engine
    //     1.x library (m.db at its top level) is refused, not skipped.
    if (runs(14)) {
        FixtureStick s(fixture, "seabass_track_edit_schema");
        assert(sqlScalar(pathToUtf8(s.mdb()), "SELECT schemaVersionMajor FROM Information;") == 3);
        for (const bool v2 : {true, false}) {
            const fs::path root = seabass::testing::scratchRoot() / (v2 ? "seabass_track_edit_v2" : "seabass_track_edit_v1");
            fs::remove_all(root);
            const fs::path library = root / "Engine Library";
            fs::create_directories(root);
            {
                auto db = djinterop::engine::create_database(
                    pathToUtf8(library), v2 ? djinterop::engine::engine_schema::schema_2_21_2 : djinterop::engine::latest_v1_schema);
                djinterop::track_snapshot snapshot;
                snapshot.title = "Old";
                snapshot.relative_path = "../Contents/old.mp3";
                snapshot.rating = 40;
                db.create_track(snapshot);
            }
            const std::string file = pathToUtf8((library / "../Contents/old.mp3").lexically_normal());
            const QString engine = pathToQString(library);
            const TrackRows rows = findTrackRows({}, engine, file);
            if (!v2) {
                assert(rows.engine.state == CatalogRows::State::Unwritable && !rows.refusal().empty());
                std::cout << "    Engine 1.x: refused (" << rows.refusal() << ")\n";
                fs::remove_all(root);
                continue;
            }
            assert(sqlScalar(pathToUtf8(library / "Database2" / "m.db"), "SELECT schemaVersionMajor FROM Information;") == 2);
            assert(rows.engine.state == CatalogRows::State::Listed);
            for (const std::optional<int> stars : {std::optional<int>(5), std::optional<int>()}) {
                auto change = std::make_shared<TrackFieldEditChange>(QString(), engine, file, "Old",
                                                                     TrackFieldValue{TrackField::Rating, stars, {}}, rows);
                assert(saveChanges({change}, {}, engine).error.isEmpty());
                infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(library));
                const auto tracks = application::ScanLibrary(reader).execute();
                assert(tracks.size() == 1 && tracks[0].rating == stars);
            }
            fs::remove_all(root);
        }
        std::cout << "case 14 (Engine 3.x fixture, 2.x written and cleared, 1.x refused) OK\n";
    }

    // 15. An Engine 1.x library (m.db at the library folder's top
    //     level): the stick scan hands no Engine path over for it, so it
    //     is found from the stick's root; the edit is refused, at lookup
    //     and again at Save for an edit staged before it appeared.
    if (runs(15)) {
        FixtureStick s(fixture, "seabass_track_edit_engine1x");
        const std::string file = s.inBothRekordbox().front().filePath;
        fs::remove_all(s.engine);
        application::DetectedStick before;
        infrastructure::media::scanMountedRoot(pathToUtf8(s.stick), before);
        assert(before.rekordboxPath && !before.enginePath);
        const QString pioneer = QString::fromStdString(*before.rekordboxPath);
        const auto staged = std::make_shared<TrackFieldEditChange>(pioneer, QString(), file, "T",
                                                                   TrackFieldValue{TrackField::Rating, 4, {}},
                                                                   findTrackRows(pioneer, QString(), file));
        {
            auto db = djinterop::engine::create_database(pathToUtf8(s.engine), djinterop::engine::latest_v1_schema);
        }
        application::DetectedStick scanned;
        infrastructure::media::scanMountedRoot(pathToUtf8(s.stick), scanned);
        assert(scanned.rekordboxPath && !scanned.enginePath && "the scan reports no Engine path for a 1.x library");
        const QString engineFromScan = scanned.enginePath ? QString::fromStdString(*scanned.enginePath) : QString();
        const TrackRows rows = findTrackRows(QString::fromStdString(*scanned.rekordboxPath), engineFromScan, file);
        assert(rows.engine.state == CatalogRows::State::Unwritable);
        assert(rows.refusal().find("Engine 1.x") != std::string::npos);
        const std::string pdbBefore = fileBytes(s.pdb());
        const auto result = saveChanges({staged}, pioneer, engineFromScan);
        assert(!result.error.isEmpty() && result.error.contains("Engine 1.x"));
        assert(fileBytes(s.pdb()) == pdbBefore && "nothing written");
        std::cout << "case 15 (an Engine 1.x library found from the stick root: refused at lookup and at Save) OK\n";
    }

    // 16. A track only export.pdb lists: OneLibrary and Engine are not
    //     opened for writing, not backed up for it, not rewritten, even in
    //     a batch of 15 that puts export.pdb through a scratch copy; and
    //     the origin ledger is told only of what reached export.pdb.
    if (runs(16)) {
        FixtureStick s(fixture, "seabass_track_edit_pdb_only");
        // Engine's import counter level with export.pdb, as any earlier
        // Seabass save leaves a stick: every save levels it otherwise, and
        // that write is not this change's.
        {
            const auto state =
                infrastructure::engine::readRekordboxImportState(pathToUtf8(s.engine), pathToUtf8(s.pioneer));
            assert(state.error.empty());
            std::string error;
            assert(infrastructure::engine::markRekordboxLibraryImported(pathToUtf8(s.engine), state.librarySequence,
                                                                         &error));
        }
        std::vector<std::shared_ptr<PendingChange>> changes;
        std::vector<std::string> files;
        for (const auto &t : s.inBothRekordbox()) {
            if (files.size() == 15) {
                break;
            }
            if (s.copiesOf(t.filePath).count("engine")) {
                continue;
            }
            const std::string stored = oneLibraryPathOf(s, t.filePath);
            oneLibraryExec(s, "UPDATE content SET path = ? WHERE path = ?", {"/Elsewhere" + stored, stored});
            assert(s.copiesOf(t.filePath).size() == 1);
            files.push_back(t.filePath);
        }
        for (const auto &f : files) {
            changes.push_back(ratingChange(s, f, 5));
        }
        const fs::path oneDb = pathFromUtf8(s.oneLibraryDb());
        const auto oneBefore = listing(oneDb.parent_path());
        const auto engineDump = seabass::testing::dumpDatabase(pathToUtf8(s.mdb()));
        infrastructure::WorkCounters::instance().reset();
        const auto result = saveChanges(changes, s.pioneerQ(), s.engineQ());
        assert(result.error.isEmpty());
        // Fifteen tracks OneLibrary does not list, checked against one read
        // of it for the whole save, not one copy of the database each.
        const auto opens = infrastructure::WorkCounters::instance().snapshot().encryptedDatabaseOpens;
        if (opens != 1) {
            std::cerr << "    OneLibrary opened " << opens << " times\n";
        }
        assert(opens == 1 && "one read of OneLibrary for the save");
        // OneLibrary's files byte for byte and time for time as they were
        // (export.pdb, in the same folder, was written and is left out).
        auto oneAfter = listing(oneDb.parent_path());
        auto oneBeforeNoPdb = oneBefore;
        oneAfter.erase(pathToUtf8(s.pdb()));
        oneBeforeNoPdb.erase(pathToUtf8(s.pdb()));
        if (oneAfter != oneBeforeNoPdb) {
            std::cerr << "    before:\n";
            printListing(oneBeforeNoPdb, s.stick);
            std::cerr << "    after:\n";
            printListing(oneAfter, s.stick);
            assert(false && "OneLibrary is untouched, times included");
        }
        // Engine: no track row touched. The save does write one row of it,
        // Information's import counter, because export.pdb changed and
        // Engine must be told the change was Seabass's (save_loop.cpp's
        // settleImportLevel; every save that writes export.pdb does it),
        // so the file itself changes and is backed up under that step.
        const auto engineDiffs =
            seabass::testing::differences(engineDump, seabass::testing::dumpDatabase(pathToUtf8(s.mdb())));
        for (const auto &d : engineDiffs) {
            if (d.table != "Information") {
                seabass::testing::printDifferences(engineDiffs);
                assert(false && "only Engine's import counter changed");
            }
        }
        for (const auto &backup : result.backups) {
            infrastructure::backup::FilesystemBackupStore store(backup.backupDir.toStdString());
            for (const auto &record : store.list()) {
                for (const auto &path : record.filePaths) {
                    assert(key(path) != key(s.oneLibraryDb()) && "no backup of OneLibrary, which nothing wrote");
                    if (record.label == "track-edit") {
                        assert(key(path) != key(pathToUtf8(s.mdb())) && "this change backed up no Engine database");
                    }
                }
            }
        }
        for (const auto &f : files) {
            assert(s.copiesOf(f).at("rekordbox").rating == std::optional<int>(5));
        }
        std::cout << "case 16 (a track only export.pdb lists leaves OneLibrary and Engine untouched) OK\n";
    }

    // 17. A OneLibrary-only stick: the rating lands there, and nothing is
    //     reported to the origin ledger, which records export.pdb's.
    if (runs(17)) {
        FixtureStick s(fixture, "seabass_track_edit_onelibrary_only");
        const std::string file = s.inBothRekordbox().front().filePath;
        fs::remove(s.pdb());
        fs::remove_all(s.engine);
        const auto change = std::make_shared<TrackFieldEditChange>(
            s.pioneerQ(), QString(), file, "T", TrackFieldValue{TrackField::Rating, 4, {}},
            findTrackRows(s.pioneerQ(), QString(), file));
        const auto result = saveChanges({change}, s.pioneerQ(), QString());
        assert(result.error.isEmpty());
        infrastructure::onelibrary::OneLibraryReader reader(pathToUtf8(s.pioneer));
        bool found = false;
        for (const auto &t : application::ScanLibrary(reader).execute()) {
            if (key(t.filePath) == key(file)) {
                assert(t.rating == std::optional<int>(4));
                found = true;
            }
        }
        assert(found);
        assert(change->rekordboxWrites().empty() && "export.pdb was not written, so no rekordbox rating is reported");
        std::cout << "case 17 (OneLibrary only: written, and no rekordbox write reported) OK\n";
    }

    // 18. The stick's index, read for the track details: a lookup leaves
    //     the stick byte for byte as it was, its -wal and -shm included,
    //     and how long it takes. TRACK_EDIT_MEASURE=<stick root> measures
    //     a copy of another stick the same way.
    if (runs(18)) {
        const auto measure = [](const fs::path &stick, const std::string &name) {
            const QString pioneer = pathToQString(stick / "PIONEER");
            const QString engine = fs::exists(stick / "Engine Library" / "Database2" / "m.db")
                ? pathToQString(stick / "Engine Library") : QString();
            const auto before = listing(stick);
            const auto start = std::chrono::steady_clock::now();
            const StickPathIndex index = buildStickPathIndex(pioneer, engine);
            const auto built = std::chrono::steady_clock::now();
            infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(stick / "PIONEER"));
            const auto tracks = application::ScanLibrary(reader).execute();
            const auto selectStart = std::chrono::steady_clock::now();
            int editable = 0;
            for (const auto &t : tracks) {
                const TrackRows rows = index.rowsFor(t.filePath);
                (void)index.limitFor(rows);
                editable += rows.refusal().empty() ? 1 : 0;
            }
            const auto selectEnd = std::chrono::steady_clock::now();
            const auto after = listing(stick);
            const double buildMs = std::chrono::duration<double, std::milli>(built - start).count();
            const double perSelectionUs =
                std::chrono::duration<double, std::micro>(selectEnd - selectStart).count() / std::max<size_t>(1, tracks.size());
            std::cerr << "    " << name << ": index built in " << buildMs << " ms; " << tracks.size()
                      << " export.pdb tracks answered at " << perSelectionUs << " us each, " << editable
                      << " editable\n";
            if (before != after) {
                std::cerr << "    the stick changed:\n";
                for (const auto &[path, sm] : after) {
                    const auto was = before.find(path);
                    if (was == before.end() || was->second != sm) {
                        std::cerr << "      " << pathToUtf8(pathFromUtf8(path).lexically_relative(stick)) << ": "
                                  << (was == before.end() ? std::string("new")
                                                          : "size " + std::to_string(was->second.first) + " -> "
                                                                + std::to_string(sm.first) + ", mtime "
                                                                + (was->second.second == sm.second ? "same" : "changed"))
                                  << "\n";
                    }
                }
                for (const auto &[path, sm] : before) {
                    if (!after.count(path)) {
                        std::cerr << "      " << pathToUtf8(pathFromUtf8(path).lexically_relative(stick)) << ": removed\n";
                    }
                }
            }
            return before == after;
        };
        if (const char *other = std::getenv("TRACK_EDIT_MEASURE")) {
            const fs::path copy = seabass::testing::scratchRoot() / "seabass_track_edit_measure";
            fs::remove_all(copy);
            for (const char *part : {"PIONEER/rekordbox", "Engine Library/Database2"}) {
                fs::create_directories(copy / part);
                for (const auto &entry : fs::directory_iterator(pathFromUtf8(other) / part)) {
                    if (entry.is_regular_file()) {
                        fs::copy_file(entry.path(), copy / part / entry.path().filename(),
                                      fs::copy_options::overwrite_existing);
                    }
                }
            }
            const bool unchanged = measure(copy, std::string("copy of ") + other);
            std::cerr << "    " << (unchanged ? "unchanged" : "CHANGED") << " by the lookup\n";
            fs::remove_all(copy);
            return unchanged ? 0 : 1;
        }
        FixtureStick s(fixture, "seabass_track_edit_index");
        assert(!infrastructure::isWalModeSqlite(s.mdb()) && "the fixture's m.db keeps a rollback journal");
        // OneLibrary as rekordbox leaves it while it has it open: in WAL
        // mode, with a row only its -wal holds. Made here, on the copy, so
        // the test owns its precondition: a connection is held open with
        // checkpoints off, and one row's path is moved in it.
        const fs::path wal = pathFromUtf8(s.oneLibraryDb() + "-wal");
        {
            infrastructure::onelibrary::SqlCipherLibrary lib;
            auto held = std::make_unique<infrastructure::onelibrary::SqlCipherDb>(lib, s.oneLibraryDb(), false);
            held->exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
            {
                infrastructure::onelibrary::SqlCipherStatement mode(*held, "PRAGMA journal_mode;");
                assert(mode.step() && mode.columnText(0) == "wal" && "OneLibrary is in WAL mode");
            }
            held->exec("PRAGMA wal_autocheckpoint = 0;");
            held->exec("UPDATE content SET path = '/Contents/only-in-the-wal.mp3' WHERE content_id = "
                       "(SELECT min(content_id) FROM content);");
            std::error_code ec;
            assert(fs::is_regular_file(wal, ec) && fs::file_size(wal, ec) > 0 && "the moved row is in the -wal");
            const StickPathIndex fromWal = buildStickPathIndex(s.pioneerQ(), s.engineQ());
            assert(fromWal.rowsFor(pathToUtf8(s.stick / "Contents" / "only-in-the-wal.mp3")).oneLibrary.state
                       == CatalogRows::State::Listed
                   && "the index reads the row the -wal holds");
            assert(measure(s.stick, pathToUtf8(fixture.filename()) + ", OneLibrary with a -wal")
                   && "a lookup leaves the stick as it was");
            held.reset();
        }
        // OneLibrary with no -wal or -shm beside it (the held connection,
        // closed, checkpointed the -wal into the database), and Engine's
        // m.db switched to WAL mode: still untouched, and still read whole.
        fs::remove(wal);
        fs::remove(pathFromUtf8(s.oneLibraryDb() + "-shm"));
        // A path Engine lists, read before the switch: once m.db is in
        // WAL mode with no -wal beside it (the switch's connection took
        // the -wal with it when it closed), macOS's SQLite cannot open
        // it read-only in place, which is the very case the index reads
        // from a copy for (database_read_copy.hpp).
        const auto engineRows = infrastructure::engine::engineTrackPaths(pathToUtf8(s.engine));
        assert(!engineRows.empty());
        assert(seabass::testing::sqlExec(pathToUtf8(s.mdb()), "PRAGMA journal_mode=WAL;"));
        assert(infrastructure::isWalModeSqlite(s.mdb()) && "m.db is in WAL mode now");
        assert(!fs::exists(pathFromUtf8(pathToUtf8(s.mdb()) + "-wal")) && "and has no -wal beside it");
        const auto walIndex = buildStickPathIndex(s.pioneerQ(), s.engineQ());
        const std::string engineFile =
            pathToUtf8((s.engine / pathFromUtf8(engineRows.front().second)).lexically_normal());
        assert(walIndex.rowsFor(engineFile).engine.state == CatalogRows::State::Listed && "WAL m.db read whole");
        // Still none: the index read the copy, not the stick. Were a
        // -wal here now, the measurement below would copy it and never
        // meet the bare-copy case the index reads from a copy for.
        assert(!fs::exists(pathFromUtf8(pathToUtf8(s.mdb()) + "-wal")) && "the index build wrote no -wal");
        assert(measure(s.stick, "the same, OneLibrary without a -wal and m.db in WAL mode")
               && "a lookup leaves a WAL-mode m.db and a -wal-less OneLibrary as they were");
        // A copy a crashed run left in the temporary folder goes when the
        // index is next read; anything else there, and a copy made since
        // this process started, stays.
        const fs::path temp = fs::temp_directory_path();
        const fs::path stale = temp / (std::string(infrastructure::ReadCopyPrefix) + "left-by-a-crash");
        const fs::path fresh = temp / (std::string(infrastructure::ReadCopyPrefix) + "made-just-now");
        const fs::path other = temp / "seabass-something-else";
        // Owner-tagged, as copies are named now: one whose process still
        // runs (this one, made an hour "ago", as another Seabass started
        // earlier would have) stays whatever its age; one whose process is
        // gone goes.
        const fs::path live = temp
            / (std::string(infrastructure::ReadCopyPrefix) + infrastructure::readCopyOwnerTag() + "1-0");
        const fs::path orphan = temp / (std::string(infrastructure::ReadCopyPrefix) + "999999999-12345-1-0");
        assert(!infrastructure::system::isProcessAlive(999999999, 12345) && "no such process");
        for (const auto &dir : {stale, fresh, other, live, orphan}) {
            fs::create_directories(dir / "rekordbox");
        }
        for (const auto &dir : {stale, other, live}) {
            fs::last_write_time(dir, infrastructure::readCopiesSince() - std::chrono::hours(1));
        }
        (void)buildStickPathIndex(s.pioneerQ(), s.engineQ());
        assert(!fs::exists(stale) && "an untagged copy older than this process is removed");
        assert(!fs::exists(orphan) && "a copy whose process is gone is removed");
        assert(fs::exists(live) && "a copy a running process owns stays, however old");
        assert(fs::exists(fresh) && fs::exists(other) && "nothing else is");
        fs::remove_all(fresh);
        fs::remove_all(other);
        fs::remove_all(live);
        std::cout << "case 18 (reading the stick's index changes no file on it) OK\n";
    }

    return 0;
}
