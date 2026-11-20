// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/track_field_edit_change.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <stdexcept>

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr const char *Label = "track-edit";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

bool isFile(const std::string &path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(pathFromUtf8(path), ec);
}

bool hasPdb(const std::string &pioneer)
{
    return !pioneer.empty() && isFile(exportPdbOf(pioneer));
}

bool hasOneLibrary(const std::string &pioneer)
{
    return !pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer);
}

bool hasEngine(const std::string &engine)
{
    return !engine.empty() && isFile(engineDbOf(engine));
}

QString fieldName(TrackField field)
{
    return field == TrackField::Rating ? QStringLiteral("rating") : QStringLiteral("comment");
}

std::string keyOf(const std::string &path)
{
    return application::normalizedPathKey(pathToUtf8(pathFromUtf8(path).lexically_normal()));
}

// A catalog's rows for the file whose key is `wanted`: `stored` are its
// (id, path as stored) pairs and `onStick` joins a stored path onto the
// stick the way that catalog's reader does.
template <typename Id>
CatalogRows match(const std::vector<std::pair<Id, std::string>> &stored, const std::string &wanted,
                  const std::function<std::string(const std::string &)> &onStick)
{
    CatalogRows rows;
    rows.state = CatalogRows::State::NotListed;
    std::set<std::string> spellings;
    for (const auto &[id, path] : stored) {
        if (path.empty() || keyOf(onStick(path)) != wanted) {
            continue;
        }
        spellings.insert(path);
        rows.ids.push_back(static_cast<std::int64_t>(id));
        rows.spelling = path;
    }
    if (spellings.size() > 1) {
        rows.state = CatalogRows::State::Ambiguous;
        rows.spellings.assign(spellings.begin(), spellings.end());
        rows.ids.clear();
        rows.spelling.clear();
    } else if (!spellings.empty()) {
        rows.state = CatalogRows::State::Listed;
    }
    return rows;
}

std::function<std::string(const std::string &)> rekordboxJoin(const std::string &pioneer)
{
    const std::string stickRoot = infrastructure::paths::normalStickRootForCatalogPath(pioneer);
    return [stickRoot](const std::string &stored) {
        return infrastructure::rekordbox::trackFilePathOnStick(stickRoot, stored);
    };
}

std::function<std::string(const std::string &)> engineJoin(const std::string &engine)
{
    return [engine](const std::string &stored) {
        return pathToUtf8((pathFromUtf8(engine) / pathFromUtf8(stored)).lexically_normal());
    };
}

std::vector<std::pair<std::int64_t, std::string>> pdbPaths(const infrastructure::rekordbox::PdbRowWriter &pdb)
{
    std::vector<std::pair<std::int64_t, std::string>> paths;
    for (const auto &row : pdb.trackPathRows()) {
        paths.emplace_back(row.id, row.filePath);
    }
    return paths;
}

CatalogRows pdbRows(const infrastructure::rekordbox::PdbRowWriter &pdb, const std::string &pioneer,
                    const std::string &filePath)
{
    return match(pdbPaths(pdb), keyOf(filePath), rekordboxJoin(pioneer));
}

// An Engine 1.x library on the stick: m.db at the library folder's top
// level and no Database2. The stick scan hands no Engine path over for
// one (it looks for Database2/m.db only), so it is looked for from the
// stick's root as well as at `engine`.
bool hasEngine1x(const std::string &pioneer, const std::string &engine)
{
    std::vector<fs::path> libraries;
    if (!engine.empty()) {
        libraries.push_back(pathFromUtf8(engine));
    }
    if (!pioneer.empty()) {
        libraries.push_back(pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(pioneer)) / "Engine Library");
    }
    std::error_code ec;
    for (const auto &library : libraries) {
        if (fs::is_regular_file(library / "m.db", ec) && !fs::is_regular_file(library / "Database2" / "m.db", ec)) {
            return true;
        }
    }
    return false;
}

QString catalogName(const char *catalog)
{
    return QString::fromLatin1(catalog);
}

