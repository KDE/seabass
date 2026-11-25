// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_controller.hpp"

#include <QDate>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

#include "domain/track_import_plan.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"

namespace seabass::gui
{

TrackImportController::TrackImportController(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<PreparedImport>::finished, this, &TrackImportController::onPrepared);
}

TrackImportController::~TrackImportController()
{
    // The worker reads the stick; it is told to stop and waited for, so
    // nothing of this object is used after it.
    m_cancel.cancel();
    m_watcher.waitForFinished();
}

void TrackImportController::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void TrackImportController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

void TrackImportController::forgetStaged()
{
    if (m_staged.empty() && m_stagedTracks == 0) {
        return;
    }
    m_staged.clear();
    m_stagedImports.clear();
    m_stagedTracks = 0;
    ++m_pendingRevision;
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
        forgetStaged();
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (m_staged.erase(changeId) > 0) {
                    if (m_stagedImports.erase(changeId) > 0) {
                        --m_stagedTracks;
                        ++m_appliedTracks;
                    }
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &summary) {
                const bool wrote = m_appliedTracks > 0 && summary.value("written").toInt() > 0;
                m_appliedTracks = 0;
                if (wrote) {
                    emit tracksImported();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() { forgetStaged(); });
            // A change of the batch taken back elsewhere (the session's
            // own list): what is left of it is no longer what was
            // rehearsed, so the count follows the session.
            connect(m_session, &LibraryEditSession::pendingChanged, this, [this]() {
                if (m_staged.empty() || !m_session || m_session->writing()) {
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
                    m_staged = std::move(still);
                    m_stagedTracks = tracks;
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
        }
    }
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick has no edit session open."));
        return false;
    }
    m_session->setLibraryPaths(rekordboxPath, enginePath);
    if (m_session->writing()) {
        setErrorMessage(QStringLiteral("A save is writing to the stick. Try again when it has finished."));
        return false;
    }
    return true;
}

bool TrackImportController::importFiles(const QString &rekordboxPath, const QString &enginePath,
                                        const QVariantList &files, const QString &playlist, bool create, int atIndex)
{
    setErrorMessage({});
    ImportRequest request;
    for (const QVariant &file : files) {
        const QUrl url = file.toUrl();
        const QString path = url.isLocalFile() ? url.toLocalFile() : (url.scheme().isEmpty() ? file.toString() : QString());
        if (!path.isEmpty()) {
            request.offered.push_back(QFileInfo(path).absoluteFilePath().toStdString());
        }
    }
    if (request.offered.empty()) {
        setErrorMessage(QStringLiteral("Nothing that was dropped is a file on this computer."));
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

bool TrackImportController::importPossibleDuplicates()
{
    setErrorMessage({});
    if (m_possibleDuplicates.empty() || m_request.offered.empty()) {
        return false;
    }
    ImportRequest again = m_request;
    again.importAnyway.insert(m_possibleDuplicates.begin(), m_possibleDuplicates.end());
    dropStaged();
    return start(std::move(again));
}

bool TrackImportController::start(ImportRequest request)
{
    if (m_busy) {
        setErrorMessage(QStringLiteral("An import is being prepared already."));
        return false;
    }
    if (!attachSession(request.rekordboxPath, request.enginePath)) {
        return false;
    }
    if (m_session->pendingCount() > 0) {
        setErrorMessage(m_stagedTracks > 0
                            ? QStringLiteral("An import is waiting to be saved. Save it, or take it back, before the next.")
                            : QStringLiteral("Save or discard the edits that are waiting first: an import is checked "
                                             "against the stick as it is now."));
        return false;
    }
    request.date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")).toStdString();
    m_request = request;
    m_cancel = application::CancellationToken();
    setBusy(true);
    m_watcher.setFuture(
        QtConcurrent::run([request, cancel = m_cancel]() { return prepareTrackImport(request, cancel); }));
    return true;
}

void TrackImportController::onPrepared()
{
    QString thrown;
    PreparedImport prepared = takeResult(m_watcher, &thrown);
    setBusy(false);
    if (m_cancel.cancelled()) {
        return;
    }
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("The import could not be prepared: %1").arg(thrown));
        return;
    }
    if (!prepared.refusal.empty()) {
        setErrorMessage(QString::fromStdString(prepared.refusal));
        return;
    }
    // The stick may have started a save, or taken an edit, meanwhile.
    if (!attachSession(m_request.rekordboxPath, m_request.enginePath)) {
        return;
    }
    if (m_session->pendingCount() > 0) {
        setErrorMessage(QStringLiteral("Edits were staged while the import was being prepared. Save or discard them, "
                                       "then import again."));
        return;
    }

    m_possibleDuplicates = prepared.possibleDuplicates;
    QVariantList notImported;
    for (const LeftOut &left : prepared.notImported) {
        notImported << QVariantMap{{QStringLiteral("name"), QFileInfo(QString::fromStdString(left.sourceFile)).fileName()},
                                   {QStringLiteral("why"), QString::fromStdString(left.why)}};
    }
    const QString playlist = m_request.playlist ? QString::fromStdString(m_request.playlist->path) : QString();
    const bool isNew = m_request.playlist && m_request.playlist->create;
    int stagedTracks = 0;
    if (!prepared.staged.changes.empty()) {
        std::set<QString> ids;
        std::set<QString> imports;
        for (const auto &change : prepared.staged.changes) {
            ids.insert(change->id());
            if (std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                imports.insert(change->id());
            }
        }
        if (!m_session->stageAll(prepared.staged.changes)) {
            // The session said why (a lock another instance holds, a
            // browsed backup).
            setErrorMessage(QStringLiteral("The import could not be staged on this stick."));
            return;
        }
        m_staged = std::move(ids);
        m_stagedImports = std::move(imports);
        m_stagedTracks = stagedTracks = static_cast<int>(m_stagedImports.size());
        m_stagedPlaylist = playlist;
        m_stagedPlaylistIsNew = isNew;
        ++m_pendingRevision;
        emit pendingChanged();
    }
    m_lastResult = QVariantMap{
        {QStringLiteral("staged"), stagedTracks},
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

int TrackImportController::trackCountChange(const QString &playlist) const
{
    return m_stagedTracks > 0 && !playlist.isEmpty() && playlist == m_stagedPlaylist ? m_stagedTracks : 0;
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
