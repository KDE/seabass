// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/track.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::onelibrary;
using namespace seabass::domain;
using seabass::gui::OneLibraryCueWriterAdapter;
namespace fs = std::filesystem;

namespace
{

// Same minimal-but-real exportLibrary.db fixture onelibrary_cue_writer_test
// .cpp uses -- see that file's own comment for why this schema shape.
void createFixture(const std::string &pioneerRoot)
{
    fs::create_directories(fs::path(pioneerRoot) / "rekordbox");
    std::string dbPath = OneLibraryCueWriter::dbPathFor(pioneerRoot);

    std::string key = deriveOneLibraryKey();
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
    db.exec("PRAGMA key = '" + key + "';");
    db.exec("CREATE TABLE content(content_id integer primary key, title varchar, path varchar);");
    db.exec(
        "CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, "
        "colorTableIndex integer, cueComment varchar, isActiveLoop integer, inUsec integer, "
        "outUsec integer);");
    db.exec("CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);");
    db.exec("CREATE TABLE playlist_content(content_id integer, playlist_id integer, sequenceNo integer);");
    db.exec("INSERT INTO content (content_id, title, path) VALUES (1, 'Test Track', '/Contents/Test Track.mp3');");
}

std::vector<CuePoint> sampleCues()
{
    CuePoint hot1{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};
    return {hot1};
}

}  // namespace

int main()
{
    // Case 1: a known sourceId correctly resolves to its file path and
    // writes through to the real database -- proving the adapter's whole
    // point (application::CueWriter's sourceId-based interface, bridged
    // to OneLibraryCueWriter's path-based one) actually works, verified
    // via an entirely independent second connection, not just "the call
    // didn't throw".
    {
        fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_cue_writer_adapter_test";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());

        std::string filePath = (scratch / "Contents" / "Test Track.mp3").string();
        std::unordered_map<std::string, std::string> sourceIdToPath{{"1", filePath}};
        OneLibraryCueWriterAdapter adapter(pioneerRoot.string(), sourceIdToPath);
        adapter.writeHotCues("1", sampleCues());

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement countStmt(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        countStmt.step();
        assert(countStmt.columnInt64(0) == 1);

        fs::remove_all(scratch, ec);
        std::cout << "case 1 (known sourceId resolves to its path and writes through) OK\n";
    }

    // Case 2: an unknown sourceId throws clearly rather than silently
    // no-op'ing or writing to the wrong track -- this is exactly the
    // failure mode a caller building an incomplete sourceId->path map
    // would otherwise hit silently.
    {
        fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_cue_writer_adapter_test2";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());

        OneLibraryCueWriterAdapter adapter(pioneerRoot.string(), {});
        bool threw = false;
        try {
            adapter.writeHotCues("does-not-exist", sampleCues());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        fs::remove_all(scratch, ec);
        std::cout << "case 2 (unknown sourceId throws instead of silently no-op'ing) OK\n";
    }

    std::cout << "All onelibrary_cue_writer_adapter_test cases passed.\n";
    return 0;
}
