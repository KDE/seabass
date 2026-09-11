// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

namespace seabass::infrastructure
{

// Unpacks an archive written by writeZipArchive() into destDir, creating
// the subdirectories its entry names imply.
//
// Reads just enough of the format to undo what the writer does: the
// end-of-central-directory record, the central directory, and each
// entry's stored or deflated data. It is not a general zip library. It
// rejects an entry whose name would escape destDir, which matters because
// a corpus set can arrive from anywhere.
//
// Exists so a data set can be kept and moved as one file instead of six
// thousand, and still be read by the corpus runner. The two from-scratch
// parsers in the test suite are deliberately not this function: they exist
// to check the writer's output without trusting any of our own reading
// code, so they stay independent.
//
// Throws std::runtime_error on anything it does not recognise.
void extractZipArchive(const std::filesystem::path &zipPath, const std::filesystem::path &destDir);

}  // namespace seabass::infrastructure
