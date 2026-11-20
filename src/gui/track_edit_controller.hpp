// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

#include <map>
#include <optional>
#include <string>

#include "gui/edit/changes/track_field_edit_change.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Browse's track details editing a track's rating and comment (#105):
// each edit is staged into the stick's edit session as one
// TrackFieldEditChange, written by the page's Save into every catalog on
// the stick that lists the file, backed up first and undoable. Tracks are
// named by their file, so an edit made while browsing one catalog is the
// same edit in the others.
//
// Staging a field's original value again takes its staged change back
// rather than leaving one that writes what is already there.
class TrackEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The longest comment the track lookUpCommentLimit() was last asked
    // about can take, as {filePath, known, limited, units, asciiOnly,
    // reason}: see CommentLimit. `known` is false until the lookup is done.
    Q_PROPERTY(QVariantMap commentLimit READ commentLimit NOTIFY commentLimitChanged)

public:
    explicit TrackEditController(QObject *parent = nullptr);
    ~TrackEditController() override;

    int pendingRevision() const { return m_pendingRevision; }
    QString errorMessage() const { return m_errorMessage; }
    QVariantMap commentLimit() const { return m_commentLimit; }

    // Reads export.pdb off the GUI thread for the room the file's comment
    // has there; commentLimit says it when done. A later call wins.
    Q_INVOKABLE void lookUpCommentLimit(const QString &rekordboxPath, const QString &filePath);

    // Stars 1 to 5, or -1 for none; `original` is the value the stick has
    // (-1 for none). Returns false, staging nothing, when refused.
    Q_INVOKABLE bool setRating(const QString &rekordboxPath, const QString &enginePath, const QString &filePath,
                               const QString &title, int original, int stars);
    // The comment as typed: line breaks become spaces and the ends are
    // trimmed (export.pdb's readers trim a padded string, so a trailing
    // space would not survive there and the catalogs would disagree).
    // Refused when the file's comment limit does not allow it.
    Q_INVOKABLE bool setComment(const QString &rekordboxPath, const QString &enginePath, const QString &filePath,
                                const QString &title, const QString &original, const QString &text);
    Q_INVOKABLE bool hasStagedRating(const QString &filePath) const;
    // -1 for "no stars".
    Q_INVOKABLE int stagedRating(const QString &filePath) const;
    Q_INVOKABLE bool hasStagedComment(const QString &filePath) const;
    Q_INVOKABLE QString stagedComment(const QString &filePath) const;
    Q_INVOKABLE void unstageRating(const QString &filePath);
    Q_INVOKABLE void unstageComment(const QString &filePath);
    // What setComment() makes of `text`.
    Q_INVOKABLE static QString cleanComment(const QString &text);

signals:
    void pendingChanged();
    void errorMessageChanged();
    void commentLimitChanged();
    // A save wrote at least one of these edits: the page should rescan.
    void tracksSaved();
    // One edit is on the stick now: `field` "rating" (value: stars, -1
    // for none) or "comment" (text), for a pane showing the track.
    void fieldSaved(const QString &filePath, const QString &field, int stars, const QString &text);

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    bool stage(const QString &rekordboxPath, const QString &enginePath, const QString &filePath, const QString &title,
               TrackFieldValue value);
    void unstage(TrackField field, const QString &filePath);
    void setErrorMessage(const QString &message);
    void forgetAll();

    QPointer<LibraryEditSession> m_session;
    // By change id: what is staged, for the queries.
    std::map<QString, TrackFieldValue> m_staged;
    // The file each staged change is for, by change id.
    std::map<QString, QString> m_files;
    // The changes applied by the running save.
    int m_applied = 0;
    int m_pendingRevision = 0;
    QString m_errorMessage;

    QVariantMap m_commentLimit;
    // The last lookup's result, for setComment()'s check.
    std::optional<std::pair<std::string, CommentLimit>> m_limit;
    QString m_limitRekordboxPath;
    QFutureWatcher<CommentLimit> m_limitWatcher;
    // A lookup asked for while one runs: started when that one is done.
    std::optional<std::pair<QString, QString>> m_nextLookup;
    QString m_runningLookup;
};

}  // namespace seabass::gui
