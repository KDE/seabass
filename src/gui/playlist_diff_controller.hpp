// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/playlist_diff.hpp"
#include "domain/track.hpp"
#include "gui/async_request.hpp"

namespace seabass::gui
{

// The rows of the side-by-side diff, one per line of the view. A fold
// row stands in for a run of identical lines the view collapses the way
// git folds unchanged context; expandFold() opens it.
class PlaylistDiffRowModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    enum Role {
        KindRole = Qt::UserRole + 1,  // "same", "onlyA", "onlyB", "movedA", "movedB", "mixed", "fold"
        LeftKindRole,                 // "same", "only", "moved" or "" when the left half is blank
        LeftPositionRole,             // 1-based position in A, 0 when blank
        LeftTitleRole,
        LeftArtistRole,
        LeftBpmRole,
        LeftKeyRole,
        LeftDurationSecondsRole,
        LeftPartnerPositionRole,      // moved: 1-based position of the same track in B
        RightKindRole,
        RightPositionRole,
        RightTitleRole,
        RightArtistRole,
        RightBpmRole,
        RightKeyRole,
        RightDurationSecondsRole,
        RightPartnerPositionRole,
        PartnerRowRole,               // moved: the row holding the other half, -1 if folded away or absent
        FoldCountRole,                // fold: how many identical lines it stands for
    };

    struct Row
    {
        QString kind;
        QString leftKind;
        int leftPosition = 0;
        QString leftTitle;
        QString leftArtist;
        double leftBpm = 0.0;
        QString leftKey;
        double leftDurationSeconds = 0.0;
        int leftPartnerPosition = 0;
        QString rightKind;
        int rightPosition = 0;
        QString rightTitle;
        QString rightArtist;
        double rightBpm = 0.0;
        QString rightKey;
        double rightDurationSeconds = 0.0;
        int rightPartnerPosition = 0;
        int partnerRow = -1;
        int foldCount = 0;
        int foldStart = -1;  // fold: index into the unfolded rows
    };

    explicit PlaylistDiffRowModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(std::vector<Row> rows);
    const std::vector<Row> &rows() const { return m_rows; }

private:
    std::vector<Row> m_rows;
};

// Compare Playlists: two playlists of one catalog side by side, git style.
// Read-only, like Browse Library: it reads the catalog through
// LibraryCatalogCache and writes nothing.
class PlaylistDiffController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The catalog's playlists, sorted by name, with how many entries each has.
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY playlistsChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY playlistsChanged)
    Q_PROPERTY(QString playlistA READ playlistA WRITE setPlaylistA NOTIFY selectionChanged)
    Q_PROPERTY(QString playlistB READ playlistB WRITE setPlaylistB NOTIFY selectionChanged)
    // Collapse runs of identical lines, two lines of context kept on each side.
    Q_PROPERTY(bool foldIdentical READ foldIdentical WRITE setFoldIdentical NOTIFY foldIdenticalChanged)
    Q_PROPERTY(PlaylistDiffRowModel *rows READ rows CONSTANT)
    Q_PROPERTY(int onlyACount READ onlyACount NOTIFY diffChanged)
    Q_PROPERTY(int onlyBCount READ onlyBCount NOTIFY diffChanged)
    Q_PROPERTY(int sharedCount READ sharedCount NOTIFY diffChanged)
    Q_PROPERTY(int movedCount READ movedCount NOTIFY diffChanged)
    Q_PROPERTY(int entriesA READ entriesA NOTIFY diffChanged)
    Q_PROPERTY(int entriesB READ entriesB NOTIFY diffChanged)
    // One sentence on how B stands to A.
    Q_PROPERTY(QString verdict READ verdict NOTIFY diffChanged)
    // The other playlists that share a track with A, most alike first:
    // [{name, count, shared, onlyA, onlyB, relation, glyph, detail}].
    // relation is "identical", "superset", "subset" or "overlap".
    Q_PROPERTY(QVariantList relatives READ relatives NOTIFY diffChanged)

public:
    explicit PlaylistDiffController(QObject *parent = nullptr);

    // Reading the catalog, or diffing two long playlists on a worker.
    bool busy() const { return m_scan.busy() || m_diffJob.busy(); }
    QString errorMessage() const { return m_errorMessage; }
    QStringList playlistNames() const { return m_names; }
    QVariantMap playlistTrackCounts() const;
    QString playlistA() const { return m_playlistA; }
    QString playlistB() const { return m_playlistB; }
    void setPlaylistA(const QString &name);
    void setPlaylistB(const QString &name);
    bool foldIdentical() const { return m_foldIdentical; }
    void setFoldIdentical(bool fold);
    PlaylistDiffRowModel *rows() { return &m_rows; }
    int onlyACount() const { return m_diff.onlyA; }
    int onlyBCount() const { return m_diff.onlyB; }
    int sharedCount() const { return m_diff.shared; }
    int movedCount() const { return m_diff.moved; }
    int entriesA() const { return static_cast<int>(entriesOf(m_playlistA).size()); }
    int entriesB() const { return static_cast<int>(entriesOf(m_playlistB).size()); }
    QString verdict() const { return m_verdict; }
    QVariantList relatives() const { return m_relatives; }

    // format: "rekordbox", "engine" or "onelibrary"; path: that catalog's
    // path, the same the other read-only pages take.
    Q_INVOKABLE void scan(const QString &format, const QString &path);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void swapPlaylists();
    // Opens the fold row at `row` (its identical lines take its place).
    Q_INVOKABLE void expandFold(int row);
    // "Artist - Title" per line, for the clipboard.
    Q_INVOKABLE QString onlyInAText() const;
    Q_INVOKABLE QString onlyInBText() const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    // The catalog's tracks as a finished scan would hand them over, for a
    // test that needs playlists the fixture does not have.
    void setTracksForTesting(std::vector<domain::Track> tracks);

signals:
    void busyChanged();
    void errorMessageChanged();
    void playlistsChanged();
    void selectionChanged();
    void foldIdenticalChanged();
    void diffChanged();
    void scanCancelled();

private:
    struct ScanResult
    {
        std::vector<domain::Track> tracks;
        QString errorMessage;
        bool cancelled = false;
    };
    // The entries of one playlist: indices into m_tracks, in playlist order.
    using Entries = std::vector<int>;

    void onScanFinished(ScanResult &&result);
    void setTracks(std::vector<domain::Track> tracks);
    void clearCatalog();
    void setErrorMessage(const QString &message);
    void indexPlaylists();
    void chooseDefaults();
    const Entries &entriesOf(const QString &name) const;
    std::vector<std::string> idsOf(const Entries &entries) const;
    void recompute();
    void applyDiff(domain::PlaylistDiff diff);
    void rebuildRows();
    QString onlyText(bool sideA) const;

    QString m_errorMessage;
    std::vector<domain::Track> m_tracks;
    std::map<QString, Entries> m_playlists;
    QStringList m_names;
    QString m_playlistA;
    QString m_playlistB;
    bool m_foldIdentical = true;
    domain::PlaylistDiff m_diff;
    std::vector<int> m_openFolds;  // unfolded-row indices of the folds opened by hand
    QString m_verdict;
    QVariantList m_relatives;
    PlaylistDiffRowModel m_rows;
    quint64 m_diffGeneration = 0;  // the key of each worker diff, so every one supersedes the last

    // Last, so it is destroyed first. See docs/async-requests.md.
    AsyncRequest<ScanResult> m_scan{this, [this]() { emit busyChanged(); }};
    AsyncRequest<domain::PlaylistDiff> m_diffJob{this, [this]() { emit busyChanged(); }};
};

}  // namespace seabass::gui
