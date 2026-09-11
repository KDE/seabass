#include "metadata_backup_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <map>
#include <string>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/metadata_backup_plan.hpp"
#include "domain/metadata_merge.hpp"
#include "gui/local_file_url.hpp"
#include "gui/metadata_row_text.hpp"
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


// Reads every catalog on the stick, folds them into files, and works out
// what storing them would change. Runs entirely on a background thread --
// no access to the controller.
MetadataBackupScanResult runScanTask(QString libraryPath, std::shared_ptr<QtProgressReporter> reporter,
                                      application::CancellationToken cancel)
{
    MetadataBackupScanResult result;
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
        // proposal holding the union of their cues, not three proposals
        // that each look like a separate track to back up.
        std::vector<domain::Track> rows;
        for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.oneLibrary, &read.catalogs.engine}) {
            if (*catalog) {
                rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
            }
        }
        const auto stickTracks = application::collapseCatalogRows(rows);

        // Playlists to filter by, counted off the collapsed files rather
        // than the raw rows: a track three catalogs list belongs to its
        // playlist once, not three times. Sorted, because a std::map is,
        // and a picker whose order changed between scans of the same
        // stick would be its own small bug.
        std::map<std::string, int> playlistCounts;
        for (const auto &track : stickTracks) {
            for (const auto &member : track.playlists) {
                playlistCounts[member.name]++;
            }
        }
        for (const auto &[name, count] : playlistCounts) {
            result.playlistNames << QString::fromStdString(name);
            result.playlistTrackCounts.insert(QString::fromStdString(name), count);
        }

        reporter->start("Reading the metadata store", 0);
        MetadataStore store;
        const auto storedTracks = store.readAll();
        // For display only, and fetched separately rather than carried
        // on domain::Track so nothing in the matching or merging can
        // reach for it.
        const auto stickLabels = store.stickLabelsByTrackId();
        reporter->finish();
        result.storedTrackCount = static_cast<int>(storedTracks.size());

        if (cancel.cancelled()) {
            result.cancelled = true;
            return result;
        }

        // When this stick's catalogs were last written. The merge rule
        // consults it only where nothing else separates two copies, but
        // where it does the answer turns on it, so it is read from the
        // files rather than assumed.
        result.plan =
            domain::planMetadataBackup(stickTracks, storedTracks, catalogsLastModified(libraryPath.toStdString()));

        for (auto &proposal : result.plan.proposals) {
            if (proposal.storedId.empty()) {
                continue;
            }
            // storedId is the row id as text, which is how the store
            // spells it on a domain::Track; stoll is safe on anything
            // readAll() produced and guarded for anything that was not.
            try {
                const auto found = stickLabels.find(std::stoll(proposal.storedId));
                if (found != stickLabels.end()) {
                    proposal.storedFrom = found->second;
                }
            } catch (const std::exception &) {
                // No label, so the row simply does not say where its
                // stored copy came from. Not worth failing a scan over.
            }
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
}

