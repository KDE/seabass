// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include <djinterop/pad_color.hpp>

#include "application/ports/library_reader.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// "" for alpha == 0 (djinterop::pad_color's own default-constructed
// value, and what an Engine hot cue that was never explicitly colored
// reads back as -- see the .cpp's own comment), otherwise "#RRGGBB".
// Exposed here (rather than kept anonymous-namespace-private) so it has
// direct unit test coverage.
std::string colorHex(const djinterop::pad_color &c);

// Reads an Engine Library (Database2/m.db etc.) using the vendored
// libdjinterop. This is the only place that knows about libdjinterop or the
// on-disk Engine schema.
class LibdjinteropEngineReader : public application::LibraryReader
{
public:
    // engineLibraryPath is the directory containing "Database2/"
    // (i.e. the "Engine Library" folder itself).
    explicit LibdjinteropEngineReader(std::string engineLibraryPath);

    std::vector<domain::Track> readAll() override;

private:
    std::string m_engineLibraryPath;
};

}  // namespace seabass::infrastructure::engine
