// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/gzip_text.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
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

constexpr std::string_view Magic = "seabass-rekordbox-analysis";
// A track's three files are well under a megabyte; this only stops a
// damaged file from taking the machine's memory with it.
constexpr std::uintmax_t MaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t MaxText = 128u * 1024u * 1024u;

std::string bytesField(const std::optional<std::string> &bytes)
{
    return bytes ? "b64:" + keptAnalysisBase64(*bytes) : std::string("none");
}

bool parseBytesField(std::string_view text, std::optional<std::string> &into)
{
    if (text == "none") {
        into.reset();
        return true;
    }
    if (text.substr(0, 4) != "b64:") {
        return false;
    }
    auto bytes = keptAnalysisUnbase64(text.substr(4));
    if (!bytes) {
        return false;
    }
    into = std::move(*bytes);
    return true;
}

std::string numberOrNull(const std::optional<std::int64_t> &value)
{
    return value ? std::to_string(*value) : std::string("null");
}

template <typename T>
bool parseNumber(std::string_view text, T &into)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), into);
    return !text.empty() && ec == std::errc() && end == text.data() + text.size();
}

bool parseNumberOrNull(std::string_view text, std::optional<std::int64_t> &into)
{
    if (text == "null") {
        into.reset();
        return true;
    }
    std::int64_t v = 0;
    if (!parseNumber(text, v)) {
        return false;
    }
    into = v;
    return true;
}

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

std::string fingerprintList(const std::vector<std::string> &list)
{
    std::string out;
    for (const std::string &one : list) {
        out += (out.empty() ? "" : ",") + one;
    }
    return out.empty() ? std::string("-") : out;
}

bool parseFingerprintList(std::string_view text, std::vector<std::string> &into)
{
    into.clear();
    if (text == "-") {
        return true;
    }
    for (std::string_view one : [&] {
             std::vector<std::string_view> parts;
             std::size_t start = 0;
             while (true) {
                 const std::size_t comma = text.find(',', start);
                 parts.push_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                                    : comma - start));
                 if (comma == std::string_view::npos) {
                     return parts;
                 }
                 start = comma + 1;
             }
         }()) {
        if (!isSha256Hex(one)) {
            return false;
        }
        into.emplace_back(one);
    }
    return true;
}