// Writes the chosen tracks into the store. Runs entirely on a background
// thread -- no access to the controller.
//
// Takes the tracks rather than re-reading the stick: they were read once
// by the scan the user has been looking at, and reading again would let
// the list they ticked and the write they authorised disagree about a
// stick edited in between.
MetadataBackupTaskResult runStoreTask(std::vector<domain::Track> tracks, QString libraryPath, QString libraryId,
                                       QString stickLabel, std::shared_ptr<QtProgressReporter> reporter,
                                       application::CancellationToken cancel)
{
    MetadataBackupTaskResult result;
    try {
        MetadataSource source;
        source.stickRoot = fs::path(libraryPath.toStdString()).parent_path();
        source.libraryId = libraryId.toStdString();
        source.stickLabel = stickLabel.toStdString();
        // Read from the files, not from the rows: the merge rule's last
        // step needs to know whether this stick has been worked on since
        // an entry was stored, and a catalog's mtime is the only date a
        // stick offers.
        source.catalogModifiedAt = catalogsLastModified(libraryPath.toStdString());

        MetadataStore store;
        result.summary = store.store(tracks, source, *reporter, cancel);
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
        return metadataDurationText(row.durationSeconds);
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
    // The marks stay. They are keyed by store row id, so they cannot
    // land on the wrong track however the list is rebuilt underneath
    // them.
    //
    // They used to be dropped here, on the argument that a mark you
    // cannot see is a mark you might delete by accident -- true, but the
    // cure was worse: reset() is what a changed search term runs
    // through, so typing in the search box silently cleared every tick
    // while the page went on reporting them as staged, because nothing
    // told the controller. One search, and the count and the tick boxes
    // disagreed until you left the page.
    //
    // The floating Save button answers the original worry properly: it
    // carries a live count of everything staged and lists it, so a row
    // filtered or paged out of sight is still visible in the one place
    // that decides. clearStagingFor() handles rows that have genuinely
    // stopped existing.
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


void StoredTrackListModel::clearStagingFor(const QList<qint64> &trackIds)
{
    for (const qint64 id : trackIds) {
        m_stagedForDeletion.remove(id);
    }
}

QStringList StoredTrackListModel::stagedDescriptions() const
{
    QStringList lines;
    QSet<qint64> described;
    for (const auto &row : m_rows) {
        const qint64 id = static_cast<qint64>(row.id);
        if (!m_stagedForDeletion.contains(id)) {
            continue;
        }
        described.insert(id);
        QString name = QString::fromStdString(row.title.empty() ? row.filename : row.title);
        if (!row.artist.empty()) {
            name = QString::fromStdString(row.artist) + QStringLiteral(" - ") + name;
        }
        lines << QStringLiteral("Forget %1").arg(name);
    }
    // A row can be staged and then filtered or paged out of the loaded
    // list. It still gets deleted, so it still has to be accounted for
    // here -- named where the list can name it, counted where it cannot.
    const int unnamed = m_stagedForDeletion.size() - described.size();
    if (unnamed > 0) {
        lines << QStringLiteral("Forget %1 more not currently listed").arg(unnamed);
    }
    return lines;
}

// ---- controller -----------------------------------------------------

MetadataBackupController::MetadataBackupController(QObject *parent) : QObject(parent)
{
    connect(&m_scanWatcher, &QFutureWatcher<MetadataBackupScanResult>::finished, this,
            &MetadataBackupController::onScanFinished);
    connect(&m_saveWatcher, &QFutureWatcher<MetadataBackupTaskResult>::finished, this,
            &MetadataBackupController::onSaveFinished);
    refresh();
}

MetadataBackupController::~MetadataBackupController()
{
    m_cancel.cancel();
    if (m_scanWatcher.isRunning()) {
        m_scanWatcher.waitForFinished();
    }
    if (m_saveWatcher.isRunning()) {
        m_saveWatcher.waitForFinished();
    }
}

QString MetadataBackupController::storeLocation() const
{
    return QString::fromStdString(MetadataStore::defaultDatabasePath().parent_path().string());
}

QString MetadataBackupController::state() const
{
    // The one string SaveOverlayButton compares against. It disables
    // itself while a save is in flight and keeps itself visible, which
    // is exactly the behaviour wanted here.
    return m_writing ? QStringLiteral("writing") : QStringLiteral("idle");
}

QStringList MetadataBackupController::pendingDescriptions() const
{
    QStringList lines = m_proposalModel.stagedDescriptions();
    lines << m_browseModel.stagedDescriptions();
    return lines;
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
        // The marks survive a reset now, so the count that depends on
        // them has to be re-announced with the list. Leaving this out is
        // what made a search clear every tick box on screen while the
        // page went on insisting three tracks were staged.
        emit selectionChanged();
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromUtf8(e.what()));
    }
}

// ---- choosing what the list shows ------------------------------------

