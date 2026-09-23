// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/junk_cue.hpp"
#include "domain/library_consistency.hpp"
#include <set>

#include "infrastructure/engine/engine_analysis_state.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_sample_rates.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

class ArtworkRescueSources;

class LibraryEditSession;

// Read-only Qt list model over the LibraryConsistencyIssues
// LibraryConsistencyController last computed, across every present
// catalog at once (see the controller's own class comment).
class LibraryConsistencyIssueListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by LibraryConsistencyController; not constructible from QML")

    // QAbstractListModel gives QML no row count of its own: rowCount() is
    // an override with a default argument, so it is not invokable, and a
    // page binding to `model.count` reads undefined and assigns 0 without
    // complaint. That is not hypothetical -- LibraryHealthHubPage's entire
    // summary was built on `issues.count` and reported a clean library
    // however many problems the scan found.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        FormatRole,  // "rekordbox", "engine", or "onelibrary", which catalog this issue is in
        SurvivorRole,  // {sourceId, title, artist, filePath}, or an empty map when there's no survivor
        BrokenTracksRole,  // QVariantList of {sourceId, title, artist, filePath}
        CueMergeNeededRole,
        // This issue's repair/deletion is staged in the edit session.
        StagedRole,
        StagedDescriptionRole,
    };

    explicit LibraryConsistencyIssueListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void clear();
    // Adds one format's worth of freshly-scanned issues to whatever's
    // already shown, how the progressive, per-format scan builds up the
    // combined view instead of replacing it each time (see
    // LibraryConsistencyController::scanNextPendingFormat()).
    void appendIssues(std::vector<domain::LibraryConsistencyIssue> issues);
    // Replaces one specific issue's own entry in place, used right
    // after a per-item repair/delete so that one row updates without
    // disturbing every other format's already-shown results the way a
    // full clear()+appendIssues() rescan would.
    void removeIssueAt(int index);
    const std::vector<domain::LibraryConsistencyIssue> &issues() const { return m_issues; }
    void setStaged(int index, bool staged, const QString &description);
    void clearStaged();
    int count() const { return static_cast<int>(m_issues.size()); }

signals:
    void countChanged();

private:
    std::vector<domain::LibraryConsistencyIssue> m_issues;
    std::vector<QString> m_stagedDescriptions;  // empty = not staged; parallel to m_issues

public:
    // Repairable rows nobody has staged yet: what "Stage All Safe
    // Repairs" would actually do if pressed.
    int repairableUnstaged() const;

private:
};

// Read-only Qt list model over the JunkCueIssues LibraryConsistencyController
// last found, a memory cue sitting at position 0 on some track (see
// domain::JunkCueFinder's own doc comment for why only memory, not hot,
// cues count). Same progressive per-format population as
// LibraryConsistencyIssueListModel above, and populated by the same scan.
class JunkCueIssueListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by LibraryConsistencyController; not constructible from QML")

    // QAbstractListModel gives QML no row count of its own: rowCount() is
    // an override with a default argument, so it is not invokable, and a
    // page binding to `model.count` reads undefined and assigns 0 without
    // complaint. That is not hypothetical -- LibraryHealthHubPage's entire
    // summary was built on `issues.count` and reported a clean library
    // however many problems the scan found.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        FormatRole = Qt::UserRole + 1,  // "rekordbox", "engine", or "onelibrary"
        TitleRole,
        ArtistRole,
        // Same brokenTrackToMap() shape LibraryConsistencyIssueListModel's
        // own survivor/brokenTracks roles already use (sourceId, title,
        // artist, filePath, artworkPath, durationMs, cues) -- everything
        // TrackWaveformCard needs, for the memory-cue section to show one
        // instead of a bare title/artist line.
        TrackRole,
        StagedRole,  // this cue's removal is staged in the edit session
        ReasonRole,      // why this cue is in the list, in the row's own words
        PositionMsRole,  // where the cue this row is about actually sits
    };

    explicit JunkCueIssueListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void clear();
    void appendIssues(std::vector<domain::JunkCueIssue> issues);
    // Removes one row with no write, "Ignore" is just "stop showing me
    // this in the current view," not a persisted dismissal.
    void removeAt(int index);
    const std::vector<domain::JunkCueIssue> &issues() const { return m_issues; }
    void setStaged(int index, bool staged);
    void clearStaged();
    int count() const { return static_cast<int>(m_issues.size()); }

