// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/rekordbox_xml.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace seabass::domain
{

namespace
{

// One membership, flattened: which track, which playlist path, where in it.
struct Entry
{
    std::size_t trackIndex = 0;
    int position = -1;
    std::size_t order = 0;  // tie-breaker: the order the catalogs handed them to us
};

std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

XmlPlaylistNode *childNamed(XmlPlaylistNode &parent, const std::string &name)
{
    for (auto &child : parent.children) {
        if (child.name == name) {
            return &child;
        }
    }
    return nullptr;
}

}  // namespace

XmlPlaylistNode buildPlaylistTree(const std::vector<Track> &tracks)
{
    // Collected per full path first, so every membership of one playlist
    // is sorted together regardless of which track (or which catalog)
    // mentioned it. std::map keeps the paths in a stable, predictable
    // order -- two runs over the same library produce byte-identical XML,
    // which is what makes the output diffable and the tests assertable.
    std::map<std::string, std::vector<Entry>> byPath;
    std::size_t order = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        for (const auto &membership : tracks[i].playlists) {
            if (membership.name.empty()) {
                continue;
            }
            byPath[membership.name].push_back(Entry{i, membership.position, order++});
        }
    }

    XmlPlaylistNode root;
    root.name = "ROOT";

    for (auto &[path, entries] : byPath) {
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            // -1 means "the reader could not tell", which is a weaker
            // claim than any real position, so those go last rather than
            // first -- sorting them as -1 would put them before track 0.
            const bool aKnown = a.position >= 0;
            const bool bKnown = b.position >= 0;
            if (aKnown != bKnown) {
                return aKnown;
            }
            if (aKnown && a.position != b.position) {
                return a.position < b.position;
            }
            return a.order < b.order;
        });

        auto segments = splitPath(path);
        if (segments.empty()) {
            continue;
        }

        XmlPlaylistNode *node = &root;
        for (const auto &segment : segments) {
            XmlPlaylistNode *next = childNamed(*node, segment);
            if (next == nullptr) {
                node->children.push_back(XmlPlaylistNode{segment, {}, {}});
                next = &node->children.back();
            }
            node = next;
        }

        for (const auto &entry : entries) {
            node->trackIndices.push_back(entry.trackIndex);
        }
    }

    return root;
}

}  // namespace seabass::domain
