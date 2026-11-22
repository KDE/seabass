// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/alignment_cache.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/flat_json.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::local
{

namespace fs = std::filesystem;

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

// Locale-independent both ways, for silence_cache.cpp's reason: a
// comma-decimal locale must not turn "21.3" into 21.
std::string toText(double value)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss.precision(6);
    oss << std::fixed << value;
    return oss.str();
}

std::optional<double> parseDouble(const std::string &text)
{
    std::istringstream iss(text);
    iss.imbue(std::locale::classic());
    double value = 0.0;
    iss >> value;
    if (iss.fail() || !iss.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<long long> parseLongLong(const std::string &text)
{
    long long value = 0;
    const char *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

CachedAudioAlignmentProbe::CachedAudioAlignmentProbe(std::string stickRoot,
                                                     std::unique_ptr<domain::AudioAlignmentProbe> inner,
                                                     bool innerDecodes, std::string decoder)
    : m_inner(std::move(inner)), m_innerDecodes(innerDecodes && m_inner != nullptr), m_decoder(std::move(decoder))
{
    if (stickRoot.empty()) {
        return;  // nowhere to keep it: every pair goes to `inner`
    }
    // The root as CachedAudioContentProbe takes it, and for its reasons:
    // the cache path from the root as given (a drive root keeps its
    // separator), the prefix arithmetic on the root without one.
    m_stickRoot = normalizeSeparators(paths::normalStickRoot(stickRoot));
    m_cachePath = paths::stickAlignmentCache(pathFromUtf8(m_stickRoot));
    while (m_stickRoot.size() > 1 && m_stickRoot.back() == '/') {
        m_stickRoot.pop_back();
    }

    std::ifstream in(m_cachePath, std::ios::binary);
    if (!in) {
        return;
    }
    const std::string method = std::to_string(MethodVersion);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = parseFlatObject(line);
        if (!fields) {
            continue;  // a torn or hand-edited line costs one pair, not the cache
        }
        auto field = [&](const char *name) -> std::optional<std::string> {
            const auto it = fields->find(name);
            return it == fields->end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        if (field("method") != method) {
            // Another version of the measurement: dropped, and the file
            // rewritten without it at the next save.
            m_dirty = true;
            continue;
        }
        const auto decoder = field("decoder");
        if (!decoder || decoder->empty()) {
            continue;
        }
        const auto a = field("a");
        const auto b = field("b");
        const auto result = field("result");
        const auto sizeA = parseLongLong(field("asize").value_or(""));
        const auto mtimeA = parseLongLong(field("amtime").value_or(""));
        const auto sizeB = parseLongLong(field("bsize").value_or(""));
        const auto mtimeB = parseLongLong(field("bmtime").value_or(""));
        if (!a || !b || a->empty() || b->empty() || !(*a < *b) || !result || !sizeA || !mtimeA || !sizeB
            || !mtimeB) {
            continue;
        }
        Entry entry{{*sizeA, *mtimeA}, {*sizeB, *mtimeB}, Result::Shift, 0.0};
        if (*result == "same") {
            entry.result = Result::Same;
        } else if (*result == "no") {
            entry.result = Result::NotLinedUp;
        } else if (*result == "shift") {
            const auto shift = parseDouble(field("shift").value_or(""));
            if (!shift) {
                continue;
            }
            entry.shiftMs = *shift;
        } else {
            continue;
        }
        m_entries[{*decoder, {*a, *b}}] = entry;
    }
}

CachedAudioAlignmentProbe::~CachedAudioAlignmentProbe()
{
    save();
}

std::string CachedAudioAlignmentProbe::relativeKey(const std::string &absoluteFilePath) const
{
    const std::string path = normalizeSeparators(absoluteFilePath);
    if (m_stickRoot.empty() || path.size() <= m_stickRoot.size() + 1) {
        return {};
    }
    if (path.compare(0, m_stickRoot.size(), m_stickRoot) != 0 || path[m_stickRoot.size()] != '/') {
        return {};
    }
    return path.substr(m_stickRoot.size() + 1);
}

std::optional<CachedAudioAlignmentProbe::FileState> CachedAudioAlignmentProbe::stateOf(const std::string &path)
{
    std::error_code ec;
    const auto fsPath = pathFromUtf8(path);
    const auto size = std::filesystem::file_size(fsPath, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (ec) {
        return std::nullopt;
    }
    return FileState{static_cast<long long>(size),
                     static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count())};
}

CachedAudioAlignmentProbe::Lookup CachedAudioAlignmentProbe::lookup(const std::string &absolutePathA,
                                                                    const std::string &absolutePathB) const
{
    Lookup pair;
    const std::string keyA = relativeKey(absolutePathA);
    const std::string keyB = relativeKey(absolutePathB);
    if (keyA.empty() || keyB.empty() || keyA == keyB) {
        return pair;
    }
    const auto stateA = stateOf(absolutePathA);
    const auto stateB = stateOf(absolutePathB);
    if (!stateA || !stateB) {
        return pair;
    }
    pair.reversed = keyB < keyA;
    pair.key = pair.reversed ? std::pair{keyB, keyA} : std::pair{keyA, keyB};
    pair.a = pair.reversed ? *stateB : *stateA;
    pair.b = pair.reversed ? *stateA : *stateB;
    pair.usable = true;
    return pair;
}

namespace
{
// The decoder "same bytes" lines are written with: they hold for all.
const std::string BytesDecoder = "bytes";
}  // namespace

const CachedAudioAlignmentProbe::Entry *CachedAudioAlignmentProbe::trusted(const Lookup &pair) const
{
    if (!pair.usable) {
        return nullptr;
    }
    for (const std::string *decoder : {&BytesDecoder, &m_decoder}) {
        const auto it = m_entries.find({*decoder, pair.key});
        if (it != m_entries.end() && it->second.a == pair.a && it->second.b == pair.b) {
            return &it->second;
        }
    }
    return nullptr;
}

void CachedAudioAlignmentProbe::store(const Lookup &pair, Result result, double shiftAsAsked)
{
    if (!pair.usable) {
        return;
    }
    m_entries[{result == Result::Same ? BytesDecoder : m_decoder, pair.key}] =
        Entry{pair.a, pair.b, result, pair.reversed ? -shiftAsAsked : shiftAsAsked};
    m_dirty = true;
}

bool CachedAudioAlignmentProbe::answersAtOnce(const std::string &absolutePathA, const std::string &absolutePathB)
{
    return trusted(lookup(absolutePathA, absolutePathB)) != nullptr;
}

bool CachedAudioAlignmentProbe::sameContent(const std::string &absolutePathA, const std::string &absolutePathB)
{
    const Lookup pair = lookup(absolutePathA, absolutePathB);
    if (const Entry *entry = trusted(pair)) {
        return entry->result == Result::Same;
    }
    if (m_inner == nullptr || !m_inner->sameContent(absolutePathA, absolutePathB)) {
        return false;
    }
    store(pair, Result::Same, 0.0);
    return true;
}

domain::AudioAlignmentProbe::Answer CachedAudioAlignmentProbe::align(const std::string &absolutePathA,
                                                                     const std::string &absolutePathB)
{
    const Lookup pair = lookup(absolutePathA, absolutePathB);
    Answer answer = Answer::noOpinion();
    if (const Entry *entry = trusted(pair)) {
        ++m_fromCache;
        if (entry->result == Result::NotLinedUp) {
            answer = Answer::notLinedUp();
        } else {
            const double shift = entry->result == Result::Same ? 0.0 : entry->shiftMs;
            answer = Answer::linedUp(pair.reversed ? -shift : shift);
        }
    } else if (m_inner != nullptr) {
        // Same bytes asked first, so they are stored as such: a later
        // sameContent() is then answered without reading either file.
        if (m_inner->sameContent(absolutePathA, absolutePathB)) {
            store(pair, Result::Same, 0.0);
            answer = Answer::linedUp(0.0);
        } else {
            answer = m_inner->align(absolutePathA, absolutePathB);
            if (answer.kind == Answer::Kind::LinedUp) {
                store(pair, Result::Shift, answer.shiftMs);
            } else if (answer.kind == Answer::Kind::NotLinedUp) {
                store(pair, Result::NotLinedUp, 0.0);
            }
        }
    }
    ++(answer.kind == Answer::Kind::LinedUp ? m_linedUp : m_unknown);
    return answer;
}

bool CachedAudioAlignmentProbe::save()
{
    if (!m_dirty || m_cachePath.empty()) {
        return true;
    }
    std::string out;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        // A pair either of whose files is cleanly gone is dropped, and so
        // is one either of whose files changed since: no decoder will
        // trust that line again. A stat that errors keeps it
        // (silence_cache.cpp's reasoning).
        bool drop = false;
        const std::pair<const std::string *, const FileState *> files[] = {
            {&it->first.second.first, &it->second.a}, {&it->first.second.second, &it->second.b}};
        for (const auto &[key, stored] : files) {
            const fs::path path = pathFromUtf8(m_stickRoot) / pathFromUtf8(*key);
            std::error_code ec;
            const bool present = fs::exists(path, ec);
            if (!ec && !present) {
                drop = true;
            } else if (!ec) {
                const auto now = stateOf(pathToUtf8(path));
                drop = drop || (now && !(*now == *stored));
            }
        }
        if (drop) {
            it = m_entries.erase(it);
            continue;
        }
        const Entry &entry = it->second;
        std::map<std::string, std::string> fields{
            {"decoder", it->first.first},
            {"a", it->first.second.first},
            {"asize", std::to_string(entry.a.sizeBytes)},
            {"amtime", std::to_string(entry.a.mtimeSeconds)},
            {"b", it->first.second.second},
            {"bsize", std::to_string(entry.b.sizeBytes)},
            {"bmtime", std::to_string(entry.b.mtimeSeconds)},
            {"method", std::to_string(MethodVersion)},
        };
        switch (entry.result) {
        case Result::Same:
            fields["result"] = "same";
            break;
        case Result::NotLinedUp:
            fields["result"] = "no";
            break;
        case Result::Shift:
            fields["result"] = "shift";
            fields["shift"] = toText(entry.shiftMs);
            break;
        }
        out += writeFlatObject(fields);
        out += "\n";
        ++it;
    }
    std::error_code dirEc;
    std::filesystem::create_directories(m_cachePath.parent_path(), dirEc);
    if (!writeFileDurablyAtomic(pathToUtf8(m_cachePath), out)) {
        return false;
    }
    m_dirty = false;
    return true;
}

}  // namespace seabass::infrastructure::local
