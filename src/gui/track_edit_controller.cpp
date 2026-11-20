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

QVariantMap limitMap(const QString &filePath, const std::optional<CommentLimit> &limit)
{
    QVariantMap map;
    map.insert(QStringLiteral("filePath"), filePath);
    map.insert(QStringLiteral("known"), limit.has_value());
    map.insert(QStringLiteral("limited"), limit && limit->limited);
    map.insert(QStringLiteral("units"), limit ? static_cast<int>(limit->units) : 0);
    map.insert(QStringLiteral("asciiOnly"), limit && limit->asciiOnly);
    map.insert(QStringLiteral("reason"), limit ? QString::fromStdString(limit->reason) : QString());
    return map;
}

}  // namespace

TrackEditController::TrackEditController(QObject *parent) : QObject(parent)
{
    m_commentLimit = limitMap({}, std::nullopt);
    connect(&m_limitWatcher, &QFutureWatcher<CommentLimit>::finished, this, [this]() {
        const QString path = m_runningLookup;
        QString error;
        CommentLimit limit = takeResult(m_limitWatcher, &error);
        if (!error.isEmpty()) {
            // Not known is not "no limit": nothing longer than is there.
            limit.limited = true;
            limit.units = 0;
            limit.reason = "The comment's room in export.pdb could not be read: " + error.toStdString();
        }
        m_runningLookup.clear();
        if (m_nextLookup) {
            const auto [rekordboxPath, filePath] = *m_nextLookup;
            m_nextLookup.reset();
            lookUpCommentLimit(rekordboxPath, filePath);
            return;
        }
        m_limit = std::make_pair(path.toStdString(), limit);
        m_commentLimit = limitMap(path, limit);
        emit commentLimitChanged();
    });
}

TrackEditController::~TrackEditController()
{
    awaitQuietly(m_limitWatcher);
}

QString TrackEditController::cleanComment(const QString &text)
{
    QString cleaned = text;
    cleaned.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    return cleaned.trimmed();
}

void TrackEditController::lookUpCommentLimit(const QString &rekordboxPath, const QString &filePath)
{
    if (m_limitWatcher.isRunning()) {
        m_nextLookup = std::make_pair(rekordboxPath, filePath);
        return;
    }
    m_limitRekordboxPath = rekordboxPath;
    m_runningLookup = filePath;
    if (m_commentLimit.value(QStringLiteral("filePath")).toString() != filePath
        || m_commentLimit.value(QStringLiteral("known")).toBool()) {
        m_commentLimit = limitMap(filePath, std::nullopt);
        emit commentLimitChanged();
    }
    const std::string file = filePath.toStdString();
    m_limitWatcher.setFuture(QtConcurrent::run([rekordboxPath, file]() { return commentLimitFor(rekordboxPath, file); }));
}

bool TrackEditController::attachSession(const QString &rekordboxPath, const QString &enginePath)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session =
        registry->sessionFor(registry->libraryIdForPath(rekordboxPath.isEmpty() ? enginePath : rekordboxPath));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                const auto it = m_staged.find(changeId);
                if (it != m_staged.end()) {
                    const TrackFieldValue value = it->second;
                    const QString file = m_files[changeId];
                    m_staged.erase(it);
                    m_files.erase(changeId);
                    emit fieldSaved(file,
                                    value.field == TrackField::Rating ? QStringLiteral("rating") : QStringLiteral("comment"),
                                    value.rating.value_or(-1), QString::fromStdString(value.text));
                    ++m_applied;
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
                const bool wrote = m_applied > 0;
                m_applied = 0;
                if (wrote) {
                    emit tracksSaved();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() { forgetAll(); });
            // A change taken back elsewhere (the session's own unstage).
            connect(m_session, &LibraryEditSession::pendingChanged, this, [this]() {
                bool dropped = false;
                for (auto it = m_staged.begin(); it != m_staged.end();) {
                    if (!m_session->hasChange(it->first) && !m_session->writing()) {
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
                                const QString &title, TrackFieldValue value)
{
    const QString id = trackFieldChangeId(value.field, filePath.toStdString());
    auto change = std::make_unique<TrackFieldEditChange>(rekordboxPath, enginePath, filePath.toStdString(),
                                                         title.toStdString(), value);
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
    if (filePath.isEmpty()) {
        setErrorMessage(QStringLiteral("This track has no file on the stick, so its rating cannot be changed here."));
        return false;
    }
    if (stars != -1 && (stars < 1 || stars > 5)) {
        setErrorMessage(QStringLiteral("A rating is 1 to 5 stars."));
        return false;
    }
    // No catalog tells "no stars" from 0, so they are one value here too.
    const int was = original >= 1 && original <= 5 ? original : -1;
    if (stars == was) {
        unstage(TrackField::Rating, filePath);
        return true;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    return stage(rekordboxPath, enginePath, filePath, title,
                 TrackFieldValue{TrackField::Rating, stars == -1 ? std::nullopt : std::optional<int>(stars), {}});
}

bool TrackEditController::setComment(const QString &rekordboxPath, const QString &enginePath,
                                     const QString &filePath, const QString &title, const QString &original,
                                     const QString &text)
{
    setErrorMessage({});
    if (filePath.isEmpty()) {
        setErrorMessage(QStringLiteral("This track has no file on the stick, so its comment cannot be changed here."));
        return false;
    }
    const QString comment = cleanComment(text);
    if (comment == original) {
        unstage(TrackField::Comment, filePath);
        return true;
    }
    // The field is held to the limit as it is typed; this is the check
    // behind it, with the limit read again when it is not this file's.
    const std::string file = filePath.toStdString();
    const CommentLimit limit =
        m_limit && m_limit->first == file ? m_limit->second : commentLimitFor(rekordboxPath, file);
    if (const std::string why = commentLimitRefusal(comment.toStdString(), limit); !why.empty()) {
        setErrorMessage(QStringLiteral("This comment cannot be saved: %1.").arg(QString::fromStdString(why)));
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    return stage(rekordboxPath, enginePath, filePath, title,
                 TrackFieldValue{TrackField::Comment, std::nullopt, comment.toStdString()});
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
