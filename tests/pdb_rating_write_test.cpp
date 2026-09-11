// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

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

    fs::remove_all(scratch);
    std::cout << "all pdb_rating_write_test cases passed\n";
    return 0;
}
