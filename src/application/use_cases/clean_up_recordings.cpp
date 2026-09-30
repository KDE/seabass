// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/clean_up_recordings.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "infrastructure/file_clock.hpp"
#include "infrastructure/file_placement.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;
using infrastructure::longPathSafe;

namespace
{

std::string asciiLower(std::string text)
{
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::optional<RecordingSource> sourceForFolderName(const std::string &folderUtf8)
{
    const std::string lower = asciiLower(folderUtf8);
    for (const auto &spelling : recordingFolderSpellings()) {
        if (lower == asciiLower(spelling.name)) {
            return spelling.source;
        }
    }
    return std::nullopt;
}

bool stillPresent(const fs::path &path)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(longPathSafe(path), ec);
    // Unknown counts as there: "could not ask" must never read as "gone".
    return !fs::status_known(status) || fs::exists(status);
}

fs::path withoutTrailingSeparator(const fs::path &path)
{
    fs::path normal = path.lexically_normal();
    if (!normal.has_filename() && normal.has_parent_path() && normal != normal.root_path()) {
        normal = normal.parent_path();
    }
    return normal;
}

// The rule the listing applies, applied again to a path handed in: an
// audio file directly inside a recording folder directly under the stick
// root, both real entries (no symlinks). Empty when it is a recording.
std::string whyNotARecording(const fs::path &stickRoot, const fs::path &path)
{
    const fs::path normal = path.lexically_normal();
    const fs::path folder = normal.parent_path();
    if (withoutTrailingSeparator(folder.parent_path()) != withoutTrailingSeparator(stickRoot)) {
        return "not directly inside a recording folder on this stick";
    }
    if (!sourceForFolderName(pathToUtf8(folder.filename()))) {
        return "its folder is not a recording folder";
    }
    if (!isRecordingFileName(pathToUtf8(normal.filename()))) {
        return "not an audio file";
    }
    std::error_code ec;
    const fs::file_status folderStatus = fs::symlink_status(longPathSafe(folder), ec);
    if (ec || !fs::is_directory(folderStatus)) {
        return "its folder is not a plain folder";
    }
    const fs::file_status status = fs::symlink_status(longPathSafe(normal), ec);
    if (ec || !fs::exists(status)) {
        return "no longer on the stick";
    }
    if (!fs::is_regular_file(status)) {
        return "not a plain file";
    }
    return {};
}

std::int64_t unixSeconds(fs::file_time_type time)
{
    return std::chrono::duration_cast<std::chrono::seconds>(infrastructure::toSystemClock(time).time_since_epoch())
        .count();
}

}  // namespace

const std::vector<RecordingFolderSpelling> &recordingFolderSpellings()
{
    static const std::vector<RecordingFolderSpelling> spellings = {
        {"Sessions", RecordingSource::EngineOs},
        {"PIONEER REC", RecordingSource::Pioneer},
        {"PIONEER DJ REC", RecordingSource::Pioneer},
        {"ALPHATHETA REC", RecordingSource::AlphaTheta},
    };
    return spellings;
}

const char *recordingSourceKey(RecordingSource source)
{
    switch (source) {
        case RecordingSource::EngineOs:
            return "engine";
        case RecordingSource::Pioneer:
            return "pioneer";
        case RecordingSource::AlphaTheta:
            return "alphatheta";
    }
    return "engine";
}

bool isRecordingFileName(const std::string &fileNameUtf8)
{
    if (fileNameUtf8.empty() || fileNameUtf8.front() == '.') {
        return false;
    }
    static const std::array<std::string_view, 6> extensions = {".wav", ".aif", ".aiff", ".mp3", ".flac", ".m4a"};
    const std::string lower = asciiLower(fileNameUtf8);
    for (const auto extension : extensions) {
        if (lower.size() > extension.size() && lower.compare(lower.size() - extension.size(), extension.size(),
                                                             extension.data(), extension.size()) == 0) {
            return true;
        }
    }
    return false;
}

