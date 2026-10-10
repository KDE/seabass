// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/rekordbox_export_sync_controller.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/phased_progress.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/engine_update_planning.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "domain/track_matching.hpp"
#include "gui/edit/changes/add_engine_track_change.hpp"
#include "gui/edit/changes/engine_membership_change.hpp"
#include "gui/edit/changes/engine_playlist_changes.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/changes/record_rekordbox_baseline_change.hpp"
#include "gui/edit/changes/remove_engine_track_change.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/local/engine_update_stick_facts.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

// What one analysis read and decided, built on the worker with no access
// to the controller, and kept for stageSelected(): the baseline a save
// records is computed from the same reads as the proposal it answers.
struct RekordboxExportSyncAnalysis
{
    domain::EngineUpdateProposal proposal;
    // As read, for domain::nextBaseline.
    std::vector<domain::Track> rekordbox;
    std::vector<domain::PlaylistInfo> rekordboxPlaylists;
    std::optional<domain::RekordboxBaseline> baseline;
    std::uint64_t currentSequence = 0;
    std::string stickRoot;
    // A track by its baseline pathKey, to the file Engine finds it by: the
    // file of the Engine row the planner paired with it, when there is
    // one, else rekordbox's. A membership names its track and its "after"
    // anchor by key and by rekordbox's row, and EngineMembershipChange
    // finds the Engine row by file; a pair matched by name (a row whose
    // file is missing, OneStick) has two different files, and only
    // Engine's names its row there. A track added in the same save gets
    // rekordbox's file, which is the one AddEngineTrackChange writes.
    std::map<std::string, std::string> engineFileByKey;
    QString errorMessage;
    bool cancelled = false;
};

namespace
{

using domain::CueEdit;
using domain::EngineUpdateEdit;
using domain::MembershipEdit;
using domain::MetadataEdit;
using domain::PlaylistCreate;
using domain::PlaylistDelete;
using domain::PlaylistRename;
using domain::SyncPlan;
using domain::TrackToAdd;
using domain::TrackToRemove;
using Row = RekordboxExportSyncListModel::Row;
using Section = RekordboxExportSyncListModel::Section;

// A change this page stages with something of its own said about it: an
// id of its own (the chosen cues of a conflict, staged after the track's
// other cue write, which has the same id), a description (a rating sent
// to Engine is not "put back"), or the unit and verb the save's summary
// is told in. Everything else is the wrapped change's, as OwnedChange has
// it. A change this page already owns keeps its id unless one is given:
// wrapping it a second time must not rename it.
class PageChange final : public OwnedChange
{
public:
    struct Said
    {
        QString id;
        QString description;
        QString unit;
        QString verb;
    };

    PageChange(std::unique_ptr<PendingChange> inner, Said said)
        : OwnedChange(rekordboxExportSyncOwner(), std::move(inner)), m_said(std::move(said))
    {
    }

