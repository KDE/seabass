// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/copy_beat_grids.hpp"
#include "domain/beat_grid.hpp"
#include "gui/async_request.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// The tracks a copy would write, title and artist, in plan order.
class BeatGridTrackListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles { TitleRole = Qt::UserRole + 1, ArtistRole };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    struct Row
    {
        QString title;
        QString artist;
    };
    void setRows(std::vector<Row> rows);

signals:
    void countChanged();

private:
    std::vector<Row> m_rows;
};

// What one analysis found: Copy Beat Grids planned for this stick and run
// as a preview (application::BeatGridCopyOptions::previewOnly), so every
// count is what a copy would do now, and nothing was written.
struct BeatGridAnalysis
{
    bool cancelled = false;
    QString error;
    // Engine 1.x, whose grids Seabass does not write. Nothing else is
    // counted then.
    bool engineTooOld = false;
    // The ledger on the stick could not be read; what is wrong with it.
    QString ledgerError;
    std::string stickRoot;
    QString enginePath;
    bool overwriteEngineEdits = false;
    int matched = 0;
    application::BeatGridCopyResult preview;
    // For each row in preview.wouldWriteItems, by Engine id: the beats the
    // preview read, and what the ledger said for it.
    std::map<std::string, std::vector<domain::Beat>> beats;
    std::map<std::string, std::string> ownFingerprints;
};

// The Beat Grids page (Library Health): runs the plan and a preview of
// Copy Beat Grids off the UI thread, shows what a copy would do in
// counts, and stages one CopyBeatGridChange per track it would write into
// the library's edit session, which Save writes like every other Library
// Health fix.
//
// The counts are a preview's and are named for what they are
// (wouldCopyCount, never "written"): what a save did is said by the save
// itself, from the changes that landed. Re-analysed after every save and
// undo, so a copy that landed shows as "already the same".
class BeatGridsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool scanCancellable READ busy NOTIFY busyChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(QString scanLabel READ scanLabel NOTIFY scanProgressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // An analysis has landed and its counts are current.
    Q_PROPERTY(bool analysed READ analysed NOTIFY resultChanged)
    Q_PROPERTY(bool engineTooOld READ engineTooOld NOTIFY resultChanged)
    Q_PROPERTY(QString ledgerWarning READ ledgerWarning NOTIFY resultChanged)
    // Tracks in both libraries with one rekordbox track for one Engine row.
    Q_PROPERTY(int matchedCount READ matchedCount NOTIFY resultChanged)
    // The preview's buckets. wouldCopyCount is what a copy WOULD write.
    Q_PROPERTY(int wouldCopyCount READ wouldCopyCount NOTIFY resultChanged)
    Q_PROPERTY(int alreadySameCount READ alreadySameCount NOTIFY resultChanged)
    Q_PROPERTY(int notAnalysedCount READ notAnalysedCount NOTIFY resultChanged)
    Q_PROPERTY(int lockedCount READ lockedCount NOTIFY resultChanged)
    Q_PROPERTY(int editedCount READ editedCount NOTIFY resultChanged)
    Q_PROPERTY(int noRekordboxGridCount READ noRekordboxGridCount NOTIFY resultChanged)
    // Rows the preview could not check (the row or its grid did not read).
    Q_PROPERTY(int unreadableCount READ unreadableCount NOTIFY resultChanged)
    Q_PROPERTY(QAbstractListModel *tracks READ tracks CONSTANT)
    // Also write over grids locked or moved on the player. Setting it runs
    // the analysis again with it.
    Q_PROPERTY(bool replaceEngineEdits READ replaceEngineEdits WRITE setReplaceEngineEdits NOTIFY
                   replaceEngineEditsChanged)
    // How many copies are staged and not saved yet.
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY stagedChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)

public:
    explicit BeatGridsController(QObject *parent = nullptr);
    ~BeatGridsController() override;

    bool busy() const { return m_scan.busy(); }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString scanLabel() const { return m_scanLabel; }
    QString errorMessage() const { return m_errorMessage; }
    bool analysed() const { return m_result.has_value(); }
    bool engineTooOld() const { return m_result && m_result->engineTooOld; }
    QString ledgerWarning() const;
    int matchedCount() const { return m_result ? m_result->matched : 0; }
    int wouldCopyCount() const { return m_result ? m_result->preview.wouldWrite : 0; }
    int alreadySameCount() const { return m_result ? m_result->preview.alreadySame : 0; }
    int notAnalysedCount() const { return m_result ? m_result->preview.notAnalysed : 0; }
    int lockedCount() const { return m_result ? m_result->preview.lockedOnEngine : 0; }
    int editedCount() const { return m_result ? m_result->preview.editedOnEngine : 0; }
    int noRekordboxGridCount() const { return m_result ? m_result->preview.noRekordboxGrid : 0; }
    int unreadableCount() const { return m_result ? m_result->preview.failed : 0; }
    QAbstractListModel *tracks() { return &m_tracks; }
    bool replaceEngineEdits() const { return m_replaceEngineEdits; }
    void setReplaceEngineEdits(bool replace);
    int stagedCount() const { return static_cast<int>(m_stagedIds.size()); }
    bool writing() const;

    // Plans and previews the copy for the stick holding these two
    // catalogs. A request for the same paths while one runs is served by
    // it.
    Q_INVOKABLE void analyze(const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void cancelScan();
    // Stages a copy of every grid the last analysis would write. Refused
    // while an analysis runs, or with nothing to write.
    Q_INVOKABLE void stage();
    Q_INVOKABLE void unstage();

signals:
    void busyChanged();
    void scanProgressChanged();
    void errorMessageChanged();
    void resultChanged();
    void replaceEngineEditsChanged();
    void stagedChanged();
    void writingChanged();
    void scanCancelled();

private:
    void start(bool restart);
    void attachSession();
    void setErrorMessage(const QString &message);
    void setScanProgress(int current, int total);
    void clearStaged();

    QString m_rekordboxPath;
    QString m_enginePath;
    bool m_replaceEngineEdits = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_scanLabel;
    QString m_errorMessage;
    std::optional<BeatGridAnalysis> m_result;
    BeatGridTrackListModel m_tracks;
    QStringList m_stagedIds;
    QPointer<LibraryEditSession> m_session;

    // Last, so it is destroyed first: its destructor cancels the analysis.
    AsyncRequest<std::shared_ptr<BeatGridAnalysis>> m_scan{this, [this]() { emit busyChanged(); }};
};

}  // namespace seabass::gui
