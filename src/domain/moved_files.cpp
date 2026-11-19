// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/moved_files.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "domain/matching_policy.hpp"
#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

// The last segment of a path spelled either way: a catalog row's
// `filename` field may be empty (OneLibrary rows have one, a stray file
// has what the walk gave it), and the path always has one.
std::string basenameOf(const std::string &path)
{
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string nameOf(const Track &track)
{
    return normalizeFilename(track.filename.empty() ? basenameOf(track.filePath) : track.filename);
}

// What one finding's rows say about the file, taken from whichever row
// says it: rekordbox records the size and OneLibrary mirrors the title,
// and a row with none of it is still that track.
struct RowFacts
{
    std::string name;
    std::uint64_t sizeBytes = 0;
    std::optional<std::string> tagKey;
    double durationSeconds = 0.0;
};

RowFacts factsOf(const MovedFileFinding &finding)
{
    RowFacts facts;
    facts.name = normalizeFilename(basenameOf(finding.expectedPath));
    for (const Track &row : finding.rows) {
        if (facts.sizeBytes == 0 && row.catalogFileBytes > 0) {
            facts.sizeBytes = row.catalogFileBytes;
        }
        if (!facts.tagKey) {
            facts.tagKey = titleArtistKey(row);
        }
        if (facts.durationSeconds <= 0.0 && row.durationSeconds > 0.0) {
            facts.durationSeconds = row.durationSeconds;
        }
    }
    return facts;
}

bool rank(const MovedFileCandidate &a, const MovedFileCandidate &b)
{
    if (a.confidence != b.confidence) {
        return a.confidence == MovedFileCandidate::Confidence::High;
    }
    return a.file.filePath < b.file.filePath;
}

}  // namespace

const MovedFileCandidate *MovedFileFinding::only() const
{
    return candidates.size() == 1 ? &candidates.front() : nullptr;
}

bool MovedFileFinding::oneClick() const
{
    const MovedFileCandidate *one = only();
    return one != nullptr && one->confidence == MovedFileCandidate::Confidence::High;
}

bool MovedFileFinding::oneToConfirm() const
{
    const MovedFileCandidate *one = only();
    return one != nullptr && one->confidence == MovedFileCandidate::Confidence::NeedsConfirming;
}

std::string MovedFileFinding::title() const
{
    for (const Track &row : rows) {
        if (!row.title.empty()) {
            return row.title;
        }
    }
    return {};
}

std::string MovedFileFinding::artist() const
{
    for (const Track &row : rows) {
        if (!row.artist.empty()) {
            return row.artist;
        }
    }
    return {};
}

std::vector<MovedFileFinding> findMovedFiles(const std::vector<Track> &missingRows,
                                             const std::vector<Track> &filesOnDisk,
                                             const std::function<std::string(const std::string &)> &pathKey)
{
    return findMovedFiles(missingRows, filesOnDisk, pathKey, MatchingPolicy::exactMatchSeconds());
}

