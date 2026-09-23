// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/sqlite_db_set.hpp"

#include "infrastructure/stick_backup/file_entry_source.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

namespace
{

constexpr char SqliteMagic[] = "SQLite format 3";

std::uint32_t bigEndian32(const unsigned char *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

bool readPrefix(const fs::path &path, unsigned char *out, std::size_t length)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(in.gcount()) == length;
}


// The file's hash, or nothing when it could not be read to the end.
//
// The `while (in)` loop leaves on badbit exactly as readily as on
// eofbit, and this used to return the hash of whatever prefix it had --
// which is how a failing stick certified its own data loss. The capture
// below hashes each member twice, once in flight and once off the
// stick, and calls them consistent when the two agree. On a stick that
// refuses at 128 KiB of a 268 KiB database, BOTH passes stop at the
// same place and hash the same prefix, so they agree, the set is
// reported Captured, and a 0-byte m.db goes into the archive with
// nothing marking it. A restore then writes it over a good one.
//
// Measured on a real damaged stick, 2026-09-23, not reasoned about:
// A1's m.db read 131072 of 274432 bytes and the archive held 0.
std::optional<hashing::Sha256Digest> hashFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    hashing::Sha256 hasher;
    std::vector<char> buffer(1u << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = in.gcount();
        if (got > 0) {
            hasher.update(std::as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(got))));
        }
    }
    if (in.bad()) {
        return std::nullopt;  // a prefix is not this file's hash
    }
    return hasher.finish();
}

}  // namespace

std::string DbSetFingerprint::toHex() const
{
    static constexpr char Alphabet[] = "0123456789abcdef";
    std::string out;
    auto put = [&out](std::uint64_t value, int bytes) {
        for (int shift = bytes * 8 - 4; shift >= 0; shift -= 4) {
            out.push_back(Alphabet[(value >> shift) & 0xf]);
        }
    };
    put(changeCounter, 4);
    put(schemaCookie, 4);
    put(mainSize, 8);
    put(hasWal ? 1 : 0, 1);
    put(walSize, 8);
    put(walSalt1, 4);
    put(walSalt2, 4);
    put(hasJournal ? 1 : 0, 1);
    put(journalSize, 8);
    return out;
}

std::vector<fs::path> dbSetMembers(const fs::path &mainDb)
{
    std::vector<fs::path> members{mainDb};
    std::error_code ec;
    for (const char *suffix : {"-wal", "-journal"}) {
        fs::path sidecar = mainDb;
        sidecar += suffix;
        if (fs::is_regular_file(sidecar, ec)) {
            members.push_back(sidecar);
        }
    }
    return members;
}

std::optional<DbSetFingerprint> fingerprintDbSet(const fs::path &mainDb)
{
    unsigned char header[100];
    if (!readPrefix(mainDb, header, sizeof header) || std::memcmp(header, SqliteMagic, sizeof SqliteMagic) != 0) {
        return std::nullopt;
    }
    DbSetFingerprint fp;
    fp.changeCounter = bigEndian32(header + 24);
    fp.schemaCookie = bigEndian32(header + 40);
    std::error_code ec;
    fp.mainSize = fs::file_size(mainDb, ec);
    if (ec) {
        return std::nullopt;
    }
    fs::path wal = mainDb;
    wal += "-wal";
    if (fs::is_regular_file(wal, ec)) {
        fp.hasWal = true;
        fp.walSize = fs::file_size(wal, ec);
        unsigned char walHeader[32];
        if (readPrefix(wal, walHeader, sizeof walHeader)) {
            fp.walSalt1 = bigEndian32(walHeader + 16);
            fp.walSalt2 = bigEndian32(walHeader + 20);
        }
    }
    fs::path journal = mainDb;
    journal += "-journal";
    if (fs::is_regular_file(journal, ec)) {
        fp.hasJournal = true;
        fp.journalSize = fs::file_size(journal, ec);
    }
    return fp;
}

