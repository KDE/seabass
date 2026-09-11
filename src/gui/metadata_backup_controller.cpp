#include "metadata_backup_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/metadata_merge.hpp"
#include "gui/local_file_url.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using infrastructure::local::MetadataSource;
using infrastructure::local::MetadataStore;
using infrastructure::local::StoredTrack;

namespace
{

// One page of the browse list. Big enough that a normal library needs
// one or two, small enough that the first page paints immediately.
constexpr int PageSize = 200;

QString durationText(double seconds)
{
    if (seconds <= 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Runs entirely on a background thread -- no access to the controller.
MetadataBackupTaskResult runBackupTask(QString libraryPath, QString libraryId, QString stickLabel,
                                        std::shared_ptr<QtProgressReporter> reporter,
                                        application::CancellationToken cancel)
{
    MetadataBackupTaskResult result;
    try {
        const auto read = readAllStickCatalogs(libraryPath.toStdString(), *reporter, cancel);
        for (const auto &name : read.catalogs.present()) {
            result.catalogsRead << QString::fromStdString(name);
        }
        for (const auto &name : read.failed) {
            result.catalogsUnreadable << QString::fromStdString(name);
        }
        if (result.catalogsRead.isEmpty()) {
            result.errorMessage = result.catalogsUnreadable.isEmpty()
                ? QStringLiteral("No rekordbox or Engine library was found on this stick.")
                : QStringLiteral("None of this stick's catalogs could be read.");
            return result;
        }

        // Rows from every catalog, folded into one Track per file: the
        // three formats are one library, so a file they all list is one
        // stored track holding the union of their cues. Storing each
        // catalog in turn instead would make the second one's read
        // conflict with the first one's write over the same row.
        std::vector<domain::Track> rows;
        for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.oneLibrary, &read.catalogs.engine}) {
            if (*catalog) {
                rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
            }
        }
        const auto files = application::collapseCatalogRows(rows);

        MetadataSource source;
        source.stickRoot = fs::path(libraryPath.toStdString()).parent_path();
        source.libraryId = libraryId.toStdString();
        source.stickLabel = stickLabel.toStdString();
        // Read from the files, not from the rows: the merge rule's last
        // step needs to know whether this stick has been worked on since
        // the entry was stored, and a catalog's mtime is the only date a
        // stick offers.
        source.catalogModifiedAt = catalogsLastModified(libraryPath.toStdString());

        MetadataStore store;
        result.summary = store.store(files, source, *reporter, cancel);
        result.succeeded = true;
    } catch (const application::OperationCancelled &) {
        result.succeeded = true;
        result.summary.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
}

}  // namespace

// ---- model ----------------------------------------------------------

StoredTrackListModel::StoredTrackListModel(QObject *parent) : QAbstractListModel(parent) {}

int StoredTrackListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QHash<int, QByteArray> StoredTrackListModel::roleNames() const
{
    return {
        {TrackIdRole, "trackId"},         {TitleRole, "title"},
        {ArtistRole, "artist"},           {FilenameRole, "filename"},
        {RelativePathRole, "relativePath"}, {DurationTextRole, "durationText"},
        {BpmRole, "bpm"},                 {MusicKeyRole, "musicKey"},
        {RatingRole, "rating"},           {CommentRole, "comment"},
        {CueCountRole, "cueCount"},       {PlaylistCountRole, "playlistCount"},
        {ArtworkUrlRole, "artworkUrl"},   {StickLabelRole, "stickLabel"},
        {UpdatedAtRole, "updatedAt"},
        {StagedForDeletionRole, "stagedForDeletion"},
    };
}

QVariant StoredTrackListModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const StoredTrack &row = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TrackIdRole:
        return QVariant::fromValue(static_cast<qint64>(row.id));
    case TitleRole:
        return QString::fromStdString(row.title);
    case ArtistRole:
        return QString::fromStdString(row.artist);
    case FilenameRole:
        return QString::fromStdString(row.filename);
    case RelativePathRole:
        return QString::fromStdString(row.relativePath);
    case DurationTextRole:
        return durationText(row.durationSeconds);
    case BpmRole:
        return row.bpm;
    case MusicKeyRole:
        return QString::fromStdString(row.key);
    case RatingRole:
        // -1, not 0: "unrated" and "rated zero stars" are different
        // facts and the store keeps them apart, so the model must too.
        return row.rating.value_or(-1);
    case CommentRole:
        return QString::fromStdString(row.comment);
    case CueCountRole:
        return row.cueCount;
    case PlaylistCountRole:
        return row.playlistCount;
    case ArtworkUrlRole:
        return toLocalFileUrl(row.artworkPath);
    case StickLabelRole:
        return QString::fromStdString(row.stickLabel);
    case UpdatedAtRole:
        return QString::fromStdString(row.updatedAt);
    case StagedForDeletionRole:
        return m_stagedForDeletion.contains(static_cast<qint64>(row.id));
    default:
        return {};
    }
}

