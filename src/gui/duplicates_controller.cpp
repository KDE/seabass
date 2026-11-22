// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "duplicates_controller.hpp"

#include "gui/stick_path.hpp"
#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "application/ports/cue_writer.hpp"
#include "application/use_cases/consolidate_duplicate_cues.hpp"
#include "application/use_cases/measure_duplicate_cue_shifts.hpp"
#include "domain/duplicate_cue_shift.hpp"
#include "domain/matching_policy.hpp"
#include "gui/cancellable_audio_probe.hpp"
#include "gui/cue_shift_text.hpp"
#include "infrastructure/audio/alignment_probe_factory.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/audio/silence_probe_factory.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/local_file_url.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "application/phased_progress.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/copy_cues_change.hpp"
#include "gui/qt_path.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::ConsolidationPlan;

namespace
{

// Mirrors cli/main.cpp's humanSize() exactly.
QString humanSize(std::uint64_t bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        unit++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return QString::fromUtf8(buf);
}

// Bytes that would be freed if this group kept only its single largest
// file instead of every copy, 0 if fewer than two tracks have a known
// size (nothing meaningful to compare).
std::uint64_t wastedBytes(const ConsolidationPlan &plan)
{
    std::uint64_t total = 0;
    std::uint64_t largest = 0;
    int known = 0;
    for (const auto &t : plan.group.tracks) {
        if (t.fileSizeBytes == 0) {
            continue;
        }
        known++;
        total += t.fileSizeBytes;
        largest = std::max(largest, t.fileSizeBytes);
    }
    return known >= 2 ? total - largest : 0;
}

// What lining the copies up measured, in the order the page lists them
// and numbered as its cards are ("COPY 2"): for each copy, its shift
// against the first copy it lines up with, or that it lines up with none.
// Copies that line up only among themselves are said as such. Empty for a
// group whose copies all line up as they are, or that was not measured.
QString alignmentNote(const domain::DuplicateGroup &group)
{
    if (!group.cueShiftsMeasured) {
        return {};
    }
    const size_t n = group.tracks.size();
    auto partnerOf = [&](size_t i) -> std::optional<size_t> {
        for (size_t j = 0; j < n; ++j) {
            if (j != i && domain::cueShiftBetween(group, j, i)) {
                return j;
            }
        }
        return std::nullopt;
    };
    // The first copy of the first set that lines up at all: what the
    // others are said against when they do not line up with it.
    std::optional<size_t> firstLinedUp;
    for (size_t i = 0; i < n && !firstLinedUp; ++i) {
        if (partnerOf(i)) {
            firstLinedUp = i;
        }
    }
    // Nothing lines up with anything: one sentence for the group.
    if (!firstLinedUp) {
        return n == 2 ? QStringLiteral("Copy 2 could not be lined up with copy 1, so no cues are copied between them.")
                      : QStringLiteral("None of the copies could be lined up with another, so no cues are copied "
                                       "between them.");
    }
    QStringList lines;
    for (size_t i = 0; i < n; ++i) {
        const QString copy = QStringLiteral("Copy %1").arg(i + 1);
        const std::optional<size_t> partner = partnerOf(i);
        if (!partner) {
            lines << QStringLiteral("%1 could not be lined up with any other copy, so no cues are copied to or from it.")
                         .arg(copy);
            continue;
        }
        if (*partner < i) {
            const double shift = *domain::cueShiftBetween(group, *partner, i);
            if (shift != 0.0) {
                lines << shiftSentence(copy, QStringLiteral("copy %1").arg(*partner + 1), shift);
            }
            continue;
        }
        // The first of its own set: worth a line only when that set is
        // not the first one.
        if (firstLinedUp && *firstLinedUp != i && !domain::cueShiftBetween(group, *firstLinedUp, i)) {
            lines << QStringLiteral("%1 could not be lined up with copy %2, so no cues are copied between them.")
                         .arg(copy)
                         .arg(*firstLinedUp + 1);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// Cues an Unambiguous plan's copy would leave out, summed over its
// targets: those that land off a target's ends once moved onto it.
int cuesLeftOut(const ConsolidationPlan &plan)
{
    if (!plan.source) {
        return 0;
    }
    int leftOut = 0;
    for (size_t i = 0; i < plan.targets.size(); ++i) {
        const double shift = i < plan.targetShiftMs.size() ? plan.targetShiftMs[i] : 0.0;
        leftOut += domain::movedCues(plan.source->cues, shift, plan.targets[i].durationSeconds).leftOut;
    }
    return leftOut;
}

}  // namespace

ConsolidationPlanListModel::ConsolidationPlanListModel(QObject *parent) : QAbstractListModel(parent) {}

int ConsolidationPlanListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_plans.size());
}

QVariant ConsolidationPlanListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_plans.size()) {
        return {};
    }
    const auto row = static_cast<size_t>(index.row());
    const auto &plan = m_plans[row];
    switch (role) {
    case KindRole:
        if (plan.kind == ConsolidationPlan::Kind::Unambiguous) {
            return QStringLiteral("unambiguous");
        }
        return plan.kind == ConsolidationPlan::Kind::Unaligned ? QStringLiteral("unaligned")
                                                                 : QStringLiteral("conflict");
    case FilenameRole:
        return QString::fromStdString(plan.group.tracks.empty() ? "" : plan.group.tracks.front().filename);
    case DescriptionRole:
        if (plan.kind == ConsolidationPlan::Kind::Unambiguous) {
            QString text = QString("%1 cue(s) on one copy, missing on %2 other copy/copies")
                               .arg(plan.source->cues.size())
                               .arg(plan.targets.size());
            if (const int leftOut = cuesLeftOut(plan); leftOut > 0) {
                text += QStringLiteral(". %1 cue(s) would land outside a copy once moved, and are left out of it")
                            .arg(leftOut);
            }
            return text;
        }
        if (plan.kind == ConsolidationPlan::Kind::Unaligned) {
            return QStringLiteral("The copies' audio could not be lined up, so their cues cannot be copied or "
                                  "compared; not touching them");
        }
        return QString("%1 copies have different cues, not touching them").arg(plan.group.tracks.size());
    case ActionableRole:
        return plan.kind == ConsolidationPlan::Kind::Unambiguous;
    case TracksRole: {
        QVariantList result;
        for (const auto &t : plan.group.tracks) {
            QVariantMap trackMap;
            trackMap["side"] = QString::fromStdString(t.format);
            trackMap["sourceId"] = QString::fromStdString(t.sourceId);
            trackMap["title"] = QString::fromStdString(t.title);
            trackMap["artist"] = QString::fromStdString(t.artist);
            trackMap["filePath"] = QString::fromStdString(t.filePath);
            trackMap["artworkPath"] = toLocalFileUrl(t.artworkPath);
            trackMap["sizeBytes"] = static_cast<qulonglong>(t.fileSizeBytes);

            QStringList playlists;
            for (const auto &p : t.playlists) {
                playlists << QString::fromStdString(p.name);
            }
            trackMap["playlists"] = playlists;

            QVariantList cues;
            for (const auto &c : t.cues) {
                QVariantMap cueMap;
                cueMap["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
                cueMap["hotCueNumber"] = c.hotCueNumber;
                cueMap["positionMs"] = c.positionMs;
                cueMap["color"] = QString::fromStdString(c.color);
                cues << cueMap;
            }
            trackMap["cues"] = cues;
            trackMap["durationMs"] = t.durationSeconds * 1000.0;

            result << trackMap;
        }
        return result;
    }
    case WastedBytesRole:
        return QString("%1 could be freed if this were on the stick once").arg(humanSize(wastedBytes(plan)));
    case StagedRole:
        return row < m_stagedDescriptions.size() && !m_stagedDescriptions[row].isEmpty();
    case StagedDescriptionRole:
        return row < m_stagedDescriptions.size() ? m_stagedDescriptions[row] : QString();
    case AlignmentNoteRole:
        return alignmentNote(plan.group);
    default:
        return {};
    }
}

