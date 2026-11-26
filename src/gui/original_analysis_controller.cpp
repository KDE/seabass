// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/original_analysis_controller.hpp"

#include <QThread>
#include <QtConcurrent>

#include <chrono>
#include <cstdio>
#include <filesystem>

#include "application/use_cases/carry_cues_to_new_grid.hpp"
#include "gui/analysis_settings.hpp"
#include "gui/async_request.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

constexpr std::chrono::milliseconds LetGoAfter{1000};

// As cleanup_controller.cpp's, for the same reason it gives.
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

template <typename Watcher>
void waitBriefly(const Watcher &watcher)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + (AsyncWorkers::instance().shuttingDown() ? std::chrono::milliseconds(0) : LetGoAfter);
    while (watcher.future().isValid() && !watcher.future().isFinished() && std::chrono::steady_clock::now() < deadline) {
        QThread::msleep(5);
    }
}

}  // namespace

OriginalAnalysisController::OriginalAnalysisController(QObject *parent) : QObject(parent)
{
    connect(&m_prepareWatcher, &QFutureWatcher<PreparedAnalysisRestore>::finished, this,
            &OriginalAnalysisController::onPrepared);
    connect(&m_countWatcher, &QFutureWatcher<int>::finished, this, &OriginalAnalysisController::onCounted);
}

OriginalAnalysisController::~OriginalAnalysisController()
{
    // The workers write nothing and hold their own copies of what they
    // read: told to stop, waited for a moment, then let go.
    m_cancel.cancel();
    waitBriefly(m_prepareWatcher);
    waitBriefly(m_countWatcher);
    disconnect(&m_prepareWatcher, nullptr, this, nullptr);
    disconnect(&m_countWatcher, nullptr, this, nullptr);
}

void OriginalAnalysisController::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void OriginalAnalysisController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

bool OriginalAnalysisController::attachSession(const QString &rekordboxPath, const QString &enginePath)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session =
        registry->sessionFor(registry->libraryIdForPath(rekordboxPath.isEmpty() ? enginePath : rekordboxPath));
    if (!session) {
        setErrorMessage(QStringLiteral("This stick cannot be edited."));
        return false;
    }
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_staged.clear();
        m_session = session;
        connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
            const auto before = m_staged.size();
            std::erase_if(m_staged, [&](const auto &change) { return change->id() == changeId; });
            if (m_staged.size() != before) {
                emit stagedChanged();
                emit cueReviewChanged();
            }
        });
        connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
            m_staged.clear();
            emit stagedChanged();
            emit cueReviewChanged();
        });
        connect(m_session, &LibraryEditSession::pendingChanged, this, &OriginalAnalysisController::prune);
    }
    return true;
}

void OriginalAnalysisController::prune()
{
    if (!m_session || m_session->writing()) {
        return;
    }
    const auto before = m_staged.size();
    std::erase_if(m_staged, [this](const auto &change) { return !m_session->hasChange(change->id()); });
    if (m_staged.size() != before) {
        emit stagedChanged();
        emit cueReviewChanged();
    }
}

void OriginalAnalysisController::refreshKeptCount(const QString &rekordboxPath, const QString &enginePath,
                                                  const QString &playlist)
{
    AnalysisRestoreRequest request;
    request.rekordboxPath = rekordboxPath;
    request.enginePath = enginePath;
    request.playlist = playlist.toStdString();
    if (m_countWatcher.future().isValid() && !m_countWatcher.future().isFinished()) {
        // One at a time: the newest question is asked when this one lands.
        m_countNext = request;
        return;
    }
    m_countingPlaylist = playlist;
    m_countWatcher.setFuture(runRead([request, cancel = m_cancel]() { return countKeptOriginals(request, cancel); }));
}

void OriginalAnalysisController::onCounted()
{
    QString thrown;
    const int count = takeResult(m_countWatcher, &thrown);
    if (m_countNext) {
        const AnalysisRestoreRequest next = *m_countNext;
        m_countNext.reset();
        refreshKeptCount(next.rekordboxPath, next.enginePath, QString::fromStdString(next.playlist));
        return;
    }
    m_keptCount = thrown.isEmpty() ? count : -1;
    m_keptCountPlaylist = m_countingPlaylist;
    emit keptCountChanged();
}

