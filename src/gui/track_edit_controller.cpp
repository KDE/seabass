// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_edit_controller.hpp"

#include <QDebug>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"

namespace seabass::gui
{

namespace
{
std::function<void()> &buildGate()
{
    static std::function<void()> gate;
    return gate;
}
}  // namespace

void TrackEditController::setIndexBuildGateForTesting(std::function<void()> gate)
{
    buildGate() = std::move(gate);
}

TrackEditController::TrackEditController(QObject *parent) : QObject(parent)
{
    m_commentLimit = trackInfo({});
    connect(&m_indexWatcher, &QFutureWatcher<IndexRead>::finished, this, [this]() {
        QString error;
        IndexRead read = takeResult(m_indexWatcher, &error);
        if (!error.isEmpty()) {
            read.error = "the stick's catalogs could not be read: " + error.toStdString();
        }
        if (m_hold) {
            m_held = std::move(read);
            return;
        }
        takeRead(std::move(read));
    });
}

// Not waited for: a read of the stick still running finishes on its own
// worker and is dropped, as the other controllers let theirs go (see
// PlaylistEditController). The task holds nothing of this object, only
// copies of the paths, and removes its own temporary copy when it ends;
// the watcher going away with this object stops listening to it.
TrackEditController::~TrackEditController() = default;

void TrackEditController::setHoldIndexForTesting(bool hold)
{
    if (m_hold == hold) {
        return;
    }
    m_hold = hold;
    emit holdIndexForTestingChanged();
    if (!m_hold && m_held) {
        IndexRead read = std::move(*m_held);
        m_held.reset();
        takeRead(std::move(read));
    }
}

void TrackEditController::takeRead(IndexRead read)
{
    if (read.generation != m_generation) {
        // Read before the stick changed: read it again.
        m_indexState = IndexState::None;
        buildIndex();
        return;
    }
    if (read.error.empty() && read.index.valid()) {
        m_index = std::move(read.index);
        m_indexState = IndexState::Ready;
        m_indexError.clear();
    } else {
        // Not kept: the next track shown reads the stick again.
        m_index = {};
        m_indexState = IndexState::Failed;
        m_indexError = QString::fromStdString(read.error.empty() ? "Seabass could not read this stick, so nothing can be changed here."
                                                                 : read.error);
    }
    publish();
}

QString TrackEditController::cleanComment(const QString &text)
{
    QString cleaned = text;
    cleaned.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    return cleaned.trimmed();
}

QVariantMap TrackEditController::trackInfo(const QString &filePath) const
{
    QVariantMap map;
    map.insert(QStringLiteral("filePath"), filePath);
    const bool ready = m_indexState == IndexState::Ready && !filePath.isEmpty();
    map.insert(QStringLiteral("known"), ready);
    map.insert(QStringLiteral("reading"), m_indexState == IndexState::Building || m_indexState == IndexState::None);
    map.insert(QStringLiteral("failed"), m_indexState == IndexState::Failed);
    TrackRows rows;
    CommentLimit limit;
    std::string refusal;
    if (ready) {
        rows = m_index.rowsFor(filePath.toStdString());
        limit = m_index.limitFor(rows);
        refusal = rows.refusal();
    } else if (m_indexState == IndexState::Failed) {
        refusal = m_indexError.toStdString();
    }
    map.insert(QStringLiteral("editable"), ready && refusal.empty());
    map.insert(QStringLiteral("refusal"), QString::fromStdString(refusal));
    map.insert(QStringLiteral("limited"), limit.limited);
    map.insert(QStringLiteral("units"), static_cast<int>(limit.units));
    map.insert(QStringLiteral("asciiOnly"), limit.asciiOnly);
    map.insert(QStringLiteral("reason"), QString::fromStdString(limit.reason));
    return map;
}

void TrackEditController::publish()
{
    m_commentLimit = trackInfo(m_shownFile);
    emit commentLimitChanged();
}

void TrackEditController::buildIndex()
{
    if (m_indexWatcher.isRunning()) {
        return;
    }
    if (m_rekordboxPath.isEmpty() && m_enginePath.isEmpty()) {
        // Nothing to read, ever: a library with neither catalog's path.
        // An answer, not "still reading": the pane says nothing can be
        // changed here, and why.
        m_index = {};
        m_indexState = IndexState::Failed;
        m_indexError = QStringLiteral("This library has no rekordbox or Engine catalog on a stick, so nothing can be changed here.");
        return;
    }
    m_indexState = IndexState::Building;
    if (m_session && m_session->writing()) {
        // Not while a save writes the catalogs: saveFinished reads them.
        return;
    }
    ++m_builds;
    const QString rekordbox = m_rekordboxPath;
    const QString engine = m_enginePath;
    const int generation = m_generation;
    const std::function<void()> gate = buildGate();
    m_indexWatcher.setFuture(QtConcurrent::run([rekordbox, engine, generation, gate]() {
        IndexRead read;
        read.generation = generation;
        if (gate) {
            gate();
        }
        try {
            read.index = buildStickPathIndex(rekordbox, engine);
        } catch (const StickReadError &e) {
            // The pane says which catalog in a sentence; the detail is for
            // the log.
            qWarning().noquote() << "track details: could not read the" << QString::fromStdString(e.catalog())
                                 << "on" << rekordbox << engine << ":" << e.what();
            read.error = "Seabass could not read the " + e.catalog() + " on this stick, so nothing can be changed here.";
        } catch (const std::exception &e) {
            qWarning().noquote() << "track details: could not read" << rekordbox << engine << ":" << e.what();
            read.error = "Seabass could not read this stick, so nothing can be changed here.";
        } catch (...) {
            read.error = "Seabass could not read this stick, so nothing can be changed here.";
        }
        return read;
    }));
}

void TrackEditController::invalidate()
{
    ++m_generation;
    m_index = {};
    m_indexState = IndexState::None;
    m_held.reset();
    if (!m_shownFile.isEmpty()) {
        buildIndex();
    }
    publish();
}

void TrackEditController::lookUpTrack(const QString &rekordboxPath, const QString &enginePath, const QString &filePath)
{
    setErrorMessage({});
    // The session is watched from the first track shown, so an edit left
    // open in a field is staged before any save of this stick.
    attachSession(rekordboxPath, enginePath, false);
    m_shownFile = filePath;
    if (rekordboxPath != m_rekordboxPath || enginePath != m_enginePath) {
        m_rekordboxPath = rekordboxPath;
        m_enginePath = enginePath;
        invalidate();
        return;
    }
    // A failed read is not retried for every track shown: only on the
    // events that can change it (invalidate()).
    if (m_indexState == IndexState::None) {
        buildIndex();
    }
    publish();
}

void TrackEditController::noteDroppedComment(const QString &title)
{
    const QString said = QStringLiteral("The comment typed for \"%1\" was not staged.").arg(title);
    setErrorMessage(m_lastRefusal.isEmpty() ? said : said + QLatin1Char(' ') + m_lastRefusal);
}

bool TrackEditController::answer(const QString &filePath, TrackRows &rows, CommentLimit &limit, QString &why) const
{
    if (m_indexState != IndexState::Ready) {
        why = m_indexState == IndexState::Failed ? m_indexError : QStringLiteral("Seabass is still reading the stick.");
        return false;
    }
    rows = m_index.rowsFor(filePath.toStdString());
    limit = m_index.limitFor(rows);
    return true;
}

bool TrackEditController::attachSession(const QString &rekordboxPath, const QString &enginePath, bool reportMissing)
{
    auto *registry = EditSessionRegistry::instance();
    const QString any = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    LibraryEditSession *session = any.isEmpty() ? nullptr : registry->sessionFor(registry->libraryIdForPath(any));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::aboutToSave, this, [this]() {
                emit saveStarting();
                // A read running now overlaps the save: it is not used.
                if (m_indexWatcher.isRunning()) {
                    ++m_generation;
                }
            });
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                const auto it = m_staged.find(changeId);
                if (it == m_staged.end()) {
                    return;
                }
                const TrackFieldValue value = it->second;
                const QString file = m_files[changeId];
                m_staged.erase(it);
                m_files.erase(changeId);
                // Off the list either way; shown as saved only when written.
                if (!m_session->lastSkippedIds().contains(changeId)) {
                    emit fieldSaved(file,
                                    value.field == TrackField::Rating ? QStringLiteral("rating")
                                                                      : QStringLiteral("comment"),
                                    value.rating.value_or(-1), QString::fromStdString(value.text));
                    ++m_applied;
                }
                ++m_pendingRevision;
                emit pendingChanged();
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
                const bool wrote = m_applied > 0;
                m_applied = 0;
                // Any save of this stick, this page's or not, Undo Last
                // Save too: the catalogs may have changed.
                invalidate();
                if (wrote) {
                    emit tracksSaved();
                }
            });
            connect(m_session, &LibraryEditSession::stickPresenceChanged, this, [this]() { invalidate(); });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() { forgetAll(); });
            // A change taken back elsewhere (the session's own unstage).
            connect(m_session, &LibraryEditSession::pendingChanged, this, [this]() {
                if (m_session->writing()) {
                    return;
                }
                bool dropped = false;
                for (auto it = m_staged.begin(); it != m_staged.end();) {
                    if (!m_session->hasChange(it->first)) {
                        m_files.erase(it->first);
                        it = m_staged.erase(it);
                        dropped = true;
                    } else {
                        ++it;
                    }
                }
                if (dropped) {
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
        }
    }
    if (!m_session) {
        if (reportMissing) {
            m_lastRefusal = QStringLiteral("This stick has no edit session open.");
            setErrorMessage(m_lastRefusal);
        }
        return false;
    }
    m_session->setLibraryPaths(rekordboxPath, enginePath);
    if (reportMissing && m_session->writing()) {
        m_lastRefusal = QStringLiteral("A save was writing to the stick.");
        setErrorMessage(QStringLiteral("A save is writing to the stick. Try again when it has finished."));
        return false;
    }
    return true;
}

