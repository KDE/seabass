// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter's row index (what addTrack() and the calls a save makes per
// track look up) held against the writer as it was before the index:
// setIndexedForTesting(false) walks the tables on every call and copies
// the whole buffer to undo a refused track, as addTrack() did at commit
// f4362099.
//
// 1. The same 400 tracks (repeated and new artists, albums, genres,
//    labels, keys and covers, names that differ only in letter case,
//    non-ASCII names, enough rows to start new pages in every table they
//    touch, a playlist entry for many) added to copies of the T73 export.pdb
//    in four ways: a writer and a commit per track, one writer for all, and
//    a mix of the two, each with the index and without. The same way gives
//    the same bytes with and without it; one writer and a writer per track
//    differ in nothing but the sequence numbers commit() counts up.
// 2. The same with refusals among the tracks: a file named already (as
//    spelled and in another letter case), an analysis file named already,
//    an empty title, a title ending in a blank, a genre whose id cannot be
//    given (after the track's new artist and album rows were placed) and a
//    label whose page does not add up (after its artist, album and genre
//    rows were). Each refused alike with and without the index, and the
//    file in the end the bytes of a run without those tracks.
// 3. The index after every mutating call is the one built afresh from
//    the buffer, and a writer that makes every kind of edit between its
//    tracks ends with the bytes of one without the index.
// 4. The work per track does not grow with the library: no table walked
//    once the index is built, and no more than a few pages copied aside,
//    on the T73 file and on one 1,500 tracks larger. Without the index the
//    same tracks walk ever more rows, which is what this would catch.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;
using namespace seabass::infrastructure::rekordbox;

