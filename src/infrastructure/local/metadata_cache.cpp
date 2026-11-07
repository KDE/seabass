// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/metadata_cache.hpp"

#include "infrastructure/paths/seabass_paths.hpp"

#include <charconv>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <locale>
#include <sstream>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/flat_json.hpp"
#include "infrastructure/paths/utf8_path.hpp"

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

// See DurationCache's own toText(): ostringstream with an explicit
// classic locale, so a machine whose locale uses a comma decimal
// separator still writes "266.376000" and can read it back.
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

// Deliberately NOT std::stod, for the locale reason spelled out in
// DurationCache's own parseDouble().
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

// Missing string fields are absent metadata, not a corrupt line: a file
// with no artist tag is completely ordinary, and dropping its entry
// would mean re-reading that file on every single scan forever.
std::string fieldOrEmpty(const std::map<std::string, std::string> &fields, const char *key)
{
    auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
}

long long intFieldOrZero(const std::map<std::string, std::string> &fields, const char *key)
{
    auto it = fields.find(key);
    if (it == fields.end()) {
        return 0;
    }
    auto parsed = parseLongLong(it->second);
    return parsed ? *parsed : 0;
}

// The "mp3leadin" field (#50): absent when the entry was written before
// the probe read lead-ins (or for a file that is not an MP3), "-" when
// the probe looked and found none it could read, otherwise a version
// token and the nine fields of domain::Mp3LeadIn separated by spaces:
//   "1 <info> <tag N|L|C|O> <crcValid> <delay> <padding> <modeMatches> <vbri> <rate> <samplesPerFrame>"
// A value this build cannot parse reads as absent, so the file is
// probed again rather than taken to have no lead-in.
constexpr const char *LeadInNone = "-";

std::string leadInText(const std::optional<domain::Mp3LeadIn> &leadIn)
{
    if (!leadIn) {
        return LeadInNone;
    }
    const char tag = leadIn->tag == domain::Mp3LeadIn::Tag::Lame   ? 'L'
                     : leadIn->tag == domain::Mp3LeadIn::Tag::Lavc ? 'C'
                     : leadIn->tag == domain::Mp3LeadIn::Tag::Other ? 'O'
                                                                    : 'N';
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "1 " << int(leadIn->infoFrame) << ' ' << tag << ' ' << int(leadIn->lameCrcValid) << ' '
        << leadIn->encoderDelay << ' ' << leadIn->encoderPadding << ' ' << int(leadIn->infoFrameModeMatchesAudio)
        << ' ' << int(leadIn->vbri) << ' ' << leadIn->sampleRate << ' ' << leadIn->samplesPerFrame;
    return out.str();
}

// True when `text` is a lead-in this build reads, with `out` filled.
bool parseLeadIn(const std::string &text, application::FileMetadata &out)
{
    if (text == LeadInNone) {
        out.mp3LeadInRead = true;
        out.mp3LeadIn.reset();
        return true;
    }
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    int version = 0;
    int info = 0;
    char tag = 0;
    int crc = 0;
    int modes = 0;
    int vbri = 0;
    domain::Mp3LeadIn leadIn;
    if (!(in >> version >> info >> tag >> crc >> leadIn.encoderDelay >> leadIn.encoderPadding >> modes >> vbri
          >> leadIn.sampleRate >> leadIn.samplesPerFrame)
        || version != 1) {
        return false;
    }
    switch (tag) {
    case 'N':
        leadIn.tag = domain::Mp3LeadIn::Tag::None;
        break;
    case 'L':
        leadIn.tag = domain::Mp3LeadIn::Tag::Lame;
        break;
    case 'C':
        leadIn.tag = domain::Mp3LeadIn::Tag::Lavc;
        break;
    case 'O':
        leadIn.tag = domain::Mp3LeadIn::Tag::Other;
        break;
    default:
        return false;
    }
    leadIn.infoFrame = info != 0;
    leadIn.lameCrcValid = crc != 0;
    leadIn.infoFrameModeMatchesAudio = modes != 0;
    leadIn.vbri = vbri != 0;
    out.mp3LeadInRead = true;
    out.mp3LeadIn = leadIn;
    return true;
}

}  // namespace

