// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

#include "gui/rekordbox_export_sync_list_model.hpp"
#include "gui/staged_cue_edit_controller.hpp"

namespace seabass::gui
{

class LibraryEditSession;
struct RekordboxExportSyncAnalysis;

// Sync after Rekordbox Export's page (docs/sync-after-rekordbox-export-
// plan.md, "The page" and "GUI"): a proposal first, then one save, as Sync
// Cue Points is.
//
// analyze() reads, on a worker and through LibraryCatalogCache at
// Detail::Full, both catalogs of one stick (Engine's cues at each file's
// own sample rate), what only the stick can say
// (readEngineUpdateStickFacts: the playlist trees, pdbImportKey, the
// baseline, export.pdb's sequence), and runs EngineUpdatePlanner. A
// baseline that is on the stick and cannot be read fails the analysis
// with the reason and an empty list: the plan falls back only for a stick
// that has none.
//
// Ticks are the model's (RekordboxExportSyncListModel: rows sharing a key
// together, dependsOn both ways). stageSelected() stages the ticked rows
// as one batch in the plan's staging order, every change owned by
// "rekordbox-export-sync" (borrowed ones through OwnedChange), and
// RecordRekordboxBaselineChange last, built with domain::nextBaseline
// from what the page offered, what it stages and what was left unticked.
// A second stageSelected() replaces the first batch: the record is
// computed from the whole selection, so it cannot be added to.
//
// After every save of the stick's session (an undo included) the page is
// analysed again, so it shows what is left: nothing, or the conflicts.
class RekordboxExportSyncController : public StagedCueEditController
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::RekordboxExportSyncListModel *rows READ rows CONSTANT)
    // Whether the stick has a record from an earlier save of this page,
    // and its export.pdb sequence; currentSequence is export.pdb's now.
    Q_PROPERTY(bool hasBaseline READ hasBaseline NOTIFY analysisChanged)
    Q_PROPERTY(qint64 baselineSequence READ baselineSequence NOTIFY analysisChanged)
    Q_PROPERTY(qint64 currentSequence READ currentSequence NOTIFY analysisChanged)
    // What the proposal was compared against, and with no earlier record
    // the first run's rule, in full sentences over the sections.
    Q_PROPERTY(QString introText READ introText NOTIFY analysisChanged)
    // Something to write or decide, and all of it cues: the page links to
    // Sync Cue Points.
    Q_PROPERTY(bool onlyCues READ onlyCues NOTIFY analysisChanged)
    // Whether the last analysis has landed (false before the first one,
    // and after one that failed).
    Q_PROPERTY(bool analyzed READ analyzed NOTIFY analysisChanged)
    // Rows per section name (RekordboxExportSyncListModel::sectionName),
    // every section present.
    Q_PROPERTY(QVariantMap sectionCounts READ sectionCounts NOTIFY listChanged)
    // Ticked rows per section name, for the section-wide checkboxes.
    Q_PROPERTY(QVariantMap sectionCheckedCounts READ sectionCheckedCounts NOTIFY listChanged)
    // Rows per category for the overview bar
    // (RekordboxExportSyncListModel::categoryCounts).
    Q_PROPERTY(QVariantMap categoryCounts READ categoryCounts NOTIFY listChanged)
    // What the proposal does, counted plainly, and said per category in
    // the overview bar's legend (RekordboxExportSyncListModel::
    // summaryCounts, legendTexts).
    Q_PROPERTY(QVariantMap summaryCounts READ summaryCounts NOTIFY listChanged)
    Q_PROPERTY(QVariantMap legendTexts READ legendTexts NOTIFY listChanged)
    // Analysed, and nothing to write or decide
    // (domain::EngineUpdateProposal::empty).
    Q_PROPERTY(bool proposalEmpty READ proposalEmpty NOTIFY analysisChanged)
    // Writable rows ticked.
    Q_PROPERTY(int checkedCount READ checkedCount NOTIFY listChanged)
    // Conflicts not answered yet.
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY listChanged)

public:
    explicit RekordboxExportSyncController(QObject *parent = nullptr);
    ~RekordboxExportSyncController() override;

    RekordboxExportSyncListModel *rows() { return &m_model; }
    bool hasBaseline() const;
    qint64 baselineSequence() const;
    qint64 currentSequence() const;
    QString introText() const;
    bool onlyCues() const;
    bool analyzed() const { return m_analysis != nullptr; }
    QVariantMap sectionCounts() const { return m_model.sectionCounts(); }
    QVariantMap sectionCheckedCounts() const { return m_model.sectionCheckedCounts(); }
    QVariantMap categoryCounts() const { return m_model.categoryCounts(); }
    QVariantMap summaryCounts() const { return m_model.summaryCounts(); }
    QVariantMap legendTexts() const { return RekordboxExportSyncListModel::legendTexts(m_model.summaryCounts()); }
    bool proposalEmpty() const;
    int checkedCount() const { return m_model.checkedCount(); }
    int conflictCount() const { return m_model.unresolvedConflictCount(); }

    // Read-only. The stick's DetectedStick.label, .rekordboxPath and
    // .enginePath; both catalogs are needed.
    Q_INVOKABLE void analyze(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath);

    // `row` is the model's row. See RekordboxExportSyncListModel.
    Q_INVOKABLE void setIncluded(int row, bool included);
    // `section` by name: "playlists", "tracksToAdd", ... (sectionCounts'
    // keys). Conflicts can only be unticked here (every answer taken back).
    Q_INVOKABLE void setSectionIncluded(const QString &section, bool included);
    // Answers the conflict at `row`: rekordbox's side or Engine's. Its
    // edits become ticked rows of their sections under the conflict's key;
    // nothing is staged until stageSelected().
    Q_INVOKABLE void resolveConflict(int row, bool rekordboxSide);
    Q_INVOKABLE void clearConflictResolution(int row);
    // Every conflict of the list, answered ones included, answered the one
    // way (refusals stay open), or every answer taken back.
    Q_INVOKABLE void resolveAllConflicts(bool rekordboxSide);
    Q_INVOKABLE void clearAllConflictResolutions();

    // Stages the ticked rows, and the record of this save, replacing what
    // an earlier call staged; the page's Save ("Sync Engine") writes them.
    Q_INVOKABLE void stageSelected();
    // Takes back what stageSelected() staged.
    Q_INVOKABLE void unstageAll();

signals:
    void analysisChanged();
    void listChanged();

protected:
    StagedPlanModel *stagedPlanModel() override { return &m_model; }
    void reanalyzeAfterUndo() override;
    void onStagedCleared() override;

private:
    void startAnalysis(bool restart);
    QString analysisKey() const;
    void onAnalyzeFinished(RekordboxExportSyncAnalysis &&result);
    void attachSession();
    void onSessionSaveFinished();

    RekordboxExportSyncListModel m_model;
    QString m_stickLabel;
    QString m_rekordboxPath;
    QString m_enginePath;
    std::shared_ptr<const RekordboxExportSyncAnalysis> m_analysis;
    // Every change id the last stageSelected() put in the session, the
    // record's included.
    QStringList m_stagedIds;
    // The session saveFinished is connected on, so a repeat attach does
    // not connect it twice.
    LibraryEditSession *m_connectedSession = nullptr;
};

}  // namespace seabass::gui
