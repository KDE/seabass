// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// TrackAnalysisCache: a rekordbox row's analysis kept on the computer, so
// a row seen before costs no read of the stick, in this run or the next,
// until its analysis files change. On a copy of the T73 fixture (a stick
// as rekordbox 7.2 wrote it), with a .2EX built by hand for the 3Band case.

#include <QCoreApplication>
#include <QFile>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/waveform.hpp"
#include "gui/track_analysis_cache.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::gui::TrackAnalysisCache;
using seabass::domain::WaveformLook;

namespace
{

void putU32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

std::string section(const std::string &fourcc, const std::vector<std::uint32_t> &fields, const std::string &body)
{
    std::string out = fourcc;
    const auto headerLength = static_cast<std::uint32_t>(12 + 4 * fields.size());
    putU32(out, headerLength);
    putU32(out, headerLength + static_cast<std::uint32_t>(body.size()));
    for (auto f : fields) {
        putU32(out, f);
    }
    return out + body;
}

// A .2EX with a PWV7 and a PWV6 of 1,200 entries, the PWVC after it.
std::string twoExFile()
{
    std::string pwv6;
    for (int i = 0; i < 1200; ++i) {
        pwv6 += std::string{static_cast<char>(100), static_cast<char>(50), static_cast<char>(25)};
    }
    std::string body = section("PWV7", {3, 300, 0x960000}, std::string(900, '\x10')) + section("PWV6", {3, 1200}, pwv6)
                       + section("PWVC", {0, 0}, "");
    std::string out = "PMAI";
    putU32(out, 28);
    putU32(out, static_cast<std::uint32_t>(28 + body.size()));
    out += std::string(16, '\0');
    return out + body;
}

bool sameAnalysis(const seabass::domain::TrackAnalysis &a, const seabass::domain::TrackAnalysis &b)
{
    if (a.waveformLook != b.waveformLook || a.beatRateRecorded != b.beatRateRecorded
        || a.waveform.size() != b.waveform.size() || a.beats.size() != b.beats.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.waveform.size(); ++i) {
        const auto close = [](double x, double y) { return std::abs(x - y) <= 0.5 / 255.0 + 1e-9; };
        if (!close(a.waveform[i].low, b.waveform[i].low) || !close(a.waveform[i].mid, b.waveform[i].mid)
            || !close(a.waveform[i].high, b.waveform[i].high)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.beats.size(); ++i) {
        if (a.beats[i].timeMs != b.beats[i].timeMs || a.beats[i].beatInBar != b.beats[i].beatInBar) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: track_analysis_cache_test <tests/fixtures/testshark_t73/stick/PIONEER>\n";
        return 2;
    }
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_track_analysis_cache_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path pioneer = scratch / "PIONEER";
    std::error_code ec;
    seabass::testing::copyPioneerFixture(seabass::pathFromUtf8(argv[1]), pioneer, ec);
    assert(!ec);
    const std::string root = seabass::pathToUtf8(pioneer);
    const QString dbFile = QString::fromStdString(seabass::pathToUtf8(scratch / "analysis.db"));

    // ---- 1. the kept form ----------------------------------------------
    {
        seabass::domain::TrackAnalysis analysis;
        analysis.waveformLook = WaveformLook::RekordboxThreeBand;
        analysis.beatRateRecorded = false;
        analysis.waveform = {{1.0, 0.5, 0.0}, {0.2, 0.1, 0.05}};
        analysis.beats = {{12.5, 1}, {512.25, 2}, {1012.0, 0}};
        const std::string bytes = TrackAnalysisCache::serialise(analysis);
        const auto back = TrackAnalysisCache::deserialise(bytes);
        assert(back && sameAnalysis(*back, analysis));
        assert(!TrackAnalysisCache::deserialise(bytes.substr(0, bytes.size() - 1)) && "cut short");
        assert(!TrackAnalysisCache::deserialise(bytes + "x") && "too long");
        std::string otherVersion = bytes;
        otherVersion[0] = 99;
        assert(!TrackAnalysisCache::deserialise(otherVersion) && "another build's");
        assert(!TrackAnalysisCache::deserialise(""));
        std::cout << "case 1 (the kept form reads back; a cut, long or foreign one does not) OK\n";
    }

    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(root);
    const auto tracks = reader.readAll();
    const seabass::infrastructure::rekordbox::AnlzPathIndex index(root);
    std::string id;
    std::string analyzePath;
    for (const auto &track : tracks) {
        if (auto path = index.pathFor(static_cast<std::uint32_t>(std::stoul(track.sourceId)))) {
            id = track.sourceId;
            analyzePath = *path;
            break;
        }
    }
    assert(!id.empty() && "a fixture track with an analysis file");
    const fs::path dat = pioneer / seabass::pathFromUtf8(seabass::infrastructure::rekordbox::anlzRelativePath(analyzePath, false));
    const fs::path twoEx = pioneer / seabass::pathFromUtf8(
                                         seabass::infrastructure::rekordbox::anlzSiblingRelativePath(analyzePath, ".2EX"));
    const auto fromStick = seabass::infrastructure::rekordbox::readTrackAnalysis(root, id, nullptr, &index, true);
    assert(!fromStick.waveform.empty() && !fromStick.beats.empty());

    // ---- 2. read once, then kept, across runs --------------------------
    {
        TrackAnalysisCache cache(dbFile);
        assert(cache.persistent());
        const auto first = cache.rekordboxAnalysis(root, id, &index);
        assert(sameAnalysis(first, fromStick) && first.waveformLook == WaveformLook::RekordboxBlue);
        assert(cache.counters().reads == 1 && cache.counters().stores == 1 && cache.counters().hits == 0);
        const auto second = cache.rekordboxAnalysis(root, id, &index);
        assert(sameAnalysis(second, fromStick));
        assert(cache.counters().reads == 1 && cache.counters().hits == 1 && "the second ask reads nothing");
    }
    {
        TrackAnalysisCache nextRun(dbFile);
        const auto kept = nextRun.rekordboxAnalysis(root, id, &index);
        assert(sameAnalysis(kept, fromStick));
        assert(nextRun.counters().reads == 0 && nextRun.counters().hits == 1 && "kept between runs");
        std::cout << "case 2 (read from the stick once, then from the cache, in the next run too) OK\n";
    }

    // ---- 3. a changed analysis file is read again ----------------------
    {
        TrackAnalysisCache cache(dbFile);
        // A .2EX appears (rekordbox exported again): the 3Band overview.
        std::ofstream(twoEx, std::ios::binary) << twoExFile();
        const auto withTwoEx = cache.rekordboxAnalysis(root, id, &index);
        assert(cache.counters().reads == 1 && "a new .2EX is a new key");
        assert(withTwoEx.waveformLook == WaveformLook::RekordboxThreeBand && withTwoEx.waveform.size() == 1200);
        // The .DAT rewritten (its time moved on): read again.
        // A copy first: the fixture's files are hard links, and moving
        // a link's time moves the committed file's.
        {
            const auto when = fs::last_write_time(dat);
            fs::path copy = dat;
            copy += ".copy";
            fs::copy_file(dat, copy);
            fs::rename(copy, dat);
            fs::last_write_time(dat, when + std::chrono::seconds(10));
        }
        const auto rewritten = cache.rekordboxAnalysis(root, id, &index);
        assert(cache.counters().reads == 2 && "a changed .DAT is a new key");
        assert(sameAnalysis(rewritten, withTwoEx));
        assert(cache.rekordboxAnalysis(root, id, &index).waveformLook == WaveformLook::RekordboxThreeBand);
        assert(cache.counters().reads == 2 && cache.counters().hits == 1);
        std::cout << "case 3 (a .2EX added or a .DAT rewritten is read again) OK\n";
    }

    // ---- 4. a damaged database is made again ---------------------------
    {
        {
            QFile damage(dbFile);
            assert(damage.open(QIODevice::WriteOnly | QIODevice::Truncate));
            damage.write(QByteArray(4096, 'x'));
        }
        QFile::remove(dbFile + QStringLiteral("-wal"));
        QFile::remove(dbFile + QStringLiteral("-shm"));
        TrackAnalysisCache cache(dbFile);
        assert(cache.persistent() && cache.counters().rebuilds == 1);
        const auto analysis = cache.rekordboxAnalysis(root, id, &index);
        assert(analysis.waveformLook == WaveformLook::RekordboxThreeBand && cache.counters().reads == 1);
        std::cout << "case 4 (a damaged database is made again, and the stick read) OK\n";
    }

    // ---- 5. no database at all still reads ------------------------------
    {
        TrackAnalysisCache none(QString{});
        assert(!none.persistent());
        assert(sameAnalysis(none.rekordboxAnalysis(root, id, &index),
                            seabass::infrastructure::rekordbox::readTrackAnalysis(root, id, nullptr, &index, true)));
        assert(none.rekordboxAnalysis(root, "not a number", &index).waveform.empty());
        std::cout << "case 5 (without a database the stick is read as before) OK\n";
    }

    // ---- 6. Seabass's own rewrite of the same size, in the same step ----
    // FAT keeps a file's time to two seconds: Analyse again rewriting a
    // grid in place within one step of the last write leaves the size and
    // the time as they were. Simulated by setting the time back by hand
    // after each rewrite. Each way an analysis file is written in place:
    // AnlzFile::writeRaw() (Analyse again, Restore original analysis, the
    // cue writers), writeFileDurablyAtomic() (the import's built files),
    // copyFileDurablyAtomic() (a failed save putting its copy back).
    {
        namespace rb = seabass::infrastructure::rekordbox;
        const std::string datPath = seabass::pathToUtf8(dat);
        // The first beat's time moved by `shiftMs`, nothing else: the same
        // size. PQTZ: a 24-byte header, then eight bytes a beat, the time
        // (u32, big-endian) in the last four.
        const auto shiftedGrid = [&](std::uint32_t shiftMs) {
            rb::AnlzFile file = rb::AnlzFile::readRaw(datPath);
            bool shifted = false;
            for (auto &section : file.sections) {
                if (section.rawBytes.compare(0, 4, "PQTZ") == 0 && section.rawBytes.size() >= 32) {
                    std::string &b = section.rawBytes;
                    std::uint32_t time = 0;
                    for (int i = 0; i < 4; ++i) {
                        time = (time << 8) | static_cast<unsigned char>(b[28 + i]);
                    }
                    time += shiftMs;
                    for (int i = 0; i < 4; ++i) {
                        b[28 + i] = static_cast<char>((time >> (8 * (3 - i))) & 0xFF);
                    }
                    shifted = true;
                }
            }
            assert(shifted && "the fixture's .DAT has a grid");
            return file;
        };
        const auto stamp = [&] { return std::make_pair(fs::file_size(dat), fs::last_write_time(dat)); };
        const auto putTimeBack = [&](const std::pair<std::uintmax_t, fs::file_time_type> &before) {
            fs::last_write_time(dat, before.second);
            assert(stamp() == before && "the same size and time as before the rewrite");
        };

        TrackAnalysisCache cache(dbFile);
        const auto before = cache.rekordboxAnalysis(root, id, &index);
        assert(cache.rekordboxAnalysis(root, id, &index).beats.front().timeMs == before.beats.front().timeMs);
        const int readsBefore = cache.counters().reads;
        assert(cache.counters().hits >= 1 && "kept");

        // writeRaw(), as Analyse again writes the grid.
        {
            const auto was = stamp();
            const rb::AnlzFile moved = shiftedGrid(5);
            moved.writeRaw(datPath);
            putTimeBack(was);
            const auto after = cache.rekordboxAnalysis(root, id, &index);
            assert(cache.counters().reads == readsBefore + 1 && "read again");
            assert(after.beats.front().timeMs == before.beats.front().timeMs + 5 && "the new grid, not the kept one");
        }
        // writeFileDurablyAtomic(), as the import writes its built files.
        {
            const auto was = stamp();
            assert(seabass::infrastructure::writeFileDurablyAtomic(datPath, shiftedGrid(5).toBytes()));
            putTimeBack(was);
            const auto after = cache.rekordboxAnalysis(root, id, &index);
            assert(after.beats.front().timeMs == before.beats.front().timeMs + 10);
        }
        // copyFileDurablyAtomic(), as a rollback puts the copy it took back.
        {
            const fs::path copy = scratch / "taken-before.DAT";
            fs::copy_file(dat, copy);
            assert(seabass::infrastructure::writeFileDurablyAtomic(datPath, shiftedGrid(5).toBytes()));
            const auto was = stamp();
            assert(cache.rekordboxAnalysis(root, id, &index).beats.front().timeMs == before.beats.front().timeMs + 15);
            assert(seabass::infrastructure::copyFileDurablyAtomic(seabass::pathToUtf8(copy), datPath));
            putTimeBack(was);
            const auto after = cache.rekordboxAnalysis(root, id, &index);
            assert(after.beats.front().timeMs == before.beats.front().timeMs + 10 && "the grid put back");
        }
        // A cache made after the write (the app's is made at its first
        // use) forgets it too: its entry was kept by a cache before it.
        {
            TrackAnalysisCache earlier(dbFile);
            const auto kept = earlier.rekordboxAnalysis(root, id, &index);
            const auto was = stamp();
            assert(seabass::infrastructure::writeFileDurablyAtomic(datPath, shiftedGrid(5).toBytes()));
            putTimeBack(was);
            TrackAnalysisCache later(dbFile);
            const auto after = later.rekordboxAnalysis(root, id, &index);
            assert(later.counters().reads == 1 && later.counters().hits == 0);
            assert(after.beats.front().timeMs == kept.beats.front().timeMs + 5);
        }
        std::cout << "case 6 (a rewrite of the same size and time by any of Seabass's writers is read again) OK\n";
    }

    fs::remove_all(scratch);
    std::cout << "track_analysis_cache_test: all OK\n";
    return 0;
}