// Why the rows a catalog has now are not the ones it had at staging, or
// empty when they are.
QString drifted(const char *catalog, const CatalogRows &then, const CatalogRows &now, const QString &title)
{
    using State = CatalogRows::State;
    const QString name = catalogName(catalog);
    if (now.state == State::Ambiguous) {
        return QStringLiteral("%1 now lists \"%2\" under more than one spelling of its path, so Seabass cannot tell "
                              "which row is the track. Nothing was written.")
            .arg(name, title);
    }
    if (then.state == State::Listed) {
        if (now.state != State::Listed) {
            return QStringLiteral("%1 no longer lists \"%2\", which it did when the edit was made, so no catalog was "
                                  "written: they would disagree. Show the track again and redo the edit.")
                .arg(name, title);
        }
        if (now.spelling != then.spelling || now.ids.size() != then.ids.size()) {
            return QStringLiteral("%1 lists \"%2\" differently from when the edit was made, so no catalog was "
                                  "written. Show the track again and redo the edit.")
                .arg(name, title);
        }
        return {};
    }
    if (now.state == State::Listed) {
        return QStringLiteral("%1 lists \"%2\" now and did not when the edit was made, so no catalog was written. "
                              "Show the track again and redo the edit.")
            .arg(name, title);
    }
    return {};
}

}  // namespace

bool TrackRows::listedAnywhere() const
{
    using State = CatalogRows::State;
    return rekordbox.state == State::Listed || oneLibrary.state == State::Listed || engine.state == State::Listed;
}

std::string TrackRows::refusal() const
{
    using State = CatalogRows::State;
    for (const auto &[name, rows] : {std::pair<const char *, const CatalogRows *>{"export.pdb", &rekordbox},
                                     std::pair<const char *, const CatalogRows *>{"OneLibrary", &oneLibrary},
                                     std::pair<const char *, const CatalogRows *>{"Engine", &engine}}) {
        if (rows->state == State::Unwritable) {
            return std::string(name) + " is on the stick in a form Seabass does not write (an Engine 1.x library), "
                   + "so editing here would leave it saying something else";
        }
        if (rows->state == State::Ambiguous) {
            std::string spelled;
            for (const auto &s : rows->spellings) {
                spelled += (spelled.empty() ? "\"" : ", \"") + s + "\"";
            }
            return std::string(name) + " lists this file under more than one spelling of its path (" + spelled
                   + "), so Seabass cannot tell which row is the track and does not edit it";
        }
    }
    if (!listedAnywhere()) {
        return "no catalog on the stick lists this file";
    }
    return {};
}

namespace
{
// Every row of one catalog, with its path's key, and the rows by key.
struct IndexedCatalog
{
    bool present = false;
    std::vector<std::pair<std::int64_t, std::string>> rows;
    std::unordered_map<std::string, std::vector<std::size_t>> byKey;

    void add(std::int64_t id, const std::string &stored, const std::function<std::string(const std::string &)> &join)
    {
        if (stored.empty()) {
            return;
        }
        byKey[keyOf(join(stored))].push_back(rows.size());
        rows.emplace_back(id, stored);
    }

    CatalogRows find(const std::string &wanted) const
    {
        if (!present) {
            return {};
        }
        CatalogRows out;
        out.state = CatalogRows::State::NotListed;
        const auto it = byKey.find(wanted);
        if (it == byKey.end()) {
            return out;
        }
        std::set<std::string> spellings;
        for (const std::size_t i : it->second) {
            spellings.insert(rows[i].second);
            out.ids.push_back(rows[i].first);
            out.spelling = rows[i].second;
        }
        if (spellings.size() > 1) {
            out.state = CatalogRows::State::Ambiguous;
            out.spellings.assign(spellings.begin(), spellings.end());
            out.ids.clear();
            out.spelling.clear();
        } else {
            out.state = CatalogRows::State::Listed;
        }
        return out;
    }
};
}  // namespace

