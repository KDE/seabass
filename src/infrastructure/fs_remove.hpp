// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>

namespace seabass::infrastructure
{

// Removes one file or one empty directory and answers whether it is
// gone. That is not the question std::filesystem::remove() answers.
//
// remove() answers "did I unlink something", and reports a name it
// cannot resolve as a quiet false with no error -- the same answer it
// gives for a file that was already deleted. On macOS that is not a
// theoretical difference. Its exFAT driver hands out decomposed names
// from a directory read ("Ko" + U+0308 for the "ö" in "Kölsch") and
// resolves either spelling for stat, but unlink and rmdir take only the
// composed one and answer ENOENT for the spelling the directory read
// just produced. An exact restore counted 42 folders as removed that
// were still on the stick, and said so in its summary; Clean Up would
// have told someone a track was deleted when it was not.
//
// So: remove, then look. If the entry is still there, retry the composed
// spelling of its name -- but only once the filesystem itself confirms
// the two names are one entry, so that on a filesystem where they are
// two files (ext4, APFS) nothing else is touched. There the first remove
// worked anyway.
//
// Paths are long-path prefixed here, so callers do not have to: a track
// under a long artist/album path can sit past Windows' MAX_PATH, where
// the unprefixed calls answer "not there" about a file that is present.
//
// `failure` is set only when the entry is still there afterwards, and
// carries a reason fit to show a person -- never the empty string, and
// never "The operation completed successfully".
bool removeEntry(const std::filesystem::path &target, std::string &failure);

}  // namespace seabass::infrastructure
