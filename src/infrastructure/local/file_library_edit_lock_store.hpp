// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "application/ports/library_edit_lock_store.hpp"

namespace seabass::infrastructure::local
{

// One JSON cookie per library under a local directory (by default
// appDataDirectory()/"edit-locks"), named after the sanitised library id.
//
// Acquisition is an atomic exclusive create of the final file (O_EXCL /
// CREATE_NEW), so two instances racing for the same library cannot both
// win. The body is written right after the create; a reader that catches
// the file in that gap (or a torn one after a crash) sees an unparseable
// cookie and treats it as HeldByOther while it is younger than a few
// seconds, Stale after that.
//
// Staleness: same host and the recorded process is gone (pid dead, or
// pid recycled with a different start id) -- provable, so the cookie is
// replaced silently; other host (only possible through a shared home
// directory) and no heartbeat for ten minutes.
class FileLibraryEditLockStore : public application::LibraryEditLockStore
{
public:
    using LivenessFn = std::function<bool(std::int64_t pid, std::uint64_t startId)>;
    using ClockFn = std::function<std::int64_t()>;  // unix seconds

    static constexpr std::int64_t ForeignHostStaleAfterSeconds = 10 * 60;
    static constexpr std::int64_t UnparseableStaleAfterSeconds = 5;

    // liveness/clock/hostName default to the real ones; tests inject
    // fakes.
    explicit FileLibraryEditLockStore(std::filesystem::path directory, LivenessFn liveness = {},
                                      ClockFn clock = {}, std::string hostName = {});

    static std::filesystem::path defaultDirectory();

    application::EditLockProbe probe(const std::string &libraryId, const std::string &myInstanceId) override;
    bool tryAcquire(const application::LibraryEditLock &lock) override;
    void heartbeat(const std::string &libraryId, const std::string &instanceId) override;
    void release(const std::string &libraryId, const std::string &instanceId) override;
    void forceRemove(const std::string &libraryId) override;
    std::vector<application::LibraryEditLock> listAll() override;

    std::filesystem::path pathFor(const std::string &libraryId) const;

    // Serialisation, exposed for tests.
    static std::string serialize(const application::LibraryEditLock &lock);
    static std::optional<application::LibraryEditLock> deserialize(const std::string &text);

private:
    std::optional<application::LibraryEditLock> readCookie(const std::filesystem::path &path) const;
    bool isStale(const application::LibraryEditLock &lock) const;
    bool createExclusive(const std::filesystem::path &path, const std::string &body) const;

    std::filesystem::path m_directory;
    LivenessFn m_liveness;
    ClockFn m_clock;
    std::string m_hostName;
};

}  // namespace seabass::infrastructure::local