QHash<int, QByteArray> ConsolidationPlanListModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {FilenameRole, "filename"},
        {DescriptionRole, "description"},
        {ActionableRole, "actionable"},
        {TracksRole, "tracks"},
        {WastedBytesRole, "wastedBytesDescription"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
        {AlignmentNoteRole, "alignmentNote"},
    };
}

void ConsolidationPlanListModel::setPlans(std::vector<domain::ConsolidationPlan> plans)
{
    beginResetModel();
    m_plans = std::move(plans);
    m_stagedDescriptions.assign(m_plans.size(), QString());
    endResetModel();
}

void ConsolidationPlanListModel::removePlanAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_plans.erase(m_plans.begin() + index);
    m_stagedDescriptions.erase(m_stagedDescriptions.begin() + index);
    endRemoveRows();
}

void ConsolidationPlanListModel::setStaged(int index, bool staged, const QString &description)
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return;
    }
    m_stagedDescriptions[static_cast<size_t>(index)] = staged ? description : QString();
    emit dataChanged(this->index(index), this->index(index), {StagedRole, StagedDescriptionRole});
}

void ConsolidationPlanListModel::clearStaged()
{
    if (m_plans.empty()) {
        return;
    }
    std::fill(m_stagedDescriptions.begin(), m_stagedDescriptions.end(), QString());
    emit dataChanged(index(0), index(static_cast<int>(m_plans.size()) - 1), {StagedRole, StagedDescriptionRole});
}

