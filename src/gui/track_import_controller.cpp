// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_controller.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "domain/track_import_plan.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/changes/restore_analysis_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/analysis_settings.hpp"
#include "gui/async_request.hpp"
#include "gui/future_result.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/track_import_analysis.hpp"
#include "gui/track_import_cover.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

namespace seabass::gui
{

namespace
{

// How long going away waits for a worker told to stop before letting it
// go, as a page's read waits (AsyncRequest).
constexpr std::chrono::milliseconds LetGoAfter{1000};

// A cue's move as the review shows it, in whole ms with its sign: the move
// as application::cueMoveMs() counts it (the larger of the move and the
// move as stored), cut to whole ms the way the track's largest move is,
// so a row never reads more than its track and never less than the files
// will hold.
int shownMoveMs(double oldMs, double newMs)
{
    const int amount = static_cast<int>(std::floor(application::cueMoveMs(oldMs, newMs)));
    return newMs < oldMs ? -amount : amount;
}

// Where the rows' covers are kept: beside the cover cache's file
// (CoverCache::defaultDatabaseFile()), in the platform's cache folder, or
// under SEABASS_HOME when that is set.
QString importCoverRoot()
{
    if (qEnvironmentVariableIsSet("SEABASS_HOME")) {
        return pathToQString(infrastructure::paths::localRoot() / "cache" / "import-covers");
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/seabass/import-covers");
}

// stagedImport's revision: never the same twice in a run, across
// controllers too, so a page handed another controller is never told
// that nothing changed.
int nextStagedImportRevision()
{
    static int revision = 0;
    return ++revision;
}

}  // namespace

TrackImportController::TrackImportController(QObject *parent)
    : QObject(parent)
    , m_coverFolder(importCoverRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    connect(&m_watcher, &QFutureWatcher<PreparedImport>::finished, this, &TrackImportController::onPrepared);
    connect(&m_reanalysisWatcher, &QFutureWatcher<PreparedReanalysis>::finished, this,
            &TrackImportController::onReanalysed);
}

TrackImportController::~TrackImportController()
{
    // The worker is told to stop and waited for a moment, as a page's
    // read is (docs/async-requests.md, rule 7): it stops at its next look
    // at the token, which an analysis takes every piece it reads. One
    // that does not by then (a decoder waiting for a file that stopped
    // answering, a file read from a stick being pulled) is let go rather
    // than freezing the window. It can finish alone: it writes nothing
    // (an offer is read, planned and analysed; only the save writes),
    // and it holds its own copies of the request and the token and
    // reaches this object only through the progress sink's QPointer. Its
    // answer goes nowhere. endProcess() sees to it at the end of the
    // process.
    m_cancel.cancel();
    const auto deadline = std::chrono::steady_clock::now()
                          + (AsyncWorkers::instance().shuttingDown() ? std::chrono::milliseconds(0) : LetGoAfter);
    const auto running = [this]() {
        return (m_watcher.future().isValid() && !m_watcher.future().isFinished())
               || (m_reanalysisWatcher.future().isValid() && !m_reanalysisWatcher.future().isFinished());
    };
    while (running() && std::chrono::steady_clock::now() < deadline) {
        QThread::msleep(5);
    }
    disconnect(&m_watcher, nullptr, this, nullptr);
    disconnect(&m_reanalysisWatcher, nullptr, this, nullptr);
    // The rows' covers were for this controller's list alone.
    if (!m_coverFiles.empty()) {
        QDir(m_coverFolder).removeRecursively();
    }
}

void TrackImportController::setExperimentalEnabled(bool enabled)
{
    if (m_experimentalEnabled != enabled) {
        m_experimentalEnabled = enabled;
        emit experimentalEnabledChanged();
    }
}

bool TrackImportController::allowedToStage()
{
    if (!m_experimentalEnabled) {
        setErrorMessage(tr("Importing tracks and Analyse again are experimental features, switched off in "
                                       "Preferences."));
        return false;
    }
    return true;
}

ImportAnalyser TrackImportController::analyser() const
{
    if (m_analyserForTesting) {
        return m_analyserForTesting;
    }
    // The tempo range as Preferences has it now: it may have changed
    // since the last run.
    const QSettings settings = openSeabassSettings();
    const domain::TempoRange range = storedAnalysisTempoRange(settings);
    return [range](const std::string &file, const application::CancellationToken &cancel) {
        return analyseForImport(file, cancel, range);
    };
}

void TrackImportController::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void TrackImportController::setProgress(const QString &step, int done, int total, const QString &file,
                                        const QStringList &inHand, double fraction, bool planned)
{
    if (m_progressStep != step || m_progressDone != done || m_progressTotal != total || m_progressFile != file
        || m_progressFiles != inHand || m_progressFraction != fraction || m_progressPlanned != planned) {
        m_progressFiles = inHand;
        m_progressFraction = fraction;
        m_progressPlanned = planned;
        m_progressStep = step;
        m_progressDone = done;
        m_progressTotal = total;
        m_progressFile = file;
        emit progressChanged();
    }
}

void TrackImportController::cancel()
{
    if (m_busy) {
        m_cancel.cancel();
    }
}

void TrackImportController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message || (!message.isEmpty() && m_shownErrorSource != m_errorSource)) {
        m_errorMessage = message;
        m_shownErrorSource = m_errorSource;
        emit errorMessageChanged();
    }
}

