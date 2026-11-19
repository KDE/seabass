// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/restored_databases.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

namespace
{

// One line per database, then one per file the restore put beside it:
//   database<TAB>path<TAB>archive fingerprint<TAB>written fingerprint
//   file<TAB>database path<TAB>file path
constexpr std::string_view Header = "seabass-restored-databases\t1";

fs::path recordPath(const fs::path &stickRoot)
{
    return stickRoot / pathFromUtf8(std::string(RestoredDatabasesPath));
}

std::vector<std::string> splitTabs(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

}  // namespace

RestoredDatabases readRestoredDatabases(const fs::path &stickRoot)
{
    RestoredDatabases record;
    std::ifstream in(longPathSafe(recordPath(stickRoot)), std::ios::binary);
    if (!in) {
        return record;
    }
    std::string line;
    if (!std::getline(in, line) || line != Header) {
        return record;
    }
    while (std::getline(in, line)) {
        const std::vector<std::string> fields = splitTabs(line);
        const bool database = fields.size() == 4 && fields[0] == "database" && !fields[1].empty()
                              && !fields[2].empty() && !fields[3].empty();
        const bool file = fields.size() == 3 && fields[0] == "file" && record.count(fields[1]) != 0
                          && !fields[2].empty();
        if (!database && !file) {
            // Half a record says nothing reliable about any of it.
            return {};
        }
        if (database) {
            record[fields[1]] = {fields[2], fields[3], {}};
        } else {
            record[fields[1]].files.push_back(fields[2]);
        }
    }
    for (auto &[path, restored] : record) {
        std::sort(restored.files.begin(), restored.files.end());
        restored.files.erase(std::unique(restored.files.begin(), restored.files.end()), restored.files.end());
    }
    return record;
}

bool writeRestoredDatabases(const fs::path &stickRoot, const RestoredDatabases &record)
{
    const fs::path path = recordPath(stickRoot);
    std::error_code ec;
    if (record.empty()) {
        fs::remove(longPathSafe(path), ec);
        return !ec;
    }
    std::ostringstream out;
    out << Header << '\n';
    for (const auto &[database, restored] : record) {
        out << "database\t" << database << '\t' << restored.archiveFingerprint << '\t' << restored.writtenFingerprint
            << '\n';
        for (const std::string &file : restored.files) {
            out << "file\t" << database << '\t' << file << '\n';
        }
    }
    fs::create_directories(longPathSafe(path.parent_path()), ec);
    return writeFileDurablyAtomic(pathToUtf8(path), out.str());
}

bool holdsBackupDatabase(const RestoredDatabases &record, const std::string &path, const std::string &archiveFingerprint,
                         const std::string &liveFingerprint)
{
    if (liveFingerprint == archiveFingerprint) {
        return true;
    }
    const auto restored = record.find(path);
    return restored != record.end() && restored->second.archiveFingerprint == archiveFingerprint
           && restored->second.writtenFingerprint == liveFingerprint;
}

}  // namespace seabass::infrastructure::stick_backup
