// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/engine_update_planning.hpp"
#include "gui/staged_plan_model.hpp"

namespace seabass::gui
{

// The rows of Sync after Rekordbox Export's page: one per row of the
// proposal (domain::EngineUpdateProposal), grouped by section in the
// page's order (docs/sync-after-rekordbox-export-plan.md, "The page"), so
// a ListView's section.property can head them. Conflicts come first: they
// are the only rows that need an answer before staging.
//
// A row of sections playlists to restoresToRekordbox is something the
// save writes when it is ticked; a conflict is a question, ticked only by
// answering it (resolveConflict); Engine's own and the refused adds are
// shown and never written.
//
// Every row also says, line by line, exactly what the save does with it
// (details), from its own payload and the rows beside it, never from a
// file: built on demand, for the row the page expands.
//
// Ticks follow the planner's rules. Rows sharing a key are one item and
// are ticked together. dependsOn holds both ways: unticking a row unticks
// every row that depends on it (a playlist's create takes its members
// with it), and ticking a row ticks what it depends on (a member brings
// its playlist's create and its track's add).
//
// Answering a conflict adds the chosen side's edits as rows of their own
// sections, under the conflict's key, ticked; answering it the other way
// replaces them. An answer whose side writes nothing adds no row; the
// conflict row then says which side was taken (resolvedSide) and the
// save records it as decided.
class RekordboxExportSyncListModel : public QAbstractListModel, public StagedPlanModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by RekordboxExportSyncController; not constructible from QML")

public:
    enum Roles {
        // The section's name (sectionName()), for section.property.
        SectionRole = Qt::UserRole + 1,
        // Its place in the page's order, 0 for conflicts.
        SectionIndexRole,
        KindRole,
        KeyRole,
        TitleRole,
        ArtistRole,
        DetailRole,
        ReasonRole,
        IsConflictRole,
        RekordboxChoiceLabelRole,
        EngineChoiceLabelRole,
        // "", "rekordbox" or "engine": conflict rows, and the rows an
        // answer added.
        ResolvedSideRole,
        IncludedRole,
        DependsOnRole,
        StagedRole,
        StagedDescriptionRole,
        // "to Engine", "back to rekordbox", "kept"; "" for an unanswered
        // conflict and a refused add.
        DirectionRole,
        // A row an answer added (its conflict is resolvedSide's).
        FromConflictRole,
        // QStringList, one line each: what happens to this track or
        // playlist (details()).
        DetailsRole,
    };

    // The page's order: the questions first, then what a tick writes,
    // then what is only shown.
    enum class Section {
        Conflicts,
        Playlists,
        TracksToAdd,
        TracksToRemove,
        Membership,
        MetadataToEngine,
        CuesToEngine,
        RestoresToRekordbox,
        EngineOwnKept,
        NotAdded,
    };

    struct Row
    {
        Section section = Section::Playlists;
        // createPlaylist, createFolder, renamePlaylist, deletePlaylist,
        // deleteFolder, addTrack, removeTrack, addMember, removeMember,
        // rating, comment, cues, conflict, kept, notAdded.
        QString kind;
        // The planner's; an answer's rows carry the conflict's key, and
        // its dependsOn joined with the edit's.
        domain::EngineUpdateItemHeader header;
        // Rows the save can write.
        std::optional<domain::EngineUpdateEdit> edit;
        // Conflict rows.
        std::optional<domain::EngineUpdateConflict> conflict;
        QString title;
        QString artist;
        QString detail;
        bool included = false;
        QString resolvedSide;
        // Unique for the life of the model, never reused: what an answer's
        // rows name their conflict by while indexes move.
        int uid = 0;
        int fromConflictUid = -1;
        // planKeyAt(): section, kind, key and an ordinal, so the same row
        // of the same proposal has the same key after a rescan.
        QString planKey;
        bool staged = false;
        QString stagedDescription;
    };

    explicit RekordboxExportSyncListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // `stickRoot`: where paths in the detail line are shown from.
    void setProposal(const domain::EngineUpdateProposal &proposal, const std::string &stickRoot);
    void clear();
    const std::vector<Row> &rows() const { return m_rows; }

    static QString sectionName(Section section);
    static std::optional<Section> sectionFromName(const QString &name);
    // Sections playlists to restoresToRekordbox: what a tick writes.
    static bool writable(Section section);

    // With dependsOn as above. A conflict row is ticked by answering it;
    // unticking it takes the answer back. Engine's own and the refused
    // adds cannot be ticked. False when nothing changed.
    bool setIncluded(int row, bool included);
    void setSectionIncluded(Section section, bool included);
    // False for a row that is not a conflict, or a side nothing can be
    // written for while the other side can (a refusal: both empty).
    bool resolveConflict(int row, bool rekordboxSide);
    bool clearResolution(int row);
    // Every conflict row, answered ones included, answered the one way (a
    // refusal, which nothing answers, stays as it is), or every answer
    // taken back. One model reset, not a row change per answer.
    void resolveAllConflicts(bool rekordboxSide);
    void clearAllResolutions();

    // What the save does with row `row`, one line each: for a track to add
    // its tags, cues, cover and the playlists it joins in this save; for a
    // removal Engine's row and the playlists it leaves; for a membership
    // the playlist, the place and the track it goes after; for cues each
    // cue that changes, old and new; for a rating or a comment old and
    // new; for a restore what goes back and why; for a conflict what each
    // side's edits would write; for Engine's own and a refused add the
    // reason in full.
    QStringList details(int row) const;

    // Rows per section name, every section present (0 when empty).
    QVariantMap sectionCounts() const;
    // Ticked rows per section name, every section present: what a
    // section-wide checkbox shows (ticked when every row is).
    QVariantMap sectionCheckedCounts() const;
    // Writable rows ticked.
    int checkedCount() const;
    // Conflicts not answered yet.
    int unresolvedConflictCount() const;
    int rowIndexOfUid(int uid) const;

    int planCount() const override { return static_cast<int>(m_rows.size()); }
    QString planKeyAt(int index) const override;
    void setStaged(int index, bool staged, const QString &description) override;
    void removePlanAt(int index) override;
    void clearStaged() override;

signals:
    void countsChanged();

private:
    Row makeRow(Section section, QString kind, const domain::EngineUpdateItemHeader &header);
    Row rowForEdit(const domain::EngineUpdateEdit &edit) const;
    void appendEdit(const domain::EngineUpdateEdit &edit);
    void assignPlanKey(Row &row);
    int endOfSection(Section section) const;
    void removeAnswerRows(int conflictUid);
    void announceIncluded();
    QStringList detailsOf(const Row &row) const;
    QStringList editLines(const domain::EngineUpdateEdit &edit) const;
    QString titleByPathKey(const std::string &pathKey) const;

    std::vector<Row> m_rows;
    // Set while resolveAllConflicts or clearAllResolutions runs inside one
    // model reset: no row signals, no announcement until the end.
    bool m_bulk = false;
    std::string m_stickRoot;
    int m_nextUid = 1;
    // Rows so far per planKey prefix, for the ordinal that ends a key.
    std::map<QString, int> m_ordinals;
};

}  // namespace seabass::gui