struct StickPathIndex::Data
{
    IndexedCatalog rekordbox;
    IndexedCatalog oneLibrary;
    IndexedCatalog engine;
    bool engineUnwritable = false;
    std::map<std::int64_t, std::optional<infrastructure::rekordbox::PdbRowWriter::TrackCommentCapacity>> comments;
};

StickPathIndex buildStickPathIndex(const QString &pioneerRoot, const QString &enginePath)
{
    const std::string pioneer = pioneerRoot.toStdString();
    const std::string engine = enginePath.toStdString();
    StickPathIndex index;
    index.pioneerRoot = pioneerRoot;
    index.enginePath = enginePath;
    auto built = std::make_shared<StickPathIndex::Data>();
    auto &data = *built;
    const auto join = rekordboxJoin(pioneer);
    if (hasPdb(pioneer)) {
        data.rekordbox.present = true;
        const infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(pioneer));
        for (const auto &row : pdb.trackPathRows()) {
            data.rekordbox.add(row.id, row.filePath, join);
            data.comments[row.id] = row.comment;
        }
    }
    if (hasOneLibrary(pioneer)) {
        data.oneLibrary.present = true;
        for (const auto &[id, path] : infrastructure::onelibrary::oneLibraryContentPaths(pioneer)) {
            data.oneLibrary.add(id, path, join);
        }
    }
    if (hasEngine(engine)) {
        data.engine.present = true;
        const auto engineJoined = engineJoin(engine);
        for (const auto &[id, path] : infrastructure::engine::engineTrackPaths(engine)) {
            data.engine.add(id, path, engineJoined);
        }
    }
    data.engineUnwritable = hasEngine1x(pioneer, engine);
    index.m_data = std::move(built);
    return index;
}

TrackRows StickPathIndex::rowsFor(const std::string &filePath) const
{
    TrackRows rows;
    if (!m_data) {
        return rows;
    }
    const std::string wanted = keyOf(filePath);
    rows.rekordbox = m_data->rekordbox.find(wanted);
    rows.oneLibrary = m_data->oneLibrary.find(wanted);
    rows.engine = m_data->engine.find(wanted);
    if (m_data->engineUnwritable) {
        rows.engine = {};
        rows.engine.state = CatalogRows::State::Unwritable;
    }
    return rows;
}

CommentLimit StickPathIndex::limitFor(const TrackRows &rows) const
{
    CommentLimit limit;
    if (!m_data || rows.rekordbox.state != CatalogRows::State::Listed) {
        return limit;
    }
    for (const auto id : rows.rekordbox.ids) {
        const auto it = m_data->comments.find(id);
        const auto capacity = it == m_data->comments.end() ? std::nullopt : it->second;
        const std::size_t units = capacity ? capacity->units : 0;
        limit.units = limit.limited ? std::min(limit.units, units) : units;
        limit.asciiOnly = limit.asciiOnly || !capacity || !capacity->utf16;
        limit.limited = true;
    }
    limit.reason = limitReason(limit);
    return limit;
}

TrackRows findTrackRows(const QString &pioneerRoot, const QString &enginePath, const std::string &filePath)
{
    return buildStickPathIndex(pioneerRoot, enginePath).rowsFor(filePath);
}

QString trackFieldChangeId(TrackField field, const std::string &filePath)
{
    return QStringLiteral("addcue:track-") + fieldName(field) + QLatin1Char(':')
           + QString::fromStdString(keyOf(filePath));
}

TrackFieldEditChange::TrackFieldEditChange(QString pioneerRoot, QString enginePath, std::string filePath,
                                           std::string title, TrackFieldValue value, TrackRows rows)
    : m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)),
      m_filePath(std::move(filePath)),
      m_title(std::move(title)),
      m_value(std::move(value)),
      m_rows(std::move(rows))
{
}

QString TrackFieldEditChange::id() const
{
    return trackFieldChangeId(m_value.field, m_filePath);
}

QString TrackFieldEditChange::owner() const
{
    return QStringLiteral("addcue");
}

