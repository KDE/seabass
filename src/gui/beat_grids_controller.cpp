// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/beat_grids_controller.hpp"

#include <QCryptographicHash>
#include <QTimer>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <utility>

#include "application/phased_progress.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "gui/beat_grid_display.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/local_file_url.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/metadata_row_text.hpp"
#include "gui/playlist_thumbnail_provider.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "domain/matching_policy.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

namespace fs = std::filesystem;

// --- the list of tracks ------------------------------------------------

int BeatGridTrackListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant BeatGridTrackListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
    case RekordboxIdRole:
        return row.rekordboxId;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case BpmRole:
        return row.bpm;
    case KeyNameRole:
        return row.keyName;
    case ArtworkUrlRole:
        return row.artworkUrl;
    case FallbackArtworkUrlRole:
        return row.fallbackArtworkUrl;
    case MatchedRole:
        return row.matched;
    case TickedRole:
        return row.ticked;
    case PendingRole:
        return row.pending;
    case GridStateRole:
        return row.gridState;
    case GridToneRole:
        return row.gridTone;
    case GridTipRole:
        return row.gridTip;
    case WaveformStateRole:
        return row.waveformState;
    case WaveformToneRole:
        return row.waveformTone;
    case WaveformTipRole:
        return row.waveformTip;
    case NoteRole:
        return row.note;
    case NoteToneRole:
        return row.noteTone;
    default:
        return {};
    }
}

QHash<int, QByteArray> BeatGridTrackListModel::roleNames() const
{
    return {
        {RekordboxIdRole, "rekordboxId"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {BpmRole, "bpm"},
        {KeyNameRole, "keyName"},
        {ArtworkUrlRole, "artworkUrl"},
        {FallbackArtworkUrlRole, "fallbackArtworkUrl"},
        {MatchedRole, "matched"},
        {TickedRole, "ticked"},
        {PendingRole, "pending"},
        {GridStateRole, "gridState"},
        {GridToneRole, "gridTone"},
        {GridTipRole, "gridTip"},
        {WaveformStateRole, "waveformState"},
        {WaveformToneRole, "waveformTone"},
        {WaveformTipRole, "waveformTip"},
        {NoteRole, "note"},
        {NoteToneRole, "noteTone"},
    };
}

void BeatGridTrackListModel::setRows(std::vector<Row> rows)
{
    const bool countMoved = rows.size() != m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (countMoved) {
        emit countChanged();
    }
}

void BeatGridTrackListModel::refresh(const std::function<void(Row &)> &update)
{
    // One announcement for the run of rows between the first and the last
    // that changed: a tick moves one row, a batch of states a few dozen.
    int first = -1;
    int last = -1;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row before = m_rows[i];
        update(m_rows[i]);
        const Row &after = m_rows[i];
        if (before.ticked != after.ticked || before.pending != after.pending || before.gridState != after.gridState
            || before.gridTip != after.gridTip || before.waveformState != after.waveformState
            || before.waveformTip != after.waveformTip || before.note != after.note
            || before.gridTone != after.gridTone || before.waveformTone != after.waveformTone
            || before.noteTone != after.noteTone) {
            if (first < 0) {
                first = static_cast<int>(i);
            }
            last = static_cast<int>(i);
        }
    }
    if (first >= 0) {
        emit dataChanged(index(first), index(last));
    }
}

QVariantMap BeatGridTrackListModel::get(int row) const
{
    if (row < 0 || static_cast<size_t>(row) >= m_rows.size()) {
        return {};
    }
    QVariantMap out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it) {
        out.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    }
    return out;
}

int BeatGridTrackListModel::indexOf(const QString &rekordboxId) const
{
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].rekordboxId == rekordboxId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool BeatGridTrackState::canRestore() const
{
    using Outcome = application::EngineAnalysisRestoreResult::Outcome;
    return !failed && facts.kept == application::EngineAnalysisFacts::Kept::Kept && restore
        && (restore->outcome == Outcome::Restored || restore->outcome == Outcome::AlreadyEngines);
}

bool BeatGridTrackState::wouldWrite() const
{
    return !failed
        && (result.grid.outcome == application::BeatGridWriteResult::Outcome::Written
            || result.waveform == application::AnalysisImportResult::Waveform::Written);
}

// --- the work, on workers ------------------------------------------------

namespace
{

using Outcome = application::BeatGridWriteResult::Outcome;
using Waveform = application::AnalysisImportResult::Waveform;

// Runs on a worker thread and touches nothing of the controller. Reads
// both catalogs (through the shared cache, as Sync Cue Points does) and
// plans the copy the way the command line does.
std::shared_ptr<BeatGridLibrary> runLoad(const QString &rekordboxPath, const QString &enginePath,
                                         std::shared_ptr<QtProgressReporter> reporter,
                                         application::CancellationToken cancel)
{
    auto out = std::make_shared<BeatGridLibrary>();
    out->rekordboxPath = rekordboxPath;
    out->enginePath = enginePath;
    try {
        namespace paths = infrastructure::paths;
        const std::string rekordbox = rekordboxPath.toStdString();
        const std::string engine = enginePath.toStdString();
        if (rekordbox.empty() || engine.empty()) {
            out->error = QStringLiteral("Copying beat grids and waveforms needs a rekordbox export and an Engine "
                                        "library on the same stick.");
            return out;
        }
        // The ledger lives on one stick, and a track is keyed by its path
        // from that stick's root.
        const std::string stickRoot = paths::normalStickRootForCatalogPath(rekordbox);
        if (stickRoot != paths::normalStickRootForCatalogPath(engine)) {
            out->error = QStringLiteral("The rekordbox export and the Engine library are on different sticks, so "
                                        "nothing is copied between them here.");
            return out;
        }
        out->stickRoot = stickRoot;
        out->stickReadOnly = infrastructure::media::isMountedReadOnly(stickRoot);
        // Engine 1.x keeps m.db at the library's root and its analysis in
        // a shape the writer does not write.
        std::error_code ec;
        const fs::path library = pathFromUtf8(engine);
        if (!fs::is_regular_file(library / "Database2" / "m.db", ec) && fs::is_regular_file(library / "m.db", ec)) {
            out->engineTooOld = true;
            return out;
        }

        auto &cache = LibraryCatalogCache::instance();
        // One bar (#58): the reads the cache still has to make.
        std::optional<size_t> planned = 0;
        const auto addUnits = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        addUnits(cache.plannedUnits("rekordbox", rekordbox, LibraryCatalogCache::Detail::Full, cancel));
        addUnits(cache.plannedUnits("engine", engine, LibraryCatalogCache::Detail::Full, cancel));
        application::PhasedProgress progress(*reporter, "Reading the libraries", planned.value_or(0));

        // Full, as Sync Cue Points reads them: matching refuses to pair
        // two tracks of one name while either length is unknown.
        const auto rekordboxTracks = cache.tracksFor("rekordbox", rekordbox, LibraryCatalogCache::Detail::Full,
                                                     progress, cancel);
        auto engineTracks = cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Full, progress, cancel);
        // Streaming tracks (TIDAL) have no file on the stick.
        engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                          [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                           engineTracks.end());
        out->plan = application::planBeatGridCopy(rekordboxTracks, engineTracks, stickRoot,
                                                  &application::SyncLibraries::stickFiles());
        if (cancel.cancelled()) {
            throw application::OperationCancelled();
        }

        std::string ledgerError;
        infrastructure::local::readBeatGridLedger(pathFromUtf8(stickRoot), &ledgerError);
        out->ledgerError = QString::fromStdString(ledgerError);

        // Which analysis file is a track's, from one pass over export.pdb
        // rather than one per track asked about.
        try {
            out->anlzIndex = std::make_shared<const infrastructure::rekordbox::AnlzPathIndex>(rekordbox);
        } catch (const std::exception &) {
            // Each track then finds its file the slow way.
        }

        std::map<std::string, int> itemOf;
        for (size_t i = 0; i < out->plan.items.size(); ++i) {
            itemOf[out->plan.items[i].rekordboxTrackId] = static_cast<int>(i);
        }
        std::map<std::string, std::string> engineArt;
        for (const domain::Track &t : engineTracks) {
            if (!t.artworkPath.empty()) {
                engineArt[t.sourceId] = t.artworkPath;
            }
        }
        std::map<std::string, std::vector<std::pair<int, std::string>>> members;
        out->tracks.reserve(rekordboxTracks.size());
        for (const domain::Track &t : rekordboxTracks) {
            BeatGridLibrary::Track track;
            track.rekordboxId = t.sourceId;
            track.title = t.title.empty() ? t.filename : t.title;
            track.artist = t.artist;
            track.bpm = t.bpm;
            track.key = t.key;
            track.artworkUrl = toLocalFileUrl(t.artworkPath);
            track.artworkPath = t.artworkPath;
            track.filePath = t.filePath;
            track.durationMs = t.durationSeconds > 0.0 ? t.durationSeconds * 1000.0 : 0.0;
            track.cues = t.cues;
            if (const auto item = itemOf.find(t.sourceId); item != itemOf.end()) {
                track.item = item->second;
                const auto art = engineArt.find(out->plan.items[static_cast<size_t>(item->second)].engineTrackId);
                if (art != engineArt.end()) {
                    track.fallbackArtworkUrl = toLocalFileUrl(art->second);
                    track.fallbackArtworkPath = art->second;
                }
            }
            out->trackIndex[t.sourceId] = out->tracks.size();
            for (const auto &membership : t.playlists) {
                members[membership.name].emplace_back(membership.position, t.sourceId);
            }
            out->tracks.push_back(std::move(track));
        }
        // A playlist's tracks in its own order, and its small cover from
        // the first four distinct covers in that order, as Browse makes it.
        const auto thumbnailFor = [](const QStringList &covers) {
            const QString id = PlaylistThumbnailProvider::idForCovers(covers);
            return id.isEmpty() ? QString() : QStringLiteral("image://playlist-thumb/") + id;
        };
        QStringList libraryCovers;
        for (const auto &track : out->tracks) {
            const QString cover = track.artworkUrl.isEmpty() ? track.fallbackArtworkUrl : track.artworkUrl;
            if (!cover.isEmpty() && !libraryCovers.contains(cover)) {
                libraryCovers << cover;
                if (libraryCovers.size() == 4) {
                    break;
                }
            }
        }
        out->playlistThumbnails.insert(QString(), thumbnailFor(libraryCovers));
        for (auto &[name, entries] : members) {
            std::stable_sort(entries.begin(), entries.end(),
                             [](const auto &a, const auto &b) { return a.first < b.first; });
            std::vector<std::string> ids;
            ids.reserve(entries.size());
            QStringList covers;
            for (const auto &[position, id] : entries) {
                ids.push_back(id);
                const auto &track = out->tracks[out->trackIndex[id]];
                const QString cover = track.artworkUrl.isEmpty() ? track.fallbackArtworkUrl : track.artworkUrl;
                if (covers.size() < 4 && !cover.isEmpty() && !covers.contains(cover)) {
                    covers << cover;
                }
            }
            out->playlistThumbnails.insert(QString::fromStdString(name), thumbnailFor(covers));
            out->playlists[name] = std::move(ids);
        }
        progress.done();
    } catch (const application::OperationCancelled &) {
        out->cancelled = true;
    } catch (const std::exception &e) {
        out->error = QString::fromStdString(e.what());
    }
    return out;
}

