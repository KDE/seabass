// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"

namespace seabass::infrastructure::cleanup
{

// True for the file extensions this project is willing to treat as audio.
// Case-insensitive.
//
// Deliberately a closed list rather than "anything that is not obviously
// something else". Everything this list admits becomes a candidate for
// deletion, so a wrong "yes" is dangerous while a wrong "no" only means
// a stray file goes unnoticed and stays on the stick. When in doubt, say
// no.
bool isAudioExtension(const std::string &path);

// Every audio file at or under `root`, walked with DirectoryReader
// rather than fs::recursive_directory_iterator.
//
// That is not a style preference. On Windows, directory_iterator handed
// a \\?\ path does not fail -- it silently enumerates the process's
// working directory and reports success, so a walk built on it can miss
// whole subtrees while looking like it worked. This walk feeds a
// deletion decision, and a subtree silently missing from it means files
// that ARE referenced never appear in the referenced set. See
// infrastructure/long_paths.hpp for the full account.
//
// Symlinks are neither followed nor reported: a link's target can sit
// outside the tree, and nothing here should ever offer to delete
// something it reached by leaving the stick.
//
// Unreadable entries are skipped rather than aborting the walk -- one
// bad directory on a real stick should not lose the other 2000 files --
// but see `incomplete` on the result, which callers must respect.
struct AudioFileWalkResult
{
    std::vector<application::AudioFileOnDisk> files;

    // True when at least one directory could not be read, so the file
    // list is a subset of what is actually on the stick.
    //
    // This is a reporting problem, not a safety one, and it is worth
    // being precise about which: a file the walk never saw simply never
    // becomes a deletion candidate, so an incomplete walk can only ever
    // propose *fewer* deletions than a complete one. What it must not do
    // is present its total as the whole truth -- "632 files, 8.66 GB
    // reclaimable" is a claim about the stick, and a caller that shows
    // it without saying part of the stick was unreadable is lying by
    // omission. Surface it; do not refuse to act on it.
    bool incomplete = false;

    std::size_t directoriesVisited = 0;
};

AudioFileWalkResult walkAudioFiles(const std::string &root, const application::CancellationToken &cancel);

}  // namespace seabass::infrastructure::cleanup
