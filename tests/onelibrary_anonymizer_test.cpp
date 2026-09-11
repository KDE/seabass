// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The OneLibrary scrubber had no test at all, and a miss here ships a
// DJ's real metadata inside an encrypted database nobody eyeballs.
//
// Every case works the same way: plant a distinctive secret into a copy
// of the committed fixture, run the anonymizer, and assert the secret is
// gone from every text column of every table -- not merely from the one
// it was planted in. Asserting "the value is gone" rather than "the code
// ran" is the point: the comment scrub used to target a column named
// "comment", which this schema does not have (it is "djComment"), so it
// ran happily and scrubbed nothing.
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

#include "infrastructure/onelibrary/onelibrary_anonymizer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::onelibrary;

namespace
{

struct Db
{
    SqlCipherLibrary lib;
    SqlCipherDb db;
    explicit Db(const fs::path &path, bool readOnly = false) : db(lib, path.string(), readOnly)
    {
        db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    }
};

std::vector<std::string> tablesOf(const SqlCipherDb &db)
{
    std::vector<std::string> out;
    SqlCipherStatement s(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    while (s.step()) {
        out.push_back(s.columnText(0));
    }
    return out;
}

std::vector<std::string> columnsOf(const SqlCipherDb &db, const std::string &table)
{
    std::vector<std::string> out;
    SqlCipherStatement s(db, "PRAGMA table_info(\"" + table + "\")");
    while (s.step()) {
        out.push_back(s.columnText(1));
    }
    return out;
}

// Searches every column of every table. A scrub that moved a secret from
// one column to another, or missed a copy of it, fails here.
std::string findAnywhere(const SqlCipherDb &db, const std::string &secret)
{
    for (const auto &table : tablesOf(db)) {
        for (const auto &column : columnsOf(db, table)) {
            try {
                SqlCipherStatement s(db, "SELECT count(*) FROM \"" + table + "\" WHERE \"" + column + "\" = ?");
                s.bindText(1, secret);
                if (s.step() && s.columnInt64(0) > 0) {
                    return table + "." + column;
                }
            } catch (const std::exception &) {
                // Not a comparable column in this schema; nothing to check.
            }
        }
    }
    return {};
}

void setValue(const SqlCipherDb &db, const std::string &sql, const std::string &value)
{
    SqlCipherStatement s(db, sql);
    s.bindText(1, value);
    s.run();
}

}  // namespace

int main(int argc, char **argv)
{
    const fs::path fixture = argc > 1
        ? fs::path(argv[1])
        : fs::path("tests/fixtures/anonymized_library/rekordbox/rekordbox/exportLibrary.db");
    if (!fs::is_regular_file(fixture)) {
        std::cerr << "fixture not found at " << fixture << " -- run from the repository root\n";
        return 1;
    }

    const fs::path root = seabass::testing::scratchRoot() / "seabass_onelibrary_anonymizer_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const fs::path db = root / "exportLibrary.db";
    fs::copy_file(fixture, db, fs::copy_options::overwrite_existing, ec);
    assert(!ec);

    // Each secret is planted in a different column, all in one pass, so
    // one anonymizer run proves all of them.
    const std::string title = "SECRET_TITLE_do_not_ship";
    const std::string comment = "SECRET_DJ_COMMENT_do_not_ship";
    const std::string isrc = "SECRETISRC01";
    const std::string tag = "SECRET_MYTAG_do_not_ship";
    const std::string artist = "SECRET_ARTIST_do_not_ship";
    const std::string playlist = "SECRET_PLAYLIST_do_not_ship";
    const std::string searchTitle = "SECRET_SEARCH_TITLE_do_not_ship";
    {
        Db handle(db);
        setValue(handle.db, "UPDATE content SET title = ? WHERE content_id = (SELECT MIN(content_id) FROM content)", title);
        setValue(handle.db, "UPDATE content SET djComment = ? WHERE content_id = (SELECT MIN(content_id) FROM content)", comment);
        setValue(handle.db, "UPDATE content SET isrc = ? WHERE content_id = (SELECT MIN(content_id) FROM content)", isrc);
        setValue(handle.db, "UPDATE content SET titleForSearch = ? WHERE content_id = (SELECT MIN(content_id) FROM content)", searchTitle);
        setValue(handle.db, "UPDATE myTag SET name = ? WHERE myTag_id = (SELECT MIN(myTag_id) FROM myTag)", tag);
        setValue(handle.db, "UPDATE artist SET name = ? WHERE artist_id = (SELECT MIN(artist_id) FROM artist)", artist);
        setValue(handle.db, "UPDATE playlist SET name = ? WHERE playlist_id = (SELECT MIN(playlist_id) FROM playlist)", playlist);
    }

    // Planting has to actually take, or every "it is gone" below is
    // satisfied by a value that was never there.
    {
        Db handle(db, true);
        for (const auto &secret : {title, comment, isrc, tag, artist, playlist, searchTitle}) {
            assert(!findAnywhere(handle.db, secret).empty());
        }
        std::cout << "case 1 (the secrets really are in the database before scrubbing) OK\n";
    }

    const auto result = anonymizeOneLibraryDatabase(db.string());
    assert(result.errorMessage.empty());
    assert(result.tracksScrubbed > 0);
    assert(result.artistsRenamed > 0);
    assert(result.myTagsRenamed > 0);
    std::cout << "case 2 (the anonymizer runs and reports work on every table) OK\n";

    {
        Db handle(db, true);
        struct Case { const std::string &secret; const char *what; };
        const Case cases[] = {
            {title, "a real track title"},
            {comment, "the DJ's own comment (the column is djComment, not comment)"},
            {isrc, "the ISRC, which names the exact commercial recording"},
            {tag, "a My Tag name, the same free text that gets exportExt.pdb deleted"},
            {artist, "a real artist name"},
            {playlist, "a real playlist name"},
            {searchTitle, "titleForSearch, the normalised copy of the title"},
        };
        for (const auto &c : cases) {
            const std::string found = findAnywhere(handle.db, c.secret);
            if (!found.empty()) {
                std::cerr << "LEAK: " << c.what << " survived in " << found << "\n";
            }
            assert(found.empty());
        }
        std::cout << "case 3 (every planted secret is gone from every column of every table) OK\n";
    }

    // A schema that grows a text column nobody scrubbed should be loud
    // rather than silently shipped. Listed explicitly: handled columns
    // are scrubbed above, benign ones carry no library content.
    {
        Db handle(db, true);
        const std::vector<std::string> handled = {
            "title", "titleForSearch", "path", "fileName", "djComment", "comment", "isrc",
            "subtitle", "kuvoDeliveryComment",
        };
        const std::vector<std::string> benign = {
            "content_id", "bpmx100", "length", "trackNo", "discNo", "artist_id_artist",
            "artist_id_remixer", "artist_id_originalArtist", "artist_id_composer", "artist_id_lyricist",
            "album_id", "genre_id", "label_id", "key_id", "color_id", "image_id", "rating",
            "releaseYear", "releaseDate", "dateCreated", "dateAdded", "fileSize", "fileType",
            "bitrate", "bitDepth", "samplingRate", "djPlayCount", "isHotCueAutoLoadOn",
            "isKuvoDeliverStatusOn", "masterDbId", "masterContentId", "analysisDataFilePath",
            "analysedBits", "contentLink", "hasModified", "cueUpdateCount",
            "analysisDataUpdateCount", "informationUpdateCount",
        };
        for (const auto &column : columnsOf(handle.db, "content")) {
            const bool known = std::find(handled.begin(), handled.end(), column) != handled.end()
                || std::find(benign.begin(), benign.end(), column) != benign.end();
            if (!known) {
                std::cerr << "content." << column << " is new and unclassified: either scrub it or add it to "
                             "the benign list, having checked it carries no library content\n";
            }
            assert(known);
        }
        std::cout << "case 4 (every column of content is either scrubbed or knowingly benign) OK\n";
    }

    fs::remove_all(root, ec);
    std::cout << "all cases passed\n";
    return 0;
}
