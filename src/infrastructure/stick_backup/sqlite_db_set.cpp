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
#include "infrastructure/paths/shared_read_stream.hpp"
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
    SharedReadStream in(path);
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
    SharedReadStream in(path);
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

namespace
{

// Asks about a -wal or -journal, and asks again while the answer is
// "access denied". On Windows a file deleted while somebody still has it
// open stays in the directory until the last handle closes, and until
// then every stat and open of it is refused with access denied rather
// than "not there". A rollback journal is deleted by every commit, so a
// set being written answers that way for a moment, over and over, and
// the moment ends in one of the two answers the callers know: gone, or
// the next transaction's journal. Asked once, it read as a set that
// could not be read: 9 to 20 of 400 captures of a database being written
// (backup_database_capture_test case 4c on Windows, 2026-11-18).
// A sidecar that is refused for longer than this is refused for real,
// and the answer stays the error it was. 63 ms in all.
template <typename Ask>
auto askSidecar(std::error_code &ec, Ask ask)
{
    auto answer = ask(ec);
    for (const int waitMs : {1, 2, 4, 8, 16, 32}) {
        if (ec != std::errc::permission_denied) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        answer = ask(ec);
    }
    return answer;
}

bool sidecarIsThere(const fs::path &sidecar, std::error_code &ec)
{
    return askSidecar(ec, [&sidecar](std::error_code &asked) { return fs::is_regular_file(sidecar, asked); });
}

std::uintmax_t sidecarSize(const fs::path &sidecar, std::error_code &ec)
{
    return askSidecar(ec, [&sidecar](std::error_code &asked) { return fs::file_size(sidecar, asked); });
}

// What listing a set learned about its sidecars.
//
// Refused is "access denied" that outlasted askSidecar's waiting. On
// Windows that is still a file on its way out, held open a little longer
// by whoever else has a handle on it -- a virus scanner, an indexer, a
// slow machine -- and so still the writer. It is not a set that cannot
// be read, and it is told apart from one here: any other failure of the
// stat is Unknown, and stays a read error.
enum class SidecarPresence
{
    Known,
    Refused,
    Unknown,
};

std::vector<fs::path> listDbSet(const fs::path &mainDb, SidecarPresence &presence)
{
    presence = SidecarPresence::Known;
    std::vector<fs::path> members{mainDb};
    for (const char *suffix : {"-wal", "-journal"}) {
        fs::path sidecar = mainDb;
        sidecar += suffix;
        std::error_code ec;
        const bool there = sidecarIsThere(sidecar, ec);
        if (!absenceIsCertain(ec)) {
            if (ec != std::errc::permission_denied) {
                presence = SidecarPresence::Unknown;
            } else if (presence == SidecarPresence::Known) {
                presence = SidecarPresence::Refused;
            }
        }
        if (there) {
            members.push_back(sidecar);
        }
    }
    return members;
}

}  // namespace

std::vector<fs::path> dbSetMembers(const fs::path &mainDb, bool *presenceKnown)
{
    SidecarPresence presence = SidecarPresence::Known;
    std::vector<fs::path> members = listDbSet(mainDb, presence);
    if (presenceKnown != nullptr) {
        *presenceKnown = presence == SidecarPresence::Known;
    }
    return members;
}

bool dbSetSidecarRefused(const fs::path &mainDb)
{
    // The header as fingerprintDbSet() reads it, so that what is a
    // database here is one there.
    unsigned char header[100];
    if (!readPrefix(mainDb, header, sizeof header) || std::memcmp(header, SqliteMagic, sizeof SqliteMagic) != 0) {
        return false;
    }
    SidecarPresence presence = SidecarPresence::Known;
    listDbSet(mainDb, presence);
    return presence == SidecarPresence::Refused;
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
    const bool walThere = sidecarIsThere(wal, sidecarEc);
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
    const bool journalThere = sidecarIsThere(journal, sidecarEc);
    if (!absenceIsCertain(sidecarEc)) {
        return std::nullopt;
    }
    if (journalThere) {
        fp.hasJournal = true;
        fp.journalSize = fs::file_size(journal, ec);
    }
    return fp;
}

std::optional<hashing::Sha256Digest> hashDbSet(const fs::path &mainDb)
{
    bool presenceKnown = true;
    const std::vector<fs::path> members = dbSetMembers(mainDb, &presenceKnown);
    if (!presenceKnown) {
        return std::nullopt;
    }
    const std::string mainName = pathToGenericUtf8(mainDb.filename());
    hashing::Sha256 set;
    for (const fs::path &member : members) {
        const std::optional<hashing::Sha256Digest> bytes = hashFile(member);
        if (!bytes) {
            return std::nullopt;
        }
        set.update(pathToGenericUtf8(member.filename()).substr(mainName.size()));
        set.update(std::string_view("\0", 1));
        set.update(std::as_bytes(std::span<const std::uint8_t>(bytes->data(), bytes->size())));
    }
    return set.finish();
}

