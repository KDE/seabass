// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "domain/local_restore.hpp"
#include "infrastructure/file_clock.hpp"
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
// The legacy PCOB lists, for anything they hold that PCO2 does not.
//
// On a modern export PCO2 is a superset and this adds nothing. It is not
// always a modern export: a track last written by an older rekordbox can
// carry hot cues in PCOB alone, and a player of that generation shows
// them. Reading only PCO2 under-reported such a track -- one real stick
// showed one cue here and three on an XDJ-RX2 -- and, now that
// RekordboxCueWriter rewrites the legacy lists too, under-reporting
// would be worse than cosmetic: a save would write back a set that never
// contained them and take them off the player. Read everything that is
// there, and writing it back is safe.
//
// Called for the .EXT and again for the .DAT, because the two hold
// different halves of the legacy set: slots 4-8 live in the .EXT and 1-3
// in the .DAT (see anlz_legacy_cue_codec.hpp).
//
// "The same cue" is decided exactly as LocalRestorePlanner decides it,
// and the reason is visible in the fixture: rekordbox stores one cue in
// both lists at slightly different positions. One real track has all
// eight of its pads recorded 52 ms earlier in the legacy list than in
// PCO2; others differ by 1 ms or by half a second. Comparing positions
// exactly would read that track as sixteen hot cues when a player shows
// eight. So a hot cue is keyed by its slot, because the hardware has one
// cue per pad and two entries for pad 3 are one cue however far apart
// they sit; a memory cue has no slot, so it is the same cue when it is
// within PositionToleranceMs. PCO2 wins either way -- it is the list
// rekordbox refines, and the one modern players read.
void appendLegacyCues(const std::string &anlzBytes, std::vector<domain::CuePoint> &cues)
{
    if (anlzBytes.empty()) {
        return;
    }
    std::istringstream ifs(anlzBytes, std::ios::binary);
    kaitai::kstream ks(&ifs);
    Anlz anlz(&ks);

    auto alreadyKnown = [&cues](const domain::CuePoint &candidate) {
        for (const auto &have : cues) {
            if (have.kind != candidate.kind) {
                continue;
            }
            if (candidate.kind == domain::CuePoint::Kind::Hot) {
                if (have.hotCueNumber == candidate.hotCueNumber) {
                    return true;
                }
                continue;
            }
            if (std::abs(have.positionMs - candidate.positionMs) <= domain::LocalRestorePlanner::PositionToleranceMs) {
                return true;
            }
        }
        return false;
    };

    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() != Anlz::SECTION_TAGS_CUES) {
            continue;
        }
        auto *tag = dynamic_cast<Anlz::cue_tag_t *>(section->body());
        if (!tag) {
            continue;
        }
        const bool isHot = tag->type() == Anlz::CUE_LIST_TYPE_HOT_CUES;
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
            // A legacy entry carries no colour or comment of its own.
            if (!alreadyKnown(cp)) {
                cues.push_back(std::move(cp));
            }
        }
    }
}