bool MetadataBackupController::selectStick(const QString &libraryPath, const QString &libraryId,
                                            const QString &stickLabel)
{
    // Same stick, already scanned: nothing to do, and re-scanning would
    // throw away staging for no reason.
    if (!m_browsingStore && m_sourceLibraryPath == libraryPath && m_hasScanned) {
        return true;
    }
    if (dirty()) {
        // The page puts the question; this only refuses. Staging was
        // decided against numbers a new scan replaces, so carrying it
        // over would stage a choice made about a different stick.
        return false;
    }
    startScan(libraryPath, libraryId, stickLabel);
    return true;
}

void MetadataBackupController::discardStagingAndSelectStick(const QString &libraryPath, const QString &libraryId,
                                                             const QString &stickLabel)
{
    clearAllStaging();
    startScan(libraryPath, libraryId, stickLabel);
}

void MetadataBackupController::startScan(const QString &libraryPath, const QString &libraryId,
                                          const QString &stickLabel)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    m_sourceLibraryPath = libraryPath;
    m_sourceLibraryId = libraryId;
    m_sourceStickLabel = stickLabel;
    m_browsingStore = false;
    m_hasScanned = false;
    m_scanCancelled = false;
    m_catalogsUnreadable.clear();
    // The playlist filter belonged to the stick being left. Keeping it
    // would silently narrow the new stick's list by a playlist that may
    // not even exist on it.
    m_playlist.clear();
    m_playlistNames.clear();
    m_playlistTrackCounts.clear();
    m_proposalModel.clear();
    emit sourceChanged();
    emit filterChanged();
    emit analysisChanged();
    emit selectionChanged();

    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
    setProgress(0, 0);
    setCurrentPhase(QStringLiteral("Reading this stick"));
    m_cancel = application::CancellationToken();
    setBusy(true);
    m_scanWatcher.setFuture(QtConcurrent::run(runScanTask, libraryPath, makeReporter(), m_cancel));
}

bool MetadataBackupController::browseStore()
{
    if (m_browsingStore) {
        return true;
    }
    if (dirty()) {
        return false;
    }
    discardStagingAndBrowseStore();
    return true;
}

void MetadataBackupController::discardStagingAndBrowseStore()
{
    clearAllStaging();
    m_browsingStore = true;
    m_hasScanned = false;
    m_scanCancelled = false;
    m_catalogsUnreadable.clear();
    m_sourceLibraryPath.clear();
    m_sourceLibraryId.clear();
    m_sourceStickLabel.clear();
    m_playlist.clear();
    m_playlistNames.clear();
    m_playlistTrackCounts.clear();
    m_proposalModel.clear();
    emit sourceChanged();
    emit filterChanged();
    emit analysisChanged();
    refresh();
}

void MetadataBackupController::cancel()
{
    m_cancel.cancel();
}

void MetadataBackupController::onScanFinished()
{
    const MetadataBackupScanResult result = m_scanWatcher.result();
    setBusy(false);
    setCurrentPhase({});
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        m_scanCancelled = true;  // there is no plan, and the page must not pretend one is coming
        emit analysisChanged();
        return;
    }
    if (result.cancelled) {
        // Cancelling left no plan. Saying so is the whole fix: without
        // it hasScanned stayed false with the stick still selected, so
        // the page sat on "Reading X..." with an empty list and no busy
        // indicator, looking hung, and the empty-state label stayed
        // suppressed because it waits for hasScanned.
        m_scanCancelled = true;
        emit analysisChanged();
        return;
    }

    // Catalogs that are on the stick but could not be read. Not an error
    // -- what was read is still worth storing -- but the tracks only the
    // unreadable one held are silently missing from this plan, and a
    // user who believes their Engine cues were just backed up when
    // Engine was never opened has been misled.
    m_catalogsUnreadable = result.catalogsUnreadable;
    m_scanCancelled = false;
    m_storedTrackCount = result.storedTrackCount;
    m_tracksSeen = result.plan.tracksSeen;
    m_alreadyCurrent = result.plan.alreadyCurrent;
    m_withoutIdentity = result.plan.withoutIdentity;
    m_playlistNames = result.playlistNames;
    m_playlistTrackCounts = result.playlistTrackCounts;
    m_proposalModel.setProposals(result.plan.proposals);
    m_proposalModel.setFilter(m_search, m_playlist);
    m_hasScanned = true;
    emit analysisChanged();
    emit selectionChanged();
    // storedTrackCount's NOTIFY is storeChanged, not analysisChanged.
    // The browse summary and the stored list's empty state both read it,
    // and without this they kept the previous number until something
    // else happened to emit it.
    emit storeChanged();
}

