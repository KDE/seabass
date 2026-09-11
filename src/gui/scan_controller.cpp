// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "scan_controller.hpp"

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
        return toLocalFileUrl(track.artworkPath);
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
    };
}

void TrackListModel::setTracks(std::vector<domain::Track> tracks)
{
    beginResetModel();
    m_tracks = std::move(tracks);
    endResetModel();
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

// Runs entirely on a background thread (see ScanController::scan()) - no
// access to the controller itself, so everything it needs travels in by
// value and its result travels back out as a plain struct.
ScanTaskResult runScanTask(QString format, QString path, QString siblingRekordboxPath,
                            std::shared_ptr<QtProgressReporter> reporter, application::CancellationToken cancel)
{
    ScanTaskResult result;
    try {
        auto &catalogCache = LibraryCatalogCache::instance();
        std::vector<domain::Track> tracks;
        if (format == "rekordbox") {
            tracks = catalogCache.tracksFor("rekordbox", path.toStdString(), *reporter, cancel);
        } else if (format == "engine") {
            tracks = catalogCache.tracksFor("engine", path.toStdString(), *reporter, cancel);

            if (!siblingRekordboxPath.isEmpty()) {
                try {
                    // This is a second full scan (to build the artwork
                    // lookup) that can take as long as the one above, give
                    // it the same reporter rather than let the bar sit at
                    // 100% while this runs silently in the background. Goes
                    // through the same cache -- Sync/Stick Statistics may
                    // already have this exact rekordbox catalog cached from
                    // a prior page visit.
                    auto rbTracks =
                        catalogCache.tracksFor("rekordbox", siblingRekordboxPath.toStdString(), *reporter, cancel);

                    std::unordered_map<std::string, std::string> artworkByTitleArtist;
                    for (const auto &rbTrack : rbTracks) {
                        if (rbTrack.artworkPath.empty() || rbTrack.title.empty() || rbTrack.artist.empty()) {
                            continue;
                        }
                        artworkByTitleArtist[domain::normalizeFilename(rbTrack.title + "|" + rbTrack.artist)] =
                            rbTrack.artworkPath;
                    }

                    for (auto &track : tracks) {
                        if (!track.artworkPath.empty() || track.title.empty() || track.artist.empty()) {
                            continue;
                        }
                        auto it = artworkByTitleArtist.find(domain::normalizeFilename(track.title + "|" + track.artist));
                        if (it != artworkByTitleArtist.end()) {
                            track.artworkPath = it->second;
                        }
                    }
                } catch (const application::OperationCancelled &) {
                    throw;  // a cancel is a cancel, even during the nice-to-have part
                } catch (const std::exception &) {
                    // Borrowing cover art from the sibling library is a
                    // nice-to-have, never let it break browsing Engine
                    // tracks on their own.
                }
            }
        } else {
            // format == "onelibrary": path is the same PIONEER root
            // rekordbox uses (exportLibrary.db lives alongside export.pdb
            // under it), not a separate stored path. OneLibrary is a
            // third view onto that same side of the stick, not an
            // independent catalog with its own DetectedStick field.
            tracks = catalogCache.tracksFor("onelibrary", path.toStdString(), *reporter, cancel);
        }
        result.tracks = std::move(tracks);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

ScanController::ScanController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<ScanTaskResult>::finished, this, &ScanController::onScanFinished);
}

void ScanController::scan(const QString &format, const QString &path, const QString &siblingRekordboxPath)
{
    if (m_busy) {
        return;  // a scan is already running, never overlap two
    }
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

    m_scanCancel = application::CancellationToken();
    m_watcher.setFuture(QtConcurrent::run(runScanTask, format, path, siblingRekordboxPath, reporter, m_scanCancel));
}

void ScanController::cancelScan()
{
    if (m_busy) {
        m_scanCancel.cancel();
    }
}

void ScanController::onScanFinished()
{
    ScanTaskResult result = m_watcher.result();

    if (result.cancelled) {
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        setBusy(false);
        return;
    }

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

void ScanController::applyFilters()
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

    m_model.setTracks(std::move(result));
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
        m["artworkPath"] = toLocalFileUrl(track.artworkPath);
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