void TrackImportController::refreshStagedImport()
{
    // The rows as the save will write them: the batch's changes in the
    // session's order, which is the save's.
    QVariantList rows;
    int atIndex = -1;
    if (m_session && !m_staged.empty()) {
        std::map<std::string, std::shared_ptr<ImportTrackChange>> imports;
        std::vector<std::shared_ptr<ImportTrackChange>> inOrder;
        bool entries = false;
        for (const auto &change : m_session->pendingChanges()) {
            if (m_staged.count(change->id()) == 0) {
                continue;
            }
            if (auto import = std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                imports[import->destinationFile()] = import;
                inOrder.push_back(import);
            }
        }
        const auto rowOf = [this](const ImportTrackChange &import) {
            const domain::ImportedTrack &track = import.track();
            const auto cover = m_covers.find(import.id());
            return QVariantMap{
                {QStringLiteral("changeId"), import.id()},
                {QStringLiteral("existing"), false},
                {QStringLiteral("filePath"), QString::fromStdString(import.destinationFile())},
                // The file on the computer it is copied from: what waits
                // is not offered again (Browse's "Add to the library").
                {QStringLiteral("sourceFile"), QString::fromStdString(import.sourceFile())},
                {QStringLiteral("title"), QString::fromStdString(track.title)},
                {QStringLiteral("artist"), QString::fromStdString(track.artist)},
                {QStringLiteral("album"), QString::fromStdString(track.album)},
                {QStringLiteral("durationSeconds"), track.durationSeconds},
                {QStringLiteral("bpm"), track.bpm},
                {QStringLiteral("key"), QString::fromStdString(track.key)},
                {QStringLiteral("bitrate"), track.bitrate},
                {QStringLiteral("artworkPath"), cover == m_covers.end() ? QString() : cover->second},
            };
        };
        for (const auto &change : m_session->pendingChanges()) {
            if (m_staged.count(change->id()) == 0) {
                continue;
            }
            if (const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(change)) {
                entries = true;
                const auto import = imports.find(entry->filePath());
                if (import != imports.end()) {
                    rows << rowOf(*import->second);
                } else {
                    rows << QVariantMap{{QStringLiteral("changeId"), entry->id()},
                                        {QStringLiteral("existing"), true},
                                        {QStringLiteral("filePath"), QString::fromStdString(entry->filePath())},
                                        {QStringLiteral("title"), entry->subject()}};
                }
            } else if (const auto place = std::dynamic_pointer_cast<PlaceInPlaylistChange>(change)) {
                atIndex = place->toIndex();
            }
        }
        // Into no playlist: the new tracks alone, in the save's order.
        if (!entries) {
            for (const auto &import : inOrder) {
                rows << rowOf(*import);
            }
        }
    }
    m_stagedImport = QVariantMap{
        {QStringLiteral("revision"), nextStagedImportRevision()},
        {QStringLiteral("playlist"), stagedPlaylist()},
        {QStringLiteral("isNew"), stagedPlaylistIsNew()},
        {QStringLiteral("atIndex"), atIndex},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("landed"), m_landed},
    };
    pruneCovers();
    emit stagedImportChanged();
}

QString TrackImportController::keepCover(const std::string &jpeg)
{
    const QByteArray bytes = QByteArray::fromStdString(jpeg);
    const QString file = m_coverFolder + QLatin1Char('/')
                         + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex())
                         + QStringLiteral(".jpg");
    if (m_coverFiles.count(file) > 0) {
        return file;
    }
    QDir().mkpath(m_coverFolder);
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush()) {
        out.close();
        QFile::remove(file);
        // The row shows the track without a cover; the save reads it from
        // the file all the same.
        return {};
    }
    m_coverFiles.insert(file);
    return file;
}

void TrackImportController::pruneCovers()
{
    // A cover is kept while a row shows it: a staged track's, or one the
    // last save wrote until the stick has been read again.
    std::set<QString> shown;
    for (const QVariant &row : m_landed) {
        shown.insert(row.toMap().value(QStringLiteral("changeId")).toString());
    }
    std::erase_if(m_covers, [&](const auto &entry) {
        return m_staged.count(entry.first) == 0 && shown.count(entry.first) == 0;
    });
    std::set<QString> used;
    for (const auto &[id, file] : m_covers) {
        used.insert(file);
    }
    std::erase_if(m_coverFiles, [&](const QString &file) {
        if (used.count(file) > 0) {
            return false;
        }
        QFile::remove(file);
        return true;
    });
}

void TrackImportController::releaseLanded()
{
    if (m_landed.isEmpty()) {
        return;
    }
    m_landed.clear();
    refreshStagedImport();
}

void TrackImportController::pruneCueReview()
{
    const std::size_t before = m_reviewed.size();
    std::erase_if(m_reviewed, [this](const auto &change) { return m_reanalysed.count(change->id()) == 0; });
    if (m_reviewed.size() != before) {
        emit cueReviewChanged();
    }
}

void TrackImportController::forgetReanalysis()
{
    if (!m_reviewed.empty()) {
        m_reviewed.clear();
        emit cueReviewChanged();
    }
    if (m_reanalysed.empty()) {
        return;
    }
    m_reanalysed.clear();
    ++m_pendingRevision;
    emit pendingChanged();
}

void TrackImportController::forgetStaged()
{
    if (m_staged.empty() && m_stagedTracks == 0) {
        return;
    }
    m_staged.clear();
    m_stagedImports.clear();
    m_stagedTracks = 0;
    m_stagedExisting = 0;
    ++m_pendingRevision;
    refreshStagedImport();
    emit pendingChanged();
}