signals:
    void countChanged();

private:
    std::vector<domain::JunkCueIssue> m_issues;
    std::vector<bool> m_staged;  // parallel to m_issues

public:
    // Rows nobody has staged yet: what "Remove All" would still do.
    int unstagedCount() const;

private:
};

// Result of a background scan task for one format, see
// LibraryConsistencyController::scanNextPendingFormat(). Built entirely
// on a worker thread, no access to the controller.
struct LibraryConsistencyScanResult
{
    std::vector<domain::LibraryConsistencyIssue> issues;
    std::vector<domain::JunkCueIssue> junkCues;
    // This format's own playlist membership tally, unfiltered by
    // whatever TrackScope playlistName below scoped `issues`/`junkCues`
    // to -- computed before that filtering, same "picking a playlist
    // never shrinks the picker's own list of choices" convention
    // SyncController's own playlistNames/playlistTrackCounts follow. One
    // format's worth only; LibraryConsistencyController::onScanFinished()
    // merges each format's contribution into the running cross-catalog
    // union as the progressive per-format scan completes.
    QStringList playlistNames;
    QVariantMap playlistTrackCounts;
    // Engine only: which tracks' cover art a player can actually find.
    // Engine's own "import rekordbox library" leaves art pointing at a
    // path on the importing computer, which no player has.
    infrastructure::engine::ArtworkAudit artwork;
    // Engine only: rows that do not say their sample rate, and what
    // their files said when asked during the same pass.
    infrastructure::engine::SampleRateAudit sampleRates;
    // Engine only: how many tracks the player will analyse on first load
    // (#38). One count query, no file reads.
    infrastructure::engine::AnalysisStateAudit analysisState;
    // rekordbox only: the art this catalog holds per audio file, which
    // the Engine pass after it uses to rebuild an Engine copy that is
    // gone or empty. Every scan starts with rekordbox, so it is there by
    // the time Engine asks.
    infrastructure::engine::ArtworkSourceByTrackFile artSources;
    // Engine only: where covers this library has lost can be found again
    // (tags, stick backups). Shared with the repair that follows.
    std::shared_ptr<ArtworkRescueSources> rescue;
    QString errorMessage;
    bool cancelled = false;  // stopped via cancelScan(); nothing else is set
};

