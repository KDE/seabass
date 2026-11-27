// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Every write of a rekordbox analysis file (.DAT, .EXT, .2EX) is told to
// infrastructure::noteAnalysisFileWritten(), so that TrackAnalysisCache
// never serves the analysis a file held before it was rewritten.
// writeFileDurablyAtomic() and copyFileDurablyAtomic() tell it themselves,
// so a writer that goes through them is covered without a list.
//
// This test reads src/ for every other way of writing a file (a stream
// opened for writing, a rename, a copy, a file opened to be created) and
// fails for a source file that has one and is not listed below. Each
// entry says why its writes are never an analysis file on a stick, or the
// file has to call noteAnalysisFileWritten() itself. A writer added
// without either fails here, by name.
//
// That writeFileDurablyAtomic() and copyFileDurablyAtomic() do tell it,
// and that the cache then reads the file again, is
// track_analysis_cache_test's case 6.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

namespace
{

enum class Kind {
    // Writes files, never an analysis file of a stick's library.
    NeverAnalysis,
    // Can write an analysis file: calls noteAnalysisFileWritten().
    NotesItself,
};

struct Entry
{
    Kind kind;
    const char *why;
};

// The source files (from src/) that write a file other than through the
// two durable writes, and why that is safe.
const std::map<std::string, Entry> &knownWriters()
{
    static const std::map<std::string, Entry> writers = {
        {"application/use_cases/anonymize_library.cpp", {Kind::NeverAnalysis, "an anonymised copy's listing and manifest"}},
        {"application/use_cases/compact_stick_backup.cpp", {Kind::NeverAnalysis, "the stick backup's archive"}},
        {"application/use_cases/open_stick_backup.cpp", {Kind::NeverAnalysis, "a browsed backup's folders on the computer"}},
        {"application/use_cases/restore_stick_backup.cpp",
         {Kind::NotesItself, "places every restored file by rename, analysis files among them"}},
        {"cli/main.cpp", {Kind::NeverAnalysis, "a rekordbox XML named on the command line"}},
        {"gui/app_color_scheme.cpp", {Kind::NeverAnalysis, "a colour scheme in the user's settings"}},
        {"gui/backup_changelog_text.cpp", {Kind::NeverAnalysis, "a backup's history as text, where the user saves it"}},
        {"gui/cover_cache.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"gui/edit/changes/moved_file_changes.cpp", {Kind::NeverAnalysis, "moves an audio file to the place its row names"}},
        {"gui/edit/format_write_session.cpp", {Kind::NeverAnalysis, "a database's scratch copy"}},
        {"gui/edit/save_context.cpp",
         {Kind::NeverAnalysis, "moves audio files back, copies a database's members, sets a database's side files "
                               "aside"}},
        {"gui/edit/staged_edits_store.cpp", {Kind::NeverAnalysis, "the staged edits on the computer"}},
        {"gui/stick_backup_controller.cpp", {Kind::NeverAnalysis, "the stick backup's archive and its journal"}},
        {"gui/track_import_cover.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"infrastructure/audio/qt_cover_art_preparer.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"infrastructure/audio/qt_flac_recording_encoder.cpp", {Kind::NeverAnalysis, "a recording's FLAC file"}},
        {"infrastructure/backup/filesystem_backup_store.cpp",
         {Kind::NeverAnalysis, "a backup's description, audio files moved back; files are put back through "
                               "writeFileDurablyAtomic()"}},
        {"infrastructure/backup/stick_write_lock.cpp", {Kind::NeverAnalysis, "the stick's lock file"}},
        {"infrastructure/database_read_copy.hpp", {Kind::NeverAnalysis, "read copies of a database"}},
        {"infrastructure/durable_file_write.cpp", {Kind::NotesItself, "the two durable writes themselves"}},
        {"infrastructure/engine/libdjinterop_engine_anonymizer.cpp",
         {Kind::NeverAnalysis, "an Engine Library copied for an anonymised copy"}},
        {"infrastructure/engine/libdjinterop_engine_library_creator.cpp",
         {Kind::NeverAnalysis, "a new Engine Library's databases and covers"}},
        {"infrastructure/engine/libdjinterop_engine_reader.cpp", {Kind::NeverAnalysis, "a cover cached on the computer"}},
        {"infrastructure/local/analysis_ledger_file.cpp", {Kind::NeverAnalysis, "sets a ledger that does not read aside"}},
        {"infrastructure/local/file_library_edit_lock_store.cpp", {Kind::NeverAnalysis, "an edit lock file"}},
        {"infrastructure/local/metadata_store.cpp", {Kind::NeverAnalysis, "the metadata database and its export"}},
        {"infrastructure/logging/file_operation_log.hpp", {Kind::NeverAnalysis, "the operation log"}},
        {"infrastructure/media/windows_usb_formatter.cpp", {Kind::NeverAnalysis, "a script for the formatter"}},
        {"infrastructure/no_replace_rename.cpp",
         {Kind::NeverAnalysis, "the rename that never replaces; its one caller writes a FLAC"}},
        {"infrastructure/no_replace_rename.hpp", {Kind::NeverAnalysis, "the declaration of the same"}},
        {"infrastructure/rekordbox/rekordbox_library_anonymizer.cpp",
         {Kind::NeverAnalysis, "analysis files copied into a new anonymised copy, which no cache entry names"}},
        {"infrastructure/sqlite_pending_journal.hpp", {Kind::NeverAnalysis, "a database and its journal kept aside"}},
        {"infrastructure/stick_backup/backup_artwork_lookup.cpp", {Kind::NeverAnalysis, "a cover read from a backup"}},
        {"infrastructure/stick_backup/posix_archive_file.cpp", {Kind::NeverAnalysis, "the stick backup's archive"}},
        {"infrastructure/track_import/verified_file_copy.cpp", {Kind::NeverAnalysis, "an imported audio file"}},
        {"infrastructure/zip_archive_reader.cpp", {Kind::NeverAnalysis, "a browsed backup's entry, on the computer"}},
        {"infrastructure/zip_archive_writer.cpp", {Kind::NeverAnalysis, "a zip archive"}},
        {"storageprobe/write_probe.cpp", {Kind::NeverAnalysis, "the speed probe's own test file"}},
    };
    return writers;
}

// Ways to write a file other than the two durable writes.
const std::regex &rawWrite()
{
    static const std::regex pattern(
        R"(\bofstream\b|\bfs::rename\(|std::filesystem::rename\(|\bcopy_file\(|\bfs::copy\(|std::filesystem::copy\()"
        R"(|QFile::rename|QFile::copy|\.rename\(|QSaveFile|\bfopen\(|_wfopen|O_CREAT|QIODevice::WriteOnly)"
        R"(|QIODevice::Append|QIODevice::ReadWrite|MoveFileEx|\bCopyFile(W|A|Ex|ExW)?\(|renameNoReplace\()");
    return pattern;
}

// The code of a line: not a // comment, whole or trailing.
std::string codeOf(const std::string &line)
{
    const auto comment = line.find("//");
    return comment == std::string::npos ? line : line.substr(0, comment);
}

}  // namespace

int main()
{
    const fs::path src = fs::path(SEABASS_SOURCE_DIR) / "src";
    int failures = 0;
    std::set<std::string> found;
    for (const auto &entry : fs::recursive_directory_iterator(src)) {
        const fs::path extension = entry.path().extension();
        if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".hpp" && extension != ".mm")) {
            continue;
        }
        const std::string relative = seabass::pathToGenericUtf8(entry.path().lexically_relative(src));
        std::ifstream in(entry.path());
        std::stringstream whole;
        whole << in.rdbuf();
        std::istringstream lines(whole.str());
        std::string line;
        int lineNumber = 0;
        std::string firstHit;
        while (std::getline(lines, line)) {
            ++lineNumber;
            if (std::regex_search(codeOf(line), rawWrite())) {
                if (firstHit.empty()) {
                    firstHit = relative + ":" + std::to_string(lineNumber) + ": " + line;
                }
            }
        }
        if (firstHit.empty()) {
            continue;
        }
        found.insert(relative);
        const auto known = knownWriters().find(relative);
        if (known == knownWriters().end()) {
            std::cerr << "FAIL: an unlisted writer: " << firstHit
                      << "\n      If it can write an analysis file, call noteAnalysisFileWritten() for it; then "
                         "list the file here with what it writes.\n";
            ++failures;
            continue;
        }
        if (known->second.kind == Kind::NotesItself
            && whole.str().find("noteAnalysisFileWritten(") == std::string::npos) {
            std::cerr << "FAIL: " << relative << " writes analysis files (" << known->second.why
                      << ") but does not call noteAnalysisFileWritten()\n";
            ++failures;
        }
    }
    // A listed file that writes nothing any more is taken off the list,
    // so the list stays what src/ does.
    for (const auto &[file, why] : knownWriters()) {
        if (!found.count(file)) {
            std::cerr << "FAIL: " << file << " is listed (" << why.why << ") but writes no file any more\n";
            ++failures;
        }
    }
    if (failures > 0) {
        std::cerr << "analysis_file_writers_test: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "analysis_file_writers_test: " << found.size()
              << " source files write files other than through the durable writes, each listed: OK\n";
    return 0;
}
