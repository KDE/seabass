// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace seabass::application
{

// One instance's claim on a library for editing -- see
// docs/edit-mode-and-cancel.md. Held from the first staged change until
// the changes are saved or discarded; other instances show the library
// as read-only meanwhile.
//
// Lives in local app data, never on the stick: a cookie on the stick
// would resurface days later, on another machine, as a stale lock nobody
// can explain, whereas a local cookie can name the owning process so
// staleness is a fact (the pid is dead), not a guess.
struct LibraryEditLock
{
    std::string libraryId;    // StickIdentity::libraryId()
    std::string instanceId;   // random per process, so a restart of the same pid number is a new instance
    std::string hostname;
    std::string stickLabel;   // for the "locked by ..." message only
    std::string mountPoint;
    std::int64_t pid = 0;
    std::uint64_t processStartId = 0;  // guards against pid reuse; 0 when the platform cannot tell
    std::string startedAtUtc;          // ISO 8601, for display
    std::int64_t heartbeatUnix = 0;    // seconds; the only field that changes after acquisition
};

enum class EditLockStatus {
    Free,
    HeldByThisInstance,
    HeldByOther,
    Stale,  // a cookie exists but its owner is provably gone (or, from another host, silent for too long)
};

struct EditLockProbe
{
    EditLockStatus status = EditLockStatus::Free;
    std::optional<LibraryEditLock> holder;  // set for HeldByThisInstance/HeldByOther/Stale when the cookie parsed
};

class LibraryEditLockStore
{
public:
    virtual ~LibraryEditLockStore() = default;

    virtual EditLockProbe probe(const std::string &libraryId, const std::string &myInstanceId) = 0;

    // Atomically creates the cookie. Replaces a Stale cookie and one held
    // by this same instance; returns false (nothing written) when another
    // live instance holds it.
    virtual bool tryAcquire(const LibraryEditLock &lock) = 0;

    // Refreshes heartbeatUnix. No-op unless the cookie is ours.
    virtual void heartbeat(const std::string &libraryId, const std::string &instanceId) = 0;

    // Removes the cookie if it is ours; leaves another instance's alone.
    virtual void release(const std::string &libraryId, const std::string &instanceId) = 0;

    // "Remove Lock": removes whatever cookie exists, whoever holds it.
    virtual void forceRemove(const std::string &libraryId) = 0;

    virtual std::vector<LibraryEditLock> listAll() = 0;
};

}  // namespace seabass::application
