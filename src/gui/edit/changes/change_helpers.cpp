// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/change_helpers.hpp"

#include <QStringList>

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>

#include "application/path_key.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

std::vector<std::string> filesWrittenFor(WriteScope scope, const domain::TrackId &track, const QString &root,
                                         SaveContext &ctx)
{
    const std::string &format = track.first;
    const std::string &sourceId = track.second;
    const std::string rootPath = root.toStdString();
    std::vector<std::string> files;

    if (format == "rekordbox") {
        // Cues live in this track's own analysis file. Resolved through the
        // save's shared index when there is one, falling back to the direct
        // lookup exactly as the writers do.
        const auto *index = sharedAnlzPathIndex(ctx, root);
        std::optional<std::string> analyzePath;
        try {
            const auto id = static_cast<std::uint32_t>(std::stoul(sourceId));
            analyzePath = index ? index->pathFor(id)
                                : infrastructure::rekordbox::findAnlzPathForTrackId(rootPath, id);
        } catch (const std::exception &) {
            return {};
        }
        if (scope.cueData && analyzePath) {
            files.push_back(infrastructure::rekordbox::extAnlzPath(rootPath, *analyzePath));
        }
        if (scope.catalogRows) {
            files.push_back((fs::path(rootPath) / "rekordbox" / "export.pdb").string());
        }
        // Only when this workflow actually mirrors there. Presence of the
        // database is not the test -- Sync leaves it alone even when it
        // exists.
        if (scope.oneLibraryMirror && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rootPath)) {
            files.push_back(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rootPath));
        }
    } else if (format == "engine") {
        // One shared database, whatever the track: the same file every
        // time, deduplicated by the caller.
        files.push_back((fs::path(rootPath) / "Database2" / "m.db").string());
    } else {
        files.push_back(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rootPath));
    }
    return files;
}

QString issueFormat(const domain::LibraryConsistencyIssue &issue)
{
    if (issue.survivor) {
        return QString::fromStdString(issue.survivor->format);
    }
    if (!issue.brokenGroup.empty()) {
        return QString::fromStdString(issue.brokenGroup.front().format);
    }
    return {};
}

QString issueKeyFor(const domain::LibraryConsistencyIssue &issue)
{
    QStringList ids;
    if (issue.survivor) {
        ids << QString::fromStdString(issue.survivor->sourceId);
    }
    for (const auto &broken : issue.brokenGroup) {
        ids << QString::fromStdString(broken.sourceId);
    }
    return issueFormat(issue) + ":" + ids.join('+');
}

QString junkKeyFor(const domain::Track &track)
{
    return QString::fromStdString(track.format) + ":" + QString::fromStdString(track.sourceId);
}

QString describeCues(const std::vector<domain::CuePoint> &cues)
{
    int hot = 0;
    int memory = 0;
    for (const auto &cue : cues) {
        (cue.kind == domain::CuePoint::Kind::Hot ? hot : memory)++;
    }
    QString result = QString("%1 hot").arg(hot);
    if (memory > 0) {
        result += QString(", %1 memory (not written - Engine writer only handles hot cues)").arg(memory);
    }
    return result;
}

// Holds the index plus whether building it failed, so a catalog that
// cannot be read is not retried once per item.
namespace
{
struct SharedAnlzIndex
{
    std::unique_ptr<infrastructure::rekordbox::AnlzPathIndex> index;
};
}  // namespace

const infrastructure::rekordbox::AnlzPathIndex *sharedAnlzPathIndex(SaveContext &ctx, const QString &pioneerRoot)
{
    SharedAnlzIndex &shared = ctx.shared<SharedAnlzIndex>(
        "anlz-index:" + pioneerRoot.toStdString(), [&]() {
            auto holder = std::make_unique<SharedAnlzIndex>();
            try {
                holder->index = std::make_unique<infrastructure::rekordbox::AnlzPathIndex>(pioneerRoot.toStdString());
            } catch (const std::exception &e) {
                // Not fatal: every caller falls back to looking one id up
                // at a time, which is what it did before this existed.
                ctx.log().record(std::string("could not index analysis paths, falling back to per-track lookups: ")
                                 + e.what());
            }
            return holder;
        });
    return shared.index.get();
}