void StoredTrackListModel::reset(std::vector<StoredTrack> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    // The marks go with the old list. reset() is what a changed search
    // term and a finished delete both run through, and a mark that
    // outlived the rows it was made on would be invisible: the user
    // would tick three rows, type in the search box, press Delete and
    // lose tracks they could no longer see. Only loadMore()'s append
    // keeps the marks, and that genuinely is the same list.
    m_stagedForDeletion.clear();
    endResetModel();
}

void StoredTrackListModel::append(const std::vector<StoredTrack> &rows)
{
    if (rows.empty()) {
        return;
    }
    const int first = static_cast<int>(m_rows.size());
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(rows.size()) - 1);
    m_rows.insert(m_rows.end(), rows.begin(), rows.end());
    endInsertRows();
}

// ---- selection and deletion staging ---------------------------------

qint64 StoredTrackListModel::trackIdAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return 0;
    }
    return static_cast<qint64>(m_rows[static_cast<std::size_t>(row)].id);
}

void StoredTrackListModel::emitRowChanged(int row, const QList<int> &roles)
{
    const QModelIndex at = index(row);
    emit dataChanged(at, at, roles);
}

void StoredTrackListModel::setStagedForDeletion(int row, bool staged)
{
    const qint64 id = trackIdAt(row);
    if (id == 0) {
        return;
    }
    if (staged) {
        m_stagedForDeletion.insert(id);
    } else {
        m_stagedForDeletion.remove(id);
    }
    emitRowChanged(row, {StagedForDeletionRole});
}

void StoredTrackListModel::stageAllLoadedForDeletion()
{
    if (m_rows.empty()) {
        return;
    }
    for (const auto &row : m_rows) {
        m_stagedForDeletion.insert(static_cast<qint64>(row.id));
    }
    emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {StagedForDeletionRole});
}

void StoredTrackListModel::clearDeletionStaging()
{
    if (m_stagedForDeletion.isEmpty()) {
        return;
    }
    m_stagedForDeletion.clear();
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {StagedForDeletionRole});
    }
}

QList<qint64> StoredTrackListModel::stagedForDeletionIds() const
{
    // Straight from the set rather than walked in list order: a row can
    // be staged and then scrolled out of the loaded page, and a delete
    // that quietly dropped it would leave the page reporting a count it
    // did not act on.
    return QList<qint64>(m_stagedForDeletion.begin(), m_stagedForDeletion.end());
}

// ---- controller -----------------------------------------------------

MetadataBackupController::MetadataBackupController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<MetadataBackupTaskResult>::finished, this,
            &MetadataBackupController::onBackupFinished);
    refresh();
}

MetadataBackupController::~MetadataBackupController()
{
    m_cancel.cancel();
    if (m_watcher.isRunning()) {
        m_watcher.waitForFinished();
    }
}

QString MetadataBackupController::storeLocation() const
{
    return QString::fromStdString(MetadataStore::defaultDatabasePath().parent_path().string());
}

MetadataStore *MetadataBackupController::store()
{
    if (!m_store) {
        try {
            m_store = std::make_unique<MetadataStore>();
        } catch (const std::exception &e) {
            setErrorMessage(QString::fromUtf8(e.what()));
            return nullptr;
        }
    }
    return m_store.get();
}

void MetadataBackupController::refresh()
{
    auto *db = store();
    if (!db) {
        return;
    }
    try {
        m_storedTrackCount = db->trackCount();
        m_matchCount = m_search.isEmpty() ? m_storedTrackCount : db->trackCount(m_search.toStdString());
        m_artworkBytes = static_cast<qint64>(db->artworkBytesOnDisk());
        m_browseModel.reset(db->browse(m_search.toStdString(), PageSize, 0));
        emit storeChanged();
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromUtf8(e.what()));
    }
}

void MetadataBackupController::search(const QString &text)
{
    m_search = text.trimmed();
    refresh();
}

void MetadataBackupController::loadMore()
{
    auto *db = store();
    if (!db || !canLoadMore()) {
        return;
    }
    try {
        m_browseModel.append(db->browse(m_search.toStdString(), PageSize, m_browseModel.rowCount()));
        emit storeChanged();
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromUtf8(e.what()));
    }
}

QVariantList MetadataBackupController::cuesFor(qint64 trackId)
{
    QVariantList cues;
    auto *db = store();
    if (!db) {
        return cues;
    }
    for (const auto &cue : db->cuesFor(trackId)) {
        QVariantMap entry;
        entry["kind"] = cue.kind == domain::CuePoint::Kind::Hot ? "hot" : "memory";
        entry["hotCueNumber"] = cue.hotCueNumber;
        entry["positionMs"] = cue.positionMs;
        entry["positionText"] = durationText(cue.positionMs / 1000.0);
        entry["color"] = QString::fromStdString(cue.color);
        entry["comment"] = QString::fromStdString(cue.comment);
        entry["isLoop"] = cue.isLoop;
        entry["loopEndMs"] = cue.loopEndMs;
        cues.append(entry);
    }
    return cues;
}

