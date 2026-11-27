// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter's row index (what addTrack() and the calls a save makes per
// track look up) held against the writer as it was before the index:
// setIndexedForTesting(false) walks the tables on every call and copies
// the whole buffer to undo a refused edit, as addTrack() and
// setTrackTempoAndKey() did at commit f4362099.
//
// What that reference shares with the indexed path, and so cannot tell
// apart from it: the code that places and writes rows and pages
// (placeRows(), rowsFitting(), writeRows(), startPage(), appendRowsToPage(),
// addRow(), pageAddsUp(), deleteRowAt()), the walks that find rows and
// ids (findRow(), playlistEntriesOf(), forEachPresentRow(),
// highestIdEverIn()), newTrackRefusal(), the row encoders and commit().
// Of those, what differs from f4362099 is bookkeeping a writer that never
// builds its index and never opens an undo does not act on (the index
// updated or dropped, pages taken aside for an undo), the rows added
// recorded for commit()'s read-back, the refusal of a page chain that comes
// back on itself, and that read-back itself. The f4362099 code is not built
// here: a copy of it in the repository is what this must not hold, and the
// commit is not in a shallow clone to build it from. It was held against
// this writer outside the repository (addTrack() and the playlist calls on
// every export.pdb at hand: the same bytes and answers).
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
// 5. An append over a slot past num_row_offsets whose present bit is set:
//    the index forgets the row the slot held.
// 6. A track row whose analyze_path, or file_path, the parser cannot read:
//    every call answers as the walks did, thrown or not, in their words.
// 7. Album rows of one name with ids 3, 0 and 5 in every order: the album
//    a new track takes is the one the walk took.
// 8. commit() refuses a row its table's pages do not reach and a table
//    added to that does not read back; a track naming no genre is not
//    refused for an unreadable genre table; a track's own read-back
//    refuses a name that leads to another row.
// 9. A table entry across two small pages: a refused track puts back both.
// 10. setTrackTempoAndKey() refused alike with and without the index.
// 11. A real export (anonymized): lookup rows there already, history rows,
//     deleted rows, UTF-16 strings; the same answers and bytes.
// 12. A page chain that comes back on itself is refused, not walked for
//     ever.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;
using namespace seabass::infrastructure::rekordbox;
using Pdb = rekordbox_pdb_t;

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

// What a call did: "ok" and what it returned, or the kind and the words of
// what it threw, for holding one way of answering against another.
std::string outcomeOf(const std::function<std::string()> &call)
{
    try {
        return "ok " + call();
    } catch (const std::exception &e) {
        return std::string(typeid(e).name()) + ": " + e.what();
    }
}

// The file a writer opened on `bytes` holds; the writer itself, set up.
struct Opened
{
    std::unique_ptr<PdbRowWriter> writer;
    fs::path file;
};
Opened openOn(const fs::path &file, const std::string &bytes, bool indexed)
{
    writeFile(file, bytes);
    Opened opened{std::make_unique<PdbRowWriter>(pathToUtf8(file)), file};
    opened.writer->setIndexedForTesting(indexed);
    if (indexed) {
        opened.writer->buildIndexForTesting();
    }
    return opened;
}

// A row's string `index` (track rows: ofs_strings at 94): where its header is.
size_t trackStringAt(const std::string &pdb, size_t body, int index)
{
    return body + u16(pdb, body + 94 + 2 * static_cast<size_t>(index));
}