DbSetCapture captureDbSet(const fs::path &stickRoot, const std::string &relativeMainDb, ArchiveUpdater &updater, int retries,
                          const std::function<void(std::uint64_t)> &progress, bool salvage)
{
    DbSetCapture capture;
    const fs::path mainDb = stickRoot / pathFromUtf8(relativeMainDb);
    // Members are the main file plus a suffix, so their archive names are
    // the main file's name plus the same suffix -- no path arithmetic.
    auto relativeNameOf = [&](const fs::path &member) {
        return relativeMainDb + member.filename().string().substr(mainDb.filename().string().size());
    };
    std::error_code ec;
    for (const fs::path &member : dbSetMembers(mainDb)) {
        std::uint64_t size = fs::file_size(member, ec);
        if (!ec && size >= MaxCapturableDbBytes) {
            capture.status = DbSetCapture::Status::TooLarge;
            capture.detail = relativeNameOf(member) + " is " + std::to_string(size) + " bytes";
            return capture;
        }
    }

    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
        }
        std::optional<DbSetFingerprint> before = fingerprintDbSet(mainDb);
        std::vector<fs::path> members = dbSetMembers(mainDb);
        capture.entries.clear();
        capture.memberRelativePaths.clear();
        capture.memberMtimes.clear();
        bool torn = false;
        bool readError = false;
        std::size_t appended = 0;
        std::uint64_t attemptBytes = 0;

        for (std::size_t m = 0; m < members.size(); ++m) {
            const fs::path &member = members[m];
            std::string relative = relativeNameOf(member);
            fs::file_time_type mtime = fs::last_write_time(member, ec);
            FileEntrySource source(member);
            if (ec || !source.ok()) {
                // A sidecar that vanished between listing and opening is a
                // rollback journal being deleted by a commit -- that is the
                // writer we are looking for, so retry. The main file being
                // unreadable is a real error.
                if (m == 0) {
                    readError = true;
                    capture.detail = relative + ": " + (ec ? ec.message() : std::string("could not open"));
                } else {
                    torn = true;
                    capture.detail = relative + " appeared or vanished while reading";
                }
                ec.clear();
                break;
            }
            auto entry = updater.appendFile(relative, toUnixSeconds(mtime), source, application::CancellationToken::none(),
                                            [&](std::uint64_t bytes) {
                                                if (progress) {
                                                    progress(attemptBytes + bytes);
                                                }
                                            });
            if (!entry) {
                readError = true;
                capture.detail = relative + ": read interrupted";
                break;
            }
            // What reached the archive against what the file claims to
            // be. The hash comparison below cannot see this on its own:
            // a device that refuses partway makes BOTH passes stop at
            // the same offset, so they agree about a prefix. This is
            // the check that does not depend on the two passes failing
            // differently.
            std::error_code sizeEc;
            const std::uintmax_t sizeNow = fs::file_size(member, sizeEc);
            if (source.readFailed() || sizeEc || entry->entry.size != sizeNow) {
                capture.detail = relative + ": read "
                    + std::to_string(entry->entry.size) + " of "
                    + (sizeEc ? std::string("an unreadable size") : std::to_string(sizeNow)) + " bytes";
                // A device that refused is a read error however many
                // times it is asked; a size that moved underneath is
                // something writing, which is worth another pass.
                if (source.readFailed()) {
                    readError = true;
                } else {
                    torn = true;
                }
                // It IS in the archive, and nothing will list it: forget
                // it now rather than count it for later. Counted, it was
                // forgotten with the rest of a failed attempt -- but a
                // salvage run KEEPS its last attempt, and kept that
                // entry with it, unlisted, in the archive with no row
                // (backup_database_capture_test case 4b, about one run
                // in ten, when the writer thread moved the size).
                updater.forgetLastEntries(1);
                break;
            }
            ++appended;
            attemptBytes += entry->entry.size;
            capture.entries.push_back(*entry);
            capture.memberRelativePaths.push_back(relative);
            capture.memberMtimes.push_back(toUnixSeconds(mtime));
        }
        capture.bytesRead += attemptBytes;

        if (!readError && !torn) {
            // Second pass: re-hash every member on the stick. Any difference
            // from what was streamed means a writer was active.
            for (std::size_t i = 0; i < members.size() && !torn; ++i) {
                std::optional<hashing::Sha256Digest> after = hashFile(members[i]);
                torn = !after || *after != capture.entries[i].sha256;
                capture.bytesRead += capture.entries[i].entry.size;
            }
            std::optional<DbSetFingerprint> afterFp = fingerprintDbSet(mainDb);
            torn = torn || !before || !afterFp || !(*before == *afterFp) || dbSetMembers(mainDb).size() != members.size();
        }

        if (!torn && !readError) {
            capture.status = DbSetCapture::Status::Captured;
            capture.fingerprint = before.value_or(DbSetFingerprint{});
            return capture;
        }
        // A salvage run keeps the attempt instead. The database is the
        // most valuable thing on the stick -- the cues, the playlists,
        // the edits -- and off a failing device a copy that may be
        // inconsistent is still the only copy on offer. Only on the last
        // attempt: the earlier ones are still worth retrying, since the
        // fault may be intermittent.
        // Whatever it got, as long as it got the main file. A sidecar
        // that appeared or vanished mid-pass leaves fewer entries than
        // members, and on a failing device that is the ordinary case
        // rather than a reason to keep nothing: the main database is
        // where the cues and playlists are.
        const bool lastAttempt = attempt == retries;
        if (salvage && lastAttempt && !readError && !capture.entries.empty()) {
            capture.status = DbSetCapture::Status::Salvaged;
            capture.fingerprint = before.value_or(DbSetFingerprint{});
            capture.detail = relativeMainDb + " could not be read consistently off this stick; what is here is one "
                                              "attempt and its parts may not agree with each other";
            return capture;
        }
        updater.forgetLastEntries(appended);
        capture.entries.clear();
        capture.memberRelativePaths.clear();
        capture.memberMtimes.clear();
        if (readError) {
            capture.status = DbSetCapture::Status::ReadError;
            return capture;
        }
    }
    capture.status = DbSetCapture::Status::Unstable;
    capture.detail = relativeMainDb + " kept changing while being read";
    return capture;
}

}  // namespace seabass::infrastructure::stick_backup
