// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/beat_grids_controller.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "application/phased_progress.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "gui/edit/changes/copy_beat_grid_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/qt_path.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

// --- the list of tracks ------------------------------------------------

int BeatGridTrackListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant BeatGridTrackListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    default:
        return {};
    }
}

QHash<int, QByteArray> BeatGridTrackListModel::roleNames() const
{
    return {{TitleRole, "title"}, {ArtistRole, "artist"}};
}

void BeatGridTrackListModel::setRows(std::vector<Row> rows)
{
    const bool countMoved = rows.size() != m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (countMoved) {
        emit countChanged();
    }
}

// --- the analysis, on a worker -------------------------------------------

namespace
{

// Runs on a worker thread and touches nothing of the controller. Reads
// both catalogs (through the shared cache, as Sync Cue Points does),
// plans the copy the way the command line does, and asks the writer for a
// preview of every row: the same checks a copy makes, nothing written.
std::shared_ptr<BeatGridAnalysis> runAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                              bool overwriteEngineEdits, std::shared_ptr<QtProgressReporter> reporter,
                                              application::CancellationToken cancel)
{
    auto out = std::make_shared<BeatGridAnalysis>();
    out->enginePath = enginePath;
    out->overwriteEngineEdits = overwriteEngineEdits;
    try {
        namespace paths = infrastructure::paths;
        const std::string rekordbox = rekordboxPath.toStdString();
        const std::string engine = enginePath.toStdString();
        if (rekordbox.empty() || engine.empty()) {
            out->error = QStringLiteral("Copying beat grids needs a rekordbox export and an Engine library on the "
                                        "same stick.");
            return out;
        }
        // The ledger lives on one stick, and a track is keyed by its path
        // from that stick's root.
        const std::string stickRoot = paths::normalStickRootForCatalogPath(rekordbox);
        if (stickRoot != paths::normalStickRootForCatalogPath(engine)) {
            out->error = QStringLiteral("The rekordbox export and the Engine library are on different sticks, so "
                                        "their beat grids are not copied here.");
            return out;
        }
        out->stickRoot = stickRoot;
        // Engine 1.x keeps m.db at the library's root and its grids in a
        // shape the writer does not write.
        std::error_code ec;
        const fs::path library = pathFromUtf8(engine);
        if (!fs::is_regular_file(library / "Database2" / "m.db", ec) && fs::is_regular_file(library / "m.db", ec)) {
            out->engineTooOld = true;
            return out;
        }

        auto &cache = LibraryCatalogCache::instance();
        // One bar (#58): the reads the cache still has to make, then one
        // tick per matched track, at most one per rekordbox track.
        std::optional<size_t> planned = 0;
        const auto addUnits = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        addUnits(cache.plannedUnits("rekordbox", rekordbox, LibraryCatalogCache::Detail::Full, cancel));
        addUnits(cache.plannedUnits("engine", engine, LibraryCatalogCache::Detail::Full, cancel));
        addUnits(cache.countTracks("rekordbox", rekordbox, cancel));
        application::PhasedProgress progress(*reporter, "Checking beat grids", planned.value_or(0));

        // Full, as Sync Cue Points reads them: matching refuses to pair
        // two tracks of one name while either length is unknown.
        const auto rekordboxTracks = cache.tracksFor("rekordbox", rekordbox, LibraryCatalogCache::Detail::Full,
                                                     progress, cancel);
        auto engineTracks = cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Full, progress, cancel);
        // Streaming tracks (TIDAL) have no file on the stick.
        engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                          [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                           engineTracks.end());
        const application::BeatGridCopyPlan plan = application::planBeatGridCopy(
            rekordboxTracks, engineTracks, stickRoot, &application::SyncLibraries::stickFiles());
        out->matched = static_cast<int>(plan.items.size());

        std::string ledgerError;
        const auto ledger = infrastructure::local::readBeatGridLedger(pathFromUtf8(stickRoot), &ledgerError);
        out->ledgerError = QString::fromStdString(ledgerError);

        infrastructure::engine::LibdjinteropEngineCueWriter writer(engine);
        std::map<std::string, std::vector<domain::Beat>> read;
        // A stop is seen between rows by copyBeatGrids() itself: anything
        // thrown from here would count as a row that failed.
        const application::BeatGridSource source = [&](const application::BeatGridCopyItem &item) {
            auto beats = infrastructure::rekordbox::readBeatGrid(rekordbox, item.rekordboxTrackId);
            read[item.engineTrackId] = beats;
            return beats;
        };
        application::BeatGridCopyOptions options;
        options.previewOnly = true;
        options.overwriteEngineEdits = overwriteEngineEdits;
        out->preview = application::copyBeatGrids(plan, source, writer, ledger, options, {}, cancel, progress);
        if (out->preview.cancelled || cancel.cancelled()) {
            throw application::OperationCancelled();
        }
        if (out->preview.unsupportedLibrary > 0) {
            out->engineTooOld = true;
        }
        for (const auto &item : out->preview.wouldWriteItems) {
            out->beats[item.engineTrackId] = std::move(read[item.engineTrackId]);
            if (!item.ledgerKey.empty()) {
                if (const auto own = ledger.find(item.ledgerKey); own != ledger.end()) {
                    out->ownFingerprints[item.engineTrackId] = own->second;
                }
            }
        }
        progress.done();
    } catch (const application::OperationCancelled &) {
        out->cancelled = true;
    } catch (const std::exception &e) {
        out->error = QString::fromStdString(e.what());
    }
    return out;
}

}  // namespace

