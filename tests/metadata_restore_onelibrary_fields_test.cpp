// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What Metadata Restore offers for a track once the OneLibrary reader
// reads rating and comment (#105), against what it offered when the
// reader left them out ("before": the same rows with OneLibrary's two
// fields blank, which is what that reader returned).
//
//   metadata_restore_onelibrary_fields_test <tests/fixtures/one_stick_two_catalogs>
//
// The stick's three catalogs fold into one file, first non-empty value in
// the order rekordbox, OneLibrary, Engine (collapseCatalogRows), and that
// value meets the store's by the shared merge rule. The rule kept: a case
// changes only where the catalogs genuinely hold different values, which
// the old reader could not see; agreement restores exactly as before.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/collapse_catalog_rows.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/track.hpp"
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
namespace fs = std::filesystem;

namespace
{

std::string key(const std::string &p)
{
    return application::normalizedPathKey(pathToUtf8(pathFromUtf8(p).lexically_normal()));
}

struct Stick
{
    fs::path root, pioneer, engine;
    std::string file;
    std::string pdbId, oneId, engineId;
};

std::vector<domain::Track> readAll(const Stick &s)
{
    std::vector<domain::Track> rows;
    infrastructure::rekordbox::KaitaiRekordboxReader rb(pathToUtf8(s.pioneer));
    for (auto &t : application::ScanLibrary(rb).execute()) {
        rows.push_back(std::move(t));
    }
    infrastructure::onelibrary::OneLibraryReader one(pathToUtf8(s.pioneer));
    for (auto &t : application::ScanLibrary(one).execute()) {
        rows.push_back(std::move(t));
    }
    infrastructure::engine::LibdjinteropEngineReader en(pathToUtf8(s.engine));
    for (auto &t : application::ScanLibrary(en).execute()) {
        rows.push_back(std::move(t));
    }
    return rows;
}

void oneLibraryExec(const Stick &s, const std::string &sql, const std::vector<std::string> &texts = {})
{
    infrastructure::onelibrary::SqlCipherLibrary lib;
    infrastructure::onelibrary::SqlCipherDb db(lib, infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(s.pioneer)),
                                               false);
    db.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    infrastructure::onelibrary::SqlCipherStatement st(db, sql);
    int i = 1;
    for (const auto &t : texts) {
        st.bindText(i++, t);
    }
    st.run();
}

// A copy of the fixture with one file in all three catalogs: a track
// export.pdb and OneLibrary list, and an Engine row pointed at it.
Stick makeStick(const fs::path &fixture, const std::string &name)
{
    Stick s;
    s.root = seabass::testing::scratchRoot() / name;
    fs::remove_all(s.root);
    s.pioneer = s.root / "PIONEER";
    s.engine = s.root / "Engine Library";
    std::error_code ec;
    seabass::testing::copyPioneerFixture(fixture / "rekordbox", s.pioneer, ec);
    assert(!ec);
    fs::copy(fixture / "engine", s.engine, fs::copy_options::recursive, ec);
    assert(!ec);
    std::map<std::string, std::map<std::string, std::string>> byFile;  // key -> format -> id
    std::map<std::string, std::string> pathOf;
    for (const auto &t : readAll(s)) {
        if (!t.filePath.empty()) {
            byFile[key(t.filePath)][t.format] = t.sourceId;
            if (t.format == "rekordbox") {
                pathOf[key(t.filePath)] = t.filePath;
            }
        }
    }
    for (const auto &[k, formats] : byFile) {
        if (formats.count("rekordbox") && formats.count("onelibrary") && formats.count("engine")) {
            s.file = pathOf[k];
            s.pdbId = formats.at("rekordbox");
            s.oneId = formats.at("onelibrary");
            s.engineId = formats.at("engine");
            return s;  // Engine lists it already
        }
    }
    for (const auto &[k, formats] : byFile) {
        if (formats.count("rekordbox") && formats.count("onelibrary")) {
            s.file = pathOf[k];
            s.pdbId = formats.at("rekordbox");
            s.oneId = formats.at("onelibrary");
            break;
        }
    }
    assert(!s.file.empty());
    const std::string db = pathToUtf8(s.engine / "Database2" / "m.db");
    s.engineId = seabass::testing::sqlRows(db, "SELECT id FROM Track ORDER BY id LIMIT 1;").at(0).at(0);
    const std::string relative = pathToGenericUtf8(pathFromUtf8(s.file).lexically_relative(s.engine));
    assert(seabass::testing::sqlExec(db, "UPDATE Track SET path = '" + relative + "' WHERE id = " + s.engineId + ";"));
    return s;
}