std::optional<std::string> readWholeFile(const fs::path &file, std::string *problem)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        *problem = "cannot be read (" + ec.message() + ")";
        return std::nullopt;
    }
    if (size > MaxBytes) {
        *problem = "is " + std::to_string(size) + " bytes, far more than an entry holds";
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *problem = "cannot be opened";
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || bytes.size() != size) {
        *problem = "cannot be read whole";
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

fs::path keptRekordboxAnalysisFile(const fs::path &stickRoot, const std::string &pathKey)
{
    const std::string hex = hashing::toHex(hashing::Sha256::of(std::string_view(pathKey)));
    return paths::stickKeptRekordboxAnalysisDir(stickRoot) / (hex.substr(0, 32) + ".tsv.gz");
}

std::string serializeKeptRekordboxAnalysis(const KeptRekordboxAnalysis &e)
{
    std::string text = std::string(Magic) + "\t" + std::to_string(KeptRekordboxAnalysisFormatVersion) + "\t"
                       + escapeManifestField(e.stickPath) + "\n";
    text += "pathKey\t" + escapeManifestField(e.pathKey) + "\n";
    text += "audioFile\t" + std::to_string(e.audioFileSize) + "\t" + std::to_string(e.audioFileMtimeUnix) + "\n";
    text += "originalGrid\t" + (e.originalGrid.empty() ? std::string("-") : e.originalGrid) + "\n";
    text += "seabassGrids\t" + fingerprintList(e.seabassGrids) + "\n";
    for (const KeptAnlzFileSet &set : e.files) {
        text += "anlz\t" + escapeManifestField(set.datPath) + "\t" + bytesField(set.dat) + "\t" + bytesField(set.ext)
                + "\t" + bytesField(set.twoEx) + "\n";
    }
    for (const KeptRekordboxRow &row : e.rows) {
        text += "row\t" + escapeManifestField(row.library) + "\t" + std::to_string(row.rowId) + "\t"
                + numberOrNull(row.tempo) + "\t" + numberOrNull(row.keyId) + "\n";
    }
    text += "#sha256\t" + hashing::toHex(hashing::Sha256::of(std::string_view(text))) + "\n";
    return text;
}

KeptRekordboxAnalysisLookup parseKeptRekordboxAnalysis(const std::string &text)
{
    KeptRekordboxAnalysisLookup out;
    const auto refuse = [&](const std::string &why) {
        out = {};
        out.status = KeptRekordboxAnalysisLookup::Status::Damaged;
        out.error = why;
        return out;
    };
    if (text.size() < 2 || text.back() != '\n') {
        return refuse("does not end with a newline: it was cut short");
    }
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

    std::vector<std::vector<std::string_view>> lines;
    for (std::size_t start = 0; start < body.size();) {
        const std::size_t end = body.find('\n', start);
        lines.push_back(splitTabs(body.substr(start, end - start)));
        start = end + 1;
    }
    if (lines.size() < 5 || lines[0].size() != 3 || lines[0][0] != Magic) {
        return refuse("not a Seabass kept rekordbox analysis");
    }
    if (lines[0][1] != std::to_string(KeptRekordboxAnalysisFormatVersion)) {
        return refuse("format version " + std::string(lines[0][1]) + ", this Seabass reads "
                      + std::to_string(KeptRekordboxAnalysisFormatVersion));
    }
    KeptRekordboxAnalysis e;
    const auto unescaped = [](std::string_view field, std::string &into) {
        const auto v = unescapeManifestField(field);
        if (!v) {
            return false;
        }
        into = *v;
        return true;
    };
    if (!unescaped(lines[0][2], e.stickPath) || e.stickPath.empty()) {
        return refuse("line 1: a bad stick path");
    }
    const auto named = [&](std::size_t i, std::string_view name, std::size_t fields) {
        return lines[i].size() == fields && lines[i][0] == name;
    };
    if (!named(1, "pathKey", 2) || !unescaped(lines[1][1], e.pathKey) || e.pathKey.empty()) {
        return refuse("line 2: a bad pathKey");
    }
    if (!named(2, "audioFile", 3) || !parseNumber(lines[2][1], e.audioFileSize)
        || !parseNumber(lines[2][2], e.audioFileMtimeUnix)) {
        return refuse("line 3: a bad audioFile");
    }
    if (!named(3, "originalGrid", 2)
        || !(lines[3][1] == "-" || isSha256Hex(lines[3][1]))) {
        return refuse("line 4: a bad originalGrid");
    }
    e.originalGrid = lines[3][1] == "-" ? std::string() : std::string(lines[3][1]);
    if (!named(4, "seabassGrids", 2) || !parseFingerprintList(lines[4][1], e.seabassGrids)) {
        return refuse("line 5: a bad seabassGrids");
    }
    bool rowsBegun = false;
    for (std::size_t i = 5; i < lines.size(); ++i) {
        const std::string where = "line " + std::to_string(i + 1) + ": ";
        if (named(i, "anlz", 5) && !rowsBegun) {
            KeptAnlzFileSet set;
            if (!unescaped(lines[i][1], set.datPath) || set.datPath.empty() || !parseBytesField(lines[i][2], set.dat)
                || !parseBytesField(lines[i][3], set.ext) || !parseBytesField(lines[i][4], set.twoEx)) {
                return refuse(where + "a bad anlz");
            }
            e.files.push_back(std::move(set));
        } else if (named(i, "row", 5)) {
            rowsBegun = true;
            KeptRekordboxRow row;
            if (!unescaped(lines[i][1], row.library) || (row.library != "rekordbox" && row.library != "onelibrary")
                || !parseNumber(lines[i][2], row.rowId) || !parseNumberOrNull(lines[i][3], row.tempo)
                || !parseNumberOrNull(lines[i][4], row.keyId)) {
                return refuse(where + "a bad row");
            }
            e.rows.push_back(std::move(row));
        } else {
            return refuse(where + "not an anlz or row line in its place");
        }
    }
    out.status = KeptRekordboxAnalysisLookup::Status::Found;
    out.entry = std::move(e);
    return out;
}

KeptRekordboxAnalysisLookup StickKeptRekordboxAnalysisStore::find(const std::string &pathKey) const
{
    const fs::path file = keptRekordboxAnalysisFile(m_stickRoot, pathKey);
    const std::string name = pathToUtf8(file);
    KeptRekordboxAnalysisLookup out;
    const auto damaged = [&](const std::string &why) {
        out = {};
        out.status = KeptRekordboxAnalysisLookup::Status::Damaged;
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
    if (!fs::is_regular_file(file, ec)) {
        return damaged("is not a file");
    }
    std::string problem;
    const std::optional<std::string> bytes = readWholeFile(file, &problem);
    if (!bytes) {
        return damaged(problem);
    }
    std::istringstream in(*bytes);
    std::string text;
    if (!gunzip(in, false, MaxText, &text, &problem)) {
        return damaged(problem);
    }
    out = parseKeptRekordboxAnalysis(text);
    if (out.status == KeptRekordboxAnalysisLookup::Status::Damaged) {
        out.error = name + ": " + out.error;
        return out;
    }
    if (out.entry.pathKey != pathKey) {
        return damaged("holds the entry of another track");
    }
    return out;
}

void StickKeptRekordboxAnalysisStore::keep(const KeptRekordboxAnalysis &entry)
{
    if (entry.pathKey.empty() || entry.stickPath.empty()) {
        throw std::invalid_argument("a rekordbox analysis to keep needs the track's key and its place on the stick");
    }
    const fs::path file = keptRekordboxAnalysisFile(m_stickRoot, entry.pathKey);
    const std::string name = pathToUtf8(file);
    std::error_code ec;
    if (!fs::is_directory(m_stickRoot, ec)) {
        throw std::runtime_error(pathToUtf8(m_stickRoot) + " is not a directory");
    }
    std::string problem;
    const std::optional<std::string> packed = gzipBytes(serializeKeptRekordboxAnalysis(entry), &problem);
    if (!packed) {
        throw std::runtime_error("cannot pack the kept analysis for " + name + ": " + problem);
    }
    if (m_beforeChange) {
        m_beforeChange(name);
    }
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("cannot create " + pathToUtf8(file.parent_path()) + " (" + ec.message() + ")");
    }
    if (!writeFileDurablyAtomic(name, *packed)) {
        throw std::runtime_error("cannot write " + name);
    }
    // Believed only once it reads back as what was meant.
    const KeptRekordboxAnalysisLookup back = find(entry.pathKey);
    if (back.status != KeptRekordboxAnalysisLookup::Status::Found || !(back.entry == entry)) {
        throw std::runtime_error(name + " does not read back as it was written"
                                 + (back.error.empty() ? std::string() : " (" + back.error + ")"));
    }
}

void StickKeptRekordboxAnalysisStore::drop(const std::string &pathKey)
{
    const fs::path file = keptRekordboxAnalysisFile(m_stickRoot, pathKey);
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

namespace
{

// The regular files of `dir` with `suffix`, sorted; `error` set when the
// folder cannot be walked whole.
std::vector<fs::path> entryFilesIn(const fs::path &dir, const std::string &suffix, std::string *error)
{
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return files;
    }
    fs::directory_iterator it(dir, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        std::error_code typeEc;
        if (it->is_regular_file(typeEc) && name.size() > suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            files.push_back(it->path());
        }
    }
    if (ec) {
        *error = pathToUtf8(dir) + " cannot be read whole (" + ec.message() + ")";
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

std::vector<KeptAnalysisFileInfo> listKeptRekordboxAnalyses(const fs::path &stickRoot, std::string *error)
{
    error->clear();
    std::vector<KeptAnalysisFileInfo> out;
    for (const fs::path &file : entryFilesIn(paths::stickKeptRekordboxAnalysisDir(stickRoot), ".tsv.gz", error)) {
        KeptAnalysisFileInfo info;
        info.file = pathToUtf8(file);
        std::error_code ec;
        info.bytes = fs::file_size(file, ec);
        // The first line alone: the stick path is on it.
        std::ifstream in(file, std::ios::binary);
        std::string first;
        std::string problem;
        if (in && gunzip(in, true, 64 * 1024, &first, &problem)) {
            const std::size_t newline = first.find('\n');
            const auto fields = splitTabs(std::string_view(first).substr(0, newline));
            if (fields.size() == 3 && fields[0] == Magic) {
                if (const auto path = unescapeManifestField(fields[2])) {
                    info.trackFile = *path;
                    info.readable = !path->empty();
                }
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<KeptAnalysisFileInfo> listKeptEngineAnalyses(const fs::path &stickRoot, std::string *error)
{
    error->clear();
    std::vector<KeptAnalysisFileInfo> out;
    for (const fs::path &file : entryFilesIn(paths::stickKeptEngineAnalysisDir(stickRoot), ".tsv", error)) {
        KeptAnalysisFileInfo info;
        info.file = pathToUtf8(file);
        std::error_code ec;
        info.bytes = fs::file_size(file, ec);
        std::string problem;
        if (const auto text = readWholeFile(file, &problem)) {
            const application::KeptEngineAnalysisLookup parsed = parseKeptEngineAnalysis(*text);
            if (parsed.status == application::KeptEngineAnalysisLookup::Status::Found) {
                info.trackFile = parsed.entry.trackFile;
                info.readable = true;
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace seabass::infrastructure::local
