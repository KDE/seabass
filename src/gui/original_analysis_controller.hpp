// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/analysis_restore_prepare.hpp"
#include "gui/edit/direct_write_hold.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Restore original analysis, for Browse's playlist menu: the tracks of a
// playlist whose analysis by rekordbox Analyse again kept on the stick
// (before it first replaced it) are each given it back, the player's too
// where it is kept with it, one RestoreAnalysisChange per track, staged in
// the stick's edit session; nothing is written until the save. A track
// whose player's analysis alone is kept (Beat Grids & Waveforms) is not
// counted nor touched: Restore Engine's Analysis is its way back.
//
// keptCount() is the menu's question, answered on a worker
// (refreshKeptCount()): how many of the playlist's tracks have an original
// kept, by the kept files' names alone (countKeptOriginals()), the two
// folders listed once until the next save. restoreOriginalAnalysis() prepares the changes on a worker and
// stages them; lastRestore() then holds the summary, and cueReview() the
// tracks whose cue points Analyse again moved, each with the switch that
// moves them back with the grid (off unless every move is under the
// threshold set in Preferences, as for Analyse again).
//
// Not behind the Experimental switch: it is the way back from Analyse
// again, and stays there when the feature is switched off.
class OriginalAnalysisController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The playlist the last answered count is of, and the count; -1 while
    // none is known.
    Q_PROPERTY(QString keptCountPlaylist READ keptCountPlaylist NOTIFY keptCountChanged)
    Q_PROPERTY(int keptCount READ keptCount NOTIFY keptCountChanged)
    // {playlist, staged, tracks, leftAlone: [{name, why}], cueTracks,
    // cuesToMove, autoCueTracks, askedCueTracks, cueFollowThresholdMs,
    // blockedCueTracks, unknownOriginTracks, cuesCannotMoveTracks}; a
    // track whose cue points cannot go back is counted only in the last.
    Q_PROPERTY(QVariantMap lastRestore READ lastRestore NOTIFY restoreReady)
    // The staged restores whose review moves cue points:
    // [{changeId, subject, moves, largestMoveMs, moveCues, chosen,
    // cannotMove}]; `cannotMove` says why the cue points cannot go back
    // (empty when they can), and such a track's switch stays off.
    Q_PROPERTY(QVariantList cueReview READ cueReview NOTIFY cueReviewChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY stagedChanged)

public:
    explicit OriginalAnalysisController(QObject *parent = nullptr);
    ~OriginalAnalysisController() override;

    bool busy() const { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QString keptCountPlaylist() const { return m_keptCountPlaylist; }
    int keptCount() const { return m_keptCount; }
    QVariantMap lastRestore() const { return m_lastRestore; }
    QVariantList cueReview() const;
    int stagedCount() const { return static_cast<int>(m_staged.size()); }

    // How many of the playlist's tracks have an original analysis kept;
    // the answer comes as keptCountChanged().
    Q_INVOKABLE void refreshKeptCount(const QString &rekordboxPath, const QString &enginePath, const QString &playlist);
    // The last count answered for the playlist on that stick, at once, or
    // -1 when none is known or the kept files changed since (a save, a
    // removal in Free Up Space): for a menu that has to be right the
    // moment it opens and must not change while it is open.
    Q_INVOKABLE int cachedKeptCount(const QString &rekordboxPath, const QString &enginePath,
                                    const QString &playlist) const;
    // Reads the stick's playlists and kept folders in the background, so
    // cachedKeptCount() knows every playlist's count before its menu first
    // opens: when Browse loads a stick, and again after each save on it.
    Q_INVOKABLE void warmKeptCounts(const QString &rekordboxPath, const QString &enginePath);
    // Stages a restore for each such track of the playlist; false, with
    // errorMessage set, when it cannot start. restoreReady() when staged.
    Q_INVOKABLE bool restoreOriginalAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                             const QString &playlist);
    // The person's choice for one track's cue points, or every track's;
    // a track whose cue points cannot go back keeps its switch off. False
    // while a save writes.
    Q_INVOKABLE bool setMoveCuesBack(const QString &changeId, bool move);
    Q_INVOKABLE bool setMoveCuesBackForAll(bool move);
    // Takes the staged restores back.
    Q_INVOKABLE void dropStaged();

