// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone benchmark: where does a staged Save actually spend its time on
// a real USB stick, and what would the three candidate write strategies cost
// instead?
//
// Removing 201 stray cues on a real 29 GB exFAT stick took 155 s, while
// reading every cue file it touched takes well under a second. This tool
// separates the two halves of that gap:
//
//   Part A  per-item overhead that is pure repeated work (read-only): the
//           export.pdb re-parse per item, the SQLCipher open + passphrase
//           key derivation per item, the whole-file CRC32 per item.
//   Part B  the durability strategy, i.e. what the writes themselves cost
//           under each of the three candidate designs.
//
// SAFETY: this tool never writes to the real library. Everything it writes
// goes under <stick>/.seabass-writebench/, which it creates and removes.
// PIONEER, Engine Library and .seabass-backups are only ever read.
//
// Build (from the repo root, after a normal configure):
//   cmake --build build --target stick_write_bench
// Run:
//   build/stick_write_bench /media/you/STICK 10 50 100 200
//   build/stick_write_bench /media/you/STICK --device /dev/sdX1 10 50 100 200
//
// The counts are how many stray cues to simulate. Stray cues are almost
// always one per track, so N cues means N distinct .EXT files.
//
// With --device, the stick is unmounted and remounted before every timed
// run and then left idle to settle. That is worth the wall-clock: it gives
// each run a cold page cache and a freshly mounted filesystem, and the
// idle gap lets the stick's own controller finish the garbage collection
// it would otherwise do in the middle of the next measurement. Without
// it, run-to-run spread of three to seven times is normal.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <zlib.h>

#include <kaitai/kaitaistream.h>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace fs = std::filesystem;
namespace rb = seabass::infrastructure::rekordbox;
namespace ol = seabass::infrastructure::onelibrary;
using Pdb = rekordbox_pdb_t;
using Clock = std::chrono::steady_clock;

namespace
{

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A plain write with no fsync: what options 2 and 3 do per file, leaving
// durability to one filesystem sync later.
bool writePlain(const fs::path &path, const std::string &data)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const char *p = data.data();
    size_t remaining = data.size();
    bool ok = true;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n <= 0) {
            ok = false;
            break;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);
    return ok;
}

// One flush of everything dirty on the stick's filesystem. Linux-only;
// Windows has no user-mode equivalent (see the plan's platform notes).
int g_syncCalls = 0;
void syncFilesystemAt(const fs::path &anyPathOnIt)
{
    ++g_syncCalls;
#if defined(__linux__)
    int fd = ::open(anyPathOnIt.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::syncfs(fd);
        ::close(fd);
        return;
    }
#endif
    ::sync();
}

// Unmount and remount, then wait. Everything here is outside the timer.
// Returns false if the stick did not come back where it was.
bool remountAndSettle(const std::string &device, const fs::path &mountPoint, int settleSeconds)
{
    if (device.empty()) {
        return true;
    }
    const std::string quiet = " >/dev/null 2>&1";
    std::string unmount = "udisksctl unmount -b " + device + quiet;
    std::string mount = "udisksctl mount -b " + device + quiet;
    if (std::system(unmount.c_str()) != 0) {
        std::cerr << "    (unmount of " << device << " failed; is something holding the stick open?)\n";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::system(mount.c_str());
    for (int waited = 0; waited < 20 && !fs::is_directory(mountPoint); ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!fs::is_directory(mountPoint)) {
        std::cerr << "    (stick did not come back at " << mountPoint << ")\n";
        return false;
    }
    // Idle, so the controller's own background work happens now rather
    // than inside the next measurement.
    std::this_thread::sleep_for(std::chrono::seconds(settleSeconds));
    return true;
}

void dropCacheFor(const fs::path &path)
{
#if defined(__linux__)
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

std::uint32_t crc32Of(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned long crc = crc32(0L, Z_NULL, 0);
    std::vector<char> buffer(65536);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        crc = crc32(crc, reinterpret_cast<const Bytef *>(buffer.data()), static_cast<uInt>(in.gcount()));
    }
    return static_cast<std::uint32_t>(crc);
}

struct IndexedTrack
{
    std::uint32_t id = 0;
    std::string analyzePath;
};

// One pass over export.pdb collecting every track's analyze_path -- the
// proposed replacement for calling findAnlzPathForTrackId() per item.
std::vector<IndexedTrack> buildAnlzIndex(const std::string &pioneerRoot)
{
    std::vector<IndexedTrack> out;
    std::string pdbPath = pioneerRoot + "/rekordbox/export.pdb";
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        rb::forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *rowTrack = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (!rowTrack) {
                        continue;
                    }
                    std::string analyzePath = rb::sqlText(rowTrack->analyze_path());
                    if (!analyzePath.empty()) {
                        out.push_back({rowTrack->id(), analyzePath});
                    }
                }
            }
        });
    }
    return out;
}