bool TrackImportController::attachSession(const QString &rekordboxPath, const QString &enginePath)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session =
        registry->sessionFor(registry->libraryIdForPath(rekordboxPath.isEmpty() ? enginePath : rekordboxPath));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        const bool landed = !m_landed.isEmpty();
        m_landed.clear();
        forgetStaged();
        forgetReanalysis();
        if (landed) {
            refreshStagedImport();
        }
        m_session = session;
        emit saveWritingChanged();
        if (m_session) {
            connect(m_session, &LibraryEditSession::stateChanged, this, &TrackImportController::saveWritingChanged);
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (m_reanalysed.erase(changeId) > 0) {
                    ++m_pendingRevision;
                    emit pendingChanged();
                    pruneCueReview();
                }
                if (m_staged.count(changeId) > 0) {
                    // Its row is written: shown as it was until the page
                    // has read the stick again.
                    for (const QVariant &row : m_stagedImport.value(QStringLiteral("rows")).toList()) {
                        if (row.toMap().value(QStringLiteral("changeId")).toString() == changeId) {
                            m_landed << row;
                        }
                    }
                }
                if (m_staged.erase(changeId) > 0) {
                    // Every change of the batch is something the page
                    // has to read again.
                    ++m_appliedTracks;
                    if (m_stagedImports.erase(changeId) > 0) {
                        --m_stagedTracks;
                    }
                    if (m_staged.empty()) {
                        m_stagedExisting = 0;
                    }
                    // The rows are read again at the end of the save
                    // (saveFinished): every change it applied has its
                    // row in `landed` by then.
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &summary) {
                if (m_appliedTracks > 0) {
                    refreshStagedImport();
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
                const bool wrote = m_appliedTracks > 0 && summary.value("written").toInt() > 0;
                m_appliedTracks = 0;
                if (wrote) {
                    emit tracksImported();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                const bool landed = !m_landed.isEmpty();
                m_landed.clear();
                forgetStaged();
                forgetReanalysis();
                if (landed) {
                    refreshStagedImport();
                    emit pendingChanged();
                }
            });
            // A change of the batch taken back elsewhere (the session's
            // own list): what is left of it is no longer what was
            // rehearsed, so the count follows the session.
            connect(m_session, &LibraryEditSession::pendingChanged, this, [this]() {
                if (!m_session || m_session->writing()) {
                    return;
                }
                // A new analysis taken back elsewhere is no longer waiting.
                const std::size_t reanalysed = m_reanalysed.size();
                std::erase_if(m_reanalysed, [this](const auto &entry) { return !m_session->hasChange(entry.first); });
                if (m_reanalysed.size() != reanalysed) {
                    ++m_pendingRevision;
                    emit pendingChanged();
                    pruneCueReview();
                }
                if (m_staged.empty()) {
                    return;
                }
                int tracks = 0;
                std::set<QString> still;
                for (const QString &id : m_staged) {
                    if (m_session->hasChange(id)) {
                        still.insert(id);
                        tracks += m_stagedImports.count(id) > 0 ? 1 : 0;
                    }
                }
                if (still.size() != m_staged.size()) {
                    int existing = 0;
                    for (const QString &id : still) {
                        existing += m_stagedExistingIds.count(id) > 0 ? 1 : 0;
                    }
                    m_stagedExisting = existing;
                    m_staged = std::move(still);
                    m_stagedTracks = tracks;
                    ++m_pendingRevision;
                    refreshStagedImport();
                    emit pendingChanged();
                }
            });
        }
    }
    if (!m_session) {
        setErrorMessage(tr("This stick has no edit session open."));
        return false;
    }
    m_session->setLibraryPaths(rekordboxPath, enginePath);
    if (m_session->writing()) {
        setErrorMessage(tr("A save is writing to the stick. Try again when it has finished."));
        return false;
    }
    return true;
}

bool TrackImportController::importFiles(const QString &rekordboxPath, const QString &enginePath,
                                        const QVariantList &files, const QString &playlist, bool create, int atIndex)
{
    ImportRequest request;
    if (!requestFor(files, request)) {
        return false;
    }
    request.rekordboxPath = rekordboxPath;
    request.enginePath = enginePath;
    if (!playlist.isEmpty()) {
        ImportPlaylist into;
        into.path = playlist.toStdString();
        into.create = create;
        if (atIndex >= 0) {
            into.atIndex = atIndex;
        }
        request.playlist = into;
    }
    return start(std::move(request));
}

bool TrackImportController::importMakingPlaylistWhereMissing(const QString &rekordboxPath, const QString &enginePath,
                                                             const QVariantList &files, const QString &playlist,
                                                             int atIndex)
{
    ImportRequest request;
    if (!requestFor(files, request)) {
        return false;
    }
    if (playlist.isEmpty() || playlist.contains(QLatin1Char('/'))) {
        setErrorMessage(tr("The playlist is made at the top level; \"%1\" is no name for it.").arg(playlist));
        return false;
    }
    request.rekordboxPath = rekordboxPath;
    request.enginePath = enginePath;
    ImportPlaylist into;
    into.path = playlist.toStdString();
    into.createWhereMissing = true;
    if (atIndex >= 0) {
        into.atIndex = atIndex;
    }
    request.playlist = into;
    return start(std::move(request), false, true);
}