bool OriginalAnalysisController::restoreOriginalAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                                         const QString &playlist)
{
    setErrorMessage({});
    if (m_busy) {
        setErrorMessage(QStringLiteral("The original analysis is being looked up already."));
        return false;
    }
    if (playlist.isEmpty()) {
        setErrorMessage(QStringLiteral("Restore original analysis takes the tracks of a playlist."));
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    if (m_session->writing()) {
        setErrorMessage(QStringLiteral("A save is writing to this stick. Restore when it has finished."));
        return false;
    }
    AnalysisRestoreRequest request;
    request.rekordboxPath = rekordboxPath;
    request.enginePath = enginePath;
    request.playlist = playlist.toStdString();
    request.cueFollowThresholdMs = storedCueFollowThresholdMs(openSeabassSettings());
    m_request = request;
    m_preparingPlaylist = playlist;
    m_cancel = application::CancellationToken();
    setBusy(true);
    m_prepareWatcher.setFuture(
        runRead([request, cancel = m_cancel]() { return prepareAnalysisRestore(request, cancel); }));
    return true;
}

void OriginalAnalysisController::onPrepared()
{
    QString thrown;
    PreparedAnalysisRestore prepared = takeResult(m_prepareWatcher, &thrown);
    const QString playlist = m_preparingPlaylist;
    m_preparingPlaylist.clear();
    setBusy(false);
    if (m_cancel.cancelled()) {
        return;
    }
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("The original analysis could not be looked up: %1").arg(thrown));
        return;
    }
    if (!prepared.refusal.empty()) {
        setErrorMessage(QString::fromStdString(prepared.refusal));
        return;
    }
    if (!attachSession(m_request.rekordboxPath, m_request.enginePath)) {
        return;
    }
    // A track whose Analyse again waits to be saved keeps waiting: the two
    // would write over each other in one save.
    std::vector<std::shared_ptr<RestoreAnalysisChange>> changes;
    for (auto &change : prepared.changes) {
        if (m_session->hasChange(ReanalyseTrackChange::idFor(change->stickPath()))) {
            prepared.leftAlone.push_back({change->audioFile(), "Analyse again of it waits to be saved: save it or take "
                                                               "it back first"});
        } else {
            changes.push_back(change);
        }
    }
    if (!changes.empty()) {
        std::vector<std::shared_ptr<PendingChange>> pending(changes.begin(), changes.end());
        if (!m_session->stageAll(std::move(pending))) {
            setErrorMessage(QStringLiteral("The restores could not be staged on this stick."));
            return;
        }
        std::erase_if(m_staged, [&](const auto &earlier) {
            return std::any_of(changes.begin(), changes.end(), [&](const auto &c) { return c->id() == earlier->id(); });
        });
        m_staged.insert(m_staged.end(), changes.begin(), changes.end());
        emit stagedChanged();
    }
    int cueTracks = 0;
    int cuesToMove = 0;
    int autoTracks = 0;
    for (const auto &change : changes) {
        const int moves = static_cast<int>(change->cueReview().plan.moves.size());
        cueTracks += moves > 0 ? 1 : 0;
        cuesToMove += moves;
        autoTracks += moves > 0 && change->movesOnTheirOwnUnderMs() > 0 ? 1 : 0;
    }
    QVariantList leftAlone;
    for (const LeftOut &left : prepared.leftAlone) {
        leftAlone << QVariantMap{{QStringLiteral("name"), pathToQString(pathFromUtf8(left.sourceFile).filename())},
                                 {QStringLiteral("why"), QString::fromStdString(left.why)}};
    }
    m_lastRestore = QVariantMap{
        {QStringLiteral("playlist"), playlist},
        {QStringLiteral("staged"), static_cast<int>(changes.size())},
        {QStringLiteral("tracks"), prepared.tracks},
        {QStringLiteral("leftAlone"), leftAlone},
        {QStringLiteral("cueTracks"), cueTracks},
        {QStringLiteral("cuesToMove"), cuesToMove},
        {QStringLiteral("autoCueTracks"), autoTracks},
        {QStringLiteral("askedCueTracks"), cueTracks - autoTracks},
        {QStringLiteral("cueFollowThresholdMs"), m_request.cueFollowThresholdMs},
    };
    emit cueReviewChanged();
    emit restoreReady();
}

QVariantList OriginalAnalysisController::cueReview() const
{
    QVariantList rows;
    for (const auto &change : m_staged) {
        const auto &plan = change->cueReview().plan;
        if (plan.moves.empty()) {
            continue;
        }
        rows << QVariantMap{{QStringLiteral("changeId"), change->id()},
                            {QStringLiteral("subject"), change->subject()},
                            {QStringLiteral("moves"), static_cast<int>(plan.moves.size())},
                            {QStringLiteral("largestMoveMs"), static_cast<int>(application::largestCueMoveMs(plan))},
                            {QStringLiteral("moveCues"), change->moveCues()},
                            {QStringLiteral("chosen"), change->moveCuesChosen()}};
    }
    return rows;
}

bool OriginalAnalysisController::setMoveCuesBack(const QString &changeId, bool move)
{
    if (m_session && m_session->writing()) {
        return false;
    }
    for (const auto &change : m_staged) {
        if (change->id() == changeId) {
            change->setMoveCues(move);
        }
    }
    emit cueReviewChanged();
    return true;
}

