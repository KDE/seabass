// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "scan_controller.hpp"

#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "domain/camelot_key.hpp"
#include "domain/track_matching.hpp"
#include "domain/track_scope.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/local_file_url.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

TrackListModel::TrackListModel(QObject *parent) : QAbstractListModel(parent) {}

int TrackListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_tracks.size());
}

QVariant TrackListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_tracks.size()) {
        return {};
    }
    const auto &track = m_tracks[static_cast<size_t>(index.row())];
    switch (role) {
    case SourceIdRole:
        return QString::fromStdString(track.sourceId);
    case TitleRole:
        return QString::fromStdString(track.title);
    case ArtistRole:
        return QString::fromStdString(track.artist);
    case DurationSecondsRole:
        return track.durationSeconds;
    case CueCountRole:
        return static_cast<int>(track.cues.size());
    case PlayCountRole:
        return track.playCount ? *track.playCount : -1;
    case FilePathRole:
        return QString::fromStdString(track.filePath);
    case ArtworkPathRole:
        // A row whose catalog names no art at all shows the borrowed art
        // outright, as it did before the fallback existed; one whose art
        // is named but missing on the stick gets it from ArtworkImage.
        return artworkFor(track);
    case FallbackArtworkPathRole:
        return fallbackArtworkFor(track);
    case BpmRole:
        return track.bpm;
    case KeyRole:
        return QString::fromStdString(track.key);
    case CuesRole: {
        QVariantList cues;
        for (const auto &c : track.cues) {
            QVariantMap m;
            m["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
            m["hotCueNumber"] = c.hotCueNumber;
            m["positionMs"] = c.positionMs;
            m["isLoop"] = c.isLoop;
            m["loopEndMs"] = c.loopEndMs;
            m["color"] = QString::fromStdString(c.color);
            m["comment"] = QString::fromStdString(c.comment);
            cues << m;
        }
        return cues;
    }
    case PlaylistNamesRole: {
        QStringList names;
        for (const auto &p : track.playlists) {
            names << QString::fromStdString(p.name);
        }
        return names;
    }
    case StreamingSourceRole:
        return QString::fromStdString(track.streamingSource);
    case RatingRole:
        return track.rating ? *track.rating : -1;
    case BitrateRole:
        // 0 means "this format does not record one" as much as it means
        // "unknown" -- Engine stores no bitrate at all -- so the panel
        // hides the field rather than showing "0 kbps".
        return track.bitrate;
    case CommentRole:
        return QString::fromStdString(track.comment);
    case AlbumRole:
        return QString::fromStdString(track.album);
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    return {
        {SourceIdRole, "sourceId"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {DurationSecondsRole, "durationSeconds"},
        {CueCountRole, "cueCount"},
        {PlayCountRole, "playCount"},
        {FilePathRole, "filePath"},
        {ArtworkPathRole, "artworkPath"},
        {BpmRole, "bpm"},
        {KeyRole, "key"},
        {CuesRole, "cues"},
        {PlaylistNamesRole, "playlistNames"},
        {StreamingSourceRole, "streamingSource"},
        {RatingRole, "rating"},
        {BitrateRole, "bitrate"},
        {CommentRole, "comment"},
        {AlbumRole, "album"},
        {FallbackArtworkPathRole, "fallbackArtworkPath"},
    };
}

void TrackListModel::setTracks(std::vector<domain::Track> tracks)
{
    beginResetModel();
    m_tracks = std::move(tracks);
    endResetModel();
}

void TrackListModel::updateTracks(std::vector<domain::Track> tracks)
{
    // The same tracks, or a new list? By sourceId, which is unique within
    // one catalog; a repeated one makes the moves below ambiguous, so it
    // counts as a new list too.
    bool sameTracks = tracks.size() == m_tracks.size();
    if (sameTracks) {
        std::unordered_map<std::string, int> shown;
        shown.reserve(m_tracks.size());
        for (const auto &track : m_tracks) {
            sameTracks = shown.emplace(track.sourceId, 0).second;
            if (!sameTracks) {
                break;
            }
        }
        for (size_t i = 0; sameTracks && i < tracks.size(); ++i) {
            auto it = shown.find(tracks[i].sourceId);
            sameTracks = it != shown.end() && it->second++ == 0;
        }
    }
    if (!sameTracks) {
        setTracks(std::move(tracks));
        return;
    }

    // Into the new order one move at a time (a sort by cues reorders when
    // the cues land): a move keeps the delegate, where a layout change
    // would make the view rebuild every row. Nothing moves when the order
    // held, which is every sort but the one by cues.
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (m_tracks[i].sourceId == tracks[i].sourceId) {
            continue;
        }
        size_t from = i + 1;
        while (m_tracks[from].sourceId != tracks[i].sourceId) {
            ++from;
        }
        beginMoveRows(QModelIndex(), static_cast<int>(from), static_cast<int>(from), QModelIndex(),
                      static_cast<int>(i));
        std::rotate(m_tracks.begin() + static_cast<std::ptrdiff_t>(i),
                    m_tracks.begin() + static_cast<std::ptrdiff_t>(from),
                    m_tracks.begin() + static_cast<std::ptrdiff_t>(from) + 1);
        endMoveRows();
    }
    m_tracks = std::move(tracks);
    if (!m_tracks.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_tracks.size()) - 1));
    }
}

