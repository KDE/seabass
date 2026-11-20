// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_edit_controller.hpp"

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

QVariantMap infoMap(const QString &filePath, const TrackLookup *lookup)
{
    QVariantMap map;
    map.insert(QStringLiteral("filePath"), filePath);
    map.insert(QStringLiteral("known"), lookup != nullptr);
    map.insert(QStringLiteral("editable"), lookup != nullptr && lookup->error.empty());
    map.insert(QStringLiteral("refusal"), lookup ? QString::fromStdString(lookup->error) : QString());
    map.insert(QStringLiteral("limited"), lookup && lookup->limit.limited);
    map.insert(QStringLiteral("units"), lookup ? static_cast<int>(lookup->limit.units) : 0);
    map.insert(QStringLiteral("asciiOnly"), lookup && lookup->limit.asciiOnly);
    map.insert(QStringLiteral("reason"), lookup ? QString::fromStdString(lookup->limit.reason) : QString());
    return map;
}

}  // namespace

TrackEditController::TrackEditController(QObject *parent) : QObject(parent)
{
    m_commentLimit = infoMap({}, nullptr);
    connect(&m_lookupWatcher, &QFutureWatcher<TrackLookup>::finished, this, [this]() {
        QString error;
        TrackLookup lookup = takeResult(m_lookupWatcher, &error);
        if (!error.isEmpty()) {
            lookup.error = "the stick's catalogs could not be read: " + error.toStdString();
        }
        const QString file = QString::fromStdString(lookup.filePath);
        if (!file.isEmpty()) {
            m_lookups[file] = std::move(lookup);
        }
        if (m_nextLookup) {
            const Ask next = *m_nextLookup;
            m_nextLookup.reset();
            lookUpTrack(next.rekordboxPath, next.enginePath, next.filePath);
            return;
        }
        if (file == m_shownFile) {
            m_commentLimit = trackInfo(file);
            emit commentLimitChanged();
        }
    });
}

TrackEditController::~TrackEditController()
{
    awaitQuietly(m_lookupWatcher);
}

QString TrackEditController::cleanComment(const QString &text)
{
    QString cleaned = text;
    cleaned.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    return cleaned.trimmed();
}

const TrackLookup *TrackEditController::lookupFor(const QString &filePath) const
{
    const auto it = m_lookups.find(filePath);
    return it == m_lookups.end() ? nullptr : &it->second;
}

QVariantMap TrackEditController::trackInfo(const QString &filePath) const
{
    return infoMap(filePath, lookupFor(filePath));
}

void TrackEditController::lookUpTrack(const QString &rekordboxPath, const QString &enginePath, const QString &filePath)
{
    // The session is watched from the first track shown, so an edit left
    // open in a field is staged before any save of this stick.
    attachSession(rekordboxPath, enginePath, false);
    m_shownFile = filePath;
    m_commentLimit = trackInfo(filePath);
    emit commentLimitChanged();
    if (m_lookupWatcher.isRunning()) {
        m_nextLookup = Ask{rekordboxPath, enginePath, filePath};
        return;
    }
    const std::string file = filePath.toStdString();
    m_lookupWatcher.setFuture(QtConcurrent::run(
        [rekordboxPath, enginePath, file]() { return seabass::gui::lookUpTrack(rekordboxPath, enginePath, file); }));
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
            connect(m_session, &LibraryEditSession::aboutToSave, this, &TrackEditController::saveStarting);
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
                if (wrote) {
                    // The catalogs changed: what was looked up is stale.
                    m_lookups.clear();
                    emit tracksSaved();
                }
            });
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
            setErrorMessage(QStringLiteral("This stick has no edit session open."));
        }
        return false;
    }
    m_session->setLibraryPaths(rekordboxPath, enginePath);
    if (reportMissing && m_session->writing()) {
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
    if (stars != -1 && (stars < 1 || stars > 5)) {
        setErrorMessage(QStringLiteral("A rating is 1 to 5 stars."));
        return false;
    }
    const TrackLookup *lookup = lookupFor(filePath);
    if (lookup == nullptr) {
        setErrorMessage(QStringLiteral("Seabass is still reading where the stick keeps this track. Try again in a "
                                       "moment."));
        return false;
    }
    if (!lookup->error.empty()) {
        setErrorMessage(QStringLiteral("This rating cannot be changed: %1.").arg(QString::fromStdString(lookup->error)));
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
                 TrackFieldValue{TrackField::Rating, stars == -1 ? std::nullopt : std::optional<int>(stars), {}},
                 lookup->rows);
}

bool TrackEditController::setComment(const QString &rekordboxPath, const QString &enginePath,
                                     const QString &filePath, const QString &title, const QString &original,
                                     const QString &text)
{
    setErrorMessage({});
    const QString comment = cleanComment(text);
    if (comment == original) {
        unstage(TrackField::Comment, filePath);
        return true;
    }
    // Never a read here: the lookup made when the track was shown is what
    // the edit is checked against and staged with.
    const TrackLookup *lookup = lookupFor(filePath);
    if (lookup == nullptr) {
        setErrorMessage(QStringLiteral("Seabass is still reading where the stick keeps this track. Try again in a "
                                       "moment."));
        return false;
    }
    if (!lookup->error.empty()) {
        setErrorMessage(QStringLiteral("This comment cannot be changed: %1.").arg(QString::fromStdString(lookup->error)));
        return false;
    }
    if (const std::string why = commentLimitRefusal(comment.toStdString(), lookup->limit); !why.empty()) {
        setErrorMessage(QStringLiteral("This comment cannot be saved: %1.").arg(QString::fromStdString(why)));
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath, true)) {
        return false;
    }
    return stage(rekordboxPath, enginePath, filePath, title,
                 TrackFieldValue{TrackField::Comment, std::nullopt, comment.toStdString()}, lookup->rows);
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
