// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/library_reader.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Read-only use case: return every track (with cues) found in a library.
// Works against any LibraryReader adapter, so it's agnostic to whether the
// source is a rekordbox USB export or an Engine Library.
class ScanLibrary
{
public:
    explicit ScanLibrary(LibraryReader &reader) : m_reader(reader) {}

    std::vector<domain::Track> execute() { return m_reader.readAll(); }

private:
    LibraryReader &m_reader;
};

}  // namespace seabass::application
