#include "gui/edit/changes/add_cue_change.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "application/ports/cue_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

QString formatPosition(double positionMs)
{
    int totalSeconds = static_cast<int>(positionMs / 1000.0);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

}  // namespace

AddCueChange::AddCueChange(QString format, QString path, QString sourceId, double positionMs, QString kind,
                            int hotCueNumber, QString color, QString comment, bool isLoop, double loopEndMs,
                            QString trackTitle)
    : m_format(std::move(format)),
      m_path(std::move(path)),
      m_sourceId(std::move(sourceId)),
      m_positionMs(positionMs),
      m_kind(std::move(kind)),
      m_hotCueNumber(hotCueNumber),
      m_color(std::move(color)),
      m_comment(std::move(comment)),
      m_isLoop(isLoop),
      m_loopEndMs(loopEndMs),
      m_trackTitle(std::move(trackTitle))
{
}

QString AddCueChange::id() const
{
    // A hot slot can hold one cue: staging the same slot twice replaces
    // the earlier one. Memory cues are distinct per position.
    QString slot = m_kind == "hot" ? QStringLiteral("hot%1").arg(m_hotCueNumber)
                                   : QStringLiteral("memory@%1").arg(static_cast<int>(m_positionMs));
    return "addcue:" + m_format + ":" + m_sourceId + ":" + slot;
}

QString AddCueChange::description() const
{
    QString what = m_isLoop ? QStringLiteral("hot loop %1").arg(m_hotCueNumber)
        : m_kind == "hot"  ? QStringLiteral("hot cue %1").arg(m_hotCueNumber)
                           : QStringLiteral("memory cue");
    QString track = m_trackTitle.isEmpty() ? "track " + m_sourceId : "\"" + m_trackTitle + "\"";
    return QStringLiteral("Add %1 at %2 to %3").arg(what, formatPosition(m_positionMs), track);
}

QString AddCueChange::verb() const
{
    return QStringLiteral("added");
}

QString AddCueChange::unit() const
{
    return QStringLiteral("cues");
}

QStringList AddCueChange::formatsTouched() const
{
    return {m_format};
}

QVariantMap AddCueChange::summary() const
{
    return {
        {"changeId", id()},
        {"sourceId", m_sourceId},
        {"kind", m_kind},
        {"hotCueNumber", m_hotCueNumber},
        {"positionMs", m_positionMs},
        {"isLoop", m_isLoop},
        {"loopEndMs", m_loopEndMs},
        {"color", m_color},
        {"comment", m_comment},
    };
}

std::vector<BackupTarget> AddCueChange::filesToBackup(SaveContext &ctx) const
{
    std::vector<BackupTarget> targets;
    for (const auto &file :
         filesWrittenFor(WriteScope{.catalogRows = false, .oneLibraryMirror = true}, {m_format.toStdString(), m_sourceId.toStdString()}, m_path, ctx)) {
        targets.push_back({file, "add-cue"});
    }
    return targets;
}

