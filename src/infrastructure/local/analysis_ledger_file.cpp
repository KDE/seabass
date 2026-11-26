// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/analysis_ledger_file.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/gzip_text.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::infrastructure::local
{

namespace
{

namespace fs = std::filesystem;
using stick_backup::escapeManifestField;
using stick_backup::unescapeManifestField;

constexpr std::string_view Magic = "seabass-analysis";
constexpr std::string_view TrailerPrefix = "#sha256\t";
constexpr std::size_t TrackFieldCount = 16;

// A ledger of ten thousand tracks is a few megabytes of text; this only
// stops a damaged file from taking the machine's memory with it.
constexpr std::size_t MaxInflatedBytes = 64u * 1024u * 1024u;

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

// Plain decimal only: no sign where none is allowed, no spaces, nothing
// after the digits.
template<typename T>
bool parseInteger(std::string_view text, T &out)
{
    if (text.empty() || text.front() == '+') {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool parseRevision(std::string_view text, int &out)
{
    return parseInteger(text, out) && out >= 0;
}

// "70.00": digits, a point, exactly two digits.
bool isTempoText(std::string_view text)
{
    const std::size_t point = text.find('.');
    if (point == std::string_view::npos || point == 0 || text.size() - point != 3) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i != point && (text[i] < '0' || text[i] > '9')) {
            return false;
        }
    }
    return true;
}

// "-" or 64 lowercase hex digits; "-" reads as empty.
bool parseFingerprint(std::string_view text, std::string &out)
{
    if (text == "-") {
        out.clear();
        return true;
    }
    if (!isSha256Hex(text)) {
        return false;
    }
    out = std::string(text);
    return true;
}

// Why `entry`, filed under `key`, cannot be written; empty when it can.
std::string unwritable(const std::string &key, const AnalysisLedgerEntry &entry)
{
    if (key.empty()) {
        return "an entry has no key";
    }
    if (entry.pathKey != key) {
        return "the entry filed under " + key + " is for " + entry.pathKey;
    }
    if (entry.inputRevision < 0 || entry.gridRevision < 0 || entry.keyRevision < 0 || entry.waveformRevision < 0) {
        return "the entry for " + key + " has a negative revision";
    }
    if (!isTempoText(entry.tempoMin) || !isTempoText(entry.tempoMax)) {
        return "the entry for " + key + " has a tempo range not written as formatTempoBpm() writes one";
    }
    for (const std::string *fingerprint : {&entry.rekordboxGrid, &entry.engineGrid}) {
        if (!fingerprint->empty() && !isSha256Hex(*fingerprint)) {
            return "the entry for " + key + " has a grid fingerprint that is not 64 lowercase hex digits";
        }
    }
    if (entry.keyWritten && entry.key.empty()) {
        return "the entry for " + key + " says a key was written and names none";
    }
    if (entry.key == "-") {
        return "the entry for " + key + " has \"-\" for a key, which reads back as none";
    }
    return {};
}

std::optional<AnalysisLedger> parseText(std::string_view text, std::string *problem)
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
    const std::string_view claimed = trailer.substr(TrailerPrefix.size());
    if (!isSha256Hex(claimed)) {
        *problem = "its #sha256 line is malformed";
        return std::nullopt;
    }
    const std::string_view body = text.substr(0, trailerStart);
    if (hashing::toHex(hashing::Sha256::of(body)) != claimed) {
        *problem = "its contents do not match its #sha256 line";
        return std::nullopt;
    }

    AnalysisLedger ledger;
    std::optional<std::string> previousKey;
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
        const std::vector<std::string_view> f = splitTabs(line);
        if (lineNumber == 1) {
            if (f[0] != Magic) {
                return fail("it is not a Seabass analysis ledger");
            }
            if (f.size() != 2) {
                return fail("its header has " + std::to_string(f.size()) + " fields, not 2");
            }
            int version = 0;
            if (!parseInteger(f[1], version) || version != AnalysisLedgerFormatVersion) {
                return fail("it is format version " + escapeManifestField(f[1]) + ", this Seabass reads version "
                            + std::to_string(AnalysisLedgerFormatVersion) + " only");
            }
            continue;
        }
        if (f[0] != "t") {
            return fail("unknown line kind \"" + escapeManifestField(f[0]) + "\"");
        }
        if (f.size() != TrackFieldCount) {
            return fail("a track line of " + std::to_string(f.size()) + " fields, not "
                        + std::to_string(TrackFieldCount));
        }
        AnalysisLedgerEntry e;
        std::optional<std::string> key = unescapeManifestField(f[1]);
        if (!key || key->empty()) {
            return fail("no key, or a key with a bad escape");
        }
        std::optional<std::string> version = unescapeManifestField(f[8]);
        std::optional<std::string> heard = f[13] == "-" ? std::optional<std::string>(std::string())
                                                         : unescapeManifestField(f[13]);
        if (!parseInteger(f[2], e.fileSize) || !parseInteger(f[3], e.fileMtimeUnix)) {
            return fail("the file's size or time is not a decimal number");
        }
        if (!parseRevision(f[4], e.inputRevision) || !parseRevision(f[5], e.gridRevision)
            || !parseRevision(f[6], e.keyRevision) || !parseRevision(f[7], e.waveformRevision)) {
            return fail("a revision is not a decimal number from 0");
        }
        if (!version) {
            return fail("the library version has a bad escape");
        }
        if (!isTempoText(f[9]) || !isTempoText(f[10])) {
            return fail("the tempo range is not two numbers with two decimals each");
        }
        if (!parseFingerprint(f[11], e.rekordboxGrid) || !parseFingerprint(f[12], e.engineGrid)) {
            return fail("a grid fingerprint is neither \"-\" nor 64 lowercase hex digits");
        }
        if (!heard || (f[13] != "-" && heard->empty())) {
            return fail("the key is empty or has a bad escape");
        }
        if ((f[14] != "0" && f[14] != "1") || (f[14] == "1" && heard->empty())) {
            return fail("keyWritten is not 0 or 1, or is 1 beside no key");
        }
        if (!parseInteger(f[15], e.analysedAtUnix)) {
            return fail("the time of the analysis is not a decimal number");
        }
        if (previousKey && !(*previousKey < *key)) {
            return fail(*previousKey == *key ? "a key listed twice" : "keys out of order");
        }
        previousKey = *key;
        e.pathKey = *key;
        e.libraryVersion = std::move(*version);
        e.tempoMin = std::string(f[9]);
        e.tempoMax = std::string(f[10]);
        e.key = std::move(*heard);
        e.keyWritten = f[14] == "1";
        ledger.emplace(std::move(*key), std::move(e));
    }
    if (lineNumber == 0) {
        *problem = "it has no header";
        return std::nullopt;
    }
    return ledger;
}

}  // namespace

