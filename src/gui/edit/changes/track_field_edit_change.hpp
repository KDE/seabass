// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One authored field of a track, as Browse's track details edit it (#105).
// Rating and comment today. Another field (title, artist, genre) is one
// more value here, its words in fieldName()/describe, and its write per
// catalog in TrackFieldEditChange::apply(), next to these two: the
// staging, the lookup of the track's rows in each catalog, the backups,
// the rollback and Undo are the same for every field.
enum class TrackField
{
    Rating,
    Comment,
};

struct TrackFieldValue
{
    TrackField field = TrackField::Rating;
    // Rating: 1 to 5 stars, or nullopt for none.
    std::optional<int> rating;
    // Comment: the text, UTF-8, empty for none.
    std::string text;
};

// Where one catalog lists a track's file: found by its path with letter
// case and Unicode composition folded the way FAT and exFAT fold them
// (application::normalizedPathKey), since another catalog, or the disk,
// may spell a folder in another case (#103). Every row found must be in
// ONE spelling: rows in two spellings are two rows the player may tell
// apart, and which of them is "the track" is not something Seabass can
// know, so that is Ambiguous and the edit is refused.
struct CatalogRows
{
    enum class State
    {
        NoCatalog,  // the stick has no such catalog
        NotListed,  // the catalog does not list the file
        Listed,
        Ambiguous,
        // The catalog is there in a form Seabass does not write (an Engine
        // 1.x library, m.db at the library's top level): the edit is
        // refused rather than written to the others alone.
        Unwritable,
    };
    State state = State::NoCatalog;
    // The catalog's own spelling of the path, as stored (Listed).
    std::string spelling;
    // Its rows for the file, in that spelling (Listed).
    std::vector<std::int64_t> ids;
    // The spellings found (Ambiguous).
    std::vector<std::string> spellings;
    // Each listed row's comment, as the catalog holds it (Listed, when
    // read through the stick's index).
    std::vector<std::string> comments;
};

// The file's rows in each catalog of the stick.
struct TrackRows
{
    CatalogRows rekordbox;   // export.pdb
    CatalogRows oneLibrary;  // exportLibrary.db
    CatalogRows engine;      // m.db
    bool listedAnywhere() const;
    // The catalogs listing the file hold more than one comment for it,
    // blanks at either end aside: choosing any one of them is then an
    // edit, though it is the one shown (#108).
    bool commentsDisagree() const;
    // Why an edit of this track cannot be staged, one plain sentence; empty
    // when it can.
    std::string refusal() const;
};

struct CommentLimit;

// Every catalog on one stick read once (export.pdb whole, OneLibrary's
// content paths, Engine's track paths), each row's path folded to its
// key: a track shown is then answered from memory, without touching the
// stick again. Built off the GUI thread (buildStickPathIndex); copied
// cheaply, the rows are shared.
class StickPathIndex
{
public:
    QString pioneerRoot;
    QString enginePath;
    bool valid() const { return m_data != nullptr; }
    TrackRows rowsFor(const std::string &filePath) const;
    CommentLimit limitFor(const TrackRows &rows) const;

    struct Data;

private:
    friend StickPathIndex buildStickPathIndex(const QString &, const QString &);
    std::shared_ptr<const Data> m_data;
};

// A catalog of the stick that could not be read: `catalog` names it the
// way the pane says it ("export.pdb", "OneLibrary database", "Engine
// library"), what() is the technical detail, for the log.
class StickReadError : public std::runtime_error
{
public:
    StickReadError(std::string catalog, const std::string &detail)
        : std::runtime_error(detail), m_catalog(std::move(catalog))
    {
    }
    const std::string &catalog() const { return m_catalog; }

private:
    std::string m_catalog;
};