void TrackEditController::forgetAll()
{
    if (!m_staged.empty()) {
        m_staged.clear();
        m_files.clear();
        ++m_pendingRevision;
        emit pendingChanged();
    }
}

void TrackEditController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

bool TrackEditController::stage(const QString &rekordboxPath, const QString &enginePath, const QString &filePath,
                                const QString &title, TrackFieldValue value, const TrackRows &rows)
{
    const QString id = trackFieldChangeId(value.field, filePath.toStdString());
    auto change = std::make_unique<TrackFieldEditChange>(rekordboxPath, enginePath, filePath.toStdString(),
                                                         title.toStdString(), value, rows);
    if (!m_session->stage(std::move(change))) {
        // Refused (another instance holds the stick, or another page is
        // editing it): the session says so itself.
        return false;
    }
    m_staged[id] = std::move(value);
    m_files[id] = filePath;
    ++m_pendingRevision;
    emit pendingChanged();
    return true;
}

void TrackEditController::unstage(TrackField field, const QString &filePath)
{
    const QString id = trackFieldChangeId(field, filePath.toStdString());
    m_files.erase(id);
    if (m_staged.erase(id) == 0) {
        return;
    }
    if (m_session && m_session->hasChange(id)) {
        m_session->unstage(id);
    }
    ++m_pendingRevision;
    emit pendingChanged();
}

