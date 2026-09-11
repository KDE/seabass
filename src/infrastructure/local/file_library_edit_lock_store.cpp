#include "infrastructure/local/file_library_edit_lock_store.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "application/stick_identity.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/app_data_directory.hpp"
#include "infrastructure/local/flat_json.hpp"
#include "infrastructure/system/process_liveness.hpp"

namespace seabass::infrastructure::local
{

namespace fs = std::filesystem;
using application::EditLockProbe;
using application::EditLockStatus;
using application::LibraryEditLock;

namespace
{

constexpr const char *CookieExtension = ".json";

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string isoTimestampUtc(std::int64_t unixSeconds)
{
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::int64_t toInt(const std::map<std::string, std::string> &fields, const char *key)
{
    auto it = fields.find(key);
    if (it == fields.end()) {
        return 0;
    }
    try {
        return std::stoll(it->second);
    } catch (const std::exception &) {
        return 0;
    }
}

std::string toString(const std::map<std::string, std::string> &fields, const char *key)
{
    auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
}

// Seconds since the file was last written, by the filesystem's clock;
// a large value when the file is gone or unreadable.
std::int64_t ageSeconds(const fs::path &path)
{
    std::error_code ec;
    auto written = fs::last_write_time(path, ec);
    if (ec) {
        return 1LL << 40;
    }
    auto now = fs::file_time_type::clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - written).count();
}

}  // namespace

FileLibraryEditLockStore::FileLibraryEditLockStore(fs::path directory, LivenessFn liveness, ClockFn clock,
                                                   std::string hostName)
    : m_directory(std::move(directory)),
      m_liveness(liveness ? std::move(liveness) : LivenessFn(&system::isProcessAlive)),
      m_clock(clock ? std::move(clock) : ClockFn(&unixNow)),
      m_hostName(hostName.empty() ? system::hostName() : std::move(hostName))
{
}

fs::path FileLibraryEditLockStore::defaultDirectory()
{
    return appDataDirectory() / "edit-locks";
}

fs::path FileLibraryEditLockStore::pathFor(const std::string &libraryId) const
{
    return m_directory / (application::StickIdentity::sanitizeForFileName(libraryId) + CookieExtension);
}

std::string FileLibraryEditLockStore::serialize(const LibraryEditLock &lock)
{
    std::map<std::string, std::string> fields = {
        {"libraryId", lock.libraryId},
        {"instanceId", lock.instanceId},
        {"hostname", lock.hostname},
        {"stickLabel", lock.stickLabel},
        {"mountPoint", lock.mountPoint},
        {"pid", std::to_string(lock.pid)},
        {"processStartId", std::to_string(lock.processStartId)},
        {"startedAtUtc", lock.startedAtUtc},
        {"heartbeatUnix", std::to_string(lock.heartbeatUnix)},
    };
    return writeFlatObject(fields) + "\n";
}

std::optional<LibraryEditLock> FileLibraryEditLockStore::deserialize(const std::string &text)
{
    auto fields = parseFlatObject(text);
    if (!fields) {
        return std::nullopt;
    }
    LibraryEditLock lock;
    lock.libraryId = toString(*fields, "libraryId");
    lock.instanceId = toString(*fields, "instanceId");
    lock.hostname = toString(*fields, "hostname");
    lock.stickLabel = toString(*fields, "stickLabel");
    lock.mountPoint = toString(*fields, "mountPoint");
    lock.pid = toInt(*fields, "pid");
    lock.processStartId = static_cast<std::uint64_t>(toInt(*fields, "processStartId"));
    lock.startedAtUtc = toString(*fields, "startedAtUtc");
    lock.heartbeatUnix = toInt(*fields, "heartbeatUnix");
    if (lock.libraryId.empty() || lock.instanceId.empty()) {
        return std::nullopt;
    }
    return lock;
}

std::optional<LibraryEditLock> FileLibraryEditLockStore::readCookie(const fs::path &path) const
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return deserialize(buffer.str());
}

bool FileLibraryEditLockStore::isStale(const LibraryEditLock &lock) const
{
    if (lock.hostname == m_hostName) {
        return !m_liveness(lock.pid, lock.processStartId);
    }
    return m_clock() - lock.heartbeatUnix > ForeignHostStaleAfterSeconds;
}

