// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include "gui/qt_path.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

// The stick a catalog path ("<stick>/PIONEER", "<stick>/Engine Library")
// is on, as the QString a request names its stick by.
inline QString stickRootOf(const QString &catalogPath)
{
    if (catalogPath.isEmpty()) {
        return {};
    }
    return qtPathFromUtf8(infrastructure::paths::stickRootForCatalogPath(pathToUtf8(pathFromQString(catalogPath))));
}

}  // namespace seabass::gui
