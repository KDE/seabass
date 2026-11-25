// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/track_import_staging.hpp"

#include <QSet>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "domain/import_field_checks.hpp"
#include "domain/imported_track.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

// The first `characters` characters of `text`, cut between two of them.
std::string firstCharacters(const std::string &text, size_t characters)
{
    size_t at = 0;
    for (size_t seen = 0; at < text.size() && seen < characters; ++seen) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
            ++at;
        }
    }
    return text.substr(0, at);
}

}  // namespace

StagedImport stageTrackImport(const domain::ImportPlan &plan, const ImportTarget &target,
                              application::TrackTagReader &tags, application::TrackMetadataProbe &probe,
                              const std::string &date, const std::optional<ImportPlaylist> &playlist)
{
    if (target.libraries.isEmpty() || target.stickRoot.isEmpty()) {
        throw std::invalid_argument("stageTrackImport: a stick with at least one library is required");
    }
    if (!domain::isCalendarDate(date)) {
        throw std::invalid_argument("stageTrackImport: \"" + date + "\" is not a date as YYYY-MM-DD");
    }
    if (playlist && playlist->atIndex && *playlist->atIndex < 0) {
        // Each file would be clamped to the top in turn, and the batch
        // come out backwards.
        throw std::invalid_argument("stageTrackImport: a place in a playlist is counted from 0");
    }
    int toImport = 0;
    for (const auto &entry : plan.entries) {
        toImport += entry.verdict == domain::ImportVerdict::Import ? 1 : 0;
    }

    StagedImport staged;
    if (playlist) {
        const QString name = QString::fromStdString(playlist->path);
        const PlaylistWhereabouts where = playlistWhereabouts(target.pioneerRoot, target.enginePath, playlist->path);
        const auto asSet = [](const QStringList &list) { return QSet<QString>(list.begin(), list.end()); };
        if (asSet(where.onStick) != asSet(target.libraries)) {
            staged.refusal = "The libraries on the stick (" + where.onStick.join(QStringLiteral(", ")).toStdString()
                             + ") are not the ones this import was planned for ("
                             + target.libraries.join(QStringLiteral(", ")).toStdString() + ").";
            return staged;
        }
        if (!where.several.isEmpty()) {
            staged.refusal = "More than one playlist is called \"" + playlist->path + "\" in "
                             + where.several.join(QStringLiteral(", ")).toStdString()
                             + ", so the tracks cannot be put into one of them by name.";
            return staged;
        }
        if (playlist->create && playlist->path.find('/') != std::string::npos) {
            staged.refusal = "A new playlist is made at the top level; \"" + playlist->path + "\" names a folder.";
            return staged;
        }
        if (!playlist->create && asSet(where.having) != asSet(where.onStick)) {
            QStringList lacking;
            for (const QString &library : where.onStick) {
                if (!where.having.contains(library)) {
                    lacking << library;
                }
            }
            staged.refusal = "The playlist \"" + playlist->path + "\" is not in every library on the stick (not in "
                             + lacking.join(QStringLiteral(", ")).toStdString()
                             + "), so the tracks would be listed in some libraries and not in others.";
            return staged;
        }
        // A folder at that path counts as "having" it and takes no
        // tracks: the libraries that hold it must hold it as a playlist.
        for (const QString &library : where.having) {
            const std::string catalog = library.toStdString();
            const std::vector<std::string> playlists = playlistPathsIn(
                catalog, (catalog == "engine" ? target.enginePath : target.pioneerRoot).toStdString());
            if (std::find(playlists.begin(), playlists.end(), playlist->path) == playlists.end()) {
                staged.refusal = "\"" + playlist->path + "\" is a folder in " + catalog
                                 + ", not a playlist, so it takes no tracks.";
                return staged;
            }
        }
    }
    // The batch rehearsed against export.pdb as it is, row after row,
    // each with its playlist entry.
    ImportRehearsal rehearsal(target.libraries.contains(QStringLiteral("rekordbox")) ? target.pioneerRoot : QString());
    if (playlist) {
        if (const std::string why = rehearsal.usePlaylist(playlist->path, playlist->create); !why.empty()) {
            staged.refusal = "The playlist \"" + playlist->path + "\" cannot take the tracks: " + why + ".";
            return staged;
        }
    }
    std::vector<std::shared_ptr<ImportTrackChange>> imports;
    for (const auto &entry : plan.entries) {
        if (entry.verdict != domain::ImportVerdict::Import) {
            continue;
        }
        const std::string &source = entry.offered.file.filePath;
        const auto leave = [&](std::string why) { staged.leftOut.push_back({source, std::move(why)}); };
        const std::optional<domain::TrackTags> fileTags = tags.read(source);
        const auto metadata = probe.read(source);
        if (!fileTags || !metadata) {
            leave("it could not be read as audio");
            continue;
        }
        domain::AudioFormat audio;
        audio.durationSeconds = metadata->durationSeconds;
        audio.bitrate = metadata->bitrate;
        audio.sampleRateHz = metadata->sampleRate;
        audio.fileSizeBytes = entry.offered.file.fileSizeBytes;
        domain::ImportedTrack track;
        try {
            track = domain::describeImportedTrack(*fileTags, audio, "/" + entry.destination, date);
        } catch (const std::invalid_argument &e) {
            leave(e.what());
            continue;
        }
        if (target.libraries.contains(QStringLiteral("engine")) && track.sampleRateHz <= 0) {
            leave("its sample rate could not be read, which an Engine row needs");
            continue;
        }
        {
            // Its place has to be free now, whatever the plan saw.
            std::error_code ec;
            const auto root = pathFromUtf8(target.stickRoot.toStdString());
            const auto place = root / pathFromUtf8(entry.destination);
            if (std::filesystem::symlink_status(place, ec).type() != std::filesystem::file_type::not_found) {
                leave("/" + entry.destination + " is on the stick already");
                continue;
            }
            // And every folder on the way has to be one, or be free: a
            // file where a folder goes reads as "nothing there" for what
            // lies below it, and the copy fails on it.
            bool blocked = false;
            for (auto folder = place.parent_path(); folder != root && folder.has_relative_path() && !blocked;
                 folder = folder.parent_path()) {
                const auto kind = std::filesystem::symlink_status(folder, ec).type();
                blocked = kind != std::filesystem::file_type::not_found && kind != std::filesystem::file_type::directory;
            }
            if (blocked) {
                leave("a file is where the folder for /" + entry.destination + " has to be");
                continue;
            }
        }
        std::string refusal = ImportTrackChange::rowRefusal(track, target.libraries, audio.fileSizeBytes);
        if (!refusal.empty() && track.comment.size() > 250) {
            // The one text that is long in real files (a tracklist, a
            // licence): shortened in the row, whole in the file.
            track.comment = domain::storableText(firstCharacters(track.comment, 250));
            refusal = ImportTrackChange::rowRefusal(track, target.libraries, audio.fileSizeBytes);
        }
        if (!refusal.empty()) {
            leave(refusal);
            continue;
        }
        std::shared_ptr<ImportTrackChange> change;
        try {
            change = std::make_shared<ImportTrackChange>(source, target.stickRoot, target.pioneerRoot,
                                                         target.enginePath, track, target.libraries, toImport);
        } catch (const std::invalid_argument &e) {
            leave(e.what());
            continue;
        }
        // Last, so that only a track that is staged takes room in it.
        if (const std::string full = rehearsal.add(track, audio.fileSizeBytes); !full.empty()) {
            leave(full);
            continue;
        }
        imports.push_back(std::move(change));
        staged.staged.push_back(source);
    }

    if (imports.empty()) {
        return staged;
    }
    if (playlist && playlist->create) {
        // The batch's size, so the save's scratch-copy decision is made
        // for the batch; and one unit with the first import, so a cancel
        // does not leave the playlist behind empty.
        auto made = std::make_shared<CreatePlaylistChange>(target.pioneerRoot, target.enginePath, playlist->path,
                                                           target.libraries, toImport);
        made->tieTo(imports.front()->unitKeys().last());
        staged.changes.push_back(made);
    }
    std::vector<std::string> placed;
    for (const auto &change : imports) {
        staged.changes.push_back(change);
        if (playlist) {
            const std::string file = change->destinationFile();
            // At the end, each: the run is put at its place once, below.
            auto entry = std::make_shared<AddToPlaylistChange>(target.pioneerRoot, target.enginePath, playlist->path,
                                                               file, change->subject(), target.libraries);
            // "/Contents/..." is what the import's own unit key is made of.
            entry->tieTo(change->unitKeys().last());
            staged.changes.push_back(entry);
            placed.push_back(file);
        }
    }
    if (playlist && playlist->atIndex) {
        // One move of the whole run, rehearsed like the rows. A playlist
        // that cannot take the move keeps the tracks at its end, said in
        // `note`, rather than costing the import.
        if (const std::string why = rehearsal.place(*playlist->atIndex); why.empty()) {
            staged.changes.push_back(std::make_shared<PlaceInPlaylistChange>(
                target.pioneerRoot, target.enginePath, playlist->path, placed, *playlist->atIndex, target.libraries));
        } else {
            staged.note = "The tracks go to the end of \"" + playlist->path + "\", not to the place asked for: " + why
                          + ".";
        }
    }
    return staged;
}

}  // namespace seabass::gui