namespace
{

// Mirrors cli/main.cpp's handleDuplicates writer wiring exactly, so a
// GUI-applied consolidation behaves identically to the CLI's. One per
// format per save (SaveContext::shared), except OneLibrary, whose adapter
// needs the tracks it is about to write (see below).

// Runs entirely on a background thread (see DuplicatesController::
// startRescan()) - no access to the controller itself.
DuplicatesTaskResult runRescanTask(QString format, QString path, std::shared_ptr<QtProgressReporter> reporter,
                                   application::CancellationToken cancel)
{
    DuplicatesTaskResult result;
    try {
        // One bar (#58): the read the cache still has to make, then the
        // grouping, which has no count and is one unit at the end.
        // Then lining up the copies' audio, which is counted in pairs
        // nobody knows before the grouping, and so is planned as a share
        // (application::cueShiftProgressUnits()).
        auto &cache = LibraryCatalogCache::instance();
        const auto readUnits = cache.plannedUnits(format.toStdString(), path.toStdString(),
                                                  LibraryCatalogCache::Detail::Full, cancel);
        const size_t shiftUnits = application::cueShiftProgressUnits(readUnits.value_or(0));
        application::PhasedProgress progress(*reporter, "Looking for duplicates",
                                             readUnits ? *readUnits + 1 + shiftUnits : 0);
        std::vector<domain::Track> tracks =
            cache.tracksFor(format.toStdString(), path.toStdString(), progress, cancel);

        // Streaming tracks (Engine/TIDAL) have no real local file.
        // Never propose "syncing" cues onto/from one. See
        // domain::Track::streamingSource's own doc comment for why.
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                     tracks.end());

        // No per-item progress for this pass (it's not a simple linear
        // scan), but the label change alone is the actual fix: without
        // it, the progress bar sat frozen at 100% -- the raw file scan's
        // own end state -- for however long grouping took on a real
        // library, with nothing telling the user it was still working.
        progress.start("Finding duplicates...", 0);
        // Same probe Clean Up Duplicates builds, so the two pages group
        // by one rule rather than two. Null when the user left no window
        // for comparing audio or this build has no decoder, which is the
        // behaviour this page has always had.
        auto audioProbe = infrastructure::audio::makeAudioContentProbe(
            pathToUtf8(pathFromQString(path).parent_path()));
        result.audioComparisonUnavailable = audioProbe == nullptr
            && domain::MatchingPolicy::compareAudioSeconds() > domain::MatchingPolicy::exactMatchSeconds();
        // Cancel has to reach the decoding, which is the slowest thing
        // here and which the finder itself knows nothing about. See
        // CancellableAudioContentProbe.
        CancellableAudioContentProbe cancellableProbe(audioProbe.get(), cancel);
        auto recordAudioComparison = [&]() {
            if (audioProbe == nullptr) {
                return;
            }
            result.filesAudioCompared = audioProbe->decodedCount();
            result.filesAudioUnreadable = audioProbe->failedCount();
            if (result.filesAudioCompared == 0 && result.filesAudioUnreadable > 0) {
                result.audioComparisonUnavailable = true;
            }
            // Explicit rather than left to the destructor: a stick that
            // refuses the write is a thing this scan saw.
            audioProbe->save();
        };
        // Every cue this page copies or compares crosses from one copy to
        // another, and copies of one song are often not sample-aligned:
        // each group with cues is lined up before it is planned. See
        // domain/duplicate_cue_shift.hpp.
        auto alignment = infrastructure::audio::makeAudioAlignmentProbe(
            pathToUtf8(pathFromQString(path).parent_path()));
        CancellableAudioAlignmentProbe cancellableAlignment(alignment.get(), cancel);
        auto allPlans = application::ConsolidateDuplicateCues().execute(
            tracks, audioProbe != nullptr ? &cancellableProbe : nullptr,
            [&](std::vector<domain::DuplicateGroup> &groups) {
                // Before the long part, so a scan cancelled while lining
                // up still keeps what the grouping measured.
                recordAudioComparison();
                application::measureDuplicateCueShifts(groups, &cancellableAlignment,
                                                       infrastructure::audio::cueTimelineOffsetMs, progress,
                                                       shiftUnits, cancel);
            });
        result.pairsNotLinedUp = alignment->unknownCount();
        result.alignmentDecoderMissing = !alignment->hasDecoder();
        alignment->save();
        std::vector<ConsolidationPlan> actionable;
        for (auto &plan : allPlans) {
            // Unaligned is listed like Conflict: a group a person has to
            // look at, and one that vanished from the page would read as
            // a duplicate the scan never found.
            if (plan.kind == ConsolidationPlan::Kind::Unambiguous || plan.kind == ConsolidationPlan::Kind::Conflict
                || plan.kind == ConsolidationPlan::Kind::Unaligned) {
                actionable.push_back(std::move(plan));
            }
        }

        // Waveforms are deliberately NOT loaded here -- QML fetches one
        // on demand via PlaybackController::waveformFor() instead (see
        // ConsolidationPlanListModel::setPlans()'s own comment for why:
        // eagerly decoding one per displayed track here was the same
        // shape of bug confirmed to make Sync's own "scanning" phase
        // take 5+ minutes on a real library).
        result.plans = std::move(actionable);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

DuplicatesController::DuplicatesController(QObject *parent) : StagedCueEditController(parent) {}

QString DuplicatesController::totalWastedBytesHuman() const
{
    std::uint64_t total = 0;
    for (const auto &plan : m_model.plans()) {
        total += wastedBytes(plan);
    }
    return humanSize(total);
}

QString ConsolidationPlanListModel::planKeyAt(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return {};
    }
    QStringList ids;
    for (const auto &t : m_plans[static_cast<size_t>(index)].group.tracks) {
        ids << QString::fromStdString(t.sourceId);
    }
    ids.sort();
    return ids.join('+');
}

// The base wires the session's state and staged-change signals; the only
// part specific to this page is which of the two library paths the
// session should be told about, which depends on the format toggle.
//
// A row whose change lands is dropped rather than re-derived: once its
// cues are copied the group is AlreadyConsistent, which this model never
// shows, and DuplicateTrackFinder groups by filename/title+artist+
// duration only -- never cues -- so no other group's classification can
// change either. See StagedCueEditController::onSessionChangeApplied().
void DuplicatesController::attachSession()
{
    attachSessionForPath(m_path);
    if (LibraryEditSession *s = session()) {
        if (m_format == "engine") {
            s->setLibraryPaths(QString(), m_path);
        } else {
            s->setLibraryPaths(m_path, QString());
        }
    }
}

void DuplicatesController::scan(const QString &format, const QString &path)
{
    // Always answered (docs/async-requests.md): the same library again by
    // the scan already running, another by a new scan that supersedes it.
    // Setting the scope and then dropping the rescan while busy, as this
    // did, put the old format's groups under the new format's session.
    m_format = format;
    m_path = path;
    attachSession();
    startRescan(false);
}

bool DuplicatesController::hasOneLibrary(const QString &pioneerRoot) const
{
    return infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.toStdString());
}

