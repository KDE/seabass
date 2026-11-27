// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/cue_alignment_controller.hpp"

#include <QSettings>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "gui/edit/changes/align_cues_change.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr char ToleranceKey[] = "maintain/cueAlignToleranceMs";

// A rekordbox or OneLibrary row's grid, on rekordbox's timeline, where its
// cues are too: from the analysis file the row names, which is also where
// its cues are kept, else (a DeviceLibrary row naming none) by its id.
std::vector<double> rekordboxGridOf(const domain::Track &track, const fs::path &stickRoot, const std::string &pioneerRoot)
{
    try {
        if (!track.analysisFile.empty()) {
            const fs::path dat = stickRoot / pathFromUtf8(track.analysisFile.substr(track.analysisFile.front() == '/' ? 1 : 0));
            std::error_code ec;
            if (fs::is_regular_file(dat, ec)) {
                return infrastructure::rekordbox::pqtzBeatTimes(infrastructure::rekordbox::AnlzFile::readRaw(pathToUtf8(dat)));
            }
        }
        std::vector<double> beats;
        if (track.format == "rekordbox") {
            for (const domain::Beat &beat : infrastructure::rekordbox::readBeatGrid(pioneerRoot, track.sourceId)) {
                beats.push_back(beat.timeMs);
            }
        }
        return beats;
    } catch (const std::exception &) {
        return {};
    }
}

// A cue as its library names it: rekordbox's pads by letter, Engine's by
// number (pad 1 is hot cue A).
QString cueName(const domain::Track &track, const domain::CuePoint &cue, bool loopEnd)
{
    const bool hot = cue.kind == domain::CuePoint::Kind::Hot;
    const int number = std::clamp(cue.hotCueNumber, 1, 26);
    QString name;
    if (!hot) {
        name = cue.isLoop ? QStringLiteral("Memory loop") : QStringLiteral("Memory cue");
    } else if (track.format == "engine") {
        name = (cue.isLoop ? QStringLiteral("Loop on pad %1") : QStringLiteral("Pad %1")).arg(number);
    } else {
        name = (cue.isLoop ? QStringLiteral("Loop %1") : QStringLiteral("Hot cue %1"))
                   .arg(QChar(QLatin1Char(static_cast<char>('A' + number - 1))));
    }
    return cue.isLoop && loopEnd ? name + QStringLiteral(" (end)") : name;
}

int wholeMs(double offMs)
{
    return std::max(1, static_cast<int>(std::lround(std::abs(offMs))));
}

}  // namespace