infrastructure::onelibrary::OneLibraryCueWriter &sharedOneLibraryWriter(
    SaveContext &ctx, const std::string &pioneerRoot, const std::optional<std::string> &realStickRoot)
{
    // Keyed on the database being written, not on the feature: two
    // features staging into the same library in one save must share the
    // connection, not open a second one against the same file.
    // normalizedPathKey, not the raw string: callers reach this by several
    // routes (m_path, session.writeRoot(), realRoot, fc.pioneerRoot), and a
    // trailing slash or any other spelling would open a second writer
    // against one exportLibrary.db -- the staleness abort this exists to
    // prevent, on somebody else's machine.
    const std::string key = "onelibrary-writer:" + application::normalizedPathKey(pioneerRoot);
    // The checkpoint hangs off creation, not off FormatWriteSession: this
    // format never gets a scratch copy (hint = 0, see below), so that
    // session's commit() returns before it could do anything, and it
    // holds no reference to this writer. make() runs once per key per
    // save, so the hook is registered once however many features mirror.
    bool created = false;
    auto &writer = ctx.shared<infrastructure::onelibrary::OneLibraryCueWriter>(key, [&]() {
        created = true;
        return std::make_unique<infrastructure::onelibrary::OneLibraryCueWriter>(pioneerRoot, realStickRoot);
    });
    if (created) {
        // So a rolled-back save folds THIS database, whatever the change
        // that failed happened to touch.
        ctx.noteWalDatabase(
            infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot),
            [](const std::string &db) { return infrastructure::onelibrary::OneLibraryCueWriter::foldLogOf(db); },
            [&ctx, key]() {
                auto *live = ctx.sharedIfPresent<infrastructure::onelibrary::OneLibraryCueWriter>(key);
                return live != nullptr && live->hasWritten();
            });
        // Looked up by key rather than captured by reference: a rolled
        // back change destroys every shared() writer (SaveContext::
        // rollBackChange) before the hooks run, so on that path there is
        // nothing to fold and sharedIfPresent() says so; a captured
        // reference would dangle instead.
        ctx.onFinish([&ctx, key](bool ok) {
            if (auto *live = ctx.sharedIfPresent<infrastructure::onelibrary::OneLibraryCueWriter>(key)) {
                // Folded on every path, cancelled or not: rows that landed
                // belong in the database rather than in a log, and a fold
                // that did not happen is a warning the save carries either
                // way. ok is unused now -- the answer does not depend on it.
                (void)ok;
                live->finishWriting();
            }
        });
    }
    return writer;
}

infrastructure::engine::LibdjinteropEngineCueWriter &sharedEngineCueWriter(SaveContext &ctx,
                                                                           const std::string &engineLibraryPath)
{
    const std::string key = "engine-cue-writer:" + engineLibraryPath;
    return ctx.shared<infrastructure::engine::LibdjinteropEngineCueWriter>(key, [&]() {
        return std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(engineLibraryPath);
    });
}

FormatWriteSession &sharedFormatWriteSession(SaveContext &ctx, const std::string &format,
                                              const std::string &catalogPath, int itemCountHint,
                                              const std::string &label)
{
    // The database, not the feature -- see the header for what went
    // wrong while this was keyed the other way.
    const std::string key = "write-session:" + FormatWriteSession::databaseFileFor(format, catalogPath);
    // OneLibrary never gets a scratch copy, whatever it was asked for.
    //
    // exportLibrary.db is a WAL database (PRAGMA journal_mode reports
    // "wal" on the committed fixture). A save holds its writers open
    // across the commit, and in WAL mode the committed rows sit in
    // exportLibrary.db-wal until something checkpoints them -- while
    // FormatWriteSession commits by copying the single .db file back.
    // So a scratch copy loses this format's writes twice over: what
    // went through the scratch is stranded in a -wal file nobody
    // copies, and what went to the real file is overwritten by the
    // copy. Both were measured, the second by a test that had been
    // passing only because its fixture was not WAL.
    //
    // Zero items is how FormatWriteSession is told not to bother, and
    // it is the honest answer here: the optimisation was never
    // available for this format, it only looked available. Engine's
    // m.db is a rollback-journal database and keeps its scratch copy.
    const int hint = format == "onelibrary" ? 0 : itemCountHint;
    // For the whole save: a failed change is rolled back, and the session
    // still has to commit what the changes before it wrote.
    return ctx.sharedForWholeSave<FormatWriteSession>(key, [&]() {
        return std::make_unique<FormatWriteSession>(format, catalogPath, hint, label, ctx);
    });
}

}  // namespace seabass::gui