void TrackListModel::setFallbackArtwork(std::shared_ptr<const FallbackArtwork> fallbackArtwork)
{
    m_fallbackArtwork = std::move(fallbackArtwork);
}

QString TrackListModel::artworkFor(const domain::Track &track) const
{
    return track.artworkPath.empty() ? fallbackArtworkFor(track) : toLocalFileUrl(track.artworkPath);
}

QString TrackListModel::fallbackArtworkFor(const domain::Track &track) const
{
    if (!m_fallbackArtwork) {
        return {};
    }
    const auto it = m_fallbackArtwork->find(track.sourceId);
    return it == m_fallbackArtwork->end() ? QString() : toLocalFileUrl(it->second);
}

int TrackListModel::indexOfSourceId(const QString &sourceId) const
{
    const std::string wanted = sourceId.toStdString();
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].sourceId == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QVariantMap TrackListModel::trackAt(int index) const
{
    QVariantMap m;
    if (index < 0 || static_cast<size_t>(index) >= m_tracks.size()) {
        return m;
    }
    const QModelIndex idx = createIndex(index, 0);
    const auto roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        m[QString::fromUtf8(it.value())] = data(idx, it.key());
    }
    return m;
}

namespace
{

LibraryCatalogCache *s_catalogCacheForTesting = nullptr;

// Engine rows' fallback art: the same song's rekordbox art, matched on
// title and artist. Borrowed for every Engine row the rekordbox side has
// art for, not only the ones without art of their own: the readers no
// longer look for the art file, so an Engine row naming art that is not
// on the stick reads the same as one whose art is there, and only the
// image itself finds out, at display time.
std::shared_ptr<const FallbackArtwork> borrowRekordboxArt(const std::vector<domain::Track> &engineTracks,
                                                          const std::vector<domain::Track> &rekordboxTracks)
{
    std::unordered_map<std::string, std::string> artworkByTitleArtist;
    for (const auto &rbTrack : rekordboxTracks) {
        if (rbTrack.artworkPath.empty() || rbTrack.title.empty() || rbTrack.artist.empty()) {
            continue;
        }
        artworkByTitleArtist[domain::normalizeFilename(rbTrack.title + "|" + rbTrack.artist)] = rbTrack.artworkPath;
    }
    auto fallback = std::make_shared<FallbackArtwork>();
    for (const auto &track : engineTracks) {
        if (track.title.empty() || track.artist.empty()) {
            continue;
        }
        auto it = artworkByTitleArtist.find(domain::normalizeFilename(track.title + "|" + track.artist));
        if (it != artworkByTitleArtist.end() && it->second != track.artworkPath) {
            (*fallback)[track.sourceId] = it->second;
        }
    }
    return fallback;
}

// Runs entirely on a background thread (see ScanController::scan()) - no
// access to the controller itself, so everything it needs travels in by
// value and its results travel back out: the Tracks result through the
// relay, then as the task's own result the Cues result for rekordbox, or
// a cancelled or failed result in place of whichever did not happen.
ScanTaskResult runScanTask(LibraryCatalogCache *catalogCache, QString format, QString path,
                           QString siblingRekordboxPath, std::shared_ptr<QtProgressReporter> reporter,
                           std::shared_ptr<ScanPhaseRelay> relay, application::CancellationToken cancel,
                           std::uint64_t generation)
{
    const auto report = [generation](ScanTaskResult result) {
        result.generation = generation;
        return result;
    };
    // "onelibrary": path is the same PIONEER root rekordbox uses
    // (exportLibrary.db lives alongside export.pdb under it), not a
    // separate stored path. OneLibrary is a third view onto that same
    // side of the stick, not an independent catalog with its own
    // DetectedStick field.
    const std::string catalog =
        format == "rekordbox" ? "rekordbox" : (format == "engine" ? "engine" : "onelibrary");
    // Engine and OneLibrary carry their cues in the catalog itself, so
    // their Tracks stage already has them; only rekordbox reads its cues
    // from a file per track, and only rekordbox has a second phase.
    const bool cuesOutsideCatalog = catalog == "rekordbox";
    bool tracksPublished = false;
    try {
        ScanTaskResult tracksResult;
        tracksResult.phase = ScanTaskResult::Phase::Tracks;
        tracksResult.tracks =
            catalogCache->tracksFor(catalog, path.toStdString(), LibraryCatalogCache::Detail::Tracks, *reporter, cancel);

        if (catalog == "engine" && !siblingRekordboxPath.isEmpty()) {
            try {
                // A second catalog read, for the art only, through the
                // same cache and with the same reporter, so the bar does
                // not sit at 100% while it runs. The catalog alone: the
                // art is named there, and the cues are no use here.
                const auto rbTracks = catalogCache->tracksFor("rekordbox", siblingRekordboxPath.toStdString(),
                                                              LibraryCatalogCache::Detail::Tracks, *reporter, cancel);
                tracksResult.fallbackArtwork = borrowRekordboxArt(tracksResult.tracks, rbTracks);
            } catch (const application::OperationCancelled &) {
                throw;  // a cancel is a cancel, even during the nice-to-have part
            } catch (const std::exception &) {
                // Borrowing cover art from the sibling library is a
                // nice-to-have, never let it break browsing Engine
                // tracks on their own.
            }
        }
        cancel.throwIfCancelled();
        tracksResult.cuesPending = cuesOutsideCatalog;
        tracksResult.generation = generation;
        emit relay->tracksRead(std::make_shared<ScanTaskResult>(std::move(tracksResult)));
        tracksPublished = true;
        if (cuesOutsideCatalog) {
            // The cues: waits for the prefetch's cue pass when it is
            // reading this catalog already, runs the pass here otherwise.
            // No progress reported: the list is up, and the page says the
            // cues are on their way without a bar over it.
            ScanTaskResult cuesResult;
            cuesResult.phase = ScanTaskResult::Phase::Cues;
            cuesResult.tracks = catalogCache->tracksFor(catalog, path.toStdString(), LibraryCatalogCache::Detail::Cues,
                                                        application::NullProgressReporter::instance(), cancel);
            // Checked again: a wait on another thread's pass does not see
            // the cancel, and a page that has let go of this scan must not
            // get its cues after all.
            cancel.throwIfCancelled();
            cuesResult.generation = generation;
            emit relay->tracksRead(std::make_shared<ScanTaskResult>(std::move(cuesResult)));
        }

        // And the rest: a catalog that records no length for a track (an
        // Engine 3.x library leaves most of them out) gets it from the
        // Full stage, which probes the file, so a list that showed those
        // rows without a length fills them in. Waits for the prefetch's
        // Full pass or runs it here, like the cues.
        ScanTaskResult fullResult;
        fullResult.phase = ScanTaskResult::Phase::Full;
        fullResult.tracks = catalogCache->tracksFor(catalog, path.toStdString(), LibraryCatalogCache::Detail::Full,
                                                    application::NullProgressReporter::instance(), cancel);
        cancel.throwIfCancelled();
        return report(std::move(fullResult));
    } catch (const application::OperationCancelled &) {
        ScanTaskResult cancelled;
        cancelled.phase = tracksPublished ? ScanTaskResult::Phase::Cues : ScanTaskResult::Phase::Tracks;
        cancelled.cancelled = true;
        return report(std::move(cancelled));
    } catch (const std::exception &e) {
        ScanTaskResult failed;
        failed.phase = tracksPublished ? ScanTaskResult::Phase::Cues : ScanTaskResult::Phase::Tracks;
        failed.errorMessage = QString::fromStdString(e.what());
        return report(std::move(failed));
    } catch (...) {
        ScanTaskResult failed;
        failed.phase = tracksPublished ? ScanTaskResult::Phase::Cues : ScanTaskResult::Phase::Tracks;
        failed.errorMessage = QStringLiteral("Unknown error");
        return report(std::move(failed));
    }
}

}  // namespace

