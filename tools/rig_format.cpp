// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: format a real USB stick and prove the result is the drive
// that was asked for -- rig check D1.
//
//   rig_format <mount point> <fat32|exfat> <label> [--execute]
//
// THIS ERASES THE DRIVE. Without --execute it detects, checks every
// refusal below and reports what it would have done, which is the mode a
// round can carry without consequences. With --execute it formats.
//
// The drive is named by its MOUNT POINT, never by a device path. The
// whole-disk path a format needs is read back out of this project's own
// RemovableMediaLocator, so the caller cannot hand a format a device node
// it merely typed or remembered -- the same discipline FormatUsbStick
// enforces internally, applied one level out, where the typo would be.
//
// Four refusals, all before anything destructive happens:
//
//   - the mount point must match exactly one detected removable drive;
//   - that drive must have a whole-disk path (a folder library or a
//     browsed backup has none, and neither is a drive);
//   - its capacity must be at or under RIG_FORMAT_MAX_BYTES, 256 GiB by
//     default, so a mounted internal disk is refused rather than erased;
//   - it must not be either reference backup's directory, which is the
//     one path on the rig that must never be written at all.
//
// Afterwards the drive has to come back by itself: mounted again, under
// the label that was asked for, carrying no rekordbox or Engine library
// and no files. The wait is for the desktop's own automount, which is
// what a person formatting a stick in the app also waits for.
//
// The stick is left EMPTY. Whatever runs this is responsible for putting
// it back from its reference; the rig does that in the check right after.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "application/ports/progress_reporter.hpp"
#include "application/ports/removable_media_locator.hpp"
#include "application/use_cases/format_usb_stick.hpp"
#include "domain/usb_filesystem.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

// 256 GiB. Every stick this rig runs on is well under it; an internal
// disk that somehow reached the refusals above is not.
constexpr std::uint64_t DefaultMaxBytes = 256ULL * 1024 * 1024 * 1024;

class PrintingProgress : public application::ProgressReporter
{
public:
    void start(const std::string &label, size_t) override { std::cout << "   " << label << "\n"; }
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { std::cout << "   warning: " << message << "\n"; }
};

std::uint64_t maxBytes()
{
    const char *raw = std::getenv("RIG_FORMAT_MAX_BYTES");
    if (raw == nullptr || *raw == '\0') {
        return DefaultMaxBytes;
    }
    try {
        return std::stoull(raw);
    } catch (const std::exception &) {
        return DefaultMaxBytes;
    }
}

// Both references, as directories: a stick that somehow mounted at one of
// them is the one drive on this machine that must never be touched.
bool isReferenceDirectory(const fs::path &mountPoint)
{
    for (const char *variable : {"RIG_REFERENCE_A", "RIG_REFERENCE_B"}) {
        const char *raw = std::getenv(variable);
        if (raw == nullptr || *raw == '\0') {
            continue;
        }
        std::error_code ec;
        // The variable is read as UTF-8, like argv.
        const fs::path reference = pathFromUtf8(raw).parent_path();
        if (!reference.empty() && fs::equivalent(reference, mountPoint, ec)) {
            return true;
        }
    }
    return false;
}

std::vector<application::DetectedStick> detect(application::RemovableMediaLocator &locator)
{
    return locator.detect();
}

// The one drive mounted at this path, or nothing. Two matches is a
// refusal rather than a choice: this rig does not guess which drive a
// format was meant for.
const application::DetectedStick *stickAt(const std::vector<application::DetectedStick> &sticks,
                                          const fs::path &mountPoint, std::string &why)
{
    const application::DetectedStick *found = nullptr;
    for (const auto &stick : sticks) {
        if (stick.mountPoint.empty()) {
            continue;
        }
        std::error_code ec;
        if (!fs::equivalent(pathFromUtf8(stick.mountPoint), mountPoint, ec)) {
            continue;
        }
        if (found != nullptr) {
            why = "more than one detected drive is mounted at " + pathToUtf8(mountPoint);
            return nullptr;
        }
        found = &stick;
    }
    if (found == nullptr) {
        why = "no detected removable drive is mounted at " + pathToUtf8(mountPoint);
    }
    return found;
}

bool parseFilesystem(const std::string &text, domain::UsbFilesystem &out)
{
    if (text == "fat32" || text == "FAT32") {
        out = domain::UsbFilesystem::Fat32;
        return true;
    }
    if (text == "exfat" || text == "exFAT" || text == "EXFAT") {
        out = domain::UsbFilesystem::ExFat;
        return true;
    }
    return false;
}

