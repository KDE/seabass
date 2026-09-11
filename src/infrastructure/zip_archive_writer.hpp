// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

namespace seabass::infrastructure
{

// Writes a standard .zip archive at zipPath containing every regular
// file found (recursively) under sourceDir, with archive-relative
// paths preserving sourceDir's own subdirectory structure (e.g.
// sourceDir/rekordbox/export.pdb becomes the archive entry
// "rekordbox/export.pdb"). Deflate-compressed (zlib is already a
// project dependency -- see infrastructure/compression/zlib_compressor
// for the same library used elsewhere -- so this needed no new
// dependency, just the low-level raw-deflate half of its API the
// higher-level compress()/decompress() wrapper doesn't expose).
//
// Implements just enough of the ZIP file format (local file headers,
// central directory, end-of-central-directory record) to produce an
// archive any standard unzip tool (Windows Explorer, Archive Manager,
// `unzip`) opens correctly -- not a general-purpose zip library, no
// reading/updating support, since nothing in this codebase needs
// those.
//
// Throws std::runtime_error if sourceDir doesn't exist, contains no
// files, or the archive can't be written (e.g. disk full).
void writeZipArchive(const std::filesystem::path &sourceDir, const std::filesystem::path &zipPath);

}  // namespace seabass::infrastructure