void ScanController::setCatalogCacheForTesting(LibraryCatalogCache *cache)
{
    s_catalogCacheForTesting = cache;
}

ScanController::ScanController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<ScanTaskResult>::finished, this, &ScanController::onScanFinished);
}

ScanController::~ScanController()
{
    m_scanCancel.cancel();
}

void ScanController::scan(const QString &format, const QString &path, const QString &siblingRekordboxPath)
{
    if (m_busy) {
        return;  // a scan is already running, never overlap two
    }
    // A cue phase still running belongs to the list this scan replaces.
    m_scanCancel.cancel();
    setCuesPending(false);
    setErrorMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    // The reporter is owned by the background task (via shared_ptr, kept
    // alive for exactly as long as the task runs), not by this controller.
    // If the user navigates away and this ScanController is destroyed
    // mid-scan, the task keeps running harmlessly in the background instead
    // of touching a dangling object. Signals are connected with `this` as
    // the context object, so Qt stops delivering them once we're gone.
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this,
            [this](const QString &, int total) { setScanProgress(0, total); });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });

    auto relay = std::make_shared<ScanPhaseRelay>();
    connect(relay.get(), &ScanPhaseRelay::tracksRead, this, &ScanController::onTracksRead);

    LibraryCatalogCache *cache =
        s_catalogCacheForTesting != nullptr ? s_catalogCacheForTesting : &LibraryCatalogCache::instance();
    m_scanCancel = application::CancellationToken();
    ++m_scanGeneration;
    m_watcher.setFuture(QtConcurrent::run(runScanTask, cache, format, path, siblingRekordboxPath, reporter, relay,
                                          m_scanCancel, m_scanGeneration));
}

