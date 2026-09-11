// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <map>

namespace seabass::gui
{

class LibraryEditSession;

// The one genuinely new cue-writing feature in this codebase -- every
// other write path here only ever merges or copies cues that already
// exist somewhere (Clean Up, Sync, Local Cue Backup).
//
// Staged, not written: addCue() stages one PendingChange per cue in the
// library's LibraryEditSession (the first one takes the edit lock and
// makes Browse's floating Save appear); Save writes them. The change
// re-scans the library fresh right before writing (never trusts whatever
// cue list the calling page had cached) so the augmented cue list handed
// to the existing per-format CueWriter always starts from the track's
// real current state -- same "never trust stale data before a mutating
// write" stance every other write path here takes.
//
// Position-only for now: no beatgrid-snap option yet (see
// docs/onelibrary-format.md-style reasoning -- rekordbox's beatgrid lives
// in a section of the ANLZ file this project deliberately treats as
// opaque bytes; Engine's is available via libdjinterop but unused here
// too). A later pass can add snapping without touching the write path at
// all, only how the position it's given gets computed.
//
// Handles all three catalogs as primary write targets, including
// OneLibrary -- but OneLibrary goes through OneLibraryCueWriter directly
// rather than the application::CueWriter dispatch rekordbox/Engine share,
// since that writer deliberately isn't a CueWriter (it keys by file path,
// not sourceId -- content_id is a separate id space from export.pdb's
// track id). See the .cpp for the branch.
//
// Hot loops (isLoop) are Engine-only: LibdjinteropEngineCueWriter writes
// them through libdjinterop's own tested loop API, but RekordboxCueWriter
// can't -- AnlzCueCodec's own doc comment marks loop encoding out of
// scope pending real hardware verification of the still-uncertain raw
// byte fields (see anlz_cue_codec.hpp). Refused outright there rather
// than silently written as a plain point cue, which would quietly
// discard the loop-out a DJ asked to save.
class AddCueController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Staging is instant; busy is kept for the panel's bindings and is
    // simply never true now.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Mirrors the session: true while a save is writing to the stick.
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    // Bumps whenever the staged cues change, so QML re-reads pendingCuesFor().
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)

public:
    explicit AddCueController(QObject *parent = nullptr);

    bool busy() const { return false; }
    bool writing() const;
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    int pendingRevision() const { return m_pendingRevision; }

    // kind is "hot" or "memory"; hotCueNumber is ignored for "memory".
    // color is "#RRGGBB" or empty (writer picks its own default). isLoop
    // is only honored for format == "engine" and kind == "hot" -- see
    // the .cpp for why rekordbox and OneLibrary loop writes are refused
    // rather than silently downgraded to a point cue. trackTitle is only
    // for the pending-change description.
    Q_INVOKABLE void addCue(const QString &format, const QString &path, const QString &sourceId, double positionMs,
                             const QString &kind, int hotCueNumber, const QString &color, const QString &comment,
                             bool isLoop, double loopEndMs, const QString &trackTitle = QString());

    // Staged cues for one track, each {changeId, kind, hotCueNumber,
    // positionMs, isLoop, loopEndMs, color, comment}.
    Q_INVOKABLE QVariantList pendingCuesFor(const QString &sourceId) const;
    Q_INVOKABLE void unstage(const QString &changeId);

signals:
    void busyChanged();
    void writingChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void pendingChanged();
    // At least one staged cue reached the stick: the page should rescan.
    void cuesSaved();

private:
    void attachSession(const QString &format, const QString &path);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);

    QPointer<LibraryEditSession> m_session;
    std::map<QString, QVariantMap> m_pending;  // changeId -> cue summary (with "sourceId")
    int m_pendingRevision = 0;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
