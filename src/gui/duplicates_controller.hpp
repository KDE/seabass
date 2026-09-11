// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/edit/changes/copy_cues_change.hpp"
#include "gui/staged_cue_edit_controller.hpp"
#include "gui/staged_plan_model.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Read-only Qt list model over the consolidation plans DuplicatesController
// last computed. Only Unambiguous (fixable) and Conflict (informational)
// plans are exposed -- NoCues/AlreadyConsistent groups need no attention,
// same filtering cli/main.cpp's handleDuplicates applies.
class ConsolidationPlanListModel : public QAbstractListModel, public StagedPlanModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by DuplicatesController; not constructible from QML")

public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        FilenameRole,
        DescriptionRole,
        ActionableRole,
        TracksRole,
        WastedBytesRole,
        // A copy is staged for this group (see DuplicatesController): what
        // will happen on Save, and from which copy.
        StagedRole,
        StagedDescriptionRole,
    };

    explicit ConsolidationPlanListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Waveforms are NOT precomputed here -- QML fetches a track's
    // waveform on demand via PlaybackController::waveformFor() instead
    // of this controller decoding one eagerly for every displayed track
    // up front (see sync_controller.hpp's own comment on the same
    // change, made for the same reason: it's what made a real "scanning
    // takes forever" report possible in the first place).
    void setPlans(std::vector<domain::ConsolidationPlan> plans);
    const std::vector<domain::ConsolidationPlan> &plans() const { return m_plans; }
    // Removes one plan without a full rescan -- once its cues are on the
    // stick that group is AlreadyConsistent, which this model never shows
    // at all (see runRescanTask's own filtering), and DuplicateTrackFinder
    // groups by filename/title+artist+duration only, never cues, so
    // removing it can't change any other group's own classification.
    void removePlanAt(int index) override;
    void setStaged(int index, bool staged, const QString &description) override;
    void clearStaged() override;

    int planCount() const override { return static_cast<int>(m_plans.size()); }
    // A group's identity across rescans: its member track ids, sorted.
    QString planKeyAt(int index) const override;

private:
    std::vector<domain::ConsolidationPlan> m_plans;
    std::vector<QString> m_stagedDescriptions;  // empty = not staged; parallel to m_plans
};

// Result of a background rescan task -- see DuplicatesController::rescan().
// Built entirely on a worker thread, with no access to the controller.
struct DuplicatesTaskResult
{
    std::vector<domain::ConsolidationPlan> plans;
    QString errorMessage;  // empty on success
    bool cancelled = false;  // stopped via cancelScan(); nothing else is set
};

// One "copy source's cues onto these targets" operation -- applyOne() and
// copyFromTrack() each produce exactly one, applyAllUnambiguous() produces
// one per unambiguous group. Tracks are copied by value so the save loop
// never touches the GUI-thread model.
// Wraps ConsolidateDuplicateCues for QML: scans a library, finds duplicate
// tracks, and (for Unambiguous groups) can copy cues from the one copy that
// has them onto the others -- mirroring cli/main.cpp's handleDuplicates
// wiring exactly so GUI and CLI behave identically.
//
// Edits are staged, not written: applyOne()/copyFromTrack()/
// applyAllUnambiguous() each stage one CopyCuesChange per group in the
// library's LibraryEditSession (the first one takes the edit lock), the
// row shows what will happen, and the page's Save writes them all. A
// group whose change reached the stick disappears from the list.
//
// The scan itself runs on a background thread (see ScanController for
// the same reasoning): it can take several seconds for a large library.
class DuplicatesController : public StagedCueEditController
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::ConsolidationPlanListModel *plans READ plansModel CONSTANT)
    // Sum, across every duplicate group currently listed (Unambiguous and
    // Conflict alike -- being a duplicate doesn't depend on cue-consolidation
    // status), of every group's file sizes minus its single largest copy:
    // the disk space that would be freed if each group kept only one file.
    // Purely informational -- Seabass has no feature that deletes audio
    // files, this is a number, not an action.
    Q_PROPERTY(QString totalWastedBytesHuman READ totalWastedBytesHuman NOTIFY plansChanged)

public:
    explicit DuplicatesController(QObject *parent = nullptr);

    ConsolidationPlanListModel *plansModel() { return &m_model; }
    QString totalWastedBytesHuman() const;

    // format is "rekordbox", "engine", or "onelibrary"; path is the
    // corresponding DetectedStick.rekordboxPath / .enginePath (OneLibrary
    // shares rekordboxPath -- exportLibrary.db lives alongside
    // export.pdb under the same PIONEER root).
    Q_INVOKABLE void scan(const QString &format, const QString &path);

    // Same convention as CleanupController::hasOneLibrary()/
    // ScanController::hasOneLibrary().
    Q_INVOKABLE bool hasOneLibrary(const QString &pioneerRoot) const;

    // Stage copying the one cued copy's cues onto the rest of the group.
    Q_INVOKABLE void applyOne(int index);
    Q_INVOKABLE void applyAllUnambiguous();

    // Manual override for a Conflict group: stages copying sourceTrackId's
    // cues onto every other track in that same group. Unlike applyOne,
    // this works regardless of plan kind -- it's the human decision the
    // domain model defers to when copies disagree (see
    // ConsolidationPlan::Kind::Conflict's doc comment). Staging a second
    // choice for the same group replaces the first.
    Q_INVOKABLE void copyFromTrack(int index, const QString &sourceTrackId);

signals:
    // Also covers totalWastedBytesHuman and the staged descriptions the
    // rows carry -- everything this page derives from the plan list.
    // Staging alone is the base's stagedChanged().
    void plansChanged();

protected:
    StagedPlanModel *stagedPlanModel() override { return &m_model; }
    void reanalyzeAfterUndo() override { rescan(); }
    void onStagedChangeApplied(bool) override { emit plansChanged(); }
    void onStagedCleared() override { emit plansChanged(); }

private:
    void rescan();
    void onRescanFinished();
    void attachSession();
    void stageCopy(int index, const DuplicatesCopyOp &op);

    ConsolidationPlanListModel m_model;
    QFutureWatcher<DuplicatesTaskResult> m_watcher;
    QString m_format;
    QString m_path;
};

}  // namespace seabass::gui
