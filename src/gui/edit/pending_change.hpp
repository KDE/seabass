// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace seabass::gui
{

class SaveContext;

// One file a change will overwrite, and the label its backup belongs
// under. A save can hold changes of several kinds from one page, and each
// kind keeps its own label, so this carries both rather than assuming one.
struct BackupTarget
{
    std::string file;
    std::string label;
};

struct ChangeOutcome
{
    bool ok = false;
    QString error;

    static ChangeOutcome success() { return {true, {}}; }
    static ChangeOutcome failure(QString error) { return {false, std::move(error)}; }
};

// One staged edit to a library: exactly what one Save step writes. The
// unit is the user-visible thing the summary counts ("5 of 31 tracks
// written"), which is why a change is one track, one group, one settings
// field, one cue -- never a batch. The save loop applies changes in order
// and checks for cancellation only *between* them, so every change is
// either fully on the stick or still pending, never half-written.
//
// apply() runs on a worker thread with no access to any controller or
// model: everything a change needs travels in by value at construction
// (same rule as every background task in src/gui). Per-save resources
// (backups, the operation log, per-format writers and scratch copies)
// come from the SaveContext.
class PendingChange
{
public:
    virtual ~PendingChange() = default;

    // Stable within a session, e.g. "sync:engine:1234". Staging a change
    // with an id already staged replaces the earlier one.
    virtual QString id() const = 0;
    // One human line for the pending list / tooltip.
    virtual QString description() const = 0;
    // Plural noun for the summary: "tracks", "settings", "groups", "cues".
    virtual QString unit() const = 0;
    // Past-tense verb for the summary, in the user's terms rather than
    // the code's: what THEY asked for, not what the save did to make it
    // happen.
    //
    // Everything used to report "written", because that is what a save
    // does. Removing 27 stray cues then said "27 of 27 cues written",
    // which is true of the files and false of the intent -- the user
    // deleted something and was told something was written. A person
    // reading that has to translate it back, and the one case where they
    // most need to trust the message is the one where they just deleted
    // data.
    virtual QString verb() const { return QStringLiteral("written"); }
    // Catalog formats this change writes ("rekordbox", "engine",
    // "onelibrary"), for cache invalidation after the save.
    virtual QStringList formatsTouched() const = 0;
    // Which editing page this change belongs to. One save may only ever
    // hold changes from one page: two pages staging into the same library
    // would each get their own scratch copy of the same database and the
    // second commit would silently discard the first's work. The id's
    // prefix is the page for every change whose page stages exactly one
    // kind; a page that stages several kinds (Library Health stages both
    // repairs and orphan deletions) overrides this to name itself.
    virtual QString owner() const { return id().section(QLatin1Char(':'), 0, 0); }

    // Every file this change will overwrite, if it can say so before it
    // runs. The save loop backs all of them up in one pass BEFORE the
    // first change is applied, so the backup is complete and durable
    // before anything on the stick is touched -- otherwise the backup of
    // item 201 lands only after items 1 to 200 were already overwritten.
    //
    // Returning nothing is allowed and means "I cannot say yet". Those
    // changes keep backing up as they go via SaveContext::backupOnce(),
    // which stays correct either way: it skips a file this already did.
    virtual std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const
    {
        (void)ctx;
        return {};
    }

    virtual ChangeOutcome apply(SaveContext &ctx) = 0;
};

}  // namespace seabass::gui