// The rows a preview batch finished, handed to the GUI thread a few at a
// time, so a batch that is cancelled keeps what it had done.
using PreviewRows = std::vector<std::pair<std::string, BeatGridTrackState>>;

struct PreviewInput
{
    std::shared_ptr<const BeatGridLibrary> library;
    std::vector<application::BeatGridCopyItem> items;
    application::AnalysisCopyOptions options;
};

// rekordbox's analysis of an item, as the command line reads it: the grid,
// the waveform sections, the key in Engine's numbering and the tempo.
application::RekordboxAnalysis readAnalysis(const BeatGridLibrary &library, const application::BeatGridCopyItem &item)
{
    return infrastructure::local::rekordboxAnalysisOf(library.rekordboxPath.toStdString(), item,
                                                      library.anlzIndex.get());
}

// Each item through the writer with nothing written (previewOnly), as the
// command line's check takes it. The stick's metadata cache is read, never
// written: a preview leaves the stick as it was.
QString runPreviewBatch(const PreviewInput &in, const std::function<void(PreviewRows)> &deliver,
                        std::shared_ptr<QtProgressReporter> reporter, application::CancellationToken cancel)
{
    const BeatGridLibrary &library = *in.library;
    std::string ledgerError;
    const auto ledger = infrastructure::local::readBeatGridLedger(pathFromUtf8(library.stickRoot), &ledgerError);
    infrastructure::engine::LibdjinteropEngineCueWriter writer(library.enginePath.toStdString());
    // Read only: a preview finds what is kept and never writes it.
    infrastructure::local::StickKeptEngineAnalysisStore kept(pathFromUtf8(library.stickRoot));
    writer.setKeptAnalysisStore(&kept);
#ifdef SEABASS_HAVE_TAGLIB
    infrastructure::audio::TagLibMetadataProbe probe;
#else
    application::NullTrackMetadataProbe probe;
#endif
    infrastructure::local::CachedSampleRates rates(library.stickRoot, probe);
    const application::AnalysisFileFactsSource facts = [&rates](const application::BeatGridCopyItem &item) {
        return infrastructure::local::analysisFileFactsOf(item, rates);
    };
    application::AnalysisCopyOptions options = in.options;
    options.previewOnly = true;

    reporter->start("Checking tracks", in.items.size());
    PreviewRows chunk;
    // Small enough that rows appear while the bar moves, large enough that
    // a thousand rows are not a thousand hops to the GUI thread.
    constexpr size_t ChunkSize = 12;
    for (size_t i = 0; i < in.items.size(); ++i) {
        // Between rows, never inside one.
        if (cancel.cancelled()) {
            break;
        }
        const application::BeatGridCopyItem &item = in.items[i];
        BeatGridTrackState state;
        try {
            application::RekordboxAnalysis analysis = readAnalysis(library, item);
            const application::AnalysisItemOutcome outcome =
                application::importAnalysisItem(item, analysis, writer, ledger, options, facts);
            state.result = outcome.result;
            state.options = outcome.options;
            if (state.wouldWrite()) {
                state.analysisDigest = infrastructure::local::rekordboxAnalysisDigest(analysis);
            }
            state.facts = writer.engineAnalysisFacts(item.engineTrackId, item.ledgerKey);
            if (state.facts.kept == application::EngineAnalysisFacts::Kept::Kept) {
                state.restore = writer.restoreEngineAnalysis(item.engineTrackId, item.ledgerKey, {false, true});
            }
        } catch (const std::exception &e) {
            state.failed = true;
            state.failure = QString::fromStdString(e.what());
        }
        chunk.emplace_back(item.rekordboxTrackId, std::move(state));
        if (chunk.size() >= ChunkSize) {
            deliver(std::move(chunk));
            chunk = {};
        }
        reporter->tick(i + 1);
    }
    if (!chunk.empty()) {
        deliver(std::move(chunk));
    }
    reporter->finish();
    if (cancel.cancelled()) {
        throw application::OperationCancelled();
    }
    return QString::fromStdString(ledgerError);
}

// Hands a worker's rows to the controller on its own thread, and to
// nothing once the controller has gone. The controller holds it for its
// whole life, and every batch's worker holds it too: were a worker's the
// last hold, the sink would go (deleteLater, posted from that thread)
// with the batch's last rows still queued for it, and they were seen to
// be dropped so: a page whose batch had ended with rows never filled.
class PreviewSink : public QObject
{
public:
    using Take = std::function<void(std::uint64_t, PreviewRows)>;
    explicit PreviewSink(Take take) : m_take(std::move(take)) {}
    void post(std::uint64_t generation, PreviewRows rows)
    {
        auto shared = std::make_shared<PreviewRows>(std::move(rows));
        QMetaObject::invokeMethod(
            this, [this, generation, shared]() { m_take(generation, std::move(*shared)); }, Qt::QueuedConnection);
    }

private:
    Take m_take;
};

QString plural(int n, const QString &one, const QString &many)
{
    return n == 1 ? one : QString::number(n) + QLatin1Char(' ') + many;
}

// What a part of a row's preview says, as the row shows it: the words,
// a tone ("good" for what would be copied, "muted" for nothing to do,
// "warn" for what keeps it from being copied), and a sentence for its tip.
struct Words
{
    QString text;
    QString tone;
    QString tip;
};

QString cannotKeepTip(const BeatGridTrackState &s)
{
    // What was kept before cannot be read: the write refuses the row (the
    // preview says so, as the save would), and the way out is the button.
    if (s.facts.kept == application::EngineAnalysisFacts::Kept::Damaged) {
        return QStringLiteral("What Seabass kept of Engine's own analysis of this track before cannot be read, so it "
                              "is not replaced. \"Let the Player Analyse Again\" clears it.");
    }
    return QStringLiteral("Its file is not under the stick's root, so Engine's own analysis of it cannot be kept on "
                          "the stick first, and it is not replaced.");
}

QString unreadableTip()
{
    return QStringLiteral("Whether the player has analysed this track cannot be read from the Engine library, so "
                          "it is left alone.");
}