MetadataCache::MetadataCache(std::string stickRoot) : m_stickRoot(normalizeSeparators(std::move(stickRoot)))
{
    while (!m_stickRoot.empty() && m_stickRoot.back() == '/') {
        m_stickRoot.pop_back();
    }
    m_cachePath = paths::stickMetadataCache(pathFromUtf8(m_stickRoot));

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
        if (!parsedDuration || !parsedSize || !parsedMtime || *parsedDuration < 0.0) {
            continue;
        }

        application::FileMetadata metadata;
        metadata.title = fieldOrEmpty(*fields, "title");
        metadata.artist = fieldOrEmpty(*fields, "artist");
        metadata.album = fieldOrEmpty(*fields, "album");
        metadata.durationSeconds = *parsedDuration;
        metadata.bitrate = static_cast<int>(intFieldOrZero(*fields, "bitrate"));
        metadata.sampleRate = static_cast<int>(intFieldOrZero(*fields, "samplerate"));
        // Absent reads as "estimated", not as "exact". An entry written
        // by some future version that dropped the field, or a
        // hand-edited line, must not be able to promote a guess into a
        // fact that a caller will then delete a file over.
        auto estimated = fields->find("estimated");
        metadata.durationIsEstimated = estimated == fields->end() || estimated->second != "0";
        if (auto leadIn = fields->find("mp3leadin"); leadIn != fields->end()) {
            parseLeadIn(leadIn->second, metadata);
        }
        // No duration is only good for an entry that holds a lead-in.
        if (*parsedDuration == 0.0 && !metadata.mp3LeadInRead) {
            continue;
        }

        m_entries[normalizeSeparators(path->second)] = Entry{std::move(metadata), *parsedSize, *parsedMtime};
    }
}

std::string MetadataCache::relativeKey(const std::string &absoluteFilePath) const
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

std::optional<application::FileMetadata> MetadataCache::lookupLeadIn(const std::string &absoluteFilePath,
                                                                     bool checkFile) const
{
    const std::string key = relativeKey(absoluteFilePath);
    if (key.empty()) {
        return std::nullopt;
    }
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    if (checkFile) {
        const FileStat current = statFile(absoluteFilePath);
        if (!current.ok || current.sizeBytes != it->second.sizeBytes ||
            current.mtimeSeconds != it->second.mtimeSeconds) {
            return std::nullopt;  // changed (or gone) since it was read
        }
    }
    return it->second.metadata;
}

std::optional<application::FileMetadata> MetadataCache::lookupByPathOnly(const std::string &absoluteFilePath) const
{
    auto found = lookupLeadIn(absoluteFilePath, false);
    if (found && found->durationSeconds <= 0.0) {
        return std::nullopt;  // a lead-in alone
    }
    return found;
}

std::optional<application::FileMetadata> MetadataCache::lookup(const std::string &absoluteFilePath) const
{
    auto found = lookupLeadIn(absoluteFilePath, true);
    if (found && found->durationSeconds <= 0.0) {
        return std::nullopt;  // a lead-in alone
    }
    return found;
}

void MetadataCache::store(const std::string &absoluteFilePath, const application::FileMetadata &metadata)
{
    if (metadata.durationSeconds <= 0.0 && !metadata.mp3LeadInRead) {
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
    m_entries[key] = Entry{metadata, current.sizeBytes, current.mtimeSeconds};
    m_dirty = true;
}

bool MetadataCache::save()
{
    if (!m_dirty) {
        return true;
    }
    std::string out;
    for (const auto &[key, entry] : m_entries) {
        std::map<std::string, std::string> fields{{"path", key},
                                {"title", entry.metadata.title},
                                {"artist", entry.metadata.artist},
                                {"album", entry.metadata.album},
                                {"duration", toText(entry.metadata.durationSeconds)},
                                {"estimated", entry.metadata.durationIsEstimated ? "1" : "0"},
                                {"bitrate", toText(static_cast<long long>(entry.metadata.bitrate))},
                                {"samplerate", toText(static_cast<long long>(entry.metadata.sampleRate))},
                                {"size", toText(entry.sizeBytes)},
                                {"mtime", toText(entry.mtimeSeconds)}};
        if (entry.metadata.mp3LeadInRead) {
            fields["mp3leadin"] = leadInText(entry.metadata.mp3LeadIn);
        }
        out += writeFlatObject(fields);
        out += "\n";
    }
    // The cache lives in <stick>/Seabass/caches now, which may not exist
    // yet: writeFileDurablyAtomic() writes to the path it is given and
    // does not create parents, so without this every save fails silently
    // on a stick Seabass has not written to before.
    std::error_code dirEc;
    std::filesystem::create_directories(m_cachePath.parent_path(), dirEc);
    if (!writeFileDurablyAtomic(pathToUtf8(m_cachePath), out)) {
        return false;
    }
    m_dirty = false;
    return true;
}

}  // namespace seabass::infrastructure::local