void DuplicatesController::startRescan(bool restart)
{
    const QString format = m_format;
    const QString path = m_path;
    const QString key = format + QLatin1Char('\n') + path;
    if (!restart && scanServes(key)) {
        return;
    }
    setErrorMessage({});
    setScanProgress(0, 0);
    auto reporter = makeReporter();
    startScan<DuplicatesTaskResult>(
        key, stickRootOf(path), restart,
        [format, path, reporter](application::CancellationToken cancel) {
            return runRescanTask(format, path, reporter, cancel);
        },
        [this](DuplicatesTaskResult &&result) { onRescanFinished(std::move(result)); });
}

void DuplicatesController::setAudioComparisonNote(const DuplicatesTaskResult &result)
{
    // Word for word what CleanupController says, because it is the same
    // rule doing the same work on the same files, and two pages
    // describing one thing differently is how a DJ ends up believing
    // they are two things.
    if (result.audioComparisonUnavailable) {
        m_audioComparisonNote = QStringLiteral(
            "No audio could be decoded here, so tracks whose lengths nearly agree were compared by "
            "their stored lengths alone.");
    } else if (result.filesAudioCompared > 0) {
        m_audioComparisonNote = QStringLiteral("Compared the audio of %1 file%2 whose stored lengths "
                                                "were close but not close enough to settle it.")
                                     .arg(result.filesAudioCompared)
                                     .arg(result.filesAudioCompared == 1 ? "" : "s");
        if (result.filesAudioUnreadable > 0) {
            m_audioComparisonNote += QStringLiteral(" %1 more could not be read and was left ungrouped.")
                                          .arg(result.filesAudioUnreadable);
        }
    } else {
        m_audioComparisonNote.clear();
    }
    // Copies that could not be lined up are each said on their own row;
    // this says why when the reason is the build rather than the files.
    if (result.pairsNotLinedUp > 0 && result.alignmentDecoderMissing) {
        if (!m_audioComparisonNote.isEmpty()) {
            m_audioComparisonNote += QLatin1Char(' ');
        }
        m_audioComparisonNote += QStringLiteral(
            "This build cannot decode audio, so copies that are not identical files could not be lined up, "
            "and no cue is moved between them.");
    }
}