Words gridWords(const BeatGridTrackState &s)
{
    switch (s.result.grid.outcome) {
    case Outcome::Written:
        return s.result.completedRow
            ? Words{QStringLiteral("New"), QStringLiteral("good"),
                    QStringLiteral("The player has not analysed this track: it gets rekordbox's beat grid as its own.")}
            : Words{QStringLiteral("Replaces Engine's"), QStringLiteral("good"),
                    QStringLiteral("rekordbox's beat grid becomes the one the player plays by. Engine's own stays "
                                   "beside it, so the player can go back to it.")};
    case Outcome::AlreadySame:
        return {QStringLiteral("Already the same"), QStringLiteral("muted"),
                QStringLiteral("The Engine library already has rekordbox's beat grid for this track.")};
    case Outcome::NoSourceGrid:
        return {QStringLiteral("No beat grid in rekordbox"), QStringLiteral("warn"),
                QStringLiteral("rekordbox has no beat grid for this track, so there is none to copy.")};
    case Outcome::RowNotAnalysed:
    case Outcome::NoSampleRate:
        return {QStringLiteral("Left for the player"), QStringLiteral("warn"),
                QStringLiteral("The player has not analysed this track yet and it cannot be completed from "
                               "rekordbox, so the player analyses it when it is first loaded.")};
    case Outcome::RowLocked:
        return {QStringLiteral("Locked on the player"), QStringLiteral("warn"),
                QStringLiteral("The beat grid is locked on the player. \"Also replace grids locked or changed on "
                               "the player\" replaces it all the same.")};
    case Outcome::RowEditedOnEngine:
        return {QStringLiteral("Changed on the player"), QStringLiteral("warn"),
                QStringLiteral("The beat grid was moved on the player. \"Also replace grids locked or changed on "
                               "the player\" replaces it all the same.")};
    case Outcome::UnsupportedSchema:
        return {QStringLiteral("Engine library too old"), QStringLiteral("warn"), QString()};
    case Outcome::NotRequested:
        return {QStringLiteral("Not chosen"), QStringLiteral("muted"),
                QStringLiteral("Beat grids are not chosen above.")};
    case Outcome::CannotKeepEngines:
        return {QStringLiteral("Cannot be kept"), QStringLiteral("warn"), cannotKeepTip(s)};
    case Outcome::RowUnreadable:
        return {QStringLiteral("Cannot be read"), QStringLiteral("warn"), unreadableTip()};
    }
    return {};
}

Words waveformWords(const BeatGridTrackState &s)
{
    switch (s.result.waveform) {
    case Waveform::Written:
        return s.result.completedRow
            ? Words{QStringLiteral("New"), QStringLiteral("good"),
                    QStringLiteral("The player has not analysed this track: it gets rekordbox's waveform as its own.")}
            : Words{QStringLiteral("Replaces Engine's"), QStringLiteral("good"),
                    QStringLiteral("rekordbox's waveform replaces the one the player drew.")};
    case Waveform::AlreadySame:
        return {QStringLiteral("Already the same"), QStringLiteral("muted"),
                QStringLiteral("The Engine library already has rekordbox's waveform for this track.")};
    case Waveform::NoSourceWaveform:
        return {QStringLiteral("No waveform in rekordbox"), QStringLiteral("warn"),
                QStringLiteral("rekordbox has no colour or three-band waveform for this track.")};
    case Waveform::RowNotAnalysed:
    case Waveform::NoSampleCount:
        return {QStringLiteral("Left for the player"), QStringLiteral("warn"),
                QStringLiteral("The player has not analysed this track yet and it cannot be completed from "
                               "rekordbox, so the player analyses it when it is first loaded.")};
    case Waveform::UnsupportedSchema:
        return {QStringLiteral("Engine library too old"), QStringLiteral("warn"), QString()};
    case Waveform::NotRequested:
        return {QStringLiteral("Not chosen"), QStringLiteral("muted"),
                QStringLiteral("Waveforms are not chosen above.")};
    case Waveform::CannotKeepEngines:
        return {QStringLiteral("Cannot be kept"), QStringLiteral("warn"), cannotKeepTip(s)};
    case Waveform::RowUnreadable:
        return {QStringLiteral("Cannot be read"), QStringLiteral("warn"), unreadableTip()};
    }
    return {};
}

// The player has not analysed the row (whatever else is true of it).
bool notAnalysed(const BeatGridTrackState &s)
{
    return s.result.grid.outcome == Outcome::RowNotAnalysed || s.result.grid.outcome == Outcome::NoSampleRate
        || s.result.waveform == Waveform::RowNotAnalysed || s.result.waveform == Waveform::NoSampleCount;
}

// Why a row the player has not analysed cannot be completed.
QString whyLeftForThePlayer(const BeatGridTrackState &s)
{
    if (!s.options.grids || !s.options.waveforms) {
        return QStringLiteral("Not analysed by the player yet: only completed with both beat grids and waveforms");
    }
    if (s.result.grid.outcome == Outcome::NoSourceGrid) {
        return QStringLiteral("Not analysed by the player yet: rekordbox has no beat grid to complete it with");
    }
    if (s.result.waveform == Waveform::NoSourceWaveform) {
        return QStringLiteral("Not analysed by the player yet: rekordbox has no waveform to complete it with");
    }
    if (s.result.grid.outcome == Outcome::RowLocked) {
        return QStringLiteral("Not analysed by the player yet, and its beat grid is locked");
    }
    if (s.result.waveform == Waveform::NoSampleCount || s.result.grid.outcome == Outcome::NoSampleRate) {
        return QStringLiteral("Not analysed by the player yet: its length could not be read from the file");
    }
    return QStringLiteral("Not analysed by the player yet");
}

// Why a ticked track would be left alone, in the summary's words; empty
// when something would be written.
QString leftAloneReason(const BeatGridTrackState &s)
{
    if (s.failed) {
        return QStringLiteral("could not be read");
    }
    if (s.wouldWrite()) {
        return {};
    }
    if (s.result.grid.outcome == Outcome::UnsupportedSchema || s.result.waveform == Waveform::UnsupportedSchema) {
        return QStringLiteral("Engine library too old");
    }
    if (s.result.grid.outcome == Outcome::CannotKeepEngines || s.result.waveform == Waveform::CannotKeepEngines) {
        return QStringLiteral("Engine's analysis cannot be kept");
    }
    if (s.result.grid.outcome == Outcome::RowUnreadable || s.result.waveform == Waveform::RowUnreadable) {
        return QStringLiteral("analysed or not cannot be read");
    }
    if (notAnalysed(s)) {
        return QStringLiteral("left for the player");
    }
    if (s.result.grid.outcome == Outcome::RowLocked) {
        return QStringLiteral("locked on the player");
    }
    if (s.result.grid.outcome == Outcome::RowEditedOnEngine) {
        return QStringLiteral("changed on the player");
    }
    if (s.result.grid.outcome == Outcome::AlreadySame || s.result.waveform == Waveform::AlreadySame) {
        return QStringLiteral("already the same");
    }
    return QStringLiteral("nothing in rekordbox to copy");
}

}  // namespace

// --- the controller ----------------------------------------------------

BeatGridsController::BeatGridsController(QObject *parent) : QObject(parent)
{
    m_options.grids = true;
    m_options.waveforms = true;
    m_options.overwriteEngineEdits = false;
    recount();
    // The rows' reads wait while any library is being written.
    connect(EditSessionRegistry::instance(), &EditSessionRegistry::stateChanged, this,
            &BeatGridsController::onWriteStateChanged);
    connect(this, &BeatGridsController::writingChanged, this, &BeatGridsController::onWriteStateChanged);
    m_rowReadsHeld = rowReadsHeld();
}

BeatGridsController::~BeatGridsController() = default;

QString BeatGridsController::ledgerWarning() const
{
    if (m_ledgerError.isEmpty()) {
        return {};
    }
    return QStringLiteral("Seabass's record of the beat grids it copied before cannot be read (%1). Until it can, "
                          "every grid Seabass copied counts as changed on the player and is left alone. The next "
                          "copy writes a new record, of the tracks it copies and of the ticked ones already the "
                          "same.")
        .arg(m_ledgerError);
}

QVariantMap BeatGridsController::playlistThumbnails() const
{
    return m_library ? m_library->playlistThumbnails : QVariantMap();
}

bool BeatGridsController::writing() const
{
    return m_session && m_session->writing();
}

// --- the rows' waveforms -------------------------------------------------

bool BeatGridsController::rowReadsHeld() const
{
    return writing() || EditSessionRegistry::instance()->anyWriting();
}

void BeatGridsController::onWriteStateChanged()
{
    const bool held = rowReadsHeld();
    if (held == m_rowReadsHeld) {
        return;
    }
    m_rowReadsHeld = held;
    if (held) {
        // A read under way may meet a file half rewritten: put back, and
        // read again once the write is over.
        if (m_rowRead.busy()) {
            const QString id = m_rowRead.key();
            m_rowRead.cancel();
            m_rowWanted.removeAll(id);
            m_rowWanted.prepend(id);
        }
        if (m_engineRead.busy()) {
            const QString id = m_engineRead.key();
            m_engineRead.cancel();
            m_engineWanted.removeAll(id);
            m_engineWanted.prepend(id);
        }
        return;
    }
    // Any library may now say something else about any track. What was
    // read stays drawn until each row has its track's again.
    ++m_rowGeneration;
    emit rowAnalysesStale();
    pumpRowReads();
    pumpEngineReads();
}

const BeatGridLibrary::Track *BeatGridsController::libraryTrack(const QString &rekordboxId) const
{
    if (!m_library) {
        return nullptr;
    }
    const auto at = m_library->trackIndex.find(rekordboxId.toStdString());
    return at == m_library->trackIndex.end() ? nullptr : &m_library->tracks[at->second];
}

