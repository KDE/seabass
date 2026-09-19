// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/silence_cache.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/flat_json.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

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

// Locale-independent both ways. std::stod and a default-imbued stream
// honour the global C locale, which QCoreApplication sets from the
// environment -- on a comma-decimal machine "0.512" parsed as 0 and
// every cached entry was silently wrong. Same fix DurationCache carries,
// and the same reason it is worth repeating rather than defaulting.
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
    // Only ever compared against another value produced the same way,
    // so the epoch it counts from does not matter, only that it is
    // stable for an unchanged file.
    out.mtimeSeconds = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
    out.ok = true;
    return out;
}

}  // namespace

CachedAudioContentProbe::CachedAudioContentProbe(std::string stickRoot,
                                                   std::unique_ptr<domain::AudioContentProbe> inner)
    : m_stickRoot(normalizeSeparators(std::move(stickRoot))), m_inner(std::move(inner))
{
    // The cache path is built from what the caller gave us, BEFORE the
    // trailing separator is stripped. Stripping first turns a stick
    // mounted at a drive root into a drive-relative path: "H:/" becomes
    // "H:", and "H:" / "Seabass/..." is "H:Seabass\..." -- resolved
    // against whatever the process's current directory on H: happens to
    // be, not against the stick. On Linux a library directly under "/"
    // goes the other way and leaves an empty root, putting the cache in
    // the process's working directory.
    m_cachePath = paths::stickSilenceCache(m_stickRoot).string();
    // Stripped only for relativeKey()'s prefix arithmetic below, which
    // is what actually needs a separator-free root.
    while (m_stickRoot.size() > 1 && m_stickRoot.back() == '/') {
        m_stickRoot.pop_back();
    }

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
        auto total = fields->find("total");
        auto lead = fields->find("lead");
        auto trail = fields->find("trail");
        auto size = fields->find("size");
        auto mtime = fields->find("mtime");
        if (path == fields->end() || total == fields->end() || lead == fields->end() ||
            trail == fields->end() || size == fields->end() || mtime == fields->end()) {
            continue;
        }
        auto parsedTotal = parseDouble(total->second);
        auto parsedLead = parseDouble(lead->second);
        auto parsedTrail = parseDouble(trail->second);
        auto parsedSize = parseLongLong(size->second);
        auto parsedMtime = parseLongLong(mtime->second);
        if (!parsedTotal || !parsedLead || !parsedTrail || !parsedSize || !parsedMtime) {
            continue;
        }
        // A total of zero is not a measurement, and negative silences
        // are not a shape this ever writes. Both would come from a
        // hand-edited or truncated file, and either would make a wrong
        // content length look like a real one.
        if (*parsedTotal <= 0.0 || *parsedLead < 0.0 || *parsedTrail < 0.0) {
            continue;
        }
        m_entries[normalizeSeparators(path->second)] =
            Entry{*parsedTotal, *parsedLead, *parsedTrail, *parsedSize, *parsedMtime};
    }
}

CachedAudioContentProbe::~CachedAudioContentProbe()
{
    // A caller that never calls save() still keeps what was decoded.
    // Failure is ignored on purpose: a read-only stick is ordinary, and
    // a destructor is no place to report it.
    save();
}

std::string CachedAudioContentProbe::relativeKey(const std::string &absoluteFilePath) const
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

std::optional<domain::AudioContentSpan> CachedAudioContentProbe::measure(const std::string &absoluteFilePath)
{
    const std::string key = relativeKey(absoluteFilePath);
    const FileStat current = statFile(absoluteFilePath);

    if (!key.empty() && current.ok) {
        auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.sizeBytes == current.sizeBytes &&
            it->second.mtimeSeconds == current.mtimeSeconds) {
            domain::AudioContentSpan span;
            span.totalSeconds = it->second.totalSeconds;
            span.leadingSilenceSeconds = it->second.leadingSilenceSeconds;
            span.trailingSilenceSeconds = it->second.trailingSilenceSeconds;
            return span;
        }
    }

    if (m_inner == nullptr) {
        return std::nullopt;
    }
    // Asked and refused already, this run. Not written to the cache
    // file (see m_failed's own comment) but not re-attempted either:
    // the finder asks once per pair, so without this a file in a group
    // of five copies is decoded four times over, each attempt paying
    // the full cost of failing.
    if (m_failed.count(absoluteFilePath) > 0) {
        return std::nullopt;
    }
    auto measured = m_inner->measure(absoluteFilePath);
    if (!measured) {
        m_failed.insert(absoluteFilePath);
        return std::nullopt;
    }
    // Counted only now. decodedCount() is printed as "compared the
    // audio of N files", so it has to mean files that were actually
    // read, not files that were attempted.
    ++m_decoded;
    if (!key.empty() && current.ok && measured->totalSeconds > 0.0) {
        m_entries[key] = Entry{measured->totalSeconds, measured->leadingSilenceSeconds,
                                measured->trailingSilenceSeconds, current.sizeBytes, current.mtimeSeconds};
        m_dirty = true;
    }
    return measured;
}

bool CachedAudioContentProbe::save()
{
    if (!m_dirty) {
        return true;
    }
    std::string out;
    // Entries whose file is gone are dropped on the way out rather than
    // written again. Without this the file only ever grows: every
    // re-rip, rename and deletion leaves a row that is re-parsed on
    // every future scan of the stick and can never match anything.
    // Cheap, because a save only happens when something was measured.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        std::error_code existsEc;
        const bool present = std::filesystem::exists(
            std::filesystem::path(m_stickRoot) / it->first, existsEc);
        // A stat that ERRORS is not a missing file: an unreadable
        // directory or a stick pulled mid-save would otherwise empty the
        // cache. Only a clean "no" drops a row.
        if (!existsEc && !present) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &[key, entry] : m_entries) {
        out += writeFlatObject({{"path", key},
                                {"total", toText(entry.totalSeconds)},
                                {"lead", toText(entry.leadingSilenceSeconds)},
                                {"trail", toText(entry.trailingSilenceSeconds)},
                                {"size", toText(entry.sizeBytes)},
                                {"mtime", toText(entry.mtimeSeconds)}});
        out += "\n";
    }
    // <stick>/Seabass/caches may not exist yet, and
    // writeFileDurablyAtomic() does not create parents -- without this
    // every save fails silently on a stick Seabass has not written to.
    std::error_code dirEc;
    std::filesystem::create_directories(std::filesystem::path(m_cachePath).parent_path(), dirEc);
    if (!writeFileDurablyAtomic(m_cachePath, out)) {
        return false;
    }
    m_dirty = false;
    return true;
}

}  // namespace seabass::infrastructure::local
