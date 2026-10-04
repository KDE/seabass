// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/align_playlist_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "playlist-sync";

QString libraryName(const std::string &format)
{
    if (format == "rekordbox") {
        return QStringLiteral("rekordbox");
    }
    if (format == "onelibrary") {
        return QStringLiteral("OneLibrary");
    }
    return QStringLiteral("Engine");
}

}  // namespace

AlignPlaylistChange::AlignPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist,
                                         std::string reference, std::vector<domain::PlaylistAlignment> alignments,
                                         int itemCountHint)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_reference(std::move(reference)), m_alignments(std::move(alignments)), m_itemCountHint(itemCountHint)
{
}

QString AlignPlaylistChange::idFor(const std::string &playlist)
{
    return QStringLiteral("playlist-sync:") + QString::fromStdString(playlist);
}

QString AlignPlaylistChange::id() const
{
    return idFor(m_playlist);
}

QString AlignPlaylistChange::owner() const
{
    return QStringLiteral("library-health");
}

QString AlignPlaylistChange::description() const
{
    return QStringLiteral("Make \"%1\" match its %2 version in every library")
        .arg(QString::fromStdString(m_playlist), libraryName(m_reference));
}

QString AlignPlaylistChange::subject() const
{
    return QString::fromStdString(m_playlist);
}

QString AlignPlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString AlignPlaylistChange::verb() const
{
    return QStringLiteral("matched");
}

QStringList AlignPlaylistChange::formatsTouched() const
{
    QStringList formats;
    for (const auto &a : m_alignments) {
        formats << QString::fromStdString(a.format);
    }
    return formats;
}

std::vector<BackupTarget> AlignPlaylistChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    for (const auto &a : m_alignments) {
        if (a.format == "rekordbox") {
            targets.push_back({pathToUtf8(pathFromUtf8(pioneer) / "rekordbox" / "export.pdb"), Label});
        } else if (a.format == "onelibrary") {
            targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
        } else if (a.format == "engine") {
            targets.push_back({pathToUtf8(pathFromUtf8(m_enginePath.toStdString()) / "Database2" / "m.db"), Label});
        }
    }
    return targets;
}

ChangeOutcome AlignPlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    int added = 0;
    int removed = 0;
    try {
        for (const auto &a : m_alignments) {
            if (a.format == "rekordbox") {
                // Removals and additions the way rekordbox makes them
                // (PdbRowWriter::removePlaylistEntries and
                // appendPlaylistEntries, #62), refused whole when the
                // rows cannot be placed as rekordbox would.
                if (a.remove.empty() && a.add.empty()) {
                    continue;
                }
                FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
                const std::string root = session.writeRoot();
                const auto ids = infrastructure::rekordbox::rekordboxPlaylistIdsByPath(root);
                const auto playlist = ids.find(m_playlist);
                if (playlist == ids.end()) {
                    return ChangeOutcome::failure(QStringLiteral("rekordbox has no playlist \"%1\" any more")
                                                      .arg(QString::fromStdString(m_playlist)));
                }
                infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pathFromUtf8(root) / "rekordbox" / "export.pdb"));
                size_t cleared = 0;
                size_t appended = 0;
                try {
                    if (!a.remove.empty()) {
                        std::set<uint32_t> ids;
                        for (const auto &track : a.remove) {
                            ids.insert(static_cast<uint32_t>(std::stoul(track.sourceId)));
                        }
                        cleared = rows.removePlaylistEntries(playlist->second, ids);
                    }
                    if (!a.add.empty()) {
                        std::vector<uint32_t> ids;
                        for (const auto &track : a.add) {
                            ids.push_back(static_cast<uint32_t>(std::stoul(track.sourceId)));
                        }
                        appended = rows.appendPlaylistEntries(playlist->second, ids);
                    }
                } catch (const infrastructure::rekordbox::PdbPageFull &e) {
                    return ChangeOutcome::failure(
                        QStringLiteral("export.pdb: the playlist's entries could not be placed (%1)").arg(QString::fromStdString(e.what())));
                }
                if (cleared + appended > 0) {
                    if (!rows.commit()) {
                        return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                    }
                    session.noteItemApplied();
                    removed += static_cast<int>(cleared);
                    added += static_cast<int>(appended);
                }
            } else if (a.format == "onelibrary") {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
                auto &writer = sharedOneLibraryWriter(ctx, pioneer);
                // A row gone since the scan (the leftover repair in the
                // same save removes it) has nothing to add or take out.
                for (const auto &track : a.add) {
                    try {
                        added += writer.addToPlaylist(m_playlist, track.filePath) ? 1 : 0;
                    } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    }
                }
                for (const auto &track : a.remove) {
                    try {
                        removed += writer.removeFromPlaylist(m_playlist, track.filePath) ? 1 : 0;
                    } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    }
                }
            } else if (a.format == "engine") {
                FormatWriteSession &session =
                    sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
                const std::string root = session.writeRoot();
                for (const auto &track : a.add) {
                    added += infrastructure::engine::addToEnginePlaylist(root, m_playlist, std::stoll(track.sourceId)) ? 1 : 0;
                }
                for (const auto &track : a.remove) {
                    removed +=
                        infrastructure::engine::removeFromEnginePlaylist(root, m_playlist, std::stoll(track.sourceId)) ? 1 : 0;
                }
                session.noteItemApplied();
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    ctx.log().record(std::string(Label) + ": \"" + m_playlist + "\" matched to " + m_reference + ": " +
                     std::to_string(added) + " entr(ies) added, " + std::to_string(removed) + " removed");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