// --- the controller ----------------------------------------------------

BeatGridsController::BeatGridsController(QObject *parent) : QObject(parent) {}

BeatGridsController::~BeatGridsController() = default;

QString BeatGridsController::ledgerWarning() const
{
    if (!m_result || m_result->ledgerError.isEmpty()) {
        return {};
    }
    return QStringLiteral("Seabass's record of the beat grids it copied before cannot be read (%1). Until it can, "
                          "every grid Seabass copied counts as changed on the player and is left alone. The next "
                          "copy writes a new record.")
        .arg(m_result->ledgerError);
}

bool BeatGridsController::writing() const
{
    return m_session && m_session->writing();
}

void BeatGridsController::setReplaceEngineEdits(bool replace)
{
    // What is staged was previewed with the other setting.
    if (m_replaceEngineEdits == replace || !m_stagedIds.isEmpty()) {
        return;
    }
    m_replaceEngineEdits = replace;
    emit replaceEngineEditsChanged();
    if (!m_rekordboxPath.isEmpty() && !m_enginePath.isEmpty()) {
        start(false);
    }
}

void BeatGridsController::analyze(const QString &rekordboxPath, const QString &enginePath)
{
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    start(false);
}

void BeatGridsController::cancelScan()
{
    if (busy()) {
        m_scan.cancel();
    }
}

