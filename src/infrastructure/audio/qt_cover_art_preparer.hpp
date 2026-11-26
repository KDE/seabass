// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/cover_art_preparer.hpp"

namespace seabass::infrastructure::audio
{

// CoverArtPreparer through QImage: reads any picture Qt's image plugins
// read (its EXIF rotation applied), lays a transparent one on white,
// scales it down with smoothing and writes a baseline JPEG, lowering the
// quality step by step until it fits MaxCoverBytes.
//
// Needs Qt's JPEG image plugin at run time; without it every picture is
// refused, saying so. Safe from any thread.
class QtCoverArtPreparer : public application::CoverArtPreparer
{
public:
    application::CoverArtResult prepare(const std::string &pictureBytes) override;
};

}  // namespace seabass::infrastructure::audio