QString TrackFieldEditChange::subject() const
{
    return QString::fromStdString(m_title.empty() ? pathToUtf8(pathFromUtf8(m_filePath).filename()) : m_title);
}

QString TrackFieldEditChange::description() const
{
    if (m_value.field == TrackField::Rating) {
        if (!m_value.rating) {
            return QStringLiteral("Clear the rating of \"%1\"").arg(subject());
        }
        return QStringLiteral("Rate \"%1\" %2 star%3")
            .arg(subject())
            .arg(*m_value.rating)
            .arg(*m_value.rating == 1 ? QString() : QStringLiteral("s"));
    }
    if (m_value.text.empty()) {
        return QStringLiteral("Clear the comment on \"%1\"").arg(subject());
    }
    return QStringLiteral("Change the comment on \"%1\"").arg(subject());
}

QString TrackFieldEditChange::unit() const
{
    return QStringLiteral("tracks");
}

QString TrackFieldEditChange::verb() const
{
    return QStringLiteral("edited");
}

// A track's rating and its comment are one edit of the track: a cancel
// keeps them together.
QStringList TrackFieldEditChange::unitKeys() const
{
    return {QStringLiteral("track-edit:") + QString::fromStdString(keyOf(m_filePath))};
}

QStringList TrackFieldEditChange::formatsTouched() const
{
    using State = CatalogRows::State;
    QStringList formats;
    if (m_rows.rekordbox.state == State::Listed) {
        formats << QStringLiteral("rekordbox");
    }
    if (m_rows.oneLibrary.state == State::Listed) {
        formats << QStringLiteral("onelibrary");
    }
    if (m_rows.engine.state == State::Listed) {
        formats << QStringLiteral("engine");
    }
    return formats;
}

// Every catalog that listed the file when the edit was staged, before
// anything is written.
std::vector<BackupTarget> TrackFieldEditChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    using State = CatalogRows::State;
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (m_rows.rekordbox.state == State::Listed && hasPdb(pioneer)) {
        targets.push_back({exportPdbOf(pioneer), Label});
    }
    if (m_rows.oneLibrary.state == State::Listed && hasOneLibrary(pioneer)) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
    }
    if (m_rows.engine.state == State::Listed && hasEngine(m_enginePath.toStdString())) {
        targets.push_back({engineDbOf(m_enginePath.toStdString()), Label});
    }
    return targets;
}

// The rating, for the rekordbox baseline's origin ledger: Sync after
// Rekordbox Export then knows Seabass wrote it. 0 is "no stars". A
// comment has no place in that record (RekordboxWrite carries none).
std::vector<RekordboxWrite> TrackFieldEditChange::rekordboxWrites() const
{
    if (m_value.field != TrackField::Rating || !m_wroteRekordboxSide) {
        return {};
    }
    RekordboxWrite write;
    write.filePath = m_filePath;
    write.rating = m_value.rating.value_or(0);
    return {std::move(write)};
}

namespace
{
struct TrackEditCount
{
    int edits = 0;
};
constexpr const char *CountKey = "track-edit:count";
}  // namespace

void TrackFieldEditChange::beforeSave(SaveContext &ctx)
{
    ++ctx.sharedForWholeSave<TrackEditCount>(CountKey, [] { return std::make_unique<TrackEditCount>(); }).edits;
}

