// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

using Pdb = rekordbox_pdb_t;
using Anlz = rekordbox_anlz_t;
namespace domain = seabass::domain;

std::string rekordboxCueColor(bool hasRgb, unsigned char r, unsigned char g, unsigned char b, int colorId)
{
    if (hasRgb) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
        return buf;
    }
    // color_id == 0 is documented (community ANLZ reverse-engineering,
    // e.g. crate-digger) as "no color", the same nxs1-era legacy slot
    // Rekordbox itself falls back to displaying an uncolored pad from.
    // Previously returned as the literal string "0", which doesn't match
    // any other format's own "no color" representation (see
    // libdjinterop_engine_reader.cpp's colorHex()) -- cueSetsEqual()'s
    // exact color comparison was treating every uncolored cue pair as a
    // real conflict because of this string mismatch alone, confirmed as
    // the actual cause of a full batch of "genuine" cross-source sync
    // conflicts that weren't genuine at all.
    if (colorId == 0) {
        return "";
    }
    return std::to_string(colorId);
}

namespace
{

// Non-const reference because the generated _is_null_*()/accessor
// methods are not declared const (they lazily compute and cache the
// field on first access). Thin wrapper around rekordboxCueColor() (see
// that function's own doc comment in the header for the actual logic
// and why it matters) -- extracted there specifically so it has direct
// unit test coverage without needing a full parsed ANLZ file.
std::string cueColor(Anlz::cue_extended_entry_t &cue)
{
    bool hasRgb = !cue._is_null_color_red() && !cue._is_null_color_green() && !cue._is_null_color_blue();
    return rekordboxCueColor(hasRgb, hasRgb ? cue.color_red() : 0, hasRgb ? cue.color_green() : 0,
                              hasRgb ? cue.color_blue() : 0, static_cast<int>(cue.color_id()));
}

// Takes the file's bytes rather than its path: where they came from is
// the AnlzByteSource's business (a real PIONEER folder, or an entry in a
// stick backup being browsed). Kaitai parses from any std::istream, so
// this is the same parse either way.
std::vector<domain::CuePoint> readCues(const std::string &anlzBytes)
{
    std::vector<domain::CuePoint> cues;
    if (anlzBytes.empty()) {
        return cues;
    }
    std::istringstream ifs(anlzBytes, std::ios::binary);

    kaitai::kstream ks(&ifs);
    Anlz anlz(&ks);
    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() != Anlz::SECTION_TAGS_CUES_2) {
            continue;
        }
        auto *tag = dynamic_cast<Anlz::cue_extended_tag_t *>(section->body());
        if (!tag) {
            continue;
        }

        bool isHot = tag->type() == Anlz::CUE_LIST_TYPE_HOT_CUES;
        for (const auto &cue : *tag->cues()) {
            domain::CuePoint cp;
            cp.kind = (isHot && cue->hot_cue() != 0) ? domain::CuePoint::Kind::Hot
                                                      : domain::CuePoint::Kind::Memory;
            cp.hotCueNumber = static_cast<int>(cue->hot_cue());
            cp.positionMs = static_cast<double>(cue->time());
            cp.isLoop = cue->type() == Anlz::CUE_ENTRY_TYPE_LOOP;
            if (cp.isLoop) {
                cp.loopEndMs = static_cast<double>(cue->loop_time());
            }
            cp.color = cueColor(*cue);
            if (!cue->_is_null_comment()) {
                cp.comment = cue->comment();
            }
            cues.push_back(std::move(cp));
        }
    }
    return cues;
}

struct PlaylistTreeInfo
{
    std::string name;
    uint32_t parentId = 0;
    bool isFolder = false;
};

// Builds the full path (e.g. "Techno/Peak Time") for a playlist id by
// walking parent_id links up to the root. Guards against a malformed/
// cyclic parent chain rather than looping forever.
std::string playlistPath(uint32_t id, const std::unordered_map<uint32_t, PlaylistTreeInfo> &tree)
{
    std::vector<std::string> parts;
    uint32_t current = id;
    for (int guard = 0; current != 0 && guard < 64; ++guard) {
        auto it = tree.find(current);
        if (it == tree.end()) {
            break;
        }
        parts.push_back(it->second.name);
        current = it->second.parentId;
    }
    std::reverse(parts.begin(), parts.end());

    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            path += "/";
        }
        path += parts[i];
    }
    return path;
}

}  // namespace