bool TrackImportController::requestFor(const QVariantList &files, ImportRequest &request)
{
    setErrorSource(QStringLiteral("import"));
    setErrorMessage({});
    if (!allowedToStage()) {
        return false;
    }
    for (const QVariant &file : files) {
        const QUrl url = file.toUrl();
        const QString path = url.isLocalFile() ? url.toLocalFile() : (url.scheme().isEmpty() ? file.toString() : QString());
        if (!path.isEmpty()) {
            request.offered.push_back(QFileInfo(path).absoluteFilePath().toStdString());
        }
    }
    if (request.offered.empty()) {
        setErrorMessage(tr("Nothing that was dropped is a file on this computer."));
        return false;
    }
    return true;
}

bool TrackImportController::importPossibleDuplicates()
{
    setErrorSource(QStringLiteral("import"));
    setErrorMessage({});
    if (m_possibleDuplicates.empty() || m_request.offered.empty() || !allowedToStage()) {
        return false;
    }
    ImportRequest again = m_request;
    again.importAnyway.insert(m_possibleDuplicates.begin(), m_possibleDuplicates.end());
    // The batch staged from the offer waits until the new one is ready,
    // as a batch that is joined does (onPrepared()): a start refused or
    // cancelled leaves it as it was.
    return start(std::move(again), true);
}

bool TrackImportController::start(ImportRequest request, bool replacesStaged, bool joinsFirst)
{
    if (m_busy) {
        setErrorMessage(!m_reanalysingPlaylist.isEmpty()
                            ? tr("Tracks are being analysed again. Import when that has finished.")
                            : tr("An import is being prepared already."));
        return false;
    }
    if (!attachSession(request.rekordboxPath, request.enginePath)) {
        return false;
    }
    bool joins = false;
    if (m_session->pendingCount() > 0) {
        // More for the playlist of the batch that waits: the two offers
        // become one, prepared again from the start, so that what is
        // staged is still one batch rehearsed against the stick as it
        // is. The batch that waits stays until the joined one is ready
        // (onPrepared), so an offer that is cancelled or refused costs
        // nothing.
        const auto into = [](const ImportRequest &r) { return r.playlist ? r.playlist->path : std::string(); };
        const bool oursOnly = !m_staged.empty() && m_session->pendingCount() == static_cast<int>(m_staged.size());
        if (oursOnly && request.rekordboxPath == m_request.rekordboxPath && request.enginePath == m_request.enginePath
            && into(request) == into(m_request)) {
            // The staged batch's own offer again needs nothing joined.
            if (!replacesStaged) {
                ImportRequest joined = m_request;
                for (const std::string &offered : request.offered) {
                    // Offered again by the person: no longer left out.
                    joined.leftOutByPerson.erase(offered);
                }
                if (joinsFirst) {
                    // The new files first, the waiting ones after them, the
                    // run at the place the new offer names: the files added
                    // last come first.
                    std::vector<std::string> order = request.offered;
                    for (const std::string &waiting : m_request.offered) {
                        if (std::find(order.begin(), order.end(), waiting) == order.end()) {
                            order.push_back(waiting);
                        }
                    }
                    joined.offered = std::move(order);
                    if (request.playlist && request.playlist->atIndex && joined.playlist) {
                        joined.playlist->atIndex = request.playlist->atIndex;
                    }
                } else {
                    for (const std::string &offered : request.offered) {
                        if (std::find(joined.offered.begin(), joined.offered.end(), offered) == joined.offered.end()) {
                            joined.offered.push_back(offered);
                        }
                    }
                }
                request = std::move(joined);
            }
            joins = true;
        } else {
            setErrorMessage(!m_reanalysed.empty()
                                ? tr("Tracks analysed again are waiting to be saved: an import is checked "
                                                 "against the stick as it is. Save or take them back first.")
                            : oursOnly
                                ? tr("An import into another playlist is waiting to be saved. Save it, or take "
                                                 "it back, before the next.")
                                : tr("Save or discard the edits that are waiting first: an import is checked "
                                                 "against the stick as it is now."));
            return false;
        }
    }
    request.coverReader = readCoverPictures;
    request.analysisJobs = importAnalysisJobs();
    request.analyser = analyser();
    request.date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")).toStdString();
    m_joining = joins;
    m_joined = joins ? m_staged : std::set<QString>();
    m_preparing = request;
    m_cancel = application::CancellationToken();
    setBusy(true);
    setProgress(QStringLiteral("stick"), 0, 0, {}, {}, 0.0, false);
    const ImportProgressSink progress = progressSink();
    // A read, on a thread of its own (runRead(), not the pool the writes
    // use): one let go at the end (see the destructor) must not hold up
    // the end of the process.
    m_watcher.setFuture(
        runRead([request, cancel = m_cancel, progress]() { return prepareTrackImport(request, cancel, progress); }));
    return true;
}

ImportProgressSink TrackImportController::progressSink()
{
    // Passed on to this thread; one of a run that was cancelled or has
    // finished meanwhile is not shown.
    const QPointer<TrackImportController> self(this);
    const int offer = ++m_offer;
    return [self, offer](const ImportProgress &at) {
        static const char *const steps[] = {"stick", "files", "check", "analysis"};
        const QString step = QLatin1String(steps[static_cast<int>(at.step)]);
        const QString file = QString::fromStdString(at.file);
        const int done = static_cast<int>(at.done);
        const int total = static_cast<int>(at.total);
        const double fraction =
            at.barTotal > 0 ? static_cast<double>(at.barDone) / static_cast<double>(at.barTotal) : 0.0;
        const bool planned = at.barTotal > 0;
        QStringList inHand;
        for (const std::string &name : at.inHand) {
            inHand.push_back(QString::fromStdString(name));
        }
        QMetaObject::invokeMethod(
            qApp,
            [self, offer, step, done, total, file, inHand, fraction, planned]() {
                if (self && self->m_busy && self->m_offer == offer && !self->m_cancel.cancelled()) {
                    // The one bar only goes forward, and once it has a
                    // plan it keeps it.
                    self->setProgress(step, done, total, file, inHand, std::max(self->m_progressFraction, fraction),
                                      self->m_progressPlanned || planned);
                }
            },
            Qt::QueuedConnection);
    };
}

