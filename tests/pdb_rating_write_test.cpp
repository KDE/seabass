// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "scratch_path.hpp"

using seabass::domain::Track;
using seabass::infrastructure::rekordbox::KaitaiRekordboxReader;
using seabass::infrastructure::rekordbox::PdbRowWriter;
namespace fs = std::filesystem;

namespace
{

// A fresh copy of the committed fixture's whole PIONEER tree, so the
// reader can resolve everything it normally does and each case starts
// from the same bytes.
fs::path freshPioneerCopy(const fs::path &scratch)
{
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    assert(fs::is_directory(source));
    const fs::path dest = scratch / "PIONEER";
    fs::copy(source, dest, fs::copy_options::recursive);
    return dest;
}

std::map<std::string, Track> byId(const std::vector<Track> &tracks)
{
    std::map<std::string, Track> out;
    for (const auto &track : tracks) {
        out[track.sourceId] = track;
    }
    return out;
}

// Everything about a track except its rating. A rating written at the
// wrong offset lands on color_id, on the padding beside it, or on
// duration, and shows up here rather than as a wrong rating -- which is
// the failure this test actually exists to catch. An offset counted off
// a spec by hand deserves to be checked against something other than the
// same count.
bool sameApartFromRating(const Track &a, const Track &b)
{
    return a.title == b.title && a.artist == b.artist && a.filename == b.filename && a.filePath == b.filePath &&
           a.durationSeconds == b.durationSeconds && a.bpm == b.bpm && a.key == b.key && a.bitrate == b.bitrate &&
           a.comment == b.comment && a.playCount == b.playCount && a.artworkPath == b.artworkPath &&
           a.cues.size() == b.cues.size();
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_pdb_rating_write_test";
    const fs::path pioneer = freshPioneerCopy(scratch);
    const fs::path pdb = pioneer / "rekordbox" / "export.pdb";
    assert(fs::exists(pdb));

    KaitaiRekordboxReader reader(pioneer.string());
    const auto before = reader.readAll();
    assert(!before.empty());
    const auto beforeById = byId(before);

    // A track the fixture leaves unrated, so "it now has a rating" is
    // unambiguous rather than a value that was already there.
    std::string targetId;
    for (const auto &track : before) {
        if (!track.rating) {
            targetId = track.sourceId;
            break;
        }
    }
    assert(!targetId.empty());
    const auto trackId = static_cast<uint32_t>(std::stoul(targetId));

    // ---- case 1: a rating lands, and nothing else moves --------------
    {
        PdbRowWriter writer(pdb.string());
        assert(writer.setTrackRating(trackId, 4));
        assert(writer.commit());

        KaitaiRekordboxReader after(pioneer.string());
        const auto afterTracks = after.readAll();
        assert(afterTracks.size() == before.size());
        const auto afterById = byId(afterTracks);

        assert(afterById.at(targetId).rating.has_value());
        assert(*afterById.at(targetId).rating == 4);

        // Every field of every track, unchanged -- including the target's
        // own everything-else.
        for (const auto &[id, track] : beforeById) {
            assert(afterById.count(id) == 1);
            assert(sameApartFromRating(track, afterById.at(id)));
            if (id != targetId) {
                assert(track.rating == afterById.at(id).rating);
            }
        }
        std::cout << "case 1 (the rating lands, and no other byte of any track moves) OK\n";
    }

    // ---- case 2: zero clears it -------------------------------------
    {
        PdbRowWriter writer(pdb.string());
        assert(writer.setTrackRating(trackId, 0));
        assert(writer.commit());

        KaitaiRekordboxReader after(pioneer.string());
        const auto afterById = byId(after.readAll());
        // rekordbox stores unrated and zero stars as the same byte, and
        // the reader maps 0 to "no rating". So writing 0 clears a rating
        // rather than setting a zero-star one -- there is no way to say
        // the second thing in this format, and a caller must not assume
        // otherwise.
        assert(!afterById.at(targetId).rating.has_value());
        std::cout << "case 2 (zero clears the rating; the format cannot say \"zero stars\") OK\n";
    }

    // ---- case 2b: a play count lands in its own u2 --------------------
    {
        // A track the fixture has played, so the new count replaces a real
        // one rather than filling a zero.
        std::string playedId;
        for (const auto &track : before) {
            if (track.playCount && *track.playCount > 0) {
                playedId = track.sourceId;
                break;
            }
        }
        assert(!playedId.empty());
        const auto playedRow = static_cast<uint32_t>(std::stoul(playedId));
        {
            PdbRowWriter writer(pdb.string());
            assert(writer.setTrackPlayCount(playedRow, 1234));
            assert(writer.commit());
        }
        KaitaiRekordboxReader after(pioneer.string());
        const auto afterById = byId(after.readAll());
        assert(afterById.at(playedId).playCount.has_value() && *afterById.at(playedId).playCount == 1234);
        // Nobody else's count, and none of the fields either side of it.
        for (const auto &[id, track] : beforeById) {
            const auto &now = afterById.at(id);
            if (id != playedId) {
                assert(track.playCount == now.playCount);
            }
            assert(track.durationSeconds == now.durationSeconds && track.bpm == now.bpm);
        }
        // Clamped to the field rather than wrapped round to a small number.
        {
            PdbRowWriter writer(pdb.string());
            assert(writer.setTrackPlayCount(playedRow, 70000));
            assert(writer.commit());
        }
        KaitaiRekordboxReader clamped(pioneer.string());
        assert(*byId(clamped.readAll()).at(playedId).playCount == 65535);
        PdbRowWriter missing(pdb.string());
        assert(!missing.setTrackPlayCount(4294967295u, 3));  // no such track
        std::cout << "case 2b (a play count lands, clamped to its u2, and no other count moves) OK\n";
    }

    // ---- case 3: refusals -------------------------------------------
    {
        PdbRowWriter writer(pdb.string());
        assert(!writer.setTrackRating(4294967295u, 3));  // no such track

        bool threw = false;
        try {
            writer.setTrackRating(trackId, 6);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            writer.setTrackRating(trackId, -1);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 3 (an unknown track says so; an impossible rating refuses) OK\n";
    }

    // ---- case 4: a comment cannot be grown --------------------------
    //
    // Measured here rather than assumed, because it decides what the
    // Restore Metadata page may offer. export.pdb keeps a track's
    // comment in a device_sql_string whose byte span is fixed at export
    // time, and overwriteTrackText() re-encodes into exactly that span.
    // So a comment can be shortened or replaced with one that fits, and
    // never lengthened.
    //
    // On the committed fixture 1160 of 1161 tracks have no comment at
    // all, which is a span of zero bytes: precisely the tracks a restore
    // would want to give a comment back to are the ones that cannot take
    // one. Growing the row is the page-allocator problem
    // docs/library-health-format-divergence.md sizes up, not a field
    // overwrite.
    {
        std::string emptyCommentId;
        std::string shortCommentId;
        for (const auto &track : before) {
            if (track.comment.empty() && emptyCommentId.empty()) {
                emptyCommentId = track.sourceId;
            }
            if (!track.comment.empty() && shortCommentId.empty()) {
                shortCommentId = track.sourceId;
            }
        }
        assert(!emptyCommentId.empty());
        assert(!shortCommentId.empty());

        const std::string wanted = "peak time closer, mixes into the Detroit one";
        for (const auto &id : {emptyCommentId, shortCommentId}) {
            const Track &track = beforeById.at(id);
            PdbRowWriter writer(pdb.string());
            PdbRowWriter::TrackTextOverride override;
            // All four fields are always written, so the three we do not
            // mean to change are passed back as they are.
            override.title = track.title;
            override.filename = track.filename;
            override.filePath = track.filePath;
            override.comment = wanted;
            assert(writer.overwriteTrackText(static_cast<uint32_t>(std::stoul(id)), override));
            assert(writer.commit());

            KaitaiRekordboxReader after(pioneer.string());
            const std::string got = byId(after.readAll()).at(id).comment;
            // Truncated, never grown to what was asked for. The span is
            // not the same as the old text's length -- the fixture's one
            // commented track carries 14 characters in a span that takes
            // 20 -- so the limit is whatever the exporter left room for,
            // which a caller has no way to know in advance.
            assert(got.size() < wanted.size());
            assert(got == wanted.substr(0, got.size()));
            if (track.comment.empty()) {
                // And an empty comment is a span of nothing at all.
                assert(got.empty());
            }
        }
        std::cout << "case 4 (a rekordbox comment fits its existing span or not at all) OK\n";
    }

    // The pdb's other name tables. Genres, albums and labels had no
    // writer at all until the byte sweep found a real export shipping
    // every album title and record label in the library, so this asserts
    // the new one reaches all three and that the file still parses after.
    {
        const fs::path pioneer = freshPioneerCopy(scratch);
        const fs::path pdb = pioneer / "rekordbox" / "export.pdb";

        auto namesIn = [&](const fs::path &file) {
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string before = namesIn(pdb);

        int albums = 0;
        int genres = 0;
        int labels = 0;
        {
            PdbRowWriter writer(pdb.string());
            int leftAlone = -1;
            albums = writer.overwriteAllNames(PdbRowWriter::NameTable::Albums,
                                              [](size_t i) { return "Album " + std::to_string(i); }, &leftAlone);
            assert(leftAlone == 0);
            genres = writer.overwriteAllNames(PdbRowWriter::NameTable::Genres,
                                              [](size_t i) { return "Genre " + std::to_string(i); }, &leftAlone);
            assert(leftAlone == 0);
            labels = writer.overwriteAllNames(PdbRowWriter::NameTable::Labels,
                                              [](size_t i) { return "Label " + std::to_string(i); }, &leftAlone);
            assert(leftAlone == 0 && "a healthy export leaves no name row behind");
            assert(writer.commit());
        }
        // The fixture really does have rows in all three tables; a silent
        // zero here would make the whole case vacuous.
        assert(albums > 0);
        assert(genres > 0);
        assert(labels > 0);

        const std::string after = namesIn(pdb);
        assert(after.size() == before.size());  // never resized, like every other edit
        assert(after != before);
        // Each placeholder is written into the exact byte span the real
        // name occupied, so what survives depends on how long that name
        // was. Album and genre names here are long enough to hold the
        // index; the label names are not, and truncate to "Label" -- the
        // same behaviour every other overwrite in this class has, not a
        // wrong offset. Labels share both the code path and the offset
        // constant with genres, which is checked with its index intact.
        assert(after.find("Album 0") != std::string::npos);
        assert(after.find("Genre 0") != std::string::npos);
        assert(after.find("Label") != std::string::npos);

        // And the catalog still reads back intact -- a name written at a
        // wrong offset corrupts the row it lands in, which shows up here
        // rather than as a wrong name.
        KaitaiRekordboxReader reader(pioneer.string());
        const auto tracks = reader.readAll();
        assert(tracks.size() > 1000);

        std::cout << "case 5 (albums/genres/labels are all scrubbed: " << albums << "/" << genres << "/" << labels
                  << ", and the catalog still reads) OK\n";
    }

    // ---- Name offsets that leave their row's page ----
    //
    // Every text write in this writer is in place, at an offset read out
    // of the row: a u1 or u2 for where the string starts, and the
    // string's own u2 header for how long it is. A page is 4096 bytes and
    // those reach 65535, so a damaged one points into another table's
    // page -- here, at a genre or label name -- and an unbounded write
    // overwrites THAT with "Album N" or "Artist N" or a track title.
    // buffer.at() never objects: the target is inside the file.
    //
    // The bound was added to overwriteAllNames() for #46 and existed for
    // the tag names before that, but the per-artist and per-track
    // writers the anonymizer runs FIRST never had it.
    {
        auto readAll = [](const fs::path &file) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        auto writeAll = [](const fs::path &file, const std::string &bytes) {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        // Present rows by table, found through the parser rather than
        // through the writer under test.
        struct Rows
        {
            std::size_t lenPage = 0;
            std::vector<std::size_t> albums, artists, tracks;
            std::vector<std::size_t> otherNames;  // genre and label name strings
        };
        auto rowsIn = [](const std::string &bytes) {
            Rows r;
            std::istringstream iss(bytes);
            kaitai::kstream ks(&iss);
            rekordbox_pdb_t parsed(false, &ks);
            r.lenPage = parsed.len_page();
            for (const auto &t : *parsed.tables()) {
                std::vector<std::size_t> *into = nullptr;
                std::size_t nameAt = 0;
                switch (t->type()) {
                case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: into = &r.albums; break;
                case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: into = &r.artists; break;
                case rekordbox_pdb_t::PAGE_TYPE_TRACKS: into = &r.tracks; break;
                case rekordbox_pdb_t::PAGE_TYPE_GENRES:
                case rekordbox_pdb_t::PAGE_TYPE_LABELS: into = &r.otherNames; nameAt = 4; break;
                default: continue;
                }
                auto pageRef = t->first_page();
                for (;;) {
                    auto page = pageRef->body();
                    if (page->is_data_page()) {
                        for (const auto &group : *page->row_groups()) {
                            for (const auto &row : *group->rows()) {
                                if (row->present()) {
                                    into->push_back(r.lenPage * page->page_index() + row->row_base() + nameAt);
                                }
                            }
                        }
                    }
                    if (pageRef->index() == t->last_page()->index()) {
                        break;
                    }
                    pageRef = page->next_page();
                }
            }
            return r;
        };
        // A row, and another table's name on a different page less than a
        // u2 further on: somewhere a damaged offset in that row reaches.
        auto withinReach = [](const Rows &r, const std::vector<std::size_t> &rows, std::size_t &row,
                              std::size_t &victim) {
            for (const std::size_t a : rows) {
                for (const std::size_t n : r.otherNames) {
                    if (n > a && n - a <= 0xFFFF && n / r.lenPage != a / r.lenPage) {
                        row = a;
                        victim = n;
                        return true;
                    }
                }
            }
            return false;
        };
        auto putU16 = [](std::string &bytes, std::size_t at, std::size_t value) {
            bytes[at] = static_cast<char>(value & 0xFF);
            bytes[at + 1] = static_cast<char>((value >> 8) & 0xFF);
        };

        // 5b: the wholesale album pass. Refused, and counted.
        {
            const fs::path pdb = freshPioneerCopy(scratch) / "rekordbox" / "export.pdb";
            std::string bytes = readAll(pdb);
            const Rows r = rowsIn(bytes);
            std::size_t row = 0, victim = 0;
            assert(withinReach(r, r.albums, row, victim) && "an album row within reach of another table's name");
            bytes[row] = static_cast<char>(static_cast<unsigned char>(bytes[row]) | 0x04);  // far-name form
            putU16(bytes, row + 0x16, victim - row);
            writeAll(pdb, bytes);
            const std::string victimBefore = bytes.substr(victim, 32);

            int leftAlone = -1;
            int renamed = 0;
            {
                PdbRowWriter writer(pdb.string());
                renamed = writer.overwriteAllNames(PdbRowWriter::NameTable::Albums,
                                                   [](size_t i) { return "Album " + std::to_string(i); }, &leftAlone);
                assert(writer.commit());
            }
            assert(readAll(pdb).substr(victim, 32) == victimBefore && "the other table's name is not overwritten");
            assert(leftAlone == 1 && "and the album row that kept its real name is counted");
            assert(renamed > 0 && "the rest of the table is still scrubbed");
            std::cout << "case 5b (an album name offset pointing into another table is refused, and counted) OK\n";
        }

        // 5c: the per-artist writer, which the anonymizer runs before the
        // wholesale pass and which had no bound at all.
        {
            const fs::path pdb = freshPioneerCopy(scratch) / "rekordbox" / "export.pdb";
            std::string bytes = readAll(pdb);
            const Rows r = rowsIn(bytes);
            std::size_t row = 0, victim = 0;
            assert(withinReach(r, r.artists, row, victim) && "an artist row within reach of another table's name");
            std::uint32_t artistId = 0;
            std::memcpy(&artistId, bytes.data() + row + 4, 4);  // little-endian host, as everywhere in this suite
            bytes[row] = static_cast<char>(static_cast<unsigned char>(bytes[row]) | 0x04);
            putU16(bytes, row + 10, victim - row);
            writeAll(pdb, bytes);
            const std::string victimBefore = bytes.substr(victim, 32);
            {
                PdbRowWriter writer(pdb.string());
                assert(!writer.overwriteArtistName(artistId, "Artist 0") && "refused, not written somewhere else");
            }
            assert(readAll(pdb).substr(victim, 32) == victimBefore);
            std::cout << "case 5c (an artist name offset pointing into another table is refused) OK\n";
        }

        // 5d: a track whose title offset points into another table. The
        // whole row is refused -- nothing of it written -- so the caller
        // is told, instead of the other three fields going in and the
        // title's refusal being dropped.
        {
            const fs::path pioneer = freshPioneerCopy(scratch);
            const fs::path pdb = pioneer / "rekordbox" / "export.pdb";
            std::string bytes = readAll(pdb);
            const Rows r = rowsIn(bytes);
            std::size_t row = 0, victim = 0;
            assert(withinReach(r, r.tracks, row, victim) && "a track row within reach of another table's name");
            std::uint32_t trackId = 0;
            std::memcpy(&trackId, bytes.data() + row + 72, 4);
            constexpr std::size_t TitleSlot = 94 + 17 * 2;  // ofs_strings[17]
            putU16(bytes, row + TitleSlot, victim - row);
            writeAll(pdb, bytes);
            const std::string victimBefore = bytes.substr(victim, 32);
            const std::string rowBefore = bytes.substr(row, 512);

            PdbRowWriter::TrackTextOverride text;
            text.title = "Track 0";
            text.comment = "Comment 0";
            text.filename = "f0.mp3";
            text.filePath = "/Contents/f0.mp3";
            {
                PdbRowWriter writer(pdb.string());
                assert(writer.trackExists(trackId) && "the id was read from the row being damaged");
                assert(!writer.overwriteTrackText(trackId, text) && "the row is refused, so the caller hears of it");
                writer.commit();  // whatever it would commit, it must not include this row
            }
            const std::string after = readAll(pdb);
            assert(after.substr(victim, 32) == victimBefore && "the other table's name is not overwritten");
            assert(after.substr(row, 512) == rowBefore && "and no other field of the row was written either");
            std::cout << "case 5d (a track whose title offset leaves its page is refused whole) OK\n";
        }
    }

    fs::remove_all(scratch);
    std::cout << "all pdb_rating_write_test cases passed\n";
    return 0;
}