void ScanController::cancelScan()
{
    if (!m_busy && !m_cuesPending) {
        return;
    }
    // Let go at once, in either phase, and say so once. The read notices
    // the cancel at its next track, or once the pass it is waiting on is
    // done, and whatever it reports then belongs to a generation nobody
    // is listening for. That matters in the Tracks phase too: the task
    // may have relayed its list a moment before this, still queued for
    // this thread. Under the old generation that list was published after
    // the cancel and the page popped from under it, and every handler of
    // tracksPublished ran for a scan the user had just cancelled.
    m_scanCancel.cancel();
    ++m_scanGeneration;
    setCuesPending(false);
    setBusy(false);
    emit scanCancelled();
}

void ScanController::onTracksRead(std::shared_ptr<ScanTaskResult> result)
{
    if (result) {
        handleResult(*result);
    }
}

void ScanController::onScanFinished()
{
    QString thrown;
    ScanTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        // The task catches everything it throws, so this is one that got
        // out anyway; it belongs to the scan the watcher is watching.
        result.generation = m_scanGeneration;
        result.errorMessage = thrown;
    }
    if (!result.cancelled && result.errorMessage.isEmpty() && result.phase == ScanTaskResult::Phase::Tracks) {
        return;  // published through the relay already, and that was all
    }
    handleResult(result);
}

void ScanController::handleResult(ScanTaskResult &result)
{
    if (result.generation != m_scanGeneration) {
        return;
    }

    if (result.cancelled) {
        setCuesPending(false);
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        // A failed cue phase leaves the list it already published up.
        setErrorMessage(result.errorMessage);
        setCuesPending(false);
        setBusy(false);
        return;
    }
    switch (result.phase) {
    case ScanTaskResult::Phase::Tracks:
        publishTracks(result);
        break;
    case ScanTaskResult::Phase::Cues:
        publishCues(result);
        break;
    case ScanTaskResult::Phase::Full:
        publishDetails(result);
        break;
    }
}

