// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "musicanalyze/key.hpp"

namespace seabass::domain
{

// The key detector is the analysis library's (lib/musicanalyze, key.hpp,
// where what it does is written down). Its names under the ones Seabass
// has used for them; keyName() spells a key the way CamelotKey::parse()
// reads it.
using musicanalyze::KeyAnalysis;
using musicanalyze::keyFromSamples;
using musicanalyze::KeyLowestSampleRateHz;
using musicanalyze::keyName;
using musicanalyze::MusicalKey;
using musicanalyze::pitchPicture;

}  // namespace seabass::domain