// Finds catalog rows whose backing audio file is missing and, where a
// healthy same-catalog duplicate exists, repairs them: merges any cues
// the broken row(s) have onto the survivor (if it doesn't already have
// them), then removes the broken row via that format's own existing
// track-removal primitive. Works across all three catalogs at once,
// scan() finds every format actually present on the stick and scans them
// one after another, adding each format's results to the shared list as
// soon as that format finishes rather than waiting for all three (see
// scanNextPendingFormat()). Every issue carries its own format (read off
// the domain::Track objects it holds, every reader already tags each
// track with the catalog it came from), so repairAll()/repairOne()/
// deleteOrphan() dispatch to the right writer per issue rather than
// assuming one format for the whole list.
//
// Repairs and removals are staged, not written: each button stages one
// PendingChange per issue/cue in the library's LibraryEditSession (the
// first one takes the edit lock), rows show it, and the page's Save
// writes them; a row whose change reached the stick disappears. The
// per-writer staleness guard (RekordboxCueWriter/OneLibraryCueWriter
// etc., each constructed fresh at save time) is what catches "the stick
// changed underneath us."
//
// A row with no healthy survivor anywhere (Kind::Missing) is never
// auto-repaired. There's nothing to consolidate onto. OneLibrary rows
// in that state can be deleted manually via deleteOrphan() (safe,
// independent of anything else); rekordbox/Engine have no equivalent
// "delete with no replacement, clear playlist memberships instead of
// reassigning them" primitive yet, so those stay informational only
// ("re-add via Rekordbox/Engine software"), deleteOrphan() is a no-op
// for them. A Kind::Conflict issue is never auto-resolved either (the
// whole point is that guessing would be wrong). LibraryConsistencyPage
// .qml offers manual resolution for those via CleanupController's own
// existing two-track-merge feature instead of anything in this class.
class LibraryConsistencyController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::LibraryConsistencyIssueListModel *issues READ issuesModel CONSTANT)
    Q_PROPERTY(seabass::gui::JunkCueIssueListModel *junkCues READ junkCuesModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while the read-only scan runs (never during a write): it can
    // be stopped via cancelScan(), which also drops the formats still
    // queued, after which scanCancelled() fires.
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    // Mirrors the session: true while a save is writing to the stick.
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY issuesChanged)
    // Per check, because the page shows them per check: one number beside
    // the buttons that staged it. The total above is what the Save button
    // and the leave guard ask for.
    Q_PROPERTY(int stagedIssueCount READ stagedIssueCount NOTIFY issuesChanged)
    Q_PROPERTY(int stagedJunkCueCount READ stagedJunkCueCount NOTIFY issuesChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(QString scanningFormat READ scanningFormat NOTIFY scanningFormatChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(int repairableCount READ repairableCount NOTIFY issuesChanged)
    // What the "do all of it" buttons have left to do. Distinct from the
    // counts above, which say what the check found: a button that has
    // staged everything it can offer is a button with nothing behind it,
    // and it should look like one.
    Q_PROPERTY(int unstagedRepairableCount READ unstagedRepairableCount NOTIFY issuesChanged)
    Q_PROPERTY(int unstagedJunkCueCount READ unstagedJunkCueCount NOTIFY issuesChanged)
    // The stick itself, before anything about its library: a filesystem
    // the kernel has set read-only takes every write down with it.
    Q_PROPERTY(bool stickReadOnly READ stickReadOnly NOTIFY stickHealthChanged)
    Q_PROPERTY(bool repairingFilesystem READ repairingFilesystem NOTIFY stickHealthChanged)
    Q_PROPERTY(QString filesystemMessage READ filesystemMessage NOTIFY stickHealthChanged)
    Q_PROPERTY(int artworkTracksWithArt READ artworkTracksWithArt NOTIFY artworkChanged)
    Q_PROPERTY(int artworkReadableCount READ artworkReadableCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkUnreadableCount READ artworkUnreadableCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkRepairableCount READ artworkRepairableCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkImportedCount READ artworkImportedCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkMissingFileCount READ artworkMissingFileCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkEmptyFileCount READ artworkEmptyFileCount NOTIFY artworkChanged)
    Q_PROPERTY(int artworkBrokenRowCount READ artworkBrokenRowCount NOTIFY artworkChanged)
    Q_PROPERTY(QString artworkError READ artworkError NOTIFY artworkChanged)
    // Tracks whose Engine row does not say what sample rate they are, and
    // how many of those the files themselves can answer for. Engine turns
    // every cue position into a time with this number, so a missing one
    // is every cue on that track being placed by a guess.
    // #38: what the player will have to analyse. Advice only, so there
    // is no staged/fixable counterpart -- see the header of
    // engine_analysis_state.hpp for why Seabass does not offer to do it.
    Q_PROPERTY(int analysisNotAnalyzedCount READ analysisNotAnalyzedCount NOTIFY analysisStateChanged)
    Q_PROPERTY(int analysisTracksChecked READ analysisTracksChecked NOTIFY analysisStateChanged)
    Q_PROPERTY(bool analysisKnown READ analysisKnown NOTIFY analysisStateChanged)
    Q_PROPERTY(bool analysisLibraryPresent READ analysisLibraryPresent NOTIFY analysisStateChanged)
    Q_PROPERTY(QString analysisError READ analysisError NOTIFY analysisStateChanged)
    Q_PROPERTY(int sampleRateMissingCount READ sampleRateMissingCount NOTIFY sampleRatesChanged)
    Q_PROPERTY(int sampleRateFixableCount READ sampleRateFixableCount NOTIFY sampleRatesChanged)
    Q_PROPERTY(bool sampleRateFillStaged READ sampleRateFillStaged NOTIFY sampleRatesChanged)
    // The check itself could not run. Distinct from finding nothing:
    // without it, a library that would not open showed the same green
    // "every track says what it is" as a healthy one.
    Q_PROPERTY(QString sampleRateError READ sampleRateError NOTIFY sampleRatesChanged)
    // Whether an Engine player will offer to import the rekordbox library
    // over the Engine side on the next insert, and whether the fix for
    // that is staged. See infrastructure/engine/engine_import_state.hpp:
    // accepting that offer overwrites the Engine metadata this app
    // repairs, so it is worth saying before the stick is in the player.
    Q_PROPERTY(bool playerWillOfferImport READ playerWillOfferImport NOTIFY importStateChanged)
    Q_PROPERTY(bool importMarkStaged READ importMarkStaged NOTIFY importStateChanged)
    Q_PROPERTY(bool artworkRepairStaged READ artworkRepairStaged NOTIFY artworkChanged)
    // Backs the Playlist picker in JunkCuePage.qml -- same shape/
    // convention as SyncController's own playlistNames/
    // playlistTrackCounts (index 0 of ["All tracks"] + these is the "no
    // filter" choice; see PlaylistPickerCombo.qml). Unfiltered by
    // whatever playlist scan() was last given, so picking one never
    // shrinks the picker's own list of choices.
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY issuesChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY issuesChanged)

public:
    explicit LibraryConsistencyController(QObject *parent = nullptr);

    LibraryConsistencyIssueListModel *issuesModel() { return &m_model; }
    JunkCueIssueListModel *junkCuesModel() { return &m_junkCueModel; }
    bool busy() const { return m_busy; }
    bool writing() const;
    bool canUndo() const;
    int stagedCount() const { return static_cast<int>(m_stagedIssues.size() + m_stagedJunk.size()); }
    int stagedIssueCount() const { return static_cast<int>(m_stagedIssues.size()); }
    // Cues, not entries in m_stagedJunk: that map holds one entry per
    // TRACK, and the page prints this beside a sentence counting cues
    // ("I found 185 cue(s) sitting at 0:00" over "173 staged"). The
    // model knows which rows are staged, and a row is a cue.
    int stagedJunkCueCount() const { return m_junkCueModel.count() - m_junkCueModel.unstagedCount(); }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    // "rekordbox"/"engine"/"onelibrary" while that format's scan is
    // actually running, empty once the whole sequence finishes, lets
    // the page show "Scanning Engine..." progressively.
    QString scanningFormat() const { return m_scanningFormat; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    // Computed directly from the model rather than counted via realized
    // ListView delegates in QML. ListView virtualizes, so itemAtIndex()
    // is null for anything outside the visible/cache range, which would
    // silently undercount on a long list.
    int repairableCount() const;
    int unstagedRepairableCount() const { return m_model.repairableUnstaged(); }
    int unstagedJunkCueCount() const { return m_junkCueModel.unstagedCount(); }

    // Cover art, Engine only. A track counts as readable when its art is
    // stored the way Engine stores its own: a hash, with the image in
    // "Engine Library/Artwork". Anything still pointing at the importing
    // computer's path is art no player will show.
    bool stickReadOnly() const { return m_stickReadOnly; }
    bool repairingFilesystem() const { return m_repairingFilesystem; }
    // What the last check said, empty until one has run.
    QString filesystemMessage() const { return m_filesystemMessage; }

    int artworkTracksWithArt() const { return m_artwork.tracksWithArt; }
    int artworkReadableCount() const { return m_artwork.readableByAPlayer; }
    int artworkUnreadableCount() const { return static_cast<int>(m_artwork.unreadable.size()); }
    int artworkRepairableCount() const { return m_artwork.repairable(); }
    // The two faults are different things to say to a person, and the
    // notice said the first one about both: an imported path points at the
    // importing computer and can be repaired from the rekordbox art beside
    // it, while a missing cached file is a row that is already right with
    // its image deleted, which nothing here can put back.
    int artworkImportedCount() const;
    int artworkMissingFileCount() const;
    // Art whose file is there and holds nothing a player can draw: what
    // a stick pulled mid-write leaves behind.
    int artworkEmptyFileCount() const;
    // And a third: the track asked for art, and the row it points at has
    // no hash to find it by. Nothing here can repair that.
    int artworkBrokenRowCount() const;
    // Set when the audit could not read the database. Without it an audit
    // that failed looks exactly like a library with nothing wrong: no
    // counts, no notice, no word to the user.
    QString artworkError() const { return QString::fromStdString(m_artwork.error); }
    // Staged as one change per track (the unit the save summary counts),
    // so "staged" is "any of them is".
    bool artworkRepairStaged() const { return !m_stagedArtwork.empty(); }
    // #38. analysisKnown is false for an Engine 1.x library, which has
    // no such column, and for one that could not be read -- neither is
    // "nothing to analyse", and the page must not say so.
    int analysisNotAnalyzedCount() const { return m_analysisState.notAnalyzed; }
    int analysisTracksChecked() const { return m_analysisState.tracksChecked; }
    bool analysisKnown() const { return m_analysisState.error.empty() && m_analysisState.hasColumn; }
    bool analysisLibraryPresent() const { return m_analysisState.libraryPresent; }
    QString analysisError() const { return QString::fromStdString(m_analysisState.error); }
    int sampleRateMissingCount() const { return static_cast<int>(m_sampleRates.missing.size()); }
    int sampleRateFixableCount() const { return m_sampleRates.fixable(); }
    bool sampleRateFillStaged() const { return m_sampleRateFillStaged; }
    QString sampleRateError() const { return QString::fromStdString(m_sampleRates.error); }
    bool playerWillOfferImport() const { return m_importState.playerWillOfferImport(); }
    bool importMarkStaged() const { return m_importMarkStaged; }

    // Scans every format actually present: rekordbox if rekordboxPath is
    // non-empty, engine if enginePath is non-empty, onelibrary if
    // exportLibrary.db exists under rekordboxPath. Progressive: each
    // format's issues are added to the model as soon as that format's
    // scan finishes, not all at once at the end. playlistName empty (the
    // default) scans/checks the whole library, same as before this
    // parameter existed; a real name scopes both the junk-cue list and
    // the consistency check to just that playlist's tracks, via
    // domain::TrackScope -- playlistNames/playlistTrackCounts themselves
    // stay unfiltered so the picker never shrinks its own choices.
    Q_INVOKABLE void scan(const QString &rekordboxPath, const QString &enginePath,
                           const QString &playlistName = QString());

    // Stages repairing every currently-Repairable issue across every
    // format: merged cues onto each survivor where needed, then the
    // broken rows removed. Never touches Conflict/Missing issues.
    Q_INVOKABLE void repairAll();
    // Same, scoped to the single issue at index.
    Q_INVOKABLE void repairOne(int index);
    // Stages deleting a single Missing issue's broken row(s), OneLibrary
    // only (see class comment). No-op for a rekordbox/Engine issue.
    Q_INVOKABLE void deleteOrphan(int index);
    Q_INVOKABLE void unstageIssue(int index);

    // Stages giving every repairable track Engine's own artwork storage:
    // the image copied into the library, the row pointed at it. Save
    // writes it, like every other change on this page.
    // Hands the stick to the platform's own check-and-repair: udisks2
    // behind polkit on Linux, an elevated Repair-Volume behind UAC on
    // Windows, diskutil on macOS. Seabass itself never elevates.
    Q_INVOKABLE void repairStickFilesystem();

    Q_INVOKABLE void repairArtwork();
    // Stages writing every sample rate a file could answer for. Staging
    // only, like every other fix here: Save writes it.
    Q_INVOKABLE void fillSampleRates();
    Q_INVOKABLE void unstageSampleRateFill();
    // Stages telling Engine the rekordbox library is already imported.
    Q_INVOKABLE void markRekordboxImported();
    Q_INVOKABLE void unstageRekordboxImportMark();
    Q_INVOKABLE void unstageArtworkRepair();

    // Stages rewriting the track's full cue list with the offending 0:00
    // memory cue (and any other memory cue also sitting at 0, if somehow
    // more than one) removed, same "pass the complete replacement set"
    // contract as CueWriter::writeHotCues() everywhere else.
    Q_INVOKABLE void removeJunkCue(int index);
    Q_INVOKABLE void removeAllJunkCues();
    Q_INVOKABLE void unstageJunkCue(int index);
    // Local-only dismiss, no write, see JunkCueIssueListModel::removeAt().
    // Unstages the row first if it was staged.
    Q_INVOKABLE void ignoreJunkCue(int index);
    Q_INVOKABLE void ignoreAllJunkCues();

    // Reverts every file the last save touched (the session's undo).
    Q_INVOKABLE void undoLastOperation();

    bool scanCancellable() const { return m_busy && !writing(); }
    Q_INVOKABLE void cancelScan();

signals:
    void scanCancelled();
    void busyChanged();
    void writingChanged();
    void scanProgressChanged();
    void scanningFormatChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void issuesChanged();
    void artworkChanged();
    void analysisStateChanged();
    void sampleRatesChanged();
    void importStateChanged();
    void stickHealthChanged();
    // The repair is over and this is how it went. A property the page
    // could poll would not do: "it worked" and "it did not" are the whole
    // point of pressing the button, and the user who pressed it is
    // already having a bad day -- the page says it in a dialog, once, and
    // needs an edge to do that on.
    void filesystemRepairFinished(bool repaired, bool declined, const QString &message);
    void canUndoChanged();

private:
    void onScanFinished();
    void onFilesystemRepairFinished();
    void scanNextPendingFormat();
    void setBusy(bool busy);
    void setScanProgress(int current, int total);
    void attachSession();
    bool ensureSessionForStaging();
    int repairItemCountHint(const QString &format) const;
    void stageIssue(int index);
    void stageJunkCue(int index);
    int indexOfIssueKey(const QString &issueKey) const;
    int indexOfJunkKey(const QString &junkKey) const;
    void setScanningFormat(const QString &format);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    // A "Staged N ... Press Save" line, which stops being true the moment
    // nothing is staged any more -- the last change landing, an unstage, or
    // a discard. Tracked apart from any other status so clearing it never
    // wipes a message about something else.
    void setStagedStatusMessage(const QString &message);
    void clearStagedStatusIfNothingStaged();
    QString pathForFormat(const QString &format) const;
    std::shared_ptr<QtProgressReporter> makeReporter();
    // Folds one format's own playlist tally (LibraryConsistencyScanResult
    // ::playlistNames/playlistTrackCounts) into the running cross-catalog
    // union, max count per name across whichever catalogs have it -- same
    // semantics as SyncController's own collectPlaylistSummary(), just
    // applied incrementally as each format's progressive scan completes
    // instead of all at once.
    void mergePlaylistSummary(const QStringList &names, const QVariantMap &counts);

    LibraryConsistencyIssueListModel m_model;
    JunkCueIssueListModel m_junkCueModel;
    QFutureWatcher<LibraryConsistencyScanResult> m_watcher;
    application::CancellationToken m_scanCancel;  // fresh per scan(), shared by its per-format tasks
    QPointer<LibraryEditSession> m_session;
    struct StagedInfo
    {
        QString changeId;
        QString description;
    };
    std::map<QString, StagedInfo> m_stagedIssues;  // issue key -> what is staged for it
    std::map<QString, QString> m_stagedJunk;    // junk key -> change id
    infrastructure::engine::ArtworkAudit m_artwork;
    bool m_stickReadOnly = false;
    bool m_repairingFilesystem = false;
    QString m_filesystemMessage;
    QFutureWatcher<infrastructure::media::FilesystemRepairResult> m_repairWatcher;
    // The cover-art changes this page has staged, by change id.
    std::set<QString> m_stagedArtwork;
    infrastructure::engine::SampleRateAudit m_sampleRates;
    infrastructure::engine::AnalysisStateAudit m_analysisState;
    std::set<QString> m_stagedSampleRates;
    bool m_sampleRateFillStaged = false;
    infrastructure::engine::RekordboxImportState m_importState;
    bool m_importMarkStaged = false;
    // A rekordbox repair's OneLibrary mirror can stale another listed
    // issue: re-scan once the save that applied one has finished.
    bool m_rescanAfterSave = false;
    QString m_rekordboxPath;
    QString m_enginePath;
    // The playlistName scan() was last called with -- so the automatic
    // re-scan onWriteFinished() runs after every repair/removal stays
    // scoped to whatever playlist was selected, instead of silently
    // reverting to "All tracks".
    QString m_currentPlaylistName;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
    std::vector<QString> m_pendingScanFormats;
    // Carried from the rekordbox pass to the Engine pass of the same scan.
    infrastructure::engine::ArtworkSourceByTrackFile m_artSources;
    std::shared_ptr<ArtworkRescueSources> m_rescue;
    // Where this computer keeps full stick backups, for the rescue above.
    QString m_backupDirectory;
    bool m_busy = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_scanningFormat;
    QString m_errorMessage;
    QString m_statusMessage;
    bool m_statusIsAboutStaging = false;
};

}  // namespace seabass::gui
