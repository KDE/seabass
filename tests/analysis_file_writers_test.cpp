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
// opened for writing, a Windows handle or a POSIX descriptor opened for
// writing, a rename, a link, a copy, a file opened to be created or cut,
// a SQLite database opened to be written) and
// fails for a source file that has one and is not listed below. Each
// entry says why its writes are never an analysis file on a stick, or the
// file has to call noteAnalysisFileWritten() itself. A writer added
// without either fails here, by name.
//
// A file that does write analysis files, through the durable writes, is
// not passed as a whole: each of its other writes is listed by the line
// it is on (a pattern of that line, and why), and a write on any other
// line of it fails here, as does a listed line that is not there any more.
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
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

namespace
{

enum class Kind {
    // Writes files, never an analysis file of a stick's library.
    NeverAnalysis,
    // Can write an analysis file: calls noteAnalysisFileWritten().
    NotesItself,
    // Writes analysis files through writeFileDurablyAtomic() or
    // copyFileDurablyAtomic(); its other writes are the lines listed.
    DurableByLine,
};

// A line of a DurableByLine file that writes another way: a pattern found
// in the line's code, and why that write is never an analysis file.
struct LineExemption
{
    const char *pattern;
    const char *why;
};

struct Entry
{
    Kind kind;
    const char *why;
    std::vector<LineExemption> lines = {};
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
        {"cli/damage_filesystem.cpp", {Kind::NeverAnalysis, "a hidden test command that damages a FAT32 device's bookkeeping"}},
        {"cli/main.cpp", {Kind::NeverAnalysis, "a rekordbox XML named on the command line"}},
        {"gui/app_color_scheme.cpp", {Kind::NeverAnalysis, "a colour scheme in the user's settings"}},
        {"gui/backup_changelog_text.cpp", {Kind::NeverAnalysis, "a backup's history as text, where the user saves it"}},
        {"gui/cover_cache.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"gui/edit/changes/import_track_change.cpp",
         {Kind::DurableByLine,
          "writes an imported track's analysis files through writeFileDurablyAtomic()",
          {{R"(QTemporaryFile largePicture\()", "a cover picture for Engine in the computer's temporary folder"}}}},
        {"gui/edit/changes/moved_file_changes.cpp", {Kind::NeverAnalysis, "moves an audio file to the place its row names"}},
        {"gui/edit/format_write_session.cpp", {Kind::NeverAnalysis, "a database's scratch copy"}},
        {"gui/edit/save_context.cpp",
         {Kind::NeverAnalysis, "moves audio files back, copies a database's members, sets a database's side files "
                               "aside"}},
        {"gui/edit/staged_edits_store.cpp", {Kind::NeverAnalysis, "the staged edits on the computer"}},
        {"gui/stick_backup_controller.cpp", {Kind::NeverAnalysis, "the stick backup's archive and its journal"}},
        {"gui/track_analysis_cache.cpp", {Kind::NeverAnalysis, "the analysis cache's own database on the computer"}},
        {"gui/track_import_controller.cpp", {Kind::NeverAnalysis, "a staged track's cover in a cache folder on the computer"}},
        {"gui/track_import_cover.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"infrastructure/audio/qt_cover_art_preparer.cpp", {Kind::NeverAnalysis, "a QBuffer in memory"}},
        {"infrastructure/audio/qt_flac_recording_encoder.cpp", {Kind::NeverAnalysis, "a recording's FLAC file"}},
        {"infrastructure/backup/filesystem_backup_store.cpp",
         {Kind::DurableByLine,
          "puts a backup's files back, analysis files among them, through writeFileDurablyAtomic()",
          {{R"(std::ofstream out\(dir / DescriptionFileName)", "a backup's description, in the backup folder"},
           {R"(fs::rename\(move\.to, move\.from,)", "an audio file a restore moves back to where it was"},
           {R"(fs::rename\(\(\*it\)->from, \(\*it\)->to,)", "the same audio file moved again when the restore is undone"}}}},
        {"infrastructure/backup/stick_write_lock.cpp", {Kind::NeverAnalysis, "the stick's lock file"}},
        {"infrastructure/database_read_copy.hpp", {Kind::NeverAnalysis, "read copies of a database"}},
        {"infrastructure/durable_file_write.cpp", {Kind::NotesItself, "the two durable writes themselves"}},
        {"infrastructure/engine/engine_artwork.cpp", {Kind::NeverAnalysis, "an Engine database's covers"}},
        {"infrastructure/engine/engine_import_state.cpp", {Kind::NeverAnalysis, "an Engine database's import counter"}},
        {"infrastructure/engine/engine_pending_journals.hpp", {Kind::NeverAnalysis, "an Engine database whose journal is settled"}},
        {"infrastructure/engine/engine_playlists.cpp", {Kind::NeverAnalysis, "an Engine database's playlists"}},
        {"infrastructure/engine/engine_track_rows.cpp", {Kind::NeverAnalysis, "an Engine database's track rows"}},
        {"infrastructure/engine/libdjinterop_engine_anonymizer.cpp",
         {Kind::NeverAnalysis, "an Engine Library copied for an anonymised copy"}},
        {"infrastructure/engine/libdjinterop_engine_cleanup_writer.cpp", {Kind::NeverAnalysis, "an Engine database"}},
        {"infrastructure/engine/libdjinterop_engine_library_creator.cpp",
         {Kind::NeverAnalysis, "a new Engine Library's databases and covers"}},
        {"infrastructure/engine/libdjinterop_engine_reader.cpp", {Kind::NeverAnalysis, "a cover cached on the computer"}},
        {"infrastructure/local/analysis_ledger_file.cpp", {Kind::NeverAnalysis, "sets a ledger that does not read aside"}},
        {"infrastructure/local/file_library_edit_lock_store.cpp", {Kind::NeverAnalysis, "an edit lock file"}},
        {"infrastructure/local/local_cue_store.cpp", {Kind::NeverAnalysis, "the cue store's database on the computer"}},
        {"infrastructure/local/metadata_store.cpp", {Kind::NeverAnalysis, "the metadata database and its export"}},
        {"infrastructure/logging/file_operation_log.cpp", {Kind::NeverAnalysis, "the operation log"}},
        {"infrastructure/logging/file_operation_log.hpp", {Kind::NeverAnalysis, "the operation log"}},
        {"infrastructure/media/windows_removable_media_mounter.cpp", {Kind::NeverAnalysis, "a volume handle, to mount or eject a stick"}},
        {"infrastructure/media/windows_usb_formatter.cpp", {Kind::NeverAnalysis, "a script for the formatter"}},
        {"infrastructure/no_replace_rename.cpp",
         {Kind::NeverAnalysis, "the rename that never replaces; its one caller writes a FLAC"}},
        {"infrastructure/no_replace_rename.hpp", {Kind::NeverAnalysis, "the declaration of the same"}},
        {"infrastructure/onelibrary/sqlcipher_dyn.cpp", {Kind::NeverAnalysis, "a database in memory, to see that SQLCipher loads"}},
        {"infrastructure/onelibrary/sqlcipher_dyn.hpp", {Kind::NeverAnalysis, "SQLite's flag for opening to write, defined"}},
        {"infrastructure/rekordbox/rekordbox_library_anonymizer.cpp",
         {Kind::NeverAnalysis, "analysis files copied into a new anonymised copy, which no cache entry names"}},
        {"infrastructure/sqlite_pending_journal.hpp", {Kind::NeverAnalysis, "a database and its journal kept aside"}},
        {"infrastructure/sqlite_settle_copy.cpp", {Kind::NeverAnalysis, "a database's copy, settled"}},
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
        R"(|QIODevice::Append|QIODevice::ReadWrite|MoveFileEx|\bCopyFile(W|A|Ex|ExW)?\(|renameNoReplace\()"
        // A stream that may write: std::fstream (not the header's name),
        // or any stream given an output mode.
        R"(|\bfstream\b(?!>)|\bios(_base)?::(out|app|trunc)\b)"
        // A Windows handle opened for writing, or to create a file.
        R"(|\bGENERIC_WRITE\b|\bFILE_(GENERIC_WRITE|WRITE_DATA|APPEND_DATA)\b)"
        R"(|\b(CREATE_ALWAYS|CREATE_NEW|OPEN_ALWAYS|TRUNCATE_EXISTING)\b)"
        // A POSIX descriptor opened for writing.
        R"(|\bO_(WRONLY|RDWR|TRUNC|APPEND)\b)"
        // Renames, links and replacements under other names.
        R"(|\brename\(|\b_wrename\(|\bMoveFile(W|A)?\(|\bReplaceFile(W|A)?\(|\bCreateHardLink(W|A)?\()"
        R"(|\bcreate_(hard_link|symlink|directory_symlink)\(|\bQFile::link\b|\bresize_file\()"
        // Qt's other ways to create or cut a file.
        R"(|QIODevice::Truncate|QIODevice::NewOnly|\bQTemporaryFile\b)"
        // A SQLite database opened to be written.
        R"(|\bSQLITE_OPEN_READWRITE\b|\bsqlite3_open\()");
    return pattern;
}

// The code of a line: not a // comment, whole or trailing, nor an
// #include, which names a header and opens nothing.
std::string codeOf(const std::string &line)
{
    if (line.find_first_not_of(" \t") != std::string::npos && line.compare(line.find_first_not_of(" \t"), 8, "#include") == 0) {
        return {};
    }
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
        // file:line: text of every raw write, for a DurableByLine entry.
        std::vector<std::pair<std::string, std::string>> hits;
        while (std::getline(lines, line)) {
            ++lineNumber;
            if (std::regex_search(codeOf(line), rawWrite())) {
                if (firstHit.empty()) {
                    firstHit = relative + ":" + std::to_string(lineNumber) + ": " + line;
                }
                hits.emplace_back(relative + ":" + std::to_string(lineNumber) + ": " + line, codeOf(line));
            }
        }
        if (firstHit.empty()) {
            continue;
        }
        found.insert(relative);
        const auto known = knownWriters().find(relative);
        if (known != knownWriters().end() && known->second.kind == Kind::DurableByLine) {
            const std::string &text = whole.str();
            if (text.find("writeFileDurablyAtomic(") == std::string::npos
                && text.find("copyFileDurablyAtomic(") == std::string::npos) {
                std::cerr << "FAIL: " << relative << " is listed as writing analysis files through the durable "
                          << "writes (" << known->second.why << ") but calls neither\n";
                ++failures;
            }
            std::vector<bool> used(known->second.lines.size(), false);
            for (const auto &[where, code] : hits) {
                bool exempt = false;
                for (std::size_t i = 0; i < known->second.lines.size(); ++i) {
                    if (std::regex_search(code, std::regex(known->second.lines[i].pattern))) {
                        used[i] = true;
                        exempt = true;
                    }
                }
                if (!exempt) {
                    std::cerr << "FAIL: a write that is not listed, in a file that writes analysis files: " << where
                              << "\n      Write an analysis file through writeFileDurablyAtomic(); list any other "
                                 "write here by its line, with what it writes.\n";
                    ++failures;
                }
            }
            for (std::size_t i = 0; i < used.size(); ++i) {
                if (!used[i]) {
                    std::cerr << "FAIL: " << relative << " has no line like " << known->second.lines[i].pattern
                              << " (" << known->second.lines[i].why << ") any more: take it off the list\n";
                    ++failures;
                }
            }
            continue;
        }
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
