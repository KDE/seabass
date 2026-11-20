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
// the stick that lists the file, backed up first and undoable.
//
// Where each catalog lists a track, and how long a comment export.pdb's
// row can hold, come from one index of the stick (StickPathIndex): every
// catalog read once, off the GUI thread, and every track shown answered
// from it. The index is read again after any save of the stick's session
// (whoever staged it, Undo Last Save included), when the page reads the
// stick again (invalidate()), when the stick's paths change and when the
// stick goes or comes back; never while a save is writing, and a read
// that failed is not kept. Until it is there, nothing can be edited and
// the pane says the stick is being read. Nothing in this class reads the
// stick on the GUI thread.
//
// Staging a field's original value again takes its staged change back
// rather than leaving one that writes what is already there.
class TrackEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The track shown (lookUpTrack), as {filePath, known, reading, failed,
    // editable, refusal, limited, units, asciiOnly, reason}: `known` once
    // the index answers, `reading` while it is being built, `failed` when
    // the read failed (`refusal` says why), `editable` false when the
    // track cannot be edited (`refusal` says why), the rest its
    // CommentLimit.
    Q_PROPERTY(QVariantMap commentLimit READ commentLimit NOTIFY commentLimitChanged)
    // Test seam: while true, a finished read of the stick is held back and
    // published only when this goes false again, so a test can look at
    // the pane in its "reading" state.
    Q_PROPERTY(bool holdIndexForTesting READ holdIndexForTesting WRITE setHoldIndexForTesting NOTIFY
                   holdIndexForTestingChanged)

public:
    explicit TrackEditController(QObject *parent = nullptr);
    ~TrackEditController() override;

    int pendingRevision() const { return m_pendingRevision; }
    QString errorMessage() const { return m_errorMessage; }
    QVariantMap commentLimit() const { return m_commentLimit; }
    bool holdIndexForTesting() const { return m_hold; }
    void setHoldIndexForTesting(bool hold);

    // The track the pane shows: answered from the stick's index, read
    // first when there is none. Clears errorMessage: it was about the
    // track shown before.
    Q_INVOKABLE void lookUpTrack(const QString &rekordboxPath, const QString &enginePath, const QString &filePath);
    // The stick changed under the index (the page read it again): read it
    // again.
    Q_INVOKABLE void invalidate();
    // What the index says about `filePath`, in commentLimit's shape.
    Q_INVOKABLE QVariantMap trackInfo(const QString &filePath) const;
    // A comment typed for `title` that could not be staged when the pane
    // turned to another track: errorMessage says so, with `why`.
    Q_INVOKABLE void noteDroppedComment(const QString &title, const QString &why);
    Q_INVOKABLE void clearError() { setErrorMessage({}); }

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
    void holdIndexForTestingChanged();
    // A save wrote at least one of these edits: the page should rescan.
    void tracksSaved();
    // One edit is on the stick now: `field` "rating" (stars, -1 for none)
    // or "comment" (text), for a pane showing the track.
    void fieldSaved(const QString &filePath, const QString &field, int stars, const QString &text);
    // The stick's session is about to save: an edit still open in a field
    // is to be staged now (LibraryEditSession::aboutToSave).
    void saveStarting();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath, bool reportMissing);
    bool stage(const QString &rekordboxPath, const QString &enginePath, const QString &filePath, const QString &title,
               TrackFieldValue value, const TrackRows &rows);
    void unstage(TrackField field, const QString &filePath);
    void setErrorMessage(const QString &message);
    void forgetAll();
    // Starts reading the stick's index, unless one is being read or a
    // save is writing (then it starts when the save is done).
    void buildIndex();
    void publish();
    // The rows and limit for `filePath`, when the index is there; else the
    // sentence saying why an edit cannot be staged yet.
    bool answer(const QString &filePath, TrackRows &rows, CommentLimit &limit, QString &why) const;

    QPointer<LibraryEditSession> m_session;
    // By change id: what is staged, for the queries, and for which file.
    std::map<QString, TrackFieldValue> m_staged;
    std::map<QString, QString> m_files;
    // The changes applied by the running save.
    int m_applied = 0;
    int m_pendingRevision = 0;
    QString m_errorMessage;
    QVariantMap m_commentLimit;

    enum class IndexState
    {
        None,
        Building,
        Ready,
        Failed,
    };
    struct IndexRead
    {
        StickPathIndex index;
        std::string error;
        int generation = 0;
    };
    IndexState m_indexState = IndexState::None;
    StickPathIndex m_index;
    QString m_indexError;
    QString m_rekordboxPath;
    QString m_enginePath;
    QString m_shownFile;
    // Bumped by invalidate(): a read started before it is not used.
    int m_generation = 0;
    bool m_waitingForSave = false;
    QFutureWatcher<IndexRead> m_indexWatcher;
    bool m_hold = false;
    std::optional<IndexRead> m_held;
    void takeRead(IndexRead read);
};

}  // namespace seabass::gui