    QString id() const override
    {
        if (!m_said.id.isEmpty()) {
            return m_said.id;
        }
        return inner().owner() == rekordboxExportSyncOwner() ? inner().id() : OwnedChange::id();
    }
    QString description() const override
    {
        return m_said.description.isEmpty() ? OwnedChange::description() : m_said.description;
    }
    QString unit() const override { return m_said.unit.isEmpty() ? OwnedChange::unit() : m_said.unit; }
    QString verb() const override { return m_said.verb.isEmpty() ? OwnedChange::verb() : m_said.verb; }

private:
    Said m_said;
};

QString q(const std::string &text)
{
    return QString::fromStdString(text);
}

// Whether a write onto rekordbox puts back what Seabass wrote there (the
// export dropped it) rather than Engine's value over rekordbox's.
// Until MetadataEdit says so itself, its reason does: only ExportDropped
// is a restore.
bool restoresSeabassWrite(const domain::MetadataEdit &edit)
{
    return edit.header.reason == domain::EngineUpdateReason::ExportDropped;
}

QString titleOf(const domain::Track &track)
{
    return q(track.title.empty() ? track.filename : track.title);
}

int depthOf(const std::string &path)
{
    return static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

std::shared_ptr<application::TrackMetadataProbe> metadataProbe()
{
#ifdef SEABASS_HAVE_TAGLIB
    return std::make_shared<infrastructure::audio::TagLibMetadataProbe>();
#else
    return std::make_shared<application::NullTrackMetadataProbe>();
#endif
}

RekordboxExportSyncAnalysis runAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                        const std::shared_ptr<QtProgressReporter> &reporter,
                                        application::CancellationToken cancel)
{
    RekordboxExportSyncAnalysis result;
    try {
        if (rekordboxPath.isEmpty() || enginePath.isEmpty()) {
            throw std::runtime_error("Sync after Rekordbox Export needs a stick with both a rekordbox export and an "
                                     "Engine library.");
        }
        // The cache is keyed by the path as the session names it, so a
        // save's invalidation reaches these entries.
        const std::string pioneer = rekordboxPath.toStdString();
        const std::string engine = enginePath.toStdString();

        // First: a damaged record fails here, before the long reads, and
        // the page falls back only for a stick that has none.
        application::EngineUpdateStickFacts facts = infrastructure::local::readEngineUpdateStickFacts(
            pathToUtf8(pathFromQString(rekordboxPath)), pathToUtf8(pathFromQString(enginePath)));
        cancel.throwIfCancelled();

        auto &cache = LibraryCatalogCache::instance();
        std::optional<size_t> planned = 0;
        const auto addUnits = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        addUnits(cache.plannedUnits("rekordbox", pioneer, LibraryCatalogCache::Detail::Full, cancel));
        addUnits(cache.plannedUnits("engine", engine, LibraryCatalogCache::Detail::Full, cancel));
        application::PhasedProgress progress(*reporter, "Comparing the libraries", planned.value_or(0));

        // Full: the file checks and lengths matching needs, and Engine's
        // cues at each file's own rate (the Cues stage's sample-rate
        // source), so a 48 kHz file's cues are not read 9 percent late.
        std::vector<domain::Track> rekordboxTracks =
            cache.tracksFor("rekordbox", pioneer, LibraryCatalogCache::Detail::Full, progress, cancel);
        std::vector<domain::Track> engineTracks =
            cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Full, progress, cancel);
        cancel.throwIfCancelled();

        result.rekordbox = rekordboxTracks;
        result.rekordboxPlaylists = facts.rekordboxPlaylists;
        result.baseline = facts.baseline;
        result.currentSequence = facts.currentSequence;
        result.stickRoot = facts.stickRoot;
        const auto keyOf = [&result](const domain::Track &track) {
            return baselinePathKey(baselineStickRelativePath(result.stickRoot, track.filePath));
        };
        for (const auto &track : result.rekordbox) {
            if (const std::string key = keyOf(track); !key.empty()) {
                result.engineFileByKey.emplace(key, track.filePath);
            }
        }
        {
            // The planner's own pairing (EngineUpdatePlanner::plan):
            // streaming rows left out, then matchTracks within one stick.
            std::vector<domain::Track> engineRows;
            for (const auto &track : engineTracks) {
                if (track.streamingSource.empty()) {
                    engineRows.push_back(track);
                }
            }
            for (const auto &[r, e] : domain::matchTracks(result.rekordbox, engineRows, domain::MatchScope::OneStick)) {
                if (const std::string key = keyOf(*r); !key.empty() && !e->filePath.empty()) {
                    result.engineFileByKey[key] = e->filePath;
                }
            }
        }

        domain::EngineUpdateInput input =
            application::buildEngineUpdateInput(std::move(rekordboxTracks), std::move(engineTracks), std::move(facts));
        // The save's own path functions (rekordbox_baseline_ledger.hpp),
        // so the keys of this proposal are the keys the record is made
        // with. buildEngineUpdateInput's are the same rule spelled in the
        // application layer; these are the ones the save uses.
        input.stickRelativeOf = [root = result.stickRoot](const std::string &filePath) {
            return baselineStickRelativePath(root, filePath);
        };
        input.pathKeyOf = [](const std::string &relative) { return baselinePathKey(relative); };
        result.proposal = domain::EngineUpdatePlanner::plan(input);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
}

// The rows one change is made of, and how to make it once the save's
// Engine change count is known.
struct Planned
{
    std::vector<std::size_t> rows;
    bool engine = true;
    std::function<std::unique_ptr<PendingChange>(int engineChanges)> make;
};

}  // namespace