void BeatGridsController::requestRowAnalysis(const QString &rekordboxId)
{
    if (libraryTrack(rekordboxId) == nullptr) {
        return;
    }
    if (const RowAnalysis *kept = m_rowAnalyses.object(rekordboxId); kept && kept->generation == m_rowGeneration) {
        return;
    }
    if ((m_rowRead.busy() && m_rowRead.key() == rekordboxId) || m_rowWanted.contains(rekordboxId)) {
        return;
    }
    m_rowWanted.append(rekordboxId);
    pumpRowReads();
}

void BeatGridsController::releaseRowAnalysis(const QString &rekordboxId)
{
    // Not yet read: never read. Being read: kept when it lands, for a
    // row scrolled back to.
    m_rowWanted.removeAll(rekordboxId);
}

void BeatGridsController::pumpRowReads()
{
    if (m_rowRead.busy() || m_rowReadsHeld || m_rowWanted.isEmpty() || !m_library) {
        return;
    }
    const QString id = m_rowWanted.takeFirst();
    const BeatGridLibrary::Track *track = libraryTrack(id);
    if (track == nullptr) {
        pumpRowReads();
        return;
    }
    ++m_rowAnalysisReads;
    emit rowAnalysisReadsChanged();
    const std::uint64_t generation = m_rowGeneration;
    using Request = AsyncRequest<std::shared_ptr<RowAnalysis>>;
    // The library as read, with the analysis paths of its export.pdb: the
    // worker looks nothing up in the database itself.
    Request::Work work = [library = m_library, sourceId = id.toStdString(),
                          filePath = track->filePath](application::CancellationToken cancel) {
        auto out = std::make_shared<RowAnalysis>();
        if (cancel.cancelled()) {
            return out;
        }
        domain::TrackAnalysis analysis = infrastructure::rekordbox::readTrackAnalysis(
            library->rekordboxPath.toStdString(), sourceId, nullptr, library->anlzIndex.get());
        out->columns.reserve(static_cast<qsizetype>(analysis.waveform.size() * 3));
        const auto byte = [](double v) {
            return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
        };
        for (const auto &column : analysis.waveform) {
            out->columns.append(byte(column.low));
            out->columns.append(byte(column.mid));
            out->columns.append(byte(column.high));
        }
        out->beats = std::move(analysis.beats);
        out->beatRateRecorded = analysis.beatRateRecorded;
        // Looked at here rather than on the GUI thread: the popup is
        // offered only where there is a file to play.
        std::error_code ec;
        out->fileThere = !filePath.empty() && fs::is_regular_file(pathFromUtf8(filePath), ec);
        return out;
    };
    Request::Ending ending{
        [this, id, generation](std::shared_ptr<RowAnalysis> &&read) {
            read->generation = generation;
            const int cost = std::max(1, static_cast<int>((read->columns.size()
                                                            + read->beats.size() * sizeof(domain::Beat)) / 1024));
            m_rowAnalyses.insert(id, new RowAnalysis(std::move(*read)), cost);
            emit rowAnalysisReady(id);
            pumpRowReads();
        },
        // A stick gone or a worker refused: the row stays without one.
        [this](const QString &) { pumpRowReads(); },
        [] {},
    };
    m_rowRead.start(id, stickRootOf(m_library->rekordboxPath), std::move(work), std::move(ending));
}

void BeatGridsController::forgetRowAnalyses()
{
    m_rowRead.cancel();
    m_rowWanted.clear();
    m_rowAnalyses.clear();
    m_engineRead.cancel();
    m_engineWanted.clear();
    m_engineAnalyses.clear();
    ++m_rowGeneration;
}

// --- an opened row's Engine side -----------------------------------------

namespace
{

// A grid as the comparison states it: its tempo (the median beat spacing,
// so one odd marker does not move it) and its first downbeat. Empty where
// there are fewer than two beats.
QVariantMap gridFacts(const std::vector<domain::Beat> &beats)
{
    if (beats.size() < 2) {
        return {{QStringLiteral("hasGrid"), false}};
    }
    std::vector<double> gaps;
    gaps.reserve(beats.size() - 1);
    for (std::size_t i = 1; i < beats.size(); ++i) {
        gaps.push_back(beats[i].timeMs - beats[i - 1].timeMs);
    }
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
    const double beatMs = gaps[gaps.size() / 2];
    // The first beat said to be a downbeat; a grid that says nothing about
    // bars starts its bars at its first beat.
    double downbeat = beats.front().timeMs;
    for (const auto &beat : beats) {
        if (beat.beatInBar == 1) {
            downbeat = beat.timeMs;
            break;
        }
    }
    return {
        {QStringLiteral("hasGrid"), beatMs > 0.0},
        {QStringLiteral("bpm"), beatMs > 0.0 ? 60000.0 / beatMs : 0.0},
        {QStringLiteral("firstDownbeatMs"), downbeat},
    };
}

}  // namespace

void BeatGridsController::requestEngineAnalysis(const QString &rekordboxId)
{
    if (libraryTrack(rekordboxId) == nullptr) {
        return;
    }
    if (const EngineAnalysis *kept = m_engineAnalyses.object(rekordboxId);
        kept && kept->generation == m_rowGeneration) {
        return;
    }
    if ((m_engineRead.busy() && m_engineRead.key() == rekordboxId) || m_engineWanted.contains(rekordboxId)) {
        return;
    }
    m_engineWanted.append(rekordboxId);
    pumpEngineReads();
}

void BeatGridsController::releaseEngineAnalysis(const QString &rekordboxId)
{
    m_engineWanted.removeAll(rekordboxId);
}

void BeatGridsController::pumpEngineReads()
{
    if (m_engineRead.busy() || m_rowReadsHeld || m_engineWanted.isEmpty() || !m_library) {
        return;
    }
    const QString id = m_engineWanted.takeFirst();
    const BeatGridLibrary::Track *track = libraryTrack(id);
    if (track == nullptr) {
        pumpEngineReads();
        return;
    }
    const std::string engineTrackId = track->item >= 0
        ? m_library->plan.items[static_cast<std::size_t>(track->item)].engineTrackId
        : std::string();
    const std::uint64_t generation = m_rowGeneration;
    // The preference is read here, on the GUI thread; the file there.
    const bool correctMp3 = domain::MatchingPolicy::correctMp3CueTiming();
    using Request = AsyncRequest<std::shared_ptr<EngineAnalysis>>;
    // Only the overview Engine draws across the whole track (no detail):
    // all a 2.x or 3.x library keeps, and what the player shows.
    Request::Work work = [enginePath = m_library->enginePath.toStdString(), engineTrackId,
                          filePath = track->filePath, correctMp3](application::CancellationToken cancel) {
        auto out = std::make_shared<EngineAnalysis>();
        out->hasEngineTrack = !engineTrackId.empty() && !enginePath.empty();
        if (!out->hasEngineTrack || cancel.cancelled()) {
            return out;
        }
        if (correctMp3 && domain::hasMp3Extension(filePath)) {
            out->rekordboxShiftMs = infrastructure::audio::playbackShiftFor(
                infrastructure::audio::readMp3LeadIn(filePath), true,
                infrastructure::audio::GridTimeline::Rekordbox).beatsMs;
        }
        domain::TrackAnalysis analysis = infrastructure::engine::readTrackAnalysis(enginePath, engineTrackId);
        out->columns.reserve(static_cast<qsizetype>(analysis.waveform.size() * 3));
        const auto byte = [](double v) {
            return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
        };
        for (const auto &column : analysis.waveform) {
            out->columns.append(byte(column.low));
            out->columns.append(byte(column.mid));
            out->columns.append(byte(column.high));
        }
        out->beats = std::move(analysis.beats);
        out->beatRateRecorded = analysis.beatRateRecorded;
        return out;
    };
    Request::Ending ending{
        [this, id, generation](std::shared_ptr<EngineAnalysis> &&read) {
            read->generation = generation;
            const int cost = std::max(1, static_cast<int>((read->columns.size()
                                                            + read->beats.size() * sizeof(domain::Beat)) / 1024));
            m_engineAnalyses.insert(id, new EngineAnalysis(std::move(*read)), cost);
            emit engineAnalysisReady(id);
            pumpEngineReads();
        },
        // A stick gone or a worker refused: the row says it has nothing.
        [this](const QString &) { pumpEngineReads(); },
        [] {},
    };
    m_engineRead.start(id, stickRootOf(m_library->rekordboxPath), std::move(work), std::move(ending));
}

QVariantMap BeatGridsController::engineAnalysis(const QString &rekordboxId) const
{
    const EngineAnalysis *kept = m_engineAnalyses.object(rekordboxId);
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (kept == nullptr || track == nullptr) {
        return {{QStringLiteral("ready"), false}};
    }
    QVariantMap out = gridFacts(kept->beats);
    out.insert(QStringLiteral("ready"), true);
    out.insert(QStringLiteral("hasEngineTrack"), kept->hasEngineTrack);
    out.insert(QStringLiteral("columns"), kept->columns);
    out.insert(QStringLiteral("durationMs"), track->durationMs);
    out.insert(QStringLiteral("rateRecorded"), kept->beatRateRecorded);
    out.insert(QStringLiteral("rekordboxShiftMs"), kept->rekordboxShiftMs);
    return out;
}

