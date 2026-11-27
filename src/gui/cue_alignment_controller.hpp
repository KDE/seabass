// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "domain/cue_beat_alignment.hpp"
#include "domain/track.hpp"
#include "gui/async_request.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// One track with a cue point off its beat, as the scan measured it: the
// library's row with its cues as read, the cues on the timeline of that
// library's own grid, and every one of them measured against it
// (domain::measureCuesAgainstBeats). What is listed of it depends on the
// tolerance, which is applied afterwards.
struct MeasuredTrack
{
    QString key;   // format + row id (junkKeyFor)
    QString path;  // the catalog folder the row is written under
    domain::Track track;
    // What the reader added to the row's cues to put them on rekordbox's
    // timeline (an Engine row of an MP3); the grid is on the library's
    // own, so the cues were measured with this taken off.
    double gridShiftMs = 0.0;
    domain::CueBeatAlignment alignment;
    // An Engine row of an MP3 whose cue starts that are off their beat are
    // all off by what the file's audio sits apart in rekordbox and in
    // Engine (its lead-in, read from the file whatever the correction in
    // Preferences says, to 2.5 ms): the row holds the other library's
    // number, or its grid does. Which side is wrong is Sync Cue Points'
    // question (its cue offset repair), not a matter of the nearest beat.
    // Also set where the file's lead-in cannot be read: such a row cannot
    // be told from one of those.
    bool offByLibraryOffset = false;
    // Listed, never ticked or staged.
    bool held() const { return alignment.sameOffset || offByLibraryOffset; }
};

struct CueAlignmentScan
{
    std::vector<MeasuredTrack> tracks;
    // Tracks with cue points, and those of them with a grid to measure
    // against, over every library read.
    int tracksWithCues = 0;
    int tracksMeasured = 0;
    QStringList formats;
};

// The tracks listed at the tolerance set: those with a cue point more
// than a millisecond and at most the tolerance from its beat.
class CueAlignmentListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        KeyRole = Qt::UserRole + 1,
        FormatRole,
        TitleRole,
        ArtistRole,
        // How many of its cue points are listed, and the furthest of them
        // from its beat, in whole milliseconds.
        OffCountRole,
        LargestOffMsRole,
        // "Hot cue C 8 ms late, memory cue 12 ms early", for the row.
        DetailRole,
        // Every cue off by one amount: the grid is more likely shifted
        // than the cues misplaced. Or off by the file's own offset
        // between rekordbox and Engine (MeasuredTrack::
        // offByLibraryOffset, said by LibraryOffsetRole). Listed, never
        // ticked or staged.
        SameOffsetRole,
        LibraryOffsetRole,
        TickedRole,
        StagedRole,
    };
    struct Row
    {
        std::size_t track = 0;  // into the scan's tracks
        int offCount = 0;
        int largestOffMs = 0;
        QString detail;
        bool ticked = false;
        bool staged = false;
    };

    explicit CueAlignmentListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_rows.size()); }
    // A row by its role names; empty past the end.
    Q_INVOKABLE QVariantMap get(int row) const;

    void reset(const std::shared_ptr<const CueAlignmentScan> &scan, std::vector<Row> rows);
    const std::vector<Row> &rows() const { return m_rows; }
    const MeasuredTrack &trackOf(const Row &row) const { return m_scan->tracks[row.track]; }
    void setTicked(int row, bool ticked);
    void setStaged(int row, bool staged);

signals:
    void countChanged();

private:
    std::shared_ptr<const CueAlignmentScan> m_scan;
    std::vector<Row> m_rows;
};