void setStick(const Stick &s, std::optional<int> pdbStars, std::optional<int> oneStars, std::optional<int> engineStars,
              const std::string &pdbComment, const std::string &oneComment, const std::string &engineComment)
{
    {
        infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(s.pioneer / "rekordbox" / "export.pdb"));
        assert(pdb.setTrackRating(static_cast<uint32_t>(std::stoul(s.pdbId)), pdbStars.value_or(0)));
        std::string refusal;
        const auto room = pdb.trackCommentCapacity(static_cast<uint32_t>(std::stoul(s.pdbId)));
        if (room && room->units >= pdbComment.size()) {
            assert(pdb.setTrackComment(static_cast<uint32_t>(std::stoul(s.pdbId)), pdbComment, &refusal));
        } else {
            assert(pdbComment.empty() && "export.pdb takes the comment the case needs");
        }
        assert(pdb.commit());
    }
    oneLibraryExec(s, "UPDATE content SET rating = " + std::to_string(oneStars.value_or(0)) + ", djComment = ? WHERE content_id = "
                          + s.oneId, {oneComment});
    const std::string db = pathToUtf8(s.engine / "Database2" / "m.db");
    assert(seabass::testing::sqlExec(db, "UPDATE Track SET rating = " + std::to_string(engineStars.value_or(0) * 20)
                                             + ", comment = '" + engineComment + "' WHERE id = " + s.engineId + ";"));
}

struct Offer
{
    bool rating = false;
    std::optional<int> ratingValue;
    bool ratingConflict = false;
    bool comment = false;
    std::string commentValue;
    bool commentConflict = false;
    std::optional<int> stickRating;
    std::string stickComment;
};

// What Restore offers for the file, with OneLibrary's two fields read
// (`after`) or blank as the old reader left them.
Offer offer(const Stick &s, bool after, std::optional<int> storedStars, const std::string &storedComment,
            std::int64_t storedAt, std::int64_t stickAt)
{
    auto rows = readAll(s);
    if (!after) {
        for (auto &r : rows) {
            if (r.format == "onelibrary") {
                r.rating.reset();
                r.comment.clear();
            }
        }
    }
    const auto files = application::collapseCatalogRows(rows);
    const domain::Track *stick = nullptr;
    for (const auto &f : files) {
        if (key(f.filePath) == key(s.file)) {
            stick = &f;
        }
    }
    assert(stick != nullptr);
    domain::Track stored = *stick;
    stored.format = "store";
    stored.sourceId = "1";
    stored.catalogRows.clear();
    stored.rating = storedStars;
    stored.comment = storedComment;
    stored.metadataModifiedAt = storedAt;
    Offer o;
    o.stickRating = stick->rating;
    o.stickComment = stick->comment;
    for (const auto &p : domain::planMetadataRestore(files, {stored}, stickAt)) {
        if (key(p.stickTrack.filePath) != key(s.file)) {
            continue;
        }
        o.rating = p.ratingOffered;
        o.ratingValue = p.rating;
        o.ratingConflict = p.ratingConflict;
        o.comment = p.commentOffered;
        o.commentValue = p.comment;
        o.commentConflict = p.commentConflict;
    }
    return o;
}