std::vector<MovedFileFinding> findMovedFiles(const std::vector<Track> &missingRows,
                                             const std::vector<Track> &filesOnDisk,
                                             const std::function<std::string(const std::string &)> &pathKey,
                                             double durationToleranceSeconds)
{
    // One finding per expected path, the rows of every catalog on it.
    std::vector<MovedFileFinding> findings;
    std::map<std::string, size_t> findingByKey;
    for (const Track &row : missingRows) {
        if (row.filePath.empty() || !row.streamingSource.empty()) {
            continue;
        }
        const std::string key = pathKey(row.filePath);
        auto it = findingByKey.find(key);
        if (it == findingByKey.end()) {
            MovedFileFinding finding;
            finding.expectedPath = row.filePath;
            findings.push_back(std::move(finding));
            it = findingByKey.emplace(key, findings.size() - 1).first;
        }
        findings[it->second].rows.push_back(row);
    }

    // The files by the two things a row can be found by.
    std::map<std::string, std::vector<const Track *>> filesByName;
    std::map<std::string, std::vector<const Track *>> filesByTagKey;
    for (const Track &file : filesOnDisk) {
        if (file.filePath.empty()) {
            continue;
        }
        filesByName[nameOf(file)].push_back(&file);
        if (auto key = titleArtistKey(file)) {
            filesByTagKey[*key].push_back(&file);
        }
    }

    // Which findings each file is a candidate for, to tell one that two
    // tracks claim.
    std::map<std::string, std::vector<size_t>> claimsByFileKey;
    for (size_t i = 0; i < findings.size(); ++i) {
        MovedFileFinding &finding = findings[i];
        const RowFacts facts = factsOf(finding);
        std::map<std::string, MovedFileCandidate> byFileKey;

        if (!facts.name.empty()) {
            const auto named = filesByName.find(facts.name);
            if (named != filesByName.end()) {
                for (const Track *file : named->second) {
                    MovedFileCandidate candidate;
                    candidate.file = *file;
                    if (facts.sizeBytes > 0 && file->fileSizeBytes > 0 && facts.sizeBytes == file->fileSizeBytes) {
                        candidate.confidence = MovedFileCandidate::Confidence::High;
                        candidate.reason = "the same name and the same size";
                    } else if (facts.sizeBytes > 0 && file->fileSizeBytes > 0) {
                        candidate.confidence = MovedFileCandidate::Confidence::NeedsConfirming;
                        candidate.reason = "the same name, but " + std::to_string(file->fileSizeBytes)
                            + " bytes where the library recorded " + std::to_string(facts.sizeBytes);
                    } else {
                        candidate.confidence = MovedFileCandidate::Confidence::NeedsConfirming;
                        candidate.reason = "the same name; the library does not record the file's size, so the "
                                           "size cannot be compared";
                    }
                    byFileKey[pathKey(file->filePath)] = std::move(candidate);
                }
            }
        }

        // Another name, the same tags and length: matchTracks' own
        // fallback, which two real lengths have to pass. A length that
        // cannot be read stands aside there only when nothing else could
        // be confused with it; here every such pair needs a person
        // anyway, so a missing length is simply not a match.
        if (facts.tagKey && facts.durationSeconds > 0.0) {
            const auto tagged = filesByTagKey.find(*facts.tagKey);
            if (tagged != filesByTagKey.end()) {
                for (const Track *file : tagged->second) {
                    const std::string fileKey = pathKey(file->filePath);
                    if (byFileKey.contains(fileKey) || file->durationSeconds <= 0.0
                        || std::fabs(file->durationSeconds - facts.durationSeconds) > durationToleranceSeconds) {
                        continue;
                    }
                    MovedFileCandidate candidate;
                    candidate.file = *file;
                    candidate.confidence = MovedFileCandidate::Confidence::NeedsConfirming;
                    candidate.reason = "another name, but the same title, artist and length";
                    byFileKey[fileKey] = std::move(candidate);
                }
            }
        }

        for (auto &[fileKey, candidate] : byFileKey) {
            claimsByFileKey[fileKey].push_back(i);
            finding.candidates.push_back(std::move(candidate));
        }
    }

    // A file two tracks could be goes back to one place only, and which
    // is not for this code to pick.
    for (auto &finding : findings) {
        for (auto &candidate : finding.candidates) {
            const auto &claims = claimsByFileKey[pathKey(candidate.file.filePath)];
            if (claims.size() < 2) {
                continue;
            }
            std::string others;
            for (size_t other : claims) {
                if (&findings[other] == &finding) {
                    continue;
                }
                const std::string name = findings[other].title().empty() ? basenameOf(findings[other].expectedPath)
                                                                         : findings[other].title();
                others += (others.empty() ? "" : ", ") + ("\"" + name + "\"");
            }
            candidate.confidence = MovedFileCandidate::Confidence::NeedsConfirming;
            candidate.reason += "; it also matches " + others;
        }
        std::sort(finding.candidates.begin(), finding.candidates.end(), rank);
    }
    return findings;
}

}  // namespace seabass::domain