void TrackImportController::onPrepared()
{
    QString thrown;
    PreparedImport prepared = takeResult(m_watcher, &thrown);
    setErrorSource(QStringLiteral("import"));
    setBusy(false);
    setProgress({}, 0, 0, {});
    if (m_cancel.cancelled()) {
        return;
    }
    // Experimental features switched off while it was prepared: nothing
    // new is staged.
    if (!allowedToStage()) {
        return;
    }
    if (!thrown.isEmpty()) {
        setErrorMessage(tr("The import could not be prepared: %1").arg(thrown));
        return;
    }
    if (!prepared.refusal.empty()) {
        setErrorMessage(QString::fromStdString(prepared.refusal));
        return;
    }
    // The stick may have started a save, or taken an edit, meanwhile.
    if (!attachSession(m_preparing.rekordboxPath, m_preparing.enginePath)) {
        return;
    }
    // The batch this one joins has to wait as it did: saved, taken back
    // or cut down meanwhile, what was prepared holds tracks that are on
    // the stick by now, or that were just refused.
    const bool joining = m_joining;
    if (joining && m_staged != m_joined) {
        setErrorMessage(tr("The import that was waiting changed while more was added to it. Offer the new "
                                       "files again."));
        return;
    }
    // Nothing but that batch may wait.
    if (m_session->pendingCount() != (joining ? static_cast<int>(m_staged.size()) : 0)) {
        setErrorMessage(tr("Edits were staged while the import was being prepared. Save or discard them, "
                                       "then import again."));
        return;
    }
    if (joining && !prepared.staged.changes.empty()) {
        // The joined batch holds everything the waiting one does: that
        // one goes now, and its place is taken at once below (in order:
        // staging over it by id would leave the old changes where they
        // stood, the move of the run before the tracks added to it).
        // What could refuse the staging below (a save writing, another
        // editor's changes, a lock) is ruled out by the batch that
        // waited having been staged here.
        dropStaged();
        if (m_session->pendingCount() > 0) {
            setErrorMessage(tr("The import that waits could not be taken back to add to it."));
            return;
        }
    }
    m_request = m_preparing;

    m_possibleDuplicates = prepared.possibleDuplicates;
    QVariantList notImported;
    for (const LeftOut &left : prepared.notImported) {
        notImported << QVariantMap{{QStringLiteral("name"), QFileInfo(QString::fromStdString(left.sourceFile)).fileName()},
                                   {QStringLiteral("why"), QString::fromStdString(left.why)},
                                   {QStringLiteral("onStick"), left.onStick}};
    }
    const QString playlist = m_request.playlist ? QString::fromStdString(m_request.playlist->path) : QString();
    // Made by the save in at least one library: for Own Recordings' playlist
    // (createWhereMissing) only where a library lacks it.
    const bool isNew = m_request.playlist && (m_request.playlist->create || !prepared.staged.createdIn.isEmpty());
    int stagedTracks = 0;
    if (!prepared.staged.changes.empty()) {
        std::set<QString> ids;
        std::set<QString> imports;
        std::set<QString> importUnits;
        for (const auto &change : prepared.staged.changes) {
            ids.insert(change->id());
            if (std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                imports.insert(change->id());
                importUnits.insert(change->unitKeys().last());
            }
        }
        // A playlist entry tied to no import is a track of the stick's.
        std::set<QString> existing;
        for (const auto &change : prepared.staged.changes) {
            if (std::dynamic_pointer_cast<AddToPlaylistChange>(change)) {
                bool tied = false;
                for (const QString &key : change->unitKeys()) {
                    tied = tied || importUnits.count(key) > 0;
                }
                if (!tied) {
                    existing.insert(change->id());
                }
            }
        }
        // Each row's source on the computer: an import's its own, a
        // track of the stick's the k-th offered file found there, as
        // staged.addedToPlaylist lists them in the entries' order.
        std::map<QString, std::string> sources;
        std::size_t existingIndex = 0;
        for (const auto &change : prepared.staged.changes) {
            if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                sources[change->id()] = import->sourceFile();
            } else if (existing.count(change->id()) > 0 && existingIndex < prepared.staged.addedToPlaylist.size()) {
                sources[change->id()] = prepared.staged.addedToPlaylist[existingIndex++];
            }
        }
        if (!m_session->stageAll(prepared.staged.changes)) {
            // The session said why (a lock another instance holds, a
            // browsed backup).
            setErrorMessage(tr("The import could not be staged on this stick."));
            return;
        }
        m_sourceOf = std::move(sources);
        // Each row's cover as a file of the app's cache folder: the list
        // is handed its path, and draws it as it draws the stick's
        // covers, through the cover cache.
        m_covers.clear();
        for (const auto &[id, jpeg] : prepared.covers) {
            if (QString file = keepCover(jpeg); !file.isEmpty()) {
                m_covers[id] = std::move(file);
            }
        }
        m_landed.clear();
        m_staged = std::move(ids);
        m_stagedImports = std::move(imports);
        m_stagedTracks = stagedTracks = static_cast<int>(m_stagedImports.size());
        m_stagedExistingIds = std::move(existing);
        m_stagedExisting = static_cast<int>(m_stagedExistingIds.size());
        m_stagedPlaylist = playlist;
        m_stagedPlaylistIsNew = isNew;
        ++m_pendingRevision;
        refreshStagedImport();
        emit pendingChanged();
    }
    QStringList followed;
    for (const std::string &name : prepared.followedGrids) {
        followed << QString::fromStdString(name);
    }
    m_lastResult = QVariantMap{
        {QStringLiteral("staged"), stagedTracks},
        {QStringLiteral("addedToPlaylist"), stagedTracks > 0 || !prepared.staged.changes.empty()
                                                ? static_cast<int>(prepared.staged.addedToPlaylist.size())
                                                : 0},
        {QStringLiteral("analysed"), prepared.analysed},
        {QStringLiteral("withGrid"), prepared.withGrid},
        {QStringLiteral("followedGrids"), followed},
        {QStringLiteral("megabytes"), static_cast<double>(prepared.bytesToCopy) / (1024.0 * 1024.0)},
        {QStringLiteral("playlist"), playlist},
        {QStringLiteral("playlistIsNew"), isNew},
        {QStringLiteral("note"), QString::fromStdString(prepared.staged.note)},
        {QStringLiteral("notImported"), notImported},
        {QStringLiteral("possibleDuplicates"), static_cast<int>(prepared.possibleDuplicates.size())},
    };
    emit resultReady();
}