EditLockProbe FileLibraryEditLockStore::probe(const std::string &libraryId, const std::string &myInstanceId)
{
    EditLockProbe result;
    fs::path path = pathFor(libraryId);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return result;
    }
    auto lock = readCookie(path);
    if (!lock) {
        // Mid-write by another instance, or torn by a crash: only its age
        // can tell those apart.
        result.status = ageSeconds(path) > UnparseableStaleAfterSeconds ? EditLockStatus::Stale
                                                                         : EditLockStatus::HeldByOther;
        return result;
    }
    result.holder = lock;
    if (lock->instanceId == myInstanceId) {
        result.status = EditLockStatus::HeldByThisInstance;
    } else if (isStale(*lock)) {
        result.status = EditLockStatus::Stale;
    } else {
        result.status = EditLockStatus::HeldByOther;
    }
    return result;
}

bool FileLibraryEditLockStore::createExclusive(const fs::path &path, const std::string &body) const
{
#if defined(_WIN32)
    HANDLE handle = ::CreateFileA(path.string().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = ::WriteFile(handle, body.data(), static_cast<DWORD>(body.size()), &written, nullptr)
        && written == body.size();
    ::CloseHandle(handle);
    return ok;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (offset < body.size()) {
        ssize_t n = ::write(fd, body.data() + offset, body.size() - offset);
        if (n <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(n);
    }
    ::close(fd);
    return ok;
#endif
}

bool FileLibraryEditLockStore::tryAcquire(const LibraryEditLock &lockIn)
{
    LibraryEditLock lock = lockIn;
    if (lock.hostname.empty()) {
        lock.hostname = m_hostName;
    }
    if (lock.heartbeatUnix == 0) {
        lock.heartbeatUnix = m_clock();
    }
    if (lock.startedAtUtc.empty()) {
        lock.startedAtUtc = isoTimestampUtc(lock.heartbeatUnix);
    }

    std::error_code ec;
    fs::create_directories(m_directory, ec);
    fs::path path = pathFor(lock.libraryId);
    std::string body = serialize(lock);

    // Two attempts: the first may lose to an existing cookie that turns
    // out to be stale (or our own), which is removed before the second.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (createExclusive(path, body)) {
            return true;
        }
        EditLockProbe existing = probe(lock.libraryId, lock.instanceId);
        if (existing.status == EditLockStatus::HeldByOther) {
            return false;
        }
        if (existing.status == EditLockStatus::Free) {
            continue;  // vanished between the create and the probe; retry
        }
        // Compare-and-delete, not check-then-delete: between the probe
        // and the remove another instance may have replaced the stale
        // cookie with its own live one. Removing that would let two
        // instances both believe they hold the lock.
        if (auto now = readCookie(path); now && existing.holder && now->instanceId != existing.holder->instanceId) {
            continue;
        }
        fs::remove(path, ec);
    }
    return false;
}

void FileLibraryEditLockStore::heartbeat(const std::string &libraryId, const std::string &instanceId)
{
    fs::path path = pathFor(libraryId);
    auto lock = readCookie(path);
    if (!lock || lock->instanceId != instanceId) {
        return;
    }
    lock->heartbeatUnix = m_clock();
    // Atomic replace: a reader never sees a torn heartbeat rewrite.
    writeFileDurablyAtomic(path.string(), serialize(*lock));
}

void FileLibraryEditLockStore::release(const std::string &libraryId, const std::string &instanceId)
{
    fs::path path = pathFor(libraryId);
    auto lock = readCookie(path);
    if (!lock || lock->instanceId != instanceId) {
        return;
    }
    std::error_code ec;
    fs::remove(path, ec);
}

void FileLibraryEditLockStore::forceRemove(const std::string &libraryId)
{
    std::error_code ec;
    fs::remove(pathFor(libraryId), ec);
}

std::vector<LibraryEditLock> FileLibraryEditLockStore::listAll()
{
    std::vector<LibraryEditLock> locks;
    std::error_code ec;
    if (!fs::is_directory(m_directory, ec)) {
        return locks;
    }
    for (const auto &entry : fs::directory_iterator(m_directory, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != CookieExtension) {
            continue;
        }
        if (auto lock = readCookie(entry.path())) {
            locks.push_back(*lock);
        }
    }
    return locks;
}

}  // namespace seabass::infrastructure::local