// ---- staging ---------------------------------------------------------

void MetadataBackupController::toggleStagedForAdd(int row)
{
    const int index = m_proposalModel.sourceIndexOfRow(row);
    if (index < 0) {
        return;
    }
    m_proposalModel.setStaged(index, !m_proposalModel.isStaged(index));
    emit selectionChanged();
}

void MetadataBackupController::stageAllForAdd()
{
    m_proposalModel.stageAll();
    emit selectionChanged();
}

void MetadataBackupController::unstageAllForAdd()
{
    m_proposalModel.unstageAll();
    emit selectionChanged();
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

void MetadataBackupController::clearAllStaging()
{
    m_browseModel.clearDeletionStaging();
    m_proposalModel.unstageAll();
    emit selectionChanged();
}

// ---- committing ------------------------------------------------------

void MetadataBackupController::save()
{
    if (m_busy || !dirty()) {
        return;
    }
    if (stagedForDeletionCount() > 0) {
        // Adding to a backup is safe and reversible; taking something
        // out of one is neither, and what goes may be the only copy
        // left. The page asks, then calls saveConfirmed().
        emit deletionConfirmationRequired();
        return;
    }
    beginSave();
}

void MetadataBackupController::saveConfirmed()
{
    if (m_busy || !dirty()) {
        return;
    }
    beginSave();
}

void MetadataBackupController::beginSave()
{
    setErrorMessage({});
    m_hasResult = false;
    m_lastRun.clear();
    emit resultChanged();

    const std::vector<domain::Track> tracks = m_proposalModel.stagedTracks();
    if (tracks.empty()) {
        // Deletions only. No stick to read and nothing to write, so this
        // is a database delete and does not need a thread or a bar.
        if (applyStagedDeletions()) {
            emit saveCompleted();
        }
        return;
    }

    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
    setProgress(0, 0);
    setCurrentPhase(QStringLiteral("Storing"));
    m_cancel = application::CancellationToken();
    setBusy(true);
    setWriting(true);
    m_saveWatcher.setFuture(QtConcurrent::run(runStoreTask, tracks, m_sourceLibraryPath, m_sourceLibraryId,
                                               m_sourceStickLabel, makeReporter(), m_cancel));
}

void MetadataBackupController::onSaveFinished()
{
    const MetadataBackupTaskResult result = m_saveWatcher.result();
    setBusy(false);
    setWriting(false);
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
        // No catalog lists here. A save reads no catalogs -- the scan
        // did that, and the tracks came with it -- so reporting the
        // empty ones would be answering a question this run never
        // asked. The scan's own lists live on catalogsUnreadable.
    };
    m_hasResult = true;

    if (summary.cancelled) {
        // Stopped part way. What is still staged is the best description
        // of what is left to do, so it stays -- and the deletions do NOT
        // happen: the user pressed Cancel, and taking that as consent to
        // permanently forget the rows this page warns may be the last
        // copy of anything would be the worst thing on the page.
        emit resultChanged();
        return;
    }

    // Staged additions have landed, so they are no longer staged.
    m_proposalModel.unstageAll();
    // Deletions ride along in the same Save, after the write, so a run
    // that fails to store does not also forget things.
    const bool deletionsDone = applyStagedDeletions();
    emit resultChanged();

    // And the plan is now stale by exactly the amount that was just
    // written. Leaving it up meant a list still headed "1469 tracks to
    // back up", with every row still badged "new", describing work that
    // had already happened -- the page contradicting the summary dialog
    // in front of it. Cheap to redo: the catalogs this re-reads are the
    // ones LibraryCatalogCache is still holding.
    //
    // A cancelled run never reaches here: it returned above with its
    // staging intact, because that staging is still the best
    // description of what is left to do.
    if (!m_browsingStore && !m_sourceLibraryPath.isEmpty()) {
        // Kept across the rescan. startScan() drops the playlist filter
        // because it is normally switching to a different stick, whose
        // playlists are not this one's -- but this is the same stick,
        // and someone working through a library playlist by playlist
        // should not be thrown back to "All tracks" by each save they
        // make. Restored after the call, which onScanFinished() then
        // applies when the new proposals arrive.
        const QString playlist = m_playlist;
        startScan(m_sourceLibraryPath, m_sourceLibraryId, m_sourceStickLabel);
        m_playlist = playlist;
        emit filterChanged();
    }

    // Last, and after the rescan rather than before it. A page waiting
    // to leave pops itself in this handler, which destroys this object;
    // starting a fresh scan afterwards would launch a thread on a
    // controller already scheduled for deletion and then block the UI
    // thread in ~MetadataBackupController waiting for it to notice the
    // cancel.
    //
    // Only when the whole save did what it said. A delete half that
    // failed left its marks in place on purpose, and calling that
    // "completed" would carry the user off the page, away from both the
    // message and the staging that is still sitting there.
    if (deletionsDone) {
        emit saveCompleted();
    }
}

