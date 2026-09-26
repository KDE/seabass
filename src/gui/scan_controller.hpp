// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/track.hpp"

namespace seabass::gui
{

class LibraryCatalogCache;

// Cover art borrowed from another catalog, by the borrowing track's
// sourceId: an Engine row's fallback is the same song's rekordbox art.
using FallbackArtwork = std::unordered_map<std::string, std::string>;

// Read-only Qt list model over the tracks ScanController last read.
class TrackListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by ScanController; not constructible from QML")

public:
    enum Roles {
        SourceIdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        DurationSecondsRole,
        CueCountRole,
        PlayCountRole,
        FilePathRole,
        ArtworkPathRole,
        BpmRole,
        KeyRole,
        CuesRole,
        PlaylistNamesRole,
        StreamingSourceRole,
        RatingRole,
        BitrateRole,
        CommentRole,
        AlbumRole,
        FallbackArtworkPathRole,
    };

    explicit TrackListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTracks(std::vector<domain::Track> tracks);

    // The same rows again with more in them (Browse's cue pass landing):
    // rows the list already shows are updated where they stand, moved
    // rather than rebuilt when the sort order changed, so the view keeps
    // its delegates, its scroll position and its current row. Anything
    // but the same set of tracks is a new list, and resets like
    // setTracks().
    void updateTracks(std::vector<domain::Track> tracks);

    // Served as fallbackArtworkPath, and as artworkPath for a row whose
    // own catalog names no art. Kept until the next call.
    void setFallbackArtwork(std::shared_ptr<const FallbackArtwork> fallbackArtwork);

    // Random-access read of one row, by index into *this currently
    // displayed* (filtered/sorted) list -- not the full unfiltered
    // library. Every role, keyed by its own role name (same shape a
    // ListView delegate sees), as one QVariantMap: for a caller outside
    // a ListView/Repeater context (the track detail page's own prev/next
    // transition lookup) that needs a specific row's full data without
    // instantiating a delegate for it. Empty map for an out-of-range
    // index. Just reuses data()/roleNames(), no separate conversion
    // logic to keep in sync.
    Q_INVOKABLE QVariantMap trackAt(int index) const;
    // rowCount() itself isn't Q_INVOKABLE (it's an override with a
    // QModelIndex parameter QML can't supply) -- this is the plain,
    // no-argument form callers outside a ListView binding actually need.
    Q_INVOKABLE int trackCount() const { return rowCount(); }

    // The row showing this track now, or -1.
    Q_INVOKABLE int indexOfSourceId(const QString &sourceId) const;

    // The artworkPath and fallbackArtworkPath roles of a track from this
    // scan, as URLs, for the lists that are built outside the model.
    QString artworkFor(const domain::Track &track) const;
    QString fallbackArtworkFor(const domain::Track &track) const;

private:
    std::vector<domain::Track> m_tracks;
    std::shared_ptr<const FallbackArtwork> m_fallbackArtwork;
};

// Result of a background scan task, see ScanController::scan(). Kept
// separate from ScanController's own state since it's built entirely on a
// worker thread, with no access to the controller itself.
struct ScanTaskResult
{
    // Which read this is: the catalog alone (Tracks), the same tracks
    // again with the cues a format keeps outside its catalog (Cues), or
    // once more with what the Full stage adds: the lengths a catalog
    // left out and Seabass probed, and the sizes (Full). Tracks and Cues
    // come through the relay; the task's own result is the Full result,
    // a failure, or a cancel.
    enum class Phase { Tracks, Cues, Full };
    Phase phase = Phase::Tracks;
    std::vector<domain::Track> tracks;
    // Only on the Tracks result: Engine rows' cover art borrowed from
    // the sibling rekordbox catalog, by sourceId.
    std::shared_ptr<const FallbackArtwork> fallbackArtwork;
    // Only on the Tracks result: a Cues result follows.
    bool cuesPending = false;
    QString errorMessage;  // empty on success
    bool cancelled = false;  // stopped via cancelScan(); tracks is empty, errorMessage too
    // The scan() this belongs to, so a result from a scan that has since
    // been replaced or cancelled is dropped.
    std::uint64_t generation = 0;
};

// Carries a scan's first publication (the Tracks result) from its
// worker thread to the controller, while the task goes on to read the
// cues; the second arrives as the task's own result. Owned by the task,
// like its progress reporter, so a controller that is gone is never
// touched: its connection went with it.
class ScanPhaseRelay : public QObject
{
    Q_OBJECT

signals:
    void tracksRead(std::shared_ptr<seabass::gui::ScanTaskResult> result);
};