// Reads the stick: off the GUI thread. Throws StickReadError when a
// catalog that is there cannot be read. A WAL-mode database is read from
// a copy, so the stick is left byte for byte as it was. An Engine 1.x library is looked for at
// `enginePath` and, since the stick scan reports none for it, beside the
// PIONEER folder.
StickPathIndex buildStickPathIndex(const QString &pioneerRoot, const QString &enginePath);

// The same for one file, reading the stick (buildStickPathIndex).
TrackRows findTrackRows(const QString &pioneerRoot, const QString &enginePath, const std::string &filePath);

// The change's id for `field` of the file at `filePath`: one per field and
// track, so staging the same field again replaces the earlier value.
QString trackFieldChangeId(TrackField field, const std::string &filePath);

// The value of one field written into every catalog on the stick that
// holds the track, so the three stay level: export.pdb (its rating byte,
// its comment string in place, PdbRowWriter), OneLibrary (content.rating
// and content.djComment, OneLibraryCueWriter) and Engine (Track.rating
// and Track.comment through libdjinterop). The track is found in each by
// its file, the way the moved-file relink finds it (rekordboxLookupSpellings
// for export.pdb, the writers' own lookups for the others), so a
// re-export that renumbered the rows does not matter. A catalog that does
// not list the file is left alone; one that lists it and cannot take the
// value fails the change, and the save puts back everything it wrote.
class TrackFieldEditChange : public PendingChange
{
public:
    // `rows` is where each catalog listed the file when the edit was
    // staged (findTrackRows()): at Save each must list it the same way,
    // or the save fails saying which one does not, rather than writing
    // the others and leaving the catalogs disagreeing.
    TrackFieldEditChange(QString pioneerRoot, QString enginePath, std::string filePath, std::string title,
                         TrackFieldValue value, TrackRows rows);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    std::vector<RekordboxWrite> rekordboxWrites() const override;
    // Counts the save's track edits, so the first one opens each
    // catalog's write session knowing how many writes are coming (one
    // whole-file replace of export.pdb for a large batch, not one each).
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

    const TrackFieldValue &value() const { return m_value; }
    const std::string &filePath() const { return m_filePath; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_filePath;
    std::string m_title;
    TrackFieldValue m_value;
    TrackRows m_rows;
    // The last apply() wrote the rating into export.pdb or OneLibrary.
    bool m_wroteRekordboxSide = false;
};

// The longest comment every catalog holding the file can take (#105).
// OneLibrary's djComment and Engine's Track.comment are SQLite TEXT
// columns with no length of their own, so the limit comes from export.pdb
// alone: what its track row can take wherever it can be moved to (#108),
// in UTF-16 code units (PdbRowWriter::trackCommentCapacity()). No limit
// when export.pdb is absent or does not list the file.
struct CommentLimit
{
    bool limited = false;
    // How many units fit, when limited.
    std::size_t units = 0;
    // The field takes plain ASCII only (units are then characters too).
    bool asciiOnly = false;
    // export.pdb's row is one Seabass does not rewrite: no comment can be
    // written, whatever its length (units is 0).
    bool unwritable = false;
    // Why there is a limit, one plain sentence for a tooltip; empty when
    // there is none.
    std::string reason;
};

// From the rows findTrackRows() found: export.pdb's rows for the file
// decide it. Reads export.pdb whole: off the GUI thread.
CommentLimit commentLimitFor(const QString &pioneerRoot, const TrackRows &rows);

// One track's rows and comment limit, read together off the GUI thread
// for the track details; `error` says why they could not be read.
struct TrackLookup
{
    std::string filePath;
    TrackRows rows;
    CommentLimit limit;
    std::string error;
};
TrackLookup lookUpTrack(const QString &pioneerRoot, const QString &enginePath, const std::string &filePath);

// The plain sentence saying why `limit` holds, empty when it is none.
std::string limitReason(const CommentLimit &limit);

// Why `comment` cannot be written under `limit`, or empty when it can.
std::string commentLimitRefusal(const std::string &comment, const CommentLimit &limit);

}  // namespace seabass::gui
