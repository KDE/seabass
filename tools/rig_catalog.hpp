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
#include <map>
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
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

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
    std::string label = pathToUtf8(root.filename());
    if (!label.empty()) {
        return label;
    }
    const std::string rootName = pathToUtf8(root.root_name());
    if (!rootName.empty() && std::isalpha(static_cast<unsigned char>(rootName.front()))) {
        return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(rootName.front()))));
    }
    return label;
}

// An Engine reader that reads each row's cues at its file's own sample
// rate, as the app does (docs/sync-after-rekordbox-export-plan.md, step
// 6b): a row the player has not analysed yet records no rate, and the
// reader alone takes 44.1 kHz, which reads a 48 kHz file's cues 9 percent
// late. The rate comes from the stick's metadata cache, or a probe of the
// file. What was probed is never saved: a check must not change what it
// checks.
class EngineReaderAtFileRates
{
public:
    explicit EngineReaderAtFileRates(const std::filesystem::path &engineLibrary)
        : m_rates(infrastructure::paths::stickRootForCatalogPath(pathToUtf8(engineLibrary)), m_probe),
          m_reader(pathToUtf8(engineLibrary))
    {
        m_reader.setSampleRateSource([this](const std::string &file) { return m_rates.rateOf(file); });
    }
    EngineReaderAtFileRates(const EngineReaderAtFileRates &) = delete;
    EngineReaderAtFileRates &operator=(const EngineReaderAtFileRates &) = delete;

    infrastructure::engine::LibdjinteropEngineReader &reader() { return m_reader; }
    std::size_t probes() const { return m_rates.probes(); }

private:
#ifdef SEABASS_HAVE_TAGLIB
    infrastructure::audio::TagLibMetadataProbe m_probe;
#else
    application::NullTrackMetadataProbe m_probe;
#endif
    infrastructure::local::CachedSampleRates m_rates;
    infrastructure::engine::LibdjinteropEngineReader m_reader;
};

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
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        std::cout << "  rekordbox: " << read.size() << " tracks\n";
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    const std::filesystem::path engine = root / "Engine Library";
    if (std::filesystem::exists(engine / "Database2" / "m.db") || std::filesystem::exists(engine / "m.db")) {
        EngineReaderAtFileRates engineAtRates(engine);
        std::vector<domain::Track> read = application::ScanLibrary(engineAtRates.reader()).execute();
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

// A file's sha256, or nothing when it cannot be opened or a read fails
// part-way: the hash of a prefix is not the file's.
inline std::optional<infrastructure::hashing::Sha256Digest> sha256OfFile(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
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
    if (in.bad()) {
        return std::nullopt;
    }
    return hasher.finish();
}

// Every catalog file on the stick now (isCatalogFile: export.pdb and its
// siblings, OneLibrary and its log, Engine's databases and logs), by its
// '/'-separated path from the stick root, with its sha256 in hex. Where a
// file cannot be read its value is "UNREADABLE", which no hash equals.
inline std::map<std::string, std::string> catalogFileShas(const std::filesystem::path &root)
{
    std::map<std::string, std::string> out;
    for (const std::filesystem::path &dir : {root / "PIONEER" / "rekordbox", root / "Engine Library" / "Database2"}) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string relative = pathToGenericUtf8(std::filesystem::relative(entry.path(), root));
            if (!isCatalogFile(relative)) {
                continue;
            }
            const auto digest = sha256OfFile(entry.path());
            out[relative] = digest ? infrastructure::hashing::toHex(*digest) : std::string("UNREADABLE");
        }
    }
    return out;
}

// The sticks no rig tool writes: rig-platform.sh's is_protected_label, a
// label (or any folder of the path given) starting with WHALESHARK or
// CORSAIR in any case. WHALESHARK and WHALESHARK2 are Sebastian's working
// sticks and CORSAIR is a reference.
inline bool isProtectedStick(const std::filesystem::path &root)
{
    const auto protectedName = [](std::string name) {
        for (char &c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return name.rfind("WHALESHARK", 0) == 0 || name.rfind("CORSAIR", 0) == 0;
    };
    if (protectedName(stickLabelFor(root))) {
        return true;
    }
    for (const auto &part : root.lexically_normal()) {
        if (protectedName(pathToUtf8(part))) {
            return true;
        }
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
        // The manifest key is UTF-8 with forward slashes; root / row.path
        // would read it in the ANSI code page on Windows.
        const std::filesystem::path onStick = root / pathFromUtf8(row.path);
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
        // A read the stick refused leaves the loop just as an ending does,
        // and the hash of a prefix is not the file's. Say which it was.
        if (in.bad()) {
            std::cout << "  UNREADABLE " << row.path << "\n";
            ++bad;
            continue;
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
