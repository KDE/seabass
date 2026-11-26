// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// LocalKeepRecordingFiles on a real folder:
//
//   - sweepLeftovers() removes the encoder's and the tag writer's leftover
//     temporary files older than a day, and says each; it leaves a younger
//     one, one whose FLAC a job has in hand, one whose name only looks
//     like one (no 16 hex digits, an audio extension after the suffix), a
//     link named like one (and what it points to), a folder named like
//     one, and one in a folder below;
//   - isWithin(): a folder inside the root, the root itself, a folder
//     still to be made inside it, and one outside it.

#include "infrastructure/local/local_keep_recording_files.hpp"

#include "infrastructure/audio/recording_temp_names.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using seabass::infrastructure::LocalKeepRecordingFiles;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (ok) {
        std::cout << "ok: " << what << "\n";
    } else {
        std::cout << "FAILED: " << what << "\n";
        ++failures;
    }
}

void writeFile(const fs::path &path, const std::string &content, std::chrono::hours age)
{
    std::ofstream(path, std::ios::binary) << content;
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

bool noteNames(const std::vector<std::string> &notes, const fs::path &path)
{
    for (const std::string &note : notes) {
        if (note.find(seabass::pathToUtf8(path)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void sweep(const fs::path &root)
{
    using seabass::infrastructure::audio::RecordingEncodeTempSuffix;
    using seabass::infrastructure::audio::RecordingTagTempSuffix;
    const fs::path dir = root / "sweep";
    fs::create_directories(dir / "below");
    const std::chrono::hours old(25);
    const std::chrono::hours young(23);

    const fs::path oldEncode = dir / (std::string("Set.flac.0123456789abcdef") + RecordingEncodeTempSuffix);
    const fs::path oldTag = dir / (std::string("Set \xC3\xA9.flac.fedcba9876543210") + RecordingTagTempSuffix);
    const fs::path youngEncode = dir / (std::string("Set.flac.00000000000000aa") + RecordingEncodeTempSuffix);
    const fs::path inHand = dir / (std::string("Busy.flac.00000000000000bb") + RecordingTagTempSuffix);
    const fs::path notHex = dir / (std::string("Set.flac.0123456789abcdeX") + RecordingEncodeTempSuffix);
    const fs::path shortToken = dir / (std::string("Set.flac.0123456789abcde") + RecordingEncodeTempSuffix);
    const fs::path audioAfter = dir / (std::string("Set.flac.0123456789abcdef") + RecordingEncodeTempSuffix + ".flac");
    const fs::path music = dir / "Set.flac";
    const fs::path pointedTo = dir / "elsewhere.bin";
    const fs::path link = dir / (std::string("Link.flac.00000000000000cc") + RecordingEncodeTempSuffix);
    const fs::path folderNamed = dir / (std::string("Dir.flac.00000000000000dd") + RecordingEncodeTempSuffix);
    const fs::path below = dir / "below" / (std::string("Set.flac.00000000000000ee") + RecordingEncodeTempSuffix);

    for (const fs::path &p : {oldEncode, oldTag, inHand, notHex, shortToken, audioAfter, music, pointedTo, below}) {
        writeFile(p, "bytes", old);
    }
    writeFile(youngEncode, "bytes", young);
    std::error_code ec;
    fs::create_symlink(pointedTo, link, ec);
    check(!ec, "the test made its link (" + ec.message() + ")");
    fs::create_directory(folderNamed);
    fs::last_write_time(folderNamed, fs::file_time_type::clock::now() - old);

    LocalKeepRecordingFiles files;
    std::vector<std::string> asked;
    const std::vector<std::string> notes =
        files.sweepLeftovers(seabass::pathToUtf8(dir), [&asked](const std::string &flacName) {
            asked.push_back(flacName);
            return flacName == "Busy.flac";
        });
    check(!fs::exists(oldEncode) && noteNames(notes, oldEncode), "an old encoder leftover: removed and said");
    check(!fs::exists(oldTag) && noteNames(notes, oldTag), "an old tag writer leftover, a name outside ASCII: removed and said");
    check(notes.size() == 2, "nothing else is said: " + std::to_string(notes.size()) + " notes");
    check(fs::exists(youngEncode), "a leftover younger than a day: left (another Seabass may be writing it)");
    check(fs::exists(inHand), "a leftover of a FLAC a job has in hand: left");
    bool askedBusy = false;
    for (const std::string &name : asked) {
        askedBusy = askedBusy || name == "Busy.flac";
    }
    check(askedBusy, "the job registry is asked by the FLAC's name");
    check(fs::exists(notHex) && fs::exists(shortToken), "names without 16 hex digits: left");
    check(fs::exists(audioAfter) && fs::exists(music), "a name ending in .flac: left");
    check(fs::is_symlink(fs::symlink_status(link)) && fs::exists(pointedTo), "a link named like a leftover: neither it nor what it points to");
    check(fs::is_directory(folderNamed), "a folder named like a leftover: left");
    check(fs::exists(below), "a leftover in a folder below: not looked for");
}

void within(const fs::path &root)
{
    const fs::path stick = root / "stick";
    const fs::path inside = stick / "Sessions";
    const fs::path outside = root / "computer" / "Music";
    fs::create_directories(inside);
    fs::create_directories(outside);
    LocalKeepRecordingFiles files;
    const std::string stickText = seabass::pathToUtf8(stick);
    check(files.isWithin(seabass::pathToUtf8(inside), stickText) == std::optional<bool>(true), "a folder on the stick: within");
    check(files.isWithin(stickText, stickText) == std::optional<bool>(true), "the stick's root: within");
    check(files.isWithin(seabass::pathToUtf8(inside / "to" / "be" / "made"), stickText) == std::optional<bool>(true),
          "a folder still to be made on the stick: within");
    check(files.isWithin(seabass::pathToUtf8(outside), stickText) == std::optional<bool>(false),
          "a folder on this computer: not within");
    check(!files.isWithin(seabass::pathToUtf8(outside), seabass::pathToUtf8(root / "no-such-stick")).has_value(),
          "a stick that is not there: cannot tell");
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "local-keep-recording-files-test";
    fs::remove_all(root);
    fs::create_directories(root);
    sweep(root);
    within(root);
    fs::remove_all(root);
    if (failures > 0) {
        std::cout << "local_keep_recording_files_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "local_keep_recording_files_test: all passed\n";
    return 0;
}
