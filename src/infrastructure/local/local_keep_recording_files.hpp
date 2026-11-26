// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/use_cases/keep_recording.hpp"

namespace seabass::infrastructure
{

// application::KeepRecordingFiles on this computer's file systems, through
// std::filesystem, long paths prefixed. Paths are UTF-8.
//
// isWithin() compares as files, each folder up the way with the root, and
// on Linux and macOS also by device: when the root is a mount point (the
// stick's), anything on the same device is within it, so a bind mount of a
// folder of the stick elsewhere is caught. Any error on the way is "cannot
// tell" (nullopt), never "not within". On Windows the comparison is by
// file (fs::equivalent compares the volume serial and the file index) and
// no device rule is added.
class LocalKeepRecordingFiles : public application::KeepRecordingFiles
{
public:
    std::optional<std::uint64_t> fileSize(const std::string &path) override;
    std::optional<bool> exists(const std::string &path) override;
    std::optional<std::uint64_t> freeBytes(const std::string &folder) override;
    std::optional<std::string> readFile(const std::string &path, std::uint64_t maxBytes) override;
    std::optional<bool> isWithin(const std::string &path, const std::string &folder) override;
    std::optional<std::uint64_t> maxNameBytes(const std::string &folder) override;
    std::string makeFolder(const std::string &folder) override;
    std::string removeFile(const std::string &path) override;
    std::vector<std::string> sweepLeftovers(const std::string &folder,
                                            const std::function<bool(const std::string &)> &inHand) override;
};

}  // namespace seabass::infrastructure
