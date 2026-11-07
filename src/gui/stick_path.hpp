// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>

#include "application/use_cases/plan_engine_update.hpp"
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

// Where a stored track's audio file is now, or empty when no stick that
// is plugged in has it. The backup holds no audio, so a stored row plays
// from a stick: first the one it was backed up from, by label, then any
// other, because a clone or a second stick with the same export keeps the
// folders and not the label. `libraries` is one {label, catalogPath} per
// stick with a library on it.
//
// A stat per stick: for the row under the pointer or the open one, never
// for a whole list.
inline QString storedTrackFileOnSticks(const QString &relativePath, const QString &stickLabel,
                                       const QVariantList &libraries)
{
    if (relativePath.isEmpty()) {
        return {};
    }
    // The store keeps stick-relative paths (stickRelativePath refuses the
    // rest), and anything else would name a file off the stick.
    const std::filesystem::path relative = pathFromQString(relativePath);
    if (relative.has_root_path()) {
        return {};
    }
    std::size_t components = 0;
    for (const auto &part : relative) {
        if (part == "..") {
            return {};
        }
        ++components;
    }
    // A bare filename is the store's fallback key for a track it could
    // not place under its stick's root (stickRelativePath), not a
    // location: a file of that name at a stick's root is some other file.
    // A track that really lies at a stick's root cannot be played from a
    // stored row, which is the price.
    if (components < 2) {
        return {};
    }
    for (const bool sameLabel : {true, false}) {
        for (const QVariant &entry : libraries) {
            const QVariantMap library = entry.toMap();
            if ((library.value(QStringLiteral("label")).toString() == stickLabel) != sameLabel) {
                continue;
            }
            const QString root = stickRootOf(library.value(QStringLiteral("catalogPath")).toString());
            if (root.isEmpty()) {
                continue;
            }
            const std::filesystem::path stickRoot = pathFromQString(root);
            if (application::fileExistsUnder(pathToUtf8(stickRoot))(pathToUtf8(relative))) {
                return pathToQString(stickRoot / relative);
            }
        }
    }
    return {};
}

}  // namespace seabass::gui
