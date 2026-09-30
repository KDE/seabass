// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Clean Up Recordings (application/use_cases/clean_up_recordings.hpp)
// against small planted stick trees in scratch: which folders count, what
// is left alone, what is refused, and that a file is counted deleted only
// when the filesystem agrees. Every count the report gives is checked
// against the filesystem here, not trusted.

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "application/use_cases/clean_up_recordings.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include "scratch_path.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace seabass::application;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
namespace fs = std::filesystem;
using Status = RecordingOutcome::Status;

namespace
{

// A playable WAV: 44-byte header, then `frames` of 16-bit stereo 44.1 kHz.
// The samples are silence except for a tag at the end, so two files made
// with different tags have the same size and different bytes.
std::string wavBytes(std::uint32_t frames, const std::string &tag)
{
    const std::uint32_t dataBytes = frames * 4;
    std::string out;
    auto put32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    auto put16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<char>(v & 0xff));
        out.push_back(static_cast<char>(v >> 8));
    };
    out += "RIFF";
    put32(36 + dataBytes);
    out += "WAVEfmt ";
    put32(16);
    put16(1);       // PCM
    put16(2);       // stereo
    put32(44100);
    put32(44100 * 4);
    put16(4);
    put16(16);
    out += "data";
    put32(dataBytes);
    std::string data(dataBytes, '\0');
    for (std::size_t i = 0; i < tag.size() && i < data.size(); ++i) {
        data[data.size() - 1 - i] = tag[i];
    }
    return out + data;
}

void write(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    assert(out.good());
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool present(const fs::path &path)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
}

int filesIn(const fs::path &folder)
{
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        ++n;
    }
    return n;
}

class CountingProbe : public TrackDurationProbe
{
public:
    std::optional<double> durationSeconds(const std::string &path) override
    {
        asked.insert(path);
        return 12.5;
    }
    std::set<std::string> asked;
};