bool TrackEditController::setRating(const QString &rekordboxPath, const QString &enginePath, const QString &filePath,
                                    const QString &title, int original, int stars)
{
    setErrorMessage({});
    m_lastRefusal.clear();
    if (stars != -1 && (stars < 1 || stars > 5)) {
        setErrorMessage(QStringLiteral("A rating is 1 to 5 stars."));
        return false;
    }
    TrackRows rows;
    CommentLimit limit;
    QString why;
    if (!answer(filePath, rows, limit, why)) {
        m_lastRefusal = why;
        setErrorMessage(m_indexState == IndexState::Failed ? why : why + QStringLiteral(" Try again in a moment."));
        return false;
    }
    if (const std::string refusal = rows.refusal(); !refusal.empty()) {
        setErrorMessage(QStringLiteral("This rating cannot be changed: %1.").arg(QString::fromStdString(refusal)));
        return false;
    }
    // No catalog tells "no stars" from 0, so they are one value here too.
    const int was = original >= 1 && original <= 5 ? original : -1;
    if (stars == was) {
        unstage(TrackField::Rating, filePath);
        return true;
    }
    if (!attachSession(rekordboxPath, enginePath, true)) {
        return false;
    }
    return stage(rekordboxPath, enginePath, filePath, title,
                 TrackFieldValue{TrackField::Rating, stars == -1 ? std::nullopt : std::optional<int>(stars), {}}, rows);
}