void ScanController::publishDetails(ScanTaskResult &result)
{
    // The same tracks with the lengths and sizes the Full stage added.
    // Nothing about the page's state changes: the rows update where
    // they stand, and only a listener that cares hears about it.
    m_allTracks = std::move(result.tracks);
    applyFilters(true);
    emit detailsPublished();
}

void ScanController::publishTracks(ScanTaskResult &result)
{
    std::set<std::string> uniquePlaylistNames;
    std::unordered_map<std::string, int> countByPlaylist;
    for (const auto &track : result.tracks) {
        for (const auto &playlist : track.playlists) {
            uniquePlaylistNames.insert(playlist.name);
            countByPlaylist[playlist.name]++;
        }
    }
    m_playlistNames.clear();
    m_playlistTrackCounts.clear();
    for (const auto &name : uniquePlaylistNames) {
        QString qName = QString::fromStdString(name);
        m_playlistNames << qName;
        m_playlistTrackCounts[qName] = countByPlaylist[name];
    }

    // m_allTracks (which totalTrackCount() reads) must be updated before
    // playlistNamesChanged fires. QML bindings that read totalTrackCount
    // in response to that signal would otherwise see the previous scan's
    // track count for one notification cycle.
    m_allTracks = std::move(result.tracks);
    m_model.setFallbackArtwork(std::move(result.fallbackArtwork));
    // Before the list and before busy clears, so whatever reacts to
    // either already knows whether the cue counts it sees are final.
    setCuesPending(result.cuesPending);
    emit playlistNamesChanged();

    // Playlist selection is catalog-specific (a name picked in one
    // format's playlist list may not exist, or mean the same thing, in
    // another), so that's cleared on every fresh scan. The search query
    // is not: it's just free text, and the search field's own displayed
    // text doesn't get cleared alongside it (there's no reverse binding
    // from m_currentSearchQuery back to the QML field), so clearing it
    // here used to leave the field showing a query no longer actually
    // applied. Keeping it applied here instead means the field's text
    // and the actually-filtered results never drift apart.
    m_currentPlaylistFilter.clear();
    applyFilters();

    setBusy(false);
    emit tracksPublished(false);
}

void ScanController::publishCues(ScanTaskResult &result)
{
    // The same tracks with their cues: the playlists are what they were,
    // and so is whatever the page has selected among them, so none of
    // that is announced again. The rows update where they stand.
    m_allTracks = std::move(result.tracks);
    applyFilters(true);
    setCuesPending(false);
    emit tracksPublished(true);
}

void ScanController::filterByPlaylist(const QString &playlistName)
{
    m_currentPlaylistFilter = playlistName;
    applyFilters();
}

void ScanController::search(const QString &query)
{
    m_currentSearchQuery = query;
    applyFilters();
}

bool ScanController::hasOneLibrary(const QString &pioneerRoot) const
{
    return infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.toStdString());
}

void ScanController::setSort(const QString &field, bool ascending)
{
    m_sortField = field;
    m_sortAscending = ascending;
    applyFilters();
}

void ScanController::setHideStreamingTracks(bool hide)
{
    if (m_hideStreamingTracks == hide) {
        return;
    }
    m_hideStreamingTracks = hide;
    applyFilters();
}