QString MetadataBackupController::cueSummaryFor(qint64 trackId)
{
    const QVariantList cues = cuesFor(trackId);
    if (cues.isEmpty()) {
        return {};
    }
    QStringList lines;
    for (const QVariant &entry : cues) {
        const QVariantMap cue = entry.toMap();
        QString line = cue["kind"].toString() == QStringLiteral("hot")
            ? QStringLiteral("Hot cue %1").arg(cue["hotCueNumber"].toInt())
            : QStringLiteral("Memory cue");
        line += QStringLiteral(" at ") + cue["positionText"].toString();
        if (cue["isLoop"].toBool()) {
            line += QStringLiteral(" (loop)");
        }
        const QString comment = cue["comment"].toString();
        if (!comment.isEmpty()) {
            line += QStringLiteral(": ") + comment;
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

QString MetadataBackupController::mergeRuleHelp() const
{
    return QString::fromStdString(domain::mergeRuleExplanation());
}

void MetadataBackupController::toggleStagedForDeletion(int row)
{
    const qint64 id = m_browseModel.trackIdAt(row);
    if (id == 0) {
        return;
    }
    m_browseModel.setStagedForDeletion(row, !m_browseModel.isStagedForDeletion(id));
    emit selectionChanged();
}

void MetadataBackupController::stageAllForDeletion()
{
    m_browseModel.stageAllLoadedForDeletion();
    emit selectionChanged();
}

void MetadataBackupController::clearDeletionStaging()
{
    m_browseModel.clearDeletionStaging();
    emit selectionChanged();
}

int MetadataBackupController::deleteStaged()
{
    auto *db = store();
    if (!db) {
        return 0;
    }
    const QList<qint64> ids = m_browseModel.stagedForDeletionIds();
    if (ids.isEmpty()) {
        return 0;
    }
    int removed = 0;
    try {
        removed = db->removeTracks(std::vector<std::int64_t>(ids.begin(), ids.end()));
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromUtf8(e.what()));
        return 0;
    }
    // refresh() resets the model, which clears the marks: the rows they
    // referred to are gone.
    refresh();
    emit selectionChanged();
    return removed;
}

QStringList MetadataBackupController::playlistsFor(qint64 trackId)
{
    QStringList names;
    auto *db = store();
    if (!db) {
        return names;
    }
    for (const auto &playlist : db->playlistsFor(trackId)) {
        names << QString::fromStdString(playlist.name);
    }
    return names;
}

void MetadataBackupController::backUp(const QString &libraryPath, const QString &libraryId,
                                       const QString &stickLabel)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    m_hasResult = false;
    m_lastRun.clear();
    emit resultChanged();
    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
    setProgress(0, 0);
    setCurrentPhase(QStringLiteral("Reading this stick"));
    m_cancel = application::CancellationToken();
    setBusy(true);
    m_watcher.setFuture(
        QtConcurrent::run(runBackupTask, libraryPath, libraryId, stickLabel, makeReporter(), m_cancel));
}

void MetadataBackupController::cancel()
{
    m_cancel.cancel();
}

void MetadataBackupController::onBackupFinished()
{
    const MetadataBackupTaskResult result = m_watcher.result();
    setBusy(false);
    setCurrentPhase({});
    if (!result.succeeded) {
        setErrorMessage(result.errorMessage);
        return;
    }

    const auto &summary = result.summary;
    m_lastRun = QVariantMap{
        {"tracksSeen", summary.tracksSeen},
        {"tracksAdded", summary.tracksAdded},
        {"tracksUpdated", summary.tracksUpdated},
        {"tracksSkipped", summary.tracksSkipped},
        {"tracksUnchanged", summary.tracksUnchanged},
        {"tracksWithoutIdentity", summary.tracksWithoutIdentity},
        {"cuesStored", summary.cuesStored},
        {"artworkFilesAdded", summary.artworkFilesAdded},
        {"artworkBytesAdded", static_cast<qint64>(summary.artworkBytesAdded)},
        {"cancelled", summary.cancelled},
        {"catalogsRead", result.catalogsRead},
        {"catalogsUnreadable", result.catalogsUnreadable},
    };
    m_hasResult = true;
    emit resultChanged();
    refresh();
}

std::shared_ptr<QtProgressReporter> MetadataBackupController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    // Each phase (one per catalog read, then the store write) reports
    // its own 0..N. Adding the previous phases' totals as a baseline
    // keeps one bar moving forward instead of several restarting, which
    // is the only shape that reads as progress.
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        m_phaseBaseline += m_currentPhaseTotal;
        m_currentPhaseTotal = total;
        setCurrentPhase(label);
        setProgress(m_phaseBaseline, m_phaseBaseline + total);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setProgress(m_phaseBaseline + current, m_phaseBaseline + m_currentPhaseTotal); });
    return reporter;
}

void MetadataBackupController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void MetadataBackupController::setProgress(int current, int total)
{
    if (m_progressCurrent == current && m_progressTotal == total) {
        return;
    }
    m_progressCurrent = current;
    m_progressTotal = total;
    emit progressChanged();
}

void MetadataBackupController::setCurrentPhase(const QString &phase)
{
    if (m_currentPhase == phase) {
        return;
    }
    m_currentPhase = phase;
    emit currentPhaseChanged();
}

void MetadataBackupController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