// The names of the present rows of a table of names, in table order.
std::vector<std::string> namesIn(const std::string &pdb, rekordbox_pdb_t::page_type_t type)
{
    using Pdb = rekordbox_pdb_t;
    std::vector<std::string> names;
    std::istringstream in(pdb);
    kaitai::kstream ks(&in);
    Pdb parsed(false, &ks);
    for (const auto &table : *parsed.tables()) {
        if (table->type() != type) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    kaitai::kstruct *body = row->body();
                    if (auto *g = dynamic_cast<Pdb::genre_row_t *>(body)) {
                        names.push_back(sqlText(g->name()));
                    } else if (auto *l = dynamic_cast<Pdb::label_row_t *>(body)) {
                        names.push_back(sqlText(l->name()));
                    } else if (auto *k = dynamic_cast<Pdb::key_row_t *>(body)) {
                        names.push_back(sqlText(k->name()));
                    } else if (auto *a = dynamic_cast<Pdb::artist_row_t *>(body)) {
                        names.push_back(sqlText(a->name()));
                    } else if (auto *w = dynamic_cast<Pdb::artwork_row_t *>(body)) {
                        names.push_back(sqlText(w->path()));
                    } else if (auto *al = dynamic_cast<Pdb::album_row_t *>(body)) {
                        names.push_back(sqlText(al->name()));
                    } else if (auto *t = dynamic_cast<Pdb::track_row_t *>(body)) {
                        names.push_back(sqlText(t->file_path()) + "\t" + sqlText(t->analyze_path()));
                    }
                }
            }
        });
    }
    return names;
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
    if (argc < 3) {
        std::cerr << "usage: pdb_row_index_test <tests/fixtures/testshark_t73> <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path anonymized = seabass::pathFromUtf8(argv[2]);
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
                // Master's comment edit (#108) rebuilds the row: written over
                // itself where it fits, moved (the old row deleted, the new
                // one on the same page or the table's last) where it does not.
                check("comment grown in place: " + std::to_string(w.setTrackComment(ids[14], "a little longer", &refusal))
                      + refusal);
                check("comment moved: " + std::to_string(w.setTrackComment(ids[17], std::string(300, 'c'), &refusal))
                      + refusal);
                check("comment moved again: "
                      + std::to_string(w.setTrackComment(ids[17], std::string(900, 'd'), &refusal)) + refusal);
                check("comment refused: "
                      + std::to_string(w.setTrackComment(ids[18], std::string(70000, 'e'), &refusal)) + refusal);
                check("comment on no row: " + std::to_string(w.setTrackComment(999999, "none", &refusal)));
            }
            check("a moved row found by its file: "
                  + std::to_string(w.trackIdsWithFilePath(trackNumber(17).pathOnStick).size()));
            {
                const auto kept = w.trackTempoAndKeyId(ids[5]);
                assert(kept);
                check("tempo and key id put back: " + std::to_string(w.setTrackTempoAndKeyId(ids[19], *kept)));
            }
            check("artist renamed in place: " + std::to_string(w.overwriteArtistName(1, "Someone Else")));
            check("playlist renamed in place: " + std::to_string(w.overwritePlaylistName(playlist, "Held In Place")));
            check("albums renamed: "
                  + std::to_string(w.overwriteAllNames(PdbRowWriter::NameTable::Albums,
                                                        [](size_t index) { return "A" + std::to_string(index); }, &left)));
            check("labels renamed: "
                  + std::to_string(w.overwriteAllNames(PdbRowWriter::NameTable::Labels,
                                                        [](size_t index) { return "L" + std::to_string(index); }, &left)));
            check("playlists renamed: "
                  + std::to_string(w.overwriteAllNames(PdbRowWriter::NameTable::Playlists,
                                                        [](size_t index) { return "Q" + std::to_string(index); }, &left)));
            check("tags refused on export.pdb: "
                  + std::to_string(w.overwriteAllTagNames([](size_t index) { return "T" + std::to_string(index); }, &left)));
            for (int i = 100; i < 105; ++i) {
                add(trackNumber(i));  // their albums and labels by the old names: new rows
            }
            w.endEdit();
            check("an edit closed, as a save's change closes it");
            add(trackNumber(105));
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


    // 5. A slot at or past num_row_offsets whose present bit is set: the
    //    parser reads it as a row, and so does the index when it is built.
    //    An append takes the slot; the index forgets the row it held. T73's
    //    entries page holds 38 row offsets, the last group six of sixteen;
    //    slot 38 is made to show a deleted entry as present.
    {
        std::string bytes = baseBytes;
        const std::uint32_t page = u32(bytes, tableEntryOf(bytes, 8) + 12);
        const size_t base = LenPage * page;
        assert((u32(bytes, base + 24) & 0x1FFF) == 38);
        const size_t group = base + LenPage - 2 * 0x24;
        std::optional<std::uint32_t> deletedSlot;
        for (std::uint32_t r = 0; r < 38 && !deletedSlot; ++r) {
            if (((u16(bytes, base + LenPage - (r / 16) * 0x24 - 4) >> (r % 16)) & 1) == 0) {
                deletedSlot = r;
            }
        }
        assert(deletedSlot);
        const std::uint32_t heap = u16(bytes, base + LenPage - (*deletedSlot / 16) * 0x24 - 6 - 2 * (*deletedSlot % 16));
        const std::uint32_t strayPlaylist = u32(bytes, base + 40 + heap + 8);
        // Two tracks the playlist does not hold: the stray row is made to
        // name the first, which the parser then reads as in the playlist.
        std::vector<std::uint32_t> outside;
        {
            PdbRowWriter look(pathToUtf8(copyOfBase("stray-look.pdb")));
            const auto held = look.playlistTrackIds(strayPlaylist);
            for (const auto &row : look.trackPathRows()) {
                if (std::find(held.begin(), held.end(), row.id) == held.end()) {
                    outside.push_back(row.id);
                }
            }
        }
        assert(outside.size() >= 2);
        const std::uint32_t strayTrack = outside[0];
        const std::uint32_t other = outside[1];
        putU32(bytes, base + 40 + heap + 4, strayTrack);
        putU16(bytes, group - 6 - 2 * 6, heap);
        putU16(bytes, group - 4, u16(bytes, group - 4) | (1u << 6));
        const auto run = [&](bool indexed) {
            Opened o = openOn(scratch / (indexed ? "stray-now.pdb" : "stray-before.pdb"), bytes, indexed);
            std::vector<std::string> said;
            const auto step = [&](const std::string &what, const std::function<std::string()> &call) {
                said.push_back(what + ": " + outcomeOf(call));
                if (indexed) {
                    const std::string mismatch = o.writer->indexMismatchForTesting();
                    if (!mismatch.empty()) {
                        std::cerr << "after " << what << " the index differs from the buffer in: " << mismatch << "\n";
                    }
                    assert(mismatch.empty());
                }
            };
            step("other in", [&] { return std::to_string(o.writer->appendPlaylistEntries(strayPlaylist, {other})); });
            step("stray's track in",
                 [&] { return std::to_string(o.writer->appendPlaylistEntries(strayPlaylist, {strayTrack})); });
            step("both again", [&] {
                return std::to_string(o.writer->appendPlaylistEntries(strayPlaylist, {strayTrack, other}));
            });
            step("track", [&] { return std::to_string(o.writer->addTrack(trackNumber(1))); });
            assert(o.writer->commit());
            return std::make_pair(readFile(o.file), said);
        };
        const auto now = run(true);
        const auto before = run(false);
        for (size_t i = 0; i < now.second.size(); ++i) {
            if (now.second[i] != before.second[i]) {
                std::cerr << "  " << now.second[i] << " / " << before.second[i] << "\n";
            }
        }
        assert(now.second == before.second && "every call answered as without the index");
        assert(now.second[1] == "stray's track in: ok 1" && "the row the slot held is gone, so the track goes in");
        assert(now.first == before.first);
        std::cout << "case 5 (an append over a slot whose present bit was set: the index forgets the row it held, the "
                     "same answers and bytes as without the index) OK\n";
    }

    // 6. Strings the parser cannot read: an analyze_path, then a file_path.
    //    The walks the index replaced read a row's analyze_path only for a
    //    track with an analysis file and only until a row matched, and
    //    threw there; the indexed calls answer the same, in the same words.
    {
        const auto tracks = presentRowsOf(baseBytes, TracksTable);
        assert(tracks.size() >= 4);
        const auto pathOf = [&](size_t body) {
            const size_t at = trackStringAt(baseBytes, body, 20);
            const unsigned char kind = static_cast<unsigned char>(baseBytes[at]);
            assert(kind % 2 == 1 && "T73's paths are short ASCII strings");
            return baseBytes.substr(at + 1, kind / 2 - 1);
        };
        const std::string firstPath = pathOf(tracks[0]);
        const std::string lastPath = pathOf(tracks.back());
        // A long ASCII string of length 0: its text would be -4 bytes long.
        const auto unreadable = [&](int stringIndex) {
            std::string bytes = baseBytes;
            const size_t at = trackStringAt(bytes, tracks[1], stringIndex);
            bytes[at] = 0x40;
            putU16(bytes, at + 1, 0);
            return bytes;
        };
        const auto withAnalysis = [](PdbRowWriter::NewTrack t, const std::string &path) {
            t.pathOnStick = path;
            t.analyzePath = "/PIONEER/USBANLZ/P0FF/0000FFFF/ANLZ0000.DAT";
            return t;
        };
        PdbRowWriter::NewTrack plain = trackNumber(41);
        plain.analyzePath.clear();
        for (const int stringIndex : {14, 20}) {
            const std::string bytes = unreadable(stringIndex);
            const auto runAll = [&](bool indexed) {
                std::vector<std::string> said;
                const auto ask = [&](const std::string &what, const std::function<std::string(PdbRowWriter &)> &call) {
                    Opened o = openOn(scratch / (indexed ? "unreadable-now.pdb" : "unreadable-before.pdb"), bytes, indexed);
                    const std::string before = o.writer->bufferForTesting();
                    said.push_back(what + ": " + outcomeOf([&] { return call(*o.writer); }));
                    if (said.back().find(": ok") == std::string::npos) {
                        assert(o.writer->bufferForTesting() == before && "a refusal changes nothing");
                    } else if (o.writer->hasEdits()) {
                        assert(o.writer->commit());
                        said.back() += " " + std::to_string(std::hash<std::string>()(readFile(o.file)));
                    }
                };
                ask("no analysis file", [&](PdbRowWriter &w) { return std::to_string(w.addTrack(plain)); });
                ask("an analysis file", [&](PdbRowWriter &w) {
                    return std::to_string(w.addTrack(withAnalysis(trackNumber(43), "/Contents/New/43.mp3")));
                });
                ask("a file the first row names", [&](PdbRowWriter &w) {
                    return std::to_string(w.addTrack(withAnalysis(trackNumber(45), firstPath)));
                });
                ask("a file the last row names", [&](PdbRowWriter &w) {
                    return std::to_string(w.addTrack(withAnalysis(trackNumber(47), lastPath)));
                });
                ask("the rows of a file", [&](PdbRowWriter &w) {
                    return std::to_string(w.trackIdsWithFilePath(lastPath).size());
                });
                return said;
            };
            const auto now = runAll(true);
            const auto before = runAll(false);
            for (size_t i = 0; i < now.size(); ++i) {
                std::cerr << "  string " << stringIndex << ": " << now[i] << (now[i] == before[i] ? "" : "  /  WITHOUT THE INDEX: " + before[i]) << "\n";
            }
            assert(now == before && "every call answered as without the index, in the same words");
            if (stringIndex == 14) {
                assert(now[0].find("no analysis file: ok") == 0 && "a track without an analysis file never reads one");
                assert(now[2].find("already names") != std::string::npos && "a row before the unreadable one matched");
                assert(now[4].find("the rows of a file: ok 1") == 0);
            } else {
                assert(now[2].find("already names") != std::string::npos);
                assert(now[4].find("the rows of a file: ok") == std::string::npos);
            }
            assert(now[1].find(": ok") == std::string::npos && now[3].find(": ok") == std::string::npos);
        }
        std::cout << "case 6 (an analyze_path, then a file_path, the parser cannot read: every call answers as the walks "
                     "did, thrown or not, in the same words) OK\n";
    }

    // 7. Album rows of one name and album artist with ids 3, 0 and 5 in
    //    every order: the album a new track takes is the one the walk took,
    //    a row of id 0 included (3, 0, 5 gives 5; 3, 5, 0 gives a new one).
    {
        const fs::path made = copyOfBase("albums-base.pdb");
        {
            PdbRowWriter w(pathToUtf8(made));
            for (int i = 0; i < 3; ++i) {
                PdbRowWriter::NewTrack t = trackNumber(700 + i);
                t.album = "XA" + std::to_string(i + 1);
                t.albumArtist.clear();
                w.addTrack(t);
            }
            assert(w.commit());
        }
        const std::string albumBase = readFile(made);
        // The three rows in table order, each by its own name, which
        // follows its 22 bytes of fixed fields.
        std::vector<size_t> bodies;
        for (const size_t body : presentRowsOf(albumBase, AlbumsTable)) {
            const size_t name = albumBase.find("XA", body);
            if (name < body + 32) {
                assert(albumBase[name + 2] == static_cast<char>('1' + bodies.size()));
                bodies.push_back(body);
            }
        }
        assert(bodies.size() == 3);
        std::vector<std::uint32_t> ids{0, 3, 5};
        int orders = 0;
        do {
            std::string bytes = albumBase;
            for (size_t i = 0; i < 3; ++i) {
                putU32(bytes, bodies[i] + 12, ids[i]);
                const size_t name = bytes.find("XA" + std::to_string(i + 1), bodies[i]);
                bytes[name + 2] = 'X';
            }
            std::uint32_t walkTakes = 0;
            for (const std::uint32_t id : ids) {
                walkTakes = walkTakes == 0 || id < walkTakes ? id : walkTakes;
            }
            std::vector<std::string> files;
            for (const bool indexed : {true, false}) {
                Opened o = openOn(scratch / (indexed ? "albums-now.pdb" : "albums-before.pdb"), bytes, indexed);
                PdbRowWriter::NewTrack t = trackNumber(710);
                t.album = "XAX";
                t.albumArtist.clear();
                const std::uint32_t id = o.writer->addTrack(t);
                if (indexed) {
                    assert(o.writer->indexMismatchForTesting().empty());
                }
                assert(o.writer->commit());
                files.push_back(readFile(o.file));
                const std::string &after = files.back();
                std::uint32_t albumOfTrack = 0;
                for (const size_t body : presentRowsOf(after, TracksTable)) {
                    if (u32(after, body + 72) == id) {
                        albumOfTrack = u32(after, body + 64);
                    }
                }
                if (walkTakes != 0) {
                    assert(albumOfTrack == walkTakes && "the album the walk took");
                } else {
                    assert(albumOfTrack > 5 && "a new album, as the walk made one");
                }
            }
            assert(files[0] == files[1]);
            ++orders;
        } while (std::next_permutation(ids.begin(), ids.end()));
        assert(orders == 6);
        std::cout << "case 7 (album ids 3, 0 and 5 in all six orders: the album the walk took, the same bytes with the "
                     "index as without) OK\n";
    }

    // 8. What commit() reads back of the tables rows were added to: a row
    //    its table's pages do not reach, a chain that comes back on itself,
    //    a table whose rows cannot be read. Each made of the buffer as a bug
    //    in the writer could leave it; T73 has no genre rows, so the track's
    //    genre starts the table's first page.
    {
        const auto withGenre = [&](const std::string &name) {
            Opened o = openOn(scratch / "readback.pdb", baseBytes, true);
            PdbRowWriter::NewTrack t = trackNumber(800);
            t.genre = name;
            o.writer->addTrack(t);
            return o;
        };
        {
            Opened o = withGenre("Reached");
            std::string &buffer = o.writer->bufferForTesting();
            const size_t entry = tableEntryOf(buffer, GenresTable);
            assert(u32(buffer, entry + 8) != u32(buffer, entry + 12) && "the genre is on a page of its own");
            putU32(buffer, entry + 12, u32(buffer, entry + 8));  // the last page: the index page
            assert(!o.writer->commit());
            std::cerr << "  " << o.writer->commitRefusal() << "\n";
            assert(o.writer->commitRefusal().find("genres table") != std::string::npos
                   && o.writer->commitRefusal().find("is not reached through the table's pages") != std::string::npos);
            assert(readFile(o.file) == baseBytes && "nothing written");
        }
        {
            Opened o = withGenre("Cycled");
            std::string &buffer = o.writer->bufferForTesting();
            const size_t entry = tableEntryOf(buffer, GenresTable);
            const std::uint32_t last = u32(buffer, entry + 12);
            putU32(buffer, LenPage * last + 12, u32(buffer, entry + 8));  // the new page leads back to the first
            putU32(buffer, entry + 12, u32(buffer, entry + 4));           // and the last page is the candidate
            assert(!o.writer->commit());
            std::cerr << "  " << o.writer->commitRefusal() << "\n";
            assert(o.writer->commitRefusal().find("genres table") != std::string::npos
                   && o.writer->commitRefusal().find("does not read back") != std::string::npos);
        }
        // A genre row whose name cannot be read, in a table the save added
        // to, and the same table untouched by a track that names no genre:
        // the first is refused, the second written (the table reads as it
        // did before the save).
        {
            Opened o = withGenre("First");
            assert(o.writer->commit());
            std::string bytes = readFile(o.file);
            const auto genres = presentRowsOf(bytes, GenresTable);
            assert(genres.size() == 1);
            bytes[genres.front() + 4] = 0x40;
            putU16(bytes, genres.front() + 5, 0);
            for (const bool indexed : {true, false}) {
                Opened again = openOn(scratch / "unreadable-genre.pdb", bytes, false);
                again.writer->setIndexedForTesting(indexed);
                PdbRowWriter::NewTrack named = trackNumber(801);
                named.genre = "Second";
                const std::string said = outcomeOf([&] { return std::to_string(again.writer->addTrack(named)); });
                assert(said.find("ok") != 0 && "a track naming a genre: the genre table is read, and refused");
                PdbRowWriter::NewTrack none = trackNumber(802);
                none.genre.clear();
                const std::string kept = outcomeOf([&] { return std::to_string(again.writer->addTrack(none)); });
                if (indexed) {
                    assert(kept.find("ok") == 0 && "a track naming no genre does not read the genre table");
                    assert(again.writer->commit() && "nor does the commit, which added none");
                } else {
                    // The walk before the index read every lookup table back
                    // for every track.
                    assert(kept.find("ok") != 0);
                }
            }
        }
        // The read-back of the track itself: two genre rows of one id, the
        // name asked for first. The id found by the name leads to the other.
        {
            Opened o = openOn(scratch / "dup-genre.pdb", baseBytes, true);
            for (const char *name : {"Dup One", "Dup Two"}) {
                PdbRowWriter::NewTrack t = trackNumber(name[4] == 'O' ? 810 : 811);
                t.genre = name;
                o.writer->addTrack(t);
            }
            assert(o.writer->commit());
            std::string bytes = readFile(o.file);
            const auto genres = presentRowsOf(bytes, GenresTable);
            assert(genres.size() == 2);
            putU32(bytes, genres[1], u32(bytes, genres[0]));
            std::vector<std::string> said;
            for (const bool indexed : {true, false}) {
                Opened again = openOn(scratch / (indexed ? "dup-genre-now.pdb" : "dup-genre-before.pdb"), bytes, indexed);
                const std::string before = again.writer->bufferForTesting();
                PdbRowWriter::NewTrack t = trackNumber(812);
                t.genre = "Dup One";
                said.push_back(outcomeOf([&] { return std::to_string(again.writer->addTrack(t)); }));
                assert(again.writer->bufferForTesting() == before && !again.writer->hasEdits());
            }
            std::cerr << "  " << said[0] << "\n";
            assert(said[0] == said[1] && said[0].find("does not read back as written; not kept") != std::string::npos);
        }
        std::cout << "case 8 (commit() refuses a row its table's pages do not reach and a table that does not read "
                     "back; a track's own read-back refuses a name that leads elsewhere, as before the index) OK\n";
    }

    // 9. A table's entry in the header across two pages: pages of 256
    //    bytes and fifteen tables, the artwork table's entry the fifteenth
    //    (bytes 252 to 267). A track whose artwork row starts a page and
    //    whose own row then fits no page is refused; the buffer is the bytes
    //    it was, both pages of the entry put back.
    {
        constexpr size_t Small = 256;
        const std::vector<std::uint32_t> types{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 13};
        std::string bytes((2 + types.size()) * Small, '\0');
        putU32(bytes, 4, Small);
        putU32(bytes, 8, static_cast<std::uint32_t>(types.size()));
        const std::uint32_t candidate = static_cast<std::uint32_t>(2 + types.size());
        putU32(bytes, 12, candidate + 1);
        putU32(bytes, 20, 7);
        for (size_t t = 0; t < types.size(); ++t) {
            const std::uint32_t page = static_cast<std::uint32_t>(2 + t);
            const size_t entry = 28 + 16 * t;
            const bool artwork = types[t] == 13;
            putU32(bytes, entry, types[t]);
            putU32(bytes, entry + 4, artwork ? candidate : candidate + 1);
            putU32(bytes, entry + 8, page);
            putU32(bytes, entry + 12, page);
            const size_t base = Small * page;
            putU32(bytes, base + 4, page);
            putU32(bytes, base + 8, types[t]);
            putU32(bytes, base + 12, artwork ? candidate : candidate + 1);
            bytes[base + 27] = 0x24;
            // The artwork page is full; the others empty.
            putU16(bytes, base + 28, artwork ? 0 : Small - 40);
            putU16(bytes, base + 30, artwork ? Small - 40 : 0);
        }
        assert(28 + 16 * 14 < Small && 28 + 16 * 15 > Small && "the fifteenth entry crosses into the second page");
        PdbRowWriter::NewTrack t;
        t.pathOnStick = "/Contents/Small/track.mp3";
        t.title = "Small";
        t.date = "2026-11-26";
        t.artworkPath = PdbRowWriter::artworkPathFor('a', 1);
        for (const bool indexed : {true, false}) {
            Opened o = openOn(scratch / "small-pages.pdb", bytes, indexed);
            const std::string said = outcomeOf([&] { return std::to_string(o.writer->addTrack(t)); });
            std::cerr << "  " << (indexed ? "indexed: " : "without the index: ") << said << "\n";
            assert(said.find("does not fit on a page") != std::string::npos);
            if (o.writer->bufferForTesting() != bytes) {
                for (size_t i = 0; i < bytes.size(); ++i) {
                    if (o.writer->bufferForTesting()[i] != bytes[i]) {
                        std::cerr << "  byte " << i << " (page " << i / Small << ") not put back\n";
                    }
                }
            }
            assert(o.writer->bufferForTesting() == bytes && !o.writer->hasEdits());
        }
        std::cout << "case 9 (a table entry across two small pages: a refused track puts back both) OK\n";
    }

    // 10. setTrackTempoAndKey() refused: the key row the track needs goes
    //     on a page that does not add up. Refused alike with and without
    //     the index, the buffer the bytes it was.
    {
        std::string bytes = baseBytes;
        const std::uint32_t keysLast = u32(bytes, tableEntryOf(bytes, KeysTable) + 12);
        putU16(bytes, LenPage * keysLast + 28, u16(bytes, LenPage * keysLast + 28) + 4);
        // A track without a key: T73's first, its key_id cleared.
        const size_t firstTrack = presentRowsOf(bytes, TracksTable).front();
        putU32(bytes, firstTrack + 32, 0);
        const std::uint32_t keyless = u32(bytes, firstTrack + 72);
        std::vector<std::string> said;
        for (const bool indexed : {true, false}) {
            Opened o = openOn(scratch / "tempo-key.pdb", bytes, indexed);
            bool keyWritten = true;
            said.push_back(outcomeOf([&] {
                return std::to_string(o.writer->setTrackTempoAndKey(keyless, 128.0, "A key nobody has", &keyWritten));
            }));
            assert(!keyWritten && o.writer->bufferForTesting() == bytes && !o.writer->hasEdits());
            // And a tempo alone, which needs no row, goes in alike.
            said.push_back(outcomeOf([&] { return std::to_string(o.writer->setTrackTempoAndKey(keyless, 99.0, "")); }));
            assert(o.writer->commit());
            said.push_back(std::to_string(std::hash<std::string>()(readFile(o.file))));
        }
        std::cerr << "  " << said[0] << "\n";
        assert(said[0].find("does not add up") != std::string::npos);
        assert(said[0] == said[3] && said[1] == said[4] && said[2] == said[5]);
        std::cout << "case 10 (a tempo and key refused: alike with and without the index, nothing changed) OK\n";
    }

    // 11. A real export, anonymized (tests/fixtures/anonymized_library):
    //     genre, label, key, artwork, artist and album rows there already
    //     (tracks naming them take them), history rows, 546 deleted track
    //     rows, paths and names in rekordbox's UTF-16 form. Tracks naming
    //     what is there and what is not, and refusals (a file named already,
    //     as spelled and in other letters, an analysis file named already):
    //     the same answers and bytes with the index as without.
    {
        const std::string real = readFile(anonymized / "rekordbox" / "rekordbox" / "export.pdb");
        const auto genres = namesIn(real, Pdb::PAGE_TYPE_GENRES);
        const auto labels = namesIn(real, Pdb::PAGE_TYPE_LABELS);
        const auto keys = namesIn(real, Pdb::PAGE_TYPE_KEYS);
        const auto artwork = namesIn(real, Pdb::PAGE_TYPE_ARTWORK);
        const auto artists = namesIn(real, Pdb::PAGE_TYPE_ARTISTS);
        const auto albums = namesIn(real, Pdb::PAGE_TYPE_ALBUMS);
        const auto rows = namesIn(real, Pdb::PAGE_TYPE_TRACKS);
        assert(genres.size() >= 4 && labels.size() >= 10 && artwork.size() >= 1000 && keys.size() >= 20);
        assert(!presentRowsOf(real, 12).empty() && "history rows");
        // Paths in UTF-16, and one with an analysis file, to name again.
        std::string utf16Path;
        std::string analysed;
        const auto trackBodies = presentRowsOf(real, TracksTable);
        assert(trackBodies.size() == rows.size());
        for (size_t k = 0; k < rows.size(); ++k) {
            const std::string path = rows[k].substr(0, rows[k].find('\t'));
            const std::string analysis = rows[k].substr(rows[k].find('\t') + 1);
            if (static_cast<unsigned char>(real[trackStringAt(real, trackBodies[k], 20)]) == 0x90 && utf16Path.empty()) {
                utf16Path = path;
            }
            if (!analysis.empty() && analysed.empty()) {
                analysed = analysis;
            }
        }
        assert(!utf16Path.empty() && !analysed.empty());
        const auto pick = [](const std::vector<std::string> &names, int i) {
            std::vector<std::string> usable;
            for (const auto &n : names) {
                if (!n.empty() && n.back() != ' ' && n.find('\0') == std::string::npos) {
                    usable.push_back(n);
                }
            }
            return usable.empty() ? std::string() : usable[static_cast<size_t>(i) % usable.size()];
        };
        std::vector<PdbRowWriter::NewTrack> attempts;
        std::vector<bool> refused;
        for (int i = 0; i < 150; ++i) {
            PdbRowWriter::NewTrack t = trackNumber(5000 + i);
            if (i % 2 == 0) {
                t.genre = pick(genres, i);
                t.label = pick(labels, i);
                t.key = pick(keys, i);
                t.artist = pick(artists, i);
            }
            if (i % 3 == 0) {
                t.album = pick(albums, i);
                t.albumArtist.clear();
            }
            if (i % 4 == 1) {
                t.artworkPath = pick(artwork, i);
            }
            attempts.push_back(t);
            refused.push_back(false);
            if (i % 25 == 3) {
                PdbRowWriter::NewTrack again = trackNumber(6000 + i);
                again.pathOnStick = utf16Path;
                again.analyzePath.clear();
                attempts.push_back(again);
                refused.push_back(true);
                PdbRowWriter::NewTrack cased = again;
                for (char &c : cased.pathOnStick) {
                    c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
                }
                attempts.push_back(cased);
                refused.push_back(true);
                PdbRowWriter::NewTrack sharing = trackNumber(7000 + i);
                sharing.analyzePath = analysed;
                attempts.push_back(sharing);
                refused.push_back(true);
            }
        }
        const fs::path realBase = scratch / "real-base.pdb";
        std::uint32_t realPlaylist = 0;
        {
            writeFile(realBase, real);
            PdbRowWriter w(pathToUtf8(realBase));
            realPlaylist = w.createPlaylist(0, "Index Test", false);
            assert(w.commit());
        }
        const std::string realBytes = readFile(realBase);
        const auto run = [&](const std::string &name, const Run &how) {
            const fs::path file = scratch / name;
            writeFile(file, realBytes);
            return addAll(file, attempts, refused, how, realPlaylist);
        };
        const Outcome now = run("real-now.pdb", {{attempts.size()}, true, true});
        const Outcome before = run("real-before.pdb", {{attempts.size()}, false, false});
        assert(now.refusals == before.refusals && now.refusals.size() == 18);
        assert(now.bytes == before.bytes);
        // The genres and labels there already were taken, not made again.
        for (const auto &[type, there] : {std::make_pair(Pdb::PAGE_TYPE_GENRES, genres), std::make_pair(Pdb::PAGE_TYPE_LABELS, labels)}) {
            std::set<std::string> made;
            for (size_t i = 0; i < attempts.size(); ++i) {
                const std::string &name = type == Pdb::PAGE_TYPE_GENRES ? attempts[i].genre : attempts[i].label;
                if (!refused[i] && !name.empty() && std::find(there.begin(), there.end(), name) == there.end()) {
                    made.insert(name);
                }
            }
            assert(namesIn(now.bytes, type).size() == there.size() + made.size());
        }
        std::cout << "case 11 (a real export with every lookup table filled, history rows, deleted rows and UTF-16 "
                     "strings: the same answers and bytes with the index as without) OK\n";
    }

    // 12. A table whose pages lead back to one already passed: refused in
    //     plain words rather than walked for ever, with and without the
    //     index. T73's tracks table has three data pages; the third leads
    //     back to the first.
    {
        std::string bytes = baseBytes;
        const size_t entry = tableEntryOf(bytes, TracksTable);
        std::vector<std::uint32_t> chain;
        for (std::uint32_t page = u32(bytes, entry + 8);; page = u32(bytes, LenPage * page + 12)) {
            chain.push_back(page);
            if (page == u32(bytes, entry + 12)) {
                break;
            }
        }
        assert(chain.size() >= 3);
        putU32(bytes, LenPage * chain[chain.size() - 2] + 12, chain[1]);
        for (const bool indexed : {true, false}) {
            Opened o = openOn(scratch / "cycle.pdb", bytes, false);
            o.writer->setIndexedForTesting(indexed);
            const std::string said = outcomeOf([&] { return std::to_string(o.writer->trackIdsWithFilePath("/x").size()); });
            const std::string added = outcomeOf([&] { return std::to_string(o.writer->addTrack(trackNumber(900))); });
            std::cerr << "  " << said << "\n";
            assert(said.find("lead back to page") != std::string::npos && added.find("lead back to page") != std::string::npos);
        }
        std::cout << "case 12 (a page chain that comes back on itself: refused in plain words) OK\n";
    }

    std::cout << "pdb_row_index_test: all cases passed\n";
    return 0;
}