QVariantList ScanController::tracksByArtist(const QString &artist, const QString &excludeSourceId) const
{
    QVariantList result;
    const QString wanted = artist.trimmed();
    if (wanted.isEmpty()) {
        return result;
    }
    const std::string exclude = excludeSourceId.toStdString();
    for (const auto &track : m_allTracks) {
        if (track.sourceId == exclude) {
            continue;
        }
        const QString candidate = QString::fromStdString(track.artist).trimmed();
        if (candidate.compare(wanted, Qt::CaseInsensitive) != 0) {
            continue;
        }
        QVariantMap m;
        m["sourceId"] = QString::fromStdString(track.sourceId);
        m["title"] = QString::fromStdString(track.title);
        m["durationSeconds"] = track.durationSeconds;
        m["bpm"] = track.bpm;
        m["key"] = QString::fromStdString(track.key);
        m["cueCount"] = static_cast<int>(track.cues.size());
        m["artworkPath"] = m_model.artworkFor(track);
        m["fallbackArtworkPath"] = m_model.fallbackArtworkFor(track);
        // Where the track can be found, for the row's tooltip.
        QStringList playlists;
        for (const auto &membership : track.playlists) {
            playlists << QString::fromStdString(membership.name);
        }
        m["playlistNames"] = playlists;
        result.append(m);
    }
    // By title, so the same artist reads the same way every time. The
    // scan's own sort order is whatever the user chose for the main
    // list, which would make this list reshuffle under them.
    std::sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["title"].toString().compare(b.toMap()["title"].toString(), Qt::CaseInsensitive) < 0;
    });
    return result;
}

QVariantList ScanController::findMergeCandidates(const QString &query, const QString &excludeSourceId) const
{
    QVariantList result;
    if (query.isEmpty()) {
        return result;
    }
    QString lowerQuery = query.toLower();
    std::string exclude = excludeSourceId.toStdString();
    for (const auto &track : m_allTracks) {
        if (track.sourceId == exclude) {
            continue;
        }
        // Streaming tracks (Engine/TIDAL) have no real local file.
        // Never suggest merging with one. See
        // domain::Track::streamingSource's own doc comment.
        if (!track.streamingSource.empty()) {
            continue;
        }
        QString title = QString::fromStdString(track.title);
        QString artist = QString::fromStdString(track.artist);
        if (!title.toLower().contains(lowerQuery) && !artist.toLower().contains(lowerQuery)) {
            continue;
        }
        QVariantMap m;
        m["sourceId"] = QString::fromStdString(track.sourceId);
        m["title"] = title;
        m["artist"] = artist;
        m["durationSeconds"] = track.durationSeconds;
        m["filePath"] = QString::fromStdString(track.filePath);
        result << m;
        if (result.size() >= 50) {
            break;
        }
    }
    return result;
}

namespace
{

std::optional<int> playlistPosition(const domain::Track &track, const std::string &playlistName)
{
    for (const auto &p : track.playlists) {
        if (p.name == playlistName) {
            return p.position;
        }
    }
    return std::nullopt;
}

}  // namespace

void ScanController::applyFilters(bool inPlace)
{
    std::vector<domain::Track> result = m_allTracks;
    std::string playlistFilter = m_currentPlaylistFilter.toStdString();

    if (!m_currentPlaylistFilter.isEmpty()) {
        result = domain::filterByScope(result, domain::TrackScope::playlist(playlistFilter));
    }

    if (m_hideStreamingTracks) {
        std::vector<domain::Track> filtered;
        for (const auto &track : result) {
            if (track.streamingSource.empty()) {
                filtered.push_back(track);
            }
        }
        result = std::move(filtered);
    }

    if (!m_currentSearchQuery.isEmpty()) {
        // Deliberately QString::toLower(), not domain::TrackScope::search()
        // -- TrackScope's search is ASCII-only case folding (see its own
        // doc comment), while this box has to handle real music metadata
        // correctly (accented artist/title names are common), which needs
        // QString's Unicode-aware case folding. Swapping this to
        // TrackScope would silently regress search for exactly the
        // tracks this codebase already goes out of its way to handle
        // correctly elsewhere (see KeyBadge.qml's own Unicode handling).
        QString query = m_currentSearchQuery.toLower();
        std::vector<domain::Track> filtered;
        for (const auto &track : result) {
            QString title = QString::fromStdString(track.title).toLower();
            QString artist = QString::fromStdString(track.artist).toLower();
            if (title.contains(query) || artist.contains(query)) {
                filtered.push_back(track);
            }
        }
        result = std::move(filtered);
    }

    // A strict weak ordering must return false for both (a, b) and (b, a)
    // when the two compare equal. Negating "a < b" to get descending order
    // breaks that (both (a, b) and (b, a) would return true for ties), so
    // descending order is done by swapping the arguments instead.
    auto lessAscending = [&](const domain::Track &a, const domain::Track &b) {
        if (m_sortField == "title") {
            return a.title < b.title;
        }
        if (m_sortField == "artist") {
            return a.artist < b.artist;
        }
        if (m_sortField == "key") {
            // Unparseable/missing keys sort as the lowest wheel position
            // (mirrors "plays"'s value_or(-1) sentinel below), so they
            // consistently land at one end rather than being scattered
            // alphabetically as plain strings would.
            auto parsedA = domain::CamelotKey::parse(a.key).value_or(domain::CamelotKey{});
            auto parsedB = domain::CamelotKey::parse(b.key).value_or(domain::CamelotKey{});
            return std::make_pair(parsedA.number, parsedA.isMinor) < std::make_pair(parsedB.number, parsedB.isMinor);
        }
        if (m_sortField == "bpm") {
            return a.bpm < b.bpm;
        }
        if (m_sortField == "duration") {
            return a.durationSeconds < b.durationSeconds;
        }
        if (m_sortField == "cues") {
            return a.cues.size() < b.cues.size();
        }
        if (m_sortField == "plays") {
            return a.playCount.value_or(-1) < b.playCount.value_or(-1);
        }
        // "playlist", the selected playlist's own track order; when no
        // playlist is selected this leaves every position unknown, so
        // stable_sort just preserves scan order.
        auto posA = playlistPosition(a, playlistFilter);
        auto posB = playlistPosition(b, playlistFilter);
        return posA.value_or(std::numeric_limits<int>::max()) < posB.value_or(std::numeric_limits<int>::max());
    };
    std::stable_sort(result.begin(), result.end(), [&](const domain::Track &a, const domain::Track &b) {
        return m_sortAscending ? lessAscending(a, b) : lessAscending(b, a);
    });

    if (inPlace) {
        m_model.updateTracks(std::move(result));
    } else {
        m_model.setTracks(std::move(result));
    }
}