// Wraps the catalog cache for QML: reads every track out of a rekordbox,
// Engine or OneLibrary library path, exposed as `tracks`. The read itself
// runs on a background thread (via QtConcurrent) since it can take several
// seconds for a large library, scan() returns immediately, and `busy`/
// `scanCurrent`/`scanTotal` track progress for a UI-thread progress bar
// that can actually render while the work happens.
//
// In two phases for rekordbox, whose cues live in per-track ANLZ files
// outside its catalog: the catalog first (a tenth of a second on a stick),
// published at once, then the cues (seconds, cold), published again into
// the same rows. `busy` covers the first phase only; `cuesPending` the
// second. Engine and OneLibrary keep their cues in the catalog, so they
// publish once. Browse never needs file sizes and never asks for them.
class ScanController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::TrackListModel *tracks READ tracksModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while a scan is running: it can be stopped at any time via
    // cancelScan(), after which scanCancelled() fires instead of the
    // results changing (nothing partial is ever kept or cached).
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY playlistNamesChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY playlistNamesChanged)
    Q_PROPERTY(int totalTrackCount READ totalTrackCount NOTIFY playlistNamesChanged)
    // True between the two publications of a rekordbox scan: the list is
    // there, its cue counts are not yet. Sorting by cues meanwhile sorts
    // what is known.
    Q_PROPERTY(bool cuesPending READ cuesPending NOTIFY cuesPendingChanged)

public:
    explicit ScanController(QObject *parent = nullptr);
    // Stops a scan still running for this page, cue phase included,
    // without waiting for it: the task holds nothing of this controller.
    ~ScanController() override;

    // Test seam: every ScanController scans through this cache rather
    // than LibraryCatalogCache::instance() while it is set. nullptr
    // restores the real one. Taken at scan() time.
    static void setCatalogCacheForTesting(LibraryCatalogCache *cache);

    TrackListModel *tracksModel() { return &m_model; }
    bool busy() const { return m_busy; }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString errorMessage() const { return m_errorMessage; }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    int totalTrackCount() const { return static_cast<int>(m_allTracks.size()); }
    bool cuesPending() const { return m_cuesPending; }

    // format is "rekordbox" or "engine"; path is the corresponding
    // DetectedStick.rekordboxPath / .enginePath. siblingRekordboxPath (only
    // used when format is "engine") gives Engine tracks the same song's
    // rekordbox art, matched by title+artist, as fallbackArtworkPath: shown
    // when the Engine art does not load, and as the art itself for a track
    // its Engine catalog names none for. A scan whose first phase is
    // still running is ignored rather than overlapped; one in its cue
    // phase is stopped and replaced.
    Q_INVOKABLE void scan(const QString &format, const QString &path, const QString &siblingRekordboxPath = QString());

    // True if exportLibrary.db (OneLibrary) exists for the stick at this
    // PIONEER root. It isn't present on every rekordbox export (only
    // newer hardware/rekordbox versions create it), so ScanPage.qml uses
    // this to disable rather than hide the third library-source option.
    // A cheap file-existence check, not a scan, safe to call directly
    // from a QML binding.
    Q_INVOKABLE bool hasOneLibrary(const QString &pioneerRoot) const;

    // playlistName empty shows every scanned track again.
    Q_INVOKABLE void filterByPlaylist(const QString &playlistName);

    // Matches against title or artist, case-insensitive substring. Empty
    // clears the search.
    Q_INVOKABLE void search(const QString &query);

    // field is one of "playlist" (the selected playlist's own order, or
    // scan order when no playlist is selected), "title", "artist", "key",
    // "bpm", "duration", "cues", "plays".
    Q_INVOKABLE void setSort(const QString &field, bool ascending);

    // For the manual "Merge with..." picker (ScanPage.qml): searches the
    // full last-scanned track list (not the page's own filtered/sorted
    // `tracks` model. A search typed into the picker must never disturb
    // what's currently shown on the page underneath it) by title/artist,
    // same case-insensitive substring match as search() above, excluding
    // excludeSourceId (the track already chosen as one side of the
    // merge). Returns light {sourceId, title, artist, durationSeconds,
    // filePath} maps, not full Track data, just enough to render picker
    // rows and disambiguate near-identical titles by file path. Capped at
    // 50 matches; a query that vague isn't narrowing anything anyway.
    Q_INVOKABLE QVariantList findMergeCandidates(const QString &query, const QString &excludeSourceId) const;

    // Every other track by one artist, for the "more by this artist" list
    // on the track panel. Matched on the whole artist string rather than
    // a substring: "Kollektiv Turmstrasse" and "Kollektiv Turmstrasse &
    // Someone" are different credits, and folding them together would
    // offer a jump to a track the DJ did not ask about. Searches the
    // unfiltered set, so the list still works while a playlist or a
    // search is narrowing the view.
    Q_INVOKABLE QVariantList tracksByArtist(const QString &artist, const QString &excludeSourceId) const;

    // For the Matching panel (Experimental): searches the full
    // last-scanned track list (same m_allTracks findMergeCandidates()
    // already searches, not the page's filtered/sorted `tracks` model) for
    // tracks compatible with anchorSourceId's key/BPM, excluding streaming
    // tracks (Engine/TIDAL) the same way findMergeCandidates() does --
    // never worth suggesting here either. keyTiers is an additive
    // (union) selection of domain::keyRelationMatchesAnyMode()'s own
    // four tier names -- "match" (same key), "relative" (relative major/
    // minor), "harmonic" (one step around the wheel, same mode -- the
    // classic harmonic-mixing move, either energy direction), "energymix"
    // (the one-step energy-mix diagonal) -- a track matches if its
    // relation to the anchor is any tier in the set. An empty keyTiers
    // means no key filtering at all
    // (sorted by BPM closeness instead), same vocabulary the key-tier row
    // uses.
    // bpmTolerancePct is a hard filter -- anything outside it is excluded,
    // not just deprioritized. minRating <= 0 disables the rating filter.
    // textQuery is a case-insensitive title/artist substring match, same
    // as findMergeCandidates(). targetPlaylistName is a filter too, not
    // just where Before/After would write: empty ("All tracks" selected)
    // searches the whole library same as before, but a real playlist
    // name restricts candidates to that playlist's own members -- This
    // Playlist behaves exactly like any other filter here (key tiers,
    // rating, BPM), not a separate write-target concept layered on top.
    // Each result carries {sourceId, title, artist, key, keyRelation,
    // camelotLabel, bpm, rating, artworkPath, durationSeconds,
    // playlistNames}. camelotLabel is e.g. "8A" (empty if the key didn't
    // parse). keyRelation is domain::keyRelationLabel() of how the
    // anchor's key relates to this track's own key -- "anchor -> track"
    // order, so "Energy Boost"/"Energy Drop" read as "mixing from the
    // anchor into this track" ("Same key", "Relative major/minor", ...;
    // empty if either key didn't parse) -- shown regardless of keyTiers,
    // including when keyTiers is empty, where it's arguably most useful
    // since that's the one state that doesn't already filter by it.
    Q_INVOKABLE QVariantList findCompatibleTracks(const QString &anchorSourceId, const QStringList &keyTiers,
                                                   int minRating, double bpmTolerancePct, const QString &textQuery,
                                                   const QString &targetPlaylistName) const;

    // Thin QML-facing wrapper around domain::classifyKeyRelation() --
    // the track detail page's own prev/next transition panel uses this
    // rather than re-deriving key relationships itself. `relation` is a
    // lowercase machine-readable tag ("same"/"relative"/"adjacentup"/
    // "adjacentdown"/"energymix"/"unrelated"/"unknown") so a caller can
    // pick its own wording/color per tier (e.g. a friendlier "Dissonant
    // transition" for "unrelated" than domain::keyRelationLabel()'s own
    // neutral "Unrelated key", which several other, less opinionated
    // callers also use); `label` is that same neutral default for a
    // caller that doesn't want to override it. Like
    // classifyKeyRelation() itself, "adjacentup"/"adjacentdown" depend on
    // argument order (keyA -> keyB); every other tag is symmetric.
    Q_INVOKABLE QVariantMap keyRelation(const QString &keyA, const QString &keyB) const;

    // Backs Settings' "Hide tracks from streaming services" toggle.
    // Streaming tracks (Engine/TIDAL) still show up in m_allTracks (they
    // came from a real scan), this just excludes them from what
    // applyFilters() actually displays, same as the search/playlist
    // filters above. Unrelated to the *unconditional* exclusion of
    // streaming tracks from Clean Up/Sync/Library Consistency. This is
    // purely a display preference.
    Q_INVOKABLE void setHideStreamingTracks(bool hide);

    bool scanCancellable() const { return m_busy; }
    // Also stops the cue phase: scanCancelled() fires at once and the
    // cues never land, even while the read itself is still finishing on
    // its thread.
    Q_INVOKABLE void cancelScan();

