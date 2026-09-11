// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::application
{

// Port for writing a track's hot cues back into a library. Implemented for
// both rekordbox (RekordboxCueWriter, via the ANLZ PCO2 sections) and
// Engine (LibdjinteropEngineCueWriter, via libdjinterop).
class CueWriter
{
public:
    virtual ~CueWriter() = default;
    virtual void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) = 0;
};

}  // namespace seabass::application
