// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/kept_engine_analysis_file.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::infrastructure::local
{

namespace
{

namespace fs = std::filesystem;
using application::EngineSqlValue;
using application::KeptEngineAnalysis;
using application::KeptEngineAnalysisLookup;
using stick_backup::escapeManifestField;
using stick_backup::unescapeManifestField;

constexpr std::string_view Magic = "seabass-engine-analysis";
// An entry is a few kB; this only stops a damaged file from taking the
// machine's memory with it.
constexpr std::uintmax_t MaxBytes = 16u * 1024u * 1024u;

constexpr char Base64Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(std::string_view bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto v = (std::uint32_t(std::uint8_t(bytes[i])) << 16) | (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8)
            | std::uint32_t(std::uint8_t(bytes[i + 2]));
        out += Base64Digits[(v >> 18) & 63];
        out += Base64Digits[(v >> 12) & 63];
        out += Base64Digits[(v >> 6) & 63];
        out += Base64Digits[v & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        std::uint32_t v = std::uint32_t(std::uint8_t(bytes[i])) << 16;
        if (rest == 2) {
            v |= std::uint32_t(std::uint8_t(bytes[i + 1])) << 8;
        }
        out += Base64Digits[(v >> 18) & 63];
        out += Base64Digits[(v >> 12) & 63];
        out += rest == 2 ? Base64Digits[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Strict: whole quads, padding only at the end, no other characters.
std::optional<std::string> unbase64(std::string_view text)
{
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        const char *at = std::strchr(Base64Digits, c);
        return c != '\0' && at != nullptr ? static_cast<int>(at - Base64Digits) : -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int d[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && last && k >= 2) {
                d[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0) {
                return std::nullopt;
            }
            d[k] = digit(c);
            if (d[k] < 0) {
                return std::nullopt;
            }
        }
        const std::uint32_t v = (std::uint32_t(d[0]) << 18) | (std::uint32_t(d[1]) << 12) | (std::uint32_t(d[2]) << 6)
            | std::uint32_t(d[3]);
        out += static_cast<char>((v >> 16) & 0xff);
        if (pad < 2) {
            out += static_cast<char>((v >> 8) & 0xff);
        }
        if (pad < 1) {
            out += static_cast<char>(v & 0xff);
        }
    }
    return out;
}

std::string valueText(const EngineSqlValue &v)
{
    switch (v.type) {
    case EngineSqlValue::Type::Null:
        return "null";
    case EngineSqlValue::Type::Integer:
        return "int:" + std::to_string(v.integer);
    case EngineSqlValue::Type::Real: {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(v.real)));
        return std::string("real:") + hex;
    }
    case EngineSqlValue::Type::Text:
        return "text:" + base64(v.bytes);
    case EngineSqlValue::Type::Blob:
        return "blob:" + base64(v.bytes);
    }
    return "null";
}

std::optional<EngineSqlValue> parseValue(std::string_view text)
{
    if (text == "null") {
        return EngineSqlValue::null();
    }
    const auto after = [&](std::string_view prefix) -> std::optional<std::string_view> {
        if (text.substr(0, prefix.size()) == prefix) {
            return text.substr(prefix.size());
        }
        return std::nullopt;
    };
    if (const auto digits = after("int:")) {
        std::int64_t v = 0;
        const auto [end, ec] = std::from_chars(digits->data(), digits->data() + digits->size(), v);
        if (ec != std::errc() || end != digits->data() + digits->size() || digits->empty()) {
            return std::nullopt;
        }
        return EngineSqlValue::ofInteger(v);
    }
    if (const auto hex = after("real:")) {
        if (hex->size() != 16) {
            return std::nullopt;
        }
        std::uint64_t bits = 0;
        const auto [end, ec] = std::from_chars(hex->data(), hex->data() + hex->size(), bits, 16);
        if (ec != std::errc() || end != hex->data() + hex->size()) {
            return std::nullopt;
        }
        return EngineSqlValue::ofReal(std::bit_cast<double>(bits));
    }
    if (const auto b = after("text:")) {
        if (auto bytes = unbase64(*b)) {
            return EngineSqlValue::ofText(std::move(*bytes));
        }
        return std::nullopt;
    }
    if (const auto b = after("blob:")) {
        if (auto bytes = unbase64(*b)) {
            return EngineSqlValue::ofBlob(std::move(*bytes));
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Fingerprints as one field: "-" for none, else comma-separated.
std::string fingerprintList(const std::vector<std::string> &list)
{
    std::string out;
    for (const std::string &fingerprint : list) {
        out += (out.empty() ? "" : ",") + fingerprint;
    }
    return out.empty() ? std::string("-") : out;
}

std::optional<std::vector<std::string>> parseFingerprintList(std::string_view text)
{
    std::vector<std::string> out;
    if (text == "-") {
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = text.find(',', start);
        const std::string_view one = text.substr(start, comma == std::string_view::npos ? text.npos : comma - start);
        if (!isSha256Hex(one)) {
            return std::nullopt;
        }
        out.emplace_back(one);
        if (comma == std::string_view::npos) {
            return out;
        }
        start = comma + 1;
    }
}

// The body's lines in their order, field name and value. Format 1 had no
// seabassColumnStates, seabassColumnAnalyses or seabassFiles, format 2 no
// seabassGrids.
std::vector<std::pair<std::string, std::string>> fieldsOf(const KeptEngineAnalysis &e, int version)
{
    const auto &o = e.original;
    std::vector<std::pair<std::string, std::string>> fields{
        {"pathKey", escapeManifestField(e.pathKey)},
        {"trackFile", escapeManifestField(e.trackFile)},
        {"trackId", std::to_string(e.trackId)},
        {"library", escapeManifestField(e.libraryUuid)},
        {"seabassState", e.seabassState.empty() ? "-" : e.seabassState},
        {"seabassAnalysis", e.seabassAnalysis.empty() ? "-" : e.seabassAnalysis},
    };
    if (version >= 2) {
        fields.push_back({"seabassColumnStates", fingerprintList(e.seabassColumnStates)});
        fields.push_back({"seabassColumnAnalyses", fingerprintList(e.seabassColumnAnalyses)});
        fields.push_back({"seabassFiles", fingerprintList(e.seabassFiles)});
    }
    if (version >= 3) {
        fields.push_back({"seabassGrids", fingerprintList(e.seabassGrids)});
    }
    const std::vector<std::pair<std::string, std::string>> columns{
        {"isAnalyzed", valueText(o.isAnalyzed)},
        {"isBeatGridLocked", valueText(o.isBeatGridLocked)},
        {"bpm", valueText(o.bpm)},
        {"bpmAnalyzed", valueText(o.bpmAnalyzed)},
        {"key", valueText(o.key)},
        {"trackData", valueText(o.trackData)},
        {"overviewWaveFormData", valueText(o.overviewWaveFormData)},
        {"beatData", valueText(o.beatData)},
        {"overviewFilePath", escapeManifestField(o.overviewFileRelativePath)},
        {"overviewFile", o.overviewFile ? "blob:" + base64(*o.overviewFile) : std::string("none")},
    };
    fields.insert(fields.end(), columns.begin(), columns.end());
    return fields;
}

}  // namespace

std::string keptAnalysisBase64(std::string_view bytes)
{
    return base64(bytes);
}

std::optional<std::string> keptAnalysisUnbase64(std::string_view text)
{
    return unbase64(text);
}

fs::path keptEngineAnalysisFile(const fs::path &stickRoot, const std::string &pathKey)
{
    const std::string hex = hashing::toHex(hashing::Sha256::of(std::string_view(pathKey)));
    return paths::stickKeptEngineAnalysisDir(stickRoot) / (hex.substr(0, 32) + ".tsv");
}

std::string serializeKeptEngineAnalysis(const KeptEngineAnalysis &entry)
{
    std::string text = std::string(Magic) + "\t" + std::to_string(KeptEngineAnalysisFormatVersion) + "\n";
    for (const auto &[name, value] : fieldsOf(entry, KeptEngineAnalysisFormatVersion)) {
        text += name;
        text += '\t';
        text += value;
        text += '\n';
    }
    text += "#sha256\t" + hashing::toHex(hashing::Sha256::of(std::string_view(text))) + "\n";
    return text;
}

KeptEngineAnalysisLookup parseKeptEngineAnalysis(const std::string &text)
{
    KeptEngineAnalysisLookup out;
    const auto refuse = [&](const std::string &why) {
        out = {};
        out.status = KeptEngineAnalysisLookup::Status::Damaged;
        out.error = why;
        return out;
    };
    if (text.empty() || text.back() != '\n') {
        return refuse("does not end with a newline: it was cut short");
    }
    // At least one line before the checksum's: a lone newline is no entry,
    // and the search below starts a byte before the last newline.
    if (text.size() < 2) {
        return refuse("is empty but for a newline");
    }
    // The checksum line first: nothing else is believed without it.
    const std::size_t lastLine = text.rfind('\n', text.size() - 2);
    const std::size_t sumStart = lastLine == std::string::npos ? 0 : lastLine + 1;
    const std::string_view sumLine(text.data() + sumStart, text.size() - 1 - sumStart);
    constexpr std::string_view SumTag = "#sha256\t";
    if (sumLine.substr(0, SumTag.size()) != SumTag) {
        return refuse("has no #sha256 line at its end");
    }
    const std::string_view body(text.data(), sumStart);
    if (hashing::toHex(hashing::Sha256::of(body)) != sumLine.substr(SumTag.size())) {
        return refuse("its #sha256 does not match: it was changed or damaged");
    }

    std::vector<std::pair<std::string_view, std::string_view>> lines;
    std::size_t start = 0;
    while (start < body.size()) {
        const std::size_t end = body.find('\n', start);
        const std::string_view line = body.substr(start, end - start);
        start = end + 1;
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos) {
            return refuse("line " + std::to_string(lines.size() + 1) + ": not two fields");
        }
        lines.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    if (lines.empty() || lines[0].first != Magic) {
        return refuse("not a Seabass kept Engine analysis");
    }
    // Formats 1 and 2 are read as well: an entry they wrote holds the
    // player's analysis as surely as one of today's, and is rewritten as
    // format 3 by the next write to its row.
    int version = 0;
    if (lines[0].second == "1") {
        version = 1;
    } else if (lines[0].second == "2") {
        version = 2;
    } else if (lines[0].second == std::to_string(KeptEngineAnalysisFormatVersion)) {
        version = KeptEngineAnalysisFormatVersion;
    } else {
        return refuse("format version " + std::string(lines[0].second) + ", this Seabass reads 1 to "
                      + std::to_string(KeptEngineAnalysisFormatVersion));
    }
    const std::vector<std::pair<std::string, std::string>> names = fieldsOf({}, version);
    if (lines.size() != names.size() + 1) {
        return refuse(std::to_string(lines.size() - 1) + " fields, not " + std::to_string(names.size()));
    }
    KeptEngineAnalysis e;
    auto &o = e.original;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto &[name, value] = lines[i + 1];
        const std::string &wanted = names[i].first;
        if (name != wanted) {
            return refuse("line " + std::to_string(i + 2) + ": " + std::string(name) + " where " + wanted + " belongs");
        }
        const auto bad = [&]() { return refuse("line " + std::to_string(i + 2) + ": a bad " + wanted); };
        const auto text = [&](std::string &into) {
            const auto v = unescapeManifestField(value);
            if (!v) {
                return false;
            }
            into = *v;
            return true;
        };
        const auto fingerprint = [&](std::string &into) {
            if (value == "-") {
                into.clear();
                return true;
            }
            if (!isSha256Hex(value)) {
                return false;
            }
            into = std::string(value);
            return true;
        };
        const auto fingerprints = [&](std::vector<std::string> &into) {
            auto list = parseFingerprintList(value);
            if (!list) {
                return false;
            }
            into = std::move(*list);
            return true;
        };
        const auto column = [&](EngineSqlValue &into) {
            auto v = parseValue(value);
            if (!v) {
                return false;
            }
            into = std::move(*v);
            return true;
        };
        bool ok = true;
        if (wanted == "pathKey") {
            ok = text(e.pathKey) && !e.pathKey.empty();
        } else if (wanted == "trackFile") {
            ok = text(e.trackFile);
        } else if (wanted == "trackId") {
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), e.trackId);
            ok = ec == std::errc() && end == value.data() + value.size() && !value.empty();
        } else if (wanted == "library") {
            ok = text(e.libraryUuid);
        } else if (wanted == "seabassState") {
            ok = fingerprint(e.seabassState);
        } else if (wanted == "seabassAnalysis") {
            ok = fingerprint(e.seabassAnalysis);
        } else if (wanted == "seabassColumnStates") {
            ok = fingerprints(e.seabassColumnStates);
        } else if (wanted == "seabassColumnAnalyses") {
            ok = fingerprints(e.seabassColumnAnalyses);
        } else if (wanted == "seabassFiles") {
            ok = fingerprints(e.seabassFiles);
        } else if (wanted == "seabassGrids") {
            ok = fingerprints(e.seabassGrids);
        } else if (wanted == "isAnalyzed") {
            ok = column(o.isAnalyzed);
        } else if (wanted == "isBeatGridLocked") {
            ok = column(o.isBeatGridLocked);
        } else if (wanted == "bpm") {
            ok = column(o.bpm);
        } else if (wanted == "bpmAnalyzed") {
            ok = column(o.bpmAnalyzed);
        } else if (wanted == "key") {
            ok = column(o.key);
        } else if (wanted == "trackData") {
            ok = column(o.trackData);
        } else if (wanted == "overviewWaveFormData") {
            ok = column(o.overviewWaveFormData);
        } else if (wanted == "beatData") {
            ok = column(o.beatData);
        } else if (wanted == "overviewFilePath") {
            ok = text(o.overviewFileRelativePath);
        } else if (wanted == "overviewFile") {
            if (value == "none") {
                o.overviewFile.reset();
            } else if (value.substr(0, 5) == "blob:") {
                auto bytes = unbase64(value.substr(5));
                ok = bytes.has_value();
                if (ok) {
                    o.overviewFile = std::move(*bytes);
                }
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            return bad();
        }
    }
    out.status = KeptEngineAnalysisLookup::Status::Found;
    out.entry = std::move(e);
    return out;
}

KeptEngineAnalysisLookup StickKeptEngineAnalysisStore::find(const std::string &pathKey)
{
    const fs::path file = keptEngineAnalysisFile(m_stickRoot, pathKey);
    const std::string name = pathToUtf8(file);
    KeptEngineAnalysisLookup out;
    const auto damaged = [&](const std::string &why) {
        out = {};
        out.status = KeptEngineAnalysisLookup::Status::Damaged;
        out.error = name + ": " + why;
        return out;
    };
    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    if (ec) {
        return damaged("cannot be looked at (" + ec.message() + ")");
    }
    if (!exists) {
        return out;
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return damaged("cannot be read (" + ec.message() + ")");
    }
    if (size > MaxBytes) {
        return damaged("is " + std::to_string(size) + " bytes, far more than an entry holds");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return damaged("cannot be opened");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return damaged("cannot be read");
    }
    out = parseKeptEngineAnalysis(text);
    if (out.status == KeptEngineAnalysisLookup::Status::Damaged) {
        out.error = name + ": " + out.error;
        return out;
    }
    // A file in another track's place says nothing about this one.
    if (out.entry.pathKey != pathKey) {
        return damaged("holds the entry of another track");
    }
    return out;
}

void StickKeptEngineAnalysisStore::keep(const KeptEngineAnalysis &entry)
{
    if (entry.pathKey.empty()) {
        throw std::invalid_argument("an Engine analysis to keep needs the track's key");
    }
    const fs::path file = keptEngineAnalysisFile(m_stickRoot, entry.pathKey);
    const std::string name = pathToUtf8(file);
    std::error_code ec;
    if (!fs::is_directory(m_stickRoot, ec)) {
        throw std::runtime_error(pathToUtf8(m_stickRoot) + " is not a directory");
    }
    const std::string text = serializeKeptEngineAnalysis(entry);
    if (m_beforeChange) {
        m_beforeChange(name);
    }
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("cannot create " + pathToUtf8(file.parent_path()) + " (" + ec.message() + ")");
    }
    if (!writeFileDurablyAtomic(name, text)) {
        throw std::runtime_error("cannot write " + name);
    }
}

void StickKeptEngineAnalysisStore::drop(const std::string &pathKey)
{
    const fs::path file = keptEngineAnalysisFile(m_stickRoot, pathKey);
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec) {
        return;
    }
    const std::string name = pathToUtf8(file);
    if (m_beforeChange) {
        m_beforeChange(name);
    }
    std::string failure;
    if (!removeEntry(file, failure)) {
        throw std::runtime_error("cannot remove " + name + ": " + failure);
    }
    fsyncDirectoryContaining(name);
}

}  // namespace seabass::infrastructure::local
