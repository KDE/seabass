// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// How many of a stick's analysis files hold legacy and modern cue lists
// that disagree (#60), counted from what the readers found while reading
// the tracks' cues (Track::cueLists), so the stick's summary says it at a
// glance before a sync. Each file once, however many rows name it, as
// Library Health's Cue lists check counts them.
//
// Honest: `disagree` means something only when complete(), every file
// examined. A file that could not be read, or whose cues were not read
// yet, is not a file whose lists agree.
struct CueListCount
{
    int files = 0;       // analysis files the rows name
    int examined = 0;    // both files read and compared
    int disagree = 0;    // of those, the lists differ
    int unreadable = 0;  // a file missing, or not an analysis file
    int notChecked = 0;  // cues not read yet

    bool complete() const { return files > 0 && examined == files; }
};

namespace detail
{
// One file however the rows spell it: export.pdb pads its strings with
// spaces, OneLibrary may use the other separator, and a FAT stick does not
// tell case apart. The trim and separators normalizedPathKey() makes, kept
// here so the domain does not reach for it.
inline std::string analysisFileKey(std::string path)
{
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\0')) {
        path.pop_back();
    }
    for (char &c : path) {
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}
}  // namespace detail

inline CueListCount countCueLists(const std::vector<Track> &tracks)
{
    CueListCount count;
    std::set<std::string> seen;
    for (const Track &track : tracks) {
        if (track.analysisFile.empty() || !seen.insert(detail::analysisFileKey(track.analysisFile)).second) {
            continue;
        }
        ++count.files;
        switch (track.cueLists) {
        case Track::CueListsCheck::Examined:
            ++count.examined;
            break;
        case Track::CueListsCheck::Disagree:
            ++count.examined;
            ++count.disagree;
            break;
        case Track::CueListsCheck::Unreadable:
            ++count.unreadable;
            break;
        case Track::CueListsCheck::NotChecked:
            ++count.notChecked;
            break;
        }
    }
    return count;
}

// What a summary says about it: "3 cue lists disagree" once every file
// was examined and some disagree, "2 analysis files could not be read"
// when any could not be, and nothing while the cues are still being read
// or when there are no analysis files. When every file was examined and
// all agree, nothing too, unless `sayWhenAgree`: "Cue lists agree in all
// 1383 analysis files". The count of disagreements is never given beside
// an unread file: those might disagree as well.
inline std::string describeCueListCount(const CueListCount &count, bool sayWhenAgree = false)
{
    if (count.files == 0 || count.notChecked > 0) {
        return {};
    }
    if (count.unreadable > 0) {
        return std::to_string(count.unreadable)
            + (count.unreadable == 1 ? " analysis file could not be read" : " analysis files could not be read");
    }
    if (count.disagree > 0) {
        return std::to_string(count.disagree) + (count.disagree == 1 ? " cue list disagrees" : " cue lists disagree");
    }
    if (sayWhenAgree) {
        return "Cue lists agree in all " + std::to_string(count.examined)
            + (count.examined == 1 ? " analysis file" : " analysis files");
    }
    return {};
}

}  // namespace seabass::domain