namespace
{

std::vector<domain::Beat> shiftedBeats(const std::vector<domain::Beat> &beats, double byMs)
{
    std::vector<domain::Beat> out = beats;
    for (domain::Beat &beat : out) {
        beat.timeMs += byMs;
    }
    return out;
}

}  // namespace

QVariantList BeatGridsController::comparisonBeatTicks(const QString &rekordboxId, double widthPx) const
{
    const RowAnalysis *kept = m_rowAnalyses.object(rekordboxId);
    const EngineAnalysis *engine = m_engineAnalyses.object(rekordboxId);
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (kept == nullptr || engine == nullptr || track == nullptr) {
        return {};
    }
    return beatTicksForDisplay(shiftedBeats(kept->beats, engine->rekordboxShiftMs), kept->beatRateRecorded,
                               track->durationMs, widthPx);
}

QVariantList BeatGridsController::engineBeatTicks(const QString &rekordboxId, double widthPx) const
{
    const EngineAnalysis *kept = m_engineAnalyses.object(rekordboxId);
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (kept == nullptr || track == nullptr) {
        return {};
    }
    return beatTicksForDisplay(kept->beats, kept->beatRateRecorded, track->durationMs, widthPx);
}

QVariantMap BeatGridsController::rowGridFacts(const QString &rekordboxId) const
{
    const RowAnalysis *kept = m_rowAnalyses.object(rekordboxId);
    const EngineAnalysis *engine = m_engineAnalyses.object(rekordboxId);
    if (kept == nullptr || engine == nullptr) {
        return {{QStringLiteral("ready"), false}};
    }
    QVariantMap out = gridFacts(shiftedBeats(kept->beats, engine->rekordboxShiftMs));
    out.insert(QStringLiteral("ready"), true);
    out.insert(QStringLiteral("shiftMs"), engine->rekordboxShiftMs);
    return out;
}

QVariantMap BeatGridsController::rowAnalysis(const QString &rekordboxId) const
{
    const RowAnalysis *kept = m_rowAnalyses.object(rekordboxId);
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (kept == nullptr || track == nullptr) {
        return {{QStringLiteral("ready"), false}};
    }
    return {
        {QStringLiteral("ready"), true},
        {QStringLiteral("columns"), kept->columns},
        {QStringLiteral("durationMs"), track->durationMs},
        {QStringLiteral("playable"), kept->fileThere},
    };
}

QVariantList BeatGridsController::rowBeatTicks(const QString &rekordboxId, double widthPx) const
{
    const RowAnalysis *kept = m_rowAnalyses.object(rekordboxId);
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (kept == nullptr || track == nullptr) {
        return {};
    }
    return beatTicksForDisplay(kept->beats, kept->beatRateRecorded, track->durationMs, widthPx);
}

QVariantMap BeatGridsController::zoomTrack(const QString &rekordboxId) const
{
    const BeatGridLibrary::Track *track = libraryTrack(rekordboxId);
    if (track == nullptr) {
        return {};
    }
    return {
        {QStringLiteral("format"), QStringLiteral("rekordbox")},
        {QStringLiteral("libraryPath"), m_library->rekordboxPath},
        {QStringLiteral("sourceId"), rekordboxId},
        {QStringLiteral("filePath"), QString::fromStdString(track->filePath)},
        {QStringLiteral("title"), QString::fromStdString(track->title)},
        {QStringLiteral("artist"), QString::fromStdString(track->artist)},
        {QStringLiteral("artworkPath"), QString::fromStdString(track->artworkPath)},
        {QStringLiteral("cues"), metadataCueList(track->cues)},
        {QStringLiteral("fallbackArtworkPath"), QString::fromStdString(track->fallbackArtworkPath)},
    };
}

bool BeatGridsController::canStageAnything() const
{
    return m_library && !m_library->engineTooOld && !m_library->stickReadOnly && m_stagedIds.isEmpty() && !writing()
        && (m_options.grids || m_options.waveforms) && m_summary.pending == 0;
}

bool BeatGridsController::canStage() const
{
    return canStageAnything() && m_summary.rows > 0;
}

bool BeatGridsController::canRestore() const
{
    return canStageAnything() && m_summary.restorable > 0;
}

bool BeatGridsController::canLetPlayerAnalyse() const
{
    return canStageAnything() && m_summary.analysed > 0;
}

QString BeatGridsController::notInEngineText() const
{
    if (m_notInEngineShown == 0) {
        return {};
    }
    // Preferences' two switches (music/copyBeatGridsToEngine and
    // music/copyWaveformsToEngine): a track Sync after Rekordbox Export adds
    // is new and unanalysed, so it is completed only with both on. The
    // switches are experimental, as this page is, so a reader here sees
    // them.
    return plural(m_notInEngineShown, QStringLiteral("One track here is"), QStringLiteral("tracks here are"))
        + QStringLiteral(" not in the Engine library yet, so nothing can be copied to ")
        + (m_notInEngineShown == 1 ? QStringLiteral("it") : QStringLiteral("them"))
        + QStringLiteral(". Sync the libraries first with <a href=\"sync\">Sync after Rekordbox Export</a>, which "
                         "also copies their beat grids and waveforms as it adds them when both are switched on in "
                         "Preferences, or come back here once the libraries are in step.");
}

void BeatGridsController::load(const QString &rekordboxPath, const QString &enginePath)
{
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    startLoad();
}

void BeatGridsController::reloadIfStale()
{
    if (!m_libraryStale || m_rekordboxPath.isEmpty() || m_enginePath.isEmpty()) {
        return;
    }
    m_libraryStale = false;
    startLoad();
}

void BeatGridsController::cancelLoad()
{
    if (m_load.busy()) {
        m_load.cancel();
    }
}

