// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/library_reader.hpp"
#include "application/use_cases/fill_file_sizes.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Read-only use case: return every track (with cues and file sizes) found
// in a library. Works against any LibraryReader adapter, so it's agnostic
// to whether the source is a rekordbox USB export or an Engine Library.
// The sizes and the artwork check are done here (completeTracks())
// because the readers no longer stat audio files or covers (that is the
// Full stage of the GUI's catalog cache, which runs the same function);
// the CLI and the tools that go through this class get what they always
// got.
class ScanLibrary
{
public:
    explicit ScanLibrary(LibraryReader &reader) : m_reader(reader) {}

    std::vector<domain::Track> execute()
    {
        std::vector<domain::Track> tracks = m_reader.readAll();
        completeTracks(tracks);
        return tracks;
    }

private:
    LibraryReader &m_reader;
};

}  // namespace seabass::application
