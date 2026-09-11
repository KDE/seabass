// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// One tagged section of an ANLZ file, kept as opaque raw bytes (fourcc +
// len_header + len_tag + body, i.e. exactly len_tag bytes). We only need
// to semantically understand the one section we're editing (PCO2, hot
// cues); every other section (waveforms, beatgrid, path, ...) is copied
// through untouched as bytes we never interpret -- the safest possible
// approach for a format we can't afford to silently corrupt.
struct AnlzRawSection
{
    uint32_t fourcc = 0;
    std::string rawBytes;
};

// A parsed-at-the-section-level ANLZ file: the fixed header (magic,
// len_header, len_file, and whatever padding fills out len_header) kept
// verbatim, plus the sequence of raw sections that follow it.
//
// This is this project's most-exercised write path (every cue sync/
// consolidation goes through it), so it gets the same hardening as
// PdbRowWriter: writeRaw() refuses if the file at `path` has changed
// (size/mtime) since readRaw() read it, re-validates its own
// reassembled output before writing it anywhere, and writes durably
// (fsync'd) and atomically (same-directory temp file + rename) rather
// than truncating the real file in place.
class AnlzFile
{
public:
    static AnlzFile readRaw(const std::string &path);

    // Recomputes len_file from the current header+sections size, then
    // writes the file out. Throws std::runtime_error if the file at
    // `path` has changed since readRaw() read it, if the reassembled
    // bytes don't parse back as a valid ANLZ file, or if the durable
    // write/rename fails.
    void writeRaw(const std::string &path) const;

    std::string headerBytes;
    std::vector<AnlzRawSection> sections;

private:
    std::string m_sourcePath;
    std::uintmax_t m_sourceFileSize = 0;
    std::filesystem::file_time_type m_sourceMtime;
};

}  // namespace seabass::infrastructure::rekordbox
