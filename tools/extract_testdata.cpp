// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Copies a real stick's library metadata into the local test-data
// directory, verbatim and NOT anonymized.
//
// This is the other half of the corpus (see docs/real-data-testing.md).
// The anonymized export is what can be shared; this is what actually
// carries the awkward blobs, the odd encodings and the deleted rows that
// break code, so it is what integrity and stability tests run against.
// It never leaves the machine.
//
// C++ rather than a shell script on purpose: the same program has to run
// on Windows and eventually macOS, and a script full of lsblk/findmnt
// does not. Nothing here is platform-specific.
//
// What it copies: both catalogs' databases, every analysis file, and the
// device-settings files. What it does not: audio and artwork, which are
// enormous and which nothing under test reads.
//
// Usage:
//   extract_testdata <stick-mount-point> <destination-dir> [set-name] [--zip]
//
// --zip additionally writes <destination-dir>/<name>-<date>.zip and
// removes the directory, so a set is one file to move around. The corpus
// runner reads either form.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/zip_archive_writer.hpp"

namespace fs = std::filesystem;

namespace
{

struct Tally
{
    int files = 0;
    std::uintmax_t bytes = 0;
};

void copyInto(const fs::path &from, const fs::path &to, Tally &tally)
{
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        return;
    }
    if (fs::is_directory(from, ec)) {
        fs::create_directories(to, ec);
        for (const auto &entry : fs::recursive_directory_iterator(from, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const fs::path relative = fs::relative(entry.path(), from, ec);
            const fs::path target = to / relative;
            fs::create_directories(target.parent_path(), ec);
            if (fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec)) {
                ++tally.files;
                tally.bytes += entry.file_size(ec);
            }
        }
        return;
    }
    fs::create_directories(to.parent_path(), ec);
    if (fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
        ++tally.files;
        tally.bytes += fs::file_size(from, ec);
    }
}

std::string nowStamp()
{
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(now));
}

std::string humanSize(std::uintmax_t bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: extract_testdata <stick-mount-point> <destination-dir> [set-name] [--zip]\n";
        return 1;
    }
    bool asZip = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--zip") {
            asZip = true;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 2) {
        std::cerr << "usage: extract_testdata <stick-mount-point> <destination-dir> [set-name] [--zip]\n";
        return 1;
    }
    const fs::path stick = positional[0];
    const fs::path destinationRoot = positional[1];
    std::error_code ec;

    if (!fs::is_directory(stick, ec)) {
        std::cerr << stick << " is not a directory\n";
        return 1;
    }
    const bool hasRekordbox = fs::is_directory(stick / "PIONEER", ec);
    const bool hasEngine = fs::is_directory(stick / "Engine Library", ec);
    if (!hasRekordbox && !hasEngine) {
        std::cerr << "no PIONEER or Engine Library folder under " << stick << "\n";
        return 1;
    }

    std::string setName = positional.size() > 2 ? positional[2] : stick.filename().string();
    if (setName.empty()) {
        setName = "stick";
    }
    const fs::path destination = destinationRoot / (setName + "-" + nowStamp());
    if (fs::exists(destination, ec)) {
        std::cerr << destination << " already exists -- refusing to write into it\n";
        return 1;
    }

    fs::create_directories(destination, ec);

    // This data is not shareable and the easiest way to leak it is a later
    // `git add -A` somewhere that turns out to be inside a working tree.
    // An ignore file in the set itself is prevention rather than
    // prohibition: it holds wherever the set is put, including inside a
    // repository, and it does not false-positive the way refusing to write
    // under any enclosing `.git` does (a home directory is often itself a
    // repository).
    {
        std::ofstream ignore(destination / ".gitignore");
        ignore << "# Real, un-anonymized library data. Never commit any of it.\n*\n";
    }
    Tally rekordbox, engine, settings;

    if (hasRekordbox) {
        // Both catalog databases plus the OneLibrary mirror: all three are
        // read by code under test, and in a local set there is no reason to
        // hold any of them back.
        for (const char *name : {"export.pdb", "exportExt.pdb", "exportLibrary.db"}) {
            copyInto(stick / "PIONEER" / "rekordbox" / name, destination / "PIONEER" / "rekordbox" / name, rekordbox);
        }
        copyInto(stick / "PIONEER" / "USBANLZ", destination / "PIONEER" / "USBANLZ", rekordbox);
        // Device Profile reads these, so a set without them cannot test it.
        for (const char *name : {"MYSETTING.DAT", "MYSETTING2.DAT", "DEVSETTING.DAT", "DJMMYSETTING.DAT",
                                  "djprofile.nxs"}) {
            copyInto(stick / "PIONEER" / name, destination / "PIONEER" / name, settings);
        }
    }
    if (hasEngine) {
        copyInto(stick / "Engine Library" / "Database2", destination / "Engine Library" / "Database2", engine);
    }

    const std::uintmax_t total = rekordbox.bytes + engine.bytes + settings.bytes;
    {
        std::ofstream note(destination / "SET.txt");
        note << "Seabass local test data set\n"
             << "===========================\n\n"
             << "Source stick: " << stick.string() << "\n"
             << "Extracted:    " << nowStamp() << "\n\n"
             << "rekordbox databases and analysis files: " << rekordbox.files << " files, "
             << humanSize(rekordbox.bytes) << "\n"
             << "Engine databases:                       " << engine.files << " files, " << humanSize(engine.bytes)
             << "\n"
             << "Device settings files:                  " << settings.files << " files, "
             << humanSize(settings.bytes) << "\n"
             << "Total:                                  " << humanSize(total) << "\n\n"
             << "THIS DATA IS NOT ANONYMIZED.\n"
             << "It holds real titles, artists, album names and file paths. Keep it on\n"
             << "this machine. Do not commit it, do not attach it to anything, and do\n"
             << "not use it as the fixture that ships with the project -- that one is\n"
             << "produced by `seabass-cli anonymize` instead.\n\n"
             << "Audio and artwork were deliberately not copied: nothing under test\n"
             << "reads them and they are what makes a stick large.\n";
    }

    fs::path written = destination;
    if (asZip) {
        // One file is easier to move between machines than 6,000, and the
        // corpus runner unpacks a zipped set the same way it reads a
        // directory. The .gitignore goes into the archive too, so
        // unpacking it anywhere keeps the same protection.
        fs::path zipPath = destination;
        zipPath += ".zip";
        try {
            seabass::infrastructure::writeZipArchive(destination, zipPath);
        } catch (const std::exception &e) {
            std::cerr << "could not write " << zipPath.string() << ": " << e.what() << "\n"
                      << "the extracted directory is still at " << destination.string() << "\n";
            return 1;
        }
        fs::remove_all(destination, ec);
        written = zipPath;
    }

    std::cout << "Wrote " << written.string() << "\n"
              << "  rekordbox: " << rekordbox.files << " files, " << humanSize(rekordbox.bytes) << "\n"
              << "  Engine:    " << engine.files << " files, " << humanSize(engine.bytes) << "\n"
              << "  settings:  " << settings.files << " files, " << humanSize(settings.bytes) << "\n"
              << "  total:     " << humanSize(total) << "\n\n"
              << "NOT ANONYMIZED. Real titles, artists and paths. Keep it on this machine.\n";
    if (asZip) {
        std::cout << "The set's own SET.txt inside the archive says the same.\n";
    } else {
        std::cout << "See SET.txt in that directory.\n";
    }
    return 0;
}