std::shared_ptr<const CueAlignmentScan> scanCueAlignment(const QString &rekordboxPath, const QString &enginePath,
                                                         application::CancellationToken cancel)
{
    auto scan = std::make_shared<CueAlignmentScan>();
    const std::string pioneerRoot = rekordboxPath.toStdString();
    const std::string engineRoot = enginePath.toStdString();
    const fs::path stickRoot = pioneerRoot.empty() ? pathFromUtf8(engineRoot).parent_path() : pathFromUtf8(pioneerRoot).parent_path();

    // A DeviceLibrary row and a OneLibrary row that name one analysis file
    // are one cue source (see domain::Track::analysisFile): listed once,
    // as the DeviceLibrary row, whose write mirrors into OneLibrary.
    std::set<std::string> rekordboxAnalysisFiles;
    const auto measure = [&](const std::string &format, const QString &path) {
        const std::vector<domain::Track> tracks =
            LibraryCatalogCache::instance().tracksFor(format, path.toStdString(), LibraryCatalogCache::Detail::Cues,
                                                      application::NullProgressReporter::instance(), cancel);
        scan->formats << QString::fromStdString(format);
        for (const domain::Track &track : tracks) {
            cancel.throwIfCancelled();
            if (format == "rekordbox" && !track.analysisFile.empty()) {
                rekordboxAnalysisFiles.insert(track.analysisFile);
            }
            if (track.cues.empty()) {
                continue;
            }
            if (format == "onelibrary" && rekordboxAnalysisFiles.count(track.analysisFile) > 0) {
                continue;
            }
            ++scan->tracksWithCues;
            // Cues whose place rests on a guessed sample rate, and an
            // Engine row still holding rekordbox's positions as its own
            // import left them, cannot be measured against Engine's grid.
            if (track.cueRateUnverified || (format == "engine" && track.engineCuesOnRekordboxTimeline)) {
                continue;
            }
            MeasuredTrack measured;
            std::vector<double> beats;
            if (format == "engine") {
                for (const domain::Beat &beat : infrastructure::engine::readBeatGrid(engineRoot, track.sourceId)) {
                    beats.push_back(beat.timeMs);
                }
                // Engine's grid is on Engine's timeline; its cues were
                // read onto rekordbox's.
                measured.gridShiftMs = track.cueTimelineShiftMs;
            } else {
                beats = rekordboxGridOf(track, stickRoot, pioneerRoot);
            }
            if (beats.size() < 2) {
                continue;
            }
            ++scan->tracksMeasured;
            std::vector<domain::CuePoint> onGrid = track.cues;
            for (domain::CuePoint &cue : onGrid) {
                cue.positionMs -= measured.gridShiftMs;
                if (cue.isLoop) {
                    cue.loopEndMs -= measured.gridShiftMs;
                }
            }
            measured.alignment = domain::measureCuesAgainstBeats(onGrid, beats);
            // Kept only when some tolerance the page can set lists it.
            if (domain::offBeatWithin(measured.alignment, CueAlignmentController::MaximumToleranceMs).empty()) {
                continue;
            }
            if (std::abs(measured.gridShiftMs) > domain::CueOnBeatWithinMs) {
                int off = 0;
                bool all = true;
                for (const domain::CueBeatDistance &point : measured.alignment.points) {
                    if (point.loopEnd || std::abs(point.offMs) <= domain::CueOnBeatWithinMs) {
                        continue;
                    }
                    ++off;
                    all = all && std::abs(std::abs(point.offMs) - std::abs(measured.gridShiftMs)) <= 2.5;
                }
                measured.offByLibraryOffset = off > 0 && all;
            }
            measured.key = junkKeyFor(track);
            measured.path = path;
            measured.track = track;
            scan->tracks.push_back(std::move(measured));
        }
    };

    std::error_code ec;
    if (!pioneerRoot.empty() && fs::exists(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb", ec)) {
        measure("rekordbox", rekordboxPath);
    }
    if (!pioneerRoot.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot)) {
        measure("onelibrary", rekordboxPath);
    }
    if (!engineRoot.empty() && fs::exists(infrastructure::engine::engineMainDatabasePath(stickRoot), ec)) {
        measure("engine", enginePath);
    }
    std::stable_sort(scan->tracks.begin(), scan->tracks.end(), [](const MeasuredTrack &a, const MeasuredTrack &b) {
        return a.track.title != b.track.title ? a.track.title < b.track.title : a.track.format < b.track.format;
    });
    return scan;
}

// ---- The list ----

CueAlignmentListModel::CueAlignmentListModel(QObject *parent) : QAbstractListModel(parent) {}

int CueAlignmentListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CueAlignmentListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const Row &row = m_rows[static_cast<std::size_t>(index.row())];
    const MeasuredTrack &measured = trackOf(row);
    switch (role) {
    case KeyRole:
        return measured.key;
    case FormatRole:
        return QString::fromStdString(measured.track.format);
    case TitleRole:
        return QString::fromStdString(measured.track.title);
    case ArtistRole:
        return QString::fromStdString(measured.track.artist);
    case OffCountRole:
        return row.offCount;
    case LargestOffMsRole:
        return row.largestOffMs;
    case DetailRole:
        return row.detail;
    case SameOffsetRole:
        return measured.held();
    case LibraryOffsetRole:
        return measured.offByLibraryOffset;
    case TickedRole:
        return row.ticked;
    case StagedRole:
        return row.staged;
    }
    return {};
}

QHash<int, QByteArray> CueAlignmentListModel::roleNames() const
{
    return {
        {KeyRole, "key"},         {FormatRole, "format"},         {TitleRole, "title"},
        {ArtistRole, "artist"},   {OffCountRole, "offCount"},     {LargestOffMsRole, "largestOffMs"},
        {DetailRole, "detail"},   {SameOffsetRole, "sameOffset"}, {TickedRole, "ticked"},
        {StagedRole, "staged"},   {LibraryOffsetRole, "libraryOffset"},
    };
}

QVariantMap CueAlignmentListModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= count()) {
        return map;
    }
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it) {
        map.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    }
    return map;
}

void CueAlignmentListModel::reset(const std::shared_ptr<const CueAlignmentScan> &scan, std::vector<Row> rows)
{
    beginResetModel();
    m_scan = scan;
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged();
}

