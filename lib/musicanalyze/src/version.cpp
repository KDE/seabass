// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "musicanalyze/version.hpp"

#define MUSICANALYZE_TEXT_OF(x) #x
#define MUSICANALYZE_TEXT(x) MUSICANALYZE_TEXT_OF(x)

namespace musicanalyze
{

const char *version()
{
    return MUSICANALYZE_TEXT(MUSICANALYZE_VERSION_MAJOR) "." MUSICANALYZE_TEXT(MUSICANALYZE_VERSION_MINOR) "." MUSICANALYZE_TEXT(
        MUSICANALYZE_VERSION_PATCH);
}

Revisions revisions()
{
    return Revisions{GridRevision, KeyRevision, WaveformRevision};
}

}  // namespace musicanalyze
