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

// Compare Playlists: two playlists side by side, git style, each side
// from any catalog the stick has (DeviceLibrary, OneLibrary, Engine), so
// the copy one catalog holds can be held against the other's. Read-only,
// like Browse Library: it reads the catalogs through LibraryCatalogCache
// and writes nothing.
//
// What makes two entries "the same track": within one catalog, the row
// (its sourceId), as it always was. Across two catalogs, the pairing every
// other page uses, domain::matchTracks within one stick, resolved once per
// scan into one identity per track shared by the catalogs; a track the
// other catalog has no row for stays its own.
class PlaylistDiffController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The catalogs the last scan read: "rekordbox", "onelibrary", "engine".
    Q_PROPERTY(QStringList formats READ formats NOTIFY playlistsChanged)
    // The catalog each side reads its playlist from.
    Q_PROPERTY(QString formatA READ formatA WRITE setFormatA NOTIFY selectionChanged)
    Q_PROPERTY(QString formatB READ formatB WRITE setFormatB NOTIFY selectionChanged)
    // The catalog's name as the page says it ("Engine", "DeviceLibrary").
    Q_PROPERTY(QString catalogLabelA READ catalogLabelA NOTIFY selectionChanged)
    Q_PROPERTY(QString catalogLabelB READ catalogLabelB NOTIFY selectionChanged)
    // Each side's catalog's playlists, sorted by name, with how many
    // entries each has.
    Q_PROPERTY(QStringList playlistNamesA READ playlistNamesA NOTIFY playlistsChanged)
    Q_PROPERTY(QStringList playlistNamesB READ playlistNamesB NOTIFY playlistsChanged)
    Q_PROPERTY(QVariantMap playlistTrackCountsA READ playlistTrackCountsA NOTIFY playlistsChanged)
    Q_PROPERTY(QVariantMap playlistTrackCountsB READ playlistTrackCountsB NOTIFY playlistsChanged)
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
    // The other playlists, of every catalog read, that share a track with
    // A, most alike first: [{name, format, catalog, label, count, shared,
    // onlyA, onlyB, relation, glyph, detail}]. label is the name, with
    // its catalog in front when that is not A's ("Engine: Spacy Techno").
    // relation is "identical", "superset", "subset" or "overlap".
    Q_PROPERTY(QVariantList relatives READ relatives NOTIFY diffChanged)

public:
    explicit PlaylistDiffController(QObject *parent = nullptr);

    // Reading the catalogs, or diffing two long playlists on a worker.
    bool busy() const { return m_scan.busy() || m_diffJob.busy(); }
    QString errorMessage() const { return m_errorMessage; }
    QStringList formats() const;
    QString formatA() const { return m_formatA; }
    QString formatB() const { return m_formatB; }
    void setFormatA(const QString &format);
    void setFormatB(const QString &format);
    QString catalogLabelA() const { return catalogLabel(m_formatA); }
    QString catalogLabelB() const { return catalogLabel(m_formatB); }
    QStringList playlistNamesA() const { return namesOf(m_formatA); }
    QStringList playlistNamesB() const { return namesOf(m_formatB); }
    QVariantMap playlistTrackCountsA() const { return countsOf(m_formatA); }
    QVariantMap playlistTrackCountsB() const { return countsOf(m_formatB); }
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
    int entriesA() const { return static_cast<int>(entriesOf(m_formatA, m_playlistA).size()); }
    int entriesB() const { return static_cast<int>(entriesOf(m_formatB, m_playlistB).size()); }
    QString verdict() const { return m_verdict; }
    QVariantList relatives() const { return m_relatives; }

    // The catalog's name as the page says it, for "rekordbox", "onelibrary"
    // and "engine".
    static QString catalogLabel(const QString &format);

    // Reads every catalog the stick has: DeviceLibrary and, when the stick
    // has one, OneLibrary under rekordboxPath (the PIONEER folder), Engine
    // under enginePath. Either path may be empty.
    Q_INVOKABLE void scan(const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void cancelScan();
    // Whether a OneLibrary catalog sits beside the DeviceLibrary one.
    Q_INVOKABLE bool hasOneLibrary(const QString &pioneerRoot) const;
    // Swaps the two sides, catalogs and playlists.
    Q_INVOKABLE void swapPlaylists();
    // B is `name` of catalog `format` (a relative's chip), in one step.
    Q_INVOKABLE void chooseB(const QString &format, const QString &name);
    // Opens the fold row at `row` (its identical lines take its place).
    Q_INVOKABLE void expandFold(int row);
    // "Artist - Title" per line, for the clipboard.
    Q_INVOKABLE QString onlyInAText() const;
    Q_INVOKABLE QString onlyInBText() const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    // One catalog's tracks as a finished scan would hand them over, for a
    // test that needs playlists the fixture does not have.
    void setTracksForTesting(std::vector<domain::Track> tracks, const QString &format = QStringLiteral("engine"));

signals:
    void busyChanged();
    void errorMessageChanged();
    void playlistsChanged();
    void selectionChanged();
    void foldIdenticalChanged();
    void diffChanged();
    void scanCancelled();

private:
    // The entries of one playlist: indices into its catalog's tracks, in
    // playlist order.
    using Entries = std::vector<int>;
    struct Catalog
    {
        std::vector<domain::Track> tracks;
        // Per track: the identity it shares with its rows in the other
        // catalogs, the same string wherever matchTracks paired them.
        std::vector<std::string> identities;
        std::map<QString, Entries> playlists;
        QStringList names;
    };
    struct ScanResult
    {
        std::map<QString, Catalog> catalogs;
        QStringList failures;
        bool cancelled = false;
    };

    static void resolveIdentities(std::map<QString, Catalog> &catalogs);
    static void indexPlaylists(Catalog &catalog);
    void onScanFinished(ScanResult &&result);
    void setCatalogs(std::map<QString, Catalog> catalogs);
    void clearCatalogs();
    void setErrorMessage(const QString &message);
    // A side's catalog or playlist that is not there falls back; with
    // `avoidSamePair`, B equal to A falls back too (the pair a scan opens on).
    void chooseDefaults(bool avoidSamePair);
    QString nearestRelativeInB() const;
    const Catalog *catalogOf(const QString &format) const;
    QStringList namesOf(const QString &format) const;
    QVariantMap countsOf(const QString &format) const;
    const Entries &entriesOf(const QString &format, const QString &name) const;
    // The keys two entries are compared by: the row within one catalog,
    // the shared identity across two.
    std::vector<std::string> idsOf(const QString &format, const Entries &entries, bool acrossCatalogs) const;
    const domain::Track &trackOf(const QString &format, int index) const;
    // A side's playlist as a sentence names it: with its catalog when the
    // two sides are different catalogs.
    QString sideName(bool sideA) const;
    void recompute();
    void applyDiff(domain::PlaylistDiff diff);
    void rebuildRows();
    QString onlyText(bool sideA) const;

    QString m_errorMessage;
    std::map<QString, Catalog> m_catalogs;
    QString m_formatA;
    QString m_formatB;
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
