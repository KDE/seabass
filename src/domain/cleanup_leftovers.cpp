// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/cleanup_leftovers.hpp"

#include "domain/duplicate_cue_consolidation.hpp"

#include <map>
#include <set>

namespace seabass::domain
{

std::vector<CleanupLeftover> CleanupLeftoverFinder::find(const std::vector<Track> &oneLibraryRows,
                                                         const std::vector<Track> &rekordboxTracks,
                                                         const std::vector<std::string> &deletedRekordboxFiles,
                                                         const FileKey &fileKey, AudioContentProbe *probe)
{
    std::set<std::string> deleted;
    for (const std::string &file : deletedRekordboxFiles) {
        deleted.insert(fileKey(file));
    }
    std::set<std::string> live;
    for (const Track &track : rekordboxTracks) {
        if (!track.filePath.empty()) {
            live.insert(fileKey(track.filePath));
        }
    }
    std::set<std::string> inOneLibrary;
    // Every row at a leftover file, in reader order: the first stands for
    // the file, the rest only contribute their ids.
    std::map<std::string, CleanupLeftover> leftovers;
    std::vector<std::string> order;
    for (const Track &row : oneLibraryRows) {
        if (row.filePath.empty() || !row.streamingSource.empty()) {
            continue;
        }
        const std::string key = fileKey(row.filePath);
        inOneLibrary.insert(key);
        if (!deleted.count(key) || live.count(key)) {
            continue;
        }
        auto [it, inserted] = leftovers.try_emplace(key);
        if (inserted) {
            it->second.row = row;
            order.push_back(key);
        }
        it->second.rowIds.push_back(row.sourceId);
    }
    if (leftovers.empty()) {
        return {};
    }

    // One duplicate search over the leftovers and the live rekordbox
    // tracks together, so a leftover is matched by exactly the rules that
    // made Clean Up remove it. A group can hold several leftovers (three
    // copies, two removed); each is judged on the live tracks beside it.
    std::vector<Track> pool;
    pool.reserve(leftovers.size() + rekordboxTracks.size());
    for (const std::string &key : order) {
        pool.push_back(leftovers.at(key).row);
    }
    for (const Track &track : rekordboxTracks) {
        if (!track.filePath.empty()) {
            pool.push_back(track);
        }
    }
    for (const DuplicateGroup &group : DuplicateTrackFinder::find(pool, probe)) {
        std::vector<const Track *> survivors;
        std::vector<std::string> members;
        for (const Track &track : group.tracks) {
            const std::string key = fileKey(track.filePath);
            if (track.format == "onelibrary" && leftovers.count(key)) {
                members.push_back(key);
            } else if (track.format != "onelibrary" && live.count(key)) {
                survivors.push_back(&track);
            }
        }
        for (const std::string &key : members) {
            CleanupLeftover &leftover = leftovers.at(key);
            if (survivors.size() > 1) {
                leftover.kind = CleanupLeftover::Kind::SeveralSurvivors;
            } else if (survivors.size() == 1) {
                leftover.survivor = *survivors.front();
                leftover.kind = inOneLibrary.count(fileKey(survivors.front()->filePath))
                    ? CleanupLeftover::Kind::Repairable
                    : CleanupLeftover::Kind::SurvivorNotInOneLibrary;
            }
        }
    }

    std::vector<CleanupLeftover> result;
    result.reserve(order.size());
    for (const std::string &key : order) {
        CleanupLeftover &leftover = leftovers.at(key);
        if (leftover.kind != CleanupLeftover::Kind::Repairable
            && leftover.kind != CleanupLeftover::Kind::SurvivorNotInOneLibrary) {
            leftover.survivor.reset();
        }
        result.push_back(std::move(leftover));
    }
    return result;
}

}  // namespace seabass::domain