void CueAlignmentListModel::setTicked(int row, bool ticked)
{
    if (row < 0 || row >= count() || m_rows[static_cast<std::size_t>(row)].ticked == ticked) {
        return;
    }
    m_rows[static_cast<std::size_t>(row)].ticked = ticked;
    emit dataChanged(index(row), index(row), {TickedRole});
}

void CueAlignmentListModel::setStaged(int row, bool staged)
{
    if (row < 0 || row >= count() || m_rows[static_cast<std::size_t>(row)].staged == staged) {
        return;
    }
    m_rows[static_cast<std::size_t>(row)].staged = staged;
    emit dataChanged(index(row), index(row), {StagedRole});
}

// ---- The controller ----

CueAlignmentController::CueAlignmentController(QObject *parent) : QObject(parent)
{
    QSettings settings;
    m_toleranceMs = std::clamp(settings.value(QLatin1String(ToleranceKey), DefaultToleranceMs).toInt(), MinimumToleranceMs,
                               MaximumToleranceMs);
}

CueAlignmentController::~CueAlignmentController() = default;

bool CueAlignmentController::writing() const
{
    return m_session && m_session->writing();
}

void CueAlignmentController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

void CueAlignmentController::setToleranceMs(int ms)
{
    ms = std::clamp(ms, MinimumToleranceMs, MaximumToleranceMs);
    // What is staged was staged at the tolerance it shows: it does not
    // change under it.
    if (ms == m_toleranceMs || !m_staged.isEmpty()) {
        return;
    }
    m_toleranceMs = ms;
    QSettings settings;
    settings.setValue(QLatin1String(ToleranceKey), ms);
    emit toleranceChanged();
    relist();
}

void CueAlignmentController::relist()
{
    std::vector<CueAlignmentListModel::Row> rows;
    QSet<QString> listed;
    if (m_result) {
        for (std::size_t i = 0; i < m_result->tracks.size(); ++i) {
            const MeasuredTrack &measured = m_result->tracks[i];
            const std::vector<domain::CueBeatDistance> off = domain::offBeatWithin(measured.alignment, m_toleranceMs);
            if (off.empty()) {
                continue;
            }
            CueAlignmentListModel::Row row;
            row.track = i;
            row.offCount = static_cast<int>(off.size());
            QStringList parts;
            for (const domain::CueBeatDistance &point : off) {
                row.largestOffMs = std::max(row.largestOffMs, wholeMs(point.offMs));
                parts << QStringLiteral("%1 %2 ms %3")
                             .arg(cueName(measured.track, measured.track.cues[point.cueIndex], point.loopEnd))
                             .arg(wholeMs(point.offMs))
                             .arg(point.offMs > 0 ? QStringLiteral("late") : QStringLiteral("early"));
            }
            row.detail = parts.join(QStringLiteral(", "));
            row.ticked = !measured.held() && m_ticked.contains(measured.key);
            row.staged = m_staged.contains(measured.key);
            listed.insert(measured.key);
            rows.push_back(std::move(row));
        }
    }
    // A track the tolerance no longer lists is no longer ticked.
    m_ticked.intersect(listed);
    m_model.reset(m_result, std::move(rows));
    emit listChanged();
}

int CueAlignmentController::cuePointCount() const
{
    int count = 0;
    for (const auto &row : m_model.rows()) {
        count += row.offCount;
    }
    return count;
}

int CueAlignmentController::tickableCount() const
{
    return static_cast<int>(std::count_if(m_model.rows().begin(), m_model.rows().end(), [this](const auto &row) {
        return !m_model.trackOf(row).held();
    }));
}

int CueAlignmentController::tickedCount() const
{
    return static_cast<int>(std::count_if(m_model.rows().begin(), m_model.rows().end(), [](const auto &row) { return row.ticked; }));
}

int CueAlignmentController::stagedCount() const
{
    return static_cast<int>(m_staged.size());
}

void CueAlignmentController::scan(const QString &rekordboxPath, const QString &enginePath)
{
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    setErrorMessage({});
    const QString any = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    const QString stickRoot = QString::fromStdString(pathToGenericUtf8(pathFromUtf8(any.toStdString()).parent_path()));
    AsyncRequest<std::shared_ptr<const CueAlignmentScan>>::Ending ending;
    ending.result = [this](std::shared_ptr<const CueAlignmentScan> &&result) {
        m_result = std::move(result);
        emit scanChanged();
        relist();
    };
    ending.error = [this](const QString &message) {
        setErrorMessage(QStringLiteral("Could not read the stick's cue points and beat grids: %1").arg(message));
    };
    ending.cancelled = [] {};
    m_scan.restart(QStringLiteral("cue-alignment:") + any, stickRoot,
                   [rekordboxPath, enginePath](application::CancellationToken cancel) {
                       return scanCueAlignment(rekordboxPath, enginePath, cancel);
                   },
                   std::move(ending));
}