QVariantList ScanController::findCompatibleTracks(const QString &anchorSourceId, const QStringList &keyTiers,
                                                    int minRating, double bpmTolerancePct, const QString &textQuery,
                                                    const QString &targetPlaylistName) const
{
    QVariantList result;
    std::string anchorId = anchorSourceId.toStdString();
    const domain::Track *anchor = nullptr;
    for (const auto &track : m_allTracks) {
        if (track.sourceId == anchorId) {
            anchor = &track;
            break;
        }
    }
    if (!anchor) {
        return result;
    }

    std::vector<std::string> tiers;
    tiers.reserve(static_cast<size_t>(keyTiers.size()));
    for (const auto &tier : keyTiers) {
        tiers.push_back(tier.toStdString());
    }
    bool keyFilterActive = !tiers.empty();
    std::string targetPlaylist = targetPlaylistName.toStdString();
    QString lowerQuery = textQuery.toLower();
    double bpmLow = anchor->bpm * (1.0 - bpmTolerancePct / 100.0);
    double bpmHigh = anchor->bpm * (1.0 + bpmTolerancePct / 100.0);

    // Sorted by key relation (closer first, per KeyRelation's own
    // declaration order -- see camelot_key.hpp) then BPM distance from
    // the anchor; Ignore Key sorts by BPM distance alone, since key
    // relation isn't a filter in that mode and isn't a meaningful order
    // either.
    std::vector<std::pair<domain::KeyRelation, const domain::Track *>> ranked;
    for (const auto &track : m_allTracks) {
        if (track.sourceId == anchorId) {
            continue;
        }
        // Streaming tracks (Engine/TIDAL) are valid playlist members in
        // principle, but never worth surfacing as a "compatible track to
        // add" suggestion here -- see findMergeCandidates()'s own
        // identical exclusion just above.
        if (!track.streamingSource.empty()) {
            continue;
        }
        // anchor first, candidate second -- classifyKeyRelation()'s
        // Adjacent split is directional (see its own doc comment), and
        // "anchor -> candidate" is the natural reading here: the anchor
        // is what's already selected/playing, the candidate is what you'd
        // mix into next.
        domain::KeyRelation relation = domain::classifyKeyRelation(anchor->key, track.key);
        if (!domain::keyRelationMatchesAnyMode(relation, tiers)) {
            continue;
        }
        if (track.bpm < bpmLow || track.bpm > bpmHigh) {
            continue;
        }
        if (minRating > 0 && (!track.rating || *track.rating < minRating)) {
            continue;
        }
        if (!lowerQuery.isEmpty()) {
            QString title = QString::fromStdString(track.title).toLower();
            QString artist = QString::fromStdString(track.artist).toLower();
            if (!title.contains(lowerQuery) && !artist.contains(lowerQuery)) {
                continue;
            }
        }
        // This Playlist is a filter like any other here, not just the
        // write target: with a real playlist selected (not "All
        // tracks"), Matching Tracks is scoped to that playlist's own
        // members -- finding a track to reorder within it, never one to
        // pull in from elsewhere. Only with "All tracks" selected
        // (targetPlaylist empty) does the search range over the whole
        // library, but Before/After also has nowhere real to write in
        // that case (see MatchingPage.qml's hasTarget).
        if (!targetPlaylist.empty() && !playlistPosition(track, targetPlaylist).has_value()) {
            continue;
        }
        ranked.emplace_back(relation, &track);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [anchor, keyFilterActive](const auto &a, const auto &b) {
        if (keyFilterActive && a.first != b.first) {
            return static_cast<int>(a.first) < static_cast<int>(b.first);
        }
        double bpmDistanceA = a.second->bpm > anchor->bpm ? a.second->bpm - anchor->bpm : anchor->bpm - a.second->bpm;
        double bpmDistanceB = b.second->bpm > anchor->bpm ? b.second->bpm - anchor->bpm : anchor->bpm - b.second->bpm;
        return bpmDistanceA < bpmDistanceB;
    });

    for (const auto &[relation, trackPtr] : ranked) {
        const domain::Track &track = *trackPtr;
        QVariantMap m;
        m["sourceId"] = QString::fromStdString(track.sourceId);
        m["title"] = QString::fromStdString(track.title);
        m["artist"] = QString::fromStdString(track.artist);
        m["key"] = QString::fromStdString(track.key);
        m["keyRelation"] = QString::fromStdString(domain::keyRelationLabel(relation));
        auto parsedKey = domain::CamelotKey::parse(track.key);
        m["camelotLabel"] =
            parsedKey ? QString::number(parsedKey->number) + (parsedKey->isMinor ? "A" : "B") : QString();
        m["bpm"] = track.bpm;
        m["rating"] = track.rating ? *track.rating : -1;
        m["artworkPath"] = m_model.artworkFor(track);
        m["fallbackArtworkPath"] = m_model.fallbackArtworkFor(track);
        m["durationSeconds"] = track.durationSeconds;
        QStringList playlistNames;
        for (const auto &p : track.playlists) {
            playlistNames << QString::fromStdString(p.name);
        }
        m["playlistNames"] = playlistNames;
        result << m;
        if (result.size() >= 100) {
            break;
        }
    }
    return result;
}