bool MetadataBackupController::applyStagedDeletions()
{
    const QList<qint64> ids = m_browseModel.stagedForDeletionIds();
    if (ids.isEmpty()) {
        refresh();
        emit selectionChanged();
        return true;
    }
    auto *db = store();
    if (!db) {
        // store() has already set the error message. The marks stay --
        // nothing was deleted, so nothing should look as though it was
        // -- and the page is told, because the caller may have just
        // reported a successful run whose delete half never ran.
        emit selectionChanged();
        emit actionFeedback(QStringLiteral("The metadata backup could not be opened; nothing was deleted."),
                            true);
        return false;
    }
    int removed = 0;
    try {
        removed = db->removeTracks(std::vector<std::int64_t>(ids.begin(), ids.end()));
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromUtf8(e.what()));
        emit selectionChanged();
        emit actionFeedback(QStringLiteral("Nothing was deleted: %1").arg(QString::fromUtf8(e.what())), true);
        return false;
    }
    // The rows they referred to are gone, so the marks go with them.
    // Explicitly, because a reset no longer does it.
    m_browseModel.clearStagingFor(ids);
    refresh();
    emit selectionChanged();
    emit actionFeedback(removed == 1 ? QStringLiteral("One track removed from the metadata backup.")
                                     : QStringLiteral("%1 tracks removed from the metadata backup.").arg(removed),
                        false);
    return true;
}

// ---- filtering -------------------------------------------------------

void MetadataBackupController::search(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_search == trimmed) {
        return;
    }
    m_search = trimmed;
    // Both populations, because either may be the one showing and the
    // field is the same field. The proposal list filters in memory; the
    // browse list re-queries, which is what refresh() does.
    m_proposalModel.setFilter(m_search, m_playlist);
    emit analysisChanged();
    emit filterChanged();
    refresh();
}

void MetadataBackupController::setPlaylist(const QString &name)
{
    if (m_playlist == name) {
        return;
    }
    m_playlist = name;
    m_proposalModel.setFilter(m_search, m_playlist);
    emit analysisChanged();
    emit filterChanged();
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
        entry["positionText"] = metadataDurationText(cue.positionMs / 1000.0);
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

QString MetadataBackupController::mergeRuleHelp() const
{
    return QString::fromStdString(domain::mergeRuleExplanation());
}

// ---- progress and state ----------------------------------------------

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

void MetadataBackupController::setWriting(bool writing)
{
    if (m_writing == writing) {
        return;
    }
    m_writing = writing;
    // state() is what SaveOverlayButton watches, and it hangs off
    // busyChanged -- the same signal, so the button and the progress bar
    // cannot disagree about whether a write is in flight.
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