namespace
{

constexpr size_t LenPage = 4096;
constexpr std::uint32_t TracksTable = 0;
constexpr std::uint32_t GenresTable = 1;
constexpr std::uint32_t ArtistsTable = 2;
constexpr std::uint32_t AlbumsTable = 3;
constexpr std::uint32_t LabelsTable = 4;
constexpr std::uint32_t KeysTable = 5;
constexpr std::uint32_t ArtworkTable = 13;

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path &p, const std::string &bytes)
{
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

std::uint32_t u16(const std::string &b, size_t at)
{
    return static_cast<unsigned char>(b[at]) | (static_cast<unsigned char>(b[at + 1]) << 8);
}

std::uint32_t u32(const std::string &b, size_t at)
{
    return u16(b, at) | (u16(b, at + 2) << 16);
}

void putU16(std::string &b, size_t at, std::uint32_t v)
{
    b[at] = static_cast<char>(v & 0xFF);
    b[at + 1] = static_cast<char>((v >> 8) & 0xFF);
}

void putU32(std::string &b, size_t at, std::uint32_t v)
{
    putU16(b, at, v & 0xFFFF);
    putU16(b, at + 2, v >> 16);
}

// The table entry (type, empty candidate, first page, last page) of `type`.
size_t tableEntryOf(const std::string &pdb, std::uint32_t type)
{
    for (std::uint32_t t = 0; t < u32(pdb, 8); ++t) {
        if (u32(pdb, 28 + 16 * t) == type) {
            return 28 + 16 * t;
        }
    }
    assert(false && "the file has the table");
    return 0;
}

// How many data pages the table of `type` has.
size_t dataPagesOf(const std::string &pdb, std::uint32_t type)
{
    const size_t entry = tableEntryOf(pdb, type);
    size_t pages = 0;
    for (std::uint32_t page = u32(pdb, entry + 8);; page = u32(pdb, LenPage * page + 12)) {
        if ((static_cast<unsigned char>(pdb[LenPage * page + 27]) & 0x40) == 0) {
            ++pages;
        }
        if (page == u32(pdb, entry + 12)) {
            break;
        }
    }
    return pages;
}

// The heap offset of every present row of the table of `type`.
std::vector<size_t> presentRowsOf(const std::string &pdb, std::uint32_t type)
{
    std::vector<size_t> rows;
    const size_t entry = tableEntryOf(pdb, type);
    for (std::uint32_t page = u32(pdb, entry + 8);; page = u32(pdb, LenPage * page + 12)) {
        const size_t base = LenPage * page;
        if ((static_cast<unsigned char>(pdb[base + 27]) & 0x40) == 0) {
            for (std::uint32_t r = 0; r < (u32(pdb, base + 24) & 0x1FFF); ++r) {
                const size_t group = base + LenPage - (r / 16) * 0x24;
                if ((u16(pdb, group - 4) >> (r % 16)) & 1) {
                    rows.push_back(base + 40 + u16(pdb, group - 6 - 2 * (r % 16)));
                }
            }
        }
        if (page == u32(pdb, entry + 12)) {
            break;
        }
    }
    return rows;
}

// The file with every sequence number zeroed: the header's and each
// page's, which commit() sets and which count commits, not rows.
std::string withoutSequences(std::string pdb)
{
    putU32(pdb, 20, 0);
    for (size_t base = 0; base + LenPage <= pdb.size(); base += LenPage) {
        putU32(pdb, base + 16, 0);
    }
    return pdb;
}

std::string artistName(int k)
{
    return "Artist number " + std::to_string(k) + " of a library big enough";
}

// Track i of the set: what repeats and what is new is spread so that every
// lookup table gets a few hundred rows, some names twice in another case.
PdbRowWriter::NewTrack trackNumber(int i)
{
    PdbRowWriter::NewTrack t;
    const std::string artist = i % 13 == 0   ? "dj shadow"
                               : i % 13 == 1 ? "DJ Shadow"
                               : i % 17 == 0 ? "Bj\xc3\xb6rk Gu\xc3\xb0mundsd\xc3\xb3ttir"
                                             : artistName((i * 7) % 180);
    const std::string folder = i % 29 == 0 ? "Bj\xc3\xb6rk" : "Folder " + std::to_string(i % 40);
    t.pathOnStick = "/Contents/" + folder + "/" + std::to_string(i) + " Track"
                    + (i % 5 == 0 ? ".mp3" : i % 5 == 1 ? ".m4a" : i % 5 == 2 ? ".wav" : ".flac");
    t.title = "Title " + std::to_string(i) + (i % 5 == 0 ? " \xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9 \xe6\x9d\xb1\xe4\xba\xac" : "");
    t.artist = artist;
    t.remixer = i % 3 == 0 ? artistName((i * 5) % 220) : std::string();
    t.composer = i % 4 == 0 ? "Composer " + std::to_string(i % 90) : std::string();
    t.albumArtist = i % 2 == 1 ? artistName(i % 40) : std::string();
    t.album = i % 19 == 0 ? "ALBUM 3 TITLE" : "Album " + std::to_string((i * 3) % 160) + " Title";
    t.genre = i % 23 == 0 ? "genre number 11 long enough" : "Genre number " + std::to_string((i * 11) % 170) + " long enough";
    t.label = "Label " + std::to_string((i * 13) % 170) + " Records Limited";
    t.key = i % 6 == 5 ? std::string() : "Key " + std::to_string((i * 17) % 160) + " in minor or major";
    t.comment = i % 3 == 0 ? std::string() : "comment " + std::to_string(i);
    if (i % 2 == 0) {
        char analysis[64];
        std::snprintf(analysis, sizeof analysis, "/PIONEER/USBANLZ/P%03X/%08X/ANLZ0000.DAT", i % 4096, 0xAB000000u + i);
        t.analyzePath = analysis;
    }
    t.bpm = 100.0 + i % 60 + 0.25;
    t.durationSeconds = 200 + i;
    t.trackNumber = i % 20;
    t.discNumber = i % 3;
    t.year = 1990 + i % 30;
    t.bitrate = i % 5 == 0 ? 320 : 0;
    t.sampleDepth = i % 2 == 0 ? 16 : 24;
    t.sampleRateHz = i % 2 == 0 ? 44100 : 48000;
    t.fileSizeBytes = 1000000u + i;
    t.date = "2026-11-26";
    return t;
}

// The number trackNumber() made a track from.
int numberOf(const PdbRowWriter::NewTrack &t)
{
    return static_cast<int>(t.fileSizeBytes - 1000000u);
}

// Whether track i has a cover: named after the artwork row it will get,
// as the import names it (nextArtworkId()). Track i goes into the
// playlist when i is even.
bool withCover(int i)
{
    return i % 3 != 1;
}

// One way of adding tracks to a file.
struct Run
{
    // How many tracks each writer adds before its commit: {1, 1, ...} is
    // a writer per track, {N} one writer for all.
    std::vector<size_t> batches;
    bool indexed = true;
    // After every call: the index is the one built afresh (every part built
    // when the writer opens).
    bool checkIndex = false;
};

struct Outcome
{
    std::string bytes;
    // what() of every refusal, in order.
    std::vector<std::string> refusals;
};

// Adds `tracks` (a refusal expected where `refused` says) to the file at
// `pdb`, in playlist `playlist` for every other one; returns the file.
Outcome addAll(const fs::path &pdb, const std::vector<PdbRowWriter::NewTrack> &tracks, const std::vector<bool> &refused,
               const Run &run, std::uint32_t playlist)
{
    Outcome out;
    size_t next = 0;
    for (const size_t batch : run.batches) {
        PdbRowWriter w(pathToUtf8(pdb));
        w.setIndexedForTesting(run.indexed);
        if (run.checkIndex) {
            w.buildIndexForTesting();
        }
        for (size_t k = 0; k < batch && next < tracks.size(); ++k, ++next) {
            PdbRowWriter::NewTrack t = tracks[next];
            const int number = numberOf(t);
            if (withCover(number)) {
                t.artworkPath = PdbRowWriter::artworkPathFor('a', w.nextArtworkId());
            }
            try {
                const std::uint32_t id = w.addTrack(t);
                assert(!refused[next] && "a track that should be refused was added");
                if (number % 2 == 0) {
                    assert(w.appendPlaylistEntries(playlist, {id}) == 1);
                    assert(w.appendPlaylistEntries(playlist, {id}) == 0 && "a track already in it is not added twice");
                }
                assert(w.trackIdsWithFilePath(t.pathOnStick) == std::vector<std::uint32_t>{id});
            } catch (const std::exception &e) {
                if (!refused[next]) {
                    std::cerr << "unexpected refusal of " << t.pathOnStick << ": " << e.what() << "\n";
                }
                assert(refused[next] && "a track that should go in was refused");
                out.refusals.push_back(e.what());
            }
            if (run.checkIndex) {
                const std::string mismatch = w.indexMismatchForTesting();
                if (!mismatch.empty()) {
                    std::cerr << "after track " << next << " the index differs from the buffer in: " << mismatch << "\n";
                }
                assert(mismatch.empty());
            }
        }
        if (w.hasEdits()) {
            assert(w.commit());
        }
    }
    assert(next == tracks.size());
    out.bytes = readFile(pdb);
    return out;
}

template <typename Exception, typename Call>
std::string refusalOf(Call &&call)
{
    try {
        call();
    } catch (const Exception &e) {
        return e.what();
    } catch (const std::exception &e) {
        std::cerr << "another exception: " << e.what() << "\n";
        assert(false && "refused with another kind of exception");
    }
    assert(false && "not refused");
    return {};
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: pdb_row_index_test <tests/fixtures/testshark_t73>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_pdb_row_index_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    // The base: T73's export.pdb, copied, with one playlist to fill.
    const fs::path base = scratch / "base.pdb";
    fs::copy_file(fixture / "stick" / "PIONEER" / "rekordbox" / "export.pdb", base);
    std::uint32_t playlist = 0;
    {
        PdbRowWriter w(pathToUtf8(base));
        playlist = w.createPlaylist(0, "Index Test", false);
        assert(w.commit());
    }
    const std::string baseBytes = readFile(base);
    const auto copyOfBase = [&](const std::string &name) {
        const fs::path file = scratch / name;
        writeFile(file, baseBytes);
        return file;
    };

    const int count = 400;
    std::vector<PdbRowWriter::NewTrack> tracks;
    for (int i = 0; i < count; ++i) {
        tracks.push_back(trackNumber(i));
    }
    const std::vector<bool> noneRefused(tracks.size(), false);
    const std::vector<size_t> perTrack(tracks.size(), 1);
    const std::vector<size_t> oneWriter{tracks.size()};
    // Some alone, some together, a batch that starts and ends mid-page.
    const std::vector<size_t> mixed = [&] {
        std::vector<size_t> batches;
        size_t done = 0;
        for (size_t step : {1, 1, 1, 37, 1, 120, 1, 1, 64, 1, 13}) {
            batches.push_back(step);
            done += step;
        }
        batches.push_back(tracks.size() - done);
        return batches;
    }();

    // 1. Each way with and without the index.
    {
        const Outcome perTrackBefore = addAll(copyOfBase("per-track-before.pdb"), tracks, noneRefused, {perTrack, false, false}, playlist);
        const Outcome perTrackNow = addAll(copyOfBase("per-track-now.pdb"), tracks, noneRefused, {perTrack, true, false}, playlist);
        assert(perTrackNow.bytes == perTrackBefore.bytes && "a writer and a commit per track: the same bytes");
        std::cout << "case 1a (a writer and a commit per track: the same bytes with the index as without) OK\n";

        const Outcome oneBefore = addAll(copyOfBase("one-before.pdb"), tracks, noneRefused, {oneWriter, false, false}, playlist);
        const Outcome oneNow = addAll(copyOfBase("one-now.pdb"), tracks, noneRefused, {oneWriter, true, true}, playlist);
        assert(oneNow.bytes == oneBefore.bytes && "one writer for all: the same bytes");
        std::cout << "case 1b (one writer for all 400: the same bytes with the index as without; the index the "
                     "buffer's after every track) OK\n";

        const Outcome mixedBefore = addAll(copyOfBase("mixed-before.pdb"), tracks, noneRefused, {mixed, false, false}, playlist);
        const Outcome mixedNow = addAll(copyOfBase("mixed-now.pdb"), tracks, noneRefused, {mixed, true, true}, playlist);
        assert(mixedNow.bytes == mixedBefore.bytes && "some alone, some together: the same bytes");
        std::cout << "case 1c (some tracks alone, some together: the same bytes with the index as without) OK\n";

        // Between the ways: nothing but the sequence numbers. Those count
        // commits: one writer for all moves the header's on by one, a
        // writer per track by one per track.
        assert(oneNow.bytes != perTrackNow.bytes);
        assert(withoutSequences(oneNow.bytes) == withoutSequences(perTrackNow.bytes));
        assert(withoutSequences(mixedNow.bytes) == withoutSequences(perTrackNow.bytes));
        assert(u32(oneNow.bytes, 20) == u32(baseBytes, 20) + 1);
        assert(u32(perTrackNow.bytes, 20) == u32(baseBytes, 20) + count);
        assert(u32(mixedNow.bytes, 20) == u32(baseBytes, 20) + mixed.size());
        std::cout << "case 1d (one writer, a writer per track, and the mix differ only in the sequence numbers that "
                     "count commits) OK\n";

        // What the set covers: new pages in every table a track row names,
        // three of them tables the fixture had no row in.
        for (const std::uint32_t table : {TracksTable, GenresTable, ArtistsTable, AlbumsTable, LabelsTable, KeysTable, ArtworkTable}) {
            std::cerr << "  table " << table << ": " << dataPagesOf(baseBytes, table) << " data pages before, "
                      << dataPagesOf(oneNow.bytes, table) << " after\n";
            assert(dataPagesOf(oneNow.bytes, table) >= dataPagesOf(baseBytes, table) + 1);
        }
        assert(dataPagesOf(baseBytes, GenresTable) == 0 && dataPagesOf(baseBytes, LabelsTable) == 0
               && dataPagesOf(baseBytes, ArtworkTable) == 0);
        // And the app reads every track, the playlist with every other one.
        const fs::path stick = scratch / "read" / "PIONEER" / "rekordbox";
        fs::create_directories(stick);
        writeFile(stick / "export.pdb", oneNow.bytes);
        const auto read = KaitaiRekordboxReader(pathToUtf8(scratch / "read" / "PIONEER")).readAll();
        assert(read.size() == 11 + tracks.size());
        size_t inPlaylist = 0;
        bool nonAscii = false;
        for (const auto &t : read) {
            inPlaylist += t.playlists.size();
            nonAscii = nonAscii || t.artist == "Bj\xc3\xb6rk Gu\xc3\xb0mundsd\xc3\xb3ttir";
        }
        assert(inPlaylist >= tracks.size() / 2 && nonAscii);
        std::cout << "case 1e (the set starts new pages in all seven tables it writes and reads back whole) OK\n";
    }

    // 2. Refusals among the tracks. The base for it holds a genre whose id
    //    is at the top of the range (no new genre can be given one) and a
    //    label page that does not add up (no new label can go on it).
    {
        const fs::path refusingBase = copyOfBase("refusing-base.pdb");
        {
            PdbRowWriter w(pathToUtf8(refusingBase));
            PdbRowWriter::NewTrack t = trackNumber(9000);
            t.genre = "Top Genre";
            t.label = "Broken Label";
            w.addTrack(t);
            assert(w.commit());
        }
        std::string bytes = readFile(refusingBase);
        const auto genres = presentRowsOf(bytes, GenresTable);
        assert(genres.size() == 1);
        putU32(bytes, genres.front(), 0xFFFFFFF0u);
        const std::uint32_t labelsLast = u32(bytes, tableEntryOf(bytes, LabelsTable) + 12);
        putU16(bytes, LenPage * labelsLast + 28, u16(bytes, LenPage * labelsLast + 28) + 4);
        writeFile(refusingBase, bytes);
        const std::string refusingBytes = bytes;

        std::vector<PdbRowWriter::NewTrack> attempts;
        std::vector<bool> refused;
        std::vector<PdbRowWriter::NewTrack> kept;
        const auto attempt = [&](PdbRowWriter::NewTrack t, bool refuse) {
            attempts.push_back(t);
            refused.push_back(refuse);
            if (!refuse) {
                kept.push_back(t);
            }
        };
        for (int i = 0; i < 160; ++i) {
            PdbRowWriter::NewTrack t = trackNumber(i);
            t.genre = i % 3 == 0 ? std::string() : "Top Genre";
            t.label = i % 4 == 0 ? std::string() : "Broken Label";
            attempt(t, false);
            if (i % 10 == 3) {
                PdbRowWriter::NewTrack again = trackNumber(i);
                again.title = "Another title";
                again.analyzePath.clear();
                attempt(again, true);  // the same file
            }
            if (i % 10 == 4) {
                PdbRowWriter::NewTrack cased = trackNumber(i);
                for (char &c : cased.pathOnStick) {
                    c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
                }
                cased.analyzePath.clear();
                attempt(cased, true);  // the same file in other letters
            }
            if (i % 10 == 6 && !trackNumber(i).analyzePath.empty()) {
                PdbRowWriter::NewTrack shared = trackNumber(10000 + i);
                shared.analyzePath = trackNumber(i).analyzePath;
                attempt(shared, true);  // another track's analysis file
            }
            if (i % 10 == 7) {
                PdbRowWriter::NewTrack untitled = trackNumber(11000 + i);
                untitled.title.clear();
                attempt(untitled, true);
                PdbRowWriter::NewTrack blank = trackNumber(12000 + i);
                blank.title = "Ends in a blank ";
                attempt(blank, true);
            }
            if (i % 10 == 8) {
                // A new artist and album, then a genre no id can be given.
                PdbRowWriter::NewTrack newGenre = trackNumber(13000 + i);
                newGenre.artist = "Only In A Refused Track " + std::to_string(i);
                newGenre.album = "Album Only In A Refused Track " + std::to_string(i);
                newGenre.genre = "A New Genre " + std::to_string(i);
                newGenre.label.clear();
                attempt(newGenre, true);
                // A new artist, album and key, then a label its page cannot take.
                PdbRowWriter::NewTrack newLabel = trackNumber(14000 + i);
                newLabel.artist = "Another Only In A Refused Track " + std::to_string(i);
                newLabel.genre = "Top Genre";
                newLabel.key = "A key only a refused track has " + std::to_string(i);
                newLabel.label = "A New Label " + std::to_string(i);
                attempt(newLabel, true);
            }
        }
        const auto run = [&](const std::string &name, const std::vector<PdbRowWriter::NewTrack> &list,
                             const std::vector<bool> &refusals, const Run &how) {
            const fs::path file = scratch / name;
            writeFile(file, refusingBytes);
            return addAll(file, list, refusals, how, playlist);
        };
        const std::vector<bool> keptNone(kept.size(), false);
        const Outcome withIndex = run("refusals-now.pdb", attempts, refused, {{attempts.size()}, true, true});
        const Outcome without = run("refusals-before.pdb", attempts, refused, {{attempts.size()}, false, false});
        const Outcome keptOnly = run("kept-now.pdb", kept, keptNone, {{kept.size()}, true, false});
        const Outcome keptBefore = run("kept-before.pdb", kept, keptNone, {{kept.size()}, false, false});
        const Outcome perTrackWithIndex =
            run("refusals-per-track.pdb", attempts, refused, {std::vector<size_t>(attempts.size(), 1), true, true});
        const Outcome keptPerTrack = run("kept-per-track.pdb", kept, keptNone, {std::vector<size_t>(kept.size(), 1), false, false});
        assert(withIndex.refusals.size() == static_cast<size_t>(std::count(refused.begin(), refused.end(), true)));
        assert(withIndex.refusals == without.refusals && "each refused alike, in the same words");
        assert(withIndex.refusals == perTrackWithIndex.refusals);
        size_t midway = 0;
        for (const std::string &why : withIndex.refusals) {
            midway += why.find("not making a row after that") != std::string::npos || why.find("does not add up") != std::string::npos;
        }
        assert(midway == 2 * 16 && "the refusals after rows were placed are among them");
        assert(withIndex.bytes == without.bytes);
        assert(withIndex.bytes == keptOnly.bytes && "a refused track leaves no trace, its artist and album rows included");
        assert(keptOnly.bytes == keptBefore.bytes);
        assert(perTrackWithIndex.bytes == keptPerTrack.bytes);
        assert(withIndex.bytes != refusingBytes);
        std::cout << "case 2 (refusals among the tracks, two kinds after rows were placed: each refused alike with and "
                     "without the index, and the file the bytes of a run without those tracks) OK\n";
    }

    // 3. Every kind of edit between tracks: after each, the index is the
    //    one built afresh from the buffer (then built whole again, so the
    //    next edit is checked against every part), and the file in the end
    //    is the bytes of the writer without the index.
    {
        const auto everyEdit = [&](const fs::path &file, bool indexed) {
            PdbRowWriter w(pathToUtf8(file));
            w.setIndexedForTesting(indexed);
            std::vector<std::string> said;
            const auto check = [&](const std::string &what) {
                said.push_back(what);
                if (!indexed) {
                    return;
                }
                const std::string mismatch = w.indexMismatchForTesting();
                if (!mismatch.empty()) {
                    std::cerr << "after " << what << " the index differs from the buffer in: " << mismatch << "\n";
                }
                assert(mismatch.empty());
                w.buildIndexForTesting();
            };
            if (indexed) {
                w.buildIndexForTesting();
            }
            std::vector<std::uint32_t> ids;
            const auto add = [&](PdbRowWriter::NewTrack t) {
                if (withCover(numberOf(t))) {
                    t.artworkPath = PdbRowWriter::artworkPathFor('a', w.nextArtworkId());
                }
                ids.push_back(w.addTrack(t));
                if (numberOf(t) % 2 == 0) {
                    w.appendPlaylistEntries(playlist, {ids.back()});
                }
                check("adding " + t.pathOnStick + " as " + std::to_string(ids.back()));
            };
            for (int i = 0; i < 60; ++i) {
                add(trackNumber(i));
            }
            bool keyWritten = false;
            check("a key there is: " + std::to_string(w.setTrackTempoAndKey(ids[5], 128.0, "Key 17 in minor or major", &keyWritten))
                  + std::to_string(keyWritten));
            check("a key nobody had: " + std::to_string(w.setTrackTempoAndKey(ids[11], 99.5, "A key nobody had", &keyWritten))
                  + std::to_string(keyWritten));
            check("removed: " + std::to_string(w.removeTrack(ids[3])));
            add(trackNumber(3));  // its file is named by no present row now
            check("text: " + std::to_string(w.overwriteTrackText(ids[7], {"New Title", "New comment", "moved.flac",
                                                                          "/Contents/Moved/moved.flac"})));
            add(trackNumber(7));
            {
                const auto capacity = w.trackPathCapacity(ids[9]);
                assert(capacity && capacity->filePath > 12 && capacity->filename > 4);
                std::string path = "/Contents/R/" + std::string(capacity->filePath - 12, 'r');
                std::string name = std::string(capacity->filename - 4, 'n') + ".mp3";
                std::string refusal;
                check("path: " + std::to_string(w.setTrackFilePath(ids[9], name, path, &refusal)) + refusal);
                add(trackNumber(9));
            }
            int left = 0;
            check("artists renamed: "
                  + std::to_string(w.overwriteAllNames(PdbRowWriter::NameTable::Artists,
                                                        [](size_t index) { return "P" + std::to_string(index); }, &left)));
            check("genres renamed: "
                  + std::to_string(w.overwriteAllNames(PdbRowWriter::NameTable::Genres,
                                                        [](size_t index) { return "G" + std::to_string(index); }, &left)));
            for (int i = 60; i < 70; ++i) {
                add(trackNumber(i));  // their artists and genres by the old names: new rows
            }
            check("appended: " + std::to_string(w.appendPlaylistEntries(playlist, {ids[1], ids[3], ids[1]})));
            check("removed entries: " + std::to_string(w.removePlaylistEntries(playlist, {ids[0]})));
            {
                std::vector<std::uint32_t> order = w.playlistTrackIds(playlist);
                std::reverse(order.begin(), order.end());
                check("reordered: " + std::to_string(w.reorderPlaylist(playlist, order)));
            }
            check("removed entry: " + std::to_string(w.removePlaylistEntry(playlist, ids[2])));
            check("repointed: " + std::to_string(w.repointPlaylistEntry(playlist, ids[4], ids[13])));
            check("reassigned: " + std::to_string(w.reassignPlaylistMemberships(ids[6], ids[15])));
            const std::uint32_t second = w.createPlaylist(0, "Second", false);
            check("created");
            check("second filled: " + std::to_string(w.appendPlaylistEntries(second, {ids[20], ids[21], ids[22]})));
            check("renamed: " + std::to_string(w.renamePlaylist(second, "Second Renamed")));
            check("deleted: " + std::to_string(w.deletePlaylist(second)));
            check("rating: " + std::to_string(w.setTrackRating(ids[12], 4)));
            check("plays: " + std::to_string(w.setTrackPlayCount(ids[12], 17)));
            check("copied: " + std::to_string(w.copyTrackFieldsIfMissing(ids[1], ids[11], true, true, true)));
            {
                std::string refusal;
                check("comment: " + std::to_string(w.setTrackComment(ids[14], "short", &refusal)) + refusal);
            }
            check("extra: " + std::to_string(w.overwriteTrackExtraText(ids[16], {"", "", "", ""})));
            for (int i = 70; i < 80; ++i) {
                add(trackNumber(i));
            }
            // The highest track id a deleted row's: zeroing the space of
            // deleted rows takes it, and the next id is one lower.
            add(trackNumber(399));
            check("the newest removed: " + std::to_string(w.removeTrack(ids.back())));
            check("zeroed: " + std::to_string(w.zeroUnusedSpace()));
            for (int i = 80; i < 100; ++i) {
                add(trackNumber(i));
            }
            assert(w.commit());
            return std::make_pair(readFile(file), said);
        };
        const auto now = everyEdit(copyOfBase("every-edit-now.pdb"), true);
        const auto before = everyEdit(copyOfBase("every-edit-before.pdb"), false);
        assert(now.second == before.second && "every call answered alike");
        assert(now.first == before.first);
        std::cout << "case 3 (every kind of edit between tracks: the index the buffer's after each, and the same "
                     "bytes as without it) OK\n";
    }

    // 4. The work per track, after the first, on T73's file and on one
    //    1,500 tracks larger: rows of whole-table walks, the walks, and
    //    bytes copied aside to undo a track.
    {
        const fs::path small = copyOfBase("scale-small.pdb");
        const fs::path large = copyOfBase("scale-large.pdb");
        {
            PdbRowWriter w(pathToUtf8(large));
            for (int i = 0; i < 1500; ++i) {
                PdbRowWriter::NewTrack t = trackNumber(20000 + i);
                t.analyzePath.clear();
                w.addTrack(t);
            }
            assert(w.commit());
        }
        struct Work
        {
            PdbRowWriter::WorkCounts first;
            PdbRowWriter::WorkCounts rest;
        };
        const auto minus = [](PdbRowWriter::WorkCounts a, const PdbRowWriter::WorkCounts &b) {
            a.rowsVisited -= b.rowsVisited;
            a.tableWalks -= b.tableWalks;
            a.bytesCopied -= b.bytesCopied;
            return a;
        };
        const int more = 100;
        const auto measure = [&](const fs::path &file, bool indexed) {
            PdbRowWriter w(pathToUtf8(file));
            w.setIndexedForTesting(indexed);
            const auto one = [&](int i) {
                PdbRowWriter::NewTrack t = trackNumber(30000 + i);
                t.artworkPath = PdbRowWriter::artworkPathFor('a', w.nextArtworkId());
                const std::uint32_t id = w.addTrack(t);
                w.appendPlaylistEntries(playlist, {id});
                assert(w.trackIdsWithFilePath(t.pathOnStick).size() == 1);
            };
            Work work;
            const auto start = PdbRowWriter::workCountsForTesting();
            one(0);
            const auto afterFirst = PdbRowWriter::workCountsForTesting();
            for (int i = 1; i <= more; ++i) {
                one(i);
            }
            work.first = minus(afterFirst, start);
            work.rest = minus(PdbRowWriter::workCountsForTesting(), afterFirst);
            return work;
        };
        const Work smallNow = measure(small, true);
        const Work largeNow = measure(large, true);
        const Work smallBefore = measure(copyOfBase("scale-small-before.pdb"), false);
        const fs::path largeAgain = scratch / "scale-large-before.pdb";
        fs::copy_file(large, largeAgain);
        const Work largeBefore = measure(largeAgain, false);
        std::cerr << "  per " << more << " tracks with the index: rows " << smallNow.rest.rowsVisited << " / "
                  << largeNow.rest.rowsVisited << ", walks " << smallNow.rest.tableWalks << " / " << largeNow.rest.tableWalks
                  << ", bytes copied " << smallNow.rest.bytesCopied << " / " << largeNow.rest.bytesCopied
                  << " (T73 / 1,500 more); the first track walked " << smallNow.first.tableWalks << " / "
                  << largeNow.first.tableWalks << " tables\n";
        std::cerr << "  without it: rows " << smallBefore.rest.rowsVisited << " / " << largeBefore.rest.rowsVisited
                  << ", bytes copied " << smallBefore.rest.bytesCopied << " / " << largeBefore.rest.bytesCopied << "\n";
        for (const Work *work : {&smallNow, &largeNow}) {
            assert(work->rest.rowsVisited == 0 && work->rest.tableWalks == 0 && "no table walked once the index is built");
            // At most a page for each row a track adds (four artists, an
            // album, a genre, a label, a key, a cover, the track, a
            // playlist entry) and the header page.
            assert(work->rest.bytesCopied <= static_cast<size_t>(more) * 12 * LenPage && "a few pages a track, not the file");
            // Each of the nine parts built once: a walk for its rows and
            // one for its highest id, the history's highest id alone.
            assert(work->first.tableWalks <= 17 && "each part of the index built once");
        }
        // What the counts would show if the cost grew with the library again.
        assert(largeBefore.rest.rowsVisited > 3 * smallBefore.rest.rowsVisited);
        assert(largeBefore.rest.bytesCopied > 2 * smallBefore.rest.bytesCopied);
        std::cout << "case 4 (the work per track does not grow with the library: no table walked, a few pages copied, "
                     "on T73 and on 1,500 tracks more) OK\n";
    }

    std::cout << "pdb_row_index_test: all cases passed\n";
    return 0;
}