void print(const std::string &what, const Offer &o)
{
    std::cerr << "    " << what << ": stick reads rating " << (o.stickRating ? std::to_string(*o.stickRating) : "none")
              << ", comment \"" << o.stickComment << "\"; rating " << (o.rating ? "offered" : "not offered")
              << (o.ratingConflict ? " (conflict)" : "") << ", comment " << (o.comment ? "offered" : "not offered")
              << (o.commentConflict ? " (conflict)" : "") << "\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: metadata_restore_onelibrary_fields_test <tests/fixtures/one_stick_two_catalogs>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    constexpr std::int64_t Older = 1000;
    constexpr std::int64_t Newer = 2000;

    // 1. export.pdb 0, OneLibrary 3, Engine none; the store 4. Before, the
    //    stick read as unrated and 4 was a blank filled, written over the
    //    3 OneLibrary had and nobody saw. Now the stick reads 3, which is
    //    a real disagreement with the store, settled by the merge rule:
    //    the more recent side wins. The catalogs themselves disagree here
    //    (export.pdb unrated, OneLibrary 3), so this is the one change, and
    //    it is the one the reader exists for.
    {
        const Stick s = makeStick(fixture, "seabass_restore_ol_rating");
        setStick(s, std::nullopt, 3, std::nullopt, "", "", "");
        const Offer before = offer(s, false, 4, "", Newer, Older);
        const Offer after = offer(s, true, 4, "", Newer, Older);
        const Offer afterStickNewer = offer(s, true, 4, "", Older, Newer);
        print("1 before", before);
        print("1 after, store newer", after);
        print("1 after, stick newer", afterStickNewer);
        assert(before.rating && !before.ratingConflict && before.ratingValue == 4 && "before: a blank filled");
        assert(after.stickRating == 3 && after.ratingConflict && after.rating && after.ratingValue == 4);
        assert(!afterStickNewer.rating && "the stick's newer 3 is kept (no proposal: nothing offered)");
        std::cout << "case 1 (export.pdb 0, OneLibrary 3, store 4) OK\n";
    }

    // 2. Comments: export.pdb empty, OneLibrary "x", Engine "y"; the store
    //    "z". Before, the stick read "y" (Engine's); now "x" (OneLibrary's,
    //    folded before Engine). Both a conflict with "z", as before.
    {
        const Stick s = makeStick(fixture, "seabass_restore_ol_comment");
        setStick(s, std::nullopt, std::nullopt, std::nullopt, "", "x", "y");
        const Offer before = offer(s, false, std::nullopt, "z", Newer, Older);
        const Offer after = offer(s, true, std::nullopt, "z", Newer, Older);
        print("2 before", before);
        print("2 after", after);
        assert(before.stickComment == "y" && before.commentConflict && before.comment);
        assert(after.stickComment == "x" && after.commentConflict && after.comment && after.commentValue == "z");
        std::cout << "case 2 (export.pdb \"\", OneLibrary \"x\", Engine \"y\", store \"z\") OK\n";
    }

    // 3. Agreement: every catalog 3 and "same", the store the same: nothing
    //    offered, before or after. The store 4: the same conflict both ways.
    {
        const Stick s = makeStick(fixture, "seabass_restore_ol_agree");
        setStick(s, 3, 3, 3, "", "same", "same");
        for (const bool after : {false, true}) {
            const Offer same = offer(s, after, 3, "same", Newer, Older);
            assert(!same.rating && !same.comment && !same.ratingConflict && !same.commentConflict);
            const Offer differs = offer(s, after, 4, "same", Newer, Older);
            assert(differs.ratingConflict && differs.rating && differs.ratingValue == 4);
        }
        std::cout << "case 3 (agreement restores as before) OK\n";
    }

    // 4. A blank everywhere but the store: a blank filled, before and after.
    {
        const Stick s = makeStick(fixture, "seabass_restore_ol_blank");
        setStick(s, std::nullopt, std::nullopt, std::nullopt, "", "", "");
        for (const bool after : {false, true}) {
            const Offer o = offer(s, after, 4, "z", Older, Newer);
            assert(o.rating && !o.ratingConflict && o.comment && !o.commentConflict);
        }
        std::cout << "case 4 (blanks are filled, before and after) OK\n";
    }
    return 0;
}
