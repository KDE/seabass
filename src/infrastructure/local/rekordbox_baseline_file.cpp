// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/rekordbox_baseline_file.hpp"

#include <charconv>
#include <climits>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <zlib.h>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::infrastructure::local
{

namespace
{

namespace fs = std::filesystem;
using domain::BaselineCue;
using domain::BaselinePlaylist;
using domain::BaselineTrack;
using domain::CuePoint;
using domain::RekordboxBaseline;
using domain::ValueOrigin;
using stick_backup::escapeManifestField;
using stick_backup::unescapeManifestField;

constexpr std::string_view Magic = "seabass-rekordbox-baseline";
constexpr std::string_view TrailerPrefix = "#sha256\t";
constexpr std::size_t HeaderFieldCount = 6;

// What the full read inflates at most. A 1500-track stick is well under
// a megabyte; this only stops a damaged or hostile file from taking the
// machine's memory with it.
constexpr std::size_t MaxInflatedBytes = 128u * 1024u * 1024u;
// What the header-only read inflates at most before it gives up finding
// the first newline.
constexpr std::size_t MaxHeaderBytes = 64u * 1024u;

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

template<typename T>
bool parseInteger(std::string_view text, T &out)
{
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool parseDouble(std::string_view text, double &out)
{
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(out);
}

// The shortest decimal that std::from_chars reads back as the same
// double, so a position survives the file to the bit.
std::string formatDouble(double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

bool parseBool(std::string_view text, bool &out)
{
    if (text == "0" || text == "1") {
        out = text == "1";
        return true;
    }
    return false;
}

char originLetter(ValueOrigin origin)
{
    switch (origin) {
    case ValueOrigin::Rekordbox: return 'r';
    case ValueOrigin::Seabass: return 's';
    case ValueOrigin::Unknown: return '?';
    }
    return '?';
}

std::optional<ValueOrigin> originFromText(std::string_view text)
{
    if (text == "r") {
        return ValueOrigin::Rekordbox;
    }
    if (text == "s") {
        return ValueOrigin::Seabass;
    }
    if (text == "?") {
        return ValueOrigin::Unknown;
    }
    return std::nullopt;
}

// ---- gzip ----------------------------------------------------------

std::optional<std::string> gzipBytes(const std::string &data, std::string *problem)
{
    if (data.size() > static_cast<std::size_t>(UINT_MAX / 2)) {
        *problem = "the baseline is too large to compress";
        return std::nullopt;
    }
    z_stream stream{};
    // 15 + 16: a gzip wrapper rather than zlib's, so the .gz is one.
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        *problem = "zlib could not start compressing";
        return std::nullopt;
    }
    std::string out(deflateBound(&stream, static_cast<uLong>(data.size())) + 32, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        *problem = "zlib could not compress the baseline (code " + std::to_string(result) + ")";
        return std::nullopt;
    }
    out.resize(written);
    return out;
}

// Inflates the gzip stream in `in` into `out`. With `stopAtNewline`, stops
// as soon as `out` holds a newline and reports success without reading
// further; otherwise inflates to the end and refuses a stream that stops
// short, carries a bad gzip checksum, or has bytes after its end.
bool gunzip(std::istream &in, bool stopAtNewline, std::size_t limit, std::string *out, std::string *problem)
{
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        *problem = "zlib could not start decompressing";
        return false;
    }
    struct End
    {
        z_stream *s;
        ~End() { inflateEnd(s); }
    } end{&stream};

    char input[16384];
    char output[65536];
    bool streamEnded = false;
    while (!streamEnded) {
        if (stream.avail_in == 0) {
            in.read(input, sizeof input);
            const std::streamsize got = in.gcount();
            if (in.bad()) {
                *problem = "reading it failed";
                return false;
            }
            if (got == 0) {
                *problem = out->empty() ? "it is empty" : "it is truncated (the compressed data stops early)";
                return false;
            }
            stream.next_in = reinterpret_cast<Bytef *>(input);
            stream.avail_in = static_cast<uInt>(got);
        }
        // Again while input is left, or while the last call filled the
        // output buffer: inflate may hold output it had no room for.
        do {
            stream.next_out = reinterpret_cast<Bytef *>(output);
            stream.avail_out = sizeof output;
            const int result = inflate(&stream, Z_NO_FLUSH);
            if (result == Z_STREAM_END) {
                streamEnded = true;
            } else if (result == Z_BUF_ERROR) {
                // No progress possible without more input; with input
                // still unread that cannot happen to intact data.
                if (stream.avail_in > 0) {
                    *problem = "it is not intact gzip data (zlib made no progress)";
                    return false;
                }
                break;
            } else if (result != Z_OK) {
                *problem = "it is not intact gzip data (zlib code " + std::to_string(result) + ")";
                return false;
            }
            const std::size_t produced = sizeof output - stream.avail_out;
            const std::size_t before = out->size();
            out->append(output, produced);
            if (out->size() > limit) {
                *problem = "it inflates to more than " + std::to_string(limit) + " bytes";
                return false;
            }
            if (stopAtNewline && out->find('\n', before) != std::string::npos) {
                return true;
            }
        } while (!streamEnded && (stream.avail_in > 0 || stream.avail_out == 0));
    }
    if (stopAtNewline) {
        return true;  // the caller says there was no newline
    }
    if (stream.avail_in > 0 || in.peek() != std::char_traits<char>::eof()) {
        *problem = "it has bytes after the end of the compressed data";
        return false;
    }
    return true;
}

// ---- text ----------------------------------------------------------

bool parseHeader(std::string_view line, RekordboxBaseline &out, std::string *problem)
{
    const std::vector<std::string_view> fields = splitTabs(line);
    if (fields.empty() || fields[0] != Magic) {
        *problem = "it is not a Seabass rekordbox baseline";
        return false;
    }
    if (fields.size() < 2) {
        *problem = "its header has no version";
        return false;
    }
    int version = 0;
    if (!parseInteger(fields[1], version) || version != RekordboxBaselineFormatVersion) {
        *problem = "it is format version " + std::string(fields[1]) + ", this Seabass reads version "
                   + std::to_string(RekordboxBaselineFormatVersion) + " only";
        return false;
    }
    if (fields.size() != HeaderFieldCount) {
        *problem = "its header has " + std::to_string(fields.size()) + " fields, not "
                   + std::to_string(HeaderFieldCount);
        return false;
    }
    if (!parseInteger(fields[2], out.pdbSequence)) {
        *problem = "its header's pdb sequence is not a decimal number";
        return false;
    }
    std::optional<std::string> uuid = unescapeManifestField(fields[3]);
    if (!uuid) {
        *problem = "its header's Engine uuid is badly escaped";
        return false;
    }
    if (!parseInteger(fields[4], out.recordedAtUnix)) {
        *problem = "its header's recording time is not a number";
        return false;
    }
    std::optional<std::string> writer = unescapeManifestField(fields[5]);
    if (!writer) {
        *problem = "its header's writer is badly escaped";
        return false;
    }
    out.engineUuid = std::move(*uuid);
    out.writer = std::move(*writer);
    return true;
}

// Sections in file order; a line from an earlier section after a later
// one is refused.
int sectionOf(std::string_view kind)
{
    if (kind == "p") {
        return 1;
    }
    if (kind == "t" || kind == "c") {
        return 2;
    }
    if (kind == "m") {
        return 3;
    }
    if (kind == "x") {
        return 4;
    }
    return 0;
}

std::optional<RekordboxBaseline> parseText(std::string_view text, std::string *problem)
{
    if (text.empty() || text.back() != '\n') {
        *problem = "it does not end with a newline (truncated)";
        return std::nullopt;
    }
    std::size_t trailerStart = text.size() >= 2 ? text.rfind('\n', text.size() - 2) : std::string_view::npos;
    trailerStart = trailerStart == std::string_view::npos ? 0 : trailerStart + 1;
    const std::string_view trailer = text.substr(trailerStart, text.size() - 1 - trailerStart);
    if (trailer.substr(0, TrailerPrefix.size()) != TrailerPrefix) {
        *problem = "it has no #sha256 line (truncated)";
        return std::nullopt;
    }
    const std::optional<hashing::Sha256Digest> claimed =
        hashing::digestFromHex(trailer.substr(TrailerPrefix.size()));
    if (!claimed) {
        *problem = "its #sha256 line is malformed";
        return std::nullopt;
    }
    const std::string_view body = text.substr(0, trailerStart);
    if (hashing::Sha256::of(body) != *claimed) {
        *problem = "its contents do not match its #sha256 line";
        return std::nullopt;
    }

    RekordboxBaseline baseline;
    std::map<std::uint32_t, std::size_t> playlistIndex;
    int section = 0;
    std::size_t lineStart = 0;
    std::size_t lineNumber = 0;
    const auto fail = [&](const std::string &what) {
        *problem = "line " + std::to_string(lineNumber) + ": " + what;
        return std::nullopt;
    };
    while (lineStart < body.size()) {
        const std::size_t lineEnd = body.find('\n', lineStart);
        const std::string_view line = body.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        ++lineNumber;
        if (lineNumber == 1) {
            if (!parseHeader(line, baseline, problem)) {
                *problem = "line 1: " + *problem;
                return std::nullopt;
            }
            continue;
        }
        const std::vector<std::string_view> f = splitTabs(line);
        const int lineSection = sectionOf(f[0]);
        if (lineSection == 0) {
            return fail("unknown line kind \"" + escapeManifestField(f[0]) + "\"");
        }
        if (lineSection < section) {
            return fail("a \"" + std::string(f[0]) + "\" line after a later section");
        }
        section = lineSection;

        if (f[0] == "p") {
            BaselinePlaylist p;
            std::optional<std::string> path;
            if (f.size() != 5 || !parseInteger(f[1], p.id) || !parseInteger(f[2], p.parentId)
                || !parseBool(f[3], p.folder) || !(path = unescapeManifestField(f[4]))) {
                return fail("malformed playlist line");
            }
            p.path = std::move(*path);
            if (!playlistIndex.emplace(p.id, baseline.playlists.size()).second) {
                return fail("playlist id " + std::to_string(p.id) + " listed twice");
            }
            baseline.playlists.push_back(std::move(p));
        } else if (f[0] == "t") {
            BaselineTrack t;
            std::optional<std::string> pathKey;
            std::optional<std::string> relative;
            std::optional<std::string> analysis;
            std::optional<std::string> comment;
            std::optional<ValueOrigin> ratingOrigin;
            if (f.size() != 10 || !(pathKey = unescapeManifestField(f[1])) || pathKey->empty()
                || !(relative = unescapeManifestField(f[2])) || !parseInteger(f[3], t.pdbId)
                || !(analysis = unescapeManifestField(f[4])) || !(ratingOrigin = originFromText(f[6]))
                || !(comment = unescapeManifestField(f[7])) || !parseDouble(f[8], t.bpm)
                || !parseInteger(f[9], t.durationMs)) {
                return fail("malformed track line");
            }
            if (f[5] != "-") {
                int rating = 0;
                if (!parseInteger(f[5], rating)) {
                    return fail("malformed track rating");
                }
                t.rating = rating;
            }
            t.pathKey = std::move(*pathKey);
            t.stickRelativePath = std::move(*relative);
            t.analysisFile = std::move(*analysis);
            t.ratingOrigin = *ratingOrigin;
            t.comment = std::move(*comment);
            baseline.tracks.push_back(std::move(t));
        } else if (f[0] == "c") {
            if (baseline.tracks.empty()) {
                return fail("a cue line with no track line before it");
            }
            BaselineCue c;
            std::optional<std::string> colour;
            std::optional<std::string> comment;
            std::optional<ValueOrigin> origin;
            if (f.size() != 9 || (f[1] != "hot" && f[1] != "memory") || !parseInteger(f[2], c.cue.hotCueNumber)
                || !parseDouble(f[3], c.cue.positionMs) || !parseBool(f[4], c.cue.isLoop)
                || !parseDouble(f[5], c.cue.loopEndMs) || !(colour = unescapeManifestField(f[6]))
                || !(origin = originFromText(f[7])) || !(comment = unescapeManifestField(f[8]))) {
                return fail("malformed cue line");
            }
            c.cue.kind = f[1] == "hot" ? CuePoint::Kind::Hot : CuePoint::Kind::Memory;
            c.cue.color = std::move(*colour);
            c.cue.comment = std::move(*comment);
            c.origin = *origin;
            baseline.tracks.back().cues.push_back(std::move(c));
        } else if (f[0] == "m") {
            std::uint32_t id = 0;
            std::size_t position = 0;
            std::optional<std::string> pathKey;
            if (f.size() != 4 || !parseInteger(f[1], id) || !parseInteger(f[2], position)
                || !(pathKey = unescapeManifestField(f[3])) || pathKey->empty()) {
                return fail("malformed membership line");
            }
            const auto at = playlistIndex.find(id);
            if (at == playlistIndex.end()) {
                return fail("membership of playlist " + std::to_string(id) + ", which no playlist line names");
            }
            std::vector<std::string> &members = baseline.playlists[at->second].members;
            if (position != members.size()) {
                return fail("membership position " + std::to_string(position) + " where "
                            + std::to_string(members.size()) + " was next");
            }
            members.push_back(std::move(*pathKey));
        } else {
            std::optional<std::string> key;
            std::optional<std::string> state;
            if (f.size() != 3 || !(key = unescapeManifestField(f[1])) || key->empty()
                || !(state = unescapeManifestField(f[2]))) {
                return fail("malformed declined line");
            }
            if (!baseline.declined.emplace(std::move(*key), std::move(*state)).second) {
                return fail("a declined item listed twice");
            }
        }
    }
    if (lineNumber == 0) {
        *problem = "it has no header";
        return std::nullopt;
    }
    return baseline;
}

std::optional<std::string> serialize(const RekordboxBaseline &b, std::string *problem)
{
    std::set<std::uint32_t> ids;
    for (const BaselinePlaylist &p : b.playlists) {
        if (!ids.insert(p.id).second) {
            *problem = "two playlists have the id " + std::to_string(p.id);
            return std::nullopt;
        }
        for (const std::string &member : p.members) {
            if (member.empty()) {
                *problem = "a member of playlist " + std::to_string(p.id) + " has no pathKey";
                return std::nullopt;
            }
        }
    }

    std::string out;
    out += Magic;
    out += '\t' + std::to_string(RekordboxBaselineFormatVersion);
    out += '\t' + std::to_string(b.pdbSequence);
    out += '\t' + escapeManifestField(b.engineUuid);
    out += '\t' + std::to_string(b.recordedAtUnix);
    out += '\t' + escapeManifestField(b.writer);
    out += '\n';

    for (const BaselinePlaylist &p : b.playlists) {
        out += "p\t" + std::to_string(p.id) + '\t' + std::to_string(p.parentId) + '\t' + (p.folder ? "1" : "0")
               + '\t' + escapeManifestField(p.path) + '\n';
    }
    for (const BaselineTrack &t : b.tracks) {
        if (t.pathKey.empty()) {
            *problem = "a track (" + t.stickRelativePath + ") has no pathKey";
            return std::nullopt;
        }
        if (!std::isfinite(t.bpm)) {
            *problem = "the BPM of " + t.pathKey + " is not a finite number";
            return std::nullopt;
        }
        out += "t\t" + escapeManifestField(t.pathKey) + '\t' + escapeManifestField(t.stickRelativePath) + '\t'
               + std::to_string(t.pdbId) + '\t' + escapeManifestField(t.analysisFile) + '\t'
               + (t.rating ? std::to_string(*t.rating) : std::string("-")) + '\t' + originLetter(t.ratingOrigin)
               + '\t' + escapeManifestField(t.comment) + '\t' + formatDouble(t.bpm) + '\t'
               + std::to_string(t.durationMs) + '\n';
        for (const BaselineCue &c : t.cues) {
            if (!std::isfinite(c.cue.positionMs) || !std::isfinite(c.cue.loopEndMs)) {
                *problem = "a cue of " + t.pathKey + " has a position that is not a finite number";
                return std::nullopt;
            }
            out += std::string("c\t") + (c.cue.kind == CuePoint::Kind::Hot ? "hot" : "memory") + '\t'
                   + std::to_string(c.cue.hotCueNumber) + '\t' + formatDouble(c.cue.positionMs) + '\t'
                   + (c.cue.isLoop ? "1" : "0") + '\t' + formatDouble(c.cue.loopEndMs) + '\t'
                   + escapeManifestField(c.cue.color) + '\t' + originLetter(c.origin) + '\t'
                   + escapeManifestField(c.cue.comment) + '\n';
        }
    }
    for (const BaselinePlaylist &p : b.playlists) {
        for (std::size_t i = 0; i < p.members.size(); ++i) {
            out += "m\t" + std::to_string(p.id) + '\t' + std::to_string(i) + '\t' + escapeManifestField(p.members[i])
                   + '\n';
        }
    }
    for (const auto &[key, state] : b.declined) {
        if (key.empty()) {
            *problem = "a declined item has no key";
            return std::nullopt;
        }
        out += "x\t" + escapeManifestField(key) + '\t' + escapeManifestField(state) + '\n';
    }

    const hashing::Sha256Digest digest = hashing::Sha256::of(out);
    out += TrailerPrefix;
    out += hashing::toHex(digest);
    out += '\n';
    return out;
}

// Opens the baseline for reading. False with `error` empty when there is
// none; false with `error` set when it cannot be opened.
bool openBaseline(const fs::path &file, std::ifstream &in, std::string *error)
{
    std::error_code ec;
    const fs::file_status status = fs::status(file, ec);
    if (status.type() == fs::file_type::not_found) {
        error->clear();
        return false;
    }
    if (ec) {
        *error = pathToUtf8(file) + ": cannot look at it (" + ec.message() + ")";
        return false;
    }
    if (status.type() != fs::file_type::regular) {
        *error = pathToUtf8(file) + ": it is not a regular file";
        return false;
    }
    in.open(file, std::ios::binary);
    if (!in) {
        *error = pathToUtf8(file) + ": cannot open it";
        return false;
    }
    error->clear();
    return true;
}

}  // namespace

std::optional<domain::RekordboxBaseline> readRekordboxBaseline(const std::filesystem::path &stickRoot,
                                                               std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("readRekordboxBaseline: the error out-parameter is required");
    }
    const fs::path file = paths::stickRekordboxBaseline(stickRoot);
    std::ifstream in;
    if (!openBaseline(file, in, error)) {
        return std::nullopt;
    }
    std::string text;
    std::string problem;
    if (!gunzip(in, false, MaxInflatedBytes, &text, &problem)) {
        *error = pathToUtf8(file) + ": " + problem;
        return std::nullopt;
    }
    std::optional<RekordboxBaseline> baseline = parseText(text, &problem);
    if (!baseline) {
        *error = pathToUtf8(file) + ": " + problem;
    }
    return baseline;
}