// What is on the drive now, so a format that quietly did nothing cannot
// read as success. Counts anything the filesystem itself did not put
// there: a fresh FAT32 or exFAT volume has no entries a user would call
// files, but some formatters leave a volume-label entry or a lost+found.
std::vector<std::string> contentsOf(const fs::path &root)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        if (name == "System Volume Information" || name == ".Spotlight-V100" || name == ".fseventsd"
            || name == ".Trashes" || name == "lost+found" || name == ".metadata_never_index") {
            continue;
        }
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: rig_format <mount point> <fat32|exfat> <label> [--execute]\n";
        return 2;
    }
    const fs::path mountPoint = pathFromUtf8(argv[1]);
    const std::string filesystemText = argv[2];
    const std::string label = argv[3];
    const bool execute = (argc == 5 && std::string(argv[4]) == "--execute");
    if (argc == 5 && !execute) {
        std::cerr << "unknown argument: " << argv[4] << "\n";
        return 2;
    }

    domain::UsbFilesystem filesystem = domain::UsbFilesystem::ExFat;
    if (!parseFilesystem(filesystemText, filesystem)) {
        std::cout << "not a filesystem this formats: " << filesystemText << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    try {
        auto locator = infrastructure::media::createRemovableMediaLocator();
        auto mounter = infrastructure::media::createRemovableMediaMounter();
        auto formatter = infrastructure::media::createUsbFormatter();

        auto sticks = detect(*locator);
        std::string why;
        const application::DetectedStick *target = stickAt(sticks, mountPoint, why);
        if (target == nullptr) {
            std::cout << why << "\nRIG RESULT: FAIL\n";
            return 1;
        }

        std::cout << "target: " << target->mountPoint << " (" << target->devicePath << " on "
                  << (target->wholeDiskPath.empty() ? std::string("no whole disk") : target->wholeDiskPath) << "), label \""
                  << target->label << "\", " << target->capacityBytes << " bytes\n";
        std::cout << "asked for: " << domain::usbFilesystemName(filesystem) << ", label \"" << label << "\"\n";

        if (target->isFolder || target->isBrowsedBackup) {
            std::cout << "that is a folder library, not a drive\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (target->wholeDiskPath.empty()) {
            std::cout << "no whole-disk path, so there is nothing a format could be pointed at\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (target->capacityBytes > maxBytes()) {
            std::cout << "refusing: " << target->capacityBytes << " bytes is over the " << maxBytes()
                      << " byte ceiling (RIG_FORMAT_MAX_BYTES)\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (isReferenceDirectory(mountPoint)) {
            std::cout << "refusing: that is a reference backup's own directory\nRIG RESULT: FAIL\n";
            return 1;
        }

        const std::vector<std::string> before = contentsOf(mountPoint);
        std::cout << "holds " << before.size() << " top-level entries before\n";

        if (!execute) {
            std::cout << "dry run, nothing was erased; pass --execute to format\nRIG RESULT: PASS\n";
            return 0;
        }

        PrintingProgress progress;
        application::FormatUsbStick useCase(*locator, *mounter, *formatter);
        std::string error;
        std::cout << "formatting " << target->wholeDiskPath << "\n";
        // wholeDiskPath is read from the detection above and handed
        // straight back, so the use case's own re-detection is comparing
        // against a path this process never composed.
        const std::string wholeDiskPath = target->wholeDiskPath;
        // Who it is, not only where it is: the use case refuses if the
        // drive at that path is a different one by the time it looks.
        const application::StickIdentity chosen = target->identity;
        if (!useCase.execute(wholeDiskPath, chosen, filesystem, label, error, progress)) {
            std::cout << "format refused or failed: " << error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "formatter reported success\n";

        // The drive has to come back on its own. This waits for the
        // desktop's automount, which is what a person formatting a stick
        // in the app waits for too -- and a drive that never comes back
        // is a real failure of this feature, not of this check.
        const application::DetectedStick *formatted = nullptr;
        std::vector<application::DetectedStick> after;
        for (int attempt = 0; attempt < 30; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            after = detect(*locator);
            for (const auto &stick : after) {
                if (stick.wholeDiskPath == wholeDiskPath && stick.mounted && !stick.mountPoint.empty()) {
                    formatted = &stick;
                    break;
                }
            }
            if (formatted != nullptr) {
                break;
            }
        }
        if (formatted == nullptr) {
            std::cout << "the drive never came back mounted within 30s\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "back at " << formatted->mountPoint << ", label \"" << formatted->label << "\"\n";

        bool pass = true;
        if (formatted->label != label) {
            std::cout << "label is \"" << formatted->label << "\", asked for \"" << label << "\"\n";
            pass = false;
        }
        if (formatted->rekordboxPath || formatted->enginePath) {
            std::cout << "a library survived the format, which means it did not happen\n";
            pass = false;
        }
        const std::vector<std::string> left = contentsOf(pathFromUtf8(formatted->mountPoint));
        if (!left.empty()) {
            std::cout << left.size() << " entries survived the format:";
            for (const auto &name : left) {
                std::cout << " " << name;
            }
            std::cout << "\n";
            pass = false;
        }

        std::cout << (pass ? "RIG RESULT: PASS\n" : "RIG RESULT: FAIL\n");
        return pass ? 0 : 1;
    } catch (const std::exception &error) {
        std::cout << "threw: " << error.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
}
