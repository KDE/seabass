// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/work_counters.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace seabass::infrastructure::rekordbox
{

using Pdb = rekordbox_pdb_t;

// export.pdb stores strings in fixed-length fields and right-pads them
// with spaces (see rekordbox_library_anonymizer.cpp, which has always
// trimmed for exactly this reason). Trimming here rather than at each
// call site because every string in the catalog comes through this one
// function, and the padding is not data: it is the format's filler, and
// a field cannot distinguish a trailing space it was given from one it
// was padded with.
//
// Left untrimmed, this was destructive rather than cosmetic. Every
// track's filePath arrived padded, so normalizedPathKey() said a
// catalog's own file and the same file on disk were different files --
// which is what findUnreferencedFiles and resolvePendingDeletions use to
// decide that a file can be deleted.
std::string sqlText(Pdb::device_sql_string_t *s)
{
    auto trimmed = [](std::string value) {
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\0')) {
            value.pop_back();
        }
        return value;
    };
    if (!s) {
        return "";
    }
    auto *body = s->body();
    if (auto *a = dynamic_cast<Pdb::device_sql_short_ascii_t *>(body)) {
        return trimmed(a->text());
    }
    if (auto *a = dynamic_cast<Pdb::device_sql_long_ascii_t *>(body)) {
        return trimmed(a->text());
    }
    if (auto *a = dynamic_cast<Pdb::device_sql_long_utf16le_t *>(body)) {
        return trimmed(a->text());
    }
    return "";
}

std::string extAnlzPath(const std::string &pioneerRoot, const std::string &analyzePath)
{
    std::string rel = analyzePath;
    size_t pos = rel.find("/PIONEER/");
    if (pos != std::string::npos) {
        rel = rel.substr(pos + std::string("/PIONEER/").size());
    }
    size_t dot = rel.rfind(".DAT");
    if (dot != std::string::npos) {
        rel = rel.substr(0, dot) + ".EXT";
    }
    return pioneerRoot + "/" + rel;
}

std::string datAnlzPath(const std::string &pioneerRoot, const std::string &analyzePath)
{
    std::string rel = analyzePath;
    size_t pos = rel.find("/PIONEER/");
    if (pos != std::string::npos) {
        rel = rel.substr(pos + std::string("/PIONEER/").size());
    }
    return pioneerRoot + "/" + rel;
}

std::optional<std::string> findAnlzPathForTrackId(const std::string &pioneerRoot, uint32_t trackId)
{
    std::string pdbPath = pioneerRoot + "/rekordbox/export.pdb";
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }

    WorkCounters::instance().noteTrackDatabaseParse();
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);

    std::optional<std::string> result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS || result) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            if (result) {
                return;
            }
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowTrack = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (!rowTrack || rowTrack->id() != trackId) {
                        continue;
                    }
                    std::string analyzePath = sqlText(rowTrack->analyze_path());
                    if (!analyzePath.empty()) {
                        result = analyzePath;
                    }
                    return;
                }
            }
        });
    }
    return result;
}

std::string trackFilePathOnStick(const std::string &stickRoot, std::string storedPath)
{
    // The stored path carries its own leading separator and stickRoot
    // usually ends in one: joined by operator/ after stripping ours, so
    // the result has exactly one, and made native so it compares equal
    // to OneLibraryReader's spelling of the same file.
    if (!storedPath.empty() && (storedPath.front() == '/' || storedPath.front() == '\\')) {
        storedPath.erase(0, 1);
    }
    return (std::filesystem::path(stickRoot) / storedPath).make_preferred().string();
}

std::vector<std::string> deletedTrackFilePaths(const std::string &pioneerRoot)
{
    const std::string pdbPath = pioneerRoot + "/rekordbox/export.pdb";
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    const std::string stickRoot = std::filesystem::path(pioneerRoot).parent_path().string();

    std::set<std::string> live;
    std::set<std::string> deleted;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            const unsigned allocated = page->num_row_offsets();
            unsigned seen = 0;
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (seen++ >= allocated) {
                        return;
                    }
                    if (row->present()) {
                        auto *track = static_cast<Pdb::track_row_t *>(row->body());
                        live.insert(sqlText(track->file_path()));
                        continue;
                    }
                    // body() parses only present rows, so the dead one is
                    // read by hand from where its row offset points.
                    try {
                        auto *io = row->_io();
                        io->seek(row->row_base());
                        Pdb::track_row_t track(io, row.get(), &pdb);
                        const std::string path = sqlText(track.file_path());
                        if (!path.empty() && path.front() == '/') {
                            deleted.insert(path);
                        }
                    } catch (const std::exception &) {
                        // Reused space: not a row any more.
                    }
                }
            }
        });
    }
    std::vector<std::string> result;
    for (const std::string &path : deleted) {
        if (!live.count(path)) {
            result.push_back(trackFilePathOnStick(stickRoot, path));
        }
    }
    return result;
}

}  // namespace seabass::infrastructure::rekordbox