void CueAlignmentController::cancelScan()
{
    m_scan.cancel();
}

void CueAlignmentController::setTicked(int row, bool ticked)
{
    if (row < 0 || row >= m_model.count() || !m_staged.isEmpty()) {
        return;
    }
    const MeasuredTrack &measured = m_model.trackOf(m_model.rows()[static_cast<std::size_t>(row)]);
    if (measured.held()) {
        return;
    }
    if (ticked) {
        m_ticked.insert(measured.key);
    } else {
        m_ticked.remove(measured.key);
    }
    m_model.setTicked(row, ticked);
    emit listChanged();
}

void CueAlignmentController::tickAll(bool ticked)
{
    if (!m_staged.isEmpty()) {
        return;
    }
    for (int i = 0; i < m_model.count(); ++i) {
        const MeasuredTrack &measured = m_model.trackOf(m_model.rows()[static_cast<std::size_t>(i)]);
        if (measured.held()) {
            continue;
        }
        if (ticked) {
            m_ticked.insert(measured.key);
        } else {
            m_ticked.remove(measured.key);
        }
        m_model.setTicked(i, ticked);
    }
    emit listChanged();
}

void CueAlignmentController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(any));
    if (session == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    if (!m_session) {
        return;
    }
    connect(m_session, &LibraryEditSession::stateChanged, this, &CueAlignmentController::writingChanged);
    connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
        const QString prefix = QStringLiteral("align-cues:");
        if (changeId.startsWith(prefix) && m_staged.remove(changeId.mid(prefix.size()))) {
            m_ticked.remove(changeId.mid(prefix.size()));
            m_rescanAfterSave = true;
        }
    });
    connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
        // What was written is read again: the cues are where they went,
        // and the list says what is left.
        if (m_rescanAfterSave) {
            m_rescanAfterSave = false;
            scan(m_rekordboxPath, m_enginePath);
        } else {
            relist();
        }
    });
    connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
        if (!m_staged.isEmpty()) {
            m_staged.clear();
            relist();
        }
    });
}

void CueAlignmentController::stage()
{
    setErrorMessage({});
    if (m_ticked.isEmpty() || !m_result) {
        return;
    }
    attachSession();
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick's library could not be identified; nothing was changed."));
        return;
    }
    if (m_session->writing()) {
        setErrorMessage(QStringLiteral("A save is running. Stage more once it has finished."));
        return;
    }
    for (int i = 0; i < m_model.count(); ++i) {
        const CueAlignmentListModel::Row &row = m_model.rows()[static_cast<std::size_t>(i)];
        const MeasuredTrack &measured = m_model.trackOf(row);
        if (!row.ticked || row.staged || measured.held()) {
            continue;
        }
        // Aligned on the grid's own timeline, where they were measured,
        // and handed over on the timeline the cues were read on.
        std::vector<domain::CuePoint> cues = measured.track.cues;
        for (domain::CuePoint &cue : cues) {
            cue.positionMs -= measured.gridShiftMs;
            if (cue.isLoop) {
                cue.loopEndMs -= measured.gridShiftMs;
            }
        }
        cues = domain::cuesAlignedToBeats(std::move(cues), measured.alignment, m_toleranceMs);
        for (domain::CuePoint &cue : cues) {
            cue.positionMs += measured.gridShiftMs;
            if (cue.isLoop) {
                cue.loopEndMs += measured.gridShiftMs;
            }
        }
        if (!m_session->stage(std::make_unique<AlignCuesChange>(measured.path, measured.track, std::move(cues), row.offCount))) {
            // The session said why (another page's edits are staged).
            break;
        }
        m_staged.insert(measured.key);
        m_model.setStaged(i, true);
    }
    emit listChanged();
}

void CueAlignmentController::unstage()
{
    if (m_session) {
        for (const QString &key : m_staged) {
            m_session->unstage(QStringLiteral("align-cues:") + key);
        }
    }
    m_staged.clear();
    relist();
}

}  // namespace seabass::gui
