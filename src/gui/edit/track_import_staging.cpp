// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/track_import_staging.hpp"

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
            const auto place = pathFromUtf8(target.stickRoot.toStdString()) / pathFromUtf8(entry.destination);
            if (std::filesystem::symlink_status(place, ec).type() != std::filesystem::file_type::not_found) {
                leave("/" + entry.destination + " is on the stick already");
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
        try {
            imports.push_back(std::make_shared<ImportTrackChange>(source, target.stickRoot,
                                                                  target.pioneerRoot, target.enginePath, track,
                                                                  target.libraries, toImport));
        } catch (const std::invalid_argument &e) {
            leave(e.what());
            continue;
        }
        staged.staged.push_back(source);
    }

    if (imports.empty()) {
        return staged;
    }
    if (playlist && playlist->create) {
        staged.changes.push_back(std::make_shared<CreatePlaylistChange>(target.pioneerRoot, target.enginePath,
                                                                        playlist->path, target.libraries));
    }
    int offset = 0;
    for (const auto &change : imports) {
        staged.changes.push_back(change);
        if (playlist) {
            const std::string file = change->destinationFile();
            auto entry = std::make_shared<AddToPlaylistChange>(
                target.pioneerRoot, target.enginePath, playlist->path, file, change->subject(), target.libraries,
                playlist->atIndex ? std::optional<int>(*playlist->atIndex + offset) : std::nullopt);
            // "/Contents/..." is what the import's own unit key is made of.
            entry->tieTo(change->unitKeys().last());
            staged.changes.push_back(entry);
        }
        ++offset;
    }
    return staged;
}

}  // namespace seabass::gui