void BeatGridsController::start(bool restart)
{
    const QString key = m_rekordboxPath + QLatin1Char('\n') + m_enginePath + QLatin1Char('\n')
        + (m_replaceEngineEdits ? QStringLiteral("replace") : QStringLiteral("keep"));
    if (!restart && m_scan.busy() && m_scan.key() == key) {
        return;
    }
    setErrorMessage({});
    setScanProgress(0, 0);

    auto reporter = makeMainThreadShared<QtProgressReporter>();
    const auto current = m_scan.speaksForNext();
    connect(reporter.get(), &QtProgressReporter::started, this, [this, current](const QString &label, int total) {
        if (current()) {
            m_scanLabel = label;
            setScanProgress(0, total);
            emit scanProgressChanged();
        }
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this, current](const QString &label) {
        if (current() && m_scanLabel != label) {
            m_scanLabel = label;
            emit scanProgressChanged();
        }
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this, [this, current](int done) {
        if (current()) {
            setScanProgress(done, m_scanTotal);
        }
    });

    using Request = AsyncRequest<std::shared_ptr<BeatGridAnalysis>>;
    Request::Work work = [rekordboxPath = m_rekordboxPath, enginePath = m_enginePath,
                          overwrite = m_replaceEngineEdits, reporter](application::CancellationToken cancel) {
        return runAnalysis(rekordboxPath, enginePath, overwrite, reporter, cancel);
    };
    Request::Ending ending{
        [this](std::shared_ptr<BeatGridAnalysis> &&result) {
            if (!result || result->cancelled) {
                emit scanCancelled();
                return;
            }
            if (!result->error.isEmpty()) {
                setErrorMessage(result->error);
                m_result.reset();
                m_tracks.setRows({});
                emit resultChanged();
                return;
            }
            std::vector<BeatGridTrackListModel::Row> rows;
            rows.reserve(result->preview.wouldWriteItems.size());
            for (const auto &item : result->preview.wouldWriteItems) {
                rows.push_back({QString::fromStdString(item.title), QString::fromStdString(item.artist)});
            }
            m_result = std::move(*result);
            m_tracks.setRows(std::move(rows));
            emit resultChanged();
        },
        [this](const QString &message) { setErrorMessage(message); },
        [this]() { emit scanCancelled(); },
    };
    const QString stick = stickRootOf(m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath);
    if (restart) {
        m_scan.restart(key, stick, std::move(work), std::move(ending));
    } else {
        m_scan.start(key, stick, std::move(work), std::move(ending));
    }
}

void BeatGridsController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    LibraryEditSession *session = any.isEmpty() ? nullptr : registry->sessionFor(registry->libraryIdForPath(any));
    if (session == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    emit writingChanged();
    if (!m_session) {
        return;
    }
    connect(m_session, &LibraryEditSession::stateChanged, this, &BeatGridsController::writingChanged);
    connect(m_session, &LibraryEditSession::changesDiscarded, this, &BeatGridsController::clearStaged);
    // Whatever the save was (this page's copies, another check's fix, an
    // undo), the rows may have changed: the counts are read again. The
    // copies that landed are no longer staged.
    connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
        QStringList still;
        for (const QString &id : std::as_const(m_stagedIds)) {
            if (m_session && m_session->hasChange(id)) {
                still << id;
            }
        }
        if (still != m_stagedIds) {
            m_stagedIds = still;
            emit stagedChanged();
        }
        if (!m_rekordboxPath.isEmpty() && !m_enginePath.isEmpty()) {
            start(true);
        }
    });
}

void BeatGridsController::stage()
{
    if (busy() || !m_result || m_result->preview.wouldWriteItems.empty() || !m_stagedIds.isEmpty()) {
        return;
    }
    setErrorMessage({});
    attachSession();
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick's library could not be identified; nothing was changed."));
        return;
    }
    if (m_session->writing()) {
        setErrorMessage(QStringLiteral("A save is running. Stage more once it has finished."));
        return;
    }
    m_session->setLibraryPaths(m_rekordboxPath, m_enginePath);
    const auto &items = m_result->preview.wouldWriteItems;
    const int count = static_cast<int>(items.size());
    std::vector<std::unique_ptr<PendingChange>> changes;
    changes.reserve(items.size());
    QStringList ids;
    for (const auto &item : items) {
        const auto beats = m_result->beats.find(item.engineTrackId);
        const auto own = m_result->ownFingerprints.find(item.engineTrackId);
        changes.push_back(std::make_unique<CopyBeatGridChange>(
            m_result->enginePath, m_result->stickRoot, item,
            beats == m_result->beats.end() ? std::vector<domain::Beat>() : beats->second,
            own == m_result->ownFingerprints.end() ? std::nullopt : std::optional<std::string>(own->second),
            m_result->overwriteEngineEdits, count, changes.empty()));
        ids << CopyBeatGridChange::idFor(item.engineTrackId);
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session said why (another page's edits, or the lock)
    }
    m_stagedIds = ids;
    emit stagedChanged();
}

void BeatGridsController::unstage()
{
    if (m_stagedIds.isEmpty()) {
        return;
    }
    if (m_session) {
        m_session->unstageAll(m_stagedIds);
    }
    clearStaged();
}

void BeatGridsController::clearStaged()
{
    if (m_stagedIds.isEmpty()) {
        return;
    }
    m_stagedIds.clear();
    emit stagedChanged();
}

void BeatGridsController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void BeatGridsController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

}  // namespace seabass::gui