RecordingListing listRecordings(const fs::path &stickRoot, TrackDurationProbe &durations)
{
    RecordingListing listing;
    std::error_code ec;
    std::vector<std::pair<fs::path, RecordingSource>> folders;
    for (fs::directory_iterator it(longPathSafe(stickRoot), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        const auto source = sourceForFolderName(name);
        if (!source) {
            continue;
        }
        std::error_code statusEc;
        if (!fs::is_directory(it->symlink_status(statusEc)) || statusEc) {
            continue;  // a file or a link by that name is not a recording folder
        }
        folders.emplace_back(stickRoot / it->path().filename(), *source);
    }
    if (ec) {
        listing.unreadableFolders.push_back(pathToUtf8(stickRoot) + ": " + ec.message());
    }

    for (const auto &[folder, source] : folders) {
        const std::string folderName = pathToUtf8(folder.filename());
        std::error_code walkEc;
        for (fs::directory_iterator it(longPathSafe(folder), walkEc), end; !walkEc && it != end; it.increment(walkEc)) {
            const fs::path path = folder / it->path().filename();
            const std::string fileName = pathToUtf8(path.filename());
            std::error_code statusEc;
            const fs::file_status status = it->symlink_status(statusEc);
            std::string reason;
            if (statusEc) {
                reason = "could not be read: " + statusEc.message();
            } else if (fs::is_directory(status)) {
                reason = "a folder";
            } else if (!fs::is_regular_file(status)) {
                reason = "not a plain file";
            } else if (!fileName.empty() && fileName.front() == '.') {
                reason = "a hidden file";
            } else if (!isRecordingFileName(fileName)) {
                reason = "not an audio file";
            }
            if (!reason.empty()) {
                listing.leftAlone.push_back({pathToUtf8(path), fileName, folderName, reason});
                continue;
            }
            std::error_code sizeEc;
            const std::uint64_t size = fs::file_size(longPathSafe(path), sizeEc);
            std::error_code timeEc;
            const fs::file_time_type time = fs::last_write_time(longPathSafe(path), timeEc);
            if (sizeEc) {
                listing.leftAlone.push_back({pathToUtf8(path), fileName, folderName,
                                             "could not be read: " + sizeEc.message()});
                continue;
            }
            Recording recording;
            recording.path = pathToUtf8(path);
            recording.fileName = fileName;
            recording.folderName = folderName;
            recording.source = source;
            recording.sizeBytes = size;
            recording.modifiedUnix = timeEc ? 0 : unixSeconds(time);
            recording.durationSeconds = durations.durationSeconds(recording.path);
            listing.totalBytes += size;
            listing.recordings.push_back(std::move(recording));
        }
        if (walkEc) {
            listing.unreadableFolders.push_back(folderName + ": " + walkEc.message());
        }
    }
    std::sort(listing.recordings.begin(), listing.recordings.end(), [](const Recording &a, const Recording &b) {
        return a.folderName != b.folderName ? a.folderName < b.folderName : a.fileName < b.fileName;
    });
    return listing;
}

RecordingsReport deleteRecordings(const fs::path &stickRoot, const std::vector<std::string> &recordingPaths,
                                  CancellationToken cancel,
                                  const std::function<void(const RecordingsProgress &)> &progress,
                                  const RecordingDeleteHooks &hooks)
{
    using Status = RecordingOutcome::Status;
    auto volumeOf = [&hooks](const fs::path &path) {
        return hooks.volumeIdOf ? hooks.volumeIdOf(path) : infrastructure::volumeIdOf(longPathSafe(path));
    };
    const std::optional<std::uint64_t> stickVolume = volumeOf(stickRoot);

    RecordingsReport report;
    report.requested = static_cast<int>(recordingPaths.size());
    std::set<std::string> seen;
    // The path each outcome is validated as, and then acted on as.
    std::vector<fs::path> normalized;
    for (const std::string &pathUtf8 : recordingPaths) {
        RecordingOutcome outcome;
        const fs::path path = pathFromUtf8(pathUtf8).lexically_normal();
        normalized.push_back(path);
        outcome.path = pathUtf8;
        outcome.fileName = pathToUtf8(path.filename());
        std::error_code ec;
        outcome.sizeBytes = fs::file_size(longPathSafe(path), ec);
        if (ec) {
            outcome.sizeBytes = 0;
        }
        if (!seen.insert(pathToUtf8(path)).second) {
            outcome.status = Status::Refused;
            outcome.reason = "listed twice";
        } else if (std::string why = whyNotARecording(stickRoot, path); !why.empty()) {
            outcome.status = Status::Refused;
            outcome.reason = why;
        }
        report.outcomes.push_back(std::move(outcome));
    }

    RecordingsProgress state;
    state.fileCount = report.requested;
    for (int i = 0; i < report.requested; ++i) {
        RecordingOutcome &outcome = report.outcomes[static_cast<std::size_t>(i)];
        if (outcome.status == Status::Refused) {
            continue;
        }
        // Between files, never inside one: the file in flight finishes.
        if (cancel.cancelled()) {
            continue;  // stays NotAttempted
        }
        const fs::path source = normalized[static_cast<std::size_t>(i)];
        state.fileIndex = i;
        state.fileName = outcome.fileName;
        if (progress) {
            progress(state);
        }
        if (hooks.beforeDelete) {
            hooks.beforeDelete(source);
        }
        // removeEntry() answers "gone" for a file that was never there, so
        // it must still be a plain file, on the volume the run began on.
        {
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(longPathSafe(source), ec);
            const std::optional<std::uint64_t> volumeNow = volumeOf(stickRoot);
            if (!fs::status_known(status) || !fs::is_regular_file(status) || !stickVolume || volumeNow != stickVolume) {
                outcome.status = Status::Vanished;
                outcome.reason = "no longer on the stick when its turn to be deleted came (was the stick removed?)";
                continue;
            }
        }
        std::string failure;
        if (!infrastructure::removeEntry(source, failure)) {
            outcome.status = Status::DeleteFailed;
            outcome.reason = "could not delete it from the stick: " + failure;
            continue;
        }
        if (hooks.afterDelete) {
            hooks.afterDelete(source);
        }
        if (stillPresent(source)) {
            outcome.status = Status::DeleteFailed;
            outcome.reason = "still on the stick after it was deleted";
            continue;
        }
        outcome.status = Status::Deleted;
    }

    // Every claim, asked of the filesystem again now that the run is over.
    for (std::size_t i = 0; i < report.outcomes.size(); ++i) {
        RecordingOutcome &outcome = report.outcomes[i];
        if (outcome.status == Status::Deleted && stillPresent(normalized[i])) {
            outcome.status = Status::DeleteFailed;
            outcome.reason = "still on the stick when checked after the run";
        }
    }

    for (const RecordingOutcome &outcome : report.outcomes) {
        switch (outcome.status) {
            case Status::Deleted:
                report.deleted++;
                break;
            case Status::DeleteFailed:
            case Status::Refused:
            case Status::Vanished:
                report.failed++;
                break;
            case Status::NotAttempted:
                report.notAttempted++;
                break;
        }
    }
    // Every requested path counted exactly once.
    if (report.deleted + report.failed + report.notAttempted != report.requested
        || report.outcomes.size() != recordingPaths.size()) {
        throw std::logic_error("recordings report does not add up to what was asked for");
    }
    report.cancelled = cancel.cancelled() && report.notAttempted > 0;
    return report;
}

}  // namespace seabass::application