bool OriginalAnalysisController::setMoveCuesBackForAll(bool move)
{
    if (m_session && m_session->writing()) {
        return false;
    }
    for (const auto &change : m_staged) {
        if (!change->cueReview().plan.moves.empty()) {
            change->setMoveCues(move);
        }
    }
    emit cueReviewChanged();
    return true;
}

void OriginalAnalysisController::dropStaged()
{
    if (!m_session || m_session->writing() || m_staged.empty()) {
        return;
    }
    QStringList ids;
    for (const auto &change : m_staged) {
        ids << change->id();
    }
    m_session->unstageAll(ids);
    m_staged.clear();
    emit stagedChanged();
    emit cueReviewChanged();
}

// Free Up Space.

std::uint64_t KeptAnalysisSpaceController::Group::bytes() const
{
    std::uint64_t sum = 0;
    for (const auto &file : files) {
        sum += file.bytes;
    }
    return sum;
}

KeptAnalysisSpaceController::KeptAnalysisSpaceController(QObject *parent) : QObject(parent)
{
    connect(&m_listWatcher, &QFutureWatcher<Listing>::finished, this, &KeptAnalysisSpaceController::onListed);
    connect(&m_removeWatcher, &QFutureWatcher<Removal>::finished, this, &KeptAnalysisSpaceController::onRemoved);
}

KeptAnalysisSpaceController::~KeptAnalysisSpaceController()
{
    waitBriefly(m_listWatcher);
    // A removal runs to its end: it holds the lock, and a file is either
    // removed or not.
    awaitQuietly(m_removeWatcher);
    disconnect(&m_listWatcher, nullptr, this, nullptr);
    disconnect(&m_removeWatcher, nullptr, this, nullptr);
}

namespace
{

bool rekordboxTrackGone(const infrastructure::local::KeptAnalysisFileInfo &info, const fs::path &root)
{
    if (!info.readable || info.trackFile.size() < 2 || info.trackFile.front() != '/') {
        return true;
    }
    std::error_code ec;
    return !fs::is_regular_file(root / pathFromUtf8(info.trackFile.substr(1)), ec);
}

bool engineTrackGone(const infrastructure::local::KeptAnalysisFileInfo &info, const fs::path &engineLibrary)
{
    if (!info.readable || info.trackFile.empty()) {
        return true;
    }
    std::error_code ec;
    return !fs::is_regular_file((engineLibrary / pathFromUtf8(info.trackFile)).lexically_normal(), ec);
}

}  // namespace

KeptAnalysisSpaceController::Listing KeptAnalysisSpaceController::listKept(const std::string &stickRoot,
                                                                           const std::string &enginePath)
{
    Listing listing;
    const fs::path root = pathFromUtf8(stickRoot);
    const fs::path engineLibrary = enginePath.empty() ? root / "Engine Library" : pathFromUtf8(enginePath);
    Group rekordbox{QStringLiteral("rekordbox"), false, {}, false};
    Group engine{QStringLiteral("engine"), false, {}, false};
    Group rekordboxGone{QStringLiteral("rekordbox"), true, {}, true};
    Group engineGone{QStringLiteral("engine"), true, {}, true};
    std::string error;
    for (auto &info : infrastructure::local::listKeptRekordboxAnalyses(root, &error)) {
        (rekordboxTrackGone(info, root) ? rekordboxGone : rekordbox).files.push_back(std::move(info));
    }
    std::string engineError;
    for (auto &info : infrastructure::local::listKeptEngineAnalyses(root, &engineError)) {
        (engineTrackGone(info, engineLibrary) ? engineGone : engine).files.push_back(std::move(info));
    }
    const std::string both = error.empty() ? engineError : engineError.empty() ? error : error + "; " + engineError;
    listing.error = QString::fromStdString(both);
    for (Group *group : {&rekordbox, &engine, &rekordboxGone, &engineGone}) {
        if (!group->files.empty()) {
            listing.groups.push_back(std::move(*group));
        }
    }
    return listing;
}

QVariantList KeptAnalysisSpaceController::groups() const
{
    QVariantList rows;
    for (const Group &group : m_groups) {
        rows << QVariantMap{{QStringLiteral("kind"), group.kind},
                            {QStringLiteral("gone"), group.gone},
                            {QStringLiteral("count"), static_cast<int>(group.files.size())},
                            {QStringLiteral("bytes"), static_cast<qlonglong>(group.bytes())},
                            {QStringLiteral("sizeHuman"), humanSize(group.bytes())},
                            {QStringLiteral("included"), group.included}};
    }
    return rows;
}

