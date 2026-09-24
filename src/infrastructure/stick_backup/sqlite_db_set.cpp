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
#include "infrastructure/paths/utf8_path.hpp"
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


// A member read through FileEntrySource, or -- in a test -- one that
// stops at a given offset the way a failing stick does. "Refused" is
// either: the device said no, or the test said it would have.
class StopsAt : public EntrySource
{
public:
    StopsAt(FileEntrySource &inner, std::optional<std::uint64_t> limit) : m_inner(inner), m_limit(limit) {}
    bool refused() const { return m_inner.readFailed() || m_stopped; }
    std::size_t read(std::span<std::byte> out) override
    {
        if (m_limit) {
            if (m_done >= *m_limit) {
                m_stopped = m_stopped || m_inner.read(out.first(std::min<std::size_t>(out.size(), 1))) > 0;
                return 0;
            }
            out = out.first(static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), *m_limit - m_done)));
        }
        const std::size_t got = m_inner.read(out);
        m_done += got;
        return got;
    }

private:
    FileEntrySource &m_inner;
    std::optional<std::uint64_t> m_limit;
    std::uint64_t m_done = 0;
    bool m_stopped = false;
};

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

// "Not there" is an answer; anything else is not. is_regular_file()
// reports both as plain false, and for a sidecar the first is the
// ordinary case -- most databases have no -wal -- so refusing on any
// error at all would refuse nearly every capture.
bool absenceIsCertain(const std::error_code &ec)
{
    return !ec || ec == std::errc::no_such_file_or_directory;
}