ChangeOutcome TrackFieldEditChange::apply(SaveContext &ctx)
{
    using State = CatalogRows::State;
    m_wroteRekordboxSide = false;
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string engine = m_enginePath.toStdString();
    const bool rating = m_value.field == TrackField::Rating;
    if (rating && m_value.rating && (*m_value.rating < 1 || *m_value.rating > 5)) {
        return ChangeOutcome::failure(QStringLiteral("A rating is 1 to 5 stars."));
    }
    // How many track edits this save holds: the write sessions take it
    // as their hint, so a batch goes through one scratch copy and one
    // replace of each database.
    const int hint = std::max(
        1, ctx.sharedForWholeSave<TrackEditCount>(CountKey, [] { return std::make_unique<TrackEditCount>(); }).edits);
    const QString title = subject();
    QStringList written;
    try {
        // OneLibrary first, then Engine, then export.pdb. Each is put back
        // by the save's rollback when a later one fails, so the order
        // decides only which failure is met first; export.pdb's write is
        // a whole-file replace onto the stick, the one most likely to meet
        // a stick that is full or gone, so it goes last and a failure
        // there has the most to roll back rather than the least.
        // A catalog that did not list the file when the edit was staged
        // is only looked at, read-only, to see that it still does not: it
        // is not opened for writing, backed up for this change, or put
        // through a write session (which would copy it back unchanged).
        if (hasEngine1x(pioneer, engine)) {
            return ChangeOutcome::failure(
                QStringLiteral("The stick has an Engine 1.x library, which Seabass does not write, so \"%1\" was not "
                               "edited in any catalog: they would disagree.")
                    .arg(title));
        }
        if (m_rows.oneLibrary.state == State::Listed) {
            if (!hasOneLibrary(pioneer)) {
                return ChangeOutcome::failure(
                    QStringLiteral("OneLibrary is no longer on the stick, so no catalog was written."));
            }
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            const CatalogRows now = match(writer.contentPaths(), keyOf(m_filePath), rekordboxJoin(pioneer));
            if (const QString why = drifted("OneLibrary", m_rows.oneLibrary, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
            // By the catalog's own spelling: its rows, and only them.
            const std::string path = rekordboxJoin(pioneer)(now.spelling);
            if (rating) {
                writer.writeAnnotationForPath(path, m_value.rating.value_or(0), std::nullopt);
            } else {
                writer.writeAnnotationForPath(path, std::nullopt, m_value.text);
            }
            written << QStringLiteral("OneLibrary");
        } else if (hasOneLibrary(pioneer)) {
            const CatalogRows now =
                match(infrastructure::onelibrary::oneLibraryContentPaths(pioneer), keyOf(m_filePath), rekordboxJoin(pioneer));
            if (const QString why = drifted("OneLibrary", m_rows.oneLibrary, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
        }
        if (m_rows.engine.state == State::Listed) {
            if (!hasEngine(engine)) {
                return ChangeOutcome::failure(
                    QStringLiteral("Engine's library is no longer on the stick, so no catalog was written."));
            }
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, hint, Label);
            const CatalogRows now =
                match(infrastructure::engine::engineTrackPaths(session.writeRoot()), keyOf(m_filePath), engineJoin(engine));
            if (const QString why = drifted("Engine", m_rows.engine, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
            infrastructure::engine::LibdjinteropEngineCueWriter writer(session.writeRoot());
            for (const auto id : now.ids) {
                if (rating) {
                    writer.writeRating(std::to_string(id), m_value.rating);
                } else {
                    writer.writeAnnotation(std::to_string(id), std::nullopt, m_value.text);
                }
            }
            session.noteItemApplied();
            written << QStringLiteral("Engine");
        } else if (hasEngine(engine)) {
            const FormatWriteSession *existing = existingFormatWriteSession(ctx, "engine", engine);
            const CatalogRows now = match(infrastructure::engine::engineTrackPaths(existing ? existing->writeRoot() : engine),
                                          keyOf(m_filePath), engineJoin(engine));
            if (const QString why = drifted("Engine", m_rows.engine, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
        }
        if (m_rows.rekordbox.state == State::Listed) {
            if (!hasPdb(pioneer)) {
                return ChangeOutcome::failure(
                    QStringLiteral("export.pdb is no longer on the stick, so no catalog was written."));
            }
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, hint, Label);
            infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(session.writeRoot()));
            const CatalogRows now = pdbRows(pdb, pioneer, m_filePath);
            if (const QString why = drifted("export.pdb", m_rows.rekordbox, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
            for (const auto id : now.ids) {
                const auto row = static_cast<uint32_t>(id);
                if (rating) {
                    // 0 is how export.pdb says "no stars": the byte has no
                    // other value for it.
                    pdb.setTrackRating(row, m_value.rating.value_or(0));
                } else {
                    std::string refusal;
                    if (!pdb.setTrackComment(row, m_value.text, &refusal)) {
                        return ChangeOutcome::failure(
                            QStringLiteral("The comment on \"%1\" cannot be written into export.pdb: %2.")
                                .arg(title, QString::fromStdString(refusal.empty() ? "its row has gone" : refusal)));
                    }
                }
            }
            if (!pdb.commit()) {
                return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb."));
            }
            session.noteItemApplied();
            // The origin ledger records export.pdb's ratings (the
            // rekordbox baseline is read from it), so only a rating that
            // reached export.pdb is reported as Seabass's.
            m_wroteRekordboxSide = true;
            written << QStringLiteral("export.pdb");
        } else if (hasPdb(pioneer)) {
            const FormatWriteSession *existing = existingFormatWriteSession(ctx, "rekordbox", pioneer);
            const infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(existing ? existing->writeRoot() : pioneer));
            const CatalogRows now = pdbRows(pdb, pioneer, m_filePath);
            if (const QString why = drifted("export.pdb", m_rows.rekordbox, now, title); !why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QStringLiteral("Could not write the %1 of \"%2\": %3")
                                          .arg(fieldName(m_value.field), title, QString::fromUtf8(e.what())));
    } catch (...) {
        return ChangeOutcome::failure(
            QStringLiteral("Could not write the %1 of \"%2\".").arg(fieldName(m_value.field), title));
    }
    if (written.isEmpty()) {
        // Staging refuses a track no catalog lists, and a catalog that
        // listed it and does not now fails above: this is only reached
        // when the stick lost all of them at once.
        return ChangeOutcome::failure(
            QStringLiteral("No catalog on the stick lists \"%1\" any more, so nothing was written.").arg(title));
    }
    ctx.log().record(std::string(Label) + ": " + description().toStdString() + " in "
                     + written.join(QStringLiteral(", ")).toStdString());
    return ChangeOutcome::success();
}

std::string limitReason(const CommentLimit &limit)
{
    if (!limit.limited) {
        return {};
    }
    return limit.units == 0
        ? "export.pdb keeps no room for a comment on this track, and Seabass cannot make its row larger."
        : "export.pdb keeps room for " + std::to_string(limit.units) + " characters in this track's comment"
              + (limit.asciiOnly ? ", plain ASCII only (no accented letters)," : "")
              + " and Seabass cannot make its row larger.";
}

CommentLimit commentLimitFor(const QString &pioneerRoot, const TrackRows &rows)
{
    if (rows.rekordbox.state != CatalogRows::State::Listed) {
        return {};
    }
    try {
        return buildStickPathIndex(pioneerRoot, QString()).limitFor(rows);
    } catch (const std::exception &e) {
        CommentLimit limit;
        limit.limited = true;
        limit.reason = std::string("export.pdb could not be read, so the comment cannot be changed: ") + e.what();
        return limit;
    }
}

TrackLookup lookUpTrack(const QString &pioneerRoot, const QString &enginePath, const std::string &filePath)
{
    TrackLookup lookup;
    lookup.filePath = filePath;
    try {
        const StickPathIndex index = buildStickPathIndex(pioneerRoot, enginePath);
        lookup.rows = index.rowsFor(filePath);
        lookup.limit = index.limitFor(lookup.rows);
        lookup.error = lookup.rows.refusal();
    } catch (const std::exception &e) {
        lookup.error = std::string("the stick's catalogs could not be read: ") + e.what();
    } catch (...) {
        lookup.error = "the stick's catalogs could not be read";
    }
    return lookup;
}

std::string commentLimitRefusal(const std::string &comment, const CommentLimit &limit)
{
    if (!limit.limited) {
        return {};
    }
    return infrastructure::rekordbox::PdbRowWriter::trackCommentRefusal(
        comment, infrastructure::rekordbox::PdbRowWriter::TrackCommentCapacity{limit.units, !limit.asciiOnly});
}

}  // namespace seabass::gui