RekordboxExportSyncController::RekordboxExportSyncController(QObject *parent) : StagedCueEditController(parent)
{
    // Every count the page shows comes from the rows, and the rows change
    // from many places: an analysis, a tick, an answer, a row leaving once
    // its change landed. The model says so for all of them.
    connect(&m_model, &RekordboxExportSyncListModel::countsChanged, this, &RekordboxExportSyncController::listChanged);
}

RekordboxExportSyncController::~RekordboxExportSyncController() = default;

bool RekordboxExportSyncController::hasBaseline() const
{
    return m_analysis && m_analysis->proposal.hasBaseline;
}

qint64 RekordboxExportSyncController::baselineSequence() const
{
    return m_analysis ? static_cast<qint64>(m_analysis->proposal.baselineSequence) : 0;
}

qint64 RekordboxExportSyncController::currentSequence() const
{
    return m_analysis ? static_cast<qint64>(m_analysis->proposal.currentSequence) : 0;
}

QString RekordboxExportSyncController::introText() const
{
    if (!m_analysis) {
        return {};
    }
    return QString::fromStdString(application::engineUpdateIntroText(m_analysis->proposal));
}

bool RekordboxExportSyncController::proposalEmpty() const
{
    return m_analysis && m_analysis->proposal.empty();
}

bool RekordboxExportSyncController::onlyCues() const
{
    return m_analysis && m_analysis->proposal.onlyCues();
}

QString RekordboxExportSyncController::analysisKey() const
{
    return m_rekordboxPath + QLatin1Char('\n') + m_enginePath;
}

void RekordboxExportSyncController::analyze(const QString &stickLabel, const QString &rekordboxPath,
                                            const QString &enginePath)
{
    if (rekordboxPath + QLatin1Char('\n') + enginePath == analysisKey() && scanServes(analysisKey())) {
        return;  // the analysis this asks for is the one running
    }
    m_stickLabel = stickLabel;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    startAnalysis(false);
}

void RekordboxExportSyncController::startAnalysis(bool restart)
{
    attachSession();
    setErrorMessage({});
    setScanProgress(0, 0);
    auto reporter = makeReporter();
    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    startScan<RekordboxExportSyncAnalysis>(
        analysisKey(), stickRootOf(enginePath.isEmpty() ? rekordboxPath : enginePath), restart,
        [rekordboxPath, enginePath, reporter](application::CancellationToken cancel) {
            return runAnalysis(rekordboxPath, enginePath, reporter, std::move(cancel));
        },
        [this](RekordboxExportSyncAnalysis &&result) { onAnalyzeFinished(std::move(result)); });
}

void RekordboxExportSyncController::onAnalyzeFinished(RekordboxExportSyncAnalysis &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        // Refuse, never fall back: nothing of an earlier analysis stays
        // listed to be staged against a stick it no longer describes.
        m_analysis.reset();
        m_model.clear();
        setErrorMessage(result.errorMessage);
        emit analysisChanged();
        return;
    }
    auto analysis = std::make_shared<RekordboxExportSyncAnalysis>(std::move(result));
    m_model.setProposal(analysis->proposal, analysis->stickRoot);
    m_analysis = std::move(analysis);
    // Rows staged before this analysis keep their mark while they are
    // still listed; the changes themselves live in the session.
    for (const auto &[key, info] : m_stagedByKey) {
        const int index = indexOfStagedKey(key);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    emit analysisChanged();
}

void RekordboxExportSyncController::reanalyzeAfterUndo()
{
    if (!m_rekordboxPath.isEmpty() && !m_enginePath.isEmpty()) {
        startAnalysis(true);
    }
}

void RekordboxExportSyncController::onStagedCleared()
{
    m_stagedIds.clear();
}

void RekordboxExportSyncController::attachSession()
{
    attachSessionForPath(m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath);
    LibraryEditSession *s = session();
    if (!s) {
        return;
    }
    s->setLibraryPaths(m_rekordboxPath, m_enginePath);
    if (!m_stickLabel.isEmpty()) {
        s->setStickLabel(m_stickLabel);
    }
    // The base disconnects everything from a session it leaves, this
    // included, so it is made again for a new one and once only.
    if (s != m_connectedSession) {
        m_connectedSession = s;
        connect(s, &LibraryEditSession::saveFinished, this, &RekordboxExportSyncController::onSessionSaveFinished);
    }
}