fs::path freshDir(const fs::path &root, const std::string &name)
{
    const fs::path dir = root / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::string> pathsOf(const RecordingListing &listing)
{
    std::vector<std::string> out;
    for (const auto &r : listing.recordings) {
        out.push_back(r.path);
    }
    return out;
}


RecordingsReport run(const fs::path &stick, const std::vector<std::string> &paths,
                     CancellationToken cancel = CancellationToken(),
                     const std::function<void(const RecordingsProgress &)> &progress = {},
                     const RecordingDeleteHooks &hooks = {})
{
    return deleteRecordings(stick, paths, cancel, progress, hooks);
}

// ---------------------------------------------------------------------

void allSpellingsAreFoundAndNothingElse(const fs::path &root)
{
    const fs::path stick = freshDir(root, "listing");
    write(stick / "Sessions" / "Session-0001.wav", wavBytes(100, "a"));
    write(stick / "Sessions" / "Milkshaken.WAV", wavBytes(200, "b"));
    write(stick / "Sessions" / "notes.txt", "set list");
    write(stick / "Sessions" / "._Session-0001.wav", "AppleDouble");
    fs::create_directories(stick / "Sessions" / "older");
    write(stick / "Sessions" / "older" / "Session-0000.wav", wavBytes(10, "deep"));
    // Case differs from the spelling on purpose: FAT does not care.
    write(stick / "pioneer rec" / "REC001.WAV", wavBytes(50, "c"));
    write(stick / "PIONEER DJ REC" / "REC002.WAV", wavBytes(60, "d"));
    write(stick / "ALPHATHETA REC" / "REC003.wav", wavBytes(70, "e"));
    // Library and app folders, and loose files: never recordings.
    write(stick / "Contents" / "Artist" / "Session-0001.wav", wavBytes(100, "lib"));
    write(stick / "Contents" / "REC001.WAV", wavBytes(100, "lib2"));
    write(stick / "PIONEER" / "rekordbox" / "REC004.WAV", wavBytes(10, "p"));
    write(stick / "Engine Library" / "Session-0003.wav", wavBytes(10, "e2"));
    write(stick / "Seabass" / "Session-0004.wav", wavBytes(10, "s"));
    write(stick / "Session-0005.wav", wavBytes(10, "loose"));
    // A FILE named like a recording folder is not a folder.
    write(stick / "PIONEER DJ REC.wav", wavBytes(10, "f"));

    CountingProbe probe;
    const RecordingListing listing = listRecordings(stick, probe);
    std::set<std::string> names;
    for (const auto &r : listing.recordings) {
        names.insert(r.folderName + "/" + r.fileName);
        const std::string generic = pathToUtf8(pathFromUtf8(r.path).lexically_relative(stick));
        assert(generic.rfind("Contents", 0) != 0);
        assert(generic.rfind("PIONEER" + std::string(1, static_cast<char>(fs::path::preferred_separator)), 0) != 0);
        assert(r.durationSeconds && *r.durationSeconds == 12.5);
    }
    const std::set<std::string> expected = {"Sessions/Milkshaken.WAV", "Sessions/Session-0001.wav",
                                            "pioneer rec/REC001.WAV", "PIONEER DJ REC/REC002.WAV",
                                            "ALPHATHETA REC/REC003.wav"};
    if (names != expected) {
        for (const auto &n : names) {
            std::cerr << "listed: " << n << "\n";
        }
    }
    assert(names == expected);
    assert(probe.asked.size() == 5);

    std::uint64_t total = 0;
    for (const auto &r : listing.recordings) {
        assert(r.sizeBytes == fs::file_size(pathFromUtf8(r.path)));
        total += r.sizeBytes;
        if (r.folderName == "Sessions") {
            assert(r.source == RecordingSource::EngineOs);
        } else if (r.folderName == "ALPHATHETA REC") {
            assert(r.source == RecordingSource::AlphaTheta);
        } else {
            assert(r.source == RecordingSource::Pioneer);
        }
    }
    assert(listing.totalBytes == total);

    std::set<std::string> leftAlone;
    for (const auto &l : listing.leftAlone) {
        leftAlone.insert(l.fileName + ":" + l.reason);
        assert(l.folderName == "Sessions");
    }
    assert(leftAlone == (std::set<std::string>{"notes.txt:not an audio file", "._Session-0001.wav:a hidden file",
                                               "older:a folder"}));
    assert(listing.unreadableFolders.empty());

    {
        // No probe: the hub's quick summary, which asks for no durations.
        NullTrackDurationProbe none;
        const RecordingListing quick = listRecordings(stick, none);
        assert(quick.recordings.size() == 5);
        for (const auto &r : quick.recordings) {
            assert(!r.durationSeconds);
        }
    }
    std::cout << "ok: all four spellings listed, leftovers named, library folders never\n";
}

void theTickedAreDeletedAndTheCountsAddUp(const fs::path &root)
{
    const fs::path stick = freshDir(root, "delete");
    write(stick / "Sessions" / "A.wav", wavBytes(1000, "a"));
    write(stick / "Sessions" / "B.wav", wavBytes(900, "b"));
    write(stick / "PIONEER REC" / "REC001.WAV", wavBytes(700, "c"));
    write(stick / "PIONEER REC" / "readme.txt", "not mine");
    const std::vector<std::string> ticked = {pathToUtf8(stick / "Sessions" / "A.wav"),
                                             pathToUtf8(stick / "PIONEER REC" / "REC001.WAV")};
    std::vector<std::string> announced;
    const RecordingsReport report = run(stick, ticked, CancellationToken(), [&](const RecordingsProgress &p) {
        assert(p.fileCount == 2);
        announced.push_back(p.fileName);
    });
    // The report's claims...
    assert(report.requested == 2);
    assert(report.deleted == 2);
    assert(report.failed == 0);
    assert(report.notAttempted == 0);
    assert(!report.cancelled);
    assert(announced == (std::vector<std::string>{"A.wav", "REC001.WAV"}));
    for (const auto &o : report.outcomes) {
        assert(o.status == Status::Deleted);
        // ...each asked of the filesystem.
        assert(!present(pathFromUtf8(o.path)));
    }
    // What was not ticked, and what is not a recording, stays; so do the
    // folders, which the hardware makes again anyway.
    assert(present(stick / "Sessions" / "B.wav"));
    assert(present(stick / "PIONEER REC" / "readme.txt"));
    assert(fs::is_directory(stick / "Sessions"));
    assert(fs::is_directory(stick / "PIONEER REC"));
    std::cout << "ok: the ticked recordings go, the counts match the filesystem, the rest stays\n";
}

void cancelFinishesTheFileInFlightAndStops(const fs::path &root)
{
    const fs::path stick = freshDir(root, "cancel");
    write(stick / "Sessions" / "A.wav", wavBytes(1000, "a"));
    write(stick / "Sessions" / "B.wav", wavBytes(1000, "b"));
    write(stick / "Sessions" / "C.wav", wavBytes(1000, "c"));
    NullTrackDurationProbe none;
    const RecordingListing listing = listRecordings(stick, none);
    assert(listing.recordings.size() == 3);
    CancellationToken cancel;
    // Asked for while the first file is in flight.
    const RecordingsReport report = run(stick, pathsOf(listing), cancel, [&](const RecordingsProgress &p) {
        if (p.fileIndex == 0) {
            cancel.cancel();
        }
    });
    assert(report.cancelled);
    assert(report.outcomes[0].status == Status::Deleted);
    assert(report.outcomes[1].status == Status::NotAttempted);
    assert(report.outcomes[2].status == Status::NotAttempted);
    assert(report.deleted == 1);
    assert(report.notAttempted == 2);
    assert(!present(stick / "Sessions" / "A.wav"));
    assert(present(stick / "Sessions" / "B.wav"));
    assert(present(stick / "Sessions" / "C.wav"));
    std::cout << "ok: cancel finishes the file in flight and touches nothing after it\n";
}

void pathsThatAreNotRecordingsAreRefused(const fs::path &root)
{
    const fs::path stick = freshDir(root, "refuse");
    const fs::path library = stick / "Contents" / "Artist" / "Track.wav";
    const fs::path notes = stick / "Sessions" / "notes.txt";
    const fs::path hidden = stick / "Sessions" / "._Session-0001.wav";
    const fs::path nested = stick / "Sessions" / "older" / "Session-0000.wav";
    const fs::path engine = stick / "Engine Library" / "Session-0001.wav";
    const fs::path pioneer = stick / "PIONEER" / "REC001.WAV";
    const fs::path elsewhere = root / "refuse-other" / "Sessions" / "Session-0001.wav";
    for (const auto &p : {library, notes, hidden, nested, engine, pioneer, elsewhere}) {
        write(p, wavBytes(100, "x"));
    }
    // Written so that the lexical path leaves the folder: validated and
    // acted on as the same, normalised path.
    const std::string sneaky = pathToUtf8(stick / "Sessions" / ".." / "Contents" / "Artist" / "Track.wav");
    const RecordingsReport report = run(stick, {pathToUtf8(library), pathToUtf8(notes), pathToUtf8(hidden),
                                                pathToUtf8(nested), pathToUtf8(engine), pathToUtf8(pioneer),
                                                pathToUtf8(elsewhere), sneaky});
    assert(report.failed == 8);
    assert(report.deleted == 0);
    for (const auto &o : report.outcomes) {
        assert(o.status == Status::Refused);
    }
    for (const auto &p : {library, notes, hidden, nested, engine, pioneer, elsewhere}) {
        assert(present(p));
    }

    // The same path twice is deleted once and refused once.
    const fs::path recording = stick / "Sessions" / "Session-0001.wav";
    write(recording, wavBytes(100, "y"));
    const RecordingsReport twice = run(stick, {pathToUtf8(recording), pathToUtf8(recording)});
    assert(twice.outcomes[0].status == Status::Deleted);
    assert(twice.outcomes[1].status == Status::Refused);
    assert(twice.deleted == 1 && twice.failed == 1);
    std::cout << "ok: anything but a recording directly in a recording folder is refused\n";
}

#if !defined(_WIN32)
void theValidatedPathIsTheDeletedPath(const fs::path &root)
{
    // ".." after a link: the path reads as Sessions/A.wav, but the
    // system would resolve it through the link to a file elsewhere.
    // Validated as the normalised path, so acted on as that one too.
    const fs::path stick = freshDir(root, "dotdot");
    const fs::path outside = freshDir(root, "dotdot-outside");
    write(stick / "Sessions" / "A.wav", wavBytes(100, "mine"));
    write(outside / "A.wav", wavBytes(100, "not mine"));
    fs::create_directories(outside / "sub");
    fs::create_directory_symlink(outside / "sub", stick / "Sessions" / "lnk");
    const std::string written = pathToUtf8(stick / "Sessions" / "lnk" / ".." / "A.wav");
    const RecordingsReport report = run(stick, {written});
    assert(report.outcomes[0].status == Status::Deleted);
    assert(!present(stick / "Sessions" / "A.wav"));
    assert(present(outside / "A.wav"));
    std::cout << "ok: the path that was checked is the path that is deleted\n";
}
#endif

#if !defined(_WIN32)
void aRecordingTheStickWillNotLetGoOfIsReported(const fs::path &root)
{
    if (::geteuid() == 0) {
        // root ignores the folder's permissions, so the precondition is
        // not reachable here; a skip is a failure, so say it loudly.
        std::cerr << "FAIL: running as root, cannot make a folder refuse an unlink\n";
        std::exit(1);
    }
    const fs::path stick = freshDir(root, "stuck");
    const std::string a = wavBytes(1000, "stuck");
    write(stick / "Sessions" / "A.wav", a);
    fs::permissions(stick / "Sessions", fs::perms::owner_read | fs::perms::owner_exec);
    const RecordingsReport report = run(stick, {pathToUtf8(stick / "Sessions" / "A.wav")});
    fs::permissions(stick / "Sessions", fs::perms::owner_all);
    assert(report.outcomes[0].status == Status::DeleteFailed);
    // The unlink's own refusal, with its reason, not the later backstop.
    assert(report.outcomes[0].reason.rfind("could not delete it from the stick: ", 0) == 0);
    assert(report.deleted == 0);
    assert(report.failed == 1);
    assert(read(stick / "Sessions" / "A.wav") == a);
    std::cout << "ok: a recording the stick will not let go of is reported, not counted\n";
}
#endif

void theFilesystemIsAskedAgainAfterTheDelete(const fs::path &root)
{
    // An unlink that says yes while the file is still there.
    const fs::path stick = freshDir(root, "reappears");
    const fs::path original = stick / "Sessions" / "A.wav";
    const std::string a = wavBytes(500, "back");
    write(original, a);
    RecordingDeleteHooks hooks;
    hooks.afterDelete = [&](const fs::path &path) { write(path, a); };
    const RecordingsReport report = run(stick, {pathToUtf8(original)}, CancellationToken(), {}, hooks);
    assert(report.outcomes[0].status == Status::DeleteFailed);
    // Caught at the delete itself; the pass at the end of the run is the
    // backstop behind it and would say "checked after the run".
    assert(report.outcomes[0].reason == "still on the stick after it was deleted");
    assert(report.deleted == 0);
    assert(present(original));
    std::cout << "ok: the filesystem is asked again after every delete\n";
}

void aRecordingGoneBeforeItsDeleteIsNotCountedAsDeleted(const fs::path &root)
{
    // Taken away between the plan and the delete.
    {
        const fs::path stick = freshDir(root, "vanish");
        const fs::path original = stick / "Sessions" / "A.wav";
        write(original, wavBytes(500, "a"));
        RecordingDeleteHooks hooks;
        hooks.beforeDelete = [](const fs::path &path) { fs::remove(path); };
        const RecordingsReport report = run(stick, {pathToUtf8(original)}, CancellationToken(), {}, hooks);
        assert(report.outcomes[0].status == Status::Vanished);
        assert(report.deleted == 0);
        assert(report.failed == 1);
    }
    // The stick itself going: its volume answers differently (or not at
    // all) after the first file, and the rest stay where they are.
    {
        const fs::path stick = freshDir(root, "ejected");
        write(stick / "Sessions" / "A.wav", wavBytes(500, "a"));
        write(stick / "Sessions" / "B.wav", wavBytes(500, "b"));
        NullTrackDurationProbe none;
        const RecordingListing listing = listRecordings(stick, none);
        int deletes = 0;
        RecordingDeleteHooks hooks;
        hooks.beforeDelete = [&deletes](const fs::path &) { ++deletes; };
        hooks.volumeIdOf = [&deletes](const fs::path &) -> std::optional<std::uint64_t> {
            if (deletes >= 2) {
                return std::nullopt;
            }
            return 1;
        };
        const RecordingsReport report = run(stick, pathsOf(listing), CancellationToken(), {}, hooks);
        assert(report.outcomes[0].status == Status::Deleted);
        assert(report.outcomes[1].status == Status::Vanished);
        assert(report.deleted == 1);
        assert(!present(stick / "Sessions" / "A.wav"));
        assert(present(stick / "Sessions" / "B.wav"));
    }
    std::cout << "ok: a recording gone before its delete is Vanished, not Deleted\n";
}

#if !defined(_WIN32)
void linksAndOddEntriesAreNotRecordings(const fs::path &root)
{
    const fs::path stick = freshDir(root, "links");
    const fs::path elsewhere = freshDir(root, "links-elsewhere");
    write(elsewhere / "REC001.WAV", wavBytes(100, "x"));
    write(elsewhere / "Session-0009.wav", wavBytes(100, "y"));
    // A recording folder that is a link: not followed.
    fs::create_directory_symlink(elsewhere, stick / "PIONEER REC");
    // A link and a FIFO dressed as recordings inside a real one.
    write(stick / "Sessions" / "Session-0001.wav", wavBytes(100, "z"));
    fs::create_symlink(elsewhere / "Session-0009.wav", stick / "Sessions" / "Session-0009.wav");
    assert(::mkfifo(pathToUtf8(stick / "Sessions" / "pipe.wav").c_str(), 0600) == 0);  // narrow-ok: POSIX test helper
    NullTrackDurationProbe none;
    const RecordingListing listing = listRecordings(stick, none);
    assert(listing.recordings.size() == 1);
    assert(listing.recordings[0].fileName == "Session-0001.wav");
    std::set<std::string> leftAlone;
    for (const auto &l : listing.leftAlone) {
        leftAlone.insert(l.fileName + ":" + l.reason);
    }
    assert(leftAlone == (std::set<std::string>{"Session-0009.wav:not a plain file", "pipe.wav:not a plain file"}));
    // Handed in anyway, they are refused and left.
    const RecordingsReport report = run(stick, {pathToUtf8(stick / "Sessions" / "Session-0009.wav"),
                                                pathToUtf8(stick / "PIONEER REC" / "REC001.WAV")});
    assert(report.outcomes[0].status == Status::Refused);
    assert(report.outcomes[1].status == Status::Refused);
    assert(present(elsewhere / "REC001.WAV"));
    assert(present(elsewhere / "Session-0009.wav"));
    std::cout << "ok: linked folders, links and FIFOs are not recordings\n";
}
#endif


void theFileNameRules()
{
    assert(isRecordingFileName("REC001.WAV"));
    assert(isRecordingFileName("take.aiff"));
    assert(isRecordingFileName("take.m4a"));
    assert(!isRecordingFileName("._take.wav"));
    assert(!isRecordingFileName(".wav"));
    assert(!isRecordingFileName("take.wav.txt"));
    std::cout << "ok: file-name rules\n";
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "recordings";
    fs::create_directories(root);
    allSpellingsAreFoundAndNothingElse(root);
    theTickedAreDeletedAndTheCountsAddUp(root);
    cancelFinishesTheFileInFlightAndStops(root);
    pathsThatAreNotRecordingsAreRefused(root);
#if !defined(_WIN32)
    aRecordingTheStickWillNotLetGoOfIsReported(root);
#endif
#if !defined(_WIN32)
    theValidatedPathIsTheDeletedPath(root);
#endif
    theFilesystemIsAskedAgainAfterTheDelete(root);
    aRecordingGoneBeforeItsDeleteIsNotCountedAsDeleted(root);
#if !defined(_WIN32)
    linksAndOddEntriesAreNotRecordings(root);
#endif
    theFileNameRules();
    std::cout << "clean_up_recordings_test: all passed\n";
    return 0;
}