void TrackImportController::dropStaged()
{
    if (m_session && !m_staged.empty() && !m_session->writing()) {
        const QStringList ids(m_staged.begin(), m_staged.end());
        m_session->unstageAll(ids);
    }
    forgetStaged();
}

bool TrackImportController::analyseAgain(const QString &rekordboxPath, const QString &enginePath,
                                        const QString &playlist)
{
    setErrorSource(QStringLiteral("reanalysis"));
    setErrorMessage({});
    if (!allowedToStage()) {
        return false;
    }
    if (m_busy) {
        setErrorMessage(!m_reanalysingPlaylist.isEmpty()
                            ? tr("Tracks are being analysed again already.")
                            : tr("An import is being prepared. Analyse again when it has finished."));
        return false;
    }
    if (playlist.isEmpty()) {
        setErrorMessage(tr("Analyse again takes the tracks of a playlist."));
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    ReanalysisRequest request;
    request.rekordboxPath = rekordboxPath;
    request.enginePath = enginePath;
    request.playlist = playlist.toStdString();
    request.analysisJobs = importAnalysisJobs();
    request.analyser = analyser();
    request.cueFollowThresholdMs = storedCueFollowThresholdMs(openSeabassSettings());
    m_reanalysing = request;
    m_reanalysingPlaylist = playlist;
    m_cancel = application::CancellationToken();
    setBusy(true);
    setProgress(QStringLiteral("stick"), 0, 0, {}, {}, 0.0);
    const ImportProgressSink progress = progressSink();
    m_reanalysisWatcher.setFuture(
        runRead([request, cancel = m_cancel, progress]() { return prepareReanalysis(request, cancel, progress); }));
    return true;
}

void TrackImportController::onReanalysed()
{
    QString thrown;
    PreparedReanalysis prepared = takeResult(m_reanalysisWatcher, &thrown);
    const QString playlist = m_reanalysingPlaylist;
    m_reanalysingPlaylist.clear();
    setErrorSource(QStringLiteral("reanalysis"));
    setBusy(false);
    setProgress({}, 0, 0, {});
    if (m_cancel.cancelled() || !allowedToStage()) {
        return;
    }
    if (!thrown.isEmpty()) {
        setErrorMessage(tr("The tracks could not be analysed: %1").arg(thrown));
        return;
    }
    if (!prepared.refusal.empty()) {
        setErrorMessage(QString::fromStdString(prepared.refusal));
        return;
    }
    // The stick may have started a save meanwhile.
    if (!attachSession(m_reanalysing.rekordboxPath, m_reanalysing.enginePath)) {
        return;
    }
    // A track whose Restore original analysis waits to be saved keeps
    // waiting: the two would write over each other in one save, and the
    // restore's own staging refuses the same the other way round.
    std::erase_if(prepared.changes, [&](const std::shared_ptr<ReanalyseTrackChange> &change) {
        if (!m_session->hasChange(RestoreAnalysisChange::idFor(change->stickPath()))) {
            return false;
        }
        prepared.leftAlone.push_back({change->audioFile(), "Restore original analysis of it waits to be saved: save "
                                                           "it or take it back first"});
        return true;
    });
    if (!prepared.changes.empty()) {
        std::vector<std::shared_ptr<PendingChange>> changes(prepared.changes.begin(), prepared.changes.end());
        if (!m_session->stageAll(std::move(changes))) {
            // The session said why (a lock another instance holds, another
            // page's edits, a browsed backup).
            setErrorMessage(tr("The new analyses could not be staged on this stick."));
            return;
        }
        for (const auto &change : prepared.changes) {
            m_reanalysed[change->id()] = playlist;
        }
        // This run's changes take the place of earlier ones of the same
        // track (the session replaced those); the rest still wait, and
        // their switches stay reachable.
        std::erase_if(m_reviewed, [&](const auto &earlier) {
            return std::any_of(prepared.changes.begin(), prepared.changes.end(),
                               [&](const auto &change) { return change->id() == earlier->id(); });
        });
        m_reviewed.insert(m_reviewed.end(), prepared.changes.begin(), prepared.changes.end());
        ++m_pendingRevision;
        emit pendingChanged();
    }
    pruneCueReview();
    int cueTracks = 0;
    int cuesToMove = 0;
    int autoTracks = 0;
    int autoCues = 0;
    int tooMuch = 0;
    for (const auto &change : prepared.changes) {
        const auto &plan = change->cueReview().plan;
        const int moves = static_cast<int>(plan.moves.size());
        cueTracks += moves > 0 ? 1 : 0;
        cuesToMove += moves;
        // As the worker decided, by the request's threshold.
        if (change->movesOnTheirOwnUnderMs() > 0) {
            ++autoTracks;
            autoCues += moves;
        }
        tooMuch += plan.verdict == application::CueGridPlan::Verdict::GridChangedTooMuch ? 1 : 0;
    }
    QVariantList leftAloneTracks;
    for (const LeftOut &left : prepared.leftAlone) {
        leftAloneTracks << QVariantMap{{QStringLiteral("name"), pathToQString(pathFromUtf8(left.sourceFile).filename())},
                                       {QStringLiteral("why"), QString::fromStdString(left.why)}};
    }
    m_lastReanalysis = QVariantMap{
        {QStringLiteral("playlist"), playlist},
        {QStringLiteral("analysed"), static_cast<int>(prepared.changes.size())},
        {QStringLiteral("leftAlone"), static_cast<int>(prepared.leftAlone.size())},
        {QStringLiteral("leftAloneTracks"), leftAloneTracks},
        {QStringLiteral("cueTracks"), cueTracks},
        {QStringLiteral("cuesToMove"), cuesToMove},
        {QStringLiteral("autoCueTracks"), autoTracks},
        {QStringLiteral("autoCues"), autoCues},
        {QStringLiteral("askedCueTracks"), cueTracks - autoTracks},
        {QStringLiteral("askedCues"), cuesToMove - autoCues},
        {QStringLiteral("cueFollowThresholdMs"), m_reanalysing.cueFollowThresholdMs},
        {QStringLiteral("gridChangedTooMuch"), tooMuch},
    };
    emit cueReviewChanged();
    emit reanalysisReady();
}

void TrackImportController::dropReanalysis(const QString &playlist)
{
    QStringList ids;
    for (const auto &[id, from] : m_reanalysed) {
        if (from == playlist) {
            ids << id;
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    if (m_session && !m_session->writing()) {
        m_session->unstageAll(ids);
    }
}

bool TrackImportController::saveWriting() const
{
    return m_session && m_session->writing();
}

bool TrackImportController::setMoveCues(const QString &changeId, bool move)
{
    if (saveWriting()) {
        return false;
    }
    for (const auto &change : m_reviewed) {
        if (change->id() == changeId) {
            const bool was = change->moveCues();
            change->setMoveCues(move);
            if (was != move) {
                emit moveCuesChanged(changeId, move);
            }
        }
    }
    return true;
}

bool TrackImportController::setMoveCuesForAll(bool move)
{
    if (saveWriting()) {
        return false;
    }
    for (const auto &change : m_reviewed) {
        if (change->cueReview().plan.moves.empty()) {
            continue;
        }
        // The person's choice for every track, those already so included.
        const bool was = change->moveCues();
        change->setMoveCues(move);
        if (was != move) {
            emit moveCuesChanged(change->id(), move);
        }
    }
    return true;
}

bool TrackImportController::movesCues(const QString &changeId) const
{
    return std::any_of(m_reviewed.begin(), m_reviewed.end(),
                       [&](const auto &change) { return change->id() == changeId && change->moveCues(); });
}

int TrackImportController::cueFollowThresholdMs() const
{
    return storedCueFollowThresholdMs(openSeabassSettings());
}

int TrackImportController::applyCueThreshold(int ms)
{
    if (saveWriting()) {
        return -1;
    }
    ms = clampedCueFollowThresholdMs(ms);
    if (ms != cueFollowThresholdMs()) {
        QSettings settings = openSeabassSettings();
        storeCueFollowThresholdMs(settings, ms);
        settings.sync();
        emit cueFollowThresholdChanged();
    }
    int keptByHand = 0;
    for (const auto &change : m_reviewed) {
        const auto &plan = change->cueReview().plan;
        if (plan.moves.empty()) {
            continue;
        }
        const bool follows = application::cuesFollowOnTheirOwn(plan, ms);
        const bool was = change->moveCues();
        if (change->moveCuesChosen()) {
            keptByHand += was != follows ? 1 : 0;
            continue;
        }
        change->setMoveCuesByThreshold(follows, ms);
        if (change->moveCues() != was) {
            emit moveCuesChanged(change->id(), change->moveCues());
        }
    }
    return keptByHand;
}

QVariantList TrackImportController::cueReview() const
{
    QVariantList tracks;
    for (const auto &change : m_reviewed) {
        const auto &review = change->cueReview();
        if (review.plan.moves.empty()) {
            continue;
        }
        QVariantList rows;
        for (const application::CueGridMove &move : review.plan.moves) {
            QVariantMap row{
                {QStringLiteral("label"), ReanalyseTrackChange::cueLabel(move.kind, move.hotCueNumber, move.isLoop)},
                {QStringLiteral("name"), QString::fromStdString(move.comment)},
                {QStringLiteral("color"), QString::fromStdString(move.color)},
                {QStringLiteral("isLoop"), move.isLoop},
                {QStringLiteral("oldText"), ReanalyseTrackChange::clockText(move.oldMs)},
                {QStringLiteral("newText"), ReanalyseTrackChange::clockText(move.newMs)},
                {QStringLiteral("diffMs"), shownMoveMs(move.oldMs, move.newMs)},
                {QStringLiteral("strip"), change->reviewStrip(move)},
            };
            if (move.isLoop) {
                row.insert(QStringLiteral("oldEndText"), ReanalyseTrackChange::clockText(move.oldEndMs));
                row.insert(QStringLiteral("newEndText"), ReanalyseTrackChange::clockText(move.newEndMs));
            }
            rows << row;
        }
        tracks << QVariantMap{
            {QStringLiteral("id"), change->id()},
            {QStringLiteral("title"), change->subject()},
            {QStringLiteral("gridShiftMs"), static_cast<int>(std::lround(change->gridShiftMs()))},
            {QStringLiteral("oldBpm"), review.oldBpm},
            {QStringLiteral("newBpm"), change->analysis()->bpm},
            {QStringLiteral("moveCues"), change->moveCues()},
            {QStringLiteral("largestMoveMs"), application::largestCueMoveMs(review.plan)},
            {QStringLiteral("offBeat"), review.plan.offBeat},
            {QStringLiteral("rows"), rows},
        };
    }
    return tracks;
}

QStringList TrackImportController::cueReviewTooDifferent() const
{
    QStringList titles;
    for (const auto &change : m_reviewed) {
        if (change->cueReview().plan.verdict == application::CueGridPlan::Verdict::GridChangedTooMuch) {
            titles << change->subject();
        }
    }
    return titles;
}

int TrackImportController::reanalysisCount(const QString &playlist) const
{
    return static_cast<int>(std::count_if(m_reanalysed.begin(), m_reanalysed.end(),
                                          [&](const auto &entry) { return entry.second == playlist; }));
}

int TrackImportController::trackCountChange(const QString &playlist) const
{
    if (m_staged.empty()) {
        return 0;
    }
    if (playlist.isEmpty()) {
        return m_stagedTracks;
    }
    return playlist == m_stagedPlaylist ? m_stagedTracks + m_stagedExisting : 0;
}

bool TrackImportController::dropStagedTrack(const QString &changeId)
{
    setErrorSource(QStringLiteral("import"));
    setErrorMessage({});
    if (!m_session || m_session->writing() || m_staged.count(changeId) == 0) {
        return false;
    }
    std::vector<std::shared_ptr<PendingChange>> batch;
    std::shared_ptr<PendingChange> picked;
    for (const auto &change : m_session->pendingChanges()) {
        if (m_staged.count(change->id()) > 0) {
            batch.push_back(change);
            if (change->id() == changeId) {
                picked = change;
            }
        }
    }
    // What goes: a new track's import with what shares its unit (its
    // playlist entry), or the entry of a track the stick has.
    QStringList ids;
    QString unitKey;
    std::string file;
    if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(picked)) {
        unitKey = import->unitKeys().last();
        file = import->destinationFile();
        for (const auto &change : batch) {
            if (change == picked
                || (std::dynamic_pointer_cast<AddToPlaylistChange>(change) && change->unitKeys().contains(unitKey))) {
                ids << change->id();
            }
        }
    } else if (const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(picked)) {
        file = entry->filePath();
        ids << entry->id();
    } else {
        return false;
    }
    // What stays: is there still a track in the batch?
    std::shared_ptr<ImportTrackChange> nextImport;
    bool anyLeft = false;
    for (const auto &change : batch) {
        if (ids.contains(change->id())) {
            continue;
        }
        const auto import = std::dynamic_pointer_cast<ImportTrackChange>(change);
        if (import && !nextImport) {
            nextImport = import;
        }
        anyLeft = anyLeft || import || std::dynamic_pointer_cast<AddToPlaylistChange>(change);
    }
    const auto source = m_sourceOf.find(changeId);
    if (source != m_sourceOf.end()) {
        m_request.leftOutByPerson.insert(source->second);
    }
    if (!anyLeft) {
        // The last of it: the batch whole, the playlist it would make too.
        dropStaged();
        return true;
    }
    for (const auto &change : batch) {
        if (const auto place = std::dynamic_pointer_cast<PlaceInPlaylistChange>(change)) {
            // The run is moved without it.
            if (!place->leaveOut(file)) {
                ids << place->id();
            }
        } else if (const auto made = std::dynamic_pointer_cast<CreatePlaylistChange>(change);
                   made && !unitKey.isEmpty() && made->unitKeys().contains(unitKey)) {
            // The playlist made for the batch goes with its first track
            // still to come, or stands alone for the stick's tracks.
            made->tieTo(nextImport ? nextImport->unitKeys().last() : QString());
        }
    }
    m_session->unstageAll(ids);
    return true;
}

QString TrackImportController::suggestedPlaylistName(const QVariantList &files) const
{
    if (files.size() != 1) {
        return {};
    }
    const QUrl url = files.front().toUrl();
    const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : files.front().toString());
    // Without what a playlist name may not hold in every library.
    return info.isDir() ? info.fileName().remove(QLatin1Char('/')).remove(QLatin1Char(';')).trimmed() : QString();
}

QString TrackImportController::fileDialogFilter() const
{
    QStringList patterns;
    for (const std::string &extension : domain::importableExtensions({"rekordbox", "onelibrary", "engine"})) {
        patterns << QStringLiteral("*") + QString::fromStdString(extension);
    }
    return patterns.join(QLatin1Char(' '));
}

}  // namespace seabass::gui
