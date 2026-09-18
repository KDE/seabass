// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/export_rekordbox_xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/junk_cue.hpp"
#include "domain/local_restore.hpp"
#include "domain/rekordbox_xml.hpp"
#include "infrastructure/rekordbox/rekordbox_xml_writer.hpp"

namespace seabass::application
{

namespace
{

// Cross-format cue positions are never bit-identical: the two formats
// store different units and every conversion rounds. The project already
// has one answer for "how far apart is still the same cue", and this uses
// it rather than inventing a second -- a smaller number would be wrong
// here in particular, since rekordbox's own legacy and PCO2 cue lists
// hold the same pad up to 514 ms apart within a single file (see the
// legacy PCOB codec added for issue #33).
constexpr double CuePositionToleranceMs = domain::LocalRestorePlanner::PositionToleranceMs;

std::string lowerExtension(const std::string &path)
{
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string applyPathMap(const std::string &path, const std::map<std::string, std::string> &prefixMap)
{
    // Longest prefix wins, so a map holding both "/Volumes/X" and
    // "/Volumes/X/Contents" does the specific thing for the second.
    const std::string *bestFrom = nullptr;
    const std::string *bestTo = nullptr;
    for (const auto &[from, to] : prefixMap) {
        if (path.rfind(from, 0) != 0) {
            continue;
        }
        if (bestFrom == nullptr || from.size() > bestFrom->size()) {
            bestFrom = &from;
            bestTo = &to;
        }
    }
    if (bestFrom == nullptr) {
        return path;
    }
    return *bestTo + path.substr(bestFrom->size());
}

std::string slotName(const domain::CuePoint &cue)
{
    if (cue.kind == domain::CuePoint::Kind::Hot) {
        return "hot " + std::to_string(cue.hotCueNumber);
    }
    return "memory";
}

// Two rows for the same file, from different catalogs, each with a cue in
// the same slot but at positions further apart than the rounding
// tolerance. Reported, never resolved: picking a winner here would be
// synchronization's job, and it would do it by writing to the stick.
void collectConflicts(const std::vector<domain::Track> &rows, std::vector<CueConflict> &conflicts)
{
    std::map<std::string, std::vector<const domain::Track *>> byFile;
    for (const auto &row : rows) {
        if (row.filePath.empty()) {
            continue;
        }
        byFile[normalizedPathKey(row.filePath)].push_back(&row);
    }

    for (const auto &[key, fileRows] : byFile) {
        if (fileRows.size() < 2) {
            continue;
        }
        for (std::size_t i = 0; i < fileRows.size(); ++i) {
            for (std::size_t j = i + 1; j < fileRows.size(); ++j) {
                const domain::Track &a = *fileRows[i];
                const domain::Track &b = *fileRows[j];
                if (a.format == b.format) {
                    continue;
                }
                for (const auto &cueA : a.cues) {
                    if (cueA.kind != domain::CuePoint::Kind::Hot) {
                        continue;  // only a numbered slot can disagree with itself
                    }
                    for (const auto &cueB : b.cues) {
                        if (cueB.kind != domain::CuePoint::Kind::Hot || cueB.hotCueNumber != cueA.hotCueNumber) {
                            continue;
                        }
                        if (std::abs(cueA.positionMs - cueB.positionMs) <= CuePositionToleranceMs) {
                            continue;
                        }
                        conflicts.push_back(CueConflict{a.filePath, a.title.empty() ? b.title : a.title,
                                                         a.artist.empty() ? b.artist : a.artist, slotName(cueA),
                                                         a.format, cueA.positionMs, b.format, cueB.positionMs});
                    }
                }
            }
        }
    }
}

bool sameCue(const domain::CuePoint &a, const domain::CuePoint &b)
{
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == domain::CuePoint::Kind::Hot && a.hotCueNumber != b.hotCueNumber) {
        return false;
    }
    return std::abs(a.positionMs - b.positionMs) <= CuePositionToleranceMs;
}

// Every cue rekordbox's own catalog already had, per file. What the merge
// adds on top of this is exactly the answer to "did the cues I set on the
// Denon gear make it into rekordbox".
std::map<std::string, std::vector<domain::CuePoint>> rekordboxCuesByFile(const std::vector<domain::Track> &rows)
{
    std::map<std::string, std::vector<domain::CuePoint>> cues;
    for (const auto &row : rows) {
        if (row.format != "rekordbox" || row.filePath.empty()) {
            continue;
        }
        auto &existing = cues[normalizedPathKey(row.filePath)];
        existing.insert(existing.end(), row.cues.begin(), row.cues.end());
    }
    return cues;
}

}  // namespace

ExportRekordboxXmlResult ExportRekordboxXml::execute(const std::vector<domain::Track> &rows,
                                                      const ExportRekordboxXmlOptions &options)
{
    ExportRekordboxXmlResult result;

    collectConflicts(rows, result.conflicts);
    const auto alreadyInRekordbox = rekordboxCuesByFile(rows);

    // The step that makes this worth having: one Track per file, carrying
    // the union of every catalog's cues for it.
    std::vector<domain::Track> files = collapseCatalogRows(rows);

    std::vector<domain::Track> exportable;
    exportable.reserve(files.size());

    for (auto &file : files) {
        if (!file.streamingSource.empty()) {
            // A streaming row's path names a cache on whichever computer
            // played it. It is not a file here and must never be written
            // as one.
            ++result.tracksStreaming;
            continue;
        }
        if (file.filePath.empty()) {
            ++result.tracksWithoutPath;
            if (options.reportUnresolved) {
                result.unresolvedPaths.push_back(file.filename);
            }
            continue;
        }
        const std::string ext = lowerExtension(file.filePath);
        if (std::find(options.excludeExtensions.begin(), options.excludeExtensions.end(), ext) !=
            options.excludeExtensions.end()) {
            ++result.tracksExcludedByExtension;
            continue;
        }

        if (options.dropJunkMemoryCues) {
            const std::size_t before = file.cues.size();
            file.cues.erase(std::remove_if(file.cues.begin(), file.cues.end(),
                                            [](const domain::CuePoint &cue) {
                                                return domain::isJunkCue(cue);
                                            }),
                             file.cues.end());
            result.junkMemoryCuesDropped += before - file.cues.size();
        }

        // Counted against what rekordbox's own catalog held for this file,
        // before the path is rewritten out from under the lookup.
        {
            const auto it = alreadyInRekordbox.find(normalizedPathKey(file.filePath));
            const std::vector<domain::CuePoint> *had = it == alreadyInRekordbox.end() ? nullptr : &it->second;
            for (const auto &cue : file.cues) {
                const bool knownToRekordbox =
                    had != nullptr && std::any_of(had->begin(), had->end(), [&cue](const domain::CuePoint &existing) {
                        return sameCue(cue, existing);
                    });
                if (!knownToRekordbox) {
                    ++result.cuesFromOtherCatalogs;
                }
            }
        }

        file.filePath = applyPathMap(file.filePath, options.pathPrefixMap);
        result.cuesWritten += file.cues.size();
        exportable.push_back(std::move(file));
    }

    const domain::XmlPlaylistNode tree = domain::buildPlaylistTree(exportable);

    // Counted off the tree rather than off the memberships, so it says
    // how many playlists rekordbox will show.
    std::vector<const domain::XmlPlaylistNode *> pending{&tree};
    while (!pending.empty()) {
        const domain::XmlPlaylistNode *node = pending.back();
        pending.pop_back();
        if (node->children.empty()) {
            if (node != &tree) {
                ++result.playlistsWritten;
            }
            continue;
        }
        if (!node->trackIndices.empty()) {
            ++result.playlistsWritten;  // the same-named leaf a folder-with-tracks becomes
        }
        for (const auto &child : node->children) {
            pending.push_back(&child);
        }
    }

    result.tracksWritten = exportable.size();
    result.xml = infrastructure::rekordbox::writeRekordboxXml(exportable, tree);
    return result;
}

}  // namespace seabass::application