QVariantMap ScanController::keyRelation(const QString &keyA, const QString &keyB) const
{
    domain::KeyRelation relation = domain::classifyKeyRelation(keyA.toStdString(), keyB.toStdString());
    QVariantMap m;
    m["label"] = QString::fromStdString(domain::keyRelationLabel(relation));
    switch (relation) {
    case domain::KeyRelation::Same:
        m["relation"] = QStringLiteral("same");
        break;
    case domain::KeyRelation::Relative:
        m["relation"] = QStringLiteral("relative");
        break;
    case domain::KeyRelation::AdjacentUp:
        m["relation"] = QStringLiteral("adjacentup");
        break;
    case domain::KeyRelation::AdjacentDown:
        m["relation"] = QStringLiteral("adjacentdown");
        break;
    case domain::KeyRelation::EnergyMix:
        m["relation"] = QStringLiteral("energymix");
        break;
    case domain::KeyRelation::Unrelated:
        m["relation"] = QStringLiteral("unrelated");
        break;
    case domain::KeyRelation::Unknown:
    default:
        m["relation"] = QStringLiteral("unknown");
        break;
    }
    return m;
}

void ScanController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void ScanController::setCuesPending(bool pending)
{
    if (m_cuesPending == pending) {
        return;
    }
    m_cuesPending = pending;
    emit cuesPendingChanged();
}

void ScanController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void ScanController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