// A save of this stick ended (this page's, or the undo of one): what is
// staged of ours is whatever the session still holds, and the page is
// analysed again, so it shows what is left.
void RekordboxExportSyncController::onSessionSaveFinished()
{
    LibraryEditSession *s = session();
    QStringList still;
    for (const QString &id : std::as_const(m_stagedIds)) {
        if (s && s->hasChange(id)) {
            still << id;
        }
    }
    m_stagedIds = still;
    // The base drops one row per landed change; a membership change is
    // many rows, so the rest of them go here.
    bool dropped = false;
    for (auto it = m_stagedByKey.begin(); it != m_stagedByKey.end();) {
        if (!s || !s->hasChange(it->second.changeId)) {
            it = m_stagedByKey.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    if (dropped) {
        clearStagedStatusIfNothingStaged();
        emit stagedChanged();
    }
    if (m_rekordboxPath.isEmpty() || m_enginePath.isEmpty() || scanServes(analysisKey())) {
        return;  // an undo already asked for it (reanalyzeAfterUndo)
    }
    startAnalysis(true);
}

void RekordboxExportSyncController::setIncluded(int row, bool included)
{
    m_model.setIncluded(row, included);
}

void RekordboxExportSyncController::setSectionIncluded(const QString &section, bool included)
{
    if (const auto s = RekordboxExportSyncListModel::sectionFromName(section)) {
        m_model.setSectionIncluded(*s, included);
    }
}

void RekordboxExportSyncController::resolveConflict(int row, bool rekordboxSide)
{
    m_model.resolveConflict(row, rekordboxSide);
}

void RekordboxExportSyncController::clearConflictResolution(int row)
{
    m_model.clearResolution(row);
}

void RekordboxExportSyncController::resolveAllConflicts(bool rekordboxSide)
{
    m_model.resolveAllConflicts(rekordboxSide);
}

void RekordboxExportSyncController::clearAllConflictResolutions()
{
    m_model.clearAllResolutions();
}

void RekordboxExportSyncController::unstageAll()
{
    if (m_stagedIds.isEmpty() && m_stagedByKey.empty()) {
        return;
    }
    if (LibraryEditSession *s = session(); s && !m_stagedIds.isEmpty()) {
        s->unstageAll(m_stagedIds);
    }
    m_stagedIds.clear();
    m_stagedByKey.clear();
    m_model.clearStaged();
    clearStagedStatusIfNothingStaged();
    emit stagedChanged();
}

QString RekordboxExportSyncController::describeMetadataWrite(const std::vector<domain::MetadataEdit> &edits,
                                                            bool toEngine)
{
    using domain::MetadataEdit;
    if (edits.empty()) {
        return {};
    }
    QStringList fields;
    QString ratingText;
    bool restore = true;
    for (const MetadataEdit &e : edits) {
        if (e.field == MetadataEdit::Field::Rating) {
            fields << QStringLiteral("rating");
            ratingText = e.rating && *e.rating > 0 ? QStringLiteral("%1 star(s)").arg(*e.rating)
                                                   : QStringLiteral("unrated");
        } else {
            fields << QStringLiteral("comment");
        }
        restore = restore && restoresSeabassWrite(e);
    }
    const bool ratingAlone = fields.size() == 1 && fields.front() == QLatin1String("rating");
    const QString what = fields.join(QStringLiteral(" and "));
    if (toEngine) {
        const QString title = titleOf(edits.front().engine);
        return ratingAlone ? QStringLiteral("Set the rating of \"%1\" in Engine to %2").arg(title, ratingText)
                           : QStringLiteral("Set the %1 of \"%2\" in Engine as rekordbox has it").arg(what, title);
    }
    const QString title = titleOf(edits.front().rekordbox);
    if (restore) {
        // The export dropped what Seabass wrote there: it goes back.
        return QStringLiteral("Put the %1 Seabass wrote back on \"%2\" in rekordbox").arg(what, title);
    }
    // Nothing recorded who wrote rekordbox's value (an answer toward
    // Engine of an OriginUnknown conflict): Engine's goes over it, and is
    // not called Seabass's.
    return ratingAlone ? QStringLiteral("Set the rating of \"%1\" in rekordbox to %2, Engine's").arg(title, ratingText)
                       : QStringLiteral("Set the %1 of \"%2\" in rekordbox to Engine's").arg(what, title);
}

void RekordboxExportSyncController::stageSelected()
{
    if (busy() || !m_analysis) {
        return;
    }
    attachSession();
    LibraryEditSession *s = session();
    if (!s) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (s->writing()) {
        setErrorMessage("A save is running. Stage more once it has finished.");
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    // The record of a save is computed from the whole selection, so a
    // second stage replaces the first rather than adding to it.
    unstageAll();

    const RekordboxExportSyncAnalysis &analysis = *m_analysis;
    const std::vector<Row> &rows = m_model.rows();

    // The ticked rows by what they are, in the model's order.
    std::vector<std::size_t> creates, adds, renames, members, metadataToEngine, cuesToEngine, deletes, removals,
        restoresMetadata, restoresCues;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row &row = rows[i];
        if (!RekordboxExportSyncListModel::writable(row.section) || !row.included || !row.edit) {
            continue;
        }
        const EngineUpdateEdit &edit = *row.edit;
        if (std::holds_alternative<PlaylistCreate>(edit)) {
            creates.push_back(i);
        } else if (std::holds_alternative<TrackToAdd>(edit)) {
            adds.push_back(i);
        } else if (std::holds_alternative<PlaylistRename>(edit)) {
            renames.push_back(i);
        } else if (std::holds_alternative<MembershipEdit>(edit)) {
            members.push_back(i);
        } else if (const auto *m = std::get_if<MetadataEdit>(&edit)) {
            (m->direction == MetadataEdit::Direction::ToEngine ? metadataToEngine : restoresMetadata).push_back(i);
        } else if (const auto *c = std::get_if<CueEdit>(&edit)) {
            (c->plan.direction == SyncPlan::Direction::ToB ? cuesToEngine : restoresCues).push_back(i);
        } else if (std::holds_alternative<PlaylistDelete>(edit)) {
            deletes.push_back(i);
        } else if (std::holds_alternative<TrackToRemove>(edit)) {
            removals.push_back(i);
        }
    }
    // Parents first for creates, deepest first for deletes, whatever order
    // an answer's rows joined in; and a conflict's chosen cues after the
    // track's other cue write, so the complete set is written last.
    std::stable_sort(creates.begin(), creates.end(), [&](std::size_t a, std::size_t b) {
        return depthOf(std::get<PlaylistCreate>(*rows[a].edit).path)
            < depthOf(std::get<PlaylistCreate>(*rows[b].edit).path);
    });
    std::stable_sort(deletes.begin(), deletes.end(), [&](std::size_t a, std::size_t b) {
        return depthOf(std::get<PlaylistDelete>(*rows[a].edit).path)
            > depthOf(std::get<PlaylistDelete>(*rows[b].edit).path);
    });
    const auto answersLast = [&](std::vector<std::size_t> &list) {
        std::stable_partition(list.begin(), list.end(), [&](std::size_t i) { return rows[i].fromConflictUid < 0; });
    };
    answersLast(cuesToEngine);
    answersLast(restoresCues);

    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    std::vector<Planned> planned;

    // The plan's staging order: create playlists, add tracks, renames,
    // memberships, metadata, cues to Engine, playlist deletes, track
    // removals, restores to rekordbox, record baseline.
    for (std::size_t i : creates) {
        const auto e = std::get<PlaylistCreate>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<CreateEnginePlaylistChange>(enginePath, e.path, hint);
                           }});
    }
    const auto probe = metadataProbe();
    for (std::size_t i : adds) {
        const auto e = std::get<TrackToAdd>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e, probe](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<AddEngineTrackChange>(enginePath, e.rekordbox, probe, hint);
                           }});
    }
    for (std::size_t i : renames) {
        const auto e = std::get<PlaylistRename>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<RenameEnginePlaylistChange>(enginePath, e.fromPath, e.toPath,
                                                                                   hint);
                           }});
    }
    {
        // One change per playlist with every ticked edit for it: removes
        // first, adds in rekordbox's order.
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::size_t>> byPlaylist;
        for (std::size_t i : members) {
            const auto &e = std::get<MembershipEdit>(*rows[i].edit);
            if (!byPlaylist.count(e.playlistPath)) {
                order.push_back(e.playlistPath);
            }
            byPlaylist[e.playlistPath].push_back(i);
        }
        for (const std::string &playlist : order) {
            std::vector<std::size_t> group = byPlaylist[playlist];
            std::stable_partition(group.begin(), group.end(), [&](std::size_t i) {
                return std::get<MembershipEdit>(*rows[i].edit).kind == MembershipEdit::Kind::Remove;
            });
            std::vector<EngineMembershipChange::Add> addList;
            std::vector<EngineMembershipChange::Remove> removeList;
            for (std::size_t i : group) {
                const auto &e = std::get<MembershipEdit>(*rows[i].edit);
                if (e.kind == MembershipEdit::Kind::Remove) {
                    removeList.push_back({e.track.sourceId, e.track.filePath, e.track.title});
                    continue;
                }
                EngineMembershipChange::Add add;
                const auto own = analysis.engineFileByKey.find(e.pathKey);
                add.filePath = own != analysis.engineFileByKey.end() ? own->second : e.track.filePath;
                add.title = e.track.title;
                if (!e.afterPathKey.empty()) {
                    // The anchor by its file. A key no file is known for
                    // still goes as a path (a non-empty one that names
                    // nothing), so the writer appends rather than putting
                    // the track first.
                    const auto file = analysis.engineFileByKey.find(e.afterPathKey);
                    add.afterFilePath = file != analysis.engineFileByKey.end() ? file->second : e.afterPathKey;
                }
                addList.push_back(std::move(add));
            }
            planned.push_back({group, true,
                               [enginePath, playlist, addList, removeList](int hint) -> std::unique_ptr<PendingChange> {
                                   return std::make_unique<EngineMembershipChange>(enginePath, playlist, addList,
                                                                                   removeList, hint);
                               }});
        }
    }
    // A track's rating and comment are one change: one id per track and
    // catalog, and a second would replace the first.
    const auto metadataChanges = [&](const std::vector<std::size_t> &list, bool toEngine) {
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::size_t>> byTrack;
        for (std::size_t i : list) {
            const auto &e = std::get<MetadataEdit>(*rows[i].edit);
            const std::string id = toEngine ? e.engine.sourceId : e.rekordbox.sourceId;
            if (!byTrack.count(id)) {
                order.push_back(id);
            }
            byTrack[id].push_back(i);
        }
        for (const std::string &id : order) {
            const std::vector<std::size_t> group = byTrack[id];
            const auto &first = std::get<MetadataEdit>(*rows[group.front()].edit);
            domain::MetadataRestoreProposal proposal;
            proposal.stickTrack = toEngine ? first.engine : first.rekordbox;
            std::vector<MetadataEdit> edits;
            for (std::size_t i : group) {
                const auto &e = std::get<MetadataEdit>(*rows[i].edit);
                edits.push_back(e);
                if (e.field == MetadataEdit::Field::Rating) {
                    proposal.ratingOffered = true;
                    // 0 stars is unrated to every writer: a cleared rating
                    // is written as one.
                    proposal.rating = e.rating.value_or(0);
                } else {
                    proposal.commentOffered = true;
                    proposal.comment = e.comment;
                }
            }
            const QString description = describeMetadataWrite(edits, toEngine);
            const QString format = toEngine ? QStringLiteral("engine") : QStringLiteral("rekordbox");
            const QString path = toEngine ? enginePath : rekordboxPath;
            planned.push_back({group, toEngine,
                               [format, path, id, proposal, description](int hint) -> std::unique_ptr<PendingChange> {
                                   auto inner = std::make_unique<RestoreMetadataChange>(format, path, q(id), proposal,
                                                                                        hint);
                                   return std::make_unique<PageChange>(std::move(inner),
                                                                       PageChange::Said{{}, description, {}, {}});
                               }});
        }
    };
    const auto cueChanges = [&](const std::vector<std::size_t> &list, bool toEngine) {
        for (std::size_t i : list) {
            const SyncPlan plan = std::get<CueEdit>(*rows[i].edit).plan;
            const bool answer = rows[i].fromConflictUid >= 0;
            planned.push_back({{i}, toEngine,
                               [rekordboxPath, enginePath, plan, answer](int hint) -> std::unique_ptr<PendingChange> {
                                   auto inner = std::make_unique<SyncPlanChange>(rekordboxPath, enginePath, plan, hint);
                                   if (!answer) {
                                       return ownedByRekordboxExportSync(std::move(inner));
                                   }
                                   // The track's other cue write has this
                                   // id; both are staged, this one last.
                                   const QString id =
                                       rekordboxExportSyncOwner() + QStringLiteral(":answer:") + inner->id();
                                   return std::make_unique<PageChange>(std::move(inner),
                                                                       PageChange::Said{id, {}, {}, {}});
                               }});
        }
    };
    metadataChanges(metadataToEngine, true);
    cueChanges(cuesToEngine, true);
    for (std::size_t i : deletes) {
        const auto e = std::get<PlaylistDelete>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int) -> std::unique_ptr<PendingChange> {
                               return deleteEnginePlaylistChange(enginePath, e.path);
                           }});
    }
    for (std::size_t i : removals) {
        const auto e = std::get<TrackToRemove>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<RemoveEngineTrackChange>(enginePath, e.engine, hint);
                           }});
    }
    metadataChanges(restoresMetadata, false);
    cueChanges(restoresCues, false);

    // What the save may write into m.db: every Engine change, which is what
    // decides the scratch copy.
    const int engineChanges =
        static_cast<int>(std::count_if(planned.begin(), planned.end(), [](const Planned &p) { return p.engine; }));

    // The record: what the page offered, what it stages, what was left.
    std::set<std::string> offered;
    std::set<std::string> applied;
    std::map<std::string, std::string> declined;
    const auto withCueItems = [](const domain::EngineUpdateItemHeader &header) {
        std::vector<std::string> keys{header.key};
        for (const auto &item : header.cueItems) {
            keys.push_back(item.key);
        }
        return keys;
    };
    for (const Row &row : rows) {
        for (const auto &key : withCueItems(row.header)) {
            offered.insert(key);
        }
    }
    for (const auto &key : analysis.proposal.declinedSuppressed) {
        offered.insert(key);
    }
    std::set<int> answeredByARow;
    for (const Row &row : rows) {
        if (RekordboxExportSyncListModel::writable(row.section) && row.included && row.fromConflictUid >= 0) {
            answeredByARow.insert(row.fromConflictUid);
        }
    }
    // A row is declined only when the user unticked it itself: one whose
    // dependency is unticked went with it (a playlist's create takes its
    // members) and is neither applied nor declined, so it is offered again
    // with the record before. Declined on its own key, a member would stay
    // out after rekordbox renamed its playlist and the create came back.
    std::set<std::string> listedKeys;
    std::set<std::string> tickedKeys;
    for (const Row &row : rows) {
        if (row.section == Section::Conflicts || RekordboxExportSyncListModel::writable(row.section)) {
            listedKeys.insert(row.header.key);
        }
        if (RekordboxExportSyncListModel::writable(row.section) && row.included) {
            tickedKeys.insert(row.header.key);
        }
    }
    const auto untickedWithADependency = [&](const Row &row) {
        return std::any_of(row.header.dependsOn.begin(), row.header.dependsOn.end(), [&](const std::string &key) {
            return listedKeys.count(key) && !tickedKeys.count(key);
        });
    };
    for (const Row &row : rows) {
        if (row.section == Section::Conflicts) {
            // An answer whose side writes nothing is decided all the same:
            // the item advances, and Engine keeps what it has.
            const bool emptyAnswer = !row.resolvedSide.isEmpty() && row.conflict
                && (row.resolvedSide == QLatin1String("rekordbox") ? row.conflict->rekordboxChoice.empty()
                                                                   : row.conflict->engineChoice.empty());
            // Left open on a run without a record (unanswered, or its
            // answer unticked): recorded as the rekordbox side stands, so
            // it reads as Engine's own from now on and is not asked again
            // (the plan's step 10 review decision). Keeping "the record
            // before" would take the item out of a record there was none
            // of, and a member both sides hold would then come back as a
            // change of both. A refusal answers nothing and comes back.
            const bool refusal = row.conflict && row.conflict->rekordboxChoice.empty()
                && row.conflict->engineChoice.empty();
            const bool openWithoutRecord = !analysis.baseline && !refusal;
            if ((emptyAnswer || openWithoutRecord) && !answeredByARow.count(row.uid)) {
                for (const auto &key : withCueItems(row.header)) {
                    applied.insert(key);
                }
            }
            continue;  // unanswered: comes back, never declined
        }
        if (!RekordboxExportSyncListModel::writable(row.section)) {
            continue;  // Engine's own and the refused adds are never written
        }
        if (row.included) {
            continue;  // applied below, through the change it rides in
        }
        if (row.fromConflictUid >= 0) {
            continue;  // an answer left unticked leaves its conflict open
        }
        if (untickedWithADependency(row)) {
            continue;  // unticked with what it depends on: offered, not declined
        }
        declined[row.header.key] = row.header.rekordboxState;
        for (const auto &item : row.header.cueItems) {
            declined[item.key] = item.rekordboxState;
        }
    }

    std::vector<std::unique_ptr<PendingChange>> changes;
    RecordRekordboxBaselineChange::AppliedBy appliedBy;
    std::vector<std::pair<std::vector<std::size_t>, QString>> rowsOfChange;
    std::set<std::pair<QString, QString>> unitsAndVerbs;
    for (Planned &p : planned) {
        std::unique_ptr<PendingChange> change = p.make(engineChanges);
        std::vector<std::string> keys;
        for (std::size_t i : p.rows) {
            for (const auto &key : withCueItems(rows[i].header)) {
                applied.insert(key);
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                    keys.push_back(key);
                }
            }
        }
        appliedBy.emplace_back(change->id().toStdString(), std::move(keys));
        rowsOfChange.emplace_back(p.rows, change->id());
        unitsAndVerbs.insert({change->unit(), change->verb()});
        changes.push_back(std::move(change));
    }
    for (const auto &key : applied) {
        declined.erase(key);
    }

    if (changes.empty() && declined.empty() && analysis.proposal.hasBaseline) {
        setStatusMessage(QStringLiteral("Nothing is selected, so there is nothing to stage."));
        return;
    }

    // The save's summary sums every change's units under the first
    // change's noun and verb: "12 of 12 playlists created" for a save that
    // also added tracks would not be true. A save of more than one kind of
    // change is told in changes.
    if (unitsAndVerbs.size() > 1) {
        changes.front() = std::make_unique<PageChange>(
            std::move(changes.front()), PageChange::Said{{}, {}, QStringLiteral("changes"), QStringLiteral("made")});
    }

    const auto stickRelativeOf = [root = analysis.stickRoot](const std::string &filePath) {
        return baselineStickRelativePath(root, filePath);
    };
    const auto pathKeyOf = [](const std::string &relative) { return baselinePathKey(relative); };
    domain::RekordboxBaseline next =
        domain::nextBaseline(analysis.baseline ? &*analysis.baseline : nullptr, analysis.rekordbox,
                             analysis.rekordboxPlaylists, analysis.currentSequence, offered, applied, declined,
                             stickRelativeOf, pathKeyOf);
    const int changeCount = static_cast<int>(changes.size());
    changes.push_back(std::make_unique<RecordRekordboxBaselineChange>(analysis.stickRoot, std::move(next),
                                                                      std::move(appliedBy)));

    std::vector<QString> descriptions;
    QStringList ids;
    for (const auto &change : changes) {
        descriptions.push_back(change->description());
        ids << change->id();
    }
    if (!s->stageAll(std::move(changes))) {
        return;  // the session said why (a lock, a read-only backup, another page)
    }
    m_stagedIds = ids;
    for (std::size_t c = 0; c < rowsOfChange.size(); ++c) {
        const auto &[rowIndexes, id] = rowsOfChange[c];
        for (std::size_t i : rowIndexes) {
            m_stagedByKey[rows[i].planKey] = {id, descriptions[c]};
            m_model.setStaged(static_cast<int>(i), true, descriptions[c]);
        }
    }
    emit stagedChanged();
    setStagedStatusMessage(changeCount > 0
                               ? QStringLiteral("Staged %1 change(s). Press Sync Engine to write them to the stick.")
                                     .arg(changeCount)
                               : QStringLiteral("Staged the record of what was left unselected. Press Sync Engine to "
                                                "write it to the stick."));
}

}  // namespace seabass::gui