void BeatGridsController::startLoad()
{
    const QString key = m_rekordboxPath + QLatin1Char('\n') + m_enginePath;
    if (m_load.busy() && m_load.key() == key) {
        return;
    }
    setErrorMessage({});
    setLoadProgress(0, 0);

    auto reporter = makeMainThreadShared<QtProgressReporter>();
    const auto current = m_load.speaksForNext();
    connect(reporter.get(), &QtProgressReporter::started, this, [this, current](const QString &label, int total) {
        if (current()) {
            m_loadLabel = label;
            setLoadProgress(0, total);
            emit loadProgressChanged();
        }
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this, current](const QString &label) {
        if (current() && m_loadLabel != label) {
            m_loadLabel = label;
            emit loadProgressChanged();
        }
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this, [this, current](int done) {
        if (current()) {
            setLoadProgress(done, m_loadTotal);
        }
    });

    using Request = AsyncRequest<std::shared_ptr<BeatGridLibrary>>;
    Request::Work work = [rekordboxPath = m_rekordboxPath, enginePath = m_enginePath,
                          reporter](application::CancellationToken cancel) {
        return runLoad(rekordboxPath, enginePath, reporter, cancel);
    };
    Request::Ending ending{
        [this](std::shared_ptr<BeatGridLibrary> &&result) {
            if (!result || result->cancelled) {
                emit loadCancelled();
                return;
            }
            if (!result->error.isEmpty()) {
                setErrorMessage(result->error);
                return;
            }
            adoptLibrary(std::move(result));
        },
        [this](const QString &message) { setErrorMessage(message); },
        [this]() { emit loadCancelled(); },
    };
    m_load.start(key, stickRootOf(m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath), std::move(work),
                 std::move(ending));
}

void BeatGridsController::adoptLibrary(std::shared_ptr<BeatGridLibrary> library)
{
    m_library = std::move(library);
    m_playlistNames.clear();
    m_playlistTrackCounts.clear();
    for (const auto &[name, ids] : m_library->playlists) {
        const QString shown = QString::fromStdString(name);
        m_playlistNames << shown;
        m_playlistTrackCounts.insert(shown, static_cast<int>(ids.size()));
    }
    if (m_ledgerError != m_library->ledgerError) {
        m_ledgerError = m_library->ledgerError;
        emit ledgerWarningChanged();
    }
    // A playlist the library no longer has shows every track.
    if (!m_shownPlaylist.isEmpty() && !m_library->playlists.contains(m_shownPlaylist.toStdString())) {
        m_shownPlaylist.clear();
        emit shownPlaylistChanged();
    }
    // Ticks of tracks that can no longer be ticked go (a library read
    // again after another page's save); the others stay.
    QSet<QString> tickable;
    for (const std::string &id : tickableIn(QString())) {
        tickable.insert(QString::fromStdString(id));
    }
    const QSet<QString> ticked = m_ticked;
    m_ticked.intersect(tickable);
    if (m_ticked != ticked) {
        ++m_tickRevision;
        emit ticksChanged();
    }
    m_states.clear();
    ++m_generation;
    // Another read of the stick: the rows' analyses are read again, from
    // this one.
    forgetRowAnalyses();
    emit libraryChanged();
    rebuildRows();
    recount();
    schedulePreview();
}

const std::vector<std::string> *BeatGridsController::playlistIds(const QString &name) const
{
    if (!m_library || name.isEmpty()) {
        return nullptr;
    }
    const auto it = m_library->playlists.find(name.toStdString());
    return it == m_library->playlists.end() ? nullptr : &it->second;
}

std::vector<std::string> BeatGridsController::tickableIn(const QString &name) const
{
    std::vector<std::string> out;
    if (!m_library || m_library->engineTooOld) {
        return out;
    }
    if (name.isEmpty()) {
        for (const auto &track : m_library->tracks) {
            if (track.item >= 0) {
                out.push_back(track.rekordboxId);
            }
        }
        return out;
    }
    if (const auto *ids = playlistIds(name)) {
        std::set<std::string> seen;
        for (const std::string &id : *ids) {
            const auto at = m_library->trackIndex.find(id);
            if (at != m_library->trackIndex.end() && m_library->tracks[at->second].item >= 0 && seen.insert(id).second) {
                out.push_back(id);
            }
        }
    }
    return out;
}

void BeatGridsController::rebuildRows()
{
    std::vector<BeatGridTrackListModel::Row> rows;
    if (m_library && !m_library->engineTooOld) {
        const auto add = [&](const BeatGridLibrary::Track &track) {
            BeatGridTrackListModel::Row row;
            row.rekordboxId = QString::fromStdString(track.rekordboxId);
            row.title = QString::fromStdString(track.title);
            row.artist = QString::fromStdString(track.artist);
            row.bpm = track.bpm;
            row.keyName = QString::fromStdString(track.key);
            row.artworkUrl = track.artworkUrl;
            row.fallbackArtworkUrl = track.fallbackArtworkUrl;
            row.matched = track.item >= 0;
            describe(row);
            rows.push_back(std::move(row));
        };
        if (const auto *ids = playlistIds(m_shownPlaylist)) {
            rows.reserve(ids->size());
            for (const std::string &id : *ids) {
                if (const auto at = m_library->trackIndex.find(id); at != m_library->trackIndex.end()) {
                    add(m_library->tracks[at->second]);
                }
            }
        } else {
            rows.reserve(m_library->tracks.size());
            for (const auto &track : m_library->tracks) {
                add(track);
            }
        }
    }
    int notInEngine = 0;
    for (const auto &row : rows) {
        notInEngine += row.matched ? 0 : 1;
    }
    m_tracks.setRows(std::move(rows));
    if (notInEngine != m_notInEngineShown) {
        m_notInEngineShown = notInEngine;
        emit notInEngineChanged();
    }
}

void BeatGridsController::describe(BeatGridTrackListModel::Row &row) const
{
    row.ticked = m_ticked.contains(row.rekordboxId);
    row.pending = false;
    row.gridState.clear();
    row.gridTone.clear();
    row.gridTip.clear();
    row.waveformState.clear();
    row.waveformTone.clear();
    row.waveformTip.clear();
    row.note.clear();
    row.noteTone.clear();
    if (!row.matched) {
        row.note = QStringLiteral("Not in the Engine library yet: sync the libraries first");
        row.noteTone = QStringLiteral("muted");
        return;
    }
    if (!m_options.grids && !m_options.waveforms) {
        row.gridState = row.waveformState = QStringLiteral("Not chosen");
        row.gridTone = row.waveformTone = QStringLiteral("muted");
        return;
    }
    const auto state = m_states.find(row.rekordboxId.toStdString());
    if (state == m_states.end()) {
        row.pending = true;
        return;
    }
    const BeatGridTrackState &s = state->second;
    if (s.failed) {
        row.gridState = row.waveformState = QStringLiteral("Could not be read");
        row.gridTone = row.waveformTone = QStringLiteral("danger");
        row.gridTip = row.waveformTip = s.failure;
        return;
    }
    const Words grid = gridWords(s);
    const Words waveform = waveformWords(s);
    row.gridState = grid.text;
    row.gridTone = grid.tone;
    row.gridTip = grid.tip;
    row.waveformState = waveform.text;
    row.waveformTone = waveform.tone;
    row.waveformTip = waveform.tip;
    if (s.result.completedRow) {
        row.note = QStringLiteral("Loads without analysis");
        row.noteTone = QStringLiteral("good");
    } else if (notAnalysed(s)) {
        row.note = whyLeftForThePlayer(s);
        row.noteTone = QStringLiteral("warn");
    }
    // The way back, said quietly on the row whatever else it says.
    if (s.facts.kept == application::EngineAnalysisFacts::Kept::Kept) {
        const QString kept = QStringLiteral("Engine's analysis kept");
        if (row.note.isEmpty()) {
            row.note = kept;
            row.noteTone = QStringLiteral("muted");
        } else {
            row.note += QStringLiteral(". ") + kept;
        }
    }
}

void BeatGridsController::refreshRows()
{
    m_tracks.refresh([this](BeatGridTrackListModel::Row &row) { describe(row); });
}

void BeatGridsController::setShownPlaylist(const QString &name)
{
    const QString wanted = playlistIds(name) ? name : QString();
    if (wanted == m_shownPlaylist) {
        return;
    }
    m_shownPlaylist = wanted;
    emit shownPlaylistChanged();
    rebuildRows();
    schedulePreview();
}

void BeatGridsController::setGrids(bool on)
{
    if (m_options.grids == on || !m_stagedIds.isEmpty()) {
        return;
    }
    m_options.grids = on;
    emit optionsChanged();
    invalidateStates();
}

void BeatGridsController::setWaveforms(bool on)
{
    if (m_options.waveforms == on || !m_stagedIds.isEmpty()) {
        return;
    }
    m_options.waveforms = on;
    emit optionsChanged();
    invalidateStates();
}

void BeatGridsController::setReplaceEngineEdits(bool on)
{
    if (m_options.overwriteEngineEdits == on || !m_stagedIds.isEmpty()) {
        return;
    }
    m_options.overwriteEngineEdits = on;
    emit optionsChanged();
    invalidateStates();
}

void BeatGridsController::invalidateStates()
{
    ++m_generation;
    m_states.clear();
    refreshRows();
    recount();
    schedulePreview();
}

void BeatGridsController::setTicked(const QString &rekordboxId, bool ticked)
{
    if (!m_stagedIds.isEmpty() || !m_library) {
        return;
    }
    const auto at = m_library->trackIndex.find(rekordboxId.toStdString());
    if (at == m_library->trackIndex.end() || m_library->tracks[at->second].item < 0 || m_library->engineTooOld) {
        return;
    }
    if (ticked == m_ticked.contains(rekordboxId)) {
        return;
    }
    if (ticked) {
        m_ticked.insert(rekordboxId);
    } else {
        m_ticked.remove(rekordboxId);
    }
    ticksMoved();
}

void BeatGridsController::setPlaylistTicked(const QString &name, bool ticked)
{
    if (!m_stagedIds.isEmpty()) {
        return;
    }
    bool moved = false;
    for (const std::string &id : tickableIn(name)) {
        const QString qid = QString::fromStdString(id);
        if (ticked && !m_ticked.contains(qid)) {
            m_ticked.insert(qid);
            moved = true;
        } else if (!ticked && m_ticked.remove(qid)) {
            moved = true;
        }
    }
    if (moved) {
        ticksMoved();
    }
}

void BeatGridsController::ticksMoved()
{
    ++m_tickRevision;
    emit ticksChanged();
    refreshRows();
    recount();
    schedulePreview();
}

int BeatGridsController::playlistCheckState(const QString &name) const
{
    const auto ids = tickableIn(name);
    int ticked = 0;
    for (const std::string &id : ids) {
        ticked += m_ticked.contains(QString::fromStdString(id)) ? 1 : 0;
    }
    if (ticked == 0) {
        return Qt::Unchecked;
    }
    return ticked == static_cast<int>(ids.size()) ? Qt::Checked : Qt::PartiallyChecked;
}

int BeatGridsController::tickablePlaylistTracks(const QString &name) const
{
    return static_cast<int>(tickableIn(name).size());
}

// --- previews ----------------------------------------------------------

void BeatGridsController::schedulePreview()
{
    // Once per turn of the event loop: a playlist ticked and shown in one
    // go asks once.
    if (m_previewScheduled) {
        return;
    }
    m_previewScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_previewScheduled = false;
        runPreview();
    });
}

