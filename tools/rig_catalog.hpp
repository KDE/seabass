// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: what the rig tools share -- reading a stick's library
// fingerprint without writing to the stick, and proving its catalog files
// are the ones a full stick backup recorded.

#pragma once

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "application/use_cases/scan_library.hpp"
#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::rig
{

// The label the rig tools use to tell two test sticks apart: the mount
// point's own final path component, which on Linux/macOS is the name
// udev/DiskArbitration gave the auto-mounted volume (RIG_STICK_A/B's
// default paths end in it) and so happens to equal the real label there.
// A bare Windows drive root ("E:\") has no such component at all --
// filename() is only ever non-empty below the root -- so this returned
// empty on Windows for every caller: rig_advise's "--expect e=..." never
// matched (every stick read back as "no such stick"), and rig_clone named
// the archive it makes from an empty label plus ".zip", i.e. a file
// literally called ".zip". root_name() ("E:") still holds the drive
// letter; lower-cased, it is exactly what the rig's own RIG_STICK_A=/e
// convention already expects.
inline std::string stickLabelFor(const std::filesystem::path &root)
{
    std::string label = root.filename().string();
    if (!label.empty()) {
        return label;
    }
    const std::string rootName = root.root_name().string();
    if (!rootName.empty() && std::isalpha(static_cast<unsigned char>(rootName.front()))) {
        return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(rootName.front()))));
    }
    return label;
}

// The stick's library fingerprint, for information: rekordbox and Engine
// tracks together, as the app fingerprints them -- but without filling in
// missing lengths. The fill probes audio files and writes its results to a
// cache on the stick, and a check must not change what it checks: the
// first rig run left that cache behind as two "extras".
inline std::optional<domain::LibraryFingerprint> fingerprintStick(const std::filesystem::path &root)
{
    std::vector<domain::Track> tracks;
    bool anyRead = false;
    const std::filesystem::path pioneer = root / "PIONEER";
    if (std::filesystem::exists(pioneer / "rekordbox" / "export.pdb")) {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        std::cout << "  rekordbox: " << read.size() << " tracks\n";
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    const std::filesystem::path engine = root / "Engine Library";
    if (std::filesystem::exists(engine / "Database2" / "m.db") || std::filesystem::exists(engine / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(engine.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        std::cout << "  engine: " << read.size() << " tracks\n";
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    if (!anyRead) {
        return std::nullopt;
    }
    return domain::fingerprintLibrary(tracks);
}

inline bool isCatalogFile(const std::string &path)
{
    const auto endsWith = [&path](std::string_view tail) {
        return path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (path.rfind("PIONEER/rekordbox/", 0) == 0) {
        return endsWith(".pdb") || endsWith(".db") || endsWith(".db-wal");
    }
    if (path.rfind("Engine Library/Database2/", 0) == 0) {
        return endsWith(".db") || endsWith(".db-wal");
    }
    return false;
}

// Every catalog file the manifest lists, hashed on the stick and compared.
// Returns the number checked; mismatches are printed and counted in `bad`.
inline std::size_t checkCatalogFiles(const std::filesystem::path &archive, const std::filesystem::path &root, std::size_t &bad)
{
    namespace sb = infrastructure::stick_backup;
    sb::PosixArchiveFile file(archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
    sb::Zip64Reader reader = sb::Zip64Reader::open(file);
    const std::optional<std::size_t> index = reader.findEntry(sb::ManifestEntryName);
    if (!index) {
        throw std::runtime_error("the archive has no manifest");
    }
    std::string manifestError;
    const std::optional<sb::BackupManifest> manifest = sb::BackupManifest::parse(reader.readEntryToString(*index), &manifestError);
    if (!manifest) {
        throw std::runtime_error("the manifest is damaged: " + manifestError);
    }
    std::size_t checked = 0;
    for (const sb::ManifestRow &row : manifest->rows) {
        if (row.kind != sb::ManifestRow::Kind::File || !isCatalogFile(row.path)) {
            continue;
        }
        ++checked;
        const std::filesystem::path onStick = root / row.path;
        std::ifstream in(onStick, std::ios::binary);
        if (!in) {
            std::cout << "  MISSING " << row.path << "\n";
            ++bad;
            continue;
        }
        infrastructure::hashing::Sha256 hasher;
        std::vector<char> buffer(1 << 20);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) {
                hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(buffer.data()), static_cast<std::size_t>(got)));
            }
        }
        const bool same = hasher.finish() == row.sha256;
        std::cout << "  " << (same ? "same    " : "DIFFERS ") << row.path << "\n";
        if (!same) {
            ++bad;
        }
    }
    return checked;
}

}  // namespace seabass::rig
