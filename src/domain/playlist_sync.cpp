// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/playlist_sync.hpp"

#include <cctype>
#include <map>
#include <set>

namespace seabass::domain
{

namespace
{

// One library, indexed: every file it lists, and each playlist's members,
// by file key. Two rows for one file (OneLibrary keeps those) are one
// member: the playlist either holds the file or it does not.
struct Indexed
{
    std::string format;
    std::map<std::string, const Track *> byKey;
    std::map<std::string, std::map<std::string, const Track *>> membersByPlaylist;
};

std::vector<Indexed> indexAll(const std::vector<PlaylistCatalog> &catalogs,
                              const std::function<std::string(const std::string &)> &key)
{
    std::vector<Indexed> out;
    for (const auto &catalog : catalogs) {
        Indexed index;
        index.format = catalog.format;
        for (const Track &track : catalog.tracks) {
            if (track.filePath.empty() || !track.streamingSource.empty()) {
                continue;
            }
            const std::string k = key(track.filePath);
            index.byKey.emplace(k, &track);
            for (const auto &membership : track.playlists) {
                index.membersByPlaylist[membership.name].emplace(k, &track);
            }
        }
        out.push_back(std::move(index));
    }
    return out;
}

}  // namespace

struct PlaylistIndex::Data
{
    std::vector<Indexed> indexed;
};

PlaylistIndex::PlaylistIndex(const std::vector<PlaylistCatalog> &catalogs,
                             const std::function<std::string(const std::string &)> &key)
    : m_data(std::make_unique<Data>(Data{indexAll(catalogs, key)}))
{
}

PlaylistIndex::~PlaylistIndex() = default;

bool PlaylistDifference::missingSomewhere() const
{
    for (const auto &side : sides) {
        if (!side.hasPlaylist) {
            return true;
        }
    }
    return false;
}

const PlaylistSide *PlaylistDifference::side(const std::string &format) const
{
    for (const auto &s : sides) {
        if (s.format == format) {
            return &s;
        }
    }
    return nullptr;
}

std::vector<PlaylistDifference> findPlaylistDifferences(const std::vector<PlaylistCatalog> &catalogs,
                                                        const std::function<std::string(const std::string &)> &key)
{
    if (catalogs.size() < 2) {
        return {};
    }
    return findPlaylistDifferences(PlaylistIndex(catalogs, key));
}

std::vector<PlaylistDifference> findPlaylistDifferences(const PlaylistIndex &index,
                                                        const std::function<void()> &between)
{
    std::vector<PlaylistDifference> differences;
    const std::vector<Indexed> &indexed = index.data().indexed;
    if (indexed.size() < 2) {
        return differences;
    }
    std::set<std::string> names;
    for (const auto &index : indexed) {
        for (const auto &[name, members] : index.membersByPlaylist) {
            names.insert(name);
        }
    }
    for (const std::string &name : names) {
        if (between) {
            between();
        }
        PlaylistDifference difference;
        difference.name = name;
        bool differs = false;
        for (const auto &here : indexed) {
            PlaylistSide side;
            side.format = here.format;
            const auto mine = here.membersByPlaylist.find(name);
            side.hasPlaylist = mine != here.membersByPlaylist.end();
            if (!side.hasPlaylist) {
                differs = true;
                difference.sides.push_back(std::move(side));
                continue;
            }
            side.members = static_cast<int>(mine->second.size());
            std::set<std::string> lacking;
            std::set<std::string> extra;
            std::set<std::string> notEverywhere;
            for (const auto &there : indexed) {
                if (&there == &here) {
                    continue;
                }
                const auto theirs = there.membersByPlaylist.find(name);
                if (theirs == there.membersByPlaylist.end()) {
                    continue;
                }
                for (const auto &[k, track] : theirs->second) {
                    if (here.byKey.contains(k) && !mine->second.contains(k)) {
                        lacking.insert(k);
                    }
                }
                for (const auto &[k, track] : mine->second) {
                    if (!there.byKey.contains(k)) {
                        notEverywhere.insert(k);
                    } else if (!theirs->second.contains(k)) {
                        extra.insert(k);
                    }
                }
            }
            for (const auto &k : lacking) {
                side.lacking.push_back(*here.byKey.at(k));
            }
            for (const auto &k : extra) {
                side.extra.push_back(*mine->second.at(k));
            }
            side.notInEveryLibrary = static_cast<int>(notEverywhere.size());
            differs = differs || !side.lacking.empty() || !side.extra.empty();
            difference.sides.push_back(std::move(side));
        }
        if (differs) {
            differences.push_back(std::move(difference));
        }
    }
    return differences;
}

namespace
{
std::string folded(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
    }
    return out;
}
}  // namespace

bool sameSong(const Track &a, const Track &b)
{
    return !a.title.empty() && folded(a.title) == folded(b.title) && folded(a.artist) == folded(b.artist);
}

std::vector<PlaylistAlignment> alignTo(const PlaylistDifference &difference, const std::string &reference,
                                       const std::vector<PlaylistCatalog> &catalogs,
                                       const std::function<std::string(const std::string &)> &key)
{
    return alignTo(difference, reference, PlaylistIndex(catalogs, key));
}

std::vector<PlaylistAlignment> alignTo(const PlaylistDifference &difference, const std::string &reference,
                                       const PlaylistIndex &index)
{
    std::vector<PlaylistAlignment> out;
    const std::vector<Indexed> &indexed = index.data().indexed;
    const Indexed *ref = nullptr;
    for (const auto &index : indexed) {
        if (index.format == reference) {
            ref = &index;
        }
    }
    if (ref == nullptr) {
        return out;
    }
    const auto refMembers = ref->membersByPlaylist.find(difference.name);
    if (refMembers == ref->membersByPlaylist.end()) {
        return out;
    }
    for (const auto &target : indexed) {
        if (&target == ref) {
            continue;
        }
        const auto mine = target.membersByPlaylist.find(difference.name);
        if (mine == target.membersByPlaylist.end()) {
            continue;
        }
        PlaylistAlignment alignment;
        alignment.format = target.format;
        // The target's members the reference does not list at all: left
        // alone, unless one is the old copy of a song being added.
        std::vector<const Track *> unlisted;
        for (const auto &[k, track] : mine->second) {
            if (!ref->byKey.contains(k)) {
                unlisted.push_back(track);
            }
        }
        for (const auto &[k, track] : refMembers->second) {
            const auto row = target.byKey.find(k);
            if (row != target.byKey.end() && !mine->second.contains(k)) {
                alignment.add.push_back(*row->second);
                for (auto it = unlisted.begin(); it != unlisted.end(); ++it) {
                    if (sameSong(**it, *row->second)) {
                        alignment.remove.push_back(**it);
                        unlisted.erase(it);
                        ++alignment.replacements;
                        break;
                    }
                }
            }
        }
        for (const auto &[k, track] : mine->second) {
            if (ref->byKey.contains(k) && !refMembers->second.contains(k)) {
                alignment.remove.push_back(*track);
            }
        }
        if (!alignment.add.empty() || !alignment.remove.empty()) {
            out.push_back(std::move(alignment));
        }
    }
    return out;
}

}  // namespace seabass::domain
