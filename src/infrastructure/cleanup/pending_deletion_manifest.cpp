// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/cleanup/pending_deletion_manifest.hpp"

#include <ctime>
#include <fstream>
#include <system_error>
#include <filesystem>
#include <optional>
#include <sstream>

namespace seabass::infrastructure::cleanup
{

namespace
{

std::string isoTimestampUtc()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Finds "key":"..." in one JSON-object line and returns the unescaped
// value, or nullopt if the key isn't present. Written specifically to
// read back what jsonEscape()/append() below always produce -- not a
// general-purpose JSON parser.
std::optional<std::string> extractField(const std::string &line, const std::string &key)
{
    std::string needle = "\"" + key + "\":\"";
    size_t start = line.find(needle);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    start += needle.size();
    std::string value;
    for (size_t i = start; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char next = line[i + 1];
            switch (next) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(next);
            }
            ++i;
            continue;
        }
        if (line[i] == '"') {
            return value;
        }
        value.push_back(line[i]);
    }
    return std::nullopt;  // unterminated -- malformed line
}

std::string serializeLine(const PendingDeletion &entry)
{
    std::ostringstream line;
    line << "{\"timestampUtc\":\"" << jsonEscape(entry.timestampUtc) << "\",\"format\":\"" << jsonEscape(entry.format)
         << "\",\"filePath\":\"" << jsonEscape(entry.filePath) << "\",\"title\":\"" << jsonEscape(entry.title)
         << "\",\"artist\":\"" << jsonEscape(entry.artist) << "\",\"backupId\":\"" << jsonEscape(entry.backupId)
         << "\"}\n";
    return line.str();
}

}  // namespace

PendingDeletionManifest::PendingDeletionManifest(std::string manifestPath) : m_manifestPath(std::move(manifestPath)) {}

void PendingDeletionManifest::append(PendingDeletion entry)
{
    entry.timestampUtc = isoTimestampUtc();

    // Same "open, append, close every call" pattern as FileOperationLog,
    // so multiple Seabass processes writing to the same stick don't
    // stomp on each other's lines.
    // <stick>/Seabass/orphaned may not exist yet, and ofstream will not
    // create it -- an unopened stream drops the record of what is waiting
    // to be deleted, which is the one thing this file exists to keep.
    std::error_code dirEc;
    std::filesystem::create_directories(std::filesystem::path(m_manifestPath).parent_path(), dirEc);
    std::ofstream ofs(m_manifestPath, std::ofstream::app);
    ofs << serializeLine(entry);
}

void PendingDeletionManifest::removeProcessed(const std::set<std::string> &processedFilePaths)
{
    if (processedFilePaths.empty()) {
        return;
    }
    auto entries = list();
    std::ostringstream rewritten;
    bool anyRemoved = false;
    for (const auto &entry : entries) {
        if (processedFilePaths.contains(entry.filePath)) {
            anyRemoved = true;
            continue;
        }
        rewritten << serializeLine(entry);
    }
    if (!anyRemoved) {
        return;
    }
    // <stick>/Seabass/orphaned may not exist yet, and ofstream will not
    // create it -- an unopened stream drops the record of what is waiting
    // to be deleted, which is the one thing this file exists to keep.
    std::error_code dirEc;
    std::filesystem::create_directories(std::filesystem::path(m_manifestPath).parent_path(), dirEc);
    std::ofstream ofs(m_manifestPath, std::ofstream::trunc);
    ofs << rewritten.str();
}

std::vector<PendingDeletion> PendingDeletionManifest::list() const
{
    std::vector<PendingDeletion> result;
    std::ifstream ifs(m_manifestPath);
    if (!ifs.is_open()) {
        return result;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) {
            continue;
        }
        PendingDeletion entry;
        entry.timestampUtc = extractField(line, "timestampUtc").value_or("");
        entry.format = extractField(line, "format").value_or("");
        entry.filePath = extractField(line, "filePath").value_or("");
        entry.title = extractField(line, "title").value_or("");
        entry.artist = extractField(line, "artist").value_or("");
        entry.backupId = extractField(line, "backupId").value_or("");
        result.push_back(std::move(entry));
    }
    return result;
}

}  // namespace seabass::infrastructure::cleanup