signals:
    void busyChanged();
    void errorMessageChanged();
    void keptCountChanged();
    void restoreReady();
    void cueReviewChanged();
    void stagedChanged();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    void onPrepared();
    void onCounted();
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);
    void prune();
    // The stick's identity, which the counts are kept under.
    QString stickIdFor(const QString &rekordboxPath, const QString &enginePath) const;

    QPointer<LibraryEditSession> m_session;
    QFutureWatcher<PreparedAnalysisRestore> m_prepareWatcher;
    QFutureWatcher<int> m_countWatcher;
    application::CancellationToken m_cancel;
    AnalysisRestoreRequest m_request;
    QString m_preparingPlaylist;
    QString m_countingPlaylist;
    std::optional<AnalysisRestoreRequest> m_countNext;
    AnalysisRestoreRequest m_counting;
    // Catalog path to stick identity, and the paths each warmed stick was
    // loaded by.
    mutable std::map<QString, QString> m_stickIds;
    std::map<QString, std::pair<QString, QString>> m_warmPaths;
    std::vector<QFuture<bool>> m_warmFutures;
    // Never replaced, so the destructor stops every warm still running.
    application::CancellationToken m_warmCancel;
    QString m_keptCountPlaylist;
    int m_keptCount = -1;
    bool m_busy = false;
    QString m_errorMessage;
    QVariantMap m_lastRestore;
    std::vector<std::shared_ptr<RestoreAnalysisChange>> m_staged;
};

// Free Up Space's kept original analyses: rekordbox's kept by Analyse
// again, and the player's of the same tracks (one Beat Grids & Waveforms
// kept alone is not listed: it is that page's), in four groups:
// of tracks on the stick and of tracks gone from it, for each. Removing a
// group deletes its files for good, a direct write under the stick's
// edit lock, as the page's other removals are, taken by the same path
// (Engine's first). Every group starts unticked: deleting is always the
// person's choice, and of a track not on the stick under its name all
// that is known is that, not that it was deleted.
class KeptAnalysisSpaceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // [{kind: "rekordbox" | "engine", gone, count, bytes, sizeHuman,
    // included}], in that order: rekordbox, the player, then the gone.
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY groupsChanged)
    Q_PROPERTY(QString totalBytesHuman READ totalBytesHuman NOTIFY groupsChanged)
    Q_PROPERTY(int includedCount READ includedCount NOTIFY groupsChanged)
    Q_PROPERTY(QString includedBytesHuman READ includedBytesHuman NOTIFY groupsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool removing READ removing NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY groupsChanged)

public:
    explicit KeptAnalysisSpaceController(QObject *parent = nullptr);
    ~KeptAnalysisSpaceController() override;

    struct Group
    {
        QString kind;
        bool gone = false;
        std::vector<infrastructure::local::KeptAnalysisFileInfo> files;
        bool included = false;
        std::uint64_t bytes() const;
    };
    struct Listing
    {
        std::vector<Group> groups;
        QString error;
    };
    struct Removal
    {
        int removed = 0;
        int failed = 0;
        int total = 0;
        QString error;
        // Every kept file the removal took away or tried to.
        std::set<std::string> touched;
    };
    // The listing as a worker makes it; public for a test.
    static Listing listKept(const std::string &stickRoot, const std::string &enginePath);

    QVariantList groups() const;
    int totalCount() const;
    QString totalBytesHuman() const;
    int includedCount() const;
    QString includedBytesHuman() const;
    bool busy() const { return m_listing || m_removing; }
    bool removing() const { return m_removing; }
    QString errorMessage() const { return m_error; }

    Q_INVOKABLE void refresh(const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void setIncluded(int index, bool included);
    // Deletes every file of the ticked groups; removeFinished() carries
    // the OperationSummaryDialog shape.
    Q_INVOKABLE void removeIncluded();

signals:
    void groupsChanged();
    void busyChanged();
    void removeFinished(const QVariantMap &summary);
    // The lock of `libraryId` is held elsewhere: the id the removal asked
    // for, for the locked dialog and its Remove Lock.
    void lockRefused(const QVariantMap &holder, const QString &libraryId);

private:
    void onListed();
    void onRemoved();
    // The catalog path the stick's lock is taken by.
    QString lockPath() const;

    QFutureWatcher<Listing> m_listWatcher;
    QFutureWatcher<Removal> m_removeWatcher;
    DirectWriteHold m_writeHold;
    std::vector<Group> m_groups;
    QString m_error;
    QString m_rekordboxPath;
    QString m_enginePath;
    bool m_listing = false;
    bool m_removing = false;
};

}  // namespace seabass::gui