void BeatGridsController::runPreview()
{
    if (!m_library || m_library->engineTooOld || (!m_options.grids && !m_options.waveforms)) {
        if (m_preview.busy()) {
            m_preview.cancel();
        }
        return;
    }
    // The tracks shown first, in the list's order, then the ticked ones:
    // every one that can be copied and has no state of this generation.
    std::vector<application::BeatGridCopyItem> items;
    std::set<std::string> wanted;
    const auto want = [&](const std::string &id) {
        if (m_states.contains(id) || wanted.contains(id)) {
            return;
        }
        const auto at = m_library->trackIndex.find(id);
        if (at == m_library->trackIndex.end() || m_library->tracks[at->second].item < 0) {
            return;
        }
        wanted.insert(id);
        items.push_back(m_library->plan.items[static_cast<size_t>(m_library->tracks[at->second].item)]);
    };
    for (const auto &row : m_tracks.rows()) {
        want(row.rekordboxId.toStdString());
    }
    QStringList ticked(m_ticked.begin(), m_ticked.end());
    ticked.sort();
    for (const QString &id : std::as_const(ticked)) {
        want(id.toStdString());
    }
    if (items.empty()) {
        // What a batch still running would work out is known already.
        if (m_preview.busy()) {
            m_preview.cancel();
        }
        setPreviewProgress(0, 0);
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const auto &item : items) {
        hash.addData(QByteArray::fromStdString(item.rekordboxTrackId));
        hash.addData(QByteArrayView("\n"));
    }
    const QString key = QString::number(m_generation) + QLatin1Char(':') + QString::fromLatin1(hash.resultView().toByteArray().toHex());
    if (m_preview.busy() && m_preview.key() == key) {
        return;
    }

    auto reporter = makeMainThreadShared<QtProgressReporter>();
    const auto current = m_preview.speaksForNext();
    connect(reporter.get(), &QtProgressReporter::started, this, [this, current](const QString &, int total) {
        if (current()) {
            setPreviewProgress(0, total);
        }
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this, [this, current](int done) {
        if (current()) {
            setPreviewProgress(done, m_previewTotal);
        }
    });
    // Rows of a batch that was superseded are still true of their
    // generation, so they are taken whichever batch sent them.
    const std::uint64_t generation = m_generation;
    if (!m_previewSink) {
        QPointer<BeatGridsController> self(this);
        m_previewSink = makeMainThreadShared<PreviewSink>([self](std::uint64_t made, PreviewRows rows) {
            if (self) {
                self->takePreviewRows(made, std::move(rows));
            }
        });
    }
    auto sink = std::static_pointer_cast<PreviewSink>(m_previewSink);

    const int itemCount = int(items.size());
    PreviewInput input{m_library, std::move(items), m_options};
    using Request = AsyncRequest<QString>;
    Request::Work work = [input = std::move(input), reporter, sink, generation](application::CancellationToken cancel) {
        return runPreviewBatch(
            input, [&sink, generation](PreviewRows rows) { sink->post(generation, std::move(rows)); }, reporter,
            cancel);
    };
    Request::Ending ending{
        [this, generation](QString &&ledgerError) {
            if (generation == m_generation && ledgerError != m_ledgerError) {
                m_ledgerError = ledgerError;
                emit ledgerWarningChanged();
            }
            setPreviewProgress(0, 0);
        },
        [this](const QString &message) {
            setErrorMessage(message);
            setPreviewProgress(0, 0);
        },
        []() {},
    };
    setErrorMessage({});
    m_preview.restart(key, stickRootOf(m_rekordboxPath), std::move(work), std::move(ending));
}

void BeatGridsController::takePreviewRows(std::uint64_t generation, std::vector<std::pair<std::string, BeatGridTrackState>> rows)
{
    if (generation != m_generation) {
        return;
    }
    for (auto &[id, state] : rows) {
        m_states[id] = std::move(state);
    }
    refreshRows();
    recount();
}

void BeatGridsController::recount()
{
    Summary s;
    std::map<QString, int> reasons;
    for (const QString &id : std::as_const(m_ticked)) {
        const auto state = m_states.find(id.toStdString());
        if (state == m_states.end()) {
            ++s.pending;
            continue;
        }
        const BeatGridTrackState &t = state->second;
        if (!t.failed) {
            s.kept += t.facts.kept == application::EngineAnalysisFacts::Kept::Kept ? 1 : 0;
            s.restorable += t.canRestore() ? 1 : 0;
            s.analysed += t.facts.rowAnalysed ? 1 : 0;
        }
        if (t.wouldWrite()) {
            ++s.rows;
            s.grids += t.result.grid.outcome == Outcome::Written ? 1 : 0;
            s.waveforms += t.result.waveform == Waveform::Written ? 1 : 0;
            s.completed += t.result.completedRow ? 1 : 0;
        } else {
            ++s.leftAlone;
            ++reasons[leftAloneReason(t)];
        }
    }
    if (!m_options.grids && !m_options.waveforms) {
        // Nothing is worked out; a tick waits for a part to be chosen.
        s = Summary{};
        s.text = QStringLiteral("Choose beat grids, waveforms or both.");
    }
    std::vector<std::pair<QString, int>> ordered(reasons.begin(), reasons.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    QStringList parts;
    for (const auto &[reason, count] : ordered) {
        s.groups.append(QVariantMap{{QStringLiteral("reason"), reason}, {QStringLiteral("count"), count}});
        parts << QString::number(count) + QLatin1Char(' ') + reason;
    }
    if (s.leftAlone > 0) {
        s.leftAloneText = plural(s.leftAlone, QStringLiteral("One ticked track is left alone"),
                                 QStringLiteral("ticked tracks are left alone"))
            + QStringLiteral(": ") + parts.join(QStringLiteral(", ")) + QLatin1Char('.');
    }
    if (s.text.isEmpty()) {
        const int ticked = static_cast<int>(m_ticked.size());
        if (ticked == 0) {
            s.text = QStringLiteral("Nothing is ticked. Tick a playlist or tracks to copy their beat grids and "
                                    "waveforms.");
        } else {
            QStringList what;
            if (m_options.grids) {
                what << plural(s.grids, QStringLiteral("one beat grid"), QStringLiteral("beat grids"));
            }
            if (m_options.waveforms) {
                what << plural(s.waveforms, QStringLiteral("one waveform"), QStringLiteral("waveforms"));
            }
            QString text = plural(ticked, QStringLiteral("One track ticked"), QStringLiteral("tracks ticked"))
                + QStringLiteral(": ") + what.join(QStringLiteral(" and ")) + QStringLiteral(" to copy");
            if (s.completed > 0) {
                text += QStringLiteral(", ")
                    + plural(s.completed, QStringLiteral("one track then loads"), QStringLiteral("tracks then load"))
                    + QStringLiteral(" without analysis");
            }
            text += QLatin1Char('.');
            if (s.pending > 0) {
                text += QStringLiteral(" Still checking ") + plural(s.pending, QStringLiteral("one of them"),
                                                                       QStringLiteral("of them"))
                    + QLatin1Char('.');
            }
            s.text = text;
        }
    }
    if (s.kept > 0) {
        s.engineAnalysisText = QStringLiteral("Engine's own analysis is kept for ")
            + plural(s.kept, QStringLiteral("one ticked track"), QStringLiteral("ticked tracks"));
        if (s.restorable == s.kept) {
            s.engineAnalysisText += QStringLiteral(", ready to be put back.");
        } else {
            s.engineAnalysisText += QStringLiteral("; ") + QString::number(s.restorable)
                + QStringLiteral(" can be put back, the rest changed on the player since.");
        }
    }
    m_summary = std::move(s);
    emit summaryChanged();
}

// --- staging and saving ------------------------------------------------

void BeatGridsController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    LibraryEditSession *session = any.isEmpty() ? nullptr : registry->sessionFor(registry->libraryIdForPath(any));
    if (session == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    emit writingChanged();
    if (!m_session) {
        return;
    }
    connect(m_session, &LibraryEditSession::stateChanged, this, [this]() {
        emit writingChanged();
        emit summaryChanged();
    });
    connect(m_session, &LibraryEditSession::changesDiscarded, this, &BeatGridsController::clearStaged);
    connect(m_session, &LibraryEditSession::changeApplied, this,
            [this](const QString &id) { m_appliedThisSave << id; });
    connect(m_session, &LibraryEditSession::saveFinished, this, &BeatGridsController::onSaveFinished);
}

void BeatGridsController::onSaveFinished(const QVariantMap &summary)
{
    const QStringList applied = std::exchange(m_appliedThisSave, {});
    // What landed, from the changes of this page the session applied:
    // a change rolled back, failed or never reached is not among them, and
    // one skipped at save time recorded nothing written.
    LastSave counted;
    bool ours = false;
    if (m_tally) {
        for (const QString &id : applied) {
            const auto row = m_tally->rowFor(id);
            if (!row) {
                continue;
            }
            ours = true;
            counted.restored += row->restored ? 1 : 0;
            counted.marked += row->markedForPlayer ? 1 : 0;
            counted.grids += row->grid ? 1 : 0;
            counted.waveforms += row->waveform ? 1 : 0;
            counted.completed += row->completed ? 1 : 0;
            counted.tracks += row->grid || row->waveform ? 1 : 0;
        }
    }
    QStringList still;
    for (const QString &id : std::as_const(m_stagedIds)) {
        if (m_session && m_session->hasChange(id)) {
            still << id;
        }
    }
    if (still != m_stagedIds) {
        m_stagedIds = still;
        emit stagedChanged();
    }
    if (m_stagedIds.isEmpty()) {
        m_tally.reset();
    }
    if (ours && (counted.restored > 0 || counted.marked > 0)) {
        QStringList parts;
        if (counted.restored > 0) {
            parts << QStringLiteral("Engine's analysis restored for ")
                    + plural(counted.restored, QStringLiteral("one track"), QStringLiteral("tracks"));
        }
        if (counted.marked > 0) {
            parts << plural(counted.marked, QStringLiteral("One track"), QStringLiteral("tracks"))
                    + QStringLiteral(" left for the player to analyse");
        }
        QString text = parts.join(QStringLiteral(", "));
        const int skipped = summary.value(QStringLiteral("skipped")).toInt();
        if (skipped > 0) {
            text += QStringLiteral(", ") + plural(skipped, QStringLiteral("one track skipped"),
                                                  QStringLiteral("tracks skipped"));
        }
        counted.text = text;
        m_lastSave = counted;
        emit lastSaveChanged();
    } else if (ours) {
        QStringList parts;
        if (counted.grids > 0 || counted.waveforms == 0) {
            parts << plural(counted.grids, QStringLiteral("One beat grid"), QStringLiteral("beat grids"));
        }
        if (counted.waveforms > 0) {
            parts << plural(counted.waveforms, parts.isEmpty() ? QStringLiteral("One waveform") : QStringLiteral("one waveform"),
                            QStringLiteral("waveforms"));
        }
        QString text = parts.join(QStringLiteral(" and ")) + QStringLiteral(" copied");
        if (counted.completed > 0) {
            text += QStringLiteral(", ")
                + plural(counted.completed, QStringLiteral("one track now loads"), QStringLiteral("tracks now load"))
                + QStringLiteral(" without analysis");
        }
        const int skipped = summary.value(QStringLiteral("skipped")).toInt();
        if (skipped > 0) {
            text += QStringLiteral(", ") + plural(skipped, QStringLiteral("one track skipped"),
                                                  QStringLiteral("tracks skipped"));
        }
        counted.text = text;
        m_lastSave = counted;
        emit lastSaveChanged();
    }
    // Whatever the save was (this page's copies, an undo), the rows may
    // have changed: every state is worked out again. Another page's save
    // (Sync after Rekordbox Export adds Engine rows) may also have changed
    // which tracks are in both libraries: the library is read again when
    // the page is in front again (reloadIfStale()).
    if (m_library) {
        if (!ours) {
            m_libraryStale = true;
        }
        invalidateStates();
    }
    if (ours) {
        const bool whole = summary.value(QStringLiteral("error")).toString().isEmpty()
            && summary.value(QStringLiteral("warning")).toString().isEmpty()
            && !summary.value(QStringLiteral("cancelled")).toBool() && summary.value(QStringLiteral("skipped")).toInt() == 0
            && summary.value(QStringLiteral("written")).toInt() == summary.value(QStringLiteral("total")).toInt();
        emit saveCounted(whole);
    }
}

void BeatGridsController::stage()
{
    if (!canStage()) {
        return;
    }
    setErrorMessage({});
    attachSession();
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick's library could not be identified; nothing was changed."));
        return;
    }
    m_session->setLibraryPaths(m_rekordboxPath, m_enginePath);
    std::vector<std::pair<const application::BeatGridCopyItem *, const BeatGridTrackState *>> chosen;
    std::vector<std::pair<std::string, std::string>> riders;
    QStringList ticked(m_ticked.begin(), m_ticked.end());
    ticked.sort();
    for (const QString &id : std::as_const(ticked)) {
        const auto state = m_states.find(id.toStdString());
        const auto at = m_library->trackIndex.find(id.toStdString());
        if (state == m_states.end() || at == m_library->trackIndex.end() || m_library->tracks[at->second].item < 0) {
            continue;
        }
        const BeatGridTrackState &t = state->second;
        // A ticked row already the same: no change of its own, but its grid
        // goes into the ledger with the batch, as the command line records
        // it, so a ledger that was lost does not leave it taken for the
        // player's own.
        if (!t.failed && t.result.grid.outcome == Outcome::AlreadySame && !t.result.grid.gridFingerprint.empty()
            && !t.options.keepKey.empty()) {
            riders.emplace_back(t.options.keepKey, t.result.grid.gridFingerprint);
        }
        if (!t.wouldWrite() || t.analysisDigest.empty()) {
            continue;
        }
        chosen.emplace_back(&m_library->plan.items[static_cast<size_t>(m_library->tracks[at->second].item)],
                            &state->second);
    }
    if (chosen.empty()) {
        return;
    }
    auto tally = std::make_shared<AnalysisSaveTally>();
    const int count = static_cast<int>(chosen.size());
    std::vector<std::unique_ptr<PendingChange>> changes;
    changes.reserve(chosen.size());
    QStringList ids;
    for (const auto &[item, state] : chosen) {
        auto change = std::make_unique<CopyAnalysisChange>(
            m_library->enginePath, m_library->rekordboxPath, m_library->stickRoot, *item, state->analysisDigest,
            state->options, state->result.overviewFileRelativePath, count, changes.empty(), tally);
        if (changes.empty()) {
            change->setLedgerRiders(std::move(riders));
        }
        changes.push_back(std::move(change));
        ids << CopyAnalysisChange::idFor(item->engineTrackId);
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session said why (another page's edits, or the lock)
    }
    m_tally = tally;
    m_stagedIds = ids;
    m_stagedAction = QStringLiteral("copy");
    emit stagedChanged();
    emit summaryChanged();
}

void BeatGridsController::stageRestore()
{
    if (canRestore()) {
        stageEngineAnalysisChanges(true);
    }
}

void BeatGridsController::stageLetPlayerAnalyse()
{
    if (canLetPlayerAnalyse()) {
        stageEngineAnalysisChanges(false);
    }
}

void BeatGridsController::stageEngineAnalysisChanges(bool restore)
{
    setErrorMessage({});
    attachSession();
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick's library could not be identified; nothing was changed."));
        return;
    }
    m_session->setLibraryPaths(m_rekordboxPath, m_enginePath);
    std::vector<std::pair<const application::BeatGridCopyItem *, const BeatGridTrackState *>> chosen;
    QStringList ticked(m_ticked.begin(), m_ticked.end());
    ticked.sort();
    for (const QString &id : std::as_const(ticked)) {
        const auto state = m_states.find(id.toStdString());
        const auto at = m_library->trackIndex.find(id.toStdString());
        if (state == m_states.end() || at == m_library->trackIndex.end() || m_library->tracks[at->second].item < 0) {
            continue;
        }
        const BeatGridTrackState &t = state->second;
        if (restore ? !t.canRestore() : (t.failed || !t.facts.rowAnalysed)) {
            continue;
        }
        chosen.emplace_back(&m_library->plan.items[static_cast<size_t>(m_library->tracks[at->second].item)], &t);
    }
    if (chosen.empty()) {
        return;
    }
    auto tally = std::make_shared<AnalysisSaveTally>();
    const int count = static_cast<int>(chosen.size());
    std::vector<std::unique_ptr<PendingChange>> changes;
    changes.reserve(chosen.size());
    QStringList ids;
    using Kind = EngineAnalysisChange::Kind;
    for (const auto &[item, state] : chosen) {
        const std::string overview = restore && state->restore ? state->restore->overviewFileRelativePath : std::string();
        changes.push_back(std::make_unique<EngineAnalysisChange>(restore ? Kind::Restore : Kind::LetPlayerAnalyse,
                                                                 m_library->enginePath, m_library->stickRoot, *item,
                                                                 overview, count, changes.empty(), tally));
        ids << CopyAnalysisChange::idFor(item->engineTrackId);
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session said why (another page's edits, or the lock)
    }
    m_tally = tally;
    m_stagedIds = ids;
    m_stagedAction = restore ? QStringLiteral("restore") : QStringLiteral("letPlayerAnalyse");
    emit stagedChanged();
    emit summaryChanged();
}

void BeatGridsController::unstage()
{
    if (m_stagedIds.isEmpty()) {
        return;
    }
    if (m_session) {
        m_session->unstageAll(m_stagedIds);
    }
    clearStaged();
}

void BeatGridsController::clearStaged()
{
    if (m_stagedIds.isEmpty()) {
        return;
    }
    m_stagedIds.clear();
    m_tally.reset();
    emit stagedChanged();
    emit summaryChanged();
}

void BeatGridsController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void BeatGridsController::setLoadProgress(int current, int total)
{
    if (m_loadCurrent == current && m_loadTotal == total) {
        return;
    }
    m_loadCurrent = current;
    m_loadTotal = total;
    emit loadProgressChanged();
}

void BeatGridsController::setPreviewProgress(int current, int total)
{
    if (m_previewCurrent == current && m_previewTotal == total) {
        return;
    }
    m_previewCurrent = current;
    m_previewTotal = total;
    emit previewProgressChanged();
}

}  // namespace seabass::gui
