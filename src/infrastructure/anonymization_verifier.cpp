// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/anonymization_verifier.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <type_traits>
#include <vector>

#include "application/use_cases/scan_library.hpp"
#include "infrastructure/anonymization_byte_sweep.hpp"
#include "infrastructure/anonymization_export_layout.hpp"
#include "infrastructure/anonymization_placeholder.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace seabass::infrastructure
{

namespace fs = std::filesystem;
using Anlz = rekordbox_anlz_t;

namespace
{

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// How many leading hex digits, capped: the hash is masked to 0xFFFFFF, so
// a genuine placeholder never starts with more than six.
size_t hexPrefixLength(const std::string &value)
{
    size_t i = 0;
    while (i < value.size() && isHexDigit(value[i])) {
        ++i;
    }
    return i;
}

// The rekordbox writer preserves each field's original byte length: a
// placeholder shorter than the real text it replaces is padded out with
// spaces, and the path section is padded the same way before its trailing
// NUL. That padding is not part of the value, and judging it as if it were
// reports every single track as leaking.
std::string trimPadding(std::string value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    return value;
}

bool isPrefixOf(const std::string &candidate, const std::string &whole)
{
    return candidate.size() <= whole.size() && whole.compare(0, candidate.size(), candidate) == 0;
}

std::string readAnlzPath(const std::string &sectionBytes)
{
    // Same framing the scrubber uses: length of this section's header at
    // +4, length of the path at +12, UTF-16BE text at +lenHeader.
    constexpr size_t LenHeaderOffset = 4;
    constexpr size_t LenPathOffset = 12;
    if (sectionBytes.size() < LenPathOffset + 4) {
        return {};
    }
    auto readU32BE = [&sectionBytes](size_t at) {
        return (static_cast<uint32_t>(static_cast<unsigned char>(sectionBytes[at])) << 24)
            | (static_cast<uint32_t>(static_cast<unsigned char>(sectionBytes[at + 1])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(sectionBytes[at + 2])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(sectionBytes[at + 3]));
    };
    const uint32_t lenHeader = readU32BE(LenHeaderOffset);
    const uint32_t lenPath = readU32BE(LenPathOffset);
    // Widened before adding, and the end checked rather than the sum.
    // Both are uint32_t, so `lenHeader + lenPath` wraps in 32 bits: a
    // section claiming len_header = 0xFFFFFFF8 and len_path = 0x10 sums
    // to 8 and sails past a size check. AnlzFile::readRaw() validates
    // section framing but never a section's own len_header, so that
    // section can come off a stick -- and the indexing below is
    // unchecked operator[], about 4 GB past the buffer. A heap write no
    // enclosing catch can catch.
    const std::size_t pathStart = lenHeader;
    const std::size_t pathEnd = pathStart + static_cast<std::size_t>(lenPath);
    if (lenPath < 4 || pathStart > sectionBytes.size() || pathEnd > sectionBytes.size()) {
        return {};
    }
    const size_t units = lenPath / 2 - 1;
    std::string path;
    for (size_t u = 0; u < units; ++u) {
        path.push_back(sectionBytes[lenHeader + u * 2 + 1]);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\0')) {
        path.pop_back();
    }
    return path;
}

bool isAnalysisFile(const fs::path &path)
{
    const std::string ext = path.extension().string();
    return ext == ".DAT" || ext == ".EXT" || ext == ".2EX";
}

// Files this project's own test harness writes beside a set. They are not
// part of an export and their presence is not a leak.
bool isHarnessFile(const std::string &name)
{
    return name == "SET-EXPECTATIONS.txt" || name == "REFUSAL-BASELINE.txt";
}


}  // namespace

bool looksLikeHashPlaceholder(const std::string &value, const std::string &kind)
{
    if (value.empty()) {
        return true;  // an empty field carries nothing
    }
    // The "no real value to hash" form, possibly truncated.
    if (isPrefixOf(value, kind + " (none)")) {
        return true;
    }
    const size_t hex = hexPrefixLength(value);
    if (hex == 0 || hex > 6) {
        return false;
    }
    const std::string rest = value.substr(hex);
    if (rest.empty()) {
        return true;  // truncated down to the hash alone
    }
    if (rest[0] != ' ') {
        return false;
    }
    // The label is written after the hash precisely so truncation eats the
    // label and not the part that makes tracks distinguishable.
    return isPrefixOf(rest.substr(1), kind);
}

bool looksLikeFilenamePlaceholder(const std::string &value)
{
    if (value.empty()) {
        return true;
    }
    if (isPrefixOf(value, std::string("unknown.mp3"))) {
        return true;
    }
    const size_t hex = hexPrefixLength(value);
    if (hex == 0 || hex > 6) {
        return false;
    }
    const std::string rest = value.substr(hex);
    if (rest.empty()) {
        return true;
    }
    if (rest[0] != '.') {
        return false;
    }
    // Whatever the real file's extension was, letters and digits only.
    return std::all_of(rest.begin() + 1, rest.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
}

bool looksLikeIndexedPlaceholder(const std::string &value, const std::string &kind)
{
    if (value.empty()) {
        return true;
    }
    if (!isPrefixOf(kind, value)) {
        // Could still be a truncation of the label itself.
        return isPrefixOf(value, kind);
    }
    const std::string rest = value.substr(kind.size());
    if (rest.empty()) {
        return true;
    }
    if (rest[0] != ' ') {
        return false;
    }
    return std::all_of(rest.begin() + 1, rest.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

std::string AnonymizationVerification::describe() const
{
    std::ostringstream out;
    out << (ok ? "Anonymization verified." : "ANONYMIZATION CHECK FAILED -- do not share this export.") << "\n";
    out << "Analysis files checked: " << analysisFilesChecked << "\n";
    out << "Files swept for readable bytes: " << filesSwept << "\n";
    if (audioFilesChecked > 0) {
        out << "Audio files listed and checked: " << audioFilesChecked << "\n";
    }
    out << "rekordbox tracks sampled: " << rekordboxTracksSampled << "\n";
    out << "Engine tracks sampled: " << engineTracksSampled << "\n";
    out << "OneLibrary tracks sampled: " << oneLibraryTracksSampled << "\n";
    if (!problems.empty()) {
        out << "\nProblems (" << problems.size() << "):\n";
        for (const auto &problem : problems) {
            out << "  - " << problem << "\n";
        }
    }
    if (!warnings.empty()) {
        out << "\nCould not be checked (" << warnings.size() << "):\n";
        for (const auto &warning : warnings) {
            out << "  - " << warning << "\n";
        }
    }
    return out.str();
}

namespace
{

// Every directory this file reads goes through these two, and they exist
// for one reason: a std::filesystem iterator handed an error_code returns
// end() when it cannot open the directory, and stops where it stands when
// it cannot step. A range-for over one of those is silent either way, so
// a tree that could not be read looks exactly like a tree with nothing in
// it -- and this file's verdict is "problems.empty()". The gate that
// decides whether a DJ's library goes to a stranger would have said yes
// having looked at nothing.
//
// Returns an explanation when the walk did not finish, nullopt when it
// did. Paths in that explanation are relative to `root`: this text goes
// into the report a contributor is shown and asked to paste, and a
// native absolute path carries the user's own name, in the one feature
// whose whole job is taking paths out.
//
// A walk that cannot step stops there, and says where: both iterators
// are set to end() by a failed increment, so there is nothing left to
// ask depth() or pop() -- tried, and it segfaults. What the message can
// do is name the entry it got to and how many it had seen, so a report
// that also says nothing was swept is read as "it stopped here" rather
// than "the export is empty".
template <typename Iterator>
std::optional<std::string> walkWith(const fs::path &dir, const fs::path &root,
                                    const std::function<void(const fs::directory_entry &)> &visit)
{
    auto shown = [&root](const fs::path &path) -> std::string {
        std::error_code relEc;
        const fs::path relative = fs::relative(path, root, relEc);
        if (relEc || relative.empty()) {
            // Never the absolute path: this text is shown to a
            // contributor and pasted into bug threads, and a native one
            // carries the user's own name.
            return path.filename().generic_string();
        }
        // fs::relative(root, root) is ".", which reads as nothing at all
        // in a sentence about what could not be read.
        return relative == "." ? std::string("the export root") : relative.generic_string();
    };

    std::error_code ec;
    Iterator it(dir, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        // Not there at all is not the same as there and unreadable.
        // Every caller here is already behind a "is this tree present"
        // gate, so the one way in is a tree that is half there: engine/
        // exists and engine/Database2 does not, which is what an Engine
        // export that failed partway leaves behind. That is a broken
        // export, not a leaking one, and refusing it as unreadable
        // deleted the staging tree and told the person their export
        // still held real data.
        return std::nullopt;
    }
    if (ec) {
        return "could not read " + shown(dir) + ": " + ec.message();
    }
    const Iterator end;
    std::size_t seen = 0;
    while (it != end) {
        // Kept before the step: an iterator whose increment failed must
        // not be dereferenced, and naming where it got to is the whole
        // point of the message.
        const fs::path here = it->path();
        visit(*it);
        ++seen;
        it.increment(ec);
        if (!ec) {
            continue;
        }
        // One stop and no more: a failed increment sets both iterators
        // to end(), so there is nothing left to step from. A list of
        // them would be generality the control flow forbids.
        return "stopped reading " + shown(dir) + " after " + std::to_string(seen) + " entries, at "
            + shown(here) + ": " + ec.message();
    }
    return std::nullopt;
}

std::optional<std::string> walk(const fs::path &dir, const fs::path &root,
                                const std::function<void(const fs::directory_entry &)> &visit)
{
    return walkWith<fs::directory_iterator>(dir, root, visit);
}

std::optional<std::string> walkTree(const fs::path &dir, const fs::path &root,
                                    const std::function<void(const fs::directory_entry &)> &visit)
{
    return walkWith<fs::recursive_directory_iterator>(dir, root, visit);
}

}  // namespace

AnonymizationVerification verifyAnonymizedExport(const std::string &exportRoot, int trackSampleSize)
{
    AnonymizationVerification result;
    auto fail = [&result](const std::string &what) { result.problems.push_back(what); };
    auto warn = [&result](const std::string &what) { result.warnings.push_back(what); };

    std::error_code ec;
    const fs::path root(exportRoot);
    if (!fs::is_directory(root, ec)) {
        fail(exportRoot + " is not a directory");
        return result;
    }

    // Every "is this tree here" gate below goes through this. is_directory()
    // answers false for BOTH "not there" and "I could not look", and the
    // blocks behind these gates are the layout checks and the catalog
    // sampling -- including the engine/Database2 one that exists because
    // hm.db, the play history, passed every export ever produced. A stat
    // that errors (a dying stick, a symlink loop, a parent that lost its
    // +x) would skip them in silence, which is the same shape as the
    // walks this file just stopped doing.
    // Every path this function puts in front of a person goes through
    // here: relative to the export root, and the file name when even
    // that fails. The report is pasted into bug threads, and a native
    // absolute path carries the user's own name in it.
    auto shownPath = [&root](const fs::path &path) -> std::string {
        std::error_code relEc;
        const fs::path relative = fs::relative(path, root, relEc);
        if (relEc || relative.empty()) {
            return path.filename().generic_string();
        }
        return relative == "." ? std::string("the export root") : relative.generic_string();
    };

    std::set<std::string> gatesReported;
    auto present = [&](const fs::path &dir) {
        std::error_code dirEc;
        const bool yes = fs::is_directory(dir, dirEc);
        if (dirEc && dirEc != std::errc::no_such_file_or_directory) {
            // Once per tree, not once per question: rekordbox/ is asked
            // about three times and engine/ twice, and one stat error
            // would otherwise fill the contributor's report with the
            // same line.
            if (gatesReported.insert(dir.generic_string()).second) {
                fail("could not tell whether " + shownPath(dir) + " is there: " + dirEc.message());
            }
        }
        return yes;
    };

    // --- Layout: only what the manifest says is in here, is in here. ---
    const fs::path rekordboxRoot = root / "rekordbox";
    const fs::path engineRoot = root / "engine";
    if (auto stopped = walk(root, root, [&](const fs::directory_entry &entry) {
            const std::string name = entry.path().filename().string();
            if (name == "MANIFEST.txt" || name == "files.tsv" || name == "rekordbox" || name == "engine"
                || isHarnessFile(name)) {
                return;
            }
            fail("unexpected file at the top level of the export: " + name);
        })) {
        fail(*stopped);
    }

    if (present(rekordboxRoot)) {
        if (auto stopped = walk(rekordboxRoot, root, [&](const fs::directory_entry &entry) {
                const std::string name = entry.path().filename().string();
                // The catalog itself, the analysis files, and the player
                // preference files the Device Profile feature needs.
                if (name == "rekordbox" || name == "USBANLZ" || name == "MYSETTING.DAT" || name == "MYSETTING2.DAT"
                    || name == "DEVSETTING.DAT" || name == "DJMMYSETTING.DAT") {
                    return;
                }
                fail("unexpected entry in the rekordbox tree: " + name);
            })) {
            fail(*stopped);
        }
        const fs::path catalog = rekordboxRoot / "rekordbox";
        if (present(catalog)) {
            if (auto stopped = walk(catalog, root, [&](const fs::directory_entry &entry) {
                const std::string name = entry.path().filename().string();
                // exportLibrary.db is the Device Library Plus mirror, kept
                // now that it is scrubbed; its rows are sampled below.
                if (isKeptRekordboxCatalogFile(name)) {
                    return;
                }
                // SQLite recreates these the moment anything opens the
                // database -- including this check, which reads the
                // mirror back a few lines below. They hold no content of
                // their own once the anonymizer has vacuumed, and the
                // export removes them after this runs, immediately before
                // zipping.
                if (name == "exportLibrary.db-shm" || name == "exportLibrary.db-wal") {
                    return;
                }
                // The -shm and -wal side files are unscrubbed by
                // definition. exportExt.pdb is no longer among these: it
                // is scrubbed and kept now, and the raw-byte sweep below
                // -- which reads every file in the export, this one
                // included -- is what says whether the scrub landed.
                // Nothing here trusts the name alone.
                fail("file that has no anonymizer is present: rekordbox/rekordbox/" + name);
            })) {
                fail(*stopped);
            }
        }
    }

    if (present(engineRoot)) {
        if (auto stopped = walk(engineRoot, root, [&](const fs::directory_entry &entry) {
                const std::string name = entry.path().filename().string();
                if (name == "Database2") {
                    return;
                }
                fail("unexpected entry in the Engine tree: " + name);
            })) {
            fail(*stopped);
        }
        // Inside Database2 the check used to stop, so hm.db -- the play
        // history, carrying real titles, artists, albums and full
        // directory paths -- passed verification in every export ever
        // produced. Only m.db is scrubbed; everything else at this level
        // is content nothing has examined.
        if (auto stopped = walk(engineRoot / "Database2", root, [&](const fs::directory_entry &entry) {
                std::error_code kindEc;
                if (!entry.is_regular_file(kindEc) || kindEc) {
                    // A directory here is OverviewData and friends:
                    // derived numbers, no text. An entry that could not
                    // be asked which it is gets said out loud rather than
                    // skipped with them -- except when the answer is that
                    // it is gone, which is a dangling symlink or an entry
                    // removed between the listing and the stat, and not
                    // something to throw an export away for.
                    if (kindEc && kindEc != std::errc::no_such_file_or_directory) {
                        fail("could not tell what engine/Database2/" + entry.path().filename().string()
                             + " is: " + kindEc.message());
                    }  // filename only: never the local absolute path, see below
                    return;
                }
                const std::string name = entry.path().filename().string();
                if (!isKeptEngineDatabaseFile(name)) {
                    fail("file that has no anonymizer is present: engine/Database2/" + name);
                }
            })) {
            fail(*stopped);
        }
    }

    // --- Analysis files: every embedded path, every file, no sampling. ---
    // This is where the leak was, and it was in all 2744 of them.
    if (present(rekordboxRoot / "USBANLZ")) {
        if (auto stopped = walkTree(rekordboxRoot / "USBANLZ", root, [&](const fs::directory_entry &entry) {
            std::error_code kindEc;
            if (!entry.is_regular_file(kindEc) || kindEc) {
                if (kindEc && kindEc != std::errc::no_such_file_or_directory) {
                    fail("could not tell what " + shownPath(entry.path()) + " is: " + kindEc.message());
                }
                return;
            }
            if (!isAnalysisFile(entry.path())) {
                return;
            }
            ++result.analysisFilesChecked;
            try {
                auto file = rekordbox::AnlzFile::readRaw(entry.path().string());
                for (const auto &section : file.sections) {
                    if (section.fourcc != static_cast<uint32_t>(Anlz::SECTION_TAGS_PATH)) {
                        continue;
                    }
                    const std::string path = readAnlzPath(section.rawBytes);
                    if (path.empty()) {
                        continue;
                    }
                    const size_t slash = path.find_last_of('/');
                    const std::string basename = slash == std::string::npos ? path : path.substr(slash + 1);
                    const std::string directory = slash == std::string::npos ? std::string() : path.substr(0, slash);
                    if (directory != "/Contents" && !directory.empty()) {
                        fail("analysis file still names a real directory: "
                             + fs::relative(entry.path(), root, ec).string() + " -> " + path);
                    } else if (!looksLikeFilenamePlaceholder(basename)) {
                        fail("analysis file still holds a real filename: "
                             + fs::relative(entry.path(), root, ec).string() + " -> " + path);
                    }
                }
            } catch (const std::exception &e) {
                fail("could not read " + fs::relative(entry.path(), root, ec).string() + ": " + e.what());
            }
            })) {
            fail(*stopped);
        }
    }

    // --- The audio file listing, if one was written. ---
    //
    // It carries file names, so it is exactly as capable of leaking as a
    // catalog row is, and it is plain text that anyone opening the zip
    // will read first.
    if (const fs::path listing = root / "files.tsv"; fs::is_regular_file(listing, ec)) {
        std::ifstream in(listing);
        if (!in) {
            // The last read in this function that could not tell "no
            // entries" from "could not look": an unreadable listing left
            // audioFilesChecked at a plausible number and every real
            // filename in it unexamined.
            fail("files.tsv is there and could not be read, so the names in it were never checked");
        }
        std::string line;
        int checked = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::string path = line.substr(0, line.find('\t'));
            const std::size_t slash = path.find_last_of('/');
            const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
            if (!looksLikeFilenamePlaceholder(base)) {
                fail("files.tsv still names a real file: \"" + path + "\"");
                break;  // one is enough; the rest would say the same thing
            }
            ++checked;
        }
        // getline stopping is the end of the file AND a read that broke,
        // and only the second leaves names unexamined.
        if (in.bad()) {
            fail("stopped reading files.tsv after " + std::to_string(checked)
                 + " names, so the rest were never checked");
        }
        result.audioFilesChecked = checked;
    }

    // --- Track fields, sampled through the app's own readers. ---
    auto checkTracks = [&](const std::vector<domain::Track> &tracks, const std::string &label, int &sampled) {
        int checked = 0;
        for (const auto &track : tracks) {
            if (trackSampleSize > 0 && checked >= trackSampleSize) {
                break;
            }
            ++checked;
            const std::string where = label + " track id=" + track.sourceId;
            const std::string title = trimPadding(track.title);
            const std::string artist = trimPadding(track.artist);
            const std::string filename = trimPadding(track.filename);
            const std::string filePath = trimPadding(track.filePath);
            if (!looksLikeHashPlaceholder(title, "Track")) {
                fail(where + " still has a real title: \"" + title + "\"");
            }
            if (!looksLikeIndexedPlaceholder(artist, "Artist")) {
                fail(where + " still has a real artist: \"" + artist + "\"");
            }
            if (!looksLikeFilenamePlaceholder(filename)) {
                fail(where + " still has a real filename: \"" + filename + "\"");
            }
            if (!filePath.empty()) {
                // Unlike the ANLZ-embedded paths checked above (always
                // forward-slash, straight out of rekordbox's own binary
                // format regardless of host OS), this filePath comes from
                // this app's own readers and is in native form -- '\' on
                // Windows. Checking only '/' let a Windows path's real
                // basename hide behind the drive/directory prefix,
                // failing the whole path against the placeholder check
                // instead of just the actual filename.
                const size_t slash = filePath.find_last_of("/\\");
                const std::string basename = slash == std::string::npos ? filePath : filePath.substr(slash + 1);
                if (!looksLikeFilenamePlaceholder(trimPadding(basename))) {
                    fail(where + " still has a real file path: \"" + filePath + "\"");
                }
            }
        }
        sampled = checked;
    };

    // Read back, or said to be unreadable: the floor below only asks
    // about a catalog that WAS read, because "the reader threw" is
    // already a warning with its own wording and its own reason (a
    // hand-built minimal fixture genuinely is unparseable).
    bool rekordboxWasRead = false;
    bool engineWasRead = false;
    if (present(rekordboxRoot)) {
        try {
            rekordbox::KaitaiRekordboxReader reader(rekordboxRoot.string());
            auto tracks = application::ScanLibrary(reader).execute();
            checkTracks(tracks, "rekordbox", result.rekordboxTracksSampled);
            rekordboxWasRead = true;
        } catch (const std::exception &e) {
            warn(std::string("could not read the rekordbox catalog back, so its fields were not sampled: ")
                 + e.what());
        }
    }
    bool oneLibraryWasRead = false;
    if (present(rekordboxRoot) && onelibrary::OneLibraryCueWriter::existsFor(rekordboxRoot.string())) {
        try {
            onelibrary::OneLibraryReader reader(rekordboxRoot.string());
            auto tracks = reader.readAll();
            checkTracks(tracks, "OneLibrary", result.oneLibraryTracksSampled);
            oneLibraryWasRead = true;
        } catch (const std::exception &e) {
            warn(std::string("could not read the OneLibrary mirror back, so its fields were not sampled: ")
                 + e.what());
        }
    }
    if (present(engineRoot)) {
        try {
            engine::LibdjinteropEngineReader reader(engineRoot.string());
            auto tracks = application::ScanLibrary(reader).execute();
            checkTracks(tracks, "Engine", result.engineTracksSampled);
            engineWasRead = true;
        } catch (const std::exception &e) {
            warn(std::string("could not read the Engine catalog back, so its fields were not sampled: ")
                 + e.what());
        }
    }

    // --- Raw bytes: everything the readers above structurally cannot see. ---
    if (auto stopped = walkTree(root, root, [&](const fs::directory_entry &entry) {
        std::error_code kindEc;
        if (!entry.is_regular_file(kindEc) || kindEc) {
            if (kindEc && kindEc != std::errc::no_such_file_or_directory) {
                fail("could not tell what " + shownPath(entry.path()) + " is: " + kindEc.message());
            }
            return;
        }
        // generic_string(), not string(): this path becomes part of a
        // problem message compared against a hardcoded, forward-slash
        // known-baseline list (tests/anonymization_verifier_test.cpp's
        // knownDirtyFixtureFiles) and shown to a contributor -- a native
        // backslash path on Windows matched neither, and the test's own
        // "clean copy, beyond the known baseline" case saw the two
        // already-documented leaks as new ones on every Windows run.
        const std::string relative = fs::relative(entry.path(), root, ec).generic_string();
        const std::string name = entry.path().filename().string();
        // MANIFEST.txt is prose on purpose -- it is the page explaining to
        // the contributor what was kept and what was replaced, including
        // any hardware and notes they chose to type in themselves. Sweeping
        // it reports the explanation as the leak.
        if (isHarnessFile(name) || name == "MANIFEST.txt") {
            return;
        }
        const auto unaccounted = readableTextInRawBytes(entry.path());
        if (!unaccounted) {
            // Counted as swept and clean until now. A file in the export
            // that nothing could read is the one case where this check
            // knows least and the export is about to be sent anyway.
            //
            // A file that is simply not there any more is not a file
            // that could not be read, and refusing on it would delete a
            // clean export over an entry something else removed between
            // the listing and the stat. It is still a file this check
            // did not sweep, though, so it is said out loud rather than
            // dropped: silence here is the thing this whole branch is
            // about. A warning, because the export is not shown to hold
            // anything -- and because failing would destroy it.
            std::error_code goneEc;
            if (!fs::exists(entry.path(), goneEc) && !goneEc) {
                warn(relative + " was there when the folder was listed and gone when it was read, so its bytes "
                                "were never swept");
                return;
            }
            fail(relative + " could not be read, so its bytes were never swept");
            return;
        }
        ++result.filesSwept;
        if (!unaccounted->empty()) {
            // A leak is never one string, and a failure listing 1,584 of
            // them helps nobody; enough to recognise it, and the count.
            constexpr size_t MaxReported = 5;
            std::ostringstream message;
            message << relative
                    << " still has readable text in its raw bytes, which the catalog readers cannot see -- "
                    << unaccounted->size() << " distinct: ";
            for (size_t i = 0; i < unaccounted->size() && i < MaxReported; ++i) {
                message << (i > 0 ? ", " : "") << '"' << (*unaccounted)[i] << '"';
            }
            if (unaccounted->size() > MaxReported) {
                message << ", ...";
            }
            fail(message.str());
        }
        })) {
        fail(*stopped);
    }

    // The floor under all of it. Every check above reports what it found,
    // and none of them says anything when it was handed nothing to look
    // at: an export directory this could not walk, or one holding no
    // files at all, came back ok with every counter at zero. "I found no
    // leak" and "I read nothing" must not be the same answer here.
    if (result.filesSwept == 0) {
        fail("no file in the export was swept, so this check proved nothing");
    }
    // The same floor for the reader side. A catalog that parses to zero
    // rows -- a broken page chain, a schema the reader walks to nothing
    // -- left its counter at zero and said nothing, and the byte sweep
    // does not cover for it: a single real word ("Prodigy") is not
    // two-plus-word prose and is never flagged. A catalog that is there
    // and yielded no track to check is a catalog nothing looked at.
    // The ANLZ pass is where the leak was, in all 2744 of them, and its
    // counter was the one left without a floor: USBANLZ present with
    // nothing in it that isAnalysisFile() recognises checked nothing and
    // said nothing.
    if (present(rekordboxRoot / "USBANLZ") && result.analysisFilesChecked == 0) {
        fail("rekordbox/USBANLZ is there and not one analysis file was checked, so the paths inside them were "
             "never looked at");
    }
    // Warnings, not problems: a catalog that reads back empty is either
    // a library with nothing in it (rekordbox makes an exportLibrary.db
    // the moment it writes a stick, populated or not) or a reader that
    // walked it to nothing, and only the second is a hole. Nothing
    // distinguishes them from here, and a problem deletes the staging
    // tree -- AnonymizeLibrary treats any failure as fatal -- so this
    // says what it did not check and leaves the export alone. The
    // silence is what was wrong, not the verdict.
    if (rekordboxWasRead && result.rekordboxTracksSampled == 0) {
        warn("the rekordbox catalog read back with no tracks in it, so its fields were never checked");
    }
    if (engineWasRead && result.engineTracksSampled == 0) {
        warn("the Engine catalog read back with no tracks in it, so its fields were never checked");
    }
    // The mirror carries the same titles, artists and paths as the
    // catalog beside it, and is read the same way, so it gets the same
    // floor. It was left out of the first pair for no reason at all.
    if (oneLibraryWasRead && result.oneLibraryTracksSampled == 0) {
        warn("the Device Library Plus mirror read back with no tracks in it, so its fields were never checked");
    }

    result.ok = result.problems.empty();
    return result;
}

}  // namespace seabass::infrastructure
