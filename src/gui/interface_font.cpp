// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/interface_font.hpp"

#include <QFontDatabase>
#include <QRawFont>
#include <QString>
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

// Whether the face a font actually resolves to can draw the interface.
// Not whether the family is LISTED, which was the first rule here and
// was wrong: macOS's own interface font is ".AppleSystemUIFont", which
// QFontDatabase::families() deliberately does not list, so that rule
// threw away a perfectly good platform font and replaced it with
// Helvetica Neue. Different metrics, different elision, and a page test
// that had been green went red -- caught on macOS before this landed.
//
// Coverage is the property that actually matters, and it is what fails
// in the case this exists for: the three-glyph symbol subset resolves,
// is a real face, and cannot draw a letter.
bool drawsLatin(const QFont &font)
{
    const QRawFont face = QRawFont::fromFont(font);
    if (!face.isValid()) {
        return false;
    }
    for (const QChar c : QStringLiteral("EngineOS")) {
        if (!face.supportsCharacter(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

QFont interfaceFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    if (drawsLatin(font)) {
        return font;
    }

    for (const QString &candidate : fallbackFamilies()) {
        QFont trial = font;
        trial.setFamily(candidate);
        if (drawsLatin(trial)) {
            return trial;
        }
    }

    // Nothing here can draw a letter: keep what the platform said rather
    // than inventing a name. A font database in that state is a problem
    // this function cannot fix, and pretending otherwise would hide it.
    return font;
}

}  // namespace seabass::gui