// Resolves the analysis-file source from the root itself rather than
// taking one: a browsed stick backup's extracted catalogs carry a marker
// naming their archive, so every existing construction of this reader --
// the CLI, the catalog cache, the corpus runner -- reads a backup
// correctly without knowing backups exist. See anlzSourceForPioneerRoot().
KaitaiRekordboxReader::KaitaiRekordboxReader(std::string pioneerRoot)
    : m_pioneerRoot(pioneerRoot), m_anlzSource(anlzSourceForPioneerRoot(pioneerRoot))
{
}

KaitaiRekordboxReader::KaitaiRekordboxReader(std::string pioneerRoot, std::shared_ptr<AnlzByteSource> anlzSource)
    : m_pioneerRoot(std::move(pioneerRoot)), m_anlzSource(std::move(anlzSource))
{
    if (!m_anlzSource) {
        m_anlzSource = std::make_shared<FilesystemAnlzSource>(m_pioneerRoot);
    }
}

std::vector<domain::Track> KaitaiRekordboxReader::readAll()
{
    std::vector<domain::Track> tracks;

    std::string pdbPath = m_pioneerRoot + "/rekordbox/export.pdb";
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }

    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);

    // file_path (e.g. "/Contents/Artist/Album/01_track.mp3") is relative to
    // the stick root, i.e. the PIONEER folder's parent.
    std::string stickRoot = std::filesystem::path(m_pioneerRoot).parent_path().string();

    // Artist names live in their own normalized table; track rows only
    // carry an artist_id foreign key into it.
    std::unordered_map<uint32_t, std::string> artistNameById;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_ARTISTS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowArtist = dynamic_cast<Pdb::artist_row_t *>(row->body());
                    if (!rowArtist) {
                        continue;
                    }
                    artistNameById[rowArtist->id()] = sqlText(rowArtist->name());
                }
            }
        });
    }

    // Album titles, same shape as artists above: their own normalized
    // table, with track rows carrying an album_id into it. A track with
    // no album has album_id 0, which matches no row and correctly leaves
    // the field empty.
    std::unordered_map<uint32_t, std::string> albumNameById;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_ALBUMS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowAlbum = dynamic_cast<Pdb::album_row_t *>(row->body());
                    if (!rowAlbum) {
                        continue;
                    }
                    albumNameById[rowAlbum->id()] = sqlText(rowAlbum->name());
                }
            }
        });
    }

    // Musical key names live in their own table; track rows only carry a
    // key_id foreign key into it.
    std::unordered_map<uint32_t, std::string> keyNameById;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_KEYS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowKey = dynamic_cast<Pdb::key_row_t *>(row->body());
                    if (!rowKey) {
                        continue;
                    }
                    keyNameById[rowKey->id()] = sqlText(rowKey->name());
                }
            }
        });
    }

    // Cover art images live in their own table; track rows only carry an
    // artwork_id foreign key into it.
    std::unordered_map<uint32_t, std::string> artworkPathById;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_ARTWORK) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowArtwork = dynamic_cast<Pdb::artwork_row_t *>(row->body());
                    if (!rowArtwork) {
                        continue;
                    }
                    artworkPathById[rowArtwork->id()] = sqlText(rowArtwork->path());
                }
            }
        });
    }

    // Playlists: build id -> {name, parent, isFolder} from PLAYLIST_TREE,
    // then track id -> playlist path(s) from PLAYLIST_ENTRIES. Both tables
    // are small, so this is done fully upfront rather than per-track.
    std::unordered_map<uint32_t, PlaylistTreeInfo> playlistTreeById;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_PLAYLIST_TREE) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowPlaylist = dynamic_cast<Pdb::playlist_tree_row_t *>(row->body());
                    if (!rowPlaylist) {
                        continue;
                    }
                    playlistTreeById[rowPlaylist->id()] = PlaylistTreeInfo{
                        sqlText(rowPlaylist->name()),
                        rowPlaylist->parent_id(),
                        rowPlaylist->raw_is_folder() != 0,
                    };
                }
            }
        });
    }

    std::unordered_map<uint32_t, std::vector<domain::PlaylistMembership>> playlistsByTrackId;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_PLAYLIST_ENTRIES) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowEntry = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body());
                    if (!rowEntry) {
                        continue;
                    }
                    auto it = playlistTreeById.find(rowEntry->playlist_id());
                    if (it == playlistTreeById.end() || it->second.isFolder) {
                        continue;
                    }
                    playlistsByTrackId[rowEntry->track_id()].push_back(domain::PlaylistMembership{
                        playlistPath(rowEntry->playlist_id(), playlistTreeById),
                        static_cast<int>(rowEntry->entry_index()),
                    });
                }
            }
        });
    }

    // Pre-pass: count rows across the tracks table's pages (cheap -- just
    // reads page headers, no per-row parsing) so progress can show a real
    // percentage rather than an indeterminate count.
    size_t totalRows = 0;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&totalRows](Pdb::page_t *page) { totalRows += page->num_rows(); });
    }

    m_progress->start("Scanning rekordbox tracks", totalRows);
    size_t processed = 0;

    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }

        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowTrack = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (!rowTrack) {
                        continue;
                    }

                    domain::Track track;
                    track.sourceId = std::to_string(rowTrack->id());
                    track.format = "rekordbox";
                    track.title = sqlText(rowTrack->title());
                    track.filename = sqlText(rowTrack->filename());
                    auto albumIt = albumNameById.find(rowTrack->album_id());
                    if (albumIt != albumNameById.end()) {
                        track.album = albumIt->second;
                    }
                    auto artistIt = artistNameById.find(rowTrack->artist_id());
                    if (artistIt != artistNameById.end()) {
                        track.artist = artistIt->second;
                    }
                    std::string trackFilePath = sqlText(rowTrack->file_path());
                    if (!trackFilePath.empty()) {
                        track.filePath = stickRoot + trackFilePath;
                        std::error_code ec;
                        auto size = std::filesystem::file_size(track.filePath, ec);
                        track.fileSizeBytes = ec ? 0 : size;
                    }
                    auto artworkIt = artworkPathById.find(rowTrack->artwork_id());
                    if (artworkIt != artworkPathById.end() && !artworkIt->second.empty()) {
                        track.artworkPath = stickRoot + artworkIt->second;
                    }
                    track.durationSeconds = rowTrack->duration();
                    track.bpm = rowTrack->tempo() / 100.0;
                    track.bitrate = static_cast<int>(rowTrack->bitrate());
                    auto keyIt = keyNameById.find(rowTrack->key_id());
                    if (keyIt != keyNameById.end()) {
                        track.key = keyIt->second;
                    }
                    track.playCount = rowTrack->play_count();
                    if (rowTrack->rating() > 0) {
                        track.rating = rowTrack->rating();
                    }
                    track.comment = sqlText(rowTrack->comment());
                    auto playlistsIt = playlistsByTrackId.find(rowTrack->id());
                    if (playlistsIt != playlistsByTrackId.end()) {
                        track.playlists = playlistsIt->second;
                    }

                    std::string analyzePath = sqlText(rowTrack->analyze_path());
                    if (!analyzePath.empty()) {
                        auto bytes = m_anlzSource->read(anlzRelativePath(analyzePath, /*wantExt=*/true));
                        if (bytes) {
                            track.cues = readCues(*bytes);
                        }
                    }
                    tracks.push_back(std::move(track));

                    m_progress->tick(++processed);
                    m_cancel.throwIfCancelled();
                }
            }
        });
    }

    m_progress->finish();
    return tracks;
}

}  // namespace seabass::infrastructure::rekordbox