DbSetCapture captureDbSet(const fs::path &stickRoot, const std::string &relativeMainDb, ArchiveUpdater &updater, int retries,
                          const std::function<void(std::uint64_t)> &progress, bool salvage,
                          const std::function<std::optional<std::uint64_t>(const std::string &relativePath)>
                              &readLimitForTesting)
{
    DbSetCapture capture;
    const fs::path mainDb = stickRoot / seabass::pathFromUtf8(relativeMainDb);
    // Members are the main file plus a suffix, so their archive names are
    // the main file's name plus the same suffix -- no path arithmetic.
    auto relativeNameOf = [&](const fs::path &member) {
        return relativeMainDb + pathToGenericUtf8(member.filename()).substr(pathToGenericUtf8(mainDb.filename()).size());
    };
    std::error_code ec;
    const std::string membershipUnknown = relativeMainDb + ": could not tell whether its -wal or -journal is there";
    SidecarPresence declaredPresence = SidecarPresence::Known;
    const std::vector<fs::path> declared = listDbSet(mainDb, declaredPresence);
    if (declaredPresence == SidecarPresence::Unknown) {
        // A set whose membership could not be established is not one to
        // capture: the WAL may be there and unseen, and a main file
        // taken alone restores against a WAL whose salts do not match.
        capture.status = DbSetCapture::Status::ReadError;
        capture.detail = membershipUnknown;
        return capture;
    }
    // A sidecar that is refused is not captured either, and for the same
    // reason. But it is the writer, so the attempts below are what it is
    // owed: each lists the set afresh, and one that is still refused is
    // an attempt that tore. Returned from here as a read error, the
    // database was left out of the backup with nothing tried again -- 3
    // of 400 captures of a set being written, on a machine slowed to a
    // third of its speed with six other tests running (Windows Round 13,
    // 2026-11-20).
    for (const fs::path &member : declared) {
        std::error_code sizeEc;
        const std::uint64_t size = member == mainDb ? fs::file_size(member, sizeEc) : sidecarSize(member, sizeEc);
        if (member != mainDb
            && (sizeEc == std::errc::no_such_file_or_directory || sizeEc == std::errc::permission_denied)) {
            // A sidecar that was listed and is gone is a rollback journal
            // deleted by a commit: the writer the attempts below exist
            // for, not a set that could not be read. It has
            // no size to hold against the ceiling, and every attempt
            // lists the set afresh. Refused here, one capture in twenty
            // of a database being written left it out of the backup
            // with nothing tried (71 of 1500, measured 2026-11-13).
            //
            // One that is refused instead is the same journal, deleted
            // in this gap with somebody else's handle still on it.
            continue;
        }
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

    bool sidecarRefused = false;  // the last attempt's listing, for what an unstable set says
    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
        }
        std::optional<DbSetFingerprint> before = fingerprintDbSet(mainDb);
        SidecarPresence presence = SidecarPresence::Known;
        std::vector<fs::path> members = listDbSet(mainDb, presence);
        capture.entries.clear();
        capture.memberRelativePaths.clear();
        capture.memberMtimes.clear();
        capture.memberSalvagedFromSizes.clear();
        capture.detail.clear();
        // Established once before the first attempt, so a listing that
        // is unsure of a sidecar now is a set that moved: the attempt
        // tore, whatever the stat said.
        const bool sidecarUnlisted = presence != SidecarPresence::Known;
        sidecarRefused = presence == SidecarPresence::Refused;
        if (sidecarUnlisted && !(salvage && attempt == retries)) {
            // Nothing is read for an attempt that cannot be kept: the
            // set as listed may lack a WAL that is there. A salvage
            // run's last attempt reads what it can all the same, and is
            // kept as what it is.
            continue;
        }
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
            const std::uintmax_t sizeNow = m == 0 ? fs::file_size(member, sizeEc) : sidecarSize(member, sizeEc);
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
            // A sidecar that is gone by now went while it was being
            // read. The read that ran into that can look like a refusal
            // (the journal of the next transaction is already there
            // under the same name, longer than what was read of this
            // one), and it is the writer all the same: a device that
            // refuses does not delete the file.
            const bool sidecarGone = m > 0 && sizeEc == std::errc::no_such_file_or_directory;
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
                //
                // A refusal is told from an ending by asking the file
                // how long it is after a short read, and a journal that
                // a writer is filling, deleting and starting again says
                // "longer" as readily as a device that stopped does. The
                // set tells them apart: a device that refuses changes
                // nothing, and a writer cannot regrow or replace a
                // journal without moving its size or the main file's
                // change counter. So a refusal with the fingerprint
                // moved since this attempt began is the writer. Called a
                // read error, the database was left out of the backup
                // with nothing tried again: one run in thirty of
                // backup_database_capture_test's 400 captures of a set
                // being written (4 of 120 runs, measured 2026-11-14).
                const std::optional<DbSetFingerprint> now = fingerprintDbSet(mainDb);
                const bool setMoved = before && now && !(*before == *now);
                if (source.refused() && !sidecarGone && !setMoved) {
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
        // Read with a sidecar out of reach, which only a salvage run's last
        // attempt does: never a clean capture, whatever the passes say.
        torn = torn || sidecarUnlisted;

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
            if (sidecarUnlisted) {
                // Said, because a restore removes a sidecar the archive
                // does not hold: what the -wal had and the main file
                // did not yet is not in this backup.
                note("a -wal or -journal beside it was out of reach and is not in this backup");
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
    capture.detail = sidecarRefused
                         ? relativeMainDb + ": its -wal or -journal was out of reach (access denied) when last tried; "
                                            "another program may be holding it"
                         : relativeMainDb + " kept changing while being read";
    return capture;
}

}  // namespace seabass::infrastructure::stick_backup