bool TrackEditController::setComment(const QString &rekordboxPath, const QString &enginePath,
                                     const QString &filePath, const QString &title, const QString &original,
                                     const QString &text)
{
    setErrorMessage({});
    m_lastRefusal.clear();
    const QString comment = cleanComment(text);
    if (comment == original) {
        unstage(TrackField::Comment, filePath);
        return true;
    }
    // Never a read here: answered from the stick's index.
    TrackRows rows;
    CommentLimit limit;
    QString why;
    if (!answer(filePath, rows, limit, why)) {
        m_lastRefusal = why;
        setErrorMessage(m_indexState == IndexState::Failed ? why : why + QStringLiteral(" Try again in a moment."));
        return false;
    }
    if (const std::string refusal = rows.refusal(); !refusal.empty()) {
        m_lastRefusal = QStringLiteral("This comment cannot be changed: %1.").arg(QString::fromStdString(refusal));
        setErrorMessage(m_lastRefusal);
        return false;
    }
    if (const std::string refused = commentLimitRefusal(comment.toStdString(), limit); !refused.empty()) {
        m_lastRefusal = QStringLiteral("This comment cannot be saved: %1.").arg(QString::fromStdString(refused));
        setErrorMessage(m_lastRefusal);
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath, true)) {
        return false;
    }
    return stage(rekordboxPath, enginePath, filePath, title,
                 TrackFieldValue{TrackField::Comment, std::nullopt, comment.toStdString()}, rows);
}

bool TrackEditController::hasStagedRating(const QString &filePath) const
{
    return m_staged.count(trackFieldChangeId(TrackField::Rating, filePath.toStdString())) > 0;
}

int TrackEditController::stagedRating(const QString &filePath) const
{
    const auto it = m_staged.find(trackFieldChangeId(TrackField::Rating, filePath.toStdString()));
    return it != m_staged.end() && it->second.rating ? *it->second.rating : -1;
}

bool TrackEditController::hasStagedComment(const QString &filePath) const
{
    return m_staged.count(trackFieldChangeId(TrackField::Comment, filePath.toStdString())) > 0;
}

QString TrackEditController::stagedComment(const QString &filePath) const
{
    const auto it = m_staged.find(trackFieldChangeId(TrackField::Comment, filePath.toStdString()));
    return it != m_staged.end() ? QString::fromStdString(it->second.text) : QString();
}

void TrackEditController::unstageRating(const QString &filePath)
{
    unstage(TrackField::Rating, filePath);
}

void TrackEditController::unstageComment(const QString &filePath)
{
    unstage(TrackField::Comment, filePath);
}

}  // namespace seabass::gui
