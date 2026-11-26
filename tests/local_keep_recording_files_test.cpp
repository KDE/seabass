// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// LocalKeepRecordingFiles on a real folder:
//
//   - sweepLeftovers() removes the encoder's leftover temporary files and
//     the tag copies earlier builds left, older than a day, and says each;
//     it leaves a younger one, one whose FLAC a job has in hand, one whose
//     name only looks like one (no 16 hex digits, an audio extension after
//     the suffix), a link named like one (and what it points to), a folder
//     named like one, and one in a folder below. An earlier build's tag
//     copy whose FLAC is not beside it (it can be the only copy of a kept
//     FLAC) is never removed: an old one is named in the notes; an
//     encoder's file without its FLAC goes as any;
//   - isWithin(): a folder inside the root, the root itself, a folder
//     still to be made inside it, and one outside it;
//   - withinByDevice(), the device rule, with device numbers given: a root
//     that is a mount point takes what is on its device, unless the root is
//     on the home folder's device; a root that is no mount point takes
//     nothing by device.

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
    // The FLAC oldTag is a copy of, beside it.
    const fs::path oldTagFlac = dir / "Set \xC3\xA9.flac";
    // Tag writer's copies whose FLAC is not there: an old and a young one;
    // and an encoder's file without its FLAC.
    const fs::path onlyCopy = dir / (std::string("Gone.flac.0000000000000011") + RecordingTagTempSuffix);
    const fs::path youngOnlyCopy = dir / (std::string("Fresh.flac.0000000000000022") + RecordingTagTempSuffix);
    const fs::path encodeNoFlac = dir / (std::string("Never.flac.0000000000000033") + RecordingEncodeTempSuffix);

    for (const fs::path &p : {oldEncode, oldTag, oldTagFlac, inHand, notHex, shortToken, audioAfter, music, pointedTo,
                              below, onlyCopy, encodeNoFlac}) {
        writeFile(p, "bytes", old);
    }
    writeFile(youngEncode, "bytes", young);
    writeFile(youngOnlyCopy, "bytes", young);
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
    check(!fs::exists(oldTag) && noteNames(notes, oldTag),
          "an old tag writer leftover beside its FLAC, a name outside ASCII: removed and said");
    check(fs::exists(oldTagFlac), "and its FLAC is left");
    check(!fs::exists(encodeNoFlac) && noteNames(notes, encodeNoFlac),
          "an old encoder leftover without its FLAC: removed and said (unverified by definition)");
    check(fs::exists(onlyCopy), "an old tag writer copy whose FLAC is not there: left (it may be the only copy)");
    const std::string onlyCopyNote = "A FLAC copy from an interrupted earlier run is still in the folder under this "
                                     "name; it may be a complete recording: " + seabass::pathToUtf8(onlyCopy);
    bool namedPlainly = false;
    for (const std::string &note : notes) {
        namedPlainly = namedPlainly || note == onlyCopyNote;
    }
    check(namedPlainly, "and named in the notes in plain words");
    check(fs::exists(youngOnlyCopy) && !noteNames(notes, youngOnlyCopy),
          "a younger one whose FLAC is not there: left, not named (another Seabass may be writing it)");
    check(notes.size() == 4, "nothing else is said: " + std::to_string(notes.size()) + " notes");
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

void deviceRule()
{
    using seabass::infrastructure::WithinByDevice;
    using seabass::infrastructure::withinByDevice;
    // A stick: its root is a mount point (device 7 under device 1), home on 1.
    WithinByDevice stick;
    stick.root = 7;
    stick.aboveRoot = 1;
    stick.home = 1;
    stick.there = 7;
    check(withinByDevice(stick), "a stick's mount point: a path on its device (a bind mount of it) is within");
    WithinByDevice elsewhere = stick;
    elsewhere.there = 1;
    check(!withinByDevice(elsewhere), "a stick's mount point: a path on the computer's disk is not within by device");
    WithinByDevice homeUnknown = stick;
    homeUnknown.home.reset();
    check(withinByDevice(homeUnknown), "the home folder's device unknown: the rule stays in force");
    // A "stick" whose root is a mount point on the home folder's device (a
    // btrfs subvolume's top, /home): the rule does not apply.
    WithinByDevice onHome;
    onHome.root = 1;
    onHome.aboveRoot = 2;
    onHome.home = 1;
    onHome.there = 1;
    check(!withinByDevice(onHome), "a root that is a mount point on the home device: a folder there is not within by device");
    WithinByDevice topOnHome;
    topOnHome.root = 1;
    topOnHome.aboveRoot = 1;
    topOnHome.rootIsTop = true;
    topOnHome.home = 1;
    topOnHome.there = 1;
    check(!withinByDevice(topOnHome), "\"/\" on the home device as root: not within by device");
    WithinByDevice topElsewhere = topOnHome;
    topElsewhere.home = 3;
    check(withinByDevice(topElsewhere), "\"/\" as root, home on another device: within by device");
    // A root that is no mount point takes nothing by device.
    WithinByDevice plainFolder;
    plainFolder.root = 7;
    plainFolder.aboveRoot = 7;
    plainFolder.home = 1;
    plainFolder.there = 7;
    check(!withinByDevice(plainFolder), "a root that is not a mount point: nothing by device");
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "local-keep-recording-files-test";
    fs::remove_all(root);
    fs::create_directories(root);
    sweep(root);
    within(root);
    deviceRule();
    fs::remove_all(root);
    if (failures > 0) {
        std::cout << "local_keep_recording_files_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "local_keep_recording_files_test: all passed\n";
    return 0;
}
