// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/interface_font.hpp"

#include <QFontDatabase>
#include <QStringList>

namespace seabass::gui
{

namespace
{

// Real families, in the order they are worth having, one per platform
// this project ships on plus the two a Linux container is likely to
// carry. Every one of them is a name a font database can resolve, which
// is the entire point: the thing being replaced is a category.
const QStringList &fallbackFamilies()
{
    static const QStringList families{
        QStringLiteral("Segoe UI"),        // Windows
        QStringLiteral("Helvetica Neue"),  // macOS
        QStringLiteral("Noto Sans"),       // most Linux desktops
        QStringLiteral("DejaVu Sans"),     // what a bare container has
        QStringLiteral("Liberation Sans"),
        QStringLiteral("Arial"),
    };
    return families;
}

}  // namespace

QFont interfaceFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QStringList available = QFontDatabase::families();
    if (available.contains(font.family(), Qt::CaseInsensitive)) {
        return font;
    }

    for (const QString &candidate : fallbackFamilies()) {
        if (available.contains(candidate, Qt::CaseInsensitive)) {
            font.setFamily(candidate);
            return font;
        }
    }

    // Nothing recognised: keep what the platform said rather than
    // inventing a name. A database with no families in it is a problem
    // this function cannot fix, and pretending otherwise would hide it.
    return font;
}

}  // namespace seabass::gui
