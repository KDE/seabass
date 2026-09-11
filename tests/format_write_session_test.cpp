// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FormatWriteSession: the per-format write target of one save -- backs
// the database up once, redirects a big batch to a scratch copy, and
// commits that copy back only when something complete is in it.

#include <QString>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"

#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream(path, std::ios::binary) << content;
}

fs::path makeStick(const fs::path &root)
{
    fs::remove_all(root);
    fs::create_directories(root / "Engine Library" / "Database2");
    // Big enough that the strategy prefers a scratch copy for many items
    // (see bulk_write_strategy.cpp: 1 MB at the fallback throughput is
    // well under the per-item cost of 1000 direct writes).
    writeFile(root / "Engine Library" / "Database2" / "m.db", std::string(1024 * 1024, 'o'));
    return root / "Engine Library";
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_format_write_session_test";
    auto &noProgress = seabass::application::NullProgressReporter::instance();

    // 1. Database file per format.
    {
        assert(FormatWriteSession::databaseFileFor("engine", "/s/Engine Library") == "/s/Engine Library/Database2/m.db");
        assert(FormatWriteSession::databaseFileFor("rekordbox", "/s/PIONEER") == "/s/PIONEER/rekordbox/export.pdb");
        assert(FormatWriteSession::databaseFileFor("onelibrary", "/s/PIONEER").ends_with("exportLibrary.db"));
        std::cout << "case 1 (database file per format) OK\n";
    }

    // 2. A big batch goes through a scratch copy; on a successful finish
    //    the copy is committed back and the original was backed up.
    {
        fs::path engine = makeStick(root);
        fs::path db = engine / "Database2" / "m.db";
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, {}, QString::fromStdString(engine.string()));
        auto &session = ctx.shared<FormatWriteSession>("engine", [&]() {
            return std::make_unique<FormatWriteSession>("engine", engine.string(), 1000, "test", ctx);
        });
        assert(session.usesScratch());
        assert(session.writeRoot() != session.realRoot());
        fs::path scratchDb = fs::path(session.writeRoot()) / "Database2" / "m.db";
        assert(fs::exists(scratchDb));
        writeFile(scratchDb, "new-content");
        session.noteItemApplied();
        assert(readFile(db).size() == 1024 * 1024);  // untouched so far
        auto hookError = ctx.runFinishHooks(true);
        assert(!hookError);
        assert(readFile(db) == "new-content");
        auto backups = ctx.takeBackups();
        assert(backups.size() == 1);
        assert(fs::is_directory(root / "Seabass" / "backups"));
        std::cout << "case 2 (scratch committed on success) OK\n";
    }

    // 3. Cancelled or failed after at least one item: the completed items
    //    are committed (they are whole); with zero items the scratch is
    //    discarded and the stick stays untouched.
    {
        fs::path engine = makeStick(root);
        fs::path db = engine / "Database2" / "m.db";
        CancellationToken token;
        {
            SaveContext ctx(token, noProgress, {}, {}, QString::fromStdString(engine.string()));
            auto &session = ctx.shared<FormatWriteSession>("engine", [&]() {
                return std::make_unique<FormatWriteSession>("engine", engine.string(), 1000, "test", ctx);
            });
            writeFile(fs::path(session.writeRoot()) / "Database2" / "m.db", "one-item");
            session.noteItemApplied();
            assert(!ctx.runFinishHooks(false));
            assert(readFile(db) == "one-item");
        }
        {
            SaveContext ctx(token, noProgress, {}, {}, QString::fromStdString(engine.string()));
            auto &session = ctx.shared<FormatWriteSession>("engine", [&]() {
                return std::make_unique<FormatWriteSession>("engine", engine.string(), 1000, "test", ctx);
            });
            std::string scratchRoot = session.writeRoot();
            writeFile(fs::path(scratchRoot) / "Database2" / "m.db", "never-applied");
            assert(!ctx.runFinishHooks(false));
            assert(readFile(db) == "one-item");
            // The scratch directory is gone once the context is.
            (void)scratchRoot;
        }
        std::cout << "case 3 (partial commit rule) OK\n";
    }

    // 4. A small batch writes straight to the stick: no scratch, same root.
    {
        fs::path engine = makeStick(root);
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, {}, QString::fromStdString(engine.string()));
        FormatWriteSession session("engine", engine.string(), 1, "test", ctx);
        assert(!session.usesScratch());
        assert(session.writeRoot() == session.realRoot());
        assert(!ctx.runFinishHooks(true));
        std::cout << "case 4 (direct writes for a small batch) OK\n";
    }

    fs::remove_all(root);
    std::cout << "format_write_session_test: all cases passed\n";
    return 0;
}