std::string formatTempoBpm(double bpm)
{
    if (!std::isfinite(bpm) || bpm < 0.0 || bpm > 1000.0) {
        return {};
    }
    // Hundredths as an integer, so no locale and no rounding mode of a
    // stream decides the text.
    const long long hundredths = std::llround(bpm * 100.0);
    char text[32];
    std::snprintf(text, sizeof text, "%lld.%02lld", hundredths / 100, hundredths % 100);
    return text;
}

std::optional<std::string> serializeAnalysisLedger(const AnalysisLedger &ledger, std::string *problem)
{
    std::string out;
    out += Magic;
    out += '\t' + std::to_string(AnalysisLedgerFormatVersion) + '\n';
    for (const auto &[key, e] : ledger) {
        if (const std::string why = unwritable(key, e); !why.empty()) {
            *problem = why;
            return std::nullopt;
        }
        out += "t\t" + escapeManifestField(key);
        out += '\t' + std::to_string(e.fileSize);
        out += '\t' + std::to_string(e.fileMtimeUnix);
        out += '\t' + std::to_string(e.inputRevision);
        out += '\t' + std::to_string(e.gridRevision);
        out += '\t' + std::to_string(e.keyRevision);
        out += '\t' + std::to_string(e.waveformRevision);
        out += '\t' + escapeManifestField(e.libraryVersion);
        out += '\t' + e.tempoMin;
        out += '\t' + e.tempoMax;
        out += '\t' + (e.rekordboxGrid.empty() ? std::string("-") : e.rekordboxGrid);
        out += '\t' + (e.engineGrid.empty() ? std::string("-") : e.engineGrid);
        out += '\t' + (e.key.empty() ? std::string("-") : escapeManifestField(e.key));
        out += std::string("\t") + (e.keyWritten ? "1" : "0");
        out += '\t' + std::to_string(e.analysedAtUnix);
        out += '\n';
    }
    const std::string digest = hashing::toHex(hashing::Sha256::of(out));
    out += TrailerPrefix;
    out += digest;
    out += '\n';
    return out;
}

AnalysisLedger readAnalysisLedger(const fs::path &stickRoot, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("readAnalysisLedger: the error out-parameter is required");
    }
    error->clear();
    const fs::path file = paths::stickAnalysisLedger(stickRoot);
    const std::string name = pathToUtf8(file);
    const auto refuse = [&](const std::string &why) {
        *error = name + ": " + why;
        return AnalysisLedger{};
    };

    std::error_code ec;
    const fs::file_status status = fs::status(file, ec);
    if (status.type() == fs::file_type::not_found) {
        return {};
    }
    if (ec) {
        return refuse("cannot look at it (" + ec.message() + ")");
    }
    if (status.type() != fs::file_type::regular) {
        return refuse("it is not a regular file");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return refuse("cannot open it");
    }
    std::string text;
    std::string problem;
    if (!gunzip(in, false, MaxInflatedBytes, &text, &problem)) {
        return refuse(problem);
    }
    std::optional<AnalysisLedger> ledger = parseText(text, &problem);
    if (!ledger) {
        return refuse(problem);
    }
    return std::move(*ledger);
}

bool writeAnalysisLedger(const fs::path &stickRoot, const AnalysisLedger &ledger,
                         const std::function<void(const std::string &)> &beforeWrite, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("writeAnalysisLedger: the error out-parameter is required");
    }
    if (!beforeWrite) {
        throw std::invalid_argument("writeAnalysisLedger: the beforeWrite hook is required");
    }
    error->clear();
    const fs::path file = paths::stickAnalysisLedger(stickRoot);
    const std::string fileText = pathToUtf8(file);

    std::string problem;
    const std::optional<std::string> text = serializeAnalysisLedger(ledger, &problem);
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