std::vector<fs::path> dbSetMembers(const fs::path &mainDb, bool *presenceKnown)
{
    if (presenceKnown != nullptr) {
        *presenceKnown = true;
    }
    std::vector<fs::path> members{mainDb};
    for (const char *suffix : {"-wal", "-journal"}) {
        fs::path sidecar = mainDb;
        sidecar += suffix;
        std::error_code ec;
        const bool there = fs::is_regular_file(sidecar, ec);
        if (!absenceIsCertain(ec) && presenceKnown != nullptr) {
            *presenceKnown = false;
        }
        if (there) {
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
    // A fingerprint records which sidecars a set has. If their presence
    // cannot be determined it is not a fingerprint of anything, and
    // returning one that says hasWal=false is how the second pass comes
    // to agree with the first about a set neither of them saw whole.
    std::error_code sidecarEc;
    const bool walThere = fs::is_regular_file(wal, sidecarEc);
    if (!absenceIsCertain(sidecarEc)) {
        return std::nullopt;
    }
    if (walThere) {
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
    const bool journalThere = fs::is_regular_file(journal, sidecarEc);
    if (!absenceIsCertain(sidecarEc)) {
        return std::nullopt;
    }
    if (journalThere) {
        fp.hasJournal = true;
        fp.journalSize = fs::file_size(journal, ec);
    }
    return fp;
}

DbSetCapture captureDbSet(const fs::path &stickRoot, const std::string &relativeMainDb, ArchiveUpdater &updater, int retries,
                          const std::function<void(std::uint64_t)> &progress, bool salvage,
                          const std::function<std::optional<std::uint64_t>(const std::string &relativePath)>
                              &readLimitForTesting)
{
    DbSetCapture capture;
    const fs::path mainDb = stickRoot / pathFromUtf8(relativeMainDb);
    // Members are the main file plus a suffix, so their archive names are
    // the main file's name plus the same suffix -- no path arithmetic.
    auto relativeNameOf = [&](const fs::path &member) {
        return relativeMainDb + pathToGenericUtf8(member.filename()).substr(pathToGenericUtf8(mainDb.filename()).size());
    };
    std::error_code ec;
    bool presenceKnown = true;
    const std::vector<fs::path> declared = dbSetMembers(mainDb, &presenceKnown);
    if (!presenceKnown) {
        // A set whose membership could not be established is not one to
        // capture: the WAL may be there and unseen, and a main file
        // taken alone restores against a WAL whose salts do not match.
        capture.status = DbSetCapture::Status::ReadError;
        capture.detail = relativeMainDb + ": could not tell whether its -wal or -journal is there";
        return capture;
    }
    for (const fs::path &member : declared) {
        std::error_code sizeEc;
        const std::uint64_t size = fs::file_size(member, sizeEc);
        if (sizeEc) {
            // The ceiling exists because a database at or past it cannot
            // be read safely; a stat that failed is not permission to
            // try anyway.
            capture.status = DbSetCapture::Status::ReadError;
            capture.detail = relativeNameOf(member) + ": could not be measured (" + sizeEc.message() + ")";
            return capture;
        }
        if (size >= MaxCapturableDbBytes) {
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
        capture.memberSalvagedFromSizes.clear();
        capture.detail.clear();
        // Each member that went wrong adds its line rather than replacing
        // the one before: a truncated m.db followed by a torn sidecar
        // must still say that m.db is truncated.
        const auto note = [&capture](const std::string &line) {
            capture.detail = capture.detail.empty() ? line : capture.detail + "; " + line;
        };
        bool torn = false;
        bool refused = false;  // salvage only: a member kept although the stick stopped part-way
        bool readError = false;
        std::size_t appended = 0;
        std::uint64_t attemptBytes = 0;

        for (std::size_t m = 0; m < members.size(); ++m) {
            const fs::path &member = members[m];
            std::string relative = relativeNameOf(member);
            fs::file_time_type mtime = fs::last_write_time(member, ec);
            FileEntrySource file(member);
            const std::optional<std::uint64_t> limit =
                readLimitForTesting ? readLimitForTesting(relative) : std::optional<std::uint64_t>{};
            StopsAt source(file, limit);
            if (ec || !file.ok()) {
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
            // Zero bytes of the MAIN file is not a part of a database.
            // A device that refuses at the very first page gives
            // refused() and an entry of 0, the same shape as one that
            // refuses at 128 KiB, and without this the set went into the
            // archive as Salvaged with an EMPTY m.db, a manifest row, the
            // live fingerprint beside it and databaseCaptured still true
            // -- the exact bug salvage mode was written to fix, reached
            // through the fix. It falls through to a ReadError instead,
            // which is what it was before any of this.
            //
            // Only the main file. A sidecar the stick would not give a
            // byte of is kept at 0, marked with the size it should have
            // had: dropping the set over it would throw away the m.db
            // bytes the stick still gave, and an empty -wal or -journal
            // is one SQLite reads as holding nothing rather than one it
            // chokes on. The manifest row and the salvage log say "0 of
            // N bytes" either way.
            const bool mainFileGaveNothing = m == 0 && entry->entry.size == 0;
            if (salvage && source.refused() && !sizeEc && !mainFileGaveNothing && entry->entry.size < sizeNow) {
                // Off a stick the kernel has made read-only, a refusal
                // part-way is the device, not a writer, and what came
                // before it is the only copy of this database anybody
                // will get. Kept, marked with the size it should have
                // had, so the manifest, the salvage log and a restore
                // all know it is a part. A1's m.db, 2026-09-23: 128 KiB
                // of 268 KiB readable, and refusing the set kept none.
                refused = true;
                // The caller prefixes the set's main file, so a main file
                // is not named twice.
                note((relative == relativeMainDb ? std::string() : relative + ": ") + "only "
                     + std::to_string(entry->entry.size) + " of " + std::to_string(sizeNow) + " bytes could be read");
                ++appended;
                attemptBytes += entry->entry.size;
                capture.entries.push_back(*entry);
                capture.memberRelativePaths.push_back(relative);
                capture.memberMtimes.push_back(toUnixSeconds(mtime));
                capture.memberSalvagedFromSizes.push_back(sizeNow);
                continue;
            }
            if (source.refused() || sizeEc || entry->entry.size != sizeNow) {
                note(relative + ": read " + std::to_string(entry->entry.size) + " of "
                     + (sizeEc ? std::string("an unreadable size") : std::to_string(sizeNow)) + " bytes");
                // A device that refused is a read error however many
                // times it is asked; a size that moved underneath is
                // something writing, which is worth another pass.
                if (source.refused()) {
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
            capture.memberSalvagedFromSizes.push_back(0);
        }
        capture.bytesRead += attemptBytes;

        if (!readError && !torn && !refused) {
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

        if (!torn && !readError && !refused) {
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
            if (!refused) {
                capture.detail = relativeMainDb + " could not be read consistently off this stick; what is here is one "
                                                  "attempt and its parts may not agree with each other";
            }
            return capture;
        }
        updater.forgetLastEntries(appended);
        capture.entries.clear();
        capture.memberRelativePaths.clear();
        capture.memberMtimes.clear();
        capture.memberSalvagedFromSizes.clear();
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