ChangeOutcome AddCueChange::apply(SaveContext &ctx)
{
    // Never trust whatever cue list the calling page had cached --
    // re-scan fresh (via the shared cache, which itself re-reads
    // whenever the catalog's mtime has moved) so the augmented list
    // below always starts from this track's real current state.
    std::vector<domain::Track> tracks = LibraryCatalogCache::instance().tracksFor(
        m_format.toStdString(), m_path.toStdString(), ctx.progress(), ctx.cancel());

    std::string id = m_sourceId.toStdString();
    const domain::Track *track = nullptr;
    for (const auto &t : tracks) {
        if (t.sourceId == id) {
            track = &t;
            break;
        }
    }
    if (!track) {
        return ChangeOutcome::failure("This track no longer exists in the library -- rescan and try again.");
    }

    domain::CuePoint newCue;
    newCue.kind = m_kind == "hot" ? domain::CuePoint::Kind::Hot : domain::CuePoint::Kind::Memory;
    newCue.hotCueNumber = newCue.kind == domain::CuePoint::Kind::Hot ? m_hotCueNumber : 0;
    newCue.positionMs = m_positionMs;
    newCue.isLoop = m_isLoop;
    newCue.loopEndMs = m_isLoop ? m_loopEndMs : 0.0;
    newCue.color = m_color.toStdString();
    newCue.comment = m_comment.toStdString();

    std::vector<domain::CuePoint> cues = track->cues;
    if (newCue.kind == domain::CuePoint::Kind::Hot) {
        // A hardware hot-cue pad can only ever hold one cue at a time
        // -- replace whatever's already in this slot rather than
        // adding a second entry claiming the same number, which every
        // writer here already assumes can't happen.
        cues.erase(std::remove_if(cues.begin(), cues.end(),
                                  [&](const domain::CuePoint &c) {
                                      return c.kind == domain::CuePoint::Kind::Hot
                                          && c.hotCueNumber == newCue.hotCueNumber;
                                  }),
                   cues.end());
    }
    cues.push_back(newCue);

    std::string pioneerRoot = m_path.toStdString();
    std::string kind = m_kind.toStdString();
    std::string positionText = std::to_string(static_cast<int>(m_positionMs));

    if (m_format == "onelibrary") {
        // OneLibraryCueWriter is deliberately not an
        // application::CueWriter (it keys by file path, not sourceId
        // -- see its class comment), so this is a separate primary
        // write path rather than another branch of the writer
        // dispatch below.
        if (track->filePath.empty()) {
            return ChangeOutcome::failure("This track has no known file path in OneLibrary -- can't write a cue.");
        }
        ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot), "add-cue");
        sharedOneLibraryWriter(ctx, pioneerRoot).writeCuesForPath(track->filePath, cues);
        ctx.log().record("add-cue: added " + kind + " cue at " + positionText + "ms to OneLibrary track id=" + id
                         + " (\"" + track->title + "\")");
        return ChangeOutcome::success();
    }

    application::CueWriter *writer = nullptr;
    std::unique_ptr<application::CueWriter> ownedWriter;
    if (m_format == "rekordbox") {
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        ownedWriter = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(pioneerRoot, pathIndex);
        writer = ownedWriter.get();
        auto analyzePath = pathIndex
            ? pathIndex->pathFor(static_cast<uint32_t>(std::stoul(id)))
            : infrastructure::rekordbox::findAnlzPathForTrackId(pioneerRoot, static_cast<uint32_t>(std::stoul(id)));
        if (analyzePath) {
            ctx.backupOnce(infrastructure::rekordbox::extAnlzPath(pioneerRoot, *analyzePath), "add-cue");
        }
    } else {
        // Shared for the save: opening an Engine library is a full SQLite
        // open plus schema detection, and a save adding several cues to
        // the same library should pay that once.
        writer = &sharedEngineCueWriter(ctx, pioneerRoot);
        ctx.backupOnce((fs::path(pioneerRoot) / "Database2" / "m.db").string(), "add-cue");
    }

    writer->writeHotCues(id, cues);
    // Right away, not after the save: the next AddCueChange in this save
    // re-reads the track through the cache, and the cache's freshness
    // key is the catalog's mtime, which FAT32 keeps in 2-second steps. A
    // second cue added within those two seconds read the list from before
    // the first write and wrote it back without that cue.
    LibraryCatalogCache::instance().invalidateWithOneLibraryMirror(m_format.toStdString(), m_path.toStdString());
    ctx.log().record("add-cue: added " + kind + (m_isLoop ? " loop" : " cue") + " at " + positionText + "ms to track id="
                     + id + " (\"" + track->title + "\")");

    // Best-effort OneLibrary mirror -- same secondary write every
    // other rekordbox cue path here already does, never fatal to
    // the primary write above.
    if (m_format == "rekordbox" && !track->filePath.empty()
        && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot)) {
        try {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot), "add-cue");
            sharedOneLibraryWriter(ctx, pioneerRoot).writeCuesForPath(track->filePath, cues);
            ctx.log().record("add-cue: also wrote into OneLibrary");
        } catch (const std::exception &e) {
            ctx.log().record(std::string("add-cue: OneLibrary write failed: ") + e.what());
        }
    }

    if (m_format == "engine" && newCue.kind == domain::CuePoint::Kind::Memory) {
        int otherMemoryCues = static_cast<int>(
            std::count_if(track->cues.begin(), track->cues.end(),
                          [](const domain::CuePoint &c) { return c.kind == domain::CuePoint::Kind::Memory; }));
        if (otherMemoryCues > 0) {
            ctx.log().record("add-cue: note: Engine keeps only one memory cue (the earliest by position); "
                             "this one may not be kept if an existing memory cue on this track is earlier");
        }
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