// Maintain's "Align Cue Points": every library on the stick read, each
// track's cue points measured against the beat grid of the SAME library
// (never across libraries: an MP3 sits 26 or 52 ms apart in rekordbox and
// in Engine), and the tracks listed that have a cue point a little off its
// beat. "A little" is the tolerance, set with the page's slider: a cue
// further off than that was put there on purpose and is neither listed nor
// moved. Ticked tracks are staged as AlignCuesChange, one a track; nothing
// is written before the page's Save.
class CueAlignmentController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(seabass::gui::CueAlignmentListModel *tracks READ tracks CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    // A scan has finished since the page opened (or since the last save).
    Q_PROPERTY(bool checked READ checked NOTIFY scanChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // How far from its beat a cue point may be and still be listed, in
    // milliseconds, 2 to maximumToleranceMs. Kept in the settings.
    Q_PROPERTY(int toleranceMs READ toleranceMs WRITE setToleranceMs NOTIFY toleranceChanged)
    Q_PROPERTY(int maximumToleranceMs READ maximumToleranceMs CONSTANT)
    // Over every library read: tracks with cue points, and those of them
    // with a beat grid to measure against.
    Q_PROPERTY(int tracksWithCues READ tracksWithCues NOTIFY scanChanged)
    Q_PROPERTY(int tracksMeasured READ tracksMeasured NOTIFY scanChanged)
    // Of the tracks listed: the cue points listed, the tracks that can be
    // ticked (not the ones whose cues are all off by one amount), and the
    // ticked and staged ones.
    Q_PROPERTY(int cuePointCount READ cuePointCount NOTIFY listChanged)
    Q_PROPERTY(int tickableCount READ tickableCount NOTIFY listChanged)
    Q_PROPERTY(int tickedCount READ tickedCount NOTIFY listChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY listChanged)

public:
    explicit CueAlignmentController(QObject *parent = nullptr);
    ~CueAlignmentController() override;

    static constexpr int DefaultToleranceMs = 20;
    static constexpr int MinimumToleranceMs = 2;
    static constexpr int MaximumToleranceMs = 100;

    CueAlignmentListModel *tracks() { return &m_model; }
    bool busy() const { return m_scan.busy(); }
    bool writing() const;
    bool checked() const { return m_result != nullptr; }
    QString errorMessage() const { return m_errorMessage; }
    int toleranceMs() const { return m_toleranceMs; }
    void setToleranceMs(int ms);
    int maximumToleranceMs() const { return MaximumToleranceMs; }
    int tracksWithCues() const { return m_result ? m_result->tracksWithCues : 0; }
    int tracksMeasured() const { return m_result ? m_result->tracksMeasured : 0; }
    int cuePointCount() const;
    int tickableCount() const;
    int tickedCount() const;
    int stagedCount() const;

    // rekordboxPath is the stick's PIONEER folder and enginePath its
    // Engine Library folder; either may be empty.
    Q_INVOKABLE void scan(const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void setTicked(int row, bool ticked);
    // Every track that can be ticked, or none.
    Q_INVOKABLE void tickAll(bool ticked);
    // Stages the ticked tracks, each as the list shows it at the
    // tolerance set; takes back what this page staged.
    Q_INVOKABLE void stage();
    Q_INVOKABLE void unstage();

signals:
    void busyChanged();
    void writingChanged();
    void scanChanged();
    void listChanged();
    void toleranceChanged();
    void errorMessageChanged();

private:
    void attachSession();
    void setErrorMessage(const QString &message);
    // The rows the scan gives at the tolerance set, ticks and staged marks
    // kept for the tracks still listed.
    void relist();

    QString m_rekordboxPath;
    QString m_enginePath;
    QString m_errorMessage;
    int m_toleranceMs = DefaultToleranceMs;
    std::shared_ptr<const CueAlignmentScan> m_result;
    CueAlignmentListModel m_model;
    QSet<QString> m_ticked;  // keys
    QSet<QString> m_staged;  // keys
    QPointer<LibraryEditSession> m_session;
    bool m_rescanAfterSave = false;
    AsyncRequest<std::shared_ptr<const CueAlignmentScan>> m_scan{this, [this] { emit busyChanged(); }};
};

// The scan itself, off the GUI thread: every library present, read at the
// catalog cache's cue stage, each track's grid read from its own library.
// Free, so a test can run it on a stick copy without a controller.
std::shared_ptr<const CueAlignmentScan> scanCueAlignment(const QString &rekordboxPath, const QString &enginePath,
                                                         application::CancellationToken cancel);

}  // namespace seabass::gui