int KeptAnalysisSpaceController::totalCount() const
{
    int n = 0;
    for (const Group &group : m_groups) {
        n += static_cast<int>(group.files.size());
    }
    return n;
}

QString KeptAnalysisSpaceController::totalBytesHuman() const
{
    std::uint64_t sum = 0;
    for (const Group &group : m_groups) {
        sum += group.bytes();
    }
    return humanSize(sum);
}

int KeptAnalysisSpaceController::includedCount() const
{
    int n = 0;
    for (const Group &group : m_groups) {
        n += group.included ? static_cast<int>(group.files.size()) : 0;
    }
    return n;
}

QString KeptAnalysisSpaceController::includedBytesHuman() const
{
    std::uint64_t sum = 0;
    for (const Group &group : m_groups) {
        sum += group.included ? group.bytes() : 0;
    }
    return humanSize(sum);
}

void KeptAnalysisSpaceController::refresh(const QString &rekordboxPath, const QString &enginePath)
{
    if (busy()) {
        return;
    }
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    const QString any = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    if (any.isEmpty()) {
        return;
    }
    m_listing = true;
    emit busyChanged();
    const std::string stickRoot = infrastructure::paths::normalStickRootForCatalogPath(any.toStdString());
    m_listWatcher.setFuture(runRead([stickRoot, engine = enginePath.toStdString()]() { return listKept(stickRoot, engine); }));
}

void KeptAnalysisSpaceController::onListed()
{
    QString thrown;
    Listing listing = takeResult(m_listWatcher, &thrown);
    m_listing = false;
    m_groups = std::move(listing.groups);
    m_error = thrown.isEmpty() ? listing.error : thrown;
    emit busyChanged();
    emit groupsChanged();
}

void KeptAnalysisSpaceController::setIncluded(int index, bool included)
{
    if (busy() || index < 0 || index >= static_cast<int>(m_groups.size())) {
        return;
    }
    m_groups[static_cast<std::size_t>(index)].included = included;
    emit groupsChanged();
}

void KeptAnalysisSpaceController::removeIncluded()
{
    if (busy() || includedCount() == 0) {
        return;
    }
    const QString path = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    const QString libraryId = EditSessionRegistry::instance()->libraryIdForPath(path);
    if (auto refusal = m_writeHold.acquire({libraryId}, QString())) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder);
        }
        return;
    }
    std::vector<Group> chosen;
    for (const Group &group : m_groups) {
        if (group.included) {
            chosen.push_back(group);
        }
    }
    m_removing = true;
    emit busyChanged();
    const fs::path root = pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(path.toStdString()));
    const fs::path engineLibrary = m_enginePath.isEmpty() ? root / "Engine Library" : pathFromUtf8(m_enginePath.toStdString());
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Removing kept analyses from a USB stick"));
    m_removeWatcher.setFuture(QtConcurrent::run([keepAwake, chosen = std::move(chosen), root, engineLibrary]() {
        Removal removal;
        for (const Group &group : chosen) {
            for (const auto &info : group.files) {
                ++removal.total;
                // A track gone when the page listed it may be back: its
                // kept analysis is then one to restore from, not to lose.
                if (group.gone && !(group.kind == QLatin1String("rekordbox") ? rekordboxTrackGone(info, root)
                                                                              : engineTrackGone(info, engineLibrary))) {
                    ++removal.failed;
                    continue;
                }
                std::string failure;
                if (infrastructure::removeEntry(pathFromUtf8(info.file), failure)) {
                    infrastructure::fsyncDirectoryContaining(info.file);
                    ++removal.removed;
                } else {
                    ++removal.failed;
                    if (removal.error.isEmpty()) {
                        removal.error = QString::fromStdString(failure);
                    }
                }
            }
        }
        return removal;
    }));
}

void KeptAnalysisSpaceController::onRemoved()
{
    QString thrown;
    const Removal removal = takeResult(m_removeWatcher, &thrown);
    m_writeHold.release();
    m_removing = false;
    emit busyChanged();
    QVariantMap summary{
        {QStringLiteral("written"), removal.removed},
        {QStringLiteral("total"), removal.total},
        {QStringLiteral("unit"), QStringLiteral("kept analyses")},
        {QStringLiteral("verb"), QStringLiteral("removed")},
        {QStringLiteral("cancelled"), false},
        {QStringLiteral("error"), thrown},
    };
    if (removal.failed > 0) {
        summary.insert(QStringLiteral("warning"),
                       QStringLiteral("%1 kept analyses were not removed and are still on the stick%2.")
                           .arg(removal.failed)
                           .arg(removal.error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(removal.error)));
    }
    emit removeFinished(summary);
    refresh(m_rekordboxPath, m_enginePath);
}

}  // namespace seabass::gui
