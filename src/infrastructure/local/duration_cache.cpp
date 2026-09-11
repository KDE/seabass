// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/duration_cache.hpp"

#include "infrastructure/paths/seabass_paths.hpp"

#include <charconv>
#include <locale>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <sstream>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/flat_json.hpp"

namespace seabass::infrastructure::local
{

namespace
{


std::string normalizeSeparators(std::string path)
{
    for (auto &c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

// std::to_string would give a locale-independent but fixed 6-decimal
// form; ostringstream with enough precision round-trips the double
// without dragging in <format> for one call.
std::string toText(double value)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss.precision(6);
    oss << std::fixed << value;
    return oss.str();
}

std::string toText(long long value)
{
    return std::to_string(value);
}

// Deliberately NOT std::stod: that honours the global C locale, and
// QCoreApplication sets it from the environment on startup. On a machine
// whose locale uses a comma decimal separator, std::stod("377.207000")
// stops at the '.' and returns 377 -- so every cached entry silently
// failed to parse in the Qt-linked CLI while parsing fine in a non-Qt
// test binary. Mirrors toText()'s own imbue(classic()) on the way out.
std::optional<double> parseDouble(const std::string &text)
{
    std::istringstream iss(text);
    iss.imbue(std::locale::classic());
    double value = 0.0;
    iss >> value;
    if (iss.fail() || !iss.eof()) {
        return std::nullopt;
    }
    return value;
}

std::optional<long long> parseLongLong(const std::string &text)
{
    long long value = 0;
    auto *begin = text.data();
    auto *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

struct FileStat
{
    long long sizeBytes = 0;
    long long mtimeSeconds = 0;
    bool ok = false;
};

FileStat statFile(const std::string &path)
{
    FileStat out;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return out;
    }
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return out;
    }
    out.sizeBytes = static_cast<long long>(size);
    // Only ever compared against another value produced the same way, so
    // the epoch this counts from does not matter -- just that it is
    // stable for an unchanged file.
    out.mtimeSeconds = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
    out.ok = true;
    return out;
}

}  // namespace

DurationCache::DurationCache(std::string stickRoot) : m_stickRoot(normalizeSeparators(std::move(stickRoot)))
{
    while (!m_stickRoot.empty() && m_stickRoot.back() == '/') {
        m_stickRoot.pop_back();
    }
    m_cachePath = paths::stickDurationCache(m_stickRoot).string();

    std::ifstream in(m_cachePath, std::ios::binary);
    if (!in) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = parseFlatObject(line);
        if (!fields) {
            continue;  // a torn or hand-edited line costs one entry, not the cache
        }
        auto path = fields->find("path");
        auto duration = fields->find("duration");
        auto size = fields->find("size");
        auto mtime = fields->find("mtime");
        if (path == fields->end() || duration == fields->end() || size == fields->end() ||
            mtime == fields->end()) {
            continue;
        }
        auto parsedDuration = parseDouble(duration->second);
        auto parsedSize = parseLongLong(size->second);
        auto parsedMtime = parseLongLong(mtime->second);
        if (!parsedDuration || !parsedSize || !parsedMtime || *parsedDuration <= 0.0) {
            continue;
        }
        m_entries[normalizeSeparators(path->second)] =
            Entry{*parsedDuration, *parsedSize, *parsedMtime};
    }
}

std::string DurationCache::relativeKey(const std::string &absoluteFilePath) const
{
    std::string path = normalizeSeparators(absoluteFilePath);
    if (m_stickRoot.empty() || path.size() <= m_stickRoot.size() + 1) {
        return {};
    }
    if (path.compare(0, m_stickRoot.size(), m_stickRoot) != 0 || path[m_stickRoot.size()] != '/') {
        return {};
    }
    return path.substr(m_stickRoot.size() + 1);
}

std::optional<double> DurationCache::lookup(const std::string &absoluteFilePath) const
{
    const std::string key = relativeKey(absoluteFilePath);
    if (key.empty()) {
        return std::nullopt;
    }
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    const FileStat current = statFile(absoluteFilePath);
    if (!current.ok || current.sizeBytes != it->second.sizeBytes ||
        current.mtimeSeconds != it->second.mtimeSeconds) {
        return std::nullopt;  // changed (or gone) since it was probed
    }
    return it->second.durationSeconds;
}

void DurationCache::store(const std::string &absoluteFilePath, double durationSeconds)
{
    if (durationSeconds <= 0.0) {
        return;
    }
    const std::string key = relativeKey(absoluteFilePath);
    if (key.empty()) {
        return;
    }
    const FileStat current = statFile(absoluteFilePath);
    if (!current.ok) {
        return;
    }
    m_entries[key] = Entry{durationSeconds, current.sizeBytes, current.mtimeSeconds};
    m_dirty = true;
}

bool DurationCache::save()
{
    if (!m_dirty) {
        return true;
    }
    std::string out;
    for (const auto &[key, entry] : m_entries) {
        out += writeFlatObject({{"path", key},
                                {"duration", toText(entry.durationSeconds)},
                                {"size", toText(entry.sizeBytes)},
                                {"mtime", toText(entry.mtimeSeconds)}});
        out += "\n";
    }
    // The cache lives in <stick>/Seabass/caches now, which may not exist
    // yet: writeFileDurablyAtomic() writes to the path it is given and
    // does not create parents, so without this every save fails silently
    // on a stick Seabass has not written to before.
    std::error_code dirEc;
    std::filesystem::create_directories(std::filesystem::path(m_cachePath).parent_path(), dirEc);
    if (!writeFileDurablyAtomic(m_cachePath, out)) {
        return false;
    }
    m_dirty = false;
    return true;
}

}  // namespace seabass::infrastructure::local