struct Item
{
    fs::path realExt;   // the real .EXT on the stick, read-only source
    std::string before; // its current bytes
    std::string after;  // the bytes a cue removal would leave
};

void printRow(const std::string &label, double seconds, int count, std::uint64_t bytes, int syncs)
{
    std::cout << "  " << std::left << std::setw(46) << label << std::right << std::setw(9) << std::fixed
              << std::setprecision(2) << seconds << " s";
    if (count > 0) {
        std::cout << std::setw(10) << std::setprecision(1) << (seconds * 1000.0 / count) << " ms/item";
    } else {
        std::cout << std::setw(19) << "";
    }
    if (bytes > 0) {
        std::cout << std::setw(10) << std::setprecision(1) << (static_cast<double>(bytes) / 1048576.0) << " MB";
    } else {
        std::cout << std::setw(13) << "";
    }
    if (syncs >= 0) {
        std::cout << std::setw(7) << syncs << " syncs";
    }
    std::cout << "\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: stick_write_bench <stick-mount-point> [count ...]\n";
        return 1;
    }
    const fs::path stick = argv[1];
    const std::string pioneerRoot = (stick / "PIONEER").string();
    std::string device;
    std::vector<int> counts;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            device = argv[++i];
            continue;
        }
        counts.push_back(std::atoi(arg.c_str()));
    }
    if (counts.empty()) {
        counts = {10, 50, 100, 200};
    }
    const int maxCount = *std::max_element(counts.begin(), counts.end());

    if (!fs::is_directory(pioneerRoot)) {
        std::cerr << "no PIONEER folder at " << pioneerRoot << "\n";
        return 1;
    }
    const fs::path scratch = stick / ".seabass-writebench";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "targets");
    fs::create_directories(scratch / "backups");
    const fs::path localScratch = fs::temp_directory_path() / "seabass-writebench-mirror";
    fs::remove_all(localScratch);
    fs::create_directories(localScratch);

    std::cout << "Stick: " << stick << "\n";
    if (device.empty()) {
        std::cout << "No --device given: runs share a warm page cache and whatever state the stick is in.\n";
    } else {
        std::cout << "Unmounting and remounting " << device << " before every timed run, then settling 3 s.\n";
    }

    // ---------------- Part A: repeated per-item overhead (read-only) -------

    std::cout << "\n== Part A: per-item overhead, read-only ==\n";

    auto t0 = Clock::now();
    auto index = buildAnlzIndex(pioneerRoot);
    double indexSeconds = secondsSince(t0);
    std::cout << "  export.pdb tracks indexed: " << index.size() << "\n";
    printRow("build the whole id->path index once", indexSeconds, 0, 0, -1);

    const int lookupSamples = std::min<int>(maxCount, static_cast<int>(index.size()));
    t0 = Clock::now();
    for (int i = 0; i < lookupSamples; ++i) {
        volatile auto found = rb::findAnlzPathForTrackId(pioneerRoot, index[i].id);
        (void)found;
    }
    double lookupSeconds = secondsSince(t0);
    printRow("findAnlzPathForTrackId, once per item", lookupSeconds, lookupSamples, 0, -1);

    const bool hasOneLibrary = ol::OneLibraryCueWriter::existsFor(pioneerRoot);
    double oneLibOpenSeconds = 0.0;
    double oneLibOpenOnceSeconds = 0.0;
    double crcSeconds = 0.0;
    std::uint64_t oneLibBytes = 0;
    if (hasOneLibrary) {
        const fs::path dbPath = ol::OneLibraryCueWriter::dbPathFor(pioneerRoot);
        oneLibBytes = fs::file_size(dbPath);
        const std::string key = ol::deriveOneLibraryKey();

        t0 = Clock::now();
        for (int i = 0; i < lookupSamples; ++i) {
            ol::SqlCipherLibrary lib;
            ol::SqlCipherDb db(lib, dbPath.string(), /*readOnly=*/true);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("SELECT count(*) FROM sqlite_master;");
        }
        oneLibOpenSeconds = secondsSince(t0);

        t0 = Clock::now();
        {
            ol::SqlCipherLibrary lib;
            ol::SqlCipherDb db(lib, dbPath.string(), /*readOnly=*/true);
            db.exec("PRAGMA key = '" + key + "';");
            for (int i = 0; i < lookupSamples; ++i) {
                db.exec("SELECT count(*) FROM sqlite_master;");
            }
        }
        oneLibOpenOnceSeconds = secondsSince(t0);

        t0 = Clock::now();
        for (int i = 0; i < lookupSamples; ++i) {
            volatile auto crc = crc32Of(dbPath);
            (void)crc;
        }
        crcSeconds = secondsSince(t0);

        std::cout << "  exportLibrary.db: " << (oneLibBytes / 1024) << " KB\n";
        printRow("SQLCipher open + PRAGMA key, once per item", oneLibOpenSeconds, lookupSamples, 0, -1);
        printRow("SQLCipher opened once, reused", oneLibOpenOnceSeconds, lookupSamples, 0, -1);
        printRow("whole-file CRC32, once per item", crcSeconds, lookupSamples, 0, -1);
    } else {
        std::cout << "  (no exportLibrary.db on this stick)\n";
    }

    // ---------------- Part B: the three durability strategies --------------

    // Real .EXT files, in index order, so the sizes are the real ones.
    std::vector<Item> items;
    for (const auto &entry : index) {
        if (static_cast<int>(items.size()) >= maxCount) {
            break;
        }
        fs::path ext = rb::extAnlzPath(pioneerRoot, entry.analyzePath);
        std::error_code ec;
        if (!fs::is_regular_file(ext, ec)) {
            continue;
        }
        Item item;
        item.realExt = ext;
        item.before = readWholeFile(ext);
        if (item.before.size() < 128) {
            continue;
        }
        // A cue removal shifts every following byte and shrinks the file by
        // one PCP2 entry; the exact bytes do not matter here, only the shape.
        item.after = item.before.substr(0, item.before.size() - 48);
        items.push_back(std::move(item));
    }
    std::cout << "\n  cue files sampled: " << items.size() << "\n";

    std::cout << "\n== Part B: the three write strategies ==\n";

    // Discarded warm-up: the very first writes to a freshly created directory
    // on a stick that has just been read from are not representative of
    // anything, and without this they land on whichever strategy happens to
    // run first.
    {
        const int warm = std::min<int>(40, static_cast<int>(items.size()));
        for (int i = 0; i < warm; ++i) {
            seabass::infrastructure::writeFileDurablyAtomic((scratch / "targets" / ("w" + std::to_string(i) + ".EXT")).string(),
                                                            items[i].before);
        }
        for (int i = 0; i < warm; ++i) {
            fs::remove(scratch / "targets" / ("w" + std::to_string(i) + ".EXT"));
        }
        syncFilesystemAt(stick);
        std::cout << "  (warm-up done, discarded)\n";
    }

    for (int count : counts) {
        if (count > static_cast<int>(items.size())) {
            continue;
        }
        std::uint64_t payload = 0;
        for (int i = 0; i < count; ++i) {
            payload += items[i].before.size();
        }
        std::cout << "\n  --- " << count << " stray cues, one per track: " << std::fixed << std::setprecision(1)
                  << (static_cast<double>(payload) / 1048576.0) << " MB of cue files ---\n";

        auto resetTargets = [&]() {
            // Plain writes plus one sync: this is untimed setup, so it only
            // has to leave the same state, not be durable the slow way.
            for (int i = 0; i < count; ++i) {
                writePlain(scratch / "targets" / (std::to_string(i) + ".EXT"), items[i].before);
            }
            fs::remove_all(scratch / "backups");
            fs::create_directories(scratch / "backups");
            fs::remove_all(localScratch);
            fs::create_directories(localScratch);
            syncFilesystemAt(stick);
            g_syncCalls = 0;
        };
        auto targetOf = [&](int i) { return scratch / "targets" / (std::to_string(i) + ".EXT"); };
        auto backupOf = [&](int i) { return scratch / "backups" / (std::to_string(i) + ".EXT"); };

        // Option 1: today. Per file, a durable atomic backup copy and a
        // durable atomic rewrite: two fsyncs plus a directory fsync each.
        auto option1 = [&]() {
            for (int i = 0; i < count; ++i) {
                seabass::infrastructure::writeFileDurablyAtomic(backupOf(i).string(), items[i].before);
                seabass::infrastructure::writeFileDurablyAtomic(targetOf(i).string(), items[i].after);
            }
        };

        // Option 2: relaxed. Write and rename immediately without fsync, one
        // filesystem sync at the end.
        auto option2 = [&]() {
            for (int i = 0; i < count; ++i) {
                fs::path bt = backupOf(i).string() + ".tmp";
                writePlain(bt, items[i].before);
                fs::rename(bt, backupOf(i));
                fs::path tt = targetOf(i).string() + ".tmp";
                writePlain(tt, items[i].after);
                fs::rename(tt, targetOf(i));
            }
            syncFilesystemAt(stick);
        };

        // Option 3: strict. Per-item edits land on tmpfs; the stick sees one
        // batched commit: all backups durable and in place, then all targets.
        auto option3 = [&]() {
            for (int i = 0; i < count; ++i) {
                writePlain(localScratch / (std::to_string(i) + ".EXT"), items[i].after);
            }
            for (int i = 0; i < count; ++i) {
                writePlain(backupOf(i).string() + ".tmp", items[i].before);
            }
            syncFilesystemAt(stick);
            for (int i = 0; i < count; ++i) {
                fs::rename(backupOf(i).string() + ".tmp", backupOf(i));
            }
            for (int i = 0; i < count; ++i) {
                writePlain(targetOf(i).string() + ".tmp", readWholeFile(localScratch / (std::to_string(i) + ".EXT")));
            }
            syncFilesystemAt(stick);
            for (int i = 0; i < count; ++i) {
                fs::rename(targetOf(i).string() + ".tmp", targetOf(i));
            }
            syncFilesystemAt(stick);
        };

        // A USB stick's write latency is erratic: its controller does its own
        // wear levelling and garbage collection, so whichever strategy runs
        // first eats the warm-up and whichever runs last can be charged for
        // work the earlier ones queued. Rotate the order across repetitions
        // and report the median, or the first run of the day wins every time.
        std::vector<std::vector<double>> runs(3);
        const int reps = 3;
        for (int rep = 0; rep < reps; ++rep) {
            for (int slot = 0; slot < 3; ++slot) {
                int option = (slot + rep) % 3;
                resetTargets();
                if (!remountAndSettle(device, stick, 3)) {
                    std::cerr << "    (skipping this run)\n";
                    continue;
                }
                t0 = Clock::now();
                if (option == 0) {
                    option1();
                } else if (option == 1) {
                    option2();
                } else {
                    option3();
                }
                runs[option].push_back(secondsSince(t0));
            }
        }
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const char *names[3] = {"1. per-file fsync (today)", "2. relaxed, one sync at the end",
                                "3. strict batch via tmpfs mirror"};
        const int syncs[3] = {count * 4, 1, 3};
        for (int option = 0; option < 3; ++option) {
            printRow(names[option], median(runs[option]), count, payload * 2, syncs[option]);
            std::cout << "        runs:";
            for (double s : runs[option]) {
                std::cout << " " << std::fixed << std::setprecision(2) << s;
            }
            std::cout << "\n";
        }
    }

    fs::remove_all(scratch);
    fs::remove_all(localScratch);
    std::cout << "\nScratch removed. The real library was never written to.\n";
    return 0;
}