void DuplicatesController::onRescanFinished(DuplicatesTaskResult &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }

    setAudioComparisonNote(result);
    m_model.setPlans(std::move(result.plans));
    // Groups staged before this rescan keep their mark if they are still
    // listed (the change itself lives in the session either way).
    for (const auto &[groupKey, info] : m_stagedByKey) {
        int index = indexOfStagedKey(groupKey);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    emit plansChanged();
}

void DuplicatesController::stageCopy(int index, const DuplicatesCopyOp &op)
{
    if (!session()) {
        attachSession();
    }
    QString groupKey = m_model.planKeyAt(index);
    if (stageChange(index, groupKey, std::make_unique<CopyCuesChange>(m_format, m_path, groupKey, op))) {
        emit plansChanged();
    }
}

void DuplicatesController::applyOne(int index)
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    const auto &plans = m_model.plans();
    if (index < 0 || static_cast<size_t>(index) >= plans.size()) {
        return;
    }
    const auto &plan = plans[static_cast<size_t>(index)];
    if (plan.kind != ConsolidationPlan::Kind::Unambiguous) {
        return;
    }
    stageCopy(index, {*plan.source, plan.targets, plan.targetShiftMs});
}

void DuplicatesController::copyFromTrack(int index, const QString &sourceTrackId)
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    const auto &plans = m_model.plans();
    if (index < 0 || static_cast<size_t>(index) >= plans.size()) {
        return;
    }
    const auto &group = plans[static_cast<size_t>(index)].group;

    std::string wantedId = sourceTrackId.toStdString();
    std::optional<size_t> source;
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        if (group.tracks[i].sourceId == wantedId) {
            source = i;
        }
    }
    if (!source) {
        return;
    }
    // Each target gets the chosen copy's cues moved onto its own timing.
    // A copy that could not be lined up has no known place for them, and
    // a person choosing which copy's cues to keep is not choosing where
    // the sounds are, so that is refused rather than guessed.
    std::vector<domain::Track> targets;
    std::vector<double> shifts;
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        if (group.tracks[i].sourceId == group.tracks[*source].sourceId) {
            continue;
        }
        const std::optional<double> shift = domain::cueShiftBetween(group, *source, i);
        if (!shift) {
            setErrorMessage(QStringLiteral("These copies could not be lined up, so cues cannot be copied between "
                                           "them without guessing where they belong."));
            return;
        }
        targets.push_back(group.tracks[i]);
        shifts.push_back(*shift);
    }
    stageCopy(index, {group.tracks[*source], std::move(targets), std::move(shifts)});
}

void DuplicatesController::applyAllUnambiguous()
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    const auto &plans = m_model.plans();
    int staged = 0;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (plans[i].kind == ConsolidationPlan::Kind::Unambiguous) {
            stageCopy(static_cast<int>(i), {*plans[i].source, plans[i].targets, plans[i].targetShiftMs});
            staged++;
            if (session() && !session()->lockHeld()) {
                return;  // refused at the first one; no point trying the rest
            }
        }
    }
    if (staged > 0) {
        setStagedStatusMessage(
            QStringLiteral("Staged %1 group(s). Press Save to copy the cues onto the stick.").arg(staged));
    }
}

}  // namespace seabass::gui