signals:
    void busyChanged();
    void scanProgressChanged();
    void errorMessageChanged();
    void playlistNamesChanged();
    void scanCancelled();
    void cuesPendingChanged();
    // Each time the list is published: once for Engine and OneLibrary,
    // twice for rekordbox (the catalog, then its cues), once when a
    // rekordbox scan is cancelled between the two. cuesLanded: this is
    // the second one, the same rows updated where they stand.
    void tracksPublished(bool cuesLanded);
    // The Full stage landed: rows that had no length show one now. The
    // rows update where they stand; nothing else about the page changes.
    void detailsPublished();

private:
    void setBusy(bool busy);
    void setCuesPending(bool pending);
    void setScanProgress(int current, int total);
    void setErrorMessage(const QString &message);
    // inPlace: the same tracks with more in them, see
    // TrackListModel::updateTracks().
    void applyFilters(bool inPlace = false);
    void onTracksRead(std::shared_ptr<ScanTaskResult> result);
    void onScanFinished();
    // Both publications and the task's failures end here.
    void handleResult(ScanTaskResult &result);
    void publishTracks(ScanTaskResult &result);
    void publishCues(ScanTaskResult &result);
    void publishDetails(ScanTaskResult &result);

    TrackListModel m_model;
    QFutureWatcher<ScanTaskResult> m_watcher;
    application::CancellationToken m_scanCancel;  // fresh per scan(), so a stale cancel never hits a new scan
    std::uint64_t m_scanGeneration = 0;
    std::vector<domain::Track> m_allTracks;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
    QString m_currentPlaylistFilter;
    QString m_currentSearchQuery;
    QString m_sortField = "playlist";
    bool m_sortAscending = true;
    bool m_hideStreamingTracks = false;
    bool m_busy = false;
    bool m_cuesPending = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_errorMessage;
};

}  // namespace seabass::gui