std::optional<std::uint64_t> readRekordboxBaselineSequence(const std::filesystem::path &stickRoot,
                                                           std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("readRekordboxBaselineSequence: the error out-parameter is required");
    }
    const fs::path file = paths::stickRekordboxBaseline(stickRoot);
    std::ifstream in;
    if (!openBaseline(file, in, error)) {
        return std::nullopt;
    }
    std::string text;
    std::string problem;
    if (!gunzip(in, true, MaxHeaderBytes, &text, &problem)) {
        *error = pathToUtf8(file) + ": " + problem;
        return std::nullopt;
    }
    const std::size_t newline = text.find('\n');
    if (newline == std::string::npos) {
        *error = pathToUtf8(file) + ": it has no complete header line";
        return std::nullopt;
    }
    RekordboxBaseline header;
    if (!parseHeader(std::string_view(text).substr(0, newline), header, &problem)) {
        *error = pathToUtf8(file) + ": line 1: " + problem;
        return std::nullopt;
    }
    return header.pdbSequence;
}

bool writeRekordboxBaseline(const std::filesystem::path &stickRoot, const domain::RekordboxBaseline &baseline,
                            const std::function<void(const std::string &)> &beforeWrite, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("writeRekordboxBaseline: the error out-parameter is required");
    }
    if (!beforeWrite) {
        throw std::invalid_argument("writeRekordboxBaseline: the beforeWrite hook is required");
    }
    error->clear();
    const fs::path file = paths::stickRekordboxBaseline(stickRoot);
    const std::string fileText = pathToUtf8(file);

    std::string problem;
    const std::optional<std::string> text = serialize(baseline, &problem);
    if (!text) {
        *error = fileText + ": not written: " + problem;
        return false;
    }
    const std::optional<std::string> compressed = gzipBytes(*text, &problem);
    if (!compressed) {
        *error = fileText + ": not written: " + problem;
        return false;
    }

    // Never create a stick root: a path whose stick has gone away would
    // otherwise grow a Seabass directory on whatever is mounted there now.
    std::error_code ec;
    if (!fs::is_directory(stickRoot, ec)) {
        *error = fileText + ": not written: the stick root " + pathToUtf8(stickRoot) + " is not a directory";
        return false;
    }

    beforeWrite(fileText);

    fs::create_directories(paths::stickDir(stickRoot), ec);
    if (ec) {
        *error = fileText + ": not written: cannot create its directory (" + ec.message() + ")";
        return false;
    }
    if (!writeFileDurablyAtomic(fileText, *compressed)) {
        *error = fileText + ": the write failed; the previous file, if any, is unchanged";
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::local