// Takes the file's bytes rather than its path: where they came from is
// the AnlzByteSource's business (a real PIONEER folder, or an entry in a
// stick backup being browsed). Kaitai parses from any std::istream, so
// this is the same parse either way.
//
// `datBytes` is the same track's .DAT file when the caller has it. It is
// optional only because one caller (the waveform reader) has no use for
// cues at all; a caller reading cues to write them back later must pass
// it, or the legacy slots 1-3 are invisible and the next save deletes
// them.
std::vector<domain::CuePoint> readCues(const std::string &anlzBytes, const std::string &datBytes = {})
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
    appendLegacyCues(anlzBytes, cues);
    appendLegacyCues(datBytes, cues);
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
                        // make_preferred(): trackFilePath is rekordbox's own
                        // forward-slash convention regardless of platform,
                        // while stickRoot (built from a real fs::path, see
                        // above) is already native. The plain concatenation
                        // this used to be is a fine path for actual file
                        // I/O -- Windows accepts either slash -- but not for
                        // string equality, which is exactly how this same
                        // path gets matched against OneLibraryReader's own
                        // (already-normalized, see its own make_preferred()
                        // call) filePath elsewhere: onelibrary_wal_checkpoint_
                        // test's cross-catalog match silently found nothing
                        // on Windows and every downstream assertion failed
                        // on a fixture precondition instead, confirmed
                        // directly. The same mismatch this project already
                        // fixed once for OneLibraryReader::readAll(), missed
                        // here.
                        //
                        // make_preferred() alone was not enough: it rewrites
                        // which character a separator uses, not how many
                        // appear together. trackFilePath already carries its
                        // own leading "/" (this is a stick-root-relative
                        // path, see the comment above stickRoot's own
                        // definition) and stickRoot -- the PIONEER folder's
                        // parent -- ends in one too whenever PIONEER sits
                        // directly under the stick root, which every real
                        // stick does. The two leading separators surviving
                        // make_preferred() as "E:\\\Contents\..." (three
                        // backslashes after the drive, not one) is why a
                        // live rig test comparing this against
                        // OneLibraryReader's own (correctly single-slash-
                        // stripped) filePath on Windows found the same file
                        // listed under two different strings and matched
                        // nothing -- confirmed directly, counting the
                        // backslashes either side. Stripped the same way
                        // OneLibraryReader::readAll() already strips relPath.
                        // fs::path's own operator/ rather than string
                        // concatenation, too: it inserts a separator only
                        // when stickRoot doesn't already end with one, so
                        // this stays correct on whichever of the two shapes
                        // stickRoot happens to be (this project's own
                        // pattern in OneLibraryReader::readAll(), which this
                        // now matches exactly). The join itself lives in
                        // trackFilePathOnStick(), which deleted rows share.
                        track.filePath = trackFilePathOnStick(stickRoot, trackFilePath);
                        std::error_code ec;
                        auto size = std::filesystem::file_size(track.filePath, ec);
                        track.fileSizeBytes = ec ? 0 : size;
                    }
                    auto artworkIt = artworkPathById.find(rowTrack->artwork_id());
                    if (artworkIt != artworkPathById.end() && !artworkIt->second.empty()) {
                        std::string artworkRelativePath = artworkIt->second;
                        if (artworkRelativePath.front() == '/' || artworkRelativePath.front() == '\\') {
                            artworkRelativePath.erase(0, 1);
                        }
                        track.artworkPath =
                            (std::filesystem::path(stickRoot) / artworkRelativePath).make_preferred().string();
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
                        const std::string extRelative = anlzRelativePath(analyzePath, /*wantExt=*/true);
                        auto bytes = m_anlzSource->read(extRelative);
                        if (bytes) {
                            // The .DAT as well: hot cues 1-3 live only in
                            // its legacy list, and a save writes that list
                            // back from what was read here. It is the small
                            // one of the pair (8 KB against 167 KB on a real
                            // track), and a missing one just yields nothing.
                            const std::string datRelative = anlzRelativePath(analyzePath, /*wantExt=*/false);
                            auto datBytes = m_anlzSource->read(datRelative);
                            track.cues = readCues(*bytes, datBytes ? *datBytes : std::string());
                        }
                        // The track's own edit time: rekordbox keeps a
                        // track's cues in its ANLZ .EXT file, so that
                        // file's mtime moves when this track's cues change
                        // and for no other track. Sync resolves a hot cue
                        // conflict with it instead of export.pdb's mtime,
                        // which moves for the whole library at once.
                        //
                        // Left at 0 (unknown) when the bytes did not come
                        // from a file on disk -- a browsed backup reads
                        // them out of an archive, where there is no mtime
                        // to take -- and Sync then falls back to the
                        // catalog dates for this track.
                        // The conversion is spelled out here rather than
                        // borrowed from stick_tree_walker's toUnixSeconds:
                        // this reader is compiled into narrow test targets
                        // that list their own sources, and the walker
                        // would drag its directory reader in with it.
                        std::error_code ec;
                        const auto written =
                            std::filesystem::last_write_time(std::filesystem::path(m_pioneerRoot) / extRelative, ec);
                        if (!ec) {
                            const auto asSystem = infrastructure::toSystemClock(written);
                            track.metadataModifiedAt =
                                std::chrono::duration_cast<std::chrono::seconds>(asSystem.time_since_epoch()).count();
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
